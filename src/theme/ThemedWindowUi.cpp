#include "ThemedWindowUi.h"

#include "ThemedControls.h"
#include "Utilities.h"

#include <commctrl.h>
#include <algorithm>

namespace {
std::wstring LastErrorText(const wchar_t* prefix) {
    return std::wstring(prefix) + L": " + FormatLastError(GetLastError());
}

const wchar_t* TooltipState(ThemedTooltipRole role) {
    switch (role) {
    case ThemedTooltipRole::Info: return L"info";
    case ThemedTooltipRole::Warning: return L"warning";
    case ThemedTooltipRole::Danger: return L"danger";
    case ThemedTooltipRole::Normal:
    default: return L"normal";
    }
}
}

ThemedWindowUi::ThemedWindowUi(
    HINSTANCE instance,
    HWND owner,
    HWND hwnd,
    const Theme& theme,
    DialogLayoutKind layoutKind,
    int clientWidth,
    int clientHeight)
    : instance_(instance),
      owner_(owner),
      hwnd_(hwnd),
      theme_(theme),
      layoutKind_(layoutKind),
      clientWidth_(clientWidth),
      clientHeight_(clientHeight) {}

ThemedWindowUi::~ThemedWindowUi() {
    RestoreModalOwner();
    ReleaseResources();
}

SIZE ThemedWindowUi::AdjustedWindowSize(int clientWidth, int clientHeight, DWORD style, DWORD exStyle, bool hasMenu) {
    RECT rect{0, 0, clientWidth, clientHeight};
    AdjustWindowRectEx(&rect, style, hasMenu ? TRUE : FALSE, exStyle);
    return SIZE{rect.right - rect.left, rect.bottom - rect.top};
}

POINT ThemedWindowUi::WindowPosition(const ThemedWindowCreateOptions& options, int windowWidth, int windowHeight) {
    switch (options.placement) {
    case ThemedWindowPlacement::OffsetOwner:
        return OffsetWindowFromOwnerOnMonitor(options.owner, windowWidth, windowHeight, options.offsetX, options.offsetY);
    case ThemedWindowPlacement::Manual:
        return ClampWindowToOwnerMonitor(options.owner, options.x, options.y, windowWidth, windowHeight);
    case ThemedWindowPlacement::CenterOwner:
    default:
        return CenterWindowOnOwnerMonitor(options.owner, windowWidth, windowHeight);
    }
}

ThemedWindowCreateOptions ThemedWindowUi::DialogOptions(
    HINSTANCE instance,
    HWND owner,
    const wchar_t* className,
    const wchar_t* title,
    WNDPROC wndProc,
    void* createParam,
    HICON icon,
    HICON smallIcon) {
    ThemedWindowCreateOptions options{};
    options.instance = instance;
    options.owner = owner;
    options.className = className;
    options.title = title;
    options.wndProc = wndProc;
    options.createParam = createParam;
    options.clientWidth = kThemedDialogClientWidth;
    options.clientHeight = kThemedDialogClientHeight;
    options.icon = icon;
    options.smallIcon = smallIcon;
    return options;
}

