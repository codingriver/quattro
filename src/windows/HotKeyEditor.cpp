#include "HotKeyEditor.h"

#include "../../resources/resource.h"

#include "ThemedControls.h"
#include "ThemedUi.h"
#include "ThemedWindowUi.h"
#include "Utilities.h"

#include <memory>
#include <windowsx.h>

namespace {
constexpr int kDialogWidth = 360;
constexpr int kDialogHeight = 150;
constexpr int IdCapture = 1001;

class HotKeyCapture {
public:
    static int Run(HWND owner, HINSTANCE instance, const Theme& theme, int currentKey) {
        HotKeyCapture dialog(owner, instance, theme, currentKey);
        return dialog.RunImpl();
    }

private:
    HotKeyCapture(HWND owner, HINSTANCE instance, const Theme& theme, int currentKey)
        : owner_(owner), instance_(instance), theme_(theme), currentKey_(currentKey) {}

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
                L"按下一个键，热键将保存为 Ctrl+Alt+该键。Backspace 清除，Esc 取消。",
                20,
                22,
                300,
                instructionOptions);
            capture_ = ui.HotKeyCapture(IdCapture, FormatHotKeyText(currentKey_), 20, 72, 220);
            SetFocus(capture_);
            return 0;
        }
        case ThemedControls::WM_HOTKEY_CAPTURED:
            if (wParam != IdCapture) {
                return 0;
            }
            if (lParam == VK_ESCAPE) {
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            if (lParam == VK_BACK) {
                capturedKey_ = 0;
                accepted_ = true;
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            capturedKey_ = static_cast<int>(lParam);
            accepted_ = true;
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        case WM_CLOSE:
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    HWND hwnd_ = nullptr;
    HWND capture_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    int currentKey_ = 0;
    int capturedKey_ = 0;
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

int ShowHotKeyCaptureDialog(HWND owner, HINSTANCE instance, const Theme& theme, int currentKey) {
    return HotKeyCapture::Run(owner, instance, theme, currentKey);
}

