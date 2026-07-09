#include "LocalHttpServerService.h"

#ifdef QUATTRO_WITH_HTTPLIB
#include <winsock2.h>
#include <ws2tcpip.h>
#include <httplib.h>
#endif

#include "AppLog.h"
#include "Utilities.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include <shellapi.h>
#include <windows.h>

namespace {
constexpr const wchar_t* kDetailSection = L"http";

std::string WideToUtf8Local(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWideLocal(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring ReadIniString(const std::filesystem::path& path, const wchar_t* key, const wchar_t* fallback) {
    std::wstring buffer(512, L'\0');
    DWORD copied = GetPrivateProfileStringW(kDetailSection, key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    while (copied == buffer.size() - 1) {
        buffer.resize(buffer.size() * 2);
        copied = GetPrivateProfileStringW(kDetailSection, key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    }
    buffer.resize(copied);
    return buffer;
}

bool ReadIniBool(const std::filesystem::path& path, const wchar_t* key, bool fallback) {
    return GetPrivateProfileIntW(kDetailSection, key, fallback ? 1 : 0, path.c_str()) != 0;
}

int ReadIniInt(const std::filesystem::path& path, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(kDetailSection, key, fallback, path.c_str()));
}

bool WriteUtf8TextFile(const std::filesystem::path& path, const std::wstring& text, std::wstring& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = L"创建目录失败: " + Utf8ToWideLocal(ec.message());
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = L"写入文件失败: " + path.wstring();
        return false;
    }
    const std::string bytes = WideToUtf8Local(text);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

std::wstring DefaultDetailConfigText() {
    return
        L"[http]\r\n"
        L"; Detailed settings are loaded when the HTTP server starts or restarts.\r\n"
        L"GuestRead=1\r\n"
        L"GuestUpload=0\r\n"
        L"GuestWrite=0\r\n"
        L"AuthEnabled=0\r\n"
        L"Username=admin\r\n"
        L"Password=\r\n"
        L"DirectoryListing=1\r\n"
        L"ShowHiddenFiles=0\r\n"
        L"AllowPost=1\r\n"
        L"AllowPut=0\r\n"
        L"AllowDelete=0\r\n"
        L"AllowRemoteOpen=0\r\n"
        L"DefaultDisposition=inline\r\n"
        L"OpenExtensions=.html .htm .txt .md .json .css .js .png .jpg .jpeg .gif .svg .pdf .mp4 .mp3\r\n"
        L"DownloadExtensions=.exe .msi .zip .7z .rar .dll .bat .cmd .ps1 .reg\r\n"
        L"MaxUploadBytes=536870912\r\n"
        L"MimeMap=.html=text/html; charset=utf-8|.htm=text/html; charset=utf-8|.txt=text/plain; charset=utf-8|.md=text/markdown; charset=utf-8|.css=text/css; charset=utf-8|.js=application/javascript; charset=utf-8|.json=application/json; charset=utf-8|.png=image/png|.jpg=image/jpeg|.jpeg=image/jpeg|.gif=image/gif|.svg=image/svg+xml|.pdf=application/pdf|.zip=application/zip|.mp4=video/mp4|.mp3=audio/mpeg\r\n";
}

std::vector<std::wstring> SplitWords(const std::wstring& value) {
    std::vector<std::wstring> items;
    std::wstringstream stream(value);
    std::wstring item;
    while (stream >> item) {
        items.push_back(ToLower(item));
    }
    return items;
}

bool ContainsExt(const std::vector<std::wstring>& items, std::wstring ext) {
    ext = ToLower(ext);
    return std::find(items.begin(), items.end(), ext) != items.end();
}

std::unordered_map<std::wstring, std::string> ParseMimeMap(const std::wstring& value) {
    std::unordered_map<std::wstring, std::string> map;
    std::size_t start = 0;
    while (start < value.size()) {
        const std::size_t end = value.find(L'|', start);
        const std::wstring token = Trim(value.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        const std::size_t eq = token.find(L'=');
        if (eq != std::wstring::npos) {
            std::wstring ext = ToLower(Trim(token.substr(0, eq)));
            if (!ext.empty() && ext[0] != L'.') {
                ext = L"." + ext;
            }
            const std::wstring mime = Trim(token.substr(eq + 1));
            if (!ext.empty() && !mime.empty()) {
                map[ext] = WideToUtf8Local(mime);
            }
        }
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }
    return map;
}

struct DetailConfig {
    bool guestRead = true;
    bool guestUpload = false;
    bool guestWrite = false;
    bool authEnabled = false;
    std::wstring username = L"admin";
    std::wstring password;
    bool directoryListing = true;
    bool showHiddenFiles = false;
    bool allowPost = true;
    bool allowPut = false;
    bool allowRemoteOpen = false;
    bool defaultDownload = false;
    std::uint64_t maxUploadBytes = 512ull * 1024ull * 1024ull;
    std::vector<std::wstring> openExtensions;
    std::vector<std::wstring> downloadExtensions;
    std::unordered_map<std::wstring, std::string> mimeMap;
};

DetailConfig LoadDetailConfig(const std::filesystem::path& rootPath) {
    std::wstring ignored;
    LocalHttpServerService::EnsureDetailConfig(rootPath, ignored);
    const auto path = LocalHttpServerService::DetailConfigPath(rootPath);
    DetailConfig config;
    config.guestRead = ReadIniBool(path, L"GuestRead", config.guestRead);
    config.guestUpload = ReadIniBool(path, L"GuestUpload", config.guestUpload);
    config.guestWrite = ReadIniBool(path, L"GuestWrite", config.guestWrite);
    config.authEnabled = ReadIniBool(path, L"AuthEnabled", config.authEnabled);
    config.username = ReadIniString(path, L"Username", config.username.c_str());
    config.password = ReadIniString(path, L"Password", L"");
    config.directoryListing = ReadIniBool(path, L"DirectoryListing", config.directoryListing);
    config.showHiddenFiles = ReadIniBool(path, L"ShowHiddenFiles", config.showHiddenFiles);
    config.allowPost = ReadIniBool(path, L"AllowPost", config.allowPost);
    config.allowPut = ReadIniBool(path, L"AllowPut", config.allowPut);
    config.allowRemoteOpen = ReadIniBool(path, L"AllowRemoteOpen", config.allowRemoteOpen);
    config.defaultDownload = ToLower(ReadIniString(path, L"DefaultDisposition", L"inline")) == L"attachment";
    config.maxUploadBytes = static_cast<std::uint64_t>(std::max(0, ReadIniInt(path, L"MaxUploadBytes", static_cast<int>(config.maxUploadBytes))));
    config.openExtensions = SplitWords(ReadIniString(path, L"OpenExtensions", L".html .htm .txt .md .json .css .js .png .jpg .jpeg .gif .svg .pdf .mp4 .mp3"));
    config.downloadExtensions = SplitWords(ReadIniString(path, L"DownloadExtensions", L".exe .msi .zip .7z .rar .dll .bat .cmd .ps1 .reg"));
    config.mimeMap = ParseMimeMap(ReadIniString(path, L"MimeMap", L""));
    return config;
}

std::string HtmlEscape(const std::wstring& value) {
    std::string result;
    for (wchar_t ch : value) {
        switch (ch) {
        case L'&': result += "&amp;"; break;
        case L'<': result += "&lt;"; break;
        case L'>': result += "&gt;"; break;
        case L'"': result += "&quot;"; break;
        default: result += WideToUtf8Local(std::wstring(1, ch)); break;
        }
    }
    return result;
}

std::string UrlEncodePath(const std::wstring& value) {
    const std::string utf8 = WideToUtf8Local(value);
    std::ostringstream out;
    static constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char ch : utf8) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '/') {
            out << static_cast<char>(ch);
        } else {
            out << '%' << hex[ch >> 4] << hex[ch & 0x0F];
        }
    }
    return out.str();
}

std::wstring DecodeUrlPath(std::string path) {
    std::string bytes;
    bytes.reserve(path.size());
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size() && std::isxdigit(static_cast<unsigned char>(path[i + 1])) && std::isxdigit(static_cast<unsigned char>(path[i + 2]))) {
            const std::string hex = path.substr(i + 1, 2);
            bytes.push_back(static_cast<char>(std::strtol(hex.c_str(), nullptr, 16)));
            i += 2;
        } else {
            bytes.push_back(path[i] == '+' ? ' ' : path[i]);
        }
    }
    return Utf8ToWideLocal(bytes);
}

