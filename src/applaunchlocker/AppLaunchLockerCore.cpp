#include "AppLaunchLockerCore.h"

#include "JsonValue.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <taskschd.h>
#include <wbemidl.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <system_error>

namespace {
template <typename T>
void ReleaseCom(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

class ComApartment {
public:
    explicit ComApartment(DWORD mode = COINIT_MULTITHREADED) {
        const HRESULT result = CoInitializeEx(nullptr, mode);
        initialized_ = SUCCEEDED(result);
        usable_ = initialized_ || result == RPC_E_CHANGED_MODE;
    }
    ~ComApartment() { if (initialized_) CoUninitialize(); }
    bool usable() const { return usable_; }

private:
    bool initialized_ = false;
    bool usable_ = false;
};

struct RegKeyCloser {
    void operator()(HKEY key) const { if (key) RegCloseKey(key); }
};
using UniqueRegKey = std::unique_ptr<std::remove_pointer_t<HKEY>, RegKeyCloser>;

struct ServiceHandleCloser {
    void operator()(SC_HANDLE handle) const { if (handle) CloseServiceHandle(handle); }
};
using UniqueServiceHandle = std::unique_ptr<std::remove_pointer_t<SC_HANDLE>, ServiceHandleCloser>;

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

std::wstring NormalizeIdPart(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    return Lower(std::move(value));
}

std::wstring Hex64(std::uint64_t value) {
    std::wostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill(L'0') << value;
    return stream.str();
}

std::wstring StableId(StartupSourceType source, const std::wstring& location, const std::wstring& name) {
    const std::wstring input = StartupSourceKey(source) + L"|" + NormalizeIdPart(location) + L"|" + NormalizeIdPart(name);
    std::uint64_t hash = 1469598103934665603ull;
    for (wchar_t ch : input) {
        hash ^= static_cast<std::uint16_t>(ch);
        hash *= 1099511628211ull;
    }
    return Hex64(hash);
}

std::wstring LastErrorText(DWORD code = GetLastError()) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = length && buffer ? std::wstring(buffer, length) : L"错误 " + std::to_wstring(code);
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    return text;
}

std::wstring HResultText(HRESULT result) {
    return LastErrorText(static_cast<DWORD>(result));
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring output(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), length);
    return output;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string output(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), length, nullptr, nullptr);
    return output;
}

std::wstring EscapeJson(const std::wstring& value) {
    std::wstring output;
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\\': output += L"\\\\"; break;
        case L'"': output += L"\\\""; break;
        case L'\b': output += L"\\b"; break;
        case L'\f': output += L"\\f"; break;
        case L'\n': output += L"\\n"; break;
        case L'\r': output += L"\\r"; break;
        case L'\t': output += L"\\t"; break;
        default:
            if (ch < 0x20) {
                wchar_t escaped[7]{};
                swprintf_s(escaped, L"\\u%04x", static_cast<unsigned int>(ch));
                output += escaped;
            } else {
                output.push_back(ch);
            }
            break;
        }
    }
    return output;
}

std::wstring CurrentTimestamp() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04u-%02u-%02uT%02u:%02u:%02uZ",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

std::wstring MapValue(const std::map<std::wstring, std::wstring>& values, const wchar_t* key) {
    const auto found = values.find(key);
    return found == values.end() ? std::wstring{} : found->second;
}

bool ParseUnsigned(const std::wstring& value, DWORD& output) {
    if (value.empty()) return false;
    wchar_t* end = nullptr;
    const unsigned long parsed = wcstoul(value.c_str(), &end, 10);
    if (!end || *end != L'\0') return false;
    output = static_cast<DWORD>(parsed);
    return true;
}

void AddItem(
    ScanResult& result,
    StartupSourceType source,
    std::wstring name,
    std::wstring location,
    std::wstring command,
    bool requiresAdmin,
    bool canDisable,
    std::map<std::wstring, std::wstring> original = {}) {
    StartupItem item;
    item.id = StableId(source, location, name);
    item.name = std::move(name);
    item.source = source;
    item.location = std::move(location);
    item.command = std::move(command);
    item.requiresAdmin = requiresAdmin;
    item.canDisable = canDisable;
    item.readOnly = !canDisable;
    item.original = std::move(original);
    result.items.push_back(std::move(item));
}

struct RegistryLocation {
    HKEY hive;
    const wchar_t* hiveName;
    const wchar_t* key;
    StartupSourceType source;
    bool canDisable;
};

