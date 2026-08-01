#include "AppLaunchLockerLocator.h"

#include <windows.h>
#include <softpub.h>
#include <wintrust.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>

namespace {
struct RegKeyCloser {
    void operator()(HKEY key) const { if (key) RegCloseKey(key); }
};
using UniqueRegKey = std::unique_ptr<std::remove_pointer_t<HKEY>, RegKeyCloser>;

bool ReadString(HKEY key, const wchar_t* name, std::wstring& value) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) return false;
    std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &bytes) != ERROR_SUCCESS) return false;
    while (!buffer.empty() && buffer.back() == L'\0') buffer.pop_back();
    if (type == REG_EXPAND_SZ) {
        const DWORD required = ExpandEnvironmentStringsW(buffer.c_str(), nullptr, 0);
        if (required) {
            std::wstring expanded(required, L'\0');
            ExpandEnvironmentStringsW(buffer.c_str(), expanded.data(), required);
            while (!expanded.empty() && expanded.back() == L'\0') expanded.pop_back();
            buffer = std::move(expanded);
        }
    }
    value = std::move(buffer);
    return !value.empty();
}

bool ReadProtocolVersion(HKEY key, int& version) {
    DWORD type = 0;
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    if (RegQueryValueExW(key, L"ProtocolVersion", nullptr, &type, reinterpret_cast<BYTE*>(&value), &bytes) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        version = static_cast<int>(value);
        return true;
    }
    std::wstring text;
    if (!ReadString(key, L"ProtocolVersion", text)) return false;
    wchar_t* end = nullptr;
    const long parsed = wcstol(text.c_str(), &end, 10);
    if (!end || *end != L'\0') return false;
    version = static_cast<int>(parsed);
    return true;
}

bool VerifyAuthenticode(const std::filesystem::path& path) {
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_SAFER_FLAG;
    const LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    return status == ERROR_SUCCESS;
}

bool SameFileContent(const std::filesystem::path& first, const std::filesystem::path& second) {
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(first, fileError) || fileError ||
        !std::filesystem::is_regular_file(second, fileError) || fileError) {
        return false;
    }

    const std::uintmax_t firstSize = std::filesystem::file_size(first, fileError);
    if (fileError) {
        return false;
    }
    const std::uintmax_t secondSize = std::filesystem::file_size(second, fileError);
    if (fileError || firstSize != secondSize) {
        return false;
    }

    std::ifstream firstStream(first, std::ios::binary);
    std::ifstream secondStream(second, std::ios::binary);
    if (!firstStream || !secondStream) {
        return false;
    }

    std::array<char, 64 * 1024> firstBuffer{};
    std::array<char, 64 * 1024> secondBuffer{};
    do {
        firstStream.read(firstBuffer.data(), static_cast<std::streamsize>(firstBuffer.size()));
        secondStream.read(secondBuffer.data(), static_cast<std::streamsize>(secondBuffer.size()));
        const std::streamsize firstRead = firstStream.gcount();
        const std::streamsize secondRead = secondStream.gcount();
        if (firstRead != secondRead) {
            return false;
        }
        if (firstRead > 0 &&
            std::memcmp(firstBuffer.data(), secondBuffer.data(), static_cast<std::size_t>(firstRead)) != 0) {
            return false;
        }
    } while (firstStream || secondStream);

    return firstStream.eof() && secondStream.eof();
}

bool DeployRuntimeFileIfChanged(
    const std::filesystem::path& source,
    const std::filesystem::path& target,
    const std::wstring& missingMessage,
    const std::wstring& deployFailureMessage,
    std::wstring& error) {
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(source, fileError) || fileError) {
        error = missingMessage;
        return false;
    }
    if (SameFileContent(source, target)) {
        return true;
    }

    std::filesystem::create_directories(target.parent_path(), fileError);
    if (fileError) {
        error = L"无法创建广告拦截资源目录。";
        return false;
    }

    const std::filesystem::path temporary = target.parent_path() /
        (target.filename().wstring() +
         L".quattro-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()) + L".tmp");
    std::filesystem::remove(temporary, fileError);
    fileError.clear();
    std::filesystem::copy_file(source, temporary, std::filesystem::copy_options::overwrite_existing, fileError);
    if (fileError) {
        std::filesystem::remove(temporary, fileError);
        error = deployFailureMessage;
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, fileError);
        error = deployFailureMessage;
        return false;
    }
    return true;
}

