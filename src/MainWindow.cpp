#include "MainWindow.h"

#include "AppLog.h"
#include "HotKeyEditor.h"
#include "LinkEditDialog.h"
#include "MenuCatalog.h"
#include "SearchDialog.h"
#include "ShellItemService.h"
#include "SimpleDialogs.h"
#include "StartupService.h"
#include "SystemFunctionDialog.h"
#include "UrlEditDialog.h"
#include "Utilities.h"
#include "../resources/resource.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <commctrl.h>
#include <commdlg.h>
#include <ctime>
#include <cstring>
#include <cwctype>
#include <optional>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <system_error>
#include <uxtheme.h>
#include <windowsx.h>

namespace {
constexpr int ID_HOTKEY_MAIN = 1;
constexpr int ID_HOTKEY_SEARCH = 2;
constexpr int ID_HOTKEY_LINK_BASE = 1000;
constexpr UINT_PTR ID_TIMER_DOCK = 10;
constexpr UINT_PTR ID_TIMER_HOVER_ACTIVATE = 11;
constexpr int kDockVisiblePixels = 3;
constexpr int kDockRestoreGraceMs = 1500;

template <typename T>
void SafeRelease(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

float Width(const D2D1_RECT_F& rect) {
    return rect.right - rect.left;
}

float Height(const D2D1_RECT_F& rect) {
    return rect.bottom - rect.top;
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

bool Intersects(const D2D1_RECT_F& left, const D2D1_RECT_F& right) {
    return left.right > right.left &&
           left.left < right.right &&
           left.bottom > right.top &&
           left.top < right.bottom;
}

D2D1_RECT_F IntersectRectF(const D2D1_RECT_F& left, const D2D1_RECT_F& right) {
    return D2D1::RectF(
        std::max(left.left, right.left),
        std::max(left.top, right.top),
        std::min(left.right, right.right),
        std::min(left.bottom, right.bottom));
}

D2D1_RECT_F Inset(D2D1_RECT_F rect, float dx, float dy) {
    rect.left += dx;
    rect.right -= dx;
    rect.top += dy;
    rect.bottom -= dy;
    return rect;
}

std::wstring TodayText() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    static constexpr const wchar_t* kWeekdays[] = {
        L"星期日", L"星期一", L"星期二", L"星期三", L"星期四", L"星期五", L"星期六",
    };
    return std::to_wstring(now.wMonth) + L"月" + std::to_wstring(now.wDay) + L"日 " + kWeekdays[now.wDayOfWeek % 7];
}

POINT ClampWindowPosition(int x, int y, int width, int height) {
    RECT proposed{x, y, x + width, y + height};
    HMONITOR monitor = MonitorFromRect(&proposed, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return POINT{x, y};
    }

    const RECT work = info.rcWork;
    const int minVisible = 80;
    POINT result{};
    result.x = std::max(static_cast<int>(work.left) - width + minVisible,
                        std::min(x, static_cast<int>(work.right) - minVisible));
    result.y = std::max(static_cast<int>(work.top),
                        std::min(y, static_cast<int>(work.bottom) - minVisible));
    return result;
}

int NextModelLinkPosition(const std::vector<Link>& links, int parentGroup) {
    int maxPosition = -1;
    for (const auto& link : links) {
        if (link.parentGroup == parentGroup) {
            maxPosition = std::max(maxPosition, link.pos);
        }
    }
    return maxPosition + 1;
}

int NextModelGroupPosition(const std::vector<Group>& groups, int parentGroup) {
    int maxPosition = -1;
    for (const auto& group : groups) {
        if (group.parentGroup == parentGroup) {
            maxPosition = std::max(maxPosition, group.pos);
        }
    }
    return maxPosition + 1;
}

std::optional<Color> ParseCustomColor(const std::wstring& value) {
    const std::wstring text = Trim(value);
    if (text.size() != 9 || text[0] != L'#') {
        return std::nullopt;
    }
    auto hexDigit = [](wchar_t ch) -> int {
        if (ch >= L'0' && ch <= L'9') return ch - L'0';
        if (ch >= L'a' && ch <= L'f') return 10 + ch - L'a';
        if (ch >= L'A' && ch <= L'F') return 10 + ch - L'A';
        return -1;
    };
    auto byteAt = [&](std::size_t index) -> int {
        const int hi = hexDigit(text[index]);
        const int lo = hexDigit(text[index + 1]);
        return (hi < 0 || lo < 0) ? -1 : hi * 16 + lo;
    };
    const int a = byteAt(1);
    const int r = byteAt(3);
    const int g = byteAt(5);
    const int b = byteAt(7);
    if (a < 0 || r < 0 || g < 0 || b < 0) {
        return std::nullopt;
    }
    return Color{
        static_cast<float>(r) / 255.0f,
        static_cast<float>(g) / 255.0f,
        static_cast<float>(b) / 255.0f,
        static_cast<float>(a) / 255.0f,
    };
}

bool IsFullScreenForegroundWindow() {
    HWND foreground = GetForegroundWindow();
    if (!foreground || foreground == GetDesktopWindow() || foreground == GetShellWindow()) {
        return false;
    }
    RECT rect{};
    if (!GetWindowRect(foreground, &rect)) {
        return false;
    }
    HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return false;
    }
    return rect.left <= info.rcMonitor.left &&
           rect.top <= info.rcMonitor.top &&
           rect.right >= info.rcMonitor.right &&
           rect.bottom >= info.rcMonitor.bottom;
}

int EffectiveIconSize(const Group* tag) {
    const int value = tag && tag->iconSize > 0 ? tag->iconSize : 32;
    return std::max(24, std::min(48, value));
}

int EffectiveLinkLayout(const Group* tag) {
    return tag && tag->layout == 0 ? 0 : 1;
}

float Metric(const Theme& theme, const wchar_t* component, const wchar_t* name, float fallback) {
    return theme.metric(component, name, fallback);
}

float LinkTileSide(const Theme& theme, int iconSize) {
    return std::max(
        Metric(theme, L"linkItem", L"tileMinSide", 74.0f),
        std::min(
            Metric(theme, L"linkItem", L"tileMaxSide", 96.0f),
            static_cast<float>(iconSize) + Metric(theme, L"linkItem", L"tileIconExtra", 42.0f)));
}

int LinkGridColumns(const Theme& theme, const D2D1_RECT_F& rect, float tileSide, float gap) {
    const float width = std::max(1.0f, Width(rect) - Metric(theme, L"linkItem", L"viewportPaddingX", 16.0f));
    return std::max(1, static_cast<int>((width + gap) / (tileSide + gap)));
}

struct LinkLayoutMetrics {
    int layout = 1;
    int iconSize = 32;
    bool compactTile = false;
    int columns = 1;
    float leftInset = 8.0f;
    float topInset = 8.0f;
    float bottomInset = 8.0f;
    float gapX = 4.0f;
    float gapY = 4.0f;
    float itemWidth = 74.0f;
    float itemHeight = 74.0f;
};

LinkLayoutMetrics MakeLinkLayout(const Theme& theme, const D2D1_RECT_F& rect, const Group* tag) {
    LinkLayoutMetrics metrics;
    metrics.layout = EffectiveLinkLayout(tag);
    metrics.iconSize = EffectiveIconSize(tag);

    const float available = std::max(1.0f, Width(rect) - Metric(theme, L"linkItem", L"viewportPaddingX", 16.0f));
    if (metrics.layout == 0) {
        metrics.leftInset = Metric(theme, L"linkItem", L"listLeftInset", 4.0f);
        metrics.topInset = Metric(theme, L"linkItem", L"listTopInset", 6.0f);
        metrics.bottomInset = Metric(theme, L"linkItem", L"listBottomInset", 6.0f);
        metrics.gapX = Metric(theme, L"linkItem", L"listGapX", 0.0f);
        metrics.gapY = Metric(theme, L"linkItem", L"listGapY", 2.0f);
        metrics.columns = 1;
        metrics.itemWidth = std::max(1.0f, Width(rect) - metrics.leftInset * 2.0f);
        metrics.itemHeight = std::max(Metric(theme, L"linkItem", L"listMinHeight", 34.0f), static_cast<float>(metrics.iconSize) + Metric(theme, L"linkItem", L"listHeightExtra", 12.0f));
        return metrics;
    }

    metrics.leftInset = Metric(theme, L"linkItem", L"gridLeftInset", 8.0f);
    metrics.topInset = Metric(theme, L"linkItem", L"gridTopInset", 8.0f);
    metrics.bottomInset = Metric(theme, L"linkItem", L"gridBottomInset", 8.0f);
    if (metrics.iconSize <= 24) {
        metrics.compactTile = true;
        metrics.gapX = Metric(theme, L"linkItem", L"compactGapX", 6.0f);
        metrics.gapY = Metric(theme, L"linkItem", L"compactGapY", 6.0f);
        const float preferredWidth = Metric(theme, L"linkItem", L"compactPreferredWidth", 128.0f);
        metrics.columns = std::max(1, static_cast<int>((available + metrics.gapX) / (preferredWidth + metrics.gapX)));
        metrics.itemWidth = std::max(Metric(theme, L"linkItem", L"compactMinWidth", 72.0f), (available - static_cast<float>(metrics.columns - 1) * metrics.gapX) / static_cast<float>(metrics.columns));
        metrics.itemHeight = std::max(Metric(theme, L"linkItem", L"listMinHeight", 34.0f), static_cast<float>(metrics.iconSize) + Metric(theme, L"linkItem", L"listHeightExtra", 12.0f));
        return metrics;
    }

    metrics.gapX = Metric(theme, L"linkItem", L"gridGapX", 4.0f);
    metrics.gapY = Metric(theme, L"linkItem", L"gridGapY", 4.0f);
    metrics.itemWidth = LinkTileSide(theme, metrics.iconSize);
    metrics.itemHeight = metrics.itemWidth;
    metrics.columns = LinkGridColumns(theme, rect, metrics.itemWidth, metrics.gapX);
    return metrics;
}

D2D1_RECT_F LinkItemRect(const D2D1_RECT_F& rect, const LinkLayoutMetrics& metrics, int index, float scrollOffset) {
    const int column = index % metrics.columns;
    const int row = index / metrics.columns;
    const float x = rect.left + metrics.leftInset + static_cast<float>(column) * (metrics.itemWidth + metrics.gapX);
    const float y = rect.top + metrics.topInset + static_cast<float>(row) * (metrics.itemHeight + metrics.gapY) - scrollOffset;
    return D2D1::RectF(x, y, x + metrics.itemWidth, y + metrics.itemHeight);
}

float LinkContentHeight(std::size_t count, const LinkLayoutMetrics& metrics) {
    if (count == 0) {
        return 0.0f;
    }
    const int rows = static_cast<int>((count + static_cast<std::size_t>(metrics.columns) - 1) / static_cast<std::size_t>(metrics.columns));
    return metrics.topInset +
           static_cast<float>(rows) * metrics.itemHeight +
           static_cast<float>(std::max(0, rows - 1)) * metrics.gapY +
           metrics.bottomInset;
}

float NavigationItemWidth(const Theme& theme, const std::wstring& text) {
    return std::max(
        Metric(theme, L"majorNavItem", L"textMinWidth", 86.0f),
        std::min(
            Metric(theme, L"majorNavItem", L"textMaxWidth", 168.0f),
            Metric(theme, L"majorNavItem", L"textBaseWidth", 28.0f) + static_cast<float>(text.size()) * Metric(theme, L"majorNavItem", L"textCharWidth", 12.0f)));
}

bool HasSiblingGroupName(const std::vector<Group>& groups, int parentGroup, const std::wstring& name) {
    const std::wstring normalized = ToLower(Trim(name));
    for (const auto& group : groups) {
        if (group.parentGroup == parentGroup && ToLower(Trim(group.name)) == normalized) {
            return true;
        }
    }
    return false;
}

std::wstring UniqueSiblingGroupName(const std::vector<Group>& groups, int parentGroup, const std::wstring& baseName) {
    if (!HasSiblingGroupName(groups, parentGroup, baseName)) {
        return baseName;
    }

    for (int index = 2; index < 10000; ++index) {
        std::wstring candidate = baseName + L" " + std::to_wstring(index);
        if (!HasSiblingGroupName(groups, parentGroup, candidate)) {
            return candidate;
        }
    }
    return baseName + L" " + std::to_wstring(static_cast<int>(groups.size()) + 1);
}

bool LooksLikeTodoLink(const Link& link) {
    const std::wstring text = ToLower(link.name + L" " + link.path + L" " + link.parameter + L" " + link.remark);
    return text.find(L"todo") != std::wstring::npos ||
           text.find(L"[ ]") != std::wstring::npos ||
           text.find(L"[x]") != std::wstring::npos ||
           text.find(L"待办") != std::wstring::npos ||
           text.find(L"任务") != std::wstring::npos;
}

bool IsAllTag(const Group& tag) {
    return tag.type == 1 || tag.name == L"全部" || ToLower(tag.content) == L"all";
}

bool IsTodoTag(const Group& tag) {
    const std::wstring content = ToLower(tag.content);
    return tag.type == 2 || tag.name == L"待办" || content.rfind(L"todo", 0) == 0;
}

std::wstring InitialSortKey(const std::wstring& value) {
    const std::wstring text = Trim(value);
    if (text.empty()) {
        return L"#";
    }
    const wchar_t first = text.front();
    if ((first >= L'a' && first <= L'z') || (first >= L'A' && first <= L'Z')) {
        std::wstring key(1, static_cast<wchar_t>(std::towupper(first)));
        return key + L"|" + ToLower(text);
    }
    if (first >= L'0' && first <= L'9') {
        return L"0|" + text;
    }
    if (first >= 0x4E00 && first <= 0x9FFF) {
        return L"Z|" + text;
    }
    return L"#|" + text;
}

struct ThemeMenuItem {
    std::wstring name;
    std::wstring label;
};

std::wstring XmlAttributeValue(const std::wstring& tag, const std::wstring& name) {
    const std::wstring pattern = name + L"=\"";
    std::size_t begin = tag.find(pattern);
    if (begin == std::wstring::npos) {
        return {};
    }
    begin += pattern.size();
    const std::size_t end = tag.find(L'"', begin);
    if (end == std::wstring::npos) {
        return {};
    }
    return tag.substr(begin, end - begin);
}

std::wstring ReadThemeDisplayName(const std::filesystem::path& path, const std::wstring& fallback) {
    const std::wstring xml = LoadUtf8File(path);
    const std::size_t begin = xml.find(L"<Theme");
    if (begin == std::wstring::npos) {
        return fallback;
    }
    const std::size_t end = xml.find(L">", begin);
    if (end == std::wstring::npos) {
        return fallback;
    }
    const std::wstring tag = xml.substr(begin, end - begin + 1);
    std::wstring label = XmlAttributeValue(tag, L"displayName");
    return label.empty() ? fallback : label;
}

std::vector<ThemeMenuItem> DiscoverThemeMenuItems(const std::filesystem::path& themeDirectory, const std::wstring& currentTheme) {
    std::vector<ThemeMenuItem> items;
    std::error_code ec;
    if (std::filesystem::exists(themeDirectory, ec) && std::filesystem::is_directory(themeDirectory, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(themeDirectory, ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_regular_file(ec) || entry.path().extension() != L".xml") {
                continue;
            }
            const std::wstring name = entry.path().stem().wstring();
            if (name.empty()) {
                continue;
            }
            items.push_back(ThemeMenuItem{name, ReadThemeDisplayName(entry.path(), name)});
        }
    }

    auto hasTheme = [&](const std::wstring& name) {
        return std::any_of(items.begin(), items.end(), [&](const ThemeMenuItem& item) {
            return item.name == name;
        });
    };
    if (!hasTheme(L"default")) {
        items.push_back(ThemeMenuItem{L"default", L"default"});
    }
    if (!currentTheme.empty() && currentTheme != L"default" && !hasTheme(currentTheme)) {
        const std::filesystem::path path = themeDirectory / (currentTheme + L".xml");
        if (FileExists(path)) {
            items.push_back(ThemeMenuItem{currentTheme, ReadThemeDisplayName(path, currentTheme)});
        }
    }

    std::sort(items.begin(), items.end(), [](const ThemeMenuItem& left, const ThemeMenuItem& right) {
        if (left.name == L"default" || right.name == L"default") {
            return left.name == L"default" && right.name != L"default";
        }
        return left.label < right.label;
    });
    items.erase(std::unique(items.begin(), items.end(), [](const ThemeMenuItem& left, const ThemeMenuItem& right) {
        return left.name == right.name;
    }), items.end());
    return items;
}

std::wstring EffectiveThemeName(const std::filesystem::path& themeDirectory, const std::wstring& themeName) {
    if (!themeName.empty() && FileExists(themeDirectory / (themeName + L".xml"))) {
        return themeName;
    }
    return L"default";
}

std::wstring MenuTextFromRaw(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'&' && i + 1 < text.size() && text[i + 1] != L'&') {
            continue;
        }
        result.push_back(text[i]);
    }
    return result;
}

bool DrawStockMenuIcon(HDC dc, const RECT& rect, SHSTOCKICONID id) {
    SHSTOCKICONINFO info{};
    info.cbSize = sizeof(info);
    if (FAILED(SHGetStockIconInfo(id, SHGSI_ICON | SHGSI_SMALLICON, &info)) || !info.hIcon) {
        return false;
    }
    const int size = std::min(rect.right - rect.left, rect.bottom - rect.top);
    DrawIconEx(dc, rect.left + ((rect.right - rect.left) - size) / 2, rect.top + ((rect.bottom - rect.top) - size) / 2,
               info.hIcon, size, size, 0, nullptr, DI_NORMAL);
    DestroyIcon(info.hIcon);
    return true;
}

bool DrawSystemImageListIcon(HDC dc, const RECT& rect, int imageIndex, bool disabled) {
    if (imageIndex < 0) {
        return false;
    }
    SHFILEINFOW info{};
    HIMAGELIST imageList = reinterpret_cast<HIMAGELIST>(SHGetFileInfoW(
        L"C:\\",
        FILE_ATTRIBUTE_DIRECTORY,
        &info,
        sizeof(info),
        SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES));
    if (!imageList) {
        return false;
    }

    const int size = std::min(rect.right - rect.left, rect.bottom - rect.top);
    const int x = rect.left + ((rect.right - rect.left) - size) / 2;
    const int y = rect.top + ((rect.bottom - rect.top) - size) / 2;
    return ImageList_Draw(imageList, imageIndex, dc, x, y, disabled ? ILD_BLEND50 : ILD_NORMAL) != FALSE;
}

bool EnsureMenuIconFontLoaded(const std::filesystem::path& appDirectory) {
    static std::filesystem::path loadedPath;
    static bool loaded = false;

    const std::filesystem::path fontPath = appDirectory / L"icons" / L"menu" / L"tabler" / L"tabler-icons.ttf";
    if (loaded && loadedPath == fontPath) {
        return true;
    }
    loadedPath = fontPath;
    loaded = FileExists(fontPath) && AddFontResourceExW(fontPath.c_str(), FR_PRIVATE, nullptr) > 0;
    return loaded;
}

wchar_t MenuIconGlyph(int icon) {
    switch (icon) {
    case MenuIconFile: return static_cast<wchar_t>(0xEAA4); // file
    case MenuIconFolder: return static_cast<wchar_t>(0xEAAD); // folder
    case MenuIconUrl: return static_cast<wchar_t>(0xEB54); // world
    case MenuIconSystem: return static_cast<wchar_t>(0xEBB6); // apps
    case MenuIconShield: return static_cast<wchar_t>(0xEB24); // shield
    case MenuIconOpenFolder: return static_cast<wchar_t>(0xFAF7); // folder-open
    case MenuIconWindows: return static_cast<wchar_t>(0xECD8); // brand-windows
    case MenuIconShortcut: return static_cast<wchar_t>(0xEA99); // external-link
    case MenuIconRefresh: return static_cast<wchar_t>(0xEB13); // refresh
    case MenuIconMove: return static_cast<wchar_t>(0xF22F); // arrows-move
    case MenuIconCopy: return static_cast<wchar_t>(0xEA7A); // copy
    case MenuIconCut: return static_cast<wchar_t>(0xEB1B); // scissors
    case MenuIconPaste: return static_cast<wchar_t>(0xEA6F); // clipboard
    case MenuIconEdit: return static_cast<wchar_t>(0xEA98); // edit
    case MenuIconInfo: return static_cast<wchar_t>(0xEAC5); // info-circle
    case MenuIconDelete: return static_cast<wchar_t>(0xEB41); // trash
    case MenuIconSearch: return static_cast<wchar_t>(0xEB1C); // search
    case MenuIconGroup: return static_cast<wchar_t>(0xEAAE); // folders
    case MenuIconTag: return static_cast<wchar_t>(0xEF86); // tags
    case MenuIconTheme: return static_cast<wchar_t>(0xEC0A); // shirt
    case MenuIconSize: return static_cast<wchar_t>(0xF291); // ruler-measure
    case MenuIconView: return static_cast<wchar_t>(0xEA03); // adjustments
    case MenuIconList: return static_cast<wchar_t>(0xEC14); // layout-list
    case MenuIconTile: return static_cast<wchar_t>(0xEDBA); // layout-grid
    case MenuIconSort: return static_cast<wchar_t>(0xEB5A); // arrows-sort
    case MenuIconClear: return static_cast<wchar_t>(0xEF88); // trash-x
    case MenuIconEye: return static_cast<wchar_t>(0xEA9A); // eye
    case MenuIconEyeOff: return static_cast<wchar_t>(0xECF0); // eye-off
    case MenuIconAbout: return static_cast<wchar_t>(0xEAC5); // info-circle
    case MenuIconExit: return static_cast<wchar_t>(0xEB55); // x
    case MenuIconRun: return static_cast<wchar_t>(0xED46); // player-play
    case MenuIconPin: return static_cast<wchar_t>(0xEC9C); // pin
    case MenuIconPinOff: return static_cast<wchar_t>(0xED60); // pinned
    case MenuIconSettings: return static_cast<wchar_t>(0xEB20); // settings
    case MenuIconHelp: return static_cast<wchar_t>(0xF91D); // help-circle
    case MenuIconReward: return static_cast<wchar_t>(0xEB68); // gift
    case MenuIconPower: return static_cast<wchar_t>(0xEB0D); // power
    case MenuIconRestart: return static_cast<wchar_t>(0xEB13); // refresh
    case MenuIconLogout: return static_cast<wchar_t>(0xEBA8); // logout
    case MenuIconLock: return static_cast<wchar_t>(0xEAE2); // lock
    case MenuIconSleep: return static_cast<wchar_t>(0xF228); // zzz
    case MenuIconMonitor: return static_cast<wchar_t>(0xEA89); // device-desktop
    case MenuIconVolumeUp: return static_cast<wchar_t>(0xEB51); // volume
    case MenuIconVolumeDown: return static_cast<wchar_t>(0xEB4F); // volume-2
    case MenuIconVolumeMute: return static_cast<wchar_t>(0xF1C3); // volume-off
    case MenuIconTools: return static_cast<wchar_t>(0xEBCA); // tools
    case MenuIconCalculator: return static_cast<wchar_t>(0xEB80); // calculator
    case MenuIconTerminal: return static_cast<wchar_t>(0xEBEF); // terminal-2
    case MenuIconNotebook: return static_cast<wchar_t>(0xEB96); // notebook
    case MenuIconEnvironment: return static_cast<wchar_t>(0xEF05); // variable
    case MenuIconUser: return static_cast<wchar_t>(0xEB4D); // user
    case MenuIconHistory: return static_cast<wchar_t>(0xEBEA); // history
    case MenuIconCertificate: return static_cast<wchar_t>(0xED76); // certificate
    case MenuIconComputer: return static_cast<wchar_t>(0xEA89); // device-desktop
    default: return L'\0';
    }
}