const std::array<RegistryLocation, 23> kRegistryLocations{{
    {HKEY_CURRENT_USER, L"HKCU", L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", StartupSourceType::Registry, true},
    {HKEY_CURRENT_USER, L"HKCU", L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", StartupSourceType::Registry, true},
    {HKEY_CURRENT_USER, L"HKCU", L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnceEx", StartupSourceType::Registry, true},
    {HKEY_LOCAL_MACHINE, L"HKLM", L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", StartupSourceType::Registry, true},
    {HKEY_LOCAL_MACHINE, L"HKLM", L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", StartupSourceType::Registry, true},
    {HKEY_LOCAL_MACHINE, L"HKLM", L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnceEx", StartupSourceType::Registry, true},
    {HKEY_LOCAL_MACHINE, L"HKLM", L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run", StartupSourceType::Registry, true},
    {HKEY_LOCAL_MACHINE, L"HKLM", L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnce", StartupSourceType::Registry, true},
    {HKEY_LOCAL_MACHINE, L"HKLM", L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnceEx", StartupSourceType::Registry, true},
    {HKEY_CURRENT_USER, L"HKCU", L"Software\\Microsoft\\Windows\\CurrentVersion\\RunServices", StartupSourceType::Registry, true},
    {HKEY_CURRENT_USER, L"HKCU", L"Software\\Microsoft\\Windows\\CurrentVersion\\RunServicesOnce", StartupSourceType::Registry, true},
    {HKEY_LOCAL_MACHINE, L"HKLM", L"Software\\Microsoft\\Windows\\CurrentVersion\\RunServices", StartupSourceType::Registry, true},
    {HKEY_LOCAL_MACHINE, L"HKLM", L"Software\\Microsoft\\Windows\\CurrentVersion\\RunServicesOnce", StartupSourceType::Registry, true},
    {HKEY_CURRENT_USER, L"HKCU", L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run", StartupSourceType::Registry, true},
    {HKEY_LOCAL_MACHINE, L"HKLM", L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run", StartupSourceType::Registry, true},
    {HKEY_CURRENT_USER, L"HKCU", L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", StartupSourceType::Registry, true},
    {HKEY_LOCAL_MACHINE, L"HKLM", L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", StartupSourceType::Registry, true},
    {HKEY_CURRENT_USER, L"HKCU", L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Windows", StartupSourceType::Registry, true},
    {HKEY_LOCAL_MACHINE, L"HKLM", L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", StartupSourceType::Winlogon, false},
    {HKEY_CURRENT_USER, L"HKCU", L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", StartupSourceType::Winlogon, false},
    {HKEY_LOCAL_MACHINE, L"HKLM", L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows", StartupSourceType::AppInitDll, false},
    {HKEY_LOCAL_MACHINE, L"HKLM", L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Windows", StartupSourceType::AppInitDll, false},
    {HKEY_CURRENT_USER, L"HKCU", L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run", StartupSourceType::Registry, true},
}};

bool RelevantNamedValue(const RegistryLocation& location, const std::wstring& valueName) {
    if (location.source == StartupSourceType::Winlogon) {
        const std::wstring lower = Lower(valueName);
        return lower == L"shell" || lower == L"userinit";
    }
    if (location.source == StartupSourceType::AppInitDll) {
        return Lower(valueName) == L"appinit_dlls";
    }
    if (std::wstring(location.key).find(L"CurrentVersion\\Windows") != std::wstring::npos) {
        const std::wstring lower = Lower(valueName);
        return lower == L"load" || lower == L"run";
    }
    if (std::wstring(location.key).find(L"Policies\\System") != std::wstring::npos) {
        // 该键还含 EnableLUA、ConsentPromptBehaviorAdmin 等 UAC 安全策略值，
        // 只把自定义 Shell 劫持点当作启动项，绝不枚举其它策略值。
        return Lower(valueName) == L"shell";
    }
    return true;
}

void ScanRegistry(ScanResult& result) {
    for (const auto& location : kRegistryLocations) {
        HKEY rawKey = nullptr;
        const LSTATUS opened = RegOpenKeyExW(location.hive, location.key, 0, KEY_QUERY_VALUE, &rawKey);
        if (opened == ERROR_FILE_NOT_FOUND) continue;
        if (opened != ERROR_SUCCESS) {
            result.warnings.push_back(std::wstring(location.hiveName) + L"\\" + location.key + L"：" + LastErrorText(opened));
            continue;
        }
        UniqueRegKey key(rawKey);
        DWORD index = 0;
        for (;;) {
            std::wstring name(32768, L'\0');
            DWORD nameLength = static_cast<DWORD>(name.size());
            DWORD type = 0;
            DWORD dataBytes = 0;
            LSTATUS status = RegEnumValueW(rawKey, index, name.data(), &nameLength, nullptr, &type, nullptr, &dataBytes);
            if (status == ERROR_NO_MORE_ITEMS) break;
            if (status == ERROR_MORE_DATA) { ++index; continue; }
            if (status != ERROR_SUCCESS) {
                ++index;
                continue;
            }
            name.resize(nameLength);
            ++index;
            if (!RelevantNamedValue(location, name)) continue;
            if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
            std::vector<BYTE> data(static_cast<std::size_t>(dataBytes) + sizeof(wchar_t), 0);
            DWORD readBytes = dataBytes;
            if (RegQueryValueExW(rawKey, name.c_str(), nullptr, &type, data.data(), &readBytes) != ERROR_SUCCESS) continue;
            const std::wstring command(reinterpret_cast<const wchar_t*>(data.data()));
            if (command.empty() && location.source != StartupSourceType::Winlogon) continue;
            const std::wstring fullLocation = std::wstring(location.hiveName) + L"\\" + location.key;
            const bool canDisable = location.canDisable;
            AddItem(result, location.source, name.empty() ? L"(默认)" : name, fullLocation, command,
                location.hive == HKEY_LOCAL_MACHINE, canDisable,
                {{L"hive", location.hiveName}, {L"key", location.key}, {L"valueName", name},
                 {L"valueType", std::to_wstring(type)}, {L"valueData", command}});
        }
    }
}

std::wstring ResolveShortcut(const std::filesystem::path& path) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return path.wstring();
    IPersistFile* persist = nullptr;
    if (FAILED(link->QueryInterface(IID_PPV_ARGS(&persist)))) {
        link->Release();
        return path.wstring();
    }
    std::wstring output = path.wstring();
    if (SUCCEEDED(persist->Load(path.c_str(), STGM_READ))) {
        wchar_t target[32768]{};
        wchar_t arguments[32768]{};
        WIN32_FIND_DATAW data{};
        if (SUCCEEDED(link->GetPath(target, static_cast<int>(std::size(target)), &data, SLGP_RAWPATH))) {
            link->GetArguments(arguments, static_cast<int>(std::size(arguments)));
            output = target;
            if (*arguments) output += L" " + std::wstring(arguments);
        }
    }
    persist->Release();
    link->Release();
    return output;
}

void ScanStartupFolder(ScanResult& result, REFKNOWNFOLDERID folderId, bool requiresAdmin) {
    PWSTR rawPath = nullptr;
    const HRESULT resolved = SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &rawPath);
    if (FAILED(resolved) || !rawPath) {
        result.warnings.push_back(L"启动文件夹：" + HResultText(resolved));
        if (rawPath) CoTaskMemFree(rawPath);
        return;
    }
    const std::filesystem::path folder(rawPath);
    CoTaskMemFree(rawPath);
    std::error_code error;
    if (!std::filesystem::exists(folder, error)) return;
    for (std::filesystem::directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, error), end;
         it != end; it.increment(error)) {
        if (error) {
            result.warnings.push_back(L"启动文件夹：" + Utf8ToWide(error.message()));
            error.clear();
            continue;
        }
        if (!it->is_regular_file(error)) continue;
        const std::filesystem::path path = it->path();
        const std::wstring extension = Lower(path.extension().wstring());
        const bool supported = extension == L".lnk" || extension == L".url" || extension == L".bat" ||
            extension == L".cmd" || extension == L".ps1" || extension == L".vbs" || extension == L".js" || extension == L".exe";
        if (!supported) continue;
        const std::wstring command = extension == L".lnk" ? ResolveShortcut(path) : path.wstring();
        AddItem(result, StartupSourceType::StartupFolder, path.stem().wstring(), path.wstring(), command,
            requiresAdmin, true, {{L"originalPath", path.wstring()}});
    }
}

std::wstring BstrText(BSTR value) {
    return value ? std::wstring(value, SysStringLen(value)) : std::wstring{};
}

std::wstring TaskCommand(IRegisteredTask* task) {
    ITaskDefinition* definition = nullptr;
    if (FAILED(task->get_Definition(&definition)) || !definition) return {};
    IActionCollection* actions = nullptr;
    if (FAILED(definition->get_Actions(&actions)) || !actions) {
        definition->Release();
        return {};
    }
    LONG count = 0;
    actions->get_Count(&count);
    std::wstring command;
    for (LONG index = 1; index <= count; ++index) {
        IAction* action = nullptr;
        if (FAILED(actions->get_Item(index, &action)) || !action) continue;
        TASK_ACTION_TYPE type{};
        action->get_Type(&type);
        if (type == TASK_ACTION_EXEC) {
            IExecAction* exec = nullptr;
            if (SUCCEEDED(action->QueryInterface(IID_PPV_ARGS(&exec))) && exec) {
                BSTR path = nullptr;
                BSTR arguments = nullptr;
                exec->get_Path(&path);
                exec->get_Arguments(&arguments);
                if (!command.empty()) command += L"; ";
                command += BstrText(path);
                if (arguments && SysStringLen(arguments)) command += L" " + BstrText(arguments);
                if (path) SysFreeString(path);
                if (arguments) SysFreeString(arguments);
                exec->Release();
            }
        }
        action->Release();
    }
    actions->Release();
    definition->Release();
    return command;
}

bool TaskHasAutomaticTrigger(IRegisteredTask* task) {
    ITaskDefinition* definition = nullptr;
    if (FAILED(task->get_Definition(&definition)) || !definition) return false;
    ITriggerCollection* triggers = nullptr;
    if (FAILED(definition->get_Triggers(&triggers)) || !triggers) {
        definition->Release();
        return false;
    }
    LONG count = 0;
    triggers->get_Count(&count);
    bool automatic = false;
    for (LONG index = 1; index <= count && !automatic; ++index) {
        ITrigger* trigger = nullptr;
        if (FAILED(triggers->get_Item(index, &trigger)) || !trigger) continue;
        TASK_TRIGGER_TYPE2 type{};
        if (SUCCEEDED(trigger->get_Type(&type))) {
            automatic = type != TASK_TRIGGER_CUSTOM_TRIGGER_01;
        }
        trigger->Release();
    }
    triggers->Release();
    definition->Release();
    return automatic;
}

void ScanTaskFolder(ITaskFolder* folder, ScanResult& result) {
    IRegisteredTaskCollection* tasks = nullptr;
    if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, &tasks)) && tasks) {
        LONG count = 0;
        tasks->get_Count(&count);
        for (LONG index = 1; index <= count; ++index) {
            IRegisteredTask* task = nullptr;
            VARIANT itemIndex{};
            VariantInit(&itemIndex);
            V_VT(&itemIndex) = VT_I4;
            V_I4(&itemIndex) = index;
            if (FAILED(tasks->get_Item(itemIndex, &task)) || !task) continue;
            VARIANT_BOOL enabled = VARIANT_FALSE;
            task->get_Enabled(&enabled);
            if (enabled == VARIANT_FALSE || !TaskHasAutomaticTrigger(task)) {
                task->Release();
                continue;
            }
            BSTR name = nullptr;
            BSTR path = nullptr;
            task->get_Name(&name);
            task->get_Path(&path);
            const std::wstring taskName = BstrText(name);
            const std::wstring taskPath = BstrText(path);
            const bool microsoft = Lower(taskPath).rfind(L"\\microsoft\\windows\\", 0) == 0;
            AddItem(result, StartupSourceType::ScheduledTask, taskName, taskPath, TaskCommand(task), true, !microsoft,
                {{L"taskPath", taskPath}, {L"wasEnabled", L"1"}});
            if (name) SysFreeString(name);
            if (path) SysFreeString(path);
            task->Release();
        }
        tasks->Release();
    }

    ITaskFolderCollection* folders = nullptr;
    if (FAILED(folder->GetFolders(0, &folders)) || !folders) return;
    LONG count = 0;
    folders->get_Count(&count);
    for (LONG index = 1; index <= count; ++index) {
        ITaskFolder* child = nullptr;
        VARIANT itemIndex{};
        VariantInit(&itemIndex);
        V_VT(&itemIndex) = VT_I4;
        V_I4(&itemIndex) = index;
        if (SUCCEEDED(folders->get_Item(itemIndex, &child)) && child) {
            ScanTaskFolder(child, result);
            child->Release();
        }
    }
    folders->Release();
}

void ScanScheduledTasks(ScanResult& result) {
    ITaskService* service = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service));
    if (FAILED(hr) || !service) {
        result.warnings.push_back(L"计划任务：" + HResultText(hr));
        return;
    }
    VARIANT empty{};
    VariantInit(&empty);
    hr = service->Connect(empty, empty, empty, empty);
    if (FAILED(hr)) {
        result.warnings.push_back(L"计划任务：" + HResultText(hr));
        service->Release();
        return;
    }
    BSTR rootPath = SysAllocString(L"\\");
    ITaskFolder* root = nullptr;
    hr = service->GetFolder(rootPath, &root);
    SysFreeString(rootPath);
    if (SUCCEEDED(hr) && root) {
        ScanTaskFolder(root, result);
        root->Release();
    } else {
        result.warnings.push_back(L"计划任务：" + HResultText(hr));
    }
    service->Release();
}

std::wstring ExpandEnvironment(const std::wstring& value) {
    const DWORD required = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (!required) return value;
    std::wstring output(required, L'\0');
    if (!ExpandEnvironmentStringsW(value.c_str(), output.data(), required)) return value;
    if (!output.empty() && output.back() == L'\0') output.pop_back();
    return output;
}

std::wstring ExecutableFromCommand(std::wstring command) {
    command = ExpandEnvironment(command);
    while (!command.empty() && std::iswspace(command.front())) command.erase(command.begin());
    if (command.empty()) return {};
    if (command.front() == L'"') {
        const std::size_t end = command.find(L'"', 1);
        return end == std::wstring::npos ? command.substr(1) : command.substr(1, end - 1);
    }
    const std::size_t end = command.find_first_of(L" \t");
    return command.substr(0, end);
}

bool IsWindowsPath(const std::wstring& command) {
    const std::wstring normalizedCommand = NormalizeIdPart(command);
    if (normalizedCommand.find(L"%systemroot%") != std::wstring::npos ||
        normalizedCommand.rfind(L"\\systemroot\\", 0) == 0 ||
        normalizedCommand.rfind(L"system32\\", 0) == 0) {
        return true;
    }
    const std::wstring executable = ExecutableFromCommand(command);
    if (executable.find(L'\\') == std::wstring::npos && executable.find(L'/') == std::wstring::npos) return true;
    wchar_t windows[MAX_PATH]{};
    if (!GetWindowsDirectoryW(windows, static_cast<UINT>(std::size(windows)))) return false;
    std::wstring path = NormalizeIdPart(executable);
    std::wstring root = NormalizeIdPart(windows);
    if (!root.empty() && root.back() != L'\\') root.push_back(L'\\');
    return path.rfind(root, 0) == 0;
}

void ScanServices(ScanResult& result) {
    SC_HANDLE rawManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT);
    if (!rawManager) {
        result.warnings.push_back(L"服务：" + LastErrorText());
        return;
    }
    UniqueServiceHandle manager(rawManager);
    DWORD bytesNeeded = 0;
    DWORD count = 0;
    DWORD resume = 0;
    EnumServicesStatusExW(rawManager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32 | SERVICE_DRIVER, SERVICE_STATE_ALL,
        nullptr, 0, &bytesNeeded, &count, &resume, nullptr);
    if (GetLastError() != ERROR_MORE_DATA || bytesNeeded == 0) return;
    std::vector<BYTE> buffer(bytesNeeded);
    if (!EnumServicesStatusExW(rawManager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32 | SERVICE_DRIVER, SERVICE_STATE_ALL,
            buffer.data(), static_cast<DWORD>(buffer.size()), &bytesNeeded, &count, &resume, nullptr)) {
        result.warnings.push_back(L"服务：" + LastErrorText());
        return;
    }
    auto* services = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
    for (DWORD index = 0; index < count; ++index) {
        SC_HANDLE rawService = OpenServiceW(rawManager, services[index].lpServiceName, SERVICE_QUERY_CONFIG);
        if (!rawService) continue;
        UniqueServiceHandle service(rawService);
        DWORD configBytes = 0;
        QueryServiceConfigW(rawService, nullptr, 0, &configBytes);
        if (!configBytes) continue;
        std::vector<BYTE> configBuffer(configBytes);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(configBuffer.data());
        if (!QueryServiceConfigW(rawService, config, configBytes, &configBytes)) continue;
        const bool driver = (config->dwServiceType & (SERVICE_KERNEL_DRIVER | SERVICE_FILE_SYSTEM_DRIVER | SERVICE_RECOGNIZER_DRIVER)) != 0;
        if (!driver && config->dwStartType != SERVICE_AUTO_START) continue;
        bool delayed = false;
        SERVICE_DELAYED_AUTO_START_INFO delayedInfo{};
        DWORD delayedBytes = 0;
        if (QueryServiceConfig2W(rawService, SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                reinterpret_cast<LPBYTE>(&delayedInfo), sizeof(delayedInfo), &delayedBytes)) {
            delayed = delayedInfo.fDelayedAutostart != FALSE;
        }
        bool protectedService = false;
#ifdef SERVICE_CONFIG_LAUNCH_PROTECTED
        SERVICE_LAUNCH_PROTECTED_INFO protectedInfo{};
        DWORD protectedBytes = 0;
        if (QueryServiceConfig2W(rawService, SERVICE_CONFIG_LAUNCH_PROTECTED,
                reinterpret_cast<LPBYTE>(&protectedInfo), sizeof(protectedInfo), &protectedBytes)) {
            protectedService = protectedInfo.dwLaunchProtected != SERVICE_LAUNCH_PROTECTED_NONE;
        }
#endif
        const std::wstring command = config->lpBinaryPathName ? config->lpBinaryPathName : L"";
        const bool canDisable = !driver && !protectedService && !IsWindowsPath(command);
        AddItem(result, driver ? StartupSourceType::Driver : StartupSourceType::Service,
            services[index].lpDisplayName && *services[index].lpDisplayName ? services[index].lpDisplayName : services[index].lpServiceName,
            services[index].lpServiceName, command, true, canDisable,
            {{L"serviceName", services[index].lpServiceName}, {L"startType", std::to_wstring(config->dwStartType)},
             {L"delayed", delayed ? L"1" : L"0"}});
    }
}

std::wstring VariantString(IWbemClassObject* object, const wchar_t* property) {
    VARIANT value{};
    VariantInit(&value);
    std::wstring output;
    if (SUCCEEDED(object->Get(property, 0, &value, nullptr, nullptr))) {
        if (V_VT(&value) == VT_BSTR && V_BSTR(&value)) output = BstrText(V_BSTR(&value));
    }
    VariantClear(&value);
    return output;
}

void QueryWmi(IWbemServices* services, const wchar_t* query, const wchar_t* typeName, ScanResult& result) {
    BSTR language = SysAllocString(L"WQL");
    BSTR queryText = SysAllocString(query);
    IEnumWbemClassObject* enumerator = nullptr;
    const HRESULT hr = services->ExecQuery(language, queryText,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumerator);
    SysFreeString(language);
    SysFreeString(queryText);
    if (FAILED(hr) || !enumerator) return;
    for (;;) {
        IWbemClassObject* object = nullptr;
        ULONG returned = 0;
        if (enumerator->Next(2000, 1, &object, &returned) != WBEM_S_NO_ERROR || !returned || !object) break;
        std::wstring name = VariantString(object, L"Name");
        if (name.empty()) name = VariantString(object, L"__RELPATH");
        std::wstring command = VariantString(object, L"CommandLineTemplate");
        if (command.empty()) command = VariantString(object, L"ScriptText");
        const std::wstring location = std::wstring(L"root\\subscription\\") + typeName + L"\\" + name;
        AddItem(result, StartupSourceType::WmiSubscription, name.empty() ? typeName : name, location, command, true, false);
        object->Release();
    }
    enumerator->Release();
}

void ScanWmi(ScanResult& result) {
    IWbemLocator* locator = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator));
    if (FAILED(hr) || !locator) {
        result.warnings.push_back(L"WMI：" + HResultText(hr));
        return;
    }
    IWbemServices* services = nullptr;
    BSTR namespaceName = SysAllocString(L"ROOT\\subscription");
    hr = locator->ConnectServer(namespaceName, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
    SysFreeString(namespaceName);
    if (FAILED(hr) || !services) {
        result.warnings.push_back(L"WMI：" + HResultText(hr));
        locator->Release();
        return;
    }
    CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    QueryWmi(services, L"SELECT * FROM __EventFilter", L"__EventFilter", result);
    QueryWmi(services, L"SELECT * FROM CommandLineEventConsumer", L"CommandLineEventConsumer", result);
    QueryWmi(services, L"SELECT * FROM ActiveScriptEventConsumer", L"ActiveScriptEventConsumer", result);
    QueryWmi(services, L"SELECT * FROM __FilterToConsumerBinding", L"__FilterToConsumerBinding", result);
    services->Release();
    locator->Release();
}

void ScanIfeoView(ScanResult& result, REGSAM viewFlag, const wchar_t* viewName) {
    constexpr const wchar_t* keyPath = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options";
    HKEY rawRoot = nullptr;
    const LSTATUS opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, KEY_ENUMERATE_SUB_KEYS | viewFlag, &rawRoot);
    if (opened == ERROR_FILE_NOT_FOUND) return;
    if (opened != ERROR_SUCCESS) {
        result.warnings.push_back(std::wstring(L"IFEO ") + viewName + L"：" + LastErrorText(opened));
        return;
    }
    UniqueRegKey root(rawRoot);
    for (DWORD index = 0;; ++index) {
        wchar_t name[512]{};
        DWORD length = static_cast<DWORD>(std::size(name));
        const LSTATUS enumerated = RegEnumKeyExW(rawRoot, index, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (enumerated == ERROR_NO_MORE_ITEMS) break;
        if (enumerated != ERROR_SUCCESS) continue;
        HKEY rawChild = nullptr;
        if (RegOpenKeyExW(rawRoot, name, 0, KEY_QUERY_VALUE | viewFlag, &rawChild) != ERROR_SUCCESS) continue;
        UniqueRegKey child(rawChild);
        DWORD type = 0;
        DWORD bytes = 0;
        if (RegQueryValueExW(rawChild, L"Debugger", nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
            (type != REG_SZ && type != REG_EXPAND_SZ)) continue;
        std::vector<BYTE> data(bytes + sizeof(wchar_t), 0);
        if (RegQueryValueExW(rawChild, L"Debugger", nullptr, &type, data.data(), &bytes) != ERROR_SUCCESS) continue;
        const std::wstring debugger(reinterpret_cast<const wchar_t*>(data.data()));
        // 本工具（广告拦截）写入的 IFEO Debugger 指向自身 --ifeo-noop，不作为第三方可疑项展示，
        // 避免与「广告拦截」的已拦截列表重复。
        if (Lower(debugger).find(L"--ifeo-noop") != std::wstring::npos) continue;
        const std::wstring location = std::wstring(L"HKLM\\") + keyPath + L"\\" + name + L" (" + viewName + L")";
        AddItem(result, StartupSourceType::Ifeo, name, location, debugger, true, false);
    }
}

void ScanIfeo(ScanResult& result) {
    ScanIfeoView(result, KEY_WOW64_64KEY, L"64 位");
    ScanIfeoView(result, KEY_WOW64_32KEY, L"32 位");
}

void ScanActiveSetupView(ScanResult& result, const wchar_t* keyPath, const wchar_t* viewName) {
    HKEY rawRoot = nullptr;
    const LSTATUS opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, KEY_ENUMERATE_SUB_KEYS, &rawRoot);
    if (opened == ERROR_FILE_NOT_FOUND) return;
    if (opened != ERROR_SUCCESS) {
        result.warnings.push_back(std::wstring(L"Active Setup ") + viewName + L"：" + LastErrorText(opened));
        return;
    }
    UniqueRegKey root(rawRoot);
    for (DWORD index = 0;; ++index) {
        wchar_t name[512]{};
        DWORD length = static_cast<DWORD>(std::size(name));
        const LSTATUS enumerated = RegEnumKeyExW(rawRoot, index, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (enumerated == ERROR_NO_MORE_ITEMS) break;
        if (enumerated != ERROR_SUCCESS) continue;
        HKEY rawChild = nullptr;
        if (RegOpenKeyExW(rawRoot, name, 0, KEY_QUERY_VALUE, &rawChild) != ERROR_SUCCESS) continue;
        UniqueRegKey child(rawChild);
        DWORD type = 0;
        DWORD bytes = 0;
        if (RegQueryValueExW(rawChild, L"StubPath", nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
            (type != REG_SZ && type != REG_EXPAND_SZ)) continue;
        std::vector<BYTE> data(static_cast<std::size_t>(bytes) + sizeof(wchar_t), 0);
        if (RegQueryValueExW(rawChild, L"StubPath", nullptr, &type, data.data(), &bytes) != ERROR_SUCCESS) continue;
        const std::wstring stubPath(reinterpret_cast<const wchar_t*>(data.data()));
        if (stubPath.empty()) continue;
        const std::wstring subKey = std::wstring(keyPath) + L"\\" + name;
        const std::wstring location = std::wstring(L"HKLM\\") + subKey + L" (" + viewName + L")";
        AddItem(result, StartupSourceType::ActiveSetup, name, location, stubPath, true, true,
            {{L"hive", L"HKLM"}, {L"key", subKey}, {L"valueName", L"StubPath"},
             {L"valueType", std::to_wstring(type)}, {L"valueData", stubPath}});
    }
}

void ScanActiveSetup(ScanResult& result) {
    ScanActiveSetupView(result, L"SOFTWARE\\Microsoft\\Active Setup\\Installed Components", L"64 位");
    ScanActiveSetupView(result, L"SOFTWARE\\WOW6432Node\\Microsoft\\Active Setup\\Installed Components", L"32 位");
}

std::wstring ReadStringValue(HKEY key, const wchar_t* valueName, DWORD& typeOut) {
    typeOut = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &typeOut, nullptr, &bytes) != ERROR_SUCCESS) return {};
    std::vector<BYTE> data(static_cast<std::size_t>(bytes) + sizeof(wchar_t), 0);
    if (RegQueryValueExW(key, valueName, nullptr, &typeOut, data.data(), &bytes) != ERROR_SUCCESS) return {};
    return std::wstring(reinterpret_cast<const wchar_t*>(data.data()));
}

std::vector<std::wstring> ReadMultiString(HKEY key, const wchar_t* valueName) {
    std::vector<std::wstring> values;
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS || type != REG_MULTI_SZ) {
        return values;
    }
    std::vector<BYTE> data(static_cast<std::size_t>(bytes) + 2 * sizeof(wchar_t), 0);
    if (RegQueryValueExW(key, valueName, nullptr, &type, data.data(), &bytes) != ERROR_SUCCESS) return values;
    const wchar_t* cursor = reinterpret_cast<const wchar_t*>(data.data());
    while (*cursor) {
        std::wstring entry(cursor);
        cursor += entry.size() + 1;
        if (!entry.empty()) values.push_back(std::move(entry));
    }
    return values;
}

void ScanWinlogonNotify(ScanResult& result) {
    constexpr const wchar_t* keyPath =
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\\Notify";
    HKEY rawRoot = nullptr;
    const LSTATUS opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, KEY_ENUMERATE_SUB_KEYS, &rawRoot);
    if (opened == ERROR_FILE_NOT_FOUND) return;
    if (opened != ERROR_SUCCESS) {
        result.warnings.push_back(std::wstring(L"登录通知：") + LastErrorText(opened));
        return;
    }
    UniqueRegKey root(rawRoot);
    for (DWORD index = 0;; ++index) {
        wchar_t name[512]{};
        DWORD length = static_cast<DWORD>(std::size(name));
        const LSTATUS enumerated = RegEnumKeyExW(rawRoot, index, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (enumerated == ERROR_NO_MORE_ITEMS) break;
        if (enumerated != ERROR_SUCCESS) continue;
        HKEY rawChild = nullptr;
        if (RegOpenKeyExW(rawRoot, name, 0, KEY_QUERY_VALUE, &rawChild) != ERROR_SUCCESS) continue;
        UniqueRegKey child(rawChild);
        DWORD type = 0;
        const std::wstring dll = ReadStringValue(rawChild, L"DllName", type);
        if (dll.empty()) continue;
        const std::wstring location = std::wstring(L"HKLM\\") + keyPath + L"\\" + name;
        AddItem(result, StartupSourceType::WinlogonNotify, name, location, dll, true, false);
    }
}

void ScanSessionManager(ScanResult& result) {
    constexpr const wchar_t* keyPath = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager";
    HKEY rawKey = nullptr;
    const LSTATUS opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, KEY_QUERY_VALUE, &rawKey);
    if (opened == ERROR_FILE_NOT_FOUND) return;
    if (opened != ERROR_SUCCESS) {
        result.warnings.push_back(std::wstring(L"Session Manager：") + LastErrorText(opened));
        return;
    }
    UniqueRegKey key(rawKey);
    const std::wstring location = std::wstring(L"HKLM\\") + keyPath;
    for (const std::wstring& entry : ReadMultiString(rawKey, L"BootExecute")) {
        AddItem(result, StartupSourceType::BootExecute, entry, location + L"\\BootExecute", entry, true, false);
    }
    HKEY rawKnown = nullptr;
    if (RegOpenKeyExW(rawKey, L"KnownDLLs", 0, KEY_QUERY_VALUE, &rawKnown) == ERROR_SUCCESS) {
        UniqueRegKey known(rawKnown);
        for (DWORD index = 0;; ++index) {
            wchar_t name[512]{};
            DWORD nameLength = static_cast<DWORD>(std::size(name));
            DWORD type = 0;
            const LSTATUS status = RegEnumValueW(rawKnown, index, name, &nameLength, nullptr, &type, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS) break;
            if (status != ERROR_SUCCESS) continue;
            if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
            DWORD valueType = 0;
            const std::wstring dll = ReadStringValue(rawKnown, name, valueType);
            if (dll.empty()) continue;
            AddItem(result, StartupSourceType::KnownDll, name, location + L"\\KnownDLLs", dll, true, false);
        }
    }
}

void ScanAppCertDlls(ScanResult& result) {
    constexpr const wchar_t* keyPath =
        L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\AppCertDlls";
    HKEY rawKey = nullptr;
    const LSTATUS opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, KEY_QUERY_VALUE, &rawKey);
    if (opened == ERROR_FILE_NOT_FOUND) return;
    if (opened != ERROR_SUCCESS) {
        result.warnings.push_back(std::wstring(L"AppCert DLL：") + LastErrorText(opened));
        return;
    }
    UniqueRegKey key(rawKey);
    const std::wstring location = std::wstring(L"HKLM\\") + keyPath;
    for (DWORD index = 0;; ++index) {
        wchar_t name[512]{};
        DWORD nameLength = static_cast<DWORD>(std::size(name));
        DWORD type = 0;
        const LSTATUS status = RegEnumValueW(rawKey, index, name, &nameLength, nullptr, &type, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) continue;
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
        DWORD valueType = 0;
        const std::wstring dll = ReadStringValue(rawKey, name, valueType);
        if (dll.empty()) continue;
        AddItem(result, StartupSourceType::AppCertDll, name, location, dll, true, false);
    }
}

std::wstring ResolveClsidDll(HKEY hive, const std::wstring& clsid) {
    const std::wstring server = L"Software\\Classes\\CLSID\\" + clsid + L"\\InprocServer32";
    HKEY rawKey = nullptr;
    if (RegOpenKeyExW(hive, server.c_str(), 0, KEY_QUERY_VALUE, &rawKey) != ERROR_SUCCESS) return {};
    UniqueRegKey key(rawKey);
    DWORD type = 0;
    return ReadStringValue(rawKey, L"", type);
}

void ScanShellExtensionKey(ScanResult& result, HKEY hive, const wchar_t* hiveName, const wchar_t* keyPath,
    const wchar_t* label) {
    HKEY rawKey = nullptr;
    const LSTATUS opened = RegOpenKeyExW(hive, keyPath, 0, KEY_ENUMERATE_SUB_KEYS, &rawKey);
    if (opened == ERROR_FILE_NOT_FOUND) return;
    if (opened != ERROR_SUCCESS) {
        result.warnings.push_back(std::wstring(L"Shell 扩展：") + LastErrorText(opened));
        return;
    }
    UniqueRegKey key(rawKey);
    const std::wstring location = std::wstring(hiveName) + L"\\" + keyPath;
    for (DWORD index = 0;; ++index) {
        wchar_t name[512]{};
        DWORD length = static_cast<DWORD>(std::size(name));
        const LSTATUS enumerated = RegEnumKeyExW(rawKey, index, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (enumerated == ERROR_NO_MORE_ITEMS) break;
        if (enumerated != ERROR_SUCCESS) continue;
        HKEY rawChild = nullptr;
        if (RegOpenKeyExW(rawKey, name, 0, KEY_QUERY_VALUE, &rawChild) != ERROR_SUCCESS) continue;
        UniqueRegKey child(rawChild);
        DWORD type = 0;
        std::wstring clsid = ReadStringValue(rawChild, L"", type);
        if (clsid.empty() || clsid.front() != L'{') clsid = name;
        std::wstring dll = ResolveClsidDll(hive, clsid);
        if (dll.empty()) dll = ResolveClsidDll(HKEY_LOCAL_MACHINE, clsid);
        const std::wstring display = std::wstring(label) + L"：" + name;
        AddItem(result, StartupSourceType::ShellExtension, display, location, dll.empty() ? clsid : dll, true, false);
    }
}

void ScanShellExtensions(ScanResult& result) {
    ScanShellExtensionKey(result, HKEY_LOCAL_MACHINE, L"HKLM",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers",
        L"图标叠加");
    ScanShellExtensionKey(result, HKEY_CURRENT_USER, L"HKCU",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers",
        L"图标叠加");
}

void CollectStartupApproved(HKEY hive, const wchar_t* keyPath, std::vector<std::wstring>& disabledNames) {
    HKEY rawKey = nullptr;
    if (RegOpenKeyExW(hive, keyPath, 0, KEY_QUERY_VALUE, &rawKey) != ERROR_SUCCESS) return;
    UniqueRegKey key(rawKey);
    for (DWORD index = 0;; ++index) {
        wchar_t name[512]{};
        DWORD nameLength = static_cast<DWORD>(std::size(name));
        DWORD type = 0;
        DWORD bytes = 0;
        LSTATUS status = RegEnumValueW(rawKey, index, name, &nameLength, nullptr, &type, nullptr, &bytes);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) continue;
        if (type != REG_BINARY || bytes == 0) continue;
        std::vector<BYTE> data(bytes, 0);
        DWORD readBytes = bytes;
        if (RegQueryValueExW(rawKey, name, nullptr, &type, data.data(), &readBytes) != ERROR_SUCCESS) continue;
        // StartupApproved blob: 首字节偶数（02/06）为启用，奇数（03/07）为禁用。
        if (!data.empty() && (data[0] & 1) != 0) disabledNames.push_back(Lower(std::wstring(name)));
    }
}

void AlignStartupApproved(ScanResult& result) {
    std::vector<std::wstring> disabledRun;
    std::vector<std::wstring> disabledFolder;
    CollectStartupApproved(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", disabledRun);
    CollectStartupApproved(HKEY_LOCAL_MACHINE,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", disabledRun);
    CollectStartupApproved(HKEY_LOCAL_MACHINE,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run32", disabledRun);
    CollectStartupApproved(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder", disabledFolder);
    CollectStartupApproved(HKEY_LOCAL_MACHINE,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder", disabledFolder);
    if (disabledRun.empty() && disabledFolder.empty()) return;

    const auto contains = [](const std::vector<std::wstring>& names, const std::wstring& value) {
        return std::find(names.begin(), names.end(), Lower(value)) != names.end();
    };
    for (StartupItem& item : result.items) {
        bool systemDisabled = false;
        if (item.source == StartupSourceType::Registry) {
            const auto found = item.original.find(L"valueName");
            const auto keyField = item.original.find(L"key");
            const bool runKey = keyField != item.original.end() &&
                Lower(keyField->second).find(L"currentversion\\run") != std::wstring::npos;
            if (runKey && found != item.original.end()) systemDisabled = contains(disabledRun, found->second);
        } else if (item.source == StartupSourceType::StartupFolder) {
            const auto found = item.original.find(L"originalPath");
            if (found != item.original.end()) {
                systemDisabled = contains(disabledFolder, std::filesystem::path(found->second).filename().wstring());
            }
        }
        if (systemDisabled) {
            item.name += L"（已被系统禁用）";
            item.canDisable = false;
            item.readOnly = true;
        }
    }
}

HKEY HiveFromText(const std::wstring& value) {
    if (value == L"HKCU") return HKEY_CURRENT_USER;
    if (value == L"HKLM") return HKEY_LOCAL_MACHINE;
    return nullptr;
}

// ---- StartupApproved（方案 B1）：标准 Run 项与启动文件夹改用系统“启动”开关 ----

std::wstring BytesToHex(const std::vector<BYTE>& bytes) {
    static const wchar_t digits[] = L"0123456789abcdef";
    std::wstring text;
    text.reserve(bytes.size() * 2);
    for (BYTE b : bytes) {
        text.push_back(digits[b >> 4]);
        text.push_back(digits[b & 0x0f]);
    }
    return text;
}

std::vector<BYTE> HexToBytes(const std::wstring& text) {
    const auto value = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    std::vector<BYTE> bytes;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        const int hi = value(text[i]);
        const int lo = value(text[i + 1]);
        if (hi < 0 || lo < 0) return {};
        bytes.push_back(static_cast<BYTE>((hi << 4) | lo));
    }
    return bytes;
}

std::vector<BYTE> MakeStartupApprovedBlob(bool enabled) {
    std::vector<BYTE> blob(12, 0);
    blob[0] = enabled ? 0x02 : 0x03;
    if (!enabled) {
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        CopyMemory(blob.data() + 4, &now, sizeof(now));
    }
    return blob;
}

struct StartupApprovedTarget {
    bool eligible = false;
    HKEY hive = nullptr;
    std::wstring hiveName;
    std::wstring key;
    std::wstring valueName;
};

StartupApprovedTarget ResolveStartupApprovedTarget(const DisabledRecord& record) {
    StartupApprovedTarget target;
    constexpr const wchar_t* base =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\";
    if (record.source == StartupSourceType::Registry) {
        const std::wstring key = MapValue(record.original, L"key");
        const std::wstring lowerKey = Lower(key);
        // 仅标准 Run 键（排除 RunOnce/RunOnceEx/RunServices/Policies）。
        if (lowerKey.size() < 4 || lowerKey.substr(lowerKey.size() - 4) != L"\\run") return target;
        if (lowerKey.find(L"policies") != std::wstring::npos) return target;
        target.hive = HiveFromText(MapValue(record.original, L"hive"));
        if (!target.hive) return target;
        target.hiveName = MapValue(record.original, L"hive");
        const bool wow = lowerKey.find(L"wow6432node") != std::wstring::npos;
        target.key = std::wstring(base) + (wow ? L"Run32" : L"Run");
        target.valueName = MapValue(record.original, L"valueName");
        target.eligible = !target.valueName.empty();
        return target;
    }
    if (record.source == StartupSourceType::StartupFolder) {
        const std::wstring path = MapValue(record.original, L"originalPath");
        if (path.empty()) return target;
        target.hive = record.requiresAdmin ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
        target.hiveName = record.requiresAdmin ? L"HKLM" : L"HKCU";
        target.key = std::wstring(base) + L"StartupFolder";
        target.valueName = std::filesystem::path(path).filename().wstring();
        target.eligible = !target.valueName.empty();
        return target;
    }
    return target;
}

OperationResult DisableViaStartupApproved(DisabledRecord& record, const StartupApprovedTarget& target) {
    HKEY rawKey = nullptr;
    DWORD disposition = 0;
    const LSTATUS opened = RegCreateKeyExW(target.hive, target.key.c_str(), 0, nullptr, 0,
        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &rawKey, &disposition);
    if (opened != ERROR_SUCCESS) return {false, L"无法打开系统启动开关：" + LastErrorText(opened)};
    UniqueRegKey key(rawKey);
    bool hadOriginal = false;
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(rawKey, target.valueName.c_str(), nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS && bytes > 0) {
        std::vector<BYTE> existing(bytes, 0);
        DWORD readBytes = bytes;
        if (RegQueryValueExW(rawKey, target.valueName.c_str(), nullptr, &type, existing.data(), &readBytes) == ERROR_SUCCESS) {
            hadOriginal = true;
            record.original[L"saOriginalBlob"] = BytesToHex(existing);
        }
    }
    record.original[L"saMechanism"] = L"1";
    record.original[L"saHive"] = target.hiveName;
    record.original[L"saKey"] = target.key;
    record.original[L"saValueName"] = target.valueName;
    record.original[L"saHadOriginal"] = hadOriginal ? L"1" : L"0";
    const std::vector<BYTE> blob = MakeStartupApprovedBlob(false);
    const LSTATUS written = RegSetValueExW(rawKey, target.valueName.c_str(), 0, REG_BINARY,
        blob.data(), static_cast<DWORD>(blob.size()));
    if (written != ERROR_SUCCESS) return {false, L"写入系统启动开关失败：" + LastErrorText(written)};
    return {true, L"已通过系统启动开关禁用，可随时恢复。"};
}

OperationResult RestoreViaStartupApproved(const DisabledRecord& record) {
    HKEY hive = HiveFromText(MapValue(record.original, L"saHive"));
    if (!hive) return {false, L"系统启动开关位置无效。"};
    const std::wstring key = MapValue(record.original, L"saKey");
    const std::wstring valueName = MapValue(record.original, L"saValueName");
    HKEY rawKey = nullptr;
    DWORD disposition = 0;
    const LSTATUS opened = RegCreateKeyExW(hive, key.c_str(), 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &rawKey, &disposition);
    if (opened != ERROR_SUCCESS) return {false, L"无法打开系统启动开关：" + LastErrorText(opened)};
    UniqueRegKey guard(rawKey);
    if (MapValue(record.original, L"saHadOriginal") == L"1") {
        const std::vector<BYTE> blob = HexToBytes(MapValue(record.original, L"saOriginalBlob"));
        if (blob.empty()) return {false, L"系统启动开关备份无效。"};
        const LSTATUS written = RegSetValueExW(rawKey, valueName.c_str(), 0, REG_BINARY,
            blob.data(), static_cast<DWORD>(blob.size()));
        if (written != ERROR_SUCCESS) return {false, L"恢复系统启动开关失败：" + LastErrorText(written)};
        return {true, L"已恢复。"};
    }
    // 原本没有开关记录：删除本工具写入的值即可恢复为启用状态。
    const LSTATUS removed = RegDeleteValueW(rawKey, valueName.c_str());
    if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) {
        return {false, L"恢复系统启动开关失败：" + LastErrorText(removed)};
    }
    return {true, L"已恢复。"};
}

bool IsStartupApprovedRecordDisabled(const DisabledRecord& record) {
    HKEY hive = HiveFromText(MapValue(record.original, L"saHive"));
    if (!hive) return false;
    HKEY rawKey = nullptr;
    if (RegOpenKeyExW(hive, MapValue(record.original, L"saKey").c_str(), 0, KEY_QUERY_VALUE, &rawKey) != ERROR_SUCCESS) {
        return false;
    }
    UniqueRegKey key(rawKey);
    const std::wstring valueName = MapValue(record.original, L"saValueName");
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(rawKey, valueName.c_str(), nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0) {
        return false;
    }
    std::vector<BYTE> data(bytes, 0);
    DWORD readBytes = bytes;
    if (RegQueryValueExW(rawKey, valueName.c_str(), nullptr, &type, data.data(), &readBytes) != ERROR_SUCCESS) return false;
    return !data.empty() && (data[0] & 1) != 0;
}


OperationResult DisableRegistry(const DisabledRecord& record) {
    HKEY hive = HiveFromText(MapValue(record.original, L"hive"));
    if (!hive) return {false, L"注册表位置无效。"};
    HKEY rawKey = nullptr;
    const LSTATUS opened = RegOpenKeyExW(hive, MapValue(record.original, L"key").c_str(), 0, KEY_SET_VALUE, &rawKey);
    if (opened != ERROR_SUCCESS) return {false, L"无法打开注册表启动项：" + LastErrorText(opened)};
    UniqueRegKey key(rawKey);
    const LSTATUS removed = RegDeleteValueW(rawKey, MapValue(record.original, L"valueName").c_str());
    if (removed != ERROR_SUCCESS) return {false, L"禁用注册表启动项失败：" + LastErrorText(removed)};
    return {true, L"已禁用。"};
}

OperationResult RestoreRegistry(const DisabledRecord& record) {
    HKEY hive = HiveFromText(MapValue(record.original, L"hive"));
    if (!hive) return {false, L"注册表位置无效。"};
    HKEY rawKey = nullptr;
    DWORD disposition = 0;
    const LSTATUS opened = RegCreateKeyExW(hive, MapValue(record.original, L"key").c_str(), 0, nullptr, 0,
        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &rawKey, &disposition);
    if (opened != ERROR_SUCCESS) return {false, L"无法打开注册表原位置：" + LastErrorText(opened)};
    UniqueRegKey key(rawKey);
    const std::wstring valueName = MapValue(record.original, L"valueName");
    DWORD type = 0;
    if (!ParseUnsigned(MapValue(record.original, L"valueType"), type) || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return {false, L"注册表备份类型无效。"};
    }
    const std::wstring data = MapValue(record.original, L"valueData");
    DWORD existingType = 0;
    DWORD existingBytes = 0;
    if (RegQueryValueExW(rawKey, valueName.c_str(), nullptr, &existingType, nullptr, &existingBytes) == ERROR_SUCCESS) {
        std::vector<BYTE> existing(static_cast<std::size_t>(existingBytes) + sizeof(wchar_t), 0);
        if (RegQueryValueExW(rawKey, valueName.c_str(), nullptr, &existingType, existing.data(), &existingBytes) == ERROR_SUCCESS &&
            existingType == type && std::wstring(reinterpret_cast<const wchar_t*>(existing.data())) == data) {
            return {true, L"项目已经处于原状态。"};
        }
        return {false, L"原位置已经存在同名启动项，未覆盖。"};
    }
    const DWORD bytes = static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t));
    const LSTATUS written = RegSetValueExW(rawKey, valueName.c_str(), 0, type,
        reinterpret_cast<const BYTE*>(data.c_str()), bytes);
    if (written != ERROR_SUCCESS) return {false, L"恢复注册表启动项失败：" + LastErrorText(written)};
    return {true, L"已恢复。"};
}

OperationResult DisableFolder(DisabledRecord& record) {
    const std::filesystem::path original(MapValue(record.original, L"originalPath"));
    if (original.empty()) return {false, L"启动文件路径无效。"};
    std::error_code error;
    std::filesystem::create_directories(AppLaunchLockerDataDirectory() / L"disabled", error);
    if (error) return {false, L"无法创建禁用目录：" + Utf8ToWide(error.message())};
    const std::filesystem::path backup = AppLaunchLockerDataDirectory() / L"disabled" /
        (record.recordId + original.extension().wstring());
    record.original[L"backupPath"] = backup.wstring();
    if (!MoveFileExW(original.c_str(), backup.c_str(), MOVEFILE_COPY_ALLOWED)) {
        return {false, L"移动启动文件失败：" + LastErrorText()};
    }
    return {true, L"已禁用。"};
}

OperationResult RestoreFolder(const DisabledRecord& record) {
    const std::filesystem::path original(MapValue(record.original, L"originalPath"));
    const std::filesystem::path backup(MapValue(record.original, L"backupPath"));
    if (original.empty()) return {false, L"启动文件备份记录不完整。"};
    const bool originalExists = GetFileAttributesW(original.c_str()) != INVALID_FILE_ATTRIBUTES;
    const bool backupExists = !backup.empty() && GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!backupExists && originalExists) return {true, L"项目已经处于原状态。"};
    if (!backupExists) return {false, L"备份文件不存在，无法恢复。"};
    if (originalExists) return {false, L"原位置已经存在文件，未覆盖。"};
    if (!MoveFileExW(backup.c_str(), original.c_str(), MOVEFILE_COPY_ALLOWED)) {
        return {false, L"恢复启动文件失败：" + LastErrorText()};
    }
    return {true, L"已恢复。"};
}

OperationResult SetTaskEnabled(const DisabledRecord& record, bool enabled) {
    ComApartment apartment;
    if (!apartment.usable()) return {false, L"无法初始化计划任务操作。"};
    ITaskService* service = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service));
    if (FAILED(hr) || !service) return {false, L"无法连接计划任务：" + HResultText(hr)};
    VARIANT empty{};
    VariantInit(&empty);
    hr = service->Connect(empty, empty, empty, empty);
    if (FAILED(hr)) {
        service->Release();
        return {false, L"无法连接计划任务：" + HResultText(hr)};
    }
    const std::wstring path = MapValue(record.original, L"taskPath");
    const std::size_t slash = path.find_last_of(L'\\');
    const std::wstring folderPath = slash == 0 ? L"\\" : path.substr(0, slash);
    const std::wstring taskName = slash == std::wstring::npos ? path : path.substr(slash + 1);
    BSTR folderText = SysAllocString(folderPath.c_str());
    ITaskFolder* folder = nullptr;
    hr = service->GetFolder(folderText, &folder);
    SysFreeString(folderText);
    if (FAILED(hr) || !folder) {
        service->Release();
        return {false, L"计划任务目录不存在：" + HResultText(hr)};
    }
    BSTR name = SysAllocString(taskName.c_str());
    IRegisteredTask* task = nullptr;
    hr = folder->GetTask(name, &task);
    SysFreeString(name);
    if (SUCCEEDED(hr) && task) hr = task->put_Enabled(enabled ? VARIANT_TRUE : VARIANT_FALSE);
    ReleaseCom(task);
    folder->Release();
    service->Release();
    if (FAILED(hr)) return {false, std::wstring(enabled ? L"恢复" : L"禁用") + L"计划任务失败：" + HResultText(hr)};
    return {true, enabled ? L"已恢复。" : L"已禁用。"};
}

OperationResult SetServiceStart(const DisabledRecord& record, bool restore) {
    SC_HANDLE rawManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!rawManager) return {false, L"无法打开服务管理器：" + LastErrorText()};
    UniqueServiceHandle manager(rawManager);
    SC_HANDLE rawService = OpenServiceW(rawManager, MapValue(record.original, L"serviceName").c_str(),
        SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG);
    if (!rawService) return {false, L"无法打开服务：" + LastErrorText()};
    UniqueServiceHandle service(rawService);
    DWORD startType = SERVICE_DEMAND_START;
    if (restore && !ParseUnsigned(MapValue(record.original, L"startType"), startType)) {
        return {false, L"服务备份记录无效。"};
    }
    if (!ChangeServiceConfigW(rawService, SERVICE_NO_CHANGE, startType, SERVICE_NO_CHANGE,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
        return {false, std::wstring(restore ? L"恢复" : L"修改") + L"服务启动类型失败：" + LastErrorText()};
    }
    SERVICE_DELAYED_AUTO_START_INFO delayed{};
    delayed.fDelayedAutostart = restore && MapValue(record.original, L"delayed") == L"1" ? TRUE : FALSE;
    ChangeServiceConfig2W(rawService, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed);
    return {true, restore ? L"已恢复。" : L"已改为手动启动，不会停止当前服务。"};
}

bool VerifyDisabled(const DisabledRecord& record) {
    if (MapValue(record.original, L"saMechanism") == L"1") return IsStartupApprovedRecordDisabled(record);
    switch (record.source) {
    case StartupSourceType::Registry:
    case StartupSourceType::ActiveSetup: {
        HKEY hive = HiveFromText(MapValue(record.original, L"hive"));
        HKEY rawKey = nullptr;
        if (!hive || RegOpenKeyExW(hive, MapValue(record.original, L"key").c_str(), 0, KEY_QUERY_VALUE, &rawKey) != ERROR_SUCCESS) return true;
        UniqueRegKey key(rawKey);
        return RegQueryValueExW(rawKey, MapValue(record.original, L"valueName").c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_FILE_NOT_FOUND;
    }
    case StartupSourceType::StartupFolder:
        return GetFileAttributesW(MapValue(record.original, L"originalPath").c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(MapValue(record.original, L"backupPath").c_str()) != INVALID_FILE_ATTRIBUTES;
    case StartupSourceType::ScheduledTask: {
        OperationResult result = SetTaskEnabled(record, false);
        return result.success;
    }
    case StartupSourceType::Service: {
        SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!manager) return false;
        SC_HANDLE service = OpenServiceW(manager, MapValue(record.original, L"serviceName").c_str(), SERVICE_QUERY_CONFIG);
        if (!service) { CloseServiceHandle(manager); return false; }
        DWORD bytes = 0;
        QueryServiceConfigW(service, nullptr, 0, &bytes);
        std::vector<BYTE> buffer(bytes);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        const bool ok = bytes && QueryServiceConfigW(service, config, bytes, &bytes) && config->dwStartType == SERVICE_DEMAND_START;
        CloseServiceHandle(service);
        CloseServiceHandle(manager);
        return ok;
    }
    default:
        return false;
    }
}

OperationResult ApplyDisable(DisabledRecord& record) {
    if (record.source == StartupSourceType::Registry || record.source == StartupSourceType::StartupFolder) {
        const StartupApprovedTarget target = ResolveStartupApprovedTarget(record);
        if (target.eligible) return DisableViaStartupApproved(record, target);
    }
    switch (record.source) {
    case StartupSourceType::Registry: return DisableRegistry(record);
    case StartupSourceType::ActiveSetup: return DisableRegistry(record);
    case StartupSourceType::StartupFolder: return DisableFolder(record);
    case StartupSourceType::ScheduledTask: return SetTaskEnabled(record, false);
    case StartupSourceType::Service: return SetServiceStart(record, false);
    default: return {false, L"此项目仅供查看。"};
    }
}

OperationResult ApplyRestore(const DisabledRecord& record) {
    if (MapValue(record.original, L"saMechanism") == L"1") return RestoreViaStartupApproved(record);
    switch (record.source) {
    case StartupSourceType::Registry: return RestoreRegistry(record);
    case StartupSourceType::ActiveSetup: return RestoreRegistry(record);
    case StartupSourceType::StartupFolder: return RestoreFolder(record);
    case StartupSourceType::ScheduledTask: return SetTaskEnabled(record, MapValue(record.original, L"wasEnabled") != L"0");
    case StartupSourceType::Service: return SetServiceStart(record, true);
    default: return {false, L"此记录不支持恢复。"};
    }
}

// ================= 广告拦截（简化版）：IFEO 禁止运行 =================

std::wstring CurrentExecutablePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD copied = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!copied) return {};
        if (copied < path.size() - 1) {
            path.resize(copied);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring HashHex(const std::wstring& input) {
    std::uint64_t hash = 1469598103934665603ull;
    for (wchar_t ch : input) {
        hash ^= static_cast<std::uint16_t>(ch);
        hash *= 1099511628211ull;
    }
    return Hex64(hash);
}

constexpr const wchar_t* kIfeoBase = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options";

// 从完整路径取文件名（小写归一化）。
std::wstring FileNameOf(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    return Lower(slash == std::wstring::npos ? path : path.substr(slash + 1));
}

std::wstring ExtensionOf(const std::wstring& fileName) {
    const std::size_t dot = fileName.find_last_of(L'.');
    return dot == std::wstring::npos ? std::wstring{} : Lower(fileName.substr(dot));
}

// 扩展名分类："exe"（可 IFEO 拦截）| "script"（仅提示）| "other"。
std::wstring ClassifyExtension(const std::wstring& fileName) {
    const std::wstring ext = ExtensionOf(fileName);
    if (ext == L".exe" || ext == L".com" || ext == L".scr") return L"exe";
    if (ext == L".bat" || ext == L".cmd" || ext == L".ps1" || ext == L".vbs" ||
        ext == L".js" || ext == L".wsf" || ext == L".wsh") return L"script";
    return L"other";
}

bool IsStartableCandidate(const std::wstring& fileName) {
    const std::wstring cls = ClassifyExtension(fileName);
    return cls == L"exe" || cls == L"script" || ExtensionOf(fileName) == L".lnk";
}

// 解析 .lnk 目标；需已初始化 COM。
std::wstring ResolveShortcut(const std::wstring& lnkPath) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_IShellLinkW, reinterpret_cast<void**>(&link))) || !link) {
        return {};
    }
    std::wstring target;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file))) && file) {
        if (SUCCEEDED(file->Load(lnkPath.c_str(), STGM_READ))) {
            wchar_t buffer[MAX_PATH]{};
            WIN32_FIND_DATAW find{};
            if (SUCCEEDED(link->GetPath(buffer, MAX_PATH, &find, SLGP_UNCPRIORITY))) target = buffer;
        }
        file->Release();
    }
    link->Release();
    return target;
}

