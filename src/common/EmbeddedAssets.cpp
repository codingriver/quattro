#include "EmbeddedAssets.h"

#include "../../resources/resource.h"

#include <windows.h>

#ifndef QUATTRO_ENABLE_ASSET_DECOMPRESSION
#define QUATTRO_ENABLE_ASSET_DECOMPRESSION 0
#endif

#if QUATTRO_ENABLE_ASSET_DECOMPRESSION
#include <compressapi.h>
#endif

#include <algorithm>
#include <cwctype>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace {
constexpr unsigned char kCompressedPackMagic[] = {'Q', 'A', 'S', 'P', 'A', 'C', 'K', '1'};
constexpr unsigned char kRawCatalogMagic[] = {'Q', 'A', 'R', 'C', 'A', 'T', '0', '1'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint32_t kCompressionAlgorithm = 4; // COMPRESS_ALGORITHM_XPRESS_HUFF
constexpr std::uint32_t kMaximumAssetCount = 1024;
constexpr std::uint64_t kMaximumAssetSize = 256ull * 1024ull * 1024ull;

class PackReader {
public:
    PackReader(const unsigned char* data, std::size_t size) : current_(data), remaining_(size) {}

    bool ReadBytes(std::size_t size, const unsigned char*& bytes) {
        if (size > remaining_) {
            return false;
        }
        bytes = current_;
        current_ += size;
        remaining_ -= size;
        return true;
    }

    bool ReadUint32(std::uint32_t& value) {
        const unsigned char* bytes = nullptr;
        if (!ReadBytes(4, bytes)) {
            return false;
        }
        value = static_cast<std::uint32_t>(bytes[0]) |
                (static_cast<std::uint32_t>(bytes[1]) << 8) |
                (static_cast<std::uint32_t>(bytes[2]) << 16) |
                (static_cast<std::uint32_t>(bytes[3]) << 24);
        return true;
    }

    bool ReadUint64(std::uint64_t& value) {
        const unsigned char* bytes = nullptr;
        if (!ReadBytes(8, bytes)) {
            return false;
        }
        value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
        }
        return true;
    }

    std::size_t remaining() const { return remaining_; }

private:
    const unsigned char* current_ = nullptr;
    std::size_t remaining_ = 0;
};

bool LoadResourceBytes(HMODULE module, int resourceId, const unsigned char*& bytes, std::size_t& size) {
    bytes = nullptr;
    size = 0;
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource) {
        return false;
    }
    HGLOBAL loaded = LoadResource(module, resource);
    const DWORD resourceSize = SizeofResource(module, resource);
    const auto* resourceBytes = loaded
        ? static_cast<const unsigned char*>(LockResource(loaded))
        : nullptr;
    if (!resourceBytes || resourceSize == 0) {
        return false;
    }
    bytes = resourceBytes;
    size = resourceSize;
    return true;
}

std::wstring WideFromUtf8(const unsigned char* data, std::size_t size) {
    if (!data || size == 0 || size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int sourceSize = static_cast<int>(size);
    const int length = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(data),
        sourceSize,
        nullptr,
        0);
    if (length <= 0) {
        return {};
    }
    std::wstring value(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char*>(data),
            sourceSize,
            value.data(),
            length) != length) {
        return {};
    }
    return value;
}

bool IsSafeRelativePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
        path.native().find(L'\0') != std::wstring::npos) {
        return false;
    }
    for (const auto& part : path) {
        if (part == L".." || part == L".") {
            return false;
        }
    }
    return true;
}

bool ReadCommonEntry(
    PackReader& reader,
    std::uint32_t& pathSize,
    std::uint64_t& uncompressedSize,
    const unsigned char*& hash) {
    return reader.ReadUint32(pathSize) && pathSize > 0 && pathSize <= 32768 &&
           reader.ReadUint64(uncompressedSize) && uncompressedSize > 0 && uncompressedSize <= kMaximumAssetSize &&
           reader.ReadBytes(32, hash);
}

