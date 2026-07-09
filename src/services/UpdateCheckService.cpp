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
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#ifdef QUATTRO_WITH_CURL
#include <curl/curl.h>
#endif

namespace {
constexpr const wchar_t* kDefaultLatestManifestUrl = L"https://github.com/codingriver/quattro/releases/latest/download/latest.json";

std::wstring NormalizeUpdateInfoUrl(const std::wstring& configuredUrl);

struct UpdateSourceCandidate {
    std::wstring name;
    std::wstring manifestUrl;
    std::wstring mirrorBase;
};

const std::vector<std::vector<std::wstring>>& BuiltinGithubMirrorGroups() {
    static const std::vector<std::vector<std::wstring>> mirrors{
        {L"https://gh-proxy.303066.xyz"},
        {L"https://gh-proxy.com", L"https://gh-proxy.org", L"https://gh-proxy.303066.xyz", L"https://mirror.ghproxy.com"},
        {L"https://ghfast.top", L"https://ghp.ci", L"https://gh.llkk.cc"},
        {L"https://gh.aptv.app", L"https://gh.927223.xyz", L"https://gh.halonice.com"},
        {L"https://github.akams.cn", L"https://ui.ghproxy.cc", L"https://gh.ddlc.top"},
        {L"https://gh-proxy.net", L"https://hub.gitmirror.com", L"https://github.moeyy.xyz", L"https://ghfile.geekertao.top"},
    };
    return mirrors;
}

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

std::wstring NormalizeMirrorBase(const std::wstring& value) {
    std::wstring base = Trim(value);
    while (!base.empty() && base.back() == L'/') {
        base.pop_back();
    }
    const std::wstring lower = ToLower(base);
    if (lower.rfind(L"https://", 0) != 0 && lower.rfind(L"http://", 0) != 0) {
        return {};
    }
    return base;
}

bool IsGithubComUrl(const std::wstring& url) {
    const std::wstring lower = ToLower(Trim(url));
    return lower.rfind(L"https://github.com/", 0) == 0 ||
           lower.rfind(L"http://github.com/", 0) == 0;
}

std::wstring MirrorGithubUrlInternal(const std::wstring& url, const std::wstring& mirrorBase) {
    const std::wstring trimmedUrl = Trim(url);
    const std::wstring base = NormalizeMirrorBase(mirrorBase);
    if (trimmedUrl.empty() || base.empty() || !IsGithubComUrl(trimmedUrl)) {
        return trimmedUrl;
    }
    return base + L"/" + trimmedUrl;
}

std::wstring JsonEscapeString(const std::wstring& value) {
    std::wstring escaped;
    escaped.reserve(value.size() + 8);
    for (wchar_t ch : value) {
        switch (ch) {
        case L'"': escaped += L"\\\""; break;
        case L'\\': escaped += L"\\\\"; break;
        case L'\b': escaped += L"\\b"; break;
        case L'\f': escaped += L"\\f"; break;
        case L'\n': escaped += L"\\n"; break;
        case L'\r': escaped += L"\\r"; break;
        case L'\t': escaped += L"\\t"; break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

std::wstring BuiltinMirrorConfigJson() {
    std::wstring json = L"{\n";
    json += L"  \"version\": \"" + JsonEscapeString(QuattroVersionText()) + L"\",\n";
    json += L"  \"githubMirrors\": [\n";
    const auto& groups = BuiltinGithubMirrorGroups();
    for (std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        json += L"    [\n";
        const auto& group = groups[groupIndex];
        for (std::size_t itemIndex = 0; itemIndex < group.size(); ++itemIndex) {
            json += L"      \"" + JsonEscapeString(group[itemIndex]) + L"\"";
            if (itemIndex + 1 < group.size()) {
                json += L",";
            }
            json += L"\n";
        }
        json += L"    ]";
        if (groupIndex + 1 < groups.size()) {
            json += L",";
        }
        json += L"\n";
    }
    json += L"  ]\n";
    json += L"}\n";
    return json;
}

void AddMirrorBase(std::vector<std::wstring>& bases, std::set<std::wstring>& seen, const std::wstring& value) {
    const std::wstring base = NormalizeMirrorBase(value);
    if (base.empty()) {
        return;
    }
    const std::wstring key = ToLower(base);
    if (seen.insert(key).second) {
        bases.push_back(base);
    }
}

std::vector<std::wstring> ParseMirrorBasesFromJsonValue(const JsonValue& root) {
    std::vector<std::wstring> bases;
    std::set<std::wstring> seen;
    const JsonValue* mirrors = root.get(L"githubMirrors");
    if (!mirrors || !mirrors->isArray()) {
        return bases;
    }
    for (const JsonValue& group : mirrors->arrayValue) {
        if (group.isArray()) {
            for (const JsonValue& item : group.arrayValue) {
                if (item.isString()) {
                    AddMirrorBase(bases, seen, item.stringValue);
                }
            }
            continue;
        }
        if (group.isString()) {
            AddMirrorBase(bases, seen, group.stringValue);
        }
    }
    return bases;
}

std::vector<std::wstring> MirrorBasesFromJson(const std::wstring& json, std::wstring& error) {
    JsonValue root;
    if (!ParseJson(json, root, error) || !root.isObject()) {
        if (error.empty()) {
            error = L"GitHub 镜像配置格式无效。";
        }
        return {};
    }
    return ParseMirrorBasesFromJsonValue(root);
}

std::wstring MirrorConfigVersionFromJson(const std::wstring& json) {
    JsonValue root;
    std::wstring error;
    if (!ParseJson(json, root, error) || !root.isObject()) {
        return {};
    }
    const JsonValue* version = root.get(L"version");
    return version && version->isString() ? Trim(version->stringValue) : L"";
}

std::filesystem::path MirrorConfigPath(const std::filesystem::path& appDirectory) {
    return appDirectory / L"update-mirrors.json";
}

bool WriteMirrorConfigFile(const std::filesystem::path& path, std::wstring& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = L"创建 GitHub 镜像配置目录失败: " + Utf8ToWide(ec.message());
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = L"写入 GitHub 镜像配置失败: " + path.wstring();
        return false;
    }
    const std::string utf8 = WideToUtf8(BuiltinMirrorConfigJson());
    file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    if (!file) {
        error = L"写入 GitHub 镜像配置失败: " + path.wstring();
        return false;
    }
    return true;
}

bool EnsureMirrorConfigFile(const std::filesystem::path& appDirectory, std::wstring& error) {
    const std::filesystem::path path = MirrorConfigPath(appDirectory);
    const std::wstring currentVersion = QuattroVersionText();
    if (!FileExists(path)) {
        return WriteMirrorConfigFile(path, error);
    }
    const std::wstring json = LoadUtf8File(path);
    if (MirrorConfigVersionFromJson(json) != currentVersion) {
        return WriteMirrorConfigFile(path, error);
    }
    return true;
}

std::vector<std::wstring> LoadConfiguredMirrorBases(const std::filesystem::path& appDirectory) {
    const std::wstring json = LoadUtf8File(MirrorConfigPath(appDirectory));
    if (json.empty()) {
        return {};
    }
    std::wstring error;
    return MirrorBasesFromJson(json, error);
}

std::vector<std::wstring> EffectiveMirrorBases(const std::filesystem::path& appDirectory) {
    std::vector<std::wstring> bases = LoadConfiguredMirrorBases(appDirectory);
    std::set<std::wstring> seen;
    std::vector<std::wstring> merged;
    for (const auto& base : bases) {
        AddMirrorBase(merged, seen, base);
    }
    for (const auto& group : BuiltinGithubMirrorGroups()) {
        for (const auto& base : group) {
            AddMirrorBase(merged, seen, base);
        }
    }
    return merged;
}

std::vector<UpdateSourceCandidate> BuildUpdateSourceCandidates(const std::wstring& configuredUrl, const std::vector<std::wstring>& mirrorBases) {
    std::vector<UpdateSourceCandidate> candidates;
    const std::wstring primary = NormalizeUpdateInfoUrl(configuredUrl);
    candidates.push_back(UpdateSourceCandidate{L"github", primary, L""});
    if (!IsGithubComUrl(primary)) {
        return candidates;
    }
    int index = 1;
    for (const auto& base : mirrorBases) {
        const std::wstring mirrored = MirrorGithubUrlInternal(primary, base);
        if (mirrored != primary) {
            candidates.push_back(UpdateSourceCandidate{L"github-mirror-" + std::to_wstring(index), mirrored, NormalizeMirrorBase(base)});
            ++index;
        }
    }
    return candidates;
}

std::vector<UpdateSourceCandidate> DownloadSourceCandidatesForInfo(const UpdateReleaseInfo& info, const std::filesystem::path& appDirectory) {
    std::vector<UpdateSourceCandidate> candidates;
    if (!info.sourceMirrorBase.empty()) {
        candidates.push_back(UpdateSourceCandidate{
            info.sourceName.empty() ? L"github-mirror" : info.sourceName,
            info.sourceManifestUrl,
            NormalizeMirrorBase(info.sourceMirrorBase)});
    } else {
        candidates.push_back(UpdateSourceCandidate{L"github", info.sourceManifestUrl, L""});
    }

    const std::vector<std::wstring> mirrorBases = EffectiveMirrorBases(appDirectory);
    std::set<std::wstring> seen;
    if (!info.sourceMirrorBase.empty()) {
        seen.insert(ToLower(NormalizeMirrorBase(info.sourceMirrorBase)));
    }
    int index = 1;
    for (const auto& base : mirrorBases) {
        const std::wstring normalized = NormalizeMirrorBase(base);
        if (normalized.empty() || !seen.insert(ToLower(normalized)).second) {
            ++index;
            continue;
        }
        candidates.push_back(UpdateSourceCandidate{L"github-mirror-" + std::to_wstring(index), L"", normalized});
        ++index;
    }
    return candidates;
}

std::wstring NormalizeUpdateInfoUrl(const std::wstring& configuredUrl) {
    const std::wstring trimmed = Trim(configuredUrl);
    if (trimmed.empty()) {
        return kDefaultLatestManifestUrl;
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
                    return L"https://github.com/" + owner + L"/" + repo + L"/releases/latest/download/latest.json";
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

struct CurlProgressContext {
    UpdateProgressCallback progress;
    UpdateCancelCallback cancel;
    std::uint64_t lastDownloadedBytes = 0;
    ULONGLONG lastProgressTick = GetTickCount64();
    std::wstring* error = nullptr;
};

int CurlProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* context = static_cast<CurlProgressContext*>(clientp);
    if (!context) {
        return 0;
    }
    if (context->cancel && context->cancel()) {
        if (context->error) {
            *context->error = L"下载已取消。";
        }
        return 1;
    }

    const std::uint64_t downloaded = dlnow > 0 ? static_cast<std::uint64_t>(dlnow) : 0;
    const std::uint64_t total = dltotal > 0 ? static_cast<std::uint64_t>(dltotal) : 0;
    if (downloaded > context->lastDownloadedBytes) {
        context->lastDownloadedBytes = downloaded;
        context->lastProgressTick = GetTickCount64();
    } else if (GetTickCount64() - context->lastProgressTick > 30000) {
        if (context->error) {
            *context->error = L"下载 30 秒没有进展，已超时。";
        }
        return 1;
    }

    if (context->progress) {
        context->progress(UpdateDownloadProgress{downloaded, total});
    }
    return 0;
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

bool DownloadFile(
    const std::wstring& url,
    const std::filesystem::path& path,
    std::wstring& error,
    const UpdateProgressCallback& progress,
    const UpdateCancelCallback& cancel) {
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
    CurlProgressContext progressContext;
    progressContext.progress = progress;
    progressContext.cancel = cancel;
    progressContext.error = &error;
    curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Quattro Update Downloader/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CurlProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressContext);
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    file.close();
    if (code != CURLE_OK || status < 200 || status >= 300) {
        const std::string detail = errorBuffer[0] ? errorBuffer : curl_easy_strerror(code);
        if (error.empty()) {
            error = L"下载更新失败: HTTP " + std::to_wstring(status) + L"，" + Utf8ToWide(detail);
        }
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

bool DownloadFile(const std::wstring&, const std::filesystem::path&, std::wstring& error, const UpdateProgressCallback&, const UpdateCancelCallback&) {
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

std::wstring JsonStringField(const JsonValue& value, const std::wstring& key) {
    const JsonValue* field = value.get(key);
    return field ? field->stringOr() : L"";
}

std::uint64_t JsonSizeField(const JsonValue& value, const std::wstring& key) {
    const JsonValue* field = value.get(key);
    if (!field || !field->isNumber()) {
        return 0;
    }
    return static_cast<std::uint64_t>(std::max(0.0, field->numberValue));
}

bool LooksLikeSha256(const std::wstring& value) {
    if (value.size() != 64) {
        return false;
    }
    for (wchar_t ch : value) {
        if (!((ch >= L'0' && ch <= L'9') ||
              (ch >= L'a' && ch <= L'f') ||
              (ch >= L'A' && ch <= L'F'))) {
            return false;
        }
    }
    return true;
}

bool FillFromStaticManifest(const JsonValue& root, UpdateReleaseInfo& info, std::wstring& error) {
    info.latestVersion = JsonStringField(root, L"version");
    if (info.latestVersion.empty()) {
        info.latestVersion = JsonStringField(root, L"latestVersion");
    }
    info.releaseUrl = JsonStringField(root, L"releaseUrl");
    info.releaseNotes = JsonStringField(root, L"notes");
    if (info.releaseNotes.empty()) {
        info.releaseNotes = JsonStringField(root, L"releaseNotes");
    }
    info.checksumDownloadUrl = JsonStringField(root, L"checksumUrl");
    if (info.checksumDownloadUrl.empty()) {
        info.checksumDownloadUrl = JsonStringField(root, L"checksumDownloadUrl");
    }
    if (info.latestVersion.empty()) {
        error = L"静态更新清单缺少版本号。";
        return false;
    }

    const JsonValue* assets = root.get(L"assets");
    if (assets && assets->isArray()) {
        for (const JsonValue& asset : assets->arrayValue) {
            if (!asset.isObject()) {
                continue;
            }
            const std::wstring name = JsonStringField(asset, L"name");
            const std::wstring url = JsonStringField(asset, L"url");
            const std::uint64_t size = JsonSizeField(asset, L"size");
            if (ToLower(name) == L"sha256sums.txt" && info.checksumDownloadUrl.empty()) {
                info.checksumDownloadUrl = url;
            }
            if (info.assetDownloadUrl.empty() && IsExpectedAssetName(name)) {
                info.assetName = name;
                info.assetDownloadUrl = url;
                info.assetSizeBytes = size;
                const std::wstring sha256 = JsonStringField(asset, L"sha256");
                if (LooksLikeSha256(sha256)) {
                    info.expectedSha256 = ToLower(sha256);
                }
            }
        }
    }
    return true;
}

bool FillFromGitHubApiRelease(const JsonValue& root, UpdateReleaseInfo& info, std::wstring& error) {
    info.latestVersion = JsonStringField(root, L"tag_name");
    info.releaseUrl = JsonStringField(root, L"html_url");
    info.releaseNotes = JsonStringField(root, L"body");
    if (info.latestVersion.empty()) {
        error = L"更新信息缺少版本号。";
        return false;
    }

    const JsonValue* assets = root.get(L"assets");
    if (assets && assets->isArray()) {
        for (const JsonValue& asset : assets->arrayValue) {
            if (!asset.isObject()) {
                continue;
            }
            const std::wstring name = JsonStringField(asset, L"name");
            const std::wstring url = JsonStringField(asset, L"browser_download_url");
            const std::uint64_t size = JsonSizeField(asset, L"size");
            if (ToLower(name) == L"sha256sums.txt") {
                info.checksumDownloadUrl = url;
            }
            if (info.assetDownloadUrl.empty() && IsExpectedAssetName(name)) {
                info.assetName = name;
                info.assetDownloadUrl = url;
                info.assetSizeBytes = size;
            }
        }
    }
    return true;
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

std::wstring UpdateCheckService::UpdateInfoUrlForConfig(const std::wstring& configuredUrl) {
    return NormalizeUpdateInfoUrl(configuredUrl);
}

std::wstring UpdateCheckService::ReleaseApiUrlForConfig(const std::wstring& configuredUrl) {
    return UpdateInfoUrlForConfig(configuredUrl);
}

std::wstring UpdateCheckService::MirrorGithubUrl(const std::wstring& url, const std::wstring& mirrorBase) {
    return MirrorGithubUrlInternal(url, mirrorBase);
}

std::vector<std::wstring> UpdateCheckService::ParseGithubMirrorBasesJson(const std::wstring& json, std::wstring& error) {
    return MirrorBasesFromJson(json, error);
}

std::vector<std::wstring> UpdateCheckService::UpdateInfoUrlsForConfig(const std::wstring& configuredUrl, const std::vector<std::wstring>& mirrorBases) {
    std::vector<std::wstring> urls;
    for (const auto& candidate : BuildUpdateSourceCandidates(configuredUrl, mirrorBases)) {
        urls.push_back(candidate.manifestUrl);
    }
    return urls;
}

bool UpdateCheckService::EnsureGithubMirrorConfigFile(const std::filesystem::path& appDirectory, std::wstring& error) {
    return EnsureMirrorConfigFile(appDirectory, error);
}

bool UpdateCheckService::ParseReleaseInfoJson(const std::wstring& json, UpdateReleaseInfo& info, std::wstring& error) {
    info = {};
    info.currentVersion = QuattroVersionText();

    JsonValue root;
    if (!ParseJson(json, root, error) || !root.isObject()) {
        if (error.empty()) {
            error = L"更新信息格式无效。";
        }
        return false;
    }

    const bool parsed = root.get(L"tag_name")
        ? FillFromGitHubApiRelease(root, info, error)
        : FillFromStaticManifest(root, info, error);
    if (!parsed) {
        return false;
    }

    info.updateAvailable = CompareVersions(info.currentVersion, info.latestVersion) < 0;
    if (info.updateAvailable && info.assetDownloadUrl.empty()) {
        error = L"找到新版本，但未找到适合当前架构的发布包。";
        return false;
    }
    return true;
}

bool UpdateCheckService::CheckLatest(UpdateReleaseInfo& info, std::wstring& error) const {
    std::wstring mirrorConfigError;
    EnsureMirrorConfigFile(appDirectory_, mirrorConfigError);

    std::wstring lastError;
    const std::vector<UpdateSourceCandidate> candidates = BuildUpdateSourceCandidates(releaseApiUrl_, EffectiveMirrorBases(appDirectory_));
    for (const auto& candidate : candidates) {
        std::string body;
        std::wstring currentError;
        if (!DownloadText(candidate.manifestUrl, body, currentError)) {
            lastError = candidate.name + L": " + currentError;
            continue;
        }

        UpdateReleaseInfo parsed;
        if (ParseReleaseInfoJson(Utf8ToWide(body), parsed, currentError)) {
            parsed.sourceName = candidate.name;
            parsed.sourceManifestUrl = candidate.manifestUrl;
            parsed.sourceMirrorBase = candidate.mirrorBase;
            info = std::move(parsed);
            error.clear();
            return true;
        }
        lastError = candidate.name + L": " + currentError;
    }

    info = {};
    info.currentVersion = QuattroVersionText();
    error = lastError.empty() ? L"检查更新失败。" : lastError;
    return false;
}

bool UpdateCheckService::DownloadUpdate(const UpdateReleaseInfo& info, UpdateDownloadResult& result, std::wstring& error) const {
    return DownloadUpdate(info, result, error, {}, {});
}

bool UpdateCheckService::DownloadUpdate(
    const UpdateReleaseInfo& info,
    UpdateDownloadResult& result,
    std::wstring& error,
    const UpdateProgressCallback& progress,
    const UpdateCancelCallback& cancel) const {
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
    const auto removeTemp = [&]() {
        std::error_code removeError;
        std::filesystem::remove(temp, removeError);
    };

    if (progress) {
        progress(UpdateDownloadProgress{0, info.assetSizeBytes});
    }
    const std::vector<UpdateSourceCandidate> candidates = DownloadSourceCandidatesForInfo(info, appDirectory_);
    std::wstring lastError;
    bool downloaded = false;
    std::wstring downloadMirrorBase;
    for (const auto& candidate : candidates) {
        const std::wstring assetUrl = candidate.mirrorBase.empty()
            ? info.assetDownloadUrl
            : MirrorGithubUrlInternal(info.assetDownloadUrl, candidate.mirrorBase);
        std::wstring currentError;
        if (DownloadFile(assetUrl, temp, currentError, [&](const UpdateDownloadProgress& current) {
            if (progress) {
                progress(UpdateDownloadProgress{
                    current.downloadedBytes,
                    current.totalBytes != 0 ? current.totalBytes : info.assetSizeBytes});
            }
        }, cancel)) {
            downloaded = true;
            downloadMirrorBase = candidate.mirrorBase;
            error.clear();
            break;
        }
        removeTemp();
        if (currentError == L"下载已取消。") {
            error = currentError;
            return false;
        }
        lastError = candidate.name + L": " + currentError;
        if (progress) {
            progress(UpdateDownloadProgress{0, info.assetSizeBytes});
        }
    }
    if (!downloaded) {
        error = lastError.empty() ? L"下载更新失败。" : lastError;
        return false;
    }
    const auto size = std::filesystem::file_size(temp, ec);
    if (ec || size == 0) {
        error = L"下载文件为空。";
        removeTemp();
        return false;
    }

    if (!info.expectedSha256.empty()) {
        std::wstring actual;
        if (!Sha256File(temp, actual, error)) {
            removeTemp();
            return false;
        }
        if (ToLower(actual) != ToLower(info.expectedSha256)) {
            error = L"更新包 SHA256 校验失败。";
            removeTemp();
            return false;
        }
        result.checksumVerified = true;
        result.checksumMessage = L"SHA256 校验通过。";
    } else if (!info.checksumDownloadUrl.empty()) {
        std::string checksumBody;
        std::wstring checksumError;
        const std::wstring checksumUrl = downloadMirrorBase.empty()
            ? info.checksumDownloadUrl
            : MirrorGithubUrlInternal(info.checksumDownloadUrl, downloadMirrorBase);
        if (DownloadText(checksumUrl, checksumBody, checksumError)) {
            const std::wstring expected = ExpectedSha256For(Utf8ToWide(checksumBody), info.assetName);
            std::wstring actual;
            if (!expected.empty() && Sha256File(temp, actual, error)) {
                if (ToLower(actual) != expected) {
                    error = L"更新包 SHA256 校验失败。";
                    removeTemp();
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
        removeTemp();
        return false;
    }
    result.filePath = target;
    return true;
}
