#include "UpdateCheckService.h"

#include "JsonValue.h"
#include "Utilities.h"
#include "Version.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#ifdef QUATTRO_WITH_CURL
#include <curl/curl.h>
#endif

namespace {
constexpr const wchar_t* kLatestReleaseApi = L"https://api.github.com/repos/codingriver/quattro/releases/latest";

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring StripVersionPrefix(std::wstring value) {
    value = Trim(value);
    if (!value.empty() && (value.front() == L'v' || value.front() == L'V')) {
        value.erase(value.begin());
    }
    return value;
}

std::vector<int> VersionParts(const std::wstring& value) {
    const std::wstring normalized = StripVersionPrefix(value);
    std::vector<int> parts;
    std::wstring current;
    for (wchar_t ch : normalized) {
        if (ch >= L'0' && ch <= L'9') {
            current.push_back(ch);
            continue;
        }
        if (ch == L'.') {
            parts.push_back(current.empty() ? 0 : std::stoi(current));
            current.clear();
            continue;
        }
        break;
    }
    parts.push_back(current.empty() ? 0 : std::stoi(current));
    while (parts.size() < 3) {
        parts.push_back(0);
    }
    return parts;
}

std::wstring CurrentArchitectureName() {
#if defined(_M_X64) || defined(__x86_64__)
    return L"x64";
#else
    return L"x86";
#endif
}

bool IsExpectedAssetName(const std::wstring& name) {
    const std::wstring lower = ToLower(name);
    const std::wstring arch = CurrentArchitectureName();
    return lower == L"quattro-" + arch + L".exe" ||
           lower == L"quattro.exe";
}

std::wstring SanitizeFileName(std::wstring value) {
    for (wchar_t& ch : value) {
        if (ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' ||
            ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*') {
            ch = L'_';
        }
    }
    return value;
}

std::filesystem::path UpdateDownloadDirectory() {
    return QuattroUserConfigDirectory() / L"updates";
}

std::wstring NormalizeReleaseApiUrl(const std::wstring& configuredUrl) {
    const std::wstring trimmed = Trim(configuredUrl);
    if (trimmed.empty()) {
        return kLatestReleaseApi;
    }

    const std::wstring lower = ToLower(trimmed);
    if (lower.rfind(L"https://api.github.com/repos/", 0) == 0) {
        return trimmed;
    }

    constexpr const wchar_t* githubPrefix = L"https://github.com/";
    if (lower.rfind(githubPrefix, 0) == 0) {
        std::wstring rest = trimmed.substr(std::wcslen(githubPrefix));
        const std::size_t firstSlash = rest.find(L'/');
        if (firstSlash != std::wstring::npos) {
            const std::size_t secondSlash = rest.find(L'/', firstSlash + 1);
            if (secondSlash != std::wstring::npos) {
                const std::wstring owner = rest.substr(0, firstSlash);
                const std::wstring repo = rest.substr(firstSlash + 1, secondSlash - firstSlash - 1);
                const std::wstring suffix = ToLower(rest.substr(secondSlash));
                if (!owner.empty() && !repo.empty() && suffix.rfind(L"/releases", 0) == 0) {
                    return L"https://api.github.com/repos/" + owner + L"/" + repo + L"/releases/latest";
                }
            }
        }
    }

    return trimmed;
}

#ifdef QUATTRO_WITH_CURL
std::size_t WriteStringCallback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* body = reinterpret_cast<std::string*>(userdata);
    const std::size_t total = size * nmemb;
    body->append(ptr, total);
    return total;
}

std::size_t WriteFileCallback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* file = reinterpret_cast<std::ofstream*>(userdata);
    const std::size_t total = size * nmemb;
    file->write(ptr, static_cast<std::streamsize>(total));
    return total;
}

