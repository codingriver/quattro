#pragma once
#include <windows.h>
#include <filesystem>
#include <string>

class ExplorerWebDavUploadContextMenuService {
public:
    ExplorerWebDavUploadContextMenuService(HKEY root = HKEY_CURRENT_USER,
        std::wstring keyPath = L"Software\\Classes\\AllFilesystemObjects\\shell\\Quattro.UploadToWebDav",
        bool notifyShell = true);
    static std::wstring BuildCommand(const std::filesystem::path& executablePath);
    static std::wstring BuildIconValue(const std::filesystem::path& executablePath);
    static std::filesystem::path CurrentExecutablePath();
    bool Register(const std::filesystem::path& executablePath, std::wstring& error) const;
    bool Unregister(std::wstring& error) const;
    bool Reconcile(bool enabled, const std::filesystem::path& executablePath, std::wstring& error) const;
private:
    bool WriteStringValue(HKEY key, const wchar_t* name, const std::wstring& value, std::wstring& error) const;
    void NotifyShellIfNeeded() const;
    HKEY root_ = HKEY_CURRENT_USER; std::wstring keyPath_; bool notifyShell_ = true;
};
