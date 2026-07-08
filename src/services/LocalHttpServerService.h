#pragma once

#include "Models.h"

#include <filesystem>
#include <memory>
#include <string>

struct LocalHttpServerOptions {
    int port = 18080;
    std::filesystem::path rootPath;
    bool lanAccess = true;
};

class LocalHttpServerService {
public:
    LocalHttpServerService();
    ~LocalHttpServerService();

    LocalHttpServerService(const LocalHttpServerService&) = delete;
    LocalHttpServerService& operator=(const LocalHttpServerService&) = delete;

    bool Start(const LocalHttpServerOptions& options, std::wstring& error);
    bool Restart(const LocalHttpServerOptions& options, std::wstring& error);
    void Stop();

    bool IsRunning() const;
    std::wstring BaseUrl(bool localOnly = false) const;
    const std::wstring& lastError() const { return lastError_; }
    const LocalHttpServerOptions& options() const { return options_; }

    static LocalHttpServerOptions OptionsFromConfig(const AppConfig& config, const std::filesystem::path& appDirectory);
    static std::filesystem::path DefaultRootPath(const std::filesystem::path& appDirectory);
    static std::filesystem::path DetailConfigDirectory();
    static std::filesystem::path DetailConfigPath(const std::filesystem::path& rootPath);
    static bool EnsureDetailConfig(const std::filesystem::path& rootPath, std::wstring& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    LocalHttpServerOptions options_;
    std::wstring lastError_;
};
