#include "StartupService.h"

#include "Utilities.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <windows.h>

namespace {
std::filesystem::path StartupShortcutPath() {
    PWSTR folder = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Startup, KF_FLAG_CREATE, nullptr, &folder)) && folder) {
        result = std::filesystem::path(folder) / L"Quattro.lnk";
        CoTaskMemFree(folder);
    }
    return result;
}

bool CreateShortcut(const std::filesystem::path& shortcutPath, const std::filesystem::path& exePath, std::wstring& error) {
    IShellLinkW* shellLink = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink));
    if (FAILED(hr) || !shellLink) {
        error = L"创建快捷方式对象失败。";
        return false;
    }

    shellLink->SetPath(exePath.c_str());
    shellLink->SetWorkingDirectory(exePath.parent_path().c_str());
    shellLink->SetDescription(L"Quattro");
    shellLink->SetIconLocation(exePath.c_str(), 0);

    IPersistFile* persistFile = nullptr;
    hr = shellLink->QueryInterface(IID_PPV_ARGS(&persistFile));
    if (SUCCEEDED(hr) && persistFile) {
        hr = persistFile->Save(shortcutPath.c_str(), TRUE);
        persistFile->Release();
    }
    shellLink->Release();

    if (FAILED(hr)) {
        error = L"保存开机自启动快捷方式失败。";
        return false;
    }
    return true;
}
}

bool SyncStartupShortcut(const std::filesystem::path& appDirectory, bool enabled, std::wstring& error) {
    error.clear();
    const std::filesystem::path shortcutPath = StartupShortcutPath();
    if (shortcutPath.empty()) {
        error = L"无法定位当前用户 Startup 目录。";
        return false;
    }

    std::error_code ec;
    if (enabled) {
        std::filesystem::create_directories(shortcutPath.parent_path(), ec);
        if (ec) {
            error = L"无法创建 Startup 目录。";
            return false;
        }
        return CreateShortcut(shortcutPath, appDirectory / L"Quattro.exe", error);
    }

    if (std::filesystem::exists(shortcutPath, ec)) {
        std::filesystem::remove(shortcutPath, ec);
        if (ec) {
            error = L"删除开机自启动快捷方式失败。";
            return false;
        }
    }
    return true;
}

bool StartupShortcutExists(const std::filesystem::path&) {
    std::error_code ec;
    const std::filesystem::path shortcutPath = StartupShortcutPath();
    return !shortcutPath.empty() && std::filesystem::is_regular_file(shortcutPath, ec);
}
