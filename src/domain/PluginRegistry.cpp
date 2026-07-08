#include "PluginRegistry.h"

#include "Utilities.h"

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

    PluginRecord portInspector;
    portInspector.id = L"quattro.builtin.port-inspector";
    portInspector.name = L"端口占用检查";
    portInspector.version = L"1.0.0";
    portInspector.category = L"builtin-tools";
    portInspector.kind = L"builtin-tool";
    portInspector.engine = L"port-inspector";
    portInspector.description = L"扫描指定端口占用，并可结束对应进程。";
    portInspector.permissions = L"进程查询, 结束进程";
    portInspector.author = L"Quattro快速启动器";
    portInspector.license = L"Built-in";
    portInspector.builtin = true;
    portInspector.deletable = false;
    portInspector.enabled = true;
    portInspector.installed = true;

    PluginRecord processInspector;
    processInspector.id = L"quattro.builtin.process-inspector";
    processInspector.name = L"进程ID查询";
    processInspector.version = L"1.0.0";
    processInspector.category = L"builtin-tools";
    processInspector.kind = L"builtin-tool";
    processInspector.engine = L"process-inspector";
    processInspector.description = L"按进程ID查询进程，并可结束对应进程。";
    processInspector.permissions = L"进程查询, 结束进程";
    processInspector.author = L"Quattro快速启动器";
    processInspector.license = L"Built-in";
    processInspector.builtin = true;
    processInspector.deletable = false;
    processInspector.enabled = true;
    processInspector.installed = true;

    return {
        clicker,
        timer,
        stopwatch,
        portInspector,
        processInspector,
    };
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
