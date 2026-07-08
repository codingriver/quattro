#pragma once

#include "Models.h"

#include <string>

class WebDavCredentialService {
public:
    static std::wstring TargetName(const AppConfig& config);
    static bool SavePassword(const AppConfig& config, const std::wstring& password, std::wstring& error);
    static bool LoadPassword(const AppConfig& config, std::wstring& password, std::wstring& error);
    static bool DeletePassword(const AppConfig& config, std::wstring& error);
};
