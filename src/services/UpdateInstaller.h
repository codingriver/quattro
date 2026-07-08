#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct UpdateInstallPlan {
    std::filesystem::path downloadedExe;
    std::filesystem::path currentExe;
    std::filesystem::path logPath;
    std::wstring latestVersion;
    std::wstring assetName;
    std::uint64_t assetSizeBytes = 0;
};

bool LaunchEmbeddedUpdater(const UpdateInstallPlan& plan, std::wstring& error);