struct LaunchTarget {
    bool valid = false;
    std::wstring path;       // 解析后的真实文件全路径
    std::wstring imageName;  // 文件名（小写归一化）
    std::wstring category;   // "exe" | "script" | "other"
};

// 解析一个候选路径为真实启动目标（.lnk 会解析到目标文件），并分类。需已初始化 COM。
LaunchTarget ResolveLaunchTarget(const std::wstring& inputPath) {
    LaunchTarget result;
    std::wstring path = inputPath;
    if (ExtensionOf(FileNameOf(path)) == L".lnk") {
        const std::wstring resolved = ResolveShortcut(path);
        if (resolved.empty()) return result;  // 无法解析快捷方式目标
        path = resolved;
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return result;
    result.valid = true;
    result.path = path;
    result.imageName = FileNameOf(path);
    result.category = ClassifyExtension(result.imageName);
    return result;
}

bool IsSystemPath(const std::wstring& path) {
    wchar_t systemRoot[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(systemRoot, static_cast<UINT>(std::size(systemRoot)));
    if (!length) return false;
    std::wstring root = Lower(std::wstring(systemRoot, length));
    std::wstring target = Lower(path);
    if (!root.empty() && root.back() != L'\\') root.push_back(L'\\');
    return target.compare(0, root.size(), root) == 0;
}

bool IsCriticalProcessName(const std::wstring& imageName) {
    static const wchar_t* kCritical[] = {
        L"explorer.exe", L"svchost.exe", L"winlogon.exe", L"wininit.exe", L"csrss.exe",
        L"lsass.exe", L"services.exe", L"smss.exe", L"taskmgr.exe", L"regedit.exe",
        L"cmd.exe", L"powershell.exe", L"wscript.exe", L"cscript.exe", L"rundll32.exe",
        L"dllhost.exe", L"conhost.exe", L"fontdrvhost.exe", L"dwm.exe", L"spoolsv.exe",
    };
    const std::wstring lower = Lower(imageName);
    for (const wchar_t* name : kCritical) {
        if (lower == name) return true;
    }
    return false;
}

// Authenticode 链校验（不取主体）。
bool VerifyAuthenticode(const std::wstring& filePath) {
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = filePath.c_str();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &fileInfo;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_SAFER_FLAG | WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    return status == ERROR_SUCCESS;
}

// 取签名者证书主体 CN/显示名。
bool ExtractSignerSubject(const std::wstring& filePath, std::wstring& subject) {
    HCERTSTORE store = nullptr;
    HCRYPTMSG message = nullptr;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, filePath.c_str(),
            CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, CERT_QUERY_FORMAT_FLAG_BINARY,
            0, nullptr, nullptr, nullptr, &store, &message, nullptr)) {
        return false;
    }
    bool ok = false;
    DWORD signerSize = 0;
    if (CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signerSize) && signerSize) {
        std::vector<BYTE> signerBuffer(signerSize);
        if (CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, signerBuffer.data(), &signerSize)) {
            auto* signer = reinterpret_cast<CMSG_SIGNER_INFO*>(signerBuffer.data());
            CERT_INFO certInfo{};
            certInfo.Issuer = signer->Issuer;
            certInfo.SerialNumber = signer->SerialNumber;
            PCCERT_CONTEXT cert = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                CERT_FIND_SUBJECT_CERT, &certInfo, nullptr);
            if (cert) {
                const DWORD nameLength = CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
                if (nameLength > 1) {
                    std::wstring name(nameLength, L'\0');
                    CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name.data(), nameLength);
                    name.resize(nameLength - 1);
                    subject = name;
                    ok = true;
                }
                CertFreeCertificateContext(cert);
            }
        }
    }
    if (store) CertCloseStore(store, 0);
    if (message) CryptMsgClose(message);
    return ok;
}

