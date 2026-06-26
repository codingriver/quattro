#include "BuiltinTools.h"

#include "AppLog.h"
#include "ThemedControls.h"
#include "Utilities.h"

#include <commdlg.h>
#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace {
constexpr int ID_CLICK_X = 7001;
constexpr int ID_CLICK_Y = 7002;
constexpr int ID_CLICK_COUNT = 7003;
constexpr int ID_CLICK_INTERVAL = 7004;
constexpr int ID_CLICK_BUTTON = 7005;
constexpr int ID_CLICK_PICK = 7006;
constexpr int ID_CLICK_START = 7007;
constexpr int ID_CLICK_STOP = 7008;
constexpr int ID_CLICK_STATUS = 7009;
constexpr UINT_PTR ID_CLICK_COUNTDOWN_TIMER = 7010;
constexpr UINT_PTR ID_CLICK_TIMER = 7011;
constexpr int ID_CLICK_STOP_HOTKEY = 7012;
constexpr int ID_CLICK_COUNTDOWN = 7013;
constexpr int ID_CLICK_HOTKEY = 7014;
constexpr int ID_CLICK_LIVE_POS = 7015;
constexpr UINT_PTR ID_CLICK_POSITION_TIMER = 7016;

constexpr int ID_TIMER_HOURS = 7101;
constexpr int ID_TIMER_MINUTES = 7107;
constexpr int ID_TIMER_SECONDS = 7108;
constexpr int ID_TIMER_START = 7102;
constexpr int ID_TIMER_PAUSE = 7103;
constexpr int ID_TIMER_RESET = 7104;
constexpr int ID_TIMER_STATUS = 7105;
constexpr UINT_PTR ID_TIMER_TICK = 7106;
constexpr int ID_TIMER_SOUND = 7109;
constexpr int ID_TIMER_TOPMOST = 7110;

constexpr int ID_SW_START = 7201;
constexpr int ID_SW_PAUSE = 7202;
constexpr int ID_SW_RESET = 7203;
constexpr int ID_SW_LAP = 7204;
constexpr int ID_SW_COPY = 7205;
constexpr int ID_SW_DISPLAY = 7206;
constexpr int ID_SW_LAPS = 7207;
constexpr UINT_PTR ID_SW_TICK = 7208;
constexpr int ID_SW_EXPORT = 7209;
constexpr UINT WM_QUATTRO_TOOL_AUTOMATION = WM_APP + 0x80;
constexpr UINT WM_QUATTRO_TOOL_TIMER_AUTOMATION = WM_APP + 0x81;

float ClampFloat(float value, float minValue, float maxValue) {
    return std::max(minValue, std::min(maxValue, value));
}

