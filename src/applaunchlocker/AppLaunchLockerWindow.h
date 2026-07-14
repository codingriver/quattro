#pragma once

#include "AppLaunchLockerCore.h"
#include "Theme.h"

#include <windows.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

class ThemedWindowUi;

class AppLaunchLockerWindow {
public:
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
    void RebuildCategories();
    void RebuildRows();
    void UpdateButtons();
    void ShowSelectedDetails();
    void SelectCategory(int index);
    const StartupItem* SelectedStartupItem() const;
    const DisabledRecord* SelectedDisabledRecord() const;
    void JoinWorker();

    enum class CategoryKind {
        Current,
        Disabled,
        Source,
    };

    struct CategoryEntry {
        CategoryKind kind = CategoryKind::Current;
        StartupSourceType source = StartupSourceType::Registry;
        std::wstring title;
        int count = 0;
    };

    HINSTANCE instance_ = nullptr;
    Theme theme_;
    HWND hwnd_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    HWND categoryTable_ = nullptr;
    HWND itemTable_ = nullptr;
    HWND statusText_ = nullptr;
    HWND elevateLink_ = nullptr;
    HWND detailsButton_ = nullptr;
    HWND disableButton_ = nullptr;
    HWND restoreButton_ = nullptr;
    std::vector<CategoryEntry> categories_;
    std::vector<StartupItem> items_;
    std::vector<std::size_t> visibleItemIndexes_;
    std::vector<std::size_t> visibleDisabledIndexes_;
    std::vector<DisabledRecord> disabled_;
    std::thread worker_;
    std::atomic<bool> closing_{false};
    bool busy_ = false;
    bool storeAvailable_ = true;
    bool showElevateLink_ = false;
    int selectedCategory_ = 0;
};
