#include "ThemedControls.h"
#include "ThemedD2D.h"
#include "ThemedGdiFallback.h"
#include "ThemedUi.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef HDS_NOSIZING
#define HDS_NOSIZING 0x0800
#endif

namespace {
constexpr int kTabAppearanceStandard = 0;
constexpr int kTabAppearanceEmphasizedSegmented = 1;
constexpr int kTabAppearanceMinimalUnderline = 2;
constexpr int kTabAppearanceSoftPill = 3;
constexpr int kTabAppearanceConnectedTabs = 4;
constexpr int kTabOrientationHorizontal = 0;
constexpr int kTabOrientationVertical = 1;
constexpr int kTabContainerStyleAppearanceDefault = 0;
constexpr int kTabContainerStyleFramed = 1;
constexpr int kTabContainerStyleBorderless = 2;
HICON ButtonIcon(HWND hwnd) {
    return reinterpret_cast<HICON>(SendMessageW(hwnd, BM_GETIMAGE, IMAGE_ICON, 0));
}

enum class ControlKind {
    None,
    Button,
    MiniButton,
    PrimaryButton,
    CheckBox,
    Toggle,
    Radio,
    HotKeyCapture,
    Link,
    Panel,
    GroupBox,
    TabControl,
    ToolBar,
    Separator,
    TabButton,
    ComboBox,
    Edit,
    ListBox,
    Table,
    ProgressBar,
    Slider,
    Label,
    StatusBadge,
    StatusText,
    TimeDisplay,
};

// 主题控件的运行时状态。以进程内 HWND 映射存储，避免使用桌面堆（SetPropW），
// 因为某些桌面的桌面堆不可用会导致 SetPropW 以 ERROR_NOT_ENOUGH_MEMORY 失败。
struct ControlState {
    ControlKind kind = ControlKind::None;
    const Theme* theme = nullptr;
    std::wstring text;
    std::wstring backgroundComponent;
    std::wstring statusState;
    bool hasText = false;
    bool hover = false;
    bool checked = false;
    bool selected = false;
    int tabAppearance = 0;
    int tabOrientation = 0;
    int tabContainerStyle = kTabContainerStyleAppearanceDefault;
    bool multiline = false;
    bool selectAllOnFocus = false;
    UINT dpi = 0;
    HFONT ownedFont = nullptr;
    std::optional<TablerIconManifest::Id> buttonTablerIcon;
    int radioGroup = 0;
    bool progressIndeterminate = false;
    double progressValue = 0.0;
    double sliderMinimum = 0.0;
    double sliderMaximum = 100.0;
    double sliderStep = 1.0;
    double sliderValue = 0.0;
    HIMAGELIST tableDefaultSmallImages = nullptr;
    HIMAGELIST tableCheckBoxStateImages = nullptr;
    bool tableCheckable = false;
    UINT tableDpi = USER_DEFAULT_SCREEN_DPI;
    int tableHotRow = -1;
    int tableHotColumn = -1;
    int tablePressedRow = -1;
    int tablePressedColumn = -1;
    bool tableAllowColumnResize = false;
    bool tableAllowHorizontalScroll = false;
    bool tableReserveScrollBarGutter = false;
    bool tableShowRowGridLines = false;
    bool tableShowColumnGridLines = false;
    bool tableTwoLineRows = false;
    int tableRowsUpdateDepth = 0;
    std::vector<int> tableColumnWidthModes;
    std::vector<bool> tableRowEnabled;
    std::vector<bool> tableRowActive;
    std::vector<std::vector<ThemedControls::TableCellRuntime>> tableCells;
};

std::mutex& StateMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<HWND, ControlState>& StateMap() {
    static std::unordered_map<HWND, ControlState> map;
    return map;
}

// 获取或创建控件状态（若不存在则插入默认值）。
ControlState& StateFor(HWND hwnd) {
    std::lock_guard<std::mutex> lock(StateMutex());
    return StateMap()[hwnd];
}

// 复制式读取控件状态，不存在时返回空 optional，避免持锁期间暴露引用。
std::optional<ControlState> FindState(HWND hwnd) {
    std::lock_guard<std::mutex> lock(StateMutex());
    auto& map = StateMap();
    auto it = map.find(hwnd);
    if (it == map.end()) {
        return std::nullopt;
    }
    return it->second;
}

ControlKind KindFor(HWND hwnd) {
    std::lock_guard<std::mutex> lock(StateMutex());
    auto& map = StateMap();
    auto it = map.find(hwnd);
    return it == map.end() ? ControlKind::None : it->second.kind;
}

void EraseState(HWND hwnd) {
    std::lock_guard<std::mutex> lock(StateMutex());
    auto& map = StateMap();
    auto it = map.find(hwnd);
    if (it != map.end()) {
        if (it->second.tableDefaultSmallImages) {
            ImageList_Destroy(it->second.tableDefaultSmallImages);
            it->second.tableDefaultSmallImages = nullptr;
        }
        if (it->second.tableCheckBoxStateImages) {
            ImageList_Destroy(it->second.tableCheckBoxStateImages);
            it->second.tableCheckBoxStateImages = nullptr;
        }
        if (it->second.ownedFont) {
            DeleteObject(it->second.ownedFont);
            it->second.ownedFont = nullptr;
        }
        map.erase(it);
    }
}

bool IsOwnerDrawButtonKind(ControlKind kind) {
    return kind == ControlKind::Button ||
           kind == ControlKind::MiniButton ||
           kind == ControlKind::PrimaryButton ||
           kind == ControlKind::CheckBox ||
           kind == ControlKind::Toggle ||
           kind == ControlKind::Radio ||
           kind == ControlKind::HotKeyCapture ||
           kind == ControlKind::Link ||
           kind == ControlKind::Panel ||
           kind == ControlKind::GroupBox ||
           kind == ControlKind::TabControl ||
           kind == ControlKind::ToolBar ||
           kind == ControlKind::Separator ||
           kind == ControlKind::TabButton;
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

int ScaledMetric(HWND hwnd, const Theme& theme, const wchar_t* component, const wchar_t* name, float fallback) {
    const int logicalPixels = static_cast<int>(theme.metric(component, name, fallback));
    const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : USER_DEFAULT_SCREEN_DPI;
    return MulDiv(logicalPixels, static_cast<int>(dpi ? dpi : USER_DEFAULT_SCREEN_DPI), USER_DEFAULT_SCREEN_DPI);
}

int TableScaledMetric(HWND table, const Theme& theme, const wchar_t* component, const wchar_t* name, float fallback) {
    const auto state = FindState(table);
    UINT dpi = state && state->tableDpi ? state->tableDpi : (table ? GetDpiForWindow(table) : USER_DEFAULT_SCREEN_DPI);
    if (!dpi) dpi = USER_DEFAULT_SCREEN_DPI;
    return MulDiv(static_cast<int>(theme.metric(component, name, fallback)), static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

int ActiveScaledMetric(const Theme& theme, const wchar_t* component, const wchar_t* name, float fallback) {
    return ThemedD2D::ScaleDip(
        static_cast<int>(theme.metric(component, name, fallback)),
        ThemedD2D::ActiveDpi());
}

UINT ControlDpi(HWND hwnd) {
    const auto state = FindState(hwnd);
    if (state && state->dpi) {
        return state->dpi;
    }
    const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : USER_DEFAULT_SCREEN_DPI;
    return dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
}

int ControlScaledMetric(
    HWND hwnd,
    const Theme& theme,
    const wchar_t* component,
    const wchar_t* name,
    float fallback) {
    return ThemedD2D::ScaleDip(
        static_cast<int>(theme.metric(component, name, fallback)),
        ControlDpi(hwnd));
}

std::wstring WindowText(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

void SetControlTextProp(HWND hwnd, const wchar_t* text) {
    ControlState& state = StateFor(hwnd);
    state.text = text ? text : L"";
    state.hasText = true;
}

std::wstring ControlText(HWND hwnd) {
    std::wstring text = WindowText(hwnd);
    if (!text.empty()) {
        return text;
    }
    if (auto state = FindState(hwnd); state && state->hasText) {
        return state->text;
    }
    return {};
}

std::wstring BackgroundComponent(HWND hwnd) {
    auto state = FindState(hwnd);
    if (!state || state->backgroundComponent.empty()) {
        return L"dialog";
    }
    return state->backgroundComponent;
}

void SetButtonDrawColors(const Theme& theme, const wchar_t* component, const wchar_t* state, COLORREF& fill, COLORREF& border, COLORREF& text) {
    fill = ToColorRef(theme.color(component, state, L"bg"));
    border = ToColorRef(theme.color(component, state, L"border"));
    text = ToColorRef(theme.color(component, state, L"text"));
}

const wchar_t* ButtonState(bool hover, bool pressed, bool disabled) {
    return disabled ? L"disabled" : (pressed ? L"pressed" : (hover ? L"hover" : L"normal"));
}

void FillRoundRect(HDC dc, RECT rect, int radius, COLORREF fill, COLORREF border, int borderWidth) {
    if (ThemedD2D::FillRoundedRect(
            dc,
            rect,
            static_cast<float>(std::max(0, radius)),
            fill,
            border,
            static_cast<float>(std::max(0, borderWidth)))) {
        return;
    }
    ThemedGdiFallback::FillRoundedRect(dc, rect, radius, fill, border, borderWidth);
}

void DrawRoundRect(HDC dc, RECT rect, int radius, COLORREF border, int borderWidth) {
    if (ThemedD2D::DrawRoundedRect(
            dc,
            rect,
            static_cast<float>(std::max(0, radius)),
            border,
            static_cast<float>(std::max(1, borderWidth)))) {
        return;
    }
    ThemedGdiFallback::DrawRoundedRect(dc, rect, radius, border, borderWidth);
}

void DrawThemedText(
    HDC dc,
    HFONT font,
    const wchar_t* text,
    int textLength,
    RECT rect,
    UINT format,
    COLORREF color) {
    if (ThemedD2D::DrawTextLayout(dc, font, text, textLength, rect, format, color)) {
        return;
    }
    ThemedGdiFallback::DrawText(dc, font, text, textLength, rect, format, color);
}

void FillThemedRect(HDC dc, RECT rect, COLORREF fill) {
    if (ThemedD2D::FillRect(dc, rect, fill)) {
        return;
    }
    ThemedGdiFallback::FillSolidRect(dc, rect, fill);
}

void FillThemedEllipse(HDC dc, RECT rect, COLORREF fill, COLORREF border, int borderWidth) {
    if (ThemedD2D::FillEllipse(dc, rect, fill, border, static_cast<float>(std::max(0, borderWidth)))) {
        return;
    }
    ThemedGdiFallback::FillEllipse(dc, rect, fill, border, borderWidth);
}

void DrawThemedPolyline(HDC dc, const POINT* points, int pointCount, COLORREF color, int strokeWidth) {
    if (ThemedD2D::DrawPolyline(dc, points, pointCount, color, static_cast<float>(std::max(1, strokeWidth)))) {
        return;
    }
    ThemedGdiFallback::DrawPolyline(dc, points, pointCount, color, strokeWidth);
}

void DrawThemedLine(
    HDC dc,
    int x1,
    int y1,
    int x2,
    int y2,
    COLORREF color,
    int strokeWidth,
    bool pixelSnap = true) {
    if (ThemedD2D::DrawLine(
            dc,
            static_cast<float>(x1),
            static_cast<float>(y1),
            static_cast<float>(x2),
            static_cast<float>(y2),
            color,
            static_cast<float>(std::max(1, strokeWidth)),
            pixelSnap)) {
        return;
    }
    ThemedGdiFallback::DrawLine(dc, x1, y1, x2, y2, color, strokeWidth);
}

void DrawButtonFrame(
    const Theme& theme,
    HDC dc,
    RECT rect,
    const wchar_t* component,
    bool hover,
    bool pressed,
    bool focused,
    bool disabled) {
    const wchar_t* state = ButtonState(hover, pressed, disabled);
    const int radius = ActiveScaledMetric(theme, L"button", L"radius", 6.0f);
    const int borderWidth = ActiveScaledMetric(theme, L"button", L"borderWidth", 1.0f);
    FillRoundRect(
        dc,
        rect,
        radius,
        ToColorRef(theme.color(component, state, L"bg")),
        // A disabled control must not retain the keyboard-focus accent. Native
        // buttons can keep focus briefly when they are disabled (for example
        // after clicking a Stop button), and drawing the focused border on each
        // timer refresh makes that disabled control appear to flash.
        ToColorRef(theme.color(component, (focused && !disabled) ? L"focused" : state, L"border")),
        borderWidth);
}

bool IsHover(HWND hwnd) {
    auto state = FindState(hwnd);
    return state && state->hover;
}

void InvalidateParentAround(HWND hwnd) {
    HWND parent = GetParent(hwnd);
    if (!parent) {
        return;
    }
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&rect), 2);
    InflateRect(&rect, 16, 16);
    InvalidateRect(parent, &rect, TRUE);
}

bool IsChecked(HWND hwnd) {
    auto state = FindState(hwnd);
    return state && state->checked;
}

void SetChecked(HWND hwnd, bool checked) {
    StateFor(hwnd).checked = checked;
    InvalidateRect(hwnd, nullptr, TRUE);
}

void ToggleChecked(HWND hwnd) {
    SetChecked(hwnd, !IsChecked(hwnd));
}

void SelectRadio(HWND hwnd) {
    const auto selectedState = FindState(hwnd);
    if (!selectedState || selectedState->kind != ControlKind::Radio) {
        return;
    }
    const HWND parent = GetParent(hwnd);
    std::vector<HWND> groupMembers;
    {
        std::lock_guard<std::mutex> lock(StateMutex());
        for (const auto& [candidate, state] : StateMap()) {
            if (state.kind == ControlKind::Radio
                && state.radioGroup == selectedState->radioGroup
                && GetParent(candidate) == parent) {
                groupMembers.push_back(candidate);
            }
        }
    }
    for (HWND candidate : groupMembers) {
        SetChecked(candidate, candidate == hwnd);
    }
}

double ClampSliderValue(const ControlState& state, double value) {
    const double minimum = std::min(state.sliderMinimum, state.sliderMaximum);
    const double maximum = std::max(state.sliderMinimum, state.sliderMaximum);
    double clamped = std::max(minimum, std::min(maximum, value));
    if (state.sliderStep > 0.0) {
        clamped = minimum + std::round((clamped - minimum) / state.sliderStep) * state.sliderStep;
        clamped = std::max(minimum, std::min(maximum, clamped));
    }
    return clamped;
}

void NotifySlider(HWND hwnd) {
    if (HWND parent = GetParent(hwnd)) {
        SendMessageW(parent, WM_HSCROLL, MAKEWPARAM(SB_THUMBPOSITION, 0), reinterpret_cast<LPARAM>(hwnd));
    }
}

void SetSliderFromClientX(HWND hwnd, int x, bool notify) {
    auto state = FindState(hwnd);
    if (!state || state->kind != ControlKind::Slider) {
        return;
    }
    RECT rect{};
    GetClientRect(hwnd, &rect);
    const int thumbSize = state->theme
        ? static_cast<int>(state->theme->metric(L"slider", L"thumbSize", 14.0f))
        : 14;
    const int left = thumbSize / 2;
    const int right = std::max(left + 1, static_cast<int>(rect.right) - thumbSize / 2);
    const double ratio = static_cast<double>(std::max(left, std::min(right, x)) - left) / static_cast<double>(right - left);
    const double value = state->sliderMinimum + ratio * (state->sliderMaximum - state->sliderMinimum);
    auto& mutableState = StateFor(hwnd);
    mutableState.sliderValue = ClampSliderValue(mutableState, value);
    InvalidateRect(hwnd, nullptr, FALSE);
    if (notify) {
        NotifySlider(hwnd);
    }
}

RECT ComboTextRect(const Theme& theme, RECT frame) {
    const int paddingX = ActiveScaledMetric(theme, L"comboBox", L"paddingX", 9.0f);
    const int textHeight = ActiveScaledMetric(theme, L"comboBox", L"textHeight", 20.0f);
    const int offsetY = ActiveScaledMetric(theme, L"comboBox", L"textOffsetY", 1.0f);
    const int arrowWidth = ActiveScaledMetric(theme, L"comboBox", L"arrowWidth", 28.0f);
    RECT rect{};
    rect.left = frame.left + paddingX;
    rect.right = frame.right - arrowWidth;
    rect.top = frame.top + ((frame.bottom - frame.top) - textHeight) / 2 + offsetY;
    rect.bottom = rect.top + textHeight;
    return rect;
}

std::wstring ComboBoxItemText(HWND hwnd, WPARAM index) {
    const LRESULT length = SendMessageW(hwnd, CB_GETLBTEXTLEN, index, 0);
    if (length == CB_ERR || length < 0) {
        return {};
    }

    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const LRESULT copied = SendMessageW(hwnd, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text.data()));
    if (copied == CB_ERR) {
        return {};
    }
    text.resize(static_cast<std::size_t>(copied));
    return text;
}

std::wstring ComboBoxSelectedText(HWND hwnd) {
    const LRESULT selected = SendMessageW(hwnd, CB_GETCURSEL, 0, 0);
    if (selected >= 0) {
        std::wstring text = ComboBoxItemText(hwnd, static_cast<WPARAM>(selected));
        if (!text.empty()) {
            return text;
        }
    }
    return WindowText(hwnd);
}

void InvalidateComboBox(HWND hwnd) {
    InvalidateRect(hwnd, nullptr, TRUE);
    UpdateWindow(hwnd);
}

void DrawComboOverlay(HWND hwnd, HDC targetDc = nullptr) {
    auto controlState = FindState(hwnd);
    const Theme* theme = controlState ? controlState->theme : nullptr;
    if (!theme) {
        return;
    }

    HDC dc = targetDc ? targetDc : GetDC(hwnd);
    if (!dc) {
        return;
    }
    ThemedD2D::ScopedHdcPaint d2dPaint(hwnd, dc);

    RECT rect{};
    GetClientRect(hwnd, &rect);
    const bool disabled = !IsWindowEnabled(hwnd);
    const bool focused = GetFocus() == hwnd;
    const bool hover = IsHover(hwnd);
    const wchar_t* state = disabled ? L"disabled" : (focused ? L"focused" : (hover ? L"hover" : L"normal"));
    const int radius = ActiveScaledMetric(*theme, L"comboBox", L"radius", 7.0f);
    const int borderWidth = ActiveScaledMetric(*theme, L"comboBox", L"borderWidth", 1.0f);
    FillThemedRect(dc, rect, ToColorRef(theme->color(L"dialog", L"normal", L"bg")));
    FillRoundRect(
        dc,
        rect,
        radius,
        ToColorRef(theme->color(L"comboBox", state, L"bg")),
        ToColorRef(theme->color(L"comboBox", state, L"border")),
        borderWidth);

    const std::wstring text = ComboBoxSelectedText(hwnd);

    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = nullptr;
    if (font) {
        oldFont = SelectObject(dc, font);
    }
    RECT textRect = ComboTextRect(*theme, rect);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ToColorRef(theme->color(L"comboBox", state, L"text")));
    DrawThemedText(dc, font, text.c_str(), static_cast<int>(text.size()), textRect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, GetTextColor(dc));
    if (oldFont) {
        SelectObject(dc, oldFont);
    }

    const int arrowSize = ActiveScaledMetric(*theme, L"comboBox", L"arrowSize", 5.0f);
    const int arrowPaddingRight = ActiveScaledMetric(*theme, L"comboBox", L"arrowPaddingRight", 11.0f);
    const int cx = rect.right - arrowPaddingRight - arrowSize;
    const int cy = (rect.top + rect.bottom) / 2 + ActiveScaledMetric(*theme, L"comboBox", L"arrowOffsetY", 1.0f);
    const POINT arrow[]{
        POINT{cx - arrowSize, cy - arrowSize / 2},
        POINT{cx, cy + arrowSize / 2},
        POINT{cx + arrowSize, cy - arrowSize / 2}};
    DrawThemedPolyline(
        dc,
        arrow,
        3,
        ToColorRef(theme->color(L"comboBox", state, L"arrow")),
        ActiveScaledMetric(*theme, L"comboBox", L"arrowStrokeWidth", 1.0f));

    DrawRoundRect(dc, rect, radius, ToColorRef(theme->color(L"comboBox", state, L"border")), borderWidth);
    if (!targetDc) {
        ReleaseDC(hwnd, dc);
    }
}

double ProgressValue(HWND hwnd) {
    auto state = FindState(hwnd);
    return state ? state->progressValue : 0.0;
}

bool ProgressIndeterminate(HWND hwnd) {
    auto state = FindState(hwnd);
    return state && state->progressIndeterminate;
}

void DrawStatusBadge(HWND hwnd, HDC dc, RECT rect) {
    auto controlState = FindState(hwnd);
    const Theme* theme = controlState ? controlState->theme : nullptr;
    if (!theme) {
        return;
    }

    const std::wstring backgroundComponent = BackgroundComponent(hwnd);
    FillThemedRect(dc, rect, ToColorRef(theme->color(backgroundComponent, L"normal", L"bg")));

    RECT badge = rect;
    InflateRect(&badge, 0, -1);
    const std::wstring stateValue = controlState->statusState;
    const wchar_t* state = stateValue.empty() ? L"success" : stateValue.c_str();
    const int height = std::max(1, static_cast<int>(badge.bottom - badge.top));
    FillRoundRect(
        dc,
        badge,
        height / 2,
        ToColorRef(theme->color(L"global", state, L"bg")),
        ToColorRef(theme->color(L"global", state, L"text")),
        1);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ToColorRef(theme->color(L"global", state, L"text")));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    std::wstring text = ControlText(hwnd);
    DrawThemedText(dc, font, text.c_str(), static_cast<int>(text.size()), badge,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, GetTextColor(dc));
    if (oldFont) {
        SelectObject(dc, oldFont);
    }
}

