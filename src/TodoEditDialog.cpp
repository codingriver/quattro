#include "TodoEditDialog.h"

#include "ThemedControls.h"
#include "TodoSchedule.h"
#include "Utilities.h"

#include <algorithm>
#include <vector>
#include <windowsx.h>

namespace {
constexpr int IdTitle = 101;
constexpr int IdContent = 102;
constexpr int IdSchedule = 103;
constexpr int IdTime = 104;
constexpr int IdEnabled = 105;

std::wstring GetText(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
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

class DialogWindow {
public:
    DialogWindow(HWND owner, HINSTANCE instance, const Theme& theme, TodoItem& item, bool isNew)
        : owner_(owner), instance_(instance), theme_(theme), item_(item), draft_(item), isNew_(isNew) {}

    bool Run() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DialogWindow::WindowProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"QuattroTodoEditDialog";
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        RECT ownerRect{};
        GetWindowRect(owner_, &ownerRect);
        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
            wc.lpszClassName,
            isNew_ ? L"新增待办事项" : L"编辑待办事项",
            WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_CLIPCHILDREN,
            ownerRect.left + 58,
            ownerRect.top + 72,
            520,
            460,
            owner_,
            nullptr,
            instance_,
            this);
        if (!hwnd_) {
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
        return accepted_;
    }

private:
    struct FieldFrame {
        RECT rect{};
        HWND child = nullptr;
        bool multiLine = false;
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        DialogWindow* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<DialogWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<DialogWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    HWND Label(const wchar_t* text, int x, int y, int width = 64) {
        return ThemedControls::CreateLabelText(instance_, hwnd_, text, x, y, width, theme_, font_, SS_RIGHT);
    }

    HWND SingleEdit(int id, int x, int y, int width, const std::wstring& value) {
        const int height = ThemedControls::EditFrameHeight(theme_);
        RECT frame{x, y, x + width, y + height};
        HWND edit = ThemedControls::CreateSingleLineEdit(instance_, hwnd_, id, theme_, frame, value, editFont_ ? editFont_ : font_);
        fields_.push_back(FieldFrame{frame, edit, false});
        return edit;
    }

    HWND MultiEdit(int id, int x, int y, int width, int height, const std::wstring& value) {
        RECT frame{x, y, x + width, y + height};
        HWND edit = ThemedControls::CreateMultiLineEdit(instance_, hwnd_, id, theme_, frame, value, editFont_ ? editFont_ : font_);
        fields_.push_back(FieldFrame{frame, edit, true});
        return edit;
    }

    void FillSchedule() {
        const wchar_t* items[] = {L"无时间", L"一次性", L"每日", L"每周", L"每月", L"每年"};
        for (const wchar_t* text : items) {
            SendMessageW(scheduleCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        }
        int selected = static_cast<int>(draft_.scheduleKind);
        if (selected < 0 || selected > 5) {
            selected = 0;
        }
        SendMessageW(scheduleCombo_, CB_SETCURSEL, selected, 0);
        EnableTimeBySchedule();
    }

    void EnableTimeBySchedule() {
        const int selected = static_cast<int>(SendMessageW(scheduleCombo_, CB_GETCURSEL, 0, 0));
        EnableWindow(timeEdit_, selected > 0 ? TRUE : FALSE);
        InvalidateRect(hwnd_, &timeFrame_, TRUE);
    }

    void InvalidateField(HWND child) {
        for (const auto& frame : fields_) {
            if (frame.child == child) {
                InvalidateRect(hwnd_, &frame.rect, TRUE);
                return;
            }
        }
    }

    bool Accept() {
        draft_.title = Trim(GetText(titleEdit_));
        if (draft_.title.empty()) {
            MessageBoxW(hwnd_, L"标题不能为空。", L"待办事项", MB_OK | MB_ICONWARNING);
            SetFocus(titleEdit_);
            return false;
        }

        draft_.content = Trim(GetText(contentEdit_));
        draft_.enabled = SendMessageW(enabledCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        int selected = static_cast<int>(SendMessageW(scheduleCombo_, CB_GETCURSEL, 0, 0));
        if (selected < 0 || selected > 5) {
            selected = 0;
        }
        draft_.scheduleKind = static_cast<TodoScheduleKind>(selected);
        if (draft_.scheduleKind == TodoScheduleKind::None) {
            draft_.anchorAt.clear();
            draft_.nextDueAt.clear();
        } else {
            draft_.anchorAt = NormalizeTodoTimestamp(GetText(timeEdit_));
            if (draft_.anchorAt.empty()) {
                MessageBoxW(hwnd_, L"请输入有效时间，例如 2026-06-26 09:30。", L"待办事项", MB_OK | MB_ICONWARNING);
                SetFocus(timeEdit_);
                return false;
            }
            draft_.nextDueAt = ComputeNextTodoDueAt(draft_.scheduleKind, draft_.anchorAt, CurrentTodoTimestamp());
        }

        item_ = draft_;
        accepted_ = true;
        done_ = true;
        DestroyWindow(hwnd_);
        return true;
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE: {
            font_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            editFont_ = ThemedControls::CreateEditFont(theme_);
            backgroundBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
            editBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"edit", L"normal", L"bg")));

            Label(L"标题", 26, 26);
            titleEdit_ = SingleEdit(IdTitle, 106, 20, 364, draft_.title);

            Label(L"时间类型", 26, 76);
            scheduleCombo_ = ThemedControls::CreateComboBox(instance_, hwnd_, IdSchedule, 106, 70, 150, 180, font_, theme_);
            Label(L"时间", 274, 76, 48);
            timeFrame_ = RECT{324, 70, 470, 70 + ThemedControls::EditFrameHeight(theme_)};
            std::wstring timeText = draft_.anchorAt.empty() ? CurrentTodoTimestamp() : draft_.anchorAt;
            timeEdit_ = ThemedControls::CreateSingleLineEdit(instance_, hwnd_, IdTime, theme_, timeFrame_, timeText, editFont_ ? editFont_ : font_);
            fields_.push_back(FieldFrame{timeFrame_, timeEdit_, false});
            FillSchedule();

            enabledCheck_ = ThemedControls::CreateCheckBox(
                instance_,
                hwnd_,
                IdEnabled,
                L"启用待办事项",
                106,
                116,
                180,
                ThemedControls::CheckBoxHeight(theme_),
                font_,
                draft_.enabled);

            Label(L"内容", 26, 164);
            contentEdit_ = MultiEdit(IdContent, 106, 158, 364, 156, draft_.content);

            const int buttonHeight = ThemedControls::ButtonHeight(theme_);
            ThemedControls::CreateButton(instance_, hwnd_, IDOK, L"确定", 310, 346, 74, buttonHeight, font_, true);
            ThemedControls::CreateButton(instance_, hwnd_, IDCANCEL, L"取消", 398, 346, 74, buttonHeight, font_);
            SetFocus(titleEdit_);
            SendMessageW(titleEdit_, EM_SETSEL, 0, -1);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            RECT rect{};
            GetClientRect(hwnd_, &rect);
            FillRect(dc, &rect, backgroundBrush_ ? backgroundBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            for (const auto& frame : fields_) {
                ThemedControls::DrawFieldFrame(theme_, dc, frame.rect, frame.child, !IsWindowEnabled(frame.child));
            }
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, ToColorRef(theme_.color(L"edit", IsWindowEnabled(reinterpret_cast<HWND>(lParam)) ? L"normal" : L"disabled", L"text")));
            SetBkColor(dc, ToColorRef(theme_.color(L"edit", IsWindowEnabled(reinterpret_cast<HWND>(lParam)) ? L"normal" : L"disabled", L"bg")));
            return reinterpret_cast<LRESULT>(editBrush_ ? editBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN: {
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
        case WM_COMMAND:
            if (LOWORD(wParam) == IdSchedule && HIWORD(wParam) == CBN_SELCHANGE) {
                EnableTimeBySchedule();
                return 0;
            }
            if (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS) {
                InvalidateField(reinterpret_cast<HWND>(lParam));
            }
            if (LOWORD(wParam) == IDOK) {
                Accept();
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) {
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
            RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
            if (editFont_) {
                DeleteObject(editFont_);
            }
            if (backgroundBrush_) {
                DeleteObject(backgroundBrush_);
            }
            if (editBrush_) {
                DeleteObject(editBrush_);
            }
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND titleEdit_ = nullptr;
    HWND contentEdit_ = nullptr;
    HWND scheduleCombo_ = nullptr;
    HWND timeEdit_ = nullptr;
    HWND enabledCheck_ = nullptr;
    RECT timeFrame_{};
    HFONT font_ = nullptr;
    HFONT editFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    const Theme& theme_;
    TodoItem& item_;
    TodoItem draft_;
    bool isNew_ = false;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
    bool accepted_ = false;
    bool done_ = false;
    std::vector<FieldFrame> fields_;
};
}

bool TodoEditDialog::Show(HWND owner, HINSTANCE instance, const Theme& theme, TodoItem& item, bool isNew) {
    DialogWindow dialog(owner, instance, theme, item, isNew);
    return dialog.Run();
}
