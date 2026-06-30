#include "GitHubReleaseClient.h"

#include "JsonValue.h"
#include "Utilities.h"

#ifdef QUATTRO_WITH_CURL
#include <curl/curl.h>
#endif

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace {
std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string output(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), length, nullptr, nullptr);
    return output;
}

std::wstring Utf8ToWide(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return {};
    }
    const char* data = reinterpret_cast<const char*>(bytes.data());
    const int length = MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(bytes.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring output(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(bytes.size()), output.data(), length);
    return output;
}

std::wstring Utf8StringToWide(const std::string& value) {
    return Utf8ToWide(std::vector<std::uint8_t>(value.begin(), value.end()));
}

std::wstring JsonEscape(const std::wstring& value) {
    std::wstring output;
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\\': output += L"\\\\"; break;
        case L'"': output += L"\\\""; break;
        case L'\n': output += L"\\n"; break;
        case L'\r': output += L"\\r"; break;
        case L'\t': output += L"\\t"; break;
        default: output.push_back(ch); break;
        }
    }
    return output;
}

std::wstring UrlEncode(const std::wstring& value) {
    const std::string utf8 = WideToUtf8(value);
    std::wstringstream out;
    out << std::uppercase << std::hex;
    for (unsigned char ch : utf8) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.') {
            out << static_cast<wchar_t>(ch);
        } else {
            out << L'%' << std::setw(2) << std::setfill(L'0') << static_cast<int>(ch);
        }
    }
    return out.str();
}

const JsonValue* ArrayValue(const JsonValue& value, const wchar_t* key) {
    const JsonValue* child = value.get(key);
    return child && child->isArray() ? child : nullptr;
}

std::wstring StringValue(const JsonValue& value, const wchar_t* key, const std::wstring& fallback = L"") {
    const JsonValue* child = value.get(key);
    return child ? child->stringOr(fallback) : fallback;
}

bool BoolValue(const JsonValue& value, const wchar_t* key, bool fallback = false) {
    const JsonValue* child = value.get(key);
    return child ? child->boolOr(fallback) : fallback;
}

long long Int64Value(const JsonValue& value, const wchar_t* key, long long fallback = 0) {
    const JsonValue* child = value.get(key);
    if (!child || !child->isNumber()) {
        return fallback;
    }
    return static_cast<long long>(child->numberValue);
}

std::uint64_t UInt64Value(const JsonValue& value, const wchar_t* key, std::uint64_t fallback = 0) {
    const JsonValue* child = value.get(key);
    if (!child || !child->isNumber() || child->numberValue < 0) {
        return fallback;
    }
    return static_cast<std::uint64_t>(child->numberValue);
}

bool ParseReleaseObject(const JsonValue& item, AppStoreRelease& release) {
    if (!item.isObject()) {
        return false;
    }
    release.id = Int64Value(item, L"id");
    release.tagName = StringValue(item, L"tag_name");
    release.name = StringValue(item, L"name");
    release.uploadUrl = StringValue(item, L"upload_url");
    release.draft = BoolValue(item, L"draft", false);
    release.prerelease = BoolValue(item, L"prerelease", false);
    release.publishedAt = StringValue(item, L"published_at");
    release.assets.clear();
    if (release.tagName.empty()) {
        return false;
    }
    if (const JsonValue* assets = ArrayValue(item, L"assets")) {
        for (const auto& assetValue : assets->arrayValue) {
            if (!assetValue.isObject()) {
                continue;
            }
            AppStoreReleaseAsset asset;
            asset.id = Int64Value(assetValue, L"id");
            asset.name = StringValue(assetValue, L"name");
            asset.size = UInt64Value(assetValue, L"size");
            asset.browserDownloadUrl = StringValue(assetValue, L"browser_download_url");
            if (asset.id > 0 && !asset.name.empty()) {
                release.assets.push_back(std::move(asset));
            }
        }
    }
    return true;
}

#ifdef QUATTRO_WITH_CURL
struct MemorySink {
    std::vector<std::uint8_t>* bytes = nullptr;
};

struct FileSink {
    std::ofstream* file = nullptr;
};

struct UploadSource {
    std::ifstream* file = nullptr;
};

struct ProgressContext {
    GitHubReleaseClient::TransferProgressCallback callback;
};

size_t WriteMemoryCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* sink = static_cast<MemorySink*>(userdata);
    const size_t bytes = size * nmemb;
    if (sink && sink->bytes && bytes > 0) {
        sink->bytes->insert(sink->bytes->end(), reinterpret_cast<std::uint8_t*>(ptr), reinterpret_cast<std::uint8_t*>(ptr) + bytes);
    }
    return bytes;
}

