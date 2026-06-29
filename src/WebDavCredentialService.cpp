#include "WebDavCredentialService.h"

#include "Utilities.h"

#include <wincred.h>

namespace {
std::wstring LegacyTargetName(const AppConfig& config) {
    const std::wstring key = ToLower(Trim(config.webDavUrl)) + L"|" + ToLower(Trim(config.webDavUserName));
    return L"Quattro.WebDavBackup." + Hex8(StablePathHash(key));
}

bool ReadCredential(const std::wstring& target, std::wstring& password, std::wstring& error) {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        const DWORD code = GetLastError();
        if (code == ERROR_NOT_FOUND) {
            return true;
        }
        error = L"读取 WebDAV 密码失败: " + FormatLastError(code);
        return false;
    }
    if (credential && credential->CredentialBlob && credential->CredentialBlobSize > 0) {
        password.assign(
            reinterpret_cast<const wchar_t*>(credential->CredentialBlob),
            reinterpret_cast<const wchar_t*>(credential->CredentialBlob) + credential->CredentialBlobSize / sizeof(wchar_t));
    }
    if (credential) {
        CredFree(credential);
    }
    return true;
}

bool DeleteCredential(const std::wstring& target, std::wstring& error) {
    if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
        const DWORD code = GetLastError();
        if (code == ERROR_NOT_FOUND) {
            return true;
        }
        error = L"删除 WebDAV 密码失败: " + FormatLastError(code);
        return false;
    }
    return true;
}
}

std::wstring WebDavCredentialService::TargetName(const AppConfig& config) {
    (void)config;
    return L"Quattro.WebDavBackup.Default";
}

bool WebDavCredentialService::SavePassword(const AppConfig& config, const std::wstring& password, std::wstring& error) {
    error.clear();
    if (Trim(config.webDavUrl).empty() || Trim(config.webDavUserName).empty()) {
        error = L"WebDAV 地址和用户名不能为空。";
        return false;
    }

    const std::wstring target = TargetName(config);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(target.c_str());
    credential.UserName = const_cast<LPWSTR>(config.webDavUserName.c_str());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.CredentialBlobSize = static_cast<DWORD>(password.size() * sizeof(wchar_t));
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(password.c_str()));
    if (!CredWriteW(&credential, 0)) {
        error = L"保存 WebDAV 密码失败: " + FormatLastError(GetLastError());
        return false;
    }
    return true;
}

bool WebDavCredentialService::LoadPassword(const AppConfig& config, std::wstring& password, std::wstring& error) {
    error.clear();
    password.clear();
    if (Trim(config.webDavUrl).empty() || Trim(config.webDavUserName).empty()) {
        return true;
    }

    if (!ReadCredential(TargetName(config), password, error)) {
        return false;
    }
    if (!password.empty()) {
        return true;
    }

    std::wstring legacyPassword;
    if (!ReadCredential(LegacyTargetName(config), legacyPassword, error)) {
        return false;
    }
    if (!legacyPassword.empty()) {
        password = legacyPassword;
        std::wstring saveError;
        if (!SavePassword(config, legacyPassword, saveError)) {
            error = saveError;
            return false;
        }
    }
    return true;
}

bool WebDavCredentialService::DeletePassword(const AppConfig& config, std::wstring& error) {
    error.clear();
    if (Trim(config.webDavUrl).empty() || Trim(config.webDavUserName).empty()) {
        return true;
    }
    if (!DeleteCredential(TargetName(config), error)) {
        return false;
    }
    return DeleteCredential(LegacyTargetName(config), error);
}
