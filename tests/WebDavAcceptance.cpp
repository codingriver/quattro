#include "../src/ConfigPackageService.h"
#include "../src/Storage.h"
#include "../src/Utilities.h"
#include "../src/WebDavBackupService.h"
#include "../src/WebDavCredentialService.h"

#include <filesystem>
#include <iostream>

namespace {
std::wstring Env(const wchar_t* name) {
    DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) {
        return {};
    }
    std::wstring value(size, L'\0');
    DWORD copied = GetEnvironmentVariableW(name, value.data(), size);
    if (copied == 0 || copied >= size) {
        return {};
    }
    value.resize(copied);
    return value;
}

bool Check(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << L"FAILED: " << message << L"\n";
        return false;
    }
    std::wcout << L"passed: " << message << L"\n";
    return true;
}
}

int wmain() {
    const std::wstring url = Env(L"QUATTRO_WEBDAV_URL");
    const std::wstring user = Env(L"QUATTRO_WEBDAV_USER");
    const std::wstring password = Env(L"QUATTRO_WEBDAV_PASSWORD");
    if (url.empty() || user.empty() || password.empty()) {
        std::wcerr << L"Set QUATTRO_WEBDAV_URL, QUATTRO_WEBDAV_USER and QUATTRO_WEBDAV_PASSWORD.\n";
        return 2;
    }

    std::error_code ec;
    const std::wstring runId = L"codex-webdav-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const std::filesystem::path sourceRoot = std::filesystem::temp_directory_path() / (runId + L"-source");
    const std::filesystem::path targetRoot = std::filesystem::temp_directory_path() / (runId + L"-target");
    std::filesystem::remove_all(sourceRoot, ec);
    std::filesystem::remove_all(targetRoot, ec);

    AppConfig config;
    config.webDavEnabled = true;
    config.webDavUrl = url;
    config.webDavRemotePath = L"/" + runId + L"/";
    config.webDavUserName = user;
    config.webDavKeepCount = 5;

    std::wstring error;
    if (!WebDavCredentialService::SavePassword(config, password, error)) {
        std::wcerr << error << L"\n";
        return 1;
    }

    int result = 1;
    WebDavClient cleanupClient(config, password);
    std::wstring uploadedName;
    do {
        StorageService sourceStorage(sourceRoot);
        sourceStorage.Load();
        Group sourceGroup;
        sourceGroup.name = L"WebDavAcceptanceGroup";
        sourceGroup.parentGroup = 0;
        sourceGroup.pos = -1;
        if (!Check(sourceStorage.InsertGroup(sourceGroup), L"source group insert")) break;

        Group sourceTag;
        sourceTag.name = L"WebDavAcceptanceTag";
        sourceTag.parentGroup = sourceGroup.id;
        sourceTag.pos = -1;
        if (!Check(sourceStorage.InsertGroup(sourceTag), L"source tag insert")) break;

        Link sourceLink;
        sourceLink.name = L"WebDavAcceptanceLink";
        sourceLink.parentGroup = sourceTag.id;
        sourceLink.path = L"https://webdav.acceptance.example";
        sourceLink.type = 2;
        sourceLink.pos = -1;
        if (!Check(sourceStorage.InsertLink(sourceLink), L"source link insert")) break;

        StorageService targetStorage(targetRoot);
        targetStorage.Load();
        Group targetGroup;
        targetGroup.name = L"ExistingTargetGroup";
        targetGroup.parentGroup = 0;
        targetGroup.pos = -1;
        if (!Check(targetStorage.InsertGroup(targetGroup), L"target existing group insert")) break;

        Link targetLink;
        Group targetTag;
        targetTag.name = L"ExistingTargetTag";
        targetTag.parentGroup = targetGroup.id;
        targetTag.pos = -1;
        if (!Check(targetStorage.InsertGroup(targetTag), L"target existing tag insert")) break;
        targetLink.name = L"ExistingTargetLink";
        targetLink.parentGroup = targetTag.id;
        targetLink.path = L"https://target.existing.example";
        targetLink.type = 2;
        targetLink.pos = -1;
        if (!Check(targetStorage.InsertLink(targetLink), L"target existing link insert")) break;

        if (!Check(cleanupClient.EnsureDirectory(config.webDavRemotePath), L"webdav create remote directory")) {
            std::wcerr << cleanupClient.lastError() << L"\n";
            break;
        }

        WebDavBackupService sourceBackup(sourceRoot, config);
        std::wstring testError;
        if (!Check(sourceBackup.TestConnection(testError), L"webdav test connection")) {
            std::wcerr << testError << L"\n";
            break;
        }

        WebDavBackupReport upload = sourceBackup.UploadBackup();
        if (!Check(upload.ok, L"webdav upload backup")) {
            std::wcerr << upload.message << L"\n";
            break;
        }
        uploadedName = upload.remoteName;
        std::wcout << L"uploaded=" << uploadedName << L"\n";

        WebDavBackupService targetBackup(targetRoot, config);
        std::vector<WebDavRemoteFile> backups;
        if (!Check(targetBackup.ListBackups(backups, testError), L"webdav list backups")) {
            std::wcerr << testError << L"\n";
            break;
        }
        bool foundRemote = false;
        for (const auto& item : backups) {
            foundRemote = foundRemote || item.name == uploadedName;
        }
        if (!Check(foundRemote, L"uploaded backup visible in remote list")) break;

        WebDavBackupReport download = targetBackup.DownloadAndImportMerge(uploadedName);
        if (!Check(download.ok, L"webdav download and merge import")) {
            std::wcerr << download.message << L"\n";
            break;
        }

        AppModel merged = targetStorage.Load();
        bool hasExisting = false;
        bool hasImported = false;
        for (const auto& item : merged.links) {
            hasExisting = hasExisting || item.name == L"ExistingTargetLink";
            hasImported = hasImported || (item.name == L"WebDavAcceptanceLink" && item.path == L"https://webdav.acceptance.example");
        }
        if (!Check(hasExisting, L"merge keeps existing target data")) break;
        if (!Check(hasImported, L"merge imports downloaded backup data")) break;
        result = 0;
    } while (false);

    if (!uploadedName.empty()) {
        cleanupClient.DeleteRemoteFile(WebDavClient::CombineRemotePath(config.webDavRemotePath, uploadedName));
    }
    cleanupClient.DeleteRemoteFile(config.webDavRemotePath);
    WebDavCredentialService::DeletePassword(config, error);
    std::filesystem::remove_all(sourceRoot, ec);
    std::filesystem::remove_all(targetRoot, ec);
    return result;
}