size_t WriteFileCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* sink = static_cast<FileSink*>(userdata);
    const size_t bytes = size * nmemb;
    if (sink && sink->file && bytes > 0) {
        sink->file->write(ptr, static_cast<std::streamsize>(bytes));
        if (!*sink->file) {
            return 0;
        }
    }
    return bytes;
}

size_t ReadFileCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* source = static_cast<UploadSource*>(userdata);
    const size_t capacity = size * nmemb;
    if (!source || !source->file || capacity == 0) {
        return 0;
    }
    source->file->read(ptr, static_cast<std::streamsize>(capacity));
    return static_cast<size_t>(source->file->gcount());
}

int ProgressCallback(void* userdata, curl_off_t downloadTotal, curl_off_t downloadNow, curl_off_t uploadTotal, curl_off_t uploadNow) {
    auto* context = static_cast<ProgressContext*>(userdata);
    if (!context || !context->callback) {
        return 0;
    }
    const curl_off_t total = uploadTotal > 0 ? uploadTotal : downloadTotal;
    const curl_off_t now = uploadTotal > 0 ? uploadNow : downloadNow;
    return context->callback(
        static_cast<std::uint64_t>(std::max<curl_off_t>(0, now)),
        static_cast<std::uint64_t>(std::max<curl_off_t>(0, total))) ? 0 : 1;
}

std::string CurlErrorText(CURLcode code, const char* errorBuffer, long status, const std::vector<std::uint8_t>& response) {
    std::string detail = errorBuffer && *errorBuffer ? errorBuffer : curl_easy_strerror(code);
    if (status > 0) {
        detail += " HTTP " + std::to_string(status);
    }
    if (!response.empty()) {
        std::string body(response.begin(), response.end());
        if (body.size() > 300) {
            body.resize(300);
        }
        detail += " ";
        detail += body;
    }
    return detail;
}
#endif
}

GitHubReleaseClient::GitHubReleaseClient(AppConfig config, std::wstring token)
    : config_(std::move(config)), token_(std::move(token)) {
}

bool GitHubReleaseClient::TestConnection() {
    std::vector<AppStoreRelease> releases;
    return ListReleases(releases);
}

bool GitHubReleaseClient::ListReleases(std::vector<AppStoreRelease>& releases) {
    releases.clear();
    if (!ValidateSettings()) {
        return false;
    }
    std::wstring text;
    if (!RequestJson(ApiUrl(L"/repos/" + Trim(config_.appStoreOwner) + L"/" + Trim(config_.appStoreRepo) + L"/releases?per_page=100"), text)) {
        return false;
    }
    JsonValue root;
    std::wstring error;
    if (!ParseJson(text, root, error)) {
        lastError_ = error;
        return false;
    }
    if (!root.isArray()) {
        lastError_ = L"GitHub Releases 响应不是数组。";
        return false;
    }
    for (const auto& item : root.arrayValue) {
        AppStoreRelease release;
        if (ParseReleaseObject(item, release)) {
            releases.push_back(std::move(release));
        }
    }
    return true;
}

bool GitHubReleaseClient::GetReleaseByTag(const std::wstring& tag, AppStoreRelease& release) {
    if (!ValidateSettings()) {
        return false;
    }
    std::wstring text;
    long status = 0;
    if (!RequestJson(
            L"GET",
            ApiUrl(L"/repos/" + Trim(config_.appStoreOwner) + L"/" + Trim(config_.appStoreRepo) + L"/releases/tags/" + UrlEncode(tag)),
            {},
            text,
            &status)) {
        if (status == 404) {
            lastError_ = L"Release 不存在。";
        }
        return false;
    }
    JsonValue root;
    std::wstring error;
    if (!ParseJson(text, root, error) || !ParseReleaseObject(root, release)) {
        lastError_ = error.empty() ? L"解析 Release 失败。" : error;
        return false;
    }
    return true;
}

