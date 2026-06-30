#include "AppTransferService.h"

#include "AppPackageService.h"
#include "AppStoreCredentialService.h"
#include "GitHubReleaseClient.h"
#include "Utilities.h"

#include <algorithm>
#include <filesystem>
#include <map>

namespace {
const AppStoreReleaseAsset* FindAsset(const AppStoreRelease& release, const std::wstring& name) {
    for (const auto& asset : release.assets) {
        if (asset.name == name) {
            return &asset;
        }
    }
    return nullptr;
}

bool IsQuattroManagedAsset(const std::wstring& name) {
    return ToLower(name) == L"manifest.json" || ToLower(name).rfind(L"package.zip", 0) == 0;
}

bool FindReleaseByManifestTag(GitHubReleaseClient& client, const std::wstring& tag, AppStoreRelease& release) {
    if (client.GetReleaseByTag(tag, release)) {
        return true;
    }

    std::vector<AppStoreRelease> releases;
    if (!client.ListReleases(releases)) {
        return false;
    }
    for (const auto& candidate : releases) {
        const AppStoreReleaseAsset* manifestAsset = FindAsset(candidate, L"manifest.json");
        if (!manifestAsset) {
            continue;
        }
        std::wstring manifestText;
        if (!client.DownloadAssetText(manifestAsset->id, manifestText)) {
            continue;
        }
        AppStoreManifest manifest;
        std::wstring error;
        if (ParseAppStoreManifest(manifestText, manifest, error) && manifest.tag == tag) {
            release = candidate;
            return true;
        }
    }
    return false;
}
}

AppTransferService::AppTransferService(std::filesystem::path appDirectory, AppConfig config)
    : appDirectory_(std::move(appDirectory)), config_(std::move(config)) {
}

AppTransferRunReport AppTransferService::RunTask(const AppTransferTask& task) {
    if (task.direction == L"upload") {
        return RunUploadTask(task);
    }
    if (task.direction == L"download") {
        return RunDownloadTask(task);
    }
    return AppTransferRunReport{false, L"未知队列任务类型。"};
}