bool DrawLocalMenuIcon(HDC dc, const RECT& rc, int icon, bool disabled, const std::filesystem::path& appDirectory) {
    const wchar_t glyph = MenuIconGlyph(icon);
    if (glyph == L'\0' || !EnsureMenuIconFontLoaded(appDirectory)) {
        return false;
    }

    const int size = std::min(rc.right - rc.left, rc.bottom - rc.top);
    HFONT font = CreateFontW(
        -std::max(12, size + 1),
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"tabler-icons");
    if (!font) {
        return false;
    }

    const COLORREF accent = disabled ? RGB(160, 168, 178) :
        (icon == MenuIconDelete || icon == MenuIconClear || icon == MenuIconExit || icon == MenuIconPower ? RGB(228, 48, 58) : RGB(0, 153, 215));
    const int oldBkMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldTextColor = SetTextColor(dc, accent);
    HGDIOBJ oldFont = SelectObject(dc, font);
    RECT textRect = rc;
    DrawTextW(dc, &glyph, 1, &textRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOCLIP);
    SelectObject(dc, oldFont);
    SetTextColor(dc, oldTextColor);
    SetBkMode(dc, oldBkMode);
    DeleteObject(font);
    return true;
}

void DrawFallbackMenuIcon(HDC dc, const RECT& rc, int icon, bool disabled) {
    const COLORREF blue = disabled ? RGB(150, 150, 150) : RGB(0, 120, 215);
    const COLORREF red = disabled ? RGB(150, 150, 150) : RGB(230, 50, 45);
    const COLORREF mutedColor = disabled ? RGB(170, 170, 170) : RGB(100, 116, 139);
    const COLORREF amber = disabled ? RGB(170, 170, 170) : RGB(245, 180, 40);
    const COLORREF green = disabled ? RGB(170, 170, 170) : RGB(24, 150, 92);
    const int l = rc.left + 1;
    const int t = rc.top + 1;
    const int r = rc.right - 1;
    const int b = rc.bottom - 1;
    HPEN pen = CreatePen(PS_SOLID, 2, icon == MenuIconDelete || icon == MenuIconClear ? red : blue);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HBRUSH brush = CreateSolidBrush(icon == MenuIconFolder ? amber : RGB(255, 255, 255));
    HGDIOBJ oldBrush = SelectObject(dc, brush);

    switch (icon) {
    case MenuIconRefresh:
        Arc(dc, l, t, r, b, r - 2, t + 4, l + 3, t + 4);
        MoveToEx(dc, r - 3, t + 3, nullptr);
        LineTo(dc, r - 1, t + 8);
        LineTo(dc, r - 7, t + 7);
        break;
    case MenuIconMove:
        MoveToEx(dc, l + 2, (t + b) / 2, nullptr);
        LineTo(dc, r - 3, (t + b) / 2);
        MoveToEx(dc, r - 7, t + 4, nullptr);
        LineTo(dc, r - 3, (t + b) / 2);
        LineTo(dc, r - 7, b - 4);
        break;
    case MenuIconCopy:
        Rectangle(dc, l + 4, t + 2, r - 1, b - 4);
        Rectangle(dc, l + 1, t + 5, r - 4, b - 1);
        break;
    case MenuIconCut:
        Ellipse(dc, l + 1, t + 2, l + 6, t + 7);
        Ellipse(dc, l + 1, b - 7, l + 6, b - 2);
        MoveToEx(dc, l + 6, t + 7, nullptr);
        LineTo(dc, r - 2, b - 3);
        MoveToEx(dc, l + 6, b - 7, nullptr);
        LineTo(dc, r - 2, t + 3);
        break;
    case MenuIconPaste:
        RoundRect(dc, l + 3, t + 4, r - 2, b - 1, 3, 3);
        Rectangle(dc, l + 6, t + 1, r - 5, t + 6);
        MoveToEx(dc, l + 6, t + 10, nullptr);
        LineTo(dc, r - 5, t + 10);
        MoveToEx(dc, l + 6, t + 13, nullptr);
        LineTo(dc, r - 7, t + 13);
        break;
    case MenuIconClear:
    case MenuIconDelete:
        MoveToEx(dc, l + 2, t + 2, nullptr);
        LineTo(dc, r - 2, b - 2);
        MoveToEx(dc, r - 2, t + 2, nullptr);
        LineTo(dc, l + 2, b - 2);
        break;
    case MenuIconSort:
        MoveToEx(dc, l + 2, t + 3, nullptr);
        LineTo(dc, r - 2, t + 3);
        MoveToEx(dc, l + 2, (t + b) / 2, nullptr);
        LineTo(dc, r - 6, (t + b) / 2);
        MoveToEx(dc, l + 2, b - 3, nullptr);
        LineTo(dc, r - 10, b - 3);
        break;
    case MenuIconSize:
        Rectangle(dc, l + 2, t + 2, l + 7, t + 7);
        Rectangle(dc, r - 8, t + 2, r - 2, t + 8);
        Rectangle(dc, l + 2, b - 8, l + 9, b - 2);
        Rectangle(dc, r - 9, b - 9, r - 2, b - 2);
        break;
    case MenuIconList:
        for (int i = 0; i < 3; ++i) {
            const int y = t + 3 + i * 5;
            Rectangle(dc, l + 2, y, l + 5, y + 3);
            MoveToEx(dc, l + 8, y + 1, nullptr);
            LineTo(dc, r - 2, y + 1);
        }
        break;
    case MenuIconTile:
        Rectangle(dc, l + 2, t + 2, l + 7, t + 7);
        Rectangle(dc, r - 7, t + 2, r - 2, t + 7);
        Rectangle(dc, l + 2, b - 7, l + 7, b - 2);
        Rectangle(dc, r - 7, b - 7, r - 2, b - 2);
        break;
    case MenuIconGroup:
        {
            HBRUSH fillBrush = CreateSolidBrush(amber);
            HGDIOBJ previousBrush = SelectObject(dc, fillBrush);
            RoundRect(dc, l + 1, t + 4, r - 1, b - 1, 4, 4);
            SelectObject(dc, previousBrush);
            DeleteObject(fillBrush);
        }
        MoveToEx(dc, l + 2, t + 4, nullptr);
        LineTo(dc, l + 6, t + 1);
        LineTo(dc, l + 10, t + 4);
        break;
    case MenuIconTag:
        MoveToEx(dc, l + 3, t + 2, nullptr);
        LineTo(dc, r - 2, t + 2);
        LineTo(dc, r - 2, b - 6);
        LineTo(dc, (l + r) / 2, b - 2);
        LineTo(dc, l + 3, b - 6);
        LineTo(dc, l + 3, t + 2);
        break;
    case MenuIconTheme:
        {
            HBRUSH fillBrush = CreateSolidBrush(disabled ? RGB(170, 170, 170) : RGB(255, 255, 255));
            HGDIOBJ previousBrush = SelectObject(dc, fillBrush);
            Ellipse(dc, l + 1, t + 1, r - 1, b - 1);
            SelectObject(dc, previousBrush);
            DeleteObject(fillBrush);
        }
        {
            HBRUSH greenBrush = CreateSolidBrush(green);
            HGDIOBJ previousBrush = SelectObject(dc, greenBrush);
            Ellipse(dc, l + 4, t + 4, l + 8, t + 8);
            Ellipse(dc, r - 8, t + 5, r - 4, t + 9);
            Ellipse(dc, l + 7, b - 8, l + 11, b - 4);
            SelectObject(dc, previousBrush);
            DeleteObject(greenBrush);
        }
        break;
    case MenuIconEyeOff:
        Arc(dc, l + 1, t + 3, r - 1, b - 3, l + 3, (t + b) / 2, r - 3, (t + b) / 2);
        Arc(dc, l + 1, t + 3, r - 1, b - 3, r - 3, (t + b) / 2, l + 3, (t + b) / 2);
        MoveToEx(dc, l + 2, b - 2, nullptr);
        LineTo(dc, r - 2, t + 2);
        break;
    case MenuIconView:
        Rectangle(dc, l + 1, t + 3, r - 1, b - 3);
        MoveToEx(dc, l + 4, (t + b) / 2, nullptr);
        LineTo(dc, r - 4, (t + b) / 2);
        break;
    case MenuIconReward:
        {
            HBRUSH fillBrush = CreateSolidBrush(disabled ? RGB(170, 170, 170) : RGB(255, 232, 140));
            HGDIOBJ previousBrush = SelectObject(dc, fillBrush);
            Ellipse(dc, l + 2, t + 2, r - 2, b - 2);
            SelectObject(dc, previousBrush);
            DeleteObject(fillBrush);
        }
        MoveToEx(dc, (l + r) / 2, t + 5, nullptr);
        LineTo(dc, (l + r) / 2, b - 5);
        MoveToEx(dc, l + 6, t + 8, nullptr);
        LineTo(dc, r - 6, t + 8);
        MoveToEx(dc, l + 6, b - 8, nullptr);
        LineTo(dc, r - 6, b - 8);
        break;
    default:
        HPEN mutedPen = CreatePen(PS_SOLID, 2, mutedColor);
        SelectObject(dc, mutedPen);
        Rectangle(dc, l + 2, t + 2, r - 2, b - 2);
        SelectObject(dc, pen);
        DeleteObject(mutedPen);
        break;
    }

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void DrawMenuIcon(HDC dc, const RECT& rc, int icon, bool disabled, const std::filesystem::path& appDirectory) {
    if (icon == MenuIconNone) {
        return;
    }
    bool drawn = false;
    switch (icon) {
    case MenuIconFile: drawn = DrawStockMenuIcon(dc, rc, SIID_DOCNOASSOC); break;
    case MenuIconFolder: drawn = DrawStockMenuIcon(dc, rc, SIID_FOLDER); break;
    case MenuIconUrl: drawn = DrawStockMenuIcon(dc, rc, SIID_WORLD); break;
    case MenuIconRun: drawn = DrawStockMenuIcon(dc, rc, SIID_APPLICATION); break;
    case MenuIconShield: drawn = DrawStockMenuIcon(dc, rc, SIID_SHIELD); break;
    case MenuIconOpenFolder: drawn = DrawStockMenuIcon(dc, rc, SIID_FOLDEROPEN); break;
    case MenuIconShortcut: drawn = DrawStockMenuIcon(dc, rc, SIID_LINK); break;
    case MenuIconEdit: drawn = DrawStockMenuIcon(dc, rc, SIID_RENAME); break;
    case MenuIconGroup: drawn = DrawStockMenuIcon(dc, rc, SIID_FOLDER); break;
    case MenuIconInfo:
    case MenuIconAbout: drawn = DrawStockMenuIcon(dc, rc, SIID_INFO); break;
    case MenuIconDelete: drawn = DrawStockMenuIcon(dc, rc, SIID_DELETE); break;
    case MenuIconSearch: drawn = DrawStockMenuIcon(dc, rc, SIID_FIND); break;
    case MenuIconExit: drawn = DrawStockMenuIcon(dc, rc, SIID_ERROR); break;
    default:
        break;
    }
    if (!drawn) {
        drawn = DrawLocalMenuIcon(dc, rc, icon, disabled, appDirectory);
    }
    if (!drawn) {
        DrawFallbackMenuIcon(dc, rc, icon, disabled);
    }
}

bool LooksLikeUrlText(const std::wstring& value) {
    const std::wstring lower = ToLower(Trim(value));
    return lower.rfind(L"http://", 0) == 0 ||
           lower.rfind(L"https://", 0) == 0 ||
           lower.rfind(L"ftp://", 0) == 0 ||
           lower.rfind(L"www.", 0) == 0;
}

std::wstring SafeFileName(std::wstring name) {
    name = Trim(name);
    if (name.empty()) {
        name = L"Quattro";
    }
    for (wchar_t& ch : name) {
        if (ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' ||
            ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*') {
            ch = L'_';
        }
    }
    return name;
}

std::filesystem::path DesktopDirectory() {
    PWSTR desktop = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)) && desktop) {
        result = desktop;
        CoTaskMemFree(desktop);
    }
    return result;
}

bool CreateUrlShortcutFile(const Link& link, const std::filesystem::path& path) {
    const std::wstring url = NormalizeUrl(link.path);
    return WritePrivateProfileStringW(L"InternetShortcut", L"URL", url.c_str(), path.c_str()) != FALSE;
}

bool CreateShellShortcutFile(const Link& link, const std::filesystem::path& path) {
    IShellLinkW* shellLink = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink))) || !shellLink) {
        return false;
    }

    bool ok = false;
    if (!link.pidl.empty() && ShellItemService::IsPidlBlobPlausible(link.pidl)) {
        shellLink->SetIDList(reinterpret_cast<PCIDLIST_ABSOLUTE>(link.pidl.data()));
    } else {
        shellLink->SetPath(ExpandEnvironmentStringsSafe(link.path).c_str());
    }
    if (!Trim(link.parameter).empty()) {
        shellLink->SetArguments(link.parameter.c_str());
    }
    if (!Trim(link.workDir).empty()) {
        shellLink->SetWorkingDirectory(ExpandEnvironmentStringsSafe(link.workDir).c_str());
    }
    if (!Trim(link.icon).empty() && link.icon != L"#url") {
        shellLink->SetIconLocation(ExpandEnvironmentStringsSafe(link.icon).c_str(), 0);
    }
    shellLink->SetShowCmd(link.showCmd > 0 ? link.showCmd : SW_SHOWNORMAL);
    if (!Trim(link.remark).empty()) {
        shellLink->SetDescription(link.remark.c_str());
    }

    IPersistFile* persist = nullptr;
    if (SUCCEEDED(shellLink->QueryInterface(IID_PPV_ARGS(&persist))) && persist) {
        ok = SUCCEEDED(persist->Save(path.c_str(), TRUE));
        persist->Release();
    }
    shellLink->Release();
    return ok;
}
}

class OleDropTarget final : public IDropTarget {
public:
    explicit OleDropTarget(MainWindow* window) : window_(window) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) {
            return E_POINTER;
        }
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG count = InterlockedDecrement(&refCount_);
        if (count == 0) {
            delete this;
        }
        return static_cast<ULONG>(count);
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject*, DWORD, POINTL, DWORD* effect) override {
        if (effect) {
            *effect = DROPEFFECT_COPY;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        if (effect) {
            *effect = DROPEFFECT_COPY;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* dataObject, DWORD, POINTL, DWORD* effect) override {
        const bool imported = window_ && window_->ImportDropData(dataObject);
        if (effect) {
            *effect = imported ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        }
        return imported ? S_OK : DV_E_FORMATETC;
    }

private:
    LONG refCount_ = 1;
    MainWindow* window_ = nullptr;
};

MainWindow::MainWindow(
    HINSTANCE instance,
    std::filesystem::path appDirectory,
    ConfigService& configService,
    StorageService& storageService,
    AppConfig config,
    AppModel model,
    Theme theme)
    : instance_(instance),
      appDirectory_(std::move(appDirectory)),
      configService_(configService),
      storageService_(storageService),
      config_(std::move(config)),
      model_(std::move(model)),
      theme_(std::move(theme)),
      launcher_(appDirectory_, &config_),
      iconService_(appDirectory_) {
    SelectInitialItems();
}

MainWindow::~MainWindow() {
    if (tooltip_) {
        DestroyWindow(tooltip_);
        tooltip_ = nullptr;
    }
    DiscardDeviceResources();
    SafeRelease(titleFormat_);
    SafeRelease(textFormat_);
    SafeRelease(smallFormat_);
    SafeRelease(uiWicFactory_);
    SafeRelease(dwriteFactory_);
    SafeRelease(d2dFactory_);
}

bool MainWindow::Create() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = MainWindow::WindowProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
    wc.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
    wc.lpszClassName = L"QuattroMainWindow";
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_))) {
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dwriteFactory_)))) {
        return false;
    }
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uiWicFactory_)))) {
        return false;
    }

    dwriteFactory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"zh-cn", &titleFormat_);
    dwriteFactory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-cn", &textFormat_);
    dwriteFactory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"zh-cn", &smallFormat_);
    if (!titleFormat_ || !textFormat_ || !smallFormat_) {
        return false;
    }
    titleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    smallFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    titleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    textFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    smallFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    const DWORD style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    const POINT position = ClampWindowPosition(config_.posX, config_.posY, config_.width, config_.height);
    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_LAYERED,
        wc.lpszClassName,
        L"Quattro",
        style,
        position.x,
        position.y,
        config_.width,
        config_.height,
        nullptr,
        nullptr,
        instance_,
        this);
    if (!hwnd_) {
        return false;
    }

    SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>(config_.alpha), LWA_ALPHA);
    if (config_.topMost) {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    CreateTooltip();

    if (!config_.hideOnStart) {
        ShowWindowRespectFocusPolicy(hwnd_, SW_SHOWNORMAL);
        UpdateWindow(hwnd_);
    }
    oleDropTarget_ = new OleDropTarget(this);
    if (FAILED(RegisterDragDrop(hwnd_, oleDropTarget_))) {
        oleDropTarget_->Release();
        oleDropTarget_ = nullptr;
    }
    DragAcceptFiles(hwnd_, TRUE);
    RegisterConfiguredHotKeys();
    if (config_.autoDock || config_.hideWhenInactive) {
        dockTimerId_ = SetTimer(hwnd_, ID_TIMER_DOCK, 250, nullptr);
    }
    InitializeTrayIcon();
    return true;
}