void DrawLabelControl(HWND hwnd, HDC dc, RECT rect) {
    auto controlState = FindState(hwnd);
    const Theme* theme = controlState ? controlState->theme : nullptr;
    if (!theme) return;
    FillThemedRect(
        dc, rect,
        ToColorRef(theme->color(BackgroundComponent(hwnd), L"normal", L"bg")));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    UINT format = controlState->multiline
        ? (DT_WORDBREAK | DT_NOPREFIX)
        : (DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if ((style & SS_CENTER) == SS_CENTER) format |= DT_CENTER;
    else if ((style & SS_RIGHT) == SS_RIGHT) format |= DT_RIGHT;
    else format |= DT_LEFT;
    const std::wstring text = ControlText(hwnd);
    DrawThemedText(
        dc,
        font,
        text.c_str(),
        static_cast<int>(text.size()),
        rect,
        format,
        ToColorRef(theme->color(L"label", IsWindowEnabled(hwnd) ? L"normal" : L"disabled", L"text")));
}

void DrawStatusText(HWND hwnd, HDC dc, RECT rect) {
    auto controlState = FindState(hwnd);
    const Theme* theme = controlState ? controlState->theme : nullptr;
    if (!theme) {
        return;
    }

    const std::wstring backgroundComponent = BackgroundComponent(hwnd);
    FillThemedRect(dc, rect, ToColorRef(theme->color(backgroundComponent, L"normal", L"bg")));

    const std::wstring stateValue = controlState->statusState;
    const wchar_t* state = stateValue.empty() ? L"normal" : stateValue.c_str();
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ToColorRef(theme->color(L"text", state, L"text")));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    std::wstring text = ControlText(hwnd);
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    UINT format = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
    if ((style & SS_CENTER) == SS_CENTER) {
        format |= DT_CENTER;
    } else if ((style & SS_RIGHT) == SS_RIGHT) {
        format |= DT_RIGHT;
    } else {
        format |= DT_LEFT;
    }
    DrawThemedText(dc, font, text.c_str(), static_cast<int>(text.size()), rect, format, GetTextColor(dc));
    if (oldFont) {
        SelectObject(dc, oldFont);
    }
}

HFONT CreateTimeDisplayFont(HFONT baseFont, const Theme& theme, UINT dpi) {
    LOGFONTW font{};
    if (!baseFont || GetObjectW(baseFont, sizeof(font), &font) != sizeof(font)) {
        font.lfHeight = -MulDiv(14, static_cast<int>(dpi ? dpi : USER_DEFAULT_SCREEN_DPI), USER_DEFAULT_SCREEN_DPI);
        font.lfWeight = FW_NORMAL;
        font.lfCharSet = DEFAULT_CHARSET;
        font.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(font.lfFaceName, L"Microsoft YaHei UI");
    }
    const float scale = std::max(1.0f, theme.metric(L"timeDisplay", L"fontScale", 5.0f));
    font.lfHeight = static_cast<LONG>(std::lround(static_cast<double>(font.lfHeight) * scale));
    font.lfWidth = 0;
    return CreateFontIndirectW(&font);
}

SIZE MeasureFontText(HFONT font, const std::wstring& text) {
    SIZE size = ThemedD2D::MeasureText(font, text, 0, false);
    if (size.cx > 0 && size.cy > 0) {
        return size;
    }
    HDC dc = GetDC(nullptr);
    if (!dc) {
        return {};
    }
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    if (oldFont) {
        SelectObject(dc, oldFont);
    }
    ReleaseDC(nullptr, dc);
    return size;
}

HFONT ReplaceTimeDisplayFont(HWND hwnd, HFONT baseFont) {
    auto state = FindState(hwnd);
    if (!state || state->kind != ControlKind::TimeDisplay || !state->theme) {
        return baseFont;
    }
    if (baseFont && baseFont == state->ownedFont) {
        return baseFont;
    }
    HFONT replacement = CreateTimeDisplayFont(baseFont, *state->theme, state->dpi);
    if (!replacement) {
        return baseFont;
    }

    HFONT previous = nullptr;
    {
        std::lock_guard<std::mutex> lock(StateMutex());
        auto found = StateMap().find(hwnd);
        if (found == StateMap().end()) {
            DeleteObject(replacement);
            return baseFont;
        }
        previous = found->second.ownedFont;
        found->second.ownedFont = replacement;
    }
    if (previous) {
        DeleteObject(previous);
    }
    return replacement;
}

