#include "WebDavClient.h"

#include "Utilities.h"

#ifdef QUATTRO_WITH_WEBDAV_LIBS
#include <curl/curl.h>
#include <pugixml.hpp>
#endif

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>

#ifndef QUATTRO_WITH_WEBDAV_LIBS
namespace {
std::wstring NormalizePath(std::wstring path) {
    path = Trim(path);
    std::replace(path.begin(), path.end(), L'\\', L'/');
    while (path.find(L"//") != std::wstring::npos) {
        path = ReplaceAll(path, L"//", L"/");
    }
    return path;
}
}

WebDavClient::WebDavClient(AppConfig config, std::wstring password)
    : config_(std::move(config)), password_(std::move(password)) {
}

bool WebDavClient::TestConnection() {
    lastError_ = L"当前构建未启用 libcurl/pugixml WebDAV 后端。请通过 vcpkg 安装依赖后重新配置构建。";
    return false;
}

bool WebDavClient::EnsureDirectory(const std::wstring&) {
    return TestConnection();
}

bool WebDavClient::ListFiles(const std::wstring&, std::vector<WebDavRemoteFile>& files) {
    files.clear();
    return TestConnection();
}

bool WebDavClient::UploadFile(const std::filesystem::path&, const std::wstring&) {
    return TestConnection();
}

bool WebDavClient::DownloadFile(const std::wstring&, const std::filesystem::path&) {
    return TestConnection();
}

bool WebDavClient::DeleteRemoteFile(const std::wstring&) {
    return TestConnection();
}

std::vector<WebDavRemoteFile> WebDavClient::ParsePropFindResponse(const std::string&) {
    return {};
}

std::wstring WebDavClient::FileNameFromHref(const std::wstring& href) {
    std::wstring value = href;
    const std::size_t query = value.find(L'?');
    if (query != std::wstring::npos) {
        value = value.substr(0, query);
    }
    while (!value.empty() && value.back() == L'/') {
        value.pop_back();
    }
    const std::size_t slash = value.find_last_of(L"/\\");
    return slash == std::wstring::npos ? value : value.substr(slash + 1);
}

std::wstring WebDavClient::CombineRemotePath(const std::wstring& directory, const std::wstring& fileName) {
    std::wstring left = NormalizePath(directory);
    std::wstring right = NormalizePath(fileName);
    while (!right.empty() && right.front() == L'/') {
        right.erase(right.begin());
    }
    if (left.empty() || left == L"/") {
        return L"/" + right;
    }
    while (!left.empty() && left.back() == L'/') {
        left.pop_back();
    }
    return left + L"/" + right;
}

bool WebDavClient::Request(
    const std::wstring&,
    const std::wstring&,
    RequestBody*,
    const std::vector<std::string>&,
    std::vector<std::uint8_t>*,
    long*) {
    return TestConnection();
}

std::wstring WebDavClient::UrlForRemotePath(const std::wstring& remotePath) const {
    return remotePath;
}

bool WebDavClient::ValidateSettings() {
    return true;
}

void WebDavClient::SetCurlError(const std::wstring&, int, long, const std::string&) {
}

#else
namespace {
std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string text(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), text.data(), length, nullptr, nullptr);
    return text;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring text(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), text.data(), length);
    return text;
}

std::vector<std::uint8_t> ReadBinaryFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool WriteBinaryFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    if (!data.empty()) {
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    return static_cast<bool>(file);
}

std::string LocalName(const char* name) {
    std::string value = name ? name : "";
    const std::size_t colon = value.find(':');
    if (colon != std::string::npos) {
        value = value.substr(colon + 1);
    }
    return value;
}

bool NodeIs(const pugi::xml_node& node, const char* name) {
    return LocalName(node.name()) == name;
}

pugi::xml_node FirstChildByLocalName(const pugi::xml_node& parent, const char* name) {
    for (pugi::xml_node child : parent.children()) {
        if (NodeIs(child, name)) {
            return child;
        }
    }
    return {};
}