int MainWindow::RunMessageLoop() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    MainWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<MainWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->hwnd_ = hwnd;
    } else {
        window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window) {
        return window->HandleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_QUATTRO_WAKEUP:
        WakeUp();
        return 0;
    case WM_NCCALCSIZE:
        return 0;
    case WM_NCPAINT:
        return 0;
    case WM_NCACTIVATE:
        return TRUE;
    case WM_QUATTRO_TRAY:
        if (LOWORD(lParam) == WM_LBUTTONUP) {
            WakeUp();
            return 0;
        }
        if (LOWORD(lParam) == WM_RBUTTONUP) {
            POINT point{};
            GetCursorPos(&point);
            ShowTrayMenu(point);
            return 0;
        }
        return 0;
    case WM_NCHITTEST: {
        POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        POINT clientPoint = screenPoint;
        ScreenToClient(hwnd_, &clientPoint);
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        const int frame = std::max(6, GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER));
        const bool left = clientPoint.x >= 0 && clientPoint.x < frame;
        const bool right = clientPoint.x <= width && clientPoint.x >= width - frame;
        const bool top = clientPoint.y >= 0 && clientPoint.y < frame;
        const bool bottom = clientPoint.y <= height && clientPoint.y >= height - frame;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;

        const int titleHeight = config_.showTitle ? 34 : 0;
        const int titleButtonsWidth = 116;
        if (clientPoint.y >= 0 && clientPoint.y < titleHeight && clientPoint.x < width - titleButtonsWidth) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_HOTKEY:
        if (wParam == ID_HOTKEY_MAIN) {
            if (IsEffectivelyVisible()) {
                HideMainWindow();
            } else {
                WakeUp();
            }
            return 0;
        }
        if (wParam == ID_HOTKEY_SEARCH) {
            OpenSearch();
            return 0;
        }
        if (wParam >= ID_HOTKEY_LINK_BASE) {
            const int linkId = LinkIdFromHotKeyId(static_cast<int>(wParam));
            if (linkId > 0) {
                RunLink(linkId);
            }
            return 0;
        }
        return 0;
    case WM_PAINT:
        OnPaint();
        return 0;
    case WM_DISPLAYCHANGE:
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_ACTIVATEAPP:
        if (!wParam && config_.hideWhenInactive && IsWindowVisible(hwnd_)) {
            HideMainWindow();
        } else if (wParam && dockHidden_) {
            WakeUp();
        }
        return 0;
    case WM_TIMER:
        if (wParam == ID_TIMER_DOCK) {
            UpdateDockState();
            return 0;
        }
        if (wParam == ID_TIMER_HOVER_ACTIVATE) {
            KillTimer(hwnd_, ID_TIMER_HOVER_ACTIVATE);
            hoverActivationTimerId_ = 0;
            if (pendingHoverActivationKind_ == HitKind::Group) {
                SelectGroup(pendingHoverActivationId_);
            } else if (pendingHoverActivationKind_ == HitKind::Tag) {
                SelectTag(pendingHoverActivationId_);
            }
            pendingHoverActivationKind_ = HitKind::None;
            pendingHoverActivationId_ = 0;
            return 0;
        }
        return 0;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(length, L'\0');
            DragQueryFileW(drop, i, path.data(), length + 1);
            ImportPath(path);
        }
        DragFinish(drop);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_SIZE:
        OnResize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_MOVE:
        HideLinkTooltip();
        SaveWindowState();
        return 0;
    case WM_MOUSEMOVE: {
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        HitArea next = HitTest(x, y);
        if (next.kind != hover_.kind || next.id != hover_.id) {
            hover_ = next;
            pendingHoverActivationKind_ = HitKind::None;
            pendingHoverActivationId_ = 0;
            if (hoverActivationTimerId_ != 0) {
                KillTimer(hwnd_, ID_TIMER_HOVER_ACTIVATE);
                hoverActivationTimerId_ = 0;
            }
            if (next.kind == HitKind::Group && config_.mouseEnterActiveGroup && next.id != currentGroupId_) {
                pendingHoverActivationKind_ = HitKind::Group;
                pendingHoverActivationId_ = next.id;
                const UINT delay = static_cast<UINT>(std::max(1, config_.activeGroupDelay));
                hoverActivationTimerId_ = SetTimer(hwnd_, ID_TIMER_HOVER_ACTIVATE, delay, nullptr);
            } else if (next.kind == HitKind::Tag && config_.mouseEnterActiveTag && next.id != currentTagId_) {
                pendingHoverActivationKind_ = HitKind::Tag;
                pendingHoverActivationId_ = next.id;
                const UINT delay = static_cast<UINT>(std::max(1, config_.activeTagDelay));
                hoverActivationTimerId_ = SetTimer(hwnd_, ID_TIMER_HOVER_ACTIVATE, delay, nullptr);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (!trackingMouse_) {
            TRACKMOUSEEVENT event{};
            event.cbSize = sizeof(event);
            event.dwFlags = TME_LEAVE;
            event.hwndTrack = hwnd_;
            TrackMouseEvent(&event);
            trackingMouse_ = true;
        }
        POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(hwnd_, &screenPoint);
        UpdateLinkTooltip(next, screenPoint);
        return 0;
    }
    case WM_MOUSELEAVE:
        HideLinkTooltip();
        trackingMouse_ = false;
        hover_ = {};
        pendingHoverActivationKind_ = HitKind::None;
        pendingHoverActivationId_ = 0;
        if (hoverActivationTimerId_ != 0) {
            KillTimer(hwnd_, ID_TIMER_HOVER_ACTIVATE);
            hoverActivationTimerId_ = 0;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL: {
        HideLinkTooltip();
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd_, &point);
        ScrollAtPoint(static_cast<float>(point.x), static_cast<float>(point.y), GET_WHEEL_DELTA_WPARAM(wParam), false);
        return 0;
    }
    case WM_MOUSEHWHEEL: {
        HideLinkTooltip();
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd_, &point);
        ScrollAtPoint(static_cast<float>(point.x), static_cast<float>(point.y), GET_WHEEL_DELTA_WPARAM(wParam), true);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        HideLinkTooltip();
        SetFocus(hwnd_);
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        HitArea hit = HitTest(x, y);
        switch (hit.kind) {
        case HitKind::CloseButton:
            HideMainWindow();
            return 0;
        case HitKind::SearchButton:
            OpenSearch();
            return 0;
        case HitKind::MenuButton:
            {
                POINT point{};
                GetCursorPos(&point);
                ShowMainMenu(point);
            }
            return 0;
        case HitKind::SkinButton:
            {
                POINT point{};
                GetCursorPos(&point);
                ShowThemeMenu(point);
            }
            return 0;
        case HitKind::AddButton:
            AddLink();
            return 0;
        case HitKind::Group:
            SelectGroup(hit.id);
            return 0;
        case HitKind::Tag:
            SelectTag(hit.id);
            return 0;
        case HitKind::Link:
            selectedLinkId_ = hit.id;
            if (!config_.doubleClickToRun) {
                RunLink(hit.id);
            } else {
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        default:
            return 0;
        }
    }
    case WM_RBUTTONUP: {
        HideLinkTooltip();
        SetFocus(hwnd_);
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        HitArea hit = HitTest(x, y);
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(hwnd_, &point);
        if (hit.kind == HitKind::Link) {
            selectedLinkId_ = hit.id;
            ShowLinkMenu(hit.id, point);
        } else if (hit.kind == HitKind::Group) {
            ShowGroupMenu(hit.id, point);
        } else if (hit.kind == HitKind::Tag) {
            ShowTagMenu(hit.id, point);
        } else {
            RECT client{};
            D2D1_RECT_F title{}, groups{}, tags{}, links{};
            if (GetClientRect(hwnd_, &client)) {
                BuildLayout(static_cast<float>(client.right - client.left),
                            static_cast<float>(client.bottom - client.top),
                            title,
                            groups,
                            tags,
                            links);
            }
            if (Width(groups) > 0.0f && Height(groups) > 0.0f && Contains(groups, x, y)) {
                ShowGroupBlankMenu(point);
            } else if (Width(tags) > 0.0f && Height(tags) > 0.0f && Contains(tags, x, y)) {
                ShowTagBlankMenu(point);
            } else {
                ShowBackgroundMenu(point);
            }
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        HideLinkTooltip();
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        HitArea hit = HitTest(x, y);
        if (hit.kind == HitKind::Link) {
            RunLink(hit.id);
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 260;
        info->ptMinTrackSize.y = 320;
        return 0;
    }
    case WM_CLOSE:
        if (!trayIconVisible_) {
            config_.hideNotifyIcon = false;
            InitializeTrayIcon();
        }
        SaveWindowState();
        HideMainWindow();
        return 0;
    case WM_COMMAND:
        {
        const UINT command = LOWORD(wParam);
        if (command >= ID_MENU_MOVE_TO_BASE && command < ID_MENU_MOVE_TO_BASE + ID_MENU_DYNAMIC_TARGET_LIMIT) {
            const std::size_t targetIndex = command - ID_MENU_MOVE_TO_BASE;
            std::vector<int> targets = menuMoveTargetIds_;
            if (targetIndex >= targets.size()) {
                Link* link = FindLink(CommandLinkId());
                targets = GroupedTagTargetIds(link ? link->parentGroup : 0);
            }
            if (targetIndex < targets.size()) {
                MoveLinkToTag(CommandLinkId(), targets[targetIndex]);
            }
            return 0;
        }
        if (command >= ID_MENU_COPY_TO_BASE && command < ID_MENU_COPY_TO_BASE + ID_MENU_DYNAMIC_TARGET_LIMIT) {
            const std::size_t targetIndex = command - ID_MENU_COPY_TO_BASE;
            std::vector<int> targets = menuCopyTargetIds_;
            if (targetIndex >= targets.size()) {
                targets = GroupedTagTargetIds(0);
            }
            if (targetIndex < targets.size()) {
                CopyLinkToTag(CommandLinkId(), targets[targetIndex]);
            }
            return 0;
        }
        if (command >= ID_MENU_MOVE_TAG_TO_BASE && command < ID_MENU_MOVE_TAG_TO_BASE + ID_MENU_DYNAMIC_TARGET_LIMIT) {
            const std::size_t targetIndex = command - ID_MENU_MOVE_TAG_TO_BASE;
            std::vector<int> targets = menuGroupTargetIds_;
            if (targetIndex >= targets.size()) {
                const Group* tag = FindGroup(CommandTagId());
                targets = GroupTargetIds(tag ? tag->parentGroup : 0);
            }
            if (targetIndex < targets.size()) {
                MoveTagToGroup(CommandTagId(), targets[targetIndex]);
            }
            return 0;
        }
        if (command >= ID_MENU_THEME_BASE && command < ID_MENU_THEME_BASE + 100) {
            const auto themeItems = DiscoverThemeMenuItems(appDirectory_ / L"theme", config_.theme);
            const std::size_t themeIndex = command - ID_MENU_THEME_BASE;
            if (themeIndex < themeItems.size()) {
                ApplyTheme(themeItems[themeIndex].name);
            }
            return 0;
        }
        if (command >= ID_MENU_SYSTEM_FUNCTION_BASE && command < ID_MENU_SYSTEM_FUNCTION_BASE + ID_MENU_SYSTEM_FUNCTION_LIMIT) {
            OpenSystemFunction(static_cast<std::size_t>(command - ID_MENU_SYSTEM_FUNCTION_BASE));
            return 0;
        }
        switch (command) {
        case ID_MENU_ADD_LINK:
            AddLink();
            return 0;
        case ID_MENU_ADD_FILE:
            AddFile();
            return 0;
        case ID_MENU_ADD_FOLDER:
            AddFolder();
            return 0;
        case ID_MENU_ADD_URL:
            AddUrl();
            return 0;
        case ID_MENU_ADD_SYSTEM:
            AddSystemFunction();
            return 0;
        case ID_MENU_CLEAR_TAG_LINKS:
            ClearCurrentTagLinks();
            return 0;
        case ID_MENU_RUN_LINK:
            RunLink(CommandLinkId());
            return 0;
        case ID_MENU_RUN_ADMIN:
            RunLinkAsAdmin(CommandLinkId());
            return 0;
        case ID_MENU_RUN_PRIVATE:
            RunUrlPrivate(CommandLinkId());
            return 0;
        case ID_MENU_EDIT_LINK:
            EditLink(CommandLinkId());
            return 0;
        case ID_MENU_DELETE_LINK:
            DeleteLink(CommandLinkId());
            return 0;
        case ID_MENU_MOVE_UP:
            MoveMenuContext(-1);
            return 0;
        case ID_MENU_MOVE_DOWN:
            MoveMenuContext(1);
            return 0;
        case ID_MENU_OPEN_LOCATION:
            OpenContainingFolder(CommandLinkId());
            return 0;
        case ID_MENU_COPY_PATH:
            CopyLinkPath(CommandLinkId());
            return 0;
        case ID_MENU_COPY_URL:
            CopyLinkPath(CommandLinkId());
            return 0;
        case ID_MENU_WINDOWS_CONTEXT:
            {
                POINT point{};
                GetCursorPos(&point);
                ShowWindowsContextMenu(CommandLinkId(), point);
            }
            return 0;
        case ID_MENU_CREATE_DESKTOP_SHORTCUT:
            CreateDesktopShortcut(CommandLinkId());
            return 0;
        case ID_MENU_PROPERTIES:
            OpenSystemProperties(CommandLinkId());
            return 0;
        case ID_MENU_REFRESH_LINK_ICON:
            RefreshLinkIcon(CommandLinkId());
            return 0;
        case ID_MENU_REFRESH_PAGE_ICONS:
            {
                const int tagId = CommandTagId();
                for (const auto& link : model_.links) {
                    if (link.parentGroup == tagId) {
                        iconService_.RefreshDiskCache(link);
                    }
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case ID_MENU_REPAIR_LINK:
            if (Link* link = FindLink(CommandLinkId())) {
                if (TryRepairLinkTarget(*link)) {
                    storageService_.UpdateLink(*link);
                    RegisterConfiguredHotKeys();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
            return 0;
        case ID_MENU_COPY_LINK:
            CopyLinkInternal(CommandLinkId(), false);
            return 0;
        case ID_MENU_CUT_LINK:
            CopyLinkInternal(CommandLinkId(), true);
            return 0;
        case ID_MENU_PASTE_LINK:
            PasteLinkInternal();
            return 0;
        case ID_MENU_ADD_GROUP:
            AddGroup();
            return 0;
        case ID_MENU_EDIT_GROUP:
            EditGroup(CommandGroupId());
            return 0;
        case ID_MENU_DELETE_GROUP:
            DeleteGroup(CommandGroupId());
            return 0;
        case ID_MENU_ADD_TAG:
            AddTag();
            return 0;
        case ID_MENU_EDIT_TAG:
            EditTag(CommandTagId());
            return 0;
        case ID_MENU_DELETE_TAG:
            DeleteTag(CommandTagId());
            return 0;
        case ID_MENU_SORT_POS:
            SetCurrentTagSort(0);
            return 0;
        case ID_MENU_SORT_RUNCOUNT:
            SetCurrentTagSort(1);
            return 0;
        case ID_MENU_SORT_NAME:
            SetCurrentTagSort(2);
            return 0;
        case ID_MENU_LAYOUT_LIST:
            SetCurrentTagLayout(0);
            return 0;
        case ID_MENU_LAYOUT_TILE:
            SetCurrentTagLayout(1);
            return 0;
        case ID_MENU_ICON_SMALL:
            SetCurrentTagIconSize(24);
            return 0;
        case ID_MENU_ICON_MEDIUM:
            SetCurrentTagIconSize(32);
            return 0;
        case ID_MENU_ICON_LARGE:
            SetCurrentTagIconSize(48);
            return 0;
        case ID_MENU_ALL_SORT_POS:
            SetAllTagsSort(0);
            return 0;
        case ID_MENU_ALL_SORT_RUNCOUNT:
            SetAllTagsSort(1);
            return 0;
        case ID_MENU_ALL_SORT_NAME:
            SetAllTagsSort(2);
            return 0;
        case ID_MENU_ALL_LAYOUT_LIST:
            SetAllTagsLayout(0);
            return 0;
        case ID_MENU_ALL_LAYOUT_TILE:
            SetAllTagsLayout(1);
            return 0;
        case ID_MENU_ALL_ICON_SMALL:
            SetAllTagsIconSize(24);
            return 0;
        case ID_MENU_ALL_ICON_MEDIUM:
            SetAllTagsIconSize(32);
            return 0;
        case ID_MENU_ALL_ICON_LARGE:
            SetAllTagsIconSize(48);
            return 0;
        case ID_MENU_TOGGLE_TITLE:
            ToggleConfigVisibility(&AppConfig::showTitle);
            return 0;
        case ID_MENU_TOGGLE_GROUP:
            ToggleConfigVisibility(&AppConfig::showGroup);
            return 0;
        case ID_MENU_TOGGLE_TAG:
            ToggleConfigVisibility(&AppConfig::showTag);
            return 0;
        case ID_MENU_TOGGLE_TOPMOST:
            ToggleConfigVisibility(&AppConfig::topMost);
            return 0;
        case ID_MENU_SEARCH:
            OpenSearch();
            return 0;
        case ID_MENU_SETTINGS:
            OpenSettings();
            return 0;
        case ID_MENU_RESET_LAYOUT:
            ResetLayoutToDefaults();
            return 0;
        case ID_MENU_IMPORT_CLIPBOARD:
            ImportClipboard();
            return 0;
        case ID_MENU_CLEAR_ICON_CACHE:
            ClearIconCache();
            return 0;
        case ID_MENU_REFRESH_ALL_ICONS:
            RefreshAllIcons();
            return 0;
        case ID_MENU_ABOUT:
            ShowAbout();
            return 0;
        case ID_MENU_HELP:
            OpenHelp();
            return 0;
        case ID_MENU_FAQ:
            OpenFaq();
            return 0;
        case ID_MENU_REWARD:
            OpenReward();
            return 0;
        case ID_MENU_UPDATE:
            ShowUpdateInfo();
            return 0;
        case ID_MENU_SHOW:
            if (IsEffectivelyVisible()) {
                HideMainWindow();
            } else {
                WakeUp();
            }
            return 0;
        case ID_MENU_EXIT:
            DestroyWindow(hwnd_);
            return 0;
        default:
            return 0;
        }
        }
    case WM_MEASUREITEM:
        if (MeasureThemedMenuItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lParam))) {
            return TRUE;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    case WM_DRAWITEM:
        if (DrawThemedMenuItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
            return TRUE;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    case WM_DESTROY:
        HideLinkTooltip();
        if (tooltip_) {
            DestroyWindow(tooltip_);
            tooltip_ = nullptr;
        }
        if (dockTimerId_ != 0) {
            KillTimer(hwnd_, dockTimerId_);
            dockTimerId_ = 0;
        }
        if (hoverActivationTimerId_ != 0) {
            KillTimer(hwnd_, ID_TIMER_HOVER_ACTIVATE);
            hoverActivationTimerId_ = 0;
        }
        DragAcceptFiles(hwnd_, FALSE);
        RevokeDragDrop(hwnd_);
        if (oleDropTarget_) {
            oleDropTarget_->Release();
            oleDropTarget_ = nullptr;
        }
        UnregisterConfiguredHotKeys();
        RemoveTrayIcon();
        SaveWindowState();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }
}

void MainWindow::SelectInitialItems() {
    const auto majors = MajorGroups();
    if (majors.empty()) {
        return;
    }

    currentGroupId_ = config_.currentGroupId;
    if (!FindGroup(currentGroupId_) || FindGroup(currentGroupId_)->parentGroup != 0) {
        currentGroupId_ = majors.front().id;
    }

    const auto tags = TagsForCurrentGroup();
    currentTagId_ = config_.currentTagId;
    Group* tag = FindGroup(currentTagId_);
    if (!tag || tag->parentGroup != currentGroupId_) {
        currentTagId_ = tags.empty() ? 0 : tags.front().id;
    }
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
}

void MainWindow::SelectGroup(int groupId) {
    Group* group = FindGroup(groupId);
    if (!group || group->parentGroup != 0 || currentGroupId_ == groupId) {
        return;
    }

    currentGroupId_ = groupId;
    const auto tags = TagsForCurrentGroup();
    currentTagId_ = tags.empty() ? 0 : tags.front().id;
    tagScrollOffset_ = 0.0f;
    linkScrollOffset_ = 0.0f;
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    EnsureGroupVisible(currentGroupId_);
    EnsureTagVisible(currentTagId_);
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SelectTag(int tagId) {
    Group* tag = FindGroup(tagId);
    if (!tag || tag->parentGroup != currentGroupId_ || currentTagId_ == tagId) {
        return;
    }

    currentTagId_ = tagId;
    linkScrollOffset_ = 0.0f;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int MainWindow::CommandGroupId() const {
    if (menuContextKind_ == HitKind::Group) {
        return menuContextId_;
    }
    if (menuContextKind_ == HitKind::Tag) {
        const Group* tag = FindGroup(menuContextId_);
        if (tag && tag->parentGroup > 0) {
            return tag->parentGroup;
        }
    }
    return currentGroupId_;
}

int MainWindow::CommandTagId() const {
    if (menuContextKind_ == HitKind::Tag) {
        return menuContextId_;
    }
    return currentTagId_;
}

int MainWindow::CommandLinkId() const {
    if (menuContextKind_ == HitKind::Link) {
        return menuContextId_;
    }
    return selectedLinkId_;
}

void MainWindow::AddGroup() {
    std::wstring name = UniqueSiblingGroupName(model_.groups, 0, L"新的分组");
    if (!ShowTextInputDialog(hwnd_, instance_, theme_, L"新建分组", L"分组名称", name)) {
        return;
    }

    Group group;
    group.name = name;
    group.parentGroup = 0;
    group.pos = -1;
    if (!storageService_.InsertGroup(group)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"新增分组", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.groups.push_back(group);

    Group defaultTag;
    defaultTag.name = UniqueSiblingGroupName(model_.groups, group.id, L"新的标签");
    defaultTag.parentGroup = group.id;
    defaultTag.pos = -1;
    defaultTag.layout = 1;
    defaultTag.iconSize = 32;
    if (storageService_.InsertGroup(defaultTag)) {
        model_.groups.push_back(defaultTag);
        currentTagId_ = defaultTag.id;
    } else {
        currentTagId_ = 0;
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"新增标签", MB_OK | MB_ICONWARNING);
    }

    currentGroupId_ = group.id;
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    EnsureGroupVisible(currentGroupId_);
    EnsureTagVisible(currentTagId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::EditGroup(int groupId) {
    Group* group = FindGroup(groupId);
    if (!group || group->parentGroup != 0) {
        return;
    }
    std::wstring name = group->name;
    if (!ShowTextInputDialog(hwnd_, instance_, theme_, L"编辑分组", L"分组名称", name)) {
        return;
    }
    Group edited = *group;
    edited.name = name;
    if (!storageService_.UpdateGroup(edited)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"编辑分组", MB_OK | MB_ICONWARNING);
        return;
    }
    *group = edited;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::DeleteGroup(int groupId) {
    Group* group = FindGroup(groupId);
    if (!group || group->parentGroup != 0) {
        return;
    }
    std::wstring message = L"确定删除分组“" + group->name + L"”及其标签和启动项？";
    if (MessageBoxW(hwnd_, message.c_str(), L"删除分组", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    if (!storageService_.DeleteGroup(groupId)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"删除分组", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.links.erase(std::remove_if(model_.links.begin(), model_.links.end(), [&](const Link& link) {
        Group* tag = FindGroup(link.parentGroup);
        return tag && tag->parentGroup == groupId;
    }), model_.links.end());
    model_.groups.erase(std::remove_if(model_.groups.begin(), model_.groups.end(), [groupId](const Group& item) {
        return item.id == groupId || item.parentGroup == groupId;
    }), model_.groups.end());
    RegisterConfiguredHotKeys();
    SelectInitialItems();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::AddTag() {
    const int parentGroupId = CommandGroupId();
    Group* parent = FindGroup(parentGroupId);
    if (!parent || parent->parentGroup != 0) {
        AddGroup();
        return;
    }
    std::wstring name = UniqueSiblingGroupName(model_.groups, parentGroupId, L"新的标签");
    if (!ShowTextInputDialog(hwnd_, instance_, theme_, L"新建标签", L"标签名称", name)) {
        return;
    }

    Group tag;
    tag.name = name;
    tag.parentGroup = parentGroupId;
    tag.pos = -1;
    tag.layout = 1;
    tag.iconSize = 32;
    if (name == L"全部") {
        tag.type = 1;
        tag.content = L"all";
    } else if (name == L"待办") {
        tag.type = 2;
        tag.content = L"todo";
    }
    if (!storageService_.InsertGroup(tag)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"新增标签", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.groups.push_back(tag);
    currentGroupId_ = parentGroupId;
    currentTagId_ = tag.id;
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    EnsureGroupVisible(currentGroupId_);
    EnsureTagVisible(currentTagId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::EditTag(int tagId) {
    Group* tag = FindGroup(tagId);
    if (!tag || tag->parentGroup == 0) {
        return;
    }
    std::wstring name = tag->name;
    if (!ShowTextInputDialog(hwnd_, instance_, theme_, L"编辑标签", L"标签名称", name)) {
        return;
    }
    Group edited = *tag;
    edited.name = name;
    if (name == L"全部") {
        edited.type = 1;
        edited.content = L"all";
    } else if (name == L"待办") {
        edited.type = 2;
        edited.content = L"todo";
    } else if (edited.type == 1 || edited.type == 2) {
        edited.type = 0;
        edited.content.clear();
    }
    if (!storageService_.UpdateGroup(edited)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"编辑标签", MB_OK | MB_ICONWARNING);
        return;
    }
    *tag = edited;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::DeleteTag(int tagId) {
    Group* tag = FindGroup(tagId);
    if (!tag || tag->parentGroup == 0) {
        return;
    }
    std::wstring message = L"确定删除标签“" + tag->name + L"”及其启动项？";
    if (MessageBoxW(hwnd_, message.c_str(), L"删除标签", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    if (!storageService_.DeleteGroup(tagId)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"删除标签", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.links.erase(std::remove_if(model_.links.begin(), model_.links.end(), [tagId](const Link& link) {
        return link.parentGroup == tagId;
    }), model_.links.end());
    model_.groups.erase(std::remove_if(model_.groups.begin(), model_.groups.end(), [tagId](const Group& item) {
        return item.id == tagId;
    }), model_.groups.end());
    RegisterConfiguredHotKeys();
    SelectInitialItems();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SetCurrentTagSort(int sort) {
    // View options are stored per tag page; menu context may point at a non-current tag.
    Group* tag = FindGroup(CommandTagId());
    if (!tag || tag->parentGroup == 0) {
        return;
    }
    Group edited = *tag;
    edited.sort = sort;
    if (!storageService_.UpdateGroup(edited)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"排序", MB_OK | MB_ICONWARNING);
        return;
    }
    *tag = edited;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SetCurrentTagLayout(int layout) {
    // View options are stored per tag page; menu context may point at a non-current tag.
    Group* tag = FindGroup(CommandTagId());
    if (!tag || tag->parentGroup == 0) {
        return;
    }
    Group edited = *tag;
    edited.layout = layout == 0 ? 0 : 1;
    if (!storageService_.UpdateGroup(edited)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"布局", MB_OK | MB_ICONWARNING);
        return;
    }
    *tag = edited;
    linkScrollOffset_ = 0.0f;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SetCurrentTagIconSize(int iconSize) {
    // View options are stored per tag page; menu context may point at a non-current tag.
    Group* tag = FindGroup(CommandTagId());
    if (!tag || tag->parentGroup == 0) {
        return;
    }
    Group edited = *tag;
    if (iconSize <= 24) {
        edited.iconSize = 24;
    } else if (iconSize >= 48) {
        edited.iconSize = 48;
    } else {
        edited.iconSize = 32;
    }
    if (!storageService_.UpdateGroup(edited)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"图标尺寸", MB_OK | MB_ICONWARNING);
        return;
    }
    *tag = edited;
    linkScrollOffset_ = 0.0f;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SetAllTagsSort(int sort) {
    for (Group& tag : model_.groups) {
        if (tag.parentGroup == 0) {
            continue;
        }
        Group edited = tag;
        edited.sort = sort;
        if (!storageService_.UpdateGroup(edited)) {
            MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"统一排序方式", MB_OK | MB_ICONWARNING);
            return;
        }
        tag = edited;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SetAllTagsLayout(int layout) {
    for (Group& tag : model_.groups) {
        if (tag.parentGroup == 0) {
            continue;
        }
        Group edited = tag;
        edited.layout = layout == 0 ? 0 : 1;
        if (!storageService_.UpdateGroup(edited)) {
            MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"统一查看方式", MB_OK | MB_ICONWARNING);
            return;
        }
        tag = edited;
    }
    linkScrollOffset_ = 0.0f;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SetAllTagsIconSize(int iconSize) {
    int normalized = 32;
    if (iconSize <= 24) {
        normalized = 24;
    } else if (iconSize >= 48) {
        normalized = 48;
    }

    for (Group& tag : model_.groups) {
        if (tag.parentGroup == 0) {
            continue;
        }
        Group edited = tag;
        edited.iconSize = normalized;
        if (!storageService_.UpdateGroup(edited)) {
            MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"统一图标大小", MB_OK | MB_ICONWARNING);
            return;
        }
        tag = edited;
    }
    linkScrollOffset_ = 0.0f;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ToggleConfigVisibility(bool AppConfig::*field) {
    AppConfig previous = config_;
    config_.*field = !(config_.*field);
    configService_.SaveWindowState(config_);
    ApplyConfigRuntimeChanges(previous);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int MainWindow::EnsureCurrentTag() {
    Group* tag = FindGroup(currentTagId_);
    if (tag && tag->parentGroup != 0) {
        return currentTagId_;
    }

    Group* group = FindGroup(currentGroupId_);
    if (!group || group->parentGroup != 0) {
        Group createdGroup;
        createdGroup.name = UniqueSiblingGroupName(model_.groups, 0, L"默认分组");
        createdGroup.parentGroup = 0;
        createdGroup.pos = -1;
        if (!storageService_.InsertGroup(createdGroup)) {
            MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"创建默认分组", MB_OK | MB_ICONWARNING);
            return 0;
        }
        model_.groups.push_back(createdGroup);
        currentGroupId_ = createdGroup.id;
    }

    Group createdTag;
    createdTag.name = UniqueSiblingGroupName(model_.groups, currentGroupId_, L"默认标签");
    createdTag.parentGroup = currentGroupId_;
    createdTag.pos = -1;
    createdTag.layout = 1;
    createdTag.iconSize = 32;
    if (!storageService_.InsertGroup(createdTag)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"创建默认标签", MB_OK | MB_ICONWARNING);
        return 0;
    }
    model_.groups.push_back(createdTag);
    currentTagId_ = createdTag.id;
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    return currentTagId_;
}

void MainWindow::AddLink() {
    if (EnsureCurrentTag() <= 0) {
        return;
    }
    Link link;
    link.parentGroup = CommandTagId();
    link.type = 0;
    link.pos = -1;
    link.showCmd = SW_SHOWNORMAL;
    if (!LinkEditDialog::Show(hwnd_, instance_, theme_, link, model_.groups, true)) {
        return;
    }
    if (!storageService_.InsertLink(link)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"新增启动项", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.links.push_back(link);
    RegisterConfiguredHotKeys();
    selectedLinkId_ = link.id;
    currentTagId_ = link.parentGroup;
    if (Group* tag = FindGroup(currentTagId_); tag && tag->parentGroup != 0) {
        currentGroupId_ = tag->parentGroup;
    }
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    EnsureGroupVisible(currentGroupId_);
    EnsureTagVisible(currentTagId_);
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::AddFile() {
    if (EnsureCurrentTag() <= 0) {
        return;
    }

    std::wstring buffer(32768, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrFilter = L"所有文件\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) {
        return;
    }
    ImportPath(buffer.c_str());
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::AddFolder() {
    if (EnsureCurrentTag() <= 0) {
        return;
    }

    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) || !dialog) {
        return;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (SUCCEEDED(dialog->Show(hwnd_))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                ImportPath(path);
                CoTaskMemFree(path);
                EnsureLinkVisible(selectedLinkId_);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            item->Release();
        }
    }
    dialog->Release();
}

void MainWindow::AddUrl() {
    if (EnsureCurrentTag() <= 0) {
        return;
    }

    Link link;
    link.parentGroup = currentTagId_;
    link.type = 2;
    link.icon = L"#url";
    link.pos = -1;
    link.showCmd = SW_SHOWNORMAL;
    if (!UrlEditDialog::Show(hwnd_, instance_, theme_, link, true)) {
        return;
    }
    if (!storageService_.InsertLink(link)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"添加网址", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.links.push_back(link);
    selectedLinkId_ = link.id;
    RegisterConfiguredHotKeys();
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::AddSystemFunction() {
    if (EnsureCurrentTag() <= 0) {
        return;
    }

    Link link;
    link.parentGroup = currentTagId_;
    link.pos = -1;
    link.showCmd = SW_SHOWNORMAL;
    if (!SystemFunctionDialog::Show(hwnd_, instance_, theme_, link)) {
        return;
    }
    if (!storageService_.InsertLink(link)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"添加系统功能", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.links.push_back(link);
    selectedLinkId_ = link.id;
    RegisterConfiguredHotKeys();
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OpenSystemFunction(std::size_t index) {
    Link link;
    if (!ConfigureSystemFunctionLink(index, link)) {
        return;
    }

    std::wstring error;
    if (!launcher_.Run(link, error)) {
        const std::wstring message = error.empty() ? L"打开系统功能失败。" : error;
        MessageBoxW(hwnd_, message.c_str(), L"系统功能", MB_OK | MB_ICONWARNING);
    }
}

void MainWindow::EditLink(int linkId) {
    Link* existing = FindLink(linkId);
    if (!existing) {
        return;
    }
    Link edited = *existing;
    if (!LinkEditDialog::Show(hwnd_, instance_, theme_, edited, model_.groups, false)) {
        return;
    }
    if (!storageService_.UpdateLink(edited)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"编辑启动项", MB_OK | MB_ICONWARNING);
        return;
    }
    *existing = edited;
    RegisterConfiguredHotKeys();
    selectedLinkId_ = edited.id;
    currentTagId_ = edited.parentGroup;
    if (Group* tag = FindGroup(currentTagId_); tag && tag->parentGroup != 0) {
        currentGroupId_ = tag->parentGroup;
    }
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::DeleteLink(int linkId) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }
    if (config_.deleteConfirm) {
        std::wstring message = L"确定删除启动项“" + link->name + L"”？";
        if (MessageBoxW(hwnd_, message.c_str(), L"删除启动项", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
            return;
        }
    }
    if (!storageService_.DeleteLink(linkId)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"删除启动项", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.links.erase(std::remove_if(model_.links.begin(), model_.links.end(), [linkId](const Link& item) {
        return item.id == linkId;
    }), model_.links.end());
    RegisterConfiguredHotKeys();
    if (selectedLinkId_ == linkId) {
        selectedLinkId_ = 0;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::CopyLinkInternal(int linkId, bool cut) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }
    clipboardLink_ = *link;
    clipboardSourceId_ = linkId;
    clipboardCut_ = cut;
    hasClipboardLink_ = true;
}

void MainWindow::PasteLinkInternal() {
    if (!hasClipboardLink_) {
        return;
    }
    int targetTagId = currentTagId_;
    if (menuContextKind_ == HitKind::Tag) {
        targetTagId = menuContextId_;
    } else if (menuContextKind_ == HitKind::Link) {
        if (Link* contextLink = FindLink(menuContextId_)) {
            targetTagId = contextLink->parentGroup;
        }
    }

    Group* tag = FindGroup(targetTagId);
    if (!tag || tag->parentGroup == 0) {
        MessageBoxW(hwnd_, L"请先选择一个标签。", L"粘贴启动项", MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (clipboardCut_) {
        Link* source = FindLink(clipboardSourceId_);
        if (!source) {
            hasClipboardLink_ = false;
            MessageBoxW(hwnd_, L"原启动项不存在，无法剪切粘贴。", L"粘贴启动项", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (source->parentGroup != targetTagId) {
            MoveLinkToTag(source->id, targetTagId);
        } else {
            selectedLinkId_ = source->id;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        hasClipboardLink_ = false;
        clipboardCut_ = false;
        clipboardSourceId_ = 0;
        return;
    }

    Link copy = clipboardLink_;
    copy.id = 0;
    copy.parentGroup = targetTagId;
    copy.pos = -1;
    copy.hotKey = 0;
    copy.runCount = 0;
    if (!storageService_.InsertLink(copy)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"粘贴启动项", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.links.push_back(copy);
    selectedLinkId_ = copy.id;
    currentTagId_ = targetTagId;
    currentGroupId_ = tag->parentGroup;
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    RegisterConfiguredHotKeys();
    EnsureGroupVisible(currentGroupId_);
    EnsureTagVisible(currentTagId_);
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::MoveMenuContext(int direction) {
    if (menuContextKind_ == HitKind::Link || (menuContextKind_ == HitKind::None && selectedLinkId_ > 0)) {
        MoveLinkWithinTag(CommandLinkId(), direction);
    } else if (menuContextKind_ == HitKind::Group || menuContextKind_ == HitKind::Tag) {
        MoveGroupWithinParent(menuContextId_, direction);
    }
}

void MainWindow::MoveLinkWithinTag(int linkId, int direction) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }

    Group* tag = FindGroup(link->parentGroup);
    if (tag && tag->sort != 0) {
        Group edited = *tag;
        edited.sort = 0;
        if (storageService_.UpdateGroup(edited)) {
            *tag = edited;
        }
    }

    std::vector<Link*> siblings;
    for (auto& item : model_.links) {
        if (item.parentGroup == link->parentGroup) {
            siblings.push_back(&item);
        }
    }
    std::sort(siblings.begin(), siblings.end(), [](const Link* left, const Link* right) {
        if (left->pos != right->pos) {
            return left->pos < right->pos;
        }
        return left->id < right->id;
    });

    auto it = std::find(siblings.begin(), siblings.end(), link);
    if (it == siblings.end()) {
        return;
    }
    const int index = static_cast<int>(std::distance(siblings.begin(), it));
    const int targetIndex = index + (direction < 0 ? -1 : 1);
    if (targetIndex < 0 || targetIndex >= static_cast<int>(siblings.size())) {
        return;
    }

    std::swap(siblings[index], siblings[targetIndex]);
    for (int i = 0; i < static_cast<int>(siblings.size()); ++i) {
        Link edited = *siblings[i];
        edited.pos = i;
        if (!storageService_.UpdateLink(edited)) {
            MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"移动启动项", MB_OK | MB_ICONWARNING);
            return;
        }
        *siblings[i] = edited;
    }
    selectedLinkId_ = linkId;
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::MoveGroupWithinParent(int groupId, int direction) {
    Group* group = FindGroup(groupId);
    if (!group) {
        return;
    }

    std::vector<Group*> siblings;
    for (auto& item : model_.groups) {
        if (item.parentGroup == group->parentGroup) {
            siblings.push_back(&item);
        }
    }
    std::sort(siblings.begin(), siblings.end(), [](const Group* left, const Group* right) {
        if (left->pos != right->pos) {
            return left->pos < right->pos;
        }
        return left->id < right->id;
    });

    auto it = std::find(siblings.begin(), siblings.end(), group);
    if (it == siblings.end()) {
        return;
    }
    const int index = static_cast<int>(std::distance(siblings.begin(), it));
    const int targetIndex = index + (direction < 0 ? -1 : 1);
    if (targetIndex < 0 || targetIndex >= static_cast<int>(siblings.size())) {
        return;
    }

    std::swap(siblings[index], siblings[targetIndex]);
    for (int i = 0; i < static_cast<int>(siblings.size()); ++i) {
        Group edited = *siblings[i];
        edited.pos = i;
        if (!storageService_.UpdateGroup(edited)) {
            MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"移动分组", MB_OK | MB_ICONWARNING);
            return;
        }
        *siblings[i] = edited;
    }
    if (group->parentGroup == 0) {
        currentGroupId_ = groupId;
        EnsureGroupVisible(currentGroupId_);
    } else {
        currentTagId_ = groupId;
        EnsureTagVisible(currentTagId_);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::MoveTagToGroup(int tagId, int groupId) {
    Group* tag = FindGroup(tagId);
    Group* targetGroup = FindGroup(groupId);
    if (!tag || tag->parentGroup == 0 || !targetGroup || targetGroup->parentGroup != 0 || tag->parentGroup == groupId) {
        return;
    }

    Group edited = *tag;
    edited.parentGroup = groupId;
    edited.pos = NextModelGroupPosition(model_.groups, groupId);
    if (!storageService_.UpdateGroup(edited)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"移动标签", MB_OK | MB_ICONWARNING);
        return;
    }

    *tag = edited;
    currentGroupId_ = groupId;
    currentTagId_ = tagId;
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    tagScrollOffset_ = 0.0f;
    linkScrollOffset_ = 0.0f;
    EnsureGroupVisible(currentGroupId_);
    EnsureTagVisible(currentTagId_);
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::MoveLinkToTag(int linkId, int tagId) {
    Link* link = FindLink(linkId);
    Group* tag = FindGroup(tagId);
    if (!link || !tag || tag->parentGroup == 0 || link->parentGroup == tagId) {
        return;
    }

    Link edited = *link;
    edited.parentGroup = tagId;
    edited.pos = NextModelLinkPosition(model_.links, tagId);
    if (!storageService_.UpdateLink(edited)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"移动启动项", MB_OK | MB_ICONWARNING);
        return;
    }
    *link = edited;
    selectedLinkId_ = edited.id;
    currentGroupId_ = tag->parentGroup;
    currentTagId_ = tag->id;
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    EnsureGroupVisible(currentGroupId_);
    EnsureTagVisible(currentTagId_);
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::CopyLinkToTag(int linkId, int tagId) {
    Link* link = FindLink(linkId);
    Group* tag = FindGroup(tagId);
    if (!link || !tag || tag->parentGroup == 0) {
        return;
    }

    Link copy = *link;
    copy.id = 0;
    copy.parentGroup = tagId;
    copy.pos = -1;
    copy.hotKey = 0;
    copy.runCount = 0;
    if (!storageService_.InsertLink(copy)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"复制启动项", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.links.push_back(copy);
    RegisterConfiguredHotKeys();
    selectedLinkId_ = copy.id;
    currentGroupId_ = tag->parentGroup;
    currentTagId_ = tag->id;
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    EnsureGroupVisible(currentGroupId_);
    EnsureTagVisible(currentTagId_);
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::RunLink(int linkId) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }

    std::wstring error;
    if (!launcher_.Run(*link, error)) {
        std::wstring message = error.empty() ? L"启动失败。" : error;
        message += L"\n\n是否现在修复该启动项目标？";
        if (MessageBoxW(hwnd_, message.c_str(), L"启动项", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES &&
            TryRepairLinkTarget(*link)) {
            if (!storageService_.UpdateLink(*link)) {
                MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"修复启动项", MB_OK | MB_ICONWARNING);
            } else {
                RegisterConfiguredHotKeys();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return;
    }

    if (config_.saveRunCount) {
        ++link->runCount;
        storageService_.IncrementRunCount(link->id, link->runCount);
    }
    if (config_.hideAfterLink) {
        HideMainWindow();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::RunLinkAsAdmin(int linkId) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }
    Link elevated = *link;
    elevated.isAdmin = true;
    std::wstring error;
    if (!launcher_.Run(elevated, error)) {
        MessageBoxW(hwnd_, error.empty() ? L"管理员运行失败。" : error.c_str(), L"以管理员身份运行", MB_OK | MB_ICONWARNING);
        return;
    }
    if (config_.saveRunCount) {
        ++link->runCount;
        storageService_.IncrementRunCount(link->id, link->runCount);
    }
    if (config_.hideAfterLink) {
        HideMainWindow();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::RunUrlPrivate(int linkId) {
    Link* link = FindLink(linkId);
    if (!link || !IsUrlLink(*link)) {
        return;
    }

    const std::wstring url = NormalizeUrl(link->path);
    const std::wstring arguments = L"-inprivate " + QuoteForCommandLine(url);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = hwnd_;
    info.lpVerb = L"open";
    info.lpFile = L"msedge.exe";
    info.lpParameters = arguments.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (info.hProcess) {
        CloseHandle(info.hProcess);
    }
    if (config_.saveRunCount) {
        ++link->runCount;
        storageService_.IncrementRunCount(link->id, link->runCount);
    }
    if (config_.hideAfterLink) {
        HideMainWindow();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OpenContainingFolder(int linkId) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }
    std::wstring error;
    if (!ShellItemService::OpenContainingLocation(hwnd_, *link, error)) {
        MessageBoxW(hwnd_, error.empty() ? L"无法打开所在位置。" : error.c_str(), L"打开所在目录", MB_OK | MB_ICONINFORMATION);
    }
}

void MainWindow::ShowWindowsContextMenu(int linkId, POINT screenPoint) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }
    ShellItemService::ShowNativeContextMenu(hwnd_, *link, screenPoint);
}

void MainWindow::CreateDesktopShortcut(int linkId) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }
    const std::filesystem::path desktop = DesktopDirectory();
    if (desktop.empty()) {
        MessageBoxW(hwnd_, L"无法定位桌面目录。", L"创建桌面快捷方式", MB_OK | MB_ICONWARNING);
        return;
    }
    const std::wstring extension = IsUrlLink(*link) ? L".url" : L".lnk";
    std::filesystem::path shortcutPath = desktop / (SafeFileName(link->name) + extension);
    for (int index = 2; FileExists(shortcutPath) && index < 1000; ++index) {
        shortcutPath = desktop / (SafeFileName(link->name) + L" (" + std::to_wstring(index) + L")" + extension);
    }

    const bool ok = IsUrlLink(*link)
        ? CreateUrlShortcutFile(*link, shortcutPath)
        : CreateShellShortcutFile(*link, shortcutPath);
    if (!ok) {
        MessageBoxW(hwnd_, L"创建桌面快捷方式失败。", L"创建桌面快捷方式", MB_OK | MB_ICONWARNING);
    }
}

void MainWindow::OpenSystemProperties(int linkId) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }
    ShellItemService::OpenProperties(hwnd_, *link);
}

void MainWindow::ClearCurrentTagLinks() {
    const int tagId = CommandTagId();
    Group* tag = FindGroup(tagId);
    if (!tag || tag->parentGroup == 0) {
        return;
    }
    std::vector<int> linkIds;
    for (const auto& link : model_.links) {
        if (link.parentGroup == tagId) {
            linkIds.push_back(link.id);
        }
    }
    if (linkIds.empty()) {
        return;
    }
    std::wstring message = L"确定清空“" + tag->name + L"”中的全部应用？";
    if (MessageBoxW(hwnd_, message.c_str(), L"清空本页应用", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    for (int id : linkIds) {
        if (!storageService_.DeleteLink(id)) {
            MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"清空本页应用", MB_OK | MB_ICONWARNING);
            return;
        }
    }
    model_.links.erase(std::remove_if(model_.links.begin(), model_.links.end(), [tagId](const Link& link) {
        return link.parentGroup == tagId;
    }), model_.links.end());
    if (selectedLinkId_ > 0 && !FindLink(selectedLinkId_)) {
        selectedLinkId_ = 0;
    }
    RegisterConfiguredHotKeys();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::CopyLinkPath(int linkId) {
    Link* link = FindLink(linkId);
    if (!link || !OpenClipboard(hwnd_)) {
        return;
    }
    EmptyClipboard();
    const std::wstring text = IsUrlLink(*link) ? NormalizeUrl(link->path) : link->path;
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        void* data = GlobalLock(memory);
        if (data) {
            memcpy(data, text.c_str(), bytes);
            GlobalUnlock(memory);
            SetClipboardData(CF_UNICODETEXT, memory);
            memory = nullptr;
        }
    }
    if (memory) {
        GlobalFree(memory);
    }
    CloseClipboard();
}

void MainWindow::OpenSearch() {
    int linkId = SearchDialog::Show(hwnd_, instance_, theme_, model_, config_);
    configService_.SaveWindowState(config_);
    if (linkId > 0) {
        selectedLinkId_ = linkId;
        RunLink(linkId);
    }
}

void MainWindow::OpenSettings() {
    AppConfig previous = config_;
    AppConfig next = config_;
    if (!ShowSettingsDialog(hwnd_, instance_, next, theme_)) {
        return;
    }
    config_ = next;
    configService_.Save(config_);
    SyncAutoRun(previous);
    ApplyConfigRuntimeChanges(previous);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ResetLayoutToDefaults() {
    AppConfig previous = config_;
    const AppConfig defaults;

    config_.showTitle = defaults.showTitle;
    config_.showGroup = defaults.showGroup;
    config_.showTag = defaults.showTag;
    config_.showSearchButton = defaults.showSearchButton;
    config_.showMenuButton = defaults.showMenuButton;
    config_.showSkinButton = defaults.showSkinButton;
    config_.topMost = defaults.topMost;
    config_.groupRight = defaults.groupRight;
    config_.tagRight = defaults.tagRight;
    config_.tagAlign = defaults.tagAlign;
    config_.width = defaults.width;
    config_.height = defaults.height;
    config_.groupWidth = defaults.groupWidth;
    config_.autoGroupWidth = defaults.autoGroupWidth;
    config_.tagWidth = defaults.tagWidth;
    config_.autoTagHeight = defaults.autoTagHeight;
    config_.attrWidth = defaults.attrWidth;
    config_.attrHeight = defaults.attrHeight;
    config_.alpha = defaults.alpha;

    const POINT position = ClampWindowPosition(defaults.posX, defaults.posY, defaults.width, defaults.height);
    config_.posX = position.x;
    config_.posY = position.y;

    dockHidden_ = false;
    dockRestoreRect_ = {};
    dockHideDueTick_ = GetTickCount64() + kDockRestoreGraceMs;
    groupScrollOffset_ = 0.0f;
    tagScrollOffset_ = 0.0f;
    linkScrollOffset_ = 0.0f;

    configService_.SaveWindowState(config_);
    ApplyConfigRuntimeChanges(previous);

    if (IsIconic(hwnd_)) {
        ShowWindowRespectFocusPolicy(hwnd_, SW_RESTORE);
    }
    ShowWindowRespectFocusPolicy(hwnd_, SW_SHOWNORMAL);
    SetWindowPos(
        hwnd_,
        config_.topMost ? HWND_TOPMOST : HWND_NOTOPMOST,
        config_.posX,
        config_.posY,
        config_.width,
        config_.height,
        SWP_SHOWWINDOW);
    ActivateWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ClearIconCache() {
    if (iconService_.ClearDiskCache()) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        MessageBoxW(hwnd_, L"图标缓存已清理。", L"图标缓存", MB_OK | MB_ICONINFORMATION);
    } else {
        WriteAppLog(L"图标缓存清理失败。");
        MessageBoxW(hwnd_, L"图标缓存清理失败，请确认 icons/cache 目录可写。", L"图标缓存", MB_OK | MB_ICONWARNING);
    }
}

void MainWindow::RefreshAllIcons() {
    for (const auto& link : model_.links) {
        iconService_.RefreshDiskCache(link);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::RefreshLinkIcon(int linkId) {
    Link* link = FindLink(linkId);
    if (!link) {
        return;
    }
    iconService_.RefreshDiskCache(*link);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ShowAbout() {
    MessageBoxW(
        hwnd_,
        L"Quattro\n\n轻量级 Windows 快速启动工具\nC++ / Win32 / Direct2D / DirectWrite",
        L"关于",
        MB_OK | MB_ICONINFORMATION);
}

void MainWindow::OpenHelp() {
    if (OpenConfiguredUrl(config_.helpUrl, L"帮助")) {
        return;
    }
    std::filesystem::path help = appDirectory_ / L"docs" / L"Quattro" / L"00-文档索引.md";
    if (!FileExists(help)) {
        help = appDirectory_ / L"README.md";
    }
    if (!FileExists(help)) {
        MessageBoxW(hwnd_, L"未找到帮助文档。", L"帮助", MB_OK | MB_ICONINFORMATION);
        return;
    }
    ShellExecuteW(hwnd_, L"open", help.c_str(), nullptr, help.parent_path().c_str(), SW_SHOWNORMAL);
}

void MainWindow::OpenFaq() {
    if (!OpenConfiguredUrl(config_.faqUrl, L"常见问题")) {
        MessageBoxW(hwnd_, L"常见问题链接尚未配置，可在设置窗口填写。", L"常见问题", MB_OK | MB_ICONINFORMATION);
    }
}

void MainWindow::OpenReward() {
    if (!OpenConfiguredUrl(config_.rewardUrl, L"赞助")) {
        MessageBoxW(hwnd_, L"赞助链接尚未配置，可在设置窗口填写。", L"赞助", MB_OK | MB_ICONINFORMATION);
    }
}

void MainWindow::ShowUpdateInfo() {
    if (OpenConfiguredUrl(config_.updateUrl, L"检查更新")) {
        return;
    }
    MessageBoxW(
        hwnd_,
        L"当前版本为本地构建版本。\n\n在线更新服务暂未接入，请以后续发布包或项目交付说明为准。",
        L"检查更新",
        MB_OK | MB_ICONINFORMATION);
}

bool MainWindow::OpenConfiguredUrl(const std::wstring& url, const wchar_t* title) {
    const std::wstring trimmed = Trim(url);
    if (trimmed.empty()) {
        return false;
    }
    HINSTANCE result = ShellExecuteW(hwnd_, L"open", trimmed.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        MessageBoxW(hwnd_, L"外部链接打开失败。", title, MB_OK | MB_ICONWARNING);
    }
    return true;
}

bool MainWindow::TryRepairLinkTarget(Link& link) {
    Link edited = link;
    if (!LinkEditDialog::Show(hwnd_, instance_, theme_, edited, model_.groups, false)) {
        return false;
    }
    link = edited;
    selectedLinkId_ = link.id;
    currentTagId_ = link.parentGroup;
    if (Group* tag = FindGroup(currentTagId_); tag && tag->parentGroup != 0) {
        currentGroupId_ = tag->parentGroup;
    }
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    return true;
}

void MainWindow::ShowThemeMenu(POINT screenPoint) {
    ResetMenuVisuals();
    HMENU menu = CreatePopupMenu();
    AppendThemeItemsToMenu(menu);
    ActivateWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void MainWindow::ApplyTheme(const std::wstring& themeName) {
    config_.theme = themeName;
    theme_ = Theme::Load(appDirectory_ / L"theme", config_.theme);
    configService_.Save(config_);
    ResetMenuVisuals();
    ApplyTooltipTheme();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::UpdateDockState() {
    if (!config_.autoDock || !IsWindowVisible(hwnd_) || IsIconic(hwnd_)) {
        dockHideDueTick_ = 0;
        return;
    }
    if (IsFullScreenForegroundWindow()) {
        dockHideDueTick_ = 0;
        return;
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    if (dockHidden_) {
        if (IsNearDockEdge(cursor)) {
            DockRestore();
        }
        dockHideDueTick_ = 0;
        return;
    }

    RECT window{};
    if (!GetWindowRect(hwnd_, &window)) {
        return;
    }
    if (PtInRect(&window, cursor)) {
        dockHideDueTick_ = 0;
        return;
    }

    HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return;
    }
    const RECT work = info.rcWork;
    const int threshold = 8;
    const bool nearEdge = std::abs(window.left - work.left) <= threshold ||
                          std::abs(window.top - work.top) <= threshold ||
                          std::abs(window.right - work.right) <= threshold ||
                          std::abs(window.bottom - work.bottom) <= threshold;
    if (!nearEdge) {
        dockHideDueTick_ = 0;
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (dockHideDueTick_ != 0 && now < dockHideDueTick_) {
        return;
    }

    if (config_.dockDelay <= 0) {
        DockHide();
        return;
    }

    if (dockHideDueTick_ == 0) {
        dockHideDueTick_ = now + static_cast<ULONGLONG>(config_.dockDelay);
    }
    if (now >= dockHideDueTick_) {
        DockHide();
        dockHideDueTick_ = 0;
    }
}

void MainWindow::DockHide() {
    HideLinkTooltip();
    if (dockHidden_) {
        return;
    }

    RECT window{};
    if (!GetWindowRect(hwnd_, &window)) {
        return;
    }
    dockRestoreRect_ = window;

    HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return;
    }
    const RECT work = info.rcWork;
    const int width = window.right - window.left;
    const int height = window.bottom - window.top;

    int x = window.left;
    int y = window.top;
    const int leftDistance = std::abs(window.left - work.left);
    const int rightDistance = std::abs(window.right - work.right);
    const int topDistance = std::abs(window.top - work.top);
    const int bottomDistance = std::abs(window.bottom - work.bottom);
    const int nearest = std::min(std::min(leftDistance, rightDistance), std::min(topDistance, bottomDistance));

    if (nearest == leftDistance) {
        x = work.left - width + kDockVisiblePixels;
    } else if (nearest == rightDistance) {
        x = work.right - kDockVisiblePixels;
    } else if (nearest == topDistance) {
        y = work.top - height + kDockVisiblePixels;
    } else {
        y = work.bottom - kDockVisiblePixels;
    }

    dockHidden_ = true;
    if (!SetWindowPos(hwnd_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE)) {
        dockHidden_ = false;
    }
}

void MainWindow::DockRestore() {
    if (!dockHidden_) {
        return;
    }
    const int width = dockRestoreRect_.right - dockRestoreRect_.left;
    const int height = dockRestoreRect_.bottom - dockRestoreRect_.top;
    SetWindowPos(hwnd_, nullptr, dockRestoreRect_.left, dockRestoreRect_.top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    dockHidden_ = false;
}

bool MainWindow::IsNearDockEdge(POINT screenPoint) const {
    RECT window{};
    if (!GetWindowRect(hwnd_, &window)) {
        return false;
    }
    InflateRect(&window, 8, 8);
    return PtInRect(&window, screenPoint) != FALSE;
}

void MainWindow::ImportPath(const std::wstring& path) {
    const std::wstring trimmed = Trim(path);
    if (trimmed.empty()) {
        return;
    }
    if (EnsureCurrentTag() <= 0) {
        return;
    }
    Link link;
    link.path = trimmed;
    link.name = std::filesystem::path(trimmed).stem().wstring();
    if (link.name.empty()) {
        link.name = trimmed;
    }
    std::error_code ec;
    const std::wstring lower = ToLower(trimmed);
    const bool isUrl = lower.rfind(L"http://", 0) == 0 ||
                       lower.rfind(L"https://", 0) == 0 ||
                       lower.rfind(L"ftp://", 0) == 0 ||
                       lower.rfind(L"www.", 0) == 0;
    if (ShellItemService::IsShellParseName(trimmed)) {
        link.type = 3;
    } else {
        link.type = std::filesystem::is_directory(trimmed, ec) ? 1 : (isUrl ? 2 : 0);
    }
    if (!isUrl) {
        if (auto item = ShellItemService::FromPathOrParseName(trimmed)) {
            link.pidl = item->pidl;
            if (item->isVirtual || ShellItemService::IsShellParseName(trimmed)) {
                link.type = 3;
                if (!Trim(item->displayName).empty()) {
                    link.name = item->displayName;
                }
            }
        }
    }
    link.parentGroup = currentTagId_;
    link.pos = -1;
    link.showCmd = SW_SHOWNORMAL;
    if (storageService_.InsertLink(link)) {
        model_.links.push_back(link);
        selectedLinkId_ = link.id;
        RegisterConfiguredHotKeys();
        EnsureGroupVisible(currentGroupId_);
        EnsureTagVisible(currentTagId_);
        EnsureLinkVisible(selectedLinkId_);
    } else {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"导入启动项", MB_OK | MB_ICONWARNING);
    }
}

void MainWindow::ImportClipboard() {
    if (!OpenClipboard(hwnd_)) {
        return;
    }
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle) {
        const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(handle));
        if (text) {
            ImportPath(text);
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool MainWindow::ImportDropData(IDataObject* dataObject) {
    if (!dataObject) {
        return false;
    }

    bool imported = false;
    FORMATETC fileFormat{};
    fileFormat.cfFormat = CF_HDROP;
    fileFormat.dwAspect = DVASPECT_CONTENT;
    fileFormat.lindex = -1;
    fileFormat.tymed = TYMED_HGLOBAL;
    STGMEDIUM medium{};
    if (SUCCEEDED(dataObject->GetData(&fileFormat, &medium))) {
        HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
        if (drop) {
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring path(length, L'\0');
                DragQueryFileW(drop, i, path.data(), length + 1);
                ImportPath(path);
                imported = true;
            }
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
        if (imported) {
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
    }

    const UINT shellIdListFormatId = RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
    if (shellIdListFormatId != 0) {
        FORMATETC shellFormat{};
        shellFormat.cfFormat = static_cast<CLIPFORMAT>(shellIdListFormatId);
        shellFormat.dwAspect = DVASPECT_CONTENT;
        shellFormat.lindex = -1;
        shellFormat.tymed = TYMED_HGLOBAL;
        STGMEDIUM shellMedium{};
        if (SUCCEEDED(dataObject->GetData(&shellFormat, &shellMedium))) {
            const SIZE_T bytes = shellMedium.hGlobal ? GlobalSize(shellMedium.hGlobal) : 0;
            const CIDA* cida = static_cast<const CIDA*>(GlobalLock(shellMedium.hGlobal));
            if (cida && bytes >= sizeof(UINT) * 2) {
                const UINT count = cida->cidl;
                const SIZE_T headerBytes = sizeof(UINT) * (static_cast<SIZE_T>(count) + 2);
                const auto* base = static_cast<const BYTE*>(static_cast<const void*>(cida));

                auto offsetValid = [&](UINT offset) {
                    return static_cast<SIZE_T>(offset) + sizeof(USHORT) <= bytes;
                };
                auto importAbsolutePidl = [&](PCIDLIST_ABSOLUTE pidl) {
                    if (!pidl) {
                        return;
                    }
                    auto item = ShellItemService::FromAbsolutePidl(pidl);
                    if (!item) {
                        return;
                    }
                    std::wstring target = !Trim(item->fileSystemPath).empty() ? item->fileSystemPath : item->parseName;
                    if (Trim(target).empty()) {
                        return;
                    }
                    ImportPath(target);
                    if (Link* importedLink = FindLink(selectedLinkId_)) {
                        importedLink->pidl = item->pidl;
                        if (item->isVirtual || ShellItemService::IsShellParseName(target)) {
                            importedLink->type = 3;
                        }
                        if (!Trim(item->displayName).empty()) {
                            importedLink->name = item->displayName;
                        }
                        storageService_.UpdateLink(*importedLink);
                    }
                    imported = true;
                };

                if (headerBytes <= bytes && offsetValid(cida->aoffset[0])) {
                    auto parent = reinterpret_cast<PCIDLIST_ABSOLUTE>(base + cida->aoffset[0]);
                    if (count == 0) {
                        importAbsolutePidl(parent);
                    } else {
                        for (UINT i = 0; i < count; ++i) {
                            if (!offsetValid(cida->aoffset[i + 1])) {
                                continue;
                            }
                            auto child = reinterpret_cast<PCUIDLIST_RELATIVE>(base + cida->aoffset[i + 1]);
                            PIDLIST_ABSOLUTE full = ILCombine(parent, child);
                            if (full) {
                                importAbsolutePidl(full);
                                CoTaskMemFree(full);
                            }
                        }
                    }
                }
            }
            if (cida) {
                GlobalUnlock(shellMedium.hGlobal);
            }
            ReleaseStgMedium(&shellMedium);
            if (imported) {
                InvalidateRect(hwnd_, nullptr, FALSE);
                return true;
            }
        }
    }

    FORMATETC textFormat{};
    textFormat.cfFormat = CF_UNICODETEXT;
    textFormat.dwAspect = DVASPECT_CONTENT;
    textFormat.lindex = -1;
    textFormat.tymed = TYMED_HGLOBAL;
    STGMEDIUM textMedium{};
    if (SUCCEEDED(dataObject->GetData(&textFormat, &textMedium))) {
        const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(textMedium.hGlobal));
        if (text && *text) {
            ImportPath(text);
            imported = true;
        }
        if (text) {
            GlobalUnlock(textMedium.hGlobal);
        }
        ReleaseStgMedium(&textMedium);
    }

    if (imported) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    return imported;
}

void MainWindow::ApplyConfigRuntimeChanges(const AppConfig& previous) {
    SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>(config_.alpha), LWA_ALPHA);
    SetWindowPos(hwnd_, config_.topMost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    if (previous.showTooltip != config_.showTooltip && !config_.showTooltip) {
        HideLinkTooltip();
    }
    if (previous.hideNotifyIcon != config_.hideNotifyIcon) {
        RemoveTrayIcon();
        InitializeTrayIcon();
    }
    if (previous.mainHotKey != config_.mainHotKey || previous.searchHotKey != config_.searchHotKey) {
        UnregisterConfiguredHotKeys();
        RegisterConfiguredHotKeys();
    }
    if ((config_.autoDock || config_.hideWhenInactive) && dockTimerId_ == 0) {
        dockTimerId_ = SetTimer(hwnd_, ID_TIMER_DOCK, 250, nullptr);
    } else if (!config_.autoDock && !config_.hideWhenInactive && dockTimerId_ != 0) {
        KillTimer(hwnd_, dockTimerId_);
        dockTimerId_ = 0;
    }
}

void MainWindow::SyncAutoRun(const AppConfig& previous) {
    if (previous.autoRun == config_.autoRun && (!config_.autoRun || StartupShortcutExists(appDirectory_))) {
        return;
    }

    std::wstring error;
    if (!SyncStartupShortcut(appDirectory_, config_.autoRun, error)) {
        WriteAppLog(L"开机自启动同步失败: " + error);
        MessageBoxW(hwnd_, error.empty() ? L"开机自启动同步失败。" : error.c_str(), L"开机自启动", MB_OK | MB_ICONWARNING);
        return;
    }
    WriteAppLog(config_.autoRun ? L"开机自启动已启用。" : L"开机自启动已关闭。");
}

void MainWindow::RegisterConfiguredHotKeys() {
    UnregisterConfiguredHotKeys();
    std::wstring failures;
    auto registerHotKey = [&](int id, int key, const std::wstring& name) {
        if (key == 0) {
            return;
        }
        if (!RegisterHotKey(hwnd_, id, MOD_CONTROL | MOD_ALT, static_cast<UINT>(key))) {
            const std::wstring line = name + L"（" + FormatHotKeyText(key) + L"）";
            failures += failures.empty() ? line : (L"\n" + line);
            WriteAppLog(L"热键注册失败: " + line + L" - " + FormatLastError(GetLastError()));
        }
    };

    if (config_.mainHotKey != 0) {
        registerHotKey(ID_HOTKEY_MAIN, config_.mainHotKey, L"主窗口");
    }
    if (config_.searchHotKey != 0) {
        registerHotKey(ID_HOTKEY_SEARCH, config_.searchHotKey, L"搜索");
    }

    int nextHotKeyId = ID_HOTKEY_LINK_BASE;
    for (const auto& link : model_.links) {
        if (link.hotKey == 0) {
            continue;
        }
        const int hotKeyId = nextHotKeyId++;
        if (RegisterHotKey(hwnd_, hotKeyId, MOD_CONTROL | MOD_ALT, static_cast<UINT>(link.hotKey))) {
            registeredLinkHotKeys_.push_back({hotKeyId, link.id});
        } else {
            const std::wstring line = link.name + L"（" + FormatHotKeyText(link.hotKey) + L"）";
            failures += failures.empty() ? line : (L"\n" + line);
            WriteAppLog(L"启动项热键注册失败: " + line + L" - " + FormatLastError(GetLastError()));
        }
    }

    if (!failures.empty()) {
        MessageBoxW(hwnd_, (L"以下热键注册失败，可能已被占用：\n" + failures).c_str(), L"热键", MB_OK | MB_ICONWARNING);
    }
    hotKeysRegistered_ = true;
}

void MainWindow::UnregisterConfiguredHotKeys() {
    if (!hwnd_) {
        return;
    }
    UnregisterHotKey(hwnd_, ID_HOTKEY_MAIN);
    UnregisterHotKey(hwnd_, ID_HOTKEY_SEARCH);
    for (const auto& [hotKeyId, _] : registeredLinkHotKeys_) {
        UnregisterHotKey(hwnd_, hotKeyId);
    }
    registeredLinkHotKeys_.clear();
    hotKeysRegistered_ = false;
}

void MainWindow::InitializeTrayIcon() {
    if (trayIconVisible_ || config_.hideNotifyIcon) {
        return;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = WM_QUATTRO_TRAY;
    data.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
    wcscpy_s(data.szTip, L"Quattro");
    if (Shell_NotifyIconW(NIM_ADD, &data)) {
        trayIconVisible_ = true;
    }
}

void MainWindow::RemoveTrayIcon() {
    if (!trayIconVisible_) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
    trayIconVisible_ = false;
}

void MainWindow::ShowTrayMenu(POINT screenPoint) {
    ResetMenuVisuals();
    menuContextKind_ = HitKind::None;
    menuContextId_ = 0;
    HMENU menu = CreatePopupMenu();
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_SHOW, IsWindowVisible(hwnd_) ? L"隐藏主窗口" : L"显示主窗口");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_RESET_LAYOUT, L"重置布局为默认布局");
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ABOUT, L"关于");
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_EXIT, L"退出");
    ActivateWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void MainWindow::ShowMainMenu(POINT screenPoint) {
    ResetMenuVisuals();
    menuContextKind_ = HitKind::None;
    menuContextId_ = 0;
    HMENU menu = CreatePopupMenu();
    HMENU themeMenu = CreatePopupMenu();
    HMENU systemMenu = CreatePopupMenu();
    AppendThemeItemsToMenu(themeMenu);
    AppendSystemFunctionItems(systemMenu);

    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_TOGGLE_TITLE, config_.showTitle ? L"隐藏标题栏" : L"显示标题栏", false, -1, -1, config_.showTitle ? MenuIconEyeOff : MenuIconEye);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_TOGGLE_GROUP, config_.showGroup ? L"隐藏分组" : L"显示分组", false, -1, -1, config_.showGroup ? MenuIconEyeOff : MenuIconEye);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_TOGGLE_TAG, config_.showTag ? L"隐藏标签" : L"显示标签", false, -1, -1, config_.showTag ? MenuIconEyeOff : MenuIconEye);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_TOGGLE_TOPMOST, config_.topMost ? L"取消钉住" : L"钉住", false, -1, -1, config_.topMost ? MenuIconPinOff : MenuIconPin);
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(themeMenu), L"皮肤", true);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_SETTINGS, L"设置");
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_REFRESH_ALL_ICONS, L"重置所有图标");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_CLEAR_ICON_CACHE, L"清理图标缓存");
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(systemMenu), L"系统功能", true);
    AppendThemedSeparator(menu);
    AppendUnifiedViewOptionItems(menu);
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_IMPORT_CLIPBOARD, L"从剪贴板导入");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_UPDATE, L"更新");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_EXIT, L"关闭退出");
    ActivateWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void MainWindow::ShowLinkMenu(int linkId, POINT screenPoint) {
    HideLinkTooltip();
    ResetMenuVisuals();
    HMENU menu = CreatePopupMenu();
    Link* link = FindLink(linkId);
    const bool isUrl = link && IsUrlLink(*link);
    AppendThemedMenuItem(menu, MF_STRING, isUrl ? ID_MENU_RUN_PRIVATE : ID_MENU_RUN_ADMIN, isUrl ? L"以隐私模式运行" : L"以管理员身份运行");
    AppendThemedMenuItem(menu, MF_STRING, isUrl ? ID_MENU_COPY_URL : ID_MENU_OPEN_LOCATION, isUrl ? L"复制网址(URL)" : L"打开文件位置");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_WINDOWS_CONTEXT, L"Windows 原生菜单");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_CREATE_DESKTOP_SHORTCUT, L"创建桌面快捷方式");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_REFRESH_LINK_ICON, L"刷新图标缓存");
    HMENU moveMenu = CreatePopupMenu();
    HMENU copyMenu = CreatePopupMenu();
    AppendGroupedTagTargetMenu(moveMenu, ID_MENU_MOVE_TO_BASE, menuMoveTargetIds_, link ? link->parentGroup : 0);
    AppendGroupedTagTargetMenu(copyMenu, ID_MENU_COPY_TO_BASE, menuCopyTargetIds_, 0);
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(moveMenu), L"移动到", true);
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(copyMenu), L"复制到", true);
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_EDIT_LINK, L"编辑");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_PROPERTIES, L"属性");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_DELETE_LINK, L"删除");
    selectedLinkId_ = linkId;
    menuContextKind_ = HitKind::Link;
    menuContextId_ = linkId;
    ActivateWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void MainWindow::CreateTooltip() {
    if (tooltip_ || !hwnd_) {
        return;
    }

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&controls);

    tooltip_ = CreateWindowExW(
        WS_EX_TOPMOST,
        TOOLTIPS_CLASSW,
        nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        hwnd_,
        nullptr,
        instance_,
        nullptr);
    if (!tooltip_) {
        return;
    }

    SetWindowPos(tooltip_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 520);
    SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_INITIAL, 320);
    SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_RESHOW, 80);
    SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_AUTOPOP, 12000);

    tooltipText_ = L" ";
    tooltipInfo_ = {};
    tooltipInfo_.cbSize = sizeof(tooltipInfo_);
    tooltipInfo_.uFlags = TTF_TRACK | TTF_ABSOLUTE | TTF_TRANSPARENT;
    tooltipInfo_.hwnd = hwnd_;
    tooltipInfo_.uId = 1;
    GetClientRect(hwnd_, &tooltipInfo_.rect);
    tooltipInfo_.lpszText = const_cast<LPWSTR>(tooltipText_.c_str());
    SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tooltipInfo_));
    SendMessageW(tooltip_, TTM_ACTIVATE, TRUE, 0);
    ApplyTooltipTheme();
}

void MainWindow::ApplyTooltipTheme() {
    if (!tooltip_) {
        return;
    }
    SendMessageW(tooltip_, TTM_SETTIPBKCOLOR, ToColorRef(theme_.color(L"tooltip", L"normal", L"bg")), 0);
    SendMessageW(tooltip_, TTM_SETTIPTEXTCOLOR, ToColorRef(theme_.color(L"tooltip", L"normal", L"text")), 0);

    const int paddingX = static_cast<int>(std::max(0.0f, Metric(theme_, L"tooltip", L"paddingX", 8.0f)));
    const int paddingY = static_cast<int>(std::max(0.0f, Metric(theme_, L"tooltip", L"paddingY", 5.0f)));
    RECT margin{paddingX, paddingY, paddingX, paddingY};
    SendMessageW(tooltip_, TTM_SETMARGIN, 0, reinterpret_cast<LPARAM>(&margin));
}

void MainWindow::HideLinkTooltip() {
    if (!tooltip_) {
        return;
    }
    SendMessageW(tooltip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&tooltipInfo_));
    tooltipLinkId_ = 0;
}

std::wstring MainWindow::LinkTooltipText(const Link& link) const {
    std::wstring target = Trim(link.path);
    if (!target.empty()) {
        target = IsUrlLink(link) ? NormalizeUrl(target) : ExpandEnvironmentStringsSafe(target);
    } else if (!Trim(link.workDir).empty()) {
        target = ExpandEnvironmentStringsSafe(Trim(link.workDir));
    } else {
        target = Trim(link.name);
    }

    const std::wstring remark = Trim(link.remark);
    if (target.empty()) {
        return remark;
    }
    if (remark.empty()) {
        return target;
    }
    return target + L"\r\n" + remark;
}

void MainWindow::UpdateLinkTooltip(const HitArea& hit, POINT screenPoint) {
    if (!config_.showTooltip || hit.kind != HitKind::Link) {
        HideLinkTooltip();
        return;
    }

    Link* link = FindLink(hit.id);
    if (!link) {
        HideLinkTooltip();
        return;
    }

    const std::wstring text = LinkTooltipText(*link);
    if (text.empty()) {
        HideLinkTooltip();
        return;
    }

    if (!tooltip_) {
        CreateTooltip();
    }
    if (!tooltip_) {
        return;
    }

    if (tooltipLinkId_ != hit.id || tooltipText_ != text) {
        tooltipText_ = text;
        tooltipInfo_.lpszText = const_cast<LPWSTR>(tooltipText_.c_str());
        SendMessageW(tooltip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tooltipInfo_));
        tooltipLinkId_ = hit.id;
    }

    SendMessageW(tooltip_, TTM_TRACKPOSITION, 0, MAKELPARAM(screenPoint.x + 14, screenPoint.y + 18));
    SendMessageW(tooltip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&tooltipInfo_));
}

void MainWindow::ShowGroupMenu(int groupId, POINT screenPoint) {
    ResetMenuVisuals();
    HMENU menu = CreatePopupMenu();
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_GROUP, L"新建分组");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_EDIT_GROUP, L"重命名");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_DELETE_GROUP, L"删除分组");
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_MOVE_UP, L"左移(Shift+←)");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_MOVE_DOWN, L"右移(Shift+→)");
    menuContextKind_ = HitKind::Group;
    menuContextId_ = groupId;
    ActivateWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void MainWindow::ShowGroupBlankMenu(POINT screenPoint) {
    ResetMenuVisuals();
    HMENU menu = CreatePopupMenu();
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_GROUP, L"新建分组");
    menuContextKind_ = HitKind::None;
    menuContextId_ = 0;
    ActivateWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void MainWindow::ShowTagMenu(int tagId, POINT screenPoint) {
    ResetMenuVisuals();
    HMENU menu = CreatePopupMenu();
    Group* tag = FindGroup(tagId);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_TAG, L"新建标签");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_EDIT_TAG, L"重命名标签");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_DELETE_TAG, L"删除标签");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_MOVE_UP, L"上移");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_MOVE_DOWN, L"下移");
    HMENU moveMenu = CreatePopupMenu();
    AppendGroupTargetMenu(moveMenu, ID_MENU_MOVE_TAG_TO_BASE, menuGroupTargetIds_, tag ? tag->parentGroup : 0);
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(moveMenu), L"移动到", true);
    AppendThemedSeparator(menu);
    AppendAddLinkItems(menu);
    AppendThemedSeparator(menu);
    AppendViewOptionItems(menu, tag);
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_REFRESH_PAGE_ICONS, L"刷新本页图标");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_CLEAR_TAG_LINKS, L"清空本页应用");
    menuContextKind_ = HitKind::Tag;
    menuContextId_ = tagId;
    ActivateWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void MainWindow::ShowTagBlankMenu(POINT screenPoint) {
    ResetMenuVisuals();
    HMENU menu = CreatePopupMenu();
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_TAG, L"新建标签");
    menuContextKind_ = HitKind::Group;
    menuContextId_ = currentGroupId_;
    ActivateWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void MainWindow::ShowBackgroundMenu(POINT screenPoint) {
    ResetMenuVisuals();
    menuContextKind_ = HitKind::None;
    menuContextId_ = 0;
    HMENU menu = CreatePopupMenu();
    AppendAddLinkItems(menu);
    AppendThemedSeparator(menu);
    AppendViewOptionItems(menu, FindGroup(currentTagId_));
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_REFRESH_PAGE_ICONS, L"刷新本页图标");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_CLEAR_TAG_LINKS, L"清空本页应用");
    ActivateWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void MainWindow::AppendThemeItemsToMenu(HMENU menu) {
    const std::filesystem::path themeDirectory = appDirectory_ / L"theme";
    const auto themeItems = DiscoverThemeMenuItems(themeDirectory, config_.theme);
    const std::wstring effectiveTheme = EffectiveThemeName(themeDirectory, config_.theme);
    const std::size_t count = std::min<std::size_t>(themeItems.size(), 100);
    for (std::size_t i = 0; i < count; ++i) {
        const UINT flags = MF_STRING | (effectiveTheme == themeItems[i].name ? MF_CHECKED : 0);
        AppendThemedMenuItem(menu, flags, ID_MENU_THEME_BASE + static_cast<UINT>(i), themeItems[i].label);
    }
}

