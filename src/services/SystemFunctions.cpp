#include "SystemFunctions.h"

#include "MenuCatalog.h"
#include "ShellItemService.h"
#include "Utilities.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <array>
#include <string>

namespace {
const std::array<BuiltinSystemContextMenuItem, 3> kThisPcContextItems{{
    {BuiltinSystemContextAction::ManageComputer, L"管理", MenuIconTools},
    {BuiltinSystemContextAction::MapNetworkDrive, L"映射网络驱动器", MenuIconComputer},
    {BuiltinSystemContextAction::DisconnectNetworkDrive, L"断开网络驱动器", MenuIconComputer},
}};

const std::array<BuiltinSystemContextMenuItem, 1> kRecycleBinContextItems{{
    {BuiltinSystemContextAction::EmptyRecycleBin, L"清空回收站", MenuIconClear},
}};

const std::array<BuiltinSystemContextMenuItem, 2> kNetworkContextItems{{
    {BuiltinSystemContextAction::MapNetworkDrive, L"映射网络驱动器", MenuIconComputer},
    {BuiltinSystemContextAction::DisconnectNetworkDrive, L"断开网络驱动器", MenuIconComputer},
}};

const std::array<SystemFunctionDefinition, 33> kSystemFunctions{{
    {L"this-pc", L"我的电脑", L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", L"", 3, SW_SHOWNORMAL, L"Windows Shell 系统位置", -1, MenuIconNone, BuiltinSystemContextProfile::ThisPc},
    {L"personal", L"我的文档", L"shell:Personal", L"", 3, SW_SHOWNORMAL, L"当前用户文档目录"},
    {L"network", L"网络", L"::{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}", L"", 3, SW_SHOWNORMAL, L"网络位置", -1, MenuIconNone, BuiltinSystemContextProfile::Network},
    {L"recycle-bin", L"回收站", L"::{645FF040-5081-101B-9F08-00AA002F954E}", L"", 3, SW_SHOWNORMAL, L"回收站", -1, MenuIconNone, BuiltinSystemContextProfile::RecycleBin},
    {L"control-panel", L"控制面板", L"shell:ControlPanelFolder", L"", 3, SW_SHOWNORMAL, L"控制面板"},
    {L"registry-editor", L"注册表", L"%windir%\\regedit.exe", L"", 0, SW_SHOWNORMAL, L"注册表编辑器", -1, MenuIconWindows},
    {L"calculator", L"计算器", L"calc.exe", L"", 0, SW_SHOWNORMAL, L"Windows 计算器", -1, MenuIconCalculator},
    {L"command-prompt", L"命令行", L"%ComSpec%", L"", 0, SW_SHOWNORMAL, L"命令提示符", -1, MenuIconTerminal},
    {L"notepad", L"记事本", L"notepad.exe", L"", 0, SW_SHOWNORMAL, L"记事本", -1, MenuIconNotebook},
    {L"paint", L"画图", L"mspaint.exe", L"", 0, SW_SHOWNORMAL, L"画图", -1, MenuIconTheme},
    {L"group-policy", L"组策略", L"gpedit.msc", L"", 0, SW_SHOWNORMAL, L"本地组策略编辑器", -1, MenuIconTools},
    {L"task-manager", L"任务管理器", L"taskmgr.exe", L"", 0, SW_SHOWNORMAL, L"任务管理器", -1, MenuIconSystem},
    {L"all-control-panel-tasks", L"完全控制面板", L"shell:::{ED7BA470-8E54-465E-825C-99712043E01C}", L"", 3, SW_SHOWNORMAL, L"控制面板全部任务", -1, MenuIconSettings},
    {L"system-drive", L"系统盘根目录", L"%SystemDrive%\\", L"", 1, SW_SHOWNORMAL, L"系统盘根目录"},
    {L"user-profile", L"用户目录", L"%USERPROFILE%", L"", 1, SW_SHOWNORMAL, L"当前用户目录"},
    {L"appdata", L"AppData", L"%APPDATA%", L"", 1, SW_SHOWNORMAL, L"Roaming AppData"},
    {L"common-startup", L"系统自启目录", L"shell:Common Startup", L"", 3, SW_SHOWNORMAL, L"所有用户启动目录"},
    {L"user-startup", L"用户自启目录", L"shell:Startup", L"", 3, SW_SHOWNORMAL, L"当前用户启动目录"},
    {L"recent-items", L"最近使用项目", L"shell:Recent", L"", 3, SW_SHOWNORMAL, L"最近使用项目"},
    {L"administrative-tools", L"Win管理工具", L"shell:Administrative Tools", L"", 3, SW_SHOWNORMAL, L"Windows 管理工具"},
    {L"services", L"服务", L"services.msc", L"", 0, SW_SHOWNORMAL, L"服务管理器"},
    {L"computer-management", L"计算机管理", L"compmgmt.msc", L"", 0, SW_SHOWNORMAL, L"计算机管理"},
    {L"remote-desktop", L"远程桌面连接", L"mstsc.exe", L"", 0, SW_SHOWNORMAL, L"远程桌面连接"},
    {L"user-certificates", L"用户证书", L"certmgr.msc", L"", 0, SW_SHOWNORMAL, L"当前用户证书管理"},
    {L"monitor-off", L"关闭显示器", L"powershell.exe", L"-NoProfile -WindowStyle Hidden -Command \"Add-Type -MemberDefinition '[DllImport(\\\"user32.dll\\\")]public static extern int SendMessage(int hWnd,int hMsg,int wParam,int lParam);' -Name Native -Namespace Win32; [Win32.Native]::SendMessage(0xffff,0x0112,0xf170,2)\"", 0, SW_HIDE, L"关闭显示器", -1, MenuIconMonitor},
    {L"shutdown", L"关机", L"shutdown.exe", L"/s /t 0", 0, SW_HIDE, L"关闭计算机", -1, MenuIconPower},
    {L"restart", L"重启", L"shutdown.exe", L"/r /t 0", 0, SW_HIDE, L"重启计算机", -1, MenuIconRestart},
    {L"logoff", L"注销", L"shutdown.exe", L"/l", 0, SW_HIDE, L"注销当前用户", -1, MenuIconLogout},
    {L"lock", L"锁定", L"rundll32.exe", L"user32.dll,LockWorkStation", 0, SW_HIDE, L"锁定当前会话", -1, MenuIconLock},
    {L"hibernate", L"休眠", L"shutdown.exe", L"/h", 0, SW_HIDE, L"休眠计算机", -1, MenuIconSleep},
    {L"sleep", L"睡眠", L"rundll32.exe", L"powrprof.dll,SetSuspendState 0,1,0", 0, SW_HIDE, L"睡眠计算机", -1, MenuIconSleep},
    {L"hosts", L"hosts", L"notepad.exe", L"%windir%\\System32\\drivers\\etc\\hosts", 0, SW_SHOWNORMAL, L"编辑 hosts 文件", -1, MenuIconNotebook},
    {L"environment-variables", L"环境变量", L"rundll32.exe", L"sysdm.cpl,EditEnvironmentVariables", 0, SW_SHOWNORMAL, L"编辑环境变量", -1, MenuIconEnvironment},
}};

std::wstring ResolveSystemIconTarget(const std::wstring& value) {
    const std::wstring expanded = ExpandEnvironmentStringsSafe(value);
    if (expanded.empty()) {
        return {};
    }
    if (PathIsRelativeW(expanded.c_str()) && expanded.find(L'\\') == std::wstring::npos && expanded.find(L'/') == std::wstring::npos) {
        wchar_t resolved[MAX_PATH]{};
        if (SearchPathW(nullptr, expanded.c_str(), nullptr, static_cast<DWORD>(_countof(resolved)), resolved, nullptr) > 0) {
            return resolved;
        }
    }
    return expanded;
}
}

std::span<const SystemFunctionDefinition> SystemFunctions() {
    return kSystemFunctions;
}

int SystemFunctionImageIndex(const SystemFunctionDefinition& item) {
    const std::wstring target = ResolveSystemIconTarget(item.target);
    SHFILEINFOW iconInfo{};
    if (ShellItemService::IsShellParseName(target)) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(target.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) {
            const DWORD_PTR ok = SHGetFileInfoW(
                reinterpret_cast<LPCWSTR>(pidl),
                0,
                &iconInfo,
                sizeof(iconInfo),
                SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_LARGEICON);
            CoTaskMemFree(pidl);
            if (ok) {
                return iconInfo.iIcon;
            }
        }
    }

