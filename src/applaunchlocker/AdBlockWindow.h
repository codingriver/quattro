#pragma once

#include "AppLaunchLockerCore.h"
#include "Theme.h"
#include "ThemedUi.h"

#include <windows.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

class ThemedWindowUi;
class ThemedTaskProgressDialog;
struct AdBlockScanState;

// 广告拦截（简化版）窗口：选文件/文件夹 → 扫描可启动文件 → 勾选 → 一键拦截；
// 「已拦截」页可解除。与「自启动管理」窗口独立，机制为 IFEO 禁止运行。
class AdBlockWindow {
public:
    AdBlockWindow(HINSTANCE instance, Theme theme);
    ~AdBlockWindow();

    int Run();

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
    void CreateControls();
    void PickFile();
    void PickFolder();
    void StartScan();
    void ClearScanResults();
    void StartBlockSelected();
    void StartUnblock();
    void LoadBlockedAsync();
    void CompleteScan(AdBlockScanResult scan);
    void CompleteBlocked(std::vector<DisabledRecord> blocked, std::wstring storeError);
    void CompleteOperation(OperationResult result);
    void SelectTab(int index);
    void RebuildScanRows();
    void RebuildBlockedRows();
    void UpdateButtons();
    std::wstring SelectedMode() const;
    void JoinWorker();

    HINSTANCE instance_ = nullptr;
    Theme theme_;
    HWND hwnd_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;

    HWND tabControl_ = nullptr;
    HWND contentPanel_ = nullptr;
    HWND pathEdit_ = nullptr;
    ThemedSplitButton pickPathSplit_{};
    HWND clearButton_ = nullptr;
    HWND checkButton_ = nullptr;
    HWND modeExactRadio_ = nullptr;
    HWND modeNameRadio_ = nullptr;
    HWND modeStartupRadio_ = nullptr;
    HWND scanTable_ = nullptr;
    HWND blockedTable_ = nullptr;
    HWND statusText_ = nullptr;
    HWND blockButton_ = nullptr;
    HWND unblockButton_ = nullptr;

    std::vector<StartupItem> scanItems_;
    std::vector<DisabledRecord> blocked_;
    std::thread worker_;
    std::shared_ptr<AdBlockScanState> scanState_;
    std::unique_ptr<ThemedTaskProgressDialog> scanProgressDialog_;
    std::atomic<bool> closing_{false};
    bool busy_ = false;
    bool scanRunning_ = false;
    bool testConfirmationShown_ = false;
    bool storeAvailable_ = true;
    int activeTab_ = 0;
};
