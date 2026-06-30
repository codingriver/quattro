#include "AppStoreCredentialService.h"

#include "Utilities.h"

#include <wincred.h>

namespace {
std::wstring KindName(AppStoreCredentialService::SecretKind kind) {
    switch (kind) {
    case AppStoreCredentialService::SecretKind::PackageEncryptionToken:
        return L"PackageEncryption";
    case AppStoreCredentialService::SecretKind::GitHubToken:
    default:
        return L"GitHub";
    }
}

bool ReadCredential(const std::wstring& target, std::wstring& secret, std::wstring& error) {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        const DWORD code = GetLastError();
        if (code == ERROR_NOT_FOUND) {
            return true;
        }
        error = L"读取网盘管理凭据失败: " + FormatLastError(code);
        return false;
    }
    if (credential && credential->CredentialBlob && credential->CredentialBlobSize > 0) {
        secret.assign(
            reinterpret_cast<const wchar_t*>(credential->CredentialBlob),
            reinterpret_cast<const wchar_t*>(credential->CredentialBlob) + credential->CredentialBlobSize / sizeof(wchar_t));
    }
    if (credential) {
        CredFree(credential);
    }
    return true;
}
}

std::wstring AppStoreCredentialService::TargetName(const AppConfig& config, SecretKind kind) {
    const std::wstring owner = ToLower(Trim(config.appStoreOwner));
    const std::wstring repo = ToLower(Trim(config.appStoreRepo));
    const std::wstring key = owner + L"/" + repo;
    return L"Quattro.AppStore." + KindName(kind) + L"." + Hex8(StablePathHash(key));
}

bool AppStoreCredentialService::SaveSecret(const AppConfig& config, SecretKind kind, const std::wstring& secret, std::wstring& error) {
    error.clear();
    if (Trim(config.appStoreOwner).empty() || Trim(config.appStoreRepo).empty()) {
        error = L"GitHub Owner 和 Repo 不能为空。";
        return false;
    }

    const std::wstring target = TargetName(config, kind);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(target.c_str());
    credential.UserName = const_cast<LPWSTR>(config.appStoreOwner.c_str());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.CredentialBlobSize = static_cast<DWORD>(secret.size() * sizeof(wchar_t));
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(secret.c_str()));
    if (!CredWriteW(&credential, 0)) {
        error = L"保存网盘管理凭据失败: " + FormatLastError(GetLastError());
        return false;
    }
    return true;
}

bool AppStoreCredentialService::LoadSecret(const AppConfig& config, SecretKind kind, std::wstring& secret, std::wstring& error) {
    error.clear();
    secret.clear();
    if (Trim(config.appStoreOwner).empty() || Trim(config.appStoreRepo).empty()) {
        return true;
    }
    return ReadCredential(TargetName(config, kind), secret, error);
}

bool AppStoreCredentialService::DeleteSecret(const AppConfig& config, SecretKind kind, std::wstring& error) {
    error.clear();
    if (Trim(config.appStoreOwner).empty() || Trim(config.appStoreRepo).empty()) {
        return true;
    }
    const std::wstring target = TargetName(config, kind);
    if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
        const DWORD code = GetLastError();
        if (code == ERROR_NOT_FOUND) {
            return true;
        }
        error = L"删除网盘管理凭据失败: " + FormatLastError(code);
        return false;
    }
    return true;
}