void MainWindow::AppendAddLinkItems(HMENU menu) {
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_FILE, L"添加文件");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_FOLDER, L"添加文件夹");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_URL, L"添加网址");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_SYSTEM, L"添加系统功能");
}

void MainWindow::AppendViewOptionItems(HMENU menu, const Group* tag) {
    // These submenus reflect the selected tag page, not a global display preference.
    HMENU iconMenu = CreatePopupMenu();
    const int iconSize = EffectiveIconSize(tag);
    AppendThemedMenuItem(iconMenu, MF_STRING | (iconSize == 24 ? MF_CHECKED : 0), ID_MENU_ICON_SMALL, L"小图标");
    AppendThemedMenuItem(iconMenu, MF_STRING | (iconSize == 32 ? MF_CHECKED : 0), ID_MENU_ICON_MEDIUM, L"中图标");
    AppendThemedMenuItem(iconMenu, MF_STRING | (iconSize == 48 ? MF_CHECKED : 0), ID_MENU_ICON_LARGE, L"大图标");
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(iconMenu), L"图标大小", true);

    HMENU layoutMenu = CreatePopupMenu();
    const int layout = EffectiveLinkLayout(tag);
    AppendThemedMenuItem(layoutMenu, MF_STRING | (layout == 0 ? MF_CHECKED : 0), ID_MENU_LAYOUT_LIST, L"列表");
    AppendThemedMenuItem(layoutMenu, MF_STRING | (layout == 1 ? MF_CHECKED : 0), ID_MENU_LAYOUT_TILE, L"平铺");
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(layoutMenu), L"查看方式", true);

    HMENU sortMenu = CreatePopupMenu();
    const int sort = tag ? tag->sort : 0;
    AppendThemedMenuItem(sortMenu, MF_STRING | (sort == 0 ? MF_CHECKED : 0), ID_MENU_SORT_POS, L"按位置");
    AppendThemedMenuItem(sortMenu, MF_STRING | (sort == 1 ? MF_CHECKED : 0), ID_MENU_SORT_RUNCOUNT, L"按运行次数");
    AppendThemedMenuItem(sortMenu, MF_STRING | (sort == 2 ? MF_CHECKED : 0), ID_MENU_SORT_NAME, L"按名称");
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sortMenu), L"排序方式", true);
}

