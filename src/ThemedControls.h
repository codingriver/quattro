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

int ButtonHeight(const Theme& theme);
int CompactButtonHeight(const Theme& theme);
int ButtonPaddingX(const Theme& theme);
int ButtonTextHeight(const Theme& theme);
RECT ButtonTextRect(const Theme& theme, RECT frame, bool pressed = false);
int MiniButtonHeight(const Theme& theme);
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

bool Draw(const Theme& theme, const DRAWITEMSTRUCT* draw);

}