bool GitHubReleaseClient::CreateRelease(const AppStoreManifest& manifest, AppStoreRelease& release) {
    if (!ValidateSettings()) {
        return false;
    }
    std::wstringstream wideBody;
    wideBody << L"{"
             << L"\"tag_name\":\"" << JsonEscape(manifest.tag) << L"\","
             << L"\"target_commitish\":\"" << JsonEscape(config_.appStoreDefaultBranch.empty() ? L"main" : config_.appStoreDefaultBranch) << L"\","
             << L"\"name\":\"" << JsonEscape(manifest.name + L" " + manifest.version) << L"\","
             << L"\"body\":\"" << JsonEscape(manifest.summary.empty() ? manifest.description : manifest.summary) << L"\","
             << L"\"draft\":" << (manifest.draft ? L"true" : L"false") << L","
             << L"\"prerelease\":false"
             << L"}";
    std::wstring text;
    if (!RequestJson(
            L"POST",
            ApiUrl(L"/repos/" + Trim(config_.appStoreOwner) + L"/" + Trim(config_.appStoreRepo) + L"/releases"),
            WideToUtf8(wideBody.str()),
            text)) {
        return false;
    }
    JsonValue root;
    std::wstring error;
    if (!ParseJson(text, root, error) || !ParseReleaseObject(root, release)) {
        lastError_ = error.empty() ? L"解析创建 Release 响应失败。" : error;
        return false;
    }
    return true;
}

bool GitHubReleaseClient::UpdateReleaseMetadata(long long releaseId, const std::wstring& name, const std::wstring& body) {
    if (!ValidateSettings()) {
        return false;
    }
    std::wstringstream wideBody;
    wideBody << L"{\"name\":\"" << JsonEscape(name) << L"\",\"body\":\"" << JsonEscape(body) << L"\"}";
    std::wstring text;
    return RequestJson(
        L"PATCH",
        ApiUrl(L"/repos/" + Trim(config_.appStoreOwner) + L"/" + Trim(config_.appStoreRepo) + L"/releases/" + std::to_wstring(releaseId)),
        WideToUtf8(wideBody.str()),
        text);
}

bool GitHubReleaseClient::DeleteRelease(long long releaseId) {
    if (releaseId <= 0) {
        lastError_ = L"Release id 无效。";
        return false;
    }
    std::vector<std::uint8_t> response;
    long status = 0;
    if (!RequestBytes(
            L"DELETE",
            ApiUrl(L"/repos/" + Trim(config_.appStoreOwner) + L"/" + Trim(config_.appStoreRepo) + L"/releases/" + std::to_wstring(releaseId)),
            L"application/vnd.github+json",
            {},
            response,
            &status)) {
        return false;
    }
    return status == 204 || status == 200;
}

bool GitHubReleaseClient::DeleteAsset(long long assetId) {
    if (assetId <= 0) {
        lastError_ = L"Asset id 无效。";
        return false;
    }
    std::vector<std::uint8_t> response;
    long status = 0;
    if (!RequestBytes(
            L"DELETE",
            ApiUrl(L"/repos/" + Trim(config_.appStoreOwner) + L"/" + Trim(config_.appStoreRepo) + L"/releases/assets/" + std::to_wstring(assetId)),
            L"application/vnd.github+json",
            {},
            response,
            &status)) {
        return false;
    }
    return status == 204 || status == 200;
}

bool GitHubReleaseClient::UploadReleaseAsset(const AppStoreRelease& release, const std::filesystem::path& path, const std::wstring& assetName, AppStoreReleaseAsset& asset, TransferProgressCallback progress) {
    std::wstring upload = release.uploadUrl;
    const std::size_t brace = upload.find(L'{');
    if (brace != std::wstring::npos) {
        upload = upload.substr(0, brace);
    }
    if (upload.empty()) {
        upload = L"https://uploads.github.com/repos/" + Trim(config_.appStoreOwner) + L"/" + Trim(config_.appStoreRepo) + L"/releases/" + std::to_wstring(release.id) + L"/assets";
    }
    upload += L"?name=" + UrlEncode(assetName);
    std::vector<std::uint8_t> response;
    if (!UploadFileRequest(upload, path, L"application/vnd.github+json", response, std::move(progress))) {
        return false;
    }
    JsonValue root;
    std::wstring error;
    if (!ParseJson(Utf8ToWide(response), root, error) || !root.isObject()) {
        lastError_ = error.empty() ? L"解析 asset 上传响应失败。" : error;
        return false;
    }
    asset.id = Int64Value(root, L"id");
    asset.name = StringValue(root, L"name");
    asset.size = UInt64Value(root, L"size");
    asset.browserDownloadUrl = StringValue(root, L"browser_download_url");
    return asset.id > 0;
}

bool GitHubReleaseClient::DownloadAssetText(long long assetId, std::wstring& text) {
    std::vector<std::uint8_t> bytes;
    if (!DownloadAssetBytes(assetId, bytes)) {
        return false;
    }
    text = Utf8ToWide(bytes);
    if (text.empty()) {
        lastError_ = L"GitHub asset 不是有效 UTF-8 文本。";
        return false;
    }
    return true;
}