bool DownloadText(const std::wstring& url, std::string& body, std::wstring& error) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = L"初始化 curl 失败。";
        return false;
    }
    const std::string urlUtf8 = WideToUtf8(url);
    char errorBuffer[CURL_ERROR_SIZE]{};
    curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Quattro Update Checker/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteStringCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK || status < 200 || status >= 300) {
        const std::string detail = errorBuffer[0] ? errorBuffer : curl_easy_strerror(code);
        error = L"请求更新信息失败: HTTP " + std::to_wstring(status) + L"，" + Utf8ToWide(detail);
        return false;
    }
    return true;
}

bool DownloadFile(const std::wstring& url, const std::filesystem::path& path, std::wstring& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = L"创建下载文件失败: " + path.wstring();
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        error = L"初始化 curl 失败。";
        return false;
    }
    const std::string urlUtf8 = WideToUtf8(url);
    char errorBuffer[CURL_ERROR_SIZE]{};
    curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Quattro Update Downloader/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    file.close();
    if (code != CURLE_OK || status < 200 || status >= 300) {
        const std::string detail = errorBuffer[0] ? errorBuffer : curl_easy_strerror(code);
        error = L"下载更新失败: HTTP " + std::to_wstring(status) + L"，" + Utf8ToWide(detail);
        return false;
    }
    if (!FileExists(path)) {
        error = L"下载文件不存在。";
        return false;
    }
    return true;
}
#else
bool DownloadText(const std::wstring&, std::string&, std::wstring& error) {
    error = L"当前构建未启用 curl，无法检查更新。";
    return false;
}

bool DownloadFile(const std::wstring&, const std::filesystem::path&, std::wstring& error) {
    error = L"当前构建未启用 curl，无法下载更新。";
    return false;
}
#endif

std::wstring BytesToHex(const std::vector<unsigned char>& bytes) {
    std::wstringstream stream;
    stream << std::hex;
    for (unsigned char byte : bytes) {
        stream.width(2);
        stream.fill(L'0');
        stream << static_cast<int>(byte);
    }
    return stream.str();
}

bool Sha256File(const std::filesystem::path& path, std::wstring& hash, std::wstring& error) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE handle = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        error = L"初始化 SHA256 失败。";
        return false;
    }
    if (BCryptCreateHash(algorithm, &handle, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = L"创建 SHA256 上下文失败。";
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        BCryptDestroyHash(handle);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = L"读取下载文件失败。";
        return false;
    }

    std::vector<char> buffer(64 * 1024);
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize copied = file.gcount();
        if (copied > 0 && BCryptHashData(handle, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(copied), 0) != 0) {
            BCryptDestroyHash(handle);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            error = L"计算 SHA256 失败。";
            return false;
        }
    }

    std::vector<unsigned char> digest(32);
    if (BCryptFinishHash(handle, digest.data(), static_cast<ULONG>(digest.size()), 0) != 0) {
        BCryptDestroyHash(handle);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = L"完成 SHA256 失败。";
        return false;
    }
    BCryptDestroyHash(handle);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    hash = BytesToHex(digest);
    return true;
}

std::wstring ExpectedSha256For(const std::wstring& checksumText, const std::wstring& assetName) {
    std::wstringstream stream(checksumText);
    std::wstring line;
    const std::wstring lowerAsset = ToLower(assetName);
    while (std::getline(stream, line)) {
        const std::wstring lower = ToLower(line);
        if (lower.find(lowerAsset) == std::wstring::npos) {
            continue;
        }
        std::wstringstream lineStream(line);
        std::wstring hash;
        lineStream >> hash;
        if (hash.size() == 64) {
            return ToLower(hash);
        }
    }
    return {};
}
}

UpdateCheckService::UpdateCheckService(std::filesystem::path appDirectory, std::wstring releaseApiUrl)
    : appDirectory_(std::move(appDirectory)),
      releaseApiUrl_(std::move(releaseApiUrl)) {
}

int UpdateCheckService::CompareVersions(const std::wstring& left, const std::wstring& right) {
    const std::vector<int> a = VersionParts(left);
    const std::vector<int> b = VersionParts(right);
    const std::size_t count = std::max(a.size(), b.size());
    for (std::size_t i = 0; i < count; ++i) {
        const int av = i < a.size() ? a[i] : 0;
        const int bv = i < b.size() ? b[i] : 0;
        if (av < bv) return -1;
        if (av > bv) return 1;
    }
    return 0;
}

