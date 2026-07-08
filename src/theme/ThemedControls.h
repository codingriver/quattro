#pragma once

#include "Theme.h"

#include <string>
#include <windows.h>

namespace ThemedControls {

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

void SetControlBackgroundComponent(HWND hwnd, const wchar_t* component);
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

HFONT CreateDialogFont();
HFONT CreateEditFont(const Theme& theme);

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

void DrawFieldFrame(
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

void ApplyListViewTheme(HWND list, const Theme& theme);
bool Draw(const Theme& theme, const DRAWITEMSTRUCT* draw);
bool HandleListViewCustomDraw(const Theme& theme, LPARAM lParam, LRESULT& result);

}
