#include "ThemedControls.h"

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

namespace {
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
    StatusBadge,
    StatusText,
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
    bool multiline = false;
    bool selectAllOnFocus = false;
    int radioGroup = 0;
    bool progressIndeterminate = false;
    double progressValue = 0.0;
    double sliderMinimum = 0.0;
    double sliderMaximum = 100.0;
    double sliderStep = 1.0;
    double sliderValue = 0.0;
    std::vector<int> tableColumnWidthModes;
    std::vector<bool> tableRowEnabled;
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
    StateMap().erase(hwnd);
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
    const int stroke = std::max(1, borderWidth);
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, stroke, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    if (stroke > 1) {
        InflateRect(&rect, -(stroke / 2), -(stroke / 2));
    }
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawRoundRect(HDC dc, RECT rect, int radius, COLORREF border, int borderWidth) {
    const int stroke = std::max(1, borderWidth);
    HBRUSH brush = reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HPEN pen = CreatePen(PS_SOLID, stroke, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    if (stroke > 1) {
        InflateRect(&rect, -(stroke / 2), -(stroke / 2));
    }
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
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
    const int radius = static_cast<int>(theme.metric(L"button", L"radius", 6.0f));
    const int borderWidth = static_cast<int>(theme.metric(L"button", L"borderWidth", 1.0f));
    FillRoundRect(
        dc,
        rect,
        radius,
        ToColorRef(theme.color(component, state, L"bg")),
        ToColorRef(theme.color(component, focused ? L"focused" : state, L"border")),
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
    const int paddingX = static_cast<int>(theme.metric(L"comboBox", L"paddingX", 9.0f));
    const int textHeight = static_cast<int>(theme.metric(L"comboBox", L"textHeight", 20.0f));
    const int offsetY = static_cast<int>(theme.metric(L"comboBox", L"textOffsetY", 1.0f));
    const int arrowWidth = static_cast<int>(theme.metric(L"comboBox", L"arrowWidth", 28.0f));
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

    RECT rect{};
    GetClientRect(hwnd, &rect);
    const bool disabled = !IsWindowEnabled(hwnd);
    const bool focused = GetFocus() == hwnd;
    const bool hover = IsHover(hwnd);
    const wchar_t* state = disabled ? L"disabled" : (focused ? L"focused" : (hover ? L"hover" : L"normal"));
    const int radius = static_cast<int>(theme->metric(L"comboBox", L"radius", 7.0f));
    const int borderWidth = static_cast<int>(theme->metric(L"comboBox", L"borderWidth", 1.0f));
    HBRUSH parentBrush = CreateSolidBrush(ToColorRef(theme->color(L"dialog", L"normal", L"bg")));
    FillRect(dc, &rect, parentBrush);
    DeleteObject(parentBrush);
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
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont) {
        SelectObject(dc, oldFont);
    }

    const int arrowSize = static_cast<int>(theme->metric(L"comboBox", L"arrowSize", 5.0f));
    const int arrowPaddingRight = static_cast<int>(theme->metric(L"comboBox", L"arrowPaddingRight", 11.0f));
    const int cx = rect.right - arrowPaddingRight - arrowSize;
    const int cy = (rect.top + rect.bottom) / 2 + static_cast<int>(theme->metric(L"comboBox", L"arrowOffsetY", 1.0f));
    HPEN pen = CreatePen(PS_SOLID, static_cast<int>(theme->metric(L"comboBox", L"arrowStrokeWidth", 1.0f)), ToColorRef(theme->color(L"comboBox", state, L"arrow")));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, cx - arrowSize, cy - arrowSize / 2, nullptr);
    LineTo(dc, cx, cy + arrowSize / 2);
    LineTo(dc, cx + arrowSize, cy - arrowSize / 2);
    SelectObject(dc, oldPen);
    DeleteObject(pen);

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
    HBRUSH background = CreateSolidBrush(ToColorRef(theme->color(backgroundComponent, L"normal", L"bg")));
    FillRect(dc, &rect, background);
    DeleteObject(background);

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
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &badge, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont) {
        SelectObject(dc, oldFont);
    }
}

void DrawStatusText(HWND hwnd, HDC dc, RECT rect) {
    auto controlState = FindState(hwnd);
    const Theme* theme = controlState ? controlState->theme : nullptr;
    if (!theme) {
        return;
    }

    const std::wstring backgroundComponent = BackgroundComponent(hwnd);
    HBRUSH background = CreateSolidBrush(ToColorRef(theme->color(backgroundComponent, L"normal", L"bg")));
    FillRect(dc, &rect, background);
    DeleteObject(background);

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
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format);
    if (oldFont) {
        SelectObject(dc, oldFont);
    }
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

    RECT rect{};
    GetClientRect(hwnd, &rect);
    const bool disabled = !IsWindowEnabled(hwnd);
    const wchar_t* state = disabled ? L"disabled" : L"normal";
    const int radius = static_cast<int>(theme->metric(L"progressBar", L"radius", 7.0f));
    const int borderWidth = static_cast<int>(theme->metric(L"progressBar", L"borderWidth", 1.0f));
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

