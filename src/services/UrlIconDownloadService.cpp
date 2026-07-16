#include "UrlIconDownloadService.h"

#include "Utilities.h"

#include <wininet.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>

#ifdef QUATTRO_WITH_CURL
#include <curl/curl.h>
#endif

namespace {
bool LooksLikeWebUrl(const Link& link) {
    const std::wstring lower = ToLower(Trim(link.path));
    return link.type == 2 ||
           lower.rfind(L"http://", 0) == 0 ||
           lower.rfind(L"https://", 0) == 0 ||
           lower.rfind(L"www.", 0) == 0;
}

std::wstring UrlHost(const std::wstring& value) {
    const std::wstring text = NormalizeUrl(value);
    URL_COMPONENTSW parts{};
    wchar_t host[260]{};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    if (InternetCrackUrlW(text.c_str(), 0, 0, &parts) && parts.dwHostNameLength > 0) {
        return ToLower(std::wstring(host, parts.dwHostNameLength));
    }
    return {};
}

std::wstring SchemeForUrl(const std::wstring& value) {
    const std::wstring text = NormalizeUrl(value);
    URL_COMPONENTSW parts{};
    wchar_t scheme[16]{};
    parts.dwStructSize = sizeof(parts);
    parts.lpszScheme = scheme;
    parts.dwSchemeLength = static_cast<DWORD>(std::size(scheme));
    if (InternetCrackUrlW(text.c_str(), 0, 0, &parts) && parts.dwSchemeLength > 0) {
        return ToLower(std::wstring(scheme, parts.dwSchemeLength));
    }
    return {};
}

std::wstring SafeHostFileName(std::wstring host) {
    for (wchar_t& ch : host) {
        if (ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' || ch == L'/' ||
            ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*') {
            ch = L'_';
        }
    }
    return host;
}

std::filesystem::path UrlIconDirectory(const std::filesystem::path& appDirectory) {
    return appDirectory / L"icons" / L"url";
}

bool HasExistingIcon(const std::filesystem::path& directory, const std::wstring& fileName) {
    return FileExists(directory / (fileName + L".png")) ||
           FileExists(directory / (fileName + L".ico"));
}

enum class IconFormat {
    Unknown,
    Ico,
    Png,
};

IconFormat DetectIconFormat(const std::vector<std::uint8_t>& data) {
    if (data.size() >= 8 &&
        data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4e && data[3] == 0x47 &&
        data[4] == 0x0d && data[5] == 0x0a && data[6] == 0x1a && data[7] == 0x0a) {
        return IconFormat::Png;
    }
    if (data.size() >= 4 &&
        data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01 && data[3] == 0x00) {
        return IconFormat::Ico;
    }
    return IconFormat::Unknown;
}

bool WriteFileBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return output.good();
}

#ifdef QUATTRO_WITH_CURL
std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), needed, nullptr, nullptr);
    if (!result.empty() && result.back() == '\0') {
        result.pop_back();
    }
    return result;
}

std::size_t WriteCurlData(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    constexpr std::size_t kMaxIconBytes = 1024 * 1024;
    auto* data = static_cast<std::vector<std::uint8_t>*>(userdata);
    const std::size_t bytes = size * nmemb;
    if (!data || bytes == 0) {
        return 0;
    }
    if (data->size() + bytes > kMaxIconBytes) {
        return 0;
    }
    const auto* begin = reinterpret_cast<const std::uint8_t*>(ptr);
    data->insert(data->end(), begin, begin + bytes);
    return bytes;
}

bool DownloadBytes(const std::wstring& url, std::vector<std::uint8_t>& data) {
    data.clear();
    CURL* curl = curl_easy_init();
    if (!curl) {
        return false;
    }

    const std::string urlUtf8 = WideToUtf8(url);
    char errorBuffer[CURL_ERROR_SIZE]{};
    curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Quattro URL Icon/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCurlData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);

    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    return code == CURLE_OK && status >= 200 && status < 300 && !data.empty();
}
#else
bool DownloadBytes(const std::wstring&, std::vector<std::uint8_t>& data) {
    data.clear();
    return false;
}
#endif
}