void MainWindow::AppendUnifiedViewOptionItems(HMENU menu) {
    bool hasTag = false;
    bool allSmall = true;
    bool allMedium = true;
    bool allLarge = true;
    bool allList = true;
    bool allTile = true;
    bool allSortPos = true;
    bool allSortRunCount = true;
    bool allSortName = true;

    for (const auto& tag : model_.groups) {
        if (tag.parentGroup == 0) {
            continue;
        }
        hasTag = true;
        const int iconSize = EffectiveIconSize(&tag);
        const int layout = EffectiveLinkLayout(&tag);
        allSmall = allSmall && iconSize == 24;
        allMedium = allMedium && iconSize == 32;
        allLarge = allLarge && iconSize == 48;
        allList = allList && layout == 0;
        allTile = allTile && layout == 1;
        allSortPos = allSortPos && tag.sort == 0;
        allSortRunCount = allSortRunCount && tag.sort == 1;
        allSortName = allSortName && tag.sort == 2;
    }

    const UINT disabled = hasTag ? 0 : MF_GRAYED;
    HMENU iconMenu = CreatePopupMenu();
    AppendThemedMenuItem(iconMenu, MF_STRING | disabled | (hasTag && allSmall ? MF_CHECKED : 0), ID_MENU_ALL_ICON_SMALL, L"小图标");
    AppendThemedMenuItem(iconMenu, MF_STRING | disabled | (hasTag && allMedium ? MF_CHECKED : 0), ID_MENU_ALL_ICON_MEDIUM, L"中图标");
    AppendThemedMenuItem(iconMenu, MF_STRING | disabled | (hasTag && allLarge ? MF_CHECKED : 0), ID_MENU_ALL_ICON_LARGE, L"大图标");
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(iconMenu), L"统一图标大小", true);

    HMENU layoutMenu = CreatePopupMenu();
    AppendThemedMenuItem(layoutMenu, MF_STRING | disabled | (hasTag && allList ? MF_CHECKED : 0), ID_MENU_ALL_LAYOUT_LIST, L"列表");
    AppendThemedMenuItem(layoutMenu, MF_STRING | disabled | (hasTag && allTile ? MF_CHECKED : 0), ID_MENU_ALL_LAYOUT_TILE, L"平铺");
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(layoutMenu), L"统一查看方式", true);

    HMENU sortMenu = CreatePopupMenu();
    AppendThemedMenuItem(sortMenu, MF_STRING | disabled | (hasTag && allSortPos ? MF_CHECKED : 0), ID_MENU_ALL_SORT_POS, L"按位置");
    AppendThemedMenuItem(sortMenu, MF_STRING | disabled | (hasTag && allSortRunCount ? MF_CHECKED : 0), ID_MENU_ALL_SORT_RUNCOUNT, L"按运行次数");
    AppendThemedMenuItem(sortMenu, MF_STRING | disabled | (hasTag && allSortName ? MF_CHECKED : 0), ID_MENU_ALL_SORT_NAME, L"按名称");
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sortMenu), L"统一排序方式", true);
}

