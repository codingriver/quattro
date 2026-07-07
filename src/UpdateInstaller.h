#pragma once

#include <filesystem>
#include <string>

struct UpdateInstallPlan {
    std::filesystem::path downloadedExe;
    std::filesystem::path currentExe;
    std::filesystem::path logPath;
};

bool LaunchEmbeddedUpdater(const UpdateInstallPlan& plan, std::wstring& error);
