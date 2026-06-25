#include "EmbeddedAssetInstaller.h"

#include "EmbeddedAssets.h"
#include "Utilities.h"

#include <windows.h>

#include <fstream>
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

bool WriteAssetIfMissing(const std::filesystem::path& target, const EmbeddedAsset& asset) {
    if (FileExists(target)) {
        return true;
    }

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
        if (FileExists(target)) {
            continue;
        }

        if (WriteAssetIfMissing(target, asset)) {
            ++result.filesWritten;
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