COLORREF ToColorRef(Color color) {
    const auto byte = [](float value) -> BYTE {
        return static_cast<BYTE>(ClampFloat(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return RGB(byte(color.r), byte(color.g), byte(color.b));
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

int ReadIntField(HWND hwnd, int fallback, int minValue, int maxValue) {
    const std::optional<int> parsed = ParseInt(Trim(GetText(hwnd)));
    return std::max(minValue, std::min(maxValue, parsed.value_or(fallback)));
}

void SetText(HWND hwnd, const std::wstring& text) {
    SetWindowTextW(hwnd, text.c_str());
}

std::vector<std::wstring> SplitLines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    std::wstring current;
    for (wchar_t ch : text) {
        if (ch == L'\r') {
            continue;
        }
        if (ch == L'\n') {
            if (!current.empty()) {
                lines.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

int HotKeyFromName(const std::wstring& value) {
    if (value == L"F6") return VK_F6;
    if (value == L"F7") return VK_F7;
    if (value == L"F9") return VK_F9;
    if (value == L"F10") return VK_F10;
    if (value == L"F11") return VK_F11;
    if (value == L"F12") return VK_F12;
    return VK_F8;
}

std::wstring HotKeyName(int key) {
    switch (key) {
    case VK_F6: return L"F6";
    case VK_F7: return L"F7";
    case VK_F9: return L"F9";
    case VK_F10: return L"F10";
    case VK_F11: return L"F11";
    case VK_F12: return L"F12";
    default: return L"F8";
    }
}

void CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) {
        return;
    }
    EmptyClipboard();
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

std::wstring FormatElapsed(ULONGLONG milliseconds) {
    const ULONGLONG totalSeconds = milliseconds / 1000;
    const ULONGLONG centiseconds = (milliseconds % 1000) / 10;
    const ULONGLONG seconds = totalSeconds % 60;
    const ULONGLONG minutes = (totalSeconds / 60) % 60;
    const ULONGLONG hours = totalSeconds / 3600;
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%02llu:%02llu:%02llu.%02llu", hours, minutes, seconds, centiseconds);
    return buffer;
}

class ToolDialogBase {
public:
    ToolDialogBase(HWND owner, HINSTANCE instance, const Theme& theme, PluginRegistry& registry, std::wstring title, int width, int height)
        : owner_(owner), instance_(instance), theme_(theme), registry_(registry), title_(std::move(title)), width_(width), height_(height) {}

    bool Run() {
        const std::wstring className = L"QuattroBuiltinTool_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ToolDialogBase::Proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = className.c_str();
        if (!RegisterClassExW(&wc)) {
            const DWORD error = GetLastError();
            if (error != ERROR_CLASS_ALREADY_EXISTS) {
                WriteAppLog(title_ + L"窗口类注册失败: " + FormatLastError(error));
                return false;
            }
        }

        RECT ownerRect{};
        GetWindowRect(owner_, &ownerRect);
        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
            className.c_str(),
            title_.c_str(),
            WS_CAPTION | WS_SYSMENU | WS_POPUP,
            ownerRect.left + 70,
            ownerRect.top + 70,
            width_,
            height_,
            owner_,
            nullptr,
            instance_,
            this);
        if (!hwnd_) {
            WriteAppLog(title_ + L"窗口创建失败: " + FormatLastError(GetLastError()));
            return false;
        }

        ownerWasEnabled_ = ShowModalWindow(owner_, hwnd_);
        UpdateWindow(hwnd_);
        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
        return true;
    }

protected:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        ToolDialogBase* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<ToolDialogBase*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<ToolDialogBase*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            font_ = ThemedControls::CreateDialogFont();
            backgroundBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
            OnCreate();
            return 0;
        case WM_COMMAND:
            if (OnCommand(LOWORD(wParam), HIWORD(wParam))) {
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                Close();
                return 0;
            }
            return 0;
        case WM_QUATTRO_TOOL_AUTOMATION:
            if (OnCommand(static_cast<int>(wParam), 0)) {
                return 1;
            }
            return 0;
        case WM_QUATTRO_TOOL_TIMER_AUTOMATION:
            if (OnTimer(static_cast<UINT_PTR>(wParam))) {
                return 1;
            }
            return 0;
        case WM_TIMER:
            if (OnTimer(static_cast<UINT_PTR>(wParam))) {
                return 0;
            }
            return 0;
        case WM_HOTKEY:
            if (OnHotKey(static_cast<int>(wParam))) {
                return 0;
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            RECT rect{};
            GetClientRect(hwnd_, &rect);
            FillRect(dc, &rect, backgroundBrush_ ? backgroundBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            OnPaint(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, ToColorRef(theme_.color(L"label", L"normal", L"text")));
            return reinterpret_cast<LRESULT>(backgroundBrush_ ? backgroundBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_DRAWITEM:
            if (ThemedControls::Draw(theme_, reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
                return TRUE;
            }
            return 0;
        case WM_CLOSE:
            Close();
            return 0;
        case WM_DESTROY:
            OnDestroy();
            done_ = true;
            RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
            if (font_) {
                DeleteObject(font_);
                font_ = nullptr;
            }
            if (backgroundBrush_) {
                DeleteObject(backgroundBrush_);
                backgroundBrush_ = nullptr;
            }
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    virtual void OnCreate() = 0;
    virtual bool OnCommand(int id, int notify) = 0;
    virtual bool OnTimer(UINT_PTR id) { (void)id; return false; }
    virtual bool OnHotKey(int id) { (void)id; return false; }
    virtual void OnPaint(HDC dc) { (void)dc; }
    virtual void OnDestroy() {}

    void Close() {
        done_ = true;
        DestroyWindow(hwnd_);
    }

    HFONT font() const {
        return font_ ? font_ : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    PluginRegistry& registry_;
    HWND hwnd_ = nullptr;
    std::wstring title_;
    int width_ = 0;
    int height_ = 0;
    HFONT font_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
    bool done_ = false;
};

class ClickerDialog final : public ToolDialogBase {
public:
    ClickerDialog(HWND owner, HINSTANCE instance, const Theme& theme, PluginRegistry& registry)
        : ToolDialogBase(owner, instance, theme, registry, L"连点器", 460, 340) {}

private:
    void OnCreate() override {
        const wchar_t* pluginId = L"quattro.builtin.clicker";
        ThemedControls::CreateStaticText(instance_, hwnd_, L"X 坐标", 24, 24, 80, 22, font());
        x_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", registry_.GetSetting(pluginId, L"x", L"0").c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 100, 22, 90, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CLICK_X)), instance_, nullptr);
        ThemedControls::CreateStaticText(instance_, hwnd_, L"Y 坐标", 218, 24, 80, 22, font());
        y_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", registry_.GetSetting(pluginId, L"y", L"0").c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 294, 22, 90, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CLICK_Y)), instance_, nullptr);

        ThemedControls::CreateStaticText(instance_, hwnd_, L"点击次数", 24, 62, 80, 22, font());
        count_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", registry_.GetSetting(pluginId, L"count", L"10").c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, 100, 60, 90, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CLICK_COUNT)), instance_, nullptr);
        ThemedControls::CreateStaticText(instance_, hwnd_, L"间隔(ms)", 218, 62, 80, 22, font());
        interval_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", registry_.GetSetting(pluginId, L"interval", L"1000").c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, 294, 60, 90, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CLICK_INTERVAL)), instance_, nullptr);

        ThemedControls::CreateStaticText(instance_, hwnd_, L"鼠标按键", 24, 102, 80, 22, font());
        button_ = CreateWindowExW(0, WC_COMBOBOXW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 100, 100, 120, 180, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CLICK_BUTTON)), instance_, nullptr);
        SendMessageW(button_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"左键"));
        SendMessageW(button_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"右键"));
        SendMessageW(button_, CB_SETCURSEL, registry_.GetSetting(pluginId, L"button", L"left") == L"right" ? 1 : 0, 0);

        ThemedControls::CreateStaticText(instance_, hwnd_, L"倒计时(s)", 238, 102, 80, 22, font());
        countdownEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", registry_.GetSetting(pluginId, L"countdown", L"3").c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, 318, 100, 66, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CLICK_COUNTDOWN)), instance_, nullptr);

        ThemedControls::CreateStaticText(instance_, hwnd_, L"停止热键", 24, 142, 80, 22, font());
        hotKey_ = CreateWindowExW(0, WC_COMBOBOXW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 100, 140, 120, 180, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CLICK_HOTKEY)), instance_, nullptr);
        const wchar_t* hotKeys[] = {L"F6", L"F7", L"F8", L"F9", L"F10", L"F11", L"F12"};
        const std::wstring savedHotKey = registry_.GetSetting(pluginId, L"stopHotKey", L"F8");
        int selectedHotKey = 2;
        for (int i = 0; i < 7; ++i) {
            SendMessageW(hotKey_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(hotKeys[i]));
            if (savedHotKey == hotKeys[i]) {
                selectedHotKey = i;
            }
        }
        SendMessageW(hotKey_, CB_SETCURSEL, selectedHotKey, 0);

        SendMessageW(x_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
        SendMessageW(y_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
        SendMessageW(count_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
        SendMessageW(interval_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
        SendMessageW(button_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
        SendMessageW(countdownEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
        SendMessageW(hotKey_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);

        const int bh = ThemedControls::ButtonHeight(theme_);
        ThemedControls::CreateButton(instance_, hwnd_, ID_CLICK_PICK, L"拾取当前坐标", 238, 138, 146, bh, font());
        livePosition_ = ThemedControls::CreateStaticText(instance_, hwnd_, L"当前鼠标：0, 0", 24, 184, 360, 22, font());
        start_ = ThemedControls::CreateButton(instance_, hwnd_, ID_CLICK_START, L"开始", 100, 224, 82, bh, font(), true);
        stop_ = ThemedControls::CreateButton(instance_, hwnd_, ID_CLICK_STOP, L"停止", 196, 224, 82, bh, font());
        EnableWindow(stop_, FALSE);
        status_ = ThemedControls::CreateStaticText(instance_, hwnd_, L"就绪。", 24, 274, 386, 22, font());
        SetTimer(hwnd_, ID_CLICK_POSITION_TIMER, 250, nullptr);
    }

    bool OnCommand(int id, int) override {
        if (id == ID_CLICK_PICK) {
            POINT point{};
            GetCursorPos(&point);
            SetWindowTextW(x_, std::to_wstring(point.x).c_str());
            SetWindowTextW(y_, std::to_wstring(point.y).c_str());
            return true;
        }
        if (id == ID_CLICK_START) {
            Start();
            return true;
        }
        if (id == ID_CLICK_STOP) {
            Stop(L"已停止。");
            return true;
        }
        return false;
    }

    bool OnTimer(UINT_PTR id) override {
        if (id == ID_CLICK_COUNTDOWN_TIMER) {
            --countdown_;
            if (countdown_ <= 0) {
                KillTimer(hwnd_, ID_CLICK_COUNTDOWN_TIMER);
                BeginClicking();
            } else {
                SetText(status_, std::to_wstring(countdown_) + L" 秒后开始...");
            }
            return true;
        }
        if (id == ID_CLICK_TIMER) {
            DoClick();
            --remaining_;
            if (remaining_ <= 0) {
                Stop(L"点击完成。");
            } else {
                SetText(status_, L"剩余 " + std::to_wstring(remaining_) + L" 次，按 " + HotKeyName(stopHotKey_) + L" 停止。");
            }
            return true;
        }
        if (id == ID_CLICK_POSITION_TIMER) {
            UpdateLivePosition();
            return true;
        }
        return false;
    }

    bool OnHotKey(int id) override {
        if (id == ID_CLICK_STOP_HOTKEY && running_) {
            Stop(L"已通过 F8 停止。");
            return true;
        }
        return false;
    }

    void OnDestroy() override {
        KillTimer(hwnd_, ID_CLICK_POSITION_TIMER);
        Stop(L"");
    }

    void Start() {
        if (running_) {
            return;
        }
        targetX_ = ReadIntField(x_, 0, 0, 99999);
        targetY_ = ReadIntField(y_, 0, 0, 99999);
        remaining_ = ReadIntField(count_, 10, 1, 99999);
        intervalMs_ = ReadIntField(interval_, 1000, 100, 3600000);
        countdown_ = ReadIntField(countdownEdit_, 3, 0, 60);
        const bool rightButton = SendMessageW(button_, CB_GETCURSEL, 0, 0) == 1;
        wchar_t hotKeyText[32]{};
        SendMessageW(hotKey_, CB_GETLBTEXT, SendMessageW(hotKey_, CB_GETCURSEL, 0, 0), reinterpret_cast<LPARAM>(hotKeyText));
        stopHotKey_ = HotKeyFromName(hotKeyText);

        registry_.SetSetting(L"quattro.builtin.clicker", L"x", std::to_wstring(targetX_));
        registry_.SetSetting(L"quattro.builtin.clicker", L"y", std::to_wstring(targetY_));
        registry_.SetSetting(L"quattro.builtin.clicker", L"count", std::to_wstring(remaining_));
        registry_.SetSetting(L"quattro.builtin.clicker", L"interval", std::to_wstring(intervalMs_));
        registry_.SetSetting(L"quattro.builtin.clicker", L"countdown", std::to_wstring(countdown_));
        registry_.SetSetting(L"quattro.builtin.clicker", L"stopHotKey", HotKeyName(stopHotKey_));
        registry_.SetSetting(L"quattro.builtin.clicker", L"button", rightButton ? L"right" : L"left");

        rightButton_ = rightButton;
        running_ = true;
        EnableWindow(start_, FALSE);
        EnableWindow(stop_, TRUE);
        if (!RegisterHotKey(hwnd_, ID_CLICK_STOP_HOTKEY, MOD_NOREPEAT, stopHotKey_)) {
            SetText(status_, L"停止热键注册失败，请使用停止按钮。");
        }
        if (countdown_ <= 0) {
            BeginClicking();
        } else {
            SetText(status_, std::to_wstring(countdown_) + L" 秒后开始...");
            SetTimer(hwnd_, ID_CLICK_COUNTDOWN_TIMER, 1000, nullptr);
        }
    }

    void Stop(const std::wstring& status) {
        KillTimer(hwnd_, ID_CLICK_COUNTDOWN_TIMER);
        KillTimer(hwnd_, ID_CLICK_TIMER);
        UnregisterHotKey(hwnd_, ID_CLICK_STOP_HOTKEY);
        running_ = false;
        if (start_) {
            EnableWindow(start_, TRUE);
        }
        if (stop_) {
            EnableWindow(stop_, FALSE);
        }
        if (!status.empty() && status_) {
            SetText(status_, status);
        }
    }

    void BeginClicking() {
        SetText(status_, L"正在点击，按 " + HotKeyName(stopHotKey_) + L" 或点击停止可中止。");
        DoClick();
        --remaining_;
        if (remaining_ <= 0) {
            Stop(L"点击完成。");
            return;
        }
        SetTimer(hwnd_, ID_CLICK_TIMER, static_cast<UINT>(intervalMs_), nullptr);
    }

    void DoClick() {
        SetCursorPos(targetX_, targetY_);
        INPUT inputs[2]{};
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dwFlags = rightButton_ ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
        inputs[1].type = INPUT_MOUSE;
        inputs[1].mi.dwFlags = rightButton_ ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
        SendInput(2, inputs, sizeof(INPUT));
    }

    void UpdateLivePosition() {
        if (!livePosition_) {
            return;
        }
        POINT point{};
        GetCursorPos(&point);
        SetText(livePosition_, L"当前鼠标：" + std::to_wstring(point.x) + L", " + std::to_wstring(point.y));
    }

    HWND x_ = nullptr;
    HWND y_ = nullptr;
    HWND count_ = nullptr;
    HWND interval_ = nullptr;
    HWND button_ = nullptr;
    HWND countdownEdit_ = nullptr;
    HWND hotKey_ = nullptr;
    HWND livePosition_ = nullptr;
    HWND start_ = nullptr;
    HWND stop_ = nullptr;
    HWND status_ = nullptr;
    int targetX_ = 0;
    int targetY_ = 0;
    int remaining_ = 0;
    int intervalMs_ = 1000;
    int countdown_ = 0;
    int stopHotKey_ = VK_F8;
    bool rightButton_ = false;
    bool running_ = false;
};

class TimerDialog final : public ToolDialogBase {
public:
    TimerDialog(HWND owner, HINSTANCE instance, const Theme& theme, PluginRegistry& registry)
        : ToolDialogBase(owner, instance, theme, registry, L"计时器", 410, 270) {}

private:
    void OnCreate() override {
        const int fallbackSeconds = ParseInt(registry_.GetSetting(L"quattro.builtin.timer", L"seconds", L"300")).value_or(300);
        const std::wstring hours = registry_.GetSetting(L"quattro.builtin.timer", L"hours", std::to_wstring(fallbackSeconds / 3600));
        const std::wstring minutes = registry_.GetSetting(L"quattro.builtin.timer", L"minutes", std::to_wstring((fallbackSeconds / 60) % 60));
        const std::wstring seconds = registry_.GetSetting(L"quattro.builtin.timer", L"secondsPart", std::to_wstring(fallbackSeconds % 60));

        ThemedControls::CreateStaticText(instance_, hwnd_, L"时", 28, 28, 28, 22, font());
        hours_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", hours.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, 58, 26, 58, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TIMER_HOURS)), instance_, nullptr);
        ThemedControls::CreateStaticText(instance_, hwnd_, L"分", 130, 28, 28, 22, font());
        minutes_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", minutes.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, 160, 26, 58, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TIMER_MINUTES)), instance_, nullptr);
        ThemedControls::CreateStaticText(instance_, hwnd_, L"秒", 232, 28, 28, 22, font());
        seconds_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", seconds.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, 262, 26, 58, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TIMER_SECONDS)), instance_, nullptr);
        sound_ = ThemedControls::CreateCheckBox(instance_, hwnd_, ID_TIMER_SOUND, L"声音提醒", 28, 62, 120, ThemedControls::CheckBoxHeight(theme_), font_, registry_.GetSetting(L"quattro.builtin.timer", L"sound", L"1") != L"0");
        topMost_ = ThemedControls::CreateCheckBox(instance_, hwnd_, ID_TIMER_TOPMOST, L"置顶提醒", 168, 62, 120, ThemedControls::CheckBoxHeight(theme_), font_, registry_.GetSetting(L"quattro.builtin.timer", L"topMost", L"1") != L"0");
        SendMessageW(hours_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
        SendMessageW(minutes_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
        SendMessageW(seconds_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
        display_ = ThemedControls::CreateStaticText(instance_, hwnd_, L"00:05:00", 28, 110, 330, 34, font(), SS_CENTER);
        const int bh = ThemedControls::ButtonHeight(theme_);
        ThemedControls::CreateButton(instance_, hwnd_, ID_TIMER_START, L"开始", 52, 164, 78, bh, font(), true);
        ThemedControls::CreateButton(instance_, hwnd_, ID_TIMER_PAUSE, L"暂停", 154, 164, 78, bh, font());
        ThemedControls::CreateButton(instance_, hwnd_, ID_TIMER_RESET, L"重置", 256, 164, 78, bh, font());
        UpdateDisplay(ReadDurationFields());
    }

    bool OnCommand(int id, int) override {
        if (id == ID_TIMER_START) {
            if (!running_) {
                if (remaining_ <= 0) {
                    remaining_ = ReadDurationFields();
                }
                SaveSettings();
                running_ = true;
                SetTimer(hwnd_, ID_TIMER_TICK, 1000, nullptr);
            }
            return true;
        }
        if (id == ID_TIMER_PAUSE) {
            running_ = false;
            KillTimer(hwnd_, ID_TIMER_TICK);
            return true;
        }
        if (id == ID_TIMER_RESET) {
            running_ = false;
            KillTimer(hwnd_, ID_TIMER_TICK);
            remaining_ = ReadDurationFields();
            UpdateDisplay(remaining_);
            return true;
        }
        return false;
    }

    bool OnTimer(UINT_PTR id) override {
        if (id != ID_TIMER_TICK) {
            return false;
        }
        if (remaining_ > 0) {
            --remaining_;
        }
        UpdateDisplay(remaining_);
        if (remaining_ <= 0) {
            running_ = false;
            KillTimer(hwnd_, ID_TIMER_TICK);
            if (SendMessageW(sound_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                MessageBeep(MB_ICONEXCLAMATION);
            }
            const UINT topFlag = SendMessageW(topMost_, BM_GETCHECK, 0, 0) == BST_CHECKED ? MB_TOPMOST : 0;
            MessageBoxW(hwnd_, L"计时结束。", L"计时器", MB_OK | MB_ICONINFORMATION | topFlag);
        }
        return true;
    }

    void OnDestroy() override {
        KillTimer(hwnd_, ID_TIMER_TICK);
    }

    void UpdateDisplay(int seconds) {
        remaining_ = seconds;
        wchar_t buffer[32]{};
        swprintf_s(buffer, L"%02d:%02d:%02d", seconds / 3600, (seconds / 60) % 60, seconds % 60);
        SetText(display_, buffer);
    }

    int ReadDurationFields() {
        const int hours = ReadIntField(hours_, 0, 0, 23);
        const int minutes = ReadIntField(minutes_, 5, 0, 59);
        const int seconds = ReadIntField(seconds_, 0, 0, 59);
        return std::max(1, std::min(86400, hours * 3600 + minutes * 60 + seconds));
    }

    void SaveSettings() {
        const int hours = ReadIntField(hours_, 0, 0, 23);
        const int minutes = ReadIntField(minutes_, 5, 0, 59);
        const int seconds = ReadIntField(seconds_, 0, 0, 59);
        registry_.SetSetting(L"quattro.builtin.timer", L"hours", std::to_wstring(hours));
        registry_.SetSetting(L"quattro.builtin.timer", L"minutes", std::to_wstring(minutes));
        registry_.SetSetting(L"quattro.builtin.timer", L"secondsPart", std::to_wstring(seconds));
        registry_.SetSetting(L"quattro.builtin.timer", L"seconds", std::to_wstring(hours * 3600 + minutes * 60 + seconds));
        registry_.SetSetting(L"quattro.builtin.timer", L"sound", SendMessageW(sound_, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
        registry_.SetSetting(L"quattro.builtin.timer", L"topMost", SendMessageW(topMost_, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
    }

    HWND hours_ = nullptr;
    HWND minutes_ = nullptr;
    HWND seconds_ = nullptr;
    HWND display_ = nullptr;
    HWND sound_ = nullptr;
    HWND topMost_ = nullptr;
    int remaining_ = 0;
    bool running_ = false;
};

class StopwatchDialog final : public ToolDialogBase {
public:
    StopwatchDialog(HWND owner, HINSTANCE instance, const Theme& theme, PluginRegistry& registry)
        : ToolDialogBase(owner, instance, theme, registry, L"秒表", 440, 370) {}

private:
    void OnCreate() override {
        display_ = ThemedControls::CreateStaticText(instance_, hwnd_, L"00:00:00.00", 24, 24, 350, 38, font(), SS_CENTER);
        const int bh = ThemedControls::ButtonHeight(theme_);
        ThemedControls::CreateButton(instance_, hwnd_, ID_SW_START, L"开始", 24, 76, 70, bh, font(), true);
        ThemedControls::CreateButton(instance_, hwnd_, ID_SW_PAUSE, L"暂停", 108, 76, 70, bh, font());
        ThemedControls::CreateButton(instance_, hwnd_, ID_SW_RESET, L"重置", 192, 76, 70, bh, font());
        ThemedControls::CreateButton(instance_, hwnd_, ID_SW_LAP, L"计次", 276, 76, 70, bh, font());
        ThemedControls::CreateButton(instance_, hwnd_, ID_SW_COPY, L"复制", 24, 258, 70, bh, font());
        ThemedControls::CreateButton(instance_, hwnd_, ID_SW_EXPORT, L"导出", 108, 258, 70, bh, font());
        laps_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL, 24, 124, 374, 124, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SW_LAPS)), instance_, nullptr);
        SendMessageW(laps_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
        LoadLapHistory();
    }

    bool OnCommand(int id, int) override {
        if (id == ID_SW_START) {
            if (!running_) {
                running_ = true;
                startedAt_ = GetTickCount64();
                SetTimer(hwnd_, ID_SW_TICK, 80, nullptr);
            }
            return true;
        }
        if (id == ID_SW_PAUSE) {
            if (running_) {
                accumulatedMs_ += GetTickCount64() - startedAt_;
                running_ = false;
                KillTimer(hwnd_, ID_SW_TICK);
                UpdateDisplay();
            }
            return true;
        }
        if (id == ID_SW_RESET) {
            running_ = false;
            accumulatedMs_ = 0;
            startedAt_ = 0;
            KillTimer(hwnd_, ID_SW_TICK);
            SendMessageW(laps_, LB_RESETCONTENT, 0, 0);
            SaveLapHistory();
            UpdateDisplay();
            return true;
        }
        if (id == ID_SW_LAP) {
            const std::wstring text = std::to_wstring(static_cast<int>(SendMessageW(laps_, LB_GETCOUNT, 0, 0)) + 1) + L". " + CurrentText();
            SendMessageW(laps_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            SaveLapHistory();
            return true;
        }
        if (id == ID_SW_COPY) {
            CopyTextToClipboard(hwnd_, ExportText());
            return true;
        }
        if (id == ID_SW_EXPORT) {
            ExportToFile();
            return true;
        }
        return false;
    }

    bool OnTimer(UINT_PTR id) override {
        if (id == ID_SW_TICK) {
            UpdateDisplay();
            return true;
        }
        return false;
    }

    void OnDestroy() override {
        KillTimer(hwnd_, ID_SW_TICK);
        SaveLapHistory();
    }

    ULONGLONG ElapsedMs() const {
        return accumulatedMs_ + (running_ ? GetTickCount64() - startedAt_ : 0);
    }

    std::wstring CurrentText() const {
        return FormatElapsed(ElapsedMs());
    }

    void UpdateDisplay() {
        SetText(display_, CurrentText());
    }

    std::wstring ExportText() const {
        std::wstring text = L"当前时间: " + CurrentText();
        const int count = static_cast<int>(SendMessageW(laps_, LB_GETCOUNT, 0, 0));
        for (int i = 0; i < count; ++i) {
            wchar_t buffer[128]{};
            SendMessageW(laps_, LB_GETTEXT, i, reinterpret_cast<LPARAM>(buffer));
            text += L"\r\n";
            text += buffer;
        }
        return text;
    }

    void SaveLapHistory() {
        if (!laps_) {
            return;
        }
        std::wstring history;
        const int count = static_cast<int>(SendMessageW(laps_, LB_GETCOUNT, 0, 0));
        for (int i = 0; i < count; ++i) {
            wchar_t buffer[128]{};
            SendMessageW(laps_, LB_GETTEXT, i, reinterpret_cast<LPARAM>(buffer));
            if (!history.empty()) {
                history += L"\n";
            }
            history += buffer;
        }
        registry_.SetSetting(L"quattro.builtin.stopwatch", L"laps", history);
    }

    void LoadLapHistory() {
        const auto lines = SplitLines(registry_.GetSetting(L"quattro.builtin.stopwatch", L"laps", L""));
        for (const auto& line : lines) {
            SendMessageW(laps_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
        }
    }

    void ExportToFile() {
        std::wstring path;
        wchar_t testExportPath[32768]{};
        const DWORD testExportLength = GetEnvironmentVariableW(L"QUATTRO_TEST_STOPWATCH_EXPORT", testExportPath, static_cast<DWORD>(_countof(testExportPath)));
        if (testExportLength > 0 && testExportLength < _countof(testExportPath)) {
            path.assign(testExportPath, testExportLength);
        } else {
            std::wstring buffer = L"quattro-stopwatch.txt";
            buffer.resize(32768, L'\0');
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd_;
            ofn.lpstrFile = buffer.data();
            ofn.nMaxFile = static_cast<DWORD>(buffer.size());
            ofn.lpstrFilter = L"文本文件 (*.txt)\0*.txt\0所有文件\0*.*\0";
            ofn.lpstrDefExt = L"txt";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (!GetSaveFileNameW(&ofn)) {
                return;
            }
            path = buffer.c_str();
        }
        std::ofstream file(path.c_str(), std::ios::binary | std::ios::trunc);
        const std::wstring text = ExportText();
        if (!file) {
            MessageBoxW(hwnd_, L"导出失败。", L"秒表", MB_OK | MB_ICONWARNING);
            return;
        }
        const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string bytes(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), bytes.data(), length, nullptr, nullptr);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!file.good()) {
            MessageBoxW(hwnd_, L"导出失败。", L"秒表", MB_OK | MB_ICONWARNING);
        }
    }

    HWND display_ = nullptr;
    HWND laps_ = nullptr;
    bool running_ = false;
    ULONGLONG startedAt_ = 0;
    ULONGLONG accumulatedMs_ = 0;
};
}

bool ShowBuiltinTool(HWND owner, HINSTANCE instance, const Theme& theme, PluginRegistry& registry, const std::wstring& engine) {
    if (engine == L"clicker") {
        ClickerDialog dialog(owner, instance, theme, registry);
        return dialog.Run();
    }
    if (engine == L"timer") {
        TimerDialog dialog(owner, instance, theme, registry);
        return dialog.Run();
    }
    if (engine == L"stopwatch") {
        StopwatchDialog dialog(owner, instance, theme, registry);
        return dialog.Run();
    }
    MessageBoxW(owner, L"这个内置工具暂不可用。", L"插件商店", MB_OK | MB_ICONINFORMATION);
    return false;
}
