#include "ThemedWindowUi.h"

#include "ThemedControls.h"
#include "Utilities.h"

#include <algorithm>

namespace {
std::wstring LastErrorText(const wchar_t* prefix) {
    return std::wstring(prefix) + L": " + FormatLastError(GetLastError());
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
    return ThemedUi(instance_, hwnd_, theme_, font(), layoutKind_, clientWidth_, clientHeight_);
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
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ToColorRef(theme_.color(L"label", L"normal", L"text")));
        SetBkColor(dc, ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
        result = reinterpret_cast<LRESULT>(BackgroundBrush());
        return true;
    }
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

void ThemedWindowUi::ReleaseResources() {
    if (font_ && ownsFont_) {
        DeleteObject(font_);
    }
    font_ = nullptr;
    ownsFont_ = false;
    if (backgroundBrush_) {
        DeleteObject(backgroundBrush_);
        backgroundBrush_ = nullptr;
    }
}
