#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

enum class EmbeddedAssetStorage {
    RawResource,
    XpressHuff
};

struct EmbeddedAsset {
    std::filesystem::path relativePath;
    std::array<unsigned char, 32> sha256{};
    std::uint64_t uncompressedSize = 0;
    EmbeddedAssetStorage storage = EmbeddedAssetStorage::RawResource;
    const unsigned char* payloadData = nullptr;
    std::size_t payloadSize = 0;
};

bool LoadEmbeddedAssetCatalog(
    void* moduleHandle,
    std::vector<EmbeddedAsset>& assets,
    std::wstring& error);

bool LoadEmbeddedAssetBytes(
    const EmbeddedAsset& asset,
    std::vector<unsigned char>& bytes,
    bool& decompressed,
    std::wstring& error);
