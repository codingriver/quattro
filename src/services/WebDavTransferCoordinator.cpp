#include "WebDavTransferCoordinator.h"

#include "AppLog.h"
#include "Utilities.h"
#include "WebDavTransferQueueController.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <sddl.h>

namespace {
constexpr std::uint32_t kMagic = 0x51544657;
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kUpload = 1;
constexpr std::uint32_t kDownload = 2;
constexpr std::uint32_t kShow = 3;
constexpr std::uint32_t kMaxItems = 1000;
constexpr std::uint32_t kMaxString = 32768;
constexpr UINT WM_COORDINATOR_REQUEST = WM_APP + 0x2E1;

struct Request {
    std::uint32_t kind = 0;
    std::vector<std::filesystem::path> uploads;
    std::vector<WebDavFileRecord> downloads;
};

std::wstring ScopeSuffix() {
    std::wstring suffix = Hex8(StablePathHash(GetModuleDirectory().wstring()));
    wchar_t testId[128]{};
    const DWORD length = GetEnvironmentVariableW(L"QUATTRO_TEST_RUN_ID", testId, static_cast<DWORD>(std::size(testId)));
    if (length > 0 && length < std::size(testId)) suffix += L"_Test_" + std::wstring(testId, length);
    return suffix;
}

std::wstring PipeName() { return L"\\\\.\\pipe\\QuattroWebDavTransfer_" + ScopeSuffix(); }
std::wstring MutexName() { return L"Local\\QuattroWebDavTransferHost_" + ScopeSuffix(); }

template <typename T>
void Append(std::vector<std::uint8_t>& data, const T& value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    data.insert(data.end(), bytes, bytes + sizeof(T));
}

void AppendString(std::vector<std::uint8_t>& data, const std::wstring& value) {
    const std::uint32_t length = static_cast<std::uint32_t>(std::min<std::size_t>(value.size(), kMaxString));
    Append(data, length);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(value.data());
    data.insert(data.end(), bytes, bytes + static_cast<std::size_t>(length) * sizeof(wchar_t));
}

template <typename T>
bool Read(const std::vector<std::uint8_t>& data, std::size_t& offset, T& value) {
    if (offset + sizeof(T) > data.size()) return false;
    memcpy(&value, data.data() + offset, sizeof(T)); offset += sizeof(T); return true;
}

bool ReadString(const std::vector<std::uint8_t>& data, std::size_t& offset, std::wstring& value) {
    std::uint32_t length = 0;
    if (!Read(data, offset, length) || length > kMaxString || offset + static_cast<std::size_t>(length) * sizeof(wchar_t) > data.size()) return false;
    value.assign(reinterpret_cast<const wchar_t*>(data.data() + offset), length);
    offset += static_cast<std::size_t>(length) * sizeof(wchar_t); return true;
}

std::vector<std::uint8_t> SerializeUploads(const std::vector<std::filesystem::path>& paths) {
    std::vector<std::uint8_t> data; Append(data, kMagic); Append(data, kVersion); Append(data, kUpload);
    const std::uint32_t count = static_cast<std::uint32_t>(std::min<std::size_t>(paths.size(), kMaxItems)); Append(data, count);
    for (std::uint32_t i = 0; i < count; ++i) AppendString(data, paths[i].wstring());
    return data;
}

std::vector<std::uint8_t> SerializeDownloads(const std::vector<WebDavFileRecord>& records) {
    std::vector<std::uint8_t> data; Append(data, kMagic); Append(data, kVersion); Append(data, kDownload);
    const std::uint32_t count = static_cast<std::uint32_t>(std::min<std::size_t>(records.size(), kMaxItems)); Append(data, count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto& record = records[i];
        AppendString(data, record.id); AppendString(data, record.absolutePath); AppendString(data, record.displayName);
        Append(data, record.size); AppendString(data, record.sha256); AppendString(data, record.uploadedAtUtc);
        AppendString(data, record.uploadState); const std::uint8_t ready = record.contentReady ? 1 : 0; Append(data, ready);
    }
    return data;
}

std::vector<std::uint8_t> SerializeShow() {
    std::vector<std::uint8_t> data;
    Append(data, kMagic); Append(data, kVersion); Append(data, kShow);
    const std::uint32_t count = 0; Append(data, count);
    return data;
}

bool Deserialize(const std::vector<std::uint8_t>& data, Request& request) {
    std::size_t offset = 0; std::uint32_t magic = 0, version = 0, count = 0;
    if (!Read(data, offset, magic) || !Read(data, offset, version) || !Read(data, offset, request.kind) ||
        !Read(data, offset, count) || magic != kMagic || version != kVersion || count > kMaxItems) return false;
    if (request.kind == kUpload) {
        for (std::uint32_t i = 0; i < count; ++i) { std::wstring path; if (!ReadString(data, offset, path)) return false; request.uploads.emplace_back(path); }
    } else if (request.kind == kDownload) {
        for (std::uint32_t i = 0; i < count; ++i) {
            WebDavFileRecord record; std::uint8_t ready = 0;
            if (!ReadString(data, offset, record.id) || !ReadString(data, offset, record.absolutePath) ||
                !ReadString(data, offset, record.displayName) || !Read(data, offset, record.size) ||
                !ReadString(data, offset, record.sha256) || !ReadString(data, offset, record.uploadedAtUtc) ||
                !ReadString(data, offset, record.uploadState) || !Read(data, offset, ready)) return false;
            record.contentReady = ready != 0; request.downloads.push_back(std::move(record));
        }
    } else if (request.kind != kShow || count != 0) return false;
    return offset == data.size();
}

bool WriteExact(HANDLE handle, const void* source, DWORD size) {
    const auto* bytes = static_cast<const std::uint8_t*>(source);
    DWORD offset = 0;
    while (offset < size) {
        DWORD written = 0;
        if (!WriteFile(handle, bytes + offset, size - offset, &written, nullptr) || written == 0) return false;
        offset += written;
    }
    return true;
}

bool ReadExact(HANDLE handle, void* destination, DWORD size) {
    auto* bytes = static_cast<std::uint8_t*>(destination);
    DWORD offset = 0;
    while (offset < size) {
        DWORD read = 0;
        if (!ReadFile(handle, bytes + offset, size - offset, &read, nullptr) || read == 0) return false;
        offset += read;
    }
    return true;
}

bool StartHost(std::wstring& error) {
    std::wstring executable(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) { error = L"无法获取 Quattro 程序路径。"; return false; }
    executable.resize(length);
    std::wstring command = L"\"" + executable + L"\" --webdav-transfer-host";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        error = L"无法启动 WebDAV 传输窗口：" + FormatLastError(GetLastError()); return false;
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess); return true;
}

