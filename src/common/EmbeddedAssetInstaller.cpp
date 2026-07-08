#include "EmbeddedAssetInstaller.h"

#include "EmbeddedAssets.h"
#include "Utilities.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
bool HasCoreAssets(const std::filesystem::path& directory) {
    return DirectoryExists(directory / L"theme");
}

bool CanWriteDirectory(const std::filesystem::path& directory) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return false;
    }

    const std::filesystem::path probe = directory / (L".quattro_write_test_" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    {
        std::ofstream file(probe, std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }
        file << "ok";
    }
    std::filesystem::remove(probe, ec);
    return true;
}

std::filesystem::path LocalAppDataDirectory() {
    std::wstring buffer(32768, L'\0');
    DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (copied == 0 || copied >= buffer.size()) {
        return GetModuleDirectory();
    }
    buffer.resize(copied);
    return std::filesystem::path(buffer) / L"Quattro";
}

std::filesystem::path SelectAppDirectory(const std::filesystem::path& moduleDirectory, bool& usedFallbackDirectory) {
    if (HasCoreAssets(moduleDirectory) || CanWriteDirectory(moduleDirectory)) {
        usedFallbackDirectory = false;
        return moduleDirectory;
    }

    usedFallbackDirectory = true;
    std::filesystem::path fallback = LocalAppDataDirectory();
    if (CanWriteDirectory(fallback)) {
        return fallback;
    }

    usedFallbackDirectory = false;
    return moduleDirectory;
}

std::wstring AssetTimestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[32]{};
    swprintf_s(
        buffer,
        L"%04u%02u%02u-%02u%02u%02u",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond);
    return buffer;
}

std::wstring NormalizeRelativePath(const wchar_t* path) {
    std::wstring value = ToLower(path ? path : L"");
    for (wchar_t& ch : value) {
        if (ch == L'/') {
            ch = L'\\';
        }
    }
    return value;
}

bool IsManagedAsset(const wchar_t* relativePath) {
    const std::wstring path = NormalizeRelativePath(relativePath);
    return path == L"readme.md" ||
           path.rfind(L"docs\\", 0) == 0 ||
           path.rfind(L"theme\\", 0) == 0 ||
           path.rfind(L"icons\\menu\\", 0) == 0;
}

std::vector<unsigned char> ReadBinaryFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::vector<unsigned char>(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

bool SameContent(const std::filesystem::path& target, const EmbeddedAsset& asset) {
    const std::vector<unsigned char> bytes = ReadBinaryFile(target);
    return bytes.size() == asset.size &&
           (bytes.empty() || std::equal(bytes.begin(), bytes.end(), asset.data));
}

bool WriteAsset(const std::filesystem::path& target, const EmbeddedAsset& asset) {
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        return false;
    }

    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(asset.data), static_cast<std::streamsize>(asset.size));
    return file.good();
}

bool BackupAsset(
    const std::filesystem::path& appDirectory,
    const std::filesystem::path& target,
    const EmbeddedAsset& asset,
    EmbeddedAssetInstallResult& result) {
    if (!FileExists(target)) {
        return true;
    }
    if (result.backupDirectory.empty()) {
        result.backupDirectory = appDirectory / L"backups" / L"assets" / AssetTimestamp();
    }

    std::error_code ec;
    const std::filesystem::path backup = result.backupDirectory / asset.relativePath;
    std::filesystem::create_directories(backup.parent_path(), ec);
    if (ec) {
        return false;
    }
    std::filesystem::copy_file(target, backup, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return false;
    }
    ++result.filesBackedUp;
    return true;
}
}

EmbeddedAssetInstallResult PrepareEmbeddedAssets(const std::filesystem::path& moduleDirectory) {
    EmbeddedAssetInstallResult result;
    result.appDirectory = SelectAppDirectory(moduleDirectory, result.usedFallbackDirectory);

    for (std::size_t i = 0; i < EmbeddedAssetCount(); ++i) {
        const EmbeddedAsset& asset = EmbeddedAssetAt(i);
        if (!asset.relativePath || !asset.data || asset.size == 0) {
            continue;
        }

        const std::filesystem::path target = result.appDirectory / asset.relativePath;
        const bool exists = FileExists(target);
        const bool managed = IsManagedAsset(asset.relativePath);
        if (exists && (!managed || SameContent(target, asset))) {
            ++result.filesSkipped;
            continue;
        }

        if (exists && !BackupAsset(result.appDirectory, target, asset, result)) {
            ++result.failures;
            continue;
        }

        if (WriteAsset(target, asset)) {
            if (exists) {
                ++result.filesUpdated;
            } else {
                ++result.filesWritten;
            }
        } else {
            ++result.failures;
        }
    }

    if (result.usedFallbackDirectory) {
        result.message = L"应用目录不可写，已使用本地数据目录: " + result.appDirectory.wstring();
    }
    if (result.failures > 0) {
        if (!result.message.empty()) {
            result.message += L"\n";
        }
        result.message += L"部分内置资源释放失败: " + std::to_wstring(result.failures);
    }
    return result;
}
