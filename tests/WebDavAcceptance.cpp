#include "../src/services/ConfigPackageService.h"
#include "../src/services/Storage.h"
#include "../src/common/Utilities.h"
#include "../src/services/WebDavBackupService.h"
#include "../src/services/WebDavCredentialService.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
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
        std::wcout << L"WebDAV acceptance skipped: set QUATTRO_WEBDAV_URL, QUATTRO_WEBDAV_USER and QUATTRO_WEBDAV_PASSWORD to run it.\n";
        return 0;
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
    config.webDavBackupPath = L"/" + runId + L"/backups/";
    config.webDavFilesPath = L"/" + runId + L"/files/";
    config.webDavUserName = user;
    config.webDavKeepCount = 5;

    if (Env(L"QUATTRO_WEBDAV_DIRECTORY_DELETE_ONLY") == L"1") {
        WebDavClient client(config, password);
        const std::wstring recordId(64, L'd');
        const std::wstring recordPath = WebDavClient::CombineRemotePath(config.webDavFilesPath, recordId);
        const std::filesystem::path payload = sourceRoot / L"payload.txt";
        std::filesystem::create_directories(sourceRoot, ec);
        {
            std::ofstream stream(payload, std::ios::binary | std::ios::trunc);
            stream << "webdav collection delete acceptance";
        }
        const bool prepared =
            Check(client.EnsureDirectory(recordPath), L"webdav create isolated record directory") &&
            Check(client.UploadFile(payload, WebDavClient::CombineRemotePath(recordPath, L"content")),
                L"webdav upload isolated content") &&
            Check(client.UploadFile(payload, WebDavClient::CombineRemotePath(recordPath, L"metadata.json")),
                L"webdav upload isolated metadata") &&
            Check(client.DeleteRemoteFile(WebDavClient::CombineRemotePath(recordPath, L"content")),
                L"webdav delete isolated content") &&
            Check(client.DeleteRemoteFile(WebDavClient::CombineRemotePath(recordPath, L"metadata.json")),
                L"webdav delete isolated metadata") &&
            Check(client.DeleteRemoteDirectory(recordPath), L"webdav delete isolated record directory");
        std::vector<WebDavRemoteFile> entries;
        const bool listed = Check(client.ListFiles(config.webDavFilesPath, entries),
            L"webdav list isolated parent after delete");
        const bool absent = listed && std::none_of(entries.begin(), entries.end(),
            [&](const WebDavRemoteFile& entry) { return entry.name == recordId; });
        Check(absent, L"webdav deleted record directory absent from parent");
        client.DeleteRemoteDirectory(config.webDavFilesPath);
        client.DeleteRemoteDirectory(config.webDavRemotePath);
        std::filesystem::remove_all(sourceRoot, ec);
        return prepared && absent ? 0 : 1;
    }

    std::wstring error;
    if (!WebDavCredentialService::SavePassword(config, password, error)) {
        std::wcerr << error << L"\n";
        return 1;
    }

    int result = 1;
    WebDavClient cleanupClient(config, password);
    std::wstring uploadedName;
    std::wstring uploadedFileRecordId;
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
        sourceTag.type = 4;
        sourceTag.content = L"todoItems";
        sourceTag.pos = -1;
        if (!Check(sourceStorage.InsertGroup(sourceTag), L"source tag insert")) break;

        Link sourceLink;
        sourceLink.name = L"WebDavAcceptanceLink";
        sourceLink.parentGroup = sourceTag.id;
        sourceLink.path = L"https://webdav.acceptance.example";
        sourceLink.type = 2;
        sourceLink.pos = -1;
        if (!Check(sourceStorage.InsertLink(sourceLink), L"source link insert")) break;

        TodoItem sourceTodo;
        sourceTodo.tagId = sourceTag.id;
        sourceTodo.title = L"WebDavAcceptanceTodo";
        sourceTodo.content = L"webdav todo content";
        sourceTodo.scheduleKind = TodoScheduleKind::None;
        sourceTodo.pos = -1;
        if (!Check(sourceStorage.InsertTodoItem(sourceTodo), L"source todo insert")) break;

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

        if (!Check(cleanupClient.EnsureDirectory(config.webDavBackupPath), L"webdav create remote directory")) {
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
        int importedTodoCount = 0;
        for (const auto& item : merged.links) {
            hasExisting = hasExisting || item.name == L"ExistingTargetLink";
            hasImported = hasImported || (item.name == L"WebDavAcceptanceLink" && item.path == L"https://webdav.acceptance.example");
        }
        for (const auto& item : merged.todos) {
            if (item.title == L"WebDavAcceptanceTodo") ++importedTodoCount;
        }
        if (!Check(hasExisting, L"merge keeps existing target data")) break;
        if (!Check(hasImported, L"merge imports downloaded backup data")) break;
        if (!Check(importedTodoCount == 1, L"merge imports downloaded todo once")) break;

        WebDavBackupReport repeatedDownload = targetBackup.DownloadAndImportMerge(uploadedName);
        if (!Check(repeatedDownload.ok && repeatedDownload.importReport.todosAdded == 0 &&
                repeatedDownload.importReport.todosSkippedIdentical == 1,
                L"repeated webdav download is idempotent")) break;
        merged = targetStorage.Load();
        importedTodoCount = 0;
        for (const auto& item : merged.todos) {
            if (item.title == L"WebDavAcceptanceTodo") ++importedTodoCount;
        }
        if (!Check(importedTodoCount == 1, L"repeated webdav download does not duplicate todo")) break;

        const std::filesystem::path uploadedFile = sourceRoot / L"webdav-file-delete.txt";
        {
            std::ofstream stream(uploadedFile, std::ios::binary | std::ios::trunc);
            stream << "webdav directory delete acceptance";
        }
        uploadedFileRecordId = std::wstring(64, L'd');
        const std::wstring fileRecordPath =
            WebDavClient::CombineRemotePath(config.webDavFilesPath, uploadedFileRecordId);
        if (!Check(cleanupClient.EnsureDirectory(fileRecordPath), L"webdav file record directory create") ||
            !Check(cleanupClient.UploadFile(uploadedFile,
                WebDavClient::CombineRemotePath(fileRecordPath, L"content")),
                L"webdav file record content upload") ||
            !Check(cleanupClient.UploadFile(uploadedFile,
                WebDavClient::CombineRemotePath(fileRecordPath, L"metadata.json")),
                L"webdav file record metadata upload") ||
            !Check(cleanupClient.DeleteRemoteFile(
                WebDavClient::CombineRemotePath(fileRecordPath, L"content")),
                L"webdav file record content delete") ||
            !Check(cleanupClient.DeleteRemoteFile(
                WebDavClient::CombineRemotePath(fileRecordPath, L"metadata.json")),
                L"webdav file record metadata delete") ||
            !Check(cleanupClient.DeleteRemoteDirectory(fileRecordPath),
                L"webdav file record directory delete")) {
            std::wcerr << cleanupClient.lastError() << L"\n";
            break;
        }
        std::vector<WebDavRemoteFile> fileEntries;
        if (!Check(cleanupClient.ListFiles(config.webDavFilesPath, fileEntries),
                L"webdav list files after record delete")) {
            std::wcerr << cleanupClient.lastError() << L"\n";
            break;
        }
        const bool deletedRecordStillListed = std::any_of(fileEntries.begin(), fileEntries.end(),
            [&](const WebDavRemoteFile& entry) { return entry.name == uploadedFileRecordId; });
        if (!Check(!deletedRecordStillListed, L"webdav file record directory removed from parent")) break;
        uploadedFileRecordId.clear();
        result = 0;
    } while (false);

    if (!uploadedName.empty()) {
        cleanupClient.DeleteRemoteFile(WebDavClient::CombineRemotePath(config.webDavBackupPath, uploadedName));
    }
    if (!uploadedFileRecordId.empty()) {
        cleanupClient.DeleteRemoteDirectory(
            WebDavClient::CombineRemotePath(config.webDavFilesPath, uploadedFileRecordId));
    }
    cleanupClient.DeleteRemoteDirectory(config.webDavBackupPath);
    cleanupClient.DeleteRemoteDirectory(config.webDavFilesPath);
    cleanupClient.DeleteRemoteDirectory(config.webDavRemotePath);
    WebDavCredentialService::DeletePassword(config, error);
    std::filesystem::remove_all(sourceRoot, ec);
    std::filesystem::remove_all(targetRoot, ec);
    return result;
}