UrlIconDownloadService::UrlIconDownloadService(std::filesystem::path appDirectory)
    : appDirectory_(std::move(appDirectory)) {
}

UrlIconDownloadService::~UrlIconDownloadService() {
    Shutdown();
}

void UrlIconDownloadService::RequestInitialDownload(HWND notifyHwnd, UINT notifyMessage, Link link) {
    RequestDownload(notifyHwnd, notifyMessage, std::move(link), false);
}

bool UrlIconDownloadService::RequestManualRefresh(HWND notifyHwnd, UINT notifyMessage, Link link) {
    return RequestDownload(notifyHwnd, notifyMessage, std::move(link), true);
}

bool UrlIconDownloadService::RequestDownload(HWND notifyHwnd, UINT notifyMessage, Link link, bool overwrite) {
    if (!notifyHwnd || notifyMessage == 0 || !LooksLikeWebUrl(link)) {
        return false;
    }

    const std::wstring host = UrlHost(link.path);
    if (host.empty() || !TryClaimHost(host)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
        activeHosts_.erase(host);
        return false;
    }
    threads_.emplace_back([this, notifyHwnd, notifyMessage, link = std::move(link), host, overwrite]() {
        const bool ok = DownloadIconForLink(link, overwrite);
        ReleaseHost(host);
        PostMessageW(notifyHwnd, notifyMessage, static_cast<WPARAM>(link.id), ok ? 1 : 0);
    });
    return true;
}

void UrlIconDownloadService::Shutdown() {
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ && threads_.empty()) {
            return;
        }
        stopping_ = true;
        threads.swap(threads_);
    }

    for (std::thread& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

bool UrlIconDownloadService::DownloadIconForLink(const Link& link, bool overwrite) {
    const std::wstring scheme = SchemeForUrl(link.path);
    if (scheme != L"http" && scheme != L"https") {
        return false;
    }

    const std::wstring host = UrlHost(link.path);
    if (host.empty()) {
        return false;
    }

    const std::wstring fileName = SafeHostFileName(host);
    const std::filesystem::path iconDirectory = UrlIconDirectory(appDirectory_);
    if (!overwrite && HasExistingIcon(iconDirectory, fileName)) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(iconDirectory, ec);
    if (ec) {
        return false;
    }

    std::vector<std::uint8_t> data;
    const std::wstring url = scheme + L"://" + host + L"/favicon.ico";
    if (!DownloadBytes(url, data)) {
        return false;
    }

    const IconFormat format = DetectIconFormat(data);
    if (format == IconFormat::Unknown) {
        return false;
    }

    const std::wstring extension = format == IconFormat::Png ? L".png" : L".ico";
    const std::wstring opposite = format == IconFormat::Png ? L".ico" : L".png";
    const std::filesystem::path target = iconDirectory / (fileName + extension);
    const std::filesystem::path temp = iconDirectory / (fileName + extension + L".tmp");
    if (!WriteFileBytes(temp, data)) {
        std::filesystem::remove(temp, ec);
        return false;
    }

    std::filesystem::rename(temp, target, ec);
    if (ec) {
        ec.clear();
        std::filesystem::remove(target, ec);
        ec.clear();
        std::filesystem::rename(temp, target, ec);
    }
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }

    if (overwrite) {
        std::filesystem::remove(iconDirectory / (fileName + opposite), ec);
    }
    return true;
}

bool UrlIconDownloadService::TryClaimHost(const std::wstring& host) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
        return false;
    }
    return activeHosts_.insert(host).second;
}

void UrlIconDownloadService::ReleaseHost(const std::wstring& host) {
    std::lock_guard<std::mutex> lock(mutex_);
    activeHosts_.erase(host);
}
