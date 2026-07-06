#include "../src/LocalHttpServerService.h"
#include "../src/Utilities.h"

#include <filesystem>
#include <iostream>

namespace {
bool Require(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L"\n";
        return false;
    }
    return true;
}
}

int wmain() {
    const auto root = std::filesystem::temp_directory_path() / L"quattro-http-config-acceptance";
    const auto userConfig = std::filesystem::temp_directory_path() / L"quattro-http-config-user";
    SetEnvironmentVariableW(L"QUATTRO_USER_CONFIG_DIR", userConfig.c_str());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::remove_all(userConfig, ec);

    std::wstring error;
    bool ok = LocalHttpServerService::EnsureDetailConfig(root, error);
    ok = Require(ok, L"EnsureDetailConfig should create default config") && ok;
    ok = Require(FileExists(LocalHttpServerService::DetailConfigPath(root)), L"http-server.ini should exist") && ok;
    ok = Require(LocalHttpServerService::DetailConfigPath(root).parent_path() == userConfig, L"http-server.ini should be under user config directory") && ok;

    const std::wstring configText = LoadUtf8File(LocalHttpServerService::DetailConfigPath(root));
    ok = Require(configText.find(L"GuestRead=1") != std::wstring::npos, L"default config should allow guest read") && ok;
    ok = Require(configText.find(L"GuestUpload=0") != std::wstring::npos, L"default config should keep guest upload disabled") && ok;
    ok = Require(configText.find(L"MimeMap=") != std::wstring::npos, L"default config should include MIME map") && ok;
    ok = Require(configText.find(L"DownloadExtensions=") != std::wstring::npos, L"default config should include disposition rules") && ok;

    AppConfig config;
    config.httpServerPort = 70000;
    config.httpServerLanAccess = false;
    config.httpServerRootPath = L"";
    const auto options = LocalHttpServerService::OptionsFromConfig(config, root);
    ok = Require(options.port == 65535, L"port should be clamped") && ok;
    ok = Require(!options.lanAccess, L"LAN access option should round-trip") && ok;
    ok = Require(options.rootPath == LocalHttpServerService::DefaultRootPath(root), L"empty root should use appDirectory/web") && ok;

    std::filesystem::remove_all(root, ec);
    std::filesystem::remove_all(userConfig, ec);
    SetEnvironmentVariableW(L"QUATTRO_USER_CONFIG_DIR", nullptr);
    if (!ok) {
        return 1;
    }
    std::wcout << L"HTTP server config acceptance passed.\n";
    return 0;
}
