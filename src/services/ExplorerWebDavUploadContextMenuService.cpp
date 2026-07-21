#include "ExplorerWebDavUploadContextMenuService.h"
#include "Utilities.h"
#include <shlobj.h>

namespace { std::wstring Error(LSTATUS s) { return FormatLastError(static_cast<DWORD>(s)); } }

ExplorerWebDavUploadContextMenuService::ExplorerWebDavUploadContextMenuService(HKEY root, std::wstring keyPath, bool notifyShell)
    : root_(root), keyPath_(std::move(keyPath)), notifyShell_(notifyShell) {}

std::wstring ExplorerWebDavUploadContextMenuService::BuildCommand(const std::filesystem::path& path) {
    return L"\"" + path.wstring() + L"\" --upload-to-webdav \"%1\"";
}
std::wstring ExplorerWebDavUploadContextMenuService::BuildIconValue(const std::filesystem::path& path) {
    return L"\"" + path.wstring() + L"\",0";
}
std::filesystem::path ExplorerWebDavUploadContextMenuService::CurrentExecutablePath() {
    std::wstring value(32768, L'\0'); DWORD n = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    return n ? std::filesystem::path(value.substr(0, n)) : std::filesystem::path{};
}
bool ExplorerWebDavUploadContextMenuService::WriteStringValue(HKEY key, const wchar_t* name, const std::wstring& value, std::wstring& error) const {
    const LSTATUS s = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)));
    if (s == ERROR_SUCCESS) return true; error = Error(s); return false;
}
bool ExplorerWebDavUploadContextMenuService::Register(const std::filesystem::path& path, std::wstring& error) const {
    error.clear(); if (path.empty() || path.is_relative()) { error = L"Quattro 可执行文件路径无效。"; return false; }
    HKEY key = nullptr; DWORD disposition = 0; LSTATUS s = RegCreateKeyExW(root_, keyPath_.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, &disposition);
    if (s != ERROR_SUCCESS) { error = Error(s); return false; }
    const bool ok = WriteStringValue(key, L"MUIVerb", L"上传到 WebDAV", error) && WriteStringValue(key, L"Icon", BuildIconValue(path), error) && WriteStringValue(key, L"MultiSelectModel", L"Player", error); RegCloseKey(key);
    if (!ok) { RegDeleteTreeW(root_, keyPath_.c_str()); return false; }
    HKEY command = nullptr; s = RegCreateKeyExW(root_, (keyPath_+L"\\command").c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &command, &disposition);
    if (s != ERROR_SUCCESS) { error = Error(s); RegDeleteTreeW(root_, keyPath_.c_str()); return false; }
    const bool commandOk = WriteStringValue(command, nullptr, BuildCommand(path), error); RegCloseKey(command);
    if (!commandOk) RegDeleteTreeW(root_, keyPath_.c_str()); else NotifyShellIfNeeded(); return commandOk;
}
bool ExplorerWebDavUploadContextMenuService::Unregister(std::wstring& error) const {
    error.clear(); const LSTATUS s = RegDeleteTreeW(root_, keyPath_.c_str()); if (s == ERROR_SUCCESS) NotifyShellIfNeeded(); else if (s != ERROR_FILE_NOT_FOUND && s != ERROR_PATH_NOT_FOUND) { error = Error(s); return false; } return true;
}
bool ExplorerWebDavUploadContextMenuService::Reconcile(bool enabled, const std::filesystem::path& path, std::wstring& error) const {
    if (!enabled) return Unregister(error); return Register(path, error);
}
void ExplorerWebDavUploadContextMenuService::NotifyShellIfNeeded() const { if (notifyShell_) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr); }
