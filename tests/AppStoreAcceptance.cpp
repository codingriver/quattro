#include "../src/AppStoreConfig.h"
#include "../src/AppStoreCredentialService.h"
#include "../src/AppStoreManifest.h"
#include "../src/AppPackageService.h"
#include "../src/AppStoreRegistry.h"
#include "../src/AppStoreService.h"
#include "../src/AppTransferService.h"
#include "../src/Config.h"
#include "../src/GitHubReleaseClient.h"
#include "../src/Utilities.h"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
std::wstring NowSuffix() {
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return Hex8(StablePathHash(std::to_wstring(now))) + L"-" + std::to_wstring(GetTickCount64() & 0xffff);
}

std::string WideToUtf8Local(const std::wstring& value) {
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

bool WriteUtf8(const std::filesystem::path& path, const std::wstring& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    const std::string utf8 = WideToUtf8Local(text);
    file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    return file.good();
}

const AppStoreReleaseAsset* FindManifestAsset(const AppStoreRelease& release) {
    for (const auto& asset : release.assets) {
        if (ToLower(asset.name) == L"manifest.json") {
            return &asset;
        }
    }
    return nullptr;
}

const AppStoreEntry* FindEntry(const std::vector<AppStoreEntry>& entries, const std::wstring& appId) {
    for (const auto& entry : entries) {
        if (entry.manifest.appId == appId) {
            return &entry;
        }
    }
    return nullptr;
}

void PrintWide(const std::wstring& key, const std::wstring& value) {
    std::wcout << key << L"=" << value << L"\n";
}

int Fail(const std::wstring& message) {
    std::wcerr << L"FAIL: " << message << L"\n";
    return 1;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        return Fail(L"usage: QuattroAppStoreAcceptance.exe <conf.ini> <appDirectory>");
    }

#ifndef QUATTRO_WITH_CURL
    (void)argv;
    return Fail(L"current build has no libcurl backend");
#else
    const std::filesystem::path configPath = argv[1];
    const std::filesystem::path appDirectory = argv[2];
    ConfigService configService(configPath);
    AppConfig config = configService.Load();
    config.appStoreRepository = NormalizeAppStoreRepository(config.appStoreRepository);
    config.appStoreOwner = AppStoreRepositoryOwner(config);
    config.appStoreRepo = AppStoreRepositoryName(config);

    if (config.appStoreOwner.empty() || config.appStoreRepo.empty()) {
        return Fail(L"AppStoreRepository is empty");
    }
    std::wstring githubToken;
    std::wstring error;
    if (!AppStoreCredentialService::LoadSecret(config, AppStoreCredentialService::SecretKind::GitHubToken, githubToken, error) ||
        Trim(githubToken).empty()) {
        return Fail(error.empty() ? L"GitHub token is empty" : error);
    }
    std::wstring encryptionToken;
    if (!AppStoreCredentialService::LoadSecret(config, AppStoreCredentialService::SecretKind::PackageEncryptionToken, encryptionToken, error)) {
        return Fail(error);
    }
    const bool expectEncrypted = !Trim(encryptionToken).empty();

    GitHubReleaseClient client(config, githubToken);
    if (!client.TestConnection()) {
        return Fail(L"GitHub connection failed: " + client.lastError());
    }

    AppStoreRegistry registry(appDirectory);
    if (!registry.Initialize()) {
        return Fail(L"registry init failed: " + registry.lastError());
    }

    const std::wstring suffix = NowSuffix();
    const std::wstring appId = L"quattro-acceptance-" + suffix;
    const std::wstring version = L"0.0." + std::to_wstring(GetTickCount64() & 0xffff);
    const std::wstring originalName = L"payload-" + suffix + L".txt";
    const std::wstring initialName = L"Quattro acceptance " + suffix;
    const std::wstring renamedName = L"Quattro acceptance renamed " + suffix;
    const std::wstring payload = L"quattro app store acceptance payload " + suffix + L"\n";

    const std::filesystem::path sourceDirectory = appDirectory / L"app-store" / L"acceptance-source" / appId;
    std::error_code ec;
    std::filesystem::remove_all(sourceDirectory, ec);
    std::filesystem::create_directories(sourceDirectory, ec);
    if (!WriteUtf8(sourceDirectory / originalName, payload)) {
        return Fail(L"write payload failed");
    }
    const std::wstring manifest =
        L"{\n"
        L"  \"appId\": \"" + appId + L"\",\n"
        L"  \"name\": \"" + initialName + L"\",\n"
        L"  \"displayName\": \"" + initialName + L"\",\n"
        L"  \"version\": \"" + version + L"\",\n"
        L"  \"category\": \"other\",\n"
        L"  \"sourceKind\": \"file\",\n"
        L"  \"originalName\": \"" + originalName + L"\",\n"
        L"  \"summary\": \"temporary acceptance item\",\n"
        L"  \"draft\": true\n"
        L"}\n";
    if (!WriteUtf8(sourceDirectory / L"manifest.json", manifest)) {
        return Fail(L"write manifest failed");
    }

    std::wstring uploadTaskId;
    const long long splitSizeBytes = static_cast<long long>(std::max(16, config.appStoreSplitSizeMiB)) * 1024LL * 1024LL;
    if (!registry.AddUploadTaskPlaceholder(sourceDirectory.wstring(), config.appStoreOwner, config.appStoreRepo, splitSizeBytes, uploadTaskId)) {
        return Fail(L"add upload task failed: " + registry.lastError());
    }

    AppTransferTask uploadTask;
    if (!registry.LoadTask(uploadTaskId, uploadTask)) {
        return Fail(L"load upload task failed: " + registry.lastError());
    }
    AppTransferService transfer(appDirectory, config);
    AppTransferRunReport upload = transfer.RunUploadTask(uploadTask);
    if (!upload.ok) {
        return Fail(L"upload failed: " + upload.message);
    }

    AppStoreService store(appDirectory, config);
    if (!store.Refresh()) {
        return Fail(L"refresh after upload failed: " + store.lastError());
    }
    const AppStoreEntry* uploadedEntry = FindEntry(store.apps(), appId);
    if (!uploadedEntry) {
        return Fail(L"uploaded entry not visible in store");
    }
    if (uploadedEntry->manifest.category != L"other") {
        return Fail(L"category is not other");
    }
    if (uploadedEntry->manifest.encrypted != expectEncrypted) {
        return Fail(L"encrypted state mismatch");
    }

    AppStoreManifest renamedManifest = uploadedEntry->manifest;
    renamedManifest.displayName = renamedName;
    renamedManifest.name = uploadedEntry->manifest.name;
    renamedManifest.manifestJson = SerializeAppStoreManifest(renamedManifest);
    const std::filesystem::path renameManifestPath = appDirectory / L"app-store" / L"acceptance-source" / (appId + L"-renamed-manifest.json");
    if (!WriteUtf8(renameManifestPath, renamedManifest.manifestJson)) {
        return Fail(L"write renamed manifest failed");
    }
    if (const AppStoreReleaseAsset* oldManifest = FindManifestAsset(uploadedEntry->release)) {
        if (!client.DeleteAsset(oldManifest->id)) {
            return Fail(L"delete old manifest failed: " + client.lastError());
        }
    }
    AppStoreReleaseAsset renamedManifestAsset;
    if (!client.UploadReleaseAsset(uploadedEntry->release, renameManifestPath, L"manifest.json", renamedManifestAsset)) {
        return Fail(L"upload renamed manifest failed: " + client.lastError());
    }
    if (!client.UpdateReleaseMetadata(uploadedEntry->release.id, renamedName + L" " + version, L"temporary acceptance item renamed")) {
        return Fail(L"update release metadata failed: " + client.lastError());
    }

    if (!store.Refresh()) {
        return Fail(L"refresh after rename failed: " + store.lastError());
    }
    const AppStoreEntry* renamedEntry = FindEntry(store.apps(), appId);
    if (!renamedEntry || renamedEntry->manifest.displayName != renamedName) {
        return Fail(L"renamed entry not visible in store");
    }

    std::wstring downloadTaskId;
    if (!registry.AddDownloadTask(*renamedEntry, config.appStoreOwner, config.appStoreRepo, splitSizeBytes, downloadTaskId)) {
        return Fail(L"add download task failed: " + registry.lastError());
    }
    AppTransferTask downloadTask;
    if (!registry.LoadTask(downloadTaskId, downloadTask)) {
        return Fail(L"load download task failed: " + registry.lastError());
    }
    AppTransferRunReport download = transfer.RunDownloadTask(downloadTask);
    if (!download.ok) {
        return Fail(L"download failed: " + download.message);
    }

    const std::filesystem::path downloadedFile = appDirectory / L"app-store" / L"files" / appId / version / originalName;
    const std::wstring downloaded = LoadUtf8File(downloadedFile);
    if (downloaded != payload) {
        return Fail(L"downloaded payload mismatch: " + downloadedFile.wstring());
    }

    PrintWide(L"appId", appId);
    PrintWide(L"tag", AppStoreTagFor(config.appStoreTagPattern, appId, version));
    PrintWide(L"displayName", renamedName);
    PrintWide(L"encrypted", expectEncrypted ? L"true" : L"false");
    PrintWide(L"category", L"other");
    PrintWide(L"uploadTaskId", uploadTaskId);
    PrintWide(L"downloadTaskId", downloadTaskId);
    PrintWide(L"downloadedFile", downloadedFile.wstring());
    std::wcout << L"OK\n";
    return 0;
#endif
}
