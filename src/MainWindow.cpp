#include "MainWindow.h"

#include "AppLog.h"
#include "BuiltinTools.h"
#include "Elevation.h"
#include "HotKeyEditor.h"
#include "LinkEditDialog.h"
#include "MenuCatalog.h"
#include "ShellItemService.h"
#include "SimpleDialogs.h"
#include "StartupService.h"
#include "SystemFunctions.h"
#include "ThemedControls.h"
#include "TodoEditDialog.h"
#include "TodoSchedule.h"
#include "UrlEditDialog.h"
#include "Utilities.h"
#include "WebDavBackupService.h"
#include "WebDavRecoveryService.h"
#include "../resources/resource.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <commctrl.h>
#include <commdlg.h>
#include <ctime>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <optional>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <sstream>
#include <system_error>
#include <thread>
#include <uxtheme.h>
#include <windowsx.h>

namespace {
constexpr int ID_HOTKEY_MAIN = 1;
constexpr int ID_HOTKEY_LINK_BASE = 1000;
constexpr int ID_NOTE_EDIT = 2100;
constexpr int ID_REMINDER_PANEL = 2101;
constexpr UINT_PTR ID_TIMER_DOCK = 10;
constexpr UINT_PTR ID_TIMER_HOVER_ACTIVATE = 11;
constexpr UINT_PTR ID_TIMER_NOTE_AUTOSAVE = 12;
constexpr UINT_PTR ID_TIMER_REMINDER_SCAN = 13;
constexpr UINT_PTR ID_TIMER_REMINDER_PANEL = 14;
constexpr int kDockVisiblePixels = 3;
constexpr int kDockRestoreGraceMs = 1500;
constexpr const wchar_t* kTooltipWindowClass = L"QuattroTooltipWindow";
constexpr const wchar_t* kTooltipBgProp = L"QuattroTooltipBg";
constexpr const wchar_t* kTooltipTextProp = L"QuattroTooltipText";
constexpr const wchar_t* kTooltipBorderProp = L"QuattroTooltipBorder";
constexpr const wchar_t* kTooltipPaddingXProp = L"QuattroTooltipPaddingX";
constexpr const wchar_t* kTooltipPaddingYProp = L"QuattroTooltipPaddingY";
constexpr const wchar_t* kTooltipLineGapProp = L"QuattroTooltipLineGap";
constexpr const wchar_t* kTooltipRadiusProp = L"QuattroTooltipRadius";
constexpr const wchar_t* kTooltipBorderWidthProp = L"QuattroTooltipBorderWidth";
constexpr const wchar_t* kAppDisplayName = L"Quattro快速启动器";

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

Color RgbColor(int r, int g, int b, float a = 1.0f) {
    return Color{
        ClampFloat(static_cast<float>(r) / 255.0f, 0.0f, 1.0f),
        ClampFloat(static_cast<float>(g) / 255.0f, 0.0f, 1.0f),
        ClampFloat(static_cast<float>(b) / 255.0f, 0.0f, 1.0f),
        ClampFloat(a, 0.0f, 1.0f)};
}

// Linear blend between two colors. t=0 returns a, t=1 returns b.
Color LerpColor(const Color& a, const Color& b, float t) {
    t = ClampFloat(t, 0.0f, 1.0f);
    return Color{
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t,
        a.a + (b.a - a.a) * t};
}

COLORREF WindowPropColor(HWND hwnd, const wchar_t* name, COLORREF fallback) {
    HANDLE value = GetPropW(hwnd, name);
    if (!value) {
        return fallback;
    }
    return static_cast<COLORREF>(reinterpret_cast<UINT_PTR>(value));
}

int WindowPropInt(HWND hwnd, const wchar_t* name, int fallback) {
    HANDLE value = GetPropW(hwnd, name);
    if (!value) {
        return fallback;
    }
    return static_cast<int>(reinterpret_cast<UINT_PTR>(value));
}

std::vector<std::wstring> SplitTooltipLines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    std::wstringstream stream(text);
    std::wstring line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    if (lines.empty()) {
        lines.push_back(L"");
    }
    return lines;
}

SIZE MeasureTooltipText(HDC dc, const std::wstring& text, int lineGap) {
    SIZE result{};
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    const int lineHeight = metrics.tmHeight;
    for (const auto& line : SplitTooltipLines(text)) {
        SIZE lineSize{};
        GetTextExtentPoint32W(dc, line.c_str(), static_cast<int>(line.size()), &lineSize);
        result.cx = std::max(result.cx, lineSize.cx);
    }
    const int lineCount = static_cast<int>(SplitTooltipLines(text).size());
    result.cy = lineCount * lineHeight + std::max(0, lineCount - 1) * lineGap;
    return result;
}

LRESULT CALLBACK TooltipWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONUP:
        if (GetDlgCtrlID(hwnd) == ID_REMINDER_PANEL) {
            HWND parent = GetParent(hwnd);
            if (parent) {
                SendMessageW(parent, WM_COMMAND, MAKEWPARAM(ID_REMINDER_PANEL, 0), reinterpret_cast<LPARAM>(hwnd));
            }
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        const COLORREF bg = WindowPropColor(hwnd, kTooltipBgProp, RGB(32, 32, 32));
        const COLORREF text = WindowPropColor(hwnd, kTooltipTextProp, RGB(255, 255, 255));
        const COLORREF border = WindowPropColor(hwnd, kTooltipBorderProp, bg);
        const int radius = WindowPropInt(hwnd, kTooltipRadiusProp, 0);
        const int borderWidth = WindowPropInt(hwnd, kTooltipBorderWidthProp, 1);
        HBRUSH brush = CreateSolidBrush(bg);
        HPEN customPen = borderWidth > 0 ? CreatePen(PS_SOLID, borderWidth, border) : nullptr;
        HPEN pen = customPen ? customPen : reinterpret_cast<HPEN>(GetStockObject(NULL_PEN));
        HPEN previousPen = reinterpret_cast<HPEN>(SelectObject(dc, pen));
        HBRUSH previousBrush = reinterpret_cast<HBRUSH>(SelectObject(dc, brush));
        if (radius > 0) {
            const int diameter = radius * 2;
            RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, diameter, diameter);
        } else {
            Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
        }
        SelectObject(dc, previousBrush);
        SelectObject(dc, previousPen);
        if (customPen) {
            DeleteObject(customPen);
        }
        DeleteObject(brush);

        wchar_t buffer[1024]{};
        GetWindowTextW(hwnd, buffer, static_cast<int>(std::size(buffer)));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, text);
        HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
        if (!font) {
            font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        }
        HFONT previousFont = reinterpret_cast<HFONT>(SelectObject(dc, font));
        const int paddingX = WindowPropInt(hwnd, kTooltipPaddingXProp, 8);
        const int paddingY = WindowPropInt(hwnd, kTooltipPaddingYProp, 7);
        const int lineGap = WindowPropInt(hwnd, kTooltipLineGapProp, 4);
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        int y = rect.top + paddingY;
        for (const auto& line : SplitTooltipLines(buffer)) {
            TextOutW(dc, rect.left + paddingX, y, line.c_str(), static_cast<int>(line.size()));
            y += metrics.tmHeight + lineGap;
        }
        SelectObject(dc, previousFont);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_NCDESTROY:
        RemovePropW(hwnd, kTooltipBgProp);
        RemovePropW(hwnd, kTooltipTextProp);
        RemovePropW(hwnd, kTooltipBorderProp);
        RemovePropW(hwnd, kTooltipPaddingXProp);
        RemovePropW(hwnd, kTooltipPaddingYProp);
        RemovePropW(hwnd, kTooltipLineGapProp);
        RemovePropW(hwnd, kTooltipRadiusProp);
        RemovePropW(hwnd, kTooltipBorderWidthProp);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
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

std::wstring ConfigPackageFileName() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"quattro-%04u%02u%02u-%02u%02u.q4cfg",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute);
    return buffer;
}

std::wstring FormatConfigPackageReport(const ConfigPackageReport& report) {
    std::wstring text = report.message.empty() ? (report.ok ? L"操作完成。" : L"操作失败。") : report.message;
    if (report.groupsAdded > 0 || report.tagsAdded > 0 || report.linksAdded > 0 ||
        report.notesAdded > 0 || report.todosAdded > 0 || report.pluginSettingsAdded > 0 ||
        report.urlIconsAdded > 0) {
        text += L"\n\n新增分组: " + std::to_wstring(report.groupsAdded);
        text += L"\n新增标签: " + std::to_wstring(report.tagsAdded);
        text += L"\n新增启动项: " + std::to_wstring(report.linksAdded);
        text += L"\n新增便签: " + std::to_wstring(report.notesAdded);
        text += L"\n新增待办: " + std::to_wstring(report.todosAdded);
        text += L"\n新增工具设置: " + std::to_wstring(report.pluginSettingsAdded);
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

struct TodoVisualStyle {
    Color bg;
    Color border;
    Color title;
    Color dot;
    Color tagBg;
    Color tagText;
};

struct WebDavAsyncResult {
    bool download = false;
    WebDavBackupReport report;
};

// Derive a full todo card style from a single semantic accent color, blending
// toward the theme surface (for soft backgrounds) and toward text (for readable
// foregrounds). This keeps todo state colors in sync with the active theme
// instead of hardcoding per-state RGB values.
TodoVisualStyle MakeTodoStyle(const Color& accent, const Color& surface, const Color& text) {
    TodoVisualStyle style{};
    style.bg = LerpColor(surface, accent, 0.12f);
    style.border = LerpColor(surface, accent, 0.45f);
    style.dot = accent;
    style.title = LerpColor(accent, text, 0.55f);
    style.tagBg = LerpColor(surface, accent, 0.18f);
    style.tagText = LerpColor(accent, text, 0.35f);
    return style;
}

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

float TabGroupItemWidth(const Theme& theme, const std::wstring& text, IDWriteTextFormat* format, float measuredTextWidth) {
    const float minWidth = Metric(theme, L"majorNavItem", L"minWidth", 72.0f);
    const float maxWidth = Metric(theme, L"majorNavItem", L"textMaxWidth", 168.0f);
    const float paddingX = Metric(theme, L"tabButton", L"paddingX", 12.0f);
    const float fallback = NavigationItemWidth(theme, text);
    const float desired = (measuredTextWidth > 0.0f && format)
        ? measuredTextWidth + paddingX * 2.0f
        : fallback;
    return std::max(minWidth, std::min(maxWidth, desired));
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
    return tag.type == 2 || tag.name == L"待办" || content == L"todo";
}

bool IsNoteTag(const Group& tag) {
    return tag.type == 3 || ToLower(tag.content) == L"note";
}

bool IsTodoItemsTag(const Group& tag) {
    return tag.type == 4 || ToLower(tag.content) == L"todoitems";
}

std::wstring TodoScheduleText(TodoScheduleKind kind) {
    switch (kind) {
    case TodoScheduleKind::Once:
        return L"一次性";
    case TodoScheduleKind::Secondly:
        return L"每秒";
    case TodoScheduleKind::Minutely:
        return L"每分";
    case TodoScheduleKind::Hourly:
        return L"每时";
    case TodoScheduleKind::Daily:
        return L"每日";
    case TodoScheduleKind::Weekly:
        return L"每周";
    case TodoScheduleKind::Monthly:
        return L"每月";
    case TodoScheduleKind::Yearly:
        return L"每年";
    case TodoScheduleKind::Cron:
        return L"Cron";
    default:
        return L"无时间";
    }
}

std::wstring TodoScheduleText(const TodoItem& item) {
    if (item.scheduleKind == TodoScheduleKind::Cron) {
        return item.cronExpression.empty() ? L"Cron" : L"Cron " + item.cronExpression;
    }
    if (!IsRecurringTodoSchedule(item.scheduleKind) || item.repeatInterval <= 1) {
        return TodoScheduleText(item.scheduleKind);
    }
    std::wstring unit;
    switch (item.scheduleKind) {
    case TodoScheduleKind::Secondly:
        unit = L"秒";
        break;
    case TodoScheduleKind::Minutely:
        unit = L"分";
        break;
    case TodoScheduleKind::Hourly:
        unit = L"时";
        break;
    case TodoScheduleKind::Daily:
        unit = L"天";
        break;
    case TodoScheduleKind::Weekly:
        unit = L"周";
        break;
    case TodoScheduleKind::Monthly:
        unit = L"月";
        break;
    case TodoScheduleKind::Yearly:
        unit = L"年";
        break;
    default:
        unit = L"";
        break;
    }
    return L"每 " + std::to_wstring(item.repeatInterval) + L" " + unit;
}

bool IsTodoOverdue(const TodoItem& item) {
    if (!item.enabled || !item.completedAt.empty() || item.nextDueAt.empty()) {
        return false;
    }
    SYSTEMTIME due{};
    SYSTEMTIME now{};
    if (!TryParseTodoTimestamp(item.nextDueAt, due)) {
        return false;
    }
    GetLocalTime(&now);
    FILETIME dueFile{};
    FILETIME nowFile{};
    return SystemTimeToFileTime(&due, &dueFile) &&
           SystemTimeToFileTime(&now, &nowFile) &&
           CompareFileTime(&dueFile, &nowFile) < 0;
}

bool IsTodoDueForReminder(const TodoItem& item) {
    if (!item.enabled || !item.completedAt.empty() || item.nextDueAt.empty()) {
        return false;
    }
    SYSTEMTIME due{};
    SYSTEMTIME now{};
    if (!TryParseTodoTimestamp(item.nextDueAt, due)) {
        return false;
    }
    GetLocalTime(&now);
    FILETIME dueFile{};
    FILETIME nowFile{};
    return SystemTimeToFileTime(&due, &dueFile) &&
           SystemTimeToFileTime(&now, &nowFile) &&
           CompareFileTime(&dueFile, &nowFile) <= 0;
}

std::wstring TodoReminderKey(const TodoItem& item) {
    return std::to_wstring(item.id) + L"|" + item.nextDueAt;
}

std::wstring TodoReminderText(const TodoItem& item) {
    std::wstring text = item.title;
    if (!Trim(item.content).empty()) {
        text += L"\n" + Trim(item.content);
    }
    if (!item.nextDueAt.empty()) {
        text += L"\n" + TodoScheduleText(item) + L" " + item.nextDueAt;
        if (IsRecurringTodoSchedule(item.scheduleKind) && item.repeatLimit > 0) {
            text += L" (" + std::to_wstring(item.repeatFinished) + L"/" + std::to_wstring(item.repeatLimit) + L")";
        }
    }
    return text;
}

std::wstring LimitNotificationText(const std::wstring& value, std::size_t limit) {
    if (value.size() <= limit) {
        return value;
    }
    return value.substr(0, limit > 3 ? limit - 3 : limit) + L"...";
}

std::vector<std::wstring> LimitedTooltipContentLines(const std::wstring& value) {
    constexpr std::size_t kMaxLines = 3;
    constexpr std::size_t kMaxLineLength = 120;
    std::vector<std::wstring> lines;
    bool truncated = false;
    for (auto line : SplitTooltipLines(Trim(value))) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        if (line.size() > kMaxLineLength) {
            line = line.substr(0, kMaxLineLength - 3) + L"...";
            truncated = true;
        }
        if (lines.size() >= kMaxLines) {
            truncated = true;
            break;
        }
        lines.push_back(std::move(line));
    }
    if (truncated && !lines.empty() && lines.back().size() >= 3 && lines.back().substr(lines.back().size() - 3) != L"...") {
        lines.back() += L"...";
    }
    return lines;
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

std::vector<PluginRecord> BuiltinToolMenuPlugins(const std::vector<PluginRecord>& loaded) {
    std::vector<PluginRecord> tools;
    const auto builtins = PluginRegistry::BuiltinPlugins();
    for (const auto& builtin : builtins) {
        if (builtin.kind != L"builtin-tool" || builtin.engine.empty()) {
            continue;
        }
        PluginRecord item = builtin;
        for (const auto& plugin : loaded) {
            if (plugin.kind == L"builtin-tool" && (plugin.id == builtin.id || plugin.engine == builtin.engine)) {
                item = plugin;
                if (item.id.empty()) item.id = builtin.id;
                if (item.name.empty()) item.name = builtin.name;
                if (item.engine.empty()) item.engine = builtin.engine;
                if (item.kind.empty()) item.kind = builtin.kind;
                break;
            }
        }
        tools.push_back(std::move(item));
    }

    for (const auto& plugin : loaded) {
        if (plugin.kind != L"builtin-tool" || plugin.engine.empty()) {
            continue;
        }
        const auto exists = std::find_if(tools.begin(), tools.end(), [&](const PluginRecord& item) {
            return item.id == plugin.id || item.engine == plugin.engine;
        });
        if (exists == tools.end()) {
            tools.push_back(plugin);
        }
    }
    return tools;
}

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

struct MenuIconPalette {
    COLORREF accent = RGB(0, 153, 215);
    COLORREF danger = RGB(228, 48, 58);
    COLORREF warning = RGB(245, 180, 40);
    COLORREF success = RGB(24, 150, 92);
    COLORREF muted = RGB(100, 116, 139);
    COLORREF disabled = RGB(160, 168, 178);
    COLORREF neutral = RGB(255, 255, 255);
};

bool DrawLocalMenuIcon(HDC dc, const RECT& rc, int icon, bool disabled, COLORREF color, const MenuIconPalette& palette, const std::filesystem::path& appDirectory) {
    const wchar_t glyph = MenuIconGlyph(static_cast<MenuIcon>(icon));
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

    const COLORREF accent = disabled ? palette.disabled :
        (icon == MenuIconDelete || icon == MenuIconClear || icon == MenuIconExit || icon == MenuIconPower ? palette.danger : palette.accent);
    const int oldBkMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldTextColor = SetTextColor(dc, color == CLR_INVALID ? accent : color);
    HGDIOBJ oldFont = SelectObject(dc, font);
    RECT textRect = rc;
    DrawTextW(dc, &glyph, 1, &textRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOCLIP);
    SelectObject(dc, oldFont);
    SetTextColor(dc, oldTextColor);
    SetBkMode(dc, oldBkMode);
    DeleteObject(font);
    return true;
}

void DrawFallbackMenuIcon(HDC dc, const RECT& rc, int icon, bool disabled, COLORREF color, const MenuIconPalette& palette) {
    const COLORREF blue = disabled ? palette.disabled : (color == CLR_INVALID ? palette.accent : color);
    const COLORREF red = disabled ? palette.disabled : (color == CLR_INVALID ? palette.danger : color);
    const COLORREF mutedColor = disabled ? palette.disabled : palette.muted;
    const COLORREF amber = disabled ? palette.disabled : palette.warning;
    const COLORREF green = disabled ? palette.disabled : palette.success;
    const int l = rc.left + 1;
    const int t = rc.top + 1;
    const int r = rc.right - 1;
    const int b = rc.bottom - 1;
    HPEN pen = CreatePen(PS_SOLID, 2, icon == MenuIconDelete || icon == MenuIconClear ? red : blue);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HBRUSH brush = CreateSolidBrush(icon == MenuIconFolder ? amber : palette.neutral);
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
            HBRUSH fillBrush = CreateSolidBrush(disabled ? palette.disabled : palette.neutral);
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
            HBRUSH fillBrush = CreateSolidBrush(disabled ? palette.disabled : palette.warning);
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

void DrawMenuIcon(HDC dc, const RECT& rc, int icon, bool disabled, COLORREF color, const MenuIconPalette& palette, const std::filesystem::path& appDirectory) {
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
    case MenuIconInfo: drawn = DrawStockMenuIcon(dc, rc, SIID_INFO); break;
    case MenuIconDelete: drawn = DrawStockMenuIcon(dc, rc, SIID_DELETE); break;
    default:
        break;
    }
    if (!drawn) {
        drawn = DrawLocalMenuIcon(dc, rc, icon, disabled, color, palette, appDirectory);
    }
    if (!drawn) {
        DrawFallbackMenuIcon(dc, rc, icon, disabled, color, palette);
    }
}

bool LooksLikeUrlText(const std::wstring& value) {
    const std::wstring lower = ToLower(Trim(value));
    return lower.rfind(L"http://", 0) == 0 ||
           lower.rfind(L"https://", 0) == 0 ||
           lower.rfind(L"ftp://", 0) == 0 ||
           lower.rfind(L"www.", 0) == 0;
}

bool IsVolumeMenuIcon(MenuIcon icon) {
    return icon == MenuIconVolumeUp ||
           icon == MenuIconVolumeDown ||
           icon == MenuIconVolumeMute;
}

std::optional<std::wstring> CurrentMasterVolumeText() {
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioEndpointVolume* endpoint = nullptr;
    std::optional<std::wstring> result;

    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
        SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
        SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&endpoint)))) {
        float level = 0.0f;
        BOOL muted = FALSE;
        if (SUCCEEDED(endpoint->GetMasterVolumeLevelScalar(&level)) &&
            SUCCEEDED(endpoint->GetMute(&muted))) {
            const int percent = static_cast<int>(ClampFloat(level, 0.0f, 1.0f) * 100.0f + 0.5f);
            result = L"当前音量: " + std::to_wstring(percent) + L"%" + (muted ? L"（已静音）" : L"");
        }
    }

    SafeRelease(endpoint);
    SafeRelease(device);
    SafeRelease(enumerator);
    return result;
}

