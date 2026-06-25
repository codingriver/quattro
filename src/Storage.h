#pragma once

#include "Models.h"

#include <filesystem>
#include <string>

class StorageService {
public:
    explicit StorageService(std::filesystem::path appDirectory);

    AppModel Load();
    bool InsertGroup(Group& group);
    bool UpdateGroup(const Group& group);
    bool DeleteGroup(int groupId);
    bool InsertLink(Link& link);
    bool UpdateLink(const Link& link);
    bool DeleteLink(int linkId);
    bool IncrementRunCount(int linkId, int runCount);
    const std::wstring& lastError() const { return lastError_; }
    bool sqliteAvailable() const { return sqliteAvailable_; }

private:
    AppModel LoadFallback() const;
    void EnsureDefaultData(AppModel& model) const;

    std::filesystem::path appDirectory_;
    std::wstring lastError_;
    bool sqliteAvailable_ = false;
};
