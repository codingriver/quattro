#pragma once

#include "EmbeddedExecutable.h"

#include <filesystem>
#include <string>
#include <string_view>

struct EmbeddedExecutablePrepareOptions {
    std::filesystem::path rootDirectory;
};

struct EmbeddedExecutablePrepareResult {
    bool success = false;
    bool written = false;
    bool updated = false;
    std::filesystem::path path;
    std::wstring message;
};

std::filesystem::path QuattroEmbeddedExecutableRootDirectory();

EmbeddedExecutablePrepareResult PrepareEmbeddedExecutable(
    const EmbeddedExecutableDescriptor& descriptor,
    const EmbeddedExecutablePrepareOptions& options);

EmbeddedExecutablePrepareResult PrepareEmbeddedExecutable(
    std::wstring_view id,
    const EmbeddedExecutablePrepareOptions& options);
