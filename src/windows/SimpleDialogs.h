#pragma once

#include "Models.h"
#include "ContextMenuProviderIconService.h"
#include "ShellContextMenuRefreshService.h"
#include "Theme.h"
#include "WebDavFileService.h"

#include <windows.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class LocalHttpServerService;
struct WebDavRemoteFile;
constexpr UINT WM_QUATTRO_TEST_SETTINGS_SYNC_RESULT = WM_APP + 0x86;
constexpr UINT WM_QUATTRO_TEST_SETTINGS_PROVIDER = WM_APP + 0x8A;
enum class SettingsProviderTestCommand { Rebuild, Refresh, Pending };
constexpr UINT WM_QUATTRO_TEST_WEBDAV_DELETE = WM_APP + 0x89;
enum class WebDavDeleteTestCommand { SetRows, Delete, Stop, Pending, ProgressWindow, RefreshResult, ObsoleteCompletion };
struct WebDavDeleteTestRequest {
    std::vector<WebDavFileRecord> records;
    WebDavFileDeleteOperations operations;
    bool confirm = true;
};
struct SettingsApplyResult {
    bool saved = false;
    AppConfig config;
    bool mainHotKeyRegistered = false;
    bool processLocatorHotKeyRegistered = false;
    bool copySelectedPathsHotKeyRegistered = false;
    std::wstring error;
    std::wstring warning;
};
using SettingsApplyCallback = std::function<SettingsApplyResult(const AppConfig&, bool)>;
using SettingsResetContextMenuCallback = std::function<bool()>;
using SettingsContextMenuRefreshRunner = std::function<ShellContextMenuRefreshResult(
    const ShellContextMenuRefreshRequest&,
    std::stop_token)>;
using SettingsContextMenuRefreshApplyCallback = std::function<void(
    const ShellContextMenuRefreshResult&)>;
using SettingsContextMenuProviderIconRunner = std::function<std::vector<ContextMenuProviderIconInfo>(
    std::stop_token)>;
using SettingsCopyPathContextMenuCallback = std::function<bool(bool, std::wstring&)>;
using SettingsWebDavUploadContextMenuCallback = std::function<bool(bool, std::wstring&)>;
using SettingsWebDavPasswordSaveCallback = std::function<bool(
    const AppConfig&, const std::wstring&, std::wstring&)>;

bool ShowTextInputDialog(HWND owner, HINSTANCE instance, const Theme& theme, const std::wstring& title, const std::wstring& label, std::wstring& value);
int ShowThemedMessageBox(HWND owner, HINSTANCE instance, const Theme& theme, const std::wstring& message, const std::wstring& title, UINT flags);
bool ShowHotKeyConflictDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const std::wstring& message,
    bool& ignoreFutureWarnings);
bool ShowWebDavBackupSelectionDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const std::vector<WebDavRemoteFile>& backups,
    std::wstring& selectedName);
bool ShowWebDavFileManagerDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const AppConfig& config,
    std::function<void(HWND)> backgroundTestAction = {});
bool ShowSettingsDialog(
    HWND owner,
    HINSTANCE instance,
    AppConfig& config,
    const Theme& theme,
    const std::filesystem::path& appDirectory,
    const std::filesystem::path& httpRootBaseDirectory,
    bool* importedData = nullptr,
    LocalHttpServerService* httpServer = nullptr,
    bool mainHotKeyRegistered = false,
    bool processLocatorHotKeyRegistered = false,
    bool copySelectedPathsHotKeyRegistered = false,
    SettingsApplyCallback applyCallback = {},
    SettingsResetContextMenuCallback resetContextMenuCallback = {},
    const std::vector<Link>& contextMenuLinks = {},
    SettingsContextMenuRefreshRunner contextMenuRefreshRunner = {},
    SettingsContextMenuRefreshApplyCallback contextMenuRefreshApplyCallback = {},
    SettingsContextMenuProviderIconRunner contextMenuProviderIconRunner = {},
    SettingsCopyPathContextMenuCallback copyPathContextMenuCallback = {},
    SettingsWebDavUploadContextMenuCallback webDavUploadContextMenuCallback = {},
    SettingsWebDavPasswordSaveCallback webDavPasswordSaveCallback = {});
void NotifyOpenSettingsDialogAutoRunChanged(HWND owner, bool enabled);
