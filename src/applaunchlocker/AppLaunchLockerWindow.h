#pragma once

#include "AppLaunchLockerCore.h"
#include "Theme.h"
#include "ThemedUi.h"

#include <windows.h>
#include <commctrl.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

class ThemedWindowUi;
class ThemedTaskProgressDialog;
class TaskHandle;
class AppLaunchLockerWindow {
public:
    enum class MainTab {
        StartupItems,
        Services,
        ScheduledTasks,
        Drivers,
        Advanced,
    };

    AppLaunchLockerWindow(HINSTANCE instance, Theme theme);
    ~AppLaunchLockerWindow();

    int Run();

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
    void CreateControls();
    void StartScan();
    void StartDisable();
    void StartRestore();
    void StartEntryOperation(StartupApplicationEntry entry);
    void ShowContextMenu(POINT screenPoint);
    void CopySelectedStartupInfo();
    void CopySelectedPath();
    void CopySelectedName();
    void CopySelectedCommand();
    void CopySelectedSourceField(const wchar_t* key, const std::wstring& successMessage);
    void OpenSelectedLocation();
    void ShowSelectedFileProperties();
    void CompleteScan(ScanResult result, std::vector<DisabledRecord> disabled, std::wstring storeError);
    void CompleteOperation(OperationResult result);
    void RebuildTabs();
    void RebuildRows();
    void DestroyItemImages();
    void StopIconLoadTask();
    bool StartIconLoadTask();
    void ShowPendingRows(HIMAGELIST newImages, const std::map<std::intptr_t, int>& imageIndexes);
    void ApplyIconLoadResult(std::uint64_t generation);
    void UpdateButtons();
    void ShowSelectedDetails();
    void SelectTab(int index);
    std::intptr_t RowKeyForIdentity(const std::wstring& identity);
    const StartupApplication* SelectedApplication() const;
    const StartupItem* SelectedStartupItem() const;
    const DisabledRecord* SelectedDisabledRecord() const;
    void StartOperationTask(std::function<OperationResult()> operation);

    struct TabEntry {
        MainTab tab = MainTab::StartupItems;
        std::wstring title;
        int count = 0;
    };

    HINSTANCE instance_ = nullptr;
    Theme theme_;
    HWND hwnd_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    HWND tabControl_ = nullptr;
    HWND itemTable_ = nullptr;
    HWND statusText_ = nullptr;
    HWND advancedSourceFilter_ = nullptr;
    HWND detailsButton_ = nullptr;
    HWND disableButton_ = nullptr;
    HWND restoreButton_ = nullptr;
    HIMAGELIST itemSmallImages_ = nullptr;
    int itemIconSize_ = 16;
    std::vector<TabEntry> tabs_;
    std::vector<StartupApplication> applications_;
    std::vector<std::size_t> visibleApplicationIndexes_;
    std::vector<std::intptr_t> visibleApplicationRowKeys_;
    std::vector<StartupItem> items_;
    std::vector<std::size_t> visibleItemIndexes_;
    std::vector<std::intptr_t> visibleItemRowKeys_;
    std::vector<std::size_t> visibleDisabledIndexes_;
    std::vector<std::intptr_t> visibleDisabledRowKeys_;
    std::map<std::wstring, std::intptr_t> stableRowKeys_;
    std::intptr_t nextStableRowKey_ = 1;
    std::vector<ThemedTableRow> pendingRows_;
    std::intptr_t pendingSelectedKey_ = 0;
    std::intptr_t pendingTopKey_ = 0;
    MainTab pendingRowsTab_ = MainTab::StartupItems;
    std::vector<DisabledRecord> disabled_;
    std::shared_ptr<ScanTaskHandle> scanTask_;
    std::shared_ptr<TaskHandle> iconTask_;
    std::shared_ptr<TaskHandle> operationTask_;
    std::unique_ptr<ThemedTaskProgressDialog> scanProgressDialog_;
    std::uint64_t iconGeneration_ = 1;
    std::atomic<bool> closing_{false};
    bool busy_ = false;
    bool storeAvailable_ = true;
    bool rowDisplayPending_ = false;
    int activeTab_{};
    int advancedSourceFilterIndex_ = 0;
};
