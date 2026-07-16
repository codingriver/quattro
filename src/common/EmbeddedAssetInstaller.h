#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct EmbeddedAssetInstallResult {
    std::filesystem::path appDirectory;
    bool usedFallbackDirectory = false;
    int filesWritten = 0;
    int filesUpdated = 0;
    int filesBackedUp = 0;
    int filesSkipped = 0;
    int filesDecompressed = 0;
    int rawAssets = 0;
    int compressedAssets = 0;
    int failures = 0;
    std::int64_t validationMilliseconds = 0;
    std::int64_t decompressionMilliseconds = 0;
    std::int64_t totalMilliseconds = 0;
    std::filesystem::path backupDirectory;
    std::wstring message;
};

EmbeddedAssetInstallResult PrepareEmbeddedAssets(const std::filesystem::path& moduleDirectory);
