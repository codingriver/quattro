#include "PluginRegistry.h"

#include "Utilities.h"
#include "Version.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace {
struct RuntimePluginState {
    std::unordered_map<std::wstring, bool> enabledOverrides;
    std::unordered_map<std::wstring, std::unordered_map<std::wstring, std::wstring>> settings;
};

std::unordered_map<std::wstring, RuntimePluginState>& RuntimeStates() {
    static std::unordered_map<std::wstring, RuntimePluginState> states;
    return states;
}

std::wstring StateKey(const std::filesystem::path& appDirectory) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(appDirectory, ec);
    return ToLower((ec ? appDirectory : canonical).wstring());
}

PluginRecord* FindPlugin(std::vector<PluginRecord>& plugins, const std::wstring& pluginId) {
    auto found = std::find_if(plugins.begin(), plugins.end(), [&](const PluginRecord& plugin) {
        return plugin.id == pluginId;
    });
    return found == plugins.end() ? nullptr : &*found;
}
}

PluginRegistry::PluginRegistry(std::filesystem::path appDirectory)
    : appDirectory_(std::move(appDirectory)),
      stateKey_(StateKey(appDirectory_)) {
}

std::vector<PluginRecord> PluginRegistry::BuiltinPlugins() {
    PluginRecord clicker;
    clicker.id = L"quattro.builtin.clicker";
    clicker.name = L"连点器";
    clicker.version = L"1.0.0";
    clicker.category = L"builtin-tools";
    clicker.kind = L"builtin-tool";
    clicker.engine = L"clicker";
    clicker.description = L"按指定坐标、次数和间隔连续执行鼠标点击。";
    clicker.permissions = L"鼠标控制, 热键停止";
    clicker.author = L"Quattro快速启动器";
    clicker.license = L"Built-in";
    clicker.builtin = true;
    clicker.deletable = false;
    clicker.enabled = true;
    clicker.installed = true;

    PluginRecord timer;
    timer.id = L"quattro.builtin.timer";
    timer.name = L"计时器";
    timer.version = L"1.0.0";
    timer.category = L"builtin-tools";
    timer.kind = L"builtin-tool";
    timer.engine = L"timer";
    timer.description = L"设置倒计时，到点后弹窗提醒。";
    timer.permissions = L"提醒";
    timer.author = L"Quattro快速启动器";
    timer.license = L"Built-in";
    timer.builtin = true;
    timer.deletable = false;
    timer.enabled = true;
    timer.installed = true;

    PluginRecord stopwatch;
    stopwatch.id = L"quattro.builtin.stopwatch";
    stopwatch.name = L"秒表";
    stopwatch.version = L"1.0.0";
    stopwatch.category = L"builtin-tools";
    stopwatch.kind = L"builtin-tool";
    stopwatch.engine = L"stopwatch";
    stopwatch.description = L"提供开始、暂停、重置和计次。";
    stopwatch.author = L"Quattro快速启动器";
    stopwatch.license = L"Built-in";
    stopwatch.builtin = true;
    stopwatch.deletable = false;
    stopwatch.enabled = true;
    stopwatch.installed = true;

    PluginRecord processTools;
    processTools.id = L"quattro.builtin.process-tools";
    processTools.name = L"进程工具";
    processTools.version = L"1.0.0";
    processTools.category = L"builtin-tools";
    processTools.kind = L"builtin-tool";
    processTools.engine = L"process-tools";
    processTools.description = L"通过鼠标位置、进程 ID、端口或文件路径查询进程，并执行相关操作。";
    processTools.permissions = L"文件查询, 进程查询, 结束进程, 打开目录, 全局快捷键";
    processTools.author = L"Quattro快速启动器";
    processTools.license = L"Built-in";
    processTools.builtin = true;
    processTools.deletable = false;
    processTools.enabled = true;
    processTools.installed = true;

    PluginRecord appLaunchLocker;
    appLaunchLocker.id = L"quattro.builtin.app-launch-locker";
    appLaunchLocker.name = L"自启动管理";
    appLaunchLocker.version = L"1.0.0";
    appLaunchLocker.category = L"builtin-tools";
    appLaunchLocker.kind = L"builtin-tool";
    appLaunchLocker.engine = L"app-launch-locker";
    appLaunchLocker.description = L"查看、禁用和恢复 Windows 自启动项目。";
    appLaunchLocker.permissions = L"自启动查询, 按需管理员权限";
    appLaunchLocker.author = L"Quattro快速启动器";
    appLaunchLocker.license = L"Built-in";
    appLaunchLocker.builtin = true;
    appLaunchLocker.deletable = false;
    appLaunchLocker.enabled = true;
    appLaunchLocker.installed = true;

    PluginRecord adBlock;
    adBlock.id = L"quattro.builtin.ad-block";
    adBlock.name = L"广告拦截";
    adBlock.version = L"1.0.0";
    adBlock.category = L"builtin-tools";
    adBlock.kind = L"builtin-tool";
    adBlock.engine = L"ad-block";
    adBlock.description = L"拦截指定文件或文件夹中的程序启动，可随时解除。";
    adBlock.permissions = L"程序拦截, 按需管理员权限";
    adBlock.author = L"Quattro快速启动器";
    adBlock.license = L"Built-in";
    adBlock.builtin = true;
    adBlock.deletable = false;
    adBlock.enabled = true;
    adBlock.installed = true;

    std::vector<PluginRecord> plugins = {
        clicker,
        timer,
        stopwatch,
        processTools,
    };
    if (!QuattroIsOfficialBuild()) {
        plugins.push_back(std::move(appLaunchLocker));
    }
    plugins.push_back(std::move(adBlock));
    return plugins;
}