bool IsTrustedSubject(const std::wstring& subject) {
    // 初版可信发布者表：只有 Microsoft 系被豁免；第三方安全厂商走“可拦截但强提示”。
    static const wchar_t* kTrusted[] = {L"Microsoft Windows", L"Microsoft Corporation", L"Microsoft Windows Publisher"};
    for (const wchar_t* trusted : kTrusted) {
        if (subject == trusted) return true;
    }
    return false;
}

struct GuardVerdict {
    bool allow = false;      // 是否允许拦截（可勾选）
    bool warn = false;       // 允许但需强提示（无法验证签名）
    std::wstring reason;     // 拒绝原因或提示文案
};

// 守卫：信任只来自有效官方签名，绝不依据目录名。见方案 §4。
GuardVerdict EvaluateGuard(const std::wstring& targetPath, const std::wstring& imageName) {
    if (IsSystemPath(targetPath)) return {false, false, L"系统目录程序，禁止拦截"};
    if (IsCriticalProcessName(imageName)) return {false, false, L"系统关键进程名，禁止拦截"};
    if (VerifyAuthenticode(targetPath)) {
        std::wstring subject;
        if (ExtractSignerSubject(targetPath, subject) && IsTrustedSubject(subject)) {
            return {false, false, L"受信任的官方签名程序（" + subject + L"）"};
        }
        return {true, false, {}};  // 有效签名但非豁免主体（含套壳/第三方）：允许拦截
    }
    return {true, true, L"无法验证该程序签名，请确认这不是你需要的安全软件"};
}