void DrawTimeDisplay(HWND hwnd, HDC dc, RECT rect) {
    auto state = FindState(hwnd);
    const Theme* theme = state ? state->theme : nullptr;
    if (!theme) {
        return;
    }
    const wchar_t* visualState = IsWindowEnabled(hwnd) ? L"normal" : L"disabled";
    FillThemedRect(
        dc,
        rect,
        ToColorRef(theme->color(BackgroundComponent(hwnd), L"normal", L"bg")));
    const UINT dpi = state->dpi ? state->dpi : USER_DEFAULT_SCREEN_DPI;
    const int radius = MulDiv(
        static_cast<int>(std::lround(theme->metric(L"timeDisplay", L"radius", 7.0f))),
        static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
    const int borderWidth = std::max(1, MulDiv(
        static_cast<int>(std::lround(theme->metric(L"timeDisplay", L"borderWidth", 1.0f))),
        static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI));
    FillRoundRect(
        dc,
        rect,
        radius,
        ToColorRef(theme->color(L"timeDisplay", visualState, L"bg")),
        ToColorRef(theme->color(L"timeDisplay", visualState, L"border")),
        borderWidth);

    const int paddingX = MulDiv(
        static_cast<int>(std::lround(theme->metric(L"timeDisplay", L"paddingX", 16.0f))),
        static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
    const int paddingY = MulDiv(
        static_cast<int>(std::lround(theme->metric(L"timeDisplay", L"paddingY", 8.0f))),
        static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
    RECT textRect = rect;
    InflateRect(&textRect, -paddingX, -paddingY);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    const std::wstring text = ControlText(hwnd);
    DrawThemedText(
        dc,
        font,
        text.c_str(),
        static_cast<int>(text.size()),
        textRect,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
        ToColorRef(theme->color(L"timeDisplay", visualState, L"text")));
}

void DrawProgressBar(HWND hwnd, HDC targetDc = nullptr) {
    auto controlState = FindState(hwnd);
    const Theme* theme = controlState ? controlState->theme : nullptr;
    if (!theme) {
        return;
    }

    HDC dc = targetDc ? targetDc : GetDC(hwnd);
    if (!dc) {
        return;
    }
    ThemedD2D::ScopedHdcPaint d2dPaint(hwnd, dc);

    RECT rect{};
    GetClientRect(hwnd, &rect);
    const bool disabled = !IsWindowEnabled(hwnd);
    const wchar_t* state = disabled ? L"disabled" : L"normal";
    const int radius = ActiveScaledMetric(*theme, L"progressBar", L"radius", 7.0f);
    const int borderWidth = ActiveScaledMetric(*theme, L"progressBar", L"borderWidth", 1.0f);
    FillRoundRect(
        dc,
        rect,
        radius,
        ToColorRef(theme->color(L"progressBar", state, L"track")),
        ToColorRef(theme->color(L"progressBar", state, L"border")),
        borderWidth);

    RECT fillRect = rect;
    InflateRect(&fillRect, -std::max(1, borderWidth), -std::max(1, borderWidth));
    if (fillRect.right > fillRect.left && fillRect.bottom > fillRect.top) {
        if (ProgressIndeterminate(hwnd)) {
            const int width = fillRect.right - fillRect.left;
            const int segmentWidth = std::max(width / 3, 24);
            const DWORD tick = GetTickCount();
            const int span = width + segmentWidth;
            const int offset = static_cast<int>((tick / 16) % static_cast<DWORD>(std::max(1, span)));
            fillRect.left = rect.left + std::max(1, borderWidth) + offset - segmentWidth;
            fillRect.right = std::min(rect.right - std::max(1, borderWidth), fillRect.left + segmentWidth);
            fillRect.left = std::max(rect.left + std::max(1, borderWidth), fillRect.left);
        } else {
            const double value = ClampFloat(static_cast<float>(ProgressValue(hwnd)), 0.0f, 1.0f);
            fillRect.right = fillRect.left + static_cast<int>((fillRect.right - fillRect.left) * value + 0.5);
        }
        if (fillRect.right > fillRect.left) {
            FillRoundRect(
                dc,
                fillRect,
                std::max(0, radius - borderWidth),
                ToColorRef(theme->color(L"progressBar", state, L"fill")),
                ToColorRef(theme->color(L"progressBar", state, L"fill")),
                1);
        }
    }

    if (!targetDc) {
        ReleaseDC(hwnd, dc);
    }
}

void DrawButton(const Theme& theme, const DRAWITEMSTRUCT* draw);
void DrawPrimaryButton(const Theme& theme, const DRAWITEMSTRUCT* draw);
void DrawMiniButton(const Theme& theme, const DRAWITEMSTRUCT* draw);
void DrawSlider(HWND hwnd, HDC dc);

// After a user drag (or divider double-click autosize) changes one column's
// width, refill the table by resizing the last Remaining-mode column that was
// not the dragged one, so the columns keep spanning the full client width and
// no horizontal scrollbar or trailing blank strip appears. draggedIndex /
// draggedWidth describe the column the user resized (-1 when unknown; current
// widths are then read back from the control).
void RelayoutTableRemainingColumns(HWND table, int draggedIndex, int draggedWidth) {
    const auto state = FindState(table);
    if (!state || state->kind != ControlKind::Table || state->tableColumnWidthModes.empty()) {
        return;
    }
    // Stored via ConfigureTableColumns as static_cast<int>(ThemedTableColumnWidth).
    constexpr int remainingMode = 2;  // ThemedTableColumnWidth::Remaining
    const auto& modes = state->tableColumnWidthModes;
    int adjustIndex = -1;
    for (int i = static_cast<int>(modes.size()) - 1; i >= 0; --i) {
        if (modes[i] == remainingMode && i != draggedIndex) {
            adjustIndex = i;
            break;
        }
    }
    if (adjustIndex < 0) {
        // The dragged column was the only Remaining one (or none exists). The
        // user-driven width is authoritative for the dragged column, so keep the
        // table filled by letting the last other column absorb the delta instead.
        for (int i = static_cast<int>(modes.size()) - 1; i >= 0; --i) {
            if (i != draggedIndex) {
                adjustIndex = i;
                break;
            }
        }
    }
    if (adjustIndex < 0) {
        return;
    }
    RECT client{};
    if (!GetClientRect(table, &client)) {
        return;
    }
    const int reservedGutter = state->tableAllowHorizontalScroll || !state->tableReserveScrollBarGutter
        ? 0
        : GetSystemMetricsForDpi(SM_CXVSCROLL, state->tableDpi ? state->tableDpi : USER_DEFAULT_SCREEN_DPI);
    int otherColumnsWidth = 0;
    for (int i = 0; i < static_cast<int>(modes.size()); ++i) {
        if (i == adjustIndex) {
            continue;
        }
        otherColumnsWidth += (i == draggedIndex && draggedWidth >= 0)
            ? draggedWidth
            : ListView_GetColumnWidth(table, i);
    }
    const int minimumWidth = state->tableAllowHorizontalScroll ? 24 : 1;
    const int width = std::max(minimumWidth, static_cast<int>(client.right - client.left - reservedGutter - otherColumnsWidth));
    ListView_SetColumnWidth(table, adjustIndex, width);
    if (!state->tableAllowHorizontalScroll) {
        ShowScrollBar(table, SB_HORZ, FALSE);
    }
}

void InvalidateTableRow(HWND list, int row) {
    if (row < 0) {
        return;
    }
    RECT rect{};
    if (ListView_GetItemRect(list, row, &rect, LVIR_BOUNDS)) {
        InvalidateRect(list, &rect, FALSE);
    }
}

bool IsDisabledTableRowHit(HWND table, LPARAM lParam) {
    LVHITTESTINFO hit{};
    hit.pt.x = GET_X_LPARAM(lParam);
    hit.pt.y = GET_Y_LPARAM(lParam);
    ListView_SubItemHitTest(table, &hit);
    return hit.iItem >= 0 && !ThemedControls::IsTableRowEnabled(table, hit.iItem);
}

LRESULT CALLBACK ThemedControlProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR) {
    if (message == WM_NOTIFY && KindFor(hwnd) == ControlKind::Table) {
        // The table's header (SysHeader32) sends its NM_CUSTOMDRAW notifications
        // to its parent ListView, not to the host window, so they never reach the
        // host's WM_NOTIFY handler. Route them into the shared themed-header draw
        // path here; otherwise the OS default header renders, which paints a
        // separator at every column edge including the last column's right side.
        auto* nm = reinterpret_cast<NMHDR*>(lParam);
        if (auto state = FindState(hwnd); nm && state && state->theme) {
            LRESULT drawResult = 0;
            if (ThemedControls::HandleListViewCustomDraw(*state->theme, lParam, drawResult)) {
                return drawResult;
            }
        }
        if (nm && (nm->code == HDN_ENDTRACKW || nm->code == HDN_ENDTRACKA)) {
            // The header applies the dragged width only after this notification
            // returns, so pass the final width from the notification explicitly.
            auto* headerNotify = reinterpret_cast<NMHEADERW*>(lParam);
            const int draggedWidth = headerNotify->pitem && (headerNotify->pitem->mask & HDI_WIDTH)
                ? headerNotify->pitem->cxy
                : -1;
            const LRESULT trackResult = DefSubclassProc(hwnd, message, wParam, lParam);
            RelayoutTableRemainingColumns(hwnd, headerNotify->iItem, draggedWidth);
            return trackResult;
        }
        if (nm && (nm->code == HDN_DIVIDERDBLCLICKW || nm->code == HDN_DIVIDERDBLCLICKA)) {
            // The ListView performs the autosize while handling this notification,
            // so current widths are final once DefSubclassProc returns.
            const LRESULT clickResult = DefSubclassProc(hwnd, message, wParam, lParam);
            RelayoutTableRemainingColumns(hwnd, -1, -1);
            return clickResult;
        }
    }
    switch (message) {
    case WM_SIZE:
        if (KindFor(hwnd) == ControlKind::Table) {
            if (auto state = FindState(hwnd); state && !state->tableAllowHorizontalScroll) {
                RelayoutTableRemainingColumns(hwnd, -1, -1);
                ShowScrollBar(hwnd, SB_HORZ, FALSE);
            }
        }
        break;
    case WM_SETCURSOR:
        if (KindFor(hwnd) == ControlKind::Link && IsWindowEnabled(hwnd)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_GETDLGCODE:
        if (KindFor(hwnd) == ControlKind::HotKeyCapture) {
            return DLGC_WANTALLKEYS;
        }
        break;
    case CB_SETCURSEL: {
        LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        if (KindFor(hwnd) == ControlKind::ComboBox) {
            InvalidateComboBox(hwnd);
        }
        return result;
    }
    case CB_ADDSTRING:
    case CB_INSERTSTRING:
    case CB_DELETESTRING:
    case CB_RESETCONTENT: {
        LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        if (KindFor(hwnd) == ControlKind::ComboBox) {
            InvalidateComboBox(hwnd);
        }
        return result;
    }
    case BM_SETCHECK:
        if (KindFor(hwnd) == ControlKind::CheckBox || KindFor(hwnd) == ControlKind::Toggle) {
            SetChecked(hwnd, wParam == BST_CHECKED);
            return 0;
        }
        if (KindFor(hwnd) == ControlKind::Radio) {
            if (wParam == BST_CHECKED) {
                SelectRadio(hwnd);
            } else {
                SetChecked(hwnd, false);
            }
            return 0;
        }
        if (KindFor(hwnd) == ControlKind::TabButton) {
            StateFor(hwnd).selected = wParam == BST_CHECKED;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case BM_GETCHECK:
        if (KindFor(hwnd) == ControlKind::TabButton) {
            auto state = FindState(hwnd);
            if (state && state->selected) {
                return BST_CHECKED;
            }
            return DefSubclassProc(hwnd, message, wParam, lParam);
        }
        if (KindFor(hwnd) == ControlKind::CheckBox || KindFor(hwnd) == ControlKind::Toggle || KindFor(hwnd) == ControlKind::Radio) {
            return IsChecked(hwnd) ? BST_CHECKED : BST_UNCHECKED;
        }
        break;
    case BM_CLICK:
        if ((KindFor(hwnd) == ControlKind::CheckBox || KindFor(hwnd) == ControlKind::Toggle || KindFor(hwnd) == ControlKind::Radio)
            && IsWindowEnabled(hwnd)) {
            if (KindFor(hwnd) == ControlKind::Radio) {
                SelectRadio(hwnd);
            } else {
                ToggleChecked(hwnd);
            }
            if (HWND parent = GetParent(hwnd)) {
                SendMessageW(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), BN_CLICKED), reinterpret_cast<LPARAM>(hwnd));
            }
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (KindFor(hwnd) == ControlKind::Table && IsDisabledTableRowHit(hwnd, lParam)) {
            return 0;
        }
        if (KindFor(hwnd) == ControlKind::Table) {
            ControlState& tableState = StateFor(hwnd);
            const int pressedRow = tableState.tablePressedRow;
            tableState.tablePressedRow = -1;
            tableState.tablePressedColumn = -1;
            InvalidateTableRow(hwnd, pressedRow);
        }
        if ((KindFor(hwnd) == ControlKind::CheckBox || KindFor(hwnd) == ControlKind::Toggle || KindFor(hwnd) == ControlKind::Radio)
            && IsWindowEnabled(hwnd)) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT rect{};
            GetClientRect(hwnd, &rect);
            if (PtInRect(&rect, point)) {
                if (KindFor(hwnd) == ControlKind::Radio) {
                    SelectRadio(hwnd);
                } else {
                    ToggleChecked(hwnd);
                }
            }
        }
        if (KindFor(hwnd) == ControlKind::Slider && IsWindowEnabled(hwnd)) {
            ReleaseCapture();
            SetSliderFromClientX(hwnd, GET_X_LPARAM(lParam), true);
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        if (KindFor(hwnd) == ControlKind::Table && IsDisabledTableRowHit(hwnd, lParam)) {
            return 0;
        }
        if (KindFor(hwnd) == ControlKind::Table) {
            LVHITTESTINFO hit{};
            hit.pt = POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ListView_SubItemHitTest(hwnd, &hit);
            ControlState& tableState = StateFor(hwnd);
            tableState.tablePressedRow = hit.iItem;
            tableState.tablePressedColumn = hit.iSubItem;
            InvalidateTableRow(hwnd, hit.iItem);
        }
        if (KindFor(hwnd) == ControlKind::Slider && IsWindowEnabled(hwnd)) {
            SetFocus(hwnd);
            SetCapture(hwnd);
            SetSliderFromClientX(hwnd, GET_X_LPARAM(lParam), true);
            return 0;
        }
        break;
    case WM_KEYUP:
        if ((KindFor(hwnd) == ControlKind::CheckBox || KindFor(hwnd) == ControlKind::Toggle || KindFor(hwnd) == ControlKind::Radio)
            && IsWindowEnabled(hwnd) && wParam == VK_SPACE) {
            if (KindFor(hwnd) == ControlKind::Radio) {
                SelectRadio(hwnd);
            } else {
                ToggleChecked(hwnd);
            }
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (KindFor(hwnd) == ControlKind::Table && wParam == VK_SPACE) {
            const int selected = ListView_GetNextItem(hwnd, -1, LVNI_SELECTED);
            if (selected >= 0 && !ThemedControls::IsTableRowEnabled(hwnd, selected)) {
                return 0;
            }
            if (selected >= 0 && IsWindowEnabled(hwnd)) {
                const BOOL checked = ListView_GetCheckState(hwnd, selected) != FALSE;
                ListView_SetCheckState(hwnd, selected, checked ? FALSE : TRUE);
                return 0;
            }
        }
        if (KindFor(hwnd) == ControlKind::HotKeyCapture) {
            if (wParam != VK_CONTROL && wParam != VK_MENU && wParam != VK_SHIFT
                && wParam != VK_LWIN && wParam != VK_RWIN) {
                if (HWND parent = GetParent(hwnd)) {
                    SendMessageW(
                        parent,
                        ThemedControls::WM_HOTKEY_CAPTURED,
                        static_cast<WPARAM>(GetDlgCtrlID(hwnd)),
                        static_cast<LPARAM>(wParam));
                }
            }
            return 0;
        }
        if (KindFor(hwnd) == ControlKind::Slider && IsWindowEnabled(hwnd)) {
            auto state = FindState(hwnd);
            if (state) {
                double value = state->sliderValue;
                if (wParam == VK_LEFT || wParam == VK_DOWN) value -= state->sliderStep;
                else if (wParam == VK_RIGHT || wParam == VK_UP) value += state->sliderStep;
                else if (wParam == VK_HOME) value = state->sliderMinimum;
                else if (wParam == VK_END) value = state->sliderMaximum;
                else break;
                auto& mutableState = StateFor(hwnd);
                mutableState.sliderValue = ClampSliderValue(mutableState, value);
                InvalidateRect(hwnd, nullptr, FALSE);
                NotifySlider(hwnd);
                return 0;
            }
        }
        if (KindFor(hwnd) == ControlKind::Edit
            && (GetKeyState(VK_CONTROL) & 0x8000) != 0
            && (GetKeyState(VK_MENU) & 0x8000) == 0
            && (wParam == L'A' || wParam == L'a')) {
            SendMessageW(hwnd, EM_SETSEL, 0, -1);
            return 0;
        }
        break;
    case WM_SETFONT:
        if (KindFor(hwnd) == ControlKind::TimeDisplay) {
            HFONT font = ReplaceTimeDisplayFont(hwnd, reinterpret_cast<HFONT>(wParam));
            return DefSubclassProc(hwnd, message, reinterpret_cast<WPARAM>(font), lParam);
        }
        break;
    case WM_ERASEBKGND:
        if (KindFor(hwnd) == ControlKind::Label || KindFor(hwnd) == ControlKind::StatusBadge ||
            KindFor(hwnd) == ControlKind::StatusText || KindFor(hwnd) == ControlKind::TimeDisplay) {
            return 1;
        }
        break;
    case WM_PAINT: {
        ControlKind kind = KindFor(hwnd);
        if (IsOwnerDrawButtonKind(kind)) {
            return DefSubclassProc(hwnd, message, wParam, lParam);
        }
        if (kind == ControlKind::Label || kind == ControlKind::StatusBadge ||
            kind == ControlKind::StatusText || kind == ControlKind::TimeDisplay) {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rect{};
            GetClientRect(hwnd, &rect);
            {
                // EndDraw must complete while the BeginPaint HDC is still
                // valid; committing after EndPaint leaves stale pixels.
                ThemedD2D::ScopedHdcPaint d2dPaint(hwnd, dc);
                if (kind == ControlKind::Label) DrawLabelControl(hwnd, dc, rect);
                else if (kind == ControlKind::StatusBadge) DrawStatusBadge(hwnd, dc, rect);
                else if (kind == ControlKind::StatusText) DrawStatusText(hwnd, dc, rect);
                else DrawTimeDisplay(hwnd, dc, rect);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        if (kind == ControlKind::ProgressBar) {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            DrawProgressBar(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        if (kind == ControlKind::Slider) {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            DrawSlider(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        if (kind == ControlKind::ComboBox) {
            DrawComboOverlay(hwnd);
        }
        return result;
    }
    case WM_PRINTCLIENT: {
        ControlKind kind = KindFor(hwnd);
        if (IsOwnerDrawButtonKind(kind)) {
            return DefSubclassProc(hwnd, message, wParam, lParam);
        }
        if (kind == ControlKind::Label || kind == ControlKind::StatusBadge ||
            kind == ControlKind::StatusText || kind == ControlKind::TimeDisplay) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            RECT rect{};
            GetClientRect(hwnd, &rect);
            ThemedD2D::ScopedHdcPaint d2dPaint(hwnd, dc);
            if (kind == ControlKind::Label) DrawLabelControl(hwnd, dc, rect);
            else if (kind == ControlKind::StatusBadge) DrawStatusBadge(hwnd, dc, rect);
            else if (kind == ControlKind::StatusText) DrawStatusText(hwnd, dc, rect);
            else DrawTimeDisplay(hwnd, dc, rect);
            return 0;
        }
        if (kind == ControlKind::ProgressBar) {
            DrawProgressBar(hwnd, reinterpret_cast<HDC>(wParam));
            return 0;
        }
        if (kind == ControlKind::Slider) {
            DrawSlider(hwnd, reinterpret_cast<HDC>(wParam));
            return 0;
        }
        LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        if (kind == ControlKind::ComboBox) {
            DrawComboOverlay(hwnd, reinterpret_cast<HDC>(wParam));
        }
        return result;
    }
    case WM_MOUSEMOVE: {
        if (KindFor(hwnd) == ControlKind::Slider && GetCapture() == hwnd) {
            SetSliderFromClientX(hwnd, GET_X_LPARAM(lParam), true);
            return 0;
        }
        const ControlKind kind = KindFor(hwnd);
        if (!IsHover(hwnd)) {
            StateFor(hwnd).hover = true;
            TRACKMOUSEEVENT event{};
            event.cbSize = sizeof(event);
            event.dwFlags = TME_LEAVE;
            event.hwndTrack = hwnd;
            TrackMouseEvent(&event);
            // The table's custom draw never reads the control-level hover flag
            // (action-cell hover uses the tracked hot cell below), so a full
            // erase-and-repaint on enter only flickers; skip it for tables. The
            // cursor also "enters" whenever it comes back from the header or the
            // scrollbar, so this fired constantly while moving inside the list.
            if (kind != ControlKind::Table) {
                InvalidateRect(hwnd, nullptr, IsOwnerDrawButtonKind(kind) ? FALSE : TRUE);
            }
            if (kind == ControlKind::Edit) {
                InvalidateParentAround(hwnd);
            }
        }
        if (kind == ControlKind::Table) {
            // Only repaint when the hovered row/cell actually changes. The row
            // background consumes hover state, while action cells additionally use
            // the hot column for their button state.
            LVHITTESTINFO hit{};
            hit.pt.x = GET_X_LPARAM(lParam);
            hit.pt.y = GET_Y_LPARAM(lParam);
            ListView_SubItemHitTest(hwnd, &hit);
            ControlState& tableState = StateFor(hwnd);
            if (hit.iItem != tableState.tableHotRow || hit.iSubItem != tableState.tableHotColumn) {
                const int previousRow = tableState.tableHotRow;
                tableState.tableHotRow = hit.iItem;
                tableState.tableHotColumn = hit.iSubItem;
                InvalidateTableRow(hwnd, previousRow);
                InvalidateTableRow(hwnd, hit.iItem);
            }
        }
        break;
    }
    case WM_MOUSELEAVE:
        StateFor(hwnd).hover = false;
        if (KindFor(hwnd) == ControlKind::Table) {
            ControlState& tableState = StateFor(hwnd);
            const int hotRow = tableState.tableHotRow;
            tableState.tableHotRow = -1;
            tableState.tableHotColumn = -1;
            tableState.tablePressedRow = -1;
            tableState.tablePressedColumn = -1;
            InvalidateTableRow(hwnd, hotRow);
            break;
        }
        InvalidateRect(hwnd, nullptr, IsOwnerDrawButtonKind(KindFor(hwnd)) ? FALSE : TRUE);
        if (KindFor(hwnd) == ControlKind::Edit) {
            InvalidateParentAround(hwnd);
        }
        break;
    case WM_LBUTTONDBLCLK:
        if (KindFor(hwnd) == ControlKind::Table && IsDisabledTableRowHit(hwnd, lParam)) {
            return 0;
        }
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (KindFor(hwnd) == ControlKind::Edit) {
            if (message == WM_SETFOCUS) {
                const auto state = FindState(hwnd);
                if (state && state->selectAllOnFocus) {
                    SendMessageW(hwnd, EM_SETSEL, 0, -1);
                }
            }
            InvalidateParentAround(hwnd);
            break;
        }
        if (KindFor(hwnd) == ControlKind::ComboBox) {
            LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
            InvalidateComboBox(hwnd);
            return result;
        }
        break;
    case WM_DESTROY: {
        EraseState(hwnd);
        RemoveWindowSubclass(hwnd, ThemedControlProc, subclassId);
        break;
    }
    default:
        break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void AttachThemedBehavior(HWND hwnd) {
    SetWindowSubclass(hwnd, ThemedControlProc, 1, 0);
}

void DrawSlider(HWND hwnd, HDC dc) {
    auto state = FindState(hwnd);
    if (!state || !state->theme) {
        return;
    }
    ThemedD2D::ScopedHdcPaint d2dPaint(hwnd, dc);
    const Theme& theme = *state->theme;
    RECT rect{};
    GetClientRect(hwnd, &rect);
    FillThemedRect(dc, rect, ToColorRef(theme.color(L"dialog", L"normal", L"bg")));

    const bool disabled = !IsWindowEnabled(hwnd);
    const bool hover = IsHover(hwnd);
    const wchar_t* visualState = disabled ? L"disabled" : (hover ? L"hover" : L"normal");
    const int thumbSize = ActiveScaledMetric(theme, L"slider", L"thumbSize", 14.0f);
    const int trackHeight = ActiveScaledMetric(theme, L"slider", L"trackHeight", 4.0f);
    const int left = thumbSize / 2;
    const int right = std::max(left + 1, static_cast<int>(rect.right) - thumbSize / 2);
    const double range = state->sliderMaximum - state->sliderMinimum;
    const double ratio = range == 0.0 ? 0.0 : (state->sliderValue - state->sliderMinimum) / range;
    const int thumbCenter = left + static_cast<int>(std::max(0.0, std::min(1.0, ratio)) * (right - left) + 0.5);
    const int centerY = (rect.top + rect.bottom) / 2;
    RECT track{left, centerY - trackHeight / 2, right, centerY + (trackHeight + 1) / 2};
    FillRoundRect(
        dc, track, trackHeight / 2,
        ToColorRef(theme.color(L"slider", visualState, L"track")),
        ToColorRef(theme.color(L"slider", visualState, L"track")), 1);
    RECT fill = track;
    fill.right = thumbCenter;
    if (fill.right > fill.left) {
        FillRoundRect(
            dc, fill, trackHeight / 2,
            ToColorRef(theme.color(L"slider", visualState, L"fill")),
            ToColorRef(theme.color(L"slider", visualState, L"fill")), 1);
    }
    RECT thumb{thumbCenter - thumbSize / 2, centerY - thumbSize / 2, thumbCenter + (thumbSize + 1) / 2, centerY + (thumbSize + 1) / 2};
    FillThemedEllipse(
        dc,
        thumb,
        ToColorRef(theme.color(L"slider", visualState, L"thumb")),
        ToColorRef(theme.color(L"slider", visualState, L"border")),
        1);
}

void DrawButton(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    const bool focused = (draw->itemState & ODS_FOCUS) != 0;
    const bool hover = IsHover(draw->hwndItem);
    const wchar_t* state = disabled ? L"disabled" : (pressed ? L"pressed" : (hover ? L"hover" : L"normal"));

    RECT rect = draw->rcItem;
    COLORREF fill{};
    COLORREF border{};
    COLORREF textColor{};
    SetButtonDrawColors(theme, L"button", state, fill, border, textColor);
    DrawButtonFrame(theme, draw->hDC, rect, L"button", hover, pressed, focused, disabled);

    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, textColor);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(draw->hDC, font) : nullptr;
    std::wstring text = ControlText(draw->hwndItem);
    RECT textRect = ThemedControls::ButtonTextRect(theme, rect, pressed);
    TablerIconManifest::Id tablerIcon{};
    const bool hasTablerIcon = ThemedControls::ButtonTablerIcon(draw->hwndItem, tablerIcon);
    HICON icon = hasTablerIcon ? nullptr : ButtonIcon(draw->hwndItem);
    if (hasTablerIcon || icon) {
        const int iconSize = hasTablerIcon
            ? ControlScaledMetric(draw->hwndItem, theme, L"iconButton", L"iconSize", 20.0f)
            : ControlScaledMetric(draw->hwndItem, theme, L"toolbarItem", L"iconSize", 16.0f);
        const int iconGap = text.empty() ? 0
            : ControlScaledMetric(draw->hwndItem, theme, L"toolbarItem", L"iconGap", 6.0f);
        SIZE textSize{};
        if (!text.empty()) textSize.cx = ThemedD2D::MeasureTextWidth(font, text);
        const int contentWidth = iconSize + iconGap + textSize.cx;
        const int iconX = rect.left + std::max(0, (static_cast<int>(rect.right - rect.left) - contentWidth) / 2) + (pressed ? 1 : 0);
        const int iconY = rect.top + ((rect.bottom - rect.top) - iconSize) / 2 + (pressed ? 1 : 0);
        const RECT iconRect{iconX, iconY, iconX + iconSize, iconY + iconSize};
        if (hasTablerIcon) {
            const Color themedIconColor = theme.color(L"iconButton", state, L"icon");
            const COLORREF iconColor = ToColorRef(themedIconColor.a > 0.0f
                ? themedIconColor
                : theme.color(L"button", state, L"text"));
            if (!::DrawTablerIcon(draw->hDC, iconRect, {}, tablerIcon, iconColor)) {
                const int halfWidth = std::max(2, iconSize / 4);
                const int halfHeight = std::max(1, iconSize / 8);
                const int centerX = (iconRect.left + iconRect.right) / 2;
                const int centerY = (iconRect.top + iconRect.bottom) / 2;
                const POINT points[]{
                    {centerX - halfWidth, centerY - halfHeight},
                    {centerX, centerY + halfHeight},
                    {centerX + halfWidth, centerY - halfHeight},
                };
                const float stroke = static_cast<float>(std::max(
                    1,
                    ControlScaledMetric(draw->hwndItem, theme, L"miniButton", L"arrowStrokeWidth", 2.0f)));
                if (!ThemedD2D::DrawPolyline(draw->hDC, points, 3, iconColor, stroke)) {
                    ThemedGdiFallback::DrawPolyline(
                        draw->hDC, points, 3, iconColor, static_cast<int>(stroke));
                }
            }
        } else if (!ThemedD2D::DrawIcon(draw->hDC, icon, iconRect, disabled)) {
            ThemedGdiFallback::DrawIcon(draw->hDC, icon, iconRect, disabled);
        }
        if (!text.empty()) textRect.left = iconX + iconSize + iconGap;
    }
    DrawThemedText(draw->hDC, font, text.c_str(), static_cast<int>(text.size()), textRect,
        (hasTablerIcon || icon) ? (DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS)
                               : (DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS),
        GetTextColor(draw->hDC));
    if (oldFont) {
        SelectObject(draw->hDC, oldFont);
    }
}

void DrawPrimaryButton(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    const bool focused = (draw->itemState & ODS_FOCUS) != 0;
    const bool hover = IsHover(draw->hwndItem);
    const wchar_t* state = disabled ? L"disabled" : (pressed ? L"pressed" : (hover ? L"hover" : L"normal"));

    RECT rect = draw->rcItem;
    COLORREF fill{};
    COLORREF border{};
    COLORREF textColor{};
    SetButtonDrawColors(theme, L"primaryButton", state, fill, border, textColor);
    DrawButtonFrame(theme, draw->hDC, rect, L"primaryButton", hover, pressed, focused, disabled);

    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, textColor);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(draw->hDC, font) : nullptr;
    std::wstring text = ControlText(draw->hwndItem);
    RECT textRect = ThemedControls::ButtonTextRect(theme, rect, pressed);
    TablerIconManifest::Id tablerIcon{};
    const bool hasTablerIcon = ThemedControls::ButtonTablerIcon(draw->hwndItem, tablerIcon);
    if (hasTablerIcon) {
        const int iconSize = ControlScaledMetric(
            draw->hwndItem, theme, L"iconButton", L"iconSize", 20.0f);
        const int iconGap = text.empty() ? 0
            : ControlScaledMetric(draw->hwndItem, theme, L"toolbarItem", L"iconGap", 6.0f);
        SIZE textSize{};
        if (!text.empty()) textSize.cx = ThemedD2D::MeasureTextWidth(font, text);
        const int contentWidth = iconSize + iconGap + textSize.cx;
        const int iconX = rect.left + std::max(
            0, (static_cast<int>(rect.right - rect.left) - contentWidth) / 2) + (pressed ? 1 : 0);
        const int iconY = rect.top + ((rect.bottom - rect.top) - iconSize) / 2 + (pressed ? 1 : 0);
        const RECT iconRect{iconX, iconY, iconX + iconSize, iconY + iconSize};
        if (!::DrawTablerIcon(draw->hDC, iconRect, {}, tablerIcon, textColor)) {
            const int halfWidth = std::max(2, iconSize / 4);
            const int halfHeight = std::max(1, iconSize / 8);
            const int centerX = (iconRect.left + iconRect.right) / 2;
            const int centerY = (iconRect.top + iconRect.bottom) / 2;
            const POINT points[]{
                {centerX - halfWidth, centerY - halfHeight},
                {centerX, centerY + halfHeight},
                {centerX + halfWidth, centerY - halfHeight},
            };
            const float stroke = static_cast<float>(std::max(
                1,
                ControlScaledMetric(draw->hwndItem, theme, L"miniButton", L"arrowStrokeWidth", 2.0f)));
            if (!ThemedD2D::DrawPolyline(draw->hDC, points, 3, textColor, stroke)) {
                ThemedGdiFallback::DrawPolyline(
                    draw->hDC, points, 3, textColor, static_cast<int>(stroke));
            }
        }
        if (!text.empty()) textRect.left = iconX + iconSize + iconGap;
    }
    DrawThemedText(draw->hDC, font, text.c_str(), static_cast<int>(text.size()), textRect,
        hasTablerIcon ? (DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS)
                      : (DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS),
        GetTextColor(draw->hDC));
    if (oldFont) {
        SelectObject(draw->hDC, oldFont);
    }
}

void DrawMiniButton(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    const bool focused = (draw->itemState & ODS_FOCUS) != 0;
    const bool hover = IsHover(draw->hwndItem);
    const wchar_t* state = ButtonState(hover, pressed, disabled);

    RECT rect = draw->rcItem;
    ThemedControls::DrawMiniButtonFrame(theme, draw->hDC, rect, hover, pressed, focused, disabled);

    const std::wstring text = ControlText(draw->hwndItem);
    const bool down = text == L"down" || text == L"next" || text == L"v";
    const int size = ActiveScaledMetric(theme, L"miniButton", L"arrowSize", 5.0f);
    const int stroke = ActiveScaledMetric(theme, L"miniButton", L"arrowStrokeWidth", 2.0f);
    const int cy = (rect.top + rect.bottom) / 2 + (pressed ? ActiveScaledMetric(theme, L"miniButton", L"pressedOffset", 1.0f) : 0);
    const int cx = (rect.left + rect.right) / 2 + (pressed ? ActiveScaledMetric(theme, L"miniButton", L"pressedOffset", 1.0f) : 0);
    POINT points[3]{};
    if (down) {
        points[0] = POINT{cx - size, cy - 2};
        points[1] = POINT{cx, cy + 3};
        points[2] = POINT{cx + size, cy - 2};
    } else {
        points[0] = POINT{cx - size, cy + 2};
        points[1] = POINT{cx, cy - 3};
        points[2] = POINT{cx + size, cy + 2};
    }
    DrawThemedPolyline(
        draw->hDC,
        points,
        3,
        ToColorRef(theme.color(L"miniButton", state, L"icon")),
        stroke);
}

void DrawCheckBox(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const bool focused = (draw->itemState & ODS_FOCUS) != 0;
    const bool checked = IsChecked(draw->hwndItem);
    const bool hover = IsHover(draw->hwndItem);
    const wchar_t* state = disabled ? L"disabled" : (checked ? (hover ? L"checkedHover" : L"checked") : (hover ? L"hover" : L"normal"));

    RECT rect = draw->rcItem;
    const std::wstring backgroundComponent = BackgroundComponent(draw->hwndItem);
    FillThemedRect(draw->hDC, rect, ToColorRef(theme.color(backgroundComponent, L"normal", L"bg")));

    const int radius = ActiveScaledMetric(theme, L"checkbox", L"radius", 4.0f);
    RECT box = ThemedControls::CheckBoxBoxRect(theme, rect);

    FillRoundRect(
        draw->hDC,
        box,
        radius,
        ToColorRef(theme.color(L"checkbox", state, L"boxBg")),
        ToColorRef(theme.color(L"checkbox", focused ? L"focused" : state, L"border")),
        ActiveScaledMetric(theme, L"checkbox", L"borderWidth", 1.0f));

    if (checked) {
        const int boxSize = box.right - box.left;
        const int markWidth = ActiveScaledMetric(theme, L"checkbox", L"markWidth", 2.0f);
        const POINT points[]{
            POINT{box.left + boxSize / 4, box.top + boxSize / 2},
            POINT{box.left + boxSize * 7 / 16, box.bottom - boxSize / 4},
            POINT{box.right - boxSize / 5, box.top + boxSize / 4}};
        DrawThemedPolyline(
            draw->hDC,
            points,
            static_cast<int>(std::size(points)),
            ToColorRef(theme.color(L"checkbox", L"checked", L"mark")),
            markWidth);
    }

    const bool multiline = [&] { auto s = FindState(draw->hwndItem); return s && s->multiline; }();
    RECT textRect = ThemedControls::CheckBoxTextRect(theme, rect);
    if (multiline) {
        textRect.top = rect.top + ActiveScaledMetric(theme, L"checkbox", L"textOffsetY", 1.0f);
        textRect.bottom = rect.bottom;
    }
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, ToColorRef(theme.color(L"checkbox", state, L"text")));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(draw->hDC, font) : nullptr;
    std::wstring text = ControlText(draw->hwndItem);
    const UINT format = multiline
        ? (DT_LEFT | DT_WORDBREAK)
        : (DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawThemedText(draw->hDC, font, text.c_str(), static_cast<int>(text.size()), textRect, format, GetTextColor(draw->hDC));
    if (oldFont) {
        SelectObject(draw->hDC, oldFont);
    }
}

void DrawToggle(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const bool checked = IsChecked(draw->hwndItem);
    const bool hover = IsHover(draw->hwndItem);
    const wchar_t* state = disabled ? L"disabled" : (checked ? L"checked" : (hover ? L"hover" : L"normal"));
    RECT rect = draw->rcItem;
    const std::wstring backgroundComponent = BackgroundComponent(draw->hwndItem);
    FillThemedRect(draw->hDC, rect, ToColorRef(theme.color(backgroundComponent, L"normal", L"bg")));

    const int trackWidth = ActiveScaledMetric(theme, L"toggle", L"width", 38.0f);
    const int trackHeight = ActiveScaledMetric(theme, L"toggle", L"height", 24.0f);
    const int thumbSize = ActiveScaledMetric(theme, L"toggle", L"thumbSize", 18.0f);
    RECT track{rect.left, rect.top + (rect.bottom - rect.top - trackHeight) / 2, rect.left + trackWidth, 0};
    track.bottom = track.top + trackHeight;
    FillRoundRect(
        draw->hDC, track, trackHeight / 2,
        ToColorRef(theme.color(L"toggle", state, L"track")),
        ToColorRef(theme.color(L"toggle", state, L"track")), 1);
    const int inset = std::max(1, (trackHeight - thumbSize) / 2);
    RECT thumb{};
    thumb.left = checked ? track.right - inset - thumbSize : track.left + inset;
    thumb.top = track.top + inset;
    thumb.right = thumb.left + thumbSize;
    thumb.bottom = thumb.top + thumbSize;
    FillThemedEllipse(
        draw->hDC,
        thumb,
        ToColorRef(theme.color(L"toggle", state, L"thumb")),
        ToColorRef(theme.color(L"toggle", state, L"thumb")),
        0);

    const int gap = ActiveScaledMetric(theme, L"toggle", L"gap", 8.0f);
    RECT textRect{track.right + gap, rect.top, rect.right, rect.bottom};
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, ToColorRef(theme.color(L"toggle", state, L"text")));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(draw->hDC, font) : nullptr;
    const std::wstring text = ControlText(draw->hwndItem);
    DrawThemedText(draw->hDC, font, text.c_str(), static_cast<int>(text.size()), textRect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, GetTextColor(draw->hDC));
    if (oldFont) SelectObject(draw->hDC, oldFont);
}

void DrawRadioButton(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const bool checked = IsChecked(draw->hwndItem);
    const bool hover = IsHover(draw->hwndItem);
    const wchar_t* state = disabled ? L"disabled" : (checked ? L"checked" : (hover ? L"hover" : L"normal"));
    RECT rect = draw->rcItem;
    const std::wstring backgroundComponent = BackgroundComponent(draw->hwndItem);
    FillThemedRect(draw->hDC, rect, ToColorRef(theme.color(backgroundComponent, L"normal", L"bg")));

    const int dotSize = ActiveScaledMetric(theme, L"radio", L"dotSize", 16.0f);
    RECT dot{rect.left, rect.top + (rect.bottom - rect.top - dotSize) / 2, rect.left + dotSize, 0};
    dot.bottom = dot.top + dotSize;
    FillThemedEllipse(
        draw->hDC,
        dot,
        ToColorRef(theme.color(L"radio", state, L"dot")),
        ToColorRef(theme.color(L"radio", state, L"border")),
        1);
    if (checked) {
        const int innerSize = ActiveScaledMetric(theme, L"radio", L"innerDotSize", 8.0f);
        RECT inner{dot.left + (dotSize - innerSize) / 2, dot.top + (dotSize - innerSize) / 2, 0, 0};
        inner.right = inner.left + innerSize;
        inner.bottom = inner.top + innerSize;
        const COLORREF innerColor = ToColorRef(theme.color(L"radio", L"checked", L"border"));
        FillThemedEllipse(draw->hDC, inner, innerColor, innerColor, 0);
    }
    const int gap = ActiveScaledMetric(theme, L"radio", L"gap", 8.0f);
    RECT textRect{dot.right + gap, rect.top, rect.right, rect.bottom};
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, ToColorRef(theme.color(L"radio", state, L"text")));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(draw->hDC, font) : nullptr;
    const std::wstring text = ControlText(draw->hwndItem);
    DrawThemedText(draw->hDC, font, text.c_str(), static_cast<int>(text.size()), textRect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, GetTextColor(draw->hDC));
    if (oldFont) SelectObject(draw->hDC, oldFont);
}

void DrawHotKeyCapture(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const bool focused = (draw->itemState & ODS_FOCUS) != 0 || GetFocus() == draw->hwndItem;
    const bool hover = IsHover(draw->hwndItem);
    const wchar_t* state = disabled ? L"disabled" : (focused ? L"focused" : (hover ? L"hover" : L"normal"));
    RECT rect = draw->rcItem;
    FillRoundRect(
        draw->hDC,
        rect,
        ActiveScaledMetric(theme, L"edit", L"radius", 7.0f),
        ToColorRef(theme.color(L"edit", state, L"bg")),
        ToColorRef(theme.color(L"edit", state, L"border")),
        ActiveScaledMetric(theme, L"edit", L"borderWidth", 1.0f));
    InflateRect(&rect, -ThemedControls::EditPaddingX(theme), 0);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, ToColorRef(theme.color(L"edit", state, L"text")));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(draw->hDC, font) : nullptr;
    const std::wstring text = ControlText(draw->hwndItem);
    DrawThemedText(draw->hDC, font, text.c_str(), static_cast<int>(text.size()), rect,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, GetTextColor(draw->hDC));
    if (oldFont) SelectObject(draw->hDC, oldFont);
}

void DrawLinkText(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    const auto controlState = FindState(draw->hwndItem);
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    const bool focused = (draw->itemState & ODS_FOCUS) != 0;
    const bool hover = IsHover(draw->hwndItem);
    const bool visited = controlState && controlState->selected;
    const std::wstring role = controlState ? controlState->statusState : L"normal";
    const wchar_t* state = disabled ? L"disabled"
        : (pressed ? L"pressed"
        : (focused ? L"focused"
        : (hover ? L"hover"
        : (visited ? L"visited" : (role.empty() ? L"normal" : role.c_str())))));

    RECT rect = draw->rcItem;
    const std::wstring backgroundComponent = BackgroundComponent(draw->hwndItem);
    FillThemedRect(draw->hDC, rect, ToColorRef(theme.color(backgroundComponent, L"normal", L"bg")));

    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, ToColorRef(theme.color(L"link", state, L"text")));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(draw->hDC, font) : nullptr;
    const LONG style = GetWindowLongW(draw->hwndItem, GWL_STYLE);
    UINT format = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
    if ((style & BS_CENTER) == BS_CENTER) format |= DT_CENTER;
    else if ((style & BS_RIGHT) == BS_RIGHT) format |= DT_RIGHT;
    else format |= DT_LEFT;
    const std::wstring text = ControlText(draw->hwndItem);
    DrawThemedText(draw->hDC, font, text.c_str(), static_cast<int>(text.size()), rect, format, GetTextColor(draw->hDC));
    if (oldFont) SelectObject(draw->hDC, oldFont);

    if (focused) {
        RECT focusRect = rect;
        InflateRect(&focusRect, -1, -2);
        DrawRoundRect(
            draw->hDC,
            focusRect,
            ActiveScaledMetric(theme, L"checkbox", L"radius", 4.0f),
            ToColorRef(theme.color(L"link", L"focused", L"text")),
            1);
    }
}

void DrawContainer(const Theme& theme, const DRAWITEMSTRUCT* draw, ControlKind kind) {
    RECT rect = draw->rcItem;
    const wchar_t* component = kind == ControlKind::Panel ? L"panel"
        : (kind == ControlKind::GroupBox ? L"groupBox"
        : (kind == ControlKind::TabControl ? L"tabControl" : L"toolbar"));

    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const auto state = FindState(draw->hwndItem);
    const int tabAppearance = kind == ControlKind::TabControl && state ? state->tabAppearance : 0;
    const int tabContainerStyle = kind == ControlKind::TabControl && state
        ? state->tabContainerStyle : kTabContainerStyleAppearanceDefault;
    const bool verticalTabs = kind == ControlKind::TabControl && state && state->tabOrientation == kTabOrientationVertical;
    const bool forceFramed = kind == ControlKind::TabControl &&
        tabContainerStyle == kTabContainerStyleFramed;
    const bool forceBorderless = kind == ControlKind::TabControl &&
        tabContainerStyle == kTabContainerStyleBorderless;
    const wchar_t* tabContainerState = disabled ? L"disabled"
        : (tabAppearance == kTabAppearanceMinimalUnderline ? L"minimalUnderline"
        : (tabAppearance == kTabAppearanceSoftPill ? L"softPill"
        : (tabAppearance == kTabAppearanceConnectedTabs ? L"connected" : L"normal")));
    if (kind == ControlKind::TabControl && !forceFramed) {
        const wchar_t* visualState = tabContainerState;
        FillThemedRect(draw->hDC, rect, ToColorRef(theme.color(L"tabControl", visualState, L"bg")));
        if (forceBorderless || tabAppearance == kTabAppearanceSoftPill) return;
        if (tabAppearance == kTabAppearanceMinimalUnderline) {
            const int lineWidth = std::max(1, ScaledMetric(draw->hwndItem, theme, L"tabControl", L"underlineBorderWidth", 1.0f));
        const COLORREF lineColor = ToColorRef(theme.color(L"tabControl", visualState, L"border"));
        if (verticalTabs) {
            DrawThemedLine(draw->hDC, rect.right - lineWidth, rect.top, rect.right - lineWidth, rect.bottom,
                lineColor, lineWidth);
        } else {
            DrawThemedLine(draw->hDC, rect.left, rect.bottom - lineWidth, rect.right, rect.bottom - lineWidth,
                lineColor, lineWidth);
        }
        return;
        }
    }
    const wchar_t* visualState = disabled ? L"disabled"
        : (kind == ControlKind::Panel && state && !state->statusState.empty() ? state->statusState.c_str()
        : ((kind == ControlKind::GroupBox && state && state->checked) ? L"raised"
        : (kind == ControlKind::TabControl && tabAppearance == kTabAppearanceConnectedTabs ? L"connected" : L"normal")));
    const int radius = ActiveScaledMetric(theme, component, L"radius", 7.0f);
    const int borderWidth = ActiveScaledMetric(theme, component, L"borderWidth", 1.0f);
    FillRoundRect(
        draw->hDC, rect, radius,
        ToColorRef(theme.color(component, visualState, L"bg")),
        ToColorRef(theme.color(component, visualState, L"border")),
        borderWidth);
    if (kind != ControlKind::GroupBox) return;

    const std::wstring title = ControlText(draw->hwndItem);
    if (title.empty()) return;
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0));
    SIZE size = ThemedD2D::MeasureText(font, title, 0, false);
    if (size.cx <= 0 || size.cy <= 0) {
        size = ThemedGdiFallback::MeasureText(
            draw->hDC, font, title.c_str(), static_cast<int>(title.size()));
    }
    const int inset = ScaledMetric(draw->hwndItem, theme, L"groupBox", L"titleInsetX", 10.0f);
    const int gap = ScaledMetric(draw->hwndItem, theme, L"groupBox", L"titleGap", 6.0f);
    const int titleHeight = ScaledMetric(draw->hwndItem, theme, L"groupBox", L"titleHeight", 20.0f);
    const int titleInsetY = ScaledMetric(draw->hwndItem, theme, L"groupBox", L"titleInsetY", 4.0f);
    const LONG titleTop = rect.top + titleInsetY;
    RECT titleBg{
        rect.left + inset - gap / 2,
        titleTop,
        rect.left + inset + size.cx + gap / 2,
        titleTop + std::max(size.cy, static_cast<LONG>(titleHeight))};
    FillThemedRect(draw->hDC, titleBg, ToColorRef(theme.color(L"groupBox", visualState, L"bg")));
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, ToColorRef(theme.color(L"groupBox", visualState, L"text")));
    RECT titleRect{rect.left + inset, titleTop, rect.right - inset, titleBg.bottom};
    DrawThemedText(draw->hDC, font, title.c_str(), static_cast<int>(title.size()), titleRect,
        DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, GetTextColor(draw->hDC));
}

void DrawSeparator(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    RECT rect = draw->rcItem;
    const auto state = FindState(draw->hwndItem);
    const bool vertical = state && state->checked;
    const int thickness = std::max(1, ActiveScaledMetric(theme, L"separator", L"thickness", 1.0f));
    const COLORREF color = ToColorRef(theme.color(L"separator", L"normal", L"line"));
    if (vertical) {
        const int x = (rect.left + rect.right) / 2;
        DrawThemedLine(draw->hDC, x, rect.top + 3, x, rect.bottom - 3, color, thickness);
    } else {
        const int y = (rect.top + rect.bottom) / 2;
        DrawThemedLine(draw->hDC, rect.left + 3, y, rect.right - 3, y, color, thickness);
    }
}

void DrawTabButton(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const auto controlState = FindState(draw->hwndItem);
    const bool selected = controlState && controlState->selected;
    const int appearance = controlState ? controlState->tabAppearance : 0;
    const bool vertical = controlState && controlState->tabOrientation == kTabOrientationVertical;
    const bool hover = IsHover(draw->hwndItem);
    const bool focused = (draw->itemState & ODS_FOCUS) != 0;
    const wchar_t* state = nullptr;
    if (appearance == kTabAppearanceEmphasizedSegmented) {
        state = disabled ? L"emphasizedDisabled"
            : (selected ? (hover ? L"emphasizedSelectedHover" : L"emphasizedSelected")
            : (hover ? L"emphasizedHover" : (focused ? L"emphasizedFocused" : L"emphasizedNormal")));
    } else if (appearance == kTabAppearanceMinimalUnderline) {
        state = disabled ? L"minimalDisabled"
            : (selected ? (hover ? L"minimalSelectedHover" : L"minimalSelected")
            : (hover ? L"minimalHover" : (focused ? L"minimalFocused" : L"minimalNormal")));
    } else if (appearance == kTabAppearanceSoftPill) {
        state = disabled ? L"softPillDisabled"
            : (selected ? (hover ? L"softPillSelectedHover" : L"softPillSelected")
            : (hover ? L"softPillHover" : (focused ? L"softPillFocused" : L"softPillNormal")));
    } else if (appearance == kTabAppearanceConnectedTabs) {
        state = disabled ? L"connectedDisabled"
            : (selected ? (hover ? L"connectedSelectedHover" : L"connectedSelected")
            : (hover ? L"connectedHover" : (focused ? L"connectedFocused" : L"connectedNormal")));
    } else {
        state = disabled ? L"disabled"
            : (selected ? (hover ? L"selectedHover" : L"selected")
            : (hover ? L"hover" : L"normal"));
    }

    RECT rect = draw->rcItem;
    const int radius = appearance == kTabAppearanceSoftPill
        ? ScaledMetric(draw->hwndItem, theme, L"tabButton", L"softPillRadius", 14.0f)
        : ScaledMetric(draw->hwndItem, theme, L"tabButton", L"radius", 7.0f);
    const int borderWidth = ScaledMetric(draw->hwndItem, theme, L"tabButton", L"borderWidth", 1.0f);
    const wchar_t* containerState = appearance == kTabAppearanceMinimalUnderline ? L"minimalUnderline"
        : (appearance == kTabAppearanceSoftPill ? L"softPill"
        : (appearance == kTabAppearanceConnectedTabs ? L"connected" : L"normal"));
    const Color baseColor = appearance == kTabAppearanceStandard || appearance == kTabAppearanceEmphasizedSegmented
        ? theme.color(L"tabButton", L"normal", L"bg")
        : theme.color(L"tabControl", containerState, L"bg");
    FillThemedRect(draw->hDC, rect, ToColorRef(baseColor));

    if (appearance == kTabAppearanceMinimalUnderline) {
        if (hover || focused) {
            RECT hoverRect = rect;
            const int hoverInsetY = ScaledMetric(draw->hwndItem, theme, L"tabButton", L"minimalHoverInsetY", 2.0f);
            InflateRect(&hoverRect, 0, -hoverInsetY);
            FillRoundRect(
                draw->hDC,
                hoverRect,
                ScaledMetric(draw->hwndItem, theme, L"tabButton", L"minimalHoverRadius", 6.0f),
                ToColorRef(theme.color(L"tabButton", state, L"bg")),
                ToColorRef(theme.color(L"tabButton", state, L"border")),
                borderWidth);
        }
        if (selected) {
            const int thickness = std::max(1, ScaledMetric(draw->hwndItem, theme, L"tabButton", L"underlineThickness", 3.0f));
            const int indicatorInset = vertical
                ? ScaledMetric(draw->hwndItem, theme, L"tabButton", L"verticalUnderlineInset", 6.0f)
                : ScaledMetric(draw->hwndItem, theme, L"tabButton", L"underlineInset", 11.0f);
            RECT underline{};
            if (vertical) {
                underline = RECT{
                    rect.right - thickness,
                    rect.top + indicatorInset,
                    rect.right,
                    rect.bottom - indicatorInset};
            } else {
                underline = RECT{
                    rect.left + indicatorInset,
                    rect.bottom - thickness,
                    rect.right - indicatorInset,
                    rect.bottom};
            }
            FillRoundRect(
                draw->hDC,
                underline,
                std::max(1, thickness / 2),
                ToColorRef(theme.color(L"tabButton", state, L"underline")),
                ToColorRef(theme.color(L"tabButton", state, L"underline")),
                0);
        }
    } else if (theme.metric(L"tabButton", L"segmented", 0.0f) > 0.5f) {
        RECT segmentRect = rect;
        const int inset = std::max(1, ScaledMetric(draw->hwndItem, theme, L"tabButton", L"segmentInset", 1.0f));
        InflateRect(&segmentRect, -inset, -inset);
        FillRoundRect(
            draw->hDC,
            segmentRect,
            radius,
            ToColorRef(theme.color(L"tabButton", state, L"bg")),
            ToColorRef(theme.color(L"tabButton", state, L"border")),
            borderWidth);
        if (appearance == kTabAppearanceConnectedTabs && selected) {
            const int openExtent = ScaledMetric(draw->hwndItem, theme, L"tabButton", L"connectedOpenExtent", 5.0f);
            RECT opening{};
            if (vertical) {
                opening = RECT{
                    std::max(segmentRect.left, segmentRect.right - openExtent),
                    segmentRect.top + borderWidth,
                    rect.right,
                    segmentRect.bottom - borderWidth};
            } else {
                opening = RECT{
                    segmentRect.left + borderWidth,
                    std::max(segmentRect.top, segmentRect.bottom - openExtent),
                    segmentRect.right - borderWidth,
                    rect.bottom};
            }
            FillThemedRect(draw->hDC, opening, ToColorRef(theme.color(L"tabButton", state, L"bg")));
        }
    } else {
        FillRoundRect(
            draw->hDC,
            rect,
            radius,
            ToColorRef(theme.color(L"tabButton", state, L"bg")),
            ToColorRef(theme.color(L"tabButton", state, L"border")),
            borderWidth);
    }

    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, ToColorRef(theme.color(L"tabButton", state, L"text")));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(draw->hDC, font) : nullptr;
    std::wstring text = ControlText(draw->hwndItem);
    RECT textRect = ThemedControls::TabButtonTextRect(theme, rect);
    HICON icon = ButtonIcon(draw->hwndItem);
    if (icon) {
        const int iconSize = ActiveScaledMetric(theme, L"toolbarItem", L"iconSize", 16.0f);
        const int iconGap = text.empty() ? 0 : ActiveScaledMetric(theme, L"toolbarItem", L"iconGap", 6.0f);
        SIZE textSize{};
        if (!text.empty()) textSize.cx = ThemedD2D::MeasureTextWidth(font, text);
        const int contentWidth = iconSize + iconGap + textSize.cx;
        const int iconX = rect.left + std::max(0, (static_cast<int>(rect.right - rect.left) - contentWidth) / 2);
        const int iconY = rect.top + ((rect.bottom - rect.top) - iconSize) / 2;
        const RECT iconRect{iconX, iconY, iconX + iconSize, iconY + iconSize};
        if (!ThemedD2D::DrawIcon(draw->hDC, icon, iconRect, disabled)) {
            ThemedGdiFallback::DrawIcon(draw->hDC, icon, iconRect, disabled);
        }
        if (!text.empty()) textRect.left = iconX + iconSize + iconGap;
    }
    DrawThemedText(draw->hDC, font, text.c_str(), static_cast<int>(text.size()), textRect,
        icon ? (DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS)
             : (DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS),
        GetTextColor(draw->hDC));
    if (oldFont) {
        SelectObject(draw->hDC, oldFont);
    }
}

void DrawComboBox(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    RECT rect = draw->rcItem;
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const bool selected = (draw->itemState & ODS_SELECTED) != 0;
    const bool focused = (draw->itemState & ODS_FOCUS) != 0;
    const wchar_t* state = disabled ? L"disabled" : (selected ? L"selected" : (focused ? L"focused" : L"normal"));
    FillThemedRect(draw->hDC, rect, ToColorRef(theme.color(L"comboBox", state, L"itemBg")));

    if (draw->itemID == static_cast<UINT>(-1)) {
        return;
    }
    const std::wstring text = ComboBoxItemText(draw->hwndItem, draw->itemID);
    RECT textRect = ThemedControls::ComboBoxItemTextRect(theme, rect);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, ToColorRef(theme.color(L"comboBox", state, L"text")));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(draw->hDC, font) : nullptr;
    DrawThemedText(draw->hDC, font, text.c_str(), static_cast<int>(text.size()), textRect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, GetTextColor(draw->hDC));
    if (oldFont) {
        SelectObject(draw->hDC, oldFont);
    }
}

