#pragma once

#include "Models.h"

#include <filesystem>
#include <string>
#include <vector>

struct ConfigPackageOptions {
    bool includeConfig = true;
    bool includeData = true;
    bool includeUrlIcons = true;
    bool includePluginSettings = true;
};

struct ConfigPackageReport {
    bool ok = false;
    std::wstring message;
    int groupsAdded = 0;
    int tagsAdded = 0;
    int linksAdded = 0;
    int notesAdded = 0;
    int todosAdded = 0;
    int pluginSettingsAdded = 0;
    int urlIconsAdded = 0;
    std::vector<std::wstring> warnings;
};

class ConfigPackageService {
public:
    explicit ConfigPackageService(std::filesystem::path appDirectory);

    ConfigPackageReport ExportPackage(const std::filesystem::path& targetPath, const ConfigPackageOptions& options);
    ConfigPackageReport ImportPackageMerge(const std::filesystem::path& packagePath, const ConfigPackageOptions& options);

private:
    std::filesystem::path appDirectory_;
};
