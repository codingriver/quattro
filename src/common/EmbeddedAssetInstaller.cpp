#include "EmbeddedAssetInstaller.h"

#include "EmbeddedAssets.h"
#include "Utilities.h"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
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

bool IsLocalBuildDirectory(const std::filesystem::path& moduleDirectory) {
    return FileExists(moduleDirectory / L"CMakeCache.txt") ||
           DirectoryExists(moduleDirectory / L"CMakeFiles") ||
           FileExists(moduleDirectory.parent_path() / L"CMakeCache.txt") ||
           DirectoryExists(moduleDirectory.parent_path() / L"CMakeFiles");
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
    if (IsLocalBuildDirectory(moduleDirectory)) {
        usedFallbackDirectory = false;
        return moduleDirectory;
    }

    const std::filesystem::path userConfigDirectory = QuattroUserConfigDirectory();
    if (CanWriteDirectory(userConfigDirectory)) {
        usedFallbackDirectory = false;
        return userConfigDirectory;
    }

    usedFallbackDirectory = true;
    std::filesystem::path fallback = LocalAppDataDirectory();
    if (CanWriteDirectory(fallback)) {
        return fallback;
    }

    usedFallbackDirectory = false;
    if (HasCoreAssets(moduleDirectory) || CanWriteDirectory(moduleDirectory)) {
        return moduleDirectory;
    }
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

std::wstring NormalizeRelativePath(const std::filesystem::path& path) {
    std::wstring value = ToLower(path.wstring());
    for (wchar_t& ch : value) {
        if (ch == L'/') {
            ch = L'\\';
        }
    }
    return value;
}

bool IsManagedAsset(const std::filesystem::path& relativePath) {
    const std::wstring path = NormalizeRelativePath(relativePath);
    return path == L"readme.md" ||
           path.rfind(L"docs\\", 0) == 0 ||
           path.rfind(L"theme\\", 0) == 0 ||
           path.rfind(L"icons\\menu\\", 0) == 0;
}

class BCryptAlgorithm {
public:
    ~BCryptAlgorithm() {
        if (handle_) {
            BCryptCloseAlgorithmProvider(handle_, 0);
        }
    }
    bool Open() {
        return BCryptOpenAlgorithmProvider(&handle_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    }
    BCRYPT_ALG_HANDLE get() const { return handle_; }

private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class BCryptHash {
public:
    ~BCryptHash() {
        if (handle_) {
            BCryptDestroyHash(handle_);
        }
    }
    bool Create(BCRYPT_ALG_HANDLE algorithm, std::vector<unsigned char>& object) {
        return BCryptCreateHash(
                   algorithm,
                   &handle_,
                   object.data(),
                   static_cast<ULONG>(object.size()),
                   nullptr,
                   0,
                   0) >= 0;
    }
    BCRYPT_HASH_HANDLE get() const { return handle_; }

private:
    BCRYPT_HASH_HANDLE handle_ = nullptr;
};

template <typename Feed>
bool Sha256(Feed feed, std::array<unsigned char, 32>& digest) {
    BCryptAlgorithm algorithm;
    if (!algorithm.Open()) {
        return false;
    }
    DWORD objectSize = 0;
    DWORD hashSize = 0;
    DWORD copied = 0;
    if (BCryptGetProperty(
            algorithm.get(),
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize),
            sizeof(objectSize),
            &copied,
            0) < 0 ||
        BCryptGetProperty(
            algorithm.get(),
            BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashSize),
            sizeof(hashSize),
            &copied,
            0) < 0 ||
        hashSize != digest.size()) {
        return false;
    }
    std::vector<unsigned char> object(objectSize);
    BCryptHash hash;
    if (!hash.Create(algorithm.get(), object) || !feed(hash.get())) {
        return false;
    }
    return BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
}

bool Sha256File(const std::filesystem::path& path, std::array<unsigned char, 32>& digest) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    return Sha256(
        [&](BCRYPT_HASH_HANDLE hash) {
            std::array<char, 64 * 1024> buffer{};
            while (file) {
                file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize count = file.gcount();
                if (count > 0 && BCryptHashData(
                        hash,
                        reinterpret_cast<PUCHAR>(buffer.data()),
                        static_cast<ULONG>(count),
                        0) < 0) {
                    return false;
                }
            }
            return file.eof();
        },
        digest);
}

