#pragma once

#include "ThemedUi.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
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
    bool scaleForDpi = true;
    UINT logicalDpi = USER_DEFAULT_SCREEN_DPI;
};

constexpr int kThemedDialogClientWidth = 460;
constexpr int kThemedDialogClientHeight = 246;
constexpr DialogLayoutKind kThemedDialogLayoutKind = DialogLayoutKind::Compact;

class ThemedWindowUi : public ThemedEditFrameRegistry, public ThemedTableFrameRegistry, public ThemedTooltipRegistry {
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

    static SIZE AdjustedWindowSize(int clientWidth, int clientHeight, DWORD style, DWORD exStyle, bool hasMenu = false, UINT dpi = USER_DEFAULT_SCREEN_DPI);
    static UINT TargetDpi(const ThemedWindowCreateOptions& options);
    static int ScaleForDpi(int logicalPixels, UINT dpi, UINT logicalDpi = USER_DEFAULT_SCREEN_DPI);
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
    UINT dpi() const { return dpi_; }
    HFONT font() const;
    ThemedUi ui() const;

    bool ShowModal();
    void RestoreModalOwner();
    bool HandleMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);
    void RegisterEditFrame(HWND child, RECT frame, const ThemedEditOptions& options) override;
    void RegisterTableFrame(HWND child, RECT frame) override;
    void UnregisterTableFrame(HWND child);
    void UnregisterEditFrame(HWND child);
    void MoveEditFrame(HWND child, RECT frame);
    void SetEditFrameState(HWND child, bool readOnly, bool error);
    void SetEditReadOnly(HWND child, bool readOnly);
    void SetEditError(HWND child, bool error);
    void SetEditEnabled(HWND child, bool enabled);
    void SetEditPlaceholder(HWND child, const std::wstring& placeholder);
    void DrawRegisteredEditFrames(HDC dc) const;
    void DrawRegisteredTableFrames(HDC dc) const;
    void ShowTooltip(const std::wstring& text, POINT screenPoint, const ThemedTooltipOptions& options) override;
    void HideTooltip() override;
    void FillBackground(HDC dc) const;
    void InvalidateEditFrame(HWND child) const;

private:
    struct EditFrame {
        HWND child = nullptr;
        HWND frameWindow = nullptr;
        RECT frame{};
        ThemedEditOptions options{};
    };

    static COLORREF ToColorRef(Color color);
    HBRUSH BackgroundBrush();
    HBRUSH BrushForColor(COLORREF color);
    HBRUSH ApplyEditColors(HDC dc, HWND child);
    EditFrame* FindEditFrame(HWND child);
    const EditFrame* FindEditFrame(HWND child) const;
    EditFrame* FindEditFrameWindow(HWND frameWindow);
    const EditFrame* FindEditFrameWindow(HWND frameWindow) const;
    const wchar_t* EditState(const EditFrame& editFrame) const;
    bool EnsureEditFrameClass();
    void SyncEditFrameWindow(EditFrame& editFrame);
    void PaintEditFrameWindow(HWND frameWindow, HDC dc) const;
    void ReleaseResources();
    void ApplyDpiChange(UINT newDpi, const RECT* suggestedWindowRect);
    bool EnsureTooltipWindow();
    SIZE MeasureTooltip(const std::wstring& text, const ThemedTooltipOptions& options) const;
    void PaintTooltip(HDC dc) const;
    static LRESULT CALLBACK EditFrameProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK EditChildProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data);
    static LRESULT CALLBACK TooltipProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    HINSTANCE instance_ = nullptr;
    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    const Theme& theme_;
    DialogLayoutKind layoutKind_ = DialogLayoutKind::Compact;
    int clientWidth_ = 0;
    int clientHeight_ = 0;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    mutable HFONT font_ = nullptr;
    mutable bool ownsFont_ = false;
    HBRUSH backgroundBrush_ = nullptr;
    std::unordered_map<COLORREF, HBRUSH> colorBrushes_;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
    std::vector<EditFrame> editFrames_;
    struct TableFrame {
        HWND child = nullptr;
        RECT frame{};
    };
    std::vector<TableFrame> tableFrames_;
    HWND tooltip_ = nullptr;
    std::wstring tooltipText_;
    ThemedTooltipOptions tooltipOptions_{};
};