std::vector<PluginRecord> PluginRegistry::LoadPlugins() {
    lastError_.clear();
    std::vector<PluginRecord> plugins = BuiltinPlugins();
    const auto state = RuntimeStates().find(stateKey_);
    if (state == RuntimeStates().end()) {
        return plugins;
    }

    for (const auto& [pluginId, enabled] : state->second.enabledOverrides) {
        if (PluginRecord* plugin = FindPlugin(plugins, pluginId)) {
            plugin->enabled = enabled;
        }
    }
    return plugins;
}

bool PluginRegistry::SetEnabled(const std::wstring& pluginId, bool enabled) {
    lastError_.clear();
    std::vector<PluginRecord> plugins = LoadPlugins();
    if (!FindPlugin(plugins, pluginId)) {
        lastError_ = L"内置工具不存在。";
        return false;
    }
    RuntimeStates()[stateKey_].enabledOverrides[pluginId] = enabled;
    return true;
}

bool PluginRegistry::IsEnabled(const std::wstring& pluginId) {
    for (const auto& plugin : LoadPlugins()) {
        if (plugin.id == pluginId) {
            return plugin.enabled;
        }
    }
    return false;
}

std::wstring PluginRegistry::GetSetting(const std::wstring& pluginId, const std::wstring& key, const std::wstring& fallback) {
    const auto state = RuntimeStates().find(stateKey_);
    if (state == RuntimeStates().end()) {
        return fallback;
    }
    const auto plugin = state->second.settings.find(pluginId);
    if (plugin == state->second.settings.end()) {
        return fallback;
    }
    const auto value = plugin->second.find(key);
    return value == plugin->second.end() ? fallback : value->second;
}

bool PluginRegistry::SetSetting(const std::wstring& pluginId, const std::wstring& key, const std::wstring& value) {
    lastError_.clear();
    std::vector<PluginRecord> plugins = LoadPlugins();
    if (!FindPlugin(plugins, pluginId)) {
        lastError_ = L"内置工具不存在。";
        return false;
    }
    RuntimeStates()[stateKey_].settings[pluginId][key] = value;
    return true;
}
