#include "FileHelperService.h"

#include "../common/AppLog.h"
#include "../common/Utilities.h"
#include "../domain/Models.h"
#include "ShellItemService.h"

#include <system_error>
#include <utility>

namespace {
FileHelperResult Failure(std::wstring message, std::filesystem::path path = {}) {
    return {FileHelperStatus::Failed, std::move(path), std::move(message)};
}

FileHelperResult Success(std::wstring message, const std::filesystem::path& path) {
    return {FileHelperStatus::Success, path, std::move(message)};
}

void LogFailure(const wchar_t* operation, const std::filesystem::path& path, const std::wstring& detail) {
    WriteAppLog(
        std::wstring(L"文件助手：") + operation + L"失败，路径=\"" + path.wstring() + L"\"，详情=" + detail);
}

bool SuppressExternalOpenForAcceptance() {
    return QuattroTestMode() && BackgroundAcceptanceMode();
}

bool IsFile(const std::filesystem::path& path, std::error_code& error) {
    error.clear();
    return std::filesystem::is_regular_file(path, error);
}

bool IsDirectory(const std::filesystem::path& path, std::error_code& error) {
    error.clear();
    return std::filesystem::is_directory(path, error);
}
}

FileHelperResult FileHelperService::ResolvePath(std::wstring_view input) {
    std::wstring value = Trim(std::wstring(input));
    if (value.size() >= 2 &&
        ((value.front() == L'"' && value.back() == L'"') ||
         (value.front() == L'\'' && value.back() == L'\''))) {
        value = Trim(value.substr(1, value.size() - 2));
    }
    if (value.empty()) {
        return Failure(L"请输入路径。");
    }

    value = Trim(ExpandEnvironmentStringsSafe(value));
    if (value.size() < 3) {
        return Failure(L"仅支持盘符开头的本地绝对路径。");
    }
    const bool driveLetter =
        (value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'a' && value[0] <= L'z');
    if (!driveLetter || value[1] != L':' ||
        (value[2] != L'\\' && value[2] != L'/')) {
        return Failure(L"仅支持盘符开头的本地绝对路径。");
    }

    std::filesystem::path path(value);
    path = path.lexically_normal();
    return Success(L"路径有效。", path);
}

bool FileHelperService::IsExistingPath(std::wstring_view input) {
    const FileHelperResult resolved = ResolvePath(input);
    if (resolved.status == FileHelperStatus::Failed) {
        return false;
    }
    std::error_code error;
    const bool exists = std::filesystem::exists(resolved.path, error);
    if (error || !exists) {
        return false;
    }
    return IsFile(resolved.path, error) || (!error && IsDirectory(resolved.path, error));
}

FileHelperResult FileHelperService::OpenFile(HWND owner, std::wstring_view input) const {
    FileHelperResult result = ResolvePath(input);
    if (result.status == FileHelperStatus::Failed) {
        return result;
    }

    std::error_code error;
    if (!IsFile(result.path, error)) {
        if (error) LogFailure(L"检查文件", result.path, std::to_wstring(error.value()));
        error.clear();
        const bool exists = std::filesystem::exists(result.path, error);
        return Failure(exists && !error ? L"目标不是文件。" : L"文件不存在。", result.path);
    }
    if (SuppressExternalOpenForAcceptance()) {
        WriteAppLog(L"文件助手后台验收：已记录打开文件意图，路径=\"" + result.path.wstring() + L"\"");
        return Success(L"已记录打开文件意图。", result.path);
    }

    Link link;
    link.path = result.path.wstring();
    link.type = 0;
    link.showCmd = SW_SHOWNORMAL;
    std::wstring detail;
    if (!ShellItemService::OpenShellTarget(owner, link, SW_SHOWNORMAL, detail)) {
        LogFailure(L"打开文件", result.path, detail);
        return Failure(L"无法打开文件，请检查路径和权限。", result.path);
    }
    return Success(L"已打开文件。", result.path);
}