AppTransferRunReport AppTransferService::RunUploadTask(const AppTransferTask& task) {
    AppStoreRegistry registry(appDirectory_);
    registry.SetTaskStatus(task.id, L"running", L"packaging");
    registry.UpdateTaskProgress(task.id, 0, task.totalBytes);

    std::wstring githubToken;
    std::wstring encryptionToken;
    std::wstring error;
    if (ShouldStop(task.id, registry, error)) {
        return {false, error};
    }
    if (!AppStoreCredentialService::LoadSecret(config_, AppStoreCredentialService::SecretKind::GitHubToken, githubToken, error) || Trim(githubToken).empty()) {
        registry.SetTaskStatus(task.id, L"failed", L"auth", error.empty() ? L"上传需要 GitHub Token。" : error);
        return {false, error.empty() ? L"上传需要 GitHub Token。" : error};
    }
    if (!AppStoreCredentialService::LoadSecret(config_, AppStoreCredentialService::SecretKind::PackageEncryptionToken, encryptionToken, error)) {
        registry.SetTaskStatus(task.id, L"failed", L"auth", error);
        return {false, error};
    }

    AppConfig uploadConfig = config_;
    if (task.splitSizeBytes >= 16LL * 1024LL * 1024LL) {
        uploadConfig.appStoreSplitSizeMiB = static_cast<int>(task.splitSizeBytes / (1024LL * 1024LL));
    }
    AppPackageService packageService(appDirectory_);
    const AppPackageBuildResult package = packageService.BuildUploadPackage(task.localPath, uploadConfig, encryptionToken);
    if (!package.ok) {
        registry.SetTaskStatus(task.id, L"failed", L"packaging", package.message);
        return {false, package.message};
    }
    registry.UpdateTaskManifest(task.id, package.manifest);
    if (ShouldStop(task.id, registry, error)) {
        return {false, error};
    }

    std::map<std::wstring, AppTransferPartRecord> existingByName;
    for (const auto& part : registry.LoadParts(task.id)) {
        existingByName[part.assetName] = part;
    }
    long long transferred = 0;
    const long long total = static_cast<long long>(package.manifest.totalSize);
    for (std::size_t i = 0; i < package.manifest.parts.size(); ++i) {
        const auto& manifestPart = package.manifest.parts[i];
        const std::filesystem::path localPartPath = i < package.partPaths.size()
            ? package.partPaths[i]
            : package.partPaths.front().parent_path() / manifestPart.name;
        AppTransferPartRecord record;
        record.taskId = task.id;
        record.partIndex = manifestPart.index;
        record.assetName = manifestPart.name;
        record.localPath = localPartPath.wstring();
        record.sizeBytes = static_cast<long long>(manifestPart.size);
        record.sha256 = manifestPart.sha256;
        const auto found = existingByName.find(manifestPart.name);
        if (found != existingByName.end() && found->second.status == L"completed" && found->second.assetId > 0) {
            record.assetId = found->second.assetId;
            record.status = L"completed";
            record.transferredBytes = record.sizeBytes;
            transferred += record.sizeBytes;
        } else {
            record.status = L"queued";
        }
        registry.UpsertPart(record);
    }
    registry.UpdateTaskProgress(task.id, transferred, total);

    GitHubReleaseClient client(config_, githubToken);
    AppStoreRelease release;
    registry.SetTaskStatus(task.id, L"running", L"creatingRelease");
    if (ShouldStop(task.id, registry, error)) {
        return {false, error};
    }
    if (FindReleaseByManifestTag(client, package.manifest.tag, release)) {
        registry.SetTaskStatus(task.id, L"running", L"preparingAssets");
        for (const auto& asset : release.assets) {
            if (!IsQuattroManagedAsset(asset.name)) {
                continue;
            }
            if (ShouldStop(task.id, registry, error)) {
                return {false, error};
            }
            const auto completedPart = existingByName.find(asset.name);
            const bool keepCompletedPart =
                ToLower(asset.name) != L"manifest.json" &&
                completedPart != existingByName.end() &&
                completedPart->second.status == L"completed" &&
                completedPart->second.assetId == asset.id;
            if (!keepCompletedPart && !client.DeleteAsset(asset.id)) {
                registry.SetTaskStatus(task.id, L"failed", L"preparingAssets", client.lastError());
                return {false, client.lastError()};
            }
        }
        FindReleaseByManifestTag(client, package.manifest.tag, release);
    } else if (!client.CreateRelease(package.manifest, release)) {
        registry.SetTaskStatus(task.id, L"failed", L"creatingRelease", client.lastError());
        return {false, client.lastError()};
    }

    transferred = 0;
    for (const auto& part : registry.LoadParts(task.id)) {
        const AppStoreReleaseAsset* asset = FindAsset(release, part.assetName);
        if (part.status == L"completed" && asset && asset->id == part.assetId) {
            transferred += part.sizeBytes;
        } else if (part.status == L"completed") {
            registry.SetPartStatus(task.id, part.partIndex, L"queued", 0);
        }
    }
    registry.UpdateTaskProgress(task.id, transferred, total);

    registry.SetTaskStatus(task.id, L"running", L"uploadingPart");
    for (std::size_t i = 0; i < package.manifest.parts.size(); ++i) {
        const auto& manifestPart = package.manifest.parts[i];
        const std::filesystem::path partPath = i < package.partPaths.size()
            ? package.partPaths[i]
            : package.partPaths.front().parent_path() / manifestPart.name;
        AppTransferPartRecord current;
        const auto partRecords = registry.LoadParts(task.id);
        const auto found = std::find_if(partRecords.begin(), partRecords.end(), [&](const AppTransferPartRecord& part) {
            return part.partIndex == manifestPart.index;
        });
        const AppStoreReleaseAsset* remotePart = FindAsset(release, manifestPart.name);
        if (found != partRecords.end() && found->status == L"completed" && remotePart && remotePart->id == found->assetId) {
            registry.UpdateTaskProgress(task.id, transferred, total);
            continue;
        }
        if (ShouldStop(task.id, registry, error)) {
            return {false, error};
        }
        registry.SetPartStatus(task.id, manifestPart.index, L"running", 0);
        AppStoreReleaseAsset uploaded;
        const auto uploadProgress = [&](std::uint64_t partTransferred, std::uint64_t partTotal) {
            const long long currentPartBytes = static_cast<long long>(partTotal > 0 ? std::min(partTransferred, partTotal) : partTransferred);
            registry.SetPartStatus(task.id, manifestPart.index, L"running", currentPartBytes);
            registry.UpdateTaskProgress(task.id, std::min<long long>(total, transferred + currentPartBytes), total);
            std::wstring stopMessage;
            return !ShouldStop(task.id, registry, stopMessage);
        };
        if (!client.UploadReleaseAsset(release, partPath, manifestPart.name, uploaded, uploadProgress)) {
            if (FindReleaseByManifestTag(client, package.manifest.tag, release)) {
                if (const AppStoreReleaseAsset* duplicate = FindAsset(release, manifestPart.name)) {
                    client.DeleteAsset(duplicate->id);
                    FindReleaseByManifestTag(client, package.manifest.tag, release);
                }
            }
            if (!client.UploadReleaseAsset(release, partPath, manifestPart.name, uploaded, uploadProgress)) {
                registry.SetPartStatus(task.id, manifestPart.index, L"failed", 0, 0, client.lastError());
                registry.SetTaskStatus(task.id, L"failed", L"uploadingPart", client.lastError());
                return {false, client.lastError()};
            }
        }
        registry.SetPartStatus(task.id, manifestPart.index, L"completed", static_cast<long long>(manifestPart.size), uploaded.id);
        transferred += static_cast<long long>(manifestPart.size);
        registry.UpdateTaskProgress(task.id, std::min(transferred, total), total);
    }

    registry.SetTaskStatus(task.id, L"running", L"uploadingManifest");
    if (ShouldStop(task.id, registry, error)) {
        return {false, error};
    }
    AppStoreReleaseAsset manifestAsset;
    if (!client.UploadReleaseAsset(release, package.manifestPath, L"manifest.json", manifestAsset)) {
        registry.SetTaskStatus(task.id, L"failed", L"uploadingManifest", client.lastError());
        return {false, client.lastError()};
    }
    registry.SetTaskStatus(task.id, L"completed", L"completed");
    return {true, L"上传完成: " + package.manifest.name + L" " + package.manifest.version};
}

