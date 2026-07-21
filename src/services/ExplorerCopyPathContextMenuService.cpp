#include "ExplorerCopyPathContextMenuService.h"

#include "Utilities.h"

#include <shlobj.h>

#include <iterator>
#include <vector>

namespace {
std::wstring ReadStringValue(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
        return {};
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(
            key,
            name,
            nullptr,
            &type,
            reinterpret_cast<BYTE*>(buffer.data()),
            &bytes) != ERROR_SUCCESS) {
        return {};
    }
    return buffer.data();
}

std::wstring RegistryError(LSTATUS status) {
    return FormatLastError(static_cast<DWORD>(status));
}
}

ExplorerCopyPathContextMenuService::ExplorerCopyPathContextMenuService(
    HKEY root,
    std::wstring keyPath,
    bool notifyShell)
    : root_(root), keyPath_(std::move(keyPath)), notifyShell_(notifyShell) {}

std::wstring ExplorerCopyPathContextMenuService::BuildCommand(
    const std::filesystem::path& executablePath) {
    return L"\"" + executablePath.wstring() + L"\" --copy-absolute-path \"%1\"";
}

std::wstring ExplorerCopyPathContextMenuService::BuildIconValue(
    const std::filesystem::path& executablePath) {
    return L"\"" + executablePath.wstring() + L"\",0";
}

std::filesystem::path ExplorerCopyPathContextMenuService::CurrentExecutablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return {};
        }
        if (copied < buffer.size() - 1) {
            buffer.resize(copied);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}

ExplorerCopyPathContextMenuService ExplorerCopyPathContextMenuService::ForCurrentProcess() {
    if (!QuattroTestMode()) {
        return ExplorerCopyPathContextMenuService();
    }
    wchar_t runId[256]{};
    const DWORD length = GetEnvironmentVariableW(
        L"QUATTRO_TEST_RUN_ID", runId, static_cast<DWORD>(std::size(runId)));
    const std::wstring scope = length > 0 && length < std::size(runId)
        ? std::wstring(runId, length)
        : std::wstring(L"missing-run-id");
    return ExplorerCopyPathContextMenuService(
        HKEY_CURRENT_USER,
        L"Software\\Quattro\\Tests\\CopyPathContextMenu_" + Hex8(StablePathHash(scope)),
        false);
}

ExplorerCopyPathContextMenuState ExplorerCopyPathContextMenuService::Query(
    const std::filesystem::path& executablePath) const {
    ExplorerCopyPathContextMenuState state;
    HKEY key = nullptr;
    if (RegOpenKeyExW(root_, keyPath_.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return state;
    }
    state.registered = true;
    const std::wstring label = ReadStringValue(key, L"MUIVerb");
    const std::wstring icon = ReadStringValue(key, L"Icon");
    const std::wstring multiSelect = ReadStringValue(key, L"MultiSelectModel");
    RegCloseKey(key);

    HKEY commandKey = nullptr;
    const std::wstring commandPath = keyPath_ + L"\\command";
    std::wstring command;
    if (RegOpenKeyExW(root_, commandPath.c_str(), 0, KEY_QUERY_VALUE, &commandKey) == ERROR_SUCCESS) {
        command = ReadStringValue(commandKey, nullptr);
        RegCloseKey(commandKey);
    }
    state.matchesExecutable =
        label == L"复制绝对路径" &&
        icon == BuildIconValue(executablePath) &&
        multiSelect == L"Player" &&
        command == BuildCommand(executablePath);
    return state;
}

bool ExplorerCopyPathContextMenuService::WriteStringValue(
    HKEY key,
    const wchar_t* name,
    const std::wstring& value,
    std::wstring& error) const {
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LSTATUS status = RegSetValueExW(
        key,
        name,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        bytes);
    if (status == ERROR_SUCCESS) {
        return true;
    }
    error = RegistryError(status);
    return false;
}

bool ExplorerCopyPathContextMenuService::Register(
    const std::filesystem::path& executablePath,
    std::wstring& error) const {
    error.clear();
    if (executablePath.empty() || executablePath.is_relative()) {
        error = L"Quattro 可执行文件路径无效。";
        return false;
    }

    HKEY key = nullptr;
    DWORD disposition = 0;
    LSTATUS status = RegCreateKeyExW(
        root_, keyPath_.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, &disposition);
    if (status != ERROR_SUCCESS) {
        error = RegistryError(status);
        return false;
    }
    const bool parentWritten =
        WriteStringValue(key, L"MUIVerb", L"复制绝对路径", error) &&
        WriteStringValue(key, L"Icon", BuildIconValue(executablePath), error) &&
        WriteStringValue(key, L"MultiSelectModel", L"Player", error);
    RegCloseKey(key);
    if (!parentWritten) {
        RegDeleteTreeW(root_, keyPath_.c_str());
        return false;
    }

    const std::wstring commandPath = keyPath_ + L"\\command";
    HKEY commandKey = nullptr;
    status = RegCreateKeyExW(
        root_, commandPath.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &commandKey, &disposition);
    if (status != ERROR_SUCCESS) {
        error = RegistryError(status);
        RegDeleteTreeW(root_, keyPath_.c_str());
        return false;
    }
    const bool commandWritten = WriteStringValue(commandKey, nullptr, BuildCommand(executablePath), error);
    RegCloseKey(commandKey);
    if (!commandWritten) {
        RegDeleteTreeW(root_, keyPath_.c_str());
        return false;
    }

    NotifyShellIfNeeded();
    return true;
}

bool ExplorerCopyPathContextMenuService::Unregister(std::wstring& error) const {
    error.clear();
    const LSTATUS status = RegDeleteTreeW(root_, keyPath_.c_str());
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
        return true;
    }
    if (status != ERROR_SUCCESS) {
        error = RegistryError(status);
        return false;
    }
    NotifyShellIfNeeded();
    return true;
}

bool ExplorerCopyPathContextMenuService::Reconcile(
    bool enabled,
    const std::filesystem::path& executablePath,
    std::wstring& error) const {
    if (!enabled) {
        return Unregister(error);
    }
    const ExplorerCopyPathContextMenuState state = Query(executablePath);
    if (state.registered && state.matchesExecutable) {
        error.clear();
        return true;
    }
    return Register(executablePath, error);
}

void ExplorerCopyPathContextMenuService::NotifyShellIfNeeded() const {
    if (notifyShell_) {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
}