pugi::xml_node DescendantByLocalName(const pugi::xml_node& parent, const char* name) {
    for (pugi::xml_node child : parent.children()) {
        if (NodeIs(child, name)) {
            return child;
        }
        pugi::xml_node nested = DescendantByLocalName(child, name);
        if (nested) {
            return nested;
        }
    }
    return {};
}

bool HasCollectionResourceType(const pugi::xml_node& prop) {
    pugi::xml_node resourceType = DescendantByLocalName(prop, "resourcetype");
    if (!resourceType) {
        return false;
    }
    return static_cast<bool>(DescendantByLocalName(resourceType, "collection"));
}

std::wstring NormalizePath(std::wstring path) {
    path = Trim(path);
    std::replace(path.begin(), path.end(), L'\\', L'/');
    while (path.find(L"//") != std::wstring::npos) {
        path = ReplaceAll(path, L"//", L"/");
    }
    return path;
}

std::vector<std::wstring> SplitRemotePath(const std::wstring& path) {
    std::vector<std::wstring> parts;
    std::wstring normalized = NormalizePath(path);
    std::size_t start = 0;
    while (start < normalized.size()) {
        while (start < normalized.size() && normalized[start] == L'/') {
            ++start;
        }
        const std::size_t end = normalized.find(L'/', start);
        std::wstring part = normalized.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (!part.empty()) {
            parts.push_back(part);
        }
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }
    return parts;
}

size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* output = static_cast<std::vector<std::uint8_t>*>(userdata);
    const size_t bytes = size * nmemb;
    output->insert(output->end(), reinterpret_cast<std::uint8_t*>(ptr), reinterpret_cast<std::uint8_t*>(ptr) + bytes);
    return bytes;
}

size_t ReadCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<WebDavClient::RequestBody*>(userdata);
    const size_t capacity = size * nmemb;
    if (capacity == 0) {
        return 0;
    }

    const std::uint8_t* source = nullptr;
    std::size_t available = 0;
    if (body->kind == WebDavClient::BodyKind::Upload) {
        source = body->upload.data();
        available = body->upload.size();
    } else if (body->kind == WebDavClient::BodyKind::Text) {
        source = reinterpret_cast<const std::uint8_t*>(body->text.data());
        available = body->text.size();
    }
    if (!source || body->offset >= available) {
        return 0;
    }
    const size_t copied = std::min(capacity, available - body->offset);
    std::memcpy(ptr, source + body->offset, copied);
    body->offset += copied;
    return copied;
}
}

WebDavClient::WebDavClient(AppConfig config, std::wstring password)
    : config_(std::move(config)), password_(std::move(password)) {
}

bool WebDavClient::TestConnection() {
    std::vector<WebDavRemoteFile> files;
    return ListFiles(config_.webDavRemotePath, files);
}

bool WebDavClient::EnsureDirectory(const std::wstring& remotePath) {
    if (!ValidateSettings()) {
        return false;
    }
    const auto parts = SplitRemotePath(remotePath);
    std::wstring current;
    for (const auto& part : parts) {
        current = CombineRemotePath(current, part);
        long status = 0;
        if (!Request(L"MKCOL", current, nullptr, {}, nullptr, &status)) {
            if (status == 405) {
                continue;
            }
            if (lastError_.empty()) {
                lastError_ = L"创建 WebDAV 目录请求失败。";
                if (status > 0) {
                    lastError_ += L" HTTP " + std::to_wstring(status);
                }
            }
            return false;
        }
        if (status != 201 && status != 405) {
            lastError_ = L"创建 WebDAV 目录失败，HTTP " + std::to_wstring(status) + L": " + current;
            return false;
        }
    }
    return true;
}

bool WebDavClient::ListFiles(const std::wstring& remotePath, std::vector<WebDavRemoteFile>& files) {
    files.clear();
    RequestBody body;
    body.kind = BodyKind::Text;
    body.text =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\">"
        "<d:prop><d:resourcetype/><d:getcontentlength/><d:getlastmodified/></d:prop>"
        "</d:propfind>";
    std::vector<std::uint8_t> response;
    long status = 0;
    if (!Request(L"PROPFIND", remotePath, &body, {"Depth: 1", "Content-Type: application/xml; charset=utf-8"}, &response, &status)) {
        return false;
    }
    if (status != 207 && status != 200) {
        lastError_ = L"读取 WebDAV 目录失败，HTTP " + std::to_wstring(status);
        return false;
    }
    std::string xml(response.begin(), response.end());
    files = ParsePropFindResponse(xml);
    return true;
}