std::wstring SafeFileName(std::wstring name) {
    name = Trim(name);
    if (name.empty()) {
        name = kAppDisplayName;
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
        if (window_) {
            window_->SetDragOver(true);
        }
        if (effect) {
            *effect = DROPEFFECT_COPY;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        if (window_) {
            window_->SetDragOver(true);
        }
        if (effect) {
            *effect = DROPEFFECT_COPY;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (window_) {
            window_->SetDragOver(false);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* dataObject, DWORD, POINTL, DWORD* effect) override {
        if (window_) {
            window_->SetDragOver(false);
        }
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

#define MessageBoxW(owner, message, title, flags) ShowThemedMessageBox((owner), instance_, theme_, (message), (title), (flags))

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
      pluginRegistry_(appDirectory_),
      config_(std::move(config)),
      model_(std::move(model)),
      theme_(std::move(theme)),
      launcher_(appDirectory_, &config_),
      iconService_(appDirectory_),
      urlIconDownloadService_(appDirectory_),
      runningAsAdmin_(IsRunningAsAdmin()) {
    if (config_.preferAdminRun != runningAsAdmin_) {
        config_.preferAdminRun = runningAsAdmin_;
        configService_.Save(config_);
    }
    SelectInitialItems();
}

MainWindow::~MainWindow() {
    SaveCurrentNotePage();
    if (tooltip_) {
        DestroyWindow(tooltip_);
        tooltip_ = nullptr;
    }
    if (noteEdit_) {
        DestroyWindow(noteEdit_);
        noteEdit_ = nullptr;
    }
    if (noteEditFont_) {
        DeleteObject(noteEditFont_);
        noteEditFont_ = nullptr;
    }
    if (noteEditBrush_) {
        DeleteObject(noteEditBrush_);
        noteEditBrush_ = nullptr;
    }
    if (reminderPanel_) {
        DestroyWindow(reminderPanel_);
        reminderPanel_ = nullptr;
    }
    if (tooltipFont_) {
        DeleteObject(tooltipFont_);
        tooltipFont_ = nullptr;
    }
    if (reminderPanelFont_) {
        DeleteObject(reminderPanelFont_);
        reminderPanelFont_ = nullptr;
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
    pluginRegistry_.Initialize();

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
        kAppDisplayName,
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
    reminderScanTimerId_ = SetTimer(hwnd_, ID_TIMER_REMINDER_SCAN, 1000, nullptr);
    CheckTodoReminders();
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
    case WM_QUATTRO_URL_ICON_DOWNLOADED:
        OnUrlIconDownloaded(static_cast<int>(wParam), lParam != 0);
        return 0;
    case WM_QUATTRO_WEBDAV_DONE: {
        std::unique_ptr<WebDavAsyncResult> result(reinterpret_cast<WebDavAsyncResult*>(lParam));
        if (!result) {
            return 0;
        }
        if (result->download && result->report.ok) {
            model_ = storageService_.Load();
            SelectInitialItems();
            pluginRegistry_.Initialize();
            RegisterConfiguredHotKeys();
            ClearUiBitmaps();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        const std::wstring text = result->report.importReport.message.empty()
            ? result->report.message
            : FormatConfigPackageReport(result->report.importReport);
        MessageBoxW(hwnd_, text.c_str(), result->download ? L"下载 WebDAV 备份" : L"上传 WebDAV 备份",
                    MB_OK | (result->report.ok ? MB_ICONINFORMATION : MB_ICONWARNING));
        return 0;
    }
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

        const int titleHeight = config_.showTitle
            ? static_cast<int>(Metric(theme_, L"title", L"height", 34.0f))
            : 0;
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
        if (wParam == ID_TIMER_NOTE_AUTOSAVE) {
            KillTimer(hwnd_, ID_TIMER_NOTE_AUTOSAVE);
            noteSaveTimerId_ = 0;
            SaveCurrentNotePage();
            return 0;
        }
        if (wParam == ID_TIMER_REMINDER_SCAN) {
            CheckTodoReminders();
            return 0;
        }
        if (wParam == ID_TIMER_REMINDER_PANEL) {
            HideTodoReminderPanel();
            return 0;
        }
        return 0;
    case WM_DROPFILES: {
        SetDragOver(false);
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
        HideItemTooltip();
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
        UpdateItemTooltip(next, screenPoint);
        return 0;
    }
    case WM_MOUSELEAVE:
        HideItemTooltip();
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
        HideItemTooltip();
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd_, &point);
        ScrollAtPoint(static_cast<float>(point.x), static_cast<float>(point.y), GET_WHEEL_DELTA_WPARAM(wParam), false);
        return 0;
    }
    case WM_MOUSEHWHEEL: {
        HideItemTooltip();
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd_, &point);
        ScrollAtPoint(static_cast<float>(point.x), static_cast<float>(point.y), GET_WHEEL_DELTA_WPARAM(wParam), true);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        HideItemTooltip();
        SetFocus(hwnd_);
        selectionByKeyboard_ = false;
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        HitArea hit = HitTest(x, y);
        switch (hit.kind) {
        case HitKind::CloseButton:
            HideMainWindow();
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
            if (const Group* tag = FindGroup(currentTagId_); tag && IsTodoItemsTag(*tag)) {
                AddTodoItem();
            } else {
                AddLink();
            }
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
        case HitKind::Todo:
            selectedTodoId_ = hit.id;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        default:
            return 0;
        }
    }
    case WM_RBUTTONUP: {
        HideItemTooltip();
        SetFocus(hwnd_);
        selectionByKeyboard_ = false;
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        HitArea hit = HitTest(x, y);
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(hwnd_, &point);
        if (hit.kind == HitKind::Link) {
            selectedLinkId_ = hit.id;
            ShowLinkMenu(hit.id, point);
        } else if (hit.kind == HitKind::Todo) {
            selectedTodoId_ = hit.id;
            ShowTodoMenu(hit.id, point);
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
        HideItemTooltip();
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        HitArea hit = HitTest(x, y);
        if (hit.kind == HitKind::Link) {
            RunLink(hit.id);
        } else if (hit.kind == HitKind::Todo) {
            EditTodoItem(hit.id);
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
        info->ptMinTrackSize.x = MulDiv(260, dpi, 96);
        info->ptMinTrackSize.y = MulDiv(320, dpi, 96);
        return 0;
    }
    case WM_CLOSE:
        if (exitingForPrivilegeRestart_) {
            DestroyWindow(hwnd_);
            return 0;
        }
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
        if (command == ID_REMINDER_PANEL) {
            HideTodoReminderPanel();
            return 0;
        }
        if (command == ID_NOTE_EDIT && HIWORD(wParam) == EN_CHANGE) {
            noteDirty_ = true;
            if (noteSaveTimerId_ != 0) {
                KillTimer(hwnd_, ID_TIMER_NOTE_AUTOSAVE);
            }
            noteSaveTimerId_ = SetTimer(hwnd_, ID_TIMER_NOTE_AUTOSAVE, 700, nullptr);
            return 0;
        }
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
        if (command >= ID_MENU_ADD_SYSTEM_FUNCTION_BASE && command < ID_MENU_ADD_SYSTEM_FUNCTION_BASE + ID_MENU_ADD_SYSTEM_FUNCTION_LIMIT) {
            AddSystemFunction(static_cast<std::size_t>(command - ID_MENU_ADD_SYSTEM_FUNCTION_BASE));
            return 0;
        }
        if (command >= ID_MENU_TOOL_BASE && command < ID_MENU_TOOL_BASE + ID_MENU_TOOL_LIMIT) {
            OpenBuiltinTool(static_cast<std::size_t>(command - ID_MENU_TOOL_BASE));
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
        case ID_MENU_ADD_NOTE_TAG:
            AddNoteTag();
            return 0;
        case ID_MENU_ADD_TODO_TAG:
            AddTodoTag();
            return 0;
        case ID_MENU_EDIT_TAG:
            EditTag(CommandTagId());
            return 0;
        case ID_MENU_DELETE_TAG:
            DeleteTag(CommandTagId());
            return 0;
        case ID_MENU_ADD_TODO_ITEM:
            AddTodoItem();
            return 0;
        case ID_MENU_EDIT_TODO_ITEM:
            EditTodoItem(CommandTodoId());
            return 0;
        case ID_MENU_DELETE_TODO_ITEM:
            DeleteTodoItem(CommandTodoId());
            return 0;
        case ID_MENU_TOGGLE_TODO_DONE:
            ToggleTodoDone(CommandTodoId());
            return 0;
        case ID_MENU_TOGGLE_TODO_ENABLED:
            ToggleTodoEnabled(CommandTodoId());
            return 0;
        case ID_MENU_CLEAR_DONE_TODOS:
            ClearDoneTodos();
            return 0;
        case ID_MENU_TODO_SORT_DUE:
            SetCurrentTodoSort(0);
            return 0;
        case ID_MENU_TODO_SORT_CREATED:
            SetCurrentTodoSort(1);
            return 0;
        case ID_MENU_TODO_SORT_TITLE:
            SetCurrentTodoSort(2);
            return 0;
        case ID_MENU_TODO_SORT_STATUS:
            SetCurrentTodoSort(3);
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
        case ID_MENU_SETTINGS:
            OpenSettings();
            return 0;
        case ID_MENU_RESET_LAYOUT:
            ResetLayoutToDefaults();
            return 0;
        case ID_MENU_IMPORT_CLIPBOARD:
            ImportClipboard();
            return 0;
        case ID_MENU_IMPORT_CONFIG_MERGE:
            ImportConfigPackageMerge();
            return 0;
        case ID_MENU_EXPORT_CONFIG:
            ExportConfigPackage();
            return 0;
        case ID_MENU_UPLOAD_WEBDAV_BACKUP:
            UploadWebDavBackup();
            return 0;
        case ID_MENU_DOWNLOAD_WEBDAV_BACKUP:
            DownloadWebDavBackupMerge();
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
        case ID_MENU_RESTART_PRIVILEGE:
            RestartWithOppositePrivilege();
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
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            const HitArea hit = CursorHitArea();
            switch (hit.kind) {
            case HitKind::CloseButton:
            case HitKind::MenuButton:
            case HitKind::SkinButton:
            case HitKind::AddButton:
            case HitKind::Group:
            case HitKind::Tag:
            case HitKind::Link:
            case HitKind::Todo:
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            default:
                break;
            }
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_KEYDOWN:
        if (HandleKeyDown(wParam)) {
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
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
    case WM_CTLCOLOREDIT:
        if (reinterpret_cast<HWND>(lParam) == noteEdit_) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, ToColorRef(theme_.color(L"edit", L"normal", L"text")));
            SetBkColor(dc, ToColorRef(theme_.color(L"edit", L"normal", L"bg")));
            if (!noteEditBrush_) {
                noteEditBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"edit", L"normal", L"bg")));
            }
            return reinterpret_cast<LRESULT>(noteEditBrush_);
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    case WM_DESTROY:
        SaveCurrentNotePage();
        urlIconDownloadService_.Shutdown();
        HideItemTooltip();
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
        if (noteSaveTimerId_ != 0) {
            KillTimer(hwnd_, ID_TIMER_NOTE_AUTOSAVE);
            noteSaveTimerId_ = 0;
        }
        if (reminderScanTimerId_ != 0) {
            KillTimer(hwnd_, ID_TIMER_REMINDER_SCAN);
            reminderScanTimerId_ = 0;
        }
        if (reminderPanelTimerId_ != 0) {
            KillTimer(hwnd_, ID_TIMER_REMINDER_PANEL);
            reminderPanelTimerId_ = 0;
        }
        HideTodoReminderPanel();
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

    SaveCurrentNotePage();
    currentGroupId_ = groupId;
    const auto tags = TagsForCurrentGroup();
    currentTagId_ = tags.empty() ? 0 : tags.front().id;
    selectedLinkId_ = 0;
    selectedTodoId_ = 0;
    selectionByKeyboard_ = false;
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

    SaveCurrentNotePage();
    currentTagId_ = tagId;
    selectedLinkId_ = 0;
    selectedTodoId_ = 0;
    selectionByKeyboard_ = false;
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

int MainWindow::CommandTodoId() const {
    if (menuContextKind_ == HitKind::Todo) {
        return menuContextId_;
    }
    return selectedTodoId_;
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
    SaveCurrentNotePage();
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
    model_.notes.erase(std::remove_if(model_.notes.begin(), model_.notes.end(), [&](const NotePage& note) {
        Group* tag = FindGroup(note.tagId);
        return tag && tag->parentGroup == groupId;
    }), model_.notes.end());
    model_.todos.erase(std::remove_if(model_.todos.begin(), model_.todos.end(), [&](const TodoItem& item) {
        Group* tag = FindGroup(item.tagId);
        return tag && tag->parentGroup == groupId;
    }), model_.todos.end());
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

void MainWindow::AddNoteTag() {
    const int parentGroupId = CommandGroupId();
    Group* parent = FindGroup(parentGroupId);
    if (!parent || parent->parentGroup != 0) {
        AddGroup();
        return;
    }
    Group tag;
    tag.name = UniqueSiblingGroupName(model_.groups, parentGroupId, L"便签");
    tag.parentGroup = parentGroupId;
    tag.pos = -1;
    tag.layout = 1;
    tag.iconSize = 32;
    tag.type = 3;
    tag.content = L"note";
    if (!storageService_.InsertGroup(tag)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"新增便签标签页", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.groups.push_back(tag);
    currentGroupId_ = parentGroupId;
    currentTagId_ = tag.id;
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    EnsureTagVisible(currentTagId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::AddTodoTag() {
    const int parentGroupId = CommandGroupId();
    Group* parent = FindGroup(parentGroupId);
    if (!parent || parent->parentGroup != 0) {
        AddGroup();
        return;
    }
    Group tag;
    tag.name = UniqueSiblingGroupName(model_.groups, parentGroupId, L"待办事项");
    tag.parentGroup = parentGroupId;
    tag.pos = -1;
    tag.layout = 1;
    tag.iconSize = 32;
    tag.type = 4;
    tag.content = L"todoItems";
    if (!storageService_.InsertGroup(tag)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"新增待办事项标签页", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.groups.push_back(tag);
    currentGroupId_ = parentGroupId;
    currentTagId_ = tag.id;
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
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
    SaveCurrentNotePage();
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
    model_.notes.erase(std::remove_if(model_.notes.begin(), model_.notes.end(), [tagId](const NotePage& note) {
        return note.tagId == tagId;
    }), model_.notes.end());
    model_.todos.erase(std::remove_if(model_.todos.begin(), model_.todos.end(), [tagId](const TodoItem& item) {
        return item.tagId == tagId;
    }), model_.todos.end());
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

void MainWindow::SetCurrentTodoSort(int sort) {
    Group* tag = FindGroup(CommandTagId());
    if (!tag || tag->parentGroup == 0 || !IsTodoItemsTag(*tag)) {
        return;
    }
    Group edited = *tag;
    edited.sort = std::max(0, std::min(3, sort));
    if (!storageService_.UpdateGroup(edited)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"待办排序", MB_OK | MB_ICONWARNING);
        return;
    }
    *tag = edited;
    linkScrollOffset_ = 0.0f;
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
    if (tag && tag->parentGroup != 0 && !IsNoteTag(*tag) && !IsTodoItemsTag(*tag)) {
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
    RequestInitialUrlIconDownload(link);
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
    RequestInitialUrlIconDownload(link);
    RegisterConfiguredHotKeys();
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::AddSystemFunction(std::size_t index) {
    if (EnsureCurrentTag() <= 0) {
        return;
    }

    Link link;
    link.parentGroup = CommandTagId();
    link.pos = -1;
    link.showCmd = SW_SHOWNORMAL;
    if (!ConfigureSystemFunctionLink(index, link)) {
        return;
    }
    link.parentGroup = CommandTagId();
    link.pos = -1;
    if (!storageService_.InsertLink(link)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"添加系统功能", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.links.push_back(link);
    selectedLinkId_ = link.id;
    currentTagId_ = link.parentGroup;
    if (Group* tag = FindGroup(currentTagId_); tag && tag->parentGroup != 0) {
        currentGroupId_ = tag->parentGroup;
    }
    config_.currentGroupId = currentGroupId_;
    config_.currentTagId = currentTagId_;
    configService_.SaveWindowState(config_);
    RegisterConfiguredHotKeys();
    EnsureGroupVisible(currentGroupId_);
    EnsureTagVisible(currentTagId_);
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

void MainWindow::AddTodoItem() {
    Group* tag = FindGroup(currentTagId_);
    if (!tag || !IsTodoItemsTag(*tag)) {
        return;
    }

    TodoItem item;
    item.tagId = currentTagId_;
    item.title.clear();
    item.enabled = true;
    item.scheduleKind = TodoScheduleKind::None;
    item.pos = -1;
    if (!TodoEditDialog::Show(hwnd_, instance_, theme_, item, true)) {
        return;
    }
    item.tagId = currentTagId_;
    item.pos = -1;
    if (!storageService_.InsertTodoItem(item)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"新增待办事项", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.todos.push_back(item);
    selectedTodoId_ = item.id;
    CheckTodoReminders();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::EditTodoItem(int todoId) {
    TodoItem* item = FindTodoItem(todoId);
    if (!item) {
        return;
    }
    TodoItem edited = *item;
    if (!TodoEditDialog::Show(hwnd_, instance_, theme_, edited, false)) {
        return;
    }
    edited.id = item->id;
    edited.tagId = item->tagId;
    edited.pos = item->pos;
    edited.createdAt = item->createdAt;
    if (!storageService_.UpdateTodoItem(edited)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"编辑待办事项", MB_OK | MB_ICONWARNING);
        return;
    }
    *item = edited;
    selectedTodoId_ = item->id;
    CheckTodoReminders();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::DeleteTodoItem(int todoId) {
    TodoItem* item = FindTodoItem(todoId);
    if (!item) {
        return;
    }
    std::wstring message = L"确定删除待办事项“" + item->title + L"”？";
    if (MessageBoxW(hwnd_, message.c_str(), L"删除待办事项", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    if (!storageService_.DeleteTodoItem(todoId)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"删除待办事项", MB_OK | MB_ICONWARNING);
        return;
    }
    model_.todos.erase(std::remove_if(model_.todos.begin(), model_.todos.end(), [todoId](const TodoItem& item) {
        return item.id == todoId;
    }), model_.todos.end());
    if (selectedTodoId_ == todoId) {
        selectedTodoId_ = 0;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ToggleTodoDone(int todoId) {
    TodoItem* item = FindTodoItem(todoId);
    if (!item) {
        return;
    }
    const bool complete = item->completedAt.empty();
    const std::wstring now = CurrentTodoTimestamp();
    if (!storageService_.SetTodoCompleted(todoId, complete)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"待办事项", MB_OK | MB_ICONWARNING);
        return;
    }
    if (complete && IsRecurringTodoSchedule(item->scheduleKind)) {
        ++item->repeatFinished;
        if (item->repeatLimit > 0 && item->repeatFinished >= item->repeatLimit) {
            item->completedAt = now;
            item->nextDueAt.clear();
        } else {
            item->completedAt.clear();
            item->nextDueAt = ComputeNextTodoDueAt(*item, now);
        }
    } else {
        item->completedAt = complete ? now : L"";
        if (!complete && IsRecurringTodoSchedule(item->scheduleKind) && item->nextDueAt.empty()) {
            item->nextDueAt = ComputeNextTodoDueAt(*item, now);
        }
    }
    item->updatedAt = now;
    selectedTodoId_ = todoId;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ToggleTodoEnabled(int todoId) {
    TodoItem* item = FindTodoItem(todoId);
    if (!item) {
        return;
    }
    const bool enable = !item->enabled;
    if (!storageService_.SetTodoEnabled(todoId, enable)) {
        MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"待办事项", MB_OK | MB_ICONWARNING);
        return;
    }
    item->enabled = enable;
    item->updatedAt = CurrentTodoTimestamp();
    selectedTodoId_ = todoId;
    if (enable) {
        CheckTodoReminders();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ClearDoneTodos() {
    Group* tag = FindGroup(currentTagId_);
    if (!tag || !IsTodoItemsTag(*tag)) {
        return;
    }
    std::vector<int> doneIds;
    for (const auto& item : model_.todos) {
        if (item.tagId == currentTagId_ && !item.completedAt.empty()) {
            doneIds.push_back(item.id);
        }
    }
    if (doneIds.empty()) {
        return;
    }
    if (MessageBoxW(hwnd_, L"确定清空已完成待办事项？", L"清空已完成", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    for (int id : doneIds) {
        if (!storageService_.DeleteTodoItem(id)) {
            MessageBoxW(hwnd_, storageService_.lastError().c_str(), L"清空已完成", MB_OK | MB_ICONWARNING);
            return;
        }
    }
    model_.todos.erase(std::remove_if(model_.todos.begin(), model_.todos.end(), [&](const TodoItem& item) {
        return std::find(doneIds.begin(), doneIds.end(), item.id) != doneIds.end();
    }), model_.todos.end());
    if (FindTodoItem(selectedTodoId_) == nullptr) {
        selectedTodoId_ = 0;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::CheckTodoReminders() {
    for (const auto& item : model_.todos) {
        if (!IsTodoDueForReminder(item)) {
            continue;
        }
        const std::wstring key = TodoReminderKey(item);
        if (shownReminderKeys_.find(key) != shownReminderKeys_.end()) {
            continue;
        }
        shownReminderKeys_.insert(key);
        ShowTodoReminder(item);
        return;
    }
}

void MainWindow::ShowTodoReminder(const TodoItem& item) {
    ShowTodoReminderPanel(item);
    ShowTodoSystemNotification(item);
}

void MainWindow::ShowTodoReminderPanel(const TodoItem& item) {
    HideTodoReminderPanel();

    if (!reminderPanelFont_) {
        reminderPanelFont_ = ThemedControls::CreateDialogFont();
    }

    RECT work{};
    HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        work = info.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    }

    const int width = 340;
    const int height = 136;
    const int x = work.right - width - 18;
    const int y = work.bottom - height - 18;
    const std::wstring text = L"待办提醒\r\n" + LimitNotificationText(item.title, 80) +
        (Trim(item.content).empty() ? L"" : (L"\r\n" + LimitNotificationText(Trim(item.content), 160)));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TooltipWindowProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = kTooltipWindowClass;
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    reminderPanel_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kTooltipWindowClass,
        text.c_str(),
        WS_POPUP,
        x,
        y,
        width,
        height,
        hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REMINDER_PANEL)),
        instance_,
        nullptr);
    if (!reminderPanel_) {
        return;
    }
    SendMessageW(reminderPanel_, WM_SETFONT, reinterpret_cast<WPARAM>(reminderPanelFont_), TRUE);
    const int paddingX = static_cast<int>(std::max(0.0f, Metric(theme_, L"tooltip", L"paddingX", 8.0f)));
    const int paddingY = static_cast<int>(std::max(0.0f, Metric(theme_, L"tooltip", L"paddingY", 7.0f)));
    const int lineGap = static_cast<int>(std::max(0.0f, Metric(theme_, L"tooltip", L"lineGap", 4.0f)));
    const int radius = static_cast<int>(std::max(0.0f, Metric(theme_, L"tooltip", L"radius", 6.0f)));
    const int borderWidth = static_cast<int>(std::max(0.0f, Metric(theme_, L"tooltip", L"borderWidth", 1.0f)));
    SetPropW(reminderPanel_, kTooltipBgProp, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(ToColorRef(theme_.color(L"tooltip", L"normal", L"bg")))));
    SetPropW(reminderPanel_, kTooltipTextProp, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(ToColorRef(theme_.color(L"tooltip", L"normal", L"text")))));
    SetPropW(reminderPanel_, kTooltipBorderProp, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(ToColorRef(theme_.color(L"tooltip", L"normal", L"border")))));
    SetPropW(reminderPanel_, kTooltipPaddingXProp, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(paddingX)));
    SetPropW(reminderPanel_, kTooltipPaddingYProp, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(paddingY)));
    SetPropW(reminderPanel_, kTooltipLineGapProp, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(lineGap)));
    SetPropW(reminderPanel_, kTooltipRadiusProp, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(radius)));
    SetPropW(reminderPanel_, kTooltipBorderWidthProp, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(borderWidth)));
    if (radius > 0) {
        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius * 2, radius * 2);
        if (!region || SetWindowRgn(reminderPanel_, region, TRUE) == 0) {
            if (region) {
                DeleteObject(region);
            }
        }
    }
    ShowWindow(reminderPanel_, SW_SHOWNOACTIVATE);
    SetWindowPos(reminderPanel_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    reminderPanelTimerId_ = SetTimer(hwnd_, ID_TIMER_REMINDER_PANEL, 12000, nullptr);
}

void MainWindow::HideTodoReminderPanel() {
    if (reminderPanelTimerId_ != 0 && hwnd_) {
        KillTimer(hwnd_, ID_TIMER_REMINDER_PANEL);
        reminderPanelTimerId_ = 0;
    }
    if (reminderPanel_) {
        DestroyWindow(reminderPanel_);
        reminderPanel_ = nullptr;
    }
}

bool MainWindow::EnsureNotificationIcon() {
    if (trayIconVisible_) {
        return true;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = WM_QUATTRO_TRAY;
    data.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
    wcscpy_s(data.szTip, kAppDisplayName);
    if (Shell_NotifyIconW(NIM_ADD, &data)) {
        trayIconVisible_ = true;
    }
    return trayIconVisible_;
}

void MainWindow::ShowTodoSystemNotification(const TodoItem& item) {
    if (!EnsureNotificationIcon()) {
        return;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = 1;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO;
    const std::wstring title = LimitNotificationText(item.title, 63);
    const std::wstring body = LimitNotificationText(Trim(item.content).empty() ? TodoReminderText(item) : Trim(item.content), 255);
    wcscpy_s(data.szInfoTitle, title.empty() ? L"待办提醒" : title.c_str());
    wcscpy_s(data.szInfo, body.empty() ? L"待办事项时间到了。" : body.c_str());
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void MainWindow::ShowClipboardImportNotification(int count, const std::wstring& pathDetail) {
    if (!EnsureNotificationIcon() || count <= 0) {
        return;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = 1;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO;
    const std::wstring title = count == 1 ? L"已添加启动项" : L"已添加启动项";
    const std::wstring body = pathDetail.empty()
        ? (count == 1 ? L"剪贴板内容已添加到当前标签。" : L"已从剪贴板导入多个启动项。")
        : LimitNotificationText(pathDetail, 255);
    wcscpy_s(data.szInfoTitle, LimitNotificationText(title, 63).c_str());
    wcscpy_s(data.szInfo, body.c_str());
    Shell_NotifyIconW(NIM_MODIFY, &data);
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

void MainWindow::OpenSettings() {
    AppConfig previous = config_;
    AppConfig next = config_;
    bool webDavDataImported = false;
    if (!ShowSettingsDialog(hwnd_, instance_, next, theme_, appDirectory_, &webDavDataImported)) {
        if (webDavDataImported) {
            model_ = storageService_.Load();
            SelectInitialItems();
            pluginRegistry_.Initialize();
            RegisterConfiguredHotKeys();
            ClearUiBitmaps();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
    config_ = next;
    configService_.Save(config_);
    std::wstring webDavRecoveryError;
    WebDavRecoveryService().Save(config_, webDavRecoveryError);
    if (!webDavRecoveryError.empty()) {
        WriteAppLog(webDavRecoveryError);
    }
    SyncAutoRun(previous);
    ApplyConfigRuntimeChanges(previous);
    if (webDavDataImported) {
        model_ = storageService_.Load();
        SelectInitialItems();
        pluginRegistry_.Initialize();
        RegisterConfiguredHotKeys();
        ClearUiBitmaps();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OpenBuiltinTool(std::size_t index) {
    if (menuToolEngines_.empty()) {
        const auto plugins = BuiltinToolMenuPlugins(pluginRegistry_.LoadPlugins());
        for (const auto& plugin : plugins) {
            if (plugin.kind != L"builtin-tool" || plugin.engine.empty()) {
                continue;
            }
            if (menuToolEngines_.size() >= ID_MENU_TOOL_LIMIT) {
                break;
            }
            menuToolEngines_.push_back(plugin.engine);
            menuToolEnabled_.push_back(plugin.enabled);
        }
    }
    if (index >= menuToolEngines_.size()) {
        return;
    }
    if (index < menuToolEnabled_.size() && !menuToolEnabled_[index]) {
        return;
    }
    ShowBuiltinTool(hwnd_, instance_, theme_, pluginRegistry_, menuToolEngines_[index]);
}

void MainWindow::ResetLayoutToDefaults() {
    AppConfig previous = config_;
    const AppConfig defaults;

    config_.showTitle = defaults.showTitle;
    config_.showGroup = defaults.showGroup;
    config_.showTag = defaults.showTag;
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
    if (IsUrlLink(*link)) {
        urlIconDownloadService_.RequestManualRefresh(hwnd_, WM_QUATTRO_URL_ICON_DOWNLOADED, *link);
        return;
    }
    iconService_.RefreshDiskCache(*link);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::RequestInitialUrlIconDownload(const Link& link) {
    if (IsUrlLink(link)) {
        urlIconDownloadService_.RequestInitialDownload(hwnd_, WM_QUATTRO_URL_ICON_DOWNLOADED, link);
    }
}

void MainWindow::OnUrlIconDownloaded(int linkId, bool success) {
    if (!success) {
        return;
    }
    if (Link* link = FindLink(linkId)) {
        iconService_.RefreshDiskCache(*link);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::ShowAbout() {
    const std::wstring privilegeText = runningAsAdmin_ ? L"管理员" : L"普通用户";
    const std::wstring message =
        std::wstring(kAppDisplayName) + L"\n\n轻量级 Windows 快速启动工具\nC++ / Win32 / Direct2D / DirectWrite\n\n开源仓库：https://github.com/codingriver/quattro\n当前权限：" + privilegeText;
    MessageBoxW(
        hwnd_,
        message.c_str(),
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

void MainWindow::RestartWithOppositePrivilege() {
    config_.preferAdminRun = !runningAsAdmin_;
    configService_.Save(config_);

    std::wstring error;
    const bool launched = runningAsAdmin_
        ? RestartCurrentProcessUnelevated(hwnd_, error)
        : RestartCurrentProcessElevated(hwnd_, error);
    if (!launched) {
        config_.preferAdminRun = runningAsAdmin_;
        configService_.Save(config_);
        MessageBoxW(
            hwnd_,
            error.empty() ? L"重启失败。" : error.c_str(),
            runningAsAdmin_ ? L"以普通用户重启" : L"以管理员身份重启",
            MB_OK | MB_ICONWARNING);
        return;
    }

    exitingForPrivilegeRestart_ = true;
    DestroyWindow(hwnd_);
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
    if (tooltipFont_) {
        if (tooltip_) {
            SendMessageW(tooltip_, WM_SETFONT, 0, TRUE);
        }
        DeleteObject(tooltipFont_);
        tooltipFont_ = nullptr;
    }
    if (reminderPanelFont_) {
        if (reminderPanel_) {
            SendMessageW(reminderPanel_, WM_SETFONT, 0, TRUE);
        }
        DeleteObject(reminderPanelFont_);
        reminderPanelFont_ = nullptr;
    }
    HFONT newNoteEditFont = ThemedControls::CreateEditFont(theme_);
    if (newNoteEditFont) {
        HFONT oldNoteEditFont = noteEditFont_;
        noteEditFont_ = newNoteEditFont;
        if (noteEdit_) {
            SendMessageW(noteEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(noteEditFont_), TRUE);
        }
        if (oldNoteEditFont) {
            DeleteObject(oldNoteEditFont);
        }
    }
    ApplyTooltipTheme();
    if (noteEditBrush_) {
        DeleteObject(noteEditBrush_);
        noteEditBrush_ = nullptr;
    }
    if (noteEdit_) {
        InvalidateRect(noteEdit_, nullptr, TRUE);
    }
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
    HideItemTooltip();
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
        RequestInitialUrlIconDownload(link);
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
    bool imported = false;

    HANDLE handle = GetClipboardData(CF_HDROP);
    int importedCount = 0;
    if (handle) {
        HDROP drop = static_cast<HDROP>(handle);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(length, L'\0');
            DragQueryFileW(drop, i, path.data(), length + 1);
            ImportPath(path);
            importedCount += 1;
        }
    }

    if (importedCount == 0) {
        handle = GetClipboardData(CF_UNICODETEXT);
        if (handle) {
            const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(handle));
            if (text) {
                ImportPath(text);
                GlobalUnlock(handle);
                importedCount = 1;
            }
        }
    }
    CloseClipboard();

    if (importedCount > 0) {
        ShowClipboardImportNotification(importedCount);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::ExportConfigPackage() {
    SaveCurrentNotePage();
    SaveWindowState();

    std::wstring buffer = (appDirectory_ / ConfigPackageFileName()).wstring();
    buffer.resize(32768, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrFilter = L"Quattro快速启动器 配置包 (*.q4cfg)\0*.q4cfg\0所有文件\0*.*\0";
    ofn.lpstrDefExt = L"q4cfg";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) {
        return;
    }

    ConfigPackageOptions options;
    options.includeConfig = true;
    options.includeData = true;
    options.includeUrlIcons = true;
    options.includePluginSettings = true;

    ConfigPackageService service(appDirectory_);
    const ConfigPackageReport report = service.ExportPackage(buffer.c_str(), options);
    MessageBoxW(hwnd_, FormatConfigPackageReport(report).c_str(), L"导出配置包", MB_OK | (report.ok ? MB_ICONINFORMATION : MB_ICONWARNING));
}

void MainWindow::ImportConfigPackageMerge() {
    SaveCurrentNotePage();

    std::wstring buffer(32768, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrFilter = L"Quattro快速启动器 配置包 (*.q4cfg)\0*.q4cfg\0所有文件\0*.*\0";
    ofn.lpstrDefExt = L"q4cfg";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) {
        return;
    }

    const int confirm = MessageBoxW(
        hwnd_,
        L"将把配置包中的分组、标签、启动项、便签、待办和工具设置合并到当前数据。\n\n当前数据不会被覆盖，导入前会自动备份。",
        L"合并导入配置包",
        MB_OKCANCEL | MB_ICONINFORMATION);
    if (confirm != IDOK) {
        return;
    }

    ConfigPackageOptions options;
    options.includeConfig = false;
    options.includeData = true;
    options.includeUrlIcons = true;
    options.includePluginSettings = true;

    ConfigPackageService service(appDirectory_);
    const ConfigPackageReport report = service.ImportPackageMerge(buffer.c_str(), options);
    if (report.ok) {
        model_ = storageService_.Load();
        SelectInitialItems();
        pluginRegistry_.Initialize();
        RegisterConfiguredHotKeys();
        ClearUiBitmaps();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    MessageBoxW(hwnd_, FormatConfigPackageReport(report).c_str(), L"合并导入配置包", MB_OK | (report.ok ? MB_ICONINFORMATION : MB_ICONWARNING));
}

void MainWindow::UploadWebDavBackup() {
    SaveCurrentNotePage();
    SaveWindowState();

    const HWND target = hwnd_;
    const std::filesystem::path appDirectory = appDirectory_;
    const AppConfig config = config_;
    std::thread([target, appDirectory, config]() {
        WebDavBackupService service(appDirectory, config);
        auto* result = new WebDavAsyncResult();
        result->download = false;
        result->report = service.UploadBackup();
        if (!PostMessageW(target, WM_QUATTRO_WEBDAV_DONE, 0, reinterpret_cast<LPARAM>(result))) {
            delete result;
        }
    }).detach();
}

void MainWindow::DownloadWebDavBackupMerge() {
    SaveCurrentNotePage();

    WebDavBackupService service(appDirectory_, config_);
    std::vector<WebDavRemoteFile> backups;
    std::wstring error;
    if (!service.ListBackups(backups, error)) {
        MessageBoxW(hwnd_, error.c_str(), L"下载 WebDAV 备份", MB_OK | MB_ICONWARNING);
        return;
    }
    if (backups.empty()) {
        MessageBoxW(hwnd_, L"远端目录中没有可用的 .q4cfg 备份。", L"下载 WebDAV 备份", MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::wstring fileName = backups.front().name;
    if (!ShowWebDavBackupSelectionDialog(hwnd_, instance_, theme_, backups, fileName)) {
        return;
    }

    const int confirm = MessageBoxW(
        hwnd_,
        L"将下载该 WebDAV 备份，并把其中的分组、标签、启动项、便签、待办和工具设置合并到当前数据。\n\n当前数据不会被覆盖，导入前会自动备份。",
        L"下载 WebDAV 备份",
        MB_OKCANCEL | MB_ICONINFORMATION);
    if (confirm != IDOK) {
        return;
    }

    const HWND target = hwnd_;
    const std::filesystem::path appDirectory = appDirectory_;
    const AppConfig config = config_;
    std::thread([target, appDirectory, config, fileName]() {
        WebDavBackupService worker(appDirectory, config);
        auto* result = new WebDavAsyncResult();
        result->download = true;
        result->report = worker.DownloadAndImportMerge(fileName);
        if (!PostMessageW(target, WM_QUATTRO_WEBDAV_DONE, 0, reinterpret_cast<LPARAM>(result))) {
            delete result;
        }
    }).detach();
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
        HideItemTooltip();
    }
    if (previous.hideNotifyIcon != config_.hideNotifyIcon) {
        RemoveTrayIcon();
        InitializeTrayIcon();
    }
    if (previous.mainHotKey != config_.mainHotKey) {
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
    wcscpy_s(data.szTip, kAppDisplayName);
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
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_RESTART_PRIVILEGE, runningAsAdmin_ ? L"以普通用户重启" : L"以管理员身份重启", false, -1, -1, MenuIconShield);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_RESET_LAYOUT, L"重置布局");
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
    HMENU toolMenu = CreatePopupMenu();
    AppendThemeItemsToMenu(themeMenu);
    AppendSystemFunctionItems(systemMenu);
    AppendToolItems(toolMenu);

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
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(toolMenu), L"工具箱", true);
    AppendThemedSeparator(menu);
    AppendUnifiedViewOptionItems(menu);
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_UPDATE, L"更新");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_EXIT, L"关闭退出");
    ActivateWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void MainWindow::ShowLinkMenu(int linkId, POINT screenPoint) {
    HideItemTooltip();
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

void MainWindow::ShowTodoMenu(int todoId, POINT screenPoint) {
    ResetMenuVisuals();
    HMENU menu = CreatePopupMenu();
    const TodoItem* item = FindTodoItem(todoId);
    const bool done = item && !item->completedAt.empty();
    const bool enabled = !item || item->enabled;
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_EDIT_TODO_ITEM, L"编辑待办事项");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_TOGGLE_TODO_DONE, done ? L"标记为未完成" : L"标记为完成");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_TOGGLE_TODO_ENABLED, enabled ? L"禁用待办事项" : L"启用待办事项");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_DELETE_TODO_ITEM, L"删除待办事项");
    AppendThemedSeparator(menu);
    AppendTodoSortItems(menu, FindGroup(currentTagId_));
    AppendThemedSeparator(menu);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_CLEAR_DONE_TODOS, L"清空已完成");
    selectedTodoId_ = todoId;
    menuContextKind_ = HitKind::Todo;
    menuContextId_ = todoId;
    ActivateWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void MainWindow::CreateTooltip() {
    if (tooltip_ || !hwnd_) {
        return;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TooltipWindowProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kTooltipWindowClass;
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    tooltip_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kTooltipWindowClass,
        nullptr,
        WS_POPUP,
        0,
        0,
        0,
        0,
        hwnd_,
        nullptr,
        instance_,
        nullptr);
    if (!tooltip_) {
        return;
    }

    SetWindowPos(tooltip_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    tooltipText_.clear();
    tooltipInfo_ = {};
    ApplyTooltipTheme();
}

void MainWindow::ApplyTooltipTheme() {
    if (!tooltip_) {
        return;
    }
    if (!tooltipFont_) {
        tooltipFont_ = ThemedControls::CreateDialogFont();
    }
    if (tooltipFont_) {
        SendMessageW(tooltip_, WM_SETFONT, reinterpret_cast<WPARAM>(tooltipFont_), TRUE);
    }
    const int paddingX = static_cast<int>(std::max(0.0f, Metric(theme_, L"tooltip", L"paddingX", 8.0f)));
    const int paddingY = static_cast<int>(std::max(0.0f, Metric(theme_, L"tooltip", L"paddingY", 5.0f)));
    const int lineGap = static_cast<int>(std::max(0.0f, Metric(theme_, L"tooltip", L"lineGap", 4.0f)));
    const int radius = static_cast<int>(std::max(0.0f, Metric(theme_, L"tooltip", L"radius", 6.0f)));
    const int borderWidth = static_cast<int>(std::max(0.0f, Metric(theme_, L"tooltip", L"borderWidth", 1.0f)));
    SetPropW(tooltip_, kTooltipBgProp, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(ToColorRef(theme_.color(L"tooltip", L"normal", L"bg")))));
    SetPropW(tooltip_, kTooltipTextProp, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(ToColorRef(theme_.color(L"tooltip", L"normal", L"text")))));
    SetPropW(tooltip_, kTooltipBorderProp, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(ToColorRef(theme_.color(L"tooltip", L"normal", L"border")))));
    SetPropW(tooltip_, kTooltipPaddingXProp, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(paddingX)));
    SetPropW(tooltip_, kTooltipPaddingYProp, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(paddingY)));
    SetPropW(tooltip_, kTooltipLineGapProp, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(lineGap)));
    SetPropW(tooltip_, kTooltipRadiusProp, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(radius)));
    SetPropW(tooltip_, kTooltipBorderWidthProp, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(borderWidth)));
}

void MainWindow::HideItemTooltip() {
    if (!tooltip_) {
        return;
    }
    ShowWindow(tooltip_, SW_HIDE);
    tooltipItemKind_ = HitKind::None;
    tooltipItemId_ = 0;
}

std::wstring MainWindow::LinkTooltipText(const Link& link) const {
    const auto appendLine = [](std::wstring& text, const std::wstring& line) {
        if (line.empty()) {
            return;
        }
        if (!text.empty()) {
            text += L"\r\n";
        }
        text += line;
    };

    const std::wstring name = Trim(link.name);
    const std::wstring remark = Trim(link.remark);
    const SystemFunctionDefinition* systemFunction = SystemFunctionForLink(link);

    std::wstring text;
    if (!name.empty()) {
        appendLine(text, L"名称: " + name);
    }

    if (systemFunction) {
        appendLine(text, L"类型: 系统功能");
        const std::wstring description = !remark.empty() ? remark : Trim(systemFunction->remark);
        if (!description.empty()) {
            appendLine(text, L"说明: " + description);
        }
        if (systemFunction->type == 1) {
            const std::wstring location = ExpandEnvironmentStringsSafe(Trim(systemFunction->target));
            appendLine(text, L"位置: " + location);
        } else if (std::wstring(systemFunction->name) == L"hosts") {
            const std::wstring file = ExpandEnvironmentStringsSafe(Trim(systemFunction->parameter));
            appendLine(text, L"文件: " + file);
        }
    } else {
        std::wstring installLocation = Trim(link.path);
        if (!installLocation.empty()) {
            installLocation = IsUrlLink(link) ? NormalizeUrl(installLocation) : ExpandEnvironmentStringsSafe(installLocation);
        } else if (!Trim(link.workDir).empty()) {
            installLocation = ExpandEnvironmentStringsSafe(Trim(link.workDir));
        }
        if (!installLocation.empty()) {
            appendLine(text, (IsUrlLink(link) ? L"网址: " : L"安装位置: ") + installLocation);
        }
        if (!remark.empty()) {
            appendLine(text, L"备注: " + remark);
        }
    }

    if (IsVolumeMenuIcon(SystemFunctionMenuIconForLink(link))) {
        if (const auto volume = CurrentMasterVolumeText()) {
            appendLine(text, *volume);
        }
    }
    appendLine(text, L"点击次数: " + std::to_wstring(link.runCount));
    return text;
}

std::wstring MainWindow::TodoTooltipText(const TodoItem& item) const {
    const auto appendLine = [](std::wstring& text, const std::wstring& line) {
        if (line.empty()) {
            return;
        }
        if (!text.empty()) {
            text += L"\r\n";
        }
        text += line;
    };

    std::wstring text;
    const std::wstring title = Trim(item.title);
    if (!title.empty()) {
        appendLine(text, L"标题: " + title);
    }

    std::wstring status = L"待办";
    if (!item.enabled) {
        status = L"已禁用";
    } else if (!item.completedAt.empty()) {
        status = L"已完成";
    } else if (IsTodoOverdue(item)) {
        status = L"已逾期";
    }
    appendLine(text, L"状态: " + status);

    const bool recurring = IsRecurringTodoSchedule(item.scheduleKind);
    if (recurring) {
        appendLine(text, L"重复: " + TodoScheduleText(item));
        if (!item.nextDueAt.empty()) {
            appendLine(text, L"下次: " + item.nextDueAt);
        }
        appendLine(text, L"已执行: " + std::to_wstring(std::max(0, item.repeatFinished)) + L" 次");
        if (item.repeatLimit > 0) {
            appendLine(text, L"进度: " + std::to_wstring(std::max(0, item.repeatFinished)) + L"/" + std::to_wstring(item.repeatLimit));
        }
    } else if (item.scheduleKind == TodoScheduleKind::Once && !item.nextDueAt.empty()) {
        appendLine(text, L"时间: " + item.nextDueAt);
    }

    if (!item.completedAt.empty()) {
        appendLine(text, L"完成时间: " + item.completedAt);
    }

    const auto contentLines = LimitedTooltipContentLines(item.content);
    for (std::size_t i = 0; i < contentLines.size(); ++i) {
        appendLine(text, (i == 0 ? L"内容: " : L"      ") + contentLines[i]);
    }

    return text;
}

void MainWindow::UpdateItemTooltip(const HitArea& hit, POINT screenPoint) {
    if (!config_.showTooltip) {
        HideItemTooltip();
        return;
    }

    std::wstring text;
    if (hit.kind == HitKind::Link) {
        Link* link = FindLink(hit.id);
        if (!link) {
            HideItemTooltip();
            return;
        }
        text = LinkTooltipText(*link);
    } else if (hit.kind == HitKind::Todo) {
        TodoItem* item = FindTodoItem(hit.id);
        if (!item) {
            HideItemTooltip();
            return;
        }
        text = TodoTooltipText(*item);
    } else {
        HideItemTooltip();
        return;
    }
    if (text.empty()) {
        HideItemTooltip();
        return;
    }

    if (!tooltip_) {
        CreateTooltip();
    }
    if (!tooltip_) {
        return;
    }

    if (tooltipItemKind_ != hit.kind || tooltipItemId_ != hit.id || tooltipText_ != text) {
        tooltipText_ = text;
        SetWindowTextW(tooltip_, tooltipText_.c_str());
        tooltipItemKind_ = hit.kind;
        tooltipItemId_ = hit.id;
    }

    HDC dc = GetDC(tooltip_);
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HFONT previousFont = reinterpret_cast<HFONT>(SelectObject(dc, font));
    const int lineGap = WindowPropInt(tooltip_, kTooltipLineGapProp, 4);
    const SIZE textSize = MeasureTooltipText(dc, tooltipText_, lineGap);
    SelectObject(dc, previousFont);
    ReleaseDC(tooltip_, dc);

    const int paddingX = WindowPropInt(tooltip_, kTooltipPaddingXProp, 8);
    const int paddingY = WindowPropInt(tooltip_, kTooltipPaddingYProp, 7);
    int width = std::max(80, static_cast<int>(textSize.cx) + paddingX * 2);
    int height = std::max(24, static_cast<int>(textSize.cy) + paddingY * 2);
    int x = screenPoint.x + 14;
    int y = screenPoint.y + 18;

    HMONITOR monitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (GetMonitorInfoW(monitor, &monitorInfo)) {
        if (x + width > monitorInfo.rcWork.right) {
            x = screenPoint.x - width - 14;
        }
        if (y + height > monitorInfo.rcWork.bottom) {
            y = screenPoint.y - height - 18;
        }
        x = std::max(static_cast<int>(monitorInfo.rcWork.left), x);
        y = std::max(static_cast<int>(monitorInfo.rcWork.top), y);
    }

    SetWindowPos(tooltip_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    const int radius = WindowPropInt(tooltip_, kTooltipRadiusProp, 0);
    if (radius > 0) {
        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius * 2, radius * 2);
        if (!region || SetWindowRgn(tooltip_, region, TRUE) == 0) {
            if (region) {
                DeleteObject(region);
            }
        }
    } else {
        SetWindowRgn(tooltip_, nullptr, TRUE);
    }
    ShowWindow(tooltip_, SW_SHOWNA);
    SetWindowPos(tooltip_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(tooltip_, nullptr, TRUE);
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
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_TAG, L"新建普通标签");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_NOTE_TAG, L"新建便签标签页");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_TODO_TAG, L"新建待办事项标签页");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_EDIT_TAG, L"重命名标签");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_DELETE_TAG, L"删除标签");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_MOVE_UP, L"上移");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_MOVE_DOWN, L"下移");
    HMENU moveMenu = CreatePopupMenu();
    AppendGroupTargetMenu(moveMenu, ID_MENU_MOVE_TAG_TO_BASE, menuGroupTargetIds_, tag ? tag->parentGroup : 0);
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(moveMenu), L"移动到", true);
    if (tag && IsTodoItemsTag(*tag)) {
        AppendThemedSeparator(menu);
        AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_TODO_ITEM, L"新增待办事项");
        AppendTodoSortItems(menu, tag);
        AppendThemedMenuItem(menu, MF_STRING, ID_MENU_CLEAR_DONE_TODOS, L"清空已完成");
    } else if (!tag || !IsNoteTag(*tag)) {
        AppendThemedSeparator(menu);
        AppendAddLinkItems(menu);
        AppendThemedSeparator(menu);
        AppendViewOptionItems(menu, tag);
        AppendThemedSeparator(menu);
        AppendThemedMenuItem(menu, MF_STRING, ID_MENU_REFRESH_PAGE_ICONS, L"刷新本页图标");
        AppendThemedMenuItem(menu, MF_STRING, ID_MENU_CLEAR_TAG_LINKS, L"清空本页应用");
    }
    menuContextKind_ = HitKind::Tag;
    menuContextId_ = tagId;
    ActivateWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void MainWindow::ShowTagBlankMenu(POINT screenPoint) {
    ResetMenuVisuals();
    HMENU menu = CreatePopupMenu();
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_TAG, L"新建普通标签");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_NOTE_TAG, L"新建便签标签页");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_TODO_TAG, L"新建待办事项标签页");
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
    Group* tag = FindGroup(currentTagId_);
    if (tag && IsTodoItemsTag(*tag)) {
        AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_TODO_ITEM, L"新增待办事项");
        AppendTodoSortItems(menu, tag);
        AppendThemedMenuItem(menu, MF_STRING, ID_MENU_CLEAR_DONE_TODOS, L"清空已完成");
    } else if (!tag || !IsNoteTag(*tag)) {
        AppendAddLinkItems(menu);
        AppendThemedSeparator(menu);
        AppendViewOptionItems(menu, tag);
        AppendThemedSeparator(menu);
        AppendThemedMenuItem(menu, MF_STRING, ID_MENU_REFRESH_PAGE_ICONS, L"刷新本页图标");
        AppendThemedMenuItem(menu, MF_STRING, ID_MENU_CLEAR_TAG_LINKS, L"清空本页应用");
    }
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
        AppendThemedMenuItem(menu, flags, ID_MENU_THEME_BASE + static_cast<UINT>(i), themeItems[i].label, false, -1, -1, MenuIconTheme, true);
    }
}

void MainWindow::AppendAddLinkItems(HMENU menu) {
    HMENU systemMenu = CreatePopupMenu();
    AppendSystemFunctionItems(systemMenu, ID_MENU_ADD_SYSTEM_FUNCTION_BASE);
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_FILE, L"添加文件");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_FOLDER, L"添加文件夹");
    AppendThemedMenuItem(menu, MF_STRING, ID_MENU_ADD_URL, L"添加网址");
    AppendThemedMenuItem(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(systemMenu), L"添加系统功能", true, -1, -1, MenuIconSystem);
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

void MainWindow::AppendTodoSortItems(HMENU menu, const Group* tag) {
    HMENU sortMenu = CreatePopupMenu();
    const int sort = tag ? tag->sort : 0;
    AppendThemedMenuItem(sortMenu, MF_STRING | (sort == 0 ? MF_CHECKED : 0), ID_MENU_TODO_SORT_DUE, L"按提醒时间（推荐）");
    AppendThemedMenuItem(sortMenu, MF_STRING | (sort == 1 ? MF_CHECKED : 0), ID_MENU_TODO_SORT_CREATED, L"按创建时间");
    AppendThemedMenuItem(sortMenu, MF_STRING | (sort == 2 ? MF_CHECKED : 0), ID_MENU_TODO_SORT_TITLE, L"按标题");
    AppendThemedMenuItem(sortMenu, MF_STRING | (sort == 3 ? MF_CHECKED : 0), ID_MENU_TODO_SORT_STATUS, L"按完成状态");
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

void MainWindow::AppendSystemFunctionItems(HMENU menu, UINT commandBase) {
    const auto functions = SystemFunctions();
    const std::size_t limit = commandBase == ID_MENU_ADD_SYSTEM_FUNCTION_BASE
        ? ID_MENU_ADD_SYSTEM_FUNCTION_LIMIT
        : ID_MENU_SYSTEM_FUNCTION_LIMIT;
    const std::size_t count = std::min<std::size_t>(functions.size(), limit);
    const std::size_t columnBreak = count > 18 ? (count + 1) / 2 : count + 1;
    for (std::size_t i = 0; i < count; ++i) {
        UINT flags = MF_STRING;
        if (i == columnBreak) {
            flags |= MF_MENUBARBREAK;
        }
        AppendThemedMenuItem(
            menu,
            flags,
            commandBase + static_cast<UINT>(i),
            functions[i].name,
            false,
            functions[i].menuIcon != MenuIconNone ? -1 : SystemFunctionImageIndex(functions[i]),
            functions[i].stockIcon,
            functions[i].menuIcon);
    }
    if (count == 0) {
        AppendThemedMenuItem(menu, MF_STRING | MF_GRAYED, commandBase, L"无可用功能");
    }
}

void MainWindow::AppendToolItems(HMENU menu) {
    menuToolEngines_.clear();
    menuToolEnabled_.clear();
    const auto plugins = BuiltinToolMenuPlugins(pluginRegistry_.LoadPlugins());
    for (const auto& plugin : plugins) {
        if (plugin.kind != L"builtin-tool" || plugin.engine.empty()) {
            continue;
        }
        if (menuToolEngines_.size() >= ID_MENU_TOOL_LIMIT) {
            break;
        }
        const UINT command = ID_MENU_TOOL_BASE + static_cast<UINT>(menuToolEngines_.size());
        menuToolEngines_.push_back(plugin.engine);
        menuToolEnabled_.push_back(plugin.enabled);
        AppendThemedMenuItem(menu, MF_STRING | (plugin.enabled ? 0 : MF_GRAYED), command, plugin.name, false, -1, -1, MenuIconTools);
    }
    if (menuToolEngines_.empty()) {
        AppendThemedMenuItem(menu, MF_STRING | MF_GRAYED, ID_MENU_TOOL_BASE, L"无可用工具");
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
    HideItemTooltip();
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

    const float buttonSize = ClampFloat(theme_.metric(L"titleButton", L"size", 26.0f), 18.0f, 40.0f);
    const float buttonGap = ClampFloat(theme_.metric(L"titleButton", L"gap", 2.0f), 0.0f, 12.0f);
    const float buttonRightInset = Metric(theme_, L"titleButton", L"rightInset", 4.0f);
    const float buttonTopInset = Metric(theme_, L"titleButton", L"topInset", 4.0f);
    const float buttonReserveInset = Metric(theme_, L"titleButton", L"reserveInset", 16.0f);
    const std::array<HitKind, 3> buttons = {
        HitKind::CloseButton,
        HitKind::SkinButton,
        HitKind::MenuButton,
    };
    auto isTitleButtonVisible = [&](HitKind kind) {
        return !((kind == HitKind::MenuButton && !config_.showMenuButton) ||
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
    const float titleTextLeft = appIcon.right + Metric(theme_, L"title", L"textGap", 7.0f);
    const float titleTextMaxEnd = rect.right - buttonReserve - Metric(theme_, L"title", L"textGap", 7.0f);
    // Let the title use all the space up to the buttons so it is never clipped.
    const float titleTextEnd = std::max(titleTextLeft, titleTextMaxEnd);
    D2D1_RECT_F nameRect = D2D1::RectF(titleTextLeft, rect.top, titleTextEnd, rect.bottom);
    titleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    DrawTextBlock(kAppDisplayName, titleFormat_, nameRect, theme_.color(L"title", L"normal", L"text"));

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
    DrawTabGroupFrame(rect);

    renderTarget_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float groupPadding = Metric(theme_, L"tabButton", L"groupPadding", 3.0f);
    const float itemOffsetX = groupPadding;
    if (vertical) {
        const float topInset = groupPadding;
        const float itemHeight = Metric(theme_, L"tabButton", L"height", Metric(theme_, L"majorNavItem", L"verticalHeight", 32.0f));
        const float itemGap = Metric(theme_, L"tabButton", L"groupGap", 0.0f);
        const float leftInset = groupPadding;
        float y = rect.top + topInset - groupScrollOffset_;
        bool firstVisible = true;
        for (const auto& group : MajorGroups()) {
            D2D1_RECT_F item = D2D1::RectF(rect.left + leftInset, y, rect.right - groupPadding, y + itemHeight);
            if (item.bottom < rect.top + topInset) {
                y += itemHeight + itemGap;
                continue;
            }
            if (item.top > rect.bottom - 2.0f) {
                break;
            }
            if (!firstVisible) {
                DrawTabGroupSeparator(D2D1::RectF(rect.left, item.top, rect.right, item.top), false);
            }
            const bool selected = group.id == currentGroupId_;
            const bool hovered = IsHover(HitKind::Group, group.id);
            DrawTabGroupItem(item, group.name, selected, hovered, textFormat_);
            hitAreas_.push_back(HitArea{HitKind::Group, group.id, IntersectRectF(item, rect)});
            y += itemHeight + itemGap;
            firstVisible = false;
        }
        renderTarget_->PopAxisAlignedClip();
        DrawScrollBar(rect, groupScrollOffset_, MaxGroupScrollOffset(rect), false);
        return;
    }

    float x = rect.left + itemOffsetX - groupScrollOffset_;
    const float itemHeight = Metric(theme_, L"tabButton", L"height", 30.0f);
    const float y = rect.top + std::max(0.0f, (Height(rect) - itemHeight) * 0.5f);
    const float itemGap = Metric(theme_, L"tabButton", L"groupGap", 0.0f);
    bool firstVisible = true;
    for (const auto& group : MajorGroups()) {
        const float itemWidth = TabGroupItemWidth(theme_, group.name, textFormat_, MeasureTextWidth(group.name, textFormat_));
        D2D1_RECT_F item = D2D1::RectF(x, y, x + itemWidth, y + itemHeight);
        if (item.right < rect.left + 2.0f) {
            x += itemWidth;
            continue;
        }
        if (item.left > rect.right - 2.0f) {
            break;
        }
        if (!firstVisible) {
            DrawTabGroupSeparator(D2D1::RectF(item.left, rect.top, item.left, rect.bottom), true);
        }
        const bool selected = group.id == currentGroupId_;
        const bool hovered = IsHover(HitKind::Group, group.id);
        DrawTabGroupItem(item, group.name, selected, hovered, textFormat_);
        hitAreas_.push_back(HitArea{HitKind::Group, group.id, IntersectRectF(item, rect)});
        x += itemWidth + itemGap;
        firstVisible = false;
    }
    renderTarget_->PopAxisAlignedClip();
}

void MainWindow::DrawTags(D2D1_RECT_F rect) {
    if (Height(rect) <= 0 || Width(rect) <= 0) {
        return;
    }

    FillRect(rect, theme_.color(L"minorNav", L"normal", L"bg"));
    DrawTabGroupFrame(rect);

    if (config_.tagAlign == L"right") {
        textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    } else if (config_.tagAlign == L"center") {
        textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    renderTarget_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float groupPadding = Metric(theme_, L"tabButton", L"groupPadding", 3.0f);
    const float topInset = groupPadding;
    const float itemHeight = Metric(theme_, L"tabButton", L"height", Metric(theme_, L"minorNavItem", L"height", 32.0f));
    const float itemGap = Metric(theme_, L"tabButton", L"groupGap", 0.0f);
    float y = rect.top + topInset - tagScrollOffset_;
    bool firstVisible = true;
    for (const auto& tag : TagsForCurrentGroup()) {
        D2D1_RECT_F item = D2D1::RectF(rect.left + groupPadding, y, rect.right - groupPadding, y + itemHeight);
        if (item.bottom < rect.top + topInset) {
            y += itemHeight + itemGap;
            continue;
        }
        if (item.top > rect.bottom - 2.0f) {
            break;
        }
        if (!firstVisible) {
            DrawTabGroupSeparator(D2D1::RectF(rect.left, item.top, rect.right, item.top), false);
        }
        const bool selected = tag.id == currentTagId_;
        const bool hovered = IsHover(HitKind::Tag, tag.id);
        DrawTabGroupItem(item, tag.name, selected, hovered, textFormat_);
        hitAreas_.push_back(HitArea{HitKind::Tag, tag.id, IntersectRectF(item, rect)});
        y += itemHeight + itemGap;
        firstVisible = false;
    }
    renderTarget_->PopAxisAlignedClip();
    DrawScrollBar(rect, tagScrollOffset_, MaxTagScrollOffset(rect), false);
}

void MainWindow::DrawTabGroupFrame(D2D1_RECT_F rect) {
    if (Width(rect) <= 0.0f || Height(rect) <= 0.0f) {
        return;
    }
    const float radius = Metric(theme_, L"tabButton", L"groupRadius", 10.0f);
    const float borderWidth = Metric(theme_, L"tabButton", L"groupBorderWidth", 1.0f);
    if (borderWidth <= 0.0f) {
        FillRect(rect, theme_.color(L"tabButton", L"normal", L"groupBg"));
        return;
    }
    const D2D1_RECT_F frame = Inset(rect, borderWidth * 0.5f, borderWidth * 0.5f);
    FillRoundedRect(frame, theme_.color(L"tabButton", L"normal", L"groupBg"), radius);
    DrawRoundedRect(frame, theme_.color(L"tabButton", L"normal", L"groupBorder"), radius, borderWidth);
}

void MainWindow::DrawTabGroupItem(D2D1_RECT_F rect, const std::wstring& text, bool selected, bool hovered, IDWriteTextFormat* format) {
    const wchar_t* state = selected ? (hovered ? L"selectedHover" : L"selected") : (hovered ? L"hover" : L"normal");
    const float inset = theme_.metric(L"tabButton", L"segmented", 0.0f) > 0.5f
        ? Metric(theme_, L"tabButton", L"segmentInset", 2.0f)
        : 0.0f;
    const D2D1_RECT_F segment = Inset(rect, inset, inset);
    const float radius = Metric(theme_, L"tabButton", L"radius", 8.0f);
    const float borderWidth = Metric(theme_, L"tabButton", L"borderWidth", 1.0f);
    FillRoundedRect(segment, theme_.color(L"tabButton", state, L"bg"), radius);
    DrawRoundedRect(segment, theme_.color(L"tabButton", state, L"border"), radius, borderWidth);

    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    const float paddingX = Metric(theme_, L"tabButton", L"paddingX", 12.0f);
    DrawTextBlock(text, format, Inset(rect, paddingX, 0.0f), theme_.color(L"tabButton", state, L"text"));
}

void MainWindow::DrawTabGroupSeparator(const D2D1_RECT_F& rect, bool horizontal) {
    const float width = Metric(theme_, L"tabButton", L"groupBorderWidth", 1.0f);
    if (width <= 0.0f) {
        return;
    }
    if (horizontal) {
        const float half = width * 0.5f;
        FillRect(D2D1::RectF(rect.left - half, rect.top, rect.left + half, rect.bottom), theme_.color(L"tabButton", L"normal", L"groupBorder"));
    } else {
        const float half = width * 0.5f;
        FillRect(D2D1::RectF(rect.left, rect.top - half, rect.right, rect.top + half), theme_.color(L"tabButton", L"normal", L"groupBorder"));
    }
}

void MainWindow::DrawLinks(D2D1_RECT_F rect) {
    FillRect(rect, theme_.color(L"content", L"normal", L"bg"));
    if (dragOver_) {
        Color highlight = theme_.color(L"content", L"empty", L"text");
        highlight.a = 0.14f;
        FillRect(rect, highlight);
        Color borderColor = theme_.color(L"content", L"empty", L"text");
        borderColor.a = 0.38f;
        DrawRect(D2D1::RectF(rect.left + 1.0f, rect.top + 1.0f, rect.right - 1.0f, rect.bottom - 1.0f), borderColor, 2.0f);
    }
    const Group* currentTag = FindGroup(currentTagId_);
    if (currentTag && IsNoteTag(*currentTag)) {
        DrawNotePage(rect, *currentTag);
        return;
    }
    if (currentTag && IsTodoItemsTag(*currentTag)) {
        HideNoteEdit();
        DrawTodoItems(rect, *currentTag);
        return;
    }
    HideNoteEdit();
    auto links = LinksForCurrentTag();
    if (links.empty()) {
        textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        const float emptyInsetX = Metric(theme_, L"content", L"emptyTextInsetX", 20.0f);
        const float emptyTop = Metric(theme_, L"content", L"emptyTextTop", 24.0f);
        const float emptyHeight = Metric(theme_, L"content", L"emptyTextHeight", 30.0f);
        D2D1_RECT_F emptyText = D2D1::RectF(rect.left + emptyInsetX, rect.top + emptyTop, rect.right - emptyInsetX, rect.top + emptyTop + emptyHeight);
        DrawTextBlock(L"当前标签没有启动项", textFormat_, emptyText, theme_.color(L"content", L"empty", L"text"));
        D2D1_RECT_F hintText = D2D1::RectF(rect.left + emptyInsetX, emptyText.bottom + 6.0f, rect.right - emptyInsetX, emptyText.bottom + 6.0f + emptyHeight);
        DrawTextBlock(L"拖入文件、文件夹或网址即可添加", textFormat_, hintText, theme_.color(L"content", L"empty", L"text"));
        DrawEmptyAddButton(rect, hintText.bottom + 10.0f, L"添加启动项");
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
            if (selectionByKeyboard_) {
                DrawRoundedRect(
                    Inset(item, 1.5f, 1.5f),
                    theme_.color(L"linkItem", L"selected", L"accent"),
                    Metric(theme_, L"list", L"radius", 7.0f),
                    1.0f);
            }
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
        hitAreas_.push_back(HitArea{HitKind::Link, link->id, IntersectRectF(item, rect)});
    }
    renderTarget_->PopAxisAlignedClip();
    DrawScrollBar(rect, linkScrollOffset_, MaxLinkScrollOffset(rect), false);
}

void MainWindow::DrawEmptyAddButton(const D2D1_RECT_F& contentRect, float topY, const std::wstring& label) {
    const bool hovered = IsHover(HitKind::AddButton, 0);
    const float radius = Metric(theme_, L"button", L"radius", 7.0f);
    const float height = Metric(theme_, L"button", L"height", 30.0f);
    const float paddingX = Metric(theme_, L"button", L"paddingX", 12.0f);
    const float plusGap = 8.0f;
    const float plusHalf = 5.0f;

    const float textWidth = MeasureTextWidth(label, textFormat_);
    const float buttonWidth = std::min(Width(contentRect) - 32.0f,
                                       paddingX * 2.0f + plusHalf * 2.0f + plusGap + textWidth + 6.0f);
    const float centerX = (contentRect.left + contentRect.right) * 0.5f;
    const float top = topY + 14.0f;
    D2D1_RECT_F button = D2D1::RectF(centerX - buttonWidth * 0.5f, top, centerX + buttonWidth * 0.5f, top + height);

    const wchar_t* state = hovered ? L"hover" : L"normal";
    FillRoundedRect(button, theme_.color(L"button", state, L"bg"), radius);
    DrawRoundedRect(button, theme_.color(L"button", state, L"border"), radius, Metric(theme_, L"button", L"borderWidth", 1.0f));

    const Color iconColor = theme_.color(L"button", state, L"text");
    const float contentWidth = plusHalf * 2.0f + plusGap + textWidth;
    const float contentLeft = button.left + (Width(button) - contentWidth) * 0.5f;
    const float cy = (button.top + button.bottom) * 0.5f;
    const float px = contentLeft + plusHalf;
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(iconColor.d2d(), &brush)) && brush) {
        renderTarget_->DrawLine(D2D1::Point2F(px - plusHalf, cy), D2D1::Point2F(px + plusHalf, cy), brush, 1.6f);
        renderTarget_->DrawLine(D2D1::Point2F(px, cy - plusHalf), D2D1::Point2F(px, cy + plusHalf), brush, 1.6f);
        brush->Release();
    }

    D2D1_RECT_F textRect = D2D1::RectF(px + plusHalf + plusGap, button.top, button.right - paddingX, button.bottom);
    textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    DrawTextBlock(label, textFormat_, textRect, theme_.color(L"button", state, L"text"));
    textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    hitAreas_.push_back(HitArea{HitKind::AddButton, 0, button});
}

void MainWindow::DrawNotePage(D2D1_RECT_F rect, const Group& tag) {
    const float insetX = Metric(theme_, L"linkItem", L"viewportPaddingX", 16.0f);
    const float insetY = Metric(theme_, L"list", L"paddingY", 6.0f);
    const D2D1_RECT_F frame = D2D1::RectF(
        rect.left + insetX,
        rect.top + insetY,
        rect.right - insetX,
        rect.bottom - insetY);
    FillRoundedRect(frame, theme_.color(L"edit", L"normal", L"bg"), Metric(theme_, L"edit", L"radius", 7.0f));
    DrawRoundedRect(
        frame,
        theme_.color(L"edit", noteEdit_ == GetFocus() ? L"focused" : L"normal", L"border"),
        Metric(theme_, L"edit", L"radius", 7.0f),
        Metric(theme_, L"edit", L"borderWidth", 1.0f));
    EnsureNoteEdit(frame, tag);
}

void MainWindow::DrawTodoItems(D2D1_RECT_F rect, const Group&) {
    auto items = TodosForCurrentTag();
    if (items.empty()) {
        textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        const float emptyInsetX = Metric(theme_, L"content", L"emptyTextInsetX", 20.0f);
        const float emptyTop = Metric(theme_, L"content", L"emptyTextTop", 24.0f);
        const float emptyHeight = Metric(theme_, L"content", L"emptyTextHeight", 30.0f);
        D2D1_RECT_F emptyText = D2D1::RectF(rect.left + emptyInsetX, rect.top + emptyTop, rect.right - emptyInsetX, rect.top + emptyTop + emptyHeight);
        DrawTextBlock(L"当前标签没有待办事项", textFormat_, emptyText, theme_.color(L"content", L"empty", L"text"));
        DrawEmptyAddButton(rect, emptyText.bottom, L"添加待办事项");
        return;
    }

    const float paddingX = Metric(theme_, L"linkItem", L"viewportPaddingX", 16.0f);
    const float paddingY = Metric(theme_, L"list", L"paddingY", 6.0f);
    const float rowHeight = std::max(64.0f, Metric(theme_, L"listItem", L"height", 28.0f) + 36.0f);
    const float rowGap = std::max(4.0f, Metric(theme_, L"linkItem", L"listGapY", 2.0f));
    const float contentInsetX = std::max(14.0f, Metric(theme_, L"listItem", L"paddingX", 8.0f));
    const float dotRadius = 4.0f;
    const float dotRightInset = 16.0f;
    const float tagHeight = 20.0f;
    const float tagGap = 6.0f;

    renderTarget_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float y = rect.top + paddingY - linkScrollOffset_;
    for (TodoItem* itemPtr : items) {
        TodoItem& item = *itemPtr;
        D2D1_RECT_F row = D2D1::RectF(rect.left + paddingX, y, rect.right - paddingX, y + rowHeight);
        y += rowHeight + rowGap;
        if (row.bottom < rect.top + 2.0f) {
            continue;
        }
        if (row.top > rect.bottom - 2.0f) {
            break;
        }

        const bool selected = item.id == selectedTodoId_;
        const bool hovered = IsHover(HitKind::Todo, item.id);
        const bool done = !item.completedAt.empty();
        const bool disabled = !item.enabled;
        const bool overdue = IsTodoOverdue(item);
        const bool recurring = IsRecurringTodoSchedule(item.scheduleKind);
        const Color surface = theme_.color(L"panel", L"normal", L"bg");
        const Color textColor = theme_.color(L"text", L"normal", L"text");
        TodoVisualStyle style{};
        if (disabled) {
            style = MakeTodoStyle(theme_.color(L"text", L"disabled", L"text"), surface, textColor);
        } else if (done) {
            style = MakeTodoStyle(theme_.color(L"global", L"success", L"text"), surface, textColor);
        } else if (overdue) {
            style = MakeTodoStyle(theme_.color(L"global", L"danger", L"text"), surface, textColor);
        } else if (recurring) {
            style = MakeTodoStyle(theme_.color(L"global", L"success", L"text"), surface, textColor);
        } else if (item.scheduleKind == TodoScheduleKind::Once || !item.nextDueAt.empty()) {
            style = MakeTodoStyle(theme_.color(L"global", L"info", L"text"), surface, textColor);
        } else {
            style = MakeTodoStyle(theme_.color(L"text", L"muted", L"text"), surface, textColor);
        }

        const float radius = Metric(theme_, L"list", L"radius", 7.0f);
        FillRoundedRect(row, style.bg, radius);
        DrawRoundedRect(row, selected ? theme_.color(L"linkItem", L"selected", L"accent") : style.border, radius, selected ? 1.4f : 1.0f);
        if (selected && selectionByKeyboard_) {
            DrawRoundedRect(Inset(row, 2.0f, 2.0f), theme_.color(L"linkItem", L"selected", L"accent"), std::max(0.0f, radius - 1.0f), 1.0f);
        }
        if (hovered && !selected) {
            DrawRoundedRect(Inset(row, 1.0f, 1.0f), theme_.color(L"listItem", L"hover", L"bg"), std::max(0.0f, radius - 1.0f), 1.0f);
        }

        const float dotCx = row.right - dotRightInset;
        const float titleRight = dotCx - dotRadius - 12.0f;
        FillEllipse(dotCx, row.top + 22.0f, dotRadius, style.dot);

        D2D1_RECT_F titleRect = D2D1::RectF(row.left + contentInsetX, row.top + 9.0f, titleRight, row.top + 30.0f);
        textFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        textFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        smallFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        DrawTextBlock(item.title, textFormat_, titleRect, style.title);

        std::vector<std::wstring> tags;
        if (disabled) {
            tags.push_back(L"已禁用");
        } else if (done) {
            tags.push_back(L"已完成");
        } else if (overdue) {
            tags.push_back(L"已逾期");
        }
        if (recurring) {
            tags.push_back(L"重复");
        } else if (item.scheduleKind == TodoScheduleKind::Once || !item.nextDueAt.empty()) {
            tags.push_back(L"一次性");
        } else {
            tags.push_back(L"无时间");
        }
        if (!item.nextDueAt.empty()) {
            tags.push_back(item.nextDueAt);
        }

        float tagX = row.left + contentInsetX;
        const float tagY = row.top + 36.0f;
        const float tagRight = row.right - contentInsetX;
        for (const auto& tagText : tags) {
            const float desiredWidth = std::min(150.0f, std::max(34.0f, MeasureTextWidth(tagText, smallFormat_) + 16.0f));
            const float availableWidth = tagRight - tagX;
            if (availableWidth < 34.0f) {
                break;
            }
            const float tagWidth = std::min(desiredWidth, availableWidth);
            const D2D1_RECT_F tagRect = D2D1::RectF(tagX, tagY, tagX + tagWidth, tagY + tagHeight);
            FillRoundedRect(tagRect, style.tagBg, 4.0f);
            DrawTextBlock(tagText, smallFormat_, Inset(tagRect, 8.0f, 1.0f), style.tagText);
            tagX += tagWidth + tagGap;
        }
        hitAreas_.push_back(HitArea{HitKind::Todo, item.id, IntersectRectF(row, rect)});
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
    if (kind == HitKind::CloseButton) {
        renderTarget_->DrawLine(D2D1::Point2F(cx - iconHalf, cy - iconHalf), D2D1::Point2F(cx + iconHalf, cy + iconHalf), brush, stroke);
        renderTarget_->DrawLine(D2D1::Point2F(cx + iconHalf, cy - iconHalf), D2D1::Point2F(cx - iconHalf, cy + iconHalf), brush, stroke);
    } else if (kind == HitKind::MenuButton) {
        renderTarget_->DrawLine(D2D1::Point2F(cx - menuHalfWidth, cy - menuLineGap), D2D1::Point2F(cx + menuHalfWidth, cy - menuLineGap), brush, stroke);
        renderTarget_->DrawLine(D2D1::Point2F(cx - menuHalfWidth, cy), D2D1::Point2F(cx + menuHalfWidth, cy), brush, stroke);
        renderTarget_->DrawLine(D2D1::Point2F(cx - menuHalfWidth, cy + menuLineGap), D2D1::Point2F(cx + menuHalfWidth, cy + menuLineGap), brush, stroke);
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

void MainWindow::FillEllipse(float cx, float cy, float radius, const Color& color) {
    if (radius <= 0.0f) {
        return;
    }
    ID2D1SolidColorBrush* brush = nullptr;
    if (SUCCEEDED(renderTarget_->CreateSolidColorBrush(color.d2d(), &brush)) && brush) {
        renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush);
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

float MainWindow::MeasureTextWidth(const std::wstring& text, IDWriteTextFormat* format, float maxWidth) const {
    if (text.empty() || !format || !dwriteFactory_) {
        return 0.0f;
    }
    IDWriteTextLayout* layout = nullptr;
    float width = 0.0f;
    if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
            text.c_str(),
            static_cast<UINT32>(text.size()),
            format,
            std::max(1.0f, maxWidth),
            32.0f,
            &layout)) &&
        layout) {
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(layout->GetMetrics(&metrics))) {
            width = metrics.widthIncludingTrailingWhitespace;
        }
        layout->Release();
    }
    return width;
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

void MainWindow::AppendThemedMenuItem(HMENU menu, UINT flags, UINT_PTR id, const std::wstring& text, bool submenu, int systemImageIndex, int stockIcon, int menuIcon, bool checkedIconAccent) {
    auto item = std::make_unique<MenuItemData>();
    item->text = MenuTextFromRaw(text);
    item->icon = menuIcon != MenuIconNone ? menuIcon : MenuIconFor(id, item->text);
    item->systemImageIndex = systemImageIndex;
    item->stockIcon = stockIcon;
    item->checked = (flags & MF_CHECKED) != 0;
    item->disabled = (flags & (MF_DISABLED | MF_GRAYED)) != 0;
    item->submenu = submenu || ((flags & MF_POPUP) != 0);
    item->checkedIconAccent = checkedIconAccent;
    MenuItemData* raw = item.get();
    activeMenuItems_.push_back(std::move(item));
    AppendMenuW(menu, (flags | MF_OWNERDRAW) & ~MF_STRING, id, reinterpret_cast<LPCWSTR>(raw));
}

void MainWindow::AppendThemedSeparator(HMENU menu) {
    auto item = std::make_unique<MenuItemData>();
    item->separator = true;
    MenuItemData* raw = item.get();
    activeMenuItems_.push_back(std::move(item));
    AppendMenuW(menu, MF_SEPARATOR | MF_OWNERDRAW, 0, reinterpret_cast<LPCWSTR>(raw));
}

bool MainWindow::MeasureThemedMenuItem(MEASUREITEMSTRUCT* measure) {
    if (!measure || measure->CtlType != ODT_MENU || measure->itemData == 0) {
        return false;
    }
    const auto* item = reinterpret_cast<const MenuItemData*>(measure->itemData);
    if (item->separator) {
        const int thickness = static_cast<int>(std::max(1.0f, Metric(theme_, L"separator", L"thickness", 1.0f)));
        measure->itemHeight = static_cast<UINT>(std::max(5, thickness + 8));
        measure->itemWidth = static_cast<UINT>(Metric(theme_, L"menuItem", L"widthBase", 54.0f) + Metric(theme_, L"menuItem", L"minTextWidth", 64.0f));
        return true;
    }
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
    const Color background = theme_.color(L"menu", L"normal", L"bg");

    if (item->separator) {
        HBRUSH backgroundBrush = CreateSolidBrush(ToColorRef(background));
        ::FillRect(dc, &rc, backgroundBrush);
        DeleteObject(backgroundBrush);

        const int inset = static_cast<int>(std::max(0.0f, Metric(theme_, L"separator", L"inset", 0.0f)));
        const int thickness = static_cast<int>(std::max(1.0f, Metric(theme_, L"separator", L"thickness", 1.0f)));
        RECT lineRect{
            rc.left + inset,
            rc.top + ((rc.bottom - rc.top) - thickness) / 2,
            rc.right - inset,
            rc.top + ((rc.bottom - rc.top) - thickness) / 2 + thickness};
        HBRUSH lineBrush = CreateSolidBrush(ToColorRef(theme_.color(L"separator", L"normal", L"line")));
        ::FillRect(dc, &lineRect, lineBrush);
        DeleteObject(lineBrush);
        return true;
    }

    const bool selected = (draw->itemState & ODS_SELECTED) != 0;

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
    const COLORREF iconColorRef = item->checkedIconAccent
        ? ToColorRef(theme_.color(L"menuItem", item->checked ? L"checked" : (item->disabled ? L"disabled" : L"normal"), L"icon"))
        : CLR_INVALID;
    MenuIconPalette iconPalette;
    iconPalette.accent = ToColorRef(theme_.color(L"menuItem", L"accent", L"icon"));
    iconPalette.danger = ToColorRef(theme_.color(L"menuItem", L"danger", L"icon"));
    iconPalette.warning = ToColorRef(theme_.color(L"menuItem", L"warning", L"icon"));
    iconPalette.success = ToColorRef(theme_.color(L"menuItem", L"success", L"icon"));
    iconPalette.muted = ToColorRef(theme_.color(L"menuItem", L"normal", L"icon"));
    iconPalette.disabled = ToColorRef(theme_.color(L"menuItem", L"disabled", L"icon"));
    iconPalette.neutral = ToColorRef(theme_.color(L"menu", L"normal", L"bg"));
    if (item->stockIcon >= 0) {
        if (!DrawStockMenuIcon(dc, iconRect, static_cast<SHSTOCKICONID>(item->stockIcon)) &&
            !DrawSystemImageListIcon(dc, iconRect, item->systemImageIndex, item->disabled)) {
            DrawMenuIcon(dc, iconRect, item->icon, item->disabled, iconColorRef, iconPalette, appDirectory_);
        }
    } else if (item->systemImageIndex >= 0) {
        if (!DrawSystemImageListIcon(dc, iconRect, item->systemImageIndex, item->disabled)) {
            DrawMenuIcon(dc, iconRect, item->icon, item->disabled, iconColorRef, iconPalette, appDirectory_);
        }
    } else {
        DrawMenuIcon(dc, iconRect, item->icon, item->disabled, iconColorRef, iconPalette, appDirectory_);
    }

    if (item->checked) {
        if (item->checkedIconAccent) {
            HPEN checkPen = CreatePen(PS_SOLID, static_cast<int>(Metric(theme_, L"menuItem", L"checkMarkWidth", 2.0f)), ToColorRef(theme_.color(L"menuItem", L"checked", L"mark")));
            HGDIOBJ oldPen = SelectObject(dc, checkPen);
            const int dotCenterY = (rc.top + rc.bottom) / 2;
            const int checkRight = rc.right - static_cast<int>(Metric(theme_, L"menuItem", L"checkRight", 10.0f));
            const int checkWidth = static_cast<int>(Metric(theme_, L"menuItem", L"checkWidth", 9.0f));
            const int checkHeight = static_cast<int>(Metric(theme_, L"menuItem", L"checkHeight", 7.0f));
            MoveToEx(dc, checkRight - checkWidth, dotCenterY - 1, nullptr);
            LineTo(dc, checkRight - checkWidth / 2 - 1, dotCenterY + checkHeight / 2);
            LineTo(dc, checkRight, dotCenterY - checkHeight / 2);
            SelectObject(dc, oldPen);
            DeleteObject(checkPen);
        } else {
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
    }

    RECT textRect = rc;
    textRect.left += static_cast<int>(Metric(theme_, L"menuItem", L"textLeft", 34.0f));
    textRect.right -= item->submenu ? static_cast<int>(Metric(theme_, L"menuItem", L"submenuRight", 22.0f)) : static_cast<int>(Metric(theme_, L"menuItem", item->checked && item->checkedIconAccent ? L"checkedTextRight" : L"textRight", item->checked && item->checkedIconAccent ? 28.0f : 8.0f));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ToColorRef(theme_.color(L"menuItem", item->disabled ? L"disabled" : L"normal", L"text")));
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HGDIOBJ oldFont = SelectObject(dc, font);
    DrawTextW(dc, item->text.c_str(), static_cast<int>(item->text.size()), &textRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);

    if (item->submenu) {
        const COLORREF arrowColor = ToColorRef(theme_.color(L"menuItem", item->disabled ? L"disabled" : (selected ? L"hover" : L"normal"), L"text"));
        HPEN pen = CreatePen(PS_SOLID, 1, arrowColor);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        const int midY = (rc.top + rc.bottom) / 2;
        const int arrowRight = static_cast<int>(Metric(theme_, L"menuItem", L"arrowRight", 9.0f));
        const int arrowWidth = static_cast<int>(Metric(theme_, L"menuItem", L"arrowWidth", 5.0f));
        const int arrowHalfHeight = static_cast<int>(Metric(theme_, L"menuItem", L"arrowHalfHeight", 4.0f));
        const int arrowTip = rc.right - arrowRight;
        POINT points[] = {
            {arrowTip - arrowWidth, midY - arrowHalfHeight},
            {arrowTip, midY},
            {arrowTip - arrowWidth, midY + arrowHalfHeight},
        };
        Polyline(dc, points, static_cast<int>(std::size(points)));
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
        const float groupPadding = Metric(theme_, L"tabButton", L"groupPadding", 3.0f);
        const float itemHeight = Metric(theme_, L"tabButton", L"height", Metric(theme_, L"majorNavItem", L"verticalHeight", 32.0f));
        const float itemGap = Metric(theme_, L"tabButton", L"groupGap", 0.0f);
        const float topInset = groupPadding;
        const float bottomInset = groupPadding;
        const float contentHeight = topInset + bottomInset + static_cast<float>(groups.size()) * itemHeight
            + static_cast<float>(groups.empty() ? 0 : groups.size() - 1) * itemGap;
        return std::max(0.0f, contentHeight - Height(rect));
    }

    const float groupPadding = Metric(theme_, L"tabButton", L"groupPadding", 3.0f);
    const float itemOffsetX = groupPadding;
    const float contentRightPadding = groupPadding;
    const float itemGap = Metric(theme_, L"tabButton", L"groupGap", 0.0f);
    float contentWidth = itemOffsetX + contentRightPadding;
    const auto groups = MajorGroups();
    for (const auto& group : groups) {
        contentWidth += TabGroupItemWidth(theme_, group.name, textFormat_, MeasureTextWidth(group.name, textFormat_)) + itemGap;
    }
    if (!groups.empty()) {
        contentWidth -= itemGap;
    }
    return std::max(0.0f, contentWidth - Width(rect));
}

float MainWindow::MaxTagScrollOffset(const D2D1_RECT_F& rect) const {
    if (Height(rect) <= 0 || Width(rect) <= 0 || !config_.showTag) {
        return 0.0f;
    }

    const auto tags = TagsForCurrentGroup();
    const float groupPadding = Metric(theme_, L"tabButton", L"groupPadding", 3.0f);
    const float itemHeight = Metric(theme_, L"tabButton", L"height", Metric(theme_, L"minorNavItem", L"height", 32.0f));
    const float itemGap = Metric(theme_, L"tabButton", L"groupGap", 0.0f);
    const float topInset = groupPadding;
    const float bottomInset = groupPadding;
    const float contentHeight = topInset + bottomInset + static_cast<float>(tags.size()) * itemHeight
        + static_cast<float>(tags.empty() ? 0 : tags.size() - 1) * itemGap;
    return std::max(0.0f, contentHeight - Height(rect));
}

float MainWindow::MaxLinkScrollOffset(const D2D1_RECT_F& rect) const {
    if (Height(rect) <= 0 || Width(rect) <= 0) {
        return 0.0f;
    }

    const Group* currentTag = FindGroup(currentTagId_);
    if (currentTag && IsNoteTag(*currentTag)) {
        return 0.0f;
    }
    if (currentTag && IsTodoItemsTag(*currentTag)) {
        return std::max(0.0f, TodoContentHeight(rect) - Height(rect));
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

float MainWindow::TodoContentHeight(const D2D1_RECT_F&) const {
    std::size_t count = 0;
    for (const auto& item : model_.todos) {
        if (item.tagId == currentTagId_) {
            ++count;
        }
    }
    if (count == 0) {
        return 0.0f;
    }
    const float paddingY = Metric(theme_, L"list", L"paddingY", 6.0f);
    const float itemHeight = std::max(64.0f, Metric(theme_, L"listItem", L"height", 28.0f) + 36.0f);
    const float itemGap = std::max(4.0f, Metric(theme_, L"linkItem", L"listGapY", 2.0f));
    return paddingY * 2.0f + static_cast<float>(count) * itemHeight + static_cast<float>(count - 1) * itemGap;
}

void MainWindow::EnsureGroupVisible(int groupId) {
    RECT client{};
    if (!hwnd_ || !GetClientRect(hwnd_, &client)) {
        return;
    }
    D2D1_RECT_F title{}, groupsRect{}, tagsRect{}, linksRect{};
    BuildLayout(static_cast<float>(client.right - client.left), static_cast<float>(client.bottom - client.top), title, groupsRect, tagsRect, linksRect);

    if (Height(groupsRect) > Width(groupsRect)) {
        const float groupPadding = Metric(theme_, L"tabButton", L"groupPadding", 3.0f);
        const float itemHeight = Metric(theme_, L"tabButton", L"height", Metric(theme_, L"majorNavItem", L"verticalHeight", 32.0f));
        const float itemGap = Metric(theme_, L"tabButton", L"groupGap", 0.0f);
        const float topInset = groupPadding;
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

    const float groupPadding = Metric(theme_, L"tabButton", L"groupPadding", 3.0f);
    const float itemOffsetX = groupPadding;
    const float itemGap = Metric(theme_, L"tabButton", L"groupGap", 0.0f);
    const float visibilityPadding = Metric(theme_, L"majorNavItem", L"visibilityPadding", 8.0f);
    float x = groupsRect.left + itemOffsetX;
    for (const auto& group : MajorGroups()) {
        const float width = TabGroupItemWidth(theme_, group.name, textFormat_, MeasureTextWidth(group.name, textFormat_));
        if (group.id == groupId) {
            if (x - groupScrollOffset_ < groupsRect.left + visibilityPadding) {
                groupScrollOffset_ = x - groupsRect.left - visibilityPadding;
            } else if (x + width - groupScrollOffset_ > groupsRect.right - visibilityPadding) {
                groupScrollOffset_ = x + width - groupsRect.right + visibilityPadding;
            }
            groupScrollOffset_ = ClampFloat(groupScrollOffset_, 0.0f, MaxGroupScrollOffset(groupsRect));
            return;
        }
        x += width + itemGap;
    }
}

void MainWindow::EnsureTagVisible(int tagId) {
    RECT client{};
    if (!hwnd_ || !GetClientRect(hwnd_, &client)) {
        return;
    }
    D2D1_RECT_F title{}, groupsRect{}, tagsRect{}, linksRect{};
    BuildLayout(static_cast<float>(client.right - client.left), static_cast<float>(client.bottom - client.top), title, groupsRect, tagsRect, linksRect);
    const float groupPadding = Metric(theme_, L"tabButton", L"groupPadding", 3.0f);
    const float topInset = groupPadding;
    const float itemHeight = Metric(theme_, L"tabButton", L"height", Metric(theme_, L"minorNavItem", L"height", 32.0f));
    const float itemGap = Metric(theme_, L"tabButton", L"groupGap", 0.0f);
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

void MainWindow::EnsureTodoVisible(int todoId) {
    if (todoId <= 0 || !hwnd_) {
        return;
    }
    RECT client{};
    if (!GetClientRect(hwnd_, &client)) {
        return;
    }
    D2D1_RECT_F title{}, groupsRect{}, tagsRect{}, linksRect{};
    BuildLayout(static_cast<float>(client.right - client.left), static_cast<float>(client.bottom - client.top), title, groupsRect, tagsRect, linksRect);
    auto items = TodosForCurrentTag();
    auto it = std::find_if(items.begin(), items.end(), [todoId](const TodoItem* item) {
        return item && item->id == todoId;
    });
    if (it == items.end()) {
        return;
    }

    const int index = static_cast<int>(std::distance(items.begin(), it));
    const float paddingY = Metric(theme_, L"list", L"paddingY", 6.0f);
    const float rowHeight = std::max(64.0f, Metric(theme_, L"listItem", L"height", 28.0f) + 36.0f);
    const float rowGap = std::max(4.0f, Metric(theme_, L"linkItem", L"listGapY", 2.0f));
    const float itemTop = linksRect.top + paddingY + static_cast<float>(index) * (rowHeight + rowGap);
    const float itemBottom = itemTop + rowHeight;

    const float visibilityPadding = Metric(theme_, L"linkItem", L"visibilityPadding", 8.0f);
    if (itemTop - linkScrollOffset_ < linksRect.top + visibilityPadding) {
        linkScrollOffset_ = itemTop - linksRect.top - visibilityPadding;
    } else if (itemBottom - linkScrollOffset_ > linksRect.bottom - visibilityPadding) {
        linkScrollOffset_ = itemBottom - linksRect.bottom + visibilityPadding;
    }
    linkScrollOffset_ = ClampFloat(linkScrollOffset_, 0.0f, MaxLinkScrollOffset(linksRect));
}

MainWindow::HitArea MainWindow::CursorHitArea() const {
    POINT point{};
    if (!GetCursorPos(&point) || !ScreenToClient(hwnd_, &point)) {
        return {};
    }
    return HitTest(static_cast<float>(point.x), static_cast<float>(point.y));
}

void MainWindow::MoveLinkSelection(int dx, int dy) {
    auto links = LinksForCurrentTag();
    if (links.empty()) {
        return;
    }
    RECT client{};
    if (!GetClientRect(hwnd_, &client)) {
        return;
    }
    D2D1_RECT_F title{}, groupsRect{}, tagsRect{}, linksRect{};
    BuildLayout(static_cast<float>(client.right - client.left), static_cast<float>(client.bottom - client.top), title, groupsRect, tagsRect, linksRect);
    const Group* tag = FindGroup(currentTagId_);
    const LinkLayoutMetrics metrics = MakeLinkLayout(theme_, linksRect, tag);
    const int columns = std::max(1, metrics.columns);
    const int count = static_cast<int>(links.size());

    int current = -1;
    for (int i = 0; i < count; ++i) {
        if (links[i]->id == selectedLinkId_) {
            current = i;
            break;
        }
    }
    if (current < 0) {
        selectedLinkId_ = links.front()->id;
        selectionByKeyboard_ = true;
        EnsureLinkVisible(selectedLinkId_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    int next = current;
    if (dx != 0) {
        next = std::max(0, std::min(count - 1, current + dx));
    } else if (dy != 0) {
        next = current + dy * columns;
        if (next < 0 || next >= count) {
            next = current;
        }
    }
    if (next == current) {
        return;
    }
    selectedLinkId_ = links[next]->id;
    selectionByKeyboard_ = true;
    EnsureLinkVisible(selectedLinkId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::MoveTodoSelection(int delta) {
    auto items = TodosForCurrentTag();
    if (items.empty()) {
        return;
    }

    int current = -1;
    const int count = static_cast<int>(items.size());
    for (int i = 0; i < count; ++i) {
        if (items[i]->id == selectedTodoId_) {
            current = i;
            break;
        }
    }
    if (current < 0) {
        selectedTodoId_ = delta < 0 ? items.back()->id : items.front()->id;
        selectionByKeyboard_ = true;
        EnsureTodoVisible(selectedTodoId_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    const int next = std::max(0, std::min(count - 1, current + delta));
    if (next == current) {
        return;
    }
    selectedTodoId_ = items[next]->id;
    selectionByKeyboard_ = true;
    EnsureTodoVisible(selectedTodoId_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SelectAdjacentTag(int direction) {
    const auto tags = TagsForCurrentGroup();
    if (tags.empty()) {
        return;
    }
    int index = -1;
    for (std::size_t i = 0; i < tags.size(); ++i) {
        if (tags[i].id == currentTagId_) {
            index = static_cast<int>(i);
            break;
        }
    }
    int next = index < 0 ? 0 : index + direction;
    next = std::max(0, std::min(static_cast<int>(tags.size()) - 1, next));
    if (next != index) {
        SelectTag(tags[next].id);
    }
}

void MainWindow::SelectAdjacentGroup(int direction) {
    const auto groups = MajorGroups();
    if (groups.empty()) {
        return;
    }
    int index = -1;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (groups[i].id == currentGroupId_) {
            index = static_cast<int>(i);
            break;
        }
    }
    int next = index < 0 ? 0 : index + direction;
    next = std::max(0, std::min(static_cast<int>(groups.size()) - 1, next));
    if (next != index) {
        SelectGroup(groups[next].id);
    }
}

bool MainWindow::HandleKeyDown(WPARAM key) {
    const Group* tag = FindGroup(currentTagId_);
    // Note pages own their edit control; leave keyboard handling to it.
    if (tag && IsNoteTag(*tag) && noteEdit_ && GetFocus() == noteEdit_) {
        return false;
    }

    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool todoTag = tag && IsTodoItemsTag(*tag);

    switch (key) {
    case VK_ESCAPE:
        HideMainWindow();
        return true;
    case VK_TAB:
        if (config_.showTag) {
            SelectAdjacentTag(shift ? -1 : 1);
        } else if (config_.showGroup) {
            SelectAdjacentGroup(shift ? -1 : 1);
        }
        return true;
    case VK_LEFT:
        if (ctrl && config_.showGroup) {
            SelectAdjacentGroup(-1);
        } else if (todoTag && config_.showTag) {
            SelectAdjacentTag(-1);
        } else if (!todoTag) {
            MoveLinkSelection(-1, 0);
        }
        return true;
    case VK_RIGHT:
        if (ctrl && config_.showGroup) {
            SelectAdjacentGroup(1);
        } else if (todoTag && config_.showTag) {
            SelectAdjacentTag(1);
        } else if (!todoTag) {
            MoveLinkSelection(1, 0);
        }
        return true;
    case VK_UP:
        if (ctrl && config_.showTag) {
            SelectAdjacentTag(-1);
        } else if (todoTag) {
            MoveTodoSelection(-1);
        } else if (!todoTag) {
            MoveLinkSelection(0, -1);
        }
        return true;
    case VK_DOWN:
        if (ctrl && config_.showTag) {
            SelectAdjacentTag(1);
        } else if (todoTag) {
            MoveTodoSelection(1);
        } else if (!todoTag) {
            MoveLinkSelection(0, 1);
        }
        return true;
    case VK_INSERT:
        if (todoTag) {
            AddTodoItem();
        } else {
            AddLink();
        }
        return true;
    case VK_RETURN:
        if (todoTag) {
            if (selectedTodoId_ > 0) {
                EditTodoItem(selectedTodoId_);
            }
        } else if (selectedLinkId_ > 0) {
            RunLink(selectedLinkId_);
        }
        return true;
    case VK_DELETE:
        if (todoTag) {
            if (selectedTodoId_ > 0) {
                DeleteTodoItem(selectedTodoId_);
            }
        } else if (selectedLinkId_ > 0) {
            DeleteLink(selectedLinkId_);
        }
        return true;
    case VK_F2:
        if (todoTag) {
            if (selectedTodoId_ > 0) {
                EditTodoItem(selectedTodoId_);
            }
        } else if (selectedLinkId_ > 0) {
            EditLink(selectedLinkId_);
        }
        return true;
    case 'N':
        if (ctrl) {
            if (todoTag) {
                AddTodoItem();
            } else {
                AddLink();
            }
            return true;
        }
        return false;
    case 'V':
        if (ctrl) {
            ImportClipboard();
            return true;
        }
        return false;
    default:
        return false;
    }
}

void MainWindow::SetDragOver(bool active) {
    if (dragOver_ != active) {
        dragOver_ = active;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
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

std::vector<TodoItem*> MainWindow::TodosForCurrentTag() {
    std::vector<TodoItem*> items;
    for (auto& item : model_.todos) {
        if (item.tagId == currentTagId_) {
            items.push_back(&item);
        }
    }
    const Group* tag = FindGroup(currentTagId_);
    const int sortMode = tag ? tag->sort : 0;
    std::sort(items.begin(), items.end(), [sortMode](const TodoItem* left, const TodoItem* right) {
        const bool leftDone = !left->completedAt.empty();
        const bool rightDone = !right->completedAt.empty();
        if (leftDone != rightDone) {
            return !leftDone;
        }
        if (left->enabled != right->enabled) {
            return left->enabled;
        }
        if (sortMode == 1) {
            if (left->createdAt != right->createdAt) {
                return left->createdAt < right->createdAt;
            }
        } else if (sortMode == 2) {
            const std::wstring leftTitle = InitialSortKey(left->title);
            const std::wstring rightTitle = InitialSortKey(right->title);
            if (leftTitle != rightTitle) {
                return leftTitle < rightTitle;
            }
        } else if (sortMode != 3) {
            if (left->nextDueAt != right->nextDueAt) {
                if (left->nextDueAt.empty()) {
                    return false;
                }
                if (right->nextDueAt.empty()) {
                    return true;
                }
                return left->nextDueAt < right->nextDueAt;
            }
        }
        if (left->pos != right->pos) {
            return left->pos < right->pos;
        }
        return left->id < right->id;
    });
    return items;
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

TodoItem* MainWindow::FindTodoItem(int id) {
    for (auto& item : model_.todos) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

NotePage* MainWindow::FindNotePage(int tagId) {
    for (auto& note : model_.notes) {
        if (note.tagId == tagId) {
            return &note;
        }
    }
    return nullptr;
}

const NotePage* MainWindow::FindNotePage(int tagId) const {
    for (const auto& note : model_.notes) {
        if (note.tagId == tagId) {
            return &note;
        }
    }
    return nullptr;
}

void MainWindow::SaveCurrentNotePage() {
    if (noteSaveTimerId_ != 0 && hwnd_) {
        KillTimer(hwnd_, ID_TIMER_NOTE_AUTOSAVE);
        noteSaveTimerId_ = 0;
    }
    const int tagId = noteEditTagId_ > 0 ? noteEditTagId_ : currentTagId_;
    const Group* tag = FindGroup(tagId);
    if (!noteDirty_ || !noteEdit_ || tagId <= 0 || !tag || !IsNoteTag(*tag)) {
        return;
    }

    const int textLength = GetWindowTextLengthW(noteEdit_);
    std::wstring content(static_cast<std::size_t>(std::max(0, textLength)) + 1, L'\0');
    if (textLength > 0) {
        GetWindowTextW(noteEdit_, content.data(), textLength + 1);
    }
    content.resize(static_cast<std::size_t>(std::max(0, textLength)));

    if (!storageService_.SaveNotePage(tagId, content)) {
        WriteAppLog(L"Save note page failed: " + storageService_.lastError());
        return;
    }

    const std::wstring updatedAt = CurrentTodoTimestamp();
    if (NotePage* note = FindNotePage(tagId)) {
        note->content = content;
        note->updatedAt = updatedAt;
    } else {
        NotePage newNote;
        newNote.tagId = tagId;
        newNote.content = content;
        newNote.updatedAt = updatedAt;
        model_.notes.push_back(std::move(newNote));
    }
    noteDirty_ = false;
}

void MainWindow::HideNoteEdit() {
    SaveCurrentNotePage();
    if (noteEdit_) {
        ShowWindow(noteEdit_, SW_HIDE);
    }
}

void MainWindow::EnsureNoteEdit(const D2D1_RECT_F& rect, const Group& tag) {
    RECT frame{
        static_cast<LONG>(rect.left + 0.5f),
        static_cast<LONG>(rect.top + 0.5f),
        static_cast<LONG>(rect.right + 0.5f),
        static_cast<LONG>(rect.bottom + 0.5f),
    };
    RECT editRect = ThemedControls::MultiLineEditRect(theme_, frame);
    const NotePage* note = FindNotePage(tag.id);
    const std::wstring content = note ? note->content : L"";
    if (!noteEditFont_) {
        noteEditFont_ = ThemedControls::CreateEditFont(theme_);
    }

    if (!noteEdit_) {
        noteEdit_ = ThemedControls::CreateMultiLineEdit(
            instance_,
            hwnd_,
            ID_NOTE_EDIT,
            theme_,
            frame,
            content,
            noteEditFont_,
            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_CLIPSIBLINGS);
        if (noteEdit_) {
            noteEditTagId_ = tag.id;
            noteEditFrame_ = editRect;
            noteDirty_ = false;
        }
    }

    if (!noteEdit_) {
        return;
    }
    if (noteEditTagId_ != tag.id) {
        SaveCurrentNotePage();
        SetWindowTextW(noteEdit_, content.c_str());
        noteEditTagId_ = tag.id;
        noteDirty_ = false;
    }
    if (!EqualRect(&noteEditFrame_, &editRect)) {
        MoveWindow(noteEdit_, editRect.left, editRect.top, editRect.right - editRect.left, editRect.bottom - editRect.top, TRUE);
        noteEditFrame_ = editRect;
    }
    ShowWindow(noteEdit_, SW_SHOWNA);
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
            const float tabGroupHeight = Metric(theme_, L"tabButton", L"height", 30.0f)
                + Metric(theme_, L"tabButton", L"groupPadding", 3.0f) * 2.0f;
            const float groupHeight = std::max(Metric(theme_, L"majorNav", L"height", 34.0f), tabGroupHeight);
            groups = D2D1::RectF(0.0f, top, width, std::min(bottom, top + groupHeight));
            top = groups.bottom;
        }
    }

    if (config_.showTag) {
        // Configured tag width maps 1:1 to rendered pixels; the ratio-based
        // maxTagWidth clamp keeps it from overwhelming narrow windows.
        const float configuredTagWidth = config_.tagWidth > 0 ? static_cast<float>(config_.tagWidth) : Metric(theme_, L"minorNav", L"width", 72.0f);
        const float minTagWidth = Metric(theme_, L"minorNav", L"minWidth", 40.0f);
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
