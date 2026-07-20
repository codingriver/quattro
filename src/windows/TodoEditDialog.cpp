#include "TodoEditDialog.h"

#include "../../resources/resource.h"

#include "SimpleDialogs.h"
#include "ThemedControls.h"
#include "ThemedUi.h"
#include "ThemedWindowUi.h"
#include "TodoSchedule.h"
#include "Utilities.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <commctrl.h>
#include <cwctype>
#include <optional>
#include <memory>
#include <string>
#include <vector>
#include <windowsx.h>

namespace {
#define MessageBoxW(owner, message, title, flags) ShowThemedMessageBox((owner), instance_, theme_, (message), (title), (flags))

constexpr int IdTitle = 101;
constexpr int IdContent = 102;
constexpr int IdTime = 150;
constexpr int IdRepeatNone = 160;
constexpr int IdRepeatDaily = 161;
constexpr int IdRepeatWorkday = 162;
constexpr int IdRepeatWeekly = 163;
constexpr int IdRepeatMonthly = 164;
constexpr int IdRepeatCustom = 165;
constexpr int IdWeekdayBase = 180;
constexpr int IdMonthlyFixed = 200;
constexpr int IdMonthlyDay = 202;
constexpr int IdCustomInterval = 220;
constexpr int IdCustomUnit = 221;
constexpr int IdAdvancedToggle = 240;
constexpr int IdEndNever = 250;
constexpr int IdEndCount = 252;
constexpr int IdEndCountValue = 253;

constexpr int kDialogWidth = 560;
constexpr int kDialogHeight = 640;
constexpr int kCalendarWidth = 224;
constexpr int kCalendarMonthCellWidth = 64;
constexpr int kCalendarYearCellWidth = 80;

enum class RepeatRule {
    None,
    Daily,
    Workday,
    Weekly,
    Monthly,
    Custom,
};

enum class EndMode {
    Never,
    Count,
};

enum class CalendarPickerMode {
    Day,
    Month,
    Year,
};

struct FieldFrame {
    RECT rect{};
    HWND child = nullptr;
    bool multiLine = false;
    bool error = false;
};

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

int GetInt(HWND hwnd, int fallback) {
    auto parsed = ParseInt(GetText(hwnd));
    return parsed.value_or(fallback);
}

std::wstring TwoDigits(int value) {
    wchar_t buffer[8]{};
    swprintf_s(buffer, L"%02d", value);
    return buffer;
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

SYSTEMTIME AddDays(SYSTEMTIME value, int days) {
    return FromTimeT(ToTimeT(value) + static_cast<std::time_t>(days) * 24 * 60 * 60);
}

SYSTEMTIME AddMonths(SYSTEMTIME value, int months) {
    int year = static_cast<int>(value.wYear);
    int month = static_cast<int>(value.wMonth) + months;
    while (month > 12) {
        month -= 12;
        ++year;
    }
    while (month < 1) {
        month += 12;
        --year;
    }
    value.wYear = static_cast<WORD>(year);
    value.wMonth = static_cast<WORD>(month);
    value.wDay = static_cast<WORD>(std::min<int>(value.wDay, DaysInMonth(year, month)));
    return value;
}

SYSTEMTIME DateOnly(SYSTEMTIME value) {
    value.wHour = 0;
    value.wMinute = 0;
    value.wSecond = 0;
    value.wMilliseconds = 0;
    return value;
}

int DayOfWeek(SYSTEMTIME value) {
    return FromTimeT(ToTimeT(value)).wDayOfWeek;
}

SYSTEMTIME DateForWeekday(SYSTEMTIME base, int weekday) {
    base.wHour = 0;
    base.wMinute = 0;
    base.wSecond = 0;
    const int current = DayOfWeek(base);
    const int delta = (weekday - current + 7) % 7;
    return AddDays(base, delta);
}

const wchar_t* ShortWeekdayText(int weekday) {
    static constexpr const wchar_t* kTexts[] = {L"日", L"一", L"二", L"三", L"四", L"五", L"六"};
    return (weekday >= 0 && weekday <= 6) ? kTexts[weekday] : kTexts[0];
}

const wchar_t* LongWeekdayText(int weekday) {
    static constexpr const wchar_t* kTexts[] = {L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"};
    return (weekday >= 0 && weekday <= 6) ? kTexts[weekday] : kTexts[0];
}

std::wstring CronBase(const SYSTEMTIME& anchor) {
    return std::to_wstring(anchor.wSecond) + L" " + std::to_wstring(anchor.wMinute) + L" " + std::to_wstring(anchor.wHour);
}

std::wstring CronWeekdays(const std::array<bool, 7>& weekdays) {
    std::wstring result;
    for (int i = 0; i < 7; ++i) {
        if (!weekdays[i]) {
            continue;
        }
        if (!result.empty()) {
            result += L",";
        }
        result += std::to_wstring(i);
    }
    return result;
}

bool SameTodoTime(const std::wstring& left, const std::wstring& right) {
    SYSTEMTIME l{};
    SYSTEMTIME r{};
    return TryParseTodoTimestamp(left, l) && TryParseTodoTimestamp(right, r) && ToTimeT(l) == ToTimeT(r);
}

bool TryParseHourMinute(const std::wstring& text, int& hour, int& minute) {
    int parsedHour = 0;
    int parsedMinute = 0;
    if (swscanf_s(Trim(text).c_str(), L"%d:%d", &parsedHour, &parsedMinute) != 2) {
        return false;
    }
    if (parsedHour < 0 || parsedHour > 23 || parsedMinute < 0 || parsedMinute > 59) {
        return false;
    }
    hour = parsedHour;
    minute = parsedMinute;
    return true;
}

std::vector<std::wstring> CronFields(const std::wstring& expression) {
    std::vector<std::wstring> fields;
    const std::wstring normalized = NormalizeTodoCronExpression(expression);
    std::size_t start = 0;
    while (start < normalized.size()) {
        while (start < normalized.size() && std::iswspace(normalized[start]) != 0) {
            ++start;
        }
        if (start >= normalized.size()) {
            break;
        }
        const std::size_t end = normalized.find(L' ', start);
        fields.push_back(normalized.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }
    return fields;
}

std::optional<int> ParseCronNumberField(const std::vector<std::wstring>& fields, std::size_t index, int minValue, int maxValue) {
    if (index >= fields.size()) {
        return std::nullopt;
    }
    const auto value = ParseInt(fields[index]);
    if (!value || *value < minValue || *value > maxValue) {
        return std::nullopt;
    }
    return value;
}

bool CronFieldIsAny(const std::vector<std::wstring>& fields, std::size_t index) {
    return index < fields.size() && fields[index] == L"*";
}

bool CronFieldIsWorkday(const std::vector<std::wstring>& fields, std::size_t index) {
    return index < fields.size() && fields[index] == L"1-5";
}

bool CronWeekdaySelected(const std::vector<std::wstring>& fields, int weekday) {
    if (fields.size() < 6) {
        return false;
    }
    const std::wstring dayOfWeek = fields[5];
    if (dayOfWeek == L"*") {
        return true;
    }
    if (dayOfWeek == L"1-5") {
        return weekday >= 1 && weekday <= 5;
    }

    std::size_t start = 0;
    while (start < dayOfWeek.size()) {
        const std::size_t end = dayOfWeek.find(L',', start);
        const std::wstring token = dayOfWeek.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        const auto value = ParseInt(token);
        if (value && *value == weekday) {
            return true;
        }
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

std::wstring DateSummaryText(const SYSTEMTIME& value) {
    return std::to_wstring(value.wMonth) + L"月" + std::to_wstring(value.wDay) + L"日";
}

class DialogWindow {
public:
    DialogWindow(HWND owner, HINSTANCE instance, const Theme& theme, TodoItem& item, bool isNew)
        : owner_(owner), instance_(instance), theme_(theme), item_(item), draft_(item), isNew_(isNew) {}

    bool Run() {
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        ThemedWindowCreateOptions options = ThemedWindowUi::DialogOptions(
            instance_, owner_, L"QuattroTodoEditDialog", isNew_ ? L"新建待办" : L"编辑待办",
            DialogWindow::WindowProc, this, icon, icon);
        options.clientWidth = kDialogWidth;
        options.clientHeight = kDialogHeight;
        options.style |= WS_VSCROLL;
        hwnd_ = ThemedWindowUi::CreateWindowHandle(options);
        if (!hwnd_) {
            return false;
        }

        if (windowUi_) {
            windowUi_->ShowModal();
        }
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!ThemedUi::PreTranslateMessage(message) && !IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        if (windowUi_) {
            windowUi_->RestoreModalOwner();
        }
        return accepted_;
    }

private:
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

    int ClientHeight() const {
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        return rect.bottom - rect.top;
    }

    int ContentBottomLimit() const {
        return ClientHeight() - FooterHeight();
    }

    int DrawerWidth() const {
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        return rect.right - rect.left;
    }

    int HeaderHeight() const {
        return 0;
    }

    int MetricInt(const wchar_t* component, const wchar_t* name, float fallback) const {
        return static_cast<int>(theme_.metric(component, name, fallback));
    }

    int ContentInsetX() const {
        return windowUi_->ui().layout().contentInsetX;
    }

    int FieldX() const {
        return windowUi_->ui().layout().fieldX;
    }

    int FieldRight() const {
        return DrawerWidth() - ContentInsetX();
    }

    int FieldWidth() const {
        return FieldRight() - FieldX();
    }

    int SectionGap() const {
        return windowUi_->ui().layout().sectionGap;
    }

    int RowGap() const {
        return windowUi_->ui().layout().rowGap;
    }

    int ItemGap() const {
        return windowUi_->ui().scale(MetricInt(L"global", L"itemGap", 8.0f));
    }

    int StaticTextHeight() const {
        return windowUi_->ui().scale(MetricInt(L"text", L"textHeight", 20.0f));
    }

    int FooterHeight() const {
        return windowUi_->ui().footerButtonHeight() + RowGap() * 2;
    }

    int CalendarWidth() const { return windowUi_->ui().scale(kCalendarWidth); }
    int CalendarHeaderHeight() const { return windowUi_->ui().scale(MetricInt(L"global", L"mediumControlHeight", 28.0f)); }
    int CalendarWeekdayHeight() const { return windowUi_->ui().scale(MetricInt(L"global", L"smallControlHeight", 24.0f)); }
    int CalendarCellHeight() const { return windowUi_->ui().scale(MetricInt(L"global", L"smallControlHeight", 24.0f)); }
    int CalendarHeight() const { return CalendarHeaderHeight() + CalendarWeekdayHeight() + CalendarCellHeight() * 6; }
    int CalendarMonthCellWidth() const { return windowUi_->ui().scale(kCalendarMonthCellWidth); }
    int CalendarMonthCellHeight() const { return CalendarCellHeight(); }
    int CalendarYearCellWidth() const { return windowUi_->ui().scale(kCalendarYearCellWidth); }
    int CalendarYearCellHeight() const { return CalendarCellHeight(); }

    int TextWidth(const std::wstring& text, HFONT font = nullptr) const {
        HDC dc = GetDC(hwnd_);
        if (!dc) {
            return static_cast<int>(text.size()) * 14;
        }
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(dc, font ? font : windowUi_->font()));
        SIZE size{};
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
        SelectObject(dc, oldFont);
        ReleaseDC(hwnd_, dc);
        return size.cx;
    }

    int ButtonWidth(const std::wstring& text) const {
        return windowUi_->ui().buttonWidth(
            text, ThemedButtonRole::Normal, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
    }

    int TabWidth(const std::wstring& text) const {
        return windowUi_->ui().tabButtonWidth(text);
    }

    int TextControlWidth(const std::wstring& text) const {
        return TextWidth(text) + windowUi_->ui().scale(MetricInt(L"global", L"paddingX", 10.0f));
    }

    int ComboBoxWidth(const std::wstring& text) const {
        const ThemedUi ui = windowUi_->ui();
        const int paddingX = ui.scale(MetricInt(L"comboBox", L"paddingX", 9.0f));
        const int arrowWidth = ui.scale(MetricInt(L"comboBox", L"arrowWidth", 28.0f));
        return TextWidth(text) + paddingX * 2 + arrowWidth;
    }

    int ContentTop() const {
        return -scrollY_;
    }

    bool IntersectsContentViewport(int top, int height) const {
        const int visibleTop = top - scrollY_;
        const int visibleBottom = visibleTop + height;
        return visibleBottom > HeaderHeight() && visibleTop < ClientHeight() - FooterHeight();
    }

    COLORREF ColorFor(const wchar_t* component, const wchar_t* state, const wchar_t* role) const {
        return ToColorRef(theme_.color(component, state, role));
    }

    void SetVisible(HWND hwnd, bool visible) {
        if (hwnd) {
            ShowWindow(hwnd, visible ? SW_SHOW : SW_HIDE);
        }
    }

    void SetEnabled(HWND hwnd, bool enabled) {
        windowUi_->ui().SetEnabled(hwnd, enabled);
    }

    void SetFieldVisible(HWND hwnd, bool visible) {
        if (windowUi_) windowUi_->SetEditVisible(hwnd, visible);
    }

    HWND Label(const wchar_t* text, int x, int y, int width = 180) {
        return windowUi_->ui().Label(text, x, y - scrollY_, width);
    }

    HWND Text(const wchar_t* text, int x, int y, int width, int height = 20) {
        (void)height;
        return windowUi_->ui().Label(text, x, y - scrollY_, width);
    }

    HWND ErrorText(const wchar_t* text, int x, int y, int width) {
        return windowUi_->ui().StatusText(
            text,
            x,
            y - scrollY_,
            width,
            ThemedStatusTextOptions{ThemedStatusRole::Danger, ThemedTextAlign::Start});
    }

    HWND SingleEdit(int id, int x, int y, int width, const std::wstring& value, DWORD extraStyle = ES_AUTOHSCROLL) {
        const int height = windowUi_->ui().editHeight();
        RECT frame{x, y, x + width, y + height};
        ThemedEditOptions options{};
        if ((extraStyle & ES_NUMBER) != 0) {
            options.content = ThemedEditContent::Integer;
        }
        HWND edit = windowUi_->ui().Edit(id, Offset(frame), value, options);
        fields_.push_back(FieldFrame{frame, edit, false, false});
        return edit;
    }

    HWND MultiEdit(int id, int x, int y, int width, int height, const std::wstring& value) {
        RECT frame{x, y, x + width, y + height};
        ThemedEditOptions options{};
        options.mode = ThemedEditMode::MultiLine;
        HWND edit = windowUi_->ui().Edit(id, Offset(frame), value, options);
        fields_.push_back(FieldFrame{frame, edit, true, false});
        return edit;
    }

    RECT Offset(RECT rect) const {
        rect.top -= scrollY_;
        rect.bottom -= scrollY_;
        return rect;
    }

    void MoveStatic(HWND hwnd, int x, int y, int width, int height) {
        if (hwnd) {
            windowUi_->ui().MoveControl(hwnd, x, y - scrollY_, width);
            ShowWindow(hwnd, IntersectsContentViewport(y, height) ? SW_SHOW : SW_HIDE);
        }
    }

    void MoveButton(HWND hwnd, int x, int y, int width, int height) {
        if (hwnd) {
            windowUi_->ui().MoveControl(hwnd, x, y - scrollY_, width);
            ShowWindow(hwnd, IntersectsContentViewport(y, height) ? SW_SHOW : SW_HIDE);
        }
    }

    void MoveCombo(HWND hwnd, int x, int y, int width, int height) {
        if (hwnd) {
            (void)height;
            windowUi_->ui().MoveComboBox(hwnd, x, y - scrollY_, width);
            ShowWindow(hwnd, IntersectsContentViewport(y, windowUi_->ui().comboBoxHeight()) ? SW_SHOW : SW_HIDE);
        }
    }

    void SetFrame(HWND child, RECT frame) {
        for (auto& item : fields_) {
            if (item.child == child) {
                item.rect = frame;
                windowUi_->MoveEditFrame(child, Offset(frame));
                SetFieldVisible(child, IntersectsContentViewport(frame.top, frame.bottom - frame.top));
                return;
            }
        }
    }

    void SetFieldError(HWND child, bool error) {
        for (auto& item : fields_) {
            if (item.child == child) {
                item.error = error;
                windowUi_->SetEditError(child, error);
                break;
            }
        }
    }

    void SetTabChecked(HWND hwnd, bool checked) {
        ThemedUi::SetTabSelected(hwnd, checked);
    }

    bool IsTabChecked(HWND hwnd) const {
        return ThemedUi::IsTabSelected(hwnd);
    }

    void SetTimeText(int hour, int minute) {
        SetWindowTextW(timeEdit_, (TwoDigits(hour) + L":" + TwoDigits(minute)).c_str());
    }

    bool TryReadTime(int& hour, int& minute) const {
        return TryParseHourMinute(GetText(timeEdit_), hour, minute);
    }

    void FillSimpleCombo(HWND combo, const std::vector<std::wstring>& items, int selected) {
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        for (const auto& item : items) {
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
        }
        SendMessageW(combo, CB_SETCURSEL, selected, 0);
    }

    int ComboIndex(HWND combo, int fallback = 0) const {
        const int index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
        return index >= 0 ? index : fallback;
    }

    SYSTEMTIME InitialAnchor() const {
        SYSTEMTIME value{};
        if (!draft_.anchorAt.empty() && TryParseTodoTimestamp(draft_.anchorAt, value)) {
            return value;
        }
        if (!draft_.nextDueAt.empty() && TryParseTodoTimestamp(draft_.nextDueAt, value)) {
            return value;
        }
        GetLocalTime(&value);
        value.wSecond = 0;
        value.wMilliseconds = 0;
        return value;
    }

    RepeatRule InitialRepeatRule() const {
        if (draft_.repeatMode == TodoRepeatMode::Interval && draft_.repeatInterval > 1 &&
            (draft_.scheduleKind == TodoScheduleKind::Daily ||
             draft_.scheduleKind == TodoScheduleKind::Weekly ||
             draft_.scheduleKind == TodoScheduleKind::Monthly)) {
            return RepeatRule::Custom;
        }
        if (draft_.scheduleKind == TodoScheduleKind::Daily) {
            return RepeatRule::Daily;
        }
        if (draft_.scheduleKind == TodoScheduleKind::Weekly) {
            return RepeatRule::Weekly;
        }
        if (draft_.scheduleKind == TodoScheduleKind::Monthly) {
            return RepeatRule::Monthly;
        }
        if (draft_.scheduleKind == TodoScheduleKind::Cron) {
            const auto fields = CronFields(draft_.cronExpression);
            if (fields.size() >= 6 &&
                CronFieldIsAny(fields, 3) &&
                CronFieldIsAny(fields, 4) &&
                CronFieldIsWorkday(fields, 5)) {
                return RepeatRule::Workday;
            }
            if (fields.size() >= 6 &&
                !CronFieldIsAny(fields, 3) &&
                CronFieldIsAny(fields, 4) &&
                CronFieldIsAny(fields, 5)) {
                return RepeatRule::Monthly;
            }
            if (fields.size() >= 6 &&
                CronFieldIsAny(fields, 3) &&
                CronFieldIsAny(fields, 4) &&
                !CronFieldIsAny(fields, 5)) {
                return RepeatRule::Weekly;
            }
            if (fields.size() >= 6 &&
                CronFieldIsAny(fields, 3) &&
                CronFieldIsAny(fields, 4) &&
                CronFieldIsAny(fields, 5)) {
                return RepeatRule::Daily;
            }
        }
        return RepeatRule::None;
    }

    int InitialCustomUnitIndex() const {
        if (draft_.scheduleKind == TodoScheduleKind::Weekly) {
            return 1;
        }
        if (draft_.scheduleKind == TodoScheduleKind::Monthly) {
            return 2;
        }
        return 0;
    }

    void LoadInitialState() {
        hasReminder_ = true;
        repeatRule_ = InitialRepeatRule();
        endMode_ = draft_.repeatLimit > 0 ? EndMode::Count : EndMode::Never;
        advancedExpanded_ = draft_.repeatLimit > 0;

        SYSTEMTIME anchor = InitialAnchor();
        const auto cronFields = draft_.scheduleKind == TodoScheduleKind::Cron ? CronFields(draft_.cronExpression) : std::vector<std::wstring>{};
        if (cronFields.size() >= 6) {
            if (auto second = ParseCronNumberField(cronFields, 0, 0, 59)) {
                anchor.wSecond = static_cast<WORD>(*second);
            }
            if (auto minute = ParseCronNumberField(cronFields, 1, 0, 59)) {
                anchor.wMinute = static_cast<WORD>(*minute);
            }
            if (auto hour = ParseCronNumberField(cronFields, 2, 0, 23)) {
                anchor.wHour = static_cast<WORD>(*hour);
            }
            if (repeatRule_ == RepeatRule::Monthly) {
                if (auto monthDay = ParseCronNumberField(cronFields, 3, 1, 31)) {
                    anchor.wDay = static_cast<WORD>(std::min(*monthDay, DaysInMonth(anchor.wYear, anchor.wMonth)));
                }
            }
        }
        selectedDate_ = DateOnly(anchor);
        calendarYear_ = selectedDate_.wYear;
        calendarMonth_ = selectedDate_.wMonth;
        SetTimeText(anchor.wHour, anchor.wMinute);
        SetWindowTextW(monthlyDayEdit_, std::to_wstring(std::max<int>(1, anchor.wDay)).c_str());
        SetWindowTextW(customIntervalEdit_, std::to_wstring(std::max(1, draft_.repeatInterval)).c_str());
        SetWindowTextW(endCountEdit_, std::to_wstring(std::max(1, draft_.repeatLimit)).c_str());
        SendMessageW(customUnitCombo_, CB_SETCURSEL, InitialCustomUnitIndex(), 0);

        weekdaySelected_.fill(false);
        if (draft_.scheduleKind == TodoScheduleKind::Cron) {
            for (int i = 0; i < 7; ++i) {
                if (CronWeekdaySelected(cronFields, i)) {
                    weekdaySelected_[i] = true;
                }
            }
        } else if (draft_.scheduleKind == TodoScheduleKind::Weekly) {
            weekdaySelected_[DayOfWeek(anchor)] = true;
        }
        if (std::none_of(weekdaySelected_.begin(), weekdaySelected_.end(), [](bool selected) { return selected; })) {
            weekdaySelected_[DayOfWeek(anchor)] = true;
        }
    }

    SYSTEMTIME ReadAnchorTime() const {
        SYSTEMTIME value = selectedDate_;
        int hour = 0;
        int minute = 0;
        if (!TryReadTime(hour, minute)) {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            hour = now.wHour;
            minute = now.wMinute;
        }
        value.wHour = static_cast<WORD>(hour);
        value.wMinute = static_cast<WORD>(minute);
        value.wSecond = 0;
        value.wMilliseconds = 0;
        return value;
    }

    std::wstring WeekdaySummary() const {
        std::wstring text;
        for (int i = 0; i < 7; ++i) {
            if (!weekdaySelected_[i]) {
                continue;
            }
            if (!text.empty()) {
                text += L"、";
            }
            text += LongWeekdayText(i);
        }
        return text;
    }

    std::wstring RepeatSummary() const {
        if (!hasReminder_) {
            return L"未设置";
        }
        switch (repeatRule_) {
        case RepeatRule::Daily:
            return L"每天";
        case RepeatRule::Workday:
            return L"工作日";
        case RepeatRule::Weekly:
            return L"每周" + WeekdaySummary();
        case RepeatRule::Monthly:
            return L"每月 " + std::to_wstring(GetInt(monthlyDayEdit_, ReadAnchorTime().wDay)) + L" 号";
        case RepeatRule::Custom: {
            const int interval = std::max(1, GetInt(customIntervalEdit_, 1));
            const int unit = ComboIndex(customUnitCombo_, 0);
            if (unit == 1) {
                return L"每 " + std::to_wstring(interval) + L" 周" + (WeekdaySummary().empty() ? L"" : L"，" + WeekdaySummary());
            }
            if (unit == 2) {
                return L"每 " + std::to_wstring(interval) + L" 月";
            }
            return L"每 " + std::to_wstring(interval) + L" 天";
        }
        case RepeatRule::None:
        default:
            return L"不重复";
        }
    }

    std::wstring ReminderSummary() const {
        const SYSTEMTIME anchor = ReadAnchorTime();
        return DateSummaryText(anchor) + L" " + LongWeekdayText(DayOfWeek(anchor)) + L" " +
            TwoDigits(anchor.wHour) + L":" + TwoDigits(anchor.wMinute);
    }

    std::wstring Snapshot() const {
        std::wstring result = Trim(GetText(titleEdit_)) + L"\n" + Trim(GetText(contentEdit_)) + L"\n";
        result += hasReminder_ ? L"1\n" : L"0\n";
        result += ReminderSummary() + L"\n";
        result += GetText(timeEdit_) + L"\n";
        result += std::to_wstring(static_cast<int>(repeatRule_)) + L"\n";
        result += std::to_wstring(GetInt(customIntervalEdit_, 1)) + L"\n";
        result += std::to_wstring(ComboIndex(customUnitCombo_, 0)) + L"\n";
        result += std::to_wstring(static_cast<int>(endMode_)) + L"\n";
        result += std::to_wstring(GetInt(endCountEdit_, 1)) + L"\n";
        for (bool selected : weekdaySelected_) {
            result += selected ? L"1" : L"0";
        }
        return result;
    }

    bool IsDirty() const {
        if (!initialized_) {
            return false;
        }
        if (Snapshot() != initialSnapshot_) {
            return true;
        }
        if (isNew_) {
            return !Trim(GetText(titleEdit_)).empty() || !Trim(GetText(contentEdit_)).empty();
        }
        return false;
    }

    void SelectRepeatRule(RepeatRule rule) {
        repeatRule_ = rule;
        SetTabChecked(repeatNone_, rule == RepeatRule::None);
        SetTabChecked(repeatDaily_, rule == RepeatRule::Daily);
        SetTabChecked(repeatWorkday_, rule == RepeatRule::Workday);
        SetTabChecked(repeatWeekly_, rule == RepeatRule::Weekly);
        SetTabChecked(repeatMonthly_, rule == RepeatRule::Monthly);
        SetTabChecked(repeatCustom_, rule == RepeatRule::Custom);
        Layout();
    }

    void SelectEndMode(EndMode mode) {
        endMode_ = mode;
        SetTabChecked(endNeverButton_, mode == EndMode::Never);
        SetTabChecked(endCountButton_, mode == EndMode::Count);
        Layout();
    }

    void SetWeekday(int index, bool selected) {
        if (index < 0 || index > 6) {
            return;
        }
        weekdaySelected_[index] = selected;
        SetTabChecked(weekdayButtons_[index], selected);
        Layout();
    }

    void SyncWeekdayButtons() {
        for (int i = 0; i < 7; ++i) {
            SetTabChecked(weekdayButtons_[i], weekdaySelected_[i]);
        }
    }

    void UpdateSaveState() {
        const bool canSave = !Trim(GetText(titleEdit_)).empty();
        EnableWindow(okButton_, canSave ? TRUE : FALSE);
    }

    void ClearErrors() {
        titleError_.clear();
        reminderError_.clear();
        repeatError_.clear();
        SetFieldError(titleEdit_, false);
        SetFieldError(timeEdit_, false);
        SetFieldError(monthlyDayEdit_, false);
        SetFieldError(customIntervalEdit_, false);
        SetFieldError(endCountEdit_, false);
    }

    bool Validate() {
        ClearErrors();
        bool ok = true;
        if (Trim(GetText(titleEdit_)).empty()) {
            titleError_ = L"请输入待办标题";
            SetFieldError(titleEdit_, true);
            SetFocus(titleEdit_);
            ok = false;
        }
        if (hasReminder_) {
            const SYSTEMTIME anchor = ReadAnchorTime();
            int hour = 0;
            int minute = 0;
            if (!TryReadTime(hour, minute)) {
                reminderError_ = L"请输入 HH:MM 格式的有效时间";
                SetFieldError(timeEdit_, true);
                ok = false;
            }
            if (anchor.wDay < 1 || anchor.wDay > DaysInMonth(anchor.wYear, anchor.wMonth)) {
                reminderError_ = L"请输入有效提醒时间";
                ok = false;
            }
            if ((repeatRule_ == RepeatRule::Weekly || (repeatRule_ == RepeatRule::Custom && ComboIndex(customUnitCombo_, 0) == 1)) &&
                std::none_of(weekdaySelected_.begin(), weekdaySelected_.end(), [](bool selected) { return selected; })) {
                repeatError_ = L"每周至少选择 1 天";
                ok = false;
            }
            if (repeatRule_ == RepeatRule::Monthly || (repeatRule_ == RepeatRule::Custom && ComboIndex(customUnitCombo_, 0) == 2)) {
                const int day = GetInt(monthlyDayEdit_, 0);
                if (day < 1 || day > 31) {
                    repeatError_ = L"每月固定日期必须在 1-31 之间";
                    SetFieldError(monthlyDayEdit_, true);
                    ok = false;
                } else if (day > DaysInMonth(anchor.wYear, anchor.wMonth)) {
                    repeatError_ = L"该月无 " + std::to_wstring(day) + L" 号，将顺延";
                    SetFieldError(monthlyDayEdit_, true);
                    ok = false;
                }
            }
            if (repeatRule_ == RepeatRule::Custom && GetInt(customIntervalEdit_, 0) < 1) {
                repeatError_ = L"重复间隔不能小于 1";
                SetFieldError(customIntervalEdit_, true);
                ok = false;
            }
            if (advancedExpanded_ && endMode_ == EndMode::Count && GetInt(endCountEdit_, 0) < 1) {
                repeatError_ = L"完成次数不能小于 1";
                SetFieldError(endCountEdit_, true);
                ok = false;
            }
        }
        Layout();
        if (!ok) {
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        return ok;
    }

    void ApplyToDraft() {
        draft_.title = Trim(GetText(titleEdit_));
        draft_.content = Trim(GetText(contentEdit_));
        draft_.enabled = true;

        const TodoScheduleKind oldKind = draft_.scheduleKind;
        const TodoRepeatMode oldRepeatMode = draft_.repeatMode;
        const int oldInterval = draft_.repeatInterval;
        const int oldLimit = draft_.repeatLimit;
        const std::wstring oldAnchor = draft_.anchorAt;
        const std::wstring oldCron = draft_.cronExpression;

        if (!hasReminder_) {
            draft_.scheduleKind = TodoScheduleKind::None;
            draft_.repeatMode = TodoRepeatMode::FixedPoint;
            draft_.repeatInterval = 1;
            draft_.repeatLimit = 0;
            draft_.repeatFinished = 0;
            draft_.cronExpression.clear();
            draft_.anchorAt.clear();
            draft_.nextDueAt.clear();
            return;
        }

        SYSTEMTIME anchor = ReadAnchorTime();
        draft_.anchorAt = FormatTodoTimestamp(anchor);
        draft_.repeatMode = TodoRepeatMode::FixedPoint;
        draft_.repeatInterval = 1;
        draft_.repeatLimit = advancedExpanded_ && endMode_ == EndMode::Count ? std::max(1, GetInt(endCountEdit_, 1)) : 0;
        draft_.cronExpression.clear();

        switch (repeatRule_) {
        case RepeatRule::Daily:
            draft_.scheduleKind = TodoScheduleKind::Daily;
            break;
        case RepeatRule::Workday:
            draft_.scheduleKind = TodoScheduleKind::Cron;
            draft_.cronExpression = CronBase(anchor) + L" * * 1-5";
            break;
        case RepeatRule::Weekly:
            draft_.scheduleKind = TodoScheduleKind::Cron;
            draft_.cronExpression = CronBase(anchor) + L" * * " + CronWeekdays(weekdaySelected_);
            break;
        case RepeatRule::Monthly:
            draft_.scheduleKind = TodoScheduleKind::Cron;
            draft_.cronExpression = CronBase(anchor) + L" " + std::to_wstring(std::max(1, std::min(31, GetInt(monthlyDayEdit_, anchor.wDay)))) + L" * *";
            break;
        case RepeatRule::Custom: {
            const int interval = std::max(1, GetInt(customIntervalEdit_, 1));
            draft_.repeatInterval = interval;
            draft_.repeatMode = TodoRepeatMode::Interval;
            const int unit = ComboIndex(customUnitCombo_, 0);
            if (unit == 1) {
                draft_.scheduleKind = TodoScheduleKind::Weekly;
                for (int i = 0; i < 7; ++i) {
                    if (weekdaySelected_[i]) {
                        anchor = DateForWeekday(anchor, i);
                        break;
                    }
                }
                draft_.anchorAt = FormatTodoTimestamp(anchor);
            } else if (unit == 2) {
                anchor.wDay = static_cast<WORD>(std::max(1, std::min(31, GetInt(monthlyDayEdit_, anchor.wDay))));
                draft_.anchorAt = FormatTodoTimestamp(anchor);
                draft_.scheduleKind = TodoScheduleKind::Monthly;
            } else {
                draft_.scheduleKind = TodoScheduleKind::Daily;
            }
            break;
        }
        case RepeatRule::None:
        default:
            draft_.scheduleKind = TodoScheduleKind::Once;
            break;
        }

        if (draft_.scheduleKind == TodoScheduleKind::Cron && !IsValidTodoCronExpression(draft_.cronExpression)) {
            draft_.scheduleKind = TodoScheduleKind::Once;
            draft_.cronExpression.clear();
        }

        if (draft_.scheduleKind != oldKind || draft_.repeatMode != oldRepeatMode || draft_.repeatInterval != oldInterval ||
            draft_.repeatLimit != oldLimit || !SameTodoTime(draft_.anchorAt, oldAnchor) || draft_.cronExpression != oldCron) {
            draft_.repeatFinished = 0;
        }
        draft_.nextDueAt = ComputeNextTodoDueAt(draft_, CurrentTodoTimestamp());
    }

    bool Accept() {
        if (!Validate()) {
            return false;
        }
        ApplyToDraft();
        item_ = draft_;
        accepted_ = true;
        done_ = true;
        DestroyWindow(hwnd_);
        return true;
    }

    bool ConfirmCloseIfDirty() {
        if (!IsDirty()) {
            return true;
        }
        const wchar_t* message = isNew_ ? L"已填写内容，确定放弃创建？" : L"已修改内容，确定放弃修改？";
        return MessageBoxW(hwnd_, message, isNew_ ? L"新建待办" : L"编辑待办", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    }

    void FillCombos() {
        FillSimpleCombo(customUnitCombo_, {L"天", L"周", L"月"}, 0);
    }

    void CreateControls() {
        const ThemedUi ui = windowUi_->ui();

        titleLabel_ = Label(L"待办标题 *", 0, 0, TextControlWidth(L"待办标题 *"));
        titleEdit_ = SingleEdit(IdTitle, 0, 0, 1, draft_.title);
        SendMessageW(titleEdit_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"请输入待办标题..."));
        titleErrorText_ = ErrorText(L"", 0, 0, 1);

        contentLabel_ = Label(L"备注说明", 0, 0, TextControlWidth(L"备注说明"));
        contentEdit_ = MultiEdit(IdContent, 0, 0, 1, ui.editHeight() + StaticTextHeight() + RowGap(), draft_.content);
        SendMessageW(contentEdit_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"补充描述、步骤、附件链接..."));

        timeLabel_ = Text(L"时间", 0, 0, TextControlWidth(L"时间"), StaticTextHeight());
        timeEdit_ = SingleEdit(IdTime, 260, 0, 78, L"00:00");

        repeatNone_ = ui.TabButton(IdRepeatNone, L"不重复", 0, 0, TabWidth(L"不重复"), false);
        repeatDaily_ = ui.TabButton(IdRepeatDaily, L"每天", 0, 0, TabWidth(L"每天"), false);
        repeatWorkday_ = ui.TabButton(IdRepeatWorkday, L"工作日", 0, 0, TabWidth(L"工作日"), false);
        repeatWeekly_ = ui.TabButton(IdRepeatWeekly, L"每周", 0, 0, TabWidth(L"每周"), false);
        repeatMonthly_ = ui.TabButton(IdRepeatMonthly, L"每月", 0, 0, TabWidth(L"每月"), false);
        repeatCustom_ = ui.TabButton(IdRepeatCustom, L"自定义", 0, 0, TabWidth(L"自定义"), false);

        for (int i = 0; i < 7; ++i) {
            weekdayButtons_[i] = ui.TabButton(IdWeekdayBase + i, ShortWeekdayText(i), 0, 0, TabWidth(ShortWeekdayText(i)), false);
        }

        workdayHint_ = Text(L"默认周一至周五执行", 0, 0, TextControlWidth(L"默认周一至周五执行"), StaticTextHeight());
        monthlyFixedButton_ = ui.TabButton(IdMonthlyFixed, L"每月固定", 0, 0, TabWidth(L"每月固定"), false);
        monthlyDayEdit_ = SingleEdit(IdMonthlyDay, 0, 0, ui.editHeight() + ui.scale(ThemedControls::EditPaddingX(theme_)) * 2, L"1", ES_NUMBER);
        monthlyDayLabel_ = Text(L"号", 0, 0, TextControlWidth(L"号"), StaticTextHeight());
        customPrefix_ = Text(L"每", 0, 0, TextControlWidth(L"每"), StaticTextHeight());
        customIntervalEdit_ = SingleEdit(IdCustomInterval, 0, 0, ui.editHeight() + ui.scale(ThemedControls::EditPaddingX(theme_)) * 2, L"1", ES_NUMBER);
        customUnitCombo_ = ui.ComboBox(IdCustomUnit, 0, 0, ComboBoxWidth(L"天"));
        customSuffix_ = Text(L"重复", 0, 0, TextControlWidth(L"重复"), StaticTextHeight());
        repeatErrorText_ = ErrorText(L"", 0, 0, 1);

        advancedButton_ = ui.Button(IdAdvancedToggle, L"高级设置 ▼", 0, 0, ThemedButtonRole::Normal, ThemedButtonSize::Compact, ThemedButtonWidthMode::Fixed, ButtonWidth(L"高级设置 ▼"));
        endNeverButton_ = ui.TabButton(IdEndNever, L"永不结束", 0, 0, TabWidth(L"永不结束"), false);
        endCountButton_ = ui.TabButton(IdEndCount, L"完成 N 次", 0, 0, TabWidth(L"完成 N 次"), false);
        endCountEdit_ = SingleEdit(IdEndCountValue, 0, 0, ui.editHeight() + ui.scale(ThemedControls::EditPaddingX(theme_)) * 2, L"1", ES_NUMBER);

        okButton_ = ui.Button(IDOK, isNew_ ? L"保存待办" : L"保存", 0, 0, ThemedButtonRole::Primary, ThemedButtonSize::Large, ThemedButtonWidthMode::Fixed, ButtonWidth(isNew_ ? L"保存待办" : L"保存"), true);
        cancelButton_ = ui.Button(IDCANCEL, L"取消", 0, 0, ThemedButtonRole::Normal, ThemedButtonSize::Large, ThemedButtonWidthMode::Fixed, ButtonWidth(L"取消"));

        FillCombos();
        LoadInitialState();
        SelectRepeatRule(repeatRule_);
        SetTabChecked(monthlyFixedButton_, true);
        SelectEndMode(endMode_);
        SyncWeekdayButtons();
        UpdateSaveState();
        initialized_ = true;
        initialSnapshot_ = Snapshot();
        SetFocus(titleEdit_);
        SendMessageW(titleEdit_, EM_SETSEL, 0, -1);
    }

    void Layout() {
        if (!hwnd_) {
            return;
        }

        const ThemedUi ui = windowUi_->ui();
        const int fieldHeight = ui.editHeight();
        const int labelHeight = ui.labelHeight();
        const int tabHeight = ui.tabButtonHeight();
        const int insetX = ContentInsetX();
        const int fieldX = FieldX();
        const int contentRight = FieldRight();
        const int contentWidth = FieldWidth();
        const int rowGap = RowGap();
        const int itemGap = ItemGap();
        const int textHeight = StaticTextHeight();
        const int multiEditHeight = fieldHeight + textHeight + rowGap;
        const int labelOffsetY = std::max(0, (fieldHeight - labelHeight) / 2);
        int y = windowUi_->ui().layout().contentInsetY;

        MoveStatic(titleLabel_, insetX, y + labelOffsetY, TextControlWidth(L"待办标题 *"), labelHeight);
        SetFrame(titleEdit_, RECT{fieldX, y, contentRight, y + fieldHeight});
        y += fieldHeight;
        const bool hasTitleError = !titleError_.empty();
        SetVisible(titleErrorText_, hasTitleError);
        if (hasTitleError) {
            SetWindowTextW(titleErrorText_, titleError_.c_str());
            MoveStatic(titleErrorText_, fieldX, y + rowGap, contentWidth, textHeight);
            y += textHeight + rowGap;
        }

        y += itemGap;
        MoveStatic(contentLabel_, insetX, y + labelOffsetY, TextControlWidth(L"备注说明"), labelHeight);
        SetFrame(contentEdit_, RECT{fieldX, y, contentRight, y + multiEditHeight});
        y += multiEditHeight + itemGap;

        const int reminderHeight = fieldHeight;
        reminderRect_ = RECT{insetX, y, contentRight, y + reminderHeight};
        y += reminderHeight;
        if (!reminderError_.empty()) {
            y += textHeight + rowGap;
        }
        y += rowGap;
        calendarRect_ = RECT{fieldX, y, fieldX + CalendarWidth(), y + CalendarHeight()};
        const int sideX = calendarRect_.right + itemGap + rowGap;
        MoveStatic(timeLabel_, sideX, y + labelOffsetY, TextControlWidth(L"时间"), labelHeight);
        SetFrame(timeEdit_, RECT{sideX, y + fieldHeight, sideX + 82, y + fieldHeight * 2});
        SetVisible(timeLabel_, true);
        SetFieldVisible(timeEdit_, true);
        y += CalendarHeight() + rowGap;

        repeatLabelY_ = y;
        int x = fieldX;
        const std::array<std::pair<HWND, int>, 6> repeatButtons{{
            {repeatNone_, TabWidth(L"不重复")}, {repeatDaily_, TabWidth(L"每天")}, {repeatWorkday_, TabWidth(L"工作日")},
            {repeatWeekly_, TabWidth(L"每周")}, {repeatMonthly_, TabWidth(L"每月")}, {repeatCustom_, TabWidth(L"自定义")},
        }};
        for (const auto& [button, width] : repeatButtons) {
            MoveButton(button, x, y, width, tabHeight);
            x += width + itemGap;
        }
        y += tabHeight + rowGap;

        const bool showWeekdays = repeatRule_ == RepeatRule::Weekly || (repeatRule_ == RepeatRule::Custom && ComboIndex(customUnitCombo_, 0) == 1);
        const bool showMonthly = repeatRule_ == RepeatRule::Monthly || (repeatRule_ == RepeatRule::Custom && ComboIndex(customUnitCombo_, 0) == 2);
        const bool showCustom = repeatRule_ == RepeatRule::Custom;
        SetVisible(workdayHint_, repeatRule_ == RepeatRule::Workday);
        if (repeatRule_ == RepeatRule::Workday) {
            MoveStatic(workdayHint_, fieldX, y, TextControlWidth(L"默认周一至周五执行"), textHeight);
            y += textHeight + rowGap;
        }

        const int weekdayButtonSize = TabWidth(L"日");
        for (int i = 0; i < 7; ++i) {
            SetVisible(weekdayButtons_[i], showWeekdays);
            if (showWeekdays) {
                MoveButton(weekdayButtons_[i], fieldX + i * (weekdayButtonSize + itemGap), y, weekdayButtonSize, tabHeight);
            }
        }
        if (showWeekdays) {
            y += tabHeight + rowGap;
        }

        SetVisible(monthlyFixedButton_, showMonthly);
        SetFieldVisible(monthlyDayEdit_, showMonthly);
        SetVisible(monthlyDayLabel_, showMonthly);
        if (showMonthly) {
            const int fixedWidth = TabWidth(L"每月固定");
            const int dayWidth = fieldHeight + ThemedControls::EditPaddingX(theme_) * 2;
            MoveButton(monthlyFixedButton_, fieldX, y, fixedWidth, tabHeight);
            SetFrame(monthlyDayEdit_, RECT{fieldX + fixedWidth + itemGap, y, fieldX + fixedWidth + itemGap + dayWidth, y + fieldHeight});
            MoveStatic(monthlyDayLabel_, fieldX + fixedWidth + itemGap + dayWidth + itemGap, y + labelOffsetY, TextControlWidth(L"号"), textHeight);
            y += fieldHeight + rowGap;
        }

        SetVisible(customPrefix_, showCustom);
        SetFieldVisible(customIntervalEdit_, showCustom);
        SetVisible(customUnitCombo_, showCustom);
        SetVisible(customSuffix_, showCustom);
        if (showCustom) {
            const int prefixWidth = TextControlWidth(L"每");
            const int intervalWidth = fieldHeight + ThemedControls::EditPaddingX(theme_) * 2;
            const int unitWidth = ComboBoxWidth(L"天");
            int customX = fieldX + itemGap * 2;
            MoveStatic(customPrefix_, customX, y + labelOffsetY, prefixWidth, textHeight);
            customX += prefixWidth + itemGap;
            SetFrame(customIntervalEdit_, RECT{customX, y, customX + intervalWidth, y + fieldHeight});
            customX += intervalWidth + itemGap;
            MoveCombo(
                customUnitCombo_, customX, y, unitWidth,
                ThemedControls::ComboBoxDropdownHeight(
                    theme_, windowUi_ ? windowUi_->dpi() : USER_DEFAULT_SCREEN_DPI));
            customX += unitWidth + itemGap;
            MoveStatic(customSuffix_, customX, y + labelOffsetY, TextControlWidth(L"重复"), textHeight);
            y += fieldHeight + rowGap;
        }

        const bool hasRepeatError = !repeatError_.empty();
        SetVisible(repeatErrorText_, hasRepeatError);
        if (hasRepeatError) {
            SetWindowTextW(repeatErrorText_, repeatError_.c_str());
            MoveStatic(repeatErrorText_, fieldX, y, contentWidth, textHeight);
            y += textHeight + rowGap;
        }

        const std::wstring advancedText = advancedExpanded_ ? L"高级设置 ▲" : L"高级设置 ▼";
        SetWindowTextW(advancedButton_, advancedText.c_str());
        MoveButton(advancedButton_, fieldX, y, ButtonWidth(advancedText), ThemedControls::CompactButtonHeight(theme_));
        y += ThemedControls::CompactButtonHeight(theme_) + rowGap;
        const bool recurring = hasReminder_ && repeatRule_ != RepeatRule::None;
        SetEnabled(advancedButton_, recurring);
        SetVisible(endNeverButton_, advancedExpanded_);
        SetVisible(endCountButton_, advancedExpanded_);
        SetFieldVisible(endCountEdit_, advancedExpanded_ && endMode_ == EndMode::Count);
        if (advancedExpanded_) {
            const int neverWidth = TabWidth(L"永不结束");
            const int countWidth = TabWidth(L"完成 N 次");
            const int countEditWidth = fieldHeight + ThemedControls::EditPaddingX(theme_) * 2;
            MoveButton(endNeverButton_, fieldX, y, neverWidth, tabHeight);
            MoveButton(endCountButton_, fieldX + neverWidth + itemGap, y, countWidth, tabHeight);
            SetFrame(endCountEdit_, RECT{fieldX + neverWidth + itemGap + countWidth + itemGap, y, fieldX + neverWidth + itemGap + countWidth + itemGap + countEditWidth, y + fieldHeight});
            y += fieldHeight + rowGap;
            SetEnabled(endNeverButton_, recurring);
            SetEnabled(endCountButton_, recurring);
            SetEnabled(endCountEdit_, recurring && endMode_ == EndMode::Count);
            y += rowGap;
        }

        contentHeight_ = y + rowGap;
        if (ClampScrollToContent()) {
            Layout();
            return;
        }
        LayoutFooter();
        UpdateScrollBar();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void LayoutFooter() {
        const int buttonHeight = windowUi_->ui().footerButtonHeight();
        const int footerHeight = FooterHeight();
        const int insetX = ContentInsetX();
        const int itemGap = ItemGap();
        const int y = ClientHeight() - footerHeight + (footerHeight - buttonHeight) / 2;
        const int okWidth = ButtonWidth(isNew_ ? L"保存待办" : L"保存");
        const int cancelWidth = ButtonWidth(L"取消");
        windowUi_->ui().MoveControl(okButton_, DrawerWidth() - insetX - okWidth, y, okWidth);
        windowUi_->ui().MoveControl(cancelButton_, DrawerWidth() - insetX - okWidth - itemGap - cancelWidth, y, cancelWidth);
    }

    void UpdateScrollBar() {
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        info.nMin = 0;
        info.nMax = std::max(0, contentHeight_ - 1);
        info.nPage = std::max(1, ContentBottomLimit() - HeaderHeight());
        info.nPos = scrollY_;
        SetScrollInfo(hwnd_, SB_VERT, &info, TRUE);
    }

    bool ClampScrollToContent() {
        const int page = std::max(1, ContentBottomLimit() - HeaderHeight());
        const int maxPos = std::max(0, contentHeight_ - page);
        const int next = std::max(0, std::min(scrollY_, maxPos));
        if (next == scrollY_) {
            return false;
        }
        scrollY_ = next;
        return true;
    }

    void ScrollTo(int position) {
        const int page = std::max(1, ContentBottomLimit() - HeaderHeight());
        const int maxPos = std::max(0, contentHeight_ - page);
        const int next = std::max(0, std::min(maxPos, position));
        if (next == scrollY_) {
            return;
        }
        scrollY_ = next;
        Layout();
    }

    void DrawLine(ThemedPaint& paint, int y) {
        paint.DrawLine({0, y}, {DrawerWidth(), y});
    }

    void DrawPanel(ThemedPaint& paint, RECT rect, bool selected, bool error) {
        paint.Fill(
            rect,
            ThemedPaintComponent::Panel,
            error ? ThemedPaintState::Danger : (selected ? ThemedPaintState::Selected : ThemedPaintState::Normal),
            ThemedPaintShape::RoundedRectangle);
    }

    void DrawCalendarCell(ThemedPaint& paint, RECT rect, ThemedPaintState state) {
        paint.Fill(
            rect,
            ThemedPaintComponent::CalendarDay,
            state,
            ThemedPaintShape::RoundedRectangle);
    }

    void DrawTextIn(
        ThemedPaint& paint,
        const std::wstring& text,
        RECT rect,
        ThemedPaintComponent component,
        ThemedPaintState state,
        ThemedPaintTextAlign align = ThemedPaintTextAlign::Start,
        bool ellipsis = false) {
        ThemedPaintTextOptions options;
        options.align = align;
        options.verticalAlign = ThemedPaintVerticalAlign::Center;
        options.ellipsis = ellipsis;
        paint.DrawText(text, rect, component, state, options);
    }

    RECT CalendarPrevRect() const {
        return RECT{calendarRect_.left + 8, calendarRect_.top + 5, calendarRect_.left + 30, calendarRect_.top + 27};
    }

    RECT CalendarNextRect() const {
        return RECT{calendarRect_.right - 30, calendarRect_.top + 5, calendarRect_.right - 8, calendarRect_.top + 27};
    }

    RECT CalendarYearTitleRect() const {
        return RECT{calendarRect_.left + 72, calendarRect_.top + 5, calendarRect_.left + 130, calendarRect_.top + 28};
    }

    RECT CalendarMonthTitleRect() const {
        return RECT{calendarRect_.left + 130, calendarRect_.top + 5, calendarRect_.left + 166, calendarRect_.top + 28};
    }

    int CalendarDayFromPoint(POINT point) const {
        if (calendarPickerMode_ != CalendarPickerMode::Day) {
            return 0;
        }
        const int logicalY = point.y + scrollY_;
        if (point.x < calendarRect_.left || point.x >= calendarRect_.right ||
            logicalY < calendarRect_.top + CalendarHeaderHeight() + CalendarWeekdayHeight() ||
            logicalY >= calendarRect_.bottom) {
            return 0;
        }

        SYSTEMTIME first{};
        first.wYear = static_cast<WORD>(calendarYear_);
        first.wMonth = static_cast<WORD>(calendarMonth_);
        first.wDay = 1;
        const int firstOffset = DayOfWeek(first);
        const int cellWidth = CalendarWidth() / 7;
        const int col = std::max(0, std::min(6, static_cast<int>(point.x - calendarRect_.left) / cellWidth));
        const int row = std::max(0, std::min(5, static_cast<int>(logicalY - calendarRect_.top - CalendarHeaderHeight() - CalendarWeekdayHeight()) / CalendarCellHeight()));
        const int day = row * 7 + col - firstOffset + 1;
        return (day >= 1 && day <= DaysInMonth(calendarYear_, calendarMonth_)) ? day : 0;
    }

    int CalendarMonthFromPoint(POINT point) const {
        if (calendarPickerMode_ != CalendarPickerMode::Month) {
            return 0;
        }
        const int logicalY = point.y + scrollY_;
        const int gridLeft = calendarRect_.left + 46;
        const int gridTop = calendarRect_.top + 42;
        if (point.x < gridLeft || point.x >= gridLeft + CalendarMonthCellWidth() * 2 ||
            logicalY < gridTop || logicalY >= gridTop + CalendarMonthCellHeight() * 6) {
            return 0;
        }
        const int col = static_cast<int>(point.x - gridLeft) / CalendarMonthCellWidth();
        const int row = static_cast<int>(logicalY - gridTop) / CalendarMonthCellHeight();
        const int month = row * 2 + col + 1;
        return month >= 1 && month <= 12 ? month : 0;
    }

    void ChangeCalendarMonth(int delta) {
        if (calendarPickerMode_ == CalendarPickerMode::Year) {
            calendarYearPageStart_ = std::max(1900, calendarYearPageStart_ + delta * 12);
            InvalidateRect(hwnd_, nullptr, TRUE);
            return;
        }
        if (calendarPickerMode_ == CalendarPickerMode::Month) {
            calendarYear_ = std::max(1900, calendarYear_ + delta);
            InvalidateRect(hwnd_, nullptr, TRUE);
            return;
        }
        SYSTEMTIME value{};
        value.wYear = static_cast<WORD>(calendarYear_);
        value.wMonth = static_cast<WORD>(calendarMonth_);
        value.wDay = 1;
        value = AddMonths(value, delta);
        calendarYear_ = value.wYear;
        calendarMonth_ = value.wMonth;
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void SelectCalendarMonth(int month) {
        if (month < 1 || month > 12) {
            return;
        }
        calendarMonth_ = month;
        selectedDate_.wYear = static_cast<WORD>(calendarYear_);
        selectedDate_.wMonth = static_cast<WORD>(calendarMonth_);
        selectedDate_.wDay = static_cast<WORD>(std::min<int>(selectedDate_.wDay, DaysInMonth(calendarYear_, calendarMonth_)));
        calendarPickerMode_ = CalendarPickerMode::Day;
        Layout();
    }

    int CalendarYearFromPoint(POINT point) const {
        if (calendarPickerMode_ != CalendarPickerMode::Year) {
            return 0;
        }
        const int logicalY = point.y + scrollY_;
        const int gridLeft = calendarRect_.left + 28;
        const int gridTop = calendarRect_.top + 42;
        if (point.x < gridLeft || point.x >= gridLeft + CalendarYearCellWidth() * 2 ||
            logicalY < gridTop || logicalY >= gridTop + CalendarYearCellHeight() * 6) {
            return 0;
        }
        const int col = static_cast<int>(point.x - gridLeft) / CalendarYearCellWidth();
        const int row = static_cast<int>(logicalY - gridTop) / CalendarYearCellHeight();
        return calendarYearPageStart_ + row * 2 + col;
    }

    void SelectCalendarYear(int year) {
        if (year < 1900) {
            return;
        }
        calendarYear_ = year;
        selectedDate_.wYear = static_cast<WORD>(calendarYear_);
        selectedDate_.wDay = static_cast<WORD>(std::min<int>(selectedDate_.wDay, DaysInMonth(calendarYear_, calendarMonth_)));
        calendarPickerMode_ = CalendarPickerMode::Day;
        Layout();
    }

    void SelectCalendarDay(int day) {
        if (day < 1 || day > DaysInMonth(calendarYear_, calendarMonth_)) {
            return;
        }
        selectedDate_.wYear = static_cast<WORD>(calendarYear_);
        selectedDate_.wMonth = static_cast<WORD>(calendarMonth_);
        selectedDate_.wDay = static_cast<WORD>(day);
        selectedDate_ = DateOnly(selectedDate_);
        SetWindowTextW(monthlyDayEdit_, std::to_wstring(day).c_str());
        Layout();
    }

    void SelectCalendarDate(SYSTEMTIME date) {
        date = DateOnly(date);
        selectedDate_ = date;
        calendarYear_ = date.wYear;
        calendarMonth_ = date.wMonth;
        SetWindowTextW(monthlyDayEdit_, std::to_wstring(static_cast<int>(date.wDay)).c_str());
        Layout();
    }

    void MoveCalendarSelection(int dayDelta) {
        if (calendarPickerMode_ != CalendarPickerMode::Day) {
            calendarPickerMode_ = CalendarPickerMode::Day;
            InvalidateRect(hwnd_, nullptr, TRUE);
            return;
        }
        SelectCalendarDate(AddDays(selectedDate_, dayDelta));
    }

    bool HandleCalendarKey(WPARAM key) {
        switch (key) {
        case VK_LEFT:
            MoveCalendarSelection(-1);
            return true;
        case VK_RIGHT:
            MoveCalendarSelection(1);
            return true;
        case VK_UP:
            MoveCalendarSelection(-7);
            return true;
        case VK_DOWN:
            MoveCalendarSelection(7);
            return true;
        case VK_RETURN:
        case VK_SPACE:
            SelectCalendarDay(static_cast<int>(selectedDate_.wDay));
            return true;
        default:
            return false;
        }
    }

    void ValidateMonthlyDayInline() {
        const bool relevant = hasReminder_ &&
            (repeatRule_ == RepeatRule::Monthly ||
             (repeatRule_ == RepeatRule::Custom && ComboIndex(customUnitCombo_, 0) == 2));
        if (!relevant) {
            if (repeatError_ == L"每月固定日期必须在 1-31 之间" ||
                repeatError_.find(L"该月无 ") == 0) {
                repeatError_.clear();
                SetFieldError(monthlyDayEdit_, false);
            }
            return;
        }
        const int day = GetInt(monthlyDayEdit_, 0);
        if (day < 1 || day > 31) {
            repeatError_ = L"每月固定日期必须在 1-31 之间";
            SetFieldError(monthlyDayEdit_, true);
            return;
        }
        const int maxDay = DaysInMonth(selectedDate_.wYear, selectedDate_.wMonth);
        if (day > maxDay) {
            repeatError_ = L"该月无 " + std::to_wstring(day) + L" 号，将顺延";
            SetFieldError(monthlyDayEdit_, true);
            return;
        }
        if (repeatError_ == L"每月固定日期必须在 1-31 之间" ||
            repeatError_.find(L"该月无 ") == 0) {
            repeatError_.clear();
            SetFieldError(monthlyDayEdit_, false);
        }
    }

    void DrawCalendar(ThemedPaint& paint) {
        RECT rect = Offset(calendarRect_);
        DrawPanel(paint, rect, false, false);

        RECT prev = Offset(CalendarPrevRect());
        RECT next = Offset(CalendarNextRect());
        paint.Fill(prev, ThemedPaintComponent::TabButton, ThemedPaintState::Normal, ThemedPaintShape::RoundedRectangle);
        paint.Fill(next, ThemedPaintComponent::TabButton, ThemedPaintState::Normal, ThemedPaintShape::RoundedRectangle);
        DrawTextIn(paint, L"<", prev, ThemedPaintComponent::Text, ThemedPaintState::Muted, ThemedPaintTextAlign::Center);
        DrawTextIn(paint, L">", next, ThemedPaintComponent::Text, ThemedPaintState::Muted, ThemedPaintTextAlign::Center);

        if (calendarPickerMode_ == CalendarPickerMode::Year) {
            const std::wstring title = std::to_wstring(calendarYearPageStart_) + L"-" + std::to_wstring(calendarYearPageStart_ + 11);
            DrawTextIn(paint, title, RECT{rect.left + 42, rect.top + 6, rect.right - 42, rect.top + 28}, ThemedPaintComponent::Text, ThemedPaintState::Normal, ThemedPaintTextAlign::Center);
            const int gridLeft = rect.left + 28;
            const int gridTop = rect.top + 42;
            for (int i = 0; i < 12; ++i) {
                const int year = calendarYearPageStart_ + i;
                const int row = i / 2;
                const int col = i % 2;
                RECT cell{
                    gridLeft + col * CalendarYearCellWidth() + 6,
                    gridTop + row * CalendarYearCellHeight() + 3,
                    gridLeft + (col + 1) * CalendarYearCellWidth() - 6,
                    gridTop + (row + 1) * CalendarYearCellHeight() - 3,
                };
                const bool selected = selectedDate_.wYear == year;
                if (selected) {
                    DrawCalendarCell(paint, cell, ThemedPaintState::Selected);
                }
                DrawTextIn(paint, std::to_wstring(year), cell, selected ? ThemedPaintComponent::CalendarDay : ThemedPaintComponent::Text, selected ? ThemedPaintState::Selected : ThemedPaintState::Normal, ThemedPaintTextAlign::Center);
            }
            return;
        }

        RECT yearTitle = Offset(CalendarYearTitleRect());
        RECT monthTitle = Offset(CalendarMonthTitleRect());
        paint.Fill(yearTitle, ThemedPaintComponent::TabButton, ThemedPaintState::Normal, ThemedPaintShape::RoundedRectangle);
        paint.Fill(monthTitle, ThemedPaintComponent::TabButton, ThemedPaintState::Normal, ThemedPaintShape::RoundedRectangle);
        DrawTextIn(paint, std::to_wstring(calendarYear_) + L"年", yearTitle, ThemedPaintComponent::Text, ThemedPaintState::Normal, ThemedPaintTextAlign::Center);
        DrawTextIn(paint, std::to_wstring(calendarMonth_) + L"月", monthTitle, ThemedPaintComponent::Text, ThemedPaintState::Normal, ThemedPaintTextAlign::Center);

        if (calendarPickerMode_ == CalendarPickerMode::Month) {
            static constexpr const wchar_t* kMonths[] = {
                L"1月", L"2月", L"3月", L"4月", L"5月", L"6月",
                L"7月", L"8月", L"9月", L"10月", L"11月", L"12月",
            };
            const int gridLeft = rect.left + 46;
            const int gridTop = rect.top + 42;
            for (int month = 1; month <= 12; ++month) {
                const int index = month - 1;
                const int row = index / 2;
                const int col = index % 2;
                RECT cell{
                    gridLeft + col * CalendarMonthCellWidth() + 4,
                    gridTop + row * CalendarMonthCellHeight() + 3,
                    gridLeft + (col + 1) * CalendarMonthCellWidth() - 4,
                    gridTop + (row + 1) * CalendarMonthCellHeight() - 3,
                };
                const bool selected = selectedDate_.wYear == calendarYear_ && selectedDate_.wMonth == month;
                if (selected) {
                    DrawCalendarCell(paint, cell, ThemedPaintState::Selected);
                }
                DrawTextIn(paint, kMonths[index], cell, selected ? ThemedPaintComponent::CalendarDay : ThemedPaintComponent::Text, selected ? ThemedPaintState::Selected : ThemedPaintState::Normal, ThemedPaintTextAlign::Center);
            }
            return;
        }

        const int cellWidth = CalendarWidth() / 7;
        int y = rect.top + CalendarHeaderHeight();
        for (int i = 0; i < 7; ++i) {
            RECT weekday{rect.left + i * cellWidth, y, rect.left + (i + 1) * cellWidth, y + CalendarWeekdayHeight()};
            DrawTextIn(paint, ShortWeekdayText(i), weekday, ThemedPaintComponent::Text, ThemedPaintState::Muted, ThemedPaintTextAlign::Center);
        }

        SYSTEMTIME first{};
        first.wYear = static_cast<WORD>(calendarYear_);
        first.wMonth = static_cast<WORD>(calendarMonth_);
        first.wDay = 1;
        const int firstOffset = DayOfWeek(first);
        const int days = DaysInMonth(calendarYear_, calendarMonth_);
        SYSTEMTIME today{};
        GetLocalTime(&today);
        today = DateOnly(today);
        y += CalendarWeekdayHeight();
        for (int day = 1; day <= days; ++day) {
            const int index = firstOffset + day - 1;
            const int row = index / 7;
            const int col = index % 7;
            RECT cell{
                rect.left + col * cellWidth + 3,
                y + row * CalendarCellHeight() + 1,
                rect.left + (col + 1) * cellWidth - 3,
                y + (row + 1) * CalendarCellHeight() - 1,
            };
            const bool selected = selectedDate_.wYear == calendarYear_ && selectedDate_.wMonth == calendarMonth_ && selectedDate_.wDay == day;
            const bool isToday = today.wYear == calendarYear_ && today.wMonth == calendarMonth_ && today.wDay == day;
            if (selected) {
                DrawCalendarCell(paint, cell, ThemedPaintState::Selected);
            } else if (isToday) {
                DrawCalendarCell(paint, cell, ThemedPaintState::Today);
            }
            const ThemedPaintComponent dayComponent = selected || isToday
                ? ThemedPaintComponent::CalendarDay
                : ThemedPaintComponent::Text;
            const ThemedPaintState dayState = selected
                ? ThemedPaintState::Selected
                : (isToday ? ThemedPaintState::Today : ThemedPaintState::Normal);
            DrawTextIn(
                paint, std::to_wstring(day), cell,
                dayComponent, dayState, ThemedPaintTextAlign::Center);
        }
    }

    void Paint(HDC dc) {
        ThemedPaint paint(hwnd_, dc, theme_, windowUi_->font());
        RECT client{};
        GetClientRect(hwnd_, &client);
        windowUi_->FillBackground(dc);
        windowUi_->DrawRegisteredEditFrames(dc);

        RECT reminder = Offset(reminderRect_);
        const int textHeight = StaticTextHeight();
        const int labelOffsetY = std::max(0, (static_cast<int>(reminder.bottom - reminder.top) - windowUi_->ui().labelHeight()) / 2);
        DrawTextIn(paint, L"提醒时间", RECT{ContentInsetX(), reminder.top + labelOffsetY, FieldX(), reminder.bottom}, ThemedPaintComponent::Label, ThemedPaintState::Normal);
        const std::wstring summary = ReminderSummary();
        DrawTextIn(paint, summary, RECT{FieldX(), reminder.top + labelOffsetY, reminder.right, reminder.bottom}, ThemedPaintComponent::Text, ThemedPaintState::Accent, ThemedPaintTextAlign::Start, true);
        if (!reminderError_.empty()) {
            DrawTextIn(paint, reminderError_, RECT{FieldX(), reminder.bottom + RowGap(), FieldRight(), reminder.bottom + RowGap() + textHeight}, ThemedPaintComponent::Text, ThemedPaintState::Danger);
        }

        DrawCalendar(paint);
        const int repeatLabelOffsetY = std::max(0, (windowUi_->ui().tabButtonHeight() - windowUi_->ui().labelHeight()) / 2);
        DrawTextIn(paint, L"重复规则", RECT{ContentInsetX(), repeatLabelY_ - scrollY_ + repeatLabelOffsetY, FieldX(), repeatLabelY_ - scrollY_ + windowUi_->ui().tabButtonHeight()}, ThemedPaintComponent::Label, ThemedPaintState::Normal);

        RECT footer{0, ClientHeight() - FooterHeight(), client.right, ClientHeight()};
        paint.Fill(footer, ThemedPaintComponent::Panel);
        DrawLine(paint, footer.top);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT commonResult = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, commonResult)) {
            if (message == WM_DPICHANGED && windowUi_) {
                // The common layer first updates the DPI, font, client metrics,
                // and registered frames. Re-run the business layout afterward
                // so calendar geometry and Footer controls consume the new
                // physical client size instead of stale 96-DPI coordinates.
                Layout();
            }
            return commonResult;
        }
        switch (message) {
        case WM_CREATE:
        {
            RECT client{};
            GetClientRect(hwnd_, &client);
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_, owner_, hwnd_, theme_, DialogLayoutKind::Standard,
                client.right - client.left, client.bottom - client.top);
            CreateControls();
            Layout();
            return 0;
        }
        case WM_SIZE:
            Layout();
            return 0;
        case WM_VSCROLL: {
            SCROLLINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = SIF_ALL;
            GetScrollInfo(hwnd_, SB_VERT, &info);
            int pos = scrollY_;
            switch (LOWORD(wParam)) {
            case SB_LINEUP: pos -= 32; break;
            case SB_LINEDOWN: pos += 32; break;
            case SB_PAGEUP: pos -= static_cast<int>(info.nPage); break;
            case SB_PAGEDOWN: pos += static_cast<int>(info.nPage); break;
            case SB_THUMBTRACK: pos = info.nTrackPos; break;
            default: break;
            }
            ScrollTo(pos);
            return 0;
        }
        case WM_MOUSEWHEEL:
            ScrollTo(scrollY_ - GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * 80);
            return 0;
        case WM_LBUTTONUP: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT calendar = Offset(calendarRect_);
            calendarFocused_ = PtInRect(&calendar, point) != FALSE;
            if (calendarFocused_) {
                SetFocus(hwnd_);
            }
            RECT prev = Offset(CalendarPrevRect());
            RECT next = Offset(CalendarNextRect());
            RECT yearTitle = Offset(CalendarYearTitleRect());
            RECT monthTitle = Offset(CalendarMonthTitleRect());
            if (PtInRect(&prev, point)) {
                ChangeCalendarMonth(-1);
                return 0;
            }
            if (PtInRect(&next, point)) {
                ChangeCalendarMonth(1);
                return 0;
            }
            if (PtInRect(&yearTitle, point)) {
                calendarYearPageStart_ = calendarYear_ - ((calendarYear_ - 1900) % 12);
                calendarPickerMode_ = calendarPickerMode_ == CalendarPickerMode::Year ? CalendarPickerMode::Day : CalendarPickerMode::Year;
                InvalidateRect(hwnd_, nullptr, TRUE);
                return 0;
            }
            if (PtInRect(&monthTitle, point)) {
                calendarPickerMode_ = calendarPickerMode_ == CalendarPickerMode::Month ? CalendarPickerMode::Day : CalendarPickerMode::Month;
                InvalidateRect(hwnd_, nullptr, TRUE);
                return 0;
            }
            const int year = CalendarYearFromPoint(point);
            if (year > 0) {
                SelectCalendarYear(year);
                return 0;
            }
            const int month = CalendarMonthFromPoint(point);
            if (month > 0) {
                SelectCalendarMonth(month);
                return 0;
            }
            const int day = CalendarDayFromPoint(point);
            if (day > 0) {
                SelectCalendarDay(day);
                return 0;
            }
            if (calendarPickerMode_ != CalendarPickerMode::Day) {
                calendarPickerMode_ = CalendarPickerMode::Day;
                InvalidateRect(hwnd_, nullptr, TRUE);
                return 0;
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            Paint(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            Paint(reinterpret_cast<HDC>(wParam));
            return 0;
        case WM_KEYDOWN:
            if (calendarFocused_ && HandleCalendarKey(wParam)) {
                return 0;
            }
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == IDOK) {
                Accept();
                return 0;
            }
            if (id == IDCANCEL) {
                if (ConfirmCloseIfDirty()) {
                    done_ = true;
                    DestroyWindow(hwnd_);
                }
                return 0;
            }
            if (id == IdRepeatNone) { SelectRepeatRule(RepeatRule::None); return 0; }
            if (id == IdRepeatDaily) { SelectRepeatRule(RepeatRule::Daily); return 0; }
            if (id == IdRepeatWorkday) { SelectRepeatRule(RepeatRule::Workday); return 0; }
            if (id == IdRepeatWeekly) { SelectRepeatRule(RepeatRule::Weekly); return 0; }
            if (id == IdRepeatMonthly) { SelectRepeatRule(RepeatRule::Monthly); return 0; }
            if (id == IdRepeatCustom) { SelectRepeatRule(RepeatRule::Custom); return 0; }
            if (id >= IdWeekdayBase && id < IdWeekdayBase + 7) {
                const int index = id - IdWeekdayBase;
                SetWeekday(index, !weekdaySelected_[index]);
                return 0;
            }
            if (id == IdMonthlyFixed) { SetTabChecked(monthlyFixedButton_, true); return 0; }
            if (id == IdAdvancedToggle) {
                advancedExpanded_ = !advancedExpanded_;
                Layout();
                return 0;
            }
            if (id == IdEndNever) { SelectEndMode(EndMode::Never); return 0; }
            if (id == IdEndCount) { SelectEndMode(EndMode::Count); return 0; }
            if (id == IdTitle && HIWORD(wParam) == EN_CHANGE) {
                UpdateSaveState();
                if (!Trim(GetText(titleEdit_)).empty() && !titleError_.empty()) {
                    titleError_.clear();
                    SetFieldError(titleEdit_, false);
                    Layout();
                }
            }
            if ((id == IdCustomUnit && HIWORD(wParam) == CBN_SELCHANGE) ||
                HIWORD(wParam) == EN_CHANGE) {
                if (id == IdMonthlyDay && HIWORD(wParam) == EN_CHANGE) {
                    ValidateMonthlyDayInline();
                }
                Layout();
            }
            return 0;
        }
        case WM_CLOSE:
            if (ConfirmCloseIfDirty()) {
                done_ = true;
                DestroyWindow(hwnd_);
            }
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND titleLabel_ = nullptr;
    HWND titleEdit_ = nullptr;
    HWND titleErrorText_ = nullptr;
    HWND contentLabel_ = nullptr;
    HWND contentEdit_ = nullptr;
    HWND timeLabel_ = nullptr;
    HWND timeEdit_ = nullptr;
    HWND repeatNone_ = nullptr;
    HWND repeatDaily_ = nullptr;
    HWND repeatWorkday_ = nullptr;
    HWND repeatWeekly_ = nullptr;
    HWND repeatMonthly_ = nullptr;
    HWND repeatCustom_ = nullptr;
    std::array<HWND, 7> weekdayButtons_{};
    HWND workdayHint_ = nullptr;
    HWND monthlyFixedButton_ = nullptr;
    HWND monthlyDayEdit_ = nullptr;
    HWND monthlyDayLabel_ = nullptr;
    HWND customPrefix_ = nullptr;
    HWND customIntervalEdit_ = nullptr;
    HWND customUnitCombo_ = nullptr;
    HWND customSuffix_ = nullptr;
    HWND repeatErrorText_ = nullptr;
    HWND advancedButton_ = nullptr;
    HWND endNeverButton_ = nullptr;
    HWND endCountButton_ = nullptr;
    HWND endCountEdit_ = nullptr;
    HWND okButton_ = nullptr;
    HWND cancelButton_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    const Theme& theme_;
    TodoItem& item_;
    TodoItem draft_;
    RECT reminderRect_{};
    RECT calendarRect_{};
    SYSTEMTIME selectedDate_{};
    std::array<bool, 7> weekdaySelected_{};
    RepeatRule repeatRule_ = RepeatRule::None;
    EndMode endMode_ = EndMode::Never;
    std::wstring titleError_;
    std::wstring reminderError_;
    std::wstring repeatError_;
    std::wstring initialSnapshot_;
    int scrollY_ = 0;
    int contentHeight_ = 0;
    int repeatLabelY_ = 0;
    int calendarYear_ = 1900;
    int calendarMonth_ = 1;
    int calendarYearPageStart_ = 2020;
    bool isNew_ = false;
    bool hasReminder_ = false;
    CalendarPickerMode calendarPickerMode_ = CalendarPickerMode::Day;
    bool calendarFocused_ = false;
    bool advancedExpanded_ = false;
    bool initialized_ = false;
    bool accepted_ = false;
    bool done_ = false;
    std::vector<FieldFrame> fields_;
};
}

bool TodoEditDialog::Show(HWND owner, HINSTANCE instance, const Theme& theme, TodoItem& item, bool isNew) {
    DialogWindow dialog(owner, instance, theme, item, isNew);
    return dialog.Run();
}