void DrawListBox(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    RECT rect = draw->rcItem;
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const bool selected = (draw->itemState & ODS_SELECTED) != 0;
    const bool focused = (draw->itemState & ODS_FOCUS) != 0;
    const wchar_t* state = disabled ? L"disabled" : (selected ? L"selected" : (focused ? L"focused" : L"normal"));

    FillThemedRect(draw->hDC, rect, ToColorRef(theme.color(L"listItem", state, L"bg")));

    if (draw->itemID == static_cast<UINT>(-1)) {
        return;
    }

    wchar_t buffer[1024]{};
    SendMessageW(draw->hwndItem, LB_GETTEXT, draw->itemID, reinterpret_cast<LPARAM>(buffer));
    RECT textRect = ThemedControls::ListItemTextRect(theme, rect);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, ToColorRef(theme.color(L"listItem", state, L"text")));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0));
    DrawThemedText(draw->hDC, font, buffer, -1, textRect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, GetTextColor(draw->hDC));
}

std::wstring ClassName(HWND hwnd) {
    wchar_t buffer[128]{};
    GetClassNameW(hwnd, buffer, static_cast<int>(sizeof(buffer) / sizeof(buffer[0])));
    return buffer;
}

bool TableShowsRowGridLines(HWND table) {
    auto state = FindState(table);
    return state && state->kind == ControlKind::Table && state->tableShowRowGridLines;
}

bool TableShowsColumnGridLines(HWND table) {
    auto state = FindState(table);
    return state && state->kind == ControlKind::Table && state->tableShowColumnGridLines;
}

