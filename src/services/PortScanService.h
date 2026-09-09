#pragma once

#include "ScanExecutionService.h"

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

enum class PortScanSource {
    TcpIPv4,
    TcpIPv6,
    UdpIPv4,
    UdpIPv6,
};

struct PortScanRecord {
    unsigned long processId = 0;
    std::set<std::wstring> endpoints;
};

struct PortScanSourceResult {
    std::vector<PortScanRecord> records;
    unsigned long errorCode = 0;
};

using PortScanSourceQuery =
    std::function<PortScanSourceResult(PortScanSource, unsigned short)>;

struct PortScanOperations {
    PortScanSourceQuery querySource;
};

struct PortScanResult {
    unsigned short port = 0;
    std::vector<PortScanRecord> records;
    std::vector<PortScanSource> failedSources;
    std::wstring warning;
    std::wstring error;
};

class PortScanService final {
public:
    explicit PortScanService(PortScanOperations operations = {});

    PortScanResult Scan(unsigned short port) const;
    std::shared_ptr<ScanTaskHandle> StartScan(
        unsigned short port,
        std::function<void()> completionCallback = {}) const;

private:
    PortScanResult ScanCore(unsigned short port, ScanTaskContext& context) const;

    PortScanOperations operations_;
};
