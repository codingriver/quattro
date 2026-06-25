#pragma once

#include "Models.h"

#include <filesystem>
#include <string>

class Launcher {
public:
    Launcher(std::filesystem::path appDirectory, AppConfig* config);

    bool Run(Link& link, std::wstring& errorMessage) const;
    static bool IsUrlTarget(const Link& link);
    static bool IsShellTarget(const Link& link);
    static std::wstring BuildOpenDirCommand(const std::wstring& commandTemplate, const Link& link);

private:
    bool ExecuteOpenDirCommand(const Link& link, std::wstring& errorMessage) const;

    std::filesystem::path appDirectory_;
    AppConfig* config_ = nullptr;
};
