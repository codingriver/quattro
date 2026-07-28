#pragma once

#include "Theme.h"
#include "WebDavFileService.h"

#include <windows.h>

#include <memory>
#include <string>

class ThemedWindowUi;

class WebDavFileDetailsDialog final {
public:
    WebDavFileDetailsDialog(
        HWND owner,
        HINSTANCE instance,
        const Theme& theme,
        WebDavFileRecord record,
        std::wstring remoteRecordPath);

    bool Run();

private:
    std::wstring DetailsText() const;
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    const Theme& theme_;
    WebDavFileRecord record_;
    std::wstring remoteRecordPath_;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    bool done_ = false;
};
