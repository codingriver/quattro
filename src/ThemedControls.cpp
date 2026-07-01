#include "ThemedControls.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <string>

namespace {
const wchar_t kControlKindProp[] = L"QuattroThemedControlKind";
const wchar_t kControlHoverProp[] = L"QuattroThemedControlHover";
const wchar_t kControlThemeProp[] = L"QuattroThemedControlTheme";
const wchar_t kControlSelectedProp[] = L"QuattroThemedControlSelected";
const wchar_t kControlCheckedProp[] = L"QuattroThemedControlChecked";
HANDLE ButtonKind() {
    return reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1));
}

HANDLE MiniButtonKind() {
    return reinterpret_cast<HANDLE>(static_cast<INT_PTR>(5));
}

HANDLE CheckBoxKind() {
    return reinterpret_cast<HANDLE>(static_cast<INT_PTR>(2));
}

HANDLE TabButtonKind() {
    return reinterpret_cast<HANDLE>(static_cast<INT_PTR>(3));
}

HANDLE ComboBoxKind() {
    return reinterpret_cast<HANDLE>(static_cast<INT_PTR>(4));
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

std::wstring WindowText(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
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

bool IsHover(HWND hwnd) {
    return GetPropW(hwnd, kControlHoverProp) != nullptr;
}

bool IsChecked(HWND hwnd) {
    return GetPropW(hwnd, kControlCheckedProp) != nullptr;
}

void SetChecked(HWND hwnd, bool checked) {
    if (checked) {
        SetPropW(hwnd, kControlCheckedProp, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
    } else {
        RemovePropW(hwnd, kControlCheckedProp);
    }
    InvalidateRect(hwnd, nullptr, TRUE);
}

void ToggleChecked(HWND hwnd) {
    SetChecked(hwnd, !IsChecked(hwnd));
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

void DrawComboOverlay(HWND hwnd, HDC targetDc = nullptr) {
    const auto* theme = reinterpret_cast<const Theme*>(GetPropW(hwnd, kControlThemeProp));
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

    wchar_t buffer[512]{};
    const LRESULT selected = SendMessageW(hwnd, CB_GETCURSEL, 0, 0);
    if (selected >= 0) {
        SendMessageW(hwnd, CB_GETLBTEXT, static_cast<WPARAM>(selected), reinterpret_cast<LPARAM>(buffer));
    }

    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = nullptr;
    if (font) {
        oldFont = SelectObject(dc, font);
    }
    RECT textRect = ComboTextRect(*theme, rect);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ToColorRef(theme->color(L"comboBox", state, L"text")));
    DrawTextW(dc, buffer, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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

LRESULT CALLBACK ThemedControlProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR) {
    switch (message) {
    case BM_SETCHECK:
        if (GetPropW(hwnd, kControlKindProp) == CheckBoxKind()) {
            SetChecked(hwnd, wParam == BST_CHECKED);
            return 0;
        }
        if (GetPropW(hwnd, kControlKindProp) == TabButtonKind()) {
            if (wParam == BST_CHECKED) {
                SetPropW(hwnd, kControlSelectedProp, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
            } else {
                RemovePropW(hwnd, kControlSelectedProp);
            }
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        break;
    case BM_GETCHECK:
        if (GetPropW(hwnd, kControlKindProp) == TabButtonKind()) {
            return GetPropW(hwnd, kControlSelectedProp) != nullptr ? BST_CHECKED : BST_UNCHECKED;
        }
        if (GetPropW(hwnd, kControlKindProp) == CheckBoxKind()) {
            return IsChecked(hwnd) ? BST_CHECKED : BST_UNCHECKED;
        }
        break;
    case BM_CLICK:
        if (GetPropW(hwnd, kControlKindProp) == CheckBoxKind() && IsWindowEnabled(hwnd)) {
            ToggleChecked(hwnd);
        }
        break;
    case WM_LBUTTONUP:
        if (GetPropW(hwnd, kControlKindProp) == CheckBoxKind() && IsWindowEnabled(hwnd)) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT rect{};
            GetClientRect(hwnd, &rect);
            if (PtInRect(&rect, point)) {
                ToggleChecked(hwnd);
            }
        }
        break;
    case WM_KEYUP:
        if (GetPropW(hwnd, kControlKindProp) == CheckBoxKind() && IsWindowEnabled(hwnd) && wParam == VK_SPACE) {
            ToggleChecked(hwnd);
        }
        break;
    case WM_PAINT: {
        LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        if (GetPropW(hwnd, kControlKindProp) == ComboBoxKind()) {
            DrawComboOverlay(hwnd);
        }
        return result;
    }
    case WM_PRINTCLIENT: {
        LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        if (GetPropW(hwnd, kControlKindProp) == ComboBoxKind()) {
            DrawComboOverlay(hwnd, reinterpret_cast<HDC>(wParam));
        }
        return result;
    }
    case WM_MOUSEMOVE:
        if (!IsHover(hwnd)) {
            SetPropW(hwnd, kControlHoverProp, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
            TRACKMOUSEEVENT event{};
            event.cbSize = sizeof(event);
            event.dwFlags = TME_LEAVE;
            event.hwndTrack = hwnd;
            TrackMouseEvent(&event);
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        break;
    case WM_MOUSELEAVE:
        RemovePropW(hwnd, kControlHoverProp);
        InvalidateRect(hwnd, nullptr, TRUE);
        break;
    case WM_DESTROY:
        RemovePropW(hwnd, kControlHoverProp);
        RemovePropW(hwnd, kControlKindProp);
        RemovePropW(hwnd, kControlThemeProp);
        RemovePropW(hwnd, kControlSelectedProp);
        RemovePropW(hwnd, kControlCheckedProp);
        RemoveWindowSubclass(hwnd, ThemedControlProc, subclassId);
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void AttachThemedBehavior(HWND hwnd) {
    SetWindowSubclass(hwnd, ThemedControlProc, 1, 0);
}

void DrawButton(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    const bool focused = (draw->itemState & ODS_FOCUS) != 0;
    const bool hover = IsHover(draw->hwndItem);
    const wchar_t* state = disabled ? L"disabled" : (pressed ? L"pressed" : (hover ? L"hover" : L"normal"));

    RECT rect = draw->rcItem;
    const int radius = static_cast<int>(theme.metric(L"button", L"radius", 6.0f));
    const int borderWidth = static_cast<int>(theme.metric(L"button", L"borderWidth", 1.0f));
    FillRoundRect(
        draw->hDC,
        rect,
        radius,
        ToColorRef(theme.color(L"button", state, L"bg")),
        ToColorRef(theme.color(L"button", focused ? L"focused" : state, L"border")),
        borderWidth);

    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, ToColorRef(theme.color(L"button", state, L"text")));
    std::wstring text = WindowText(draw->hwndItem);
    RECT textRect = ThemedControls::ButtonTextRect(theme, rect, pressed);
    DrawTextW(draw->hDC, text.c_str(), static_cast<int>(text.size()), &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void DrawMiniButton(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    const bool focused = (draw->itemState & ODS_FOCUS) != 0;
    const bool hover = IsHover(draw->hwndItem);
    const wchar_t* state = disabled ? L"disabled" : (pressed ? L"pressed" : (hover ? L"hover" : L"normal"));

    RECT rect = draw->rcItem;
    FillRoundRect(
        draw->hDC,
        rect,
        static_cast<int>(theme.metric(L"miniButton", L"radius", 5.0f)),
        ToColorRef(theme.color(L"miniButton", state, L"bg")),
        ToColorRef(theme.color(L"miniButton", focused ? L"focused" : state, L"border")),
        static_cast<int>(theme.metric(L"miniButton", L"borderWidth", 1.0f)));

    const std::wstring text = WindowText(draw->hwndItem);
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
    HBRUSH bg = CreateSolidBrush(ToColorRef(theme.color(L"dialog", L"normal", L"bg")));
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

    RECT textRect = ThemedControls::CheckBoxTextRect(theme, rect);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, ToColorRef(theme.color(L"checkbox", state, L"text")));
    std::wstring text = WindowText(draw->hwndItem);
    DrawTextW(draw->hDC, text.c_str(), static_cast<int>(text.size()), &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void DrawTabButton(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const bool selected = GetPropW(draw->hwndItem, kControlSelectedProp) != nullptr;
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
    std::wstring text = WindowText(draw->hwndItem);
    RECT textRect = ThemedControls::TabButtonTextRect(theme, rect);
    DrawTextW(draw->hDC, text.c_str(), static_cast<int>(text.size()), &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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
    wchar_t buffer[512]{};
    SendMessageW(draw->hwndItem, CB_GETLBTEXT, draw->itemID, reinterpret_cast<LPARAM>(buffer));
    RECT textRect = ThemedControls::ComboBoxItemTextRect(theme, rect);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, ToColorRef(theme.color(L"comboBox", state, L"text")));
    DrawTextW(draw->hDC, buffer, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
}

namespace ThemedControls {

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
    return static_cast<int>(theme.metric(L"button", L"height", 30.0f));
}

int CompactButtonHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"button", L"compactHeight", 28.0f));
}

int MiniButtonHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"miniButton", L"height", 24.0f));
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
    return ButtonHeight(theme);
}

int ComboBoxItemHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"comboBox", L"itemHeight", 24.0f));
}

RECT ComboBoxItemTextRect(const Theme& theme, RECT frame) {
    return TextRectFromMetrics(theme, L"comboBox", frame, 20.0f, true);
}

int LabelHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"label", L"height", 20.0f));
}