AppTransferRunReport AppTransferService::RunDownloadTask(const AppTransferTask& task) {
    AppStoreRegistry registry(appDirectory_);
    registry.SetTaskStatus(task.id, L"running", L"downloadingManifest");

    AppStoreManifest manifest;
    std::wstring error;
    if (ShouldStop(task.id, registry, error)) {
        return {false, error};
    }
    if (!ParseAppStoreManifest(task.manifestJson, manifest, error)) {
        registry.SetTaskStatus(task.id, L"failed", L"manifest", error);
        return {false, error};
    }

    std::wstring githubToken;
    std::wstring encryptionToken;
    if (!AppStoreCredentialService::LoadSecret(config_, AppStoreCredentialService::SecretKind::GitHubToken, githubToken, error)) {
        registry.SetTaskStatus(task.id, L"failed", L"auth", error);
        return {false, error};
    }
    if (!AppStoreCredentialService::LoadSecret(config_, AppStoreCredentialService::SecretKind::PackageEncryptionToken, encryptionToken, error)) {
        registry.SetTaskStatus(task.id, L"failed", L"auth", error);
        return {false, error};
    }
    if (manifest.encrypted && Trim(encryptionToken).empty()) {
        registry.SetTaskStatus(task.id, L"failed", L"auth", L"下载加密文件需要应用包加密 Token。");
        return {false, L"下载加密文件需要应用包加密 Token。"};
    }

    GitHubReleaseClient client(config_, githubToken);
    AppStoreRelease release;
    if (!FindReleaseByManifestTag(client, manifest.tag, release)) {
        registry.SetTaskStatus(task.id, L"failed", L"downloadingManifest", client.lastError());
        return {false, client.lastError()};
    }

    const std::filesystem::path taskDir = appDirectory_ / L"app-store" / L"downloads" / task.id;
    const std::filesystem::path partsDir = taskDir / L"parts";
    std::error_code ec;
    std::filesystem::create_directories(partsDir, ec);

    long long transferred = 0;
    const long long total = static_cast<long long>(manifest.totalSize);
    for (const auto& part : manifest.parts) {
        const std::filesystem::path partPath = partsDir / part.name;
        AppTransferPartRecord record;
        record.taskId = task.id;
        record.partIndex = part.index;
        record.assetName = part.name;
        record.localPath = partPath.wstring();
        record.sizeBytes = static_cast<long long>(part.size);
        record.sha256 = part.sha256;
        if (FileExists(partPath) && ToLower(AppStoreSha256File(partPath)) == ToLower(part.sha256)) {
            record.status = L"completed";
            record.transferredBytes = record.sizeBytes;
            transferred += record.sizeBytes;
        } else {
            record.status = L"queued";
        }
        if (const AppStoreReleaseAsset* asset = FindAsset(release, part.name)) {
            record.assetId = asset->id;
        }
        registry.UpsertPart(record);
    }
    registry.UpdateTaskProgress(task.id, transferred, total);

    registry.SetTaskStatus(task.id, L"running", L"downloadingPart");
    for (const auto& part : manifest.parts) {
        const AppStoreReleaseAsset* asset = FindAsset(release, part.name);
        if (!asset) {
            const std::wstring message = L"Release 缺少分片: " + part.name;
            registry.SetTaskStatus(task.id, L"failed", L"downloadingPart", message);
            return {false, message};
        }
        const std::filesystem::path partPath = partsDir / part.name;
        if (FileExists(partPath) && ToLower(AppStoreSha256File(partPath)) == ToLower(part.sha256)) {
            continue;
        }
        if (ShouldStop(task.id, registry, error)) {
            return {false, error};
        }
        registry.SetPartStatus(task.id, part.index, L"running", 0, asset->id);
        const auto downloadProgress = [&](std::uint64_t partTransferred, std::uint64_t partTotal) {
            const long long currentPartBytes = static_cast<long long>(partTotal > 0 ? std::min(partTransferred, partTotal) : partTransferred);
            registry.SetPartStatus(task.id, part.index, L"running", currentPartBytes, asset->id);
            registry.UpdateTaskProgress(task.id, std::min<long long>(total, transferred + currentPartBytes), total);
            std::wstring stopMessage;
            return !ShouldStop(task.id, registry, stopMessage);
        };
        if (!client.DownloadAssetFile(asset->id, partPath, downloadProgress)) {
            registry.SetPartStatus(task.id, part.index, L"failed", 0, asset->id, client.lastError());
            registry.SetTaskStatus(task.id, L"failed", L"downloadingPart", client.lastError());
            return {false, client.lastError()};
        }
        const std::wstring actual = ToLower(AppStoreSha256File(partPath));
        if (actual.empty() || actual != ToLower(part.sha256)) {
            const std::wstring message = L"下载分片校验失败: " + part.name;
            registry.SetPartStatus(task.id, part.index, L"failed", 0, asset->id, message);
            registry.SetTaskStatus(task.id, L"failed", L"downloadingPart", message);
            return {false, message};
        }
        registry.SetPartStatus(task.id, part.index, L"completed", static_cast<long long>(part.size), asset->id);
        transferred += static_cast<long long>(part.size);
        registry.UpdateTaskProgress(task.id, std::min(transferred, total), total);
    }

    AppPackageService packageService(appDirectory_);
    registry.SetTaskStatus(task.id, L"running", L"merging");
    if (ShouldStop(task.id, registry, error)) {
        return {false, error};
    }
    const std::filesystem::path packagePath = taskDir / (manifest.encrypted ? L"package.zip.enc" : L"package.zip");
    if (!packageService.AssembleParts(manifest, partsDir, packagePath, error)) {
        registry.SetTaskStatus(task.id, L"failed", L"merging", error);
        return {false, error};
    }
    registry.SetTaskStatus(task.id, L"running", L"verifying");
    if (ShouldStop(task.id, registry, error)) {
        return {false, error};
    }
    const std::filesystem::path plainZip = taskDir / L"package.zip";
    if (!packageService.DecryptPackage(manifest, packagePath, plainZip, encryptionToken, error)) {
        registry.SetTaskStatus(task.id, L"failed", L"verifying", error);
        return {false, error};
    }

    registry.SetTaskStatus(task.id, L"running", L"extracting");
    if (ShouldStop(task.id, registry, error)) {
        return {false, error};
    }
    const std::filesystem::path downloadPath = DownloadPathFor(manifest);
    if (!ExpandZip(plainZip, downloadPath, error)) {
        registry.SetTaskStatus(task.id, L"failed", L"extracting", error);
        return {false, error};
    }
    registry.SetTaskStatus(task.id, L"completed", L"completed");
    registry.UpdateTaskProgress(task.id, total, total);
    return {true, L"下载完成: " + manifest.name + L" " + manifest.version};
}