bool AddAsset(
    const unsigned char* pathBytes,
    std::uint32_t pathSize,
    const unsigned char* hash,
    std::uint64_t uncompressedSize,
    EmbeddedAssetStorage storage,
    const unsigned char* payloadData,
    std::size_t payloadSize,
    std::unordered_set<std::wstring>& paths,
    std::vector<EmbeddedAsset>& assets,
    std::wstring& error) {
    std::filesystem::path relativePath = WideFromUtf8(pathBytes, pathSize);
    if (!IsSafeRelativePath(relativePath)) {
        error = L"内置资源目录包含不安全路径。";
        return false;
    }
    std::wstring pathKey = relativePath.generic_wstring();
    std::transform(pathKey.begin(), pathKey.end(), pathKey.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    if (!paths.insert(pathKey).second) {
        error = L"内置资源目录包含重复路径。";
        return false;
    }

    EmbeddedAsset asset;
    asset.relativePath = std::move(relativePath);
    std::copy_n(hash, asset.sha256.size(), asset.sha256.begin());
    asset.uncompressedSize = uncompressedSize;
    asset.storage = storage;
    asset.payloadData = payloadData;
    asset.payloadSize = payloadSize;
    assets.push_back(std::move(asset));
    return true;
}

bool LoadCompressedCatalog(
    PackReader& reader,
    std::vector<EmbeddedAsset>& assets,
    std::wstring& error) {
    std::uint32_t version = 0;
    std::uint32_t algorithm = 0;
    std::uint32_t count = 0;
    if (!reader.ReadUint32(version) || version != kFormatVersion ||
        !reader.ReadUint32(algorithm) || algorithm != kCompressionAlgorithm ||
        !reader.ReadUint32(count) || count == 0 || count > kMaximumAssetCount) {
        error = L"压缩内置资源包头无效。";
        return false;
    }

    std::unordered_set<std::wstring> paths;
    assets.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t pathSize = 0;
        std::uint64_t uncompressedSize = 0;
        std::uint64_t compressedSize = 0;
        const unsigned char* hash = nullptr;
        const unsigned char* pathBytes = nullptr;
        const unsigned char* compressedBytes = nullptr;
        if (!reader.ReadUint32(pathSize) || pathSize == 0 || pathSize > 32768 ||
            !reader.ReadUint64(uncompressedSize) || uncompressedSize == 0 || uncompressedSize > kMaximumAssetSize ||
            !reader.ReadUint64(compressedSize) || compressedSize == 0 ||
            !reader.ReadBytes(32, hash) ||
            !reader.ReadBytes(pathSize, pathBytes) ||
            compressedSize > static_cast<std::uint64_t>(reader.remaining()) ||
            compressedSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
            !reader.ReadBytes(static_cast<std::size_t>(compressedSize), compressedBytes) ||
            !AddAsset(
                pathBytes,
                pathSize,
                hash,
                uncompressedSize,
                EmbeddedAssetStorage::XpressHuff,
                compressedBytes,
                static_cast<std::size_t>(compressedSize),
                paths,
                assets,
                error)) {
            if (error.empty()) {
                error = L"压缩内置资源包条目损坏。";
            }
            return false;
        }
    }
    return true;
}