std::optional<std::pair<std::string, std::string>> DecodeBasicAuthHeader(const std::string& header) {
    if (header.rfind("Basic ", 0) != 0) {
        return std::nullopt;
    }
    const std::string encoded = header.substr(6);
    std::string decoded;
    decoded.reserve(encoded.size() * 3 / 4);
    int value = 0;
    int bits = -8;
    for (unsigned char ch : encoded) {
        if (ch == '=') {
            break;
        }
        int digit = -1;
        if (ch >= 'A' && ch <= 'Z') {
            digit = ch - 'A';
        } else if (ch >= 'a' && ch <= 'z') {
            digit = ch - 'a' + 26;
        } else if (ch >= '0' && ch <= '9') {
            digit = ch - '0' + 52;
        } else if (ch == '+') {
            digit = 62;
        } else if (ch == '/') {
            digit = 63;
        } else {
            continue;
        }
        value = (value << 6) + digit;
        bits += 6;
        if (bits >= 0) {
            decoded.push_back(static_cast<char>((value >> bits) & 0xFF));
            bits -= 8;
        }
    }
    const std::size_t separator = decoded.find(':');
    if (separator == std::string::npos) {
        return std::nullopt;
    }
    return std::make_pair(decoded.substr(0, separator), decoded.substr(separator + 1));
}

