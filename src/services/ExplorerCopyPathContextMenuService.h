#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

struct ExplorerCopyPathContextMenuState {
    bool registered = false;
    bool matchesExecutable = false;
};

class ExplorerCopyPathContextMenuService {
public:
    ExplorerCopyPathContextMenuService(
        HKEY root = HKEY_CURRENT_USER,
        std::wstring keyPath = L"Software\\Classes\\AllFilesystemObjects\\shell\\Quattro.CopyAbsolutePath",
        bool notifyShell = true);

    static std::wstring BuildCommand(const std::filesystem::path& executablePath);
    static std::wstring BuildIconValue(const std::filesystem::path& executablePath);
    static std::filesystem::path CurrentExecutablePath();
    static ExplorerCopyPathContextMenuService ForCurrentProcess();

    ExplorerCopyPathContextMenuState Query(const std::filesystem::path& executablePath) const;
    bool Register(const std::filesystem::path& executablePath, std::wstring& error) const;
    bool Unregister(std::wstring& error) const;
    bool Reconcile(bool enabled, const std::filesystem::path& executablePath, std::wstring& error) const;

private:
    bool WriteStringValue(HKEY key, const wchar_t* name, const std::wstring& value, std::wstring& error) const;
    void NotifyShellIfNeeded() const;

    HKEY root_ = HKEY_CURRENT_USER;
    std::wstring keyPath_;
    bool notifyShell_ = true;
};
