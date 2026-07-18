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
    bool SaveNotePage(int tagId, const std::wstring& content);
    bool InsertTodoItem(TodoItem& item);
    bool UpdateTodoItem(const TodoItem& item);
    bool DeleteTodoItem(int todoId);
    bool SetTodoCompleted(int todoId, bool completed);
    bool SetTodoEnabled(int todoId, bool enabled);
    bool UpdateTodoReminderState(const TodoItem& item);
    const std::wstring& lastError() const { return lastError_; }
    bool sqliteAvailable() const { return sqliteAvailable_; }

private:
    AppModel LoadFallback() const;
    void EnsureDefaultData(AppModel& model) const;

    std::filesystem::path appDirectory_;
    std::wstring lastError_;
    bool sqliteAvailable_ = false;
};