void MainWindow::AppendSystemFunctionItems(HMENU menu) {
    const auto functions = SystemFunctions();
    const std::size_t count = std::min<std::size_t>(functions.size(), ID_MENU_SYSTEM_FUNCTION_LIMIT);
    const std::size_t columnBreak = count > 18 ? (count + 1) / 2 : count + 1;
    for (std::size_t i = 0; i < count; ++i) {
        UINT flags = MF_STRING;
        if (i == columnBreak) {
            flags |= MF_MENUBARBREAK;
        }
        AppendThemedMenuItem(
            menu,
            flags,
            ID_MENU_SYSTEM_FUNCTION_BASE + static_cast<UINT>(i),
            functions[i].name,
            false,
            functions[i].menuIcon != MenuIconNone ? -1 : SystemFunctionImageIndex(functions[i]),
            functions[i].stockIcon,
            functions[i].menuIcon);
    }
    if (count == 0) {
        AppendThemedMenuItem(menu, MF_STRING | MF_GRAYED, ID_MENU_SYSTEM_FUNCTION_BASE, L"无可用功能");
    }
}

std::vector<int> MainWindow::GroupTargetIds(int excludedGroupId) const {
    std::vector<int> targetIds;
    const auto groups = MajorGroups();
    for (const auto& group : groups) {
        if (group.id == excludedGroupId) {
            continue;
        }
        if (targetIds.size() >= ID_MENU_DYNAMIC_TARGET_LIMIT) {
            break;
        }
        targetIds.push_back(group.id);
    }
    return targetIds;
}

std::vector<int> MainWindow::GroupedTagTargetIds(int excludedTagId) const {
    std::vector<int> targetIds;
    const auto groups = MajorGroups();
    for (const auto& group : groups) {
        for (const auto& tag : model_.groups) {
            if (tag.parentGroup != group.id || tag.id == excludedTagId) {
                continue;
            }
            if (targetIds.size() >= ID_MENU_DYNAMIC_TARGET_LIMIT) {
                return targetIds;
            }
            targetIds.push_back(tag.id);
        }
    }
    return targetIds;
}

void MainWindow::AppendGroupTargetMenu(HMENU menu, UINT commandBase, std::vector<int>& targetIds, int excludedGroupId) {
    targetIds = GroupTargetIds(excludedGroupId);
    for (std::size_t i = 0; i < targetIds.size(); ++i) {
        const Group* group = FindGroup(targetIds[i]);
        if (!group) {
            continue;
        }
        AppendThemedMenuItem(menu, MF_STRING, commandBase + static_cast<UINT>(i), group->name);
    }
    if (targetIds.empty()) {
        AppendThemedMenuItem(menu, MF_STRING | MF_GRAYED, commandBase, L"无可选分组");
    }
}

void MainWindow::AppendTagTargetMenu(HMENU menu, UINT commandBase, std::vector<int>& targetIds, int excludedTagId) {
    targetIds.clear();
    for (const auto& tag : model_.groups) {
        if (tag.parentGroup == 0 || tag.id == excludedTagId) {
            continue;
        }
        if (targetIds.size() >= 500) {
            break;
        }
        const UINT command = commandBase + static_cast<UINT>(targetIds.size());
        const std::wstring text = TagDisplayName(tag);
        AppendThemedMenuItem(menu, MF_STRING, command, text);
        targetIds.push_back(tag.id);
    }
    if (targetIds.empty()) {
        AppendThemedMenuItem(menu, MF_STRING | MF_GRAYED, commandBase, L"无可选标签");
    }
}

void MainWindow::AppendGroupedTagTargetMenu(HMENU menu, UINT commandBase, std::vector<int>& targetIds, int excludedTagId) {
    targetIds.clear();
    const auto groups = MajorGroups();
    for (const auto& group : groups) {
        HMENU tagMenu = CreatePopupMenu();
        int count = 0;
        for (const auto& tag : model_.groups) {
            if (tag.parentGroup != group.id || tag.id == excludedTagId) {
                continue;
            }
            if (targetIds.size() >= ID_MENU_DYNAMIC_TARGET_LIMIT) {
                break;
            }
            const UINT command = commandBase + static_cast<UINT>(targetIds.size());
            AppendThemedMenuItem(tagMenu, MF_STRING, command, tag.name);
            targetIds.push_back(tag.id);
            ++count;
        }
        if (count == 0) {
            AppendThemedMenuItem(tagMenu, MF_STRING | MF_GRAYED, commandBase + static_cast<UINT>(targetIds.size()), L"无可选标签");
        }
        AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tagMenu), group.name, true);
    }
    if (groups.empty()) {
        AppendThemedMenuItem(menu, MF_STRING | MF_GRAYED, commandBase, L"无可选分组");
    }
}

void MainWindow::SaveWindowState() {
    if (!hwnd_ || IsIconic(hwnd_)) {
        return;
    }

    RECT rect{};
    if (!dockHidden_ && GetWindowRect(hwnd_, &rect)) {
        config_.posX = rect.left;
        config_.posY = rect.top;
    }
    RECT client{};
    if (GetClientRect(hwnd_, &client)) {
        config_.width = client.right - client.left;
        config_.height = client.bottom - client.top;
    }
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
}

void MainWindow::WakeUp() {
    if (dockHidden_) {
        DockRestore();
    }
    dockHideDueTick_ = GetTickCount64() + kDockRestoreGraceMs;
    if (!IsWindowVisible(hwnd_)) {
        ShowWindowRespectFocusPolicy(hwnd_, SW_SHOWNORMAL);
    }
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    ShowWindowRespectFocusPolicy(hwnd_, SW_RESTORE);
    ActivateWindow(hwnd_);
}

bool MainWindow::IsEffectivelyVisible() const {
    return IsWindowVisible(hwnd_) && !dockHidden_;
}

void MainWindow::HideMainWindow() {
    HideLinkTooltip();
    if (dockHidden_) {
        const int width = dockRestoreRect_.right - dockRestoreRect_.left;
        const int height = dockRestoreRect_.bottom - dockRestoreRect_.top;
        ShowWindow(hwnd_, SW_HIDE);
        SetWindowPos(hwnd_, nullptr, dockRestoreRect_.left, dockRestoreRect_.top, width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        dockHidden_ = false;
        SaveWindowState();
        return;
    }
    SaveWindowState();
    ShowWindow(hwnd_, SW_HIDE);
}

void MainWindow::DiscardDeviceResources() {
    iconService_.Clear();
    ClearUiBitmaps();
    SafeRelease(renderTarget_);
}

HRESULT MainWindow::CreateDeviceResources() {
    if (renderTarget_) {
        return S_OK;
    }

    RECT rect{};
    GetClientRect(hwnd_, &rect);
    const D2D1_SIZE_U size = D2D1::SizeU(rect.right - rect.left, rect.bottom - rect.top);
    return d2dFactory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd_, size),
        &renderTarget_);
}

void MainWindow::OnPaint() {
    PAINTSTRUCT paint{};
    BeginPaint(hwnd_, &paint);
    if (SUCCEEDED(CreateDeviceResources())) {
        Draw();
    }
    EndPaint(hwnd_, &paint);
}

void MainWindow::OnResize(UINT width, UINT height) {
    if (renderTarget_) {
        renderTarget_->Resize(D2D1::SizeU(width, height));
    }
    SaveWindowState();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::Draw() {
    renderTarget_->BeginDraw();
    const D2D1_SIZE_F size = renderTarget_->GetSize();

    renderTarget_->Clear(theme_.color(L"window", L"normal", L"bg").d2d());
    D2D1_RECT_F title{}, groups{}, tags{}, links{};
    BuildLayout(size.width, size.height, title, groups, tags, links);
    ClampScrollOffsets();
    hitAreas_.clear();

    DrawTitle(title);
    if (config_.showGroup) {
        DrawGroups(groups);
    }
    if (config_.showTag) {
        DrawTags(tags);
    }
    DrawLinks(links);
    DrawRect(D2D1::RectF(0, 0, size.width, size.height), theme_.color(L"window", L"normal", L"border"));

    HRESULT hr = renderTarget_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
    }
}

