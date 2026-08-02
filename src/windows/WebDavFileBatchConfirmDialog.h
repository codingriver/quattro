#pragma once

#include "Theme.h"

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

class ThemedWindowUi;

struct WebDavFileBatchConfirmItem {
    std::wstring name;
    std::wstring sizeText;
    std::wstring localPath;
    std::wstring status;
    bool actionable = true;
};

class WebDavFileBatchConfirmDialog final {
public:
    WebDavFileBatchConfirmDialog(
        HWND owner,
        HINSTANCE instance,
        const Theme& theme,
        std::wstring title,
        std::wstring intro,
        std::wstring confirmText,
        std::vector<WebDavFileBatchConfirmItem> items,
        bool danger = false);

    bool Run();

private:
    std::wstring DetailsText() const;
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    const Theme& theme_;
    std::wstring title_;
    std::wstring intro_;
    std::wstring confirmText_;
    std::vector<WebDavFileBatchConfirmItem> items_;
    bool danger_ = false;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    bool accepted_ = false;
    bool done_ = false;
};
