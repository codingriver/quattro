#pragma once

#include "ScanExecutionService.h"

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct PortScanRecord {
    unsigned long processId = 0;
    std::set<std::wstring> endpoints;
};

struct PortScanResult {
    unsigned short port = 0;
    std::vector<PortScanRecord> records;
};

class PortScanService final {
public:
    PortScanResult Scan(unsigned short port) const;
    std::shared_ptr<ScanTaskHandle> StartScan(
        unsigned short port,
        std::function<void()> completionCallback = {}) const;

private:
    PortScanResult ScanCore(unsigned short port, ScanTaskContext& context) const;
};