// 判断目标 PE 是 32 位还是 64 位映像，决定 IFEO 注册表视图。
std::wstring DetectIfeoView(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return L"64";
    std::wstring view = L"64";
    IMAGE_DOS_HEADER dos{};
    DWORD read = 0;
    if (ReadFile(file, &dos, sizeof(dos), &read, nullptr) && read == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE) {
        if (SetFilePointer(file, dos.e_lfanew, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER) {
            DWORD signature = 0;
            IMAGE_FILE_HEADER header{};
            if (ReadFile(file, &signature, sizeof(signature), &read, nullptr) && read == sizeof(signature) &&
                signature == IMAGE_NT_SIGNATURE &&
                ReadFile(file, &header, sizeof(header), &read, nullptr) && read == sizeof(header)) {
                if (header.Machine == IMAGE_FILE_MACHINE_I386) view = L"32";
            }
        }
    }
    CloseHandle(file);
    return view;
}

REGSAM IfeoView(const DisabledRecord& record) {
    return MapValue(record.original, L"ifeoView") == L"32" ? KEY_WOW64_32KEY : KEY_WOW64_64KEY;
}

OperationResult WriteRegString(HKEY key, const wchar_t* name, DWORD type, const std::wstring& value) {
    const LSTATUS status = RegSetValueExW(key, name, 0, type,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    if (status != ERROR_SUCCESS) return {false, std::wstring(L"写入 ") + name + L" 失败：" + LastErrorText(status)};
    return {true, {}};
}

// 删除仅由本工具创建、已无任何值和子键的空 IFEO 映像键。
void RemoveEmptyIfeoKey(const std::wstring& imageName, REGSAM view) {
    const std::wstring keyPath = std::wstring(kIfeoBase) + L"\\" + imageName;
    HKEY rawKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, KEY_QUERY_VALUE | view, &rawKey) != ERROR_SUCCESS) return;
    DWORD subKeys = 0;
    DWORD values = 0;
    RegQueryInfoKeyW(rawKey, nullptr, nullptr, nullptr, &subKeys, nullptr, nullptr, &values,
        nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(rawKey);
    if (subKeys != 0 || values != 0) return;
    HKEY rawParent = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kIfeoBase, 0, KEY_ENUMERATE_SUB_KEYS | view, &rawParent) == ERROR_SUCCESS) {
        RegDeleteKeyExW(rawParent, imageName.c_str(), view, 0);
        RegCloseKey(rawParent);
    }
}