bool Send(const std::vector<std::uint8_t>& data, std::wstring& error) {
    for (int attempt = 0; attempt < 30; ++attempt) {
        HANDLE pipe = CreateFileW(PipeName().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            const std::uint32_t size = static_cast<std::uint32_t>(data.size());
            const bool ok = WriteExact(pipe, &size, sizeof(size)) && WriteExact(pipe, data.data(), size);
            CloseHandle(pipe);
            if (!ok) error = L"发送 WebDAV 传输请求失败：" + FormatLastError(GetLastError());
            return ok;
        }
        if (attempt == 0 && !StartHost(error)) return false;
        Sleep(100);
    }
    error = L"等待 WebDAV 传输窗口超时。"; return false;
}
}

bool WebDavTransferCoordinator::SubmitUploads(const std::vector<std::filesystem::path>& paths, std::wstring& error) {
    if (paths.empty()) { error = L"没有可上传的文件。"; return false; }
    const bool ok = Send(SerializeUploads(paths), error);
    WriteAppLog(ok ? L"WebDAV 传输请求已发送：上传 " + std::to_wstring(paths.size()) + L" 个文件。"
                   : L"WebDAV 上传请求发送失败：" + error);
    return ok;
}

bool WebDavTransferCoordinator::SubmitDownloads(const std::vector<WebDavFileRecord>& records, std::wstring& error) {
    if (records.empty()) { error = L"没有可下载的文件。"; return false; }
    return Send(SerializeDownloads(records), error);
}