LRESULT CALLBACK ThemedControlProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR) {
    switch (message) {
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
    case WM_PAINT: {
        ControlKind kind = KindFor(hwnd);
        if (IsOwnerDrawButtonKind(kind)) {
            return DefSubclassProc(hwnd, message, wParam, lParam);
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
    case WM_MOUSEMOVE:
        if (KindFor(hwnd) == ControlKind::Slider && GetCapture() == hwnd) {
            SetSliderFromClientX(hwnd, GET_X_LPARAM(lParam), true);
            return 0;
        }
        if (!IsHover(hwnd)) {
            StateFor(hwnd).hover = true;
            TRACKMOUSEEVENT event{};
            event.cbSize = sizeof(event);
            event.dwFlags = TME_LEAVE;
            event.hwndTrack = hwnd;
            TrackMouseEvent(&event);
            InvalidateRect(hwnd, nullptr, TRUE);
            if (KindFor(hwnd) == ControlKind::Edit) {
                InvalidateParentAround(hwnd);
            }
        }
        break;
    case WM_MOUSELEAVE:
        StateFor(hwnd).hover = false;
        InvalidateRect(hwnd, nullptr, TRUE);
        if (KindFor(hwnd) == ControlKind::Edit) {
            InvalidateParentAround(hwnd);
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
    const Theme& theme = *state->theme;
    RECT rect{};
    GetClientRect(hwnd, &rect);
    HBRUSH background = CreateSolidBrush(ToColorRef(theme.color(L"dialog", L"normal", L"bg")));
    FillRect(dc, &rect, background);
    DeleteObject(background);

    const bool disabled = !IsWindowEnabled(hwnd);
    const bool hover = IsHover(hwnd);
    const wchar_t* visualState = disabled ? L"disabled" : (hover ? L"hover" : L"normal");
    const int thumbSize = static_cast<int>(theme.metric(L"slider", L"thumbSize", 14.0f));
    const int trackHeight = static_cast<int>(theme.metric(L"slider", L"trackHeight", 4.0f));
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
    HBRUSH thumbBrush = CreateSolidBrush(ToColorRef(theme.color(L"slider", visualState, L"thumb")));
    HPEN thumbPen = CreatePen(PS_SOLID, 1, ToColorRef(theme.color(L"slider", visualState, L"border")));
    HGDIOBJ oldBrush = SelectObject(dc, thumbBrush);
    HGDIOBJ oldPen = SelectObject(dc, thumbPen);
    Ellipse(dc, thumb.left, thumb.top, thumb.right, thumb.bottom);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(thumbPen);
    DeleteObject(thumbBrush);
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
    HICON icon = reinterpret_cast<HICON>(SendMessageW(draw->hwndItem, BM_GETIMAGE, IMAGE_ICON, 0));
    if (icon) {
        const int iconSize = static_cast<int>(theme.metric(L"toolbarItem", L"iconSize", 16.0f));
        const int iconGap = text.empty() ? 0 : static_cast<int>(theme.metric(L"toolbarItem", L"iconGap", 6.0f));
        SIZE textSize{};
        if (!text.empty()) GetTextExtentPoint32W(draw->hDC, text.c_str(), static_cast<int>(text.size()), &textSize);
        const int contentWidth = iconSize + iconGap + textSize.cx;
        const int iconX = rect.left + std::max(0, (static_cast<int>(rect.right - rect.left) - contentWidth) / 2) + (pressed ? 1 : 0);
        const int iconY = rect.top + ((rect.bottom - rect.top) - iconSize) / 2 + (pressed ? 1 : 0);
        if (disabled) {
            DrawStateW(draw->hDC, nullptr, nullptr, reinterpret_cast<LPARAM>(icon), 0, iconX, iconY, iconSize, iconSize, DST_ICON | DSS_DISABLED);
        } else {
            DrawIconEx(draw->hDC, iconX, iconY, icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
        }
        if (!text.empty()) textRect.left = iconX + iconSize + iconGap;
    }
    DrawTextW(draw->hDC, text.c_str(), static_cast<int>(text.size()), &textRect, icon ? (DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) : (DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS));
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
    DrawTextW(draw->hDC, text.c_str(), static_cast<int>(text.size()), &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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
    const int size = static_cast<int>(theme.metric(L"miniButton", L"arrowSize", 5.0f));
    const int stroke = static_cast<int>(theme.metric(L"miniButton", L"arrowStrokeWidth", 2.0f));
    const int cy = (rect.top + rect.bottom) / 2 + (pressed ? static_cast<int>(theme.metric(L"miniButton", L"pressedOffset", 1.0f)) : 0);
    const int cx = (rect.left + rect.right) / 2 + (pressed ? static_cast<int>(theme.metric(L"miniButton", L"pressedOffset", 1.0f)) : 0);
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
    HPEN pen = CreatePen(PS_SOLID, std::max(1, stroke), ToColorRef(theme.color(L"miniButton", state, L"icon")));
    HGDIOBJ oldPen = SelectObject(draw->hDC, pen);
    Polyline(draw->hDC, points, 3);
    SelectObject(draw->hDC, oldPen);
    DeleteObject(pen);
}

void DrawCheckBox(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const bool focused = (draw->itemState & ODS_FOCUS) != 0;
    const bool checked = IsChecked(draw->hwndItem);
    const bool hover = IsHover(draw->hwndItem);
    const wchar_t* state = disabled ? L"disabled" : (checked ? (hover ? L"checkedHover" : L"checked") : (hover ? L"hover" : L"normal"));

    RECT rect = draw->rcItem;
    const std::wstring backgroundComponent = BackgroundComponent(draw->hwndItem);
    HBRUSH bg = CreateSolidBrush(ToColorRef(theme.color(backgroundComponent, L"normal", L"bg")));
    FillRect(draw->hDC, &rect, bg);
    DeleteObject(bg);

    const int radius = static_cast<int>(theme.metric(L"checkbox", L"radius", 4.0f));
    RECT box = ThemedControls::CheckBoxBoxRect(theme, rect);

    FillRoundRect(
        draw->hDC,
        box,
        radius,
        ToColorRef(theme.color(L"checkbox", state, L"boxBg")),
        ToColorRef(theme.color(L"checkbox", focused ? L"focused" : state, L"border")),
        static_cast<int>(theme.metric(L"checkbox", L"borderWidth", 1.0f)));

    if (checked) {
        const int boxSize = box.right - box.left;
        const int markWidth = static_cast<int>(theme.metric(L"checkbox", L"markWidth", 2.0f));
        HPEN pen = CreatePen(PS_SOLID, std::max(1, markWidth), ToColorRef(theme.color(L"checkbox", L"checked", L"mark")));
        HGDIOBJ oldPen = SelectObject(draw->hDC, pen);
        MoveToEx(draw->hDC, box.left + boxSize / 4, box.top + boxSize / 2, nullptr);
        LineTo(draw->hDC, box.left + boxSize * 7 / 16, box.bottom - boxSize / 4);
        LineTo(draw->hDC, box.right - boxSize / 5, box.top + boxSize / 4);
        SelectObject(draw->hDC, oldPen);
        DeleteObject(pen);
    }

    const bool multiline = [&] { auto s = FindState(draw->hwndItem); return s && s->multiline; }();
    RECT textRect = ThemedControls::CheckBoxTextRect(theme, rect);
    if (multiline) {
        textRect.top = rect.top + static_cast<int>(theme.metric(L"checkbox", L"textOffsetY", 1.0f));
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
    DrawTextW(draw->hDC, text.c_str(), static_cast<int>(text.size()), &textRect, format);
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
    HBRUSH bg = CreateSolidBrush(ToColorRef(theme.color(backgroundComponent, L"normal", L"bg")));
    FillRect(draw->hDC, &rect, bg);
    DeleteObject(bg);

    const int trackWidth = static_cast<int>(theme.metric(L"toggle", L"width", 38.0f));
    const int trackHeight = static_cast<int>(theme.metric(L"toggle", L"height", 24.0f));
    const int thumbSize = static_cast<int>(theme.metric(L"toggle", L"thumbSize", 18.0f));
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
    HBRUSH thumbBrush = CreateSolidBrush(ToColorRef(theme.color(L"toggle", state, L"thumb")));
    HGDIOBJ oldBrush = SelectObject(draw->hDC, thumbBrush);
    HGDIOBJ oldPen = SelectObject(draw->hDC, GetStockObject(NULL_PEN));
    Ellipse(draw->hDC, thumb.left, thumb.top, thumb.right, thumb.bottom);
    SelectObject(draw->hDC, oldPen);
    SelectObject(draw->hDC, oldBrush);
    DeleteObject(thumbBrush);

    const int gap = static_cast<int>(theme.metric(L"toggle", L"gap", 8.0f));
    RECT textRect{track.right + gap, rect.top, rect.right, rect.bottom};
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, ToColorRef(theme.color(L"toggle", state, L"text")));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(draw->hDC, font) : nullptr;
    const std::wstring text = ControlText(draw->hwndItem);
    DrawTextW(draw->hDC, text.c_str(), static_cast<int>(text.size()), &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont) SelectObject(draw->hDC, oldFont);
}

void DrawRadioButton(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const bool checked = IsChecked(draw->hwndItem);
    const bool hover = IsHover(draw->hwndItem);
    const wchar_t* state = disabled ? L"disabled" : (checked ? L"checked" : (hover ? L"hover" : L"normal"));
    RECT rect = draw->rcItem;
    const std::wstring backgroundComponent = BackgroundComponent(draw->hwndItem);
    HBRUSH bg = CreateSolidBrush(ToColorRef(theme.color(backgroundComponent, L"normal", L"bg")));
    FillRect(draw->hDC, &rect, bg);
    DeleteObject(bg);

    const int dotSize = static_cast<int>(theme.metric(L"radio", L"dotSize", 16.0f));
    RECT dot{rect.left, rect.top + (rect.bottom - rect.top - dotSize) / 2, rect.left + dotSize, 0};
    dot.bottom = dot.top + dotSize;
    HBRUSH dotBrush = CreateSolidBrush(ToColorRef(theme.color(L"radio", state, L"dot")));
    HPEN dotPen = CreatePen(PS_SOLID, 1, ToColorRef(theme.color(L"radio", state, L"border")));
    HGDIOBJ oldBrush = SelectObject(draw->hDC, dotBrush);
    HGDIOBJ oldPen = SelectObject(draw->hDC, dotPen);
    Ellipse(draw->hDC, dot.left, dot.top, dot.right, dot.bottom);
    SelectObject(draw->hDC, oldPen);
    SelectObject(draw->hDC, oldBrush);
    DeleteObject(dotPen);
    DeleteObject(dotBrush);
    if (checked) {
        const int innerSize = static_cast<int>(theme.metric(L"radio", L"innerDotSize", 8.0f));
        RECT inner{dot.left + (dotSize - innerSize) / 2, dot.top + (dotSize - innerSize) / 2, 0, 0};
        inner.right = inner.left + innerSize;
        inner.bottom = inner.top + innerSize;
        HBRUSH innerBrush = CreateSolidBrush(ToColorRef(theme.color(L"radio", L"checked", L"border")));
        oldBrush = SelectObject(draw->hDC, innerBrush);
        oldPen = SelectObject(draw->hDC, GetStockObject(NULL_PEN));
        Ellipse(draw->hDC, inner.left, inner.top, inner.right, inner.bottom);
        SelectObject(draw->hDC, oldPen);
        SelectObject(draw->hDC, oldBrush);
        DeleteObject(innerBrush);
    }
    const int gap = static_cast<int>(theme.metric(L"radio", L"gap", 8.0f));
    RECT textRect{dot.right + gap, rect.top, rect.right, rect.bottom};
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, ToColorRef(theme.color(L"radio", state, L"text")));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(draw->hDC, font) : nullptr;
    const std::wstring text = ControlText(draw->hwndItem);
    DrawTextW(draw->hDC, text.c_str(), static_cast<int>(text.size()), &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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
        static_cast<int>(theme.metric(L"edit", L"radius", 7.0f)),
        ToColorRef(theme.color(L"edit", state, L"bg")),
        ToColorRef(theme.color(L"edit", state, L"border")),
        static_cast<int>(theme.metric(L"edit", L"borderWidth", 1.0f)));
    InflateRect(&rect, -ThemedControls::EditPaddingX(theme), 0);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, ToColorRef(theme.color(L"edit", state, L"text")));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(draw->hDC, font) : nullptr;
    const std::wstring text = ControlText(draw->hwndItem);
    DrawTextW(draw->hDC, text.c_str(), static_cast<int>(text.size()), &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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
    HBRUSH background = CreateSolidBrush(ToColorRef(theme.color(backgroundComponent, L"normal", L"bg")));
    FillRect(draw->hDC, &rect, background);
    DeleteObject(background);

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
    DrawTextW(draw->hDC, text.c_str(), static_cast<int>(text.size()), &rect, format);
    if (oldFont) SelectObject(draw->hDC, oldFont);

    if (focused) {
        RECT focusRect = rect;
        InflateRect(&focusRect, -1, -2);
        DrawFocusRect(draw->hDC, &focusRect);
    }
}

void DrawContainer(const Theme& theme, const DRAWITEMSTRUCT* draw, ControlKind kind) {
    RECT rect = draw->rcItem;
    const wchar_t* component = kind == ControlKind::Panel ? L"panel"
        : (kind == ControlKind::GroupBox ? L"groupBox"
        : (kind == ControlKind::TabControl ? L"tabControl" : L"toolbar"));

    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const auto state = FindState(draw->hwndItem);
    const wchar_t* visualState = disabled ? L"disabled"
        : (kind == ControlKind::Panel && state && !state->statusState.empty() ? state->statusState.c_str()
        : ((kind == ControlKind::GroupBox && state && state->checked) ? L"raised" : L"normal"));
    const int radius = static_cast<int>(theme.metric(component, L"radius", 7.0f));
    const int borderWidth = static_cast<int>(theme.metric(component, L"borderWidth", 1.0f));
    FillRoundRect(
        draw->hDC, rect, radius,
        ToColorRef(theme.color(component, visualState, L"bg")),
        ToColorRef(theme.color(component, visualState, L"border")),
        borderWidth);
    if (kind != ControlKind::GroupBox) return;

    const std::wstring title = ControlText(draw->hwndItem);
    if (title.empty()) return;
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(draw->hDC, font) : nullptr;
    SIZE size{};
    GetTextExtentPoint32W(draw->hDC, title.c_str(), static_cast<int>(title.size()), &size);
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
    HBRUSH background = CreateSolidBrush(ToColorRef(theme.color(L"groupBox", visualState, L"bg")));
    FillRect(draw->hDC, &titleBg, background);
    DeleteObject(background);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, ToColorRef(theme.color(L"groupBox", visualState, L"text")));
    RECT titleRect{rect.left + inset, titleTop, rect.right - inset, titleBg.bottom};
    DrawTextW(draw->hDC, title.c_str(), static_cast<int>(title.size()), &titleRect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont) SelectObject(draw->hDC, oldFont);
}

void DrawSeparator(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    RECT rect = draw->rcItem;
    const auto state = FindState(draw->hwndItem);
    const bool vertical = state && state->checked;
    HPEN pen = CreatePen(
        PS_SOLID,
        std::max(1, static_cast<int>(theme.metric(L"separator", L"thickness", 1.0f))),
        ToColorRef(theme.color(L"separator", L"normal", L"line")));
    HGDIOBJ oldPen = SelectObject(draw->hDC, pen);
    if (vertical) {
        const int x = (rect.left + rect.right) / 2;
        MoveToEx(draw->hDC, x, rect.top + 3, nullptr);
        LineTo(draw->hDC, x, rect.bottom - 3);
    } else {
        const int y = (rect.top + rect.bottom) / 2;
        MoveToEx(draw->hDC, rect.left + 3, y, nullptr);
        LineTo(draw->hDC, rect.right - 3, y);
    }
    SelectObject(draw->hDC, oldPen);
    DeleteObject(pen);
}

void DrawTabButton(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const bool selected = [&] { auto s = FindState(draw->hwndItem); return s && s->selected; }();
    const bool hover = IsHover(draw->hwndItem);
    const wchar_t* state = disabled ? L"disabled" : (selected ? (hover ? L"selectedHover" : L"selected") : (hover ? L"hover" : L"normal"));

    RECT rect = draw->rcItem;
    const int radius = static_cast<int>(theme.metric(L"tabButton", L"radius", 7.0f));
    const int borderWidth = static_cast<int>(theme.metric(L"tabButton", L"borderWidth", 1.0f));
    if (theme.metric(L"tabButton", L"segmented", 0.0f) > 0.5f) {
        HBRUSH bg = CreateSolidBrush(ToColorRef(theme.color(L"tabButton", L"normal", L"bg")));
        FillRect(draw->hDC, &rect, bg);
        DeleteObject(bg);

        RECT segmentRect = rect;
        const int inset = static_cast<int>(theme.metric(L"tabButton", L"segmentInset", 2.0f) + 0.5f);
        InflateRect(&segmentRect, -inset, -inset);
        FillRoundRect(
            draw->hDC,
            segmentRect,
            radius,
            ToColorRef(theme.color(L"tabButton", state, L"bg")),
            ToColorRef(theme.color(L"tabButton", state, L"border")),
            borderWidth);
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
    HICON icon = reinterpret_cast<HICON>(SendMessageW(draw->hwndItem, BM_GETIMAGE, IMAGE_ICON, 0));
    if (icon) {
        const int iconSize = static_cast<int>(theme.metric(L"toolbarItem", L"iconSize", 16.0f));
        const int iconGap = text.empty() ? 0 : static_cast<int>(theme.metric(L"toolbarItem", L"iconGap", 6.0f));
        SIZE textSize{};
        if (!text.empty()) GetTextExtentPoint32W(draw->hDC, text.c_str(), static_cast<int>(text.size()), &textSize);
        const int contentWidth = iconSize + iconGap + textSize.cx;
        const int iconX = rect.left + std::max(0, (static_cast<int>(rect.right - rect.left) - contentWidth) / 2);
        const int iconY = rect.top + ((rect.bottom - rect.top) - iconSize) / 2;
        if (disabled) {
            DrawStateW(draw->hDC, nullptr, nullptr, reinterpret_cast<LPARAM>(icon), 0, iconX, iconY, iconSize, iconSize, DST_ICON | DSS_DISABLED);
        } else {
            DrawIconEx(draw->hDC, iconX, iconY, icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
        }
        if (!text.empty()) textRect.left = iconX + iconSize + iconGap;
    }
    DrawTextW(draw->hDC, text.c_str(), static_cast<int>(text.size()), &textRect, icon ? (DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) : (DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS));
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
    HBRUSH brush = CreateSolidBrush(ToColorRef(theme.color(L"comboBox", state, L"itemBg")));
    FillRect(draw->hDC, &rect, brush);
    DeleteObject(brush);

    if (draw->itemID == static_cast<UINT>(-1)) {
        return;
    }
    const std::wstring text = ComboBoxItemText(draw->hwndItem, draw->itemID);
    RECT textRect = ThemedControls::ComboBoxItemTextRect(theme, rect);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, ToColorRef(theme.color(L"comboBox", state, L"text")));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(draw->hDC, font) : nullptr;
    DrawTextW(draw->hDC, text.c_str(), static_cast<int>(text.size()), &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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

    HBRUSH brush = CreateSolidBrush(ToColorRef(theme.color(L"listItem", state, L"bg")));
    FillRect(draw->hDC, &rect, brush);
    DeleteObject(brush);

    if (draw->itemID == static_cast<UINT>(-1)) {
        return;
    }

    wchar_t buffer[1024]{};
    SendMessageW(draw->hwndItem, LB_GETTEXT, draw->itemID, reinterpret_cast<LPARAM>(buffer));
    RECT textRect = ThemedControls::ListItemTextRect(theme, rect);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, ToColorRef(theme.color(L"listItem", state, L"text")));
    DrawTextW(draw->hDC, buffer, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

std::wstring ClassName(HWND hwnd) {
    wchar_t buffer[128]{};
    GetClassNameW(hwnd, buffer, static_cast<int>(sizeof(buffer) / sizeof(buffer[0])));
    return buffer;
}

void DrawHeaderItem(const Theme& theme, HWND header, const NMCUSTOMDRAW* draw) {
    const bool table = KindFor(GetParent(header)) == ControlKind::Table;
    const wchar_t* component = table ? L"tableHeader" : L"list";
    RECT rect = draw->rc;
    HBRUSH brush = CreateSolidBrush(ToColorRef(theme.color(component, L"normal", L"bg")));
    FillRect(draw->hdc, &rect, brush);
    DeleteObject(brush);

    const int index = static_cast<int>(draw->dwItemSpec);
    wchar_t text[256]{};
    HDITEMW item{};
    item.mask = HDI_TEXT | HDI_FORMAT;
    item.pszText = text;
    item.cchTextMax = static_cast<int>(sizeof(text) / sizeof(text[0]));
    Header_GetItem(header, index, &item);

    const COLORREF line = ToColorRef(theme.color(component, L"normal", L"border"));
    HPEN pen = CreatePen(PS_SOLID, 1, line);
    HGDIOBJ oldPen = SelectObject(draw->hdc, pen);
    MoveToEx(draw->hdc, rect.left, rect.bottom - 1, nullptr);
    LineTo(draw->hdc, rect.right, rect.bottom - 1);
    MoveToEx(draw->hdc, rect.right - 1, rect.top, nullptr);
    LineTo(draw->hdc, rect.right - 1, rect.bottom);
    SelectObject(draw->hdc, oldPen);
    DeleteObject(pen);

    const int paddingX = static_cast<int>(theme.metric(L"listItem", L"paddingX", 8.0f));
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
    DrawTextW(draw->hdc, text, -1, &textRect, format);
    if (oldFont) {
        SelectObject(draw->hdc, oldFont);
    }
}
}

namespace ThemedControls {

bool IsControlHovered(HWND hwnd) {
    return IsHover(hwnd);
}

namespace {
constexpr int kDialogFontPx = 14;
constexpr int kSingleLineEditFontPx = 14;

HFONT CreateThemeFontPx(int pixelHeight) {
    return CreateFontW(
        -pixelHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

RECT TextRectFromMetrics(const Theme& theme, const wchar_t* component, RECT frame, float fallbackHeight, bool center, bool pressed = false) {
    const int paddingX = static_cast<int>(theme.metric(component, L"paddingX", 0.0f));
    const int textHeight = static_cast<int>(theme.metric(component, L"textHeight", fallbackHeight));
    const int offsetY = static_cast<int>(theme.metric(component, L"textOffsetY", 0.0f));
    const int pressedOffset = pressed ? static_cast<int>(theme.metric(component, L"pressedOffset", 1.0f)) : 0;
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
    const int radius = static_cast<int>(theme.metric(L"iconButton", L"radius", 7.0f));
    const COLORREF border = focused
        ? ToColorRef(theme.color(L"button", L"focused", L"border"))
        : ToColorRef(theme.color(L"panel", L"normal", L"border"));
    FillRoundRect(
        dc,
        rect,
        radius,
        ToColorRef(theme.color(L"iconButton", state, L"bg")),
        border,
        1);
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
        static_cast<int>(theme.metric(L"miniButton", L"radius", 6.0f)),
        ToColorRef(theme.color(L"miniButton", state, L"bg")),
        ToColorRef(theme.color(L"miniButton", focused ? L"focused" : state, L"border")),
        static_cast<int>(theme.metric(L"miniButton", L"borderWidth", 1.0f)));
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
    const int boxSize = static_cast<int>(theme.metric(L"checkbox", L"boxSize", 16.0f));
    const int offsetX = static_cast<int>(theme.metric(L"checkbox", L"boxOffsetX", 0.0f));
    const int offsetY = static_cast<int>(theme.metric(L"checkbox", L"boxOffsetY", 0.0f));
    RECT box{};
    box.left = frame.left + offsetX;
    box.top = frame.top + ((frame.bottom - frame.top) - boxSize) / 2 + offsetY;
    box.right = box.left + boxSize;
    box.bottom = box.top + boxSize;
    return box;
}

RECT CheckBoxTextRect(const Theme& theme, RECT frame) {
    const RECT box = CheckBoxBoxRect(theme, frame);
    const int gap = static_cast<int>(theme.metric(L"checkbox", L"gap", 8.0f));
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
    const int minTextWidth = static_cast<int>(theme.metric(L"tabButton", L"minTextWidth", 18.0f));
    if (width > 0 && (rect.right - rect.left) < minTextWidth) {
        const int paddingX = std::max(2, (width - minTextWidth) / 2);
        rect.left = frame.left + paddingX;
        rect.right = frame.right - paddingX;
    }
    return rect;
}

RECT TabGroupInnerRect(const Theme& theme, RECT frame) {
    const int padding = static_cast<int>(theme.metric(L"tabButton", L"groupPadding", 3.0f));
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

int ComboBoxDropdownHeight(const Theme& theme) {
    return std::max(EditFrameHeight(theme), EditFrameHeight(theme) + ComboBoxItemHeight(theme) * 6);
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

RECT ListFrameInnerRect(const Theme& theme, RECT frame) {
    const int inset = std::max(1, static_cast<int>(theme.metric(L"list", L"borderWidth", 1.0f)));
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

HWND CreateLabelText(HINSTANCE instance, HWND parent, const wchar_t* text, int x, int y, int width, const Theme& theme, HFONT font, DWORD style) {
    return CreateStaticText(instance, parent, text, x, y, width, LabelHeight(theme), font, style);
}

HWND CreateStatusBadge(HINSTANCE instance, HWND parent, const wchar_t* text, int x, int y, int width, const Theme& theme, HFONT font, const wchar_t* state) {
    HWND hwnd = CreateWindowExW(
        0,
        L"STATIC",
        text,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_OWNERDRAW,
        x,
        y,
        width,
        LabelHeight(theme),
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

HWND CreateStatusText(HINSTANCE instance, HWND parent, const wchar_t* text, int x, int y, int width, const Theme& theme, HFONT font, const wchar_t* state, DWORD style) {
    HWND hwnd = CreateWindowExW(
        0,
        L"STATIC",
        text,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_OWNERDRAW | style,
        x,
        y,
        width,
        LabelHeight(theme),
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
            || kind == ControlKind::GroupBox
            || kind == ControlKind::Toggle || kind == ControlKind::Radio || kind == ControlKind::StatusBadge || kind == ControlKind::StatusText) {
            StateFor(hwnd).theme = &theme;
        } else {
            StateFor(hwnd).theme = nullptr;
        }
        InvalidateRect(hwnd, nullptr, TRUE);
    }
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

HWND CreateToggle(HINSTANCE instance, HWND parent, int id, const wchar_t* text, int x, int y, int width, HFONT font, const Theme& theme, bool checked) {
    HWND hwnd = CreateWindowExW(
        0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | BS_OWNERDRAW,
        x, y, width, ToggleHeight(theme), parent,
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

HWND CreateRadioButton(HINSTANCE instance, HWND parent, int id, const wchar_t* text, int x, int y, int width, HFONT font, const Theme& theme, int group, bool checked) {
    HWND hwnd = CreateWindowExW(
        0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | BS_OWNERDRAW,
        x, y, width, RadioButtonHeight(theme), parent,
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

HWND CreateHotKeyCapture(HINSTANCE instance, HWND parent, int id, const wchar_t* text, int x, int y, int width, HFONT font, const Theme& theme) {
    HWND hwnd = CreateWindowExW(
        0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | BS_OWNERDRAW,
        x, y, width, EditFrameHeight(theme), parent,
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
    const int insetX = static_cast<int>(state->theme->metric(L"panel", L"contentInsetX", 10.0f));
    const int insetY = static_cast<int>(state->theme->metric(L"panel", L"contentInsetY", 8.0f));
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

HWND CreateComboBox(HINSTANCE instance, HWND parent, int id, int x, int y, int width, int height, HFONT font, const Theme& theme) {
    HWND hwnd = CreateWindowExW(0, WC_COMBOBOXW, nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                                x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(hwnd, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), static_cast<LPARAM>(ComboBoxHeight(theme)));
        SendMessageW(hwnd, CB_SETITEMHEIGHT, 0, static_cast<LPARAM>(ComboBoxItemHeight(theme)));
        {
            auto& s = StateFor(hwnd);
            s.kind = ControlKind::ComboBox;
            s.theme = &theme;
        }
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

HWND CreateListBox(HINSTANCE instance, HWND parent, int id, int x, int y, int width, int height, HFONT font, const Theme& theme, DWORD extraStyle) {
    HWND hwnd = CreateWindowExW(
        0,
        L"LISTBOX",
        nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_OWNERDRAWFIXED | extraStyle,
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
        SendMessageW(hwnd, LB_SETITEMHEIGHT, 0, static_cast<LPARAM>(ListBoxItemHeight(theme)));
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

RECT SingleLineEditRect(const Theme& theme, RECT frame) {
    const int paddingX = EditPaddingX(theme);
    const int controlHeight = static_cast<int>(theme.metric(L"edit", L"singleLineControlHeight", static_cast<float>(EditTextHeight(theme))));
    const int offsetY = static_cast<int>(theme.metric(L"edit", L"singleLineOffsetY", theme.metric(L"edit", L"textOffsetY", 0.0f)));
    RECT rect{};
    rect.left = frame.left + paddingX;
    rect.right = frame.right - paddingX;
    rect.top = frame.top + ((frame.bottom - frame.top) - controlHeight) / 2 + offsetY;
    rect.bottom = rect.top + controlHeight;
    return rect;
}

RECT SingleLineEditRectForFrame(const Theme& theme, RECT frame) {
    const int paddingX = EditPaddingX(theme);
    const int preferredHeight = static_cast<int>(theme.metric(L"edit", L"singleLineControlHeight", static_cast<float>(EditTextHeight(theme))));
    const int frameHeight = std::max(1, static_cast<int>(frame.bottom - frame.top));
    const int controlHeight = std::max(1, std::min(preferredHeight, frameHeight - 2));
    const int offsetY = static_cast<int>(theme.metric(L"edit", L"singleLineOffsetY", theme.metric(L"edit", L"textOffsetY", 0.0f)));
    RECT rect{};
    rect.left = frame.left + paddingX;
    rect.right = frame.right - paddingX;
    rect.top = frame.top + ((frame.bottom - frame.top) - controlHeight) / 2 + offsetY;
    rect.bottom = rect.top + controlHeight;
    return rect;
}

RECT MultiLineEditRect(const Theme& theme, RECT frame) {
    const int paddingX = EditPaddingX(theme);
    const int paddingTop = static_cast<int>(theme.metric(L"edit", L"multiLinePaddingTop", 7.0f));
    const int paddingBottom = static_cast<int>(theme.metric(L"edit", L"multiLinePaddingBottom", 7.0f));
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
    const RECT editRect = SingleLineEditRect(theme, frame);
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
    const RECT editRect = MultiLineEditRect(theme, frame);
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

HWND CreateFramedStatic(HINSTANCE instance, HWND parent, const Theme& theme, RECT frame, const std::wstring& value, HFONT font, DWORD style) {
    const RECT textRect = FieldTextRect(theme, frame);
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
    const bool disabled = child && !IsWindowEnabled(child);
    const bool focused = child && GetFocus() == child;
    const bool hover = child && IsHover(child);
    const wchar_t* component = readOnly ? L"field" : L"edit";
    const wchar_t* state = disabled ? L"disabled" : (error ? L"error" : (focused ? L"focused" : (hover ? L"hover" : (readOnly ? L"readonly" : L"normal"))));
    FillRoundRect(
        dc,
        rect,
        static_cast<int>(theme.metric(component, L"radius", 7.0f)),
        ToColorRef(theme.color(component, state, L"bg")),
        ToColorRef(theme.color(component, state, L"border")),
        static_cast<int>(theme.metric(component, L"borderWidth", 1.0f)));
}

void DrawEditFrame(const Theme& theme, HDC dc, RECT rect, HWND child, bool readOnly, bool error) {
    const bool disabled = child && !IsWindowEnabled(child);
    const bool focused = child && GetFocus() == child;
    const bool hover = child && IsHover(child);
    const wchar_t* state = disabled ? L"disabled" : (error ? L"error" : (focused ? L"focused" : (hover ? L"hover" : (readOnly ? L"readonly" : L"normal"))));
    FillRoundRect(
        dc,
        rect,
        static_cast<int>(theme.metric(L"edit", L"radius", 7.0f)),
        ToColorRef(theme.color(L"edit", state, L"bg")),
        ToColorRef(theme.color(L"edit", state, L"border")),
        static_cast<int>(theme.metric(L"edit", L"borderWidth", 1.0f)));
}

void DrawComboFrame(const Theme& theme, HDC dc, RECT rect, HWND child) {
    const bool disabled = child && !IsWindowEnabled(child);
    const bool focused = child && GetFocus() == child;
    const bool hover = child && IsHover(child);
    const wchar_t* state = disabled ? L"disabled" : (focused ? L"focused" : (hover ? L"hover" : L"normal"));
    FillRoundRect(
        dc,
        rect,
        static_cast<int>(theme.metric(L"comboBox", L"radius", 7.0f)),
        ToColorRef(theme.color(L"comboBox", state, L"bg")),
        ToColorRef(theme.color(L"comboBox", state, L"border")),
        static_cast<int>(theme.metric(L"comboBox", L"borderWidth", 1.0f)));
}

void DrawListFrame(const Theme& theme, HDC dc, RECT rect, HWND child, bool readOnly) {
    const bool disabled = child && !IsWindowEnabled(child);
    const bool focused = child && GetFocus() == child;
    const wchar_t* state = disabled ? L"disabled" : (focused ? L"focused" : (readOnly ? L"readonly" : L"normal"));
    FillRoundRect(
        dc,
        rect,
        static_cast<int>(theme.metric(L"list", L"radius", 7.0f)),
        ToColorRef(theme.color(L"list", state, L"bg")),
        ToColorRef(theme.color(L"list", state, L"border")),
        static_cast<int>(theme.metric(L"list", L"borderWidth", 1.0f)));
}

void DrawPanelFrame(const Theme& theme, HDC dc, RECT rect, bool raised) {
    const wchar_t* state = raised ? L"raised" : L"normal";
    FillRoundRect(
        dc,
        rect,
        static_cast<int>(theme.metric(L"panel", L"radius", 7.0f)),
        ToColorRef(theme.color(L"panel", state, L"bg")),
        ToColorRef(theme.color(L"panel", state, L"border")),
        static_cast<int>(theme.metric(L"panel", L"borderWidth", 1.0f)));
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

void RegisterTable(HWND table, const Theme& theme) {
    if (!table) return;
    auto& state = StateFor(table);
    state.kind = ControlKind::Table;
    state.theme = &theme;
    AttachThemedBehavior(table);
    ApplyListViewTheme(table, theme);
}

void ConfigureTableColumns(HWND table, const std::vector<int>& widthModes) {
    if (!table) return;
    StateFor(table).tableColumnWidthModes = widthModes;
}

void SetTableRowEnabledStates(HWND table, const std::vector<bool>& enabled) {
    if (!table) return;
    StateFor(table).tableRowEnabled = enabled;
}

bool IsTableRowEnabled(HWND table, int index) {
    const auto state = FindState(table);
    return state && index >= 0 && static_cast<std::size_t>(index) < state->tableRowEnabled.size()
        ? state->tableRowEnabled[static_cast<std::size_t>(index)]
        : true;
}

void DrawTabGroupFrame(const Theme& theme, HDC dc, RECT rect) {
    FillRoundRect(
        dc,
        rect,
        static_cast<int>(theme.metric(L"tabButton", L"groupRadius", 10.0f)),
        ToColorRef(theme.color(L"tabButton", L"normal", L"groupBg")),
        ToColorRef(theme.color(L"tabButton", L"normal", L"groupBorder")),
        static_cast<int>(theme.metric(L"tabButton", L"groupBorderWidth", 1.0f)));
}

bool Draw(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    if (!draw || !draw->hwndItem) {
        return false;
    }
    ControlKind kind = KindFor(draw->hwndItem);
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
    if (!header || header->code != NM_CUSTOMDRAW) {
        return false;
    }

    const std::wstring className = ClassName(header->hwndFrom);
    if (className == L"SysHeader32") {
        auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(lParam);
        switch (draw->dwDrawStage) {
        case CDDS_PREPAINT:
            result = CDRF_NOTIFYITEMDRAW;
            return true;
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
        const bool selected = (draw->nmcd.uItemState & CDIS_SELECTED) != 0;
        const bool focused = (draw->nmcd.uItemState & CDIS_FOCUS) != 0;
        const bool enabled = IsTableRowEnabled(header->hwndFrom, static_cast<int>(draw->nmcd.dwItemSpec));
        const wchar_t* state = !enabled ? L"disabled" : (selected ? L"selected" : (focused ? L"focused" : L"normal"));
        draw->clrText = ToColorRef(theme.color((selected && enabled) ? L"listItem" : L"listItem", state, L"text"));
        draw->clrTextBk = ToColorRef(theme.color((selected && enabled) ? L"listItem" : L"list", state, L"bg"));
        result = CDRF_DODEFAULT;
        return true;
    }
    default:
        break;
    }
    return false;
}

}
