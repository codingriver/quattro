#pragma once

#include "AppLaunchLockerCore.h"
#include "Theme.h"

#include <windows.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

class ThemedWindowUi;
class ThemedTaskProgressDialog;
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
    void CompleteScan(ScanResult result, std::vector<DisabledRecord> disabled, std::wstring storeError);
    void CompleteOperation(OperationResult result);
    void RebuildTabs();
    void RebuildRows();
    void UpdateButtons();
    void ShowSelectedDetails();
    void SelectTab(int index);
    const StartupItem* SelectedStartupItem() const;
    const DisabledRecord* SelectedDisabledRecord() const;
    void JoinWorker();

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
    HWND elevateLink_ = nullptr;
    HWND detailsButton_ = nullptr;
    HWND disableButton_ = nullptr;
    HWND restoreButton_ = nullptr;
    std::vector<TabEntry> tabs_;
    std::vector<StartupItem> items_;
    std::vector<std::size_t> visibleItemIndexes_;
    std::vector<std::size_t> visibleDisabledIndexes_;
    std::vector<DisabledRecord> disabled_;
    std::thread worker_;
    std::shared_ptr<ScanTaskHandle> scanTask_;
    std::unique_ptr<ThemedTaskProgressDialog> scanProgressDialog_;
    std::atomic<bool> closing_{false};
    bool busy_ = false;
    bool storeAvailable_ = true;
    bool showElevateLink_ = false;
    int activeTab_ = 0;
};
