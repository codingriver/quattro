#pragma once

#include "EmbeddedExecutable.h"

#include <string_view>
#include <vector>

const EmbeddedExecutableDescriptor* FindEmbeddedExecutable(std::wstring_view id);
std::vector<const EmbeddedExecutableDescriptor*> EmbeddedExecutableCatalog();
