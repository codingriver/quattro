#pragma once

#include "Theme.h"

#include <string>
#include <vector>
#include <windows.h>

namespace ThemedControls {

constexpr UINT WM_HOTKEY_CAPTURED = WM_APP + 0x361;

HWND CreateButton(
    HINSTANCE instance,
    HWND parent,
    int id,
    const wchar_t* text,
    int x,
    int y,
    int width,
    int height,
    HFONT font,
    bool defaultButton = false);

HWND CreatePrimaryButton(
    HINSTANCE instance,
    HWND parent,
    int id,
    const wchar_t* text,
    int x,
    int y,
    int width,
    int height,
    HFONT font,
    bool defaultButton = true);

void SetControlTheme(HWND hwnd, const Theme& theme);
bool IsControlHovered(HWND hwnd);

HWND CreateMiniButton(
    HINSTANCE instance,
    HWND parent,
    int id,
    const wchar_t* text,
    int x,
    int y,
    int width,
    int height,
    HFONT font);

HWND CreateCheckBox(
    HINSTANCE instance,
    HWND parent,
    int id,
    const wchar_t* text,
    int x,
    int y,
    int width,
    int height,
    HFONT font,
    bool checked);

HWND CreateToggle(
    HINSTANCE instance,
    HWND parent,
    int id,
    const wchar_t* text,
    int x,
    int y,
    int width,
    HFONT font,
    const Theme& theme,
    bool checked);

HWND CreateRadioButton(
    HINSTANCE instance,
    HWND parent,
    int id,
    const wchar_t* text,
    int x,
    int y,
    int width,
    HFONT font,
    const Theme& theme,
    int group,
    bool checked);

HWND CreateHotKeyCapture(
    HINSTANCE instance,
    HWND parent,
    int id,
    const wchar_t* text,
    int x,
    int y,
    int width,
    HFONT font,
    const Theme& theme);

HWND CreateLinkText(
    HINSTANCE instance,
    HWND parent,
    int id,
    const wchar_t* text,
    int x,
    int y,
    int width,
    int height,
    HFONT font,
    const Theme& theme,
    const wchar_t* role,
    bool visited,
    DWORD style);
void SetLinkRole(HWND hwnd, const wchar_t* role);
void SetLinkVisited(HWND hwnd, bool visited);

HWND CreateGroupBox(
    HINSTANCE instance, HWND parent, int id, const wchar_t* title,
    RECT frame, HFONT font, const Theme& theme, bool raised);
HWND CreateTabControlFrame(
    HINSTANCE instance, HWND parent, int id, RECT frame, HFONT font, const Theme& theme);
HWND CreateToolBarFrame(
    HINSTANCE instance, HWND parent, int id, RECT frame, HFONT font, const Theme& theme);
RECT GroupBoxContentRect(HWND hwnd);
HWND CreateSeparator(
    HINSTANCE instance, HWND parent, RECT frame, const Theme& theme, bool vertical);

void SetControlBackgroundComponent(HWND hwnd, const wchar_t* component);
const wchar_t* ControlBackgroundComponent(HWND hwnd);
void SetControlMultiline(HWND hwnd, bool multiline);

HWND CreateTabButton(
    HINSTANCE instance,
    HWND parent,
    int id,
    const wchar_t* text,
    int x,
    int y,
    int width,
    int height,
    HFONT font,
    bool selected);

void SetTabButtonSelected(HWND hwnd, bool selected);
bool IsTabButtonSelected(HWND hwnd);
void SetTabAppearance(HWND hwnd, int appearance);
int TabAppearance(HWND hwnd);
void SetTabOrientation(HWND hwnd, int orientation);
int TabOrientation(HWND hwnd);

HWND CreateComboBox(
    HINSTANCE instance,
    HWND parent,
    int id,
    int x,
    int y,
    int width,
    int height,
    HFONT font,
    const Theme& theme);

HWND CreateListBox(
    HINSTANCE instance,
    HWND parent,
    int id,
    int x,
    int y,
    int width,
    int height,
    HFONT font,
    const Theme& theme,
    DWORD extraStyle = LBS_NOTIFY | LBS_HASSTRINGS | WS_VSCROLL);

HFONT CreateDialogFont(UINT dpi = USER_DEFAULT_SCREEN_DPI);
HFONT CreateEditFont(const Theme& theme, UINT dpi = USER_DEFAULT_SCREEN_DPI);

HWND CreateStaticText(
    HINSTANCE instance,
    HWND parent,
    const wchar_t* text,
    int x,
    int y,
    int width,
    int height,
    HFONT font,
    DWORD style = SS_LEFT);

HWND CreateLabelText(
    HINSTANCE instance,
    HWND parent,
    const wchar_t* text,
    int x,
    int y,
    int width,
    const Theme& theme,
    HFONT font,
    DWORD style = SS_LEFT);

HWND CreateStatusBadge(
    HINSTANCE instance,
    HWND parent,
    const wchar_t* text,
    int x,
    int y,
    int width,
    const Theme& theme,
    HFONT font,
    const wchar_t* state = L"success");

void SetStatusBadgeState(HWND hwnd, const wchar_t* state);

HWND CreateStatusText(
    HINSTANCE instance,
    HWND parent,
    const wchar_t* text,
    int x,
    int y,
    int width,
    const Theme& theme,
    HFONT font,
    const wchar_t* state = L"normal",
    DWORD style = SS_CENTER);

void SetStatusTextState(HWND hwnd, const wchar_t* state);

int ButtonHeight(const Theme& theme);
int CompactButtonHeight(const Theme& theme);
int ButtonPaddingX(const Theme& theme);
int ButtonTextHeight(const Theme& theme);
RECT ButtonTextRect(const Theme& theme, RECT frame, bool pressed = false);
int MiniButtonHeight(const Theme& theme);
void DrawIconButtonFrame(
    const Theme& theme,
    HDC dc,
    RECT rect,
    bool hover = false,
    bool pressed = false,
    bool focused = false,
    bool disabled = false);
void DrawMiniButtonFrame(
    const Theme& theme,
    HDC dc,
    RECT rect,
    bool hover = false,
    bool pressed = false,
    bool focused = false,
    bool disabled = false);
int CheckBoxHeight(const Theme& theme);
int ToggleHeight(const Theme& theme);
int RadioButtonHeight(const Theme& theme);
RECT CheckBoxBoxRect(const Theme& theme, RECT frame);
RECT CheckBoxTextRect(const Theme& theme, RECT frame);
int TabButtonHeight(const Theme& theme);
RECT TabButtonTextRect(const Theme& theme, RECT frame);
RECT TabGroupInnerRect(const Theme& theme, RECT frame);
void DrawTabGroupFrame(const Theme& theme, HDC dc, RECT rect);
int ComboBoxHeight(const Theme& theme);
int ComboBoxItemHeight(const Theme& theme);
int ComboBoxDropdownHeight(const Theme& theme);
int ComboBoxContentWidth(const Theme& theme, int textWidth);
RECT ComboBoxItemTextRect(const Theme& theme, RECT frame);
int ListBoxItemHeight(const Theme& theme);
int ListBoxTwoLineItemHeight(const Theme& theme);
RECT ListItemTextRect(const Theme& theme, RECT frame);
RECT ListFrameInnerRect(const Theme& theme, RECT frame);
int LabelHeight(const Theme& theme);
RECT LabelTextRect(const Theme& theme, RECT frame);
int FieldFrameHeight(const Theme& theme);
RECT FieldTextRect(const Theme& theme, RECT frame);
int EditFrameHeight(const Theme& theme);
int EditPaddingX(const Theme& theme);
int EditTextHeight(const Theme& theme);
int EditFontSizePx(const Theme& theme);
RECT SingleLineEditRect(const Theme& theme, RECT frame);
RECT SingleLineEditRectForFrame(const Theme& theme, RECT frame);
RECT MultiLineEditRect(const Theme& theme, RECT frame);
void ConfigureEditBehavior(HWND hwnd, bool selectAllOnFocus);

HWND CreateSingleLineEdit(
    HINSTANCE instance,
    HWND parent,
    int id,
    const Theme& theme,
    RECT frame,
    const std::wstring& value,
    HFONT font,
    DWORD extraStyle = ES_AUTOHSCROLL);

HWND CreateMultiLineEdit(
    HINSTANCE instance,
    HWND parent,
    int id,
    const Theme& theme,
    RECT frame,
    const std::wstring& value,
    HFONT font,
    DWORD extraStyle = ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL);

HWND CreateFramedStatic(
    HINSTANCE instance,
    HWND parent,
    const Theme& theme,
    RECT frame,
    const std::wstring& value,
    HFONT font,
    DWORD style = SS_LEFTNOWORDWRAP);

HWND CreateProgressBar(
    HINSTANCE instance,
    HWND parent,
    int id,
    const Theme& theme,
    int x,
    int y,
    int width,
    int height);

void SetProgressBarValue(HWND hwnd, double value, bool indeterminate = false);
int ProgressBarHeight(const Theme& theme);

HWND CreateSlider(
    HINSTANCE instance,
    HWND parent,
    int id,
    const Theme& theme,
    int x,
    int y,
    int width,
    double minimum,
    double maximum,
    double step,
    double value);
void SetSliderValue(HWND hwnd, double value, bool notify = false);
double SliderValue(HWND hwnd);
int SliderHeight(const Theme& theme);

void DrawFieldFrame(
    const Theme& theme,
    HDC dc,
    RECT rect,
    HWND child,
    bool readOnly = false,
    bool error = false);

void DrawEditFrame(
    const Theme& theme,
    HDC dc,
    RECT rect,
    HWND child,
    bool readOnly = false,
    bool error = false);

void DrawComboFrame(
    const Theme& theme,
    HDC dc,
    RECT rect,
    HWND child);

void DrawListFrame(
    const Theme& theme,
    HDC dc,
    RECT rect,
    HWND child,
    bool readOnly = false);

void DrawPanelFrame(
    const Theme& theme,
    HDC dc,
    RECT rect,
    bool raised = false);
HWND CreatePanel(
    HINSTANCE instance,
    HWND parent,
    int id,
    RECT frame,
    HFONT font,
    const Theme& theme,
    const wchar_t* role = L"normal",
    bool scrollable = false);
RECT PanelContentRect(HWND hwnd);
void SetPanelRole(HWND hwnd, const wchar_t* role);

void ApplyListViewTheme(HWND list, const Theme& theme);
struct TableCellRuntime {
    int role = 0;
    int actionId = 0;
    bool hasImage = false;
};
void RegisterTable(HWND table, const Theme& theme);
void ConfigureTableColumns(HWND table, const std::vector<int>& widthModes);
void SetTableColumnResizeEnabled(HWND table, bool enabled);
void SetTableRowEnabledStates(HWND table, const std::vector<bool>& enabled);
void SetTableCells(HWND table, const std::vector<std::vector<TableCellRuntime>>& cells);
void RestoreTableDefaultImageList(HWND table);
bool IsTableRowEnabled(HWND table, int index);
bool TableCellAction(HWND table, int row, int column, int& actionId);
bool Draw(const Theme& theme, const DRAWITEMSTRUCT* draw);
bool HandleListViewCustomDraw(const Theme& theme, LPARAM lParam, LRESULT& result);

}