bool LoadRawCatalog(
    HMODULE module,
    PackReader& reader,
    std::vector<EmbeddedAsset>& assets,
    std::wstring& error) {
    std::uint32_t version = 0;
    std::uint32_t count = 0;
    if (!reader.ReadUint32(version) || version != kFormatVersion ||
        !reader.ReadUint32(count) || count == 0 || count > kMaximumAssetCount) {
        error = L"原始内置资源目录头无效。";
        return false;
    }

    std::unordered_set<std::wstring> paths;
    assets.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t pathSize = 0;
        std::uint64_t uncompressedSize = 0;
        std::uint32_t resourceId = 0;
        const unsigned char* hash = nullptr;
        const unsigned char* pathBytes = nullptr;
        if (!ReadCommonEntry(reader, pathSize, uncompressedSize, hash) ||
            !reader.ReadUint32(resourceId) || resourceId == 0 || resourceId > 65535 ||
            !reader.ReadBytes(pathSize, pathBytes)) {
            error = L"原始内置资源目录条目损坏。";
            return false;
        }

        const unsigned char* rawBytes = nullptr;
        std::size_t rawSize = 0;
        if (!LoadResourceBytes(module, static_cast<int>(resourceId), rawBytes, rawSize) ||
            rawSize != uncompressedSize ||
            !AddAsset(
                pathBytes,
                pathSize,
                hash,
                uncompressedSize,
                EmbeddedAssetStorage::RawResource,
                rawBytes,
                rawSize,
                paths,
                assets,
                error)) {
            if (error.empty()) {
                error = L"无法读取原始内置资源。";
            }
            return false;
        }
    }
    return true;
}
}

bool LoadEmbeddedAssetCatalog(
    void* moduleHandle,
    std::vector<EmbeddedAsset>& assets,
    std::wstring& error) {
    assets.clear();
    error.clear();

    HMODULE module = static_cast<HMODULE>(moduleHandle);
    if (!module) {
        module = GetModuleHandleW(nullptr);
    }
    const unsigned char* catalogBytes = nullptr;
    std::size_t catalogSize = 0;
    if (!LoadResourceBytes(module, IDR_QUATTRO_ASSET_PACK, catalogBytes, catalogSize)) {
        error = L"找不到内置资源目录。";
        return false;
    }

    PackReader reader(catalogBytes, catalogSize);
    const unsigned char* magic = nullptr;
    if (!reader.ReadBytes(sizeof(kCompressedPackMagic), magic)) {
        error = L"内置资源目录头无效。";
        return false;
    }

    bool loaded = false;
    if (std::memcmp(magic, kCompressedPackMagic, sizeof(kCompressedPackMagic)) == 0) {
        loaded = LoadCompressedCatalog(reader, assets, error);
    } else if (std::memcmp(magic, kRawCatalogMagic, sizeof(kRawCatalogMagic)) == 0) {
        loaded = LoadRawCatalog(module, reader, assets, error);
    } else {
        error = L"内置资源目录格式无效。";
        return false;
    }

    if (!loaded || reader.remaining() != 0) {
        assets.clear();
        if (error.empty()) {
            error = L"内置资源目录存在多余数据。";
        }
        return false;
    }
    return true;
}

bool LoadEmbeddedAssetBytes(
    const EmbeddedAsset& asset,
    std::vector<unsigned char>& bytes,
    bool& decompressed,
    std::wstring& error) {
    bytes.clear();
    decompressed = false;
    error.clear();
    if (!asset.payloadData || asset.payloadSize == 0 ||
        asset.uncompressedSize == 0 ||
        asset.uncompressedSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        error = L"内置资源条目无效。";
        return false;
    }

    if (asset.storage == EmbeddedAssetStorage::RawResource) {
        if (asset.payloadSize != asset.uncompressedSize) {
            error = L"原始内置资源大小无效。";
            return false;
        }
        bytes.assign(asset.payloadData, asset.payloadData + asset.payloadSize);
        return true;
    }

#if QUATTRO_ENABLE_ASSET_DECOMPRESSION
    DECOMPRESSOR_HANDLE decompressor = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &decompressor)) {
        error = L"无法初始化内置资源解压器。";
        return false;
    }

    bytes.resize(static_cast<std::size_t>(asset.uncompressedSize));
    SIZE_T written = 0;
    const bool succeeded = Decompress(
        decompressor,
        asset.payloadData,
        asset.payloadSize,
        bytes.data(),
        bytes.size(),
        &written) != FALSE;
    CloseDecompressor(decompressor);
    if (!succeeded || written != bytes.size()) {
        bytes.clear();
        error = L"内置资源解压失败。";
        return false;
    }
    decompressed = true;
    return true;
#else
    error = L"当前构建未启用内置资源解压支持。";
    return false;
#endif
}
