#include "SelectedPathCopyService.h"

#include "Utilities.h"

#include <exdisp.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>

namespace {
template <typename T>
void SafeRelease(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

bool IsDesktopWindow(HWND window) {
    if (!window) {
        return false;
    }
    wchar_t className[128]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (_wcsicmp(className, L"Progman") == 0 || _wcsicmp(className, L"WorkerW") == 0) {
        return true;
    }
    return FindWindowExW(window, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr;
}

IShellView* ShellViewFromDispatch(IDispatch* dispatch) {
    if (!dispatch) {
        return nullptr;
    }
    IShellBrowser* browser = nullptr;
    if (FAILED(IUnknown_QueryService(dispatch, SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) || !browser) {
        return nullptr;
    }
    IShellView* view = nullptr;
    browser->QueryActiveShellView(&view);
    SafeRelease(browser);
    return view;
}

IShellView* DesktopShellView(IShellWindows* shellWindows) {
    VARIANT location{};
    VariantInit(&location);
    V_VT(&location) = VT_I4;
    V_I4(&location) = CSIDL_DESKTOP;

    VARIANT empty{};
    VariantInit(&empty);
    long shellWindow = 0;
    IDispatch* dispatch = nullptr;
    const HRESULT hr = shellWindows->FindWindowSW(
        &location, &empty, SWC_DESKTOP, &shellWindow, SWFO_NEEDDISPATCH, &dispatch);
    if (FAILED(hr) || !dispatch) {
        SafeRelease(dispatch);
        return nullptr;
    }
    IShellView* view = ShellViewFromDispatch(dispatch);
    SafeRelease(dispatch);
    return view;
}

IShellView* ForegroundExplorerShellView(IShellWindows* shellWindows, HWND foregroundWindow) {
    const HWND foregroundRoot = GetAncestor(foregroundWindow, GA_ROOT);
    long count = 0;
    if (FAILED(shellWindows->get_Count(&count))) {
        return nullptr;
    }

    for (long index = 0; index < count; ++index) {
        VARIANT itemIndex{};
        VariantInit(&itemIndex);
        V_VT(&itemIndex) = VT_I4;
        V_I4(&itemIndex) = index;

        IDispatch* dispatch = nullptr;
        if (FAILED(shellWindows->Item(itemIndex, &dispatch)) || !dispatch) {
            SafeRelease(dispatch);
            continue;
        }

        IWebBrowserApp* browserApp = nullptr;
        SHANDLE_PTR browserHandle = 0;
        const HRESULT browserHr = dispatch->QueryInterface(IID_PPV_ARGS(&browserApp));
        if (SUCCEEDED(browserHr) && browserApp) {
            browserApp->get_HWND(&browserHandle);
        }
        SafeRelease(browserApp);

        const HWND browserWindow = reinterpret_cast<HWND>(browserHandle);
        const HWND browserRoot = browserWindow ? GetAncestor(browserWindow, GA_ROOT) : nullptr;
        if (browserWindow == foregroundWindow || browserWindow == foregroundRoot || browserRoot == foregroundRoot) {
            IShellView* view = ShellViewFromDispatch(dispatch);
            SafeRelease(dispatch);
            if (view) {
                return view;
            }
        } else {
            SafeRelease(dispatch);
        }
    }
    return nullptr;
}

SelectedPathCopyResult ReadSelectedPaths(IShellView* view, std::vector<std::wstring>& paths) {
    IDataObject* dataObject = nullptr;
    const HRESULT selectionHr = view->GetItemObject(SVGIO_SELECTION, IID_PPV_ARGS(&dataObject));
    if (FAILED(selectionHr) || !dataObject) {
        SafeRelease(dataObject);
        return {SelectedPathCopyStatus::NoSelection, 0, {}};
    }

    IShellItemArray* items = nullptr;
    const HRESULT arrayHr = SHCreateShellItemArrayFromDataObject(dataObject, IID_PPV_ARGS(&items));
    SafeRelease(dataObject);
    if (FAILED(arrayHr) || !items) {
        SafeRelease(items);
        return {SelectedPathCopyStatus::ShellUnavailable, 0, FormatLastError(HRESULT_CODE(arrayHr))};
    }

    DWORD count = 0;
    if (FAILED(items->GetCount(&count))) {
        SafeRelease(items);
        return {SelectedPathCopyStatus::ShellUnavailable, 0, L"无法读取资源管理器选中项数量。"};
    }
    if (count == 0) {
        SafeRelease(items);
        return {SelectedPathCopyStatus::NoSelection, 0, {}};
    }

    paths.reserve(count);
    for (DWORD index = 0; index < count; ++index) {
        IShellItem* item = nullptr;
        if (FAILED(items->GetItemAt(index, &item)) || !item) {
            SafeRelease(item);
            SafeRelease(items);
            return {SelectedPathCopyStatus::ShellUnavailable, 0, L"无法读取资源管理器选中项。"};
        }

        PWSTR fileSystemPath = nullptr;
        const HRESULT pathHr = item->GetDisplayName(SIGDN_FILESYSPATH, &fileSystemPath);
        SafeRelease(item);
        if (FAILED(pathHr) || !fileSystemPath || !*fileSystemPath) {
            if (fileSystemPath) {
                CoTaskMemFree(fileSystemPath);
            }
            SafeRelease(items);
            return {SelectedPathCopyStatus::NonFileSystemItem, 0, {}};
        }
        paths.emplace_back(fileSystemPath);
        CoTaskMemFree(fileSystemPath);
    }
    SafeRelease(items);
    return {SelectedPathCopyStatus::Success, paths.size(), {}};
}
}

std::wstring SelectedPathCopyService::FormatPaths(const std::vector<std::wstring>& paths) {
    std::wstring text;
    for (const auto& path : paths) {
        if (!text.empty()) {
            text += L"\r\n";
        }
        text += path;
    }
    return text;
}

bool SelectedPathCopyService::WriteClipboardText(HWND owner, const std::wstring& text, std::wstring& errorMessage) {
    errorMessage.clear();
    if (text.empty()) {
        errorMessage = L"没有可复制的路径。";
        return false;
    }

    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        errorMessage = FormatLastError(GetLastError());
        return false;
    }
    void* target = GlobalLock(memory);
    if (!target) {
        errorMessage = FormatLastError(GetLastError());
        GlobalFree(memory);
        return false;
    }
    memcpy(target, text.c_str(), bytes);
    GlobalUnlock(memory);

    const DWORD deadline = GetTickCount() + 250;
    while (!OpenClipboard(owner)) {
        if (static_cast<LONG>(GetTickCount() - deadline) >= 0) {
            errorMessage = FormatLastError(GetLastError());
            GlobalFree(memory);
            return false;
        }
        Sleep(10);
    }

    bool success = false;
    if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory)) {
        memory = nullptr;
        success = true;
    } else {
        errorMessage = FormatLastError(GetLastError());
    }
    CloseClipboard();
    if (memory) {
        GlobalFree(memory);
    }
    return success;
}