bool WebDavTransferCoordinator::ShowQueue(std::wstring& error) {
    const bool ok = Send(SerializeShow(), error);
    WriteAppLog(ok ? L"WebDAV 传输队列显示请求已发送。" : L"WebDAV 传输队列显示请求失败：" + error);
    return ok;
}

int WebDavTransferCoordinator::RunHost(HINSTANCE instance, const Theme& theme, const AppConfig& config) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, MutexName().c_str());
    if (!mutex) return 2;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return 0; }
    WriteAppLog(L"WebDAV 公共传输宿主已启动。pid=" + std::to_wstring(GetCurrentProcessId()));

    const DWORD threadId = GetCurrentThreadId();
    std::mutex requestsMutex; std::deque<Request> requests;
    std::jthread server([&](std::stop_token token) {
        while (!token.stop_requested()) {
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            SECURITY_ATTRIBUTES security{};
            security.nLength = sizeof(security);
            if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    L"D:P(A;;GA;;;SY)(A;;GA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr)) {
                security.lpSecurityDescriptor = descriptor;
            }
            HANDLE pipe = CreateNamedPipeW(PipeName().c_str(), PIPE_ACCESS_INBOUND,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 64 * 1024, 64 * 1024, 0,
                security.lpSecurityDescriptor ? &security : nullptr);
            if (descriptor) LocalFree(descriptor);
            if (pipe == INVALID_HANDLE_VALUE) { Sleep(100); continue; }
            const bool connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
            if (token.stop_requested()) { CloseHandle(pipe); break; }
            std::uint32_t size = 0;
            if (connected && ReadExact(pipe, &size, sizeof(size)) && size <= 16 * 1024 * 1024) {
                std::vector<std::uint8_t> data(size);
                if (ReadExact(pipe, data.data(), size)) {
                    Request request;
                    if (Deserialize(data, request)) { std::lock_guard lock(requestsMutex); requests.push_back(std::move(request)); PostThreadMessageW(threadId, WM_COORDINATOR_REQUEST, 0, 0); }
                }
            }
            DisconnectNamedPipe(pipe); CloseHandle(pipe);
        }
    });

    WebDavTransferQueueController controller(nullptr, instance, theme, config);
    const auto started = std::chrono::steady_clock::now();
    bool receivedAny = false;
    MSG message{};
    for (;;) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) goto done;
            if (message.message == WM_COORDINATOR_REQUEST) {
                std::deque<Request> pending;
                { std::lock_guard lock(requestsMutex); pending.swap(requests); }
                for (auto& request : pending) {
                    if (request.kind == kUpload) {
                        WriteAppLog(L"WebDAV 传输宿主收到上传请求：" + std::to_wstring(request.uploads.size()) + L" 个文件。");
                        controller.EnqueueUploads(request.uploads);
                    } else if (request.kind == kDownload) {
                        WriteAppLog(L"WebDAV 传输宿主收到下载请求：" + std::to_wstring(request.downloads.size()) + L" 个文件。");
                        controller.EnqueueDownloads(request.downloads);
                    } else {
                        WriteAppLog(L"WebDAV 传输宿主收到显示队列请求。");
                    }
                    receivedAny = true;
                }
                controller.Show();
                continue;
            }
            if (!ThemedUi::PreTranslateMessage(message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        if (receivedAny && !controller.HasRunningOrWaitingTasks() && !controller.IsWindowVisible()) break;
        if (!receivedAny && std::chrono::steady_clock::now() - started > std::chrono::seconds(10)) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
    }
done:
    server.request_stop();
    if (HANDLE wake = CreateFileW(PipeName().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
    ReleaseMutex(mutex); CloseHandle(mutex);
    WriteAppLog(L"WebDAV 公共传输宿主已退出。pid=" + std::to_wstring(GetCurrentProcessId()));
    return controller.FailedCount() > 0 ? 2 : 0;
}
