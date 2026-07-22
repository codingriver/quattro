#include "TrackedContextMenuProviders.h"

namespace {
// 表内行序即设置页「自动跟踪」表格与右键菜单内 provider 的显示顺序
// （设置页会把未安装项沉底，但已安装项之间保持此相对顺序）。
constexpr TrackedContextMenuProviderBinding kBindings[TrackedContextMenuProviderCount] = {
    {ShellContextMenuProviderId::VsCode, L"bTrackVsCodeContextMenu",
     &AppConfig::trackVsCodeContextMenu, &ShellContextMenuTrackingOptions::vsCode,
     L"VS Code", 435,
     {L"*\\shell\\VSCode", L"Directory\\shell\\VSCode", L"*\\shell\\VSCodium"}},
    {ShellContextMenuProviderId::NotepadPlusPlus, L"bTrackNotepadPlusPlusContextMenu",
     &AppConfig::trackNotepadPlusPlusContextMenu, &ShellContextMenuTrackingOptions::notepadPlusPlus,
     L"Notepad++", 439,
     {L"Applications\\notepad++.exe", L"*\\shell\\Open with &Notepad++",
      L"*\\shellex\\ContextMenuHandlers\\ANotepad++64"}},
    {ShellContextMenuProviderId::Everything, L"bTrackEverythingContextMenu",
     &AppConfig::trackEverythingContextMenu, &ShellContextMenuTrackingOptions::everything,
     L"Everything", 438,
     {L"Applications\\Everything.exe", L"Everything.FileList"}},
    {ShellContextMenuProviderId::Git, L"bTrackGitContextMenu",
     &AppConfig::trackGitContextMenu, &ShellContextMenuTrackingOptions::git,
     L"Git (TortoiseGit)", 433,
     {L"Directory\\shellex\\ContextMenuHandlers\\TortoiseGit",
      L"Directory\\Background\\shellex\\ContextMenuHandlers\\TortoiseGit",
      L"Directory\\shell\\git_shell", L"Directory\\shell\\git_gui"},
     {L"TortoiseGitProc.exe", L"TortoiseGitMerge.exe"}},
    {ShellContextMenuProviderId::Svn, L"bTrackSvnContextMenu",
     &AppConfig::trackSvnContextMenu, &ShellContextMenuTrackingOptions::svn,
     L"SVN (TortoiseSVN)", 434,
     {L"Directory\\shellex\\ContextMenuHandlers\\TortoiseSVN",
      L"Directory\\Background\\shellex\\ContextMenuHandlers\\TortoiseSVN"},
     {L"TortoiseProc.exe", L"TortoiseMerge.exe"}},
    {ShellContextMenuProviderId::Terminal, L"bTrackTerminalContextMenu",
     &AppConfig::trackTerminalContextMenu, &ShellContextMenuTrackingOptions::terminal,
     L"CMD/PowerShell/WSL", 436,
     {}},
    {ShellContextMenuProviderId::Archive, L"bTrackArchiveContextMenu",
     &AppConfig::trackArchiveContextMenu, &ShellContextMenuTrackingOptions::archive,
     L"压缩工具 (7-Zip/WinRAR 等)", 437,
     {L"*\\shellex\\ContextMenuHandlers\\7-Zip",
      L"Directory\\shellex\\ContextMenuHandlers\\7-Zip",
      L"*\\shellex\\ContextMenuHandlers\\WinRAR",
      L"Directory\\shellex\\ContextMenuHandlers\\WinRAR",
      L"*\\shellex\\ContextMenuHandlers\\BandizipMenu",
      L"*\\shellex\\ContextMenuHandlers\\PeaZip"},
     {L"7zFM.exe", L"WinRAR.exe", L"Bandizip.exe", L"peazip.exe"}},
    {ShellContextMenuProviderId::Cursor, L"bTrackCursorContextMenu",
     &AppConfig::trackCursorContextMenu, &ShellContextMenuTrackingOptions::cursor,
     L"Cursor", 441,
     {L"*\\shell\\Cursor", L"Directory\\shell\\Cursor"}},
    {ShellContextMenuProviderId::SublimeText, L"bTrackSublimeTextContextMenu",
     &AppConfig::trackSublimeTextContextMenu, &ShellContextMenuTrackingOptions::sublimeText,
     L"Sublime Text", 442,
     {L"*\\shell\\Open with Sublime Text", L"Applications\\sublime_text.exe"}},
    {ShellContextMenuProviderId::Windsurf, L"bTrackWindsurfContextMenu",
     &AppConfig::trackWindsurfContextMenu, &ShellContextMenuTrackingOptions::windsurf,
     L"Windsurf", 443,
     {L"*\\shell\\Windsurf", L"Directory\\shell\\Windsurf"}},
    {ShellContextMenuProviderId::Trae, L"bTrackTraeContextMenu",
     &AppConfig::trackTraeContextMenu, &ShellContextMenuTrackingOptions::trae,
     L"Trae", 444,
     {L"*\\shell\\Trae", L"Directory\\shell\\Trae"}},
    {ShellContextMenuProviderId::Zed, L"bTrackZedContextMenu",
     &AppConfig::trackZedContextMenu, &ShellContextMenuTrackingOptions::zed,
     L"Zed", 445,
     {L"*\\shell\\Zed", L"Directory\\shell\\Zed", L"Applications\\zed.exe"}},
    {ShellContextMenuProviderId::Vim, L"bTrackVimContextMenu",
     &AppConfig::trackVimContextMenu, &ShellContextMenuTrackingOptions::vim,
     L"Vim", 446,
     {L"*\\shellex\\ContextMenuHandlers\\gvim", L"Applications\\gvim.exe",
      L"*\\shell\\Vim"},
     {L"gvim.exe"}},
};
}

std::span<const TrackedContextMenuProviderBinding> TrackedContextMenuProviders() {
    return kBindings;
}

std::vector<std::size_t> TrackedContextMenuDisplayOrder(const std::vector<bool>& installed) {
    std::vector<std::size_t> order;
    order.reserve(TrackedContextMenuProviderCount);
    for (std::size_t index = 0; index < TrackedContextMenuProviderCount; ++index) {
        if (index >= installed.size() || installed[index]) {
            order.push_back(index);
        }
    }
    for (std::size_t index = 0; index < TrackedContextMenuProviderCount && index < installed.size(); ++index) {
        if (!installed[index]) {
            order.push_back(index);
        }
    }
    return order;
}

bool ShellContextMenuTrackingOptions::Includes(const std::wstring& providerId) const {
    for (const auto& binding : kBindings) {
        if (providerId == binding.providerId) {
            return this->*(binding.trackingMember);
        }
    }
    return false;
}

bool ShellContextMenuTrackingOptions::Any() const {
    for (const auto& binding : kBindings) {
        if (this->*(binding.trackingMember)) {
            return true;
        }
    }
    return false;
}