HWND ThemedWindowUi::CreateWindowHandle(const ThemedWindowCreateOptions& options, std::wstring* error) {
    if (!options.instance || !options.className || !options.wndProc || options.clientWidth <= 0 || options.clientHeight <= 0) {
        if (error) {
            *error = L"创建主题窗口失败：窗口参数不完整。";
        }
        return nullptr;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = options.wndProc;
    wc.hInstance = options.instance;
    wc.hCursor = options.cursor ? options.cursor : LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = options.icon;
    wc.hIconSm = options.smallIcon;
    wc.lpszClassName = options.className;
    if (!RegisterClassExW(&wc)) {
        const DWORD code = GetLastError();
        if (code != ERROR_CLASS_ALREADY_EXISTS) {
            if (error) {
                *error = LastErrorText(L"注册主题窗口类失败");
            }
            return nullptr;
        }
    }

    const SIZE windowSize = AdjustedWindowSize(options.clientWidth, options.clientHeight, options.style, options.exStyle);
    const POINT position = WindowPosition(options, windowSize.cx, windowSize.cy);
    HWND hwnd = CreateWindowExW(
        options.exStyle,
        options.className,
        options.title ? options.title : L"",
        options.style,
        position.x,
        position.y,
        windowSize.cx,
        windowSize.cy,
        options.owner,
        nullptr,
        options.instance,
        options.createParam);
    if (!hwnd && error) {
        *error = LastErrorText(L"创建主题窗口失败");
    }
    return hwnd;
}

bool ThemedWindowUi::HandleCommonMessage(
    std::unique_ptr<ThemedWindowUi>& windowUi,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    LRESULT& result) {
    if (!windowUi) {
        return false;
    }
    if (!windowUi->HandleMessage(message, wParam, lParam, result)) {
        return false;
    }
    if (message == WM_DESTROY) {
        windowUi.reset();
    }
    return true;
}

HFONT ThemedWindowUi::font() const {
    if (!font_) {
        font_ = ThemedControls::CreateDialogFont();
        if (font_) {
            ownsFont_ = true;
        } else {
            font_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        }
    }
    return font_;
}

ThemedUi ThemedWindowUi::ui() const {
    return ThemedUi(
        instance_, hwnd_, theme_, font(), layoutKind_, clientWidth_, clientHeight_,
        const_cast<ThemedWindowUi*>(this), const_cast<ThemedWindowUi*>(this),
        const_cast<ThemedWindowUi*>(this));
}

bool ThemedWindowUi::ShowModal() {
    ownerWasEnabled_ = ShowModalWindow(owner_, hwnd_);
    return ownerWasEnabled_;
}

void ThemedWindowUi::RestoreModalOwner() {
    ::RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
}

bool ThemedWindowUi::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) {
    if (ThemedUi::HandleParentMessage(theme_, message, wParam, lParam, result)) {
        return true;
    }

    switch (message) {
    case WM_CTLCOLOREDIT: {
        result = reinterpret_cast<LRESULT>(ApplyEditColors(
            reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam)));
        return true;
    }
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        const COLORREF text = ToColorRef(theme_.color(L"comboBox", L"normal", L"text"));
        const COLORREF background = ToColorRef(theme_.color(L"comboBox", L"normal", L"itemBg"));
        SetTextColor(dc, text);
        SetBkColor(dc, background);
        result = reinterpret_cast<LRESULT>(BrushForColor(background));
        return true;
    }
    case WM_COMMAND:
        if (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS || HIWORD(wParam) == EN_CHANGE) {
            InvalidateEditFrame(reinterpret_cast<HWND>(lParam));
        }
        break;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND child = reinterpret_cast<HWND>(lParam);
        if (FindEditFrame(child)) {
            result = reinterpret_cast<LRESULT>(ApplyEditColors(dc, child));
            return true;
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ToColorRef(theme_.color(L"label", L"normal", L"text")));
        SetBkColor(dc, ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
        result = reinterpret_cast<LRESULT>(BackgroundBrush());
        return true;
    }
    case WM_PARENTNOTIFY:
        if (LOWORD(wParam) == WM_DESTROY) {
            HWND child = reinterpret_cast<HWND>(lParam);
            UnregisterEditFrame(child);
            UnregisterTableFrame(child);
        }
        break;
    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        FillRect(reinterpret_cast<HDC>(wParam), &rect, BackgroundBrush());
        result = 1;
        return true;
    }
    case WM_DESTROY:
        RestoreModalOwner();
        ReleaseResources();
        result = 0;
        return true;
    default:
        break;
    }
    return false;
}

void ThemedWindowUi::RegisterEditFrame(HWND child, RECT frame, const ThemedEditOptions& options) {
    if (!child) {
        return;
    }
    if (EditFrame* editFrame = FindEditFrame(child)) {
        editFrame->frame = frame;
        editFrame->options = options;
    } else {
        editFrames_.push_back(EditFrame{child, frame, options});
    }
}

void ThemedWindowUi::RegisterTableFrame(HWND child, RECT frame) {
    if (!child) return;
    auto it = std::find_if(tableFrames_.begin(), tableFrames_.end(), [child](const TableFrame& entry) {
        return entry.child == child;
    });
    if (it == tableFrames_.end()) tableFrames_.push_back(TableFrame{child, frame});
    else it->frame = frame;
}