RECT LabelTextRect(const Theme& theme, RECT frame) {
    return TextRectFromMetrics(theme, L"label", frame, 20.0f, true);
}

int FieldFrameHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"field", L"height", 32.0f));
}

RECT FieldTextRect(const Theme& theme, RECT frame) {
    return TextRectFromMetrics(theme, L"field", frame, 20.0f, true);
}

HFONT CreateDialogFont() {
    return CreateThemeFontPx(kDialogFontPx);
}

HFONT CreateEditFont(const Theme& theme) {
    (void)theme;
    return CreateThemeFontPx(kSingleLineEditFontPx);
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

HWND CreateButton(HINSTANCE instance, HWND parent, int id, const wchar_t* text, int x, int y, int width, int height, HFONT font, bool defaultButton) {
    const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | (defaultButton ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON);
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text, style, x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SetPropW(hwnd, kControlKindProp, ButtonKind());
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

HWND CreateMiniButton(HINSTANCE instance, HWND parent, int id, const wchar_t* text, int x, int y, int width, int height, HFONT font) {
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
                                x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SetPropW(hwnd, kControlKindProp, MiniButtonKind());
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

HWND CreateCheckBox(HINSTANCE instance, HWND parent, int id, const wchar_t* text, int x, int y, int width, int height, HFONT font, bool checked) {
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SetPropW(hwnd, kControlKindProp, CheckBoxKind());
        AttachThemedBehavior(hwnd);
        SendMessageW(hwnd, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    return hwnd;
}

HWND CreateTabButton(HINSTANCE instance, HWND parent, int id, const wchar_t* text, int x, int y, int width, int height, HFONT font, bool selected) {
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
                                x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SetPropW(hwnd, kControlKindProp, TabButtonKind());
        if (selected) {
            SetPropW(hwnd, kControlSelectedProp, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
        }
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

HWND CreateComboBox(HINSTANCE instance, HWND parent, int id, int x, int y, int width, int height, HFONT font, const Theme& theme) {
    HWND hwnd = CreateWindowExW(0, WC_COMBOBOXW, nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                                x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    if (hwnd) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(hwnd, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), static_cast<LPARAM>(ComboBoxHeight(theme)));
        SendMessageW(hwnd, CB_SETITEMHEIGHT, 0, static_cast<LPARAM>(ComboBoxItemHeight(theme)));
        SetPropW(hwnd, kControlKindProp, ComboBoxKind());
        SetPropW(hwnd, kControlThemeProp, const_cast<Theme*>(&theme));
        AttachThemedBehavior(hwnd);
    }
    return hwnd;
}

int EditFrameHeight(const Theme& theme) {
    return static_cast<int>(theme.metric(L"edit", L"height", 32.0f));
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

HWND CreateSingleLineEdit(HINSTANCE instance, HWND parent, int id, const Theme& theme, RECT frame, const std::wstring& value, HFONT font, DWORD extraStyle) {
    const RECT editRect = SingleLineEditRect(theme, frame);
    HWND hwnd = CreateWindowExW(
        0,
        L"EDIT",
        value.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | extraStyle,
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
    }
    return hwnd;
}

HWND CreateMultiLineEdit(HINSTANCE instance, HWND parent, int id, const Theme& theme, RECT frame, const std::wstring& value, HFONT font, DWORD extraStyle) {
    const RECT editRect = MultiLineEditRect(theme, frame);
    HWND hwnd = CreateWindowExW(
        0,
        L"EDIT",
        value.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | extraStyle,
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
    HANDLE kind = GetPropW(draw->hwndItem, kControlKindProp);
    if (draw->CtlType == ODT_COMBOBOX && kind == ComboBoxKind()) {
        DrawComboBox(theme, draw);
        return true;
    }
    if (draw->CtlType != ODT_BUTTON) {
        return false;
    }
    if (kind == ButtonKind()) {
        DrawButton(theme, draw);
        return true;
    }
    if (kind == MiniButtonKind()) {
        DrawMiniButton(theme, draw);
        return true;
    }
    if (kind == CheckBoxKind()) {
        DrawCheckBox(theme, draw);
        return true;
    }
    if (kind == TabButtonKind()) {
        DrawTabButton(theme, draw);
        return true;
    }
    return false;
}

}
