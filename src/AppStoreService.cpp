#include "AppStoreService.h"

#include "AppStoreCredentialService.h"
#include "GitHubReleaseClient.h"
#include "Utilities.h"

#include <algorithm>

namespace {
const AppStoreReleaseAsset* FindManifestAsset(const AppStoreRelease& release) {
    for (const auto& asset : release.assets) {
        if (ToLower(asset.name) == L"manifest.json") {
            return &asset;
        }
    }
    return nullptr;
}
}

AppStoreService::AppStoreService(std::filesystem::path appDirectory, AppConfig config)
    : appDirectory_(std::move(appDirectory)), config_(std::move(config)) {
}

bool AppStoreService::Refresh() {
    (void)appDirectory_;
    lastError_.clear();
    apps_.clear();

    std::wstring githubToken;
    std::wstring credentialError;
    if (!AppStoreCredentialService::LoadSecret(config_, AppStoreCredentialService::SecretKind::GitHubToken, githubToken, credentialError)) {
        lastError_ = credentialError;
        return false;
    }
    std::wstring encryptionToken;
    if (!AppStoreCredentialService::LoadSecret(config_, AppStoreCredentialService::SecretKind::PackageEncryptionToken, encryptionToken, credentialError)) {
        lastError_ = credentialError;
        return false;
    }

    GitHubReleaseClient client(config_, githubToken);
    std::vector<AppStoreRelease> releases;
    if (!client.ListReleases(releases)) {
        lastError_ = client.lastError();
        return false;
    }

    for (const auto& release : releases) {
        if (release.draft && !config_.appStoreIncludeDrafts) {
            continue;
        }
        const AppStoreReleaseAsset* manifestAsset = FindManifestAsset(release);
        if (!manifestAsset) {
            continue;
        }
        std::wstring manifestText;
        if (!client.DownloadAssetText(manifestAsset->id, manifestText)) {
            continue;
        }
        AppStoreManifest manifest;
        std::wstring parseError;
        if (!ParseAppStoreManifest(manifestText, manifest, parseError)) {
            continue;
        }
        manifest.draft = release.draft;
        std::wstring reason;
        if (!IsManifestCompleteForRelease(manifest, release, !Trim(encryptionToken).empty(), reason)) {
            continue;
        }
        apps_.push_back(AppStoreEntry{release, manifest});
    }

    std::sort(apps_.begin(), apps_.end(), [](const AppStoreEntry& left, const AppStoreEntry& right) {
        const std::wstring leftName = ToLower(left.manifest.name.empty() ? left.manifest.appId : left.manifest.name);
        const std::wstring rightName = ToLower(right.manifest.name.empty() ? right.manifest.appId : right.manifest.name);
        if (leftName != rightName) {
            return leftName < rightName;
        }
        return ToLower(left.manifest.version) > ToLower(right.manifest.version);
    });
    return true;
}
