#pragma once

#include "ThemedUi.h"

#include <memory>
#include <string>
#include <windows.h>

enum class ThemedWindowPlacement {
    CenterOwner,
    OffsetOwner,
    Manual,
};

struct ThemedWindowCreateOptions {
    HINSTANCE instance = nullptr;
    HWND owner = nullptr;
    const wchar_t* className = nullptr;
    const wchar_t* title = nullptr;
    WNDPROC wndProc = nullptr;
    void* createParam = nullptr;
    int clientWidth = 0;
    int clientHeight = 0;
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_CLIPCHILDREN;
    DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
    ThemedWindowPlacement placement = ThemedWindowPlacement::CenterOwner;
    int x = 0;
    int y = 0;
    int offsetX = 0;
    int offsetY = 0;
    HICON icon = nullptr;
    HICON smallIcon = nullptr;
    HCURSOR cursor = nullptr;
};

constexpr int kThemedDialogClientWidth = 460;
constexpr int kThemedDialogClientHeight = 246;
constexpr DialogLayoutKind kThemedDialogLayoutKind = DialogLayoutKind::Compact;

class ThemedWindowUi {
public:
    ThemedWindowUi(
        HINSTANCE instance,
        HWND owner,
        HWND hwnd,
        const Theme& theme,
        DialogLayoutKind layoutKind,
        int clientWidth,
        int clientHeight);
    ~ThemedWindowUi();

    ThemedWindowUi(const ThemedWindowUi&) = delete;
    ThemedWindowUi& operator=(const ThemedWindowUi&) = delete;

    static SIZE AdjustedWindowSize(int clientWidth, int clientHeight, DWORD style, DWORD exStyle, bool hasMenu = false);
    static POINT WindowPosition(const ThemedWindowCreateOptions& options, int windowWidth, int windowHeight);
    static ThemedWindowCreateOptions DialogOptions(
        HINSTANCE instance,
        HWND owner,
        const wchar_t* className,
        const wchar_t* title,
        WNDPROC wndProc,
        void* createParam,
        HICON icon = nullptr,
        HICON smallIcon = nullptr);
    static HWND CreateWindowHandle(const ThemedWindowCreateOptions& options, std::wstring* error = nullptr);
    static bool HandleCommonMessage(
        std::unique_ptr<ThemedWindowUi>& windowUi,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        LRESULT& result);

    HWND hwnd() const { return hwnd_; }
    HFONT font() const;
    ThemedUi ui() const;

    bool ShowModal();
    void RestoreModalOwner();
    bool HandleMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);

private:
    static COLORREF ToColorRef(Color color);
    HBRUSH BackgroundBrush();
    HBRUSH EditBrush();
    void ReleaseResources();

    HINSTANCE instance_ = nullptr;
    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    const Theme& theme_;
    DialogLayoutKind layoutKind_ = DialogLayoutKind::Compact;
    int clientWidth_ = 0;
    int clientHeight_ = 0;
    mutable HFONT font_ = nullptr;
    mutable bool ownsFont_ = false;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
};
