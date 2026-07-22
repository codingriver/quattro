#pragma once

#include "Models.h"
#include "TrackedContextMenuProviders.h"

#include <shlobj.h>
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum class ShellContextMenuActionKind : std::uint8_t {
    NativeShell = 0,
    Terminal = 1,
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

enum class TrackedProviderIconSource : std::uint8_t {
    None = 0,
    ExplicitRegistry,
    CommandExecutable,
    BrandExecutable,
    ApplicationExecutable,
};

class ShellItemService {
public:
    static std::wstring DetectTrackedContextMenuProvider(
        const std::wstring& text,
        const std::wstring& verb = L"");
    // 优先按绑定表精确探测键判断 provider 是否已注册；未命中时在固定的
    // Explorer shell 范围内进行有上限的菜单注册扫描，以兼容本地化或自定义键名。
    // 探测键为空的表项（如系统终端）恒为已安装。
    static bool IsTrackedProviderInstalled(const TrackedContextMenuProviderBinding& binding);
    static bool IsShellParseName(const std::wstring& value);
    static bool IsPidlBlobPlausible(const std::vector<std::uint8_t>& blob);
    static std::optional<ShellItemRef> FromAbsolutePidl(PCIDLIST_ABSOLUTE pidl);
    static std::optional<ShellItemRef> FromPathOrParseName(const std::wstring& value);
    static std::optional<ShellItemRef> FromPidlBlob(const std::vector<std::uint8_t>& blob);
    static bool RefreshLinkShellData(Link& link, bool clearOnFailure);
    static bool LoadExecutableMenuIcon(
        const std::wstring& executable,
        ShellContextMenuItem& item);
    // Resolves one representative icon for a tracked provider from its bounded
    // registry probes and executable commands. It never constructs a native
    // IContextMenu, so it is suitable for finite background presentation work.
    static bool LoadTrackedProviderIcon(
        const TrackedContextMenuProviderBinding& binding,
        ShellContextMenuItem& item,
        TrackedProviderIconSource* source = nullptr);
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

    // 新增: Native Shell高级检测
    // 从Windows注册表HKCR扫描，检测provider是否在native shell菜单中存在
    static bool IsInstalledInNativeShell(const std::wstring& providerId);

};
