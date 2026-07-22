#pragma once

#include "Models.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace ShellContextMenuProviderId {
inline constexpr wchar_t Git[] = L"git";
inline constexpr wchar_t Svn[] = L"svn";
inline constexpr wchar_t VsCode[] = L"vscode";
inline constexpr wchar_t Terminal[] = L"terminal";
inline constexpr wchar_t Archive[] = L"archive";
inline constexpr wchar_t Everything[] = L"everything";
inline constexpr wchar_t NotepadPlusPlus[] = L"notepadplusplus";
inline constexpr wchar_t Cursor[] = L"cursor";
inline constexpr wchar_t SublimeText[] = L"sublimetext";
inline constexpr wchar_t Windsurf[] = L"windsurf";
inline constexpr wchar_t Trae[] = L"trae";
inline constexpr wchar_t Zed[] = L"zed";
inline constexpr wchar_t Vim[] = L"vim";
}

struct ShellContextMenuTrackingOptions {
    bool git = false;
    bool svn = false;
    bool vsCode = false;
    bool terminal = false;
    bool archive = false;
    bool everything = false;
    bool notepadPlusPlus = false;
    bool cursor = false;
    bool sublimeText = false;
    bool windsurf = false;
    bool trae = false;
    bool zed = false;
    bool vim = false;

    bool Includes(const std::wstring& providerId) const;
    bool Any() const;
};

// 每个可跟踪 provider 的全部接线：providerId、配置键、AppConfig 成员、
// 跟踪开关成员、设置页显示文本与控件 ID、安装探测注册表键。
// Includes/Any、配置读写、设置界面生成、右键菜单 provider 顺序、
// 取消勾选后的缓存清理均遍历此表；表内行序即右键菜单内 provider 的
// 显示顺序。新增 provider 只需追加表项和 DetectProviderId 检测分支。
struct TrackedContextMenuProviderBinding {
    const wchar_t* providerId = L"";
    const wchar_t* configKey = L"";
    bool AppConfig::* configMember = nullptr;
    bool ShellContextMenuTrackingOptions::* trackingMember = nullptr;
    // 设置页表格「工具」列显示名（如 L"Git (TortoiseGit)"）。
    const wchar_t* displayName = L"";
    int checkBoxControlId = 0;
    // 安装探测：HKCR 下的 shell 注册键，任一存在即视为已安装；
    // 空表（终结符打头）表示恒为已安装（如系统终端）。
    static constexpr std::size_t MaxShellProbeKeys = 8;
    const wchar_t* shellProbeKeys[MaxShellProbeKeys] = {};
    // 仅用于没有 Icon/DefaultIcon/command 的 DLL 型 ContextMenuHandler。
    // 文件名会优先相对 InprocServer32 所在目录解析，再查询 App Paths；
    // 禁止直接把 Shell Extension DLL 的通用文件图标当作品牌图标。
    static constexpr std::size_t MaxBrandIconExecutables = 4;
    const wchar_t* brandIconExecutables[MaxBrandIconExecutables] = {};
};

inline constexpr std::size_t TrackedContextMenuProviderCount = 13;

std::span<const TrackedContextMenuProviderBinding> TrackedContextMenuProviders();

// 设置页「自动跟踪」表格的行序：已安装在前、未安装沉底，
// 两段内部均保持绑定表相对顺序。installed 与绑定表按下标对应。
std::vector<std::size_t> TrackedContextMenuDisplayOrder(const std::vector<bool>& installed);