bool TableShowsDefaultHeaderGridLine(HWND table) {
    auto state = FindState(table);
    return state && state->kind == ControlKind::Table;
}

void DrawTableGridLines(const Theme& theme, HDC dc, RECT rect, bool rowLine, bool columnLine) {
    if (!rowLine && !columnLine) {
        return;
    }
    const int width = std::max(1, ActiveScaledMetric(theme, L"table", L"gridWidth", 1.0f));
    const COLORREF color = ToColorRef(theme.color(L"table", L"normal", L"grid"));
    if (rowLine) {
        DrawThemedLine(dc, rect.left, rect.bottom - 1, rect.right, rect.bottom - 1, color, width);
    }
    if (columnLine) {
        DrawThemedLine(dc, rect.right - 1, rect.top, rect.right - 1, rect.bottom, color, width);
    }
}

void DrawTableCellGridLines(const Theme& theme, HWND table, const NMLVCUSTOMDRAW* draw, RECT rect) {
    const bool rowLine = TableShowsRowGridLines(table);
    DrawTableGridLines(theme, draw->nmcd.hdc, rect, rowLine, false);
    if (!TableShowsColumnGridLines(table) || draw->iSubItem <= 0) {
        return;
    }
    const int width = std::max(1, TableScaledMetric(table, theme, L"table", L"gridWidth", 1.0f));
    const RECT lineRect{rect.left, rect.top, rect.left + width, rect.bottom};
    FillThemedRect(draw->nmcd.hdc, lineRect, ToColorRef(theme.color(L"table", L"normal", L"grid")));
}

void DrawTableActiveRowFrame(
    const Theme& theme,
    HWND table,
    const NMLVCUSTOMDRAW* draw,
    RECT cellRect) {
    const int row = static_cast<int>(draw->nmcd.dwItemSpec);
    if (!ThemedControls::IsTableRowActive(table, row)) return;
    const int column = draw->iSubItem;
    const int columnCount = Header_GetItemCount(ListView_GetHeader(table));
    const int width = std::max(1, TableScaledMetric(
        table, theme, L"table", L"activeRowBorderWidth", 2.0f));
    const COLORREF color = ToColorRef(theme.color(L"table", L"activeRow", L"border"));
    DrawThemedLine(draw->nmcd.hdc, cellRect.left, cellRect.top, cellRect.right, cellRect.top, color, width);
    DrawThemedLine(draw->nmcd.hdc, cellRect.left, cellRect.bottom - 1, cellRect.right, cellRect.bottom - 1, color, width);
    if (column == 0) {
        DrawThemedLine(draw->nmcd.hdc, cellRect.left, cellRect.top, cellRect.left, cellRect.bottom, color, width);
    }
    if (column == columnCount - 1) {
        DrawThemedLine(draw->nmcd.hdc, cellRect.right - 1, cellRect.top, cellRect.right - 1, cellRect.bottom, color, width);
    }
}

void DrawHeaderItem(const Theme& theme, HWND header, const NMCUSTOMDRAW* draw) {
    const bool table = KindFor(GetParent(header)) == ControlKind::Table;
    const wchar_t* component = table ? L"tableHeader" : L"list";
    HWND tableHwnd = table ? GetParent(header) : nullptr;
    const int index = static_cast<int>(draw->dwItemSpec);
    const int itemCount = Header_GetItemCount(header);
    RECT rect = draw->rc;
    FillThemedRect(draw->hdc, rect, ToColorRef(theme.color(component, L"normal", L"bg")));

    wchar_t text[256]{};
    HDITEMW item{};
    item.mask = HDI_TEXT | HDI_FORMAT;
    item.pszText = text;
    item.cchTextMax = static_cast<int>(sizeof(text) / sizeof(text[0]));
    Header_GetItem(header, index, &item);

    if (!table || TableShowsDefaultHeaderGridLine(tableHwnd) ||
        TableShowsRowGridLines(tableHwnd) || TableShowsColumnGridLines(tableHwnd)) {
        const COLORREF line = table
            ? ToColorRef(theme.color(L"table", L"normal", L"grid"))
            : ToColorRef(theme.color(component, L"normal", L"border"));
        if (!table || TableShowsDefaultHeaderGridLine(tableHwnd) || TableShowsRowGridLines(tableHwnd)) {
            DrawThemedLine(draw->hdc, rect.left, rect.bottom - 1, rect.right, rect.bottom - 1, line, 1);
        }
        if ((!table || TableShowsDefaultHeaderGridLine(tableHwnd) || TableShowsColumnGridLines(tableHwnd)) &&
            index >= 0 &&
            index < itemCount - 1) {
            DrawThemedLine(draw->hdc, rect.right - 1, rect.top, rect.right - 1, rect.bottom, line, 1);
        }
    }

    const int paddingX = table
        ? TableScaledMetric(tableHwnd, theme, L"listItem", L"paddingX", 8.0f)
        : ScaledMetric(header, theme, L"listItem", L"paddingX", 8.0f);
    RECT textRect = rect;
    textRect.left += paddingX;
    textRect.right -= paddingX;
    UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
    if ((item.fmt & HDF_CENTER) == HDF_CENTER) {
        format = (format & ~DT_LEFT) | DT_CENTER;
    } else if ((item.fmt & HDF_RIGHT) == HDF_RIGHT) {
        format = (format & ~DT_LEFT) | DT_RIGHT;
    }

    SetBkMode(draw->hdc, TRANSPARENT);
    SetTextColor(draw->hdc, ToColorRef(theme.color(component, L"normal", L"text")));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(header, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(draw->hdc, font) : nullptr;
    DrawThemedText(draw->hdc, font, text, -1, textRect, format, GetTextColor(draw->hdc));
    if (oldFont) {
        SelectObject(draw->hdc, oldFont);
    }
}

std::optional<ThemedControls::TableCellRuntime> TableCellAt(HWND table, int row, int column) {
    auto state = FindState(table);
    if (!state || row < 0 || column < 0) {
        return std::nullopt;
    }
    const std::size_t rowIndex = static_cast<std::size_t>(row);
    const std::size_t columnIndex = static_cast<std::size_t>(column);
    if (rowIndex >= state->tableCells.size() || columnIndex >= state->tableCells[rowIndex].size()) {
        return std::nullopt;
    }
    return state->tableCells[rowIndex][columnIndex];
}

bool IsActionCell(const ThemedControls::TableCellRuntime& cell) {
    return cell.role == 1 || cell.role == 2;
}

bool UsesNativeTableCellDrawing(HWND, int column, const ThemedControls::TableCellRuntime* cell) {
    // Image-bearing rows must stay on the shared draw path. Native ListView
    // drawing also paints the transparent state image used only for checkbox
    // hit testing, which turns that reserved area into an opaque block on some
    // systems and bypasses the themed checkbox/row-state rendering.
    (void)column;
    (void)cell;
    return false;
}

bool UsesNativeTableRowDrawing(HWND table, int row) {
    const auto firstCell = TableCellAt(table, row, 0);
    return UsesNativeTableCellDrawing(table, 0, firstCell ? &*firstCell : nullptr);
}

bool IsTableDetailsView(HWND table) {
    if (!table) {
        return true;
    }
    const LONG_PTR style = GetWindowLongPtrW(table, GWL_STYLE);
    return (style & LVS_TYPEMASK) == LVS_REPORT;
}

bool IsTableRowHovered(HWND table, int row) {
    const auto state = FindState(table);
    return state && state->tableHotRow == row;
}

bool IsTableCellHovered(HWND table, int row, int column) {
    const auto state = FindState(table);
    return state && state->tableHotRow == row && state->tableHotColumn == column;
}

bool IsTableCellPressed(HWND table, int row, int column) {
    const auto state = FindState(table);
    return state && state->tablePressedRow == row && state->tablePressedColumn == column;
}

const wchar_t* TableRowState(int row, bool selected, bool enabled, bool hovered) {
    if (!enabled) {
        return L"disabled";
    }
    if (selected) {
        return L"selected";
    }
    if (hovered) {
        return L"hover";
    }
    return (row % 2) == 0 ? L"alternate" : L"normal";
}

COLORREF TableRowBackground(const Theme& theme, const wchar_t* state) {
    if (std::wcscmp(state, L"disabled") == 0) {
        return ToColorRef(theme.color(L"table", L"disabled", L"bg"));
    }
    if (std::wcscmp(state, L"focused") == 0) {
        return ToColorRef(theme.color(L"listItem", L"selected", L"bg"));
    }
    return ToColorRef(theme.color(L"listItem", state, L"bg"));
}

std::wstring ListViewCellText(HWND table, int row, int column) {
    wchar_t buffer[512]{};
    ListView_GetItemText(table, row, column, buffer, static_cast<int>(sizeof(buffer) / sizeof(buffer[0])));
    return buffer;
}

RECT TableSubItemRect(HWND table, int row, int column, RECT fallback) {
    RECT rect = fallback;
    RECT rowRect{};
    if (!ListView_GetItemRect(table, row, &rowRect, LVIR_BOUNDS)) {
        rowRect = fallback;
    }

    int left = 0;
    int right = 0;
    if (HWND header = ListView_GetHeader(table)) {
        RECT headerRect{};
        if (Header_GetItemRect(header, column, &headerRect)) {
            POINT leftTop{headerRect.left, 0};
            POINT rightTop{headerRect.right, 0};
            MapWindowPoints(header, table, &leftTop, 1);
            MapWindowPoints(header, table, &rightTop, 1);
            left = leftTop.x;
            right = rightTop.x;
        }
    }

    if (right <= left) {
        for (int i = 0; i < column; ++i) {
            left += ListView_GetColumnWidth(table, i);
        }
        right = left + ListView_GetColumnWidth(table, column);
    }

    if (right > left) {
        rect.left = left;
        rect.right = right;
        rect.top = rowRect.top;
        rect.bottom = rowRect.bottom;
        return rect;
    }

    RECT subItem{LVIR_BOUNDS, column, 0, 0};
    return ListView_GetSubItemRect(table, row, column, LVIR_BOUNDS, &subItem) ? subItem : rect;
}

void DrawTableActionCell(
    const Theme& theme,
    HWND table,
    const NMLVCUSTOMDRAW* draw,
    const ThemedControls::TableCellRuntime& cell) {
    const int row = static_cast<int>(draw->nmcd.dwItemSpec);
    const int column = draw->iSubItem;
    RECT cellRect = TableSubItemRect(table, row, column, draw->nmcd.rc);
    const bool enabled = ThemedControls::IsTableRowEnabled(table, row);
    const bool selected = ThemedControls::IsTableRowSelected(table, row);
    const bool hovered = IsTableRowHovered(table, row);
    const wchar_t* rowState = TableRowState(row, selected, enabled, hovered);
    FillThemedRect(draw->nmcd.hdc, cellRect, TableRowBackground(theme, rowState));

    const std::wstring text = ListViewCellText(table, row, column);
    SIZE textSize{};
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(table, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(draw->nmcd.hdc, font) : nullptr;
    if (!text.empty()) {
        textSize.cx = ThemedD2D::MeasureTextWidth(font, text);
    }

    const int paddingX = ThemedControls::ButtonPaddingX(theme);
    const int cellWidth = static_cast<int>(cellRect.right - cellRect.left);
    const int cellHeight = static_cast<int>(cellRect.bottom - cellRect.top);
    const int textWidth = static_cast<int>(textSize.cx);
    const int buttonHeight = std::min(
        std::max(1, cellHeight - 4),
        ThemedControls::CompactButtonHeight(theme));
    const int desiredWidth = std::max(
        ActiveScaledMetric(theme, L"button", L"minWidth", 64.0f),
        textWidth + paddingX * 2);
    const int buttonWidth = std::min(std::max(1, cellWidth - 4), desiredWidth);
    RECT buttonRect{
        cellRect.left + std::max(0, (cellWidth - buttonWidth) / 2),
        cellRect.top + std::max(0, (cellHeight - buttonHeight) / 2),
        0,
        0};
    buttonRect.right = buttonRect.left + buttonWidth;
    buttonRect.bottom = buttonRect.top + buttonHeight;

    const bool hot = enabled && IsTableCellHovered(table, row, column);
    const bool pressed = hot && IsTableCellPressed(table, row, column);
    const bool buttonFocused = enabled && GetFocus() == table && selected;
    const wchar_t* component = cell.role == 2 ? L"primaryButton" : L"button";
    DrawButtonFrame(theme, draw->nmcd.hdc, buttonRect, component, hot, pressed, buttonFocused, !enabled);

    SetBkMode(draw->nmcd.hdc, TRANSPARENT);
    const wchar_t* buttonState = ButtonState(hot, pressed, !enabled);
    SetTextColor(draw->nmcd.hdc, ToColorRef(theme.color(component, buttonState, L"text")));
    RECT textRect = ThemedControls::ButtonTextRect(theme, buttonRect, pressed);
    DrawThemedText(
        draw->nmcd.hdc,
        font,
        text.c_str(),
        static_cast<int>(text.size()),
        textRect,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
        GetTextColor(draw->nmcd.hdc));
    if (oldFont) {
        SelectObject(draw->nmcd.hdc, oldFont);
    }
    DrawTableCellGridLines(theme, table, draw, cellRect);
    DrawTableActiveRowFrame(theme, table, draw, cellRect);
}

void DrawTableTextCell(
    const Theme& theme,
    HWND table,
    const NMLVCUSTOMDRAW* draw) {
    const int row = static_cast<int>(draw->nmcd.dwItemSpec);
    const int column = draw->iSubItem;
    RECT cellRect = TableSubItemRect(table, row, column, draw->nmcd.rc);
    const bool enabled = ThemedControls::IsTableRowEnabled(table, row);
    const bool selected = ThemedControls::IsTableRowSelected(table, row);
    const bool hovered = IsTableRowHovered(table, row);
    const wchar_t* rowState = TableRowState(row, selected, enabled, hovered);
    FillThemedRect(draw->nmcd.hdc, cellRect, TableRowBackground(theme, rowState));

    const int paddingX = TableScaledMetric(table, theme, L"listItem", L"paddingX", 8.0f);
    RECT textRect = cellRect;
    textRect.left += paddingX;
    textRect.right -= paddingX;

    if (column == 0 && (ListView_GetExtendedListViewStyle(table) & LVS_EX_CHECKBOXES) != 0) {
        const bool checked = ListView_GetCheckState(table, row) != FALSE;
        const bool focused = GetFocus() == table && selected;
        const wchar_t* checkState = !enabled
            ? L"disabled"
            : (checked ? (hovered ? L"checkedHover" : L"checked") : (hovered ? L"hover" : L"normal"));
        const int boxSize = TableScaledMetric(table, theme, L"checkbox", L"boxSize", 16.0f);
        const int gap = TableScaledMetric(table, theme, L"checkbox", L"gap", 8.0f);
        RECT box{
            textRect.left,
            cellRect.top + std::max(0, (static_cast<int>(cellRect.bottom - cellRect.top) - boxSize) / 2),
            0,
            0};
        box.right = box.left + boxSize;
        box.bottom = box.top + boxSize;
        FillRoundRect(
            draw->nmcd.hdc,
            box,
            TableScaledMetric(table, theme, L"checkbox", L"radius", 4.0f),
            ToColorRef(theme.color(L"checkbox", checkState, L"boxBg")),
            ToColorRef(theme.color(L"checkbox", focused ? L"focused" : checkState, L"border")),
            TableScaledMetric(table, theme, L"checkbox", L"borderWidth", 1.0f));
        if (checked) {
            const int markWidth = TableScaledMetric(table, theme, L"checkbox", L"markWidth", 2.0f);
            const POINT points[]{
                POINT{box.left + boxSize / 4, box.top + boxSize / 2},
                POINT{box.left + boxSize * 7 / 16, box.bottom - boxSize / 4},
                POINT{box.right - boxSize / 5, box.top + boxSize / 4}};
            DrawThemedPolyline(
                draw->nmcd.hdc,
                points,
                3,
                ToColorRef(theme.color(L"checkbox", L"checked", L"mark")),
                markWidth);
        }
        textRect.left = box.right + gap;
    }

    const auto cell = TableCellAt(table, row, column);
    if (column == 0 && cell && cell->hasImage) {
        LVITEMW item{};
        item.mask = LVIF_IMAGE;
        item.iItem = row;
        HIMAGELIST images = ListView_GetImageList(table, LVSIL_SMALL);
        if (images && ListView_GetItem(table, &item) && item.iImage >= 0) {
            int imageWidth = 0;
            int imageHeight = 0;
            if (ImageList_GetIconSize(images, &imageWidth, &imageHeight) && imageWidth > 0 && imageHeight > 0) {
                const int availableHeight = static_cast<int>(cellRect.bottom - cellRect.top);
                const int imageY = cellRect.top + std::max(0, (availableHeight - imageHeight) / 2);
                HICON icon = ImageList_GetIcon(images, item.iImage, ILD_TRANSPARENT);
                if (icon) {
                    const RECT iconRect{
                        textRect.left, imageY, textRect.left + imageWidth, imageY + imageHeight};
                    if (!ThemedD2D::DrawIcon(draw->nmcd.hdc, icon, iconRect, !enabled)) {
                        ThemedGdiFallback::DrawIcon(draw->nmcd.hdc, icon, iconRect, !enabled);
                    }
                    DestroyIcon(icon);
                }
                textRect.left += imageWidth +
                    TableScaledMetric(table, theme, L"global", L"denseGap", 4.0f);
            }
        }
    }

    LVCOLUMNW columnInfo{};
    columnInfo.mask = LVCF_FMT;
    ListView_GetColumn(table, column, &columnInfo);
    UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
    if ((columnInfo.fmt & LVCFMT_CENTER) == LVCFMT_CENTER) {
        format = (format & ~DT_LEFT) | DT_CENTER;
    } else if ((columnInfo.fmt & LVCFMT_RIGHT) == LVCFMT_RIGHT) {
        format = (format & ~DT_LEFT) | DT_RIGHT;
    }

    const std::wstring text = ListViewCellText(table, row, column);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(table, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(draw->nmcd.hdc, font) : nullptr;
    SetBkMode(draw->nmcd.hdc, TRANSPARENT);
    const wchar_t* textState = enabled ? rowState : L"disabled";
    SetTextColor(draw->nmcd.hdc, ToColorRef(theme.color(L"listItem", textState, L"text")));
    if (cell && !cell->secondaryText.empty()) {
        const int textHeight = TableScaledMetric(table, theme, L"listItem", L"textHeight", 20.0f);
        const int denseGap = TableScaledMetric(table, theme, L"global", L"denseGap", 4.0f);
        const int contentHeight = textHeight * 2 + denseGap;
        const int top = textRect.top + std::max(0, (static_cast<int>(textRect.bottom - textRect.top) - contentHeight) / 2);
        RECT primaryRect{textRect.left, top, textRect.right, top + textHeight};
        RECT secondaryRect{textRect.left, primaryRect.bottom + denseGap, textRect.right, primaryRect.bottom + denseGap + textHeight};
        UINT lineFormat = (format & ~(DT_VCENTER | DT_BOTTOM)) | DT_TOP;
        DrawThemedText(
            draw->nmcd.hdc, font, text.c_str(), static_cast<int>(text.size()),
            primaryRect, lineFormat, GetTextColor(draw->nmcd.hdc));
        const COLORREF secondaryColor = ToColorRef(theme.color(
            L"text", enabled ? L"muted" : L"disabled", L"text"));
        DrawThemedText(
            draw->nmcd.hdc, font, cell->secondaryText.c_str(),
            static_cast<int>(cell->secondaryText.size()), secondaryRect,
            lineFormat, secondaryColor);
    } else {
        DrawThemedText(
            draw->nmcd.hdc,
            font,
            text.c_str(),
            static_cast<int>(text.size()),
            textRect,
            format,
            GetTextColor(draw->nmcd.hdc));
    }
    if (oldFont) {
        SelectObject(draw->nmcd.hdc, oldFont);
    }
    DrawTableCellGridLines(theme, table, draw, cellRect);
    DrawTableActiveRowFrame(theme, table, draw, cellRect);
}

void DrawTableRowCells(
    const Theme& theme,
    HWND table,
    const NMLVCUSTOMDRAW* draw) {
    HWND header = ListView_GetHeader(table);
    const int columnCount = header ? Header_GetItemCount(header) : 0;
    for (int column = 0; column < columnCount; ++column) {
        NMLVCUSTOMDRAW cellDraw = *draw;
        cellDraw.iSubItem = column;
        const int row = static_cast<int>(draw->nmcd.dwItemSpec);
        const auto cell = TableCellAt(table, row, column);
        if (cell && IsActionCell(*cell)) {
            DrawTableActionCell(theme, table, &cellDraw, *cell);
        } else {
            DrawTableTextCell(theme, table, &cellDraw);
        }
    }
}

void DrawTableIconTile(
    const Theme& theme,
    HWND table,
    const NMLVCUSTOMDRAW* draw) {
    const int row = static_cast<int>(draw->nmcd.dwItemSpec);
    RECT itemRect{};
    if (!ListView_GetItemRect(table, row, &itemRect, LVIR_BOUNDS)) {
        itemRect = draw->nmcd.rc;
    }

    const bool enabled = ThemedControls::IsTableRowEnabled(table, row);
    const bool selected = ThemedControls::IsTableRowSelected(table, row);
    const bool hovered = IsTableRowHovered(table, row);
    const wchar_t* rowState = TableRowState(row, selected, enabled, hovered);
    FillThemedRect(draw->nmcd.hdc, itemRect, TableRowBackground(theme, rowState));

    const int paddingX = TableScaledMetric(table, theme, L"listItem", L"paddingX", 8.0f);
    const int paddingY = TableScaledMetric(table, theme, L"listItem", L"paddingY", 8.0f);
    const int gap = TableScaledMetric(table, theme, L"global", L"denseGap", 4.0f);

    RECT contentRect = itemRect;
    contentRect.left += paddingX;
    contentRect.right -= paddingX;
    contentRect.top += paddingY;
    contentRect.bottom -= paddingY;

    const auto cell = TableCellAt(table, row, 0);
    if (ListView_GetExtendedListViewStyle(table) & LVS_EX_CHECKBOXES) {
        const bool checked = ListView_GetCheckState(table, row) != FALSE;
        const bool focused = GetFocus() == table && selected;
        const wchar_t* checkState = !enabled
            ? L"disabled"
            : (checked ? (hovered ? L"checkedHover" : L"checked") : (hovered ? L"hover" : L"normal"));
        const int boxSize = TableScaledMetric(table, theme, L"checkbox", L"boxSize", 16.0f);
        RECT box{
            contentRect.left,
            contentRect.top,
            contentRect.left + boxSize,
            contentRect.top + boxSize};
        FillRoundRect(
            draw->nmcd.hdc,
            box,
            TableScaledMetric(table, theme, L"checkbox", L"radius", 4.0f),
            ToColorRef(theme.color(L"checkbox", checkState, L"boxBg")),
            ToColorRef(theme.color(L"checkbox", focused ? L"focused" : checkState, L"border")),
            TableScaledMetric(table, theme, L"checkbox", L"borderWidth", 1.0f));
        if (checked) {
            const int markWidth = TableScaledMetric(table, theme, L"checkbox", L"markWidth", 2.0f);
            const POINT points[]{
                POINT{box.left + boxSize / 4, box.top + boxSize / 2},
                POINT{box.left + boxSize * 7 / 16, box.bottom - boxSize / 4},
                POINT{box.right - boxSize / 5, box.top + boxSize / 4}};
            DrawThemedPolyline(
                draw->nmcd.hdc,
                points,
                3,
                ToColorRef(theme.color(L"checkbox", L"checked", L"mark")),
                markWidth);
        }
        contentRect.left = box.right + gap;
    }

    const std::wstring text = ListViewCellText(table, row, 0);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(table, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(draw->nmcd.hdc, font) : nullptr;

    if (cell && cell->hasImage) {
        LVITEMW lvItem{};
        lvItem.mask = LVIF_IMAGE;
        lvItem.iItem = row;
        HIMAGELIST images = ListView_GetImageList(table, LVSIL_NORMAL);
        if (images && ListView_GetItem(table, &lvItem) && lvItem.iImage >= 0) {
            int imageWidth = 0;
            int imageHeight = 0;
            if (ImageList_GetIconSize(images, &imageWidth, &imageHeight) && imageWidth > 0 && imageHeight > 0) {
                const int contentWidth = static_cast<int>(contentRect.right - contentRect.left);
                const int iconX = static_cast<int>(contentRect.left) + std::max(0, (contentWidth - imageWidth) / 2);
                const int iconY = static_cast<int>(contentRect.top);
                if (HICON icon = ImageList_GetIcon(images, lvItem.iImage, ILD_TRANSPARENT)) {
                    const RECT iconRect{iconX, iconY, iconX + imageWidth, iconY + imageHeight};
                    if (!ThemedD2D::DrawIcon(draw->nmcd.hdc, icon, iconRect, !enabled)) {
                        ThemedGdiFallback::DrawIcon(draw->nmcd.hdc, icon, iconRect, !enabled);
                    }
                    DestroyIcon(icon);
                }
                contentRect.top = iconY + imageHeight + gap;
            }
        }
    }

    SetBkMode(draw->nmcd.hdc, TRANSPARENT);
    SetTextColor(draw->nmcd.hdc, ToColorRef(theme.color(L"listItem", enabled ? rowState : L"disabled", L"text")));
    DrawThemedText(
        draw->nmcd.hdc,
        font,
        text.c_str(),
        static_cast<int>(text.size()),
        contentRect,
        DT_CENTER | DT_TOP | DT_WORDBREAK | DT_WORD_ELLIPSIS,
        GetTextColor(draw->nmcd.hdc));
    if (oldFont) {
        SelectObject(draw->nmcd.hdc, oldFont);
    }
    DrawTableCellGridLines(theme, table, draw, itemRect);
}
}

namespace ThemedControls {

COLORREF ListSurfaceColor(const Theme& theme) {
    return ToColorRef(theme.color(L"list", L"normal", L"bg"));
}

bool IsControlHovered(HWND hwnd) {
    return IsHover(hwnd);
}

namespace {
constexpr int kDialogFontPx = 14;
constexpr int kSingleLineEditFontPx = 14;

int RenderMetric(const Theme& theme, const wchar_t* component, const wchar_t* name, float fallback) {
    return ThemedD2D::ScaleDip(
        static_cast<int>(theme.metric(component, name, fallback)),
        ThemedD2D::ActiveDpi());
}

HFONT CreateThemeFontPx(int pixelHeight) {
    return CreateFontW(
        -pixelHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

RECT TextRectFromMetrics(const Theme& theme, const wchar_t* component, RECT frame, float fallbackHeight, bool center, bool pressed = false) {
    const int paddingX = RenderMetric(theme, component, L"paddingX", 0.0f);
    const int textHeight = RenderMetric(theme, component, L"textHeight", fallbackHeight);
    const int offsetY = RenderMetric(theme, component, L"textOffsetY", 0.0f);
    const int pressedOffset = pressed ? RenderMetric(theme, component, L"pressedOffset", 1.0f) : 0;
    RECT rect{};
    rect.left = frame.left + paddingX + pressedOffset;
    rect.right = frame.right - paddingX + pressedOffset;
    if (center) {
        rect.top = frame.top + ((frame.bottom - frame.top) - textHeight) / 2 + offsetY + pressedOffset;
    } else {
        rect.top = frame.top + offsetY + pressedOffset;
    }
    rect.bottom = rect.top + textHeight;
    return rect;
}
}

int ButtonHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"button", L"height", 28.0f));
}

int CompactButtonHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"button", L"compactHeight", 28.0f));
}

int MiniButtonHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"miniButton", L"height", 24.0f));
}

void DrawIconButtonFrame(
    const Theme& theme,
    HDC dc,
    RECT rect,
    bool hover,
    bool pressed,
    bool focused,
    bool disabled) {
    const wchar_t* state = ButtonState(hover, pressed, disabled);
    const int radius = RenderMetric(theme, L"iconButton", L"radius", 7.0f);
    const COLORREF border = focused
        ? ToColorRef(theme.color(L"button", L"focused", L"border"))
        : ToColorRef(theme.color(L"panel", L"normal", L"border"));
    FillRoundRect(
        dc,
        rect,
        radius,
        ToColorRef(theme.color(L"iconButton", state, L"bg")),
        border,
        RenderMetric(theme, L"iconButton", L"borderWidth", 1.0f));
}

void DrawMiniButtonFrame(
    const Theme& theme,
    HDC dc,
    RECT rect,
    bool hover,
    bool pressed,
    bool focused,
    bool disabled) {
    const wchar_t* state = ButtonState(hover, pressed, disabled);
    FillRoundRect(
        dc,
        rect,
        RenderMetric(theme, L"miniButton", L"radius", 6.0f),
        ToColorRef(theme.color(L"miniButton", state, L"bg")),
        ToColorRef(theme.color(L"miniButton", focused ? L"focused" : state, L"border")),
        RenderMetric(theme, L"miniButton", L"borderWidth", 1.0f));
}

int ButtonPaddingX(const Theme& theme) {
    return static_cast<int>(theme.metric(L"button", L"paddingX", 12.0f));
}

int ButtonTextHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"button", L"textHeight", 20.0f));
}

RECT ButtonTextRect(const Theme& theme, RECT frame, bool pressed) {
    return TextRectFromMetrics(theme, L"button", frame, 20.0f, true, pressed);
}

int CheckBoxHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"checkbox", L"height", 24.0f));
}

int ToggleHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"toggle", L"height", 24.0f));
}

int RadioButtonHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"radio", L"height", 24.0f));
}

RECT CheckBoxBoxRect(const Theme& theme, RECT frame) {
    const int boxSize = RenderMetric(theme, L"checkbox", L"boxSize", 16.0f);
    const int offsetX = RenderMetric(theme, L"checkbox", L"boxOffsetX", 0.0f);
    const int offsetY = RenderMetric(theme, L"checkbox", L"boxOffsetY", 0.0f);
    RECT box{};
    box.left = frame.left + offsetX;
    box.top = frame.top + ((frame.bottom - frame.top) - boxSize) / 2 + offsetY;
    box.right = box.left + boxSize;
    box.bottom = box.top + boxSize;
    return box;
}

RECT CheckBoxTextRect(const Theme& theme, RECT frame) {
    const RECT box = CheckBoxBoxRect(theme, frame);
    const int gap = RenderMetric(theme, L"checkbox", L"gap", 8.0f);
    RECT textRect = TextRectFromMetrics(theme, L"checkbox", frame, 20.0f, true);
    textRect.left = box.right + gap;
    return textRect;
}

int TabButtonHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"tabButton", L"height", 28.0f));
}

RECT TabButtonTextRect(const Theme& theme, RECT frame) {
    RECT rect = TextRectFromMetrics(theme, L"tabButton", frame, 20.0f, true);
    const int width = frame.right - frame.left;
    const int minTextWidth = RenderMetric(theme, L"tabButton", L"minTextWidth", 18.0f);
    if (width > 0 && (rect.right - rect.left) < minTextWidth) {
        const int paddingX = std::max(2, (width - minTextWidth) / 2);
        rect.left = frame.left + paddingX;
        rect.right = frame.right - paddingX;
    }
    return rect;
}

RECT TabGroupInnerRect(const Theme& theme, RECT frame) {
    const int padding = RenderMetric(theme, L"tabButton", L"groupPadding", 3.0f);
    RECT rect = frame;
    InflateRect(&rect, -padding, -padding);
    return rect;
}

int ComboBoxHeight(const Theme& theme) {
    return std::max(1, EditFrameHeight(theme));
}

int ComboBoxItemHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"comboBox", L"itemHeight", 24.0f));
}

int ComboBoxDropdownHeight(const Theme& theme, UINT dpi) {
    const int frameHeight = ThemedD2D::ScaleDip(EditFrameHeight(theme), dpi);
    const int itemHeight = ThemedD2D::ScaleDip(ComboBoxItemHeight(theme), dpi);
    return std::max(frameHeight, frameHeight + itemHeight * 6);
}

int ComboBoxContentWidth(const Theme& theme, int textWidth) {
    const int paddingX = static_cast<int>(theme.metric(L"comboBox", L"paddingX", 9.0f));
    const int arrowWidth = static_cast<int>(theme.metric(L"comboBox", L"arrowWidth", 28.0f));
    return textWidth + paddingX * 2 + arrowWidth;
}

RECT ComboBoxItemTextRect(const Theme& theme, RECT frame) {
    return TextRectFromMetrics(theme, L"comboBox", frame, 20.0f, true);
}

int ListBoxItemHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"listItem", L"height", 28.0f));
}

int ListBoxTwoLineItemHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"listItem", L"twoLineHeight", 48.0f));
}

RECT ListItemTextRect(const Theme& theme, RECT frame) {
    return TextRectFromMetrics(theme, L"listItem", frame, 20.0f, true);
}

RECT ListFrameInnerRect(const Theme& theme, RECT frame, UINT dpi) {
    const int inset = std::max(1, ThemedD2D::ScaleDip(
        static_cast<int>(theme.metric(L"list", L"borderWidth", 1.0f)), dpi));
    InflateRect(&frame, -inset, -inset);
    if (frame.right <= frame.left) {
        frame.right = frame.left + 1;
    }
    if (frame.bottom <= frame.top) {
        frame.bottom = frame.top + 1;
    }
    return frame;
}

RECT TableFrameInnerRect(const Theme& theme, RECT frame, UINT dpi) {
    const int inset = std::max(1, ThemedD2D::ScaleDip(
        static_cast<int>(theme.metric(L"table", L"borderWidth", 1.0f)), dpi));
    InflateRect(&frame, -inset, -inset);
    if (frame.right <= frame.left) {
        frame.right = frame.left + 1;
    }
    if (frame.bottom <= frame.top) {
        frame.bottom = frame.top + 1;
    }
    return frame;
}

int LabelHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"label", L"height", 20.0f));
}

RECT LabelTextRect(const Theme& theme, RECT frame) {
    return TextRectFromMetrics(theme, L"label", frame, 20.0f, true);
}

int FieldFrameHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"field", L"height", 28.0f));
}

RECT FieldTextRect(const Theme& theme, RECT frame) {
    return TextRectFromMetrics(theme, L"field", frame, 20.0f, true);
}

RECT FieldMultilineTextRect(const Theme& theme, RECT frame) {
    const int paddingX = RenderMetric(theme, L"field", L"paddingX", 9.0f);
    const int fieldHeight = RenderMetric(theme, L"field", L"height", 28.0f);
    const int textHeight = RenderMetric(theme, L"field", L"textHeight", 20.0f);
    const int offsetY = RenderMetric(theme, L"field", L"textOffsetY", 1.0f);
    const int paddingY = std::max(0, (fieldHeight - textHeight) / 2 + offsetY);
    RECT rect = frame;
    rect.left += paddingX;
    rect.right -= paddingX;
    rect.top += paddingY;
    rect.bottom -= paddingY;
    if (rect.bottom < rect.top) rect.bottom = rect.top;
    if (rect.right < rect.left) rect.right = rect.left;
    return rect;
}

HFONT CreateDialogFont(UINT dpi) {
    return CreateThemeFontPx(MulDiv(kDialogFontPx, static_cast<int>(dpi ? dpi : USER_DEFAULT_SCREEN_DPI), USER_DEFAULT_SCREEN_DPI));
}

HFONT CreateEditFont(const Theme& theme, UINT dpi) {
    return CreateThemeFontPx(MulDiv(EditFontSizePx(theme), static_cast<int>(dpi ? dpi : USER_DEFAULT_SCREEN_DPI), USER_DEFAULT_SCREEN_DPI));
}

