#pragma once

#include <filesystem>
#include <string>

struct AppLaunchLockerLocationResult {
    bool found = false;
    std::filesystem::path path;
    std::wstring message;
};

// Locates an independently installed, Authenticode-signed AppLaunchLocker
// registered through Windows App Paths with an explicit compatible protocol.
AppLaunchLockerLocationResult FindInstalledAppLaunchLocker(int requiredProtocolVersion);
bool PrepareAppLaunchLockerRuntimeResources(
    const std::filesystem::path& executablePath,
    const std::filesystem::path& sourceRoot,
    std::wstring& error);
