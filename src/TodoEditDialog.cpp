#include "TodoEditDialog.h"

#include "ThemedControls.h"
#include "TodoSchedule.h"
#include "Utilities.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <vector>
#include <commctrl.h>
#include <windowsx.h>

namespace {
constexpr int IdTitle = 101;
constexpr int IdContent = 102;
constexpr int IdEnabled = 105;
constexpr int IdModeNone = 110;
constexpr int IdModeOnce = 111;
constexpr int IdModeFixedRecurring = 112;
constexpr int IdAdvancedToggle = 121;
constexpr int IdCronExpression = 122;
constexpr int IdRepeatRule = 130;
constexpr int IdUnitSecond = 140;
constexpr int IdUnitMinute = 141;
constexpr int IdUnitHour = 142;
constexpr int IdUnitDay = 143;
constexpr int IdUnitWeek = 144;
constexpr int IdUnitMonth = 145;
constexpr int IdUnitYear = 146;
constexpr int IdYear = 150;
constexpr int IdMonth = 151;
constexpr int IdDay = 152;
constexpr int IdHour = 153;
constexpr int IdMinute = 154;
constexpr int IdSecond = 155;
constexpr int IdWeekday = 156;

enum class ReminderMode {
    None,
    Once,
    Recurring,
};

enum class RepeatRule {
    None = 0,
    Hourly,
    Daily,
    Workday,
    Weekend,
    Weekly,
    Monthly,
    Yearly,
    Custom,
};

std::wstring GetText(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

int GetInt(HWND hwnd, int fallback, int minValue, int maxValue) {
    auto parsed = ParseInt(GetText(hwnd));
    int value = parsed.value_or(fallback);
    return std::max(minValue, std::min(maxValue, value));
}

std::wstring TwoDigits(int value) {
    wchar_t buffer[8]{};
    swprintf_s(buffer, L"%02d", value);
    return buffer;
}

std::wstring FourDigits(int value) {
    wchar_t buffer[8]{};
    swprintf_s(buffer, L"%04d", value);
    return buffer;
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

bool IsLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int DaysInMonth(int year, int month) {
    static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && IsLeapYear(year)) {
        return 29;
    }
    if (month < 1 || month > 12) {
        return 31;
    }
    return kDays[month - 1];
}

SYSTEMTIME FromTimeT(std::time_t value) {
    std::tm tm{};
    localtime_s(&tm, &value);
    SYSTEMTIME result{};
    result.wYear = static_cast<WORD>(tm.tm_year + 1900);
    result.wMonth = static_cast<WORD>(tm.tm_mon + 1);
    result.wDay = static_cast<WORD>(tm.tm_mday);
    result.wDayOfWeek = static_cast<WORD>(tm.tm_wday);
    result.wHour = static_cast<WORD>(tm.tm_hour);
    result.wMinute = static_cast<WORD>(tm.tm_min);
    result.wSecond = static_cast<WORD>(tm.tm_sec);
    return result;
}

std::time_t ToTimeT(const SYSTEMTIME& value) {
    std::tm tm{};
    tm.tm_year = static_cast<int>(value.wYear) - 1900;
    tm.tm_mon = static_cast<int>(value.wMonth) - 1;
    tm.tm_mday = static_cast<int>(value.wDay);
    tm.tm_hour = static_cast<int>(value.wHour);
    tm.tm_min = static_cast<int>(value.wMinute);
    tm.tm_sec = static_cast<int>(value.wSecond);
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

int DayOfWeek(SYSTEMTIME value) {
    const std::time_t t = ToTimeT(value);
    if (t == static_cast<std::time_t>(-1)) {
        return 0;
    }
    return FromTimeT(t).wDayOfWeek;
}

SYSTEMTIME AddDays(SYSTEMTIME value, int days) {
    return FromTimeT(ToTimeT(value) + static_cast<std::time_t>(days) * 24 * 60 * 60);
}

SYSTEMTIME DateForWeekday(SYSTEMTIME base, int weekday) {
    base.wHour = 0;
    base.wMinute = 0;
    base.wSecond = 0;
    const int current = DayOfWeek(base);
    const int delta = (weekday - current + 7) % 7;
    return AddDays(base, delta);
}

const wchar_t* WeekdayText(int weekday) {
    static constexpr const wchar_t* kTexts[] = {L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"};
    if (weekday < 0 || weekday > 6) {
        return kTexts[0];
    }
    return kTexts[weekday];
}

int CronWeekday(int weekday) {
    return std::max(0, std::min(6, weekday));
}

bool IsUnitButton(int id) {
    return id >= IdUnitSecond && id <= IdUnitYear;
}

TodoScheduleKind UnitFromButton(int id) {
    switch (id) {
    case IdUnitSecond:
        return TodoScheduleKind::Secondly;
    case IdUnitMinute:
        return TodoScheduleKind::Minutely;
    case IdUnitHour:
        return TodoScheduleKind::Hourly;
    case IdUnitWeek:
        return TodoScheduleKind::Weekly;
    case IdUnitMonth:
        return TodoScheduleKind::Monthly;
    case IdUnitYear:
        return TodoScheduleKind::Yearly;
    case IdUnitDay:
    default:
        return TodoScheduleKind::Daily;
    }
}

int ButtonFromUnit(TodoScheduleKind unit) {
    switch (unit) {
    case TodoScheduleKind::Secondly:
        return IdUnitSecond;
    case TodoScheduleKind::Minutely:
        return IdUnitMinute;
    case TodoScheduleKind::Hourly:
        return IdUnitHour;
    case TodoScheduleKind::Weekly:
        return IdUnitWeek;
    case TodoScheduleKind::Monthly:
        return IdUnitMonth;
    case TodoScheduleKind::Yearly:
        return IdUnitYear;
    case TodoScheduleKind::Daily:
    default:
        return IdUnitDay;
    }
}

std::wstring TimePointText(TodoScheduleKind unit, const SYSTEMTIME& value) {
    switch (unit) {
    case TodoScheduleKind::Secondly:
        return L"";
    case TodoScheduleKind::Minutely:
        return TwoDigits(value.wSecond) + L" 秒";
    case TodoScheduleKind::Hourly:
        return TwoDigits(value.wMinute) + L" 分 " + TwoDigits(value.wSecond) + L" 秒";
    case TodoScheduleKind::Daily:
        return TwoDigits(value.wHour) + L":" + TwoDigits(value.wMinute) + L":" + TwoDigits(value.wSecond);
    case TodoScheduleKind::Weekly:
        return std::wstring(WeekdayText(DayOfWeek(value))) + L" " + TwoDigits(value.wHour) + L":" +
            TwoDigits(value.wMinute) + L":" + TwoDigits(value.wSecond);
    case TodoScheduleKind::Monthly:
        return std::to_wstring(value.wDay) + L" 日 " + TwoDigits(value.wHour) + L":" +
            TwoDigits(value.wMinute) + L":" + TwoDigits(value.wSecond);
    case TodoScheduleKind::Yearly:
        return TwoDigits(value.wMonth) + L" 月 " + TwoDigits(value.wDay) + L" 日 " +
            TwoDigits(value.wHour) + L":" + TwoDigits(value.wMinute) + L":" + TwoDigits(value.wSecond);
    default:
        return FormatTodoTimestamp(value);
    }
}

const wchar_t* RepeatRuleText(RepeatRule rule) {
    switch (rule) {
    case RepeatRule::Hourly:
        return L"每小时";
    case RepeatRule::Daily:
        return L"每日";
    case RepeatRule::Workday:
        return L"每工作日";
    case RepeatRule::Weekend:
        return L"每周末";
    case RepeatRule::Weekly:
        return L"每周";
    case RepeatRule::Monthly:
        return L"每月";
    case RepeatRule::Yearly:
        return L"每年";
    case RepeatRule::Custom:
        return L"自定义规则";
    case RepeatRule::None:
    default:
        return L"不重复";
    }
}

TodoScheduleKind ScheduleKindFromRepeatRule(RepeatRule rule) {
    switch (rule) {
    case RepeatRule::Hourly:
        return TodoScheduleKind::Hourly;
    case RepeatRule::Daily:
        return TodoScheduleKind::Daily;
    case RepeatRule::Weekly:
        return TodoScheduleKind::Weekly;
    case RepeatRule::Monthly:
        return TodoScheduleKind::Monthly;
    case RepeatRule::Yearly:
        return TodoScheduleKind::Yearly;
    case RepeatRule::Workday:
    case RepeatRule::Weekend:
    case RepeatRule::Custom:
        return TodoScheduleKind::Cron;
    case RepeatRule::None:
    default:
        return TodoScheduleKind::Once;
    }
}

RepeatRule RepeatRuleFromScheduleKind(TodoScheduleKind kind) {
    switch (kind) {
    case TodoScheduleKind::Hourly:
        return RepeatRule::Hourly;
    case TodoScheduleKind::Daily:
        return RepeatRule::Daily;
    case TodoScheduleKind::Weekly:
        return RepeatRule::Weekly;
    case TodoScheduleKind::Monthly:
        return RepeatRule::Monthly;
    case TodoScheduleKind::Yearly:
        return RepeatRule::Yearly;
    case TodoScheduleKind::Cron:
        return RepeatRule::Custom;
    case TodoScheduleKind::Once:
    default:
        return RepeatRule::None;
    }
}

std::wstring CronExpressionForRepeatRule(RepeatRule rule, const SYSTEMTIME& anchor) {
    const std::wstring base = std::to_wstring(anchor.wSecond) + L" " + std::to_wstring(anchor.wMinute) + L" " +
        std::to_wstring(anchor.wHour);
    if (rule == RepeatRule::Workday) {
        return base + L" * * 1-5";
    }
    if (rule == RepeatRule::Weekend) {
        return base + L" * * 0,6";
    }
    return {};
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
            382,
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

    struct TimePart {
        HWND edit = nullptr;
        HWND label = nullptr;
        int width = 42;
    };

    void AttachTooltip(HWND hwnd, const wchar_t* text) {
        if (!tooltip_ || !hwnd) {
            return;
        }
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
        tool.hwnd = hwnd_;
        tool.uId = reinterpret_cast<UINT_PTR>(hwnd);
        tool.lpszText = const_cast<LPWSTR>(text);
        SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }

    void EnsureTooltip() {
        if (tooltip_) {
            return;
        }
        tooltip_ = CreateWindowExW(
            WS_EX_TOPMOST,
            TOOLTIPS_CLASSW,
            nullptr,
            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            hwnd_,
            nullptr,
            instance_,
            nullptr);
        if (tooltip_) {
            SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 360);
        }
    }

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

    HWND StaticText(const wchar_t* text, int x, int y, int width, int height = 20) {
        return ThemedControls::CreateStaticText(instance_, hwnd_, text, x, y, width, height, font_, SS_LEFT | SS_CENTERIMAGE);
    }

    HWND SingleEdit(int id, int x, int y, int width, const std::wstring& value, DWORD extraStyle = ES_AUTOHSCROLL) {
        const int height = ThemedControls::EditFrameHeight(theme_);
        RECT frame{x, y, x + width, y + height};
        HWND edit = ThemedControls::CreateSingleLineEdit(instance_, hwnd_, id, theme_, frame, value, editFont_ ? editFont_ : font_, extraStyle);
        fields_.push_back(FieldFrame{frame, edit, false});
        return edit;
    }

    HWND MultiEdit(int id, int x, int y, int width, int height, const std::wstring& value) {
        RECT frame{x, y, x + width, y + height};
        HWND edit = ThemedControls::CreateMultiLineEdit(instance_, hwnd_, id, theme_, frame, value, editFont_ ? editFont_ : font_);
        fields_.push_back(FieldFrame{frame, edit, true});
        return edit;
    }

    void SetFrame(HWND child, RECT frame) {
        for (auto& item : fields_) {
            if (item.child == child) {
                item.rect = frame;
                const RECT editRect = item.multiLine ? ThemedControls::MultiLineEditRect(theme_, frame) : ThemedControls::SingleLineEditRect(theme_, frame);
                MoveWindow(child, editRect.left, editRect.top, editRect.right - editRect.left, editRect.bottom - editRect.top, TRUE);
                return;
            }
        }
    }

    void SetVisible(HWND hwnd, bool visible) {
        if (hwnd) {
            ShowWindow(hwnd, visible ? SW_SHOW : SW_HIDE);
        }
    }

    void SetEnabled(HWND hwnd, bool enabled) {
        if (hwnd) {
            EnableWindow(hwnd, enabled ? TRUE : FALSE);
        }
    }

    void SetTabChecked(HWND hwnd, bool checked) {
        if (hwnd) {
            SendMessageW(hwnd, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
        }
    }

    void SetWindowTextIf(HWND hwnd, const wchar_t* text) {
        if (hwnd) {
            SetWindowTextW(hwnd, text);
        }
    }

    void SetWindowTextIf(HWND hwnd, const std::wstring& text) {
        if (hwnd) {
            SetWindowTextW(hwnd, text.c_str());
        }
    }

    ReminderMode InitialMode() const {
        if (draft_.scheduleKind == TodoScheduleKind::Once) {
            return ReminderMode::Once;
        }
        if (IsRecurringTodoSchedule(draft_.scheduleKind)) {
            return ReminderMode::Recurring;
        }
        return ReminderMode::None;
    }

    TodoScheduleKind InitialUnit() const {
        if (draft_.scheduleKind == TodoScheduleKind::Cron) {
            return TodoScheduleKind::Daily;
        }
        return IsRecurringTodoSchedule(draft_.scheduleKind) ? draft_.scheduleKind : TodoScheduleKind::Daily;
    }

    RepeatRule InitialRepeatRule() const {
        if (draft_.scheduleKind == TodoScheduleKind::None || draft_.scheduleKind == TodoScheduleKind::Once) {
            return RepeatRule::None;
        }
        return RepeatRuleFromScheduleKind(draft_.scheduleKind);
    }

    void FillRepeatRules() {
        SendMessageW(repeatCombo_, CB_RESETCONTENT, 0, 0);
        const RepeatRule rules[] = {
            RepeatRule::None,
            RepeatRule::Hourly,
            RepeatRule::Daily,
            RepeatRule::Workday,
            RepeatRule::Weekend,
            RepeatRule::Weekly,
            RepeatRule::Monthly,
            RepeatRule::Yearly,
            RepeatRule::Custom,
        };
        for (RepeatRule rule : rules) {
            SendMessageW(repeatCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(RepeatRuleText(rule)));
        }
    }

    RepeatRule SelectedRepeatRule() const {
        const int selected = static_cast<int>(SendMessageW(repeatCombo_, CB_GETCURSEL, 0, 0));
        if (selected < static_cast<int>(RepeatRule::None) || selected > static_cast<int>(RepeatRule::Custom)) {
            return RepeatRule::None;
        }
        return static_cast<RepeatRule>(selected);
    }

    void FillWeekdays() {
        for (int i = 0; i < 7; ++i) {
            SendMessageW(weekdayCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(WeekdayText(i)));
        }
    }

    void SetInputText(HWND edit, const std::wstring& text) {
        if (edit) {
            SetWindowTextW(edit, text.c_str());
        }
    }

    void LoadTimeFields() {
        SYSTEMTIME value{};
        if (draft_.anchorAt.empty() || !TryParseTodoTimestamp(draft_.anchorAt, value)) {
            GetLocalTime(&value);
        }
        SetInputText(yearEdit_, FourDigits(value.wYear));
        SetInputText(monthEdit_, TwoDigits(value.wMonth));
        SetInputText(dayEdit_, TwoDigits(value.wDay));
        SetInputText(hourEdit_, TwoDigits(value.wHour));
        SetInputText(minuteEdit_, TwoDigits(value.wMinute));
        SetInputText(secondEdit_, TwoDigits(value.wSecond));
        SendMessageW(weekdayCombo_, CB_SETCURSEL, DayOfWeek(value), 0);
    }

    void SelectMode(ReminderMode mode) {
        mode_ = mode;
        SetTabChecked(modeNone_, mode == ReminderMode::None);
        SetTabChecked(modeOnce_, mode != ReminderMode::None);
        SetTabChecked(modeFixedRecurring_, false);
        UpdateState();
    }

    void SelectReminderEnabled(bool enabled) {
        mode_ = enabled ? (SelectedRepeatRule() == RepeatRule::None ? ReminderMode::Once : ReminderMode::Recurring) : ReminderMode::None;
        SetTabChecked(modeNone_, !enabled);
        SetTabChecked(modeOnce_, enabled);
        SetTabChecked(modeFixedRecurring_, false);
        UpdateState();
    }

    void SelectRepeatRule(RepeatRule rule) {
        SendMessageW(repeatCombo_, CB_SETCURSEL, static_cast<WPARAM>(rule), 0);
        if (mode_ != ReminderMode::None) {
            mode_ = rule == RepeatRule::None ? ReminderMode::Once : ReminderMode::Recurring;
        }
        UpdateState();
    }

    void SelectRecurringMode() {
        mode_ = ReminderMode::Recurring;
        SetTabChecked(modeNone_, false);
        SetTabChecked(modeOnce_, false);
        SetTabChecked(modeFixedRecurring_, true);
        SelectUnit(unit_);
        UpdateState();
    }

    std::wstring GeneratedCronExpression() const {
        if (item_.scheduleKind == TodoScheduleKind::Cron && !scheduleEdited_) {
            return item_.cronExpression;
        }
        if (mode_ != ReminderMode::Recurring) {
            return {};
        }
        const SYSTEMTIME anchor = ReadAnchorTime();
        switch (unit_) {
        case TodoScheduleKind::Secondly:
            return L"* * * * * *";
        case TodoScheduleKind::Minutely:
            return std::to_wstring(anchor.wSecond) + L" * * * * *";
        case TodoScheduleKind::Hourly:
            return std::to_wstring(anchor.wSecond) + L" " + std::to_wstring(anchor.wMinute) + L" * * * *";
        case TodoScheduleKind::Daily:
            return std::to_wstring(anchor.wSecond) + L" " + std::to_wstring(anchor.wMinute) + L" " +
                std::to_wstring(anchor.wHour) + L" * * *";
        case TodoScheduleKind::Weekly:
            return std::to_wstring(anchor.wSecond) + L" " + std::to_wstring(anchor.wMinute) + L" " +
                std::to_wstring(anchor.wHour) + L" * * " + std::to_wstring(CronWeekday(DayOfWeek(anchor)));
        case TodoScheduleKind::Monthly:
            return std::to_wstring(anchor.wSecond) + L" " + std::to_wstring(anchor.wMinute) + L" " +
                std::to_wstring(anchor.wHour) + L" " + std::to_wstring(anchor.wDay) + L" * *";
        case TodoScheduleKind::Yearly:
            return std::to_wstring(anchor.wSecond) + L" " + std::to_wstring(anchor.wMinute) + L" " +
                std::to_wstring(anchor.wHour) + L" " + std::to_wstring(anchor.wDay) + L" " +
                std::to_wstring(anchor.wMonth) + L" *";
        default:
            return {};
        }
    }

    void SyncCronExpression() {
        if (!cronEdit_ || cronEdited_ || updatingCronEdit_) {
            return;
        }
        updatingCronEdit_ = true;
        SetWindowTextIf(cronEdit_, NormalizeTodoCronExpression(GeneratedCronExpression()));
        updatingCronEdit_ = false;
    }

    void SelectUnit(TodoScheduleKind unit) {
        unit_ = unit;
        for (HWND button : unitButtons_) {
            SetTabChecked(button, false);
        }
        const int id = ButtonFromUnit(unit);
        for (HWND button : unitButtons_) {
            if (button && GetDlgCtrlID(button) == id) {
                SetTabChecked(button, true);
                break;
            }
        }
        UpdateState();
    }

    void ShowTimePart(TimePart part, int& x, int y, bool enabled) {
        const int labelWidth = 18;
        const int frameHeight = ThemedControls::EditFrameHeight(theme_);
        RECT frame{x, y, x + part.width, y + frameHeight};
        SetFrame(part.edit, frame);
        MoveWindow(part.label, frame.right + 2, y + 5, labelWidth, 20, TRUE);
        SetVisible(part.edit, true);
        SetVisible(part.label, true);
        SetEnabled(part.edit, enabled);
        x += part.width + labelWidth + 6;
    }

    void HideTimePart(TimePart part) {
        SetVisible(part.edit, false);
        SetVisible(part.label, false);
    }

    void LayoutTimeInputs() {
        const bool hasTime = mode_ != ReminderMode::None;
        const int y = timeY_;
        int x = 106;

        HideTimePart(yearPart_);
        HideTimePart(monthPart_);
        HideTimePart(dayPart_);
        HideTimePart(hourPart_);
        HideTimePart(minutePart_);
        HideTimePart(secondPart_);
        SetVisible(weekdayCombo_, false);

        if (!hasTime) {
            return;
        }
        ShowTimePart(yearPart_, x, y, true);
        ShowTimePart(monthPart_, x, y, true);
        ShowTimePart(dayPart_, x, y, true);
        ShowTimePart(hourPart_, x, y, true);
        ShowTimePart(minutePart_, x, y, true);
        ShowTimePart(secondPart_, x, y, true);
    }

    void UpdateState() {
        const bool hasTime = mode_ != ReminderMode::None;
        timeY_ = 98;
        const int repeatY = 140;
        const int previewY = hasTime ? 178 : 100;
        const int contentY = hasTime ? 214 : 126;
        const int contentHeight = hasTime ? 78 : 118;
        const int buttonHeight = ThemedControls::ButtonHeight(theme_);
        const int windowHeight = contentY + contentHeight + 18 + buttonHeight + 22;
        SetWindowPos(hwnd_, nullptr, 0, 0, 520, std::max(382, windowHeight), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

        SetVisible(loopLabel_, hasTime);
        SetVisible(repeatCombo_, hasTime);
        SetEnabled(repeatCombo_, hasTime);
        for (HWND button : unitButtons_) {
            SetVisible(button, false);
        }
        SetVisible(advancedButton_, false);
        SetVisible(cronLabel_, false);
        SetVisible(cronEdit_, false);
        SetVisible(cronHint_, false);

        SetVisible(timeLabel_, hasTime);
        SetWindowTextIf(timeLabel_, L"时间");
        MoveWindow(timeLabel_, 24, timeY_ + 8, 64, ThemedControls::LabelHeight(theme_), TRUE);
        MoveWindow(loopLabel_, 24, repeatY + 6, 64, ThemedControls::LabelHeight(theme_), TRUE);
        MoveWindow(repeatCombo_, 106, repeatY, 160, 180, TRUE);
        MoveWindow(previewText_, 106, previewY, 370, ThemedControls::LabelHeight(theme_), TRUE);
        MoveWindow(contentLabel_, 24, contentY + 6, 64, ThemedControls::LabelHeight(theme_), TRUE);
        SetFrame(contentEdit_, RECT{106, contentY, 476, contentY + contentHeight});
        const int buttonY = contentY + contentHeight + 18;
        MoveWindow(okButton_, 318, buttonY, 74, buttonHeight, TRUE);
        MoveWindow(cancelButton_, 404, buttonY, 74, buttonHeight, TRUE);
        LayoutTimeInputs();
        UpdatePreview();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    SYSTEMTIME ReadAnchorTime() const {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        SYSTEMTIME value = now;

        value.wYear = static_cast<WORD>(GetInt(yearEdit_, now.wYear, 1900, 9999));
        value.wMonth = static_cast<WORD>(GetInt(monthEdit_, now.wMonth, 1, 12));
        value.wDay = static_cast<WORD>(GetInt(dayEdit_, now.wDay, 1, DaysInMonth(value.wYear, value.wMonth)));
        value.wHour = static_cast<WORD>(GetInt(hourEdit_, now.wHour, 0, 23));
        value.wMinute = static_cast<WORD>(GetInt(minuteEdit_, now.wMinute, 0, 59));
        value.wSecond = static_cast<WORD>(GetInt(secondEdit_, now.wSecond, 0, 59));
        value.wMilliseconds = 0;
        return value;
    }

    std::wstring PreviewText() const {
        if (mode_ == ReminderMode::None) {
            return L"无提醒";
        }
        const RepeatRule rule = SelectedRepeatRule();
        if (rule == RepeatRule::Custom && item_.scheduleKind == TodoScheduleKind::Cron && !scheduleEdited_) {
            const std::wstring nextDueAt = ComputeNextTodoDueAt(item_, CurrentTodoTimestamp());
            return L"自定义重复规则" + (nextDueAt.empty() ? L"" : L"，下次 " + nextDueAt);
        }
        const SYSTEMTIME anchor = ReadAnchorTime();
        const std::wstring anchorText = FormatTodoTimestamp(anchor);
        if (rule == RepeatRule::None) {
            return L"提醒：" + anchorText;
        }

        TodoItem preview{};
        preview.enabled = true;
        preview.anchorAt = anchorText;
        preview.repeatMode = TodoRepeatMode::FixedPoint;
        preview.repeatInterval = 1;
        preview.scheduleKind = ScheduleKindFromRepeatRule(rule);
        if (rule == RepeatRule::Workday || rule == RepeatRule::Weekend) {
            preview.cronExpression = CronExpressionForRepeatRule(rule, anchor);
        }
        const std::wstring nextDueAt = ComputeNextTodoDueAt(preview, CurrentTodoTimestamp());
        return std::wstring(RepeatRuleText(rule)) + L"提醒" + (nextDueAt.empty() ? L"" : L"，下次 " + nextDueAt);
    }

    void UpdatePreview() {
        if (previewText_) {
            SetWindowTextW(previewText_, PreviewText().c_str());
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

        const TodoScheduleKind oldKind = draft_.scheduleKind;
        const TodoRepeatMode oldRepeatMode = draft_.repeatMode;
        const int oldInterval = draft_.repeatInterval;
        const int oldLimit = draft_.repeatLimit;
        const std::wstring oldAnchor = draft_.anchorAt;
        const std::wstring oldCronExpression = draft_.cronExpression;
        const RepeatRule rule = SelectedRepeatRule();

        if (mode_ == ReminderMode::None) {
            draft_.scheduleKind = TodoScheduleKind::None;
            draft_.repeatMode = TodoRepeatMode::FixedPoint;
            draft_.cronExpression.clear();
            draft_.anchorAt.clear();
            draft_.nextDueAt.clear();
            draft_.repeatInterval = 1;
            draft_.repeatLimit = 0;
            draft_.repeatFinished = 0;
        } else if (rule == RepeatRule::Custom && item_.scheduleKind == TodoScheduleKind::Cron && !scheduleEdited_) {
            draft_.scheduleKind = item_.scheduleKind;
            draft_.repeatMode = item_.repeatMode;
            draft_.repeatInterval = item_.repeatInterval;
            draft_.repeatLimit = item_.repeatLimit;
            draft_.repeatFinished = item_.repeatFinished;
            draft_.cronExpression = item_.cronExpression;
            draft_.anchorAt = item_.anchorAt;
            draft_.nextDueAt = ComputeNextTodoDueAt(draft_, CurrentTodoTimestamp());
        } else {
            const SYSTEMTIME anchor = ReadAnchorTime();
            draft_.scheduleKind = ScheduleKindFromRepeatRule(rule);
            draft_.repeatMode = TodoRepeatMode::FixedPoint;
            draft_.repeatInterval = 1;
            draft_.repeatLimit = 0;
            draft_.anchorAt = FormatTodoTimestamp(anchor);
            draft_.cronExpression = (rule == RepeatRule::Workday || rule == RepeatRule::Weekend) ? CronExpressionForRepeatRule(rule, anchor) : L"";
            if (draft_.scheduleKind == TodoScheduleKind::Cron && !IsValidTodoCronExpression(draft_.cronExpression)) {
                MessageBoxW(hwnd_, L"重复规则无效，请重新选择。", L"待办事项", MB_OK | MB_ICONWARNING);
                return false;
            }
            if (draft_.scheduleKind != oldKind || draft_.repeatMode != oldRepeatMode || draft_.repeatInterval != oldInterval ||
                draft_.repeatLimit != oldLimit || draft_.anchorAt != oldAnchor || draft_.cronExpression != oldCronExpression) {
                draft_.repeatFinished = 0;
            }
            draft_.nextDueAt = ComputeNextTodoDueAt(draft_, CurrentTodoTimestamp());
            if (draft_.nextDueAt.empty()) {
                MessageBoxW(hwnd_, L"请输入有效时间。", L"待办事项", MB_OK | MB_ICONWARNING);
                return false;
            }
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
            EnsureTooltip();

            mode_ = InitialMode();
            unit_ = InitialUnit();
            repeatRule_ = InitialRepeatRule();

            Label(L"标题", 24, 22);
            titleEdit_ = SingleEdit(IdTitle, 106, 16, 292, draft_.title);
            enabledCheck_ = ThemedControls::CreateCheckBox(instance_, hwnd_, IdEnabled, L"启用", 414, 20, 66, ThemedControls::CheckBoxHeight(theme_), font_, draft_.enabled);

            Label(L"提醒", 24, 64);
            const int tabHeight = ThemedControls::TabButtonHeight(theme_);
            modeNone_ = ThemedControls::CreateTabButton(instance_, hwnd_, IdModeNone, L"无提醒", 106, 58, 68, tabHeight, font_, false);
            modeOnce_ = ThemedControls::CreateTabButton(instance_, hwnd_, IdModeOnce, L"提醒", 178, 58, 52, tabHeight, font_, false);
            modeFixedRecurring_ = ThemedControls::CreateTabButton(instance_, hwnd_, IdModeFixedRecurring, L"循环", 234, 58, 52, tabHeight, font_, false);
            SetVisible(modeFixedRecurring_, false);

            loopLabel_ = Label(L"重复规则", 12, 146, 76);
            repeatCombo_ = ThemedControls::CreateComboBox(instance_, hwnd_, IdRepeatRule, 106, 140, 160, 180, font_, theme_);
            FillRepeatRules();
            SendMessageW(repeatCombo_, CB_SETCURSEL, static_cast<WPARAM>(repeatRule_), 0);
            unitButtons_ = {
                ThemedControls::CreateTabButton(instance_, hwnd_, IdUnitSecond, L"秒", 106, 132, 38, tabHeight, font_, false),
                ThemedControls::CreateTabButton(instance_, hwnd_, IdUnitMinute, L"分", 145, 132, 38, tabHeight, font_, false),
                ThemedControls::CreateTabButton(instance_, hwnd_, IdUnitHour, L"时", 184, 132, 38, tabHeight, font_, false),
                ThemedControls::CreateTabButton(instance_, hwnd_, IdUnitDay, L"日", 223, 132, 38, tabHeight, font_, false),
                ThemedControls::CreateTabButton(instance_, hwnd_, IdUnitWeek, L"周", 262, 132, 38, tabHeight, font_, false),
                ThemedControls::CreateTabButton(instance_, hwnd_, IdUnitMonth, L"月", 301, 132, 38, tabHeight, font_, false),
                ThemedControls::CreateTabButton(instance_, hwnd_, IdUnitYear, L"年", 340, 132, 38, tabHeight, font_, false),
            };
            timeLabel_ = Label(L"时间", 24, 180);
            yearEdit_ = SingleEdit(IdYear, 106, 174, 52, L"", ES_NUMBER);
            yearLabel_ = StaticText(L"年", 162, 179, 20);
            monthEdit_ = SingleEdit(IdMonth, 106, 174, 42, L"", ES_NUMBER);
            monthLabel_ = StaticText(L"月", 152, 179, 20);
            dayEdit_ = SingleEdit(IdDay, 106, 174, 42, L"", ES_NUMBER);
            dayLabel_ = StaticText(L"日", 152, 179, 20);
            hourEdit_ = SingleEdit(IdHour, 106, 174, 42, L"", ES_NUMBER);
            hourLabel_ = StaticText(L"时", 152, 179, 20);
            minuteEdit_ = SingleEdit(IdMinute, 106, 174, 42, L"", ES_NUMBER);
            minuteLabel_ = StaticText(L"分", 152, 179, 20);
            secondEdit_ = SingleEdit(IdSecond, 106, 174, 42, L"", ES_NUMBER);
            secondLabel_ = StaticText(L"秒", 152, 179, 20);
            weekdayCombo_ = ThemedControls::CreateComboBox(instance_, hwnd_, IdWeekday, 106, 174, 72, 180, font_, theme_);
            FillWeekdays();
            yearPart_ = TimePart{yearEdit_, yearLabel_, 50};
            monthPart_ = TimePart{monthEdit_, monthLabel_, 34};
            dayPart_ = TimePart{dayEdit_, dayLabel_, 34};
            hourPart_ = TimePart{hourEdit_, hourLabel_, 34};
            minutePart_ = TimePart{minuteEdit_, minuteLabel_, 34};
            secondPart_ = TimePart{secondEdit_, secondLabel_, 34};
            LoadTimeFields();

            previewText_ = ThemedControls::CreateLabelText(instance_, hwnd_, L"", 106, 206, 370, theme_, font_, SS_LEFT);
            const int buttonHeight = ThemedControls::ButtonHeight(theme_);
            advancedExpanded_ = false;
            cronEdited_ = false;
            advancedButton_ = ThemedControls::CreateButton(instance_, hwnd_, IdAdvancedToggle, L"高级选项", 106, 204, 92, buttonHeight, font_);
            AttachTooltip(advancedButton_, L"展开后可查看或手动编辑实际保存的 Crontab 表达式。");
            cronLabel_ = Label(L"Cron", 24, 248);
            cronEdit_ = SingleEdit(IdCronExpression, 106, 242, 264, draft_.cronExpression);
            cronHint_ = StaticText(L"与上方时间同步；手动修改后保存时优先使用此表达式", 106, 274, 370, 20);
            AttachTooltip(cronEdit_, L"六段 Crontab：秒 分 时 日 月 周。修改后会作为自定义规则保存。");

            contentLabel_ = Label(L"内容", 24, 240);
            contentEdit_ = MultiEdit(IdContent, 106, 234, 370, 72, draft_.content);

            okButton_ = ThemedControls::CreateButton(instance_, hwnd_, IDOK, L"确定", 318, 324, 74, buttonHeight, font_, true);
            cancelButton_ = ThemedControls::CreateButton(instance_, hwnd_, IDCANCEL, L"取消", 404, 324, 74, buttonHeight, font_);

            SelectMode(mode_);
            SelectRepeatRule(repeatRule_);
            initialized_ = true;
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
                if (frame.child && IsWindowVisible(frame.child)) {
                    ThemedControls::DrawFieldFrame(theme_, dc, frame.rect, frame.child, !IsWindowEnabled(frame.child));
                }
            }
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND child = reinterpret_cast<HWND>(lParam);
            SetTextColor(dc, ToColorRef(theme_.color(L"edit", IsWindowEnabled(child) ? L"normal" : L"disabled", L"text")));
            SetBkColor(dc, ToColorRef(theme_.color(L"edit", IsWindowEnabled(child) ? L"normal" : L"disabled", L"bg")));
            return reinterpret_cast<LRESULT>(editBrush_ ? editBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, ToColorRef(theme_.color(L"label", IsWindowEnabled(reinterpret_cast<HWND>(lParam)) ? L"normal" : L"disabled", L"text")));
            return reinterpret_cast<LRESULT>(backgroundBrush_ ? backgroundBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_DRAWITEM:
            if (ThemedControls::Draw(theme_, reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
                return TRUE;
            }
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == IdModeNone) {
                scheduleEdited_ = initialized_;
                SelectMode(ReminderMode::None);
                return 0;
            }
            if (id == IdModeOnce) {
                scheduleEdited_ = initialized_;
                SelectReminderEnabled(true);
                return 0;
            }
            if (id == IdModeFixedRecurring) {
                scheduleEdited_ = initialized_;
                SelectReminderEnabled(true);
                return 0;
            }
            if (id == IdRepeatRule && HIWORD(wParam) == CBN_SELCHANGE) {
                scheduleEdited_ = initialized_;
                repeatRule_ = SelectedRepeatRule();
                SelectRepeatRule(repeatRule_);
                return 0;
            }
            if (HIWORD(wParam) == EN_CHANGE) {
                if (id >= IdYear && id <= IdSecond) {
                    scheduleEdited_ = initialized_;
                }
                UpdatePreview();
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
        }
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
            if (tooltip_) {
                DestroyWindow(tooltip_);
                tooltip_ = nullptr;
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
    HWND enabledCheck_ = nullptr;
    HWND modeNone_ = nullptr;
    HWND modeOnce_ = nullptr;
    HWND modeFixedRecurring_ = nullptr;
    HWND loopLabel_ = nullptr;
    HWND repeatCombo_ = nullptr;
    HWND advancedButton_ = nullptr;
    HWND cronLabel_ = nullptr;
    HWND cronEdit_ = nullptr;
    HWND cronHint_ = nullptr;
    std::array<HWND, 7> unitButtons_{};
    HWND yearEdit_ = nullptr;
    HWND yearLabel_ = nullptr;
    HWND monthEdit_ = nullptr;
    HWND monthLabel_ = nullptr;
    HWND dayEdit_ = nullptr;
    HWND dayLabel_ = nullptr;
    HWND hourEdit_ = nullptr;
    HWND hourLabel_ = nullptr;
    HWND minuteEdit_ = nullptr;
    HWND minuteLabel_ = nullptr;
    HWND secondEdit_ = nullptr;
    HWND secondLabel_ = nullptr;
    HWND weekdayCombo_ = nullptr;
    HWND timeLabel_ = nullptr;
    HWND previewText_ = nullptr;
    HWND contentLabel_ = nullptr;
    HWND okButton_ = nullptr;
    HWND cancelButton_ = nullptr;
    TimePart yearPart_{};
    TimePart monthPart_{};
    TimePart dayPart_{};
    TimePart hourPart_{};
    TimePart minutePart_{};
    TimePart secondPart_{};
    HFONT font_ = nullptr;
    HFONT editFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    const Theme& theme_;
    TodoItem& item_;
    TodoItem draft_;
    ReminderMode mode_ = ReminderMode::None;
    TodoScheduleKind unit_ = TodoScheduleKind::Daily;
    RepeatRule repeatRule_ = RepeatRule::None;
    int timeY_ = 148;
    HWND tooltip_ = nullptr;
    bool isNew_ = false;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
    bool initialized_ = false;
    bool scheduleEdited_ = false;
    bool advancedExpanded_ = false;
    bool cronEdited_ = false;
    bool updatingCronEdit_ = false;
    bool accepted_ = false;
    bool done_ = false;
    std::vector<FieldFrame> fields_;
};
}

bool TodoEditDialog::Show(HWND owner, HINSTANCE instance, const Theme& theme, TodoItem& item, bool isNew) {
    DialogWindow dialog(owner, instance, theme, item, isNew);
    return dialog.Run();
}