    if (SHGetFileInfoW(target.c_str(), 0, &iconInfo, sizeof(iconInfo), SHGFI_SYSICONINDEX | SHGFI_LARGEICON)) {
        return iconInfo.iIcon;
    }
    if (SHGetFileInfoW(
            target.c_str(),
            item.type == 1 ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
            &iconInfo,
            sizeof(iconInfo),
            SHGFI_SYSICONINDEX | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES)) {
        return iconInfo.iIcon;
    }
    return 0;
}

const SystemFunctionDefinition* SystemFunctionForLink(const Link& link) {
    if (const auto* builtin = BuiltinSystemFunctionForLink(link)) {
        return builtin;
    }
    const std::wstring path = ToLower(Trim(link.path));
    const std::wstring parameter = Trim(link.parameter);
    for (const auto& item : kSystemFunctions) {
        if (path == ToLower(Trim(item.target)) && parameter == Trim(item.parameter)) {
            return &item;
        }
    }
    return nullptr;
}

const SystemFunctionDefinition* BuiltinSystemFunctionForLink(const Link& link) {
    const std::wstring key = ToLower(Trim(link.systemFunctionKey));
    if (key.empty()) {
        return nullptr;
    }
    for (const auto& item : kSystemFunctions) {
        if (key == ToLower(item.key) &&
            link.type == item.type &&
            ToLower(Trim(link.path)) == ToLower(Trim(item.target)) &&
            Trim(link.parameter) == Trim(item.parameter)) {
            return &item;
        }
    }
    return nullptr;
}

