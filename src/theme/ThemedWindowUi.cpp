#include "ThemedWindowUi.h"
#include "ThemedD2D.h"
#include "ThemedGdiFallback.h"

#include "ThemedControls.h"
#include "Utilities.h"

#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <cstring>
#include <utility>

namespace {
constexpr const wchar_t* kEditFrameClassName = L"QuattroThemedEditFrame";
constexpr const wchar_t* kTableFrameClassName = L"QuattroThemedTableFrame";
constexpr UINT_PTR kEditChildSubclassId = 0x51454652;
constexpr UINT_PTR kTableChildSubclassId = 0x51544652;
constexpr UINT_PTR kToastTimerId = 0x544f4153;

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

const wchar_t* ToastState(ThemedToastRole role) {
    switch (role) {
    case ThemedToastRole::Info: return L"info";
    case ThemedToastRole::Success: return L"success";
    case ThemedToastRole::Warning: return L"warning";
    case ThemedToastRole::Danger: return L"danger";
    case ThemedToastRole::Normal:
    default: return L"normal";
    }
}

bool SameTooltipOptions(const ThemedTooltipOptions& left, const ThemedTooltipOptions& right) {
    return left.placement == right.placement &&
           left.role == right.role &&
           left.multiline == right.multiline &&
           left.enabled == right.enabled &&
           left.maxWidth == right.maxWidth;
}

bool SameToastOptions(const ThemedToastOptions& left, const ThemedToastOptions& right) {
    return left.anchor == right.anchor &&
           left.role == right.role &&
           left.multiline == right.multiline &&
           left.enabled == right.enabled &&
           left.durationMs == right.durationMs &&
           left.maxWidth == right.maxWidth;
}

int RectIntersectionArea(const RECT& left, const RECT& right) {
    const LONG x1 = std::max(left.left, right.left);
    const LONG y1 = std::max(left.top, right.top);
    const LONG x2 = std::min(left.right, right.right);
    const LONG y2 = std::min(left.bottom, right.bottom);
    if (x2 <= x1 || y2 <= y1) {
        return 0;
    }
    return static_cast<int>((x2 - x1) * (y2 - y1));
}

bool PresentLayeredWindow(HWND hwnd, const std::function<void(HDC)>& paint) {
    if (!hwnd || !paint) return false;
    RECT windowRect{};
    if (!GetWindowRect(hwnd, &windowRect)) return false;
    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    if (width <= 0 || height <= 0) return false;

    HDC screen = GetDC(nullptr);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = memory
        ? CreateDIBSection(memory, &info, DIB_RGB_COLORS, &pixels, nullptr, 0)
        : nullptr;
    if (!screen || !memory || !bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (screen) ReleaseDC(nullptr, screen);
        return false;
    }
    std::memset(pixels, 0, static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
    paint(memory);

    POINT destination{windowRect.left, windowRect.top};
    POINT source{};
    SIZE size{width, height};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    const BOOL updated = UpdateLayeredWindow(
        hwnd, screen, &destination, &size, memory, &source, 0, &blend, ULW_ALPHA);

    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return updated != FALSE;
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
      clientHeight_(clientHeight) {
    dpi_ = hwnd_ ? GetDpiForWindow(hwnd_) : USER_DEFAULT_SCREEN_DPI;
    if (!dpi_) dpi_ = USER_DEFAULT_SCREEN_DPI;
    if (hwnd_) {
        RECT client{};
        if (GetClientRect(hwnd_, &client)) {
            clientWidth_ = client.right - client.left;
            clientHeight_ = client.bottom - client.top;
        }
    }
}

ThemedWindowUi::~ThemedWindowUi() {
    RestoreModalOwner();
    ReleaseResources();
}

SIZE ThemedWindowUi::AdjustedWindowSize(int clientWidth, int clientHeight, DWORD style, DWORD exStyle, bool hasMenu, UINT dpi) {
    RECT rect{0, 0, clientWidth, clientHeight};
    AdjustWindowRectExForDpi(&rect, style, hasMenu ? TRUE : FALSE, exStyle, dpi ? dpi : USER_DEFAULT_SCREEN_DPI);
    return SIZE{rect.right - rect.left, rect.bottom - rect.top};
}

UINT ThemedWindowUi::TargetDpi(const ThemedWindowCreateOptions& options) {
    UINT dpi = options.owner ? GetDpiForWindow(options.owner) : GetDpiForSystem();
    return dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
}

DWORD ThemedWindowUi::EffectiveExStyle(const ThemedWindowCreateOptions& options, bool backgroundMode) {
    DWORD exStyle = options.exStyle;
    if (options.topMost && !backgroundMode) {
        exStyle |= WS_EX_TOPMOST;
    }
    if (backgroundMode) {
        exStyle &= ~WS_EX_TOPMOST;
        exStyle |= WS_EX_NOACTIVATE;
    }
    return exStyle;
}

int ThemedWindowUi::ScaleForDpi(int logicalPixels, UINT dpi, UINT logicalDpi) {
    return MulDiv(logicalPixels, static_cast<int>(dpi ? dpi : USER_DEFAULT_SCREEN_DPI),
        static_cast<int>(logicalDpi ? logicalDpi : USER_DEFAULT_SCREEN_DPI));
}

POINT ThemedWindowUi::WindowPosition(const ThemedWindowCreateOptions& options, int windowWidth, int windowHeight) {
    const UINT dpi = TargetDpi(options);
    const int offsetX = options.scaleForDpi ? ScaleForDpi(options.offsetX, dpi, options.logicalDpi) : options.offsetX;
    const int offsetY = options.scaleForDpi ? ScaleForDpi(options.offsetY, dpi, options.logicalDpi) : options.offsetY;
    switch (options.placement) {
    case ThemedWindowPlacement::OffsetOwner:
        return OffsetWindowFromOwnerOnMonitor(options.owner, windowWidth, windowHeight, offsetX, offsetY);
    case ThemedWindowPlacement::Manual:
        return ClampWindowToOwnerMonitor(options.owner, options.x, options.y, windowWidth, windowHeight);
    case ThemedWindowPlacement::CenterOwner:
    default:
        return CenterWindowOnOwnerMonitor(options.owner, windowWidth, windowHeight);
    }
}

std::optional<POINT> ThemedWindowUi::RestoredWindowPosition(int x, int y, int windowWidth, int windowHeight) {
    if (windowWidth <= 0 || windowHeight <= 0) {
        return std::nullopt;
    }

    const RECT proposed{x, y, x + windowWidth, y + windowHeight};
    HMONITOR monitor = MonitorFromRect(&proposed, MONITOR_DEFAULTTONULL);
    if (!monitor) {
        return std::nullopt;
    }

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info) || RectIntersectionArea(proposed, info.rcMonitor) <= 0) {
        return std::nullopt;
    }

    POINT restored{x, y};
    const RECT work = info.rcWork;
    const int workWidth = work.right - work.left;
    const int workHeight = work.bottom - work.top;
    if (windowWidth >= workWidth) {
        restored.x = work.left;
    } else {
        restored.x = std::max<int>(work.left, std::min<int>(restored.x, work.right - windowWidth));
    }
    if (windowHeight >= workHeight) {
        restored.y = work.top;
    } else {
        restored.y = std::max<int>(work.top, std::min<int>(restored.y, work.bottom - windowHeight));
    }
    return restored;
}

ThemedWindowCreateOptions ThemedWindowUi::DialogOptions(
    HINSTANCE instance,
    HWND owner,
    const wchar_t* className,
    const wchar_t* title,
    WNDPROC wndProc,
    void* createParam,
    HICON icon,
    HICON smallIcon,
    ThemedWindowSizePreset sizePreset) {
    ThemedWindowCreateOptions options{};
    options.instance = instance;
    options.owner = owner;
    options.className = className;
    options.title = title;
    options.wndProc = wndProc;
    options.createParam = createParam;
    if (sizePreset == ThemedWindowSizePreset::CompactTool) {
        options.clientWidth = kThemedCompactToolClientWidth;
        options.clientHeight = kThemedCompactToolClientHeight;
    } else {
        options.clientWidth = kThemedDialogClientWidth;
        options.clientHeight = kThemedDialogClientHeight;
    }
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

    const UINT dpi = TargetDpi(options);
    const int clientWidth = options.scaleForDpi ? ScaleForDpi(options.clientWidth, dpi, options.logicalDpi) : options.clientWidth;
    const int clientHeight = options.scaleForDpi ? ScaleForDpi(options.clientHeight, dpi, options.logicalDpi) : options.clientHeight;
    const DWORD exStyle = EffectiveExStyle(options, BackgroundAcceptanceMode());
    const SIZE windowSize = AdjustedWindowSize(clientWidth, clientHeight, options.style, exStyle, false, dpi);
    const POINT position = WindowPosition(options, windowSize.cx, windowSize.cy);
    HWND hwnd = CreateWindowExW(
        exStyle,
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
        font_ = ThemedControls::CreateDialogFont(dpi_);
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
        const_cast<ThemedWindowUi*>(this), const_cast<ThemedWindowUi*>(this), dpi_);
}

bool ThemedWindowUi::ShowModal() {
    ownerWasEnabled_ = ShowModalWindow(owner_, hwnd_);
    return ownerWasEnabled_;
}

void ThemedWindowUi::ShowModeless(bool activate) {
    if (!hwnd_) {
        return;
    }
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    ShowWindowRespectFocusPolicy(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOWNORMAL);
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    if (activate && !BackgroundAcceptanceMode()) {
        ActivateWindow(hwnd_);
    }
}

void ThemedWindowUi::ResizeClientArea(int clientWidth, int clientHeight, bool keepCenter) {
    if (!hwnd_ || clientWidth <= 0 || clientHeight <= 0) {
        return;
    }
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
    const SIZE windowSize = AdjustedWindowSize(clientWidth, clientHeight, style, exStyle, false, dpi_);
    RECT windowRect{};
    GetWindowRect(hwnd_, &windowRect);
    int x = windowRect.left;
    int y = windowRect.top;
    if (keepCenter) {
        x += ((windowRect.right - windowRect.left) - windowSize.cx) / 2;
        y += ((windowRect.bottom - windowRect.top) - windowSize.cy) / 2;
    }
    const POINT clamped = ClampWindowToOwnerMonitor(owner_, x, y, windowSize.cx, windowSize.cy);
    SetWindowPos(
        hwnd_,
        nullptr,
        clamped.x,
        clamped.y,
        windowSize.cx,
        windowSize.cy,
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    RECT client{};
    if (GetClientRect(hwnd_, &client)) {
        clientWidth_ = client.right - client.left;
        clientHeight_ = client.bottom - client.top;
    }
}

void ThemedWindowUi::RestoreModalOwner() {
    ::RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
}

bool ThemedWindowUi::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) {
    if (message == WM_SHOWWINDOW && wParam) {
        ApplyWindowBackgroundPolicy(hwnd_);
    }
    if (ThemedUi::HandleParentMessage(theme_, message, wParam, lParam, result)) {
        return true;
    }

    switch (message) {
    case WM_DPICHANGED:
        ApplyDpiChange(HIWORD(wParam), reinterpret_cast<const RECT*>(lParam));
        result = 0;
        return true;
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
    case WM_TIMER:
        if (wParam == kToastTimerId) {
            HideToast();
            result = 0;
            return true;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND child = reinterpret_cast<HWND>(lParam);
        if (FindEditFrame(child)) {
            result = reinterpret_cast<LRESULT>(ApplyEditColors(dc, child));
            return true;
        }
        const wchar_t* backgroundComponent = ThemedControls::ControlBackgroundComponent(child);
        const COLORREF background = ToColorRef(theme_.color(backgroundComponent, L"normal", L"bg"));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ToColorRef(theme_.color(L"label", L"normal", L"text")));
        SetBkColor(dc, background);
        result = reinterpret_cast<LRESULT>(BrushForColor(background));
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
        HDC dc = reinterpret_cast<HDC>(wParam);
        ThemedD2D::ScopedHdcPaint d2dPaint(hwnd_, dc);
        const COLORREF background = ToColorRef(theme_.color(L"dialog", L"normal", L"bg"));
        if (!ThemedD2D::FillRect(dc, rect, background)) {
            ThemedGdiFallback::FillSolidRect(dc, rect, background);
        }
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

void ThemedWindowUi::ApplyDpiChange(UINT newDpi, const RECT* suggestedWindowRect) {
    if (!newDpi || newDpi == dpi_) return;
    const UINT oldDpi = dpi_;
    dpi_ = newDpi;

    if (ownsFont_ && font_) {
        DeleteObject(font_);
        font_ = nullptr;
        ownsFont_ = false;
    }
    font();

    if (suggestedWindowRect) {
        const int suggestedWidth = suggestedWindowRect->right - suggestedWindowRect->left;
        const int suggestedHeight = suggestedWindowRect->bottom - suggestedWindowRect->top;
        const POINT clamped = ClampWindowToOwnerMonitor(
            owner_,
            suggestedWindowRect->left,
            suggestedWindowRect->top,
            suggestedWidth,
            suggestedHeight);
        SetWindowPos(hwnd_, nullptr,
            clamped.x, clamped.y,
            suggestedWidth,
            suggestedHeight,
            SWP_NOACTIVATE | SWP_NOZORDER);
    }

    struct DpiChangeContext {
        UINT oldDpi;
        UINT newDpi;
        HFONT font;
    };
    const DpiChangeContext dpiChange{oldDpi, newDpi, font_};
    EnumChildWindows(hwnd_, [](HWND child, LPARAM value) -> BOOL {
        const auto* dpi = reinterpret_cast<const DpiChangeContext*>(value);
        RECT rect{};
        GetWindowRect(child, &rect);
        MapWindowPoints(HWND_DESKTOP, GetParent(child), reinterpret_cast<POINT*>(&rect), 2);
        SetWindowPos(child, nullptr,
            MulDiv(rect.left, dpi->newDpi, dpi->oldDpi),
            MulDiv(rect.top, dpi->newDpi, dpi->oldDpi),
            MulDiv(rect.right - rect.left, dpi->newDpi, dpi->oldDpi),
            MulDiv(rect.bottom - rect.top, dpi->newDpi, dpi->oldDpi),
            SWP_NOACTIVATE | SWP_NOZORDER);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(dpi->font), FALSE);
        ThemedControls::SetControlDpi(child, dpi->newDpi);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&dpiChange));

    for (auto& frame : editFrames_) {
        frame.frame = RECT{
            MulDiv(frame.frame.left, newDpi, oldDpi), MulDiv(frame.frame.top, newDpi, oldDpi),
            MulDiv(frame.frame.right, newDpi, oldDpi), MulDiv(frame.frame.bottom, newDpi, oldDpi)};
        SyncEditFrameWindow(frame);
    }
    for (auto& frame : tableFrames_) {
        frame.frame = RECT{
            MulDiv(frame.frame.left, newDpi, oldDpi), MulDiv(frame.frame.top, newDpi, oldDpi),
            MulDiv(frame.frame.right, newDpi, oldDpi), MulDiv(frame.frame.bottom, newDpi, oldDpi)};
        SyncTableFrameWindow(frame);
        ThemedControls::RefreshTableDpiResources(frame.child, newDpi);
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    clientWidth_ = client.right - client.left;
    clientHeight_ = client.bottom - client.top;
    if (toast_ && IsWindowVisible(toast_)) {
        toastLayoutValid_ = false;
        PositionToast();
        if ((GetWindowLongPtrW(toast_, GWL_EXSTYLE) & WS_EX_LAYERED) != 0) {
            PresentLayeredWindow(toast_, [this](HDC memory) { PaintToast(memory); });
        } else {
            InvalidateRect(toast_, nullptr, FALSE);
        }
    }
    if (tooltip_ && IsWindowVisible(tooltip_)) {
        tooltipSize_ = MeasureTooltip(tooltipText_, tooltipOptions_);
        tooltipLayoutValid_ = true;
        SetWindowPos(
            tooltip_, nullptr, tooltipPosition_.x, tooltipPosition_.y, tooltipSize_.cx, tooltipSize_.cy,
            SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        if ((GetWindowLongPtrW(tooltip_, GWL_EXSTYLE) & WS_EX_LAYERED) != 0) {
            PresentLayeredWindow(tooltip_, [this](HDC memory) { PaintTooltip(memory); });
        } else {
            InvalidateRect(tooltip_, nullptr, FALSE);
        }
    }
    if (dpiChangedCallback_) {
        dpiChangedCallback_(newDpi);
    }
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void ThemedWindowUi::RegisterEditFrame(HWND child, RECT frame, const ThemedEditOptions& options) {
    if (!child) {
        return;
    }
    if (EditFrame* editFrame = FindEditFrame(child)) {
        editFrame->frame = frame;
        editFrame->options = options;
        SyncEditFrameWindow(*editFrame);
    } else {
        editFrames_.push_back(EditFrame{child, nullptr, frame, options});
        EditFrame& newFrame = editFrames_.back();
        if (EnsureEditFrameClass()) {
            newFrame.frameWindow = CreateWindowExW(
                WS_EX_NOPARENTNOTIFY,
                kEditFrameClassName,
                L"",
                WS_CHILD | WS_CLIPSIBLINGS,
                frame.left,
                frame.top,
                frame.right - frame.left,
                frame.bottom - frame.top,
                hwnd_,
                nullptr,
                instance_,
                this);
        }
        SetWindowSubclass(child, EditChildProc, kEditChildSubclassId, reinterpret_cast<DWORD_PTR>(this));
        SyncEditFrameWindow(newFrame);
    }
}

void ThemedWindowUi::RegisterTableFrame(HWND child, RECT frame) {
    if (!child) return;
    auto it = std::find_if(tableFrames_.begin(), tableFrames_.end(), [child](const TableFrame& entry) {
        return entry.child == child;
    });
    if (it == tableFrames_.end()) {
        tableFrames_.push_back(TableFrame{child, nullptr, frame});
        TableFrame& newFrame = tableFrames_.back();
        if (EnsureTableFrameClass()) {
            newFrame.frameWindow = CreateWindowExW(
                WS_EX_NOPARENTNOTIFY,
                kTableFrameClassName,
                L"",
                WS_CHILD | WS_CLIPSIBLINGS,
                frame.left,
                frame.top,
                frame.right - frame.left,
                frame.bottom - frame.top,
                hwnd_,
                nullptr,
                instance_,
                this);
        }
        SetWindowSubclass(child, TableChildProc, kTableChildSubclassId, reinterpret_cast<DWORD_PTR>(this));
        SyncTableFrameWindow(newFrame);
    } else {
        it->frame = frame;
        SyncTableFrameWindow(*it);
    }
}

void ThemedWindowUi::UnregisterTableFrame(HWND child) {
    TableFrame* tableFrame = FindTableFrame(child);
    if (tableFrame) {
        RemoveWindowSubclass(child, TableChildProc, kTableChildSubclassId);
        if (tableFrame->frameWindow && IsWindow(tableFrame->frameWindow)) {
            DestroyWindow(tableFrame->frameWindow);
        }
    }
    tableFrames_.erase(
        std::remove_if(tableFrames_.begin(), tableFrames_.end(), [child](const TableFrame& entry) {
            return entry.child == child;
        }),
        tableFrames_.end());
}

void ThemedWindowUi::UnregisterEditFrame(HWND child) {
    EditFrame* editFrame = FindEditFrame(child);
    if (editFrame) {
        RemoveWindowSubclass(child, EditChildProc, kEditChildSubclassId);
        if (editFrame->frameWindow && IsWindow(editFrame->frameWindow)) {
            DestroyWindow(editFrame->frameWindow);
        }
    }
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
        ? ThemedControls::MultiLineEditRect(theme_, frame, dpi_)
        : ThemedControls::SingleLineEditRectForFrame(theme_, frame, dpi_);
    SetWindowPos(
        child,
        nullptr,
        childRect.left,
        childRect.top,
        childRect.right - childRect.left,
        childRect.bottom - childRect.top,
        SWP_NOACTIVATE | SWP_NOZORDER);
    SyncEditFrameWindow(*editFrame);
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
    SyncEditFrameWindow(*editFrame);
    InvalidateEditFrame(child);
}

void ThemedWindowUi::SetEditVisible(HWND child, bool visible) {
    if (!child) return;
    ShowWindow(child, visible ? SW_SHOWNA : SW_HIDE);
    if (EditFrame* editFrame = FindEditFrame(child)) {
        SyncEditFrameWindow(*editFrame);
        InvalidateRect(hwnd_, &editFrame->frame, TRUE);
    }
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
        if (editFrame.frameWindow && IsWindow(editFrame.frameWindow)) {
            InvalidateRect(editFrame.frameWindow, nullptr, FALSE);
        } else if (IsWindow(editFrame.child) && IsWindowVisible(editFrame.child)) {
            ThemedControls::DrawEditFrame(
                theme_, dc, editFrame.frame, editFrame.child, editFrame.options.readOnly, editFrame.options.error);
        }
    }
}

void ThemedWindowUi::DrawRegisteredTableFrames(HDC dc) const {
    for (const auto& entry : tableFrames_) {
        if (entry.frameWindow && IsWindow(entry.frameWindow)) {
            InvalidateRect(entry.frameWindow, nullptr, FALSE);
        } else if (IsWindow(entry.child) && IsWindowVisible(entry.child)) {
            ThemedControls::DrawTableFrame(theme_, dc, entry.frame, entry.child);
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
    const DWORD layeredStyle = ThemedD2D::IsAvailable() ? WS_EX_LAYERED : 0u;
    tooltip_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | layeredStyle,
        kClassName, L"", WS_POPUP, 0, 0, 0, 0, hwnd_, nullptr, instance_, this);
    return tooltip_ != nullptr;
}

SIZE ThemedWindowUi::MeasureTooltip(const std::wstring& text, const ThemedTooltipOptions& options) const {
    const int maxWidth = ScaleForDpi(
        options.maxWidth > 0 ? options.maxWidth : static_cast<int>(theme_.metric(L"tooltip", L"maxWidth", 420.0f)),
        dpi_);
    const SIZE measured = ThemedD2D::MeasureText(font(), text, std::max(ScaleForDpi(80, dpi_), maxWidth), options.multiline);
    const int paddingX = ScaleForDpi(static_cast<int>(theme_.metric(L"tooltip", L"paddingX", 8.0f)), dpi_);
    const int paddingY = ScaleForDpi(static_cast<int>(theme_.metric(L"tooltip", L"paddingY", 7.0f)), dpi_);
    return SIZE{
        std::max(ScaleForDpi(80, dpi_), static_cast<int>(measured.cx) + paddingX * 2),
        std::max(ScaleForDpi(24, dpi_), static_cast<int>(measured.cy) + paddingY * 2)};
}

void ThemedWindowUi::ShowTooltip(
    const std::wstring& text,
    POINT screenPoint,
    const ThemedTooltipOptions& options) {
    if (!options.enabled || text.empty() || !EnsureTooltipWindow()) {
        HideTooltip();
        return;
    }

    const bool contentChanged = tooltipText_ != text;
    const bool optionsChanged = !SameTooltipOptions(tooltipOptions_, options);
    const bool layoutChanged = !tooltipLayoutValid_ || contentChanged || optionsChanged;
    tooltipText_ = text;
    tooltipOptions_ = options;
    if (layoutChanged) {
        tooltipSize_ = MeasureTooltip(text, options);
        tooltipLayoutValid_ = true;
    }
    const SIZE size = tooltipSize_;
    const int offsetX = ScaleForDpi(static_cast<int>(theme_.metric(L"tooltip", L"cursorOffsetX", 14.0f)), dpi_);
    const int offsetY = ScaleForDpi(static_cast<int>(theme_.metric(L"tooltip", L"cursorOffsetY", 18.0f)), dpi_);
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

    const bool positionChanged = !tooltipPositionValid_
        || tooltipPosition_.x != x
        || tooltipPosition_.y != y;
    if (!layoutChanged && !positionChanged && IsWindowVisible(tooltip_)) {
        return;
    }

    if (contentChanged) {
        SetWindowTextW(tooltip_, tooltipText_.c_str());
    }
    const UINT positionFlags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER
        | (layoutChanged ? 0 : SWP_NOSIZE);
    SetWindowPos(tooltip_, nullptr, x, y, size.cx, size.cy, positionFlags);
    tooltipPosition_ = POINT{x, y};
    tooltipPositionValid_ = true;
    const bool layered = (GetWindowLongPtrW(tooltip_, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
    if (layoutChanged && !layered) {
        const int radius = ScaleForDpi(static_cast<int>(theme_.metric(L"tooltip", L"radius", 6.0f)), dpi_);
        ThemedGdiFallback::ApplyRoundedWindowRegion(tooltip_, size, radius, false);
    }
    if (!IsWindowVisible(tooltip_)) {
        ShowWindow(tooltip_, SW_SHOWNA);
    }
    if (layered) {
        PresentLayeredWindow(tooltip_, [this](HDC memory) { PaintTooltip(memory); });
    } else if (layoutChanged) {
        InvalidateRect(tooltip_, nullptr, FALSE);
    }
}

void ThemedWindowUi::HideTooltip() {
    if (tooltip_) ShowWindow(tooltip_, SW_HIDE);
    tooltipText_.clear();
    tooltipLayoutValid_ = false;
    tooltipPositionValid_ = false;
}

void ThemedWindowUi::PaintTooltip(HDC dc) const {
    RECT rect{};
    GetClientRect(tooltip_, &rect);
    const bool layered = (GetWindowLongPtrW(tooltip_, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
    ThemedD2D::ScopedHdcPaint d2dPaint(
        tooltip_, dc, layered ? ThemedD2D::SurfaceKind::Transparent : ThemedD2D::SurfaceKind::Opaque);
    const wchar_t* state = TooltipState(tooltipOptions_.role);
    const int borderWidth = std::max(1, ScaleForDpi(
        static_cast<int>(theme_.metric(L"tooltip", L"borderWidth", 1.0f)), dpi_));
    const int radius = ScaleForDpi(static_cast<int>(theme_.metric(L"tooltip", L"radius", 6.0f)), dpi_);
    if (!ThemedD2D::FillRoundedRect(
            dc,
            rect,
            static_cast<float>(radius),
            ToColorRef(theme_.color(L"tooltip", state, L"bg")),
            ToColorRef(theme_.color(L"tooltip", state, L"border")),
            static_cast<float>(borderWidth))) {
        ThemedGdiFallback::FillRoundedRect(
            dc, rect, radius,
            ToColorRef(theme_.color(L"tooltip", state, L"bg")),
            ToColorRef(theme_.color(L"tooltip", state, L"border")), borderWidth);
    }

    const int paddingX = ScaleForDpi(static_cast<int>(theme_.metric(L"tooltip", L"paddingX", 8.0f)), dpi_);
    const int paddingY = ScaleForDpi(static_cast<int>(theme_.metric(L"tooltip", L"paddingY", 7.0f)), dpi_);
    InflateRect(&rect, -paddingX, -paddingY);
    UINT format = DT_NOPREFIX | (tooltipOptions_.multiline ? DT_WORDBREAK : DT_SINGLELINE);
    if (!ThemedD2D::DrawTextLayout(
            dc, font(), tooltipText_.c_str(), static_cast<int>(tooltipText_.size()), rect, format,
            ToColorRef(theme_.color(L"tooltip", state, L"text")))) {
        ThemedGdiFallback::DrawText(
            dc, font(), tooltipText_.c_str(), static_cast<int>(tooltipText_.size()), rect, format,
            ToColorRef(theme_.color(L"tooltip", state, L"text")));
    }
}

LRESULT CALLBACK ThemedWindowUi::TooltipProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    ThemedWindowUi* ui = reinterpret_cast<ThemedWindowUi*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ui = static_cast<ThemedWindowUi*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
    }
    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
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
        if (ui && ui->tooltip_ == hwnd) {
            ui->tooltip_ = nullptr;
            ui->tooltipLayoutValid_ = false;
            ui->tooltipPositionValid_ = false;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool ThemedWindowUi::EnsureToastWindow() {
    if (toast_) return true;
    static constexpr const wchar_t* kClassName = L"QuattroThemedToast";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ThemedWindowUi::ToastProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    const DWORD layeredStyle = ThemedD2D::IsAvailable() ? WS_EX_LAYERED : 0u;
    toast_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | layeredStyle,
        kClassName, L"", WS_POPUP, 0, 0, 0, 0, hwnd_, nullptr, instance_, this);
    return toast_ != nullptr;
}

SIZE ThemedWindowUi::MeasureToast(const std::wstring& text, const ThemedToastOptions& options) const {
    const int maxWidth = ScaleForDpi(
        options.maxWidth > 0 ? options.maxWidth : static_cast<int>(theme_.metric(L"toast", L"maxWidth", 360.0f)),
        dpi_);
    const SIZE measured = ThemedD2D::MeasureText(
        font(), text, std::max(ScaleForDpi(120, dpi_), maxWidth), options.multiline);
    const int paddingX = ScaleForDpi(static_cast<int>(theme_.metric(L"toast", L"paddingX", 12.0f)), dpi_);
    const int paddingY = ScaleForDpi(static_cast<int>(theme_.metric(L"toast", L"paddingY", 9.0f)), dpi_);
    return SIZE{
        std::max(ScaleForDpi(160, dpi_), static_cast<int>(measured.cx) + paddingX * 2),
        std::max(ScaleForDpi(32, dpi_), static_cast<int>(measured.cy) + paddingY * 2)};
}

RECT ThemedWindowUi::ToastCloseButtonRect() const {
    RECT rect{};
    if (!toast_) {
        return rect;
    }
    GetClientRect(toast_, &rect);
    const int paddingX = ScaleForDpi(static_cast<int>(theme_.metric(L"toast", L"paddingX", 12.0f)), dpi_);
    const int paddingY = ScaleForDpi(static_cast<int>(theme_.metric(L"toast", L"paddingY", 9.0f)), dpi_);
    const int visualSize = std::min(
        ScaleForDpi(static_cast<int>(theme_.metric(L"miniButton", L"height", 24.0f)), dpi_),
        ScaleForDpi(16, dpi_));
    rect.left = rect.right - paddingX - visualSize;
    rect.top = rect.top + paddingY;
    rect.right = rect.left + visualSize;
    rect.bottom = rect.top + visualSize;
    return rect;
}

void ThemedWindowUi::InvalidateToastWindow() {
    if (!toast_ || !IsWindow(toast_)) {
        return;
    }
    if ((GetWindowLongPtrW(toast_, GWL_EXSTYLE) & WS_EX_LAYERED) != 0) {
        PresentLayeredWindow(toast_, [this](HDC memory) { PaintToast(memory); });
    } else {
        InvalidateRect(toast_, nullptr, FALSE);
    }
}

void ThemedWindowUi::PositionToast() {
    if (!toast_) return;
    if (!toastLayoutValid_) {
        toastSize_ = MeasureToast(toastText_, toastOptions_);
        toastLayoutValid_ = true;
    }

    RECT anchorRect{};
    bool hasAnchor = false;
    if (toastOptions_.anchor == ThemedToastAnchor::ScreenBottomRight) {
        POINT point{};
        if (hwnd_) {
            RECT ownerRect{};
            GetWindowRect(hwnd_, &ownerRect);
            point.x = ownerRect.left + (ownerRect.right - ownerRect.left) / 2;
            point.y = ownerRect.top + (ownerRect.bottom - ownerRect.top) / 2;
        }
        HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info)) {
            anchorRect = info.rcWork;
            hasAnchor = true;
        }
    } else if (hwnd_) {
        hasAnchor = GetWindowRect(hwnd_, &anchorRect) != FALSE;
    }
    if (!hasAnchor) {
        HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info)) {
            anchorRect = info.rcWork;
            hasAnchor = true;
        }
    }
    if (!hasAnchor) return;

    const int marginX = ScaleForDpi(static_cast<int>(theme_.metric(L"toast", L"marginX", 16.0f)), dpi_);
    const int marginY = ScaleForDpi(static_cast<int>(theme_.metric(L"toast", L"marginY", 16.0f)), dpi_);
    int x = anchorRect.right - toastSize_.cx - marginX;
    int y = anchorRect.bottom - toastSize_.cy - marginY;
    if (toastOptions_.anchor == ThemedToastAnchor::OwnerTopRight) {
        y = anchorRect.top + marginY;
    }

    HMONITOR monitor = MonitorFromRect(&anchorRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        x = std::max<int>(info.rcWork.left + marginX, std::min<int>(x, info.rcWork.right - toastSize_.cx - marginX));
        y = std::max<int>(info.rcWork.top + marginY, std::min<int>(y, info.rcWork.bottom - toastSize_.cy - marginY));
    }

    const bool backgroundMode = BackgroundAcceptanceMode();
    const UINT positionFlags = SWP_NOACTIVATE | (backgroundMode ? 0u : (SWP_NOZORDER | SWP_NOOWNERZORDER));
    SetWindowPos(
        toast_,
        backgroundMode ? HWND_BOTTOM : nullptr,
        x,
        y,
        toastSize_.cx,
        toastSize_.cy,
        positionFlags);
    if ((GetWindowLongPtrW(toast_, GWL_EXSTYLE) & WS_EX_LAYERED) == 0) {
        const int radius = ScaleForDpi(static_cast<int>(theme_.metric(L"toast", L"radius", 7.0f)), dpi_);
        ThemedGdiFallback::ApplyRoundedWindowRegion(toast_, toastSize_, radius, false);
    }
}

void ThemedWindowUi::ShowToast(const std::wstring& text, const ThemedToastOptions& options) {
    if (!options.enabled || text.empty() || !EnsureToastWindow()) {
        HideToast();
        return;
    }
    const bool contentChanged = toastText_ != text;
    const bool optionsChanged = !SameToastOptions(toastOptions_, options);
    toastText_ = text;
    toastOptions_ = options;
    if (contentChanged || optionsChanged) {
        toastLayoutValid_ = false;
        SetWindowTextW(toast_, toastText_.c_str());
    }
    toastCloseHovered_ = false;
    toastClosePressed_ = false;
    PositionToast();
    if (!IsWindowVisible(toast_)) {
        ShowWindow(toast_, SW_SHOWNA);
    }
    InvalidateToastWindow();
    KillTimer(hwnd_, kToastTimerId);
    if (options.durationMs > 0) {
        SetTimer(hwnd_, kToastTimerId, static_cast<UINT>(options.durationMs), nullptr);
    }
}

void ThemedWindowUi::HideToast() {
    KillTimer(hwnd_, kToastTimerId);
    if (toast_) ShowWindow(toast_, SW_HIDE);
    toastText_.clear();
    toastLayoutValid_ = false;
    toastCloseHovered_ = false;
    toastClosePressed_ = false;
}

void ThemedWindowUi::PaintToast(HDC dc) const {
    RECT rect{};
    GetClientRect(toast_, &rect);
    const bool layered = (GetWindowLongPtrW(toast_, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
    ThemedD2D::ScopedHdcPaint d2dPaint(
        toast_, dc, layered ? ThemedD2D::SurfaceKind::Transparent : ThemedD2D::SurfaceKind::Opaque);
    const wchar_t* state = ToastState(toastOptions_.role);
    const int borderWidth = std::max(1, ScaleForDpi(
        static_cast<int>(theme_.metric(L"toast", L"borderWidth", 1.0f)), dpi_));
    const int radius = ScaleForDpi(static_cast<int>(theme_.metric(L"toast", L"radius", 7.0f)), dpi_);
    if (!ThemedD2D::FillRoundedRect(
            dc,
            rect,
            static_cast<float>(radius),
            ToColorRef(theme_.color(L"toast", state, L"bg")),
            ToColorRef(theme_.color(L"toast", state, L"border")),
            static_cast<float>(borderWidth))) {
        ThemedGdiFallback::FillRoundedRect(
            dc, rect, radius,
            ToColorRef(theme_.color(L"toast", state, L"bg")),
            ToColorRef(theme_.color(L"toast", state, L"border")), borderWidth);
    }

    const int paddingX = ScaleForDpi(static_cast<int>(theme_.metric(L"toast", L"paddingX", 12.0f)), dpi_);
    const int paddingY = ScaleForDpi(static_cast<int>(theme_.metric(L"toast", L"paddingY", 9.0f)), dpi_);
    const RECT closeRect = ToastCloseButtonRect();
    const wchar_t* closeState = toastClosePressed_ ? L"pressed" : (toastCloseHovered_ ? L"hover" : L"normal");
    ThemedControls::DrawMiniButtonFrame(theme_, dc, closeRect, toastCloseHovered_, toastClosePressed_, false, false);
    RECT closeGlyphRect = closeRect;
    const int glyphOffset = toastClosePressed_ ? ScaleForDpi(static_cast<int>(theme_.metric(L"miniButton", L"pressedOffset", 1.0f)), dpi_) : 0;
    OffsetRect(&closeGlyphRect, glyphOffset, glyphOffset);
    if (!ThemedD2D::DrawTextLayout(
            dc,
            font(),
            L"\x00D7",
            1,
            closeGlyphRect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
            ToColorRef(theme_.color(L"miniButton", closeState, L"icon")))) {
        ThemedGdiFallback::DrawText(
            dc,
            font(),
            L"\x00D7",
            1,
            closeGlyphRect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
            ToColorRef(theme_.color(L"miniButton", closeState, L"icon")));
    }
    InflateRect(&rect, -paddingX, -paddingY);
    const int closeGap = ScaleForDpi(6, dpi_);
    rect.right = std::max(rect.left, closeRect.left - closeGap);
    UINT format = DT_NOPREFIX | (toastOptions_.multiline ? DT_WORDBREAK : DT_SINGLELINE);
    if (!ThemedD2D::DrawTextLayout(
            dc, font(), toastText_.c_str(), static_cast<int>(toastText_.size()), rect, format,
            ToColorRef(theme_.color(L"toast", state, L"text")))) {
        ThemedGdiFallback::DrawText(
            dc, font(), toastText_.c_str(), static_cast<int>(toastText_.size()), rect, format,
            ToColorRef(theme_.color(L"toast", state, L"text")));
    }
}

LRESULT CALLBACK ThemedWindowUi::ToastProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto pointFromLParam = [](LPARAM value) -> POINT {
        return POINT{
            static_cast<LONG>(static_cast<short>(LOWORD(value))),
            static_cast<LONG>(static_cast<short>(HIWORD(value)))};
    };
    ThemedWindowUi* ui = reinterpret_cast<ThemedWindowUi*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ui = static_cast<ThemedWindowUi*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
    }
    switch (message) {
    case WM_NCHITTEST:
        if (ui && ui->toast_ == hwnd) {
            POINT screenPoint = pointFromLParam(lParam);
            POINT clientPoint = screenPoint;
            ScreenToClient(hwnd, &clientPoint);
            const RECT closeRect = ui->ToastCloseButtonRect();
            if (PtInRect(&closeRect, clientPoint)) {
                return HTCLIENT;
            }
        }
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
        if (ui && ui->toast_ == hwnd) {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            TrackMouseEvent(&track);
            POINT point = pointFromLParam(lParam);
            const RECT closeRect = ui->ToastCloseButtonRect();
            const bool hovered = PtInRect(&closeRect, point) != FALSE;
            if (ui->toastCloseHovered_ != hovered) {
                ui->toastCloseHovered_ = hovered;
                if (!hovered) {
                    ui->toastClosePressed_ = false;
                }
                ui->InvalidateToastWindow();
            }
            return 0;
        }
        break;
    case WM_MOUSELEAVE:
        if (ui && ui->toast_ == hwnd) {
            if (ui->toastCloseHovered_ || ui->toastClosePressed_) {
                ui->toastCloseHovered_ = false;
                ui->toastClosePressed_ = false;
                ui->InvalidateToastWindow();
            }
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        if (ui && ui->toast_ == hwnd) {
            POINT point = pointFromLParam(lParam);
            const RECT closeRect = ui->ToastCloseButtonRect();
            if (PtInRect(&closeRect, point)) {
                ui->toastClosePressed_ = true;
                ::SetCapture(hwnd);
                ui->InvalidateToastWindow();
                return 0;
            }
        }
        break;
    case WM_LBUTTONUP:
        if (ui && ui->toast_ == hwnd) {
            const bool pressed = ui->toastClosePressed_;
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            ui->toastClosePressed_ = false;
            POINT point = pointFromLParam(lParam);
            const RECT closeRect = ui->ToastCloseButtonRect();
            const bool activateClose = pressed && PtInRect(&closeRect, point) != FALSE;
            ui->toastCloseHovered_ = PtInRect(&closeRect, point) != FALSE;
            ui->InvalidateToastWindow();
            if (activateClose) {
                ui->HideToast();
            }
            return 0;
        }
        break;
    case WM_PAINT:
        if (ui) {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            ui->PaintToast(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;
    case WM_NCDESTROY:
        if (ui && ui->toast_ == hwnd) {
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            ui->toast_ = nullptr;
            ui->toastLayoutValid_ = false;
            ui->toastCloseHovered_ = false;
            ui->toastClosePressed_ = false;
        }
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
    ThemedD2D::ScopedHdcPaint d2dPaint(hwnd_, dc);
    const COLORREF background = ToColorRef(theme_.color(L"dialog", L"normal", L"bg"));
    if (!ThemedD2D::FillRect(dc, rect, background)) {
        ThemedGdiFallback::FillSolidRect(dc, rect, background);
    }
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

ThemedWindowUi::EditFrame* ThemedWindowUi::FindEditFrameWindow(HWND frameWindow) {
    auto it = std::find_if(editFrames_.begin(), editFrames_.end(), [frameWindow](const EditFrame& editFrame) {
        return editFrame.frameWindow == frameWindow;
    });
    return it == editFrames_.end() ? nullptr : &*it;
}

const ThemedWindowUi::EditFrame* ThemedWindowUi::FindEditFrameWindow(HWND frameWindow) const {
    auto it = std::find_if(editFrames_.begin(), editFrames_.end(), [frameWindow](const EditFrame& editFrame) {
        return editFrame.frameWindow == frameWindow;
    });
    return it == editFrames_.end() ? nullptr : &*it;
}

ThemedWindowUi::TableFrame* ThemedWindowUi::FindTableFrame(HWND child) {
    auto it = std::find_if(tableFrames_.begin(), tableFrames_.end(), [child](const TableFrame& tableFrame) {
        return tableFrame.child == child;
    });
    return it == tableFrames_.end() ? nullptr : &*it;
}

const ThemedWindowUi::TableFrame* ThemedWindowUi::FindTableFrame(HWND child) const {
    auto it = std::find_if(tableFrames_.begin(), tableFrames_.end(), [child](const TableFrame& tableFrame) {
        return tableFrame.child == child;
    });
    return it == tableFrames_.end() ? nullptr : &*it;
}

ThemedWindowUi::TableFrame* ThemedWindowUi::FindTableFrameWindow(HWND frameWindow) {
    auto it = std::find_if(tableFrames_.begin(), tableFrames_.end(), [frameWindow](const TableFrame& tableFrame) {
        return tableFrame.frameWindow == frameWindow;
    });
    return it == tableFrames_.end() ? nullptr : &*it;
}

const ThemedWindowUi::TableFrame* ThemedWindowUi::FindTableFrameWindow(HWND frameWindow) const {
    auto it = std::find_if(tableFrames_.begin(), tableFrames_.end(), [frameWindow](const TableFrame& tableFrame) {
        return tableFrame.frameWindow == frameWindow;
    });
    return it == tableFrames_.end() ? nullptr : &*it;
}

bool ThemedWindowUi::EnsureEditFrameClass() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ThemedWindowUi::EditFrameProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    wc.lpszClassName = kEditFrameClassName;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool ThemedWindowUi::EnsureTableFrameClass() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ThemedWindowUi::TableFrameProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kTableFrameClassName;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void ThemedWindowUi::SyncEditFrameWindow(EditFrame& editFrame) {
    if (!editFrame.frameWindow || !IsWindow(editFrame.frameWindow) || !IsWindow(editFrame.child)) {
        return;
    }
    const bool visible = (GetWindowLongPtrW(editFrame.child, GWL_STYLE) & WS_VISIBLE) != 0;
    const bool enabled = IsWindowEnabled(editFrame.child) != FALSE;
    EnableWindow(editFrame.frameWindow, enabled ? TRUE : FALSE);
    SetWindowPos(
        editFrame.frameWindow,
        editFrame.child,
        editFrame.frame.left,
        editFrame.frame.top,
        editFrame.frame.right - editFrame.frame.left,
        editFrame.frame.bottom - editFrame.frame.top,
        SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    InvalidateRect(editFrame.frameWindow, nullptr, FALSE);
}

void ThemedWindowUi::PaintEditFrameWindow(HWND frameWindow, HDC dc) const {
    const EditFrame* editFrame = FindEditFrameWindow(frameWindow);
    if (!editFrame || !IsWindow(editFrame->child)) {
        return;
    }
    RECT rect{};
    GetClientRect(frameWindow, &rect);
    const COLORREF background = ToColorRef(theme_.color(L"dialog", L"normal", L"bg"));
    {
        ThemedD2D::ScopedHdcPaint d2dPaint(frameWindow, dc);
        if (!ThemedD2D::FillRect(dc, rect, background)) {
            ThemedGdiFallback::FillSolidRect(dc, rect, background);
        }
    }
    ThemedControls::DrawEditFrame(
        theme_, dc, rect, editFrame->child, editFrame->options.readOnly, editFrame->options.error);
}

void ThemedWindowUi::SyncTableFrameWindow(TableFrame& tableFrame) {
    if (!tableFrame.frameWindow || !IsWindow(tableFrame.frameWindow) || !IsWindow(tableFrame.child)) {
        return;
    }
    const bool visible = (GetWindowLongPtrW(tableFrame.child, GWL_STYLE) & WS_VISIBLE) != 0;
    const bool enabled = IsWindowEnabled(tableFrame.child) != FALSE;
    EnableWindow(tableFrame.frameWindow, enabled ? TRUE : FALSE);
    SetWindowPos(
        tableFrame.frameWindow,
        tableFrame.child,
        tableFrame.frame.left,
        tableFrame.frame.top,
        tableFrame.frame.right - tableFrame.frame.left,
        tableFrame.frame.bottom - tableFrame.frame.top,
        SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    InvalidateRect(tableFrame.frameWindow, nullptr, FALSE);
}

void ThemedWindowUi::PaintTableFrameWindow(HWND frameWindow, HDC dc) const {
    const TableFrame* tableFrame = FindTableFrameWindow(frameWindow);
    if (!tableFrame || !IsWindow(tableFrame->child)) {
        return;
    }
    RECT rect{};
    GetClientRect(frameWindow, &rect);
    ThemedControls::DrawTableFrame(theme_, dc, rect, tableFrame->child);
}

LRESULT CALLBACK ThemedWindowUi::EditFrameProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
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
            ui->PaintEditFrameWindow(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        if (ui) {
            if (EditFrame* editFrame = ui->FindEditFrameWindow(hwnd)) {
                if (IsWindowEnabled(editFrame->child)) {
                    SetFocus(editFrame->child);
                }
            }
        }
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK ThemedWindowUi::TableFrameProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
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
            ui->PaintTableFrameWindow(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        if (ui) {
            if (TableFrame* tableFrame = ui->FindTableFrameWindow(hwnd)) {
                if (IsWindowEnabled(tableFrame->child)) {
                    SetFocus(tableFrame->child);
                }
            }
        }
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK ThemedWindowUi::EditChildProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR id,
    DWORD_PTR data) {
    auto* ui = reinterpret_cast<ThemedWindowUi*>(data);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, EditChildProc, id);
        const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        if (ui) {
            ui->UnregisterEditFrame(hwnd);
        }
        return result;
    }
    const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
    if (ui) {
        if (EditFrame* editFrame = ui->FindEditFrame(hwnd)) {
            if ((message == WM_SHOWWINDOW || message == WM_WINDOWPOSCHANGED) &&
                editFrame->frameWindow && IsWindow(editFrame->frameWindow)) {
                const bool visible = (GetWindowLongPtrW(editFrame->child, GWL_STYLE) & WS_VISIBLE) != 0;
                if (visible) {
                    EnableWindow(editFrame->frameWindow, IsWindowEnabled(editFrame->child));
                    SetWindowPos(
                        editFrame->frameWindow,
                        editFrame->child,
                        editFrame->frame.left,
                        editFrame->frame.top,
                        editFrame->frame.right - editFrame->frame.left,
                        editFrame->frame.bottom - editFrame->frame.top,
                        SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOREDRAW);
                    InvalidateRect(editFrame->frameWindow, nullptr, FALSE);
                } else {
                    SetWindowPos(
                        editFrame->frameWindow,
                        nullptr,
                        0,
                        0,
                        0,
                        0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                            SWP_HIDEWINDOW | SWP_NOREDRAW);
                }
            } else if (message == WM_ENABLE || message == WM_SETFOCUS || message == WM_KILLFOCUS ||
                       message == WM_MOUSEMOVE || message == WM_MOUSELEAVE) {
                ui->SyncEditFrameWindow(*editFrame);
            }
        }
    }
    return result;
}

LRESULT CALLBACK ThemedWindowUi::TableChildProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR id,
    DWORD_PTR data) {
    auto* ui = reinterpret_cast<ThemedWindowUi*>(data);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, TableChildProc, id);
        const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        if (ui) {
            ui->UnregisterTableFrame(hwnd);
        }
        return result;
    }
    const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
    if (ui) {
        if (TableFrame* tableFrame = ui->FindTableFrame(hwnd)) {
            if ((message == WM_SHOWWINDOW || message == WM_WINDOWPOSCHANGED) &&
                tableFrame->frameWindow && IsWindow(tableFrame->frameWindow)) {
                const bool visible = (GetWindowLongPtrW(tableFrame->child, GWL_STYLE) & WS_VISIBLE) != 0;
                if (visible) {
                    ui->SyncTableFrameWindow(*tableFrame);
                } else {
                    SetWindowPos(
                        tableFrame->frameWindow,
                        nullptr,
                        0,
                        0,
                        0,
                        0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                            SWP_HIDEWINDOW | SWP_NOREDRAW);
                }
            } else if (message == WM_ENABLE || message == WM_SETFOCUS || message == WM_KILLFOCUS ||
                       message == WM_MOUSEMOVE || message == WM_MOUSELEAVE) {
                ui->SyncTableFrameWindow(*tableFrame);
            }
        }
    }
    return result;
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
            if (editFrame.frameWindow && IsWindow(editFrame.frameWindow)) {
                InvalidateRect(editFrame.frameWindow, nullptr, FALSE);
            } else {
                InvalidateRect(hwnd_, &editFrame.frame, TRUE);
            }
            return;
        }
    }
}

void ThemedWindowUi::SetDpiChangedCallback(std::function<void(UINT)> callback) {
    dpiChangedCallback_ = std::move(callback);
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
    ThemedUi::DetachTooltips(this);
    for (auto& editFrame : editFrames_) {
        if (editFrame.child && IsWindow(editFrame.child)) {
            RemoveWindowSubclass(editFrame.child, EditChildProc, kEditChildSubclassId);
        }
        if (editFrame.frameWindow && IsWindow(editFrame.frameWindow)) {
            DestroyWindow(editFrame.frameWindow);
        }
    }
    editFrames_.clear();
    for (auto& tableFrame : tableFrames_) {
        if (tableFrame.child && IsWindow(tableFrame.child)) {
            RemoveWindowSubclass(tableFrame.child, TableChildProc, kTableChildSubclassId);
        }
        if (tableFrame.frameWindow && IsWindow(tableFrame.frameWindow)) {
            DestroyWindow(tableFrame.frameWindow);
        }
    }
    tableFrames_.clear();
    if (tooltip_) {
        DestroyWindow(tooltip_);
        tooltip_ = nullptr;
    }
    KillTimer(hwnd_, kToastTimerId);
    if (toast_) {
        DestroyWindow(toast_);
        toast_ = nullptr;
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