void ThemedWindowUi::UnregisterTableFrame(HWND child) {
    tableFrames_.erase(
        std::remove_if(tableFrames_.begin(), tableFrames_.end(), [child](const TableFrame& entry) {
            return entry.child == child;
        }),
        tableFrames_.end());
}

void ThemedWindowUi::UnregisterEditFrame(HWND child) {
    editFrames_.erase(
        std::remove_if(editFrames_.begin(), editFrames_.end(), [child](const EditFrame& editFrame) {
            return editFrame.child == child;
        }),
        editFrames_.end());
}

void ThemedWindowUi::MoveEditFrame(HWND child, RECT frame) {
    EditFrame* editFrame = FindEditFrame(child);
    if (!editFrame) {
        return;
    }
    const RECT oldFrame = editFrame->frame;
    editFrame->frame = frame;
    const RECT childRect = editFrame->options.mode == ThemedEditMode::MultiLine
        ? ThemedControls::MultiLineEditRect(theme_, frame)
        : ThemedControls::SingleLineEditRectForFrame(theme_, frame);
    SetWindowPos(
        child,
        nullptr,
        childRect.left,
        childRect.top,
        childRect.right - childRect.left,
        childRect.bottom - childRect.top,
        SWP_NOACTIVATE | SWP_NOZORDER);
    InvalidateRect(hwnd_, &oldFrame, TRUE);
    InvalidateRect(hwnd_, &frame, TRUE);
}

void ThemedWindowUi::SetEditFrameState(HWND child, bool readOnly, bool error) {
    SetEditReadOnly(child, readOnly);
    SetEditError(child, error);
}

void ThemedWindowUi::SetEditReadOnly(HWND child, bool readOnly) {
    EditFrame* editFrame = FindEditFrame(child);
    if (!editFrame) {
        return;
    }
    editFrame->options.readOnly = readOnly;
    SendMessageW(child, EM_SETREADONLY, readOnly ? TRUE : FALSE, 0);
    InvalidateEditFrame(child);
    InvalidateRect(child, nullptr, TRUE);
}

void ThemedWindowUi::SetEditError(HWND child, bool error) {
    EditFrame* editFrame = FindEditFrame(child);
    if (!editFrame) {
        return;
    }
    editFrame->options.error = error;
    InvalidateEditFrame(child);
    InvalidateRect(child, nullptr, TRUE);
}

void ThemedWindowUi::SetEditEnabled(HWND child, bool enabled) {
    EditFrame* editFrame = FindEditFrame(child);
    if (!editFrame) {
        return;
    }
    editFrame->options.enabled = enabled;
    EnableWindow(child, enabled ? TRUE : FALSE);
    InvalidateEditFrame(child);
}

void ThemedWindowUi::SetEditPlaceholder(HWND child, const std::wstring& placeholder) {
    EditFrame* editFrame = FindEditFrame(child);
    if (!editFrame) {
        return;
    }
    editFrame->options.placeholder = placeholder;
    SendMessageW(child, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(placeholder.c_str()));
}

void ThemedWindowUi::DrawRegisteredEditFrames(HDC dc) const {
    for (const auto& editFrame : editFrames_) {
        if (IsWindow(editFrame.child)) {
            ThemedControls::DrawEditFrame(
                theme_, dc, editFrame.frame, editFrame.child, editFrame.options.readOnly, editFrame.options.error);
        }
    }
}

void ThemedWindowUi::DrawRegisteredTableFrames(HDC dc) const {
    for (const auto& entry : tableFrames_) {
        if (IsWindow(entry.child)) {
            ThemedControls::DrawListFrame(theme_, dc, entry.frame, entry.child);
        }
    }
}

bool ThemedWindowUi::EnsureTooltipWindow() {
    if (tooltip_) return true;
    static constexpr const wchar_t* kClassName = L"QuattroThemedTooltip";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ThemedWindowUi::TooltipProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    tooltip_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kClassName, L"", WS_POPUP, 0, 0, 0, 0, hwnd_, nullptr, instance_, this);
    return tooltip_ != nullptr;
}

