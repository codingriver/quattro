#include "HotKeyEditor.h"

#include "../../resources/resource.h"

#include "MainHotKey.h"
#include "SimpleDialogs.h"
#include "ThemedUi.h"
#include "ThemedWindowUi.h"
#include "Utilities.h"

#include <commctrl.h>
#include <cwctype>
#include <memory>
#include <windowsx.h>

namespace {
constexpr int kDialogWidth = 360;
constexpr int kDialogHeight = 150;
constexpr int kDoubleAltMaxIntervalMs = 450;
constexpr int IdHotKeyEdit = 1001;
constexpr int IdOk = IDOK;

std::wstring GetText(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    return text;
}

std::wstring NormalizedHotKeyText(std::wstring value) {
    value = ToLower(Trim(value));
    std::wstring result;
    result.reserve(value.size());
    for (wchar_t ch : value) {
        if (!iswspace(ch) && ch != L'+' && ch != L'-' && ch != L'_') {
            result.push_back(ch);
        }
    }
    return result;
}

bool IsAltKey(WPARAM key) {
    return key == VK_MENU || key == VK_LMENU || key == VK_RMENU;
}

bool IsModifierKey(WPARAM key) {
    return key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL ||
           key == VK_MENU || key == VK_LMENU || key == VK_RMENU ||
           key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT ||
           key == VK_LWIN || key == VK_RWIN;
}

bool IsAllowedMainKey(int key) {
    return key > 0 && key != VK_CONTROL && key != VK_LCONTROL && key != VK_RCONTROL &&
           key != VK_MENU && key != VK_LMENU && key != VK_RMENU &&
           key != VK_SHIFT && key != VK_LSHIFT && key != VK_RSHIFT &&
           key != VK_LWIN && key != VK_RWIN;
}

bool TryParseFunctionKey(const std::wstring& text, int& key) {
    if (text.size() < 2 || text[0] != L'f') {
        return false;
    }
    int number = 0;
    for (std::size_t i = 1; i < text.size(); ++i) {
        if (!iswdigit(text[i])) {
            return false;
        }
        number = number * 10 + (text[i] - L'0');
    }
    if (number < 1 || number > 24) {
        return false;
    }
    key = VK_F1 + number - 1;
    return true;
}

bool TryParseNamedKey(const std::wstring& text, int& key) {
    struct NamedKey {
        const wchar_t* name;
        int key;
    };
    static constexpr NamedKey keys[] = {
        {L"backspace", VK_BACK},
        {L"delete", VK_DELETE},
        {L"del", VK_DELETE},
        {L"tab", VK_TAB},
        {L"enter", VK_RETURN},
        {L"return", VK_RETURN},
        {L"space", VK_SPACE},
        {L"esc", VK_ESCAPE},
        {L"escape", VK_ESCAPE},
        {L"insert", VK_INSERT},
        {L"ins", VK_INSERT},
        {L"home", VK_HOME},
        {L"end", VK_END},
        {L"pageup", VK_PRIOR},
        {L"pgup", VK_PRIOR},
        {L"pagedown", VK_NEXT},
        {L"pgdn", VK_NEXT},
        {L"left", VK_LEFT},
        {L"right", VK_RIGHT},
        {L"up", VK_UP},
        {L"down", VK_DOWN},
    };
    for (const auto& item : keys) {
        if (text == item.name) {
            key = item.key;
            return true;
        }
    }
    return false;
}

bool ParseHotKeyText(const std::wstring& text, const HotKeyCaptureDialogOptions& options, int& key) {
    const std::wstring trimmed = Trim(text);
    if (trimmed.empty() || trimmed == L"未设置") {
        key = 0;
        return true;
    }

    std::wstring normalized = NormalizedHotKeyText(trimmed);
    if (options.allowDoubleAlt &&
        (normalized == L"双击alt" || normalized == L"doublealt" || normalized == L"altalt")) {
        key = kMainHotKeyDoubleAlt;
        return true;
    }

    constexpr const wchar_t* ctrlAltPrefix = L"ctrlalt";
    constexpr std::size_t ctrlAltPrefixLength = 7;
    if (normalized.rfind(ctrlAltPrefix, 0) == 0) {
        normalized = normalized.substr(ctrlAltPrefixLength);
    }
    if (normalized.empty()) {
        return false;
    }

    if (normalized.size() == 1) {
        const wchar_t ch = normalized[0];
        if (ch >= L'a' && ch <= L'z') {
            key = towupper(ch);
            return true;
        }
        if (ch >= L'0' && ch <= L'9') {
            key = ch;
            return true;
        }
    }

    if (TryParseFunctionKey(normalized, key) || TryParseNamedKey(normalized, key)) {
        return IsAllowedMainKey(key);
    }
    return false;
}

std::wstring DialogHotKeyText(int key, const HotKeyCaptureDialogOptions& options) {
    return options.useMainHotKeyText ? FormatMainHotKeyText(key) : FormatHotKeyText(key);
}

class HotKeyCapture {
public:
    static int Run(HWND owner, HINSTANCE instance, const Theme& theme, int currentKey, HotKeyCaptureDialogOptions options) {
        HotKeyCapture dialog(owner, instance, theme, currentKey, options);
        return dialog.RunImpl();
    }

private:
    HotKeyCapture(HWND owner, HINSTANCE instance, const Theme& theme, int currentKey, HotKeyCaptureDialogOptions options)
        : owner_(owner), instance_(instance), theme_(theme), currentKey_(currentKey), options_(options) {}

