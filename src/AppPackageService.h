#pragma once

#include "AppStoreManifest.h"
#include "Models.h"

#include <filesystem>
#include <string>
#include <vector>

struct AppPackageBuildResult {
    bool ok = false;
    std::wstring message;
    AppStoreManifest manifest;
    std::filesystem::path manifestPath;
    std::filesystem::path packagePath;
    std::vector<std::filesystem::path> partPaths;
};

class AppPackageService {
public:
    explicit AppPackageService(std::filesystem::path appDirectory);

    AppPackageBuildResult BuildUploadPackage(
        const std::filesystem::path& sourceDirectory,
        const AppConfig& config,
        const std::wstring& encryptionToken);

    bool AssembleParts(const AppStoreManifest& manifest, const std::filesystem::path& partsDirectory, const std::filesystem::path& outputPath, std::wstring& error);
    bool DecryptPackage(const AppStoreManifest& manifest, const std::filesystem::path& encryptedPath, const std::filesystem::path& plainPath, const std::wstring& encryptionToken, std::wstring& error);
    bool VerifyFileSha256(const std::filesystem::path& path, const std::wstring& expected, std::wstring& error);

    const std::wstring& lastError() const { return lastError_; }

private:
    std::filesystem::path appDirectory_;
    std::wstring lastError_;
};

std::wstring AppStoreSha256File(const std::filesystem::path& path);
std::wstring AppStoreBase64Encode(const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> AppStoreBase64Decode(const std::wstring& text);
std::wstring SerializeAppStoreManifest(const AppStoreManifest& manifest);
