#pragma once

#include "ShellItemService.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct TerminalContextMenuProgram {
    std::wstring id;
    std::wstring text;
    std::wstring executable;
    ShellContextMenuItem iconTemplate;
};

struct TerminalContextMenuRefreshContext {
    std::vector<TerminalContextMenuProgram> programs;
};

class TerminalContextMenuService {
public:
    static TerminalContextMenuRefreshContext DetectAvailablePrograms();
    static std::vector<ShellContextMenuItem> ItemsFor(
        const Link& link,
        const TerminalContextMenuRefreshContext& context);
    static bool Invoke(
        HWND owner,
        const ShellContextMenuLocator& locator,
        std::wstring& errorMessage);
    static std::optional<std::filesystem::path> WorkingDirectoryFor(const Link& link);
};