// 写入 IFEO 拦截；补充 record.original 的恢复字段。
OperationResult ApplyIfeoBlock(DisabledRecord& record) {
    const std::wstring imageName = MapValue(record.original, L"ifeoImageName");
    if (imageName.empty()) return {false, L"拦截目标无效。"};
    const std::wstring targetPath = MapValue(record.original, L"targetPath");
    // 提权子进程内再次复核守卫（防 TOCTOU）。
    const GuardVerdict guard = EvaluateGuard(targetPath, imageName);
    if (!guard.allow) return {false, L"该程序不允许拦截：" + guard.reason};

    const REGSAM view = IfeoView(record);
    const std::wstring debugger = L"\"" + CurrentExecutablePath() + L"\" --ifeo-noop";
    const std::wstring keyPath = std::wstring(kIfeoBase) + L"\\" + imageName;

    HKEY rawKey = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, nullptr, 0,
        KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_CREATE_SUB_KEY | view, nullptr, &rawKey, nullptr);
    if (status != ERROR_SUCCESS) return {false, L"无法写入 IFEO 键：" + LastErrorText(status)};
    UniqueRegKey key(rawKey);

    if (MapValue(record.original, L"blockMode") == L"exact") {
        DWORD useFilter = 1;
        RegSetValueExW(rawKey, L"UseFilter", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&useFilter), sizeof(useFilter));
        const std::wstring subName = L"AppLaunchLocker_" + HashHex(Lower(targetPath));
        HKEY rawSub = nullptr;
        status = RegCreateKeyExW(rawKey, subName.c_str(), 0, nullptr, 0,
            KEY_SET_VALUE | view, nullptr, &rawSub, nullptr);
        if (status != ERROR_SUCCESS) return {false, L"无法写入 IFEO 过滤子键：" + LastErrorText(status)};
        UniqueRegKey sub(rawSub);
        OperationResult wrote = WriteRegString(rawSub, L"FilterFullPath", REG_SZ, targetPath);
        if (!wrote.success) return wrote;
        wrote = WriteRegString(rawSub, L"Debugger", REG_SZ, debugger);
        if (!wrote.success) return wrote;
        record.original[L"ifeoSubKey"] = subName;
        record.original[L"ifeoHadOriginal"] = L"0";
    } else {
        DWORD type = 0;
        DWORD bytes = 0;
        const LSTATUS query = RegQueryValueExW(rawKey, L"Debugger", nullptr, &type, nullptr, &bytes);
        if (query == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && bytes) {
            std::vector<BYTE> existing(bytes, 0);
            RegQueryValueExW(rawKey, L"Debugger", nullptr, &type, existing.data(), &bytes);
            record.original[L"ifeoHadOriginal"] = L"1";
            record.original[L"ifeoOriginalDebugger"] = BytesToHex(existing);
            record.original[L"ifeoOriginalType"] = std::to_wstring(type);
        } else {
            record.original[L"ifeoHadOriginal"] = L"0";
        }
        OperationResult wrote = WriteRegString(rawKey, L"Debugger", REG_SZ, debugger);
        if (!wrote.success) return wrote;
    }
    return {true, L"已拦截。"};
}

OperationResult RestoreIfeoBlock(const DisabledRecord& record) {
    const std::wstring imageName = MapValue(record.original, L"ifeoImageName");
    if (imageName.empty()) return {false, L"记录无效。"};
    const REGSAM view = IfeoView(record);
    const std::wstring keyPath = std::wstring(kIfeoBase) + L"\\" + imageName;

    HKEY rawKey = nullptr;
    const LSTATUS opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0,
        KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_ENUMERATE_SUB_KEYS | view, &rawKey);
    if (opened == ERROR_FILE_NOT_FOUND) return {true, L"已解除拦截。"};
    if (opened != ERROR_SUCCESS) return {false, L"无法打开 IFEO 键：" + LastErrorText(opened)};
    {
        UniqueRegKey key(rawKey);
        if (MapValue(record.original, L"blockMode") == L"exact") {
            const std::wstring subName = MapValue(record.original, L"ifeoSubKey");
            if (!subName.empty()) RegDeleteKeyExW(rawKey, subName.c_str(), view, 0);
            DWORD subKeys = 0;
            RegQueryInfoKeyW(rawKey, nullptr, nullptr, nullptr, &subKeys, nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr);
            if (subKeys == 0) RegDeleteValueW(rawKey, L"UseFilter");
        } else if (MapValue(record.original, L"ifeoHadOriginal") == L"1") {
            const std::vector<BYTE> data = HexToBytes(MapValue(record.original, L"ifeoOriginalDebugger"));
            DWORD type = REG_SZ;
            ParseUnsigned(MapValue(record.original, L"ifeoOriginalType"), type);
            RegSetValueExW(rawKey, L"Debugger", 0, type, data.data(), static_cast<DWORD>(data.size()));
        } else {
            RegDeleteValueW(rawKey, L"Debugger");
        }
    }
    RemoveEmptyIfeoKey(imageName, view);
    return {true, L"已解除拦截。"};
}

bool VerifyIfeoBlock(const DisabledRecord& record) {
    const std::wstring imageName = MapValue(record.original, L"ifeoImageName");
    if (imageName.empty()) return false;
    const REGSAM view = IfeoView(record);
    std::wstring keyPath = std::wstring(kIfeoBase) + L"\\" + imageName;
    if (MapValue(record.original, L"blockMode") == L"exact") {
        const std::wstring subName = MapValue(record.original, L"ifeoSubKey");
        if (subName.empty()) return false;
        keyPath += L"\\" + subName;
    }
    HKEY rawKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, KEY_QUERY_VALUE | view, &rawKey) != ERROR_SUCCESS) return false;
    UniqueRegKey key(rawKey);
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(rawKey, L"Debugger", nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || !bytes) {
        return false;
    }
    std::vector<BYTE> data(bytes + sizeof(wchar_t), 0);
    if (RegQueryValueExW(rawKey, L"Debugger", nullptr, &type, data.data(), &bytes) != ERROR_SUCCESS) return false;
    const std::wstring debugger = Lower(std::wstring(reinterpret_cast<const wchar_t*>(data.data())));
    return debugger.find(L"--ifeo-noop") != std::wstring::npos;
}

bool InitializeComForScan(bool& uninitialize) {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    uninitialize = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;
    const HRESULT security = CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
        RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
    return SUCCEEDED(security) || security == RPC_E_TOO_LATE;
}