bool GitHubReleaseClient::DownloadAssetBytes(long long assetId, std::vector<std::uint8_t>& bytes) {
    if (assetId <= 0) {
        lastError_ = L"GitHub asset id 无效。";
        return false;
    }
    return RequestBytes(
        ApiUrl(L"/repos/" + Trim(config_.appStoreOwner) + L"/" + Trim(config_.appStoreRepo) + L"/releases/assets/" + std::to_wstring(assetId)),
        L"application/octet-stream",
        bytes);
}

bool GitHubReleaseClient::DownloadAssetFile(long long assetId, const std::filesystem::path& path, TransferProgressCallback progress) {
    if (assetId <= 0) {
        lastError_ = L"GitHub asset id 无效。";
        return false;
    }
    return RequestFile(
        ApiUrl(L"/repos/" + Trim(config_.appStoreOwner) + L"/" + Trim(config_.appStoreRepo) + L"/releases/assets/" + std::to_wstring(assetId)),
        L"application/octet-stream",
        path,
        std::move(progress));
}

bool GitHubReleaseClient::RequestJson(const std::wstring& url, std::wstring& text) {
    std::vector<std::uint8_t> bytes;
    if (!RequestBytes(url, L"application/vnd.github+json", bytes)) {
        return false;
    }
    text = Utf8ToWide(bytes);
    if (text.empty()) {
        lastError_ = L"GitHub API 响应不是有效 UTF-8。";
        return false;
    }
    return true;
}

bool GitHubReleaseClient::RequestJson(const std::wstring& method, const std::wstring& url, const std::string& body, std::wstring& text, long* statusOut) {
    std::vector<std::uint8_t> bytes;
    const std::vector<std::uint8_t> requestBody(body.begin(), body.end());
    if (!RequestBytes(method, url, L"application/vnd.github+json", requestBody, bytes, statusOut)) {
        return false;
    }
    text = Utf8ToWide(bytes);
    return true;
}

#ifndef QUATTRO_WITH_CURL
bool GitHubReleaseClient::RequestBytes(const std::wstring&, const std::wstring&, std::vector<std::uint8_t>& bytes) {
    bytes.clear();
    lastError_ = L"当前构建未启用 libcurl 网盘管理网络后端。请使用 vcpkg 构建。";
    return false;
}

bool GitHubReleaseClient::RequestBytes(const std::wstring&, const std::wstring&, const std::wstring&, const std::vector<std::uint8_t>&, std::vector<std::uint8_t>& bytes, long*) {
    bytes.clear();
    lastError_ = L"当前构建未启用 libcurl 网盘管理网络后端。请使用 vcpkg 构建。";
    return false;
}

bool GitHubReleaseClient::RequestFile(const std::wstring&, const std::wstring&, const std::filesystem::path&, TransferProgressCallback) {
    lastError_ = L"当前构建未启用 libcurl 网盘管理网络后端。请使用 vcpkg 构建。";
    return false;
}

bool GitHubReleaseClient::UploadFileRequest(const std::wstring&, const std::filesystem::path&, const std::wstring&, std::vector<std::uint8_t>& response, TransferProgressCallback) {
    response.clear();
    lastError_ = L"当前构建未启用 libcurl 网盘管理网络后端。请使用 vcpkg 构建。";
    return false;
}
#else
bool GitHubReleaseClient::RequestBytes(const std::wstring& url, const std::wstring& acceptHeader, std::vector<std::uint8_t>& bytes) {
    return RequestBytes(L"GET", url, acceptHeader, {}, bytes, nullptr);
}

bool GitHubReleaseClient::RequestBytes(const std::wstring& method, const std::wstring& url, const std::wstring& acceptHeader, const std::vector<std::uint8_t>& body, std::vector<std::uint8_t>& bytes, long* statusOut) {
    bytes.clear();
    if (statusOut) {
        *statusOut = 0;
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        lastError_ = L"初始化 libcurl 失败。";
        return false;
    }

    char errorBuffer[CURL_ERROR_SIZE]{};
    const std::string methodUtf8 = WideToUtf8(method);
    const std::string urlUtf8 = WideToUtf8(url);
    const std::string bodyText(body.begin(), body.end());
    MemorySink sink{&bytes};

    curl_slist* headers = nullptr;
    const std::string accept = "Accept: " + WideToUtf8(acceptHeader);
    headers = curl_slist_append(headers, "User-Agent: Quattro Drive Manager/1.0");
    headers = curl_slist_append(headers, accept.c_str());
    headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
    if (!Trim(token_).empty()) {
        const std::string auth = "Authorization: Bearer " + WideToUtf8(Trim(token_));
        headers = curl_slist_append(headers, auth.c_str());
    }
    if (!body.empty()) {
        headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");
    }

    curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    if (method == L"GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (method == L"POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyText.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(bodyText.size()));
    } else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, methodUtf8.c_str());
        if (!body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyText.data());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(bodyText.size()));
        }
    }

    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (statusOut) {
        *statusOut = status;
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK || status >= 400) {
        lastError_ = Utf8StringToWide(CurlErrorText(code, errorBuffer, status, bytes));
        if (lastError_.empty()) {
            lastError_ = L"GitHub 请求失败。";
        }
        return false;
    }
    return true;
}