SIZE ThemedWindowUi::MeasureTooltip(const std::wstring& text, const ThemedTooltipOptions& options) const {
    HDC dc = GetDC(nullptr);
    HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(dc, font()));
    const int maxWidth = options.maxWidth > 0
        ? options.maxWidth
        : static_cast<int>(theme_.metric(L"tooltip", L"maxWidth", 420.0f));
    RECT rect{0, 0, std::max(80, maxWidth), 0};
    UINT format = DT_CALCRECT | DT_NOPREFIX;
    format |= options.multiline ? DT_WORDBREAK : DT_SINGLELINE;
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format);
    SelectObject(dc, oldFont);
    ReleaseDC(nullptr, dc);
    const int paddingX = static_cast<int>(theme_.metric(L"tooltip", L"paddingX", 8.0f));
    const int paddingY = static_cast<int>(theme_.metric(L"tooltip", L"paddingY", 7.0f));
    return SIZE{
        std::max(80, static_cast<int>(rect.right - rect.left) + paddingX * 2),
        std::max(24, static_cast<int>(rect.bottom - rect.top) + paddingY * 2)};
}

void ThemedWindowUi::ShowTooltip(
    const std::wstring& text,
    POINT screenPoint,
    const ThemedTooltipOptions& options) {
    if (!options.enabled || text.empty() || !EnsureTooltipWindow()) {
        HideTooltip();
        return;
    }
    tooltipText_ = text;
    tooltipOptions_ = options;
    const SIZE size = MeasureTooltip(text, options);
    const int offsetX = static_cast<int>(theme_.metric(L"tooltip", L"cursorOffsetX", 14.0f));
    const int offsetY = static_cast<int>(theme_.metric(L"tooltip", L"cursorOffsetY", 18.0f));
    int x = screenPoint.x + offsetX;
    int y = screenPoint.y + offsetY;
    if (options.placement == ThemedTooltipPlacement::Above) y = screenPoint.y - size.cy - offsetY;
    else if (options.placement == ThemedTooltipPlacement::Below) y = screenPoint.y + offsetY;

    HMONITOR monitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        if (x + size.cx > info.rcWork.right) x = screenPoint.x - size.cx - offsetX;
        if (y + size.cy > info.rcWork.bottom) y = screenPoint.y - size.cy - offsetY;
        x = std::max(static_cast<int>(info.rcWork.left), x);
        y = std::max(static_cast<int>(info.rcWork.top), y);
    }
    SetWindowTextW(tooltip_, tooltipText_.c_str());
    SetWindowPos(tooltip_, HWND_TOPMOST, x, y, size.cx, size.cy, SWP_NOACTIVATE);
    const int radius = static_cast<int>(theme_.metric(L"tooltip", L"radius", 6.0f));
    HRGN region = CreateRoundRectRgn(0, 0, size.cx + 1, size.cy + 1, radius * 2, radius * 2);
    if (!region || SetWindowRgn(tooltip_, region, TRUE) == 0) {
        if (region) DeleteObject(region);
    }
    ShowWindow(tooltip_, SW_SHOWNA);
    InvalidateRect(tooltip_, nullptr, TRUE);
}

void ThemedWindowUi::HideTooltip() {
    if (tooltip_) ShowWindow(tooltip_, SW_HIDE);
    tooltipText_.clear();
}

void ThemedWindowUi::PaintTooltip(HDC dc) const {
    RECT rect{};
    GetClientRect(tooltip_, &rect);
    const wchar_t* state = TooltipState(tooltipOptions_.role);
    HBRUSH brush = CreateSolidBrush(ToColorRef(theme_.color(L"tooltip", state, L"bg")));
    HPEN pen = CreatePen(
        PS_SOLID,
        std::max(1, static_cast<int>(theme_.metric(L"tooltip", L"borderWidth", 1.0f))),
        ToColorRef(theme_.color(L"tooltip", state, L"border")));
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    const int radius = static_cast<int>(theme_.metric(L"tooltip", L"radius", 6.0f));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);

    const int paddingX = static_cast<int>(theme_.metric(L"tooltip", L"paddingX", 8.0f));
    const int paddingY = static_cast<int>(theme_.metric(L"tooltip", L"paddingY", 7.0f));
    InflateRect(&rect, -paddingX, -paddingY);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ToColorRef(theme_.color(L"tooltip", state, L"text")));
    HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(dc, font()));
    UINT format = DT_NOPREFIX | (tooltipOptions_.multiline ? DT_WORDBREAK : DT_SINGLELINE);
    DrawTextW(dc, tooltipText_.c_str(), static_cast<int>(tooltipText_.size()), &rect, format);
    SelectObject(dc, oldFont);
}

