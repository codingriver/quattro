#pragma once

#include "ThemedUi.h"

#include <algorithm>
#include <string>

struct ImportToastSummary {
    int succeeded = 0;
    int failed = 0;
};

inline ThemedToastRole ImportToastRole(const ImportToastSummary& summary) {
    return summary.failed > 0 ? ThemedToastRole::Warning : ThemedToastRole::Success;
}

inline std::wstring ImportToastText(const ImportToastSummary& summary, const std::wstring& targetName = L"") {
    const int succeeded = std::max(0, summary.succeeded);
    const int failed = std::max(0, summary.failed);
    if (succeeded > 0 && failed > 0) {
        return L"已添加 " + std::to_wstring(succeeded) + L" 项，" + std::to_wstring(failed) + L" 项失败。";
    }
    if (succeeded > 0) {
        return targetName.empty()
            ? L"已添加 " + std::to_wstring(succeeded) + L" 项。"
            : L"已添加 " + std::to_wstring(succeeded) + L" 项到“" + targetName + L"”。";
    }
    if (failed > 0) {
        return L"未能添加 " + std::to_wstring(failed) + L" 项。";
    }
    return L"没有可添加的内容。";
}

inline ThemedToastRole OperationToastRole(bool success, bool partial) {
    if (!success) {
        return ThemedToastRole::Danger;
    }
    return partial ? ThemedToastRole::Warning : ThemedToastRole::Success;
}