bool RestoreLegacyBuiltinSystemFunctionKey(Link& link) {
    if (!Trim(link.systemFunctionKey).empty()) {
        return false;
    }
    const std::wstring path = ToLower(Trim(link.path));
    const std::wstring parameter = Trim(link.parameter);
    for (const auto& item : kSystemFunctions) {
        if (link.name == item.name &&
            link.type == item.type &&
            link.showCmd == item.showCmd &&
            path == ToLower(Trim(item.target)) &&
            parameter == Trim(item.parameter)) {
            link.systemFunctionKey = item.key;
            return true;
        }
    }
    return false;
}

std::span<const BuiltinSystemContextMenuItem> BuiltinSystemContextMenuItems(const Link& link) {
    const auto* item = BuiltinSystemFunctionForLink(link);
    if (!item) {
        return {};
    }
    switch (item->contextProfile) {
    case BuiltinSystemContextProfile::ThisPc:
        return kThisPcContextItems;
    case BuiltinSystemContextProfile::RecycleBin:
        return kRecycleBinContextItems;
    case BuiltinSystemContextProfile::Network:
        return kNetworkContextItems;
    default:
        return {};
    }
}

MenuIcon SystemFunctionMenuIconForLink(const Link& link) {
    MenuIcon storedIcon = MenuIconNone;
    if (TryParseMenuIconLinkIcon(Trim(link.icon), storedIcon)) {
        return storedIcon;
    }
    if (!Trim(link.icon).empty()) {
        return MenuIconNone;
    }

    const auto* item = SystemFunctionForLink(link);
    if (item && item->menuIcon != MenuIconNone) {
        return static_cast<MenuIcon>(item->menuIcon);
    }
    return MenuIconNone;
}

bool ConfigureSystemFunctionLink(std::size_t index, Link& link) {
    if (index >= kSystemFunctions.size()) {
        return false;
    }

    const auto& item = kSystemFunctions[index];
    Link next = link;
    next.name = item.name;
    next.systemFunctionKey = item.key;
    next.path = item.target;
    next.type = item.type;
    next.parameter = item.parameter;
    next.workDir.clear();
    next.icon = item.menuIcon != MenuIconNone
        ? MenuIconLinkIconValue(static_cast<MenuIcon>(item.menuIcon))
        : L"";
    next.remark = item.remark;
    next.showCmd = item.showCmd;
    next.isAdmin = false;
    ShellItemService::RefreshLinkShellData(next, true);
    link = std::move(next);
    return true;
}
