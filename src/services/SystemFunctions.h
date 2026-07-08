#pragma once

#include "MenuCatalog.h"
#include "Models.h"

#include <cstddef>
#include <span>

struct SystemFunctionDefinition {
    const wchar_t* name;
    const wchar_t* target;
    const wchar_t* parameter;
    int type;
    int showCmd;
    const wchar_t* remark;
    int stockIcon = -1;
    int menuIcon = 0;
};

std::span<const SystemFunctionDefinition> SystemFunctions();
int SystemFunctionImageIndex(const SystemFunctionDefinition& item);
const SystemFunctionDefinition* SystemFunctionForLink(const Link& link);
MenuIcon SystemFunctionMenuIconForLink(const Link& link);
bool ConfigureSystemFunctionLink(std::size_t index, Link& link);
