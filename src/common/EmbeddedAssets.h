#pragma once

#include <cstddef>

struct EmbeddedAsset {
    const wchar_t* relativePath;
    const unsigned char* data;
    std::size_t size;
};

std::size_t EmbeddedAssetCount();
const EmbeddedAsset& EmbeddedAssetAt(std::size_t index);
