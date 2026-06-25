#include "HotKeyEditor.h"

#include <windowsx.h>

namespace {
class HotKeyCapture {
public:
    static int Run(HWND owner, HINSTANCE instance, int currentKey) {
        HotKeyCapture dialog(owner, instance, currentKey);
        return dialog.RunImpl();
    }

private:
    HotKeyCapture(HWND owner, HINSTANCE instance, int currentKey)
        : owner_(owner), instance_(instance), currentKey_(currentKey) {}

    int RunImpl() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = HotKeyCapture::Proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"QuattroHotKeyCaptureDialog";
        RegisterClassExW(&wc);

        RECT ownerRect{};
        GetWindowRect(owner_, &ownerRect);
        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
            wc.lpszClassName,
            L"录入热键",
            WS_CAPTION | WS_SYSMENU | WS_POPUP,
            ownerRect.left + 110,
            ownerRect.top + 120,
            360,
            150,
            owner_,
            nullptr,
            instance_,
            this);
        if (!hwnd_) {
            return currentKey_;
        }

        EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        UpdateWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        EnableWindow(owner_, TRUE);
        SetForegroundWindow(owner_);
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
        switch (message) {
        case WM_CREATE: {
            HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            HWND label = CreateWindowExW(0, L"STATIC", L"按下一个键，热键将保存为 Ctrl+Alt+该键。Backspace 清除，Esc 取消。",
                                         WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 22, 300, 42, hwnd_, nullptr, instance_, nullptr);
            SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            HWND current = CreateWindowExW(0, L"STATIC", FormatHotKeyText(currentKey_).c_str(),
                                           WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 72, 220, 22, hwnd_, nullptr, instance_, nullptr);
            SendMessageW(current, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            SetFocus(hwnd_);
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wParam == VK_ESCAPE) {
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            if (wParam == VK_BACK) {
                capturedKey_ = 0;
                accepted_ = true;
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            if (wParam != VK_CONTROL && wParam != VK_MENU && wParam != VK_SHIFT && wParam != VK_LWIN && wParam != VK_RWIN) {
                capturedKey_ = static_cast<int>(wParam);
                accepted_ = true;
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            return 0;
        case WM_CLOSE:
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            done_ = true;
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
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

int ShowHotKeyCaptureDialog(HWND owner, HINSTANCE instance, int currentKey) {
    return HotKeyCapture::Run(owner, instance, currentKey);
}
