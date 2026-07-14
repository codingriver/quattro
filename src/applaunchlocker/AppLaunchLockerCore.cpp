#include "AppLaunchLockerCore.h"

#include "JsonValue.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <taskschd.h>
#include <wbemidl.h>

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

const std::array<RegistryLocation, 13> kRegistryLocations{{
    {HKEY_CURRENT_USER, L"HKCU", L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", StartupSourceType::Registry, true},
    {HKEY_CURRENT_USER, L"HKCU", L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", StartupSourceType::Registry, true},
    {HKEY_LOCAL_MACHINE, L"HKLM", L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", StartupSourceType::Registry, true},
    {HKEY_LOCAL_MACHINE, L"HKLM", L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", StartupSourceType::Registry, true},
    {HKEY_LOCAL_MACHINE, L"HKLM", L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run", StartupSourceType::Registry, true},
    {HKEY_LOCAL_MACHINE, L"HKLM", L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnce", StartupSourceType::Registry, true},
    {HKEY_CURRENT_USER, L"HKCU", L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run", StartupSourceType::Registry, true},
    {HKEY_LOCAL_MACHINE, L"HKLM", L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run", StartupSourceType::Registry, true},
    {HKEY_CURRENT_USER, L"HKCU", L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Windows", StartupSourceType::Registry, true},
    {HKEY_LOCAL_MACHINE, L"HKLM", L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", StartupSourceType::Winlogon, false},
    {HKEY_CURRENT_USER, L"HKCU", L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", StartupSourceType::Winlogon, false},
    {HKEY_LOCAL_MACHINE, L"HKLM", L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows", StartupSourceType::AppInitDll, false},
    {HKEY_LOCAL_MACHINE, L"HKLM", L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Windows", StartupSourceType::AppInitDll, false},
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
        const std::wstring location = std::wstring(L"HKLM\\") + keyPath + L"\\" + name + L" (" + viewName + L")";
        AddItem(result, StartupSourceType::Ifeo, name, location, debugger, true, false);
    }
}

void ScanIfeo(ScanResult& result) {
    ScanIfeoView(result, KEY_WOW64_64KEY, L"64 位");
    ScanIfeoView(result, KEY_WOW64_32KEY, L"32 位");
}

HKEY HiveFromText(const std::wstring& value) {
    if (value == L"HKCU") return HKEY_CURRENT_USER;
    if (value == L"HKLM") return HKEY_LOCAL_MACHINE;
    return nullptr;
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
    switch (record.source) {
    case StartupSourceType::Registry: {
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
    switch (record.source) {
    case StartupSourceType::Registry: return DisableRegistry(record);
    case StartupSourceType::StartupFolder: return DisableFolder(record);
    case StartupSourceType::ScheduledTask: return SetTaskEnabled(record, false);
    case StartupSourceType::Service: return SetServiceStart(record, false);
    default: return {false, L"此项目仅供查看。"};
    }
}

OperationResult ApplyRestore(const DisabledRecord& record) {
    switch (record.source) {
    case StartupSourceType::Registry: return RestoreRegistry(record);
    case StartupSourceType::StartupFolder: return RestoreFolder(record);
    case StartupSourceType::ScheduledTask: return SetTaskEnabled(record, MapValue(record.original, L"wasEnabled") != L"0");
    case StartupSourceType::Service: return SetServiceStart(record, true);
    default: return {false, L"此记录不支持恢复。"};
    }
}

bool InitializeComForScan(bool& uninitialize) {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    uninitialize = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;
    const HRESULT security = CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
        RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
    return SUCCEEDED(security) || security == RPC_E_TOO_LATE;
}
}

std::wstring StartupSourceKey(StartupSourceType source) {
    switch (source) {
    case StartupSourceType::Registry: return L"registry";
    case StartupSourceType::StartupFolder: return L"startup-folder";
    case StartupSourceType::ScheduledTask: return L"scheduled-task";
    case StartupSourceType::Service: return L"service";
    case StartupSourceType::Driver: return L"driver";
    case StartupSourceType::WmiSubscription: return L"wmi";
    case StartupSourceType::Winlogon: return L"winlogon";
    case StartupSourceType::AppInitDll: return L"appinit-dll";
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
    case StartupSourceType::Driver: return L"驱动";
    case StartupSourceType::WmiSubscription: return L"WMI";
    case StartupSourceType::Winlogon: return L"登录项";
    case StartupSourceType::AppInitDll: return L"AppInit DLL";
    case StartupSourceType::Ifeo: return L"IFEO";
    }
    return L"未知";
}

bool StartupSourceFromKey(const std::wstring& key, StartupSourceType& source) {
    for (StartupSourceType candidate : {StartupSourceType::Registry, StartupSourceType::StartupFolder,
            StartupSourceType::ScheduledTask, StartupSourceType::Service, StartupSourceType::Driver,
            StartupSourceType::WmiSubscription, StartupSourceType::Winlogon, StartupSourceType::AppInitDll,
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
    ScanStartupFolder(result, FOLDERID_Startup, false);
    ScanStartupFolder(result, FOLDERID_CommonStartup, true);
    ScanScheduledTasks(result);
    ScanServices(result);
    ScanWmi(result);
    ScanIfeo(result);
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
    if (records.back().source == StartupSourceType::StartupFolder && operation.success) {
        record = records.back();
        records.back() = record;
        if (!store_.Save(records, error)) {
            ApplyRestore(record);
            records.pop_back();
            store_.Save(records, error);
            return {false, L"无法保存启动文件备份位置，操作已撤销。"};
        }
    }
    if (!operation.success) {
        records.pop_back();
        std::wstring cleanupError;
        store_.Save(records, cleanupError);
        return operation;
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
