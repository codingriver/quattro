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
constexpr int ID_CLICK_COORD = 7001;
constexpr int ID_CLICK_COUNT = 7003;
constexpr int ID_CLICK_INTERVAL = 7004;
constexpr int ID_CLICK_BUTTON = 7005;
constexpr int ID_CLICK_PICK = 7006;
constexpr int ID_CLICK_TOGGLE = 7007;
constexpr UINT_PTR ID_CLICK_COUNTDOWN_TIMER = 7010;
constexpr UINT_PTR ID_CLICK_TIMER = 7011;
constexpr int ID_CLICK_TOGGLE_HOTKEY = 7012;
constexpr int ID_CLICK_COUNTDOWN = 7013;
constexpr int ID_CLICK_HOTKEY = 7014;
constexpr int ID_CLICK_PICK_HOTKEY_CONTROL = 7017;
constexpr int ID_CLICK_PICK_HOTKEY = 7018;
constexpr int ID_CLICK_PROGRESS = 7019;

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
constexpr UINT kTimerDisplayIntervalMs = 33;

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
    const ULONGLONG ms = milliseconds % 1000;
    const ULONGLONG seconds = totalSeconds % 60;
    const ULONGLONG minutes = (totalSeconds / 60) % 60;
    const ULONGLONG hours = totalSeconds / 3600;
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%02llu:%02llu:%02llu.%03llu", hours, minutes, seconds, ms);
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
            if (IsToolKeyMessage(message) && OnShortcutKey(message)) {
                continue;
            }
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
            if (OnAutomationTimer(static_cast<UINT_PTR>(wParam))) {
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
    virtual bool OnShortcutKey(const MSG& message) { (void)message; return false; }
    virtual bool OnTimer(UINT_PTR id) { (void)id; return false; }
    virtual bool OnAutomationTimer(UINT_PTR id) { return OnTimer(id); }
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

    bool IsToolKeyMessage(const MSG& message) const {
        if (message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN) {
            return false;
        }
        return message.hwnd == hwnd_ || IsChild(hwnd_, message.hwnd);
    }

    bool CtrlOnly() const {
        return (GetKeyState(VK_CONTROL) & 0x8000) != 0
            && (GetKeyState(VK_MENU) & 0x8000) == 0
            && (GetKeyState(VK_SHIFT) & 0x8000) == 0;
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
        : ToolDialogBase(owner, instance, theme, registry, L"连点器", 390, 246) {}

private:
    void OnCreate() override {
        const wchar_t* pluginId = L"quattro.builtin.clicker";
        const std::wstring savedX = registry_.GetSetting(pluginId, L"x", L"0");
        const std::wstring savedY = registry_.GetSetting(pluginId, L"y", L"0");

        ThemedControls::CreateStaticText(instance_, hwnd_, L"坐标（x，y）", 18, 20, 92, 22, font());
        coord_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", (savedX + L", " + savedY).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 112, 18, 100, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CLICK_COORD)), instance_, nullptr);

        const int bh = ThemedControls::ButtonHeight(theme_);
        ThemedControls::CreateButton(instance_, hwnd_, ID_CLICK_PICK, L"拾取(&P)", 228, 17, 86, bh, font());

        ThemedControls::CreateStaticText(instance_, hwnd_, L"点击次数", 18, 56, 76, 22, font());
        count_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", registry_.GetSetting(pluginId, L"count", L"10").c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, 94, 54, 70, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CLICK_COUNT)), instance_, nullptr);
        ThemedControls::CreateStaticText(instance_, hwnd_, L"间隔(ms)", 184, 56, 76, 22, font());
        interval_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", registry_.GetSetting(pluginId, L"interval", L"1000").c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, 260, 54, 86, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CLICK_INTERVAL)), instance_, nullptr);

        ThemedControls::CreateStaticText(instance_, hwnd_, L"鼠标按键", 18, 92, 76, 22, font());
        button_ = CreateWindowExW(0, WC_COMBOBOXW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 94, 90, 88, 180, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CLICK_BUTTON)), instance_, nullptr);
        SendMessageW(button_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"左键"));
        SendMessageW(button_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"右键"));
        SendMessageW(button_, CB_SETCURSEL, registry_.GetSetting(pluginId, L"button", L"left") == L"right" ? 1 : 0, 0);

        ThemedControls::CreateStaticText(instance_, hwnd_, L"倒计时(s)", 198, 92, 76, 22, font());
        countdownEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", registry_.GetSetting(pluginId, L"countdown", L"3").c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, 276, 90, 70, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CLICK_COUNTDOWN)), instance_, nullptr);

        const wchar_t* hotKeys[] = {L"F6", L"F7", L"F8", L"F9", L"F10", L"F11", L"F12"};

        ThemedControls::CreateStaticText(instance_, hwnd_, L"启动停止热键", 18, 128, 96, 22, font());
        toggleHotKey_ = CreateWindowExW(0, WC_COMBOBOXW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 116, 126, 68, 180, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CLICK_HOTKEY)), instance_, nullptr);
        FillHotKeyCombo(toggleHotKey_, registry_.GetSetting(pluginId, L"toggleHotKey", registry_.GetSetting(pluginId, L"stopHotKey", L"F8")), hotKeys, 7);

        ThemedControls::CreateStaticText(instance_, hwnd_, L"拾取热键", 198, 128, 76, 22, font());
        pickHotKey_ = CreateWindowExW(0, WC_COMBOBOXW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 276, 126, 70, 180, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CLICK_PICK_HOTKEY_CONTROL)), instance_, nullptr);
        FillHotKeyCombo(pickHotKey_, registry_.GetSetting(pluginId, L"pickHotKey", L"F9"), hotKeys, 7);

        SendMessageW(coord_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
        SendMessageW(count_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
        SendMessageW(interval_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
        SendMessageW(button_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
        SendMessageW(countdownEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
        SendMessageW(toggleHotKey_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
        SendMessageW(pickHotKey_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);

        toggle_ = ThemedControls::CreateButton(instance_, hwnd_, ID_CLICK_TOGGLE, L"启动(&S)", 126, 154, 110, bh, font(), true);
        status_ = ThemedControls::CreateStaticText(instance_, hwnd_, L"就绪。", 18, 188, 190, 22, font());
        progress_ = CreateWindowExW(0, L"STATIC", L"当前点击：0 / 0",
            WS_CHILD | WS_VISIBLE | SS_RIGHT, 218, 188, 104, 22, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CLICK_PROGRESS)), instance_, nullptr);
        SendMessageW(progress_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
        RegisterToolHotKeys();
    }

    bool OnCommand(int id, int notify) override {
        if (id == ID_CLICK_PICK) {
            PickCurrentPosition();
            return true;
        }
        if (id == ID_CLICK_TOGGLE) {
            ToggleRunning();
            return true;
        }
        if ((id == ID_CLICK_HOTKEY || id == ID_CLICK_PICK_HOTKEY_CONTROL) && notify == CBN_SELCHANGE) {
            SaveHotKeySettings();
            RegisterToolHotKeys();
            return true;
        }
        return false;
    }

    bool OnShortcutKey(const MSG& message) override {
        if (!CtrlOnly()) {
            return false;
        }
        switch (message.wParam) {
        case 'P':
            PickCurrentPosition();
            return true;
        case 'S':
            ToggleRunning();
            return true;
        case 'T':
            Stop(L"已停止。");
            return true;
        default:
            return false;
        }
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
            if (ReachedClickLimit()) {
                Stop(L"点击完成。");
            } else {
                SetText(status_, L"正在点击，按 " + HotKeyName(toggleHotKeyCode_) + L" 停止。");
            }
            return true;
        }
        return false;
    }

    bool OnHotKey(int id) override {
        if (id == ID_CLICK_TOGGLE_HOTKEY) {
            ToggleRunning();
            return true;
        }
        if (id == ID_CLICK_PICK_HOTKEY) {
            PickCurrentPosition();
            return true;
        }
        return false;
    }

    void OnDestroy() override {
        Stop(L"");
        UnregisterToolHotKeys();
    }

    void FillHotKeyCombo(HWND combo, const std::wstring& selected, const wchar_t* const* hotKeys, int count) {
        int selectedIndex = 0;
        for (int i = 0; i < count; ++i) {
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(hotKeys[i]));
            if (selected == hotKeys[i]) {
                selectedIndex = i;
            }
        }
        SendMessageW(combo, CB_SETCURSEL, selectedIndex, 0);
    }

    std::wstring SelectedHotKeyName(HWND combo, const std::wstring& fallback) const {
        const LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
        if (selected < 0) {
            return fallback;
        }
        wchar_t text[32]{};
        SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(selected), reinterpret_cast<LPARAM>(text));
        return text[0] ? std::wstring(text) : fallback;
    }

    void SaveHotKeySettings() {
        registry_.SetSetting(L"quattro.builtin.clicker", L"toggleHotKey", SelectedHotKeyName(toggleHotKey_, L"F8"));
        registry_.SetSetting(L"quattro.builtin.clicker", L"pickHotKey", SelectedHotKeyName(pickHotKey_, L"F9"));
    }

    void RegisterToolHotKeys() {
        UnregisterToolHotKeys();
        toggleHotKeyCode_ = HotKeyFromName(SelectedHotKeyName(toggleHotKey_, L"F8"));
        pickHotKeyCode_ = HotKeyFromName(SelectedHotKeyName(pickHotKey_, L"F9"));
        toggleHotKeyRegistered_ = RegisterHotKey(hwnd_, ID_CLICK_TOGGLE_HOTKEY, MOD_NOREPEAT, toggleHotKeyCode_) != FALSE;
        pickHotKeyRegistered_ = RegisterHotKey(hwnd_, ID_CLICK_PICK_HOTKEY, MOD_NOREPEAT, pickHotKeyCode_) != FALSE;
        if (!toggleHotKeyRegistered_ || !pickHotKeyRegistered_) {
            SetText(status_, L"全局热键注册失败，请换一个 F 键。");
            return;
        }
        SetText(status_, L"就绪。按 " + HotKeyName(toggleHotKeyCode_) + L" 启动/停止，" + HotKeyName(pickHotKeyCode_) + L" 拾取坐标。");
    }

    void UnregisterToolHotKeys() {
        if (toggleHotKeyRegistered_) {
            UnregisterHotKey(hwnd_, ID_CLICK_TOGGLE_HOTKEY);
            toggleHotKeyRegistered_ = false;
        }
        if (pickHotKeyRegistered_) {
            UnregisterHotKey(hwnd_, ID_CLICK_PICK_HOTKEY);
            pickHotKeyRegistered_ = false;
        }
    }

    std::pair<int, int> ReadCoordinateField() const {
        std::wstring text = GetText(coord_);
        for (wchar_t& ch : text) {
            if (ch == L'，' || ch == L'(' || ch == L')' || ch == L'（' || ch == L'）') {
                ch = L',';
            }
        }
        const std::size_t comma = text.find(L',');
        if (comma == std::wstring::npos) {
            return {targetX_, targetY_};
        }
        const int x = std::max(0, std::min(99999, ParseInt(Trim(text.substr(0, comma))).value_or(targetX_)));
        const int y = std::max(0, std::min(99999, ParseInt(Trim(text.substr(comma + 1))).value_or(targetY_)));
        return {x, y};
    }

    void SetCoordinateText(int x, int y) {
        SetText(coord_, std::to_wstring(x) + L", " + std::to_wstring(y));
    }

    void ToggleRunning() {
        if (running_) {
            Stop(L"已停止。");
        } else {
            Start();
        }
    }

    void Start() {
        if (running_) {
            return;
        }
        const auto [x, y] = ReadCoordinateField();
        targetX_ = x;
        targetY_ = y;
        SetCoordinateText(targetX_, targetY_);
        totalClicks_ = ReadIntField(count_, 10, 0, 99999);
        clickedCount_ = 0;
        intervalMs_ = ReadIntField(interval_, 1000, 100, 3600000);
        countdown_ = ReadIntField(countdownEdit_, 3, 0, 60);
        const bool rightButton = SendMessageW(button_, CB_GETCURSEL, 0, 0) == 1;
        toggleHotKeyCode_ = HotKeyFromName(SelectedHotKeyName(toggleHotKey_, L"F8"));
        pickHotKeyCode_ = HotKeyFromName(SelectedHotKeyName(pickHotKey_, L"F9"));

        registry_.SetSetting(L"quattro.builtin.clicker", L"x", std::to_wstring(targetX_));
        registry_.SetSetting(L"quattro.builtin.clicker", L"y", std::to_wstring(targetY_));
        registry_.SetSetting(L"quattro.builtin.clicker", L"count", std::to_wstring(totalClicks_));
        registry_.SetSetting(L"quattro.builtin.clicker", L"interval", std::to_wstring(intervalMs_));
        registry_.SetSetting(L"quattro.builtin.clicker", L"countdown", std::to_wstring(countdown_));
        registry_.SetSetting(L"quattro.builtin.clicker", L"toggleHotKey", HotKeyName(toggleHotKeyCode_));
        registry_.SetSetting(L"quattro.builtin.clicker", L"pickHotKey", HotKeyName(pickHotKeyCode_));
        registry_.SetSetting(L"quattro.builtin.clicker", L"button", rightButton ? L"right" : L"left");

        rightButton_ = rightButton;
        running_ = true;
        SetText(toggle_, L"停止(&S)");
        UpdateProgress();
        if (countdown_ <= 0) {
            BeginClicking();
        } else {
            SetText(status_, std::to_wstring(countdown_) + L" 秒后开始...");
            SetTimer(hwnd_, ID_CLICK_COUNTDOWN_TIMER, 1000, nullptr);
        }
    }

    void PickCurrentPosition() {
        POINT point{};
        GetCursorPos(&point);
        targetX_ = point.x;
        targetY_ = point.y;
        SetCoordinateText(targetX_, targetY_);
        registry_.SetSetting(L"quattro.builtin.clicker", L"x", std::to_wstring(targetX_));
        registry_.SetSetting(L"quattro.builtin.clicker", L"y", std::to_wstring(targetY_));
    }

    void Stop(const std::wstring& status) {
        KillTimer(hwnd_, ID_CLICK_COUNTDOWN_TIMER);
        KillTimer(hwnd_, ID_CLICK_TIMER);
        running_ = false;
        if (toggle_) {
            SetText(toggle_, L"启动(&S)");
        }
        if (!status.empty() && status_) {
            SetText(status_, status);
        }
    }

    void BeginClicking() {
        SetText(status_, L"正在点击，按 " + HotKeyName(toggleHotKeyCode_) + L" 或按钮停止。");
        DoClick();
        if (ReachedClickLimit()) {
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
        ++clickedCount_;
        UpdateProgress();
    }

    bool ReachedClickLimit() const {
        return totalClicks_ > 0 && clickedCount_ >= totalClicks_;
    }

    void UpdateProgress() {
        if (progress_) {
            const std::wstring totalText = totalClicks_ > 0 ? std::to_wstring(totalClicks_) : L"不限";
            SetText(progress_, std::to_wstring(clickedCount_) + L" / " + totalText);
        }
    }

    HWND coord_ = nullptr;
    HWND count_ = nullptr;
    HWND interval_ = nullptr;
    HWND button_ = nullptr;
    HWND countdownEdit_ = nullptr;
    HWND toggleHotKey_ = nullptr;
    HWND pickHotKey_ = nullptr;
    HWND progress_ = nullptr;
    HWND toggle_ = nullptr;
    HWND status_ = nullptr;
    int targetX_ = 0;
    int targetY_ = 0;
    int totalClicks_ = 0;
    int clickedCount_ = 0;
    int intervalMs_ = 1000;
    int countdown_ = 0;
    int toggleHotKeyCode_ = VK_F8;
    int pickHotKeyCode_ = VK_F9;
    bool rightButton_ = false;
    bool running_ = false;
    bool toggleHotKeyRegistered_ = false;
    bool pickHotKeyRegistered_ = false;
};

class TimerDialog final : public ToolDialogBase {
public:
    TimerDialog(HWND owner, HINSTANCE instance, const Theme& theme, PluginRegistry& registry)
        : ToolDialogBase(owner, instance, theme, registry, L"计时器", 300, 206) {}

private:
    void OnCreate() override {
        const int fallbackSeconds = ParseInt(registry_.GetSetting(L"quattro.builtin.timer", L"seconds", L"300")).value_or(300);
        const std::wstring hours = registry_.GetSetting(L"quattro.builtin.timer", L"hours", std::to_wstring(fallbackSeconds / 3600));
        const std::wstring minutes = registry_.GetSetting(L"quattro.builtin.timer", L"minutes", std::to_wstring((fallbackSeconds / 60) % 60));
        const std::wstring seconds = registry_.GetSetting(L"quattro.builtin.timer", L"secondsPart", std::to_wstring(fallbackSeconds % 60));

        ThemedControls::CreateStaticText(instance_, hwnd_, L"时", 18, 18, 22, 22, font());
        hours_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", hours.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, 42, 16, 46, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TIMER_HOURS)), instance_, nullptr);
        ThemedControls::CreateStaticText(instance_, hwnd_, L"分", 102, 18, 22, 22, font());
        minutes_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", minutes.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, 126, 16, 46, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TIMER_MINUTES)), instance_, nullptr);
        ThemedControls::CreateStaticText(instance_, hwnd_, L"秒", 186, 18, 22, 22, font());
        seconds_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", seconds.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER, 210, 16, 46, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TIMER_SECONDS)), instance_, nullptr);
        sound_ = ThemedControls::CreateCheckBox(instance_, hwnd_, ID_TIMER_SOUND, L"声音提醒", 18, 50, 100, ThemedControls::CheckBoxHeight(theme_), font_, registry_.GetSetting(L"quattro.builtin.timer", L"sound", L"1") != L"0");
        topMost_ = ThemedControls::CreateCheckBox(instance_, hwnd_, ID_TIMER_TOPMOST, L"置顶提醒", 132, 50, 100, ThemedControls::CheckBoxHeight(theme_), font_, registry_.GetSetting(L"quattro.builtin.timer", L"topMost", L"1") != L"0");
        SendMessageW(hours_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
        SendMessageW(minutes_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
        SendMessageW(seconds_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
        display_ = ThemedControls::CreateStaticText(instance_, hwnd_, L"00:05:00.000", 18, 84, 238, 26, font(), SS_CENTER);
        const int bh = ThemedControls::ButtonHeight(theme_);
        start_ = ThemedControls::CreateButton(instance_, hwnd_, ID_TIMER_START, L"开始(&S)", 52, 122, 76, bh, font(), true);
        pause_ = ThemedControls::CreateButton(instance_, hwnd_, ID_TIMER_PAUSE, L"暂停(&P)", 52, 122, 76, bh, font());
        reset_ = ThemedControls::CreateButton(instance_, hwnd_, ID_TIMER_RESET, L"重置(&R)", 146, 122, 76, bh, font());
        UpdateDisplay(ReadDurationMs());
        UpdateButtons();
    }

    bool OnCommand(int id, int) override {
        if (id == ID_TIMER_START) {
            StartTimer();
            return true;
        }
        if (id == ID_TIMER_PAUSE) {
            PauseTimer();
            return true;
        }
        if (id == ID_TIMER_RESET) {
            ResetTimer();
            return true;
        }
        return false;
    }

    bool OnShortcutKey(const MSG& message) override {
        if (!CtrlOnly()) {
            return false;
        }
        switch (message.wParam) {
        case 'S':
            StartTimer();
            return true;
        case 'P':
            PauseTimer();
            return true;
        case 'R':
            ResetTimer();
            return true;
        default:
            return false;
        }
    }

    bool OnTimer(UINT_PTR id) override {
        if (id != ID_TIMER_TICK) {
            return false;
        }
        remainingMs_ = CurrentRemainingMs();
        UpdateDisplay(remainingMs_);
        FinishIfExpired();
        return true;
    }

    bool OnAutomationTimer(UINT_PTR id) override {
        if (id != ID_TIMER_TICK) {
            return false;
        }
        if (running_) {
            remainingMs_ = CurrentRemainingMs();
            if (remainingMs_ > 1000) {
                remainingMs_ -= 1000;
                finishAt_ = GetTickCount64() + remainingMs_;
                UpdateDisplay(remainingMs_);
            } else {
                remainingMs_ = 0;
                UpdateDisplay(remainingMs_);
                FinishTimer();
            }
        }
        return true;
    }

    void OnDestroy() override {
        KillTimer(hwnd_, ID_TIMER_TICK);
    }

    void StartTimer() {
        if (running_) {
            return;
        }
        if (!paused_ || remainingMs_ <= 0) {
            remainingMs_ = ReadDurationMs();
        }
        SaveSettings();
        running_ = true;
        paused_ = false;
        finishAt_ = GetTickCount64() + remainingMs_;
        SetTimer(hwnd_, ID_TIMER_TICK, kTimerDisplayIntervalMs, nullptr);
        UpdateDisplay(remainingMs_);
        UpdateButtons();
    }

    void PauseTimer() {
        if (running_) {
            remainingMs_ = CurrentRemainingMs();
            running_ = false;
            paused_ = remainingMs_ > 0;
            KillTimer(hwnd_, ID_TIMER_TICK);
            UpdateDisplay(remainingMs_);
            UpdateButtons();
        }
    }

    void ResetTimer() {
        running_ = false;
        paused_ = false;
        KillTimer(hwnd_, ID_TIMER_TICK);
        remainingMs_ = ReadDurationMs();
        finishAt_ = 0;
        UpdateDisplay(remainingMs_);
        UpdateButtons();
    }

    void FinishIfExpired() {
        if (running_ && remainingMs_ <= 0) {
            FinishTimer();
        }
    }

    void FinishTimer() {
        running_ = false;
        paused_ = false;
        remainingMs_ = 0;
        finishAt_ = 0;
        KillTimer(hwnd_, ID_TIMER_TICK);
        UpdateButtons();
        if (SendMessageW(sound_, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            MessageBeep(MB_ICONEXCLAMATION);
        }
        const UINT topFlag = SendMessageW(topMost_, BM_GETCHECK, 0, 0) == BST_CHECKED ? MB_TOPMOST : 0;
        MessageBoxW(hwnd_, L"计时结束。", L"计时器", MB_OK | MB_ICONINFORMATION | topFlag);
    }

    ULONGLONG CurrentRemainingMs() const {
        if (!running_) {
            return remainingMs_;
        }
        const ULONGLONG now = GetTickCount64();
        return finishAt_ > now ? finishAt_ - now : 0;
    }

    void UpdateDisplay(ULONGLONG milliseconds) {
        remainingMs_ = milliseconds;
        SetText(display_, FormatElapsed(milliseconds));
    }

    void UpdateButtons() {
        if (start_) {
            ShowWindow(start_, running_ ? SW_HIDE : SW_SHOW);
            SetText(start_, paused_ ? L"继续(&S)" : L"开始(&S)");
        }
        if (pause_) {
            ShowWindow(pause_, running_ ? SW_SHOW : SW_HIDE);
        }
        if (reset_) {
            EnableWindow(reset_, TRUE);
        }
    }

    int ReadDurationFields() {
        const int hours = ReadIntField(hours_, 0, 0, 23);
        const int minutes = ReadIntField(minutes_, 5, 0, 59);
        const int seconds = ReadIntField(seconds_, 0, 0, 59);
        return std::max(1, std::min(86400, hours * 3600 + minutes * 60 + seconds));
    }

    ULONGLONG ReadDurationMs() {
        return static_cast<ULONGLONG>(ReadDurationFields()) * 1000;
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
    HWND start_ = nullptr;
    HWND pause_ = nullptr;
    HWND reset_ = nullptr;
    ULONGLONG remainingMs_ = 0;
    ULONGLONG finishAt_ = 0;
    bool running_ = false;
    bool paused_ = false;
};

class StopwatchDialog final : public ToolDialogBase {
public:
    StopwatchDialog(HWND owner, HINSTANCE instance, const Theme& theme, PluginRegistry& registry)
        : ToolDialogBase(owner, instance, theme, registry, L"秒表", 220, 310) {}

private:
    void OnCreate() override {
        display_ = ThemedControls::CreateStaticText(instance_, hwnd_, L"00:00:00.000", 12, 16, 178, 34, font(), SS_CENTER);
        const int bh = ThemedControls::ButtonHeight(theme_);
        start_ = ThemedControls::CreateButton(instance_, hwnd_, ID_SW_START, L"开始(&S)", 12, 58, 86, bh, font(), true);
        pause_ = ThemedControls::CreateButton(instance_, hwnd_, ID_SW_PAUSE, L"暂停(&P)", 12, 58, 86, bh, font());
        ThemedControls::CreateButton(instance_, hwnd_, ID_SW_LAP, L"计次(&L)", 104, 58, 86, bh, font());
        ThemedControls::CreateButton(instance_, hwnd_, ID_SW_RESET, L"重置(&R)", 12, 92, 86, bh, font());
        ThemedControls::CreateButton(instance_, hwnd_, ID_SW_COPY, L"复制(&C)", 104, 92, 86, bh, font());
        ThemedControls::CreateButton(instance_, hwnd_, ID_SW_EXPORT, L"导出(&E)", 12, 126, 178, bh, font());
        laps_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL, 12, 164, 178, 88, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SW_LAPS)), instance_, nullptr);
        SendMessageW(laps_, WM_SETFONT, reinterpret_cast<WPARAM>(font()), TRUE);
        LoadLapHistory();
        UpdateControls();
    }

    bool OnCommand(int id, int) override {
        if (id == ID_SW_START) {
            Start();
            return true;
        }
        if (id == ID_SW_PAUSE) {
            Pause();
            return true;
        }
        if (id == ID_SW_RESET) {
            Reset();
            return true;
        }
        if (id == ID_SW_LAP) {
            AddLap();
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

    bool OnShortcutKey(const MSG& message) override {
        if (!CtrlOnly()) {
            return false;
        }
        switch (message.wParam) {
        case 'S':
            Start();
            return true;
        case 'P':
            Pause();
            return true;
        case 'R':
            Reset();
            return true;
        case 'L':
            AddLap();
            return true;
        case 'C':
            CopyTextToClipboard(hwnd_, ExportText());
            return true;
        case 'E':
            ExportToFile();
            return true;
        default:
            return false;
        }
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

    void Start() {
        if (!running_) {
            running_ = true;
            startedAt_ = GetTickCount64();
            SetTimer(hwnd_, ID_SW_TICK, kTimerDisplayIntervalMs, nullptr);
            UpdateControls();
            UpdateDisplay();
        }
    }

    void Pause() {
        if (running_) {
            accumulatedMs_ += GetTickCount64() - startedAt_;
            running_ = false;
            KillTimer(hwnd_, ID_SW_TICK);
            UpdateControls();
            UpdateDisplay();
        }
    }

    void Reset() {
        running_ = false;
        accumulatedMs_ = 0;
        startedAt_ = 0;
        KillTimer(hwnd_, ID_SW_TICK);
        SendMessageW(laps_, LB_RESETCONTENT, 0, 0);
        SaveLapHistory();
        UpdateControls();
        UpdateDisplay();
    }

    void AddLap() {
        const std::wstring text = std::to_wstring(static_cast<int>(SendMessageW(laps_, LB_GETCOUNT, 0, 0)) + 1) + L". " + CurrentText();
        SendMessageW(laps_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        SaveLapHistory();
    }

    void UpdateDisplay() {
        SetText(display_, CurrentText());
    }

    void UpdateControls() {
        if (start_) {
            ShowWindow(start_, running_ ? SW_HIDE : SW_SHOW);
        }
        if (pause_) {
            ShowWindow(pause_, running_ ? SW_SHOW : SW_HIDE);
        }
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
    HWND start_ = nullptr;
    HWND pause_ = nullptr;
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