AppTransferRunReport AppTransferService::DeleteRemoteForUploadTask(const AppTransferTask& task) {
    std::wstring githubToken;
    std::wstring error;
    if (!AppStoreCredentialService::LoadSecret(config_, AppStoreCredentialService::SecretKind::GitHubToken, githubToken, error) || Trim(githubToken).empty()) {
        return {false, error.empty() ? L"删除远端 Release 需要 GitHub Token。" : error};
    }
    std::wstring tag = task.releaseTag;
    if (tag.empty() && !task.manifestJson.empty()) {
        AppStoreManifest manifest;
        if (ParseAppStoreManifest(task.manifestJson, manifest, error)) {
            tag = manifest.tag;
        }
    }
    if (tag.empty()) {
        return {true, L"任务没有远端 Release。"};
    }
    GitHubReleaseClient client(config_, githubToken);
    AppStoreRelease release;
    if (!FindReleaseByManifestTag(client, tag, release)) {
        return {true, L"远端 Release 不存在。"};
    }
    if (!client.DeleteRelease(release.id)) {
        return {false, client.lastError()};
    }
    return {true, L"远端 Release 已删除。"};
}

bool AppTransferService::ShouldStop(const std::wstring& taskId, AppStoreRegistry& registry, std::wstring& message) {
    if (!registry.IsTaskCancelRequested(taskId)) {
        return false;
    }
    message = L"任务已暂停，可稍后继续。";
    registry.SetTaskStatus(taskId, L"paused", L"paused", message);
    return true;
}