bool WebDavClient::UploadFile(const std::filesystem::path& localPath, const std::wstring& remotePath) {
    RequestBody body;
    body.kind = BodyKind::Upload;
    body.upload = ReadBinaryFile(localPath);
    if (body.upload.empty() && FileExists(localPath)) {
        lastError_ = L"WebDAV 上传文件为空: " + localPath.wstring();
        return false;
    }
    long status = 0;
    if (!Request(L"PUT", remotePath, &body, {"Content-Type: application/octet-stream"}, nullptr, &status)) {
        return false;
    }
    if (status < 200 || status >= 300) {
        lastError_ = L"上传 WebDAV 备份失败，HTTP " + std::to_wstring(status);
        return false;
    }
    return true;
}

bool WebDavClient::DownloadFile(const std::wstring& remotePath, const std::filesystem::path& localPath) {
    std::vector<std::uint8_t> response;
    long status = 0;
    if (!Request(L"GET", remotePath, nullptr, {}, &response, &status)) {
        return false;
    }
    if (status != 200) {
        lastError_ = L"下载 WebDAV 备份失败，HTTP " + std::to_wstring(status);
        return false;
    }
    if (response.empty()) {
        lastError_ = L"下载的 WebDAV 备份为空。";
        return false;
    }
    if (!WriteBinaryFile(localPath, response)) {
        lastError_ = L"写入 WebDAV 临时备份失败: " + localPath.wstring();
        return false;
    }
    return true;
}

bool WebDavClient::DeleteRemoteFile(const std::wstring& remotePath) {
    long status = 0;
    if (!Request(L"DELETE", remotePath, nullptr, {}, nullptr, &status)) {
        return false;
    }
    if (status != 200 && status != 202 && status != 204 && status != 404) {
        lastError_ = L"删除 WebDAV 旧备份失败，HTTP " + std::to_wstring(status);
        return false;
    }
    return true;
}

std::vector<WebDavRemoteFile> WebDavClient::ParsePropFindResponse(const std::string& xml) {
    std::vector<WebDavRemoteFile> files;
    pugi::xml_document doc;
    if (!doc.load_buffer(xml.data(), xml.size())) {
        return files;
    }
    pugi::xml_node root = doc.document_element();
    for (pugi::xml_node response : root.children()) {
        if (!NodeIs(response, "response")) {
            continue;
        }
        pugi::xml_node hrefNode = FirstChildByLocalName(response, "href");
        if (!hrefNode) {
            continue;
        }
        pugi::xml_node prop = DescendantByLocalName(response, "prop");
        WebDavRemoteFile file;
        file.href = Utf8ToWide(hrefNode.child_value());
        file.name = FileNameFromHref(file.href);
        file.collection = prop && HasCollectionResourceType(prop);
        if (pugi::xml_node sizeNode = DescendantByLocalName(prop, "getcontentlength")) {
            try {
                file.size = static_cast<std::uint64_t>(std::stoull(sizeNode.child_value()));
            } catch (...) {
                file.size = 0;
            }
        }
        if (pugi::xml_node modifiedNode = DescendantByLocalName(prop, "getlastmodified")) {
            file.lastModified = Utf8ToWide(modifiedNode.child_value());
        }
        files.push_back(std::move(file));
    }
    return files;
}

std::wstring WebDavClient::FileNameFromHref(const std::wstring& href) {
    std::wstring value = href;
    const std::size_t query = value.find(L'?');
    if (query != std::wstring::npos) {
        value = value.substr(0, query);
    }
    while (!value.empty() && value.back() == L'/') {
        value.pop_back();
    }
    const std::size_t slash = value.find_last_of(L"/\\");
    return slash == std::wstring::npos ? value : value.substr(slash + 1);
}