bool Sha256Bytes(const std::vector<unsigned char>& bytes, std::array<unsigned char, 32>& digest) {
    return Sha256(
        [&](BCRYPT_HASH_HANDLE hash) {
            return bytes.empty() || BCryptHashData(
                hash,
                const_cast<unsigned char*>(bytes.data()),
                static_cast<ULONG>(bytes.size()),
                0) >= 0;
        },
        digest);
}

bool ShouldInstallAsset(const std::filesystem::path& target, const EmbeddedAsset& asset, bool exists, bool managed) {
    if (!exists) {
        return true;
    }
    if (!managed) {
        return false;
    }

    std::error_code ec;
    const std::uint64_t currentSize = std::filesystem::file_size(target, ec);
    if (ec || currentSize != asset.uncompressedSize) {
        return true;
    }
    std::array<unsigned char, 32> currentHash{};
    return !Sha256File(target, currentHash) || currentHash != asset.sha256;
}

bool WriteAssetAtomically(const std::filesystem::path& target, const std::vector<unsigned char>& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        return false;
    }

    const std::filesystem::path temporary = target.parent_path() /
        (target.filename().wstring() +
         L".quattro-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()) + L".tmp");
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }
        file.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!file.good()) {
            file.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }

    if (!MoveFileExW(
            temporary.c_str(),
            target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

bool BackupAsset(
    const std::filesystem::path& appDirectory,
    const std::filesystem::path& target,
    const std::filesystem::path& relativePath,
    EmbeddedAssetInstallResult& result) {
    if (!FileExists(target)) {
        return true;
    }
    if (result.backupDirectory.empty()) {
        result.backupDirectory = appDirectory / L"backups" / L"assets" / AssetTimestamp();
    }

    std::error_code ec;
    const std::filesystem::path backup = result.backupDirectory / relativePath;
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
    const auto totalStarted = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration validationDuration{};
    std::chrono::steady_clock::duration decompressionDuration{};

    EmbeddedAssetInstallResult result;
    result.appDirectory = SelectAppDirectory(moduleDirectory, result.usedFallbackDirectory);

    std::vector<EmbeddedAsset> assets;
    std::wstring catalogError;
    if (!LoadEmbeddedAssetCatalog(GetModuleHandleW(nullptr), assets, catalogError)) {
        ++result.failures;
        result.message = catalogError;
        result.totalMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - totalStarted).count();
        return result;
    }
    for (const EmbeddedAsset& asset : assets) {
        if (asset.storage == EmbeddedAssetStorage::RawResource) {
            ++result.rawAssets;
        } else {
            ++result.compressedAssets;
        }
    }

    for (const EmbeddedAsset& asset : assets) {
        const std::filesystem::path target = result.appDirectory / asset.relativePath;
        const bool exists = FileExists(target);
        const bool managed = IsManagedAsset(asset.relativePath);
        const auto validationStarted = std::chrono::steady_clock::now();
        const bool shouldInstall = ShouldInstallAsset(target, asset, exists, managed);
        validationDuration += std::chrono::steady_clock::now() - validationStarted;
        if (!shouldInstall) {
            ++result.filesSkipped;
            continue;
        }

        const auto decompressionStarted = std::chrono::steady_clock::now();
        std::vector<unsigned char> bytes;
        std::wstring decompressionError;
        bool wasDecompressed = false;
        const bool loaded = LoadEmbeddedAssetBytes(asset, bytes, wasDecompressed, decompressionError);
        const auto materializationDuration = std::chrono::steady_clock::now() - decompressionStarted;
        if (!loaded) {
            ++result.failures;
            continue;
        }
        if (wasDecompressed) {
            decompressionDuration += materializationDuration;
            ++result.filesDecompressed;
        }

        std::array<unsigned char, 32> decompressedHash{};
        if (bytes.size() != asset.uncompressedSize ||
            !Sha256Bytes(bytes, decompressedHash) ||
            decompressedHash != asset.sha256) {
            ++result.failures;
            continue;
        }

        if (exists && !BackupAsset(result.appDirectory, target, asset.relativePath, result)) {
            ++result.failures;
            continue;
        }

        if (WriteAssetAtomically(target, bytes)) {
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
    result.validationMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(validationDuration).count();
    result.decompressionMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(decompressionDuration).count();
    result.totalMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - totalStarted).count();
    return result;
}
