#pragma once

#include "Models.h"

#include <shlobj.h>
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct ShellItemRef {
    std::wstring displayName;
    std::wstring parseName;
    std::wstring fileSystemPath;
    std::vector<std::uint8_t> pidl;
    bool isFileSystem = false;
    bool isVirtual = false;
};

class ShellItemService {
public:
    static bool IsShellParseName(const std::wstring& value);
    static bool IsPidlBlobPlausible(const std::vector<std::uint8_t>& blob);
    static std::optional<ShellItemRef> FromAbsolutePidl(PCIDLIST_ABSOLUTE pidl);
    static std::optional<ShellItemRef> FromPathOrParseName(const std::wstring& value);
    static std::optional<ShellItemRef> FromPidlBlob(const std::vector<std::uint8_t>& blob);
    static bool RefreshLinkShellData(Link& link, bool clearOnFailure);
    static bool OpenShellTarget(HWND owner, const Link& link, int showCmd, std::wstring& errorMessage);
    static bool OpenContainingLocation(HWND owner, const Link& link, std::wstring& errorMessage);
    static bool ShowNativeContextMenu(HWND owner, const Link& link, POINT screenPoint);
    static bool OpenProperties(HWND owner, const Link& link);
};
