#include "AppStoreManifest.h"

#include "JsonValue.h"
#include "Utilities.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace {
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

std::uint64_t UInt64Value(const JsonValue& value, const wchar_t* key, std::uint64_t fallback = 0) {
    const JsonValue* child = value.get(key);
    if (!child || !child->isNumber() || child->numberValue < 0) {
        return fallback;
    }
    return static_cast<std::uint64_t>(std::llround(child->numberValue));
}

int IntValue(const JsonValue& value, const wchar_t* key, int fallback = 0) {
    const JsonValue* child = value.get(key);
    return child ? child->intOr(fallback) : fallback;
}

}

std::wstring AppStoreTagFor(const std::wstring& pattern, const std::wstring& appId, const std::wstring& version) {
    std::wstring value = pattern.empty() ? L"{appId}-v{version}" : pattern;
    value = ReplaceAll(value, L"{appId}", appId);
    value = ReplaceAll(value, L"{version}", version);
    return value;
}

bool IsSafeAppRelativePath(const std::wstring& path) {
    const std::wstring text = Trim(path);
    if (text.empty()) {
        return false;
    }
    std::filesystem::path parsed(text);
    if (parsed.is_absolute()) {
        return false;
    }
    for (const auto& part : parsed) {
        if (part == L"..") {
            return false;
        }
    }
    return text.find(L':') == std::wstring::npos;
}

bool ParseAppStoreManifest(const std::wstring& text, AppStoreManifest& manifest, std::wstring& error) {
    manifest = {};
    manifest.manifestJson = text;
    JsonValue root;
    if (!ParseJson(text, root, error)) {
        return false;
    }
    if (!root.isObject()) {
        error = L"manifest.json 不是对象。";
        return false;
    }

    manifest.schema = IntValue(root, L"schema", 0);
    manifest.appId = StringValue(root, L"appId");
    manifest.name = StringValue(root, L"name");
    manifest.displayName = StringValue(root, L"displayName", manifest.name);
    manifest.version = StringValue(root, L"version");
    manifest.tag = StringValue(root, L"tag");
    manifest.category = ToLower(StringValue(root, L"category", L"app"));
    manifest.sourceKind = ToLower(StringValue(root, L"sourceKind"));
    manifest.originalName = StringValue(root, L"originalName");
    manifest.summary = StringValue(root, L"summary");
    manifest.description = StringValue(root, L"description");
    manifest.author = StringValue(root, L"author");
    manifest.homepage = StringValue(root, L"homepage");
    manifest.license = StringValue(root, L"license");
    manifest.draft = BoolValue(root, L"draft", false);

    const JsonValue* package = ObjectValue(root, L"package");
    if (!package) {
        error = L"manifest.json 缺少 package。";
        return false;
    }
    manifest.packageFormat = StringValue(*package, L"format");
    manifest.encrypted = BoolValue(*package, L"encrypted", false);
    manifest.splitSize = UInt64Value(*package, L"splitSize", 0);
    manifest.totalSize = UInt64Value(*package, L"totalSize", 0);
    manifest.packageSha256 = StringValue(*package, L"sha256");
    manifest.plainSha256 = StringValue(*package, L"plainSha256");

    const JsonValue* parts = ArrayValue(*package, L"parts");
    if (parts) {
        for (const auto& item : parts->arrayValue) {
            if (!item.isObject()) {
                continue;
            }
            AppPackagePart part;
            part.index = IntValue(item, L"index", 0);
            part.name = StringValue(item, L"name");
            part.size = UInt64Value(item, L"size", 0);
            part.sha256 = StringValue(item, L"sha256");
            manifest.parts.push_back(std::move(part));
        }
    }

    if (const JsonValue* encryption = ObjectValue(root, L"encryption")) {
        manifest.encryptionEnabled = BoolValue(*encryption, L"enabled", false);
        manifest.encryptionAlgorithm = StringValue(*encryption, L"algorithm");
        manifest.encryptionKdf = StringValue(*encryption, L"kdf");
        manifest.encryptionIterations = IntValue(*encryption, L"iterations", 0);
        manifest.encryptionSalt = StringValue(*encryption, L"salt");
        manifest.encryptionNonce = StringValue(*encryption, L"nonce");
        manifest.encryptionTag = StringValue(*encryption, L"tag");
    }

    if (manifest.schema != 1 || manifest.appId.empty() || manifest.name.empty() || manifest.version.empty() || manifest.tag.empty()) {
        error = L"manifest.json 缺少必填字段。";
        return false;
    }
    if (manifest.category.empty()) {
        manifest.category = L"app";
    }
    if (manifest.category != L"app" && manifest.category != L"other") {
        error = L"manifest.json category 只支持 app 或 other。";
        return false;
    }
    if (manifest.displayName.empty()) {
        manifest.displayName = manifest.name;
    }
    if (manifest.packageFormat != L"zip" || manifest.packageSha256.empty() || manifest.parts.empty()) {
        error = L"manifest.json package 不完整。";
        return false;
    }
    if (manifest.encrypted && manifest.plainSha256.empty()) {
        error = L"加密应用缺少 plainSha256。";
        return false;
    }
    std::sort(manifest.parts.begin(), manifest.parts.end(), [](const auto& left, const auto& right) {
        return left.index < right.index;
    });
    for (int i = 0; i < static_cast<int>(manifest.parts.size()); ++i) {
        const AppPackagePart& part = manifest.parts[static_cast<std::size_t>(i)];
        if (part.index != i + 1 || part.name.empty() || part.size == 0 || part.sha256.empty()) {
            error = L"manifest.json 分片定义不完整。";
            return false;
        }
    }
    return true;
}

bool IsManifestCompleteForRelease(
    const AppStoreManifest& manifest,
    const AppStoreRelease& release,
    bool hasPackageEncryptionToken,
    std::wstring& reason) {
    reason.clear();
    if (release.tagName != manifest.tag) {
        reason = L"Release tag 与 manifest tag 不一致。";
        return false;
    }
    if (manifest.tag != AppStoreTagFor(L"{appId}-v{version}", manifest.appId, manifest.version)) {
        reason = L"manifest tag 不符合 {appId}-v{version}。";
        return false;
    }
    if (manifest.encrypted && !hasPackageEncryptionToken) {
        reason = L"应用已加密，但本机未配置应用包加密 Token。";
        return false;
    }
    std::map<std::wstring, std::uint64_t> assetSizes;
    for (const auto& asset : release.assets) {
        assetSizes[asset.name] = asset.size;
    }
    for (const auto& part : manifest.parts) {
        const auto found = assetSizes.find(part.name);
        if (found == assetSizes.end()) {
            reason = L"缺少分片: " + part.name;
            return false;
        }
        if (found->second != part.size) {
            reason = L"分片大小不匹配: " + part.name;
            return false;
        }
    }
    return true;
}
