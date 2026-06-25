#pragma once

#include <filesystem>
#include <string>

struct EmbeddedAssetInstallResult {
    std::filesystem::path appDirectory;
    bool usedFallbackDirectory = false;
    int filesWritten = 0;
    int failures = 0;
    std::wstring message;
};

EmbeddedAssetInstallResult PrepareEmbeddedAssets(const std::filesystem::path& moduleDirectory);
