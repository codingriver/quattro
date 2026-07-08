#include "Elevation.h"

#include "Utilities.h"

#include <exdisp.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlwapi.h>

namespace {
std::wstring CurrentExecutablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        DWORD copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return {};
        }
        if (copied < buffer.size() - 1) {
            buffer.resize(copied);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

constexpr const wchar_t* kPrivilegeRestartArgument = L"--quattro-privilege-restart";

bool ShellExecuteCurrent(HWND owner, const wchar_t* verb, std::wstring& errorMessage) {
    const std::wstring executable = CurrentExecutablePath();
    if (executable.empty()) {
        errorMessage = FormatLastError(GetLastError());
        return false;
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.hwnd = owner;
    info.lpVerb = verb;
    info.lpFile = executable.c_str();
    info.lpParameters = kPrivilegeRestartArgument;
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        errorMessage = FormatLastError(GetLastError());
        return false;
    }
    return true;
}

bool RestartCurrentViaExplorer(std::wstring& errorMessage) {
    IShellWindows* shellWindows = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&shellWindows));
    if (FAILED(hr)) {
        errorMessage = FormatLastError(HRESULT_CODE(hr));
        return false;
    }

    VARIANT location{};
    VariantInit(&location);
    V_VT(&location) = VT_I4;
    V_I4(&location) = CSIDL_DESKTOP;

    VARIANT empty{};
    VariantInit(&empty);

    long hwnd = 0;
    IDispatch* dispatch = nullptr;
    hr = shellWindows->FindWindowSW(&location, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &dispatch);
    shellWindows->Release();
    if (FAILED(hr) || !dispatch) {
        errorMessage = L"无法找到桌面 Shell 进程。";
        return false;
    }

    IShellBrowser* shellBrowser = nullptr;
    hr = IUnknown_QueryService(dispatch, SID_STopLevelBrowser, IID_PPV_ARGS(&shellBrowser));
    dispatch->Release();
    if (FAILED(hr) || !shellBrowser) {
        errorMessage = L"无法访问桌面 Shell 浏览器。";
        return false;
    }

    IShellView* shellView = nullptr;
    hr = shellBrowser->QueryActiveShellView(&shellView);
    shellBrowser->Release();
    if (FAILED(hr) || !shellView) {
        errorMessage = L"无法访问桌面 Shell 视图。";
        return false;
    }

    IDispatch* viewDispatch = nullptr;
    hr = shellView->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&viewDispatch));
    shellView->Release();
    if (FAILED(hr) || !viewDispatch) {
        errorMessage = L"无法访问桌面 Shell 分发对象。";
        return false;
    }

    IShellFolderViewDual* folderView = nullptr;
    hr = viewDispatch->QueryInterface(IID_PPV_ARGS(&folderView));
    viewDispatch->Release();
    if (FAILED(hr) || !folderView) {
        errorMessage = L"无法访问桌面 Shell 文件夹视图。";
        return false;
    }

    IDispatch* appDispatch = nullptr;
    hr = folderView->get_Application(&appDispatch);
    folderView->Release();
    if (FAILED(hr) || !appDispatch) {
        errorMessage = L"无法访问桌面 Shell 应用对象。";
        return false;
    }

    IShellDispatch2* shellDispatch = nullptr;
    hr = appDispatch->QueryInterface(IID_PPV_ARGS(&shellDispatch));
    appDispatch->Release();
    if (FAILED(hr) || !shellDispatch) {
        errorMessage = L"无法访问桌面 Shell 调度接口。";
        return false;
    }

    const std::wstring executable = CurrentExecutablePath();
    BSTR file = SysAllocString(executable.c_str());
    VARIANT args{};
    VARIANT dir{};
    VARIANT op{};
    VARIANT show{};
    VariantInit(&args);
    VariantInit(&dir);
    VariantInit(&op);
    VariantInit(&show);
    V_VT(&args) = VT_BSTR;
    V_BSTR(&args) = SysAllocString(kPrivilegeRestartArgument);
    V_VT(&show) = VT_I4;
    V_I4(&show) = SW_SHOWNORMAL;

    hr = shellDispatch->ShellExecute(file, args, dir, op, show);
    SysFreeString(file);
    VariantClear(&args);
    shellDispatch->Release();
    if (FAILED(hr)) {
        errorMessage = FormatLastError(HRESULT_CODE(hr));
        return false;
    }
    return true;
}
}

bool IsRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID administratorsGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(
            &ntAuthority,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0,
            0,
            0,
            0,
            0,
            0,
            &administratorsGroup)) {
        CheckTokenMembership(nullptr, administratorsGroup, &isAdmin);
        FreeSid(administratorsGroup);
    }
    return isAdmin != FALSE;
}

bool RestartCurrentProcessElevated(HWND owner, std::wstring& errorMessage) {
    errorMessage.clear();
    return ShellExecuteCurrent(owner, L"runas", errorMessage);
}

bool RestartCurrentProcessUnelevated(HWND owner, std::wstring& errorMessage) {
    (void)owner;
    errorMessage.clear();
    if (RestartCurrentViaExplorer(errorMessage)) {
        return true;
    }
    if (errorMessage.empty()) {
        errorMessage = L"无法通过桌面 Shell 以普通用户重启。";
    }
    return false;
}