bool IsHiddenPath(const std::filesystem::path& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
}

bool IsWithinRoot(const std::filesystem::path& root, const std::filesystem::path& target) {
    std::error_code ec;
    const auto rootCanonical = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        return false;
    }
    const auto targetCanonical = std::filesystem::weakly_canonical(target, ec);
    if (ec) {
        return false;
    }
    auto rootIt = rootCanonical.begin();
    auto targetIt = targetCanonical.begin();
    for (; rootIt != rootCanonical.end(); ++rootIt, ++targetIt) {
        if (targetIt == targetCanonical.end() || ToLower(rootIt->wstring()) != ToLower(targetIt->wstring())) {
            return false;
        }
    }
    return true;
}

std::string FileTimeText(const std::filesystem::path& path) {
    std::error_code ec;
    const auto fileTime = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return "";
    }
    const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        fileTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    std::time_t tt = std::chrono::system_clock::to_time_t(systemTime);
    std::tm local{};
    localtime_s(&local, &tt);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
    return buffer;
}

std::string SizeText(std::uintmax_t bytes) {
    if (bytes >= 1024ull * 1024ull) {
        return std::to_string((bytes + 1024ull * 1024ull - 1) / (1024ull * 1024ull)) + " MB";
    }
    if (bytes >= 1024ull) {
        return std::to_string((bytes + 1023ull) / 1024ull) + " KB";
    }
    return std::to_string(bytes) + " B";
}

#ifdef QUATTRO_WITH_HTTPLIB
bool CheckBasicAuth(const httplib::Request& req, const DetailConfig& config) {
    if (!config.authEnabled || config.username.empty()) {
        return false;
    }
    if (!req.has_header("Authorization")) {
        return false;
    }
    const auto credentials = DecodeBasicAuthHeader(req.get_header_value("Authorization"));
    return credentials &&
        credentials->first == WideToUtf8Local(config.username) &&
        credentials->second == WideToUtf8Local(config.password);
}

bool HasReadAccess(const httplib::Request& req, const DetailConfig& config) {
    return config.guestRead || CheckBasicAuth(req, config);
}

bool HasUploadAccess(const httplib::Request& req, const DetailConfig& config) {
    return config.guestUpload || CheckBasicAuth(req, config);
}

bool HasWriteAccess(const httplib::Request& req, const DetailConfig& config) {
    return config.guestWrite || CheckBasicAuth(req, config);
}

