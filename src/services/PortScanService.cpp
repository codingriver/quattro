#include "PortScanService.h"

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>

#include <map>

namespace {
struct Tcp6RowOwnerPidCompat {
    UCHAR localAddr[16]{};
    DWORD localScopeId = 0;
    DWORD localPort = 0;
    UCHAR remoteAddr[16]{};
    DWORD remoteScopeId = 0;
    DWORD remotePort = 0;
    DWORD state = 0;
    DWORD owningPid = 0;
};

struct Tcp6TableOwnerPidCompat {
    DWORD entryCount = 0;
    Tcp6RowOwnerPidCompat table[1]{};
};

struct Udp6RowOwnerPidCompat {
    UCHAR localAddr[16]{};
    DWORD localScopeId = 0;
    DWORD localPort = 0;
    DWORD owningPid = 0;
};

struct Udp6TableOwnerPidCompat {
    DWORD entryCount = 0;
    Udp6RowOwnerPidCompat table[1]{};
};

unsigned short NetworkOrderPort(DWORD value) {
    return ntohs(static_cast<u_short>(value));
}

std::wstring TcpStateText(DWORD state) {
    switch (state) {
    case MIB_TCP_STATE_CLOSED: return L"CLOSED";
    case MIB_TCP_STATE_LISTEN: return L"LISTEN";
    case MIB_TCP_STATE_SYN_SENT: return L"SYN_SENT";
    case MIB_TCP_STATE_SYN_RCVD: return L"SYN_RCVD";
    case MIB_TCP_STATE_ESTAB: return L"ESTABLISHED";
    case MIB_TCP_STATE_FIN_WAIT1: return L"FIN_WAIT1";
    case MIB_TCP_STATE_FIN_WAIT2: return L"FIN_WAIT2";
    case MIB_TCP_STATE_CLOSE_WAIT: return L"CLOSE_WAIT";
    case MIB_TCP_STATE_CLOSING: return L"CLOSING";
    case MIB_TCP_STATE_LAST_ACK: return L"LAST_ACK";
    case MIB_TCP_STATE_TIME_WAIT: return L"TIME_WAIT";
    case MIB_TCP_STATE_DELETE_TCB: return L"DELETE_TCB";
    default: return L"UNKNOWN";
    }
}

void Add(std::map<DWORD, PortScanRecord>& records, DWORD pid, std::wstring endpoint) {
    PortScanRecord& record = records[pid];
    record.processId = pid;
    record.endpoints.insert(std::move(endpoint));
}
}

PortScanResult PortScanService::Scan(unsigned short port) const {
    ScanTaskOptions options;
    options.mode = ScanExecutionMode::CallerSingle;
    return ScanExecutionService::Run<PortScanResult>(options,
        [port](ScanTaskContext& context) { return PortScanService().ScanCore(port, context); });
}

std::shared_ptr<ScanTaskHandle> PortScanService::StartScan(
    unsigned short port,
    std::function<void()> completionCallback) const {
    ScanTaskOptions options;
    options.mode = ScanExecutionMode::BackgroundSingle;
    options.completionCallback = std::move(completionCallback);
    return ScanExecutionService::StartTyped<PortScanResult>(options,
        [port](ScanTaskContext& context) { return PortScanService().ScanCore(port, context); });
}

PortScanResult PortScanService::ScanCore(unsigned short port, ScanTaskContext& context) const {
    PortScanResult result;
    result.port = port;
    context.Report(ScanProgressUpdate{
        L"port-tables", L"端口扫描", L"正在读取网络连接", L"端口 " + std::to_wstring(port)});
    std::map<DWORD, PortScanRecord> records;

    DWORD size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == ERROR_INSUFFICIENT_BUFFER) {
        std::vector<BYTE> buffer(size);
        if (GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
            for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                const auto& row = table->table[i];
                if (NetworkOrderPort(row.dwLocalPort) == port) Add(records, row.dwOwningPid, L"TCP " + TcpStateText(row.dwState));
            }
        }
    }
    size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0) == ERROR_INSUFFICIENT_BUFFER) {
        std::vector<BYTE> buffer(size);
        if (GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            const auto* table = reinterpret_cast<const Tcp6TableOwnerPidCompat*>(buffer.data());
            for (DWORD i = 0; i < table->entryCount; ++i) {
                const auto& row = table->table[i];
                if (NetworkOrderPort(row.localPort) == port) Add(records, row.owningPid, L"TCP6 " + TcpStateText(row.state));
            }
        }
    }
    size = 0;
    if (GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) == ERROR_INSUFFICIENT_BUFFER) {
        std::vector<BYTE> buffer(size);
        if (GetExtendedUdpTable(buffer.data(), &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
            const auto* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(buffer.data());
            for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                const auto& row = table->table[i];
                if (NetworkOrderPort(row.dwLocalPort) == port) Add(records, row.dwOwningPid, L"UDP");
            }
        }
    }
    size = 0;
    if (GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0) == ERROR_INSUFFICIENT_BUFFER) {
        std::vector<BYTE> buffer(size);
        if (GetExtendedUdpTable(buffer.data(), &size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
            const auto* table = reinterpret_cast<const Udp6TableOwnerPidCompat*>(buffer.data());
            for (DWORD i = 0; i < table->entryCount; ++i) {
                const auto& row = table->table[i];
                if (NetworkOrderPort(row.localPort) == port) Add(records, row.owningPid, L"UDP6");
            }
        }
    }

    for (auto& [pid, record] : records) {
        (void)pid;
        result.records.push_back(std::move(record));
    }
    context.UpdateProgress([&result](ScanProgressUpdate& value) {
        value.status = L"扫描完成";
        value.detail = L"发现 " + std::to_wstring(result.records.size()) + L" 个占用进程";
        value.discovered = result.records.size();
    });
    return result;
}