std::wstring WebDavClient::CombineRemotePath(const std::wstring& directory, const std::wstring& fileName) {
    std::wstring left = NormalizePath(directory);
    std::wstring right = NormalizePath(fileName);
    while (!right.empty() && right.front() == L'/') {
        right.erase(right.begin());
    }
    if (left.empty() || left == L"/") {
        return L"/" + right;
    }
    while (!left.empty() && left.back() == L'/') {
        left.pop_back();
    }
    return left + L"/" + right;
}

bool WebDavClient::Request(
    const std::wstring& method,
    const std::wstring& remotePath,
    RequestBody* body,
    const std::vector<std::string>& headers,
    std::vector<std::uint8_t>* response,
    long* statusCode) {
    if (!ValidateSettings()) {
        return false;
    }
    if (statusCode) {
        *statusCode = 0;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        lastError_ = L"初始化 libcurl 失败。";
        return false;
    }

    std::vector<std::uint8_t> sink;
    std::vector<std::uint8_t>* target = response ? response : &sink;
    char errorBuffer[CURL_ERROR_SIZE]{};
    const std::string url = WideToUtf8(UrlForRemotePath(remotePath));
    const std::string user = WideToUtf8(config_.webDavUserName);
    const std::string password = WideToUtf8(password_);
    const std::string methodUtf8 = WideToUtf8(method);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Quattro WebDAV Backup/1.0");
    curl_easy_setopt(curl, CURLOPT_USERNAME, user.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, target);

    curl_slist* headerList = nullptr;
    for (const auto& header : headers) {
        headerList = curl_slist_append(headerList, header.c_str());
    }
    if (headerList) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    }

    if (method == L"GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (method == L"PUT") {
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, ReadCallback);
        curl_easy_setopt(curl, CURLOPT_READDATA, body);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(body ? body->upload.size() : 0));
    } else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, methodUtf8.c_str());
        if (body && body->kind != BodyKind::None) {
            curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
            curl_easy_setopt(curl, CURLOPT_READFUNCTION, ReadCallback);
            curl_easy_setopt(curl, CURLOPT_READDATA, body);
            const std::size_t bodySize = body->kind == BodyKind::Text ? body->text.size() : body->upload.size();
            curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(bodySize));
        }
    }

    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (statusCode) {
        *statusCode = status;
    }
    if (headerList) {
        curl_slist_free_all(headerList);
    }
    curl_easy_cleanup(curl);
    if (code != CURLE_OK) {
        std::string detail = errorBuffer;
        if (detail.empty()) {
            detail = curl_easy_strerror(code);
        }
        SetCurlError(L"WebDAV 请求失败", static_cast<int>(code), status, detail);
        return false;
    }
    return true;
}

std::wstring WebDavClient::UrlForRemotePath(const std::wstring& remotePath) const {
    std::wstring base = Trim(config_.webDavUrl);
    std::wstring path = NormalizePath(remotePath);
    while (!path.empty() && path.front() == L'/') {
        path.erase(path.begin());
    }
    while (!base.empty() && base.back() == L'/') {
        base.pop_back();
    }
    if (path.empty()) {
        return base + L"/";
    }
    return base + L"/" + path;
}

bool WebDavClient::ValidateSettings() {
    if (Trim(config_.webDavUrl).empty()) {
        lastError_ = L"WebDAV 地址未配置。";
        return false;
    }
    if (Trim(config_.webDavUserName).empty()) {
        lastError_ = L"WebDAV 用户名未配置。";
        return false;
    }
    if (password_.empty()) {
        lastError_ = L"WebDAV 密码未配置。";
        return false;
    }
    return true;
}

void WebDavClient::SetCurlError(const std::wstring& action, int curlCode, long statusCode, const std::string& detail) {
    lastError_ = action + L": curl " + std::to_wstring(curlCode);
    if (statusCode > 0) {
        lastError_ += L", HTTP " + std::to_wstring(statusCode);
    }
    if (!detail.empty()) {
        lastError_ += L"，" + Utf8ToWide(detail);
    }
}
#endif