FileHelperResult FileHelperService::OpenFolder(HWND owner, std::wstring_view input) const {
    FileHelperResult result = ResolvePath(input);
    if (result.status == FileHelperStatus::Failed) {
        return result;
    }

    std::error_code error;
    if (!IsDirectory(result.path, error)) {
        if (error) LogFailure(L"检查目录", result.path, std::to_wstring(error.value()));
        error.clear();
        const bool exists = std::filesystem::exists(result.path, error);
        return Failure(exists && !error ? L"目标不是目录。" : L"目录不存在。", result.path);
    }
    if (SuppressExternalOpenForAcceptance()) {
        WriteAppLog(L"文件助手后台验收：已记录打开目录意图，路径=\"" + result.path.wstring() + L"\"");
        return Success(L"已记录打开目录意图。", result.path);
    }

    Link link;
    link.path = result.path.wstring();
    link.type = 1;
    link.showCmd = SW_SHOWNORMAL;
    std::wstring detail;
    if (!ShellItemService::OpenShellTarget(owner, link, SW_SHOWNORMAL, detail)) {
        LogFailure(L"打开目录", result.path, detail);
        return Failure(L"无法打开目录，请检查路径和权限。", result.path);
    }
    return Success(L"已打开目录。", result.path);
}

FileHelperResult FileHelperService::CreateFile(std::wstring_view input, bool overwrite) const {
    FileHelperResult result = ResolvePath(input);
    if (result.status == FileHelperStatus::Failed) {
        return result;
    }

    std::error_code error;
    if (std::filesystem::exists(result.path, error)) {
        if (error) {
            LogFailure(L"检查文件", result.path, std::to_wstring(error.value()));
            return Failure(L"无法检查目标路径，请检查权限。", result.path);
        }
        if (!IsFile(result.path, error)) {
            return Failure(L"目标路径已被目录占用。", result.path);
        }
        if (!overwrite) {
            return {FileHelperStatus::AlreadyExists, result.path, L"文件已存在。"};
        }
    }

    const std::filesystem::path parent = result.path.parent_path();
    if (!parent.empty()) {
        error.clear();
        std::filesystem::create_directories(parent, error);
        if (error) {
            LogFailure(L"创建父目录", parent, std::to_wstring(error.value()));
            return Failure(L"无法创建必要的父目录，请检查路径和权限。", result.path);
        }
    }

    HANDLE file = ::CreateFileW(
        result.path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        overwrite ? CREATE_ALWAYS : CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (!overwrite && code == ERROR_FILE_EXISTS) {
            return {FileHelperStatus::AlreadyExists, result.path, L"文件已存在。"};
        }
        LogFailure(L"创建文件", result.path, FormatLastError(code));
        return Failure(L"创建文件失败，请检查路径和权限。", result.path);
    }
    CloseHandle(file);
    return Success(overwrite ? L"文件已覆盖。" : L"文件已创建。", result.path);
}

FileHelperResult FileHelperService::CreateFolder(std::wstring_view input) const {
    FileHelperResult result = ResolvePath(input);
    if (result.status == FileHelperStatus::Failed) {
        return result;
    }

    std::error_code error;
    if (std::filesystem::exists(result.path, error)) {
        if (error) {
            LogFailure(L"检查目录", result.path, std::to_wstring(error.value()));
            return Failure(L"无法检查目标路径，请检查权限。", result.path);
        }
        if (IsDirectory(result.path, error)) {
            return {FileHelperStatus::AlreadyExists, result.path, L"目录已存在。"};
        }
        return Failure(L"目标路径已被文件占用。", result.path);
    }

    std::filesystem::create_directories(result.path, error);
    if (error) {
        LogFailure(L"创建目录", result.path, std::to_wstring(error.value()));
        return Failure(L"创建目录失败，请检查路径和权限。", result.path);
    }
    return Success(L"目录已创建。", result.path);
}

FileHelperResult FileHelperService::OpenContainingLocation(HWND owner, std::wstring_view input) const {
    FileHelperResult result = ResolvePath(input);
    if (result.status == FileHelperStatus::Failed) {
        return result;
    }

    std::error_code error;
    const bool file = IsFile(result.path, error);
    if (error) {
        LogFailure(L"检查所在位置", result.path, std::to_wstring(error.value()));
        return Failure(L"无法检查目标路径，请检查权限。", result.path);
    }
    const bool folder = !file && IsDirectory(result.path, error);
    if (error || (!file && !folder)) {
        return Failure(L"目标不存在，无法打开所在位置。", result.path);
    }
    if (SuppressExternalOpenForAcceptance()) {
        WriteAppLog(L"文件助手后台验收：已记录打开所在位置意图，路径=\"" + result.path.wstring() + L"\"");
        return Success(L"已记录打开所在位置意图。", result.path);
    }

    std::wstring detail;
    if (!ShellItemService::OpenFileSystemContainingLocation(owner, result.path, detail)) {
        LogFailure(L"打开所在位置", result.path, detail);
        return Failure(L"无法打开所在位置，请检查路径和权限。", result.path);
    }
    return Success(L"已打开所在位置。", result.path);
}