    int RunImpl() {
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        ThemedWindowCreateOptions options = ThemedWindowUi::DialogOptions(
            instance_, owner_, L"QuattroHotKeyCaptureDialog", L"录入热键", HotKeyCapture::Proc, this, icon, icon);
        options.clientWidth = kDialogWidth;
        options.clientHeight = kDialogHeight;
        options.placement = ThemedWindowPlacement::OffsetOwner;
        options.offsetX = 110;
        options.offsetY = 120;
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options);
        if (!hwnd_) {
            return currentKey_;
        }

        if (windowUi_) {
            windowUi_->ShowModal();
        }
        UpdateWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (ThemedUi::PreTranslateMessage(message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (windowUi_) {
            windowUi_->RestoreModalOwner();
        }
        return accepted_ ? capturedKey_ : currentKey_;
    }

    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        HotKeyCapture* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<HotKeyCapture*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<HotKeyCapture*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    static LRESULT CALLBACK EditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR refData) {
        auto* dialog = reinterpret_cast<HotKeyCapture*>(refData);
        return dialog ? dialog->HandleEdit(hwnd, message, wParam, lParam) : DefSubclassProc(hwnd, message, wParam, lParam);
    }

    LRESULT HandleEdit(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, HotKeyCapture::EditProc, 1);
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wParam == VK_ESCAPE) {
                RequestCancel();
                return 0;
            }
            if (wParam == VK_BACK) {
                SetWindowTextW(edit_, L"");
                return 0;
            }
            if (!IsModifierKey(wParam)) {
                SetCapturedText(static_cast<int>(wParam));
                otherKeySinceAlt_ = true;
                lastAltUpTick_ = 0;
                return 0;
            }
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (options_.allowDoubleAlt && IsAltKey(wParam) && !otherKeySinceAlt_) {
                const DWORD now = GetTickCount();
                if (lastAltUpTick_ != 0 && now - lastAltUpTick_ <= kDoubleAltMaxIntervalMs) {
                    SetWindowTextW(edit_, FormatMainHotKeyText(kMainHotKeyDoubleAlt).c_str());
                    SendMessageW(edit_, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
                    lastAltUpTick_ = 0;
                    return 0;
                }
                lastAltUpTick_ = now;
                return 0;
            }
            if (!IsAltKey(wParam)) {
                otherKeySinceAlt_ = false;
            }
            return 0;
        default:
            break;
        }
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }

    void SetCapturedText(int key) {
        if (!IsAllowedMainKey(key)) {
            return;
        }
        SetWindowTextW(edit_, FormatHotKeyText(key).c_str());
        SendMessageW(edit_, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    }

    void Close(bool accepted) {
        accepted_ = accepted;
        done_ = true;
        DestroyWindow(hwnd_);
    }

    bool HasPendingChanges() const {
        return edit_ && GetText(edit_) != initialText_;
    }

    void RequestCancel() {
        if (HasPendingChanges()) {
            const int result = ShowThemedMessageBox(
                hwnd_,
                instance_,
                theme_,
                L"快捷键录入内容尚未确定，是否放弃修改？",
                L"录入热键",
                MB_OKCANCEL | MB_ICONWARNING);
            if (result != IDOK) {
                SetFocus(edit_);
                return;
            }
        }
        Close(false);
    }

    void Accept() {
        int key = 0;
        if (!ParseHotKeyText(GetText(edit_), options_, key)) {
            std::wstring message = L"快捷键格式无效。请输入例如 Ctrl+Alt+P、F8，或按键自动录入。";
            if (options_.allowDoubleAlt) {
                message += L"\n主窗口热键也支持“双击 Alt”。";
            }
            ShowThemedMessageBox(hwnd_, instance_, theme_, message, L"录入热键", MB_OK | MB_ICONWARNING);
            SetFocus(edit_);
            SendMessageW(edit_, EM_SETSEL, 0, static_cast<LPARAM>(-1));
            return;
        }
        capturedKey_ = key;
        Close(true);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT commonResult = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, commonResult)) {
            return commonResult;
        }
        switch (message) {
        case WM_CREATE: {
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_, owner_, hwnd_, theme_, DialogLayoutKind::Compact, kDialogWidth, kDialogHeight);
            const ThemedUi ui = windowUi_->ui();
            ThemedLabelOptions instructionOptions{};
            instructionOptions.lines = ThemedLabelLines::Two;
            ui.Label(
                options_.allowDoubleAlt
                    ? L"输入快捷键，或按一个键录入为 Ctrl+Alt+该键；也可快速按两次 Alt。"
                    : L"输入快捷键，或按一个键录入为 Ctrl+Alt+该键。Backspace 清除，Esc 取消。",
                20,
                22,
                320,
                instructionOptions);

            RECT editFrame = ui.editFrame(20, 72, 220);
            ThemedEditOptions editOptions{};
            editOptions.selectAllOnFocus = true;
            initialText_ = DialogHotKeyText(currentKey_, options_);
            edit_ = ui.Edit(IdHotKeyEdit, editFrame, initialText_, editOptions);
            ui.Button(
                IdOk,
                L"确定",
                252,
                73,
                ThemedButtonRole::Primary,
                ThemedButtonSize::Normal,
                ThemedButtonWidthMode::Fixed,
                76,
                true);
            if (edit_) {
                SetWindowSubclass(edit_, HotKeyCapture::EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
                SetFocus(edit_);
                SendMessageW(edit_, EM_SETSEL, 0, static_cast<LPARAM>(-1));
            }
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IdOk) {
                Accept();
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                RequestCancel();
                return 0;
            }
            break;
        case WM_CLOSE:
            RequestCancel();
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
        return 0;
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    int currentKey_ = 0;
    HotKeyCaptureDialogOptions options_{};
    std::wstring initialText_;
    int capturedKey_ = 0;
    DWORD lastAltUpTick_ = 0;
    bool otherKeySinceAlt_ = false;
    bool accepted_ = false;
    bool done_ = false;
};
}

std::wstring FormatHotKeyText(int key) {
    if (key <= 0) {
        return L"未设置";
    }
    wchar_t name[64]{};
    UINT scan = MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC) << 16;
    if (GetKeyNameTextW(static_cast<LONG>(scan), name, static_cast<int>(std::size(name))) > 0) {
        return L"Ctrl+Alt+" + std::wstring(name);
    }
    if (key >= VK_F1 && key <= VK_F24) {
        return L"Ctrl+Alt+F" + std::to_wstring(key - VK_F1 + 1);
    }
    return L"Ctrl+Alt+" + std::to_wstring(key);
}

int ShowHotKeyCaptureDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    int currentKey,
    HotKeyCaptureDialogOptions options) {
    return HotKeyCapture::Run(owner, instance, theme, currentKey, options);
}
