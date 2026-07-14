#pragma once

#include "Models.h"

#include <shlobj.h>
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ShellContextMenuProviderId {
inline constexpr wchar_t Git[] = L"git";
inline constexpr wchar_t Svn[] = L"svn";
inline constexpr wchar_t VsCode[] = L"vscode";
inline constexpr wchar_t Terminal[] = L"terminal";
inline constexpr wchar_t Archive[] = L"archive";
}

enum class ShellContextMenuActionKind : std::uint8_t {
    NativeShell = 0,
    Terminal = 1,
};

struct ShellContextMenuTrackingOptions {
    bool git = false;
    bool svn = false;
    bool vsCode = false;
    bool terminal = false;
    bool archive = false;

    bool Includes(const std::wstring& providerId) const;
    bool Any() const;
};

struct ShellContextMenuItem {
    std::wstring providerId;
    std::wstring text;
    std::wstring verb;
    ShellContextMenuActionKind actionKind = ShellContextMenuActionKind::NativeShell;
    std::wstring actionId;
    std::wstring executable;
    std::wstring arguments;
    std::wstring workingDirectory;
    bool enabled = true;
    bool checked = false;
    bool separator = false;
    int iconWidth = 0;
    int iconHeight = 0;
    int iconQuality = 0;
    std::vector<std::uint32_t> iconPixels;
    std::vector<ShellContextMenuItem> children;
};

struct ShellContextMenuSnapshot {
    bool complete = false;
    std::vector<ShellContextMenuItem> items;
};

struct ShellContextMenuLocator {
    std::wstring providerId;
    std::vector<std::wstring> path;
    std::wstring verb;
    ShellContextMenuActionKind actionKind = ShellContextMenuActionKind::NativeShell;
    std::wstring actionId;
    std::wstring executable;
    std::wstring arguments;
    std::wstring workingDirectory;
};

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
    static std::wstring DetectTrackedContextMenuProvider(
        const std::wstring& text,
        const std::wstring& verb = L"");
    static bool IsShellParseName(const std::wstring& value);
    static bool IsPidlBlobPlausible(const std::vector<std::uint8_t>& blob);
    static std::optional<ShellItemRef> FromAbsolutePidl(PCIDLIST_ABSOLUTE pidl);
    static std::optional<ShellItemRef> FromPathOrParseName(const std::wstring& value);
    static std::optional<ShellItemRef> FromPidlBlob(const std::vector<std::uint8_t>& blob);
    static bool RefreshLinkShellData(Link& link, bool clearOnFailure);
    static bool LoadExecutableMenuIcon(
        const std::wstring& executable,
        ShellContextMenuItem& item);
    static bool OpenShellTarget(HWND owner, const Link& link, int showCmd, std::wstring& errorMessage);
    static bool OpenContainingLocation(HWND owner, const Link& link, std::wstring& errorMessage);
    static bool QueryTrackedContextMenu(
        HWND owner,
        const Link& link,
        const ShellContextMenuTrackingOptions& tracking,
        ShellContextMenuSnapshot& snapshot);
    static bool ShowNativeContextMenu(
        HWND owner,
        const Link& link,
        POINT screenPoint,
        const ShellContextMenuTrackingOptions& tracking,
        ShellContextMenuSnapshot* snapshot = nullptr);
    static bool InvokeTrackedContextMenuItem(
        HWND owner,
        const Link& link,
        const ShellContextMenuLocator& locator,
        std::wstring& errorMessage);
    static bool OpenProperties(HWND owner, const Link& link);
};