void RequireAuth(httplib::Response& res) {
    res.status = 401;
    res.set_header("WWW-Authenticate", "Basic realm=\"Quattro HTTP\"");
    res.set_content("Authentication required.", "text/plain; charset=utf-8");
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> ParseRange(const httplib::Request& req, std::uint64_t size) {
    if (!req.has_header("Range") || size == 0) {
        return std::nullopt;
    }
    const std::string range = req.get_header_value("Range");
    if (range.rfind("bytes=", 0) != 0) {
        return std::nullopt;
    }
    const std::string spec = range.substr(6);
    const std::size_t dash = spec.find('-');
    if (dash == std::string::npos) {
        return std::nullopt;
    }
    try {
        std::uint64_t start = 0;
        std::uint64_t end = size - 1;
        if (dash == 0) {
            const std::uint64_t suffix = std::stoull(spec.substr(1));
            start = suffix >= size ? 0 : size - suffix;
        } else {
            start = std::stoull(spec.substr(0, dash));
            if (dash + 1 < spec.size()) {
                end = std::stoull(spec.substr(dash + 1));
            }
        }
        if (start >= size || end < start) {
            return std::nullopt;
        }
        end = std::min(end, size - 1);
        return std::make_pair(start, end);
    } catch (...) {
        return std::nullopt;
    }
}

std::string MimeFor(const std::filesystem::path& path, const DetailConfig& config) {
    const std::wstring ext = ToLower(path.extension().wstring());
    const auto it = config.mimeMap.find(ext);
    return it == config.mimeMap.end() ? "application/octet-stream" : it->second;
}

std::string DispositionFor(const std::filesystem::path& path, const DetailConfig& config) {
    const std::wstring ext = ToLower(path.extension().wstring());
    const bool download = ContainsExt(config.downloadExtensions, ext) || (!ContainsExt(config.openExtensions, ext) && config.defaultDownload);
    return download ? "attachment" : "inline";
}

std::string DirectoryListingHtml(const std::filesystem::path& root, const std::filesystem::path& dir, const std::string& requestPath, const DetailConfig& config) {
    std::vector<std::filesystem::directory_entry> entries;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!config.showHiddenFiles && IsHiddenPath(entry.path())) {
            continue;
        }
        entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        if (left.is_directory() != right.is_directory()) {
            return left.is_directory();
        }
        return ToLower(left.path().filename().wstring()) < ToLower(right.path().filename().wstring());
    });

    std::ostringstream html;
    html << "<!doctype html><html><head><meta charset=\"utf-8\"><title>Quattro HTTP</title>"
         << "<style>body{font-family:Segoe UI,Arial,sans-serif;margin:28px;color:#1f2933;background:#f7f9fb}h1{font-size:20px}.bar{display:flex;gap:8px;align-items:center;margin:16px 0}.list{background:white;border:1px solid #d8dee8;border-radius:8px;overflow:hidden}a{color:#185abc;text-decoration:none}.row{display:grid;grid-template-columns:1fr 120px 160px;gap:12px;padding:10px 14px;border-top:1px solid #edf1f5}.row:first-child{border-top:0}.muted{color:#6b7280}.upload{background:white;border:1px solid #d8dee8;border-radius:8px;padding:12px;margin:12px 0}</style></head><body>";
    html << "<h1>Index of " << HtmlEscape(Utf8ToWideLocal(requestPath.empty() ? "/" : requestPath)) << "</h1>";
    if (dir != root) {
        std::string parent = requestPath;
        if (!parent.empty() && parent.back() == '/') {
            parent.pop_back();
        }
        const auto pos = parent.find_last_of('/');
        parent = pos == std::string::npos ? "/" : parent.substr(0, pos + 1);
        html << "<div class=\"bar\"><a href=\"" << parent << "\">.. Parent Directory</a></div>";
    }
    if (config.guestUpload || config.authEnabled) {
        html << "<form class=\"upload\" method=\"post\" action=\"/_quattro/upload?path=" << requestPath << "\" enctype=\"multipart/form-data\">"
             << "<input type=\"file\" name=\"file\" multiple> <button type=\"submit\">Upload</button></form>";
    }
    html << "<div class=\"list\">";
    for (const auto& entry : entries) {
        const std::wstring name = entry.path().filename().wstring();
        const bool dirEntry = entry.is_directory();
        std::string href = requestPath;
        if (href.empty() || href.back() != '/') {
            href += "/";
        }
        href += UrlEncodePath(name);
        if (dirEntry) {
            href += "/";
        }
        html << "<div class=\"row\"><a href=\"" << href << "\">" << HtmlEscape(name + (dirEntry ? L"/" : L"")) << "</a><span class=\"muted\">";
        if (!dirEntry) {
            html << SizeText(entry.file_size(ec));
        }
        html << "</span><span class=\"muted\">" << FileTimeText(entry.path()) << "</span></div>";
    }
    html << "</div></body></html>";
    return html.str();
}
#endif
}

struct LocalHttpServerService::Impl {
#ifdef QUATTRO_WITH_HTTPLIB
    std::unique_ptr<httplib::Server> server;
#endif
    std::thread thread;
    std::atomic_bool running{false};
};

LocalHttpServerService::LocalHttpServerService()
    : impl_(std::make_unique<Impl>()) {
}

LocalHttpServerService::~LocalHttpServerService() {
    Stop();
}

std::filesystem::path LocalHttpServerService::DefaultRootPath(const std::filesystem::path& rootBaseDirectory) {
    return rootBaseDirectory / L"quattro_web";
}

