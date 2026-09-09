#include "PortScanService.h"

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>

#include <array>
#include <cstddef>
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

DWORD ReadTableBuffer(PortScanSource source, std::vector<BYTE>& buffer) {
    const auto read = [source](void* data, DWORD* size) {
        switch (source) {
        case PortScanSource::TcpIPv4:
            return GetExtendedTcpTable(data, size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
        case PortScanSource::TcpIPv6:
            return GetExtendedTcpTable(data, size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0);
        case PortScanSource::UdpIPv4:
            return GetExtendedUdpTable(data, size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
        case PortScanSource::UdpIPv6:
            return GetExtendedUdpTable(data, size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0);
        }
        return static_cast<DWORD>(ERROR_INVALID_PARAMETER);
    };

    DWORD size = 0;
    DWORD status = read(nullptr, &size);
    if (status == NO_ERROR) {
        buffer.clear();
        return NO_ERROR;
    }
    if (status != ERROR_INSUFFICIENT_BUFFER || size < sizeof(DWORD)) return status;

    for (int attempt = 0; attempt < 4; ++attempt) {
        buffer.resize(size);
        DWORD required = size;
        status = read(buffer.data(), &required);
        if (status == NO_ERROR) {
            if (required > 0 && required < buffer.size()) buffer.resize(required);
            return NO_ERROR;
        }
        if (status != ERROR_INSUFFICIENT_BUFFER) return status;
        const DWORD growth = std::max<DWORD>(4096, size / 2);
        size = required > size ? required : size + growth;
    }
    return ERROR_INSUFFICIENT_BUFFER;
}

template<class Table, class Row, class Count, class Visit>
bool VisitRows(const std::vector<BYTE>& buffer, Count count, Visit visit) {
    constexpr std::size_t offset = offsetof(Table, table);
    if (buffer.size() < offset) return false;
    const auto* table = reinterpret_cast<const Table*>(buffer.data());
    const std::size_t rowCount = count(*table);
    if (rowCount > (buffer.size() - offset) / sizeof(Row)) return false;
    for (std::size_t index = 0; index < rowCount; ++index) visit(table->table[index]);
    return true;
}

PortScanSourceResult QuerySource(PortScanSource source, unsigned short port) {
    PortScanSourceResult result;
    std::vector<BYTE> buffer;
    result.errorCode = ReadTableBuffer(source, buffer);
    if (result.errorCode != NO_ERROR || buffer.empty()) return result;

    std::map<DWORD, PortScanRecord> records;
    bool valid = false;
    switch (source) {
    case PortScanSource::TcpIPv4:
        valid = VisitRows<MIB_TCPTABLE_OWNER_PID, MIB_TCPROW_OWNER_PID>(buffer,
            [](const auto& table) { return table.dwNumEntries; }, [&](const auto& row) {
                if (NetworkOrderPort(row.dwLocalPort) == port) {
                    Add(records, row.dwOwningPid, L"TCP " + TcpStateText(row.dwState));
                }
            });
        break;
    case PortScanSource::TcpIPv6:
        valid = VisitRows<Tcp6TableOwnerPidCompat, Tcp6RowOwnerPidCompat>(buffer,
            [](const auto& table) { return table.entryCount; }, [&](const auto& row) {
                if (NetworkOrderPort(row.localPort) == port) {
                    Add(records, row.owningPid, L"TCP6 " + TcpStateText(row.state));
                }
            });
        break;
    case PortScanSource::UdpIPv4:
        valid = VisitRows<MIB_UDPTABLE_OWNER_PID, MIB_UDPROW_OWNER_PID>(buffer,
            [](const auto& table) { return table.dwNumEntries; }, [&](const auto& row) {
                if (NetworkOrderPort(row.dwLocalPort) == port) Add(records, row.dwOwningPid, L"UDP");
            });
        break;
    case PortScanSource::UdpIPv6:
        valid = VisitRows<Udp6TableOwnerPidCompat, Udp6RowOwnerPidCompat>(buffer,
            [](const auto& table) { return table.entryCount; }, [&](const auto& row) {
                if (NetworkOrderPort(row.localPort) == port) Add(records, row.owningPid, L"UDP6");
            });
        break;
    }
    if (!valid) {
        result.errorCode = ERROR_INVALID_DATA;
        return result;
    }
    for (auto& [pid, record] : records) {
        (void)pid;
        result.records.push_back(std::move(record));
    }
    return result;
}
}

PortScanService::PortScanService(PortScanOperations operations)
    : operations_(std::move(operations)) {
    if (!operations_.querySource) operations_.querySource = QuerySource;
}

PortScanResult PortScanService::Scan(unsigned short port) const {
    ScanTaskOptions options;
    options.mode = ScanExecutionMode::CallerSingle;
    return ScanExecutionService::Run<PortScanResult>(options,
        [service = *this, port](ScanTaskContext& context) { return service.ScanCore(port, context); });
}

std::shared_ptr<ScanTaskHandle> PortScanService::StartScan(
    unsigned short port,
    std::function<void()> completionCallback) const {
    ScanTaskOptions options;
    options.mode = ScanExecutionMode::BackgroundSingle;
    options.completionCallback = std::move(completionCallback);
    return ScanExecutionService::StartTyped<PortScanResult>(options,
        [service = *this, port](ScanTaskContext& context) { return service.ScanCore(port, context); });
}

PortScanResult PortScanService::ScanCore(unsigned short port, ScanTaskContext& context) const {
    PortScanResult result;
    result.port = port;
    context.Report(ScanProgressUpdate{
        L"port-tables", L"端口扫描", L"正在读取网络连接", L"端口 " + std::to_wstring(port)});
    std::map<DWORD, PortScanRecord> records;

    constexpr std::array sources{
        PortScanSource::TcpIPv4,
        PortScanSource::TcpIPv6,
        PortScanSource::UdpIPv4,
        PortScanSource::UdpIPv6,
    };
    std::size_t completedSources = 0;
    for (PortScanSource source : sources) {
        if (context.StopRequested()) break;
        PortScanSourceResult sourceResult = operations_.querySource(source, port);
        if (context.StopRequested()) break;
        if (sourceResult.errorCode != NO_ERROR) {
            result.failedSources.push_back(source);
            continue;
        }
        ++completedSources;
        for (auto& record : sourceResult.records) {
            for (auto& endpoint : record.endpoints) {
                Add(records, record.processId, std::move(endpoint));
            }
        }
    }

    for (auto& [pid, record] : records) {
        (void)pid;
        result.records.push_back(std::move(record));
    }
    if (!context.StopRequested() && completedSources == 0 && result.failedSources.size() == sources.size()) {
        result.error = L"无法读取网络连接信息。";
    } else if (!context.StopRequested() && !result.failedSources.empty()) {
        result.warning = L"部分网络连接信息读取失败，结果可能不完整。";
    }
    context.UpdateProgress([&result](ScanProgressUpdate& value) {
        value.status = L"扫描完成";
        value.detail = result.error.empty()
            ? L"发现 " + std::to_wstring(result.records.size()) + L" 个占用进程"
            : result.error;
        value.discovered = result.records.size();
    });
    return result;
}
