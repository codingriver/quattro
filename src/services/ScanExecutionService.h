#pragma once

#include "TaskExecutionService.h"

// Compatibility facade for scan-domain call sites. New generic background
// work should use TaskExecutionService and the Task* types directly.
using ScanExecutionMode = TaskExecutionMode;
using ScanTaskStatus = TaskStatus;
using ScanTaskOptions = TaskOptions;
using ScanProgressUpdate = TaskProgressUpdate;
using ScanProgressSnapshot = TaskProgressSnapshot;
using ScanTaskHandle = TaskHandle;
using ScanTaskContext = TaskContext;
using ScanExecutionService = TaskExecutionService;
