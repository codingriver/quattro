#pragma once

#include "Models.h"
#include "Theme.h"
#include "WebDavFileService.h"

#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

class WebDavTransferCoordinator final {
public:
    static bool SubmitUploads(const std::vector<std::filesystem::path>& paths, std::wstring& error);
    static bool SubmitDownloads(const std::vector<WebDavFileRecord>& records, std::wstring& error);
    static bool ShowQueue(std::wstring& error);
    static int RunHost(HINSTANCE instance, const Theme& theme, const AppConfig& config);
};