void MainWindow::DrawTitle(D2D1_RECT_F rect) {
    if (Height(rect) <= 0) {
        return;
    }

    FillRect(rect, theme_.color(L"title", L"normal", L"bg"));
    FillRect(D2D1::RectF(rect.left, rect.bottom - 1.0f, rect.right, rect.bottom), theme_.color(L"title", L"normal", L"line"));

    const float iconSize = Metric(theme_, L"title", L"iconSize", 20.0f);
    const float iconLeft = Metric(theme_, L"title", L"iconLeft", 9.0f);
    const float iconTop = Metric(theme_, L"title", L"iconTop", 7.0f);
    D2D1_RECT_F appIcon = D2D1::RectF(rect.left + iconLeft, rect.top + iconTop, rect.left + iconLeft + iconSize, rect.top + iconTop + iconSize);
    if (ID2D1Bitmap* bitmap = LoadAppIconBitmap()) {
        renderTarget_->DrawBitmap(bitmap, appIcon, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    const float titleTextEnd = rect.left + Metric(theme_, L"title", L"textEnd", 134.0f);
    D2D1_RECT_F nameRect = D2D1::RectF(appIcon.right + Metric(theme_, L"title", L"textGap", 7.0f), rect.top, titleTextEnd, rect.bottom);
    titleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    DrawTextBlock(L"Quattro", titleFormat_, nameRect, theme_.color(L"title", L"normal", L"text"));

    const float buttonSize = ClampFloat(theme_.metric(L"titleButton", L"size", 26.0f), 18.0f, 40.0f);
    const float buttonGap = ClampFloat(theme_.metric(L"titleButton", L"gap", 2.0f), 0.0f, 12.0f);
    const float buttonRightInset = Metric(theme_, L"titleButton", L"rightInset", 4.0f);
    const float buttonTopInset = Metric(theme_, L"titleButton", L"topInset", 4.0f);
    const float buttonReserveInset = Metric(theme_, L"titleButton", L"reserveInset", 16.0f);
    const std::array<HitKind, 4> buttons = {
        HitKind::CloseButton,
        HitKind::SkinButton,
        HitKind::MenuButton,
        HitKind::SearchButton,
    };
    auto isTitleButtonVisible = [&](HitKind kind) {
        return !((kind == HitKind::MenuButton && !config_.showMenuButton) ||
                 (kind == HitKind::SearchButton && !config_.showSearchButton) ||
                 (kind == HitKind::SkinButton && !config_.showSkinButton));
    };
    int visibleButtonCount = 0;
    for (HitKind kind : buttons) {
        if (isTitleButtonVisible(kind)) {
            ++visibleButtonCount;
        }
    }
    const float buttonReserve = visibleButtonCount > 0
        ? buttonReserveInset + static_cast<float>(visibleButtonCount) * buttonSize + static_cast<float>(visibleButtonCount - 1) * buttonGap
        : buttonReserveInset;

    if (config_.showDate) {
        D2D1_RECT_F dateRect = D2D1::RectF(titleTextEnd, rect.top, rect.right - buttonReserve, rect.bottom);
        dateRect.right = rect.right - buttonReserve;
        smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        DrawTextBlock(TodayText(), smallFormat_, dateRect, theme_.color(L"title", L"normal", L"subtext"));
    }

    float buttonRight = rect.right - buttonRightInset;
    for (HitKind kind : buttons) {
        if (!isTitleButtonVisible(kind)) {
            continue;
        }
        D2D1_RECT_F button = D2D1::RectF(buttonRight - buttonSize, rect.top + buttonTopInset, buttonRight, rect.top + buttonTopInset + buttonSize);
        if (IsHover(kind, 0)) {
            FillRect(button, kind == HitKind::CloseButton ? theme_.color(L"titleCloseButton", L"hover", L"bg") : theme_.color(L"titleButton", L"hover", L"bg"));
        }
        DrawButtonIcon(kind, button, kind == HitKind::CloseButton && IsHover(kind, 0) ? theme_.color(L"titleCloseButton", L"hover", L"icon") : theme_.color(L"titleButton", L"normal", L"icon"));
        hitAreas_.push_back(HitArea{kind, 0, button});
        buttonRight -= buttonSize + buttonGap;
    }
}

void MainWindow::DrawGroups(D2D1_RECT_F rect) {
    if (Height(rect) <= 0 || Width(rect) <= 0) {
        return;
    }

    FillRect(rect, theme_.color(L"majorNav", L"normal", L"bg"));
    const bool vertical = Height(rect) > Width(rect);
    if (vertical) {
        FillRect(D2D1::RectF(rect.left, rect.top, rect.left + 1.0f, rect.bottom), theme_.color(L"majorNav", L"normal", L"line"));
    } else {
        FillRect(D2D1::RectF(rect.left, rect.bottom - 1.0f, rect.right, rect.bottom), theme_.color(L"majorNav", L"normal", L"line"));
    }

    renderTarget_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float itemOffsetX = Metric(theme_, L"majorNavItem", L"offsetX", 4.0f);
    const float itemMinWidth = Metric(theme_, L"majorNavItem", L"minWidth", 72.0f);
    const float itemMaxWidth = Metric(theme_, L"majorNavItem", L"maxWidth", 128.0f);
    const float itemWidthAdjust = Metric(theme_, L"majorNavItem", L"widthAdjust", -14.0f);
    const float textInsetX = Metric(theme_, L"majorNavItem", L"textInsetX", 10.0f);
    if (vertical) {
        const float topInset = Metric(theme_, L"majorNavItem", L"verticalTopInset", 2.0f);
        const float itemHeight = Metric(theme_, L"majorNavItem", L"verticalHeight", 32.0f);
        const float itemGap = Metric(theme_, L"majorNavItem", L"verticalGap", 2.0f);
        float y = rect.top + topInset - groupScrollOffset_;
        for (const auto& group : MajorGroups()) {
            D2D1_RECT_F item = D2D1::RectF(rect.left, y, rect.right, y + itemHeight);
            if (item.bottom < rect.top + topInset) {
                y += itemHeight + itemGap;
                continue;
            }
            if (item.top > rect.bottom - 2.0f) {
                break;
            }
            const bool selected = group.id == currentGroupId_;
            const bool hovered = IsHover(HitKind::Group, group.id);
            if (selected) {
                FillRect(item, theme_.color(L"majorNavItem", L"selected", L"bg"));
            } else if (hovered) {
                FillRect(item, theme_.color(L"majorNavItem", L"hover", L"bg"));
            }
            textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DrawTextBlock(group.name, textFormat_, Inset(item, textInsetX, 0.0f), theme_.color(L"majorNavItem", selected ? L"selected" : L"normal", L"text"));
            hitAreas_.push_back(HitArea{HitKind::Group, group.id, IntersectRectF(item, rect)});
            y += itemHeight + itemGap;
        }
        renderTarget_->PopAxisAlignedClip();
        DrawScrollBar(rect, groupScrollOffset_, MaxGroupScrollOffset(rect), false);
        return;
    }

    float x = rect.left + itemOffsetX - groupScrollOffset_;
    const float y = rect.top;
    const float itemHeight = Height(rect) - 1.0f;
    for (const auto& group : MajorGroups()) {
        const float itemWidth = std::max(itemMinWidth, std::min(itemMaxWidth, NavigationItemWidth(theme_, group.name) + itemWidthAdjust));
        D2D1_RECT_F item = D2D1::RectF(x, y, x + itemWidth, y + itemHeight);
        if (item.right < rect.left + 2.0f) {
            x += itemWidth;
            continue;
        }
        if (item.left > rect.right - 2.0f) {
            break;
        }
        const bool selected = group.id == currentGroupId_;
        const bool hovered = IsHover(HitKind::Group, group.id);
        if (group.id == currentGroupId_) {
            FillRect(item, theme_.color(L"majorNavItem", L"selected", L"bg"));
        } else if (hovered) {
            FillRect(item, theme_.color(L"majorNavItem", L"hover", L"bg"));
        }
        textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DrawTextBlock(group.name, textFormat_, Inset(item, textInsetX, 0.0f), theme_.color(L"majorNavItem", selected ? L"selected" : L"normal", L"text"));
        hitAreas_.push_back(HitArea{HitKind::Group, group.id, IntersectRectF(item, rect)});
        x += itemWidth;
    }
    renderTarget_->PopAxisAlignedClip();
    DrawScrollBar(rect, groupScrollOffset_, MaxGroupScrollOffset(rect), true);
}

void MainWindow::DrawTags(D2D1_RECT_F rect) {
    if (Height(rect) <= 0 || Width(rect) <= 0) {
        return;
    }

    FillRect(rect, theme_.color(L"minorNav", L"normal", L"bg"));
    if (config_.tagRight) {
        FillRect(D2D1::RectF(rect.left, rect.top, rect.left + 1.0f, rect.bottom), theme_.color(L"minorNav", L"normal", L"line"));
    } else {
        FillRect(D2D1::RectF(rect.right - 1.0f, rect.top, rect.right, rect.bottom), theme_.color(L"minorNav", L"normal", L"line"));
    }

    if (config_.tagAlign == L"right") {
        textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    } else if (config_.tagAlign == L"center") {
        textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    renderTarget_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float topInset = Metric(theme_, L"minorNavItem", L"topInset", 2.0f);
    const float itemHeight = Metric(theme_, L"minorNavItem", L"height", 32.0f);
    const float itemGap = Metric(theme_, L"minorNavItem", L"gap", 2.0f);
    const float textInsetX = Metric(theme_, L"minorNavItem", L"textInsetX", 10.0f);
    const float accentWidth = Metric(theme_, L"minorNavItem", L"accentWidth", 3.0f);
    const float accentInsetY = Metric(theme_, L"minorNavItem", L"accentInsetY", 3.0f);
    float y = rect.top + topInset - tagScrollOffset_;
    for (const auto& tag : TagsForCurrentGroup()) {
        D2D1_RECT_F item = D2D1::RectF(rect.left, y, rect.right, y + itemHeight);
        if (item.bottom < rect.top + topInset) {
            y += itemHeight + itemGap;
            continue;
        }
        if (item.top > rect.bottom - 2.0f) {
            break;
        }
        const bool selected = tag.id == currentTagId_;
        const bool hovered = IsHover(HitKind::Tag, tag.id);
        if (selected) {
            FillRect(item, theme_.color(L"minorNavItem", L"selected", L"bg"));
            if (config_.tagRight) {
                FillRect(D2D1::RectF(item.right - accentWidth, item.top + accentInsetY, item.right, item.bottom - accentInsetY), theme_.color(L"minorNavItem", L"selected", L"accent"));
            } else {
                FillRect(D2D1::RectF(item.left, item.top + accentInsetY, item.left + accentWidth, item.bottom - accentInsetY), theme_.color(L"minorNavItem", L"selected", L"accent"));
            }
        } else if (hovered) {
            FillRect(item, theme_.color(L"minorNavItem", L"hover", L"bg"));
        }
        DrawTextBlock(tag.name, textFormat_, Inset(item, textInsetX, 0.0f), theme_.color(L"minorNavItem", selected ? L"selected" : L"normal", L"text"));
        hitAreas_.push_back(HitArea{HitKind::Tag, tag.id, IntersectRectF(item, rect)});
        y += itemHeight + itemGap;
    }
    renderTarget_->PopAxisAlignedClip();
    DrawScrollBar(rect, tagScrollOffset_, MaxTagScrollOffset(rect), false);
}

void MainWindow::DrawLinks(D2D1_RECT_F rect) {
    FillRect(rect, theme_.color(L"content", L"normal", L"bg"));
    auto links = LinksForCurrentTag();
    if (links.empty()) {
        textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        const float emptyInsetX = Metric(theme_, L"content", L"emptyTextInsetX", 20.0f);
        const float emptyTop = Metric(theme_, L"content", L"emptyTextTop", 24.0f);
        const float emptyHeight = Metric(theme_, L"content", L"emptyTextHeight", 30.0f);
        D2D1_RECT_F emptyText = D2D1::RectF(rect.left + emptyInsetX, rect.top + emptyTop, rect.right - emptyInsetX, rect.top + emptyTop + emptyHeight);
        DrawTextBlock(L"当前标签没有启动项", textFormat_, emptyText, theme_.color(L"content", L"empty", L"text"));
        return;
    }

    const Group* tag = FindGroup(currentTagId_);
    const LinkLayoutMetrics metrics = MakeLinkLayout(theme_, rect, tag);

    renderTarget_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (std::size_t index = 0; index < links.size(); ++index) {
        Link* link = links[index];
        D2D1_RECT_F item = LinkItemRect(rect, metrics, static_cast<int>(index), linkScrollOffset_);
        if (item.bottom < rect.top + 2.0f) {
            continue;
        }
        if (item.top > rect.bottom - 2.0f) {
            break;
        }

        FillRect(item, theme_.color(L"linkItem", L"normal", L"bg"));
        if (link->id == selectedLinkId_) {
            FillRect(item, theme_.color(L"linkItem", L"selected", L"bg"));
            FillRect(D2D1::RectF(item.left, item.top, item.left + Metric(theme_, L"linkItem", L"selectedAccentWidth", 4.0f), item.bottom), theme_.color(L"linkItem", L"selected", L"accent"));
        } else if (IsHover(HitKind::Link, link->id)) {
            FillRect(item, theme_.color(L"linkItem", L"hover", L"bg"));
        }
        if (link->isCustomColor) {
            if (auto color = ParseCustomColor(link->customColor)) {
                FillRect(D2D1::RectF(
                             item.left,
                             item.top + Metric(theme_, L"linkItem", L"customColorInsetY", 4.0f),
                             item.left + Metric(theme_, L"linkItem", L"customColorWidth", 3.0f),
                             item.bottom - Metric(theme_, L"linkItem", L"customColorInsetY", 4.0f)),
                         *color);
            }
        }

        D2D1_RECT_F icon{};
        D2D1_RECT_F nameRect{};
        IDWriteTextFormat* nameFormat = textFormat_;
        if (metrics.layout == 0 || metrics.compactTile) {
            const float iconTop = item.top + (Height(item) - metrics.iconSize) * 0.5f;
            const float iconLeft = item.left + Metric(theme_, L"linkItem", L"listIconLeft", 8.0f);
            icon = D2D1::RectF(iconLeft, iconTop, iconLeft + metrics.iconSize, iconTop + metrics.iconSize);
            nameRect = D2D1::RectF(icon.right + Metric(theme_, L"linkItem", L"listTextGap", 8.0f), item.top, item.right - Metric(theme_, L"linkItem", L"listTextRightInset", 6.0f), item.bottom);
            nameFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            nameFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            if (config_.showRunCount) {
                const float runWidth = Metric(theme_, L"linkItem", L"runCountWidth", 36.0f);
                nameRect.right = std::max(nameRect.left, item.right - runWidth - Metric(theme_, L"linkItem", L"runCountRightInset", 8.0f));
            }
        } else {
            const float iconLeft = item.left + (Width(item) - metrics.iconSize) * 0.5f;
            const float iconTop = item.top + Metric(theme_, L"linkItem", L"gridIconTop", 8.0f);
            icon = D2D1::RectF(iconLeft, iconTop, iconLeft + metrics.iconSize, iconTop + metrics.iconSize);
            nameRect = D2D1::RectF(
                item.left + Metric(theme_, L"linkItem", L"gridTextPaddingX", 4.0f),
                icon.bottom + Metric(theme_, L"linkItem", L"gridTextGap", 4.0f),
                item.right - Metric(theme_, L"linkItem", L"gridTextPaddingX", 4.0f),
                item.bottom - Metric(theme_, L"linkItem", L"gridTextPaddingBottom", 4.0f));
            nameFormat = metrics.itemWidth <= Metric(theme_, L"linkItem", L"smallTextWidthThreshold", 78.0f) ? smallFormat_ : textFormat_;
            nameFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            nameFormat->SetWordWrapping(config_.linkNameSingleLine ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_WRAP);
        }

        if (ID2D1Bitmap* bitmap = iconService_.GetBitmap(renderTarget_, *link)) {
            renderTarget_->DrawBitmap(bitmap, icon, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            const float iconRadius = Metric(theme_, L"iconFallback", L"radius", 7.0f);
            FillRoundedRect(icon, theme_.color(L"iconFallback", L"normal", L"bg"), iconRadius);
            DrawRoundedRect(icon, theme_.color(L"iconFallback", L"normal", L"border"), iconRadius);
        }
        DrawTextBlock(link->name, nameFormat, nameRect, theme_.color(L"linkItem", L"normal", L"text"));
        if (config_.showRunCount) {
            smallFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            const std::wstring runText = std::to_wstring(link->runCount);
            if (metrics.layout == 0 || metrics.compactTile) {
                const float rightInset = Metric(theme_, L"linkItem", L"runCountRightInset", 8.0f);
                const float width = Metric(theme_, L"linkItem", L"runCountWidth", 36.0f);
                D2D1_RECT_F runRect = D2D1::RectF(item.right - rightInset - width, item.top, item.right - rightInset, item.bottom);
                smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                DrawTextBlock(runText, smallFormat_, runRect, theme_.color(L"linkItem", L"normal", L"subtext"));
            } else {
                const float width = Metric(theme_, L"linkItem", L"runCountGridWidth", 28.0f);
                const float height = Metric(theme_, L"linkItem", L"runCountGridHeight", 16.0f);
                const float rightInset = Metric(theme_, L"linkItem", L"runCountGridRightInset", 4.0f);
                const float topInset = Metric(theme_, L"linkItem", L"runCountGridTopInset", 3.0f);
                D2D1_RECT_F runRect = D2D1::RectF(item.right - rightInset - width, item.top + topInset, item.right - rightInset, item.top + topInset + height);
                smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                DrawTextBlock(runText, smallFormat_, runRect, theme_.color(L"linkItem", L"normal", L"subtext"));
            }
        }
        hitAreas_.push_back(HitArea{HitKind::Link, link->id, IntersectRectF(item, rect)});
    }
    renderTarget_->PopAxisAlignedClip();
    DrawScrollBar(rect, linkScrollOffset_, MaxLinkScrollOffset(rect), false);
}

void MainWindow::DrawButtonIcon(HitKind kind, D2D1_RECT_F rect, const Color& color) {
    ID2D1SolidColorBrush* brush = nullptr;
    renderTarget_->CreateSolidColorBrush(color.d2d(), &brush);
    if (!brush) {
        return;
    }

    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float stroke = Metric(theme_, L"titleButton", L"iconStrokeWidth", 1.6f);
    const float iconHalf = Metric(theme_, L"titleButton", L"iconHalf", 5.0f);
    const float menuHalfWidth = Metric(theme_, L"titleButton", L"menuHalfWidth", 6.0f);
    const float menuLineGap = Metric(theme_, L"titleButton", L"menuLineGap", 5.0f);
    const float searchRadius = Metric(theme_, L"titleButton", L"searchRadius", 5.0f);
    const float searchOffset = Metric(theme_, L"titleButton", L"searchOffset", 2.0f);
    const float searchHandle = Metric(theme_, L"titleButton", L"searchHandle", 7.0f);
    if (kind == HitKind::CloseButton) {
        renderTarget_->DrawLine(D2D1::Point2F(cx - iconHalf, cy - iconHalf), D2D1::Point2F(cx + iconHalf, cy + iconHalf), brush, stroke);
        renderTarget_->DrawLine(D2D1::Point2F(cx + iconHalf, cy - iconHalf), D2D1::Point2F(cx - iconHalf, cy + iconHalf), brush, stroke);
    } else if (kind == HitKind::MenuButton) {
        renderTarget_->DrawLine(D2D1::Point2F(cx - menuHalfWidth, cy - menuLineGap), D2D1::Point2F(cx + menuHalfWidth, cy - menuLineGap), brush, stroke);
        renderTarget_->DrawLine(D2D1::Point2F(cx - menuHalfWidth, cy), D2D1::Point2F(cx + menuHalfWidth, cy), brush, stroke);
        renderTarget_->DrawLine(D2D1::Point2F(cx - menuHalfWidth, cy + menuLineGap), D2D1::Point2F(cx + menuHalfWidth, cy + menuLineGap), brush, stroke);
    } else if (kind == HitKind::SearchButton) {
        renderTarget_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx - searchOffset, cy - searchOffset), searchRadius, searchRadius), brush, stroke);
        renderTarget_->DrawLine(D2D1::Point2F(cx + searchOffset, cy + searchOffset), D2D1::Point2F(cx + searchHandle, cy + searchHandle), brush, stroke);
    } else if (kind == HitKind::AddButton) {
        renderTarget_->DrawLine(D2D1::Point2F(cx - menuHalfWidth, cy), D2D1::Point2F(cx + menuHalfWidth, cy), brush, stroke);
        renderTarget_->DrawLine(D2D1::Point2F(cx, cy - menuHalfWidth), D2D1::Point2F(cx, cy + menuHalfWidth), brush, stroke);
    } else if (kind == HitKind::SkinButton) {
        renderTarget_->DrawRectangle(D2D1::RectF(cx - menuHalfWidth, cy - menuHalfWidth, cx + menuHalfWidth, cy + menuHalfWidth), brush, Metric(theme_, L"titleButton", L"skinStrokeWidth", 1.4f));
        renderTarget_->DrawLine(D2D1::Point2F(cx - menuHalfWidth, cy), D2D1::Point2F(cx + menuHalfWidth, cy), brush, Metric(theme_, L"titleButton", L"skinLineStrokeWidth", 1.2f));
    }
    brush->Release();
}

ID2D1Bitmap* MainWindow::LoadAppIconBitmap() {
    if (!uiWicFactory_ || !renderTarget_) {
        return nullptr;
    }

    const std::wstring key = L"#app-icon";
    auto found = uiBitmapCache_.find(key);
    if (found != uiBitmapCache_.end()) {
        return found->second;
    }

    HICON icon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    if (!icon) {
        return nullptr;
    }

    IWICBitmap* wicBitmap = nullptr;
    IWICFormatConverter* converter = nullptr;
    ID2D1Bitmap* bitmap = nullptr;
    if (SUCCEEDED(uiWicFactory_->CreateBitmapFromHICON(icon, &wicBitmap)) &&
        SUCCEEDED(uiWicFactory_->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(
            wicBitmap,
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeMedianCut)) &&
        SUCCEEDED(renderTarget_->CreateBitmapFromWicBitmap(converter, nullptr, &bitmap)) &&
        bitmap) {
        uiBitmapCache_[key] = bitmap;
    }

    SafeRelease(converter);
    SafeRelease(wicBitmap);
    DestroyIcon(icon);
    return bitmap;
}

void MainWindow::ClearUiBitmaps() {
    for (auto& [_, bitmap] : uiBitmapCache_) {
        SafeRelease(bitmap);
    }
    uiBitmapCache_.clear();
}

void MainWindow::FillRect(const D2D1_RECT_F& rect, const Color& color) {
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(color.d2d(), &brush)) && brush) {
        renderTarget_->FillRectangle(rect, brush);
        brush->Release();
    }
}

void MainWindow::DrawRect(const D2D1_RECT_F& rect, const Color& color, float strokeWidth) {
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(color.d2d(), &brush)) && brush) {
        renderTarget_->DrawRectangle(rect, brush, strokeWidth);
        brush->Release();
    }
}

void MainWindow::FillRoundedRect(const D2D1_RECT_F& rect, const Color& color, float radius) {
    if (Width(rect) <= 0 || Height(rect) <= 0) {
        return;
    }
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(color.d2d(), &brush)) && brush) {
        const float r = std::min(radius, std::min(Width(rect), Height(rect)) * 0.5f);
        renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(rect, r, r), brush);
        brush->Release();
    }
}

void MainWindow::DrawRoundedRect(const D2D1_RECT_F& rect, const Color& color, float radius, float strokeWidth) {
    if (Width(rect) <= 0 || Height(rect) <= 0) {
        return;
    }
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(color.d2d(), &brush)) && brush) {
        const float r = std::min(radius, std::min(Width(rect), Height(rect)) * 0.5f);
        renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(rect, r, r), brush, strokeWidth);
        brush->Release();
    }
}

void MainWindow::DrawScrollBar(const D2D1_RECT_F& rect, float offset, float maxOffset, bool horizontal) {
    if (maxOffset <= 0.5f || Width(rect) <= 0 || Height(rect) <= 0) {
        return;
    }

    const float thickness = ClampFloat(theme_.metric(L"scrollbar", L"thickness", 5.0f), 2.0f, 16.0f);
    const float inset = ClampFloat(theme_.metric(L"scrollbar", L"inset", 5.0f), 0.0f, 24.0f);
    const float viewport = horizontal ? Width(rect) : Height(rect);
    const float content = viewport + maxOffset;
    const float trackLength = std::max(20.0f, viewport - inset * 2.0f);
    const float thumbLength = std::max(Metric(theme_, L"scrollbar", L"minThumbLength", 24.0f), trackLength * viewport / std::max(viewport, content));
    const float travel = std::max(1.0f, trackLength - thumbLength);
    const float thumbStart = inset + travel * ClampFloat(offset / maxOffset, 0.0f, 1.0f);

    D2D1_RECT_F track{};
    D2D1_RECT_F thumb{};
    if (horizontal) {
        const float edgeInset = Metric(theme_, L"scrollbar", L"edgeInset", 4.0f);
        track = D2D1::RectF(rect.left + inset, rect.bottom - thickness - edgeInset, rect.right - inset, rect.bottom - edgeInset);
        thumb = D2D1::RectF(rect.left + thumbStart, track.top, rect.left + thumbStart + thumbLength, track.bottom);
    } else {
        const float edgeInset = Metric(theme_, L"scrollbar", L"edgeInset", 4.0f);
        track = D2D1::RectF(rect.right - thickness - edgeInset, rect.top + inset, rect.right - edgeInset, rect.bottom - inset);
        thumb = D2D1::RectF(track.left, rect.top + thumbStart, track.right, rect.top + thumbStart + thumbLength);
    }

    FillRoundedRect(track, theme_.color(L"scrollbar", L"normal", L"track"), thickness * 0.5f);
    FillRoundedRect(thumb, theme_.color(L"scrollbar", L"normal", L"thumb"), thickness * 0.5f);
}

void MainWindow::DrawTextBlock(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, const Color& color) {
    if (text.empty() || !format) {
        return;
    }
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(color.d2d(), &brush)) && brush) {
        renderTarget_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        brush->Release();
    }
}

void MainWindow::ResetMenuVisuals() {
    activeMenuItems_.clear();
}

void MainWindow::AppendThemedMenuItem(HMENU menu, UINT flags, UINT_PTR id, const std::wstring& text, bool submenu, int systemImageIndex, int stockIcon, int menuIcon) {
    auto item = std::make_unique<MenuItemData>();
    item->text = MenuTextFromRaw(text);
    item->icon = menuIcon != MenuIconNone ? menuIcon : MenuIconFor(id, item->text);
    item->systemImageIndex = systemImageIndex;
    item->stockIcon = stockIcon;
    item->checked = (flags & MF_CHECKED) != 0;
    item->disabled = (flags & (MF_DISABLED | MF_GRAYED)) != 0;
    item->submenu = submenu || ((flags & MF_POPUP) != 0);
    MenuItemData* raw = item.get();
    activeMenuItems_.push_back(std::move(item));
    AppendMenuW(menu, (flags | MF_OWNERDRAW) & ~MF_STRING, id, reinterpret_cast<LPCWSTR>(raw));
}

void MainWindow::AppendThemedSeparator(HMENU menu) {
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
}