bool AppTransferService::ExpandZip(const std::filesystem::path& zipPath, const std::filesystem::path& targetDirectory, std::wstring& error) {
    std::error_code ec;
    std::filesystem::create_directories(targetDirectory, ec);
    const std::wstring script =
        L"$ErrorActionPreference='Stop';"
        L"$zip='" + ReplaceAll(zipPath.wstring(), L"'", L"''") + L"';"
        L"$dst='" + ReplaceAll(targetDirectory.wstring(), L"'", L"''") + L"';"
        L"if(Test-Path -LiteralPath $dst){Remove-Item -LiteralPath $dst -Recurse -Force};"
        L"New-Item -ItemType Directory -Force -Path $dst | Out-Null;"
        L"Expand-Archive -LiteralPath $zip -DestinationPath $dst -Force;";
    std::wstring command = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command " + QuoteForCommandLine(script);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        error = L"启动解压失败: " + FormatLastError(GetLastError());
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode != 0) {
        error = L"解压应用包失败。";
        return false;
    }
    return true;
}

std::filesystem::path AppTransferService::DownloadPathFor(const AppStoreManifest& manifest) const {
    return appDirectory_ / L"app-store" / L"files" / std::filesystem::path(manifest.appId) / std::filesystem::path(manifest.version);
}