// 判定一个启动项是否属于 StartupApproved 系统开关可管理的来源（标准 Run 键值 / 启动文件夹）。
bool IsAutoStartEligible(const StartupItem& item) {
    DisabledRecord probe;
    probe.source = item.source;
    probe.requiresAdmin = item.requiresAdmin;
    probe.original = item.original;
    return ResolveStartupApprovedTarget(probe).eligible;
}

// 反查：找出目标 exe 全路径所对应的、可经系统“启动”开关禁用的自启动注册项（精确完整路径匹配）。
// 需自管 COM（启动文件夹 .lnk 解析需要）。
std::vector<StartupItem> FindAutoStartEntries(const std::wstring& targetPath) {
    bool uninitialize = false;
    InitializeComForScan(uninitialize);
    ScanResult scan;
    ScanRegistry(scan);
    ScanStartupFolder(scan, FOLDERID_Startup, false);
    ScanStartupFolder(scan, FOLDERID_CommonStartup, true);
    if (uninitialize) CoUninitialize();

    const std::wstring wantLower = Lower(targetPath);
    std::vector<StartupItem> matches;
    for (StartupItem& item : scan.items) {
        if (item.source != StartupSourceType::Registry && item.source != StartupSourceType::StartupFolder) continue;
        if (!IsAutoStartEligible(item)) continue;
        const std::wstring exe = ExecutableFromCommand(item.command);
        if (exe.empty()) continue;
        if (Lower(exe) != wantLower) continue;
        matches.push_back(std::move(item));
    }
    return matches;
}
}

std::wstring StartupSourceKey(StartupSourceType source) {
    switch (source) {
    case StartupSourceType::Registry: return L"registry";
    case StartupSourceType::StartupFolder: return L"startup-folder";
    case StartupSourceType::ScheduledTask: return L"scheduled-task";
    case StartupSourceType::Service: return L"service";
    case StartupSourceType::ActiveSetup: return L"active-setup";
    case StartupSourceType::Driver: return L"driver";
    case StartupSourceType::WmiSubscription: return L"wmi";
    case StartupSourceType::Winlogon: return L"winlogon";
    case StartupSourceType::WinlogonNotify: return L"winlogon-notify";
    case StartupSourceType::AppInitDll: return L"appinit-dll";
    case StartupSourceType::AppCertDll: return L"appcert-dll";
    case StartupSourceType::BootExecute: return L"boot-execute";
    case StartupSourceType::KnownDll: return L"known-dll";
    case StartupSourceType::ShellExtension: return L"shell-extension";
    case StartupSourceType::Ifeo: return L"ifeo";
    }
    return L"unknown";
}

std::wstring StartupSourceText(StartupSourceType source) {
    switch (source) {
    case StartupSourceType::Registry: return L"注册表";
    case StartupSourceType::StartupFolder: return L"启动文件夹";
    case StartupSourceType::ScheduledTask: return L"计划任务";
    case StartupSourceType::Service: return L"服务";
    case StartupSourceType::ActiveSetup: return L"Active Setup";
    case StartupSourceType::Driver: return L"驱动";
    case StartupSourceType::WmiSubscription: return L"WMI";
    case StartupSourceType::Winlogon: return L"登录项";
    case StartupSourceType::WinlogonNotify: return L"登录通知";
    case StartupSourceType::AppInitDll: return L"AppInit DLL";
    case StartupSourceType::AppCertDll: return L"AppCert DLL";
    case StartupSourceType::BootExecute: return L"启动执行";
    case StartupSourceType::KnownDll: return L"已知 DLL";
    case StartupSourceType::ShellExtension: return L"Shell 扩展";
    case StartupSourceType::Ifeo: return L"IFEO";
    }
    return L"未知";
}

bool StartupSourceFromKey(const std::wstring& key, StartupSourceType& source) {
    for (StartupSourceType candidate : {StartupSourceType::Registry, StartupSourceType::StartupFolder,
            StartupSourceType::ScheduledTask, StartupSourceType::Service, StartupSourceType::ActiveSetup,
            StartupSourceType::Driver, StartupSourceType::WmiSubscription, StartupSourceType::Winlogon,
            StartupSourceType::WinlogonNotify, StartupSourceType::AppInitDll, StartupSourceType::AppCertDll,
            StartupSourceType::BootExecute, StartupSourceType::KnownDll, StartupSourceType::ShellExtension,
            StartupSourceType::Ifeo}) {
        if (StartupSourceKey(candidate) == key) {
            source = candidate;
            return true;
        }
    }
    return false;
}

std::filesystem::path AppLaunchLockerDataDirectory() {
    PWSTR rawPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &rawPath)) && rawPath) {
        std::filesystem::path path(rawPath);
        CoTaskMemFree(rawPath);
        return path / L"AppLaunchLocker";
    }
    if (rawPath) CoTaskMemFree(rawPath);
    return std::filesystem::temp_directory_path() / L"AppLaunchLocker";
}

void AppendAppLaunchLockerLog(const std::wstring& message) {
    if (message.empty()) return;
    const std::filesystem::path directory = AppLaunchLockerDataDirectory();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return;
    std::ofstream file(directory / L"AppLaunchLocker.log", std::ios::binary | std::ios::app);
    if (!file) return;
    const std::string line = WideToUtf8(CurrentTimestamp() + L" " + message + L"\r\n");
    file.write(line.data(), static_cast<std::streamsize>(line.size()));
}

DisabledItemStore::DisabledItemStore()
    : path_(AppLaunchLockerDataDirectory() / L"disabled-items.json") {}

DisabledItemStore::DisabledItemStore(std::filesystem::path path)
    : path_(std::move(path)) {}

bool DisabledItemStore::Load(std::vector<DisabledRecord>& records, std::wstring& error) const {
    records.clear();
    error.clear();
    std::error_code existsError;
    if (!std::filesystem::exists(path_, existsError)) return true;
    std::ifstream file(path_, std::ios::binary);
    if (!file) {
        error = L"无法读取禁用记录。";
        return false;
    }
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    JsonValue root;
    if (!ParseJson(Utf8ToWide(bytes), root, error) || !root.isObject()) {
        error = L"禁用记录文件已损坏：" + error;
        return false;
    }
    const JsonValue* version = root.get(L"version");
    if (!version || version->intOr(0) != 1) {
        error = L"禁用记录版本不受支持。";
        return false;
    }
    const JsonValue* items = root.get(L"items");
    if (!items || !items->isArray()) {
        error = L"禁用记录缺少 items。";
        return false;
    }
    for (const JsonValue& value : items->arrayValue) {
        if (!value.isObject()) continue;
        DisabledRecord record;
        const JsonValue* recordId = value.get(L"recordId");
        const JsonValue* itemId = value.get(L"itemId");
        const JsonValue* source = value.get(L"source");
        const JsonValue* name = value.get(L"name");
        const JsonValue* disabledAt = value.get(L"disabledAt");
        if (!recordId || !itemId || !source || !name || !disabledAt) continue;
        if (!StartupSourceFromKey(source->stringOr(), record.source)) continue;
        record.recordId = recordId->stringOr();
        record.itemId = itemId->stringOr();
        record.name = name->stringOr();
        record.disabledAt = disabledAt->stringOr();
        const JsonValue* requiresAdmin = value.get(L"requiresAdmin");
        record.requiresAdmin = requiresAdmin && requiresAdmin->boolOr(false);
        const JsonValue* original = value.get(L"original");
        if (original && original->isObject()) {
            for (const auto& [key, field] : original->objectValue) {
                if (field.isString()) record.original[key] = field.stringValue;
            }
        }
        if (!record.recordId.empty() && !record.itemId.empty()) records.push_back(std::move(record));
    }
    return true;
}

bool DisabledItemStore::Save(const std::vector<DisabledRecord>& records, std::wstring& error) const {
    error.clear();
    std::error_code directoryError;
    std::filesystem::create_directories(path_.parent_path(), directoryError);
    if (directoryError) {
        error = L"无法创建数据目录：" + Utf8ToWide(directoryError.message());
        return false;
    }
    std::wostringstream json;
    json << L"{\n  \"version\": 1,\n  \"items\": [";
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto& record = records[index];
        json << (index == 0 ? L"\n" : L",\n")
             << L"    {\n"
             << L"      \"recordId\": \"" << EscapeJson(record.recordId) << L"\",\n"
             << L"      \"itemId\": \"" << EscapeJson(record.itemId) << L"\",\n"
             << L"      \"source\": \"" << StartupSourceKey(record.source) << L"\",\n"
             << L"      \"name\": \"" << EscapeJson(record.name) << L"\",\n"
             << L"      \"disabledAt\": \"" << EscapeJson(record.disabledAt) << L"\",\n"
             << L"      \"requiresAdmin\": " << (record.requiresAdmin ? L"true" : L"false") << L",\n"
             << L"      \"original\": {";
        std::size_t fieldIndex = 0;
        for (const auto& [key, value] : record.original) {
            json << (fieldIndex++ == 0 ? L"\n" : L",\n")
                 << L"        \"" << EscapeJson(key) << L"\": \"" << EscapeJson(value) << L"\"";
        }
        if (!record.original.empty()) json << L"\n      ";
        json << L"}\n    }";
    }
    if (!records.empty()) json << L"\n  ";
    json << L"]\n}\n";

    const std::filesystem::path temporary = path_.wstring() + L".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = L"无法写入禁用记录临时文件。";
        return false;
    }
    const std::string utf8 = WideToUtf8(json.str());
    file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    file.close();
    if (!file) {
        error = L"写入禁用记录失败。";
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = L"保存禁用记录失败：" + LastErrorText();
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        return false;
    }
    return true;
}

StartupManager::StartupManager() = default;

StartupManager::StartupManager(DisabledItemStore store)
    : store_(std::move(store)) {}

ScanResult StartupManager::ScanAll() const {
    ScanResult result;
    bool uninitialize = false;
    if (!InitializeComForScan(uninitialize)) result.warnings.push_back(L"COM 初始化失败，部分来源无法扫描。");
    ScanRegistry(result);
    ScanActiveSetup(result);
    ScanStartupFolder(result, FOLDERID_Startup, false);
    ScanStartupFolder(result, FOLDERID_CommonStartup, true);
    ScanScheduledTasks(result);
    ScanServices(result);
    ScanWmi(result);
    ScanIfeo(result);
    ScanWinlogonNotify(result);
    ScanSessionManager(result);
    ScanAppCertDlls(result);
    ScanShellExtensions(result);
    AlignStartupApproved(result);
    // 对账：本工具经 StartupApproved 禁用的项其注册表值/文件仍在，扫描仍会命中；
    // 若系统侧仍为禁用则从“当前自启动”移除（只留在“已禁用”），若被系统改回启用则保留。
    std::vector<DisabledRecord> disabledRecords;
    std::wstring disabledError;
    if (store_.Load(disabledRecords, disabledError)) {
        for (const DisabledRecord& rec : disabledRecords) {
            if (MapValue(rec.original, L"saMechanism") == L"1" && !IsStartupApprovedRecordDisabled(rec)) continue;
            result.items.erase(std::remove_if(result.items.begin(), result.items.end(),
                [&](const StartupItem& item) { return item.id == rec.itemId; }), result.items.end());
        }
    }
    std::sort(result.items.begin(), result.items.end(), [](const StartupItem& left, const StartupItem& right) {
        if (left.canDisable != right.canDisable) return left.canDisable > right.canDisable;
        const std::wstring leftName = Lower(left.name);
        const std::wstring rightName = Lower(right.name);
        if (leftName != rightName) return leftName < rightName;
        return left.id < right.id;
    });
    if (uninitialize) CoUninitialize();
    return result;
}

bool StartupManager::LoadDisabled(std::vector<DisabledRecord>& records, std::wstring& error) const {
    return store_.Load(records, error);
}

OperationResult StartupManager::Disable(const std::wstring& itemId) const {
    std::vector<DisabledRecord> records;
    std::wstring error;
    if (!store_.Load(records, error)) return {false, error};
    if (std::any_of(records.begin(), records.end(), [&](const DisabledRecord& record) { return record.itemId == itemId; })) {
        return {false, L"此项目已经由本工具禁用。"};
    }
    ScanResult scan = ScanAll();
    const auto found = std::find_if(scan.items.begin(), scan.items.end(), [&](const StartupItem& item) { return item.id == itemId; });
    if (found == scan.items.end()) return {false, L"启动项不存在或已发生变化，请刷新后重试。"};
    if (!found->canDisable || found->readOnly) return {false, L"此项目仅供查看。"};

    DisabledRecord record;
    record.itemId = found->id;
    record.source = found->source;
    record.name = found->name;
    record.disabledAt = CurrentTimestamp();
    record.recordId = StableId(found->source, found->id, record.disabledAt);
    record.requiresAdmin = found->requiresAdmin;
    record.original = found->original;
    records.push_back(record);
    if (!store_.Save(records, error)) return {false, error};

    OperationResult operation = ApplyDisable(records.back());
    if (!operation.success) {
        records.pop_back();
        std::wstring cleanupError;
        store_.Save(records, cleanupError);
        return operation;
    }
    // ApplyDisable 可能补充恢复信息（启动文件备份路径 / StartupApproved 原始状态），需要再次落盘。
    if (!store_.Save(records, error)) {
        ApplyRestore(records.back());
        records.pop_back();
        std::wstring cleanupError;
        store_.Save(records, cleanupError);
        return {false, L"无法保存恢复信息，操作已撤销。"};
    }
    if (!VerifyDisabled(records.back())) {
        return {false, L"操作已执行，但未能确认结果；恢复记录已保留。"};
    }
    return operation;
}