bool UpdateCheckService::CheckLatest(UpdateReleaseInfo& info, std::wstring& error) const {
    info = {};
    info.currentVersion = QuattroVersionText();

    std::string body;
    if (!DownloadText(NormalizeReleaseApiUrl(releaseApiUrl_), body, error)) {
        return false;
    }

    JsonValue root;
    if (!ParseJson(Utf8ToWide(body), root, error) || !root.isObject()) {
        if (error.empty()) {
            error = L"更新信息格式无效。";
        }
        return false;
    }

    info.latestVersion = root.get(L"tag_name") ? root.get(L"tag_name")->stringOr() : L"";
    info.releaseUrl = root.get(L"html_url") ? root.get(L"html_url")->stringOr() : L"";
    info.releaseNotes = root.get(L"body") ? root.get(L"body")->stringOr() : L"";
    if (info.latestVersion.empty()) {
        error = L"更新信息缺少版本号。";
        return false;
    }

    const JsonValue* assets = root.get(L"assets");
    if (assets && assets->isArray()) {
        for (const JsonValue& asset : assets->arrayValue) {
            const std::wstring name = asset.get(L"name") ? asset.get(L"name")->stringOr() : L"";
            const std::wstring url = asset.get(L"browser_download_url") ? asset.get(L"browser_download_url")->stringOr() : L"";
            if (ToLower(name) == L"sha256sums.txt") {
                info.checksumDownloadUrl = url;
            }
            if (info.assetDownloadUrl.empty() && IsExpectedAssetName(name)) {
                info.assetName = name;
                info.assetDownloadUrl = url;
            }
        }
    }

    info.updateAvailable = CompareVersions(info.currentVersion, info.latestVersion) < 0;
    if (info.updateAvailable && info.assetDownloadUrl.empty()) {
        error = L"找到新版本，但未找到适合当前架构的发布包。";
        return false;
    }
    return true;
}

bool UpdateCheckService::DownloadUpdate(const UpdateReleaseInfo& info, UpdateDownloadResult& result, std::wstring& error) const {
    result = {};
    if (info.assetDownloadUrl.empty() || info.assetName.empty()) {
        error = L"更新包下载地址为空。";
        return false;
    }

    const std::wstring fileName = SanitizeFileName(StripVersionPrefix(info.latestVersion) + L"-" + info.assetName);
    const std::filesystem::path target = UpdateDownloadDirectory() / fileName;
    const std::filesystem::path temp = target.wstring() + L".tmp";
    std::error_code ec;
    std::filesystem::remove(temp, ec);

    if (!DownloadFile(info.assetDownloadUrl, temp, error)) {
        return false;
    }
    const auto size = std::filesystem::file_size(temp, ec);
    if (ec || size == 0) {
        error = L"下载文件为空。";
        return false;
    }

    if (!info.checksumDownloadUrl.empty()) {
        std::string checksumBody;
        std::wstring checksumError;
        if (DownloadText(info.checksumDownloadUrl, checksumBody, checksumError)) {
            const std::wstring expected = ExpectedSha256For(Utf8ToWide(checksumBody), info.assetName);
            std::wstring actual;
            if (!expected.empty() && Sha256File(temp, actual, error)) {
                if (ToLower(actual) != expected) {
                    error = L"更新包 SHA256 校验失败。";
                    return false;
                }
                result.checksumVerified = true;
                result.checksumMessage = L"SHA256 校验通过。";
            } else if (expected.empty()) {
                result.checksumMessage = L"未在 SHA256SUMS.txt 中找到当前包校验值。";
            }
        } else {
            result.checksumMessage = L"无法下载 SHA256SUMS.txt: " + checksumError;
        }
    } else {
        result.checksumMessage = L"Release 未提供 SHA256SUMS.txt。";
    }

    std::filesystem::remove(target, ec);
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        error = L"保存更新包失败: " + Utf8ToWide(ec.message());
        return false;
    }
    result.filePath = target;
    return true;
}
