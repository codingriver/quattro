#include "StartupService.h"

#include "Utilities.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <windows.h>

namespace {
constexpr const wchar_t* kAppDisplayName = L"Quattro快速启动器";
constexpr const wchar_t* kStartupShortcutName = L"Quattro快速启动器.lnk";
constexpr const wchar_t* kLegacyStartupShortcutName = L"Quattro.lnk";

std::filesystem::path StartupShortcutPath() {
    PWSTR folder = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Startup, KF_FLAG_CREATE, nullptr, &folder)) && folder) {
        result = std::filesystem::path(folder) / kStartupShortcutName;
        CoTaskMemFree(folder);
    }
    return result;
}

std::filesystem::path LegacyStartupShortcutPath() {
    PWSTR folder = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Startup, KF_FLAG_CREATE, nullptr, &folder)) && folder) {
        result = std::filesystem::path(folder) / kLegacyStartupShortcutName;
        CoTaskMemFree(folder);
    }
    return result;
}

std::filesystem::path CurrentExecutablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD copied = 0;
    for (;;) {
        copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return GetModuleDirectory() / L"Quattro.exe";
        }
        if (copied < buffer.size() - 1) {
            buffer.resize(copied);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
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
    shellLink->SetDescription(kAppDisplayName);
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

bool SyncStartupShortcut(const std::filesystem::path&, bool enabled, std::wstring& error) {
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
        const bool created = CreateShortcut(shortcutPath, CurrentExecutablePath(), error);
        if (created) {
            const std::filesystem::path legacyShortcutPath = LegacyStartupShortcutPath();
            if (!legacyShortcutPath.empty() && legacyShortcutPath != shortcutPath) {
                std::filesystem::remove(legacyShortcutPath, ec);
            }
        }
        return created;
    }

    if (std::filesystem::exists(shortcutPath, ec)) {
        std::filesystem::remove(shortcutPath, ec);
        if (ec) {
            error = L"删除开机自启动快捷方式失败。";
            return false;
        }
    }
    const std::filesystem::path legacyShortcutPath = LegacyStartupShortcutPath();
    if (!legacyShortcutPath.empty() && std::filesystem::exists(legacyShortcutPath, ec)) {
        std::filesystem::remove(legacyShortcutPath, ec);
        if (ec) {
            error = L"删除旧开机自启动快捷方式失败。";
            return false;
        }
    }
    return true;
}

bool StartupShortcutExists(const std::filesystem::path&) {
    std::error_code ec;
    const std::filesystem::path shortcutPath = StartupShortcutPath();
    if (!shortcutPath.empty() && std::filesystem::is_regular_file(shortcutPath, ec)) {
        return true;
    }
    const std::filesystem::path legacyShortcutPath = LegacyStartupShortcutPath();
    return !legacyShortcutPath.empty() && std::filesystem::is_regular_file(legacyShortcutPath, ec);
}
