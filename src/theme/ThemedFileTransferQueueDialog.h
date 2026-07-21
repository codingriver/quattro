#pragma once

#include "Theme.h"
#include "ThemedUi.h"

#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

class ThemedWindowUi;

enum class ThemedFileTransferStatus {
    Waiting,
    Preparing,
    UploadingMeta,
    Uploading,
    Confirming,
    Downloading,
    Verifying,
    UploadCompleted,
    DownloadCompleted,
    UploadFailed,
    DownloadFailed,
    Stopped,
};

struct ThemedFileTransferRow {
    std::uint64_t id = 0;
    std::wstring fileName;
    std::wstring absolutePath;
    std::uint64_t size = 0;
    ThemedFileTransferStatus status = ThemedFileTransferStatus::Waiting;
};

struct ThemedFileTransferQueueSnapshot {
    std::wstring title;
    std::wstring status;
    std::wstring detail;
    double progress = 0.0;
    std::vector<ThemedFileTransferRow> rows;
    bool running = false;
    bool stopRequested = false;
};

struct ThemedFileTransferQueueDialogOptions {
    HWND owner = nullptr;
    HINSTANCE instance = nullptr;
    Theme theme;
    HICON icon = nullptr;
    std::wstring className;
    std::wstring title = L"文件传输";
    unsigned int maxConcurrentTransfers = 1;
    std::function<ThemedFileTransferQueueSnapshot()> readSnapshot;
    std::function<void()> requestStopAll;
};

class ThemedFileTransferQueueDialog final {
public:
    explicit ThemedFileTransferQueueDialog(ThemedFileTransferQueueDialogOptions options);
    ~ThemedFileTransferQueueDialog();

    bool Show();
    void Hide();
    void Close();
    void NotifyChanged();
    bool IsOpen() const;
    bool IsVisible() const;
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
    void CreateControls();
    void LayoutControls();
    void Refresh();

    ThemedFileTransferQueueDialogOptions options_;
    HWND hwnd_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    HWND status_ = nullptr;
    HWND detail_ = nullptr;
    HWND progress_ = nullptr;
    HWND table_ = nullptr;
    HWND stop_ = nullptr;
    HWND close_ = nullptr;
    bool hasSnapshot_ = false;
    bool stopEnabled_ = true;
    ThemedFileTransferQueueSnapshot lastSnapshot_{};
    std::atomic_bool changePosted_ = false;
};