std::filesystem::path LocalHttpServerService::DetailConfigDirectory() {
    return QuattroUserConfigDirectory();
}

std::filesystem::path LocalHttpServerService::DetailConfigPath(const std::filesystem::path& rootPath) {
    (void)rootPath;
    return DetailConfigDirectory() / L"http-server.ini";
}

bool LocalHttpServerService::EnsureDetailConfig(const std::filesystem::path& rootPath, std::wstring& error) {
    std::error_code ec;
    std::filesystem::create_directories(rootPath, ec);
    if (ec) {
        error = L"创建 Web Root 失败: " + Utf8ToWideLocal(ec.message());
        return false;
    }
    std::filesystem::create_directories(DetailConfigDirectory(), ec);
    if (ec) {
        error = L"创建 HTTP 配置目录失败: " + Utf8ToWideLocal(ec.message());
        return false;
    }
    const auto configPath = DetailConfigPath(rootPath);
    if (FileExists(configPath)) {
        return true;
    }
    return WriteUtf8TextFile(configPath, DefaultDetailConfigText(), error);
}

LocalHttpServerOptions LocalHttpServerService::OptionsFromConfig(const AppConfig& config, const std::filesystem::path& rootBaseDirectory) {
    LocalHttpServerOptions options;
    options.port = std::max(1, std::min(65535, config.httpServerPort));
    options.lanAccess = config.httpServerLanAccess;
    options.rootPath = Trim(config.httpServerRootPath).empty() ? DefaultRootPath(rootBaseDirectory) : std::filesystem::path(config.httpServerRootPath);
    return options;
}

