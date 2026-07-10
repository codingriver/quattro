#pragma once

#include "DialogLayout.h"
#include "ThemedControls.h"

#include <string>
#include <windows.h>

enum class ThemedButtonRole {
    Normal,
    Primary,
    Mini,
    Tab,
};

enum class ThemedButtonSize {
    Normal,
    Compact,
    Mini,
};

enum class ThemedButtonWidthMode {
    Fixed,
    Text,
};

// ThemedUi is the preferred facade for dialog/tool-window UI code.
//
// It keeps three responsibilities together:
// 1. Common component creation, so controls always receive the same theme
//    binding, font, subclass, owner-draw flags, and cached text behavior.
// 2. Common compact dialog layout helpers, so windows do not hand-roll footer
//    button alignment or content insets.
// 3. Parent message dispatch for owner-draw controls, so a new window does not
//    accidentally create a themed button/progress bar that nobody paints.
//
// Window classes can still use ThemedControls directly for low-level drawing or
// special cases. New ordinary controls should go through this facade first.
class ThemedUi {
public:
    ThemedUi(
        HINSTANCE instance,
        HWND parent,
        const Theme& theme,
        HFONT font,
        DialogLayoutKind layoutKind,
        int clientWidth,
        int clientHeight);

    const DialogLayoutMetrics& layout() const { return layout_; }
    const Theme& theme() const { return theme_; }
    int clientWidth() const { return clientWidth_; }
    int clientHeight() const { return clientHeight_; }

    // Layout accessors. They expose the shared dialog metrics instead of
    // letting each window duplicate inset, footer, and row-spacing math.
    int contentLeft() const { return layout_.contentInsetX; }
    int contentTop() const { return layout_.contentInsetY; }
    int contentWidth() const { return clientWidth_ - layout_.contentInsetX * 2; }
    RECT contentRect() const { return layout_.ContentRect(clientWidth_, clientHeight_); }

    int labelHeight() const;
    int buttonHeight() const;
    int buttonHeight(ThemedButtonRole role, ThemedButtonSize size) const;
    int compactButtonHeight() const;
    int progressBarHeight() const;
    int buttonWidth(
        const std::wstring& text,
        ThemedButtonRole role,
        ThemedButtonSize size,
        ThemedButtonWidthMode widthMode,
        int fixedWidth = 0) const;
    int textWidth(const std::wstring& text) const;
    int footerButtonX(int buttonIndex, int buttonCount) const;
    int footerButtonY(int buttonHeight) const;
    int centeredGroupX(int groupWidth) const;
    int nextRowY(int y, int controlHeight) const;
    RECT rect(int x, int y, int width, int height) const;

    // Common component factories. These wrap ThemedControls and perform the
    // extra theme binding needed by owner-draw controls. Callers should pass
    // only semantic values, text, and geometry; visual styling stays in Theme.
    HWND Label(const std::wstring& text, int x, int y, int width, DWORD style = SS_LEFT) const;
    HWND StatusText(const std::wstring& text, int x, int y, int width, const wchar_t* state = L"normal", DWORD style = SS_CENTER) const;
    // Button is the single public entry point for creating any themed button.
    // Height is fixed by (role, size) templates and cannot be overridden by
    // callers; only width is adjustable (fixed width, or measured from text).
    HWND Button(
        int id,
        const std::wstring& text,
        int x,
        int y,
        ThemedButtonRole role = ThemedButtonRole::Normal,
        ThemedButtonSize size = ThemedButtonSize::Normal,
        ThemedButtonWidthMode widthMode = ThemedButtonWidthMode::Fixed,
        int fixedWidth = 0,
        bool defaultButton = false,
        bool selected = false) const;

    // FooterButton uses DialogLayoutMetrics::FooterButtonX/Y internally, so
    // every dialog gets the same compact footer alignment and spacing.
    HWND FooterButton(int id, const std::wstring& text, int buttonIndex, int buttonCount, bool primary = false, bool defaultButton = false) const;
    HWND CheckBox(int id, const std::wstring& text, int x, int y, int width, int height, bool checked = false) const;
    // TabButton height is fixed by the Tab template; only width is adjustable.
    HWND TabButton(int id, const std::wstring& text, int x, int y, int width, bool selected = false) const;
    HWND ComboBox(int id, int x, int y, int width, int height) const;
    HWND ListBox(int id, int x, int y, int width, int height, DWORD extraStyle = LBS_NOTIFY | LBS_HASSTRINGS | WS_VSCROLL) const;
    HWND SingleLineEdit(int id, RECT frame, const std::wstring& value, DWORD extraStyle = ES_AUTOHSCROLL) const;
    HWND MultiLineEdit(int id, RECT frame, const std::wstring& value, DWORD extraStyle = ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL) const;
    HWND FramedStatic(const std::wstring& value, RECT frame, DWORD style = SS_LEFTNOWORDWRAP) const;
    HWND ProgressBar(int id, int x, int y, int width, int height = 0) const;

    // Call this from a themed window's WndProc before falling back to
    // DefWindowProc. It centralizes the owner-draw/custom-draw bridge required
    // by common controls created through this facade.
    static bool HandleParentMessage(const Theme& theme, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);

private:
    // Internal creators that take an already-resolved template height. Kept
    // private so that dialog code cannot bypass the (role, size) height
    // templates and hand-roll button heights.
    HWND NormalButton(int id, const std::wstring& text, int x, int y, int width, int height, bool defaultButton) const;
    HWND PrimaryButton(int id, const std::wstring& text, int x, int y, int width, int height, bool defaultButton) const;
    HWND BindTheme(HWND hwnd) const;

    HINSTANCE instance_ = nullptr;
    HWND parent_ = nullptr;
    const Theme& theme_;
    HFONT font_ = nullptr;
    DialogLayoutMetrics layout_{};
    int clientWidth_ = 0;
    int clientHeight_ = 0;
};