bool DeployRuntimeDirectoryFilesIfChanged(
    const std::filesystem::path& sourceDirectory,
    const std::filesystem::path& targetDirectory,
    const std::wstring& failureMessage,
    std::wstring& error) {
    std::error_code fileError;
    if (!std::filesystem::is_directory(sourceDirectory, fileError) || fileError) {
        return true;
    }

    std::filesystem::create_directories(targetDirectory, fileError);
    if (fileError) {
        error = L"无法创建广告拦截资源目录。";
        return false;
    }

    std::filesystem::recursive_directory_iterator iterator(sourceDirectory, fileError);
    if (fileError) {
        error = failureMessage;
        return false;
    }
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(fileError)) {
        if (fileError) {
            error = failureMessage;
            return false;
        }
        std::error_code entryError;
        if (!iterator->is_regular_file(entryError)) {
            if (entryError) {
                error = failureMessage;
                return false;
            }
            continue;
        }
        const std::filesystem::path relative = iterator->path().lexically_relative(sourceDirectory);
        if (relative.empty()) {
            error = failureMessage;
            return false;
        }
        bool unsafeRelativePath = false;
        for (const auto& part : relative) {
            if (part == L"..") {
                unsafeRelativePath = true;
                break;
            }
        }
        if (unsafeRelativePath) {
            error = failureMessage;
            return false;
        }
        if (!DeployRuntimeFileIfChanged(iterator->path(), targetDirectory / relative, failureMessage, failureMessage, error)) {
            return false;
        }
    }
    return true;
}
}

AppLaunchLockerLocationResult FindInstalledAppLaunchLocker(int requiredProtocolVersion) {
    constexpr wchar_t keyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\AppLaunchLocker.exe";
    const std::array<std::pair<HKEY, REGSAM>, 4> locations{{
        {HKEY_CURRENT_USER, KEY_WOW64_64KEY}, {HKEY_CURRENT_USER, KEY_WOW64_32KEY},
        {HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY}, {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY}}};
    bool registrationSeen = false;
    for (const auto& [hive, view] : locations) {
        HKEY raw = nullptr;
        if (RegOpenKeyExW(hive, keyPath, 0, KEY_QUERY_VALUE | view, &raw) != ERROR_SUCCESS) continue;
        registrationSeen = true;
        UniqueRegKey key(raw);
        std::wstring value;
        int protocolVersion = 0;
        if (!ReadString(raw, nullptr, value) || !ReadProtocolVersion(raw, protocolVersion) ||
            protocolVersion != requiredProtocolVersion) continue;
        const std::filesystem::path path(value);
        if (!std::filesystem::is_regular_file(path) || !VerifyAuthenticode(path)) continue;
        return {true, path, L"已使用独立安装的 AppLaunchLocker。"};
    }
    return {false, {}, registrationSeen
        ? L"已安装的 AppLaunchLocker 未通过签名或协议兼容校验，将使用随包组件。"
        : std::wstring{}};
}

bool PrepareAppLaunchLockerRuntimeResources(
    const std::filesystem::path& executablePath,
    const std::filesystem::path& sourceRoot,
    std::wstring& error) {
    error.clear();
    const std::filesystem::path targetRoot = executablePath.parent_path();
    const std::filesystem::path sourceFont = sourceRoot / L"icons/menu/tabler/tabler-icons.ttf";
    const std::filesystem::path targetFont = targetRoot / L"icons/menu/tabler/tabler-icons.ttf";
    if (!DeployRuntimeFileIfChanged(
            sourceFont,
            targetFont,
            L"广告拦截组件缺少 Tabler 图标字体。",
            L"无法部署广告拦截图标字体。",
            error)) {
        return false;
    }
    const std::filesystem::path sourceTheme = sourceRoot / L"theme";
    if (!DeployRuntimeDirectoryFilesIfChanged(sourceTheme, targetRoot / L"theme", L"无法部署广告拦截主题资源。", error)) {
        return false;
    }
    return true;
}