bool MainWindow::MeasureThemedMenuItem(MEASUREITEMSTRUCT* measure) {
    if (!measure || measure->CtlType != ODT_MENU || measure->itemData == 0) {
        return false;
    }
    const auto* item = reinterpret_cast<const MenuItemData*>(measure->itemData);
    const int textLength = static_cast<int>(item->text.size());
    measure->itemHeight = static_cast<UINT>(Metric(theme_, L"menuItem", L"height", 30.0f));
    const int minTextWidth = static_cast<int>(Metric(theme_, L"menuItem", L"minTextWidth", 64.0f));
    const int maxTextWidth = static_cast<int>(Metric(theme_, L"menuItem", L"maxTextWidth", 360.0f));
    const int widthBase = static_cast<int>(Metric(theme_, L"menuItem", L"widthBase", 54.0f));
    const int charWidth = static_cast<int>(Metric(theme_, L"menuItem", L"charWidth", 13.0f));
    measure->itemWidth = static_cast<UINT>(widthBase + std::min(maxTextWidth, std::max(minTextWidth, textLength * charWidth)));
    return true;
}

bool MainWindow::DrawThemedMenuItem(const DRAWITEMSTRUCT* draw) {
    if (!draw || draw->CtlType != ODT_MENU || draw->itemData == 0) {
        return false;
    }

    const auto* item = reinterpret_cast<const MenuItemData*>(draw->itemData);
    RECT rc = draw->rcItem;
    HDC dc = draw->hDC;
    const bool selected = (draw->itemState & ODS_SELECTED) != 0;

    const Color background = theme_.color(L"menu", L"normal", L"bg");
    const Color hover = selected ? theme_.color(L"menuItem", L"hover", L"bg") : background;
    HBRUSH backgroundBrush = CreateSolidBrush(ToColorRef(hover));
    ::FillRect(dc, &rc, backgroundBrush);
    DeleteObject(backgroundBrush);

    if (selected) {
        RECT hotRect = rc;
        InflateRect(&hotRect, -static_cast<int>(Metric(theme_, L"menuItem", L"hoverInsetX", 4.0f)), -static_cast<int>(Metric(theme_, L"menuItem", L"hoverInsetY", 3.0f)));
        HBRUSH hotBrush = CreateSolidBrush(ToColorRef(theme_.color(L"menuItem", L"hover", L"bg")));
        ::FillRect(dc, &hotRect, hotBrush);
        DeleteObject(hotBrush);
    }

    const int iconLeft = static_cast<int>(Metric(theme_, L"menuItem", L"iconLeft", 8.0f));
    const int iconTopInset = static_cast<int>(Metric(theme_, L"menuItem", L"iconInsetY", 6.0f));
    const int iconSize = static_cast<int>(Metric(theme_, L"menuItem", L"iconSize", 16.0f));
    const RECT iconRect{rc.left + iconLeft, rc.top + iconTopInset, rc.left + iconLeft + iconSize, rc.bottom - iconTopInset};
    if (item->stockIcon >= 0) {
        if (!DrawStockMenuIcon(dc, iconRect, static_cast<SHSTOCKICONID>(item->stockIcon)) &&
            !DrawSystemImageListIcon(dc, iconRect, item->systemImageIndex, item->disabled)) {
            DrawMenuIcon(dc, iconRect, item->icon, item->disabled, appDirectory_);
        }
    } else if (item->systemImageIndex >= 0) {
        if (!DrawSystemImageListIcon(dc, iconRect, item->systemImageIndex, item->disabled)) {
            DrawMenuIcon(dc, iconRect, item->icon, item->disabled, appDirectory_);
        }
    } else {
        DrawMenuIcon(dc, iconRect, item->icon, item->disabled, appDirectory_);
    }

    if (item->checked) {
        HBRUSH dotBrush = CreateSolidBrush(ToColorRef(theme_.color(L"menuItem", L"checked", L"mark")));
        HGDIOBJ oldBrush = SelectObject(dc, dotBrush);
        HPEN dotPen = CreatePen(PS_SOLID, 1, ToColorRef(theme_.color(L"menuItem", L"checked", L"mark")));
        HGDIOBJ oldPen = SelectObject(dc, dotPen);
        const int dotCenterY = (rc.top + rc.bottom) / 2;
        Ellipse(dc, rc.left + 3, dotCenterY - 2, rc.left + 7, dotCenterY + 2);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(dotPen);
        DeleteObject(dotBrush);
    }

    RECT textRect = rc;
    textRect.left += static_cast<int>(Metric(theme_, L"menuItem", L"textLeft", 34.0f));
    textRect.right -= item->submenu ? static_cast<int>(Metric(theme_, L"menuItem", L"submenuRight", 22.0f)) : static_cast<int>(Metric(theme_, L"menuItem", L"textRight", 8.0f));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ToColorRef(theme_.color(L"menuItem", item->disabled ? L"disabled" : L"normal", L"text")));
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HGDIOBJ oldFont = SelectObject(dc, font);
    DrawTextW(dc, item->text.c_str(), static_cast<int>(item->text.size()), &textRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);

    if (item->submenu) {
        HPEN pen = CreatePen(PS_SOLID, 1, ToColorRef(theme_.color(L"menuItem", item->disabled ? L"disabled" : L"normal", L"text")));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        const int midY = (rc.top + rc.bottom) / 2;
        const int arrowRight = static_cast<int>(Metric(theme_, L"menuItem", L"arrowRight", 9.0f));
        const int arrowWidth = static_cast<int>(Metric(theme_, L"menuItem", L"arrowWidth", 5.0f));
        const int arrowHalfHeight = static_cast<int>(Metric(theme_, L"menuItem", L"arrowHalfHeight", 4.0f));
        MoveToEx(dc, rc.right - arrowRight - arrowWidth, midY - arrowHalfHeight, nullptr);
        LineTo(dc, rc.right - arrowRight, midY);
        LineTo(dc, rc.right - arrowRight - arrowWidth, midY + arrowHalfHeight);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }
    return true;
}

void MainWindow::ClampScrollOffsets() {
    if (!hwnd_) {
        groupScrollOffset_ = 0.0f;
        tagScrollOffset_ = 0.0f;
        linkScrollOffset_ = 0.0f;
        return;
    }

    RECT client{};
    GetClientRect(hwnd_, &client);
    D2D1_RECT_F title{}, groups{}, tags{}, links{};
    BuildLayout(static_cast<float>(client.right - client.left), static_cast<float>(client.bottom - client.top), title, groups, tags, links);
    groupScrollOffset_ = ClampFloat(groupScrollOffset_, 0.0f, MaxGroupScrollOffset(groups));
    tagScrollOffset_ = ClampFloat(tagScrollOffset_, 0.0f, MaxTagScrollOffset(tags));
    linkScrollOffset_ = ClampFloat(linkScrollOffset_, 0.0f, MaxLinkScrollOffset(links));
}

void MainWindow::ScrollAtPoint(float x, float y, int wheelDelta, bool horizontal) {
    if (wheelDelta == 0) {
        return;
    }

    RECT client{};
    GetClientRect(hwnd_, &client);
    D2D1_RECT_F title{}, groups{}, tags{}, links{};
    BuildLayout(static_cast<float>(client.right - client.left), static_cast<float>(client.bottom - client.top), title, groups, tags, links);

    const float step = horizontal ? Metric(theme_, L"scrollbar", L"wheelStepX", 72.0f) : Metric(theme_, L"scrollbar", L"wheelStepY", 80.0f);
    const float amount = -static_cast<float>(wheelDelta) / static_cast<float>(WHEEL_DELTA) * step;
    bool changed = false;
    if (config_.showGroup && Contains(groups, x, y)) {
        groupScrollOffset_ = ClampFloat(groupScrollOffset_ + amount, 0.0f, MaxGroupScrollOffset(groups));
        changed = true;
    } else if (config_.showTag && Contains(tags, x, y)) {
        tagScrollOffset_ = ClampFloat(tagScrollOffset_ + amount, 0.0f, MaxTagScrollOffset(tags));
        changed = true;
    } else if (Contains(links, x, y)) {
        linkScrollOffset_ = ClampFloat(linkScrollOffset_ + amount, 0.0f, MaxLinkScrollOffset(links));
        changed = true;
    }

    if (changed) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

float MainWindow::MaxGroupScrollOffset(const D2D1_RECT_F& rect) const {
    if (Height(rect) <= 0 || Width(rect) <= 0 || !config_.showGroup) {
        return 0.0f;
    }

    if (Height(rect) > Width(rect)) {
        const auto groups = MajorGroups();
        const float itemHeight = Metric(theme_, L"majorNavItem", L"verticalHeight", 32.0f);
        const float itemGap = Metric(theme_, L"majorNavItem", L"verticalGap", 2.0f);
        const float topInset = Metric(theme_, L"majorNavItem", L"verticalTopInset", 2.0f);
        const float bottomInset = Metric(theme_, L"majorNavItem", L"verticalBottomInset", 2.0f);
        const float contentHeight = topInset + bottomInset + static_cast<float>(groups.size()) * itemHeight
            + static_cast<float>(groups.empty() ? 0 : groups.size() - 1) * itemGap;
        return std::max(0.0f, contentHeight - Height(rect));
    }

    const float itemOffsetX = Metric(theme_, L"majorNavItem", L"offsetX", 4.0f);
    const float contentRightPadding = Metric(theme_, L"majorNavItem", L"contentRightPadding", 4.0f);
    const float itemMinWidth = Metric(theme_, L"majorNavItem", L"minWidth", 72.0f);
    const float itemMaxWidth = Metric(theme_, L"majorNavItem", L"maxWidth", 128.0f);
    const float itemWidthAdjust = Metric(theme_, L"majorNavItem", L"widthAdjust", -14.0f);
    float contentWidth = itemOffsetX + contentRightPadding;
    const auto groups = MajorGroups();
    for (const auto& group : groups) {
        contentWidth += std::max(itemMinWidth, std::min(itemMaxWidth, NavigationItemWidth(theme_, group.name) + itemWidthAdjust));
    }
    return std::max(0.0f, contentWidth - Width(rect));
}

float MainWindow::MaxTagScrollOffset(const D2D1_RECT_F& rect) const {
    if (Height(rect) <= 0 || Width(rect) <= 0 || !config_.showTag) {
        return 0.0f;
    }

    const auto tags = TagsForCurrentGroup();
    const float itemHeight = Metric(theme_, L"minorNavItem", L"height", 32.0f);
    const float itemGap = Metric(theme_, L"minorNavItem", L"gap", 2.0f);
    const float topInset = Metric(theme_, L"minorNavItem", L"topInset", 2.0f);
    const float bottomInset = Metric(theme_, L"minorNavItem", L"bottomInset", 2.0f);
    const float contentHeight = topInset + bottomInset + static_cast<float>(tags.size()) * (itemHeight + itemGap);
    return std::max(0.0f, contentHeight - Height(rect));
}

float MainWindow::MaxLinkScrollOffset(const D2D1_RECT_F& rect) const {
    if (Height(rect) <= 0 || Width(rect) <= 0) {
        return 0.0f;
    }

    auto links = const_cast<MainWindow*>(this)->LinksForCurrentTag();
    if (links.empty()) {
        return 0.0f;
    }

    const Group* tag = FindGroup(currentTagId_);
    const LinkLayoutMetrics metrics = MakeLinkLayout(theme_, rect, tag);
    const float contentHeight = LinkContentHeight(links.size(), metrics);
    return std::max(0.0f, contentHeight - Height(rect));
}

void MainWindow::EnsureGroupVisible(int groupId) {
    RECT client{};
    if (!hwnd_ || !GetClientRect(hwnd_, &client)) {
        return;
    }
    D2D1_RECT_F title{}, groupsRect{}, tagsRect{}, linksRect{};
    BuildLayout(static_cast<float>(client.right - client.left), static_cast<float>(client.bottom - client.top), title, groupsRect, tagsRect, linksRect);

    if (Height(groupsRect) > Width(groupsRect)) {
        const float itemHeight = Metric(theme_, L"majorNavItem", L"verticalHeight", 32.0f);
        const float itemGap = Metric(theme_, L"majorNavItem", L"verticalGap", 2.0f);
        const float topInset = Metric(theme_, L"majorNavItem", L"verticalTopInset", 2.0f);
        const float visibilityPadding = Metric(theme_, L"majorNavItem", L"visibilityPadding", 8.0f);
        float y = groupsRect.top + topInset;
        for (const auto& group : MajorGroups()) {
            if (group.id == groupId) {
                if (y - groupScrollOffset_ < groupsRect.top + visibilityPadding) {
                    groupScrollOffset_ = y - groupsRect.top - visibilityPadding;
                } else if (y + itemHeight - groupScrollOffset_ > groupsRect.bottom - visibilityPadding) {
                    groupScrollOffset_ = y + itemHeight - groupsRect.bottom + visibilityPadding;
                }
                groupScrollOffset_ = ClampFloat(groupScrollOffset_, 0.0f, MaxGroupScrollOffset(groupsRect));
                return;
            }
            y += itemHeight + itemGap;
        }
        return;
    }

    const float itemOffsetX = Metric(theme_, L"majorNavItem", L"offsetX", 4.0f);
    const float itemMinWidth = Metric(theme_, L"majorNavItem", L"minWidth", 72.0f);
    const float itemMaxWidth = Metric(theme_, L"majorNavItem", L"maxWidth", 128.0f);
    const float itemWidthAdjust = Metric(theme_, L"majorNavItem", L"widthAdjust", -14.0f);
    const float visibilityPadding = Metric(theme_, L"majorNavItem", L"visibilityPadding", 8.0f);
    float x = groupsRect.left + itemOffsetX;
    for (const auto& group : MajorGroups()) {
        const float width = std::max(itemMinWidth, std::min(itemMaxWidth, NavigationItemWidth(theme_, group.name) + itemWidthAdjust));
        if (group.id == groupId) {
            if (x - groupScrollOffset_ < groupsRect.left + visibilityPadding) {
                groupScrollOffset_ = x - groupsRect.left - visibilityPadding;
            } else if (x + width - groupScrollOffset_ > groupsRect.right - visibilityPadding) {
                groupScrollOffset_ = x + width - groupsRect.right + visibilityPadding;
            }
            groupScrollOffset_ = ClampFloat(groupScrollOffset_, 0.0f, MaxGroupScrollOffset(groupsRect));
            return;
        }
        x += width;
    }
}

void MainWindow::EnsureTagVisible(int tagId) {
    RECT client{};
    if (!hwnd_ || !GetClientRect(hwnd_, &client)) {
        return;
    }
    D2D1_RECT_F title{}, groupsRect{}, tagsRect{}, linksRect{};
    BuildLayout(static_cast<float>(client.right - client.left), static_cast<float>(client.bottom - client.top), title, groupsRect, tagsRect, linksRect);
    const float topInset = Metric(theme_, L"minorNavItem", L"topInset", 2.0f);
    const float itemHeight = Metric(theme_, L"minorNavItem", L"height", 32.0f);
    const float itemGap = Metric(theme_, L"minorNavItem", L"gap", 2.0f);
    const float visibilityPadding = Metric(theme_, L"minorNavItem", L"visibilityPadding", 8.0f);
    float y = tagsRect.top + topInset;
    for (const auto& tag : TagsForCurrentGroup()) {
        if (tag.id == tagId) {
            if (y - tagScrollOffset_ < tagsRect.top + visibilityPadding) {
                tagScrollOffset_ = y - tagsRect.top - visibilityPadding;
            } else if (y + itemHeight - tagScrollOffset_ > tagsRect.bottom - visibilityPadding) {
                tagScrollOffset_ = y + itemHeight - tagsRect.bottom + visibilityPadding;
            }
            tagScrollOffset_ = ClampFloat(tagScrollOffset_, 0.0f, MaxTagScrollOffset(tagsRect));
            return;
        }
        y += itemHeight + itemGap;
    }
}

void MainWindow::EnsureLinkVisible(int linkId) {
    if (linkId <= 0 || !hwnd_) {
        return;
    }
    RECT client{};
    if (!GetClientRect(hwnd_, &client)) {
        return;
    }
    D2D1_RECT_F title{}, groupsRect{}, tagsRect{}, linksRect{};
    BuildLayout(static_cast<float>(client.right - client.left), static_cast<float>(client.bottom - client.top), title, groupsRect, tagsRect, linksRect);
    auto links = LinksForCurrentTag();
    auto it = std::find_if(links.begin(), links.end(), [linkId](const Link* link) {
        return link && link->id == linkId;
    });
    if (it == links.end()) {
        return;
    }

    const int index = static_cast<int>(std::distance(links.begin(), it));
    const Group* tag = FindGroup(currentTagId_);
    const LinkLayoutMetrics metrics = MakeLinkLayout(theme_, linksRect, tag);
    const D2D1_RECT_F item = LinkItemRect(linksRect, metrics, index, 0.0f);
    const float itemTop = item.top;
    const float itemBottom = item.bottom;

    const float visibilityPadding = Metric(theme_, L"linkItem", L"visibilityPadding", 8.0f);
    if (itemTop - linkScrollOffset_ < linksRect.top + visibilityPadding) {
        linkScrollOffset_ = itemTop - linksRect.top - visibilityPadding;
    } else if (itemBottom - linkScrollOffset_ > linksRect.bottom - visibilityPadding) {
        linkScrollOffset_ = itemBottom - linksRect.bottom + visibilityPadding;
    }
    linkScrollOffset_ = ClampFloat(linkScrollOffset_, 0.0f, MaxLinkScrollOffset(linksRect));
}

std::vector<Group> MainWindow::MajorGroups() const {
    std::vector<Group> groups;
    for (const auto& group : model_.groups) {
        if (group.parentGroup == 0) {
            groups.push_back(group);
        }
    }
    std::sort(groups.begin(), groups.end(), [](const Group& left, const Group& right) {
        if (left.pos != right.pos) {
            return left.pos < right.pos;
        }
        return left.id < right.id;
    });
    return groups;
}

std::vector<Group> MainWindow::TagsForCurrentGroup() const {
    std::vector<Group> tags;
    for (const auto& group : model_.groups) {
        if (group.parentGroup == currentGroupId_) {
            tags.push_back(group);
        }
    }
    std::sort(tags.begin(), tags.end(), [](const Group& left, const Group& right) {
        if (left.pos != right.pos) {
            return left.pos < right.pos;
        }
        return left.id < right.id;
    });
    return tags;
}

std::wstring MainWindow::TagDisplayName(const Group& tag) const {
    const Group* parent = FindGroup(tag.parentGroup);
    if (parent && !parent->name.empty()) {
        return parent->name + L" / " + tag.name;
    }
    return tag.name;
}

std::vector<Link*> MainWindow::LinksForCurrentTag() {
    std::vector<Link*> links;
    const Group* tag = nullptr;
    for (const auto& group : model_.groups) {
        if (group.id == currentTagId_) {
            tag = &group;
            break;
        }
    }
    const bool showAllInGroup = tag && IsAllTag(*tag);
    const bool showTodoInGroup = tag && IsTodoTag(*tag);
    for (auto& link : model_.links) {
        if (showAllInGroup) {
            const Group* parentTag = FindGroup(link.parentGroup);
            if (parentTag && parentTag->parentGroup == currentGroupId_) {
                links.push_back(&link);
            }
        } else if (showTodoInGroup) {
            const Group* parentTag = FindGroup(link.parentGroup);
            if (parentTag && parentTag->parentGroup == currentGroupId_ && LooksLikeTodoLink(link)) {
                links.push_back(&link);
            }
        } else if (link.parentGroup == currentTagId_) {
            links.push_back(&link);
        }
    }
    const int sortMode = tag ? tag->sort : 0;
    std::sort(links.begin(), links.end(), [sortMode](const Link* left, const Link* right) {
        if (sortMode == 1 && left->runCount != right->runCount) {
            return left->runCount > right->runCount;
        }
        if (sortMode == 2) {
            const std::wstring leftName = InitialSortKey(left->name);
            const std::wstring rightName = InitialSortKey(right->name);
            if (leftName != rightName) {
                return leftName < rightName;
            }
        }
        if (left->pos != right->pos) {
            return left->pos < right->pos;
        }
        return left->id < right->id;
    });
    return links;
}

Group* MainWindow::FindGroup(int id) {
    for (auto& group : model_.groups) {
        if (group.id == id) {
            return &group;
        }
    }
    return nullptr;
}

const Group* MainWindow::FindGroup(int id) const {
    for (const auto& group : model_.groups) {
        if (group.id == id) {
            return &group;
        }
    }
    return nullptr;
}

Link* MainWindow::FindLink(int id) {
    for (auto& link : model_.links) {
        if (link.id == id) {
            return &link;
        }
    }
    return nullptr;
}

bool MainWindow::IsUrlLink(const Link& link) const {
    return link.type == 2 || LooksLikeUrlText(link.path);
}

int MainWindow::LinkIdFromHotKeyId(int hotKeyId) const {
    for (const auto& [registeredHotKeyId, linkId] : registeredLinkHotKeys_) {
        if (registeredHotKeyId == hotKeyId) {
            return linkId;
        }
    }
    return 0;
}

void MainWindow::BuildLayout(float width, float height, D2D1_RECT_F& title, D2D1_RECT_F& groups, D2D1_RECT_F& tags, D2D1_RECT_F& links) const {
    const float titleHeight = config_.showTitle ? Metric(theme_, L"title", L"height", 34.0f) : 0.0f;
    title = D2D1::RectF(0, 0, width, titleHeight);

    float left = 0.0f;
    float right = width;
    float top = titleHeight;
    const float bottom = height;

    groups = D2D1::RectF(0, top, width, top);
    tags = D2D1::RectF(0, top, 0, bottom);
    if (config_.showGroup) {
        if (config_.groupRight) {
            const float groupWidth = ClampFloat(static_cast<float>(config_.groupWidth), 40.0f, std::max(40.0f, width * 0.42f));
            groups = D2D1::RectF(std::max(left, right - groupWidth), top, right, bottom);
            right = groups.left;
        } else {
            const float groupHeight = Metric(theme_, L"majorNav", L"height", 34.0f);
            groups = D2D1::RectF(0.0f, top, width, std::min(bottom, top + groupHeight));
            top = groups.bottom;
        }
    }

    if (config_.showTag) {
        const float configuredTagWidth = config_.tagWidth > 0 ? static_cast<float>(config_.tagWidth) : Metric(theme_, L"minorNav", L"width", 72.0f);
        const float minTagWidth = Metric(theme_, L"minorNav", L"minWidth", 56.0f);
        const float maxTagRatio = Metric(theme_, L"minorNav", L"maxWidthRatio", 0.42f);
        const float maxTagWidth = std::max(minTagWidth, width * maxTagRatio);
        const float tagWidth = std::min(std::max(minTagWidth, configuredTagWidth), maxTagWidth);
        if (config_.tagRight) {
            tags = D2D1::RectF(std::max(left, right - tagWidth), top, right, bottom);
            right = tags.left;
        } else {
            tags = D2D1::RectF(left, top, std::min(right, left + tagWidth), bottom);
            left = tags.right;
        }
    }

    links = D2D1::RectF(left, top, right, bottom);
}

MainWindow::HitArea MainWindow::HitTest(float x, float y) const {
    for (auto it = hitAreas_.rbegin(); it != hitAreas_.rend(); ++it) {
        if (Contains(it->rect, x, y)) {
            return *it;
        }
    }
    return {};
}

bool MainWindow::IsHover(HitKind kind, int id) const {
    return hover_.kind == kind && hover_.id == id;
}

bool MainWindow::Contains(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