bool LocalHttpServerService::Start(const LocalHttpServerOptions& options, std::wstring& error) {
    try {
    Stop();
    options_ = options;
    std::wstring detailError;
    if (!EnsureDetailConfig(options_.rootPath, detailError)) {
        error = detailError;
        lastError_ = error;
        return false;
    }

#ifndef QUATTRO_WITH_HTTPLIB
    error = L"当前构建未启用 cpp-httplib HTTP 服务端。请通过 vcpkg 安装 cpp-httplib 后重新配置构建。";
    lastError_ = error;
    return false;
#else
    const DetailConfig detail = LoadDetailConfig(options_.rootPath);
    const std::filesystem::path root = options_.rootPath;
    impl_->server = std::make_unique<httplib::Server>();
    auto& server = *impl_->server;

    server.Get("/_quattro/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("{\"ok\":true}", "application/json; charset=utf-8");
    });

    server.Get("/_quattro/open", [detail](const httplib::Request& req, httplib::Response& res) {
        if (!detail.allowRemoteOpen && req.remote_addr != "127.0.0.1" && req.remote_addr != "::1") {
            res.status = 403;
            res.set_content("Remote open is disabled.", "text/plain; charset=utf-8");
            return;
        }
        const auto url = req.has_param("url") ? Utf8ToWideLocal(req.get_param_value("url")) : L"";
        if (url.empty()) {
            res.status = 400;
            res.set_content("Missing url.", "text/plain; charset=utf-8");
            return;
        }
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        res.set_content("OK", "text/plain; charset=utf-8");
    });

    server.Post("/_quattro/upload", [root, detail](const httplib::Request& req, httplib::Response& res) {
        if (!detail.allowPost || !HasUploadAccess(req, detail)) {
            if (detail.authEnabled && !CheckBasicAuth(req, detail)) {
                RequireAuth(res);
            } else {
                res.status = 403;
                res.set_content("Upload forbidden.", "text/plain; charset=utf-8");
            }
            return;
        }
        const std::wstring requestPath = req.has_param("path") ? DecodeUrlPath(req.get_param_value("path")) : L"/";
        const std::filesystem::path targetDir = root / std::filesystem::path(requestPath).relative_path();
        if (!IsWithinRoot(root, targetDir)) {
            res.status = 403;
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(targetDir, ec);
        std::uint64_t written = 0;
        for (const auto& file : req.form.files) {
            const auto& info = file.second;
            if (written + info.content.size() > detail.maxUploadBytes) {
                res.status = 413;
                res.set_content("Upload too large.", "text/plain; charset=utf-8");
                return;
            }
            const std::filesystem::path target = targetDir / Utf8ToWideLocal(info.filename);
            if (!IsWithinRoot(root, target)) {
                res.status = 403;
                return;
            }
            std::ofstream out(target, std::ios::binary | std::ios::trunc);
            out.write(info.content.data(), static_cast<std::streamsize>(info.content.size()));
            written += info.content.size();
        }
        res.set_content("Uploaded.", "text/plain; charset=utf-8");
    });

    server.Put(R"(/.*)", [root, detail](const httplib::Request& req, httplib::Response& res) {
        if (!detail.allowPut || !HasWriteAccess(req, detail)) {
            if (detail.authEnabled && !CheckBasicAuth(req, detail)) {
                RequireAuth(res);
            } else {
                res.status = 403;
                res.set_content("PUT forbidden.", "text/plain; charset=utf-8");
            }
            return;
        }
        if (static_cast<std::uint64_t>(req.body.size()) > detail.maxUploadBytes) {
            res.status = 413;
            return;
        }
        const std::filesystem::path target = root / std::filesystem::path(DecodeUrlPath(req.path)).relative_path();
        if (!IsWithinRoot(root, target)) {
            res.status = 403;
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(req.body.data(), static_cast<std::streamsize>(req.body.size()));
        res.status = 201;
        res.set_content("Created.", "text/plain; charset=utf-8");
    });

    server.Get(R"(/.*)", [root, detail](const httplib::Request& req, httplib::Response& res) {
        if (!HasReadAccess(req, detail)) {
            RequireAuth(res);
            return;
        }
        std::filesystem::path target = root / std::filesystem::path(DecodeUrlPath(req.path)).relative_path();
        if (!IsWithinRoot(root, target) || (!detail.showHiddenFiles && IsHiddenPath(target))) {
            res.status = 403;
            return;
        }
        std::error_code ec;
        if (std::filesystem::is_directory(target, ec)) {
            const auto index = target / L"index.html";
            if (FileExists(index)) {
                target = index;
            } else if (detail.directoryListing) {
                res.set_content(DirectoryListingHtml(root, target, req.path, detail), "text/html; charset=utf-8");
                return;
            } else {
                res.status = 403;
                return;
            }
        }
        if (!FileExists(target)) {
            res.status = 404;
            res.set_content("Not found.", "text/plain; charset=utf-8");
            return;
        }

        const auto size = static_cast<std::uint64_t>(std::filesystem::file_size(target, ec));
        std::ifstream in(target, std::ios::binary);
        std::string body(static_cast<std::size_t>(size), '\0');
        if (size > 0) {
            in.read(body.data(), static_cast<std::streamsize>(body.size()));
        }
        res.set_header("Accept-Ranges", "bytes");
        res.set_header("Content-Disposition", DispositionFor(target, detail) + "; filename=\"" + WideToUtf8Local(target.filename().wstring()) + "\"");
        res.set_content(std::move(body), MimeFor(target, detail));
    });

    const std::string host = options_.lanAccess ? "0.0.0.0" : "127.0.0.1";
    const int port = options_.port;
    if (!server.bind_to_port(host, port)) {
        error = L"HTTP 服务端口绑定失败: " + std::to_wstring(port);
        impl_->server.reset();
        lastError_ = error;
        return false;
    }
    impl_->running = true;
    impl_->thread = std::thread([this]() {
        WriteAppLog(L"HTTP 服务启动: " + BaseUrl(true));
        impl_->server->listen_after_bind();
        impl_->running = false;
    });
    lastError_.clear();
    return true;
#endif
    } catch (const std::exception& ex) {
        error = L"HTTP 服务启动异常: " + Utf8ToWideLocal(ex.what());
    } catch (...) {
        error = L"HTTP 服务启动异常: 未知错误。";
    }
    Stop();
    lastError_ = error;
    return false;
}

bool LocalHttpServerService::Restart(const LocalHttpServerOptions& options, std::wstring& error) {
    Stop();
    return Start(options, error);
}

void LocalHttpServerService::Stop() {
#ifdef QUATTRO_WITH_HTTPLIB
    if (impl_->server) {
        impl_->server->stop();
    }
#endif
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
#ifdef QUATTRO_WITH_HTTPLIB
    impl_->server.reset();
#endif
    impl_->running = false;
    lastError_.clear();
}

bool LocalHttpServerService::IsRunning() const {
    return impl_ && impl_->running.load();
}

std::wstring LocalHttpServerService::BaseUrl(bool localOnly) const {
    const std::wstring host = localOnly || !options_.lanAccess ? L"127.0.0.1" : L"localhost";
    return L"http://" + host + L":" + std::to_wstring(options_.port) + L"/";
}
