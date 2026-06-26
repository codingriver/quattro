#include "PluginStoreService.h"

#include "JsonValue.h"
#include "Utilities.h"

#include <bcrypt.h>
#include <wininet.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>

namespace {
bool IsHttpUrl(const std::wstring& value) {
    const std::wstring lower = ToLower(Trim(value));
    return lower.starts_with(L"http://") || lower.starts_with(L"https://");
}

std::wstring Utf8BytesToWide(const std::string& bytes) {
    if (bytes.empty()) {
        return {};
    }
    int length = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring text(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), length);
    return text;
}

const JsonValue* ObjectValue(const JsonValue& value, const wchar_t* key) {
    const JsonValue* child = value.get(key);
    return child && child->isObject() ? child : nullptr;
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

int IntValue(const JsonValue& value, const wchar_t* key, int fallback = 0) {
    const JsonValue* child = value.get(key);
    return child ? child->intOr(fallback) : fallback;
}

std::wstring JoinStringArray(const JsonValue& value, const wchar_t* key) {
    const JsonValue* array = ArrayValue(value, key);
    if (!array) {
        return {};
    }
    std::wstring result;
    for (const auto& item : array->arrayValue) {
        const std::wstring text = item.stringOr();
        if (text.empty()) {
            continue;
        }
        if (!result.empty()) {
            result += L", ";
        }
        result += text;
    }
    return result;
}

bool HasDeniedExtension(const std::filesystem::path& path) {
    const std::wstring ext = ToLower(path.extension().wstring());
    return ext == L".exe" || ext == L".dll" || ext == L".bat" || ext == L".cmd" ||
           ext == L".ps1" || ext == L".vbs" || ext == L".js" || ext == L".msi" ||
           ext == L".scr" || ext == L".com" || ext == L".lnk";
}

std::wstring BytesToHex(const std::vector<std::uint8_t>& bytes) {
    std::wstringstream stream;
    stream << std::hex << std::setfill(L'0');
    for (std::uint8_t byte : bytes) {
        stream << std::setw(2) << static_cast<int>(byte);
    }
    return stream.str();
}

std::wstring Sha256Hex(const std::vector<std::uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    DWORD objectLength = 0;
    DWORD copied = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &copied, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    std::vector<std::uint8_t> object(objectLength);
    std::vector<std::uint8_t> hash(32);
    BCRYPT_HASH_HANDLE handle = nullptr;
    if (BCryptCreateHash(algorithm, &handle, object.data(), objectLength, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    const PUCHAR data = const_cast<PUCHAR>(bytes.empty() ? reinterpret_cast<const UCHAR*>("") : bytes.data());
    const ULONG size = static_cast<ULONG>(bytes.size());
    const bool ok = BCryptHashData(handle, data, size, 0) == 0 &&
                    BCryptFinishHash(handle, hash.data(), static_cast<ULONG>(hash.size()), 0) == 0;
    BCryptDestroyHash(handle);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok ? BytesToHex(hash) : L"";
}
}

PluginStoreService::PluginStoreService(std::filesystem::path appDirectory, std::wstring storeUrl)
    : appDirectory_(std::move(appDirectory)), storeUrl_(std::move(storeUrl)) {
}

bool PluginStoreService::Refresh(PluginRegistry& registry) {
    lastError_.clear();
    plugins_.clear();
    if (!registry.Initialize()) {
        lastError_ = registry.lastError();
        return false;
    }
    std::wstring text;
    std::filesystem::path baseDirectory;
    if (!LoadIndexText(text, baseDirectory)) {
        return false;
    }
    if (!ParseIndex(text, baseDirectory)) {
        return false;
    }
    for (const auto& plugin : plugins_) {
        if (!plugin.record.builtin && !registry.UpsertStorePlugin(plugin.record)) {
            lastError_ = registry.lastError();
            return false;
        }
    }
    return true;
}

bool PluginStoreService::Install(const std::wstring& pluginId, PluginRegistry& registry, StorageService& storage) {
    lastError_.clear();
    if (!registry.Initialize()) {
        lastError_ = registry.lastError();
        return false;
    }
    auto it = std::find_if(plugins_.begin(), plugins_.end(), [&](const StorePluginDefinition& plugin) {
        return plugin.record.id == pluginId;
    });
    if (it == plugins_.end()) {
        lastError_ = L"商店中未找到插件。";
        return false;
    }

    StorePluginDefinition plugin = *it;
    if (!plugin.record.packageUrl.empty()) {
        std::wstring packageText;
        std::filesystem::path packageDirectory;
        if (!LoadPackageText(plugin, packageText, packageDirectory)) {
            return false;
        }
        JsonValue packageRoot;
        std::wstring packageError;
        if (!ParseJson(packageText, packageRoot, packageError)) {
            lastError_ = packageError;
            return false;
        }
        StorePluginDefinition package;
        if (!ParsePluginObject(packageRoot, packageDirectory, package)) {
            if (lastError_.empty()) {
                lastError_ = L"插件包解析失败。";
            }
            return false;
        }
        package.record.packageUrl = plugin.record.packageUrl;
        package.record.sha256 = plugin.record.sha256;
        plugin = std::move(package);
    }

    return InstallDefinition(plugin, registry, storage);
}

bool PluginStoreService::Remove(const std::wstring& pluginId, PluginRegistry& registry, StorageService& storage) {
    lastError_.clear();
    if (!registry.Initialize()) {
        lastError_ = registry.lastError();
        return false;
    }
    const auto contributions = registry.LoadContributions(pluginId);
    for (const auto& item : contributions) {
        if (item.objectType == L"link" && item.objectId > 0) {
            storage.DeleteLink(item.objectId);
        } else if (item.objectType == L"group" && item.objectId > 0) {
            storage.DeleteGroup(item.objectId);
        } else if (item.objectType == L"file" && !item.objectPath.empty()) {
            std::filesystem::path relative;
            if (ResolveSafeRelativePath(item.objectPath, relative)) {
                std::error_code ec;
                std::filesystem::remove(appDirectory_ / relative, ec);
            }
        }
    }
    if (!registry.ClearContributions(pluginId)) {
        lastError_ = registry.lastError();
        return false;
    }
    if (!registry.RemovePlugin(pluginId)) {
        lastError_ = registry.lastError();
        return false;
    }
    return true;
}

bool PluginStoreService::LoadIndexText(std::wstring& text, std::filesystem::path& baseDirectory) {
    std::wstring source = Trim(storeUrl_);
    if (source.empty()) {
        source = (appDirectory_ / L"plugins" / L"store" / L"index.json").wstring();
    }
    if (IsHttpUrl(source)) {
        storeBaseDirectory_.clear();
        baseDirectory.clear();
        return ReadTextFromUrl(source, text);
    }

    std::filesystem::path path(source);
    if (path.is_relative()) {
        path = appDirectory_ / path;
    }
    baseDirectory = path.parent_path();
    storeBaseDirectory_ = baseDirectory;
    return ReadTextFromFile(path, text);
}

bool PluginStoreService::LoadPackageText(const StorePluginDefinition& plugin, std::wstring& text, std::filesystem::path& packageDirectory) {
    const std::wstring packageUrl = ResolvePackageUrl(plugin.record.packageUrl);
    std::vector<std::uint8_t> bytes;
    if (IsHttpUrl(packageUrl)) {
        packageDirectory.clear();
        if (!ReadBytesFromUrl(packageUrl, bytes)) {
            return false;
        }
    } else {
        std::filesystem::path path(packageUrl);
        if (path.is_relative()) {
            path = plugin.packageDirectory / path;
        }
        packageDirectory = path.parent_path();
        if (!ReadBytesFromFile(path, bytes)) {
            return false;
        }
    }
    if (!plugin.record.sha256.empty()) {
        const std::wstring actual = ToLower(Sha256Hex(bytes));
        const std::wstring expected = ToLower(plugin.record.sha256);
        if (actual.empty() || actual != expected) {
            lastError_ = L"插件包 SHA-256 校验失败。";
            return false;
        }
    }
    text = Utf8BytesToWide(std::string(bytes.begin(), bytes.end()));
    if (text.empty()) {
        lastError_ = L"插件包内容为空或不是 UTF-8。";
        return false;
    }
    return true;
}

bool PluginStoreService::ParseIndex(const std::wstring& text, const std::filesystem::path& baseDirectory) {
    JsonValue root;
    std::wstring error;
    if (!ParseJson(text, root, error)) {
        lastError_ = error;
        return false;
    }
    const JsonValue* plugins = ArrayValue(root, L"plugins");
    if (!plugins) {
        lastError_ = L"插件商店索引缺少 plugins 数组。";
        return false;
    }
    for (const auto& item : plugins->arrayValue) {
        StorePluginDefinition plugin;
        if (item.isObject() && ParsePluginObject(item, baseDirectory, plugin)) {
            plugins_.push_back(std::move(plugin));
        }
    }
    return true;
}

bool PluginStoreService::ParsePluginObject(const JsonValue& object, const std::filesystem::path& baseDirectory, StorePluginDefinition& plugin) {
    if (!object.isObject()) {
        lastError_ = L"插件定义不是对象。";
        return false;
    }
    plugin.packageDirectory = baseDirectory;
    plugin.record.id = StringValue(object, L"id");
    plugin.record.name = StringValue(object, L"name");
    plugin.record.version = StringValue(object, L"version", L"1.0.0");
    plugin.record.category = StringValue(object, L"category", L"other");
    plugin.record.kind = StringValue(object, L"kind", L"link-pack");
    plugin.record.engine = StringValue(object, L"engine");
    plugin.record.description = StringValue(object, L"description", StringValue(object, L"summary"));
    plugin.record.permissions = JoinStringArray(object, L"permissions");
    plugin.record.author = StringValue(object, L"author");
    plugin.record.license = StringValue(object, L"license");
    plugin.record.packageUrl = StringValue(object, L"packageUrl");
    plugin.record.sha256 = StringValue(object, L"sha256");
    plugin.record.builtin = BoolValue(object, L"builtin", false);
    plugin.record.deletable = BoolValue(object, L"deletable", !plugin.record.builtin);
    plugin.record.enabled = BoolValue(object, L"enabledByDefault", false);
    plugin.record.installed = plugin.record.builtin;
    if (plugin.record.id.empty() || plugin.record.name.empty()) {
        lastError_ = L"插件定义缺少 id 或 name。";
        return false;
    }

    if (const JsonValue* groups = ArrayValue(object, L"groups")) {
        for (const auto& groupItem : groups->arrayValue) {
            if (!groupItem.isObject()) {
                continue;
            }
            StoreGroupDefinition group;
            group.name = StringValue(groupItem, L"name");
            if (group.name.empty()) {
                continue;
            }
            if (const JsonValue* tags = ArrayValue(groupItem, L"tags")) {
                for (const auto& tagItem : tags->arrayValue) {
                    if (!tagItem.isObject()) {
                        continue;
                    }
                    StoreTagDefinition tag;
                    tag.name = StringValue(tagItem, L"name");
                    tag.type = IntValue(tagItem, L"type", 0);
                    tag.content = StringValue(tagItem, L"content");
                    if (tag.name.empty()) {
                        continue;
                    }
                    if (const JsonValue* links = ArrayValue(tagItem, L"links")) {
                        for (const auto& linkItem : links->arrayValue) {
                            if (!linkItem.isObject()) {
                                continue;
                            }
                            StoreLinkDefinition link;
                            link.name = StringValue(linkItem, L"name");
                            link.path = StringValue(linkItem, L"path");
                            link.parameter = StringValue(linkItem, L"parameter");
                            link.workDir = StringValue(linkItem, L"workDir");
                            link.type = IntValue(linkItem, L"type", 0);
                            if (!link.name.empty() && !link.path.empty()) {
                                tag.links.push_back(std::move(link));
                            }
                        }
                    }
                    group.tags.push_back(std::move(tag));
                }
            }
            plugin.groups.push_back(std::move(group));
        }
    }

    if (const JsonValue* files = ArrayValue(object, L"files")) {
        for (const auto& fileItem : files->arrayValue) {
            if (!fileItem.isObject()) {
                continue;
            }
            StoreFileDefinition file;
            file.from = StringValue(fileItem, L"from");
            file.to = StringValue(fileItem, L"to");
            if (!file.from.empty() && !file.to.empty()) {
                plugin.files.push_back(std::move(file));
            }
        }
    }
    return ValidatePackageSafety(plugin);
}

bool PluginStoreService::InstallDefinition(const StorePluginDefinition& plugin, PluginRegistry& registry, StorageService& storage) {
    if (plugin.record.builtin) {
        return registry.SetEnabled(plugin.record.id, true);
    }

    Remove(plugin.record.id, registry, storage);
    for (const auto& file : plugin.files) {
        if (!CopyDeclaredFile(plugin, file, registry)) {
            return false;
        }
    }
    for (const auto& groupDef : plugin.groups) {
        Group group;
        group.name = groupDef.name;
        group.parentGroup = 0;
        group.pos = -1;
        if (!storage.InsertGroup(group)) {
            lastError_ = storage.lastError();
            return false;
        }
        registry.RecordContribution(plugin.record.id, L"group", group.id);

        for (const auto& tagDef : groupDef.tags) {
            Group tag;
            tag.name = tagDef.name;
            tag.parentGroup = group.id;
            tag.pos = -1;
            tag.type = tagDef.type;
            tag.content = tagDef.content;
            if (!storage.InsertGroup(tag)) {
                lastError_ = storage.lastError();
                return false;
            }
            registry.RecordContribution(plugin.record.id, L"group", tag.id);
            for (const auto& linkDef : tagDef.links) {
                Link link;
                link.name = linkDef.name;
                link.path = linkDef.path;
                link.parameter = linkDef.parameter;
                link.workDir = linkDef.workDir;
                link.type = linkDef.type;
                link.parentGroup = tag.id;
                link.pos = -1;
                if (!storage.InsertLink(link)) {
                    lastError_ = storage.lastError();
                    return false;
                }
                registry.RecordContribution(plugin.record.id, L"link", link.id);
            }
        }
    }

    if (!registry.MarkInstalled(plugin.record, (appDirectory_ / L"plugins" / L"installed" / plugin.record.id).wstring())) {
        lastError_ = registry.lastError();
        return false;
    }
    return true;
}

bool PluginStoreService::CopyDeclaredFile(const StorePluginDefinition& plugin, const StoreFileDefinition& file, PluginRegistry& registry) {
    std::filesystem::path fromRelative;
    std::filesystem::path toRelative;
    if (!ResolveSafeRelativePath(file.from, fromRelative) || !ResolveSafeRelativePath(file.to, toRelative)) {
        lastError_ = L"插件文件路径不安全。";
        return false;
    }
    if (HasDeniedExtension(fromRelative) || HasDeniedExtension(toRelative)) {
        lastError_ = L"插件文件包含不允许的可执行类型。";
        return false;
    }
    const std::filesystem::path source = plugin.packageDirectory / fromRelative;
    const std::filesystem::path target = appDirectory_ / toRelative;
    if (!FileExists(source)) {
        lastError_ = L"插件文件不存在: " + source.wstring();
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        lastError_ = L"复制插件文件失败: " + Utf8BytesToWide(ec.message());
        return false;
    }
    registry.RecordContribution(plugin.record.id, L"file", 0, toRelative.wstring());
    return true;
}

bool PluginStoreService::ResolveSafeRelativePath(const std::wstring& relative, std::filesystem::path& path) const {
    path = std::filesystem::path(Trim(relative));
    if (path.empty() || path.is_absolute()) {
        return false;
    }
    for (const auto& part : path) {
        if (part == L".." || part == L"." || part.empty()) {
            return false;
        }
    }
    return true;
}

bool PluginStoreService::ReadTextFromUrl(const std::wstring& url, std::wstring& text) {
    std::vector<std::uint8_t> bytes;
    if (!ReadBytesFromUrl(url, bytes)) {
        return false;
    }
    text = Utf8BytesToWide(std::string(bytes.begin(), bytes.end()));
    if (text.empty()) {
        lastError_ = L"插件商店内容为空或不是 UTF-8。";
        return false;
    }
    return true;
}

bool PluginStoreService::ReadBytesFromUrl(const std::wstring& url, std::vector<std::uint8_t>& bytes) {
    HINTERNET internet = InternetOpenW(L"Quattro Plugin Store", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!internet) {
        lastError_ = L"无法初始化网络访问: " + FormatLastError(GetLastError());
        return false;
    }
    HINTERNET request = InternetOpenUrlW(internet, url.c_str(), nullptr, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!request) {
        lastError_ = L"无法读取插件商店: " + FormatLastError(GetLastError());
        InternetCloseHandle(internet);
        return false;
    }

    bytes.clear();
    char buffer[4096]{};
    DWORD read = 0;
    while (InternetReadFile(request, buffer, sizeof(buffer), &read) && read > 0) {
        bytes.insert(bytes.end(), reinterpret_cast<std::uint8_t*>(buffer), reinterpret_cast<std::uint8_t*>(buffer) + read);
        read = 0;
    }
    InternetCloseHandle(request);
    InternetCloseHandle(internet);
    if (bytes.empty()) {
        lastError_ = L"下载内容为空。";
        return false;
    }
    return true;
}

bool PluginStoreService::ReadTextFromFile(const std::filesystem::path& path, std::wstring& text) {
    std::vector<std::uint8_t> bytes;
    if (!ReadBytesFromFile(path, bytes)) {
        return false;
    }
    text = Utf8BytesToWide(std::string(bytes.begin(), bytes.end()));
    if (text.empty()) {
        lastError_ = L"无法读取插件商店文件: " + path.wstring();
        return false;
    }
    return true;
}

bool PluginStoreService::ReadBytesFromFile(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        lastError_ = L"无法读取插件商店文件: " + path.wstring();
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        lastError_ = L"插件商店文件为空: " + path.wstring();
        return false;
    }
    return true;
}

std::wstring PluginStoreService::ResolvePackageUrl(const std::wstring& packageUrl) const {
    if (packageUrl.empty() || IsHttpUrl(packageUrl) || std::filesystem::path(packageUrl).is_absolute()) {
        return packageUrl;
    }
    if (IsHttpUrl(storeUrl_)) {
        const std::size_t slash = storeUrl_.find_last_of(L"/");
        if (slash != std::wstring::npos) {
            return storeUrl_.substr(0, slash + 1) + packageUrl;
        }
    }
    return packageUrl;
}

bool PluginStoreService::ValidatePackageSafety(const StorePluginDefinition& plugin) {
    if (plugin.record.kind == L"native-plugin" || plugin.record.kind == L"script-plugin") {
        lastError_ = L"当前版本不支持代码型插件。";
        return false;
    }
    for (const auto& file : plugin.files) {
        std::filesystem::path from;
        std::filesystem::path to;
        if (!ResolveSafeRelativePath(file.from, from) || !ResolveSafeRelativePath(file.to, to)) {
            lastError_ = L"插件文件路径不安全。";
            return false;
        }
        if (HasDeniedExtension(from) || HasDeniedExtension(to)) {
            lastError_ = L"插件包含不允许的可执行文件。";
            return false;
        }
    }
    return true;
}