bool GitHubReleaseClient::RequestFile(const std::wstring& url, const std::wstring& acceptHeader, const std::filesystem::path& path, TransferProgressCallback progress) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        lastError_ = L"无法创建下载文件: " + path.wstring();
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        lastError_ = L"初始化 libcurl 失败。";
        return false;
    }

    char errorBuffer[CURL_ERROR_SIZE]{};
    const std::string urlUtf8 = WideToUtf8(url);
    FileSink sink{&output};
    ProgressContext progressContext{std::move(progress)};
    std::vector<std::uint8_t> errorResponse;

    curl_slist* headers = nullptr;
    const std::string accept = "Accept: " + WideToUtf8(acceptHeader);
    headers = curl_slist_append(headers, "User-Agent: Quattro Drive Manager/1.0");
    headers = curl_slist_append(headers, accept.c_str());
    headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
    if (!Trim(token_).empty()) {
        const std::string auth = "Authorization: Bearer " + WideToUtf8(Trim(token_));
        headers = curl_slist_append(headers, auth.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, progressContext.callback ? 0L : 1L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressContext);

    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    output.close();
    if (code != CURLE_OK || status >= 400) {
        std::filesystem::remove(path, ec);
        lastError_ = Utf8StringToWide(CurlErrorText(code, errorBuffer, status, errorResponse));
        if (lastError_.empty()) {
            lastError_ = code == CURLE_ABORTED_BY_CALLBACK ? L"传输已取消。" : L"GitHub 下载失败。";
        }
        return false;
    }
    return true;
}

bool GitHubReleaseClient::UploadFileRequest(const std::wstring& url, const std::filesystem::path& path, const std::wstring& acceptHeader, std::vector<std::uint8_t>& response, TransferProgressCallback progress) {
    response.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        lastError_ = L"无法读取上传文件: " + path.wstring();
        return false;
    }
    std::error_code ec;
    const auto fileSize = std::filesystem::file_size(path, ec);
    if (ec || fileSize == 0) {
        lastError_ = L"不能上传空 asset。";
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        lastError_ = L"初始化 libcurl 失败。";
        return false;
    }

    char errorBuffer[CURL_ERROR_SIZE]{};
    const std::string urlUtf8 = WideToUtf8(url);
    MemorySink sink{&response};
    UploadSource source{&input};
    ProgressContext progressContext{std::move(progress)};

    curl_slist* headers = nullptr;
    const std::string accept = "Accept: " + WideToUtf8(acceptHeader);
    headers = curl_slist_append(headers, "User-Agent: Quattro Drive Manager/1.0");
    headers = curl_slist_append(headers, accept.c_str());
    headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    if (!Trim(token_).empty()) {
        const std::string auth = "Authorization: Bearer " + WideToUtf8(Trim(token_));
        headers = curl_slist_append(headers, auth.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, ReadFileCallback);
    curl_easy_setopt(curl, CURLOPT_READDATA, &source);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(fileSize));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, progressContext.callback ? 0L : 1L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressContext);

    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK || status >= 400) {
        lastError_ = Utf8StringToWide(CurlErrorText(code, errorBuffer, status, response));
        if (lastError_.empty()) {
            lastError_ = code == CURLE_ABORTED_BY_CALLBACK ? L"传输已取消。" : L"GitHub 上传失败。";
        }
        return false;
    }
    return true;
}
#endif

std::wstring GitHubReleaseClient::ApiUrl(const std::wstring& path) const {
    return L"https://api.github.com" + path;
}

bool GitHubReleaseClient::ValidateSettings() {
    if (Trim(config_.appStoreOwner).empty()) {
        lastError_ = L"网盘管理 GitHub Owner 未配置。";
        return false;
    }
    if (Trim(config_.appStoreRepo).empty()) {
        lastError_ = L"网盘管理 GitHub Repo 未配置。";
        return false;
    }
    return true;
}