LRESULT CALLBACK ThemedWindowUi::TooltipProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    ThemedWindowUi* ui = reinterpret_cast<ThemedWindowUi*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ui = static_cast<ThemedWindowUi*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
    }
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (ui) {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            ui->PaintTooltip(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;
    case WM_NCDESTROY:
        if (ui && ui->tooltip_ == hwnd) ui->tooltip_ = nullptr;
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void ThemedWindowUi::FillBackground(HDC dc) const {
    if (!dc) {
        return;
    }
    RECT rect{};
    GetClientRect(hwnd_, &rect);
    FillRect(dc, &rect, const_cast<ThemedWindowUi*>(this)->BackgroundBrush());
}

ThemedWindowUi::EditFrame* ThemedWindowUi::FindEditFrame(HWND child) {
    auto it = std::find_if(editFrames_.begin(), editFrames_.end(), [child](const EditFrame& editFrame) {
        return editFrame.child == child;
    });
    return it == editFrames_.end() ? nullptr : &*it;
}

const ThemedWindowUi::EditFrame* ThemedWindowUi::FindEditFrame(HWND child) const {
    auto it = std::find_if(editFrames_.begin(), editFrames_.end(), [child](const EditFrame& editFrame) {
        return editFrame.child == child;
    });
    return it == editFrames_.end() ? nullptr : &*it;
}

const wchar_t* ThemedWindowUi::EditState(const EditFrame& editFrame) const {
    if (!IsWindowEnabled(editFrame.child)) {
        return L"disabled";
    }
    if (editFrame.options.error) {
        return L"error";
    }
    if (GetFocus() == editFrame.child) {
        return L"focused";
    }
    if (ThemedControls::IsControlHovered(editFrame.child)) {
        return L"hover";
    }
    return editFrame.options.readOnly ? L"readonly" : L"normal";
}

void ThemedWindowUi::InvalidateEditFrame(HWND child) const {
    if (!child) {
        return;
    }
    for (const auto& editFrame : editFrames_) {
        if (editFrame.child == child) {
            InvalidateRect(hwnd_, &editFrame.frame, TRUE);
            return;
        }
    }
}

COLORREF ThemedWindowUi::ToColorRef(Color color) {
    const auto byte = [](float value) -> BYTE {
        return static_cast<BYTE>(std::max(0.0f, std::min(1.0f, value)) * 255.0f + 0.5f);
    };
    return RGB(byte(color.r), byte(color.g), byte(color.b));
}

HBRUSH ThemedWindowUi::BackgroundBrush() {
    if (!backgroundBrush_) {
        backgroundBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
    }
    return backgroundBrush_;
}

HBRUSH ThemedWindowUi::BrushForColor(COLORREF color) {
    auto it = colorBrushes_.find(color);
    if (it != colorBrushes_.end()) {
        return it->second;
    }
    HBRUSH brush = CreateSolidBrush(color);
    colorBrushes_.emplace(color, brush);
    return brush;
}

HBRUSH ThemedWindowUi::ApplyEditColors(HDC dc, HWND child) {
    const EditFrame* editFrame = FindEditFrame(child);
    const wchar_t* state = editFrame ? EditState(*editFrame) : L"normal";
    const COLORREF text = ToColorRef(theme_.color(L"edit", state, L"text"));
    const COLORREF background = ToColorRef(theme_.color(L"edit", state, L"bg"));
    SetBkMode(dc, OPAQUE);
    SetTextColor(dc, text);
    SetBkColor(dc, background);
    return BrushForColor(background);
}

void ThemedWindowUi::ReleaseResources() {
    if (tooltip_) {
        DestroyWindow(tooltip_);
        tooltip_ = nullptr;
    }
    if (font_ && ownsFont_) {
        DeleteObject(font_);
    }
    font_ = nullptr;
    ownsFont_ = false;
    if (backgroundBrush_) {
        DeleteObject(backgroundBrush_);
        backgroundBrush_ = nullptr;
    }
    for (const auto& [color, brush] : colorBrushes_) {
        (void)color;
        DeleteObject(brush);
    }
    colorBrushes_.clear();
}
