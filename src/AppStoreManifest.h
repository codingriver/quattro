#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct AppPackagePart {
    int index = 0;
    std::wstring name;
    std::uint64_t size = 0;
    std::wstring sha256;
};

struct AppInstallStep {
    std::wstring type;
    std::wstring path;
    std::wstring args;
    bool requiresAdmin = false;
    bool optional = false;
};

struct AppShortcutDefinition {
    std::wstring name;
    std::wstring target;
    std::wstring args;
    std::wstring workDir;
    std::wstring icon;
    std::wstring location;
};

struct AppStoreManifest {
    int schema = 0;
    std::wstring appId;
    std::wstring name;
    std::wstring displayName;
    std::wstring version;
    std::wstring tag;
    std::wstring category = L"app";
    std::wstring sourceKind;
    std::wstring originalName;
    std::wstring summary;
    std::wstring description;
    std::wstring author;
    std::wstring homepage;
    std::wstring license;
    bool draft = false;

    std::wstring packageFormat;
    bool encrypted = false;
    std::uint64_t splitSize = 0;
    std::uint64_t totalSize = 0;
    std::wstring packageSha256;
    std::wstring plainSha256;
    std::vector<AppPackagePart> parts;

    bool encryptionEnabled = false;
    std::wstring encryptionAlgorithm;
    std::wstring encryptionKdf;
    int encryptionIterations = 0;
    std::wstring encryptionSalt;
    std::wstring encryptionNonce;
    std::wstring encryptionTag;

    std::wstring installTarget;
    bool overwrite = true;
    std::vector<AppInstallStep> installSteps;
    std::vector<AppShortcutDefinition> shortcuts;
    std::vector<AppInstallStep> uninstallSteps;
    bool deleteInstallDir = true;
    bool removeShortcuts = true;

    std::wstring manifestJson;
};

struct AppStoreReleaseAsset {
    long long id = 0;
    std::wstring name;
    std::uint64_t size = 0;
    std::wstring browserDownloadUrl;
};

struct AppStoreRelease {
    long long id = 0;
    std::wstring tagName;
    std::wstring name;
    std::wstring uploadUrl;
    bool draft = false;
    bool prerelease = false;
    std::wstring publishedAt;
    std::vector<AppStoreReleaseAsset> assets;
};

struct AppStoreEntry {
    AppStoreRelease release;
    AppStoreManifest manifest;
};

bool ParseAppStoreManifest(const std::wstring& text, AppStoreManifest& manifest, std::wstring& error);
std::wstring AppStoreTagFor(const std::wstring& pattern, const std::wstring& appId, const std::wstring& version);
bool IsSafeAppRelativePath(const std::wstring& path);
bool IsManifestCompleteForRelease(
    const AppStoreManifest& manifest,
    const AppStoreRelease& release,
    bool hasPackageEncryptionToken,
    std::wstring& reason);