SelectedPathCopyResult SelectedPathCopyService::CopySelectedPaths(HWND foregroundWindow, HWND clipboardOwner) {
    if (!foregroundWindow) {
        return {SelectedPathCopyStatus::UnsupportedForegroundWindow, 0, {}};
    }

    IShellWindows* shellWindows = nullptr;
    const HRESULT shellHr = CoCreateInstance(
        CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&shellWindows));
    if (FAILED(shellHr) || !shellWindows) {
        SafeRelease(shellWindows);
        return {SelectedPathCopyStatus::ShellUnavailable, 0, FormatLastError(HRESULT_CODE(shellHr))};
    }

    const HWND foregroundRoot = GetAncestor(foregroundWindow, GA_ROOT);
    IShellView* view = IsDesktopWindow(foregroundWindow) || IsDesktopWindow(foregroundRoot)
        ? DesktopShellView(shellWindows)
        : ForegroundExplorerShellView(shellWindows, foregroundWindow);
    SafeRelease(shellWindows);
    if (!view) {
        return {SelectedPathCopyStatus::UnsupportedForegroundWindow, 0, {}};
    }

    std::vector<std::wstring> paths;
    SelectedPathCopyResult result = ReadSelectedPaths(view, paths);
    SafeRelease(view);
    if (result.status != SelectedPathCopyStatus::Success) {
        return result;
    }

    std::wstring clipboardError;
    if (!WriteClipboardText(clipboardOwner, FormatPaths(paths), clipboardError)) {
        return {SelectedPathCopyStatus::ClipboardUnavailable, 0, std::move(clipboardError)};
    }
    result.copiedCount = paths.size();
    return result;
}