HWND CreateStaticText(HINSTANCE instance, HWND parent, const wchar_t* text, int x, int y, int width, int height, HFONT font, DWORD style) {
    HWND hwnd = CreateWindowExW(
        0,
        L"STATIC",
        text,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style,
        x,
        y,
        width,
        height,
        parent,
        nullptr,
        instance,
        nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    return hwnd;
}

HWND CreateLabelText(HINSTANCE instance, HWND parent, const wchar_t* text, int x, int y, int width, int height, const Theme& theme, HFONT font, DWORD style, bool multiline) {
    HWND hwnd = CreateWindowExW(
        0,
        L"STATIC",
        text,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style,
        x,
        y,
        width,
        height,
        parent,
        nullptr,
        instance,
        nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SetControlTextProp(hwnd, text);
        auto& state = StateFor(hwnd);
        state.kind = ControlKind::Label;
        state.theme = &theme;
        state.multiline = multiline;
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

HWND CreateStatusBadge(HINSTANCE instance, HWND parent, const wchar_t* text, int x, int y, int width, const Theme& theme, HFONT font, const wchar_t* state, UINT dpi) {
    HWND hwnd = CreateWindowExW(
        0,
        L"STATIC",
        text,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        x,
        y,
        width,
        ThemedD2D::ScaleDip(LabelHeight(theme), dpi),
        parent,
        nullptr,
        instance,
        nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SetControlTextProp(hwnd, text);
        {
            auto& s = StateFor(hwnd);
            s.kind = ControlKind::StatusBadge;
            s.statusState = state ? state : L"success";
            s.theme = &theme;
        }
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

void SetStatusBadgeState(HWND hwnd, const wchar_t* state) {
    if (!hwnd) {
        return;
    }
    StateFor(hwnd).statusState = state ? state : L"success";
    InvalidateRect(hwnd, nullptr, TRUE);
}

HWND CreateStatusText(HINSTANCE instance, HWND parent, const wchar_t* text, int x, int y, int width, const Theme& theme, HFONT font, const wchar_t* state, DWORD style, UINT dpi) {
    HWND hwnd = CreateWindowExW(
        0,
        L"STATIC",
        text,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style,
        x,
        y,
        width,
        ThemedD2D::ScaleDip(LabelHeight(theme), dpi),
        parent,
        nullptr,
        instance,
        nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SetControlTextProp(hwnd, text);
        {
            auto& s = StateFor(hwnd);
            s.kind = ControlKind::StatusText;
            s.statusState = state ? state : L"normal";
            s.theme = &theme;
        }
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

SIZE MeasureTimeDisplay(const Theme& theme, HFONT baseFont, const std::wstring& text, UINT dpi) {
    const UINT effectiveDpi = dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
    HFONT displayFont = CreateTimeDisplayFont(baseFont, theme, effectiveDpi);
    const SIZE textSize = MeasureFontText(displayFont ? displayFont : baseFont, text);
    if (displayFont) {
        DeleteObject(displayFont);
    }
    const int paddingX = MulDiv(
        static_cast<int>(std::lround(theme.metric(L"timeDisplay", L"paddingX", 16.0f))),
        static_cast<int>(effectiveDpi), USER_DEFAULT_SCREEN_DPI);
    const int paddingY = MulDiv(
        static_cast<int>(std::lround(theme.metric(L"timeDisplay", L"paddingY", 8.0f))),
        static_cast<int>(effectiveDpi), USER_DEFAULT_SCREEN_DPI);
    return SIZE{
        std::max(1L, textSize.cx) + paddingX * 2,
        std::max(1L, textSize.cy) + paddingY * 2};
}

HWND CreateTimeDisplay(
    HINSTANCE instance,
    HWND parent,
    const Theme& theme,
    RECT frame,
    const std::wstring& value,
    HFONT baseFont,
    UINT dpi) {
    HWND hwnd = CreateWindowExW(
        0,
        L"STATIC",
        value.c_str(),
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        frame.left,
        frame.top,
        frame.right - frame.left,
        frame.bottom - frame.top,
        parent,
        nullptr,
        instance,
        nullptr);
    if (hwnd) {
        SetControlTextProp(hwnd, value.c_str());
        auto& state = StateFor(hwnd);
        state.kind = ControlKind::TimeDisplay;
        state.theme = &theme;
        state.dpi = dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
        AttachThemedBehavior(hwnd);
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(baseFont), TRUE);
    }
    return hwnd;
}

void SetStatusTextState(HWND hwnd, const wchar_t* state) {
    if (!hwnd) {
        return;
    }
    StateFor(hwnd).statusState = state ? state : L"normal";
    InvalidateRect(hwnd, nullptr, TRUE);
}

HWND CreateButton(HINSTANCE instance, HWND parent, int id, const wchar_t* text, int x, int y, int width, int height, HFONT font, bool defaultButton) {
    const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | BS_OWNERDRAW | (defaultButton ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON);
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text, style, x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SetControlTextProp(hwnd, text);
        StateFor(hwnd).kind = ControlKind::Button;
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

HWND CreatePrimaryButton(HINSTANCE instance, HWND parent, int id, const wchar_t* text, int x, int y, int width, int height, HFONT font, bool defaultButton) {
    const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | BS_OWNERDRAW | (defaultButton ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON);
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text, style, x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SetControlTextProp(hwnd, text);
        StateFor(hwnd).kind = ControlKind::PrimaryButton;
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

void SetControlTheme(HWND hwnd, const Theme& theme) {
    if (hwnd) {
        auto state = FindState(hwnd);
        const ControlKind kind = state ? state->kind : ControlKind::None;
        if (kind == ControlKind::ComboBox || kind == ControlKind::Edit || kind == ControlKind::ProgressBar || kind == ControlKind::Slider
            || kind == ControlKind::GroupBox || kind == ControlKind::Panel
            || kind == ControlKind::Toggle || kind == ControlKind::Radio || kind == ControlKind::Label
            || kind == ControlKind::StatusBadge || kind == ControlKind::StatusText || kind == ControlKind::TimeDisplay) {
            StateFor(hwnd).theme = &theme;
        } else {
            StateFor(hwnd).theme = nullptr;
        }
        InvalidateRect(hwnd, nullptr, TRUE);
    }
}

void SetControlDpi(HWND hwnd, UINT dpi) {
    if (!hwnd) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(StateMutex());
        auto& states = StateMap();
        const auto found = states.find(hwnd);
        if (found == states.end()) {
            return;
        }
        found->second.dpi = dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void SetButtonTablerIcon(HWND hwnd, TablerIconManifest::Id icon) {
    if (!hwnd) {
        return;
    }
    const ControlKind kind = KindFor(hwnd);
    if (kind != ControlKind::Button && kind != ControlKind::PrimaryButton) {
        return;
    }
    StateFor(hwnd).buttonTablerIcon = icon;
    InvalidateRect(hwnd, nullptr, FALSE);
}

bool ButtonTablerIcon(HWND hwnd, TablerIconManifest::Id& icon) {
    const auto state = FindState(hwnd);
    if (!state || !state->buttonTablerIcon) {
        return false;
    }
    icon = *state->buttonTablerIcon;
    return true;
}

HWND CreateMiniButton(HINSTANCE instance, HWND parent, int id, const wchar_t* text, int x, int y, int width, int height, HFONT font) {
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | BS_PUSHBUTTON | BS_OWNERDRAW,
                                x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SetControlTextProp(hwnd, text);
        StateFor(hwnd).kind = ControlKind::MiniButton;
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

HWND CreateCheckBox(HINSTANCE instance, HWND parent, int id, const wchar_t* text, int x, int y, int width, int height, HFONT font, bool checked) {
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | BS_OWNERDRAW,
                                x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SetControlTextProp(hwnd, text);
        StateFor(hwnd).kind = ControlKind::CheckBox;
        AttachThemedBehavior(hwnd);
        SendMessageW(hwnd, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    return hwnd;
}

HWND CreateToggle(HINSTANCE instance, HWND parent, int id, const wchar_t* text, int x, int y, int width, HFONT font, const Theme& theme, bool checked, UINT dpi) {
    HWND hwnd = CreateWindowExW(
        0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | BS_OWNERDRAW,
        x, y, width, ThemedD2D::ScaleDip(ToggleHeight(theme), dpi), parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SetControlTextProp(hwnd, text);
        auto& state = StateFor(hwnd);
        state.kind = ControlKind::Toggle;
        state.theme = &theme;
        AttachThemedBehavior(hwnd);
        SendMessageW(hwnd, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    return hwnd;
}

HWND CreateRadioButton(HINSTANCE instance, HWND parent, int id, const wchar_t* text, int x, int y, int width, HFONT font, const Theme& theme, int group, bool checked, UINT dpi) {
    HWND hwnd = CreateWindowExW(
        0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | BS_OWNERDRAW,
        x, y, width, ThemedD2D::ScaleDip(RadioButtonHeight(theme), dpi), parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SetControlTextProp(hwnd, text);
        auto& state = StateFor(hwnd);
        state.kind = ControlKind::Radio;
        state.theme = &theme;
        state.radioGroup = group;
        AttachThemedBehavior(hwnd);
        if (checked) {
            SendMessageW(hwnd, BM_SETCHECK, BST_CHECKED, 0);
        }
    }
    return hwnd;
}

HWND CreateHotKeyCapture(HINSTANCE instance, HWND parent, int id, const wchar_t* text, int x, int y, int width, HFONT font, const Theme& theme, UINT dpi) {
    HWND hwnd = CreateWindowExW(
        0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | BS_OWNERDRAW,
        x, y, width, ThemedD2D::ScaleDip(EditFrameHeight(theme), dpi), parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SetControlTextProp(hwnd, text);
        auto& state = StateFor(hwnd);
        state.kind = ControlKind::HotKeyCapture;
        state.theme = &theme;
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

HWND CreateLinkText(
    HINSTANCE instance,
    HWND parent,
    int id,
    const wchar_t* text,
    int x,
    int y,
    int width,
    int height,
    HFONT font,
    const Theme& theme,
    const wchar_t* role,
    bool visited,
    DWORD style) {
    HWND hwnd = CreateWindowExW(
        0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | BS_OWNERDRAW | style,
        x, y, width, height, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SetControlTextProp(hwnd, text);
        auto& state = StateFor(hwnd);
        state.kind = ControlKind::Link;
        state.theme = &theme;
        state.statusState = role ? role : L"normal";
        state.selected = visited;
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

HWND CreateContainerControl(
    HINSTANCE instance, HWND parent, int id, const wchar_t* text, RECT frame,
    HFONT font, const Theme& theme, ControlKind kind, bool raised) {
    HWND hwnd = CreateWindowExW(
        0, L"BUTTON", text ? text : L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | BS_OWNERDRAW,
        frame.left, frame.top, frame.right - frame.left, frame.bottom - frame.top,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SetControlTextProp(hwnd, text);
        auto& state = StateFor(hwnd);
        state.kind = kind;
        state.theme = &theme;
        state.checked = raised;
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

HWND CreateGroupBox(HINSTANCE instance, HWND parent, int id, const wchar_t* title, RECT frame, HFONT font, const Theme& theme, bool raised) {
    return CreateContainerControl(instance, parent, id, title, frame, font, theme, ControlKind::GroupBox, raised);
}

HWND CreatePanel(HINSTANCE instance, HWND parent, int id, RECT frame, HFONT font, const Theme& theme, const wchar_t* role, bool scrollable) {
    HWND hwnd = CreateContainerControl(instance, parent, id, L"", frame, font, theme, ControlKind::Panel, false);
    if (hwnd) {
        auto& state = StateFor(hwnd);
        state.statusState = role ? role : L"normal";
        if (scrollable) {
            SetWindowLongPtrW(hwnd, GWL_STYLE, GetWindowLongPtrW(hwnd, GWL_STYLE) | WS_VSCROLL);
        }
    }
    return hwnd;
}

RECT PanelContentRect(HWND hwnd) {
    RECT rect{};
    if (!hwnd) return rect;
    GetClientRect(hwnd, &rect);
    auto state = FindState(hwnd);
    if (!state || !state->theme) return rect;
    const int insetX = ScaledMetric(hwnd, *state->theme, L"panel", L"contentInsetX", 10.0f);
    const int insetY = ScaledMetric(hwnd, *state->theme, L"panel", L"contentInsetY", 8.0f);
    InflateRect(&rect, -insetX, -insetY);
    return rect;
}

void SetPanelRole(HWND hwnd, const wchar_t* role) {
    if (!hwnd || KindFor(hwnd) != ControlKind::Panel) return;
    StateFor(hwnd).statusState = role ? role : L"normal";
    InvalidateRect(hwnd, nullptr, TRUE);
}

HWND CreateTabControlFrame(HINSTANCE instance, HWND parent, int id, RECT frame, HFONT font, const Theme& theme) {
    return CreateContainerControl(instance, parent, id, L"", frame, font, theme, ControlKind::TabControl, false);
}

HWND CreateToolBarFrame(HINSTANCE instance, HWND parent, int id, RECT frame, HFONT font, const Theme& theme) {
    return CreateContainerControl(instance, parent, id, L"", frame, font, theme, ControlKind::ToolBar, false);
}

RECT GroupBoxContentRect(HWND hwnd) {
    RECT rect{};
    if (!hwnd) return rect;
    GetClientRect(hwnd, &rect);
    const auto state = FindState(hwnd);
    if (!state || !state->theme) return rect;
    const Theme& theme = *state->theme;
    const int paddingX = ScaledMetric(hwnd, theme, L"groupBox", L"paddingX", 12.0f);
    rect.left += paddingX;
    rect.right -= paddingX;
    rect.top += ScaledMetric(hwnd, theme, L"groupBox", L"titleInsetY", 4.0f)
        + ScaledMetric(hwnd, theme, L"groupBox", L"titleHeight", 20.0f)
        + ScaledMetric(hwnd, theme, L"groupBox", L"contentGapY", 4.0f);
    rect.bottom -= ScaledMetric(hwnd, theme, L"groupBox", L"paddingY", 10.0f);
    return rect;
}

HWND CreateSeparator(HINSTANCE instance, HWND parent, RECT frame, const Theme& theme, bool vertical) {
    HWND hwnd = CreateWindowExW(
        0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_OWNERDRAW,
        frame.left, frame.top, frame.right - frame.left, frame.bottom - frame.top,
        parent, nullptr, instance, nullptr);
    if (hwnd) {
        auto& state = StateFor(hwnd);
        state.kind = ControlKind::Separator;
        state.theme = &theme;
        state.checked = vertical;
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

void SetLinkRole(HWND hwnd, const wchar_t* role) {
    if (!hwnd) return;
    StateFor(hwnd).statusState = role ? role : L"normal";
    InvalidateRect(hwnd, nullptr, TRUE);
}

void SetLinkVisited(HWND hwnd, bool visited) {
    if (!hwnd) return;
    StateFor(hwnd).selected = visited;
    InvalidateRect(hwnd, nullptr, TRUE);
}

void SetControlBackgroundComponent(HWND hwnd, const wchar_t* component) {
    if (!hwnd) {
        return;
    }
    StateFor(hwnd).backgroundComponent = component ? component : L"";
    InvalidateRect(hwnd, nullptr, TRUE);
}

const wchar_t* ControlBackgroundComponent(HWND hwnd) {
    auto state = FindState(hwnd);
    if (!state || state->backgroundComponent.empty()) {
        return L"dialog";
    }
    return state->backgroundComponent.c_str();
}

void SetControlMultiline(HWND hwnd, bool multiline) {
    if (!hwnd) {
        return;
    }
    StateFor(hwnd).multiline = multiline;
    InvalidateRect(hwnd, nullptr, TRUE);
}

HWND CreateTabButton(HINSTANCE instance, HWND parent, int id, const wchar_t* text, int x, int y, int width, int height, HFONT font, bool selected) {
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | BS_PUSHBUTTON | BS_OWNERDRAW,
                                x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SetControlTextProp(hwnd, text);
        {
            auto& s = StateFor(hwnd);
            s.kind = ControlKind::TabButton;
            s.selected = selected;
        }
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

void SetTabButtonSelected(HWND hwnd, bool selected) {
    if (!hwnd) {
        return;
    }
    StateFor(hwnd).selected = selected;
    InvalidateRect(hwnd, nullptr, FALSE);
}

bool IsTabButtonSelected(HWND hwnd) {
    auto state = FindState(hwnd);
    return state && state->selected;
}

void SetTabAppearance(HWND hwnd, int appearance) {
    if (!hwnd) {
        return;
    }
    StateFor(hwnd).tabAppearance = std::clamp(appearance, kTabAppearanceStandard, kTabAppearanceConnectedTabs);
    InvalidateRect(hwnd, nullptr, FALSE);
}

int TabAppearance(HWND hwnd) {
    auto state = FindState(hwnd);
    return state ? state->tabAppearance : 0;
}

void SetTabOrientation(HWND hwnd, int orientation) {
    if (!hwnd) {
        return;
    }
    StateFor(hwnd).tabOrientation = std::clamp(orientation, kTabOrientationHorizontal, kTabOrientationVertical);
    InvalidateRect(hwnd, nullptr, FALSE);
}

int TabOrientation(HWND hwnd) {
    auto state = FindState(hwnd);
    return state ? state->tabOrientation : kTabOrientationHorizontal;
}

void SetTabContainerStyle(HWND hwnd, int style) {
    if (!hwnd) {
        return;
    }
    StateFor(hwnd).tabContainerStyle = std::clamp(
        style, kTabContainerStyleAppearanceDefault, kTabContainerStyleBorderless);
    InvalidateRect(hwnd, nullptr, FALSE);
}

int TabContainerStyle(HWND hwnd) {
    auto state = FindState(hwnd);
    return state ? state->tabContainerStyle : kTabContainerStyleAppearanceDefault;
}

HWND CreateComboBox(
    HINSTANCE instance, HWND parent, int id, int x, int y, int width, int height,
    HFONT font, const Theme& theme, UINT dpi) {
    HWND hwnd = CreateWindowExW(0, WC_COMBOBOXW, nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                                x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(hwnd, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1),
            static_cast<LPARAM>(ThemedD2D::ScaleDip(ComboBoxHeight(theme), dpi)));
        SendMessageW(hwnd, CB_SETITEMHEIGHT, 0,
            static_cast<LPARAM>(ThemedD2D::ScaleDip(ComboBoxItemHeight(theme), dpi)));
        {
            auto& s = StateFor(hwnd);
            s.kind = ControlKind::ComboBox;
            s.theme = &theme;
        }
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

HWND CreateListBox(
    HINSTANCE instance, HWND parent, int id, int x, int y, int width, int height,
    HFONT font, const Theme& theme, DWORD extraStyle, UINT dpi) {
    HWND hwnd = CreateWindowExW(
        0,
        L"LISTBOX",
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT | extraStyle,
        x,
        y,
        width,
        height,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        instance,
        nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(hwnd, LB_SETITEMHEIGHT, 0,
            static_cast<LPARAM>(ThemedD2D::ScaleDip(ListBoxItemHeight(theme), dpi)));
        StateFor(hwnd).kind = ControlKind::ListBox;
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

int EditFrameHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"edit", L"height", 28.0f));
}

int EditPaddingX(const Theme& theme) {
    return static_cast<int>(theme.metric(L"edit", L"paddingX", 9.0f));
}

int EditTextHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"edit", L"textHeight", 20.0f));
}

int EditFontSizePx(const Theme& theme) {
    return static_cast<int>(theme.metric(L"edit", L"singleLineFontSizePx", 14.0f));
}

RECT SingleLineEditRect(const Theme& theme, RECT frame, UINT dpi) {
    const int paddingX = ThemedD2D::ScaleDip(EditPaddingX(theme), dpi);
    const int controlHeight = ThemedD2D::ScaleDip(
        static_cast<int>(theme.metric(L"edit", L"singleLineControlHeight", static_cast<float>(EditTextHeight(theme)))), dpi);
    const int offsetY = ThemedD2D::ScaleDip(
        static_cast<int>(theme.metric(L"edit", L"singleLineOffsetY", theme.metric(L"edit", L"textOffsetY", 0.0f))), dpi);
    RECT rect{};
    rect.left = frame.left + paddingX;
    rect.right = frame.right - paddingX;
    rect.top = frame.top + ((frame.bottom - frame.top) - controlHeight) / 2 + offsetY;
    rect.bottom = rect.top + controlHeight;
    return rect;
}

RECT SingleLineEditRectForFrame(const Theme& theme, RECT frame, UINT dpi) {
    const int paddingX = ThemedD2D::ScaleDip(EditPaddingX(theme), dpi);
    const int preferredHeight = ThemedD2D::ScaleDip(
        static_cast<int>(theme.metric(L"edit", L"singleLineControlHeight", static_cast<float>(EditTextHeight(theme)))), dpi);
    const int frameHeight = std::max(1, static_cast<int>(frame.bottom - frame.top));
    const int controlHeight = std::max(1, std::min(preferredHeight, frameHeight - 2));
    const int offsetY = ThemedD2D::ScaleDip(
        static_cast<int>(theme.metric(L"edit", L"singleLineOffsetY", theme.metric(L"edit", L"textOffsetY", 0.0f))), dpi);
    RECT rect{};
    rect.left = frame.left + paddingX;
    rect.right = frame.right - paddingX;
    rect.top = frame.top + ((frame.bottom - frame.top) - controlHeight) / 2 + offsetY;
    rect.bottom = rect.top + controlHeight;
    return rect;
}

RECT MultiLineEditRect(const Theme& theme, RECT frame, UINT dpi) {
    const int paddingX = ThemedD2D::ScaleDip(EditPaddingX(theme), dpi);
    const int paddingTop = ThemedD2D::ScaleDip(static_cast<int>(theme.metric(L"edit", L"multiLinePaddingTop", 7.0f)), dpi);
    const int paddingBottom = ThemedD2D::ScaleDip(static_cast<int>(theme.metric(L"edit", L"multiLinePaddingBottom", 7.0f)), dpi);
    RECT rect{};
    rect.left = frame.left + paddingX;
    rect.right = frame.right - paddingX;
    rect.top = frame.top + paddingTop;
    rect.bottom = frame.bottom - paddingBottom;
    return rect;
}

void ConfigureEditBehavior(HWND hwnd, bool selectAllOnFocus) {
    if (!hwnd || KindFor(hwnd) != ControlKind::Edit) {
        return;
    }
    StateFor(hwnd).selectAllOnFocus = selectAllOnFocus;
}

HWND CreateSingleLineEdit(HINSTANCE instance, HWND parent, int id, const Theme& theme, RECT frame, const std::wstring& value, HFONT font, DWORD extraStyle) {
    const RECT editRect = SingleLineEditRect(theme, frame, GetDpiForWindow(parent));
    HWND hwnd = CreateWindowExW(
        0,
        L"EDIT",
        value.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | extraStyle,
        editRect.left,
        editRect.top,
        editRect.right - editRect.left,
        editRect.bottom - editRect.top,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        instance,
        nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
        auto& state = StateFor(hwnd);
        state.kind = ControlKind::Edit;
        state.theme = &theme;
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

HWND CreateMultiLineEdit(HINSTANCE instance, HWND parent, int id, const Theme& theme, RECT frame, const std::wstring& value, HFONT font, DWORD extraStyle) {
    const RECT editRect = MultiLineEditRect(theme, frame, GetDpiForWindow(parent));
    HWND hwnd = CreateWindowExW(
        0,
        L"EDIT",
        value.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | extraStyle,
        editRect.left,
        editRect.top,
        editRect.right - editRect.left,
        editRect.bottom - editRect.top,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        instance,
        nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
        auto& state = StateFor(hwnd);
        state.kind = ControlKind::Edit;
        state.theme = &theme;
        state.multiline = true;
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

HWND CreateFramedStatic(HINSTANCE instance, HWND parent, const Theme& theme, RECT frame, const std::wstring& value, HFONT font, DWORD style, bool multiline) {
    const RECT textRect = multiline ? FieldMultilineTextRect(theme, frame) : FieldTextRect(theme, frame);
    return CreateStaticText(
        instance,
        parent,
        value.c_str(),
        textRect.left,
        textRect.top,
        textRect.right - textRect.left,
        textRect.bottom - textRect.top,
        font,
        style);
}

HWND CreateProgressBar(HINSTANCE instance, HWND parent, int id, const Theme& theme, int x, int y, int width, int height) {
    HWND hwnd = CreateWindowExW(
        0,
        L"STATIC",
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_OWNERDRAW,
        x,
        y,
        width,
        height,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        instance,
        nullptr);
    if (hwnd) {
        auto& s = StateFor(hwnd);
        s.kind = ControlKind::ProgressBar;
        s.theme = &theme;
        s.progressValue = 0.0;
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

void SetProgressBarValue(HWND hwnd, double value, bool indeterminate) {
    if (!hwnd) {
        return;
    }
    auto& s = StateFor(hwnd);
    s.progressValue = std::max(0.0, std::min(1.0, value));
    s.progressIndeterminate = indeterminate;
    InvalidateRect(hwnd, nullptr, FALSE);
}

int ProgressBarHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"progressBar", L"height", 16.0f));
}

int SliderHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"slider", L"height", 24.0f));
}

HWND CreateSlider(HINSTANCE instance, HWND parent, int id, const Theme& theme, int x, int y, int width, double minimum, double maximum, double step, double value) {
    HWND hwnd = CreateWindowExW(
        0, L"STATIC", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS,
        x, y, width, SliderHeight(theme), parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (hwnd) {
        auto& state = StateFor(hwnd);
        state.kind = ControlKind::Slider;
        state.theme = &theme;
        state.sliderMinimum = minimum;
        state.sliderMaximum = maximum;
        state.sliderStep = std::max(0.0, step);
        state.sliderValue = ClampSliderValue(state, value);
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

void SetSliderValue(HWND hwnd, double value, bool notify) {
    auto state = FindState(hwnd);
    if (!state || state->kind != ControlKind::Slider) {
        return;
    }
    auto& mutableState = StateFor(hwnd);
    mutableState.sliderValue = ClampSliderValue(mutableState, value);
    InvalidateRect(hwnd, nullptr, FALSE);
    if (notify) {
        NotifySlider(hwnd);
    }
}

double SliderValue(HWND hwnd) {
    auto state = FindState(hwnd);
    return state && state->kind == ControlKind::Slider ? state->sliderValue : 0.0;
}

void DrawFieldFrame(const Theme& theme, HDC dc, RECT rect, HWND child, bool readOnly, bool error) {
    ThemedD2D::ScopedHdcPaint d2dPaint(WindowFromDC(dc), dc);
    const bool disabled = child && !IsWindowEnabled(child);
    const bool focused = child && GetFocus() == child;
    const bool hover = child && IsHover(child);
    const wchar_t* component = readOnly ? L"field" : L"edit";
    const wchar_t* state = disabled ? L"disabled" : (error ? L"error" : (focused ? L"focused" : (hover ? L"hover" : (readOnly ? L"readonly" : L"normal"))));
    FillRoundRect(
        dc,
        rect,
        RenderMetric(theme, component, L"radius", 7.0f),
        ToColorRef(theme.color(component, state, L"bg")),
        ToColorRef(theme.color(component, state, L"border")),
        RenderMetric(theme, component, L"borderWidth", 1.0f));
}

void DrawEditFrame(const Theme& theme, HDC dc, RECT rect, HWND child, bool readOnly, bool error) {
    ThemedD2D::ScopedHdcPaint d2dPaint(WindowFromDC(dc), dc);
    const bool disabled = child && !IsWindowEnabled(child);
    const bool focused = child && GetFocus() == child;
    const bool hover = child && IsHover(child);
    const wchar_t* state = disabled ? L"disabled" : (error ? L"error" : (focused ? L"focused" : (hover ? L"hover" : (readOnly ? L"readonly" : L"normal"))));
    FillRoundRect(
        dc,
        rect,
        RenderMetric(theme, L"edit", L"radius", 7.0f),
        ToColorRef(theme.color(L"edit", state, L"bg")),
        ToColorRef(theme.color(L"edit", state, L"border")),
        RenderMetric(theme, L"edit", L"borderWidth", 1.0f));
}

void DrawComboFrame(const Theme& theme, HDC dc, RECT rect, HWND child) {
    ThemedD2D::ScopedHdcPaint d2dPaint(WindowFromDC(dc), dc);
    const bool disabled = child && !IsWindowEnabled(child);
    const bool focused = child && GetFocus() == child;
    const bool hover = child && IsHover(child);
    const wchar_t* state = disabled ? L"disabled" : (focused ? L"focused" : (hover ? L"hover" : L"normal"));
    FillRoundRect(
        dc,
        rect,
        RenderMetric(theme, L"comboBox", L"radius", 7.0f),
        ToColorRef(theme.color(L"comboBox", state, L"bg")),
        ToColorRef(theme.color(L"comboBox", state, L"border")),
        RenderMetric(theme, L"comboBox", L"borderWidth", 1.0f));
}

void DrawListFrame(const Theme& theme, HDC dc, RECT rect, HWND child, bool readOnly) {
    ThemedD2D::ScopedHdcPaint d2dPaint(WindowFromDC(dc), dc);
    const bool disabled = child && !IsWindowEnabled(child);
    const bool focused = child && GetFocus() == child;
    const wchar_t* state = disabled ? L"disabled" : (focused ? L"focused" : (readOnly ? L"readonly" : L"normal"));
    FillRoundRect(
        dc,
        rect,
        RenderMetric(theme, L"list", L"radius", 7.0f),
        ToColorRef(theme.color(L"list", state, L"bg")),
        ToColorRef(theme.color(L"list", state, L"border")),
        RenderMetric(theme, L"list", L"borderWidth", 1.0f));
}

void DrawTableFrame(const Theme& theme, HDC dc, RECT rect, HWND child) {
    ThemedD2D::ScopedHdcPaint d2dPaint(WindowFromDC(dc), dc);
    const bool disabled = child && !IsWindowEnabled(child);
    const bool focused = child && GetFocus() == child;
    const wchar_t* state = disabled ? L"disabled" : (focused ? L"focused" : L"normal");
    FillRoundRect(
        dc,
        rect,
        RenderMetric(theme, L"table", L"radius", 7.0f),
        ToColorRef(theme.color(L"table", state, L"bg")),
        ToColorRef(theme.color(L"table", state, L"border")),
        RenderMetric(theme, L"table", L"borderWidth", 1.0f));
}

void DrawPanelFrame(const Theme& theme, HDC dc, RECT rect, bool raised) {
    ThemedD2D::ScopedHdcPaint d2dPaint(WindowFromDC(dc), dc);
    const wchar_t* state = raised ? L"raised" : L"normal";
    FillRoundRect(
        dc,
        rect,
        RenderMetric(theme, L"panel", L"radius", 7.0f),
        ToColorRef(theme.color(L"panel", state, L"bg")),
        ToColorRef(theme.color(L"panel", state, L"border")),
        RenderMetric(theme, L"panel", L"borderWidth", 1.0f));
}

void ApplyListViewTheme(HWND list, const Theme& theme) {
    if (!list) {
        return;
    }
    const wchar_t* component = KindFor(list) == ControlKind::Table ? L"table" : L"list";
    const COLORREF bg = ToColorRef(theme.color(component, L"normal", L"bg"));
    ListView_SetBkColor(list, bg);
    ListView_SetTextBkColor(list, bg);
    ListView_SetTextColor(list, ToColorRef(theme.color(component, L"normal", L"text")));
    if (HWND header = ListView_GetHeader(list)) {
        InvalidateRect(header, nullptr, TRUE);
    }
    InvalidateRect(list, nullptr, TRUE);
}

void RegisterTable(HWND table, const Theme& theme, UINT dpi) {
    if (!table) return;
    auto& state = StateFor(table);
    state.kind = ControlKind::Table;
    state.theme = &theme;
    state.tableDpi = dpi ? dpi : GetDpiForWindow(table);
    if (!state.tableDpi) state.tableDpi = USER_DEFAULT_SCREEN_DPI;
    AttachThemedBehavior(table);
    ApplyListViewTheme(table, theme);
    ThemedControls::RestoreTableDefaultImageList(table);
}

void ConfigureTableRowPresentation(HWND table, bool twoLines) {
    if (!table) return;
    auto& state = StateFor(table);
    if (state.tableTwoLineRows == twoLines && state.tableDefaultSmallImages) return;
    state.tableTwoLineRows = twoLines;
    if (state.tableDefaultSmallImages) {
        ListView_SetImageList(table, nullptr, LVSIL_SMALL);
        ImageList_Destroy(state.tableDefaultSmallImages);
        state.tableDefaultSmallImages = nullptr;
    }
    RestoreTableDefaultImageList(table);
    InvalidateRect(table, nullptr, TRUE);
}

void ConfigureTableColumns(HWND table, const std::vector<int>& widthModes) {
    if (!table) return;
    StateFor(table).tableColumnWidthModes = widthModes;
}

void MoveTable(HWND table, int x, int y, int width, int height) {
    if (!table) return;
    SetWindowPos(table, nullptr, x, y, std::max(1, width), std::max(1, height), SWP_NOACTIVATE | SWP_NOZORDER);
    RelayoutTableRemainingColumns(table, -1, -1);
}

void ConfigureTableGridLines(HWND table, bool rowGridLines, bool columnGridLines) {
    if (!table) return;
    auto& state = StateFor(table);
    state.tableShowRowGridLines = rowGridLines;
    state.tableShowColumnGridLines = columnGridLines;
    InvalidateRect(table, nullptr, TRUE);
    if (HWND header = ListView_GetHeader(table)) {
        InvalidateRect(header, nullptr, TRUE);
    }
}

void SetTableColumnResizeEnabled(HWND table, bool enabled) {
    if (!table) return;
    StateFor(table).tableAllowColumnResize = enabled;
    HWND header = ListView_GetHeader(table);
    if (!header) return;
    LONG_PTR style = GetWindowLongPtrW(header, GWL_STYLE);
    if (enabled) {
        style &= ~static_cast<LONG_PTR>(HDS_NOSIZING);
    } else {
        style |= HDS_NOSIZING;
    }
    SetWindowLongPtrW(header, GWL_STYLE, style);
    SetWindowPos(header, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void SetTableHorizontalScrollEnabled(HWND table, bool enabled) {
    if (!table) return;
    StateFor(table).tableAllowHorizontalScroll = enabled;
    if (!enabled) {
        RelayoutTableRemainingColumns(table, -1, -1);
        ShowScrollBar(table, SB_HORZ, FALSE);
    }
}

void SetTableScrollBarGutterReserved(HWND table, bool reserved) {
    if (!table) return;
    StateFor(table).tableReserveScrollBarGutter = reserved;
    RelayoutTableRemainingColumns(table, -1, -1);
}

void SetTableRowEnabledStates(HWND table, const std::vector<bool>& enabled) {
    if (!table) return;
    StateFor(table).tableRowEnabled = enabled;
}

void SetTableRowActiveStates(HWND table, const std::vector<bool>& active) {
    if (!table) return;
    StateFor(table).tableRowActive = active;
}

void SetTableCells(HWND table, const std::vector<std::vector<TableCellRuntime>>& cells) {
    if (!table) return;
    StateFor(table).tableCells = cells;
}

void InsertTableRowState(HWND table, int index, bool enabled, bool active, std::vector<TableCellRuntime> cells) {
    if (!table) return;
    auto& state = StateFor(table);
    const std::size_t position = static_cast<std::size_t>(std::clamp(index, 0,
        static_cast<int>(state.tableRowEnabled.size())));
    state.tableRowEnabled.insert(state.tableRowEnabled.begin() + position, enabled);
    const std::size_t activePosition = std::min(position, state.tableRowActive.size());
    state.tableRowActive.insert(state.tableRowActive.begin() + activePosition, active);
    const std::size_t cellPosition = std::min(position, state.tableCells.size());
    state.tableCells.insert(state.tableCells.begin() + cellPosition, std::move(cells));
}

void UpdateTableRowState(HWND table, int index, bool enabled, bool active, std::vector<TableCellRuntime> cells) {
    if (!table || index < 0) return;
    auto& state = StateFor(table);
    const std::size_t position = static_cast<std::size_t>(index);
    if (position >= state.tableRowEnabled.size() || position >= state.tableRowActive.size() ||
        position >= state.tableCells.size()) return;
    state.tableRowEnabled[position] = enabled;
    state.tableRowActive[position] = active;
    state.tableCells[position] = std::move(cells);
}

void RemoveTableRowState(HWND table, int index) {
    if (!table || index < 0) return;
    auto& state = StateFor(table);
    const std::size_t position = static_cast<std::size_t>(index);
    if (position < state.tableRowEnabled.size()) {
        state.tableRowEnabled.erase(state.tableRowEnabled.begin() + position);
    }
    if (position < state.tableRowActive.size()) {
        state.tableRowActive.erase(state.tableRowActive.begin() + position);
    }
    if (position < state.tableCells.size()) {
        state.tableCells.erase(state.tableCells.begin() + position);
    }
}

void BeginTableRowsUpdate(HWND table) {
    if (!table) return;
    auto& state = StateFor(table);
    if (state.tableRowsUpdateDepth++ == 0) SendMessageW(table, WM_SETREDRAW, FALSE, 0);
}

void EndTableRowsUpdate(HWND table) {
    if (!table) return;
    auto& state = StateFor(table);
    state.tableRowsUpdateDepth = std::max(0, state.tableRowsUpdateDepth - 1);
    if (state.tableRowsUpdateDepth == 0) {
        SendMessageW(table, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(table, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    }
}

void BeginTableRowUpdate(HWND table) {
    if (!table) return;
    ++StateFor(table).tableRowsUpdateDepth;
}

void EndTableRowUpdate(HWND table, int row) {
    if (!table) return;
    auto& state = StateFor(table);
    state.tableRowsUpdateDepth = std::max(0, state.tableRowsUpdateDepth - 1);
    InvalidateTableRow(table, row);
}

bool IsTableRowsUpdating(HWND table) {
    const auto state = FindState(table);
    return state && state->tableRowsUpdateDepth > 0;
}

bool IsTableRowEnabled(HWND table, int index) {
    const auto state = FindState(table);
    return state && index >= 0 && static_cast<std::size_t>(index) < state->tableRowEnabled.size()
        ? state->tableRowEnabled[static_cast<std::size_t>(index)]
        : true;
}

bool IsTableRowActive(HWND table, int index) {
    const auto state = FindState(table);
    return state && index >= 0 && static_cast<std::size_t>(index) < state->tableRowActive.size()
        ? state->tableRowActive[static_cast<std::size_t>(index)]
        : false;
}

// The CDIS_SELECTED bit in NMCUSTOMDRAW::uItemState is unreliable for ListView
// items under LVS_EX_FULLROWSELECT (it can report selected for rows that are not
// actually selected), which collapses every row onto the "selected" background
// and hides the alternating stripe. Query the control's authoritative selection
// state instead of trusting the custom-draw flag.
bool IsTableRowSelected(HWND table, int row) {
    if (!table || row < 0) {
        return false;
    }
    return (ListView_GetItemState(table, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
}

bool TableCellAction(HWND table, int row, int column, int& actionId) {
    const auto cell = TableCellAt(table, row, column);
    if (!cell || !IsActionCell(*cell) || !IsTableRowEnabled(table, row)) {
        actionId = 0;
        return false;
    }
    actionId = cell->actionId;
    return true;
}

void RestoreTableDefaultImageList(HWND table) {
    if (!table) return;
    auto& state = StateFor(table);
    if (!state.theme) return;
    if (!state.tableDefaultSmallImages) {
        const int rowHeight = TableScaledMetric(
            table, *state.theme, L"listItem",
            state.tableTwoLineRows ? L"twoLineHeight" : L"height",
            state.tableTwoLineRows ? 48.0f : 28.0f);
        // Report view adds one physical pixel around the image-list height.
        // Subtract it here so the native item rect matches the public 28px row
        // template exactly at every DPI.
        state.tableDefaultSmallImages = ImageList_Create(1, std::max(1, rowHeight - 1), ILC_COLOR32, 1, 1);
    }
    if (state.tableDefaultSmallImages) {
        ListView_SetImageList(table, state.tableDefaultSmallImages, LVSIL_SMALL);
    }
}

void CreateSystemCheckBoxImages(HWND table) {
    if (!table) return;
    auto& state = StateFor(table);
    state.tableCheckable = true;
    if (!state.theme) return;
    if (state.tableCheckBoxStateImages) {
        ListView_SetImageList(table, nullptr, LVSIL_STATE);
        ImageList_Destroy(state.tableCheckBoxStateImages);
        state.tableCheckBoxStateImages = nullptr;
    }

    // ListView still needs a state image list for checkbox state storage and
    // LVHT_ONITEMSTATEICON hit testing. The visible checkbox is rendered by the
    // shared owner-draw path, so these images are deliberately transparent.
    const int width = std::max(1,
        TableScaledMetric(table, *state.theme, L"listItem", L"paddingX", 8.0f)
        + TableScaledMetric(table, *state.theme, L"checkbox", L"boxSize", 16.0f)
        + TableScaledMetric(table, *state.theme, L"checkbox", L"gap", 8.0f));
    const int height = std::max(1, TableScaledMetric(
        table, *state.theme, L"listItem",
        state.tableTwoLineRows ? L"twoLineHeight" : L"height",
        state.tableTwoLineRows ? 48.0f : 28.0f) - 1);
    HIMAGELIST images = ImageList_Create(width, height, ILC_COLOR32, 2, 0);
    if (!images) return;

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        ImageList_Destroy(images);
        return;
    }
    ZeroMemory(bits, static_cast<SIZE_T>(width) * static_cast<SIZE_T>(height) * 4);
    ImageList_Add(images, bitmap, nullptr);
    ImageList_Add(images, bitmap, nullptr);
    DeleteObject(bitmap);

    state.tableCheckBoxStateImages = images;
    ListView_SetImageList(table, images, LVSIL_STATE);
}

void RefreshTableDpiResources(HWND table, UINT dpi) {
    if (!table) return;
    auto& state = StateFor(table);
    if (state.kind != ControlKind::Table || !state.theme) return;
    const UINT oldDpi = state.tableDpi ? state.tableDpi : USER_DEFAULT_SCREEN_DPI;
    const UINT newDpi = dpi ? dpi : oldDpi;
    if (newDpi != oldDpi) {
        const int columnCount = Header_GetItemCount(ListView_GetHeader(table));
        for (int column = 0; column < columnCount; ++column) {
            const int width = ListView_GetColumnWidth(table, column);
            ListView_SetColumnWidth(table, column, std::max(1, MulDiv(width, newDpi, oldDpi)));
        }
        state.tableDpi = newDpi;
        constexpr int remainingMode = 2;  // ThemedTableColumnWidth::Remaining
        if (std::find(
                state.tableColumnWidthModes.begin(),
                state.tableColumnWidthModes.end(),
                remainingMode) != state.tableColumnWidthModes.end()) {
            RelayoutTableRemainingColumns(table, -1, -1);
        }
    }
    const bool usesDefaultSmallImages = state.tableDefaultSmallImages
        && ListView_GetImageList(table, LVSIL_SMALL) == state.tableDefaultSmallImages;
    if (state.tableDefaultSmallImages) {
        if (usesDefaultSmallImages) {
            ListView_SetImageList(table, nullptr, LVSIL_SMALL);
        }
        ImageList_Destroy(state.tableDefaultSmallImages);
        state.tableDefaultSmallImages = nullptr;
    }
    if (usesDefaultSmallImages) {
        RestoreTableDefaultImageList(table);
    }
    if (state.tableCheckable) {
        CreateSystemCheckBoxImages(table);
    }
    InvalidateRect(table, nullptr, TRUE);
}



void DrawTabGroupFrame(const Theme& theme, HDC dc, RECT rect) {
    ThemedD2D::ScopedHdcPaint d2dPaint(WindowFromDC(dc), dc);
    FillRoundRect(
        dc,
        rect,
        RenderMetric(theme, L"tabButton", L"groupRadius", 10.0f),
        ToColorRef(theme.color(L"tabButton", L"normal", L"groupBg")),
        ToColorRef(theme.color(L"tabButton", L"normal", L"groupBorder")),
        RenderMetric(theme, L"tabButton", L"groupBorderWidth", 1.0f));
}

bool Draw(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    if (!draw || !draw->hwndItem) {
        return false;
    }
    ControlKind kind = KindFor(draw->hwndItem);
    ThemedD2D::ScopedHdcPaint d2dPaint(draw->hwndItem, draw->hDC);
    if (draw->CtlType == ODT_STATIC && kind == ControlKind::Label) {
        DrawLabelControl(draw->hwndItem, draw->hDC, draw->rcItem);
        return true;
    }
    if (draw->CtlType == ODT_STATIC && kind == ControlKind::StatusBadge) {
        DrawStatusBadge(draw->hwndItem, draw->hDC, draw->rcItem);
        return true;
    }
    if (draw->CtlType == ODT_STATIC && kind == ControlKind::StatusText) {
        DrawStatusText(draw->hwndItem, draw->hDC, draw->rcItem);
        return true;
    }
    if (draw->CtlType == ODT_STATIC && kind == ControlKind::ProgressBar) {
        DrawProgressBar(draw->hwndItem, draw->hDC);
        return true;
    }
    if (draw->CtlType == ODT_COMBOBOX && kind == ControlKind::ComboBox) {
        DrawComboBox(theme, draw);
        return true;
    }
    if (draw->CtlType == ODT_LISTBOX && kind == ControlKind::ListBox) {
        DrawListBox(theme, draw);
        return true;
    }
    if (draw->CtlType != ODT_BUTTON) {
        return false;
    }
    if (kind == ControlKind::Button) {
        DrawButton(theme, draw);
        return true;
    }
    if (kind == ControlKind::PrimaryButton) {
        DrawPrimaryButton(theme, draw);
        return true;
    }
    if (kind == ControlKind::MiniButton) {
        DrawMiniButton(theme, draw);
        return true;
    }
    if (kind == ControlKind::CheckBox) {
        DrawCheckBox(theme, draw);
        return true;
    }
    if (kind == ControlKind::Toggle) {
        DrawToggle(theme, draw);
        return true;
    }
    if (kind == ControlKind::Radio) {
        DrawRadioButton(theme, draw);
        return true;
    }
    if (kind == ControlKind::HotKeyCapture) {
        DrawHotKeyCapture(theme, draw);
        return true;
    }
    if (kind == ControlKind::Link) {
        DrawLinkText(theme, draw);
        return true;
    }
    if (kind == ControlKind::Panel || kind == ControlKind::GroupBox || kind == ControlKind::TabControl || kind == ControlKind::ToolBar) {
        DrawContainer(theme, draw, kind);
        return true;
    }
    if (kind == ControlKind::Separator) {
        DrawSeparator(theme, draw);
        return true;
    }
    if (kind == ControlKind::TabButton) {
        DrawTabButton(theme, draw);
        return true;
    }
    return false;
}

bool HandleListViewCustomDraw(const Theme& theme, LPARAM lParam, LRESULT& result) {
    auto* header = reinterpret_cast<NMHDR*>(lParam);
    if (!header) {
        return false;
    }

    const std::wstring className = ClassName(header->hwndFrom);
    if (className == L"SysHeader32" &&
        (header->code == HDN_BEGINTRACKW || header->code == HDN_BEGINTRACKA)) {
        HWND table = GetParent(header->hwndFrom);
        const auto state = FindState(table);
        if (state && state->kind == ControlKind::Table && !state->tableAllowColumnResize) {
            result = TRUE;
            return true;
        }
        return false;
    }

    if (header->code != NM_CUSTOMDRAW) {
        return false;
    }

    auto* commonDraw = reinterpret_cast<NMCUSTOMDRAW*>(lParam);
    ThemedD2D::ScopedHdcPaint d2dPaint(header->hwndFrom, commonDraw->hdc);

    if (className == L"SysHeader32") {
        auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(lParam);
        switch (draw->dwDrawStage) {
        case CDDS_PREPAINT: {
            // Erase the whole header background before per-item drawing. The
            // default header PREPAINT paints themed column separators, including
            // one at the last column's right edge (the header's right border).
            // Per-item fills only cover each column's own rect, so that trailing
            // separator would survive as a stray divider. Filling the entire
            // client here removes every default separator; our own inter-column
            // dividers are then redrawn per item.
            const bool tableHeader = KindFor(GetParent(header->hwndFrom)) == ControlKind::Table;
            const wchar_t* component = tableHeader ? L"tableHeader" : L"list";
            RECT client{};
            if (GetClientRect(header->hwndFrom, &client)) {
                FillThemedRect(draw->hdc, client, ToColorRef(theme.color(component, L"normal", L"bg")));
            }
            result = CDRF_NOTIFYITEMDRAW;
            return true;
        }
        case CDDS_ITEMPREPAINT:
            DrawHeaderItem(theme, header->hwndFrom, draw);
            result = CDRF_SKIPDEFAULT;
            return true;
        default:
            break;
        }
        return false;
    }

    if (className != L"SysListView32") {
        return false;
    }

    auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);

    switch (draw->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        result = CDRF_NOTIFYITEMDRAW;
        return true;
    case CDDS_ITEMPREPAINT: {
        const int row = static_cast<int>(draw->nmcd.dwItemSpec);
        if (!IsTableDetailsView(header->hwndFrom)) {
            DrawTableIconTile(theme, header->hwndFrom, draw);
            result = CDRF_SKIPDEFAULT;
            return true;
        }
        const bool selected = IsTableRowSelected(header->hwndFrom, row);
        const bool enabled = IsTableRowEnabled(header->hwndFrom, row);
        const bool hovered = IsTableRowHovered(header->hwndFrom, row);
        const wchar_t* state = TableRowState(row, selected, enabled, hovered);
        RECT rowRect{};
        if (ListView_GetItemRect(header->hwndFrom, row, &rowRect, LVIR_BOUNDS)) {
            FillThemedRect(draw->nmcd.hdc, rowRect, TableRowBackground(theme, state));
        }
        if (!UsesNativeTableRowDrawing(header->hwndFrom, row)) {
            DrawTableRowCells(theme, header->hwndFrom, draw);
            result = CDRF_SKIPDEFAULT;
            return true;
        }
        draw->clrText = ToColorRef(theme.color(L"listItem", enabled ? L"normal" : L"disabled", L"text"));
        draw->clrTextBk = CLR_NONE;
        result = CDRF_NOTIFYSUBITEMDRAW;
        return true;
    }
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        if (!IsTableDetailsView(header->hwndFrom)) {
            result = CDRF_SKIPDEFAULT;
            return true;
        }
        const int row = static_cast<int>(draw->nmcd.dwItemSpec);
        const int column = draw->iSubItem;
        const auto cell = TableCellAt(header->hwndFrom, row, column);
        if (cell && IsActionCell(*cell)) {
            DrawTableActionCell(theme, header->hwndFrom, draw, *cell);
            result = CDRF_SKIPDEFAULT;
            return true;
        }
        if (!UsesNativeTableCellDrawing(header->hwndFrom, column, cell ? &*cell : nullptr)) {
            DrawTableTextCell(theme, header->hwndFrom, draw);
            result = CDRF_SKIPDEFAULT;
            return true;
        }
        const bool enabled = IsTableRowEnabled(header->hwndFrom, row);
        draw->clrText = ToColorRef(theme.color(L"listItem", enabled ? L"normal" : L"disabled", L"text"));
        draw->clrTextBk = CLR_NONE;
        result = CDRF_NEWFONT;
        return true;
    }
    default:
        break;
    }
    return false;
}

}
