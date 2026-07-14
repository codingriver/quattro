#pragma once

#include <cstddef>

struct EmbeddedExecutableDescriptor {
    const wchar_t* id = nullptr;
    const wchar_t* fileName = nullptr;
    const wchar_t* version = nullptr;
    const wchar_t* sha256 = nullptr;
    const unsigned char* data = nullptr;
    std::size_t size = 0;
};