OperationResult StartupManager::Restore(const std::wstring& recordId) const {
    std::vector<DisabledRecord> records;
    std::wstring error;
    if (!store_.Load(records, error)) return {false, error};
    const auto found = std::find_if(records.begin(), records.end(), [&](const DisabledRecord& record) { return record.recordId == recordId; });
    if (found == records.end()) return {false, L"恢复记录不存在。"};
    const std::size_t index = static_cast<std::size_t>(std::distance(records.begin(), found));
    OperationResult operation = ApplyRestore(*found);
    if (!operation.success) return operation;
    records.erase(records.begin() + static_cast<std::ptrdiff_t>(index));
    if (!store_.Save(records, error)) {
        return {true, L"项目已恢复，但无法清理恢复记录：" + error};
    }
    return operation;
}

// ================= AdBlockManager（广告拦截简化版） =================

AdBlockManager::AdBlockManager()
    : store_(AppLaunchLockerDataDirectory() / L"blocked-items.json") {}

AdBlockManager::AdBlockManager(DisabledItemStore store)
    : store_(std::move(store)) {}

ScanResult AdBlockManager::ScanPath(const std::wstring& fileOrDir) const {
    ScanResult result;
    if (fileOrDir.empty()) {
        result.warnings.push_back(L"未提供路径。");
        return result;
    }
    bool uninitialize = false;
    if (!InitializeComForScan(uninitialize)) result.warnings.push_back(L"COM 初始化失败，快捷方式可能无法解析。");

    std::vector<std::wstring> candidates;
    const DWORD attributes = GetFileAttributesW(fileOrDir.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        result.warnings.push_back(L"路径不存在：" + fileOrDir);
        if (uninitialize) CoUninitialize();
        return result;
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        std::wstring pattern = fileOrDir;
        if (!pattern.empty() && pattern.back() != L'\\') pattern.push_back(L'\\');
        WIN32_FIND_DATAW find{};
        HANDLE handle = FindFirstFileW((pattern + L"*").c_str(), &find);
        if (handle != INVALID_HANDLE_VALUE) {
            do {
                if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;  // 不递归子目录
                const std::wstring fileName = find.cFileName;
                if (IsStartableCandidate(fileName)) candidates.push_back(pattern + fileName);
            } while (FindNextFileW(handle, &find));
            FindClose(handle);
        }
    } else {
        candidates.push_back(fileOrDir);
    }

    // 预先索引可经系统“启动”开关禁用的自启动项（全局仅扫一次），供逐项标注“含自启动项”。
    // 值为 true 表示至少有一个匹配的自启动注册位于 HKLM（解除/拦截需管理员）。
    std::map<std::wstring, bool> autoStartExeLower;
    {
        ScanResult startup;
        ScanRegistry(startup);
        ScanStartupFolder(startup, FOLDERID_Startup, false);
        ScanStartupFolder(startup, FOLDERID_CommonStartup, true);
        for (StartupItem& startupItem : startup.items) {
            if (startupItem.source != StartupSourceType::Registry &&
                startupItem.source != StartupSourceType::StartupFolder) continue;
            if (!IsAutoStartEligible(startupItem)) continue;
            const std::wstring exe = ExecutableFromCommand(startupItem.command);
            if (exe.empty()) continue;
            bool& requiresAdmin = autoStartExeLower[Lower(exe)];
            requiresAdmin = requiresAdmin || startupItem.requiresAdmin;
        }
    }

    for (const std::wstring& candidate : candidates) {
        const std::wstring displayName = FileNameOf(candidate);
        const LaunchTarget target = ResolveLaunchTarget(candidate);
        if (!target.valid) {
            // 无法解析（如断链快捷方式）：列出但仅查看。
            AddItem(result, StartupSourceType::Ifeo, displayName, candidate, L"", true, false,
                {{L"adBlockStatus", L"unresolved"}});
            continue;
        }
        if (target.category != L"exe") {
            // 脚本/其它：仅提示，不提供 IFEO（脚本由解释器加载，直接 IFEO 无效）。
            AddItem(result, StartupSourceType::Ifeo, displayName, candidate, target.path, true, false,
                {{L"adBlockStatus", target.category == L"script" ? L"script" : L"other"},
                 {L"targetPath", target.path}});
            continue;
        }
        const GuardVerdict guard = EvaluateGuard(target.path, target.imageName);
        std::map<std::wstring, std::wstring> original = {
            {L"targetPath", target.path},
            {L"ifeoImageName", target.imageName},
            {L"adBlockStatus", guard.allow ? (guard.warn ? L"blockable-warn" : L"blockable") : L"protected"},
        };
        if (!guard.reason.empty()) original[L"guardReason"] = guard.reason;
        const auto autoStartIt = autoStartExeLower.find(Lower(target.path));
        if (autoStartIt != autoStartExeLower.end()) {
            original[L"hasAutoStart"] = L"1";
            if (autoStartIt->second) original[L"autoStartRequiresAdmin"] = L"1";
        }
        AddItem(result, StartupSourceType::Ifeo, displayName, candidate, target.path,
            true, guard.allow, std::move(original));
    }

    if (uninitialize) CoUninitialize();
    std::sort(result.items.begin(), result.items.end(), [](const StartupItem& left, const StartupItem& right) {
        if (left.canDisable != right.canDisable) return left.canDisable > right.canDisable;
        return Lower(left.name) < Lower(right.name);
    });
    return result;
}

OperationResult AdBlockManager::Block(const std::wstring& targetPath, const std::wstring& mode) const {
    if (mode != L"exact" && mode != L"name" && mode != L"startup") return {false, L"未知拦截模式。"};
    bool uninitialize = false;
    InitializeComForScan(uninitialize);
    const LaunchTarget target = ResolveLaunchTarget(targetPath);
    if (uninitialize) CoUninitialize();
    if (!target.valid) return {false, L"目标文件不存在或已发生变化。"};
    if (target.category != L"exe") return {false, L"仅支持拦截可执行程序（exe）；脚本请从其自启动来源禁用。"};

    const GuardVerdict guard = EvaluateGuard(target.path, target.imageName);
    if (!guard.allow) return {false, L"该程序不允许拦截：" + guard.reason};

    // 启动拦截：仅禁用该程序的开机自启动项（系统 StartupApproved 开关，与任务管理器同步，不阻止手动运行）。
    if (mode == L"startup") return BlockStartup(target.path);

    std::vector<DisabledRecord> records;
    std::wstring error;
    if (!store_.Load(records, error)) return {false, error};

    // 去重：按模式+目标判定。
    const std::wstring imageName = target.imageName;
    for (const DisabledRecord& existing : records) {
        if (MapValue(existing.original, L"mechanism") != L"ifeo") continue;
        if (MapValue(existing.original, L"blockMode") != mode) continue;
        if (mode == L"exact") {
            if (Lower(MapValue(existing.original, L"targetPath")) == Lower(target.path)) return {false, L"该程序已被拦截。"};
        } else {
            if (MapValue(existing.original, L"ifeoImageName") == imageName) return {false, L"该程序已被拦截。"};
        }
    }

    DisabledRecord record;
    record.itemId = StableId(StartupSourceType::Ifeo, target.path, mode);
    record.source = StartupSourceType::Ifeo;
    record.name = FileNameOf(target.path);
    record.disabledAt = CurrentTimestamp();
    record.recordId = StableId(StartupSourceType::Ifeo, record.itemId, record.disabledAt);
    record.requiresAdmin = true;
    record.original = {
        {L"mechanism", L"ifeo"},
        {L"blockMode", mode},
        {L"ifeoImageName", imageName},
        {L"ifeoView", DetectIfeoView(target.path)},
        {L"targetPath", target.path},
    };
    if (mode == L"name") record.original[L"filterFullPath"] = L"";
    else record.original[L"filterFullPath"] = target.path;

    records.push_back(record);
    if (!store_.Save(records, error)) return {false, error};

    OperationResult applied = ApplyIfeoBlock(records.back());
    if (!applied.success) {
        records.pop_back();
        std::wstring cleanupError;
        store_.Save(records, cleanupError);
        return applied;
    }
    // ApplyIfeoBlock 补充了恢复字段（原 Debugger / 子键名），再次落盘。
    if (!store_.Save(records, error)) {
        RestoreIfeoBlock(records.back());
        records.pop_back();
        std::wstring cleanupError;
        store_.Save(records, cleanupError);
        return {false, L"无法保存拦截记录，操作已撤销。"};
    }
    if (!VerifyIfeoBlock(records.back())) {
        return {false, L"操作已执行，但未能确认结果；拦截记录已保留。"};
    }
    return applied;
}

OperationResult AdBlockManager::BlockStartup(const std::wstring& targetExe) const {
    std::vector<StartupItem> entries = FindAutoStartEntries(targetExe);
    if (entries.empty()) return {false, L"该程序未注册开机自启动项，无需用「禁止自启」模式拦截。"};

    std::vector<DisabledRecord> records;
    std::wstring error;
    if (!store_.Load(records, error)) return {false, error};

    int done = 0;
    int skipped = 0;
    std::vector<std::size_t> addedIndices;
    for (const StartupItem& entry : entries) {
        // 去重：同一自启动注册（itemId）已由本工具禁用则跳过。
        const bool exists = std::any_of(records.begin(), records.end(), [&](const DisabledRecord& r) {
            return MapValue(r.original, L"mechanism") == L"startup-approved" && r.itemId == entry.id;
        });
        if (exists) { ++skipped; continue; }

        DisabledRecord record;
        record.itemId = entry.id;
        record.source = entry.source;
        record.name = FileNameOf(targetExe);
        record.disabledAt = CurrentTimestamp();
        record.recordId = StableId(entry.source, entry.id, record.disabledAt);
        record.requiresAdmin = entry.requiresAdmin;
        record.original = entry.original;  // Registry: hive/key/valueName；StartupFolder: originalPath
        record.original[L"mechanism"] = L"startup-approved";
        record.original[L"blockMode"] = L"startup";
        record.original[L"targetPath"] = targetExe;

        const StartupApprovedTarget saTarget = ResolveStartupApprovedTarget(record);
        if (!saTarget.eligible) { ++skipped; continue; }

        const OperationResult applied = DisableViaStartupApproved(record, saTarget);
        if (!applied.success) {
            // 回滚本次已禁用的项。
            for (auto it = addedIndices.rbegin(); it != addedIndices.rend(); ++it) {
                RestoreViaStartupApproved(records[*it]);
            }
            return applied;
        }
        records.push_back(record);
        addedIndices.push_back(records.size() - 1);
        ++done;
    }

    if (done == 0) {
        return {false, skipped > 0 ? L"该程序的自启动项已全部被拦截。" : L"未找到可禁用的自启动项。"};
    }
    if (!store_.Save(records, error)) {
        for (auto it = addedIndices.rbegin(); it != addedIndices.rend(); ++it) {
            RestoreViaStartupApproved(records[*it]);
        }
        return {false, L"无法保存拦截记录，操作已撤销。"};
    }
    return {true, L"已禁止 " + std::to_wstring(done) + L" 个自启动项。"};
}

OperationResult AdBlockManager::Unblock(const std::wstring& recordId) const {
    std::vector<DisabledRecord> records;
    std::wstring error;
    if (!store_.Load(records, error)) return {false, error};
    const auto found = std::find_if(records.begin(), records.end(),
        [&](const DisabledRecord& record) { return record.recordId == recordId; });
    if (found == records.end()) return {false, L"拦截记录不存在。"};
    const std::size_t index = static_cast<std::size_t>(std::distance(records.begin(), found));
    // 按机制分派恢复：启动拦截走系统开关，其余走 IFEO。
    OperationResult operation = MapValue(found->original, L"mechanism") == L"startup-approved"
        ? RestoreViaStartupApproved(*found)
        : RestoreIfeoBlock(*found);
    if (!operation.success) return operation;
    records.erase(records.begin() + static_cast<std::ptrdiff_t>(index));
    if (!store_.Save(records, error)) {
        return {true, L"已解除拦截，但无法清理记录：" + error};
    }
    return operation;
}

bool AdBlockManager::ListBlocked(std::vector<DisabledRecord>& records, std::wstring& error) const {
    return store_.Load(records, error);
}
