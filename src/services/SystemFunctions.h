#pragma once

#include "MenuCatalog.h"
#include "Models.h"

#include <cstddef>
#include <span>

enum class BuiltinSystemContextProfile {
    None,
    ThisPc,
    RecycleBin,
    Network,
};

enum class BuiltinSystemContextAction {
    ManageComputer,
    MapNetworkDrive,
    DisconnectNetworkDrive,
    EmptyRecycleBin,
};

struct BuiltinSystemContextMenuItem {
    BuiltinSystemContextAction action;
    const wchar_t* text;
    int menuIcon;
};

struct SystemFunctionDefinition {
    const wchar_t* key;
    const wchar_t* name;
    const wchar_t* target;
    const wchar_t* parameter;
    int type;
    int showCmd;
    const wchar_t* remark;
    int stockIcon = -1;
    int menuIcon = 0;
    BuiltinSystemContextProfile contextProfile = BuiltinSystemContextProfile::None;
};

std::span<const SystemFunctionDefinition> SystemFunctions();
int SystemFunctionImageIndex(const SystemFunctionDefinition& item);
const SystemFunctionDefinition* SystemFunctionForLink(const Link& link);
const SystemFunctionDefinition* BuiltinSystemFunctionForLink(const Link& link);
bool RestoreLegacyBuiltinSystemFunctionKey(Link& link);
std::span<const BuiltinSystemContextMenuItem> BuiltinSystemContextMenuItems(const Link& link);
MenuIcon SystemFunctionMenuIconForLink(const Link& link);
bool ConfigureSystemFunctionLink(std::size_t index, Link& link);
