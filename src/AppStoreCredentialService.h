#pragma once

#include "Models.h"

#include <string>

class AppStoreCredentialService {
public:
    enum class SecretKind {
        GitHubToken,
        PackageEncryptionToken,
    };

    static std::wstring TargetName(const AppConfig& config, SecretKind kind);
    static bool SaveSecret(const AppConfig& config, SecretKind kind, const std::wstring& secret, std::wstring& error);
    static bool LoadSecret(const AppConfig& config, SecretKind kind, std::wstring& secret, std::wstring& error);
    static bool DeleteSecret(const AppConfig& config, SecretKind kind, std::wstring& error);
};
