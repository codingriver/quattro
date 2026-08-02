#include "SimpleDialogs.h"

#include "../../resources/resource.h"

#include "AppLog.h"
#include "ConfigPackageService.h"
#include "ContextMenuProviderIconService.h"
#include "DialogLayout.h"
#include "FileDialog.h"
#include "HotKeyEditor.h"
#include "IconResolverService.h"
#include "LocalHttpServerService.h"
#include "MainHotKey.h"
#include "ShellContextMenuCacheService.h"
#include "ShellContextMenuRefreshService.h"
#include "ShellItemService.h"
#include "Storage.h"
#include "ThemedControls.h"
#include "ThemedFormLayout.h"
#include "ThemedTaskProgressDialog.h"
#include "ThemedUi.h"
#include "ThemedWindowUi.h"
#include "TodoJsonBackupService.h"
#include "TodoSchedule.h"
#include "TrackedContextMenuProviders.h"
#include "Utilities.h"
#include "WebDavBackupService.h"
#include "WebDavCredentialService.h"
#include "WebDavFileBatchConfirmDialog.h"
#include "WebDavFileDetailsDialog.h"
#include "WebDavFileService.h"
#include "WebDavFileIndexCache.h"
#include "WebDavTransferCoordinator.h"

#include <commdlg.h>
#include <commctrl.h>
#include <iphlpapi.h>
#include <richedit.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace {
constexpr int ID_SETTINGS_TAB_BASE = 280;
constexpr int ID_SETTINGS_TAB_CONTROL = 279;
constexpr int ID_GLOBAL_HOTKEYS_ENABLED = 300;
constexpr int ID_MAIN_HOTKEY_CAPTURE = 301;
constexpr int ID_MAIN_HOTKEY_CLEAR = 302;
constexpr int ID_PROCESS_LOCATOR_HOTKEY_CAPTURE = 303;
constexpr int ID_HOTKEY_TABLE = 304;
constexpr int ID_COPY_SELECTED_PATHS_HOTKEY_CAPTURE = 305;
constexpr int ID_RESET_DEFAULT_HOTKEYS = 306;
constexpr int ID_GROUP_WIDTH = 401;
constexpr int ID_TAG_WIDTH = 402;
constexpr int ID_DOCK_DELAY = 403;
constexpr int ID_GROUP_DELAY = 404;
constexpr int ID_TAG_DELAY = 405;
constexpr int ID_TAG_ALIGN_LEFT = 407;
constexpr int ID_TAG_ALIGN_CENTER = 408;
constexpr int ID_TAG_ALIGN_RIGHT = 409;
constexpr int ID_WEBDAV_TEST = 410;
constexpr int ID_WEBDAV_CLEAR_PASSWORD = 411;
constexpr int ID_WEBDAV_UPLOAD = 412;
constexpr int ID_WEBDAV_DOWNLOAD = 413;
constexpr int ID_WEBDAV_BACKUP_LIST = 414;
constexpr int ID_WEBDAV_UPLOAD_CONTEXT_MENU = 419;
constexpr int ID_WEBDAV_FILE_MANAGER = 420;
constexpr int ID_WEBDAV_FILE_REFRESH = 421;
constexpr int ID_WEBDAV_FILE_DOWNLOAD = 422;
constexpr int ID_WEBDAV_FILE_DELETE = 423;
constexpr int ID_WEBDAV_FILE_TRANSFER_QUEUE = 424;
constexpr int ID_WEBDAV_FILE_SELECT_ALL = 460;
constexpr int ID_WEBDAV_FILE_CLEAR_SELECTION = 461;
constexpr int ID_WEBDAV_FILE_DOWNLOAD_SELECTED = 462;
constexpr int ID_WEBDAV_FILE_DELETE_SELECTED = 463;
constexpr int ID_WEBDAV_FILE_UPLOAD_SELECTED = 464;
constexpr UINT WM_WEBDAV_FILE_BATCH = WM_APP + 0xC7;
constexpr UINT WM_WEBDAV_FILE_LIST_DONE = WM_APP + 0xC8;
constexpr UINT WM_WEBDAV_FILE_REFRESH_REQUEST = WM_APP + 0xC9;
constexpr UINT WM_WEBDAV_FILE_DELETE_DONE = WM_APP + 0xCA;
constexpr UINT WM_WEBDAV_FILE_SHOW_TEST_DETAILS = WM_APP + 0xCB;
constexpr UINT WM_WEBDAV_FILE_APPLY_TEST_INCREMENTAL = WM_APP + 0xCC;
constexpr UINT WM_WEBDAV_FILE_DELETE_ITEM_DONE = WM_APP + 0xCD;
constexpr UINT WM_WEBDAV_FILE_SHOW_TEST_DELETE_PROGRESS = WM_APP + 0xCE;
constexpr int ID_CONFIG_EXPORT = 415;
constexpr int ID_CONFIG_IMPORT = 416;
constexpr int ID_TODO_EXPORT = 417;
constexpr int ID_TODO_IMPORT_MERGE = 418;
constexpr int ID_TODO_IMPORT_REPLACE = 452;
constexpr int ID_TODO_INCLUDE_COMPLETED = 453;
constexpr int ID_TODO_INCLUDE_DISABLED = 454;
constexpr int ID_TODO_ONLY_FUTURE = 455;
constexpr int ID_HTTP_START = 422;
constexpr int ID_HTTP_STOP = 423;
constexpr int ID_HTTP_RESTART = 424;
constexpr int ID_HTTP_OPEN_HOME = 425;
constexpr int ID_HTTP_OPEN_CONFIG_DIR = 426;
constexpr int ID_HTTP_BROWSE_ROOT = 427;
constexpr int ID_HTTP_COPY_URL = 428;
constexpr int ID_HTTP_OPEN_ROOT = 429;
constexpr int ID_SETTINGS_APPLY = 430;
constexpr int ID_HTTP_ADDRESS = 431;
constexpr int ID_LOGGING_ENABLED = 432;
// 433-439 与 441-446 由 TrackedContextMenuProviders() 表内的行键占用。
constexpr int ID_RESET_CONTEXT_MENU = 440;
constexpr int ID_REFRESH_CONTEXT_MENU_FROM_NATIVE = 449;
constexpr int ID_CONTEXT_MENU_TABLE = 447;
constexpr int ID_REGISTER_COPY_PATH_CONTEXT_MENU = 448;
constexpr int ID_HIDE_MAIN_AFTER_TOOL_OPEN = 451;
constexpr int ID_MESSAGE_TEXT = 501;
constexpr int ID_HOTKEY_CONFLICT_IGNORE = 502;
constexpr int ID_MAIN_HOTKEY_PROBE = 0x5148;
constexpr UINT WM_SETTINGS_WEBDAV_DONE = WM_APP + 0x81;
constexpr UINT WM_CONTEXT_MENU_REFRESH_DONE = WM_APP + 0x82;
constexpr UINT WM_CONTEXT_MENU_ICON_LOAD_REQUEST = WM_APP + 0x83;
constexpr UINT WM_CONTEXT_MENU_ICON_LOAD_DONE = WM_APP + 0x84;
constexpr UINT WM_SETTINGS_AUTORUN_CHANGED = WM_APP + 0x85;
constexpr const wchar_t* kSettingsDialogHwndProp = L"QuattroSettingsDialogHwnd";

enum class SettingsWebDavOperation {
    Test,
    Upload,
    List,
    DownloadPreview,
    DownloadApply,
};

struct SettingsWebDavResult {
    SettingsWebDavOperation operation = SettingsWebDavOperation::Test;
    bool ok = false;
    std::wstring message;
    WebDavBackupReport report;
    std::vector<WebDavRemoteFile> backups;
    AppConfig config;
};

struct SettingsContextMenuIconAsyncState {
    std::mutex mutex;
    std::stop_source stopSource;
    std::optional<std::vector<ContextMenuProviderIconInfo>> result;
    std::uintptr_t generation = 0;
    std::atomic_bool abandoned{false};
};

std::atomic_uintptr_t gContextMenuIconGeneration{1};

bool IsValidCachedIcon(const ShellContextMenuCachedIcon& icon) {
    return icon.width > 0 && icon.height > 0 && icon.width <= 64 && icon.height <= 64 &&
        icon.pixels.size() == static_cast<std::size_t>(icon.width * icon.height);
}

HBITMAP CreateScaledBitmapFromCachedPixels(
    const ShellContextMenuCachedIcon& icon,
    int targetSize) {
    if (!IsValidCachedIcon(icon) || targetSize <= 0) {
        return nullptr;
    }

    auto createBitmap = [](int width, int height, void** pixels) {
        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = width;
        bitmapInfo.bmiHeader.biHeight = -height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;
        return CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, pixels, nullptr, 0);
    };

    void* targetPixels = nullptr;
    HBITMAP target = createBitmap(targetSize, targetSize, &targetPixels);
    if (!target || !targetPixels) {
        if (target) DeleteObject(target);
        return nullptr;
    }

    auto* output = static_cast<std::uint32_t*>(targetPixels);
    for (int y = 0; y < targetSize; ++y) {
        const int sourceY = std::clamp(y * icon.height / targetSize, 0, icon.height - 1);
        for (int x = 0; x < targetSize; ++x) {
            const int sourceX = std::clamp(x * icon.width / targetSize, 0, icon.width - 1);
            std::uint32_t pixel = icon.pixels[
                static_cast<std::size_t>(sourceY) * icon.width + sourceX];
            // Cached shell icons are stored as premultiplied BGRA, while the
            // GDI image-list input expects straight-alpha RGB. Convert only
            // the color channels and keep alpha intact; no background is
            // baked into the provider bitmap.
            const std::uint32_t alpha = pixel >> 24;
            if (alpha < 255) {
                const auto unpremultiply = [alpha](std::uint32_t channel) {
                    return alpha == 0
                        ? 0u
                        : std::min<std::uint32_t>(255u, (channel * 255u + alpha / 2u) / alpha);
                };
                pixel = (alpha << 24) |
                    (unpremultiply((pixel >> 16) & 0xFFu) << 16) |
                    (unpremultiply((pixel >> 8) & 0xFFu) << 8) |
                    unpremultiply(pixel & 0xFFu);
            }
            output[static_cast<std::size_t>(y) * targetSize + x] = pixel;
        }
    }
    return target;
}

std::wstring CurrentLanIpv4Address() {
    ULONG size = 0;
    if (GetAdaptersInfo(nullptr, &size) != ERROR_BUFFER_OVERFLOW || size == 0) {
        return L"127.0.0.1";
    }
    std::vector<BYTE> buffer(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_INFO*>(buffer.data());
    if (GetAdaptersInfo(adapters, &size) != NO_ERROR) {
        return L"127.0.0.1";
    }
    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->Type == MIB_IF_TYPE_LOOPBACK) {
            continue;
        }
        for (IP_ADDR_STRING* address = &adapter->IpAddressList; address; address = address->Next) {
            const std::string value = address->IpAddress.String ? address->IpAddress.String : "";
            if (value.empty() || value == "0.0.0.0" || value.rfind("127.", 0) == 0) {
                continue;
            }
            std::wstring result;
            result.reserve(value.size());
            for (char ch : value) {
                result.push_back(static_cast<unsigned char>(ch));
            }
            return result;
        }
    }
    return L"127.0.0.1";
}

std::wstring HttpHostForLan(bool lanAccess) {
    return lanAccess ? CurrentLanIpv4Address() : L"127.0.0.1";
}

int ParseHttpPortText(const std::wstring& text, int fallback) {
    std::wstring value = Trim(text);
    if (value.empty()) {
        return fallback;
    }
    const std::size_t scheme = value.find(L"://");
    if (scheme != std::wstring::npos) {
        value = value.substr(scheme + 3);
    }
    const std::size_t slash = value.find_first_of(L"/\\?#");
    if (slash != std::wstring::npos) {
        value = value.substr(0, slash);
    }
    const std::size_t at = value.rfind(L'@');
    if (at != std::wstring::npos) {
        value = value.substr(at + 1);
    }
    const std::size_t colon = value.rfind(L':');
    if (colon != std::wstring::npos) {
        value = value.substr(colon + 1);
    }
    value = Trim(value);
    if (!value.empty() && value.back() == L'/') {
        value.pop_back();
    }
    auto port = ParseInt(value);
    if (!port) {
        return fallback;
    }
    return std::max(1, std::min(65535, *port));
}

std::wstring HttpAddressText(bool lanAccess, int port, bool trailingSlash) {
    std::wstring value = L"http://" + HttpHostForLan(lanAccess) + L":" + std::to_wstring(std::max(1, std::min(65535, port)));
    if (trailingSlash) {
        value += L"/";
    }
    return value;
}

struct HotKeyAvailability {
    bool available = false;
    DWORD error = ERROR_SUCCESS;
    std::wstring reason;
};

std::wstring ReservedMainHotKeyReason(int key) {
    switch (key) {
    case VK_DELETE:
        return L"Ctrl+Alt+Delete 是 Windows 安全按键，不能作为普通全局热键。";
    case VK_TAB:
        return L"Ctrl+Alt+Tab 是 Windows 窗口切换相关按键，建议换一个。";
    default:
        return {};
    }
}

HotKeyAvailability CheckMainHotKeyAvailability(HWND hwnd, int key, int currentRegisteredKey) {
    if (IsDoubleAltMainHotKey(key)) {
        return HotKeyAvailability{true, ERROR_SUCCESS, {}};
    }
    if (key <= 0 || key == currentRegisteredKey) {
        return HotKeyAvailability{true, ERROR_SUCCESS, {}};
    }

    std::wstring reservedReason = ReservedMainHotKeyReason(key);
    if (!reservedReason.empty()) {
        return HotKeyAvailability{false, ERROR_SUCCESS, std::move(reservedReason)};
    }

    SetLastError(ERROR_SUCCESS);
    if (RegisterHotKey(hwnd, ID_MAIN_HOTKEY_PROBE, MOD_CONTROL | MOD_ALT, static_cast<UINT>(key))) {
        UnregisterHotKey(hwnd, ID_MAIN_HOTKEY_PROBE);
        return HotKeyAvailability{true, ERROR_SUCCESS, {}};
    }

    const DWORD error = GetLastError();
    std::wstring reason = L"该热键无法注册，可能已被系统、输入法或其它软件占用。";
    if (error != ERROR_SUCCESS) {
        reason += L"\n\n系统返回: " + FormatLastError(error);
    }
    return HotKeyAvailability{false, error, std::move(reason)};
}

HotKeyAvailability CheckCtrlAltHotKeyAvailability(HWND hwnd, int key, int currentRegisteredKey) {
    if (key <= 0 || key == currentRegisteredKey) {
        return HotKeyAvailability{true, ERROR_SUCCESS, {}};
    }
    std::wstring reservedReason = ReservedMainHotKeyReason(key);
    if (!reservedReason.empty()) {
        return HotKeyAvailability{false, ERROR_SUCCESS, std::move(reservedReason)};
    }
    SetLastError(ERROR_SUCCESS);
    if (RegisterHotKey(hwnd, ID_MAIN_HOTKEY_PROBE, MOD_CONTROL | MOD_ALT, static_cast<UINT>(key))) {
        UnregisterHotKey(hwnd, ID_MAIN_HOTKEY_PROBE);
        return HotKeyAvailability{true, ERROR_SUCCESS, {}};
    }
    const DWORD error = GetLastError();
    std::wstring reason = L"该热键无法注册，可能已被系统、输入法或其它软件占用。";
    if (error != ERROR_SUCCESS) {
        reason += L"\n\n系统返回: " + FormatLastError(error);
    }
    return HotKeyAvailability{false, error, std::move(reason)};
}

std::wstring MainHotKeyConflictMessage(int key, const HotKeyAvailability& availability) {
    return FormatMainHotKeyText(key) + L" 不可用。\n\n" + availability.reason;
}

std::wstring MainHotKeyStatusText(int key, const HotKeyAvailability& availability) {
    if (IsDoubleAltMainHotKey(key)) {
        return availability.available ? L"当前热键可用。" : L"热键冲突：双击 Alt 不可用。";
    }
    if (key <= 0) {
        return L"未设置主窗口热键。";
    }
    if (availability.available) {
        return L"当前热键可用。";
    }
    return L"热键冲突：" + FormatMainHotKeyText(key) + L" 可能已被系统、输入法或其它软件占用。";
}

std::wstring ProcessLocatorHotKeyStatusText(int key, const HotKeyAvailability& availability) {
    if (key <= 0) {
        return L"进程定位器快捷键未设置。";
    }
    if (availability.available) {
        return L"当前快捷键可用。";
    }
    return L"进程定位器快捷键 " + FormatGlobalHotKeyText(key) + L" 已被占用。";
}

std::wstring CopySelectedPathsHotKeyStatusText(int key, const HotKeyAvailability& availability) {
    if (key <= 0) {
        return L"复制选中项绝对路径快捷键未设置。";
    }
    if (availability.available) {
        return L"当前快捷键可用。";
    }
    return L"复制选中项绝对路径快捷键 " + FormatGlobalHotKeyText(key) + L" 已被占用。";
}

void ShowHotKeyConflictMessage(HWND owner, HINSTANCE instance, const Theme& theme, const std::wstring& message) {
    if (QuattroTestMode()) {
        return;
    }
    ShowThemedMessageBox(owner, instance, theme, message, L"热键冲突", MB_OK | MB_ICONWARNING);
}

