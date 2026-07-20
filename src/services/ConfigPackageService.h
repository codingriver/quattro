#pragma once

#include "Models.h"

#include <filesystem>
#include <string>
#include <vector>

struct ConfigPackageOptions {
    bool includeConfig = true;
    bool includeData = true;
    bool includeUrlIcons = true;
};

struct ConfigPackageReport {
    bool ok = false;
    std::wstring message;
    int groupsAdded = 0;
    int groupsMerged = 0;
    int tagsAdded = 0;
    int tagsMerged = 0;
    int linksAdded = 0;
    int linksSkippedDuplicate = 0;
    int notesAdded = 0;
    int notesMerged = 0;
    int todosAdded = 0;
    int todosUpdatedFromRemote = 0;
    int todosKeptLocal = 0;
    int todosRestored = 0;
    int todosKeptDeleted = 0;
    int todosSkippedIdentical = 0;
    int todosConflicted = 0;
    int todosRemoteDeleteConflicts = 0;
    int todosFailed = 0;
    int urlIconsAdded = 0;
    std::vector<std::wstring> warnings;
};

enum class TodoRestorePolicy {
    KeepDeleted,
    RestoreDeleted,
};

struct ConfigPackageMergePreview {
    bool ok = false;
    std::wstring message;
    std::wstring stateToken;
    int packageFormatVersion = 0;
    std::vector<std::wstring> deletedTodoTitles;
    std::vector<std::wstring> warnings;
};

class ConfigPackageService {
public:
    explicit ConfigPackageService(std::filesystem::path appDirectory);

    ConfigPackageReport ExportPackage(const std::filesystem::path& targetPath, const ConfigPackageOptions& options);
    ConfigPackageReport ImportPackageMerge(const std::filesystem::path& packagePath, const ConfigPackageOptions& options);
    ConfigPackageMergePreview PreviewPackageMerge(const std::filesystem::path& packagePath, const ConfigPackageOptions& options);
    ConfigPackageReport ApplyPackageMerge(
        const std::filesystem::path& packagePath,
        const ConfigPackageOptions& options,
        TodoRestorePolicy restorePolicy,
        const std::wstring& expectedStateToken = {});

private:
    std::filesystem::path appDirectory_;
};