std::wstring GetText(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

float ClampFloat(float value, float minValue, float maxValue) {
    return std::max(minValue, std::min(maxValue, value));
}

COLORREF ToColorRef(Color color) {
    const auto byte = [](float value) -> BYTE {
        return static_cast<BYTE>(ClampFloat(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return RGB(byte(color.r), byte(color.g), byte(color.b));
}

std::wstring FormatConfigPackageReportText(const ConfigPackageReport& report) {
    std::wstring text = report.message.empty() ? (report.ok ? L"操作完成。" : L"操作失败。") : report.message;
    if (report.groupsAdded > 0 || report.groupsMerged > 0 || report.tagsAdded > 0 ||
        report.tagsMerged > 0 || report.linksAdded > 0 || report.linksSkippedDuplicate > 0 ||
        report.notesAdded > 0 || report.notesMerged > 0 || report.todosAdded > 0 ||
        report.todosUpdatedFromRemote > 0 || report.todosKeptLocal > 0 || report.todosRestored > 0 ||
        report.todosKeptDeleted > 0 || report.todosSkippedIdentical > 0 || report.todosConflicted > 0 ||
        report.todosRemoteDeleteConflicts > 0 || report.todosFailed > 0 ||
        report.urlIconsAdded > 0) {
        text += L"\n\n新增分组: " + std::to_wstring(report.groupsAdded);
        text += L"\n复用分组: " + std::to_wstring(report.groupsMerged);
        text += L"\n新增标签: " + std::to_wstring(report.tagsAdded);
        text += L"\n复用标签: " + std::to_wstring(report.tagsMerged);
        text += L"\n新增启动项: " + std::to_wstring(report.linksAdded);
        text += L"\n跳过重复启动项: " + std::to_wstring(report.linksSkippedDuplicate);
        text += L"\n新增便签: " + std::to_wstring(report.notesAdded);
        text += L"\n合并便签: " + std::to_wstring(report.notesMerged);
        text += L"\n新增待办: " + std::to_wstring(report.todosAdded);
        text += L"\n远端更新待办: " + std::to_wstring(report.todosUpdatedFromRemote);
        text += L"\n保留本地待办: " + std::to_wstring(report.todosKeptLocal);
        text += L"\n恢复已删除待办: " + std::to_wstring(report.todosRestored);
        text += L"\n保持删除: " + std::to_wstring(report.todosKeptDeleted);
        text += L"\n内容相同跳过: " + std::to_wstring(report.todosSkippedIdentical);
        text += L"\n待办冲突: " + std::to_wstring(report.todosConflicted);
        text += L"\n远端删除冲突: " + std::to_wstring(report.todosRemoteDeleteConflicts);
        text += L"\n待办失败: " + std::to_wstring(report.todosFailed);
        text += L"\n新增 URL 图标: " + std::to_wstring(report.urlIconsAdded);
    }
    if (!report.warnings.empty()) {
        text += L"\n\n警告:";
        for (const auto& warning : report.warnings) {
            text += L"\n- " + warning;
        }
    }
    return text;
}

std::wstring FormatTodoJsonImportReportText(const TodoJsonImportReport& report) {
    std::wstring text = report.message.empty() ? (report.ok ? L"操作完成。" : L"操作失败。") : report.message;
    text += L"\n\n待办总数: " + std::to_wstring(report.todosParsed);
    text += L"\n新增待办: " + std::to_wstring(report.todosAdded);
    text += L"\n远端更新待办: " + std::to_wstring(report.todosUpdatedFromRemote);
    text += L"\n保留本地待办: " + std::to_wstring(report.todosKeptLocal);
    text += L"\n恢复已删除待办: " + std::to_wstring(report.todosRestored);
    text += L"\n保持删除: " + std::to_wstring(report.todosKeptDeleted);
    text += L"\n内容相同跳过: " + std::to_wstring(report.todosSkippedIdentical);
    text += L"\n待办冲突: " + std::to_wstring(report.todosConflicted);
    text += L"\n全量替换删除: " + std::to_wstring(report.todosDeletedForReplace);
    text += L"\n新增分组: " + std::to_wstring(report.groupsCreated);
    text += L"\n新增待办标签: " + std::to_wstring(report.tagsCreated);
    text += L"\n失败条目: " + std::to_wstring(report.todosFailed);
    if (!report.warnings.empty()) {
        text += L"\n\n警告:";
        for (const auto& warning : report.warnings) {
            text += L"\n- " + warning;
        }
    }
    return text;
}

std::wstring FormatFileSize(std::uint64_t bytes) {
    if (bytes >= 1024ull * 1024ull) {
        return std::to_wstring((bytes + 1024ull * 1024ull - 1) / (1024ull * 1024ull)) + L" MB";
    }
    if (bytes >= 1024ull) {
        return std::to_wstring((bytes + 1023ull) / 1024ull) + L" KB";
    }
    return std::to_wstring(bytes) + L" B";
}

std::wstring ConfigPackageFileName() {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"quattro-%04u%02u%02u-%02u%02u.q4cfg",
        static_cast<unsigned>(local.wYear),
        static_cast<unsigned>(local.wMonth),
        static_cast<unsigned>(local.wDay),
        static_cast<unsigned>(local.wHour),
        static_cast<unsigned>(local.wMinute));
    return buffer;
}

bool SelectSavePath(HWND owner, const std::wstring& initialPath, const wchar_t* filter, const wchar_t* defExt, std::wstring& selectedPath) {
    std::wstring buffer = initialPath;
    buffer.resize(32768, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrFilter = filter;
    ofn.lpstrDefExt = defExt;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) {
        return false;
    }
    selectedPath = buffer.c_str();
    return true;
}

bool SelectOpenPath(
    HWND owner,
    const wchar_t* context,
    const wchar_t* filter,
    const wchar_t* defExt,
    const std::wstring& defaultPath,
    std::wstring& selectedPath) {
    CommonFileDialogOptions options{};
    options.owner = owner;
    options.mode = CommonFileDialogMode::FileOnly;
    options.context = context ? context : L"通用打开文件";
    options.defaultPath = defaultPath;
    options.legacyFilter = filter;
    if (defExt) {
        options.defaultExtension = defExt;
    }
    CommonFileDialogResult result{};
    if (!ShowCommonFileDialog(options, result)) {
        return false;
    }
    selectedPath = result.path;
    return true;
}

int EnglishMonthIndex(const std::wstring& month) {
    static constexpr const wchar_t* kMonths[] = {
        L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
        L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    for (int i = 0; i < 12; ++i) {
        if (_wcsicmp(month.c_str(), kMonths[i]) == 0) {
            return i + 1;
        }
    }
    return 0;
}

std::wstring ChineseDateTimeText(int year, int month, int day, int hour, int minute) {
    if (year <= 0 || month <= 0 || day <= 0) {
        return {};
    }
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%d年%d月%d日 %02d:%02d", year, month, day, hour, minute);
    return buffer;
}

std::wstring LocalBackupDateTimeTextFromUtc(int year, int month, int day, int hour, int minute, int second) {
    SYSTEMTIME utc{};
    utc.wYear = static_cast<WORD>(year);
    utc.wMonth = static_cast<WORD>(month);
    utc.wDay = static_cast<WORD>(day);
    utc.wHour = static_cast<WORD>(hour);
    utc.wMinute = static_cast<WORD>(minute);
    utc.wSecond = static_cast<WORD>(second);

    FILETIME utcFile{};
    FILETIME localFile{};
    SYSTEMTIME local{};
    if (!SystemTimeToFileTime(&utc, &utcFile) ||
        !FileTimeToLocalFileTime(&utcFile, &localFile) ||
        !FileTimeToSystemTime(&localFile, &local)) {
        return {};
    }
    return ChineseDateTimeText(local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
}

std::wstring FormatBackupModifiedDate(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    wchar_t monthText[8]{};
    if (swscanf_s(value.c_str(), L"%*3ls, %d %7ls %d %d:%d:%d", &day, monthText, static_cast<unsigned>(std::size(monthText)), &year, &hour, &minute, &second) == 6) {
        month = EnglishMonthIndex(monthText);
        const std::wstring formatted = LocalBackupDateTimeTextFromUtc(year, month, day, hour, minute, second);
        if (!formatted.empty()) {
            return formatted;
        }
    }
    if (swscanf_s(value.c_str(), L"%d-%d-%d %d:%d", &year, &month, &day, &hour, &minute) == 5) {
        const std::wstring formatted = ChineseDateTimeText(year, month, day, hour, minute);
        if (!formatted.empty()) {
            return formatted;
        }
    }
    return value;
}

std::wstring FormatWebDavLastSyncText(const std::wstring& value) {
    SYSTEMTIME time{};
    if (!TryParseTodoTimestamp(value, time)) {
        return {};
    }
    const std::wstring formatted = ChineseDateTimeText(time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute);
    return formatted.empty() ? std::wstring{} : L"最后同步：" + formatted;
}

std::wstring FormatBackupListItem(const WebDavRemoteFile& backup) {
    std::wstring text = backup.name;
    if (backup.size > 0) {
        text += L"    " + FormatFileSize(backup.size);
    }
    if (!backup.lastModified.empty()) {
        text += L"    " + FormatBackupModifiedDate(backup.lastModified);
    }
    return text;
}

std::wstring WrapLongToken(const std::wstring& value, std::size_t maxCharsPerLine) {
    if (value.size() <= maxCharsPerLine || maxCharsPerLine == 0) {
        return value;
    }
    std::wstring text;
    for (std::size_t i = 0; i < value.size(); i += maxCharsPerLine) {
        if (!text.empty()) {
            text += L"\n";
        }
        text += value.substr(i, maxCharsPerLine);
    }
    return text;
}

std::wstring FormatBackupConfirmationText(const WebDavRemoteFile& backup) {
    std::wstring text =
        L"请确认要下载并合并以下 WebDAV 备份：\n\n"
        L"文件名:\n" + WrapLongToken(backup.name, 42) + L"\n"
        L"文件大小: " + FormatFileSize(backup.size);
    const std::wstring modified = FormatBackupModifiedDate(backup.lastModified);
    if (!modified.empty()) {
        text += L"\n备份时间: " + modified;
    }
    text +=
        L"\n\n将把该备份中的分组、标签、启动项、便签和待办合并到当前数据。"
        L"\n同一待办按最后更新时间保留较新版本；本地已删除的条目会再次询问是否恢复。"
        L"\n导入前会自动备份。";
    return text;
}

int EstimateMessageRows(const std::wstring& message, int width, int averageCharWidth) {
    int rows = 1;
    int lineLength = 0;
    const int charsPerRow = std::max(12, width / std::max(1, averageCharWidth));
    for (wchar_t ch : message) {
        if (ch == L'\r') {
            continue;
        }
        if (ch == L'\n') {
            ++rows;
            lineLength = 0;
            continue;
        }
        ++lineLength;
        if (lineLength >= charsPerRow) {
            ++rows;
            lineLength = 0;
        }
    }
    return std::max(1, rows);
}

int MeasureMessageTextHeight(const Theme& theme, const std::wstring& message, int width) {
    HFONT font = ThemedControls::CreateDialogFont();
    if (!font) {
        font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    HDC dc = GetDC(nullptr);
    ThemedPaint paint(nullptr, dc, theme, font);
    const int lineHeight = std::max(20, static_cast<int>(theme.metric(L"text", L"textHeight", 20.0f)));
    const int averageCharWidth = std::max(1, lineHeight / 2);
    const SIZE measured = paint.MeasureText(message, std::max(1, width), true);
    ReleaseDC(nullptr, dc);
    if (font && font != GetStockObject(DEFAULT_GUI_FONT)) {
        DeleteObject(font);
    }
    const int editRowHeight = lineHeight + std::max(4, lineHeight / 4);
    const int controlPadding = lineHeight + std::max(8, lineHeight / 2);
    const int estimatedHeight = EstimateMessageRows(message, width, averageCharWidth) * editRowHeight + controlPadding;
    return std::max(lineHeight, std::max(static_cast<int>(measured.cy), estimatedHeight));
}

HMODULE RichEditLibrary() {
    static HMODULE module = LoadLibraryW(L"Msftedit.dll");
    return module;
}

class ThemedMessageDialog {
public:
    ThemedMessageDialog(HWND owner, HINSTANCE instance, const Theme& theme, std::wstring message, std::wstring title, UINT flags)
        : owner_(owner), instance_(instance), theme_(theme), message_(std::move(message)), title_(std::move(title)), flags_(flags) {}

    int Run() {
        RECT workArea{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);

        width_ = 430;
        const DialogLayoutMetrics layout = GetDialogLayoutMetrics(theme_, DialogLayoutKind::Mini);
        const int buttonHeight = ThemedControls::ButtonHeight(theme_);
        const int textWidth = width_ - layout.contentInsetX * 2;
        const int textHeight = MeasureMessageTextHeight(theme_, message_, textWidth);
        const int availableHeight = std::max(260, static_cast<int>(workArea.bottom - workArea.top) * 3 / 4);
        const int maxTextHeight = std::max(80, availableHeight - layout.contentInsetY - layout.footerGap - buttonHeight - layout.footerInsetY);
        textNeedsScroll_ = textHeight + 4 > maxTextHeight;
        textHeight_ = std::min(std::max(32, textHeight + 4), maxTextHeight);
        const int clientHeight = std::max(150, layout.contentInsetY + textHeight_ + layout.footerGap + buttonHeight + layout.footerInsetY);
        const std::wstring className = L"QuattroThemedMessageDialog";
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        auto options = ThemedWindowUi::DialogOptions(
            instance_, owner_, className.c_str(), title_.empty() ? L"提示" : title_.c_str(),
            ThemedMessageDialog::Proc, this, icon, icon);
        options.clientWidth = width_;
        options.clientHeight = clientHeight;
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options);
        if (!hwnd_) {
            return MessageBoxW(owner_, message_.c_str(), title_.c_str(), flags_);
        }

        windowUi_->ShowModal();
        UpdateWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return result_;
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        ThemedMessageDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<ThemedMessageDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<ThemedMessageDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool YesNo() const {
        return (flags_ & MB_TYPEMASK) == MB_YESNO;
    }

    bool YesNoCancel() const {
        return (flags_ & MB_TYPEMASK) == MB_YESNOCANCEL;
    }

    bool OkCancel() const {
        return (flags_ & MB_TYPEMASK) == MB_OKCANCEL;
    }

    void Close(int result) {
        result_ = result;
        done_ = true;
        DestroyWindow(hwnd_);
    }

    static LRESULT CALLBACK TextControlProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR) {
        switch (message) {
        case WM_KEYDOWN:
            if ((wParam == L'A' || wParam == L'a') && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                SendMessageW(hwnd, EM_SETSEL, 0, -1);
                return 0;
            }
            break;
        case WM_KILLFOCUS: {
            DWORD selectionStart = 0;
            DWORD selectionEnd = 0;
            SendMessageW(
                hwnd,
                EM_GETSEL,
                reinterpret_cast<WPARAM>(&selectionStart),
                reinterpret_cast<LPARAM>(&selectionEnd));
            SendMessageW(hwnd, EM_SETSEL, selectionEnd, selectionEnd);
            break;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, TextControlProc, subclassId);
            break;
        default:
            break;
        }
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }

    RECT MessageTextRect(const DialogLayoutMetrics& layout, int clientWidth) const {
        return RECT{
            layout.contentInsetX,
            layout.contentInsetY,
            clientWidth - layout.contentInsetX,
            layout.contentInsetY + textHeight_};
    }

    DWORD MessageTextStyle() const {
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL;
        if (textNeedsScroll_) {
            style |= WS_VSCROLL;
        }
        return style;
    }

    std::wstring MessageControlText() const {
        std::wstring text;
        text.reserve(message_.size() + 8);
        for (std::size_t i = 0; i < message_.size(); ++i) {
            const wchar_t ch = message_[i];
            if (ch == L'\r') {
                text += L"\r\n";
                if (i + 1 < message_.size() && message_[i + 1] == L'\n') {
                    ++i;
                }
            } else if (ch == L'\n') {
                text += L"\r\n";
            } else {
                text += ch;
            }
        }
        return text;
    }

    void ConfigureMessageText(HWND hwnd, bool richEdit) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SendMessageW(hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
        SetWindowSubclass(hwnd, TextControlProc, 1, 0);

        if (richEdit) {
            const COLORREF background = ToColorRef(theme_.color(L"dialog", L"normal", L"bg"));
            SendMessageW(hwnd, EM_SETBKGNDCOLOR, 0, static_cast<LPARAM>(background));

            CHARFORMAT2W format{};
            format.cbSize = sizeof(format);
            format.dwMask = CFM_COLOR;
            format.crTextColor = ToColorRef(theme_.color(L"label", L"normal", L"text"));
            SendMessageW(hwnd, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&format));

            SendMessageW(hwnd, EM_AUTOURLDETECT, TRUE, 0);
            const LRESULT mask = SendMessageW(hwnd, EM_GETEVENTMASK, 0, 0);
            SendMessageW(hwnd, EM_SETEVENTMASK, 0, static_cast<LPARAM>(mask | ENM_LINK));
            const std::wstring text = MessageControlText();
            SetWindowTextW(hwnd, text.c_str());
        }
    }

    bool CreateRichMessageText(const RECT& rect) {
        if (!RichEditLibrary()) {
            return false;
        }
        messageEdit_ = CreateWindowExW(
            0,
            MSFTEDIT_CLASS,
            nullptr,
            MessageTextStyle(),
            rect.left,
            rect.top,
            rect.right - rect.left,
            rect.bottom - rect.top,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_MESSAGE_TEXT)),
            instance_,
            nullptr);
        if (!messageEdit_) {
            return false;
        }
        messageTextIsRichEdit_ = true;
        ConfigureMessageText(messageEdit_, true);
        return true;
    }

    void CreateFallbackMessageText(const RECT& rect) {
        const std::wstring text = MessageControlText();
        messageEdit_ = CreateWindowExW(
            0,
            L"EDIT",
            text.c_str(),
            MessageTextStyle(),
            rect.left,
            rect.top,
            rect.right - rect.left,
            rect.bottom - rect.top,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_MESSAGE_TEXT)),
            instance_,
            nullptr);
        if (messageEdit_) {
            ConfigureMessageText(messageEdit_, false);
        }
    }

    void CreateMessageTextControl(const DialogLayoutMetrics& layout, int clientWidth) {
        const RECT rect = MessageTextRect(layout, clientWidth);
        if (!CreateRichMessageText(rect)) {
            CreateFallbackMessageText(rect);
        }
    }

    std::wstring LinkText(const CHARRANGE& range) const {
        if (!messageEdit_ || range.cpMax <= range.cpMin) {
            return {};
        }
        std::wstring text(static_cast<std::size_t>(range.cpMax - range.cpMin) + 1, L'\0');
        TEXTRANGEW textRange{};
        textRange.chrg = range;
        textRange.lpstrText = text.data();
        SendMessageW(messageEdit_, EM_GETTEXTRANGE, 0, reinterpret_cast<LPARAM>(&textRange));
        text.resize(std::wcslen(text.c_str()));
        while (!text.empty() && (text.back() == L'.' || text.back() == L',' || text.back() == L';' ||
                                 text.back() == L':' || text.back() == L')' || text.back() == L'）' ||
                                 text.back() == L'。' || text.back() == L'，')) {
            text.pop_back();
        }
        return Trim(text);
    }

    bool HandleMessageTextNotify(LPARAM lParam) {
        if (!messageTextIsRichEdit_ || !messageEdit_) {
            return false;
        }
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (!header || header->hwndFrom != messageEdit_ || header->code != EN_LINK) {
            return false;
        }
        auto* link = reinterpret_cast<ENLINK*>(lParam);
        if (link->msg != WM_LBUTTONUP) {
            return false;
        }
        const std::wstring url = LinkText(link->chrg);
        if (url.empty()) {
            return false;
        }
        ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT commonResult = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, commonResult)) {
            return commonResult;
        }
        switch (message) {
        case WM_CREATE: {
            RECT client{};
            GetClientRect(hwnd_, &client);
            const int clientWidth = client.right - client.left;
            const int clientHeight = client.bottom - client.top;
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_, owner_, hwnd_, theme_, DialogLayoutKind::Mini, clientWidth, clientHeight);
            font_ = windowUi_->font();
            const DialogLayoutMetrics layout = windowUi_->ui().layout();
            CreateMessageTextControl(layout, clientWidth);
            const ThemedUi ui = windowUi_->ui();
            if (YesNoCancel()) {
                ui.FooterButton(IDYES, L"是", 0, 3, true, true);
                ui.FooterButton(IDNO, L"否", 1, 3);
                ui.FooterButton(IDCANCEL, L"取消", 2, 3);
            } else if (YesNo()) {
                ui.FooterButton(IDYES, L"是", 0, 2, true, true);
                ui.FooterButton(IDNO, L"否", 1, 2);
            } else if (OkCancel()) {
                ui.FooterButton(IDOK, L"确定", 0, 2, true, true);
                ui.FooterButton(IDCANCEL, L"取消", 1, 2);
            } else {
                ui.FooterButton(IDOK, L"确定", 0, 1, true, true);
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            windowUi_->FillBackground(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_NOTIFY:
            if (HandleMessageTextNotify(lParam)) {
                return TRUE;
            }
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case IDYES:
                Close(IDYES);
                return 0;
            case IDNO:
                Close(IDNO);
                return 0;
            case IDOK:
                Close(IDOK);
                return 0;
            case IDCANCEL:
                Close(IDCANCEL);
                return 0;
            default:
                return 0;
            }
        case WM_CLOSE:
            Close(YesNo() ? IDNO : IDCANCEL);
            return 0;
        case WM_NCDESTROY:
            done_ = true;
            hwnd_ = nullptr;
            font_ = nullptr;
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND messageEdit_ = nullptr;
    const Theme& theme_;
    std::wstring message_;
    std::wstring title_;
    UINT flags_ = MB_OK;
    int width_ = 430;
    int height_ = 150;
    int textHeight_ = 32;
    int result_ = IDOK;
    HFONT font_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    bool messageTextIsRichEdit_ = false;
    bool textNeedsScroll_ = false;
    bool done_ = false;
};

class TextDialog {
public:
    TextDialog(HWND owner, HINSTANCE instance, const Theme& theme, std::wstring title, std::wstring label, std::wstring& value)
        : owner_(owner), instance_(instance), theme_(theme), title_(std::move(title)), label_(std::move(label)), value_(value) {}

    bool Run() {
        const std::wstring className = L"QuattroTextInputDialog_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        constexpr int kClientWidth = 390;
        constexpr int kClientHeight = 162;
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        ThemedWindowCreateOptions options = ThemedWindowUi::DialogOptions(
            instance_, owner_, className.c_str(), title_.c_str(), TextDialog::Proc, this, icon, icon);
        options.clientWidth = kClientWidth;
        options.clientHeight = kClientHeight;
        options.placement = ThemedWindowPlacement::OffsetOwner;
        options.offsetX = 80;
        options.offsetY = 100;
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options);
        if (!hwnd_) {
            const DWORD error = GetLastError();
            WriteAppLog(L"文本输入窗口创建失败: " + FormatLastError(error));
            return false;
        }
        if (windowUi_) windowUi_->ShowModal();
        UpdateWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (windowUi_) windowUi_->RestoreModalOwner();
        return accepted_;
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        TextDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<TextDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<TextDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT commonResult = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, commonResult)) {
            return commonResult;
        }
        switch (message) {
        case WM_CREATE: {
            constexpr int kClientWidth = 390;
            constexpr int kClientHeight = 162;
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_, owner_, hwnd_, theme_, DialogLayoutKind::Mini, kClientWidth, kClientHeight);
            const DialogLayoutMetrics layout = GetDialogLayoutMetrics(theme_, DialogLayoutKind::Mini);
            RECT client{};
            GetClientRect(hwnd_, &client);
            const int clientWidth = client.right - client.left;
            const int contentWidth = clientWidth - layout.contentInsetX * 2;
            const int labelY = layout.contentInsetY;
            const ThemedUi ui = windowUi_->ui();
            ui.SelectableLabel(label_, layout.contentInsetX, labelY, contentWidth);
            const int fieldHeight = ThemedControls::EditFrameHeight(theme_);
            const int editY = labelY + ThemedControls::LabelHeight(theme_) + layout.rowGap;
            editFrame_ = RECT{layout.contentInsetX, editY, layout.contentInsetX + contentWidth, editY + fieldHeight};
            edit_ = ui.Edit(100, editFrame_, value_);
            ui.FooterButton(IDOK, L"确定", 0, 2, true, true);
            ui.FooterButton(IDCANCEL, L"取消", 1, 2);
            SetFocus(edit_);
            SendMessageW(edit_, EM_SETSEL, 0, -1);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            windowUi_->FillBackground(dc);
            windowUi_->DrawRegisteredEditFrames(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                std::wstring next = Trim(GetText(edit_));
                if (next.empty()) {
                    ShowThemedMessageBox(hwnd_, instance_, theme_, L"名称不能为空。", title_, MB_OK | MB_ICONWARNING);
                    return 0;
                }
                value_ = next;
                accepted_ = true;
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            return 0;
        case WM_CLOSE:
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    const Theme& theme_;
    std::wstring title_;
    std::wstring label_;
    std::wstring& value_;
    RECT editFrame_{};
    std::unique_ptr<ThemedWindowUi> windowUi_;
    bool accepted_ = false;
    bool done_ = false;
};

class HotKeyConflictDialog {
public:
    HotKeyConflictDialog(
        HWND owner,
        HINSTANCE instance,
        const Theme& theme,
        std::wstring message,
        bool& ignoreFutureWarnings)
        : owner_(owner),
          instance_(instance),
          theme_(theme),
          message_(std::move(message)),
          ignoreFutureWarnings_(ignoreFutureWarnings) {}

    bool Run() {
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        ThemedWindowCreateOptions options = ThemedWindowUi::DialogOptions(
            instance_,
            owner_,
            L"QuattroHotKeyConflictDialog",
            L"热键冲突",
            HotKeyConflictDialog::Proc,
            this,
            icon,
            icon);
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options);
        if (!hwnd_) {
            return false;
        }
        if (windowUi_) {
            windowUi_->ShowModal();
        }
        UpdateWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (windowUi_) {
            windowUi_->RestoreModalOwner();
        }
        return accepted_;
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        HotKeyConflictDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<HotKeyConflictDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<HotKeyConflictDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void Close(bool accepted) {
        accepted_ = accepted;
        if (ignoreToggle_) {
            ignoreFutureWarnings_ = ThemedUi::IsChecked(ignoreToggle_);
        }
        done_ = true;
        if (windowUi_) {
            windowUi_->RestoreModalOwner();
        }
        DestroyWindow(hwnd_);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT commonResult = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, commonResult)) {
            return commonResult;
        }

        switch (message) {
        case WM_CREATE: {
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_,
                owner_,
                hwnd_,
                theme_,
                kThemedDialogLayoutKind,
                kThemedDialogClientWidth,
                kThemedDialogClientHeight);
            const ThemedUi ui = windowUi_->ui();
            const ThemedFormLayout form(ui);
            int y = ui.contentTop();

            const auto messageRow = form.row(y, ThemedRowAlign::Left, {form.item(ui.contentWidth(), ui.labelHeight() * 3)});
            ui.DetailText(ID_MESSAGE_TEXT, messageRow[0], message_);

            y = form.nextRowY(y, {form.item(ui.contentWidth(), ui.labelHeight() * 3)});
            ThemedToggleOptions toggleOptions{};
            toggleOptions.checked = ignoreFutureWarnings_;
            const auto toggleRow = form.row(y, ThemedRowAlign::Left, {form.item(ui.contentWidth(), ThemedControls::CheckBoxHeight(theme_))});
            ignoreToggle_ = ui.Toggle(
                ID_HOTKEY_CONFLICT_IGNORE,
                L"以后忽略启动时的热键冲突提示",
                toggleRow[0].left,
                toggleRow[0].top,
                toggleRow[0].right - toggleRow[0].left,
                toggleOptions);

            ui.FooterButton(IDOK, L"知道了", 0, 1, true, true);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case IDOK:
                Close(true);
                return 0;
            case IDCANCEL:
                Close(false);
                return 0;
            default:
                break;
            }
            break;
        case WM_CLOSE:
            Close(false);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    std::wstring message_;
    bool& ignoreFutureWarnings_;
    HWND hwnd_ = nullptr;
    HWND ignoreToggle_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    bool accepted_ = false;
    bool done_ = false;
};

class WebDavBackupSelectionDialog {
public:
    WebDavBackupSelectionDialog(
        HWND owner,
        HINSTANCE instance,
        const Theme& theme,
        const std::vector<WebDavRemoteFile>& backups,
        std::wstring& selectedName)
        : owner_(owner), instance_(instance), theme_(theme), backups_(backups), selectedName_(selectedName) {}

    bool Run() {
        const std::wstring className = L"QuattroWebDavBackupSelectionDialog_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        auto options = ThemedWindowUi::DialogOptions(
            instance_, owner_, className.c_str(), L"选择 WebDAV 备份",
            WebDavBackupSelectionDialog::Proc, this,
            LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON)),
            LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON)));
        options.clientWidth = kThemedDetailsClientWidth;
        options.clientHeight = kThemedDetailsClientHeight;
        options.placement = ThemedWindowPlacement::OffsetOwner;
        options.offsetX = 60;
        options.offsetY = 80;
        std::wstring createError;
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options, &createError);
        if (!hwnd_) {
            WriteAppLog(L"WebDAV 备份选择窗口创建失败: " + createError);
            return false;
        }

        windowUi_->ShowModal();
        UpdateWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return accepted_;
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        WebDavBackupSelectionDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<WebDavBackupSelectionDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<WebDavBackupSelectionDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void PopulateTable() {
        std::vector<ThemedTableRow> rows;
        rows.reserve(backups_.size());
        for (std::size_t index = 0; index < backups_.size(); ++index) {
            const auto& backup = backups_[index];
            ThemedTableRow row;
            row.key = static_cast<std::intptr_t>(index);
            row.cells = {
                ThemedTableCell{backup.name},
                ThemedTableCell{FormatFileSize(backup.size)},
                ThemedTableCell{FormatBackupModifiedDate(backup.lastModified)},
            };
            rows.push_back(std::move(row));
        }
        ThemedUi::SetTableRows(table_, rows);
        if (!rows.empty()) ThemedUi::SetTableSelectedIndex(table_, 0);
    }

    void AcceptSelection() {
        const int selected = ThemedUi::TableSelectedIndex(table_);
        if (selected < 0 || selected >= static_cast<int>(backups_.size())) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"请选择一个备份文件。", L"选择 WebDAV 备份", MB_OK | MB_ICONWARNING);
            return;
        }
        selectedName_ = backups_[static_cast<std::size_t>(selected)].name;
        accepted_ = true;
        done_ = true;
        DestroyWindow(hwnd_);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT commonResult = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, commonResult)) {
            return commonResult;
        }
        switch (message) {
        case WM_CREATE: {
            RECT client{};
            GetClientRect(hwnd_, &client);
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_, owner_, hwnd_, theme_, DialogLayoutKind::Standard,
                client.right - client.left, client.bottom - client.top);
            const ThemedUi& ui = windowUi_->ui();
            const auto& layout = ui.layout();
            const int labelY = ui.contentTop();
            ui.SelectableLabel(L"云端备份记录", ui.contentLeft(), labelY, ui.contentWidth());
            const int tableTop = labelY + ui.labelHeight() + layout.sectionGap;
            const int footerTop = layout.FooterButtonY(ui.clientHeight(), ui.footerButtonHeight());
            const RECT tableFrame{
                ui.contentLeft(), tableTop, ui.contentLeft() + ui.contentWidth(),
                std::max(tableTop + ui.tableHeightForRows(1, true), footerTop - layout.footerGap)};
            const std::vector<ThemedTableColumn> columns = {
                {L"name", L"文件名", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
                {L"size", L"大小", ThemedTableColumnAlign::End, ThemedTableColumnWidth::Content},
                {L"modified", L"修改时间", ThemedTableColumnAlign::End, ThemedTableColumnWidth::Content},
            };
            table_ = ui.Table(ID_WEBDAV_BACKUP_LIST, tableFrame, columns);
            PopulateTable();
            ui.FooterButton(IDOK, L"下载", 0, 2, true, true);
            ui.FooterButton(IDCANCEL, L"取消", 1, 2);
            SetFocus(table_);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            windowUi_->FillBackground(dc);
            windowUi_->DrawRegisteredTableFrames(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_NOTIFY: {
            ThemedTableEvent event;
            if (ThemedUi::DecodeTableEvent(table_, lParam, event) &&
                event.kind == ThemedTableEventKind::Activated) {
                AcceptSelection();
                return 0;
            }
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                AcceptSelection();
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            return 0;
        case WM_CLOSE:
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        case WM_NCDESTROY:
            done_ = true;
            hwnd_ = nullptr;
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND table_ = nullptr;
    const Theme& theme_;
    const std::vector<WebDavRemoteFile>& backups_;
    std::wstring& selectedName_;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    bool accepted_ = false;
    bool done_ = false;
};

class WebDavFileManagerDialog {
public:
    WebDavFileManagerDialog(HWND owner, HINSTANCE instance, const Theme& theme, AppConfig config)
        : owner_(owner), instance_(instance), theme_(theme), config_(std::move(config)) {}

    bool Run() {
        const std::wstring className = L"QuattroWebDavFileManagerDialog_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        auto options = ThemedWindowUi::DialogOptions(instance_, owner_, className.c_str(), L"WebDAV 文件管理", Proc, this,
            LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON)), LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON)));
        options.clientWidth = 900;
        options.clientHeight = 480;
        options.resizable = true;
        options.maximizable = true;
        options.minimizable = true;
        options.placement = ThemedWindowPlacement::OffsetOwner; options.offsetX = 60; options.offsetY = 80;
        std::wstring error; hwnd_ = ThemedWindowUi::CreateWindowHandle(options, &error);
        if (!hwnd_) { WriteAppLog(L"WebDAV 文件管理窗口创建失败: " + error); return false; }
        windowUi_->ShowModal(); UpdateWindow(hwnd_);
        MSG message{}; while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        return true;
    }
private:
    enum : UINT {
        ID_FILE_ACTION_DOWNLOAD = 431,
        ID_FILE_ACTION_DETAILS = 432,
        ID_FILE_ACTION_DELETE = 433,
        ID_FILE_ROW_MENU = 434,
        ID_FILE_ACTION_OPEN_LOCATION = 435,
        ID_FILE_ACTION_UPLOAD = 436,
    };

    struct ListResult {
        std::uint64_t generation = 0;
        bool ok = false;
        std::vector<WebDavFileRecord> records;
        std::wstring error;
        std::wstring refreshedAtUtc;
    };
    struct BatchResult {
        std::uint64_t generation = 0;
        std::vector<WebDavFileRecord> records;
    };
    struct DeleteResult {
        std::vector<std::wstring> succeededIds;
        std::vector<std::wstring> failedIds;
        std::vector<std::wstring> notStartedIds;
        std::wstring lastError;
        bool stopped = false;
    };
    struct DeleteItemResult {
        std::wstring id;
        bool ok = false;
        std::wstring error;
    };
    struct DeleteTaskState {
        std::mutex mutex;
        std::atomic_bool stopRequested{false};
        std::size_t totalFiles = 0;
        std::size_t currentFile = 0;
        std::size_t completedSteps = 0;
        std::size_t succeeded = 0;
        std::size_t failed = 0;
        std::wstring currentName;
        std::wstring phaseText;
        std::wstring lastError;
        bool finished = false;
        bool stopped = false;
    };

    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* dialog = reinterpret_cast<WebDavFileManagerDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) { auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam); dialog = static_cast<WebDavFileManagerDialog*>(create->lpCreateParams); SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog)); dialog->hwnd_ = hwnd; }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }
    static bool IsHealthy(const WebDavFileRecord& record) {
        return record.health == WebDavFileRecordHealth::Healthy;
    }
    static std::wstring HealthText(const WebDavFileRecord& record) {
        switch (record.health) {
        case WebDavFileRecordHealth::MissingMetadata: return L"Meta 缺失";
        case WebDavFileRecordHealth::InvalidMetadata: return L"Meta 无效";
        case WebDavFileRecordHealth::MetadataReadFailed: return L"Meta 读取失败";
        default: return L"正常";
        }
    }
    static std::wstring UploadedAtText(const WebDavFileRecord& record) {
        if (!IsHealthy(record)) return L"获取失败";
        const std::wstring local = WebDavFileService::FormatUploadedAtLocal(record.uploadedAtUtc);
        return local.empty() ? L"获取失败" : local;
    }
    static std::wstring RowTooltipText(const WebDavFileRecord& record) {
        return WebDavFileService::FormatRecordTooltip(record);
    }
    static bool SameRecord(const WebDavFileRecord& left, const WebDavFileRecord& right) {
        return left.id == right.id && left.absolutePath == right.absolutePath &&
            left.displayName == right.displayName && left.size == right.size &&
            left.sha256 == right.sha256 && left.uploadedAtUtc == right.uploadedAtUtc &&
            left.sourceLastWriteTimeUtc == right.sourceLastWriteTimeUtc &&
            left.uploadState == right.uploadState && left.contentReady == right.contentReady &&
            left.health == right.health && left.recordError == right.recordError;
    }
    std::intptr_t RowKey(const std::wstring& id) {
        const auto existing = rowKeys_.find(id);
        if (existing != rowKeys_.end()) return existing->second;
        const std::intptr_t key = nextRowKey_++;
        rowKeys_.emplace(id, key);
        return key;
    }
    ThemedTableRow TableRow(const WebDavFileRecord& record) {
        const bool healthy = IsHealthy(record);
        ThemedTableCell action{L"…"};
        action.role = ThemedTableCellRole::Action;
        action.actionId = ID_FILE_ROW_MENU;
        return ThemedTableRow{
            RowKey(record.id),
            {ThemedTableCell{record.displayName},
             ThemedTableCell{healthy ? FormatFileSize(record.size) : L"—"},
             ThemedTableCell{healthy ? UploadedAtText(record) : HealthText(record)},
             ThemedTableCell{WebDavFileService::LocalSyncStatusText(record)}, action},
            checkedIds_.contains(record.id),
            !deletingIds_.contains(record.id)};
    }
    int RecordIndex(const std::wstring& id) const {
        const auto found = std::find_if(records_.begin(), records_.end(),
            [&](const auto& record) { return record.id == id; });
        return found == records_.end() ? -1 : static_cast<int>(found - records_.begin());
    }
    void LayoutControls() {
        if (!windowUi_ || !directoryLabel_) return;
        RECT client{};
        GetClientRect(hwnd_, &client);
        const ThemedUi ui(instance_, hwnd_, theme_, windowUi_->font(), DialogLayoutKind::Compact,
            client.right, client.bottom, windowUi_.get(), windowUi_.get(), windowUi_.get(), windowUi_.get());
        const auto& layout = ui.layout();
        const int buttonHeight = ui.buttonHeight();
        const int refreshWidth = ui.buttonWidth(
            L"刷新", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int queueWidth = ui.buttonWidth(
            L"队列", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int topGroupWidth = refreshWidth + layout.controlGapX + queueWidth;
        const int topGroupX = ui.contentLeft() + ui.contentWidth() - topGroupWidth;
        ui.MoveControl(directoryLabel_, RECT{
            ui.contentLeft(),
            ui.contentTop(),
            ui.contentLeft() + std::max(1, topGroupX - ui.contentLeft() - layout.controlGapX),
            ui.contentTop() + ui.labelHeight()});
        MoveWindow(refreshButton_, topGroupX, ui.contentTop(), refreshWidth, buttonHeight, TRUE);
        MoveWindow(transferQueueButton_, topGroupX + refreshWidth + layout.controlGapX,
            ui.contentTop(), queueWidth, buttonHeight, TRUE);

        const int actionY = ui.nextRowY(ui.contentTop(), buttonHeight);
        const int selectAllWidth = ui.buttonWidth(
            L"全选", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int clearWidth = ui.buttonWidth(
            L"清除选择", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int downloadWidth = ui.buttonWidth(
            L"下载所选", ThemedButtonRole::Primary, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int uploadWidth = ui.buttonWidth(
            L"上传所选", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        const int deleteWidth = ui.buttonWidth(
            L"删除所选", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        MoveWindow(selectAllButton_, ui.contentLeft(), actionY, selectAllWidth, buttonHeight, TRUE);
        MoveWindow(clearSelectionButton_, ui.contentLeft() + selectAllWidth + layout.controlGapX,
            actionY, clearWidth, buttonHeight, TRUE);
        const int selectionX = ui.contentLeft() + selectAllWidth + layout.controlGapX + clearWidth + layout.controlGapX;
        const int rightActionsWidth = uploadWidth + layout.controlGapX + downloadWidth + layout.controlGapX + deleteWidth;
        const int rightActionsX = ui.contentLeft() + ui.contentWidth() - rightActionsWidth;
        ui.MoveControl(selectionStatus_, RECT{
            selectionX,
            actionY,
            selectionX + std::max(1, rightActionsX - selectionX - layout.controlGapX),
            actionY + ui.labelHeight()});
        MoveWindow(uploadSelectedButton_, rightActionsX, actionY, uploadWidth, buttonHeight, TRUE);
        MoveWindow(downloadSelectedButton_, rightActionsX + uploadWidth + layout.controlGapX,
            actionY, downloadWidth, buttonHeight, TRUE);
        MoveWindow(deleteSelectedButton_, rightActionsX + uploadWidth + layout.controlGapX + downloadWidth + layout.controlGapX,
            actionY, deleteWidth, buttonHeight, TRUE);

        const int tableTop = ui.nextRowY(actionY, buttonHeight);
        ui.MoveTable(table_, RECT{ui.contentLeft(), tableTop,
            ui.contentLeft() + ui.contentWidth(), ui.clientHeight() - layout.contentInsetY});
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    void PopulateTable() {
        std::vector<ThemedTableRow> rows; rows.reserve(records_.size());
        for (const auto& record : records_) rows.push_back(TableRow(record));
        ThemedUi::SetTableRows(table_, rows);
        if (!rows.empty() && ThemedUi::TableSelectedIndex(table_) < 0) ThemedUi::SetTableSelectedIndex(table_, 0);
        UpdateSelectionState();
    }
    struct MergeSummary { int appended = 0; int updated = 0; int unchanged = 0; };
    MergeSummary MergeRecords(const std::vector<WebDavFileRecord>& batch) {
        MergeSummary summary;
        for (const auto& record : batch) {
            if (deletedTombstones_.contains(record.id)) continue;
            const int index = RecordIndex(record.id);
            if (index < 0) {
                records_.push_back(record);
                ThemedUi::AppendTableRow(table_, TableRow(records_.back()));
                ++summary.appended;
            } else if (!SameRecord(records_[static_cast<std::size_t>(index)], record)) {
                records_[static_cast<std::size_t>(index)] = record;
                ThemedUi::UpdateTableRow(table_, index, TableRow(records_[static_cast<std::size_t>(index)]));
                ++summary.updated;
            } else {
                ++summary.unchanged;
            }
        }
        if (summary.appended > 0) UpdateSelectionState();
        return summary;
    }
    void LoadCache() {
        std::vector<WebDavFileRecord> cached;
        if (cache_.Load(cached, cacheRefreshedAt_)) {
            records_ = std::move(cached);
            PopulateTable();
        }
    }
    void StartRefresh() {
        if (refreshTask_) {
            refreshTask_->RequestStop();
            refreshTask_->Wait();
            refreshTask_.reset();
        }
        refreshBusy_ = true;
        const std::uint64_t generation = ++refreshGeneration_;
        ThemedUi::SetText(directoryLabel_, L"远端目录：" + WebDavFileService::FilesDirectory(config_) + L" · 正在后台刷新...");
        if (windowUi_) {
            const ThemedUi ui = windowUi_->ui();
            ui.SetEnabled(refreshButton_, false);
            ThemedUi::SetText(refreshButton_, L"刷新中");
        }
        const HWND target = hwnd_;
        const AppConfig config = config_;
        const std::shared_ptr<std::atomic<bool>> alive = alive_;
        wchar_t simulateRefresh[8]{};
        const bool simulateRefreshForTest = QuattroTestMode() && GetEnvironmentVariableW(
            L"QUATTRO_TEST_WEBDAV_FILE_MANAGER_SIMULATE_REFRESH",
            simulateRefresh,
            static_cast<DWORD>(std::size(simulateRefresh))) > 0;
        const std::vector<WebDavFileRecord> simulatedRecords =
            simulateRefreshForTest ? records_ : std::vector<WebDavFileRecord>{};
        ScanTaskOptions scanOptions;
        scanOptions.mode = ScanExecutionMode::BackgroundParallel;
        scanOptions.maxWorkers = 4;
        scanOptions.completionCallback = [target, alive]() {
            if (alive->load()) PostMessageW(target, WM_WEBDAV_FILE_LIST_DONE, 0, 0);
        };
        refreshTask_ = ScanExecutionService::StartTyped<ListResult>(scanOptions,
            [target, config, alive, generation, simulateRefreshForTest, simulatedRecords](ScanTaskContext& context) {
            ListResult result;
            result.generation = generation;
            context.Report(ScanProgressUpdate{
                L"webdav-files", L"WebDAV 文件扫描进度", L"正在读取远端文件记录", L"正在连接 WebDAV 服务器"});
            if (simulateRefreshForTest) {
                for (int elapsed = 0; elapsed < 1500 && !context.StopRequested(); elapsed += 20) {
                    Sleep(20);
                }
                result.ok = !context.StopRequested();
                result.records = simulatedRecords;
            } else {
                WebDavFileService service(config);
                result.ok = service.Enumerate({}, [&](std::vector<WebDavFileRecord> batch) {
                    result.records.insert(result.records.end(), batch.begin(), batch.end());
                    auto message = std::make_unique<BatchResult>();
                    message->generation = generation;
                    message->records = std::move(batch);
                    BatchResult* raw = message.release();
                    if (!alive->load() || !PostMessageW(target, WM_WEBDAV_FILE_BATCH, 0, reinterpret_cast<LPARAM>(raw))) {
                        delete raw; return false;
                    }
                    return !context.StopRequested();
                }, context.StopToken(), result.error);
            }
            if (!context.StopRequested()) {
                SYSTEMTIME utc{}; GetSystemTime(&utc); wchar_t stamp[64]{};
                swprintf_s(stamp, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear, utc.wMonth, utc.wDay,
                    utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
                result.refreshedAtUtc = stamp;
            }
            context.UpdateProgress([&result](ScanProgressUpdate& value) {
                value.status = result.ok ? L"扫描完成" : L"扫描未完成";
                value.detail = result.error;
            });
            return result;
        });
    }
    void ApplyBatch(std::unique_ptr<BatchResult> batch) {
        if (!batch || batch->generation != refreshGeneration_) return;
        const MergeSummary summary = MergeRecords(batch->records);
        WriteAppLog(L"WebDAV 文件表格增量批次: 新增 " + std::to_wstring(summary.appended) +
            L"，更新 " + std::to_wstring(summary.updated) +
            L"，未变化 " + std::to_wstring(summary.unchanged));
    }
    void FinishRefresh(std::unique_ptr<ListResult> result) {
        if (!result && refreshTask_ && refreshTask_->IsFinished()) {
            refreshTask_->Wait();
            if (refreshTask_->Status() != ScanTaskStatus::Failed) {
                result = std::make_unique<ListResult>(refreshTask_->ResultCopy<ListResult>());
            }
            refreshTask_.reset();
        }
        if (!result || result->generation != refreshGeneration_) return;
        refreshBusy_ = false;
        ThemedUi::SetText(directoryLabel_, L"远端目录：" + WebDavFileService::FilesDirectory(config_));
        if (windowUi_) {
            const ThemedUi ui = windowUi_->ui();
            ui.SetEnabled(refreshButton_, true);
            ThemedUi::SetText(refreshButton_, L"刷新");
        }
        if (!result->ok) {
            if (!result->error.empty() && result->error != L"WebDAV 文件刷新已取消。") {
                if (!result->refreshedAtUtc.empty()) cacheRefreshedAt_ = result->refreshedAtUtc;
                if (!cache_.Replace(records_, cacheRefreshedAt_)) {
                    WriteAppLog(L"WebDAV 文件索引缓存保存失败: " + cache_.path().wstring());
                }
                if (windowUi_) {
                    ThemedToastOptions toast{};
                    toast.role = ThemedToastRole::Warning;
                    toast.durationMs = 6000;
                    windowUi_->ui().ShowToast(L"远端刷新未完全成功，已保存当前清单缓存。", toast);
                }
            }
            return;
        }
        std::set<std::wstring> seenIds;
        for (const auto& record : result->records) seenIds.insert(record.id);
        int removed = 0;
        for (int index = static_cast<int>(records_.size()) - 1; index >= 0; --index) {
            const auto& id = records_[static_cast<std::size_t>(index)].id;
            if (seenIds.contains(id) || deletingIds_.contains(id) || deletedTombstones_.contains(id)) continue;
            checkedIds_.erase(id);
            ThemedUi::RemoveTableRow(table_, index);
            records_.erase(records_.begin() + index);
            ++removed;
        }
        cacheRefreshedAt_ = result->refreshedAtUtc;
        if (!cache_.Replace(records_, cacheRefreshedAt_)) {
            WriteAppLog(L"WebDAV 文件索引缓存保存失败: " + cache_.path().wstring());
        }
        for (auto it = deletedTombstones_.begin(); it != deletedTombstones_.end();) {
            if (!deletingIds_.contains(*it)) it = deletedTombstones_.erase(it); else ++it;
        }
        UpdateSelectionState();
        WriteAppLog(L"WebDAV 文件表格刷新收尾: 移除 " + std::to_wstring(removed) +
            L"，最终 " + std::to_wstring(records_.size()) + L" 行");
    }
    void ApplyTestIncrementalRefresh() {
        if (!QuattroTestMode() || records_.empty()) return;
        refreshGeneration_ = 1;
        refreshBusy_ = true;
        ThemedUi::SetTableSelectedIndex(table_, 0);
        const std::intptr_t originalFirstKey = ThemedUi::TableRowKey(table_, 0);
        WebDavFileRecord updated = records_.front();
        updated.displayName = L"report-updated.zip";
        WebDavFileRecord appended;
        appended.id = std::wstring(64, L'c');
        appended.displayName = L"new-tail.txt";
        appended.absolutePath = L"C:\\Users\\demo\\Documents\\new-tail.txt";
        appended.size = 2048;
        appended.sha256 = std::wstring(64, L'3');
        appended.uploadedAtUtc = L"2026-07-21T14:36:00.000Z";
        appended.sourceLastWriteTimeUtc = L"2026-07-21T14:35:59.0000000Z";
        auto batch = std::make_unique<BatchResult>();
        batch->generation = refreshGeneration_;
        batch->records = {updated, appended};
        ApplyBatch(std::move(batch));
        auto done = std::make_unique<ListResult>();
        done->generation = refreshGeneration_;
        done->ok = true;
        done->records = {updated, appended};
        done->refreshedAtUtc = L"2026-07-21T14:36:01.000Z";
        FinishRefresh(std::move(done));
        const bool passed = records_.size() == 2 && records_[0].id == updated.id &&
            records_[1].id == appended.id && ThemedUi::TableRowCount(table_) == 2 &&
            ThemedUi::TableRowKey(table_, 0) == originalFirstKey &&
            ThemedUi::TableSelectedIndex(table_) == 0;
        SetPropW(hwnd_, L"QuattroWebDavIncrementalApplied",
            reinterpret_cast<HANDLE>(static_cast<INT_PTR>(passed ? 1 : 2)));
    }
    int Selected() const { const int index = ThemedUi::TableSelectedIndex(table_); return index >= 0 && index < static_cast<int>(records_.size()) ? index : -1; }
    std::vector<WebDavFileRecord> CheckedRecords() const {
        std::vector<WebDavFileRecord> selected;
        for (const auto& record : records_) if (checkedIds_.contains(record.id)) selected.push_back(record);
        return selected;
    }
    void UpdateSelectionState() {
        if (!windowUi_) return;
        const ThemedUi ui = windowUi_->ui();
        ThemedUi::SetText(selectionStatus_, L"已选择 " + std::to_wstring(checkedIds_.size()) +
            L" 项 · 共 " + std::to_wstring(records_.size()) + L" 项");
        const bool hasSelection = !checkedIds_.empty();
        ui.SetEnabled(uploadSelectedButton_, hasSelection);
        ui.SetEnabled(downloadSelectedButton_, hasSelection);
        ui.SetEnabled(deleteSelectedButton_, hasSelection && !deleteBusy_);
        ui.SetEnabled(clearSelectionButton_, hasSelection);
    }
    void SelectAll(bool checked) {
        checkedIds_.clear();
        if (checked) for (const auto& record : records_) checkedIds_.insert(record.id);
        PopulateTable();
    }
    void ShowToast(const std::wstring& text, ThemedToastRole role, int durationMs = 0) {
        if (!windowUi_) return;
        ThemedToastOptions options{};
        options.role = role;
        if (durationMs > 0) options.durationMs = durationMs;
        windowUi_->ui().ShowToast(text, options);
    }
    bool ValidateDownloadTarget(const WebDavFileRecord& record, std::filesystem::path& target, std::wstring& error) const {
        return WebDavFileService::ValidateDownloadTargetPath(record.absolutePath, target, error);
    }
    bool LocalUploadTarget(const WebDavFileRecord& record, std::filesystem::path& target, std::wstring& error) const {
        if (!ValidateDownloadTarget(record, target, error)) return false;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(target, ec)) {
            error = L"本地文件不存在，无法上传。";
            target.clear();
            return false;
        }
        return true;
    }
    WebDavFileBatchConfirmItem ConfirmItem(const WebDavFileRecord& record, const std::wstring& status,
        bool actionable, const std::filesystem::path& localPath = {}) const {
        WebDavFileBatchConfirmItem item;
        item.name = record.displayName.empty() ? record.id : record.displayName;
        item.sizeText = IsHealthy(record) ? FormatFileSize(record.size) : L"—";
        item.localPath = localPath.empty() ? record.absolutePath : localPath.wstring();
        item.status = status;
        item.actionable = actionable;
        return item;
    }
    bool ConfirmBatch(const std::wstring& title, const std::wstring& intro, const std::wstring& confirmText,
        const std::vector<WebDavFileBatchConfirmItem>& items, bool danger = false) {
        return WebDavFileBatchConfirmDialog(hwnd_, instance_, theme_, title, intro, confirmText, items, danger).Run();
    }
    bool CanUploadLocal(const WebDavFileRecord& record) const {
        if (!IsHealthy(record)) return false;
        std::filesystem::path target;
        std::wstring error;
        return LocalUploadTarget(record, target, error);
    }
    void UploadRecords(const std::vector<WebDavFileRecord>& selected) {
        if (selected.empty()) return;
        std::vector<std::filesystem::path> uploadPaths;
        std::vector<WebDavFileBatchConfirmItem> items;
        items.reserve(selected.size());
        for (const auto& record : selected) {
            std::filesystem::path target;
            std::wstring error;
            if (!IsHealthy(record)) {
                items.push_back(ConfirmItem(record, L"异常记录无法定位本地文件，将跳过。", false));
            } else if (LocalUploadTarget(record, target, error)) {
                uploadPaths.push_back(target);
                items.push_back(ConfirmItem(record, L"将重新上传本地文件并覆盖远端内容。", true, target));
            } else {
                items.push_back(ConfirmItem(record, error.empty() ? L"本地文件不存在，无法上传。" : error, false));
            }
        }
        if (uploadPaths.empty()) {
            ShowToast(L"选中的文件没有可上传的本地文件。", ThemedToastRole::Warning, 5000);
            return;
        }
        if (!ConfirmBatch(L"确认上传 WebDAV 文件",
                L"请确认要上传以下本地文件。上传会覆盖对应的远端记录和内容。",
                L"确认上传", items)) {
            return;
        }
        std::wstring error;
        if (!WebDavTransferCoordinator::SubmitUploads(uploadPaths, error)) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, L"上传失败", MB_OK | MB_ICONWARNING);
            return;
        }
        ShowToast(L"已加入 WebDAV 上传队列。", ThemedToastRole::Success);
    }
    void Upload(int index = -1) {
        if (index < 0) index = Selected();
        if (index < 0 || index >= static_cast<int>(records_.size())) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"请选择一个文件。", L"WebDAV 文件管理", MB_OK | MB_ICONWARNING);
            return;
        }
        UploadRecords({records_[static_cast<std::size_t>(index)]});
    }
    void UploadSelected() {
        UploadRecords(CheckedRecords());
    }
    void DownloadRecords(const std::vector<WebDavFileRecord>& selected) {
        if (selected.empty()) return;
        std::vector<WebDavFileRecord> downloadable;
        std::vector<WebDavFileRecord> overwriteRecords;
        std::vector<WebDavFileBatchConfirmItem> items;
        std::vector<WebDavFileBatchConfirmItem> overwriteItems;
        items.reserve(selected.size());
        for (const auto& record : selected) {
            if (!IsHealthy(record)) {
                items.push_back(ConfirmItem(record, L"异常记录无法下载，将跳过。", false));
                continue;
            }
            if (!record.contentReady || ToLower(record.uploadState) != L"complete") {
                items.push_back(ConfirmItem(record, L"远端文件尚未上传完成，将跳过。", false));
                continue;
            }
            std::filesystem::path target;
            std::wstring error;
            if (!ValidateDownloadTarget(record, target, error)) {
                items.push_back(ConfirmItem(record, error.empty() ? L"文件保存路径无效，将跳过。" : error, false));
                continue;
            }
            const bool overwrite = FileExists(target);
            downloadable.push_back(record);
            items.push_back(ConfirmItem(record, overwrite ? L"将下载并覆盖本地文件。" : L"将下载到本地路径。", true, target));
            if (overwrite) {
                overwriteRecords.push_back(record);
                overwriteItems.push_back(ConfirmItem(record, L"本地文件已存在，确认后会被远端内容覆盖。", true, target));
            }
        }
        if (downloadable.empty()) {
            ShowToast(L"选中的文件没有可下载的有效记录。", ThemedToastRole::Warning, 5000);
            return;
        }
        if (!ConfirmBatch(L"确认下载 WebDAV 文件",
                L"请确认要下载以下远端文件。",
                L"确认下载", items)) {
            return;
        }
        if (!overwriteItems.empty() &&
            !ConfirmBatch(L"确认覆盖本地文件",
                L"以下本地文件已经存在。继续下载会使用远端内容覆盖这些文件。",
                L"覆盖并下载", overwriteItems, true)) {
            return;
        }
        std::wstring error;
        if (!WebDavTransferCoordinator::SubmitDownloads(downloadable, error)) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, L"批量下载失败", MB_OK | MB_ICONWARNING);
            return;
        }
        ShowToast(L"已加入 WebDAV 下载队列。", ThemedToastRole::Success);
    }
    void Download(int index = -1) {
        if (index < 0) index = Selected();
        if (index < 0 || index >= static_cast<int>(records_.size())) { ShowThemedMessageBox(hwnd_, instance_, theme_, L"请选择一个文件。", L"WebDAV 文件管理", MB_OK | MB_ICONWARNING); return; }
        DownloadRecords({records_[static_cast<std::size_t>(index)]});
    }
    void DownloadSelected() {
        DownloadRecords(CheckedRecords());
    }
    void DeleteSelected(int index = -1) {
        if (index < 0) index = Selected();
        if (index < 0 || index >= static_cast<int>(records_.size())) { ShowThemedMessageBox(hwnd_, instance_, theme_, L"请选择一个文件。", L"WebDAV 文件管理", MB_OK | MB_ICONWARNING); return; }
        DeleteRecords({records_[static_cast<std::size_t>(index)]});
    }
    static std::wstring DeletePhaseText(WebDavFileDeletePhase phase) {
        switch (phase) {
        case WebDavFileDeletePhase::DeletingContent: return L"正在删除文件内容";
        case WebDavFileDeletePhase::DeletingMetadata: return L"正在删除元数据";
        case WebDavFileDeletePhase::DeletingDirectory: return L"正在删除远端目录";
        }
        return L"正在删除";
    }
    void ShowDeleteProgress(const std::shared_ptr<DeleteTaskState>& state) {
        ThemedTaskProgressDialogOptions options{};
        options.owner = hwnd_;
        options.instance = instance_;
        options.theme = theme_;
        options.icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        options.className = L"QuattroWebDavDeleteProgress_" + std::to_wstring(GetCurrentProcessId()) +
            L"_" + std::to_wstring(GetTickCount64());
        options.title = L"删除 WebDAV 文件";
        options.initialStatus = L"正在准备删除…";
        options.initialDetail = L"正在检查远端文件记录。";
        options.stopText = L"停止";
        options.closeText = L"关闭";
        options.readSnapshot = [state]() {
            ThemedTaskProgressSnapshot snapshot;
            std::lock_guard lock(state->mutex);
            const std::size_t totalSteps = std::max<std::size_t>(1, state->totalFiles * 3);
            snapshot.title = L"删除 WebDAV 文件";
            snapshot.value = static_cast<double>(state->completedSteps) / static_cast<double>(totalSteps);
            snapshot.indeterminate = false;
            snapshot.finished = state->finished;
            snapshot.stopRequested = state->stopRequested.load();
            if (state->finished) {
                snapshot.role = state->failed > 0 ? ThemedStatusRole::Warning : ThemedStatusRole::Success;
                snapshot.status = state->stopped ? L"删除已停止" : L"删除完成";
                snapshot.detail = L"成功 " + std::to_wstring(state->succeeded) + L" 项，失败 " +
                    std::to_wstring(state->failed) + L" 项。";
                if (!state->lastError.empty()) snapshot.detail += L" 最后错误：" + state->lastError;
            } else {
                snapshot.role = state->stopRequested.load() ? ThemedStatusRole::Warning : ThemedStatusRole::Info;
                snapshot.status = state->currentFile == 0
                    ? L"正在准备删除…"
                    : L"正在删除 " + std::to_wstring(state->currentFile) + L" / " +
                        std::to_wstring(state->totalFiles);
                snapshot.detail = state->phaseText;
                if (!state->currentName.empty()) snapshot.detail += L"：" + state->currentName;
                if (state->stopRequested.load()) snapshot.detail = L"将在当前文件完成后停止。";
            }
            return snapshot;
        };
        options.requestStop = [state]() { state->stopRequested.store(true); };
        deleteProgressDialog_ = std::make_unique<ThemedTaskProgressDialog>(std::move(options));
        deleteProgressDialog_->Show();
    }
    void ShowTestDeleteProgress() {
        if (!QuattroTestMode()) return;
        auto state = std::make_shared<DeleteTaskState>();
        state->totalFiles = 5;
        state->currentFile = 2;
        state->completedSteps = 4;
        state->currentName = L"archive-report.zip";
        state->phaseText = L"正在删除元数据";
        deleteTaskState_ = state;
        ShowDeleteProgress(state);
    }
    void DeleteRecords(std::vector<WebDavFileRecord> records) {
        if (records.empty() || deleteBusy_) return;
        std::vector<WebDavFileBatchConfirmItem> items;
        items.reserve(records.size());
        for (const auto& record : records) {
            items.push_back(ConfirmItem(record, L"将删除远端内容、Meta 和记录目录。", true));
        }
        if (!ConfirmBatch(L"确认删除 WebDAV 文件",
                L"请确认要删除以下远端文件。此操作不会删除对应的本地文件。",
                L"确认删除", items, true)) {
            return;
        }
        deleteBusy_ = true;
        for (const auto& record : records) { deletingIds_.insert(record.id); deletedTombstones_.insert(record.id); }
        for (const auto& record : records) {
            const int index = RecordIndex(record.id);
            if (index >= 0) ThemedUi::UpdateTableRow(table_, index, TableRow(records_[static_cast<std::size_t>(index)]));
        }
        UpdateSelectionState();
        auto taskState = std::make_shared<DeleteTaskState>();
        taskState->totalFiles = records.size();
        deleteTaskState_ = taskState;
        ShowDeleteProgress(taskState);
        const HWND target = hwnd_; const AppConfig config = config_; const auto alive = alive_;
        std::thread([target, config, records = std::move(records), alive, taskState]() mutable {
            auto result = std::make_unique<DeleteResult>();
            WebDavFileService service(config); WebDavFileIndexCache cache(config);
            for (std::size_t recordIndex = 0; recordIndex < records.size(); ++recordIndex) {
                if (taskState->stopRequested.load()) {
                    result->stopped = true;
                    for (std::size_t pending = recordIndex; pending < records.size(); ++pending) {
                        result->notStartedIds.push_back(records[pending].id);
                    }
                    break;
                }
                const auto& record = records[recordIndex];
                {
                    std::lock_guard lock(taskState->mutex);
                    taskState->currentFile = recordIndex + 1;
                    taskState->currentName = record.displayName;
                    taskState->phaseText = L"正在准备远端删除";
                }
                std::wstring error;
                const bool ok = service.Delete(record, error, [taskState](WebDavFileDeletePhase phase, bool completed) {
                    std::lock_guard lock(taskState->mutex);
                    taskState->phaseText = DeletePhaseText(phase);
                    if (completed) ++taskState->completedSteps;
                });
                if (ok) {
                    result->succeededIds.push_back(record.id);
                    cache.Remove(record.id);
                } else {
                    result->failedIds.push_back(record.id);
                    result->lastError = error;
                }
                {
                    std::lock_guard lock(taskState->mutex);
                    taskState->completedSteps = (recordIndex + 1) * 3;
                    if (ok) ++taskState->succeeded; else { ++taskState->failed; taskState->lastError = error; }
                }
                auto item = std::make_unique<DeleteItemResult>();
                item->id = record.id;
                item->ok = ok;
                item->error = error;
                DeleteItemResult* itemRaw = item.release();
                if (!alive->load() || !PostMessageW(target, WM_WEBDAV_FILE_DELETE_ITEM_DONE, 0,
                        reinterpret_cast<LPARAM>(itemRaw))) {
                    delete itemRaw;
                }
            }
            {
                std::lock_guard lock(taskState->mutex);
                taskState->finished = true;
                taskState->stopped = result->stopped;
                if (!result->stopped) taskState->completedSteps = taskState->totalFiles * 3;
            }
            DeleteResult* raw = result.release();
            if (!alive->load() || !PostMessageW(target, WM_WEBDAV_FILE_DELETE_DONE, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
        }).detach();
    }
    void DeleteChecked() { DeleteRecords(CheckedRecords()); }
    void FinishDeleteItem(std::unique_ptr<DeleteItemResult> result) {
        if (!result) return;
        const int index = RecordIndex(result->id);
        if (result->ok) {
            if (index >= 0) {
                ThemedUi::RemoveTableRow(table_, index);
                records_.erase(records_.begin() + index);
            }
            checkedIds_.erase(result->id);
            deletingIds_.erase(result->id);
        } else {
            deletingIds_.erase(result->id);
            deletedTombstones_.erase(result->id);
            if (index >= 0) ThemedUi::UpdateTableRow(table_, index, TableRow(records_[static_cast<std::size_t>(index)]));
        }
        UpdateSelectionState();
    }
    void FinishDelete(std::unique_ptr<DeleteResult> result) {
        deleteBusy_ = false;
        if (!result) return;
        for (const auto& id : result->notStartedIds) {
            deletingIds_.erase(id);
            deletedTombstones_.erase(id);
            const int index = RecordIndex(id);
            if (index >= 0) ThemedUi::UpdateTableRow(table_, index, TableRow(records_[static_cast<std::size_t>(index)]));
        }
        UpdateSelectionState();
    }
    void ShowTransferQueue() {
        std::wstring error;
        if (!WebDavTransferCoordinator::ShowQueue(error)) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, L"传输队列", MB_OK | MB_ICONWARNING);
        }
    }
    void ShowDetails(int index) {
        if (index < 0 || index >= static_cast<int>(records_.size())) return;
        const auto& record = records_[static_cast<std::size_t>(index)];
        const std::wstring remoteRecordPath = WebDavClient::CombineRemotePath(
            WebDavFileService::FilesDirectory(config_), record.id);
        WebDavFileDetailsDialog(hwnd_, instance_, theme_, record, remoteRecordPath).Run();
    }
    void OpenContainingLocation(int index) {
        if (index < 0 || index >= static_cast<int>(records_.size())) return;
        const auto& record = records_[static_cast<std::size_t>(index)];
        std::filesystem::path target;
        std::wstring error;
        if (!ValidateDownloadTarget(record, target, error)) {
            ShowToast(error.empty() ? L"文件路径无效。" : error, ThemedToastRole::Warning, 5000);
            return;
        }
        if (!ShellItemService::OpenFileSystemContainingLocation(hwnd_, target, error)) {
            ShowToast(error.empty() ? L"无法打开文件所在位置。" : error, ThemedToastRole::Warning, 5000);
        }
    }
    int RowFromScreenPoint(POINT screenPoint) const {
        return ThemedUi::TableScreenHitTest(table_, screenPoint);
    }
    POINT ActionMenuAnchor(int row) const {
        RECT cell{};
        if (ThemedUi::TableCellScreenRect(table_, row, 4, cell)) {
            return POINT{cell.right, cell.bottom};
        }
        RECT window{};
        GetWindowRect(hwnd_, &window);
        return POINT{window.left, window.top};
    }
    void ShowFileActionMenu(int row, POINT anchor) {
        if (row < 0 || row >= static_cast<int>(records_.size())) return;
        ThemedUi::SetTableSelectedIndex(table_, row);
        const auto& record = records_[static_cast<std::size_t>(row)];
        const bool healthy = IsHealthy(record);
        const bool canUpload = CanUploadLocal(record);
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING |
            (healthy ? 0 : MF_GRAYED),
            ID_FILE_ACTION_DOWNLOAD, L"下载");
        AppendMenuW(menu, MF_STRING | (canUpload ? 0 : MF_GRAYED),
            ID_FILE_ACTION_UPLOAD, L"上传");
        AppendMenuW(menu, MF_STRING |
            (healthy ? 0 : MF_GRAYED),
            ID_FILE_ACTION_OPEN_LOCATION, L"打开文件所在位置");
        if (QuattroTestMode()) {
            SetPropW(hwnd_, L"QuattroWebDavFileActionMenuHasOpenLocation",
                reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
            SetPropW(hwnd_, L"QuattroWebDavFileActionMenuHasUpload",
                reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
        }
        AppendMenuW(menu, MF_STRING, ID_FILE_ACTION_DETAILS, L"查看详情");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_FILE_ACTION_DELETE, L"删除");
        ThemedPopupMenuOptions options{};
        options.source = ThemedPopupMenuSource::ClientArea;
        options.horizontalAlign = ThemedPopupMenuHorizontalAlign::Right;
        options.returnCommand = true;
        const UINT command = ThemedUi::ShowPopupMenu(hwnd_, menu, anchor, options).command;
        DestroyMenu(menu);
        if (command == ID_FILE_ACTION_DOWNLOAD) Download(row);
        else if (command == ID_FILE_ACTION_UPLOAD) Upload(row);
        else if (command == ID_FILE_ACTION_OPEN_LOCATION) OpenContainingLocation(row);
        else if (command == ID_FILE_ACTION_DETAILS) ShowDetails(row);
        else if (command == ID_FILE_ACTION_DELETE) DeleteSelected(row);
    }
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT common = 0; if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, common)) return common;
        switch (message) {
        case WM_CREATE: {
            RECT client{}; GetClientRect(hwnd_, &client); windowUi_ = std::make_unique<ThemedWindowUi>(instance_, owner_, hwnd_, theme_, DialogLayoutKind::Compact, client.right, client.bottom);
            windowUi_->SetDpiChangedCallback([this](UINT) { LayoutControls(); });
            const auto& ui = windowUi_->ui(); const auto& layout = ui.layout();
            const int buttonHeight = ui.buttonHeight();
            const int refreshWidth = ui.buttonWidth(L"刷新", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const int queueWidth = ui.buttonWidth(L"队列", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const int topGroupWidth = refreshWidth + layout.controlGapX + queueWidth;
            const int topGroupX = ui.contentLeft() + ui.contentWidth() - topGroupWidth;
            directoryLabel_ = ui.SelectableLabel(L"远端目录：" + WebDavFileService::FilesDirectory(config_), ui.contentLeft(), ui.contentTop(), topGroupX - ui.contentLeft() - layout.controlGapX);
            refreshButton_ = ui.Button(ID_WEBDAV_FILE_REFRESH, L"刷新", topGroupX, ui.contentTop(), ThemedButtonRole::Normal,
                ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, refreshWidth);
            transferQueueButton_ = ui.Button(ID_WEBDAV_FILE_TRANSFER_QUEUE, L"队列", topGroupX + refreshWidth + layout.controlGapX,
                ui.contentTop(), ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, queueWidth);
            ui.SetTooltip(transferQueueButton_, L"打开 WebDAV 传输队列");

            const int actionY = ui.nextRowY(ui.contentTop(), buttonHeight);
            const int selectAllWidth = ui.buttonWidth(L"全选", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const int clearWidth = ui.buttonWidth(L"清除选择", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const int uploadWidth = ui.buttonWidth(L"上传所选", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const int downloadWidth = ui.buttonWidth(L"下载所选", ThemedButtonRole::Primary, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const int deleteWidth = ui.buttonWidth(L"删除所选", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            selectAllButton_ = ui.Button(ID_WEBDAV_FILE_SELECT_ALL, L"全选", ui.contentLeft(), actionY,
                ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, selectAllWidth);
            clearSelectionButton_ = ui.Button(ID_WEBDAV_FILE_CLEAR_SELECTION, L"清除选择",
                ui.contentLeft() + selectAllWidth + layout.controlGapX, actionY,
                ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, clearWidth);
            const int selectionX = ui.contentLeft() + selectAllWidth + layout.controlGapX + clearWidth + layout.controlGapX;
            const int rightActionsWidth = uploadWidth + layout.controlGapX + downloadWidth + layout.controlGapX + deleteWidth;
            const int rightActionsX = ui.contentLeft() + ui.contentWidth() - rightActionsWidth;
            selectionStatus_ = ui.SelectableStatusText(L"已选择 0 项", selectionX, actionY, std::max(1, rightActionsX - selectionX - layout.controlGapX),
                ThemedStatusTextOptions{ThemedStatusRole::Normal, ThemedTextAlign::Start});
            uploadSelectedButton_ = ui.Button(ID_WEBDAV_FILE_UPLOAD_SELECTED, L"上传所选", rightActionsX, actionY,
                ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, uploadWidth);
            downloadSelectedButton_ = ui.Button(ID_WEBDAV_FILE_DOWNLOAD_SELECTED, L"下载所选", rightActionsX + uploadWidth + layout.controlGapX, actionY,
                ThemedButtonRole::Primary, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, downloadWidth);
            deleteSelectedButton_ = ui.Button(ID_WEBDAV_FILE_DELETE_SELECTED, L"删除所选",
                rightActionsX + uploadWidth + layout.controlGapX + downloadWidth + layout.controlGapX, actionY,
                ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Fixed, deleteWidth);

            const int top = ui.nextRowY(actionY, buttonHeight);
            RECT frame{ui.contentLeft(), top, ui.contentLeft()+ui.contentWidth(), ui.clientHeight()-layout.contentInsetY};
            const int fileSizeWidth = ui.tableColumnWidth({L"大小", L"999.99 GB"});
            const int uploadTimeWidth = ui.tableColumnWidth({L"上传时间", L"2000-00-00 00:00:00"});
            const int statusWidth = ui.tableColumnWidth({L"本地状态", L"本地不存在", L"本地较新"});
            const int actionWidth = ui.buttonWidth(
                L"…", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text) + ui.denseGap();
            ThemedTableOptions tableOptions{}; tableOptions.checkable = true; tableOptions.allowColumnResize = true;
            tableOptions.reserveScrollBarGutter = true;
            table_ = ui.Table(430, frame, {
                {L"name", L"文件名", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
                {L"size", L"大小", ThemedTableColumnAlign::End, ThemedTableColumnWidth::Fixed, fileSizeWidth},
                {L"time", L"上传时间", ThemedTableColumnAlign::End, ThemedTableColumnWidth::Fixed, uploadTimeWidth},
                {L"status", L"本地状态", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, statusWidth},
                {L"action", L"操作", ThemedTableColumnAlign::Center, ThemedTableColumnWidth::Fixed, actionWidth},
            }, tableOptions);
            ThemedTooltipOptions rowTooltipOptions{};
            rowTooltipOptions.placement = ThemedTooltipPlacement::Cursor;
            ui.SetTableRowTooltip(table_, [this](int row, std::intptr_t) {
                if (row < 0 || row >= static_cast<int>(records_.size())) return std::wstring{};
                return RowTooltipText(records_[static_cast<std::size_t>(row)]);
            }, rowTooltipOptions);
            LayoutControls();
            LoadCache();
            wchar_t showTestDetails[8]{};
            const bool showDetailsForTest = QuattroTestMode() && GetEnvironmentVariableW(
                L"QUATTRO_TEST_WEBDAV_FILE_DETAILS", showTestDetails,
                static_cast<DWORD>(std::size(showTestDetails))) > 0;
            if (showDetailsForTest) {
                WebDavFileRecord sample;
                sample.id = std::wstring(64, L'a');
                sample.displayName = L"EntryFlow.md";
                sample.absolutePath = L"D:\\ma\\xclient\\assets\\scripts\\game\\module\\kingshotbattle\\docs\\agent\\entryflow.md";
                sample.size = 5 * 1024;
                sample.sha256 = L"8df94ef5e2ab2d8ee2f6fcf011c6391b41e69d5d7a2e966ac4dd094fcf533c2e";
                sample.uploadedAtUtc = L"2026-07-21T08:31:35.872Z";
                sample.sourceLastWriteTimeUtc = L"2026-07-21T08:31:35.8720000Z";
                records_ = {std::move(sample)};
                PopulateTable();
                PostMessageW(hwnd_, WM_WEBDAV_FILE_SHOW_TEST_DETAILS, 0, 0);
            }
            UpdateSelectionState();
            wchar_t skipRefresh[8]{};
            const bool skipRefreshForTest = QuattroTestMode() && GetEnvironmentVariableW(
                L"QUATTRO_TEST_WEBDAV_FILE_MANAGER_SKIP_REFRESH", skipRefresh,
                static_cast<DWORD>(std::size(skipRefresh))) > 0;
            if (!skipRefreshForTest) PostMessageW(hwnd_, WM_WEBDAV_FILE_REFRESH_REQUEST, 0, 0);
            wchar_t incrementalTest[8]{};
            if (QuattroTestMode() && GetEnvironmentVariableW(
                    L"QUATTRO_TEST_WEBDAV_FILE_MANAGER_INCREMENTAL", incrementalTest,
                    static_cast<DWORD>(std::size(incrementalTest))) > 0) {
                PostMessageW(hwnd_, WM_WEBDAV_FILE_APPLY_TEST_INCREMENTAL, 0, 0);
            }
            wchar_t deleteProgressTest[8]{};
            if (QuattroTestMode() && GetEnvironmentVariableW(
                    L"QUATTRO_TEST_WEBDAV_DELETE_PROGRESS", deleteProgressTest,
                    static_cast<DWORD>(std::size(deleteProgressTest))) > 0) {
                PostMessageW(hwnd_, WM_WEBDAV_FILE_SHOW_TEST_DELETE_PROGRESS, 0, 0);
            }
            return 0; }
        case WM_SIZE: if (windowUi_) LayoutControls(); return 0;
        case WM_PAINT: { PAINTSTRUCT ps{}; HDC dc=BeginPaint(hwnd_,&ps); windowUi_->FillBackground(dc); windowUi_->DrawRegisteredEditFrames(dc); windowUi_->DrawRegisteredTableFrames(dc); EndPaint(hwnd_,&ps); return 0; }
        case WM_WEBDAV_FILE_REFRESH_REQUEST: StartRefresh(); return 0;
        case WM_WEBDAV_FILE_BATCH: ApplyBatch(std::unique_ptr<BatchResult>(reinterpret_cast<BatchResult*>(lParam))); return 0;
        case WM_WEBDAV_FILE_LIST_DONE: FinishRefresh(std::unique_ptr<ListResult>(reinterpret_cast<ListResult*>(lParam))); return 0;
        case WM_WEBDAV_FILE_DELETE_ITEM_DONE: FinishDeleteItem(std::unique_ptr<DeleteItemResult>(reinterpret_cast<DeleteItemResult*>(lParam))); return 0;
        case WM_WEBDAV_FILE_DELETE_DONE: FinishDelete(std::unique_ptr<DeleteResult>(reinterpret_cast<DeleteResult*>(lParam))); return 0;
        case WM_WEBDAV_FILE_SHOW_TEST_DETAILS: ShowDetails(0); return 0;
        case WM_WEBDAV_FILE_APPLY_TEST_INCREMENTAL: ApplyTestIncrementalRefresh(); return 0;
        case WM_WEBDAV_FILE_SHOW_TEST_DELETE_PROGRESS: ShowTestDeleteProgress(); return 0;
        case WM_NOTIFY: {
            ThemedTableEvent event{};
            if (ThemedUi::DecodeTableEvent(table_, lParam, event)) {
                if (event.kind == ThemedTableEventKind::CheckChanged && event.row >= 0 && event.row < static_cast<int>(records_.size())) {
                    const auto& id = records_[static_cast<std::size_t>(event.row)].id;
                    if (event.checked) checkedIds_.insert(id); else checkedIds_.erase(id);
                    UpdateSelectionState();
                    return 0;
                }
                if (event.kind == ThemedTableEventKind::ActionInvoked && event.actionId == ID_FILE_ROW_MENU) {
                    ShowFileActionMenu(event.row, ActionMenuAnchor(event.row));
                    return 0;
                }
                if (event.kind == ThemedTableEventKind::Activated && event.column != 4) {
                    Download(event.row);
                    return 0;
                }
            }
            return 0;
        }
        case WM_CONTEXTMENU: {
            if (reinterpret_cast<HWND>(wParam) != table_) return DefWindowProcW(hwnd_, message, wParam, lParam);
            POINT anchor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int row = -1;
            if (anchor.x == -1 && anchor.y == -1) {
                row = Selected();
                if (row >= 0) anchor = ActionMenuAnchor(row);
            } else {
                row = RowFromScreenPoint(anchor);
            }
            ShowFileActionMenu(row, anchor);
            return 0;
        }
        case WM_COMMAND: if (LOWORD(wParam)==ID_WEBDAV_FILE_REFRESH) { StartRefresh(); return 0; } if (LOWORD(wParam)==ID_WEBDAV_FILE_TRANSFER_QUEUE) { ShowTransferQueue(); return 0; } if (LOWORD(wParam)==ID_WEBDAV_FILE_SELECT_ALL) { SelectAll(true); return 0; } if (LOWORD(wParam)==ID_WEBDAV_FILE_CLEAR_SELECTION) { SelectAll(false); return 0; } if (LOWORD(wParam)==ID_WEBDAV_FILE_UPLOAD_SELECTED) { UploadSelected(); return 0; } if (LOWORD(wParam)==ID_WEBDAV_FILE_DOWNLOAD_SELECTED) { DownloadSelected(); return 0; } if (LOWORD(wParam)==ID_WEBDAV_FILE_DELETE_SELECTED) { DeleteChecked(); return 0; } if (LOWORD(wParam)==ID_WEBDAV_FILE_DOWNLOAD) { Download(); return 0; } if (LOWORD(wParam)==ID_WEBDAV_FILE_DELETE) { DeleteSelected(); return 0; } return 0;
        case WM_CLOSE: if (refreshTask_) refreshTask_->RequestStop(); done_=true; DestroyWindow(hwnd_); return 0;
        case WM_NCDESTROY: if (refreshTask_) refreshTask_->RequestStop(); alive_->store(false); RemovePropW(hwnd_, L"QuattroWebDavIncrementalApplied"); RemovePropW(hwnd_, L"QuattroWebDavFileActionMenuHasOpenLocation"); RemovePropW(hwnd_, L"QuattroWebDavFileActionMenuHasUpload"); done_=true; hwnd_=nullptr; return 0;
        default: return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }
    HWND owner_{}; HINSTANCE instance_{}; HWND hwnd_{}; HWND table_{}; HWND directoryLabel_{};
    HWND refreshButton_{}; HWND transferQueueButton_{}; HWND selectAllButton_{}; HWND clearSelectionButton_{};
    HWND selectionStatus_{}; HWND uploadSelectedButton_{}; HWND downloadSelectedButton_{}; HWND deleteSelectedButton_{};
    const Theme& theme_; AppConfig config_; WebDavFileIndexCache cache_{config_}; std::wstring cacheRefreshedAt_;
    std::vector<WebDavFileRecord> records_; std::map<std::wstring, std::intptr_t> rowKeys_; std::intptr_t nextRowKey_ = 1;
    std::set<std::wstring> checkedIds_; std::set<std::wstring> deletingIds_;
    std::set<std::wstring> deletedTombstones_; std::unique_ptr<ThemedWindowUi> windowUi_;
    std::shared_ptr<DeleteTaskState> deleteTaskState_;
    std::unique_ptr<ThemedTaskProgressDialog> deleteProgressDialog_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<ScanTaskHandle> refreshTask_; std::uint64_t refreshGeneration_=0; bool refreshBusy_=false; bool deleteBusy_=false; bool done_=false;
};

class SettingsDialog {
public:
    SettingsDialog(
        HWND owner,
        HINSTANCE instance,
        AppConfig& config,
        const Theme& theme,
        std::filesystem::path appDirectory,
        std::filesystem::path httpRootBaseDirectory,
        LocalHttpServerService* httpServer,
        bool mainHotKeyRegistered,
        bool processLocatorHotKeyRegistered,
        bool copySelectedPathsHotKeyRegistered,
        SettingsApplyCallback applyCallback,
        SettingsResetContextMenuCallback resetContextMenuCallback,
        std::vector<Link> contextMenuLinks,
        SettingsContextMenuRefreshRunner contextMenuRefreshRunner,
        SettingsContextMenuRefreshApplyCallback contextMenuRefreshApplyCallback,
        SettingsContextMenuProviderIconRunner contextMenuProviderIconRunner,
        SettingsCopyPathContextMenuCallback copyPathContextMenuCallback,
        SettingsWebDavUploadContextMenuCallback webDavUploadContextMenuCallback)
        : owner_(owner),
          instance_(instance),
          config_(config),
          draft_(config),
          theme_(theme),
          appDirectory_(std::move(appDirectory)),
          httpRootBaseDirectory_(std::move(httpRootBaseDirectory)),
          httpServer_(httpServer),
          mainHotKeyRegistered_(mainHotKeyRegistered),
          processLocatorHotKeyRegistered_(processLocatorHotKeyRegistered),
          copySelectedPathsHotKeyRegistered_(copySelectedPathsHotKeyRegistered),
          applyCallback_(std::move(applyCallback)),
          resetContextMenuCallback_(std::move(resetContextMenuCallback)),
          contextMenuLinks_(std::move(contextMenuLinks)),
          contextMenuRefreshRunner_(std::move(contextMenuRefreshRunner)),
          contextMenuRefreshApplyCallback_(std::move(contextMenuRefreshApplyCallback)),
          contextMenuProviderIconRunner_(std::move(contextMenuProviderIconRunner)),
          copyPathContextMenuCallback_(std::move(copyPathContextMenuCallback)),
          webDavUploadContextMenuCallback_(std::move(webDavUploadContextMenuCallback)) {}

    ~SettingsDialog() {
        AbandonContextMenuIconLoad();
        DestroyContextMenuImageList();
    }

    bool Run() {
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        ThemedWindowCreateOptions options = ThemedWindowUi::DialogOptions(
            instance_, owner_, L"QuattroSettingsDialog", L"设置", SettingsDialog::Proc, this, icon, icon);
        options.clientWidth = 656;
        options.clientHeight = 441;
        options.placement = ThemedWindowPlacement::OffsetOwner;
        options.offsetX = 60;
        options.offsetY = 70;
        std::wstring error;
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options, &error);
        if (!hwnd_) {
            WriteAppLog(L"设置窗口创建失败: " + error);
            return false;
        }
        if (owner_) {
            SetPropW(owner_, kSettingsDialogHwndProp, hwnd_);
        }
        windowUi_->ShowModal();
        UpdateWindow(hwnd_);
        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (windowUi_) {
            windowUi_->RestoreModalOwner();
        }
        if (contextMenuRefreshTask_) {
            contextMenuRefreshTask_->RequestStop();
            contextMenuRefreshTask_->Wait();
            contextMenuRefreshTask_.reset();
        }
        if (contextMenuRefreshProgressDialog_) contextMenuRefreshProgressDialog_->Close();
        if (owner_ && reinterpret_cast<HWND>(GetPropW(owner_, kSettingsDialogHwndProp)) == hwnd_) {
            RemovePropW(owner_, kSettingsDialogHwndProp);
        }
        return accepted_;
    }

    bool webDavDataImported() const {
        return importedData_;
    }

private:
    enum TabIndex {
        TabDisplay = 0,
        TabBehavior = 1,
        TabContextMenu = 2,
        TabInteraction = 3,
        TabHotKeys = 4,
        TabLinks = 5,
        TabWebDav = 6,
        TabHttp = 7,
        TabBackup = 8,
        TabCount = 9,
    };

    struct TabChild {
        HWND hwnd = nullptr;
        int tab = 0;
    };

    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        SettingsDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<SettingsDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void AddTabChild(HWND hwnd, int tab) {
        if (!hwnd) {
            return;
        }
        tabChildren_.push_back(TabChild{hwnd, tab});
    }

    int ContentY(int y) const {
        return y + tabContentOffsetY_;
    }

    HWND Label(int tab, const wchar_t* text, int x, int y, int width = 110, ThemedLabelOptions options = {}) {
        HWND hwnd = MakeUi().SelectableLabel(text, x, ContentY(y), width, options);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    HWND StatusBadge(int tab, const wchar_t* text, int x, int y, int width, ThemedStatusRole role) {
        HWND hwnd = MakeUi().StatusBadge(text, x, ContentY(y), width, role);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    HWND StatusText(int tab, const wchar_t* text, int x, int y, int width, ThemedStatusRole role) {
        ThemedStatusTextOptions options{};
        options.role = role;
        options.align = ThemedTextAlign::Start;
        HWND hwnd = MakeUi().SelectableStatusText(text, x, ContentY(y), width, options);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    HWND CheckBox(int tab, int id, const wchar_t* text, int x, int y, bool checked, int width = 210) {
        ThemedCheckBoxOptions options{};
        options.checked = checked;
        HWND hwnd = MakeUi().CheckBox(id, text, x, ContentY(y), width, options);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    HWND Toggle(int tab, int id, const wchar_t* text, int x, int y, bool checked, int width) {
        ThemedToggleOptions options{};
        options.checked = checked;
        HWND hwnd = MakeUi().Toggle(id, text, x, ContentY(y), width, options);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    ThemedUi MakeUi() const {
        return windowUi_->ui();
    }

    HWND Button(int tab, int id, const wchar_t* text, int x, int y, int width, ThemedButtonRole role = ThemedButtonRole::Normal) {
        const ThemedUi ui = MakeUi();
        const int normalWidth = ui.buttonWidth(text, role, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
        HWND hwnd = ui.Button(
            id,
            text,
            x,
            ContentY(y),
            role,
            ThemedButtonSize::Normal,
            ThemedButtonWidthMode::Fixed,
            std::max(width, normalWidth));
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    HWND FramedEdit(int tab, int id, int x, int y, int width, const std::wstring& text, ThemedEditOptions options = {}) {
        const ThemedUi ui = MakeUi();
        const int fieldHeight = ui.editHeight();
        y = ContentY(y);
        const RECT frame{x, y, x + width, y + fieldHeight};
        HWND hwnd = ui.Edit(id, frame, text, options);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    HWND FramedStatic(int tab, int x, int y, int width, const std::wstring& text) {
        const ThemedUi ui = MakeUi();
        const int fieldHeight = ui.editHeight();
        y = ContentY(y);
        const RECT frame{x, y, x + width, y + fieldHeight};
        HWND hwnd = ui.FramedStatic(text, frame);
        AddTabChild(hwnd, tab);
        return hwnd;
    }

    HWND NumberEdit(int tab, int id, int x, int y, int width, int value) {
        ThemedEditOptions options{};
        options.content = ThemedEditContent::Integer;
        return FramedEdit(tab, id, x, y, width, std::to_wstring(value), options);
    }

    int ClampNumber(HWND edit, int minValue, int maxValue, int fallback) const {
        auto value = ParseInt(GetText(edit));
        if (!value) {
            return fallback;
        }
        return std::max(minValue, std::min(maxValue, *value));
    }

    void SelectTagAlign() {
        tagAlignIndex_ = 1;
        if (draft_.tagAlign == L"left") {
            tagAlignIndex_ = 0;
        } else if (draft_.tagAlign == L"right") {
            tagAlignIndex_ = 2;
        }
        UpdateTagAlignButtons();
    }

    void UpdateTagAlignButtons() {
        const HWND buttons[] = {tagAlignLeft_, tagAlignCenter_, tagAlignRight_};
        for (int i = 0; i < 3; ++i) {
            if (buttons[i]) {
                ThemedUi::SetTabSelected(buttons[i], i == tagAlignIndex_);
            }
        }
    }

    void CreateTabs() {
        const wchar_t* titles[] = {L"显示", L"行为", L"右键菜单", L"交互", L"热键", L"链接", L"WebDAV", L"HTTP", L"备份"};
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int clientWidth = std::max(1, static_cast<int>(client.right - client.left));
        const ThemedUi ui = MakeUi();
        const DialogLayoutMetrics& layout = ui.layout();
        const int startY = layout.contentInsetY;
        std::vector<ThemedTabItem> items;
        items.reserve(TabCount);
        for (int i = 0; i < TabCount; ++i) {
            items.push_back(ThemedTabItem{ID_SETTINGS_TAB_BASE + i, titles[i], true});
        }
        const int itemHeight = ui.tabButtonHeight();
        tabStripRect_ = RECT{
            layout.contentInsetX,
            startY,
            clientWidth - layout.contentInsetX,
            startY + itemHeight + layout.rowGap};
        ThemedTabControlOptions options{};
        options.activeIndex = TabDisplay;
        options.equalWidth = false;
        options.appearance = ThemedTabControlAppearance::EmphasizedSegmented;
        settingsTabs_ = MakeUi().TabControl(ID_SETTINGS_TAB_CONTROL, tabStripRect_, items, options);
        tabContentOffsetY_ = 0;
    }

    void BindTabPages() {
        if (!settingsTabs_) return;
        for (int tab = 0; tab < TabCount; ++tab) {
            std::vector<HWND> children;
            for (const auto& child : tabChildren_) {
                if (child.hwnd && child.tab == tab) children.push_back(child.hwnd);
            }
            ThemedUi::BindTabPage(settingsTabs_, tab, children);
        }
    }

    void ShowTab(int tab) {
        if (tab < 0 || tab >= TabCount || tab == currentTab_) {
            return;
        }

        const HWND focus = GetFocus();
        bool focusMovesToTab = false;
        for (const auto& child : tabChildren_) {
            if (child.hwnd && child.tab != tab && child.hwnd == focus) {
                focusMovesToTab = true;
            }
        }

        currentTab_ = tab;
        ThemedUi::SetActiveTab(settingsTabs_, tab, false);
        if (focusMovesToTab && settingsTabs_) SetFocus(settingsTabs_);
        if (currentTab_ == TabHttp) {
            UpdateHttpButtons();
        }
        if (currentTab_ == TabWebDav) {
            UpdateWebDavLastSyncLabel();
        }
        if (currentTab_ == TabContextMenu) {
            RequestContextMenuIconLoadOnce();
        }
    }

    HWND AddSectionFrame(int tab, const std::wstring& title, RECT rect) {
        rect.top = ContentY(rect.top);
        rect.bottom = ContentY(rect.bottom);
        HWND group = MakeUi().GroupBox(nextGeneratedControlId_++, title, rect);
        AddTabChild(group, tab);
        return group;
    }

    void AddHotKeyTableRows() {
        if (!hotKeyTable_) return;
        ThemedUi::SetTableRows(hotKeyTable_, {
            ThemedTableRow{
                ID_MAIN_HOTKEY_CAPTURE,
                {
                    ThemedTableCell{L"主窗口显隐"},
                    ThemedTableCell{FormatMainHotKeyText(draft_.mainHotKey)},
                    ThemedTableCell{L"录入", -1, ThemedTableCellRole::Action, ID_MAIN_HOTKEY_CAPTURE},
                },
            },
            ThemedTableRow{
                ID_PROCESS_LOCATOR_HOTKEY_CAPTURE,
                {
                    ThemedTableCell{L"进程定位器"},
                    ThemedTableCell{FormatGlobalHotKeyText(draft_.processLocatorHotKey)},
                    ThemedTableCell{L"录入", -1, ThemedTableCellRole::Action, ID_PROCESS_LOCATOR_HOTKEY_CAPTURE},
                },
            },
            ThemedTableRow{
                ID_COPY_SELECTED_PATHS_HOTKEY_CAPTURE,
                {
                    ThemedTableCell{L"复制选中项绝对路径"},
                    ThemedTableCell{FormatGlobalHotKeyText(draft_.copySelectedPathsHotKey)},
                    ThemedTableCell{L"录入", -1, ThemedTableCellRole::Action, ID_COPY_SELECTED_PATHS_HOTKEY_CAPTURE},
                },
            },
        });
    }

    void ResetDefaultHotKeys() {
        AppConfig defaults{};
        draft_.mainHotKey = defaults.mainHotKey;
        draft_.processLocatorHotKey = defaults.processLocatorHotKey;
        draft_.copySelectedPathsHotKey = defaults.copySelectedPathsHotKey;
        UpdateHotKeyLabels();
        ShowToast(L"已恢复默认热键，保存设置后生效。", ThemedToastRole::Info);
    }

    void UpdateCopyPathContextMenuStatus(bool enabled) {
        if (registerCopyPathContextMenu_) {
            ThemedUi::SetChecked(registerCopyPathContextMenu_, enabled);
        }
        if (!copyPathContextMenuStatus_) {
            return;
        }
        SetWindowTextW(
            copyPathContextMenuStatus_,
            enabled ? L"已注册；Quattro 未运行时也可使用。" : L"未注册。");
        MakeUi().SetStatusTextRole(
            copyPathContextMenuStatus_,
            enabled ? ThemedStatusRole::Success : ThemedStatusRole::Normal);
    }

    void EnsureContextMenuProviderState() {
        const auto providers = TrackedContextMenuProviders();
        if (contextMenuProviderIcons_.size() == providers.size()) {
            return;
        }
        contextMenuProviderIcons_.clear();
        contextMenuProviderIcons_.reserve(providers.size());
        for (const auto& provider : providers) {
            ContextMenuProviderIconInfo info;
            info.providerId = provider.providerId;
            contextMenuProviderIcons_.push_back(std::move(info));
        }
        contextMenuProviderImageIndexes_.assign(providers.size(), 0);
    }

    void DestroyContextMenuImageList() {
        if (contextMenuTable_ && IsWindow(contextMenuTable_)) {
            ThemedUi::SetTableImageLists(contextMenuTable_, nullptr, nullptr);
        }
        if (contextMenuImages_) {
            ImageList_Destroy(contextMenuImages_);
            contextMenuImages_ = nullptr;
        }
    }

    void RebuildContextMenuImageList() {
        EnsureContextMenuProviderState();
        const auto providers = TrackedContextMenuProviders();
        const int iconSize = std::max(1, MakeUi().scale(16));
        HIMAGELIST images = ImageList_Create(
            iconSize, iconSize, ILC_COLOR32,
            static_cast<int>(providers.size()) + 1, 4);
        if (!images) {
            return;
        }

        IconRequest fallbackRequest;
        fallbackRequest.kind = IconSourceKind::Stock;
        fallbackRequest.size = iconSize;
        const ResolvedIcon fallbackIcon = IconResolverService(appDirectory_).Resolve(fallbackRequest);
        HBITMAP fallbackBitmap = IconResolverService::CreateBitmapFromPixels(
            fallbackIcon,
            iconSize,
            ThemedUi::ListSurfaceColor(theme_),
            true);
        int fallbackIndex = fallbackBitmap ? ImageList_Add(images, fallbackBitmap, nullptr) : -1;
        if (fallbackBitmap) {
            DeleteObject(fallbackBitmap);
        }
        if (fallbackIndex < 0) {
            BITMAPINFO bitmapInfo{};
            bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmapInfo.bmiHeader.biWidth = iconSize;
            bitmapInfo.bmiHeader.biHeight = -iconSize;
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HBITMAP blank = CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (blank && bits) {
                ZeroMemory(bits, static_cast<SIZE_T>(iconSize) * static_cast<SIZE_T>(iconSize) * 4);
                fallbackIndex = ImageList_Add(images, blank, nullptr);
            }
            if (blank) DeleteObject(blank);
        }
        if (fallbackIndex < 0) {
            ImageList_Destroy(images);
            return;
        }

        contextMenuProviderImageIndexes_.assign(providers.size(), fallbackIndex);
        for (std::size_t index = 0; index < contextMenuProviderIcons_.size(); ++index) {
            HBITMAP bitmap = CreateScaledBitmapFromCachedPixels(
                contextMenuProviderIcons_[index].icon,
                iconSize);
            if (!bitmap) {
                continue;
            }
            const int imageIndex = ImageList_Add(images, bitmap, nullptr);
            DeleteObject(bitmap);
            if (imageIndex >= 0) {
                contextMenuProviderImageIndexes_[index] = imageIndex;
            }
        }

        HIMAGELIST oldImages = contextMenuImages_;
        contextMenuImages_ = images;
        if (contextMenuTable_) {
            ThemedUi::SetTableImageLists(contextMenuTable_, contextMenuImages_, nullptr);
        }
        if (oldImages) {
            ImageList_Destroy(oldImages);
        }
    }

    void AddContextMenuTableRows() {
        if (!contextMenuTable_) return;
        EnsureContextMenuProviderState();
        const auto providers = TrackedContextMenuProviders();
        std::vector<bool> installed(providers.size(), true);
        if (contextMenuProviderLoadCompleted_) {
            for (std::size_t index = 0; index < contextMenuProviderIcons_.size(); ++index) {
                installed[index] = contextMenuProviderIcons_[index].installed;
            }
        }

        const int selected = ThemedUi::TableSelectedIndex(contextMenuTable_);
        const std::intptr_t selectedKey = selected >= 0
            ? ThemedUi::TableRowKey(contextMenuTable_, selected)
            : 0;
        contextMenuTableOrder_ = TrackedContextMenuDisplayOrder(installed);
        std::vector<ThemedTableRow> rows;
        rows.reserve(contextMenuTableOrder_.size());

        for (std::size_t bindingIndex : contextMenuTableOrder_) {
            const auto& provider = providers[bindingIndex];
            const auto& iconInfo = contextMenuProviderIcons_[bindingIndex];
            ThemedTableRow row{};
            row.key = provider.checkBoxControlId;

            std::wstring statusText;
            if (!contextMenuProviderLoadCompleted_) {
                statusText = L"检测中...";
            } else if (contextMenuProviderLoadFailed_) {
                statusText = L"获取失败";
            } else if (iconInfo.installedViaProbe) {
                statusText = L"已安装(注册表)";
            } else if (iconInfo.installedInNativeShell) {
                statusText = L"已安装(菜单)";
            } else {
                statusText = L"未安装";
            }

            row.cells = {
                ThemedTableCell{
                    provider.displayName,
                    bindingIndex < contextMenuProviderImageIndexes_.size()
                        ? contextMenuProviderImageIndexes_[bindingIndex]
                        : 0},
                ThemedTableCell{statusText},
            };
            row.checked = draft_.*(provider.configMember);
            // 安装状态只用于提示当前能否立即抓取菜单。跟踪配置始终可编辑，
            // 这样软件后续安装后可在刷新 Windows 菜单时自动开始抓取。
            row.enabled = true;
            rows.push_back(std::move(row));
        }
        ThemedUi::SetTableRows(contextMenuTable_, rows);
        if (selectedKey != 0) {
            for (std::size_t index = 0; index < rows.size(); ++index) {
                if (rows[index].key == selectedKey) {
                    ThemedUi::SetTableSelectedIndex(contextMenuTable_, static_cast<int>(index));
                    break;
                }
            }
        }
    }

    bool AllInstalledContextMenuProvidersAttempted() const {
        if (!contextMenuProviderLoadCompleted_) {
            return false;
        }
        return std::all_of(
            contextMenuProviderIcons_.begin(), contextMenuProviderIcons_.end(),
            [](const ContextMenuProviderIconInfo& info) {
                return !info.installed || info.attempted;
            });
    }

    void UpdateContextMenuRefreshButtonState() {
        if (!refreshContextMenuButton_) {
            return;
        }
        const wchar_t* text = contextMenuRefreshBusy_
            ? L"扫描中..."
            : (contextMenuIconLoadBusy_ ? L"获取图标中..." : L"从Windows菜单刷新");
        SetWindowTextW(refreshContextMenuButton_, text);
        MakeUi().SetEnabled(refreshContextMenuButton_, !contextMenuRefreshBusy_ && !contextMenuIconLoadBusy_);
    }

    void SetContextMenuIconLoadBusy(bool busy) {
        contextMenuIconLoadBusy_ = busy;
        UpdateContextMenuRefreshButtonState();
    }

    void RequestContextMenuIconLoadOnce() {
        if (contextMenuIconAutoRequested_) {
            return;
        }
        contextMenuIconAutoRequested_ = true;
        // Provider 展示缓存体积很小且固定有上限；同步读取可让重复打开设置页
        // 直接显示已有图标，避免先绘制 fallback 再异步替换造成闪烁。
        // 注入 runner 的测试/专用入口继续走原异步链路，避免读取正式缓存。
        if (!contextMenuProviderIconRunner_) {
            if (auto cached = ContextMenuProviderIconService().LoadCached();
                cached && cached->size() == TrackedContextMenuProviders().size()) {
                contextMenuProviderIcons_ = std::move(*cached);
                contextMenuProviderLoadCompleted_ = true;
                contextMenuProviderLoadFailed_ = false;
                RebuildContextMenuImageList();
                AddContextMenuTableRows();
                return;
            }
        }
        PostMessageW(hwnd_, WM_CONTEXT_MENU_ICON_LOAD_REQUEST, 0, 0);
    }

    bool StartContextMenuIconLoad(bool force) {
        if (contextMenuIconLoadBusy_ || (!force && AllInstalledContextMenuProvidersAttempted())) {
            return false;
        }
        AbandonContextMenuIconLoad();
        auto state = std::make_shared<SettingsContextMenuIconAsyncState>();
        state->generation = gContextMenuIconGeneration.fetch_add(1);
        contextMenuIconAsyncState_ = state;
        contextMenuIconLoadGeneration_ = state->generation;
        const HWND target = hwnd_;
        const SettingsContextMenuProviderIconRunner runner = contextMenuProviderIconRunner_;
        SetContextMenuIconLoadBusy(true);
        std::thread([state, target, runner]() {
            std::vector<ContextMenuProviderIconInfo> result;
            try {
                result = runner
                    ? runner(state->stopSource.get_token())
                    : ContextMenuProviderIconService().Load(state->stopSource.get_token());
            } catch (...) {
                result.clear();
            }
            if (state->abandoned.load() || state->stopSource.stop_requested()) {
                return;
            }
            {
                std::lock_guard lock(state->mutex);
                state->result = std::move(result);
            }
            PostMessageW(
                target,
                WM_CONTEXT_MENU_ICON_LOAD_DONE,
                static_cast<WPARAM>(state->generation),
                0);
        }).detach();
        return true;
    }

    void CompleteContextMenuIconLoad(std::uintptr_t generation) {
        const auto state = contextMenuIconAsyncState_;
        if (!state || generation != contextMenuIconLoadGeneration_ || generation != state->generation) {
            return;
        }
        std::optional<std::vector<ContextMenuProviderIconInfo>> result;
        {
            std::lock_guard lock(state->mutex);
            result = std::move(state->result);
        }
        contextMenuIconAsyncState_.reset();
        SetContextMenuIconLoadBusy(false);
        if (!result || result->size() != TrackedContextMenuProviders().size()) {
            contextMenuProviderLoadCompleted_ = true;
            contextMenuProviderLoadFailed_ = true;
            RebuildContextMenuImageList();
            AddContextMenuTableRows();
            return;
        }
        contextMenuProviderIcons_ = std::move(*result);
        contextMenuProviderLoadCompleted_ = true;
        contextMenuProviderLoadFailed_ = false;
        RebuildContextMenuImageList();
        AddContextMenuTableRows();
    }

    void AbandonContextMenuIconLoad() {
        const auto state = contextMenuIconAsyncState_;
        if (!state) {
            return;
        }
        state->abandoned = true;
        state->stopSource.request_stop();
        contextMenuIconAsyncState_.reset();
    }

    void ReadContextMenuTableDraft(AppConfig& value) const {
        const auto providers = TrackedContextMenuProviders();
        for (std::size_t rowIndex = 0; rowIndex < contextMenuTableOrder_.size(); ++rowIndex) {
            const auto& provider = providers[contextMenuTableOrder_[rowIndex]];
            value.*(provider.configMember) = ThemedUi::IsTableChecked(
                contextMenuTable_, static_cast<int>(rowIndex));
        }
    }

    bool HandleContextMenuTableEvent(LPARAM lParam) {
        ThemedTableEvent event{};
        if (!ThemedUi::DecodeTableEvent(contextMenuTable_, lParam, event)) {
            return false;
        }
        if (event.kind != ThemedTableEventKind::CheckChanged) {
            return true;
        }
        if (event.row < 0 || static_cast<std::size_t>(event.row) >= contextMenuTableOrder_.size()) {
            return true;
        }
        const auto& provider = TrackedContextMenuProviders()[contextMenuTableOrder_[static_cast<std::size_t>(event.row)]];
        draft_.*(provider.configMember) = event.checked;
        // 确保整行被选中显示
        ThemedUi::SetTableSelectedIndex(contextMenuTable_, event.row);
        return true;
    }

    bool HandleHotKeyTableEvent(LPARAM lParam) {
        ThemedTableEvent event{};
        if (!ThemedUi::DecodeTableEvent(hotKeyTable_, lParam, event)) {
            return false;
        }
        if (event.kind != ThemedTableEventKind::ActionInvoked) {
            return true;
        }
        if (event.actionId == ID_MAIN_HOTKEY_CAPTURE) {
            HotKeyCaptureDialogOptions options{};
            options.allowDoubleAlt = true;
            options.useMainHotKeyText = true;
            TrySetMainHotKey(ShowHotKeyCaptureDialog(hwnd_, instance_, theme_, draft_.mainHotKey, options));
        } else if (event.actionId == ID_PROCESS_LOCATOR_HOTKEY_CAPTURE) {
            TrySetProcessLocatorHotKey(ShowHotKeyCaptureDialog(hwnd_, instance_, theme_, draft_.processLocatorHotKey));
        } else if (event.actionId == ID_COPY_SELECTED_PATHS_HOTKEY_CAPTURE) {
            TrySetCopySelectedPathsHotKey(ShowHotKeyCaptureDialog(hwnd_, instance_, theme_, draft_.copySelectedPathsHotKey));
        }
        return true;
    }

    AppConfig ReadCurrentTabDraft() {
        AppConfig value = config_;
        switch (currentTab_) {
        case TabDisplay: {
            value.showTitle = ThemedUi::IsChecked(showTitle_);
            value.showGroup = ThemedUi::IsChecked(showGroup_);
            value.showTag = ThemedUi::IsChecked(showTag_);
            value.showToolboxButton = ThemedUi::IsChecked(showToolboxButton_);
            value.showSkinButton = ThemedUi::IsChecked(showSkinButton_);
            value.linkNameSingleLine = !ThemedUi::IsChecked(linkNameSingleLine_);
            value.linkNameBold = ThemedUi::IsChecked(linkNameBold_);
            value.showTooltip = ThemedUi::IsChecked(showTooltip_);
            value.groupRight = ThemedUi::IsChecked(groupRight_);
            value.tagRight = ThemedUi::IsChecked(tagRight_);
            value.tagAlign = tagAlignIndex_ == 0 ? L"left" : (tagAlignIndex_ == 2 ? L"right" : L"center");
            auto alpha = ParseInt(GetText(alphaEdit_));
            value.alpha = alpha ? std::max(64, std::min(255, *alpha)) : 255;
            value.groupWidth = ClampNumber(groupWidthEdit_, 40, 240, value.groupWidth);
            value.tagWidth = ClampNumber(tagWidthEdit_, 40, 240, value.tagWidth);
            break;
        }
        case TabBehavior:
            value.autoDock = ThemedUi::IsChecked(autoDock_);
            value.hideWhenInactive = ThemedUi::IsChecked(hideInactive_);
            value.hideMainAfterToolOpen = ThemedUi::IsChecked(hideMainAfterToolOpen_);
            value.hideAfterLink = ThemedUi::IsChecked(hideAfterLink_);
            value.hideOnStart = ThemedUi::IsChecked(hideOnStart_);
            value.hideNotifyIcon = false;
            value.deleteConfirm = ThemedUi::IsChecked(deleteConfirm_);
            value.saveRunCount = ThemedUi::IsChecked(saveRunCount_);
            value.autoRun = ThemedUi::IsChecked(autoRun_);
            value.loggingEnabled = ThemedUi::IsChecked(loggingEnabled_);
            value.dockDelay = ClampNumber(dockDelayEdit_, 0, 5000, value.dockDelay);
            break;
        case TabContextMenu: {
            ReadContextMenuTableDraft(value);
            value.registerCopyPathContextMenu = ThemedUi::IsChecked(registerCopyPathContextMenu_);
            break;
        }
        case TabInteraction:
            value.doubleClickToRun = ThemedUi::IsChecked(doubleClick_);
            value.mouseEnterActiveGroup = ThemedUi::IsChecked(enterActiveGroup_);
            value.mouseEnterActiveTag = ThemedUi::IsChecked(enterActiveTag_);
            value.activeGroupDelay = ClampNumber(groupDelayEdit_, 0, 5000, value.activeGroupDelay);
            value.activeTagDelay = ClampNumber(tagDelayEdit_, 0, 5000, value.activeTagDelay);
            break;
        case TabHotKeys:
            value.globalHotKeysEnabled = ThemedUi::IsChecked(globalHotKeysEnabled_);
            value.mainHotKey = draft_.mainHotKey;
            value.processLocatorHotKey = draft_.processLocatorHotKey;
            value.copySelectedPathsHotKey = draft_.copySelectedPathsHotKey;
            break;
        case TabLinks:
            value.openDirCommand = GetText(openDirEdit_);
            value.updateUrl = GetText(updateUrlEdit_);
            break;
        case TabWebDav:
            value = ReadWebDavDraftFromControls();
            break;
        case TabHttp:
            value = ReadHttpDraftFromControls();
            break;
        case TabBackup:
        default:
            break;
        }
        return value;
    }

    bool TrySetMainHotKey(int key) {
        if (key == 0) {
            draft_.mainHotKey = 0;
            UpdateHotKeyLabels();
            return true;
        }

        const HotKeyAvailability availability = CheckMainHotKeyAvailability(hwnd_, key, CurrentRegisteredMainHotKey());
        if (!availability.available) {
            draft_.mainHotKey = key;
            UpdateHotKeyLabels();
            ShowHotKeyConflictMessage(hwnd_, instance_, theme_, MainHotKeyConflictMessage(key, availability));
            return false;
        }

        draft_.mainHotKey = key;
        UpdateHotKeyLabels();
        return true;
    }

    int CurrentRegisteredMainHotKey() const {
        return mainHotKeyRegistered_ ? config_.mainHotKey : 0;
    }

    int CurrentRegisteredProcessLocatorHotKey() const {
        return processLocatorHotKeyRegistered_ ? config_.processLocatorHotKey : 0;
    }

    int CurrentRegisteredCopySelectedPathsHotKey() const {
        return copySelectedPathsHotKeyRegistered_ ? config_.copySelectedPathsHotKey : 0;
    }

    bool TrySetProcessLocatorHotKey(int key) {
        if (key == kMainHotKeyDoubleAlt) {
            key = 0;
        }
        const HotKeyAvailability availability = CheckCtrlAltHotKeyAvailability(hwnd_, key, CurrentRegisteredProcessLocatorHotKey());
        if (!availability.available) {
            draft_.processLocatorHotKey = key;
            UpdateHotKeyLabels();
            ShowHotKeyConflictMessage(hwnd_, instance_, theme_, ProcessLocatorHotKeyStatusText(key, availability));
            return false;
        }
        draft_.processLocatorHotKey = key;
        UpdateHotKeyLabels();
        return true;
    }

    bool TrySetCopySelectedPathsHotKey(int key) {
        if (key == kMainHotKeyDoubleAlt) {
            key = 0;
        }
        const HotKeyAvailability availability = CheckCtrlAltHotKeyAvailability(
            hwnd_, key, CurrentRegisteredCopySelectedPathsHotKey());
        if (!availability.available) {
            draft_.copySelectedPathsHotKey = key;
            UpdateHotKeyLabels();
            ShowHotKeyConflictMessage(hwnd_, instance_, theme_, CopySelectedPathsHotKeyStatusText(key, availability));
            return false;
        }
        draft_.copySelectedPathsHotKey = key;
        UpdateHotKeyLabels();
        return true;
    }

    bool ValidateHotKeysBeforeSave() {
        if (!draft_.globalHotKeysEnabled) {
            UpdateHotKeyLabels();
            return true;
        }
        if (!IsDoubleAltMainHotKey(draft_.mainHotKey) &&
            draft_.mainHotKey != 0 &&
            draft_.mainHotKey == draft_.processLocatorHotKey) {
            UpdateHotKeyLabels();
            ShowHotKeyConflictMessage(hwnd_, instance_, theme_, L"主窗口显隐和进程定位器不能使用同一个快捷键。");
            return false;
        }
        if (!IsDoubleAltMainHotKey(draft_.mainHotKey) &&
            draft_.mainHotKey != 0 &&
            draft_.mainHotKey == draft_.copySelectedPathsHotKey) {
            UpdateHotKeyLabels();
            ShowHotKeyConflictMessage(hwnd_, instance_, theme_, L"主窗口显隐和复制选中项绝对路径不能使用同一个快捷键。");
            return false;
        }
        if (draft_.processLocatorHotKey != 0 &&
            draft_.processLocatorHotKey == draft_.copySelectedPathsHotKey) {
            UpdateHotKeyLabels();
            ShowHotKeyConflictMessage(hwnd_, instance_, theme_, L"进程定位器和复制选中项绝对路径不能使用同一个快捷键。");
            return false;
        }
        const HotKeyAvailability availability = CheckMainHotKeyAvailability(hwnd_, draft_.mainHotKey, CurrentRegisteredMainHotKey());
        UpdateHotKeyLabels();
        if (!availability.available) {
            ShowHotKeyConflictMessage(hwnd_, instance_, theme_, MainHotKeyConflictMessage(draft_.mainHotKey, availability));
            return false;
        }

        const HotKeyAvailability locatorAvailability = CheckCtrlAltHotKeyAvailability(
            hwnd_, draft_.processLocatorHotKey, CurrentRegisteredProcessLocatorHotKey());
        UpdateHotKeyLabels();
        if (!locatorAvailability.available) {
            ShowHotKeyConflictMessage(
                hwnd_, instance_, theme_,
                ProcessLocatorHotKeyStatusText(draft_.processLocatorHotKey, locatorAvailability));
            return false;
        }
        const HotKeyAvailability copyAvailability = CheckCtrlAltHotKeyAvailability(
            hwnd_, draft_.copySelectedPathsHotKey, CurrentRegisteredCopySelectedPathsHotKey());
        UpdateHotKeyLabels();
        if (!copyAvailability.available) {
            ShowHotKeyConflictMessage(
                hwnd_, instance_, theme_,
                CopySelectedPathsHotKeyStatusText(draft_.copySelectedPathsHotKey, copyAvailability));
            return false;
        }
        return true;
    }

    AppConfig ReadWebDavDraftFromControls() {
        AppConfig value = config_;
        value.webDavEnabled = ThemedUi::IsChecked(webDavEnabled_);
        value.registerWebDavUploadContextMenu = ThemedUi::IsChecked(webDavUploadContextMenu_);
        value.webDavUrl = GetText(webDavUrlEdit_);
        value.webDavBackupPath = GetText(webDavBackupPathEdit_);
        value.webDavFilesPath = GetText(webDavFilesPathEdit_);
        value.webDavUserName = GetText(webDavUserNameEdit_);
        value.webDavKeepCount = ClampNumber(webDavKeepCountEdit_, 1, 100, value.webDavKeepCount);
        value.webDavLastSyncAt = draft_.webDavLastSyncAt;
        if (Trim(value.webDavBackupPath).empty()) value.webDavBackupPath = L"/Quattro/backups/";
        if (Trim(value.webDavFilesPath).empty()) value.webDavFilesPath = L"/Quattro/files/";
        return value;
    }

    void UpdateWebDavLastSyncLabel() {
        if (!webDavLastSyncLabel_) {
            return;
        }
        const std::wstring text = FormatWebDavLastSyncText(draft_.webDavLastSyncAt);
        SetWindowTextW(webDavLastSyncLabel_, text.c_str());
        ShowWindow(webDavLastSyncLabel_, text.empty() ? SW_HIDE : SW_SHOW);
    }

    void ApplyExternalAutoRun(bool enabled) {
        config_.autoRun = enabled;
        draft_.autoRun = enabled;
        if (autoRun_) {
            ThemedUi::SetChecked(autoRun_, enabled);
        }
    }

    void MarkWebDavSyncedNow(bool importedData) {
        AppConfig next = ReadWebDavDraftFromControls();
        next.webDavLastSyncAt = CurrentTodoTimestamp();
        draft_ = next;
        config_ = next;
        if (importedData) {
            importedData_ = true;
        }
        UpdateWebDavLastSyncLabel();
        if (applyCallback_) {
            mainHotKeyRegistered_ = applyCallback_(config_, importedData_);
            processLocatorHotKeyRegistered_ = config_.globalHotKeysEnabled && config_.processLocatorHotKey != 0;
            copySelectedPathsHotKeyRegistered_ = config_.globalHotKeysEnabled && config_.copySelectedPathsHotKey != 0;
            importedData_ = false;
        }
    }

    bool SaveWebDavPasswordIfNeeded(const AppConfig& value) {
        const std::wstring password = GetText(webDavPasswordEdit_);
        if (password.empty()) {
            return true;
        }
        std::wstring error;
        if (!WebDavCredentialService::SavePassword(value, password, error)) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, L"WebDAV 备份", MB_OK | MB_ICONWARNING);
            return false;
        }
        return true;
    }

    void ClearWebDavPassword() {
        if (!EnsureWebDavIdle()) {
            return;
        }
        AppConfig value = ReadWebDavDraftFromControls();
        std::wstring error;
        if (WebDavCredentialService::DeletePassword(value, error)) {
            SetWindowTextW(webDavPasswordEdit_, L"");
            ShowToast(L"WebDAV 密码已清除。", ThemedToastRole::Success);
        } else {
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, L"WebDAV 备份", MB_OK | MB_ICONWARNING);
        }
    }

    void ResetContextMenu() {
        if (contextMenuRefreshBusy_ || contextMenuIconLoadBusy_) {
            ShowToast(L"Windows 菜单正在刷新，请稍候。", ThemedToastRole::Info);
            return;
        }
        if (!resetContextMenuCallback_) {
            ShowThemedMessageBox(
                hwnd_, instance_, theme_, L"当前无法访问右键菜单缓存。", L"重置右键菜单", MB_OK | MB_ICONWARNING);
            return;
        }
        const int answer = ShowThemedMessageBox(
            hwnd_,
            instance_,
            theme_,
            L"重置后将关闭所有右键菜单跟踪开关，并清除全部启动项缓存的菜单列表、启用状态和菜单图标。是否继续？",
            L"重置右键菜单",
            MB_YESNO | MB_ICONWARNING);
        if (answer != IDYES) {
            return;
        }
        if (resetContextMenuCallback_()) {
            for (const auto& provider : TrackedContextMenuProviders()) {
                config_.*(provider.configMember) = false;
                draft_.*(provider.configMember) = false;
            }
            AddContextMenuTableRows();
            ShowToast(L"右键菜单已重置，跟踪开关与缓存均已恢复默认。", ThemedToastRole::Success, 5000);
        } else {
            ShowThemedMessageBox(
                hwnd_, instance_, theme_, L"右键菜单重置失败，请确认缓存目录可写。", L"重置右键菜单", MB_OK | MB_ICONWARNING);
        }
    }

    ShellContextMenuTrackingOptions ContextMenuTrackingDraft() const {
        ShellContextMenuTrackingOptions tracking;
        for (const auto& provider : TrackedContextMenuProviders()) {
            tracking.*(provider.trackingMember) = draft_.*(provider.configMember);
        }
        return tracking;
    }

    void SetContextMenuRefreshBusy(bool busy) {
        contextMenuRefreshBusy_ = busy;
        UpdateContextMenuRefreshButtonState();
    }

    std::wstring ContextMenuRefreshResultText(const ShellContextMenuRefreshResult& result) const {
        std::wstring message =
            L"扫描启动项: " + std::to_wstring(result.totalLinks) +
            L"\n成功: " + std::to_wstring(result.succeededLinks) +
            L"\n跳过: " + std::to_wstring(result.skippedLinks) +
            L"\n失败: " + std::to_wstring(result.failures.size()) +
            L"\n更新菜单项: " + std::to_wstring(result.menuItemCount);
        const std::size_t detailCount = std::min<std::size_t>(result.failures.size(), 5);
        for (std::size_t index = 0; index < detailCount; ++index) {
            const auto& failure = result.failures[index];
            message += L"\n\n";
            message += failure.linkName.empty() ? L"刷新服务" : failure.linkName;
            message += L": " + failure.message;
        }
        if (result.failures.size() > detailCount) {
            message += L"\n\n其余 " + std::to_wstring(result.failures.size() - detailCount) + L" 项请查看日志。";
        }
        return message;
    }

    void CompleteContextMenuRefresh() {
        if (!contextMenuRefreshTask_ || !contextMenuRefreshTask_->IsFinished()) {
            return;
        }
        contextMenuRefreshTask_->Wait();
        std::optional<ShellContextMenuRefreshResult> result;
        if (contextMenuRefreshTask_->Status() != ScanTaskStatus::Failed) {
            result = contextMenuRefreshTask_->ResultCopy<ShellContextMenuRefreshResult>();
        }
        contextMenuRefreshTask_.reset();
        if (contextMenuRefreshProgressDialog_) contextMenuRefreshProgressDialog_->Close();
        SetContextMenuRefreshBusy(false);
        if (!result) {
            ShowThemedMessageBox(
                hwnd_, instance_, theme_, L"刷新线程未返回结果。", L"从Windows菜单刷新", MB_OK | MB_ICONWARNING);
            return;
        }
        if (result->cancelled) {
            ShowToast(L"已取消 Windows 菜单刷新。", ThemedToastRole::Info);
            return;
        }
        if (result->succeededLinks > 0 && contextMenuRefreshApplyCallback_) {
            contextMenuRefreshApplyCallback_(*result);
        }
        // 原生菜单刷新只更新具体命令及其图标缓存；provider 品牌图标是
        // 独立持久化数据，不在这里重新加载或重建 ImageList。
        if (result->failures.empty()) {
            const std::wstring message =
                L"已刷新 " + std::to_wstring(result->succeededLinks) +
                L" 个启动项，更新 " + std::to_wstring(result->menuItemCount) + L" 个菜单项。";
            ShowToast(message, ThemedToastRole::Success, 5000);
            return;
        }
        ShowThemedMessageBox(
            hwnd_, instance_, theme_, ContextMenuRefreshResultText(*result),
            L"从Windows菜单刷新", MB_OK | MB_ICONWARNING);
    }

    void RefreshContextMenuFromNative() {
        if (contextMenuRefreshBusy_ || contextMenuIconLoadBusy_) {
            ShowToast(L"Windows 菜单正在刷新，请稍候。", ThemedToastRole::Info);
            return;
        }
        const ShellContextMenuTrackingOptions tracking = ContextMenuTrackingDraft();
        if (!tracking.Any()) {
            ShowThemedMessageBox(
                hwnd_, instance_, theme_, L"没有启用需要跟踪的工具。", L"从Windows菜单刷新", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (contextMenuLinks_.empty()) {
            ShowThemedMessageBox(
                hwnd_, instance_, theme_, L"当前没有可刷新的启动项。", L"从Windows菜单刷新", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (contextMenuRefreshTask_) {
            contextMenuRefreshTask_->RequestStop();
            contextMenuRefreshTask_->Wait();
            contextMenuRefreshTask_.reset();
        }

        ShellContextMenuRefreshRequest request{contextMenuLinks_, tracking};
        const HWND target = hwnd_;
        const SettingsContextMenuRefreshRunner runner = contextMenuRefreshRunner_;
        SetContextMenuRefreshBusy(true);
        ShowToast(L"正在扫描 Windows 原生菜单...", ThemedToastRole::Info, 5000);
        ScanTaskOptions scanOptions;
        scanOptions.mode = ScanExecutionMode::BackgroundSingle;
        scanOptions.completionCallback = [target]() {
            PostMessageW(target, WM_CONTEXT_MENU_REFRESH_DONE, 0, 0);
        };
        contextMenuRefreshTask_ = ScanExecutionService::StartTyped<ShellContextMenuRefreshResult>(
            std::move(scanOptions),
            [request = std::move(request), runner](ScanTaskContext& context) mutable {
                ShellContextMenuRefreshResult result;
                try {
                    result = runner
                        ? runner(request, context.StopToken())
                        : ShellContextMenuRefreshService().Refresh(request, context.StopToken());
                } catch (...) {
                    result.tracking = request.tracking;
                    result.totalLinks = static_cast<int>(request.links.size());
                    result.failures.push_back(ShellContextMenuRefreshFailure{
                        0, L"", L"刷新过程中发生未处理异常。"});
                }
                return result;
            });
        ThemedTaskProgressDialogOptions progressOptions{};
        progressOptions.owner = hwnd_;
        progressOptions.instance = instance_;
        progressOptions.theme = theme_;
        progressOptions.icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        progressOptions.className = L"QuattroShellMenuScanProgress_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        progressOptions.title = L"Windows 菜单扫描进度";
        progressOptions.readSnapshot = [task = contextMenuRefreshTask_]() {
            return ToThemedTaskProgressSnapshot(task->Snapshot());
        };
        progressOptions.requestStop = [task = contextMenuRefreshTask_]() { task->RequestStop(); };
        contextMenuRefreshProgressDialog_ =
            std::make_unique<ThemedTaskProgressDialog>(std::move(progressOptions));
        contextMenuRefreshProgressDialog_->Show();
    }

    bool PrepareWebDavOperation(AppConfig& value) {
        value = ReadWebDavDraftFromControls();
        if (!SaveWebDavPasswordIfNeeded(value)) {
            return false;
        }
        return true;
    }

    void SetWebDavBusy(bool busy, SettingsWebDavOperation operation = SettingsWebDavOperation::Test) {
        webDavBusy_ = busy;
        const ThemedUi ui = MakeUi();
        if (webDavUploadButton_) {
            ui.SetEnabled(webDavUploadButton_, !busy);
            SetWindowTextW(webDavUploadButton_, busy && operation == SettingsWebDavOperation::Upload ? L"上传中..." : L"上传到云端");
        }
        if (webDavDownloadButton_) {
            ui.SetEnabled(webDavDownloadButton_, !busy);
            const bool downloadBusy = operation == SettingsWebDavOperation::List ||
                operation == SettingsWebDavOperation::DownloadPreview ||
                operation == SettingsWebDavOperation::DownloadApply;
            SetWindowTextW(webDavDownloadButton_, busy && downloadBusy ? L"处理中..." : L"从云端下载");
        }
        if (webDavTestButton_) {
            ui.SetEnabled(webDavTestButton_, !busy);
            SetWindowTextW(webDavTestButton_, busy && operation == SettingsWebDavOperation::Test ? L"测试中..." : L"测试连接");
        }
        if (webDavClearPasswordButton_) {
            ui.SetEnabled(webDavClearPasswordButton_, !busy);
        }
        if (okButton_) {
            ui.SetEnabled(okButton_, !busy);
        }
        if (cancelButton_) {
            ui.SetEnabled(cancelButton_, !busy);
        }
        if (applyButton_) {
            ui.SetEnabled(applyButton_, !busy);
        }
    }

    bool EnsureWebDavIdle() const {
        if (webDavBusy_) {
            MessageBeep(MB_ICONINFORMATION);
            return false;
        }
        return true;
    }

    void UploadWebDavBackup() {
        if (!EnsureWebDavIdle()) {
            return;
        }
        AppConfig config;
        if (!PrepareWebDavOperation(config)) {
            return;
        }
        SetWebDavBusy(true, SettingsWebDavOperation::Upload);
        const HWND target = hwnd_;
        const std::filesystem::path appDirectory = appDirectory_;
        std::thread([target, appDirectory, config]() {
            auto result = std::make_unique<SettingsWebDavResult>();
            result->operation = SettingsWebDavOperation::Upload;
            WebDavBackupService service(appDirectory, config);
            result->report = service.UploadBackup();
            result->ok = result->report.ok;
            result->message = result->report.message;
            SettingsWebDavResult* raw = result.release();
            if (!PostMessageW(target, WM_SETTINGS_WEBDAV_DONE, 0, reinterpret_cast<LPARAM>(raw))) {
                delete raw;
            }
        }).detach();
    }

    void DownloadWebDavBackup() {
        if (!EnsureWebDavIdle()) {
            return;
        }
        AppConfig config;
        if (!PrepareWebDavOperation(config)) {
            return;
        }
        SetWebDavBusy(true, SettingsWebDavOperation::List);
        const HWND target = hwnd_;
        const std::filesystem::path appDirectory = appDirectory_;
        std::thread([target, appDirectory, config]() {
            auto result = std::make_unique<SettingsWebDavResult>();
            result->operation = SettingsWebDavOperation::List;
            result->config = config;
            WebDavBackupService service(appDirectory, config);
            std::wstring error;
            result->ok = service.ListBackups(result->backups, error);
            result->message = result->ok ? std::wstring{} : error;
            SettingsWebDavResult* raw = result.release();
            if (!PostMessageW(target, WM_SETTINGS_WEBDAV_DONE, 0, reinterpret_cast<LPARAM>(raw))) {
                delete raw;
            }
        }).detach();
    }

    void DownloadSelectedWebDavBackup(const AppConfig& config, const std::wstring& fileName) {
        SetWebDavBusy(true, SettingsWebDavOperation::DownloadPreview);
        const HWND target = hwnd_;
        const std::filesystem::path appDirectory = appDirectory_;
        std::thread([target, appDirectory, config, fileName]() {
            auto result = std::make_unique<SettingsWebDavResult>();
            result->operation = SettingsWebDavOperation::DownloadPreview;
            result->config = config;
            WebDavBackupService service(appDirectory, config);
            result->report = service.DownloadAndPreviewMerge(fileName);
            result->ok = result->report.ok;
            result->message = result->report.message;
            SettingsWebDavResult* raw = result.release();
            if (!PostMessageW(target, WM_SETTINGS_WEBDAV_DONE, 0, reinterpret_cast<LPARAM>(raw))) {
                delete raw;
            }
        }).detach();
    }

    void ApplyDownloadedWebDavBackup(const SettingsWebDavResult& previewResult, TodoRestorePolicy restorePolicy) {
        SetWebDavBusy(true, SettingsWebDavOperation::DownloadApply);
        const HWND target = hwnd_;
        const std::filesystem::path appDirectory = appDirectory_;
        const AppConfig config = previewResult.config;
        const std::filesystem::path packagePath = previewResult.report.downloadedPackagePath;
        const std::wstring remoteName = previewResult.report.remoteName;
        const std::wstring stateToken = previewResult.report.mergePreview.stateToken;
        std::thread([target, appDirectory, config, packagePath, remoteName, stateToken, restorePolicy]() {
            auto result = std::make_unique<SettingsWebDavResult>();
            result->operation = SettingsWebDavOperation::DownloadApply;
            WebDavBackupService service(appDirectory, config);
            result->report = service.ApplyDownloadedMerge(packagePath, remoteName, restorePolicy, stateToken);
            result->ok = result->report.ok;
            result->message = result->report.importReport.message.empty()
                ? result->report.message
                : FormatConfigPackageReportText(result->report.importReport);
            SettingsWebDavResult* raw = result.release();
            if (!PostMessageW(target, WM_SETTINGS_WEBDAV_DONE, 0, reinterpret_cast<LPARAM>(raw))) delete raw;
        }).detach();
    }

    void ContinueWebDavMergePreview(const SettingsWebDavResult& result) {
        if (!result.ok) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, result.message, L"从云端下载", MB_OK | MB_ICONWARNING);
            return;
        }
        const auto& titles = result.report.mergePreview.deletedTodoTitles;
        if (titles.empty()) {
            ApplyDownloadedWebDavBackup(result, TodoRestorePolicy::KeepDeleted);
            return;
        }
        std::wstring message = L"云端备份中有 " + std::to_wstring(titles.size()) +
            L" 条待办已在本地删除。是否恢复这些条目？\n\n";
        const std::size_t shown = std::min<std::size_t>(titles.size(), 8);
        for (std::size_t index = 0; index < shown; ++index) message += L"- " + titles[index] + L"\n";
        if (titles.size() > shown) message += L"- 以及其他 " + std::to_wstring(titles.size() - shown) + L" 条\n";
        message += L"\n“是”恢复这些待办；“否”保持删除并继续合并；“取消”终止本次合并。";
        const int choice = ShowThemedMessageBox(
            hwnd_, instance_, theme_, message, L"确认恢复已删除待办",
            MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (choice == IDCANCEL) {
            std::error_code ec;
            std::filesystem::remove(result.report.downloadedPackagePath, ec);
            return;
        }
        ApplyDownloadedWebDavBackup(
            result, choice == IDYES ? TodoRestorePolicy::RestoreDeleted : TodoRestorePolicy::KeepDeleted);
    }

    void ContinueWebDavDownloadSelection(const SettingsWebDavResult& result) {
        if (!result.ok) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, result.message, L"从云端下载", MB_OK | MB_ICONWARNING);
            return;
        }
        if (result.backups.empty()) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"远端目录中没有可用的 .q4cfg 备份。", L"从云端下载", MB_OK | MB_ICONINFORMATION);
            return;
        }
        std::wstring fileName = result.backups.front().name;
        WebDavBackupSelectionDialog selectionDialog(hwnd_, instance_, theme_, result.backups, fileName);
        if (!selectionDialog.Run()) {
            return;
        }
        auto selectedBackup = std::find_if(result.backups.begin(), result.backups.end(), [&](const WebDavRemoteFile& backup) {
            return backup.name == fileName;
        });
        if (selectedBackup == result.backups.end()) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"未找到所选 WebDAV 备份，请重新选择。", L"从云端下载", MB_OK | MB_ICONWARNING);
            return;
        }

        const int confirm = ShowThemedMessageBox(
            hwnd_,
            instance_,
            theme_,
            FormatBackupConfirmationText(*selectedBackup),
            L"从云端下载",
            MB_OKCANCEL | MB_ICONINFORMATION);
        if (confirm != IDOK) {
            return;
        }

        DownloadSelectedWebDavBackup(result.config, fileName);
    }

    void TestWebDavConnection() {
        if (!EnsureWebDavIdle()) {
            return;
        }
        AppConfig value = ReadWebDavDraftFromControls();
        value.webDavEnabled = true;
        std::wstring password = GetText(webDavPasswordEdit_);
        SetWebDavBusy(true, SettingsWebDavOperation::Test);
        const HWND target = hwnd_;
        std::thread([target, value, password]() mutable {
            auto result = std::make_unique<SettingsWebDavResult>();
            result->operation = SettingsWebDavOperation::Test;
            std::wstring error;
            if (password.empty() && !WebDavCredentialService::LoadPassword(value, password, error)) {
                result->message = error;
            } else {
                WebDavClient client(value, password);
                result->ok = client.TestConnection();
                result->message = result->ok ? L"WebDAV 连接成功。" : client.lastError();
            }
            SettingsWebDavResult* raw = result.release();
            if (!PostMessageW(target, WM_SETTINGS_WEBDAV_DONE, 0, reinterpret_cast<LPARAM>(raw))) {
                delete raw;
            }
        }).detach();
    }

    void HandleWebDavResult(std::unique_ptr<SettingsWebDavResult> result) {
        if (!result) {
            return;
        }
        SetWebDavBusy(false);
        switch (result->operation) {
        case SettingsWebDavOperation::Test:
            if (result->ok) {
                ShowToast(result->message, ThemedToastRole::Success);
            } else {
                ShowThemedMessageBox(hwnd_, instance_, theme_, result->message, L"WebDAV 备份", MB_OK | MB_ICONWARNING);
            }
            return;
        case SettingsWebDavOperation::Upload:
            if (result->ok) {
                MarkWebDavSyncedNow(false);
                ShowToast(result->message.empty() ? L"已上传到云端。" : result->message, ThemedToastRole::Success);
            } else {
                ShowThemedMessageBox(hwnd_, instance_, theme_, result->message, L"上传到云端", MB_OK | MB_ICONWARNING);
            }
            return;
        case SettingsWebDavOperation::List:
            ContinueWebDavDownloadSelection(*result);
            return;
        case SettingsWebDavOperation::DownloadPreview:
            ContinueWebDavMergePreview(*result);
            return;
        case SettingsWebDavOperation::DownloadApply:
            if (result->ok) {
                MarkWebDavSyncedNow(true);
                ShowToast(result->message.empty() ? L"已从云端下载并合并。" : result->message, ThemedToastRole::Success);
            } else {
                ShowThemedMessageBox(hwnd_, instance_, theme_, result->message, L"从云端下载", MB_OK | MB_ICONWARNING);
            }
            return;
        }
    }

    void ExportConfigPackage() {
        std::wstring targetPath;
        if (!SelectSavePath(hwnd_,
                (appDirectory_ / ConfigPackageFileName()).wstring(),
                L"Quattro快速启动器 配置包 (*.q4cfg)\0*.q4cfg\0所有文件\0*.*\0",
                L"q4cfg",
                targetPath)) {
            return;
        }
        ConfigPackageOptions options;
        options.includeConfig = true;
        options.includeData = true;
        options.includeUrlIcons = true;
        ConfigPackageService service(appDirectory_);
        const ConfigPackageReport report = service.ExportPackage(targetPath, options);
        if (report.ok) {
            ShowToast(L"配置包已导出。", ThemedToastRole::Success);
        } else {
            ShowThemedMessageBox(hwnd_, instance_, theme_, FormatConfigPackageReportText(report), L"导出配置包", MB_OK | MB_ICONWARNING);
        }
    }

    void ImportConfigPackage() {
        std::wstring packagePath;
        if (!SelectOpenPath(hwnd_,
                L"设置配置包导入",
                L"Quattro快速启动器 配置包 (*.q4cfg)\0*.q4cfg\0所有文件\0*.*\0",
                L"q4cfg",
                appDirectory_.wstring(),
                packagePath)) {
            return;
        }
        const int confirm = ShowThemedMessageBox(
            hwnd_,
            instance_,
            theme_,
            L"将把配置包中的分组、标签、启动项、便签和待办合并到当前数据。\n\n"
            L"同一待办按最后更新时间保留较新版本，本地已删除的待办默认保持删除。导入前会自动备份。",
            L"合并导入配置包",
            MB_OKCANCEL | MB_ICONINFORMATION);
        if (confirm != IDOK) {
            return;
        }
        ConfigPackageOptions options;
        options.includeConfig = false;
        options.includeData = true;
        options.includeUrlIcons = true;
        ConfigPackageService service(appDirectory_);
        const ConfigPackageReport report = service.ImportPackageMerge(packagePath, options);
        if (report.ok) {
            importedData_ = true;
            ShowToast(L"配置包已合并导入。", ThemedToastRole::Success);
        } else {
            ShowThemedMessageBox(hwnd_, instance_, theme_, FormatConfigPackageReportText(report), L"合并导入配置包", MB_OK | MB_ICONWARNING);
        }
    }

    void ExportTodosJson() {
        TodoJsonExportOptions options;
        options.includeCompleted = !todoIncludeCompleted_ || ThemedUi::IsChecked(todoIncludeCompleted_);
        options.includeDisabled = !todoIncludeDisabled_ || ThemedUi::IsChecked(todoIncludeDisabled_);
        options.onlyFuture = todoOnlyFuture_ && ThemedUi::IsChecked(todoOnlyFuture_);
        std::wstring targetPath;
        if (!SelectSavePath(hwnd_,
                (appDirectory_ / TodoJsonBackupService::DefaultFileName()).wstring(),
                L"JSON 文件 (*.json)\0*.json\0所有文件\0*.*\0",
                L"json",
                targetPath)) {
            return;
        }
        TodoJsonBackupService service(appDirectory_);
        std::wstring error;
        if (!service.ExportJson(targetPath, options, error)) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, error.empty() ? L"写入待办 JSON 文件失败。" : error, L"导出待办 JSON", MB_OK | MB_ICONWARNING);
            return;
        }
        ShowToast(L"待办 JSON 导出完成。", ThemedToastRole::Success);
    }

    void ImportTodosJson(TodoJsonImportMode mode) {
        std::wstring jsonPath;
        if (!SelectOpenPath(hwnd_,
                L"待办JSON导入",
                L"JSON 文件 (*.json)\0*.json\0所有文件\0*.*\0",
                L"json",
                appDirectory_.wstring(),
                jsonPath)) {
            return;
        }
        TodoJsonBackupService service(appDirectory_);
        TodoJsonImportOptions options;
        options.mode = mode;
        options.restoreDeletedPolicy = TodoJsonRestoreDeletedPolicy::KeepDeleted;

        std::wstring confirmText;
        std::wstring confirmTitle;
        UINT confirmIcon = MB_ICONINFORMATION;
        if (mode == TodoJsonImportMode::ReplaceAll) {
            const TodoJsonImportReport preview = service.PreviewImport(jsonPath, options);
            if (!preview.ok) {
                ShowThemedMessageBox(hwnd_, instance_, theme_, FormatTodoJsonImportReportText(preview), L"全量导入待办 JSON", MB_OK | MB_ICONWARNING);
                return;
            }
            confirmTitle = L"全量导入待办 JSON";
            confirmIcon = MB_ICONWARNING;
            confirmText = L"将用 JSON 中的 " + std::to_wstring(preview.todosParsed) +
                L" 项待办替换当前 " + std::to_wstring(preview.todosDeletedForReplace) +
                L" 项待办。\n\n分组、标签、启动项和便签不会被删除；导入前会自动备份当前数据库。";
        } else {
            confirmTitle = L"合并导入待办 JSON";
            confirmText =
                L"将把 JSON 中的待办事项合并到当前数据；同一待办按同步标识和更新时间保留较新版本。\n\n"
                L"缺失的分组或待办标签会自动创建，本地已删除的同一待办默认保持删除。导入前会自动备份。";
        }

        const int confirm = ShowThemedMessageBox(
            hwnd_,
            instance_,
            theme_,
            confirmText,
            confirmTitle,
            MB_OKCANCEL | confirmIcon);
        if (confirm != IDOK) {
            return;
        }
        const TodoJsonImportReport report = service.ImportJson(jsonPath, options);
        if (report.ok) {
            importedData_ = true;
            if (report.todosConflicted > 0 || report.todosFailed > 0 || !report.warnings.empty()) {
                ShowThemedMessageBox(hwnd_, instance_, theme_, FormatTodoJsonImportReportText(report), confirmTitle, MB_OK | MB_ICONINFORMATION);
            } else {
                ShowToast(report.message.empty() ? L"导入完成。" : report.message, ThemedToastRole::Success);
            }
        } else {
            ShowThemedMessageBox(hwnd_, instance_, theme_, FormatTodoJsonImportReportText(report),
                confirmTitle.empty() ? L"导入待办 JSON" : confirmTitle, MB_OK | MB_ICONWARNING);
        }
    }

    AppConfig ReadHttpDraftFromControls() {
        AppConfig value = config_;
        value.httpServerEnabled = httpServer_ && httpServer_->IsRunning();
        value.httpServerAutoStart = httpServerAutoStart_ && ThemedUi::IsChecked(httpServerAutoStart_);
        value.httpServerLanAccess = true;
        value.httpServerPort = ParseHttpPortText(GetText(httpServerAddressEdit_), value.httpServerPort);
        value.httpServerRootPath = GetText(httpServerRootEdit_);
        if (Trim(value.httpServerRootPath).empty()) {
            value.httpServerRootPath = LocalHttpServerService::DefaultRootPath(httpRootBaseDirectory_).wstring();
        }
        return value;
    }

    std::wstring CurrentHttpAddress(bool trailingSlash) {
        const AppConfig value = ReadHttpDraftFromControls();
        return HttpAddressText(value.httpServerLanAccess, value.httpServerPort, trailingSlash);
    }

    void UpdateHttpAddressField(bool trailingSlash = false) {
        if (!httpServerAddressEdit_) {
            return;
        }
        SetWindowTextW(httpServerAddressEdit_, CurrentHttpAddress(trailingSlash).c_str());
    }

    void UpdateHttpButtons() {
        const bool running = httpServer_ && httpServer_->IsRunning();
        const ThemedUi ui = MakeUi();
        if (httpStartButton_) {
            ui.SetEnabled(httpStartButton_, !running);
        }
        if (httpStopButton_) {
            ui.SetEnabled(httpStopButton_, running);
        }
        if (httpServerAddressEdit_) {
            windowUi_->SetEditEnabled(httpServerAddressEdit_, !running);
        }
        if (httpServerRootEdit_) {
            windowUi_->SetEditEnabled(httpServerRootEdit_, !running);
        }
        if (httpBrowseRootButton_) {
            ui.SetEnabled(httpBrowseRootButton_, !running);
        }
    }

    void UpdateHttpStatusLabel() {
        if (!httpServerStatusTag_ || !httpServerStatusDetail_) {
            return;
        }
        std::wstring tag;
        std::wstring detail;
        const bool running = httpServer_ && httpServer_->IsRunning();
        if (running) {
            const auto& options = httpServer_->options();
            tag = L"运行中";
            detail = HttpAddressText(options.lanAccess, options.port, true);
        } else if (httpServer_ && !Trim(httpServer_->lastError()).empty()) {
            tag = L"启动异常";
            detail = httpServer_->lastError();
        } else if (!httpServer_) {
            tag = L"异常";
            detail = L"HTTP 服务对象不可用。";
        } else {
            tag = L"未启动";
            detail = L"服务未启动。";
        }
        SetWindowTextW(httpServerStatusTag_, tag.c_str());
        MakeUi().SetStatusBadgeRole(
            httpServerStatusTag_, running ? ThemedStatusRole::Success : ThemedStatusRole::Danger);
        SetWindowTextW(httpServerStatusDetail_, detail.c_str());
        UpdateHttpButtons();
    }

    void BrowseHttpRoot() {
        if (httpServer_ && httpServer_->IsRunning()) {
            return;
        }
        CommonFileDialogOptions options{};
        options.owner = hwnd_;
        options.mode = CommonFileDialogMode::FolderOnly;
        options.context = L"HTTP绑定磁盘路径";
        options.defaultPath = GetText(httpServerRootEdit_);
        CommonFileDialogResult result{};
        if (!ShowCommonFileDialog(options, result)) {
            return;
        }
        SetWindowTextW(httpServerRootEdit_, result.path.c_str());
    }

    void OpenHttpRootDirectory() {
        const AppConfig value = ReadHttpDraftFromControls();
        const auto options = LocalHttpServerService::OptionsFromConfig(value, httpRootBaseDirectory_);
        std::error_code ec;
        std::filesystem::create_directories(options.rootPath, ec);
        if (ec) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"创建 Web Root 失败。", L"HTTP 服务", MB_OK | MB_ICONWARNING);
            return;
        }
        ShellExecuteW(hwnd_, L"open", options.rootPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void OpenHttpConfigDirectory() {
        const AppConfig value = ReadHttpDraftFromControls();
        const auto options = LocalHttpServerService::OptionsFromConfig(value, httpRootBaseDirectory_);
        std::wstring error;
        if (!LocalHttpServerService::EnsureDetailConfig(options.rootPath, error)) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, L"HTTP 服务", MB_OK | MB_ICONWARNING);
            return;
        }
        const std::filesystem::path configDirectory = LocalHttpServerService::DetailConfigDirectory();
        ShellExecuteW(hwnd_, L"open", configDirectory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void OpenHttpHome() {
        const std::wstring url = httpServer_ && httpServer_->IsRunning()
            ? HttpAddressText(httpServer_->options().lanAccess, httpServer_->options().port, true)
            : CurrentHttpAddress(true);
        ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void ShowToast(const std::wstring& text, ThemedToastRole role, int durationMs = 0) {
        if (!windowUi_) {
            return;
        }
        ThemedToastOptions options{};
        options.role = role;
        if (durationMs > 0) {
            options.durationMs = durationMs;
        }
        windowUi_->ui().ShowToast(text, options);
    }

    void CopyHttpUrl() {
        const std::wstring url = httpServer_ && httpServer_->IsRunning()
            ? HttpAddressText(httpServer_->options().lanAccess, httpServer_->options().port, true)
            : CurrentHttpAddress(true);
        if (!OpenClipboard(hwnd_)) {
            ShowToast(L"复制失败，剪贴板被其他程序占用。", ThemedToastRole::Danger);
            return;
        }
        EmptyClipboard();
        const SIZE_T bytes = (url.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        bool copied = false;
        if (memory) {
            void* target = GlobalLock(memory);
            if (target) {
                memcpy(target, url.c_str(), bytes);
                GlobalUnlock(memory);
                SetClipboardData(CF_UNICODETEXT, memory);
                memory = nullptr;
                copied = true;
            }
        }
        if (memory) {
            GlobalFree(memory);
        }
        CloseClipboard();
        if (copied) {
            ShowToast(L"访问地址已复制到剪贴板。", ThemedToastRole::Success);
        } else {
            ShowToast(L"复制失败。", ThemedToastRole::Danger);
        }
    }

    void StartHttpServerFromDialog(bool restart) {
        if (!httpServer_) {
            ShowThemedMessageBox(hwnd_, instance_, theme_, L"HTTP 服务对象不可用。", L"HTTP 服务", MB_OK | MB_ICONWARNING);
            return;
        }
        AppConfig value = ReadHttpDraftFromControls();
        value.httpServerEnabled = true;
        std::wstring error;
        const auto options = LocalHttpServerService::OptionsFromConfig(value, httpRootBaseDirectory_);
        const bool ok = restart ? httpServer_->Restart(options, error) : httpServer_->Start(options, error);
        UpdateHttpStatusLabel();
        if (ok) {
            ShowToast(L"HTTP 服务已启动。", ThemedToastRole::Success);
        } else {
            ShowThemedMessageBox(hwnd_, instance_, theme_, error, L"HTTP 服务", MB_OK | MB_ICONWARNING);
        }
    }

    void StopHttpServerFromDialog() {
        if (!httpServer_) {
            return;
        }
        httpServer_->Stop();
        UpdateHttpStatusLabel();
        ShowToast(L"HTTP 服务已停止。", ThemedToastRole::Info);
    }

    bool CommitSettings(bool closeAfterCommit) {
        if (webDavBusy_) {
            ShowToast(L"WebDAV 操作正在进行，请稍候完成。", ThemedToastRole::Warning);
            return false;
        }
        AppConfig next = ReadCurrentTabDraft();
        if (currentTab_ == TabHotKeys) {
            draft_.globalHotKeysEnabled = next.globalHotKeysEnabled;
        }
        if (currentTab_ == TabHotKeys && !ValidateHotKeysBeforeSave()) {
            return false;
        }
        if (currentTab_ == TabWebDav && !SaveWebDavPasswordIfNeeded(next)) {
            return false;
        }
        if (next.registerCopyPathContextMenu != config_.registerCopyPathContextMenu &&
            copyPathContextMenuCallback_) {
            std::wstring error;
            if (!copyPathContextMenuCallback_(next.registerCopyPathContextMenu, error)) {
                ShowThemedMessageBox(
                    hwnd_,
                    instance_,
                    theme_,
                    error.empty() ? L"更新资源管理器右键菜单失败。" : error,
                    L"复制绝对路径右键菜单",
                    MB_OK | MB_ICONWARNING);
                UpdateCopyPathContextMenuStatus(config_.registerCopyPathContextMenu);
                return false;
            }
        }
        if (next.registerWebDavUploadContextMenu != config_.registerWebDavUploadContextMenu &&
            webDavUploadContextMenuCallback_) {
            std::wstring error;
            if (!webDavUploadContextMenuCallback_(next.registerWebDavUploadContextMenu, error)) {
                ShowThemedMessageBox(
                    hwnd_, instance_, theme_,
                    error.empty() ? L"更新 WebDAV 上传右键菜单失败。" : error,
                    L"上传到 WebDAV 右键菜单", MB_OK | MB_ICONWARNING);
                return false;
            }
        }

        config_ = next;
        UpdateCopyPathContextMenuStatus(config_.registerCopyPathContextMenu);
        if (!closeAfterCommit && applyCallback_) {
            mainHotKeyRegistered_ = applyCallback_(config_, importedData_);
            processLocatorHotKeyRegistered_ = config_.globalHotKeysEnabled && config_.processLocatorHotKey != 0;
            copySelectedPathsHotKeyRegistered_ = config_.globalHotKeysEnabled && config_.copySelectedPathsHotKey != 0;
            importedData_ = false;
            UpdateHotKeyLabels();
            UpdateHttpStatusLabel();
            ShowToast(L"设置已应用。", ThemedToastRole::Success);
        }
        accepted_ = true;
        return true;
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT commonResult = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, commonResult)) {
            return commonResult;
        }
        switch (message) {
        case WM_CREATE: {
            RECT client{};
            GetClientRect(hwnd_, &client);
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_, owner_, hwnd_, theme_, DialogLayoutKind::Compact,
                client.right - client.left, client.bottom - client.top);
            windowUi_->SetDpiChangedCallback([this](UINT) {
                if (!contextMenuTable_) {
                    return;
                }
                RebuildContextMenuImageList();
                AddContextMenuTableRows();
            });
            CreateTabs();
            const ThemedUi settingsUi = MakeUi();
            const DialogLayoutMetrics& behaviorLayout = settingsUi.layout();
            const ThemedFormLayout behaviorForm(settingsUi);
            const int settingsClientWidth = settingsUi.clientWidth();
            const int pageLeft = behaviorLayout.contentInsetX;
            const int pageWidth = settingsClientWidth - behaviorLayout.contentInsetX * 2;
            const int pageTop = tabStripRect_.bottom + behaviorLayout.sectionGap;

            const int behaviorFrameLeft = behaviorLayout.contentInsetX;
            const int behaviorFrameRight = settingsClientWidth - behaviorLayout.contentInsetX;
            const ThemedContentInsets groupInsets = settingsUi.groupBoxInsets();
            const int behaviorFrameGap = behaviorLayout.sectionGap;
            const int behaviorFrameWidth = behaviorFrameRight - behaviorFrameLeft;
            const int behaviorCheckHeight = settingsUi.checkBoxHeight();
            const int behaviorContentLeft = behaviorFrameLeft + groupInsets.left;
            const int behaviorContentWidth = behaviorFrameRight - behaviorFrameLeft - groupInsets.left - groupInsets.right;
            const int behaviorColumnGap = behaviorLayout.controlGapX * 2;
            const int behaviorColumnWidth = (behaviorContentWidth - behaviorColumnGap) / 2;
            const int behaviorLeft = behaviorContentLeft;
            const int behaviorRight = behaviorContentLeft + behaviorColumnWidth + behaviorColumnGap;
            const int behaviorCheckWidth = behaviorColumnWidth;

            const ThemedSectionGeometry displayElementsSection = behaviorForm.section(
                behaviorFrameLeft, pageTop, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox})});
            HWND displayElementsGroup = AddSectionFrame(TabDisplay, L"界面元素", displayElementsSection.frame);
            const int displayElementsFirstY = behaviorForm.sectionItemY(displayElementsSection, 0, behaviorCheckHeight);
            const int displayElementsSecondY = behaviorForm.sectionItemY(displayElementsSection, 1, behaviorCheckHeight);
            const int displayElementsThirdY = behaviorForm.sectionItemY(displayElementsSection, 2, behaviorCheckHeight);
            showTitle_ = CheckBox(TabDisplay, 101, L"显示标题栏", behaviorLeft, displayElementsFirstY, draft_.showTitle, behaviorCheckWidth);
            showGroup_ = CheckBox(TabDisplay, 102, L"显示分组栏", behaviorRight, displayElementsFirstY, draft_.showGroup, behaviorCheckWidth);
            showTag_ = CheckBox(TabDisplay, 103, L"显示标签栏", behaviorLeft, displayElementsSecondY, draft_.showTag, behaviorCheckWidth);
            showToolboxButton_ = CheckBox(TabDisplay, 115, L"显示工具箱按钮", behaviorRight, displayElementsSecondY, draft_.showToolboxButton, behaviorCheckWidth);
            showSkinButton_ = CheckBox(TabDisplay, 121, L"显示主题按钮", behaviorLeft, displayElementsThirdY, draft_.showSkinButton, behaviorCheckWidth);
            showTooltip_ = CheckBox(TabDisplay, 119, L"显示提示", behaviorRight, displayElementsThirdY, draft_.showTooltip, behaviorCheckWidth);
            ThemedUi::BindGroupChildren(displayElementsGroup, {showTitle_, showGroup_, showTag_, showToolboxButton_, showSkinButton_, showTooltip_});

            const ThemedSectionGeometry displayLayoutSection = behaviorForm.section(
                behaviorFrameLeft, displayElementsSection.frame.bottom + behaviorFrameGap, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Label, ThemedSectionItemKind::Edit}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Label, ThemedSectionItemKind::Edit})});
            HWND displayLayoutGroup = AddSectionFrame(TabDisplay, L"布局与外观", displayLayoutSection.frame);
            const int displayLayoutFirstY = behaviorForm.sectionItemY(displayLayoutSection, 0, behaviorCheckHeight);
            const int displayLayoutSecondY = behaviorForm.sectionItemY(displayLayoutSection, 1, behaviorCheckHeight);
            linkNameSingleLine_ = CheckBox(TabDisplay, 118, L"启动项名称多行显示", behaviorLeft, displayLayoutFirstY, !draft_.linkNameSingleLine, behaviorCheckWidth);
            groupRight_ = CheckBox(TabDisplay, 120, L"分组栏在右侧", behaviorRight, displayLayoutFirstY, draft_.groupRight, behaviorCheckWidth);
            tagRight_ = CheckBox(TabDisplay, 122, L"标签栏在右侧", behaviorLeft, displayLayoutSecondY, draft_.tagRight, behaviorCheckWidth);
            linkNameBold_ = CheckBox(TabDisplay, 123, L"启动项文本加粗", behaviorRight, displayLayoutSecondY, draft_.linkNameBold, behaviorCheckWidth);

            const int displayLabelWidth = behaviorForm.labelWidthForTexts({L"透明度", L"标签文字", L"分组宽度", L"标签宽度"});
            const int displayFieldWidth = behaviorColumnWidth - displayLabelWidth - behaviorLayout.labelGap;
            const int displayThirdLabelY = behaviorForm.sectionItemY(displayLayoutSection, 2, settingsUi.labelHeight());
            const int displayThirdFieldY = behaviorForm.sectionItemY(displayLayoutSection, 2, settingsUi.editHeight());
            HWND alphaLabel = Label(TabDisplay, L"透明度", behaviorLeft, displayThirdLabelY, displayLabelWidth);
            alphaEdit_ = NumberEdit(TabDisplay, 201, behaviorLeft + displayLabelWidth + behaviorLayout.labelGap, displayThirdFieldY, displayFieldWidth, draft_.alpha);
            HWND tagAlignLabel = Label(TabDisplay, L"标签文字", behaviorRight, displayThirdLabelY, displayLabelWidth);
            const int alignButtonWidth = settingsUi.tabButtonWidth(L"左");
            const int alignX = behaviorRight + displayLabelWidth + behaviorLayout.labelGap;
            tagAlignLeft_ = settingsUi.TabButton(ID_TAG_ALIGN_LEFT, L"左", alignX, ContentY(displayThirdFieldY), alignButtonWidth, false);
            tagAlignCenter_ = settingsUi.TabButton(ID_TAG_ALIGN_CENTER, L"中", alignX + alignButtonWidth, ContentY(displayThirdFieldY), alignButtonWidth, true);
            tagAlignRight_ = settingsUi.TabButton(ID_TAG_ALIGN_RIGHT, L"右", alignX + alignButtonWidth * 2, ContentY(displayThirdFieldY), alignButtonWidth, false);
            AddTabChild(tagAlignLeft_, TabDisplay);
            AddTabChild(tagAlignCenter_, TabDisplay);
            AddTabChild(tagAlignRight_, TabDisplay);
            SelectTagAlign();
            const int displayFourthLabelY = behaviorForm.sectionItemY(displayLayoutSection, 3, settingsUi.labelHeight());
            const int displayFourthFieldY = behaviorForm.sectionItemY(displayLayoutSection, 3, settingsUi.editHeight());
            HWND groupWidthLabel = Label(TabDisplay, L"分组宽度", behaviorLeft, displayFourthLabelY, displayLabelWidth);
            groupWidthEdit_ = NumberEdit(TabDisplay, ID_GROUP_WIDTH, behaviorLeft + displayLabelWidth + behaviorLayout.labelGap, displayFourthFieldY, displayFieldWidth, draft_.groupWidth);
            HWND tagWidthLabel = Label(TabDisplay, L"标签宽度", behaviorRight, displayFourthLabelY, displayLabelWidth);
            tagWidthEdit_ = NumberEdit(TabDisplay, ID_TAG_WIDTH, behaviorRight + displayLabelWidth + behaviorLayout.labelGap, displayFourthFieldY, displayFieldWidth, draft_.tagWidth);
            ThemedUi::BindGroupChildren(displayLayoutGroup, {
                linkNameSingleLine_, groupRight_, tagRight_, linkNameBold_, alphaLabel, alphaEdit_, tagAlignLabel,
                tagAlignLeft_, tagAlignCenter_, tagAlignRight_, groupWidthLabel, groupWidthEdit_, tagWidthLabel, tagWidthEdit_});

            const int behaviorDelayLabelWidth = behaviorForm.labelWidthForText(L"停靠延迟");
            const int behaviorUnitWidth = behaviorForm.labelWidthForText(L"ms");
            const int behaviorFieldWidth = behaviorColumnWidth - behaviorDelayLabelWidth - behaviorLayout.labelGap
                - behaviorLayout.controlGapX - behaviorUnitWidth;
            const int behaviorWindowFrameTop = tabStripRect_.bottom + behaviorLayout.sectionGap;
            const ThemedSectionGeometry behaviorWindowSection = behaviorForm.section(
                behaviorFrameLeft, behaviorWindowFrameTop, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox, ThemedSectionItemKind::Label, ThemedSectionItemKind::Edit}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox, ThemedSectionItemKind::CheckBox})});
            HWND behaviorWindowGroup = AddSectionFrame(TabBehavior, L"窗口行为", behaviorWindowSection.frame);
            const int behaviorWindowCheckY = behaviorForm.sectionItemY(behaviorWindowSection, 0, behaviorCheckHeight);
            const int behaviorWindowLabelY = behaviorForm.sectionItemY(behaviorWindowSection, 0, settingsUi.labelHeight());
            const int behaviorWindowEditY = behaviorForm.sectionItemY(behaviorWindowSection, 0, settingsUi.editHeight());
            autoDock_ = CheckBox(TabBehavior, 105, L"贴边自动隐藏", behaviorLeft, behaviorWindowCheckY, draft_.autoDock, behaviorCheckWidth);
            HWND dockDelayLabel = Label(TabBehavior, L"停靠延迟", behaviorRight, behaviorWindowLabelY, behaviorDelayLabelWidth);
            dockDelayEdit_ = NumberEdit(
                TabBehavior,
                ID_DOCK_DELAY,
                behaviorRight + behaviorDelayLabelWidth + behaviorLayout.labelGap,
                behaviorWindowEditY,
                behaviorFieldWidth,
                draft_.dockDelay);
            HWND dockDelayUnit = Label(
                TabBehavior,
                L"ms",
                behaviorRight + behaviorDelayLabelWidth + behaviorLayout.labelGap + behaviorFieldWidth + behaviorLayout.controlGapX,
                behaviorWindowLabelY,
                behaviorUnitWidth);
            hideInactive_ = CheckBox(
                TabBehavior, 106, L"失焦隐藏", behaviorLeft,
                behaviorForm.sectionItemY(behaviorWindowSection, 1, behaviorCheckHeight),
                draft_.hideWhenInactive, behaviorCheckWidth);
            hideMainAfterToolOpen_ = CheckBox(
                TabBehavior, ID_HIDE_MAIN_AFTER_TOOL_OPEN, L"未贴边时，打开工具后隐藏主窗口",
                behaviorRight, behaviorForm.sectionItemY(behaviorWindowSection, 1, behaviorCheckHeight),
                draft_.hideMainAfterToolOpen, behaviorColumnWidth);
            ThemedUi::BindGroupChildren(
                behaviorWindowGroup,
                {autoDock_, dockDelayLabel, dockDelayEdit_, dockDelayUnit, hideInactive_, hideMainAfterToolOpen_});

            const int behaviorRunFrameTop = behaviorWindowSection.frame.bottom + behaviorFrameGap;
            const ThemedSectionGeometry behaviorRunSection = behaviorForm.section(
                behaviorFrameLeft, behaviorRunFrameTop, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox})});
            HWND behaviorRunGroup = AddSectionFrame(TabBehavior, L"运行与数据", behaviorRunSection.frame);
            const int behaviorRunFirstY = behaviorForm.sectionItemY(behaviorRunSection, 0, behaviorCheckHeight);
            const int behaviorRunColumnWidth = behaviorContentWidth / 3;
            hideAfterLink_ = CheckBox(TabBehavior, 107, L"启动项运行后隐藏", behaviorLeft, behaviorRunFirstY, draft_.hideAfterLink, behaviorRunColumnWidth);
            saveRunCount_ = CheckBox(TabBehavior, 112, L"记录运行次数", behaviorLeft + behaviorRunColumnWidth, behaviorRunFirstY, draft_.saveRunCount, behaviorRunColumnWidth);
            deleteConfirm_ = CheckBox(TabBehavior, 111, L"删除前确认", behaviorLeft + behaviorRunColumnWidth * 2, behaviorRunFirstY, draft_.deleteConfirm, behaviorContentWidth - behaviorRunColumnWidth * 2);
            ThemedUi::BindGroupChildren(behaviorRunGroup, {hideAfterLink_, saveRunCount_, deleteConfirm_});

            const int behaviorSystemFrameTop = behaviorRunSection.frame.bottom + behaviorFrameGap;
            const ThemedSectionGeometry behaviorSystemSection = behaviorForm.section(
                behaviorFrameLeft, behaviorSystemFrameTop, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox})});
            HWND behaviorSystemGroup = AddSectionFrame(TabBehavior, L"系统集成", behaviorSystemSection.frame);
            const int behaviorSystemFirstY = behaviorForm.sectionItemY(behaviorSystemSection, 0, behaviorCheckHeight);
            hideOnStart_ = CheckBox(TabBehavior, 116, L"启动后隐藏", behaviorLeft, behaviorSystemFirstY, draft_.hideOnStart, behaviorRunColumnWidth);
            autoRun_ = CheckBox(TabBehavior, 117, L"开机启动", behaviorLeft + behaviorRunColumnWidth, behaviorSystemFirstY, draft_.autoRun, behaviorRunColumnWidth);
            loggingEnabled_ = CheckBox(TabBehavior, ID_LOGGING_ENABLED, L"启用日志", behaviorLeft + behaviorRunColumnWidth * 2, behaviorSystemFirstY, draft_.loggingEnabled, behaviorContentWidth - behaviorRunColumnWidth * 2);
            ThemedUi::BindGroupChildren(behaviorSystemGroup, {hideOnStart_, autoRun_, loggingEnabled_});

            const ThemedSectionGeometry contextMenuIntegrationSection = behaviorForm.section(
                behaviorFrameLeft, pageTop, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox, ThemedSectionItemKind::Text})});
            HWND contextMenuIntegrationGroup = AddSectionFrame(
                TabContextMenu, L"系统集成", contextMenuIntegrationSection.frame);
            const std::wstring copyPathContextMenuText = L"注册“复制绝对路径”右键菜单";
            const int copyPathContextMenuWidth = std::min(
                behaviorContentWidth,
                settingsUi.textWidth(copyPathContextMenuText) + settingsUi.checkBoxHeight()
                    + behaviorLayout.controlGapX);
            const int copyPathContextMenuStatusX =
                behaviorLeft + copyPathContextMenuWidth + behaviorLayout.controlGapX;
            registerCopyPathContextMenu_ = CheckBox(
                TabContextMenu,
                ID_REGISTER_COPY_PATH_CONTEXT_MENU,
                copyPathContextMenuText.c_str(),
                behaviorLeft,
                behaviorForm.sectionItemY(contextMenuIntegrationSection, 0, behaviorCheckHeight),
                draft_.registerCopyPathContextMenu,
                copyPathContextMenuWidth);
            copyPathContextMenuStatus_ = StatusText(
                TabContextMenu,
                draft_.registerCopyPathContextMenu ? L"已注册；Quattro 未运行时也可使用。" : L"未注册。",
                copyPathContextMenuStatusX,
                behaviorForm.sectionItemY(contextMenuIntegrationSection, 0, settingsUi.labelHeight()),
                std::max(0, behaviorFrameRight - groupInsets.right - copyPathContextMenuStatusX),
                draft_.registerCopyPathContextMenu ? ThemedStatusRole::Success : ThemedStatusRole::Normal);
            ThemedUi::BindGroupChildren(
                contextMenuIntegrationGroup, {registerCopyPathContextMenu_, copyPathContextMenuStatus_});

            // 表格固定显示 5 行高度，更多 provider 由 ListView 垂直滚动承载，
            // 对话框高度不随 provider 数量增长。
            constexpr int kContextMenuVisibleRows = 5;
            const int contextMenuTableHeight = settingsUi.tableHeightForRows(kContextMenuVisibleRows, false);
            const ThemedSectionGeometry contextMenuTrackingSection = behaviorForm.contentSection(
                behaviorFrameLeft,
                contextMenuIntegrationSection.frame.bottom + behaviorFrameGap,
                behaviorFrameWidth,
                contextMenuTableHeight);
            HWND contextMenuTrackingGroup = AddSectionFrame(TabContextMenu, L"自动跟踪", contextMenuTrackingSection.frame);
            RECT contextMenuTableFrame{
                contextMenuTrackingSection.content.left,
                ContentY(contextMenuTrackingSection.content.top),
                contextMenuTrackingSection.content.right,
                ContentY(contextMenuTrackingSection.content.bottom),
            };
            ThemedTableOptions contextMenuTableOptions{};
            contextMenuTableOptions.checkable = true;
            contextMenuTableOptions.showHeader = false;
            contextMenuTableOptions.reserveScrollBarGutter = true;
            contextMenuTableOptions.fullRowSelect = true;
            contextMenuTable_ = MakeUi().Table(
                ID_CONTEXT_MENU_TABLE,
                contextMenuTableFrame,
                {
                    ThemedTableColumn{
                        L"tool",
                        L"工具",
                        ThemedTableColumnAlign::Start,
                        ThemedTableColumnWidth::Remaining},
                    ThemedTableColumn{
                        L"status",
                        L"状态",
                        ThemedTableColumnAlign::End,
                        ThemedTableColumnWidth::Fixed,
                        settingsUi.tableColumnWidth({L"已安装(注册表)", L"已安装(菜单)", L"未安装"})},
                },
                contextMenuTableOptions);
            AddTabChild(contextMenuTable_, TabContextMenu);
            RebuildContextMenuImageList();
            AddContextMenuTableRows();
            ThemedUi::BindGroupChildren(contextMenuTrackingGroup, {contextMenuTable_});

            const int resetContextMenuWidth = settingsUi.buttonWidth(
                L"重置右键菜单",
                ThemedButtonRole::Normal,
                ThemedButtonSize::Normal,
                ThemedButtonWidthMode::Text);
            const int refreshContextMenuWidth = settingsUi.buttonWidth(
                L"从Windows菜单刷新",
                ThemedButtonRole::Normal,
                ThemedButtonSize::Normal,
                ThemedButtonWidthMode::Text);
            const ThemedSectionGeometry contextMenuMaintenanceSection = behaviorForm.section(
                behaviorFrameLeft, contextMenuTrackingSection.frame.bottom + behaviorFrameGap, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::Button, ThemedSectionItemKind::Button})});
            HWND contextMenuMaintenanceGroup = AddSectionFrame(TabContextMenu, L"缓存维护", contextMenuMaintenanceSection.frame);
            const int contextMenuMaintenanceY = behaviorForm.sectionItemY(contextMenuMaintenanceSection, 0, settingsUi.buttonHeight());
            resetContextMenuButton_ = Button(
                TabContextMenu,
                ID_RESET_CONTEXT_MENU,
                L"重置右键菜单",
                behaviorLeft,
                contextMenuMaintenanceY,
                resetContextMenuWidth);
            refreshContextMenuButton_ = Button(
                TabContextMenu,
                ID_REFRESH_CONTEXT_MENU_FROM_NATIVE,
                L"从Windows菜单刷新",
                behaviorLeft + resetContextMenuWidth + behaviorLayout.controlGapX,
                contextMenuMaintenanceY,
                refreshContextMenuWidth);
            ThemedTooltipOptions resetContextMenuTooltipOptions{};
            resetContextMenuTooltipOptions.placement = ThemedTooltipPlacement::Cursor;
            settingsUi.SetTooltip(
                resetContextMenuButton_,
                L"恢复跟踪开关默认值，并清除全部菜单列表、状态与图标缓存。",
                resetContextMenuTooltipOptions);
            ThemedTooltipOptions refreshContextMenuTooltipOptions{};
            refreshContextMenuTooltipOptions.placement = ThemedTooltipPlacement::Cursor;
            settingsUi.SetTooltip(
                refreshContextMenuButton_,
                L"扫描Windows原生菜单，增量更新所有启用的工具菜单和图标。",
                refreshContextMenuTooltipOptions);
            ThemedUi::BindGroupChildren(contextMenuMaintenanceGroup, {resetContextMenuButton_, refreshContextMenuButton_});

            const ThemedSectionGeometry interactionLaunchSection = behaviorForm.section(
                behaviorFrameLeft, pageTop, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox})});
            HWND interactionLaunchGroup = AddSectionFrame(TabInteraction, L"启动操作", interactionLaunchSection.frame);
            doubleClick_ = CheckBox(
                TabInteraction, 109, L"双击运行", behaviorLeft,
                behaviorForm.sectionItemY(interactionLaunchSection, 0, behaviorCheckHeight),
                draft_.doubleClickToRun, behaviorContentWidth);
            ThemedUi::BindGroupChildren(interactionLaunchGroup, {doubleClick_});

            const ThemedSectionGeometry interactionHoverSection = behaviorForm.section(
                behaviorFrameLeft, interactionLaunchSection.frame.bottom + behaviorFrameGap, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Label, ThemedSectionItemKind::Edit})});
            HWND interactionHoverGroup = AddSectionFrame(TabInteraction, L"悬停激活", interactionHoverSection.frame);
            const int interactionHoverCheckY = behaviorForm.sectionItemY(interactionHoverSection, 0, behaviorCheckHeight);
            enterActiveGroup_ = CheckBox(TabInteraction, 124, L"鼠标进入激活分组", behaviorLeft, interactionHoverCheckY, draft_.mouseEnterActiveGroup, behaviorCheckWidth);
            enterActiveTag_ = CheckBox(TabInteraction, 125, L"鼠标进入激活标签", behaviorRight, interactionHoverCheckY, draft_.mouseEnterActiveTag, behaviorCheckWidth);
            const int delayLabelWidth = behaviorForm.labelWidthForTexts({L"分组激活延迟", L"标签激活延迟"});
            const int unitWidth = behaviorForm.labelWidthForText(L"ms");
            const int delayFieldWidth = behaviorColumnWidth - delayLabelWidth - behaviorLayout.labelGap - behaviorLayout.controlGapX - unitWidth;
            const int interactionDelayLabelY = behaviorForm.sectionItemY(interactionHoverSection, 1, settingsUi.labelHeight());
            const int interactionDelayEditY = behaviorForm.sectionItemY(interactionHoverSection, 1, settingsUi.editHeight());
            HWND groupDelayLabel = Label(TabInteraction, L"分组激活延迟", behaviorLeft, interactionDelayLabelY, delayLabelWidth);
            groupDelayEdit_ = NumberEdit(TabInteraction, ID_GROUP_DELAY, behaviorLeft + delayLabelWidth + behaviorLayout.labelGap, interactionDelayEditY, delayFieldWidth, draft_.activeGroupDelay);
            HWND groupDelayUnit = Label(TabInteraction, L"ms", behaviorLeft + behaviorColumnWidth - unitWidth, interactionDelayLabelY, unitWidth);
            HWND tagDelayLabel = Label(TabInteraction, L"标签激活延迟", behaviorRight, interactionDelayLabelY, delayLabelWidth);
            tagDelayEdit_ = NumberEdit(TabInteraction, ID_TAG_DELAY, behaviorRight + delayLabelWidth + behaviorLayout.labelGap, interactionDelayEditY, delayFieldWidth, draft_.activeTagDelay);
            HWND tagDelayUnit = Label(TabInteraction, L"ms", behaviorRight + behaviorColumnWidth - unitWidth, interactionDelayLabelY, unitWidth);
            ThemedUi::BindGroupChildren(interactionHoverGroup, {
                enterActiveGroup_, enterActiveTag_, groupDelayLabel, groupDelayEdit_, groupDelayUnit,
                tagDelayLabel, tagDelayEdit_, tagDelayUnit});

            const int hotKeyPageBottom = settingsUi.footerButtonY(settingsUi.footerButtonHeight()) - behaviorLayout.footerGap;
            const RECT hotKeyGroupFrame{
                behaviorFrameLeft,
                pageTop,
                behaviorFrameLeft + behaviorFrameWidth,
                hotKeyPageBottom,
            };
            const RECT hotKeyContent{
                hotKeyGroupFrame.left + groupInsets.left,
                hotKeyGroupFrame.top + groupInsets.top,
                hotKeyGroupFrame.right - groupInsets.right,
                hotKeyGroupFrame.bottom - groupInsets.bottom,
            };
            HWND hotKeyGroup = AddSectionFrame(TabHotKeys, L"全局快捷键", hotKeyGroupFrame);
            const int hotKeyToggleY = hotKeyContent.top;
            const int resetDefaultHotKeysWidth = settingsUi.buttonWidth(
                L"重置默认热键",
                ThemedButtonRole::Normal,
                ThemedButtonSize::Normal,
                ThemedButtonWidthMode::Text);
            globalHotKeysEnabled_ = Toggle(
                TabHotKeys, ID_GLOBAL_HOTKEYS_ENABLED, L"启用全局快捷键", behaviorLeft,
                hotKeyToggleY, draft_.globalHotKeysEnabled,
                behaviorContentWidth - resetDefaultHotKeysWidth - behaviorLayout.controlGapX);
            resetDefaultHotKeysButton_ = Button(
                TabHotKeys,
                ID_RESET_DEFAULT_HOTKEYS,
                L"重置默认热键",
                behaviorLeft + behaviorContentWidth - resetDefaultHotKeysWidth,
                hotKeyToggleY + (settingsUi.toggleHeight() - settingsUi.buttonHeight()) / 2,
                resetDefaultHotKeysWidth);
            ThemedTooltipOptions resetDefaultHotKeysTooltipOptions{};
            resetDefaultHotKeysTooltipOptions.placement = ThemedTooltipPlacement::Cursor;
            settingsUi.SetTooltip(
                resetDefaultHotKeysButton_,
                L"恢复主窗口、进程定位器和复制路径的默认热键；启用状态保持不变。",
                resetDefaultHotKeysTooltipOptions);
            const int hotKeyStatusY = hotKeyContent.bottom - settingsUi.labelHeight();
            const int hotKeyTableTop = hotKeyToggleY + settingsUi.toggleHeight() + behaviorLayout.rowGap;
            const int hotKeyTableBottom = hotKeyStatusY - behaviorLayout.rowGap;
            RECT hotKeyTableFrame{
                behaviorLeft,
                ContentY(hotKeyTableTop),
                behaviorLeft + behaviorContentWidth,
                ContentY(hotKeyTableBottom),
            };
            hotKeyTable_ = MakeUi().Table(
                ID_HOTKEY_TABLE,
                hotKeyTableFrame,
                {
                    ThemedTableColumn{
                        L"function",
                        L"功能",
                        ThemedTableColumnAlign::Start,
                        ThemedTableColumnWidth::Remaining},
                    ThemedTableColumn{
                        L"hotkey",
                        L"快捷键",
                        ThemedTableColumnAlign::Start,
                        ThemedTableColumnWidth::Fixed,
                        settingsUi.tableColumnWidth({L"快捷键", L"Ctrl+Alt+Page Down", L"双击 Alt", L"未设置"})},
                    ThemedTableColumn{
                        L"action",
                        L"操作",
                        ThemedTableColumnAlign::Start,
                        ThemedTableColumnWidth::Fixed,
                        settingsUi.buttonWidth(
                            L"录入",
                            ThemedButtonRole::Normal,
                            ThemedButtonSize::Normal,
                            ThemedButtonWidthMode::Text) + behaviorLayout.controlGapX},
                });
            AddTabChild(hotKeyTable_, TabHotKeys);
            mainHotKeyStatus_ = Label(
                TabHotKeys, L"", behaviorLeft,
                hotKeyStatusY, behaviorContentWidth);
            ThemedUi::BindGroupChildren(hotKeyGroup, {
                globalHotKeysEnabled_, resetDefaultHotKeysButton_, hotKeyTable_, mainHotKeyStatus_});
            UpdateHotKeyLabels();

            const ThemedSectionGeometry directoryCommandSection = behaviorForm.section(
                behaviorFrameLeft, pageTop, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::Label}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Edit})});
            HWND directoryCommandGroup = AddSectionFrame(TabLinks, L"目录命令", directoryCommandSection.frame);
            HWND openDirLabel = Label(
                TabLinks, L"打开目录命令", behaviorLeft,
                behaviorForm.sectionItemY(directoryCommandSection, 0, settingsUi.labelHeight()), behaviorContentWidth);
            openDirEdit_ = FramedEdit(
                TabLinks, 202, behaviorLeft,
                behaviorForm.sectionItemY(directoryCommandSection, 1, settingsUi.editHeight()), behaviorContentWidth, draft_.openDirCommand);
            ThemedUi::BindGroupChildren(directoryCommandGroup, {openDirLabel, openDirEdit_});

            const ThemedSectionGeometry publicLinksSection = behaviorForm.section(
                behaviorFrameLeft, directoryCommandSection.frame.bottom + behaviorFrameGap, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::Label}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Edit})});
            HWND publicLinksGroup = AddSectionFrame(TabLinks, L"公共链接", publicLinksSection.frame);
            HWND updateUrlLabel = Label(TabLinks, L"更新链接", behaviorLeft, behaviorForm.sectionItemY(publicLinksSection, 0, settingsUi.labelHeight()), behaviorContentWidth);
            updateUrlEdit_ = FramedEdit(TabLinks, 204, behaviorLeft, behaviorForm.sectionItemY(publicLinksSection, 1, settingsUi.editHeight()), behaviorContentWidth, draft_.updateUrl);
            ThemedUi::BindGroupChildren(publicLinksGroup, {updateUrlLabel, updateUrlEdit_});

            const int uploadWidth = settingsUi.buttonWidth(L"上传到云端", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const int downloadWidth = settingsUi.buttonWidth(L"从云端下载", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const int testWidth = settingsUi.buttonWidth(L"测试连接", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const int clearWidth = settingsUi.buttonWidth(L"清除密码", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const ThemedSectionGeometry webDavSection = behaviorForm.section(
                behaviorFrameLeft, pageTop, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Label, ThemedSectionItemKind::Edit}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Label}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Edit}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Label}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Edit}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Label}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Edit}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Button}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Button})});
            HWND webDavGroup = AddSectionFrame(TabWebDav, L"WebDAV 备份", webDavSection.frame);
            webDavEnabled_ = CheckBox(TabWebDav, 208, L"启用 WebDAV 备份", behaviorLeft, behaviorForm.sectionItemY(webDavSection, 0, behaviorCheckHeight), draft_.webDavEnabled, behaviorCheckWidth);
            webDavUploadContextMenu_ = CheckBox(TabWebDav, ID_WEBDAV_UPLOAD_CONTEXT_MENU, L"注册“上传到 WebDAV”右键菜单", behaviorRight, behaviorForm.sectionItemY(webDavSection, 0, behaviorCheckHeight), draft_.registerWebDavUploadContextMenu, behaviorCheckWidth);
            webDavLastSyncLabel_ = Label(TabWebDav, L"", behaviorRight, behaviorForm.sectionItemY(webDavSection, 0, settingsUi.labelHeight()), behaviorCheckWidth);
            UpdateWebDavLastSyncLabel();
            const int webDavServerLabelWidth = behaviorForm.labelWidthForText(L"服务器地址");
            const int webDavServerFieldX = behaviorLeft + webDavServerLabelWidth + behaviorLayout.labelGap;
            HWND webDavUrlLabel = Label(TabWebDav, L"服务器地址", behaviorLeft, behaviorForm.sectionItemY(webDavSection, 1, settingsUi.labelHeight()), webDavServerLabelWidth);
            webDavUrlEdit_ = FramedEdit(TabWebDav, 209, webDavServerFieldX, behaviorForm.sectionItemY(webDavSection, 1, settingsUi.editHeight()), behaviorContentWidth - webDavServerLabelWidth - behaviorLayout.labelGap, draft_.webDavUrl);
            HWND webDavUserLabel = Label(TabWebDav, L"用户名", behaviorLeft, behaviorForm.sectionItemY(webDavSection, 2, settingsUi.labelHeight()), behaviorCheckWidth);
            HWND webDavPasswordLabel = Label(TabWebDav, L"密码/应用密码", behaviorRight, behaviorForm.sectionItemY(webDavSection, 2, settingsUi.labelHeight()), behaviorCheckWidth);
            webDavUserNameEdit_ = FramedEdit(TabWebDav, 212, behaviorLeft, behaviorForm.sectionItemY(webDavSection, 3, settingsUi.editHeight()), behaviorCheckWidth, draft_.webDavUserName);
            ThemedEditOptions passwordOptions{};
            passwordOptions.content = ThemedEditContent::Password;
            webDavPasswordEdit_ = FramedEdit(TabWebDav, 213, behaviorRight, behaviorForm.sectionItemY(webDavSection, 3, settingsUi.editHeight()), behaviorCheckWidth, L"", passwordOptions);
            HWND webDavBackupLabel = Label(TabWebDav, L"备份目录", behaviorLeft, behaviorForm.sectionItemY(webDavSection, 4, settingsUi.labelHeight()), behaviorCheckWidth);
            HWND webDavKeepLabel = Label(TabWebDav, L"保留数量", behaviorRight, behaviorForm.sectionItemY(webDavSection, 4, settingsUi.labelHeight()), behaviorCheckWidth);
            webDavBackupPathEdit_ = FramedEdit(TabWebDav, 210, behaviorLeft, behaviorForm.sectionItemY(webDavSection, 5, settingsUi.editHeight()), behaviorCheckWidth, draft_.webDavBackupPath);
            webDavKeepCountEdit_ = NumberEdit(TabWebDav, 211, behaviorRight, behaviorForm.sectionItemY(webDavSection, 5, settingsUi.editHeight()), behaviorCheckWidth, draft_.webDavKeepCount);
            const int webDavButtonsWidth =
                testWidth + behaviorLayout.controlGapX + clearWidth + behaviorLayout.controlGapX
                + uploadWidth + behaviorLayout.controlGapX + downloadWidth;
            const int webDavButtonsX = settingsUi.centeredGroupX(webDavButtonsWidth);
            HWND webDavFilesLabel = Label(TabWebDav, L"文件目录", behaviorLeft, behaviorForm.sectionItemY(webDavSection, 6, settingsUi.labelHeight()), behaviorContentWidth);
            webDavFilesPathEdit_ = FramedEdit(TabWebDav, 219, behaviorLeft, behaviorForm.sectionItemY(webDavSection, 7, settingsUi.editHeight()), behaviorContentWidth, draft_.webDavFilesPath);
            const int webDavButtonsY = behaviorForm.sectionItemY(webDavSection, 8, settingsUi.buttonHeight());
            webDavTestButton_ = Button(TabWebDav, ID_WEBDAV_TEST, L"测试连接", webDavButtonsX, webDavButtonsY, testWidth);
            webDavClearPasswordButton_ = Button(TabWebDav, ID_WEBDAV_CLEAR_PASSWORD, L"清除密码", webDavButtonsX + testWidth + behaviorLayout.controlGapX, webDavButtonsY, clearWidth);
            webDavUploadButton_ = Button(TabWebDav, ID_WEBDAV_UPLOAD, L"上传到云端", webDavButtonsX + testWidth + behaviorLayout.controlGapX + clearWidth + behaviorLayout.controlGapX, webDavButtonsY, uploadWidth);
            webDavDownloadButton_ = Button(TabWebDav, ID_WEBDAV_DOWNLOAD, L"从云端下载", webDavButtonsX + testWidth + behaviorLayout.controlGapX + clearWidth + behaviorLayout.controlGapX + uploadWidth + behaviorLayout.controlGapX, webDavButtonsY, downloadWidth);
            const int fileManagerWidth = settingsUi.buttonWidth(L"打开文件管理", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            webDavFileManagerButton_ = Button(TabWebDav, ID_WEBDAV_FILE_MANAGER, L"打开文件管理", settingsUi.centeredGroupX(fileManagerWidth), behaviorForm.sectionItemY(webDavSection, 9, settingsUi.buttonHeight()), fileManagerWidth);
            ThemedUi::BindGroupChildren(webDavGroup, {
                webDavEnabled_, webDavUploadContextMenu_, webDavLastSyncLabel_, webDavUrlLabel, webDavUrlEdit_, webDavUserLabel,
                webDavPasswordLabel, webDavUserNameEdit_, webDavPasswordEdit_, webDavTestButton_,
                webDavClearPasswordButton_, webDavBackupLabel, webDavKeepLabel,
                webDavBackupPathEdit_, webDavKeepCountEdit_, webDavFilesLabel, webDavFilesPathEdit_, webDavUploadButton_, webDavDownloadButton_, webDavFileManagerButton_});

            const DialogLayoutMetrics& httpLayout = settingsUi.layout();
            const int httpPanelPaddingX = groupInsets.left;
            const int httpFrameLeft = pageLeft;
            const int httpFrameRight = pageLeft + pageWidth;
            const int httpContentLeft = httpFrameLeft + httpPanelPaddingX;
            const int httpContentRight = httpFrameRight - httpPanelPaddingX;
            const int httpLabelWidth = behaviorForm.labelWidthForTexts({L"站点网址", L"绑定磁盘路径"});
            const int httpFieldX = httpContentLeft + httpLabelWidth + httpLayout.labelGap;
            const int httpFrameGap = httpLayout.sectionGap;
            const int httpFrameWidth = httpFrameRight - httpFrameLeft;
            const int httpCheckHeight = settingsUi.checkBoxHeight();
            const int httpEditHeight = settingsUi.editHeight();
            const int httpLabelHeight = settingsUi.labelHeight();
            const int httpButtonHeight = settingsUi.buttonHeight();

            const int httpConfigFrameTop = tabStripRect_.bottom + httpLayout.sectionGap;
            const ThemedSectionGeometry httpServiceSection = behaviorForm.section(
                httpFrameLeft, httpConfigFrameTop, httpFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Label, ThemedSectionItemKind::Edit}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Label, ThemedSectionItemKind::Edit, ThemedSectionItemKind::Button})});
            HWND httpServiceGroup = AddSectionFrame(TabHttp, L"服务配置", httpServiceSection.frame);
            const int httpBrowseWidth = settingsUi.buttonWidth(L"选择", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const int httpOpenRootWidth = settingsUi.buttonWidth(L"打开目录", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const int httpFieldWidth = httpContentRight - httpFieldX - httpLayout.controlGapX * 2 - httpBrowseWidth - httpOpenRootWidth;
            const int httpAutoStartY = behaviorForm.sectionItemY(httpServiceSection, 0, httpCheckHeight);
            httpServerAutoStart_ = CheckBox(TabHttp, 215, L"随应用启动", httpContentLeft, httpAutoStartY, draft_.httpServerAutoStart, httpContentRight - httpContentLeft);
            const int httpAddressLabelY = behaviorForm.sectionItemY(httpServiceSection, 1, httpLabelHeight);
            const int httpAddressEditY = behaviorForm.sectionItemY(httpServiceSection, 1, httpEditHeight);
            const int httpRootLabelY = behaviorForm.sectionItemY(httpServiceSection, 2, httpLabelHeight);
            const int httpRootEditY = behaviorForm.sectionItemY(httpServiceSection, 2, httpEditHeight);
            const int httpRootButtonY = behaviorForm.sectionItemY(httpServiceSection, 2, httpButtonHeight);
            HWND httpAddressLabel = Label(TabHttp, L"站点网址", httpContentLeft, httpAddressLabelY, httpLabelWidth);
            httpServerAddressEdit_ = FramedEdit(
                TabHttp,
                ID_HTTP_ADDRESS,
                httpFieldX,
                httpAddressEditY,
                httpFieldWidth,
                HttpAddressText(true, draft_.httpServerPort, false));
            HWND httpRootLabel = Label(TabHttp, L"绑定磁盘路径", httpContentLeft, httpRootLabelY, httpLabelWidth);
            httpServerRootEdit_ = FramedEdit(
                TabHttp,
                218,
                httpFieldX,
                httpRootEditY,
                httpFieldWidth,
                Trim(draft_.httpServerRootPath).empty() ? LocalHttpServerService::DefaultRootPath(httpRootBaseDirectory_).wstring() : draft_.httpServerRootPath);
            const int httpBrowseX = httpFieldX + httpFieldWidth + httpLayout.controlGapX;
            httpBrowseRootButton_ = Button(TabHttp, ID_HTTP_BROWSE_ROOT, L"选择", httpBrowseX, httpRootButtonY, httpBrowseWidth);
            HWND httpOpenRootButton = Button(TabHttp, ID_HTTP_OPEN_ROOT, L"打开目录", httpBrowseX + httpBrowseWidth + httpLayout.controlGapX, httpRootButtonY, httpOpenRootWidth);
            ThemedUi::BindGroupChildren(httpServiceGroup, {
                httpServerAutoStart_, httpAddressLabel, httpServerAddressEdit_, httpRootLabel,
                httpServerRootEdit_, httpBrowseRootButton_, httpOpenRootButton});

            const int httpControlFrameTop = httpServiceSection.frame.bottom + httpFrameGap;
            const ThemedSectionGeometry httpControlSection = behaviorForm.section(
                httpFrameLeft, httpControlFrameTop, httpFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::Label, ThemedSectionItemKind::StatusBadge, ThemedSectionItemKind::Text}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Button})});
            HWND httpControlGroup = AddSectionFrame(TabHttp, L"运行控制", httpControlSection.frame);
            const int httpStatusY = behaviorForm.sectionItemY(httpControlSection, 0, httpLabelHeight);
            const int httpButtonY = behaviorForm.sectionItemY(httpControlSection, 1, httpButtonHeight);
            const int httpStatusLabelWidth = behaviorForm.labelWidthForText(L"状态");
            HWND httpStatusLabel = Label(TabHttp, L"状态", httpContentLeft, httpStatusY, httpStatusLabelWidth);
            httpServerStatusTag_ = StatusBadge(
                TabHttp, L"", httpContentLeft + httpStatusLabelWidth + httpLayout.controlGapX, httpStatusY, settingsUi.textWidth(L"未运行") + httpLayout.controlGapX * 2, ThemedStatusRole::Danger);
            const int httpStatusDetailX = httpContentLeft + httpStatusLabelWidth + httpLayout.controlGapX * 2 + settingsUi.textWidth(L"未运行") + httpLayout.controlGapX * 2;
            httpServerStatusDetail_ = Label(TabHttp, L"", httpStatusDetailX, httpStatusY, httpContentRight - httpStatusDetailX);
            const int httpStartWidth = settingsUi.buttonWidth(L"启动", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const int httpStopWidth = settingsUi.buttonWidth(L"停止", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const int httpRestartWidth = settingsUi.buttonWidth(L"重启", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const int httpHomeWidth = settingsUi.buttonWidth(L"打开网站", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const int httpCopyWidth = settingsUi.buttonWidth(L"复制地址", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            int httpButtonX = httpContentLeft;
            httpStartButton_ = Button(TabHttp, ID_HTTP_START, L"启动", httpButtonX, httpButtonY, httpStartWidth);
            httpButtonX += httpStartWidth + httpLayout.controlGapX;
            httpStopButton_ = Button(TabHttp, ID_HTTP_STOP, L"停止", httpButtonX, httpButtonY, httpStopWidth);
            httpButtonX += httpStopWidth + httpLayout.controlGapX;
            httpRestartButton_ = Button(TabHttp, ID_HTTP_RESTART, L"重启", httpButtonX, httpButtonY, httpRestartWidth);
            httpButtonX += httpRestartWidth + httpLayout.controlGapX;
            HWND httpHomeButton = Button(TabHttp, ID_HTTP_OPEN_HOME, L"打开网站", httpButtonX, httpButtonY, httpHomeWidth);
            httpButtonX += httpHomeWidth + httpLayout.controlGapX;
            HWND httpCopyButton = Button(TabHttp, ID_HTTP_COPY_URL, L"复制地址", httpButtonX, httpButtonY, httpCopyWidth);
            ThemedUi::BindGroupChildren(httpControlGroup, {
                httpStatusLabel, httpServerStatusTag_, httpServerStatusDetail_, httpStartButton_, httpStopButton_,
                httpRestartButton_, httpHomeButton, httpCopyButton});

            const ThemedSectionGeometry httpAdvancedSection = behaviorForm.section(
                httpFrameLeft, httpControlSection.frame.bottom + httpFrameGap, httpFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::Button, ThemedSectionItemKind::Text})});
            HWND httpAdvancedGroup = AddSectionFrame(TabHttp, L"高级配置", httpAdvancedSection.frame);
            const int httpConfigY = behaviorForm.sectionItemY(httpAdvancedSection, 0, httpButtonHeight);
            const int httpConfigWidth = settingsUi.buttonWidth(L"配置目录", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            HWND httpConfigButton = Button(TabHttp, ID_HTTP_OPEN_CONFIG_DIR, L"配置目录", httpContentLeft, httpConfigY, httpConfigWidth);
            HWND httpConfigNote = Label(TabHttp, L"权限、账号、MIME 与下载策略在配置目录修改，重启后生效。", httpContentLeft + httpConfigWidth + httpLayout.controlGapX, httpConfigY, httpContentRight - httpContentLeft - httpConfigWidth - httpLayout.controlGapX);
            ThemedUi::BindGroupChildren(httpAdvancedGroup, {httpConfigButton, httpConfigNote});
            UpdateHttpStatusLabel();

            const int configExportWidth = settingsUi.buttonWidth(L"导出配置包", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const int configImportWidth = settingsUi.buttonWidth(L"导入配置包", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const int configButtonsX = behaviorLeft;
            const ThemedSectionGeometry configBackupSection = behaviorForm.section(
                behaviorFrameLeft, pageTop, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::Button})});
            HWND configBackupGroup = AddSectionFrame(TabBackup, L"配置包", configBackupSection.frame);
            const int configButtonsY = behaviorForm.sectionItemY(configBackupSection, 0, settingsUi.buttonHeight());
            HWND configExportButton = Button(TabBackup, ID_CONFIG_EXPORT, L"导出配置包", configButtonsX, configButtonsY, configExportWidth);
            HWND configImportButton = Button(TabBackup, ID_CONFIG_IMPORT, L"导入配置包", configButtonsX + configExportWidth + behaviorLayout.controlGapX, configButtonsY, configImportWidth);
            ThemedUi::BindGroupChildren(configBackupGroup, {configExportButton, configImportButton});

            const int todoExportWidth = settingsUi.buttonWidth(L"导出", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const int todoMergeImportWidth = settingsUi.buttonWidth(L"合并导入", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const int todoReplaceImportWidth = settingsUi.buttonWidth(L"全量导入", ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const int todoButtonsX = behaviorLeft;
            const ThemedSectionGeometry todoBackupSection = behaviorForm.section(
                behaviorFrameLeft, configBackupSection.frame.bottom + behaviorFrameGap, behaviorFrameWidth,
                {behaviorForm.sectionRow({ThemedSectionItemKind::Button}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::CheckBox}),
                 behaviorForm.sectionRow({ThemedSectionItemKind::Label})});
            HWND todoBackupGroup = AddSectionFrame(TabBackup, L"待办事项", todoBackupSection.frame);
            const int todoButtonsY = behaviorForm.sectionItemY(todoBackupSection, 0, settingsUi.buttonHeight());
            HWND todoExportButton = Button(TabBackup, ID_TODO_EXPORT, L"导出", todoButtonsX, todoButtonsY, todoExportWidth);
            HWND todoMergeImportButton = Button(TabBackup, ID_TODO_IMPORT_MERGE, L"合并导入", todoButtonsX + todoExportWidth + behaviorLayout.controlGapX, todoButtonsY, todoMergeImportWidth);
            HWND todoReplaceImportButton = Button(
                TabBackup,
                ID_TODO_IMPORT_REPLACE,
                L"全量导入",
                todoButtonsX + todoExportWidth + behaviorLayout.controlGapX + todoMergeImportWidth + behaviorLayout.controlGapX,
                todoButtonsY,
                todoReplaceImportWidth,
                ThemedButtonRole::Normal);
            const int backupCheckWidth = behaviorContentWidth / 3;
            const int backupCheckY = behaviorForm.sectionItemY(todoBackupSection, 1, behaviorCheckHeight);
            todoIncludeCompleted_ = CheckBox(TabBackup, ID_TODO_INCLUDE_COMPLETED, L"含已完成", behaviorLeft, backupCheckY, true, backupCheckWidth);
            todoIncludeDisabled_ = CheckBox(TabBackup, ID_TODO_INCLUDE_DISABLED, L"含已禁用", behaviorLeft + backupCheckWidth, backupCheckY, true, backupCheckWidth);
            todoOnlyFuture_ = CheckBox(TabBackup, ID_TODO_ONLY_FUTURE, L"仅未来", behaviorLeft + backupCheckWidth * 2, backupCheckY, false, behaviorContentWidth - backupCheckWidth * 2);
            HWND todoBackupNote = Label(
                TabBackup,
                L"待办事项备份可用于 Quattro 恢复，也可通过 Apple 快捷指令导入提醒事项。",
                behaviorLeft,
                behaviorForm.sectionItemY(todoBackupSection, 2, settingsUi.labelHeight()),
                behaviorContentWidth);
            ThemedUi::BindGroupChildren(todoBackupGroup, {
                todoExportButton, todoMergeImportButton, todoReplaceImportButton, todoIncludeCompleted_, todoIncludeDisabled_, todoOnlyFuture_, todoBackupNote});

            const ThemedUi footerUi = MakeUi();
            okButton_ = footerUi.FooterButton(IDOK, L"确定", 0, 3, true, true);
            applyButton_ = footerUi.FooterButton(ID_SETTINGS_APPLY, L"应用", 1, 3);
            cancelButton_ = footerUi.FooterButton(IDCANCEL, L"取消", 2, 3);
            BindTabPages();
            ShowTab(TabDisplay);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            windowUi_->FillBackground(dc);
            windowUi_->DrawRegisteredEditFrames(dc);
            windowUi_->DrawRegisteredTableFrames(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_SETTINGS_WEBDAV_DONE:
            HandleWebDavResult(std::unique_ptr<SettingsWebDavResult>(reinterpret_cast<SettingsWebDavResult*>(lParam)));
            return 0;
        case WM_SETTINGS_AUTORUN_CHANGED:
            ApplyExternalAutoRun(wParam != 0);
            return 0;
        case WM_CONTEXT_MENU_ICON_LOAD_REQUEST:
            if (currentTab_ == TabContextMenu && contextMenuIconAutoRequested_) {
                StartContextMenuIconLoad(false);
            }
            return 0;
        case WM_CONTEXT_MENU_ICON_LOAD_DONE:
            CompleteContextMenuIconLoad(static_cast<std::uintptr_t>(wParam));
            return 0;
        case WM_CONTEXT_MENU_REFRESH_DONE:
            CompleteContextMenuRefresh();
            return 0;
        case WM_NOTIFY:
            if (HandleHotKeyTableEvent(lParam)) {
                return 0;
            }
            if (HandleContextMenuTableEvent(lParam)) {
                return 0;
            }
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_SETTINGS_TAB_CONTROL && HIWORD(wParam) == CBN_SELCHANGE) {
                ShowTab(ThemedUi::ActiveTab(settingsTabs_));
                return 0;
            }
        if (LOWORD(wParam) == ID_REGISTER_COPY_PATH_CONTEXT_MENU && HIWORD(wParam) == BN_CLICKED) {
            const bool enabled = ThemedUi::IsChecked(registerCopyPathContextMenu_);
            SetWindowTextW(
                copyPathContextMenuStatus_,
                enabled ? L"保存设置后注册。" : L"保存设置后移除。" );
            MakeUi().SetStatusTextRole(copyPathContextMenuStatus_, ThemedStatusRole::Warning);
            return 0;
        }
        if (LOWORD(wParam) >= ID_TAG_ALIGN_LEFT && LOWORD(wParam) <= ID_TAG_ALIGN_RIGHT) {
            tagAlignIndex_ = static_cast<int>(LOWORD(wParam) - ID_TAG_ALIGN_LEFT);
            UpdateTagAlignButtons();
            return 0;
        }
        if (LOWORD(wParam) == ID_HTTP_ADDRESS && (HIWORD(wParam) == EN_KILLFOCUS || HIWORD(wParam) == EN_CHANGE)) {
            UpdateHttpStatusLabel();
            if (HIWORD(wParam) == EN_KILLFOCUS) {
                UpdateHttpAddressField(false);
            }
        }
            if (LOWORD(wParam) == ID_MAIN_HOTKEY_CAPTURE) {
                HotKeyCaptureDialogOptions options{};
                options.allowDoubleAlt = true;
                options.useMainHotKeyText = true;
                TrySetMainHotKey(ShowHotKeyCaptureDialog(hwnd_, instance_, theme_, draft_.mainHotKey, options));
                return 0;
            }
            if (LOWORD(wParam) == ID_PROCESS_LOCATOR_HOTKEY_CAPTURE) {
                TrySetProcessLocatorHotKey(ShowHotKeyCaptureDialog(hwnd_, instance_, theme_, draft_.processLocatorHotKey));
                return 0;
            }
            if (LOWORD(wParam) == ID_COPY_SELECTED_PATHS_HOTKEY_CAPTURE) {
                TrySetCopySelectedPathsHotKey(ShowHotKeyCaptureDialog(hwnd_, instance_, theme_, draft_.copySelectedPathsHotKey));
                return 0;
            }
            if (LOWORD(wParam) == ID_GLOBAL_HOTKEYS_ENABLED) {
                draft_.globalHotKeysEnabled = ThemedUi::IsChecked(globalHotKeysEnabled_);
                UpdateHotKeyLabels();
                return 0;
            }
            if (LOWORD(wParam) == ID_RESET_DEFAULT_HOTKEYS) {
                ResetDefaultHotKeys();
                return 0;
            }
            if (LOWORD(wParam) == ID_MAIN_HOTKEY_CLEAR) {
                draft_.mainHotKey = 0;
                UpdateHotKeyLabels();
                return 0;
            }
            if (LOWORD(wParam) == ID_WEBDAV_TEST) {
                TestWebDavConnection();
                return 0;
            }
            if (LOWORD(wParam) == ID_WEBDAV_CLEAR_PASSWORD) {
                ClearWebDavPassword();
                return 0;
            }
            if (LOWORD(wParam) == ID_RESET_CONTEXT_MENU) {
                ResetContextMenu();
                return 0;
            }
            if (LOWORD(wParam) == ID_REFRESH_CONTEXT_MENU_FROM_NATIVE) {
                RefreshContextMenuFromNative();
                return 0;
            }
            if (LOWORD(wParam) == ID_WEBDAV_UPLOAD) {
                UploadWebDavBackup();
                return 0;
            }
            if (LOWORD(wParam) == ID_WEBDAV_DOWNLOAD) {
                DownloadWebDavBackup();
                return 0;
            }
            if (LOWORD(wParam) == ID_WEBDAV_FILE_MANAGER) {
                AppConfig value = ReadWebDavDraftFromControls();
                if (SaveWebDavPasswordIfNeeded(value)) {
                    ShowWebDavFileManagerDialog(hwnd_, instance_, theme_, value);
                }
                return 0;
            }
            if (LOWORD(wParam) == ID_CONFIG_EXPORT) {
                ExportConfigPackage();
                return 0;
            }
            if (LOWORD(wParam) == ID_CONFIG_IMPORT) {
                ImportConfigPackage();
                return 0;
            }
            if (LOWORD(wParam) == ID_TODO_EXPORT) {
                ExportTodosJson();
                return 0;
            }
            if (LOWORD(wParam) == ID_TODO_IMPORT_MERGE) {
                ImportTodosJson(TodoJsonImportMode::Merge);
                return 0;
            }
            if (LOWORD(wParam) == ID_TODO_IMPORT_REPLACE) {
                ImportTodosJson(TodoJsonImportMode::ReplaceAll);
                return 0;
            }
            if (LOWORD(wParam) == ID_HTTP_BROWSE_ROOT) {
                BrowseHttpRoot();
                return 0;
            }
            if (LOWORD(wParam) == ID_HTTP_OPEN_ROOT) {
                OpenHttpRootDirectory();
                return 0;
            }
            if (LOWORD(wParam) == ID_HTTP_OPEN_CONFIG_DIR) {
                OpenHttpConfigDirectory();
                return 0;
            }
            if (LOWORD(wParam) == ID_HTTP_OPEN_HOME) {
                OpenHttpHome();
                return 0;
            }
            if (LOWORD(wParam) == ID_HTTP_COPY_URL) {
                CopyHttpUrl();
                return 0;
            }
            if (LOWORD(wParam) == ID_HTTP_START) {
                StartHttpServerFromDialog(false);
                return 0;
            }
            if (LOWORD(wParam) == ID_HTTP_STOP) {
                StopHttpServerFromDialog();
                return 0;
            }
            if (LOWORD(wParam) == ID_HTTP_RESTART) {
                StartHttpServerFromDialog(true);
                return 0;
            }
            if (LOWORD(wParam) == ID_SETTINGS_APPLY) {
                if (contextMenuRefreshBusy_) {
                    ShowToast(L"Windows 菜单正在刷新，请稍候。", ThemedToastRole::Info);
                    return 0;
                }
                CommitSettings(false);
                return 0;
            }
            if (LOWORD(wParam) == IDOK) {
                if (contextMenuRefreshBusy_) {
                    ShowToast(L"Windows 菜单正在刷新，请等待完成后关闭设置。", ThemedToastRole::Info);
                    return 0;
                }
                if (!CommitSettings(true)) {
                    return 0;
                }
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                if (contextMenuRefreshBusy_) {
                    ShowToast(L"Windows 菜单正在刷新，请等待完成后关闭设置。", ThemedToastRole::Info);
                    return 0;
                }
                if (webDavBusy_) {
                    ShowToast(L"WebDAV 操作正在进行，请稍候完成。", ThemedToastRole::Warning);
                    return 0;
                }
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            return 0;
        case WM_CLOSE:
            if (contextMenuRefreshBusy_) {
                ShowToast(L"Windows 菜单正在刷新，请等待完成后关闭设置。", ThemedToastRole::Info);
                return 0;
            }
            if (webDavBusy_) {
                ShowToast(L"WebDAV 操作正在进行，请稍候完成。", ThemedToastRole::Warning);
                return 0;
            }
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            if (owner_ && reinterpret_cast<HWND>(GetPropW(owner_, kSettingsDialogHwndProp)) == hwnd_) {
                RemovePropW(owner_, kSettingsDialogHwndProp);
            }
            done_ = true;
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    void UpdateHotKeyLabels() {
        AddHotKeyTableRows();
        if (mainHotKeyStatus_) {
            if (!draft_.globalHotKeysEnabled) {
                SetWindowTextW(mainHotKeyStatus_, L"全局快捷键已关闭。");
                return;
            }
            if (!IsDoubleAltMainHotKey(draft_.mainHotKey) &&
                draft_.mainHotKey != 0 &&
                draft_.mainHotKey == draft_.processLocatorHotKey) {
                SetWindowTextW(mainHotKeyStatus_, L"主窗口显隐和进程定位器不能使用同一个快捷键。");
                return;
            }
            if (!IsDoubleAltMainHotKey(draft_.mainHotKey) &&
                draft_.mainHotKey != 0 &&
                draft_.mainHotKey == draft_.copySelectedPathsHotKey) {
                SetWindowTextW(mainHotKeyStatus_, L"主窗口显隐和复制选中项绝对路径不能使用同一个快捷键。");
                return;
            }
            if (draft_.processLocatorHotKey != 0 &&
                draft_.processLocatorHotKey == draft_.copySelectedPathsHotKey) {
                SetWindowTextW(mainHotKeyStatus_, L"进程定位器和复制选中项绝对路径不能使用同一个快捷键。");
                return;
            }
            const HotKeyAvailability mainAvailability = CheckMainHotKeyAvailability(hwnd_, draft_.mainHotKey, CurrentRegisteredMainHotKey());
            if (!mainAvailability.available) {
                SetWindowTextW(mainHotKeyStatus_, MainHotKeyStatusText(draft_.mainHotKey, mainAvailability).c_str());
                return;
            }
            const HotKeyAvailability locatorAvailability = CheckCtrlAltHotKeyAvailability(
                hwnd_, draft_.processLocatorHotKey, CurrentRegisteredProcessLocatorHotKey());
            if (!locatorAvailability.available) {
                SetWindowTextW(mainHotKeyStatus_, ProcessLocatorHotKeyStatusText(draft_.processLocatorHotKey, locatorAvailability).c_str());
                return;
            }
            const HotKeyAvailability copyAvailability = CheckCtrlAltHotKeyAvailability(
                hwnd_, draft_.copySelectedPathsHotKey, CurrentRegisteredCopySelectedPathsHotKey());
            SetWindowTextW(mainHotKeyStatus_, CopySelectedPathsHotKeyStatusText(draft_.copySelectedPathsHotKey, copyAvailability).c_str());
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    AppConfig& config_;
    AppConfig draft_;
    const Theme& theme_;
    std::filesystem::path appDirectory_;
    std::filesystem::path httpRootBaseDirectory_;
    LocalHttpServerService* httpServer_ = nullptr;
    bool mainHotKeyRegistered_ = false;
    bool processLocatorHotKeyRegistered_ = false;
    bool copySelectedPathsHotKeyRegistered_ = false;
    int currentTab_ = -1;
    RECT tabStripRect_{};
    int tabContentOffsetY_ = 0;
    HWND settingsTabs_ = nullptr;
    std::vector<TabChild> tabChildren_;
    int nextGeneratedControlId_ = 6000;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    HWND showTitle_ = nullptr;
    HWND showGroup_ = nullptr;
    HWND showTag_ = nullptr;
    HWND autoDock_ = nullptr;
    HWND hideInactive_ = nullptr;
    HWND hideMainAfterToolOpen_ = nullptr;
    HWND hideAfterLink_ = nullptr;
    HWND hideOnStart_ = nullptr;
    HWND doubleClick_ = nullptr;
    HWND deleteConfirm_ = nullptr;
    HWND saveRunCount_ = nullptr;
    HWND showToolboxButton_ = nullptr;
    HWND showSkinButton_ = nullptr;
    HWND autoRun_ = nullptr;
    HWND loggingEnabled_ = nullptr;
    HWND registerCopyPathContextMenu_ = nullptr;
    HWND copyPathContextMenuStatus_ = nullptr;
    HWND contextMenuTable_ = nullptr;
    HWND resetContextMenuButton_ = nullptr;
    HWND refreshContextMenuButton_ = nullptr;
    HIMAGELIST contextMenuImages_ = nullptr;
    // 表格行序 → 绑定表下标（已安装在前、未安装沉底）。
    std::vector<std::size_t> contextMenuTableOrder_;
    std::vector<ContextMenuProviderIconInfo> contextMenuProviderIcons_;
    std::vector<int> contextMenuProviderImageIndexes_;
    HWND linkNameSingleLine_ = nullptr;
    HWND linkNameBold_ = nullptr;
    HWND showTooltip_ = nullptr;
    HWND groupRight_ = nullptr;
    HWND tagRight_ = nullptr;
    HWND enterActiveGroup_ = nullptr;
    HWND enterActiveTag_ = nullptr;
    HWND alphaEdit_ = nullptr;
    HWND groupWidthEdit_ = nullptr;
    HWND tagWidthEdit_ = nullptr;
    HWND dockDelayEdit_ = nullptr;
    HWND groupDelayEdit_ = nullptr;
    HWND tagDelayEdit_ = nullptr;
    int tagAlignIndex_ = 1;
    HWND tagAlignLeft_ = nullptr;
    HWND tagAlignCenter_ = nullptr;
    HWND tagAlignRight_ = nullptr;
    HWND globalHotKeysEnabled_ = nullptr;
    HWND resetDefaultHotKeysButton_ = nullptr;
    HWND hotKeyTable_ = nullptr;
    HWND mainHotKeyStatus_ = nullptr;
    HWND openDirEdit_ = nullptr;
    HWND updateUrlEdit_ = nullptr;
    HWND webDavEnabled_ = nullptr;
    HWND webDavUrlEdit_ = nullptr;
    HWND webDavBackupPathEdit_ = nullptr;
    HWND webDavFilesPathEdit_ = nullptr;
    HWND webDavKeepCountEdit_ = nullptr;
    HWND webDavUserNameEdit_ = nullptr;
    HWND webDavPasswordEdit_ = nullptr;
    HWND webDavUploadButton_ = nullptr;
    HWND webDavDownloadButton_ = nullptr;
    HWND webDavTestButton_ = nullptr;
    HWND webDavClearPasswordButton_ = nullptr;
    HWND webDavLastSyncLabel_ = nullptr;
    HWND webDavFileManagerButton_ = nullptr;
    HWND webDavUploadContextMenu_ = nullptr;
    HWND httpServerAutoStart_ = nullptr;
    HWND httpServerAddressEdit_ = nullptr;
    HWND httpServerRootEdit_ = nullptr;
    HWND httpServerStatusTag_ = nullptr;
    HWND httpServerStatusDetail_ = nullptr;
    HWND httpBrowseRootButton_ = nullptr;
    HWND httpStartButton_ = nullptr;
    HWND httpStopButton_ = nullptr;
    HWND httpRestartButton_ = nullptr;
    HWND okButton_ = nullptr;
    HWND applyButton_ = nullptr;
    HWND cancelButton_ = nullptr;
    HWND todoIncludeCompleted_ = nullptr;
    HWND todoIncludeDisabled_ = nullptr;
    HWND todoOnlyFuture_ = nullptr;
    bool importedData_ = false;
    bool webDavBusy_ = false;
    bool contextMenuRefreshBusy_ = false;
    bool contextMenuIconLoadBusy_ = false;
    bool contextMenuIconAutoRequested_ = false;
    bool contextMenuProviderLoadCompleted_ = false;
    bool contextMenuProviderLoadFailed_ = false;
    bool accepted_ = false;
    bool done_ = false;
    SettingsApplyCallback applyCallback_;
    SettingsResetContextMenuCallback resetContextMenuCallback_;
    std::vector<Link> contextMenuLinks_;
    SettingsContextMenuRefreshRunner contextMenuRefreshRunner_;
    SettingsContextMenuRefreshApplyCallback contextMenuRefreshApplyCallback_;
    SettingsContextMenuProviderIconRunner contextMenuProviderIconRunner_;
    SettingsCopyPathContextMenuCallback copyPathContextMenuCallback_;
    SettingsWebDavUploadContextMenuCallback webDavUploadContextMenuCallback_;
    std::shared_ptr<ScanTaskHandle> contextMenuRefreshTask_;
    std::unique_ptr<ThemedTaskProgressDialog> contextMenuRefreshProgressDialog_;
    std::shared_ptr<SettingsContextMenuIconAsyncState> contextMenuIconAsyncState_;
    std::uintptr_t contextMenuIconLoadGeneration_ = 0;
};
}

bool ShowTextInputDialog(HWND owner, HINSTANCE instance, const Theme& theme, const std::wstring& title, const std::wstring& label, std::wstring& value) {
    TextDialog dialog(owner, instance, theme, title, label, value);
    return dialog.Run();
}

int ShowThemedMessageBox(HWND owner, HINSTANCE instance, const Theme& theme, const std::wstring& message, const std::wstring& title, UINT flags) {
    ThemedMessageDialog dialog(owner, instance, theme, message, title, flags);
    return dialog.Run();
}

bool ShowHotKeyConflictDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const std::wstring& message,
    bool& ignoreFutureWarnings) {
    if (QuattroTestMode()) {
        return false;
    }
    HotKeyConflictDialog dialog(owner, instance, theme, message, ignoreFutureWarnings);
    return dialog.Run();
}

bool ShowWebDavBackupSelectionDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const std::vector<WebDavRemoteFile>& backups,
    std::wstring& selectedName) {
    WebDavBackupSelectionDialog dialog(owner, instance, theme, backups, selectedName);
    return dialog.Run();
}

bool ShowWebDavFileManagerDialog(HWND owner, HINSTANCE instance, const Theme& theme, const AppConfig& config) {
    WebDavFileManagerDialog dialog(owner, instance, theme, config);
    return dialog.Run();
}

bool ShowSettingsDialog(
    HWND owner,
    HINSTANCE instance,
    AppConfig& config,
    const Theme& theme,
    const std::filesystem::path& appDirectory,
    const std::filesystem::path& httpRootBaseDirectory,
    bool* importedData,
    LocalHttpServerService* httpServer,
    bool mainHotKeyRegistered,
    bool processLocatorHotKeyRegistered,
    bool copySelectedPathsHotKeyRegistered,
    SettingsApplyCallback applyCallback,
    SettingsResetContextMenuCallback resetContextMenuCallback,
    const std::vector<Link>& contextMenuLinks,
    SettingsContextMenuRefreshRunner contextMenuRefreshRunner,
    SettingsContextMenuRefreshApplyCallback contextMenuRefreshApplyCallback,
    SettingsContextMenuProviderIconRunner contextMenuProviderIconRunner,
    SettingsCopyPathContextMenuCallback copyPathContextMenuCallback,
    SettingsWebDavUploadContextMenuCallback webDavUploadContextMenuCallback) {
    SettingsDialog dialog(
        owner,
        instance,
        config,
        theme,
        appDirectory,
        httpRootBaseDirectory,
        httpServer,
        mainHotKeyRegistered,
        processLocatorHotKeyRegistered,
        copySelectedPathsHotKeyRegistered,
        std::move(applyCallback),
        std::move(resetContextMenuCallback),
        contextMenuLinks,
        std::move(contextMenuRefreshRunner),
        std::move(contextMenuRefreshApplyCallback),
        std::move(contextMenuProviderIconRunner),
        std::move(copyPathContextMenuCallback),
        std::move(webDavUploadContextMenuCallback));
    const bool accepted = dialog.Run();
    if (importedData) {
        *importedData = dialog.webDavDataImported();
    }
    return accepted;
}

void NotifyOpenSettingsDialogAutoRunChanged(HWND owner, bool enabled) {
    if (!owner) {
        return;
    }
    HWND settingsHwnd = reinterpret_cast<HWND>(GetPropW(owner, kSettingsDialogHwndProp));
    if (!settingsHwnd || !IsWindow(settingsHwnd)) {
        return;
    }
    SendMessageW(settingsHwnd, WM_SETTINGS_AUTORUN_CHANGED, enabled ? 1 : 0, 0);
}
