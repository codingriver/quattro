#pragma once

#include "Theme.h"
#include "ThemedUi.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

class ThemedWindowUi;

struct ThemedTaskProgressSnapshot {
    std::wstring title;
    std::wstring status;
    std::wstring detail;
    ThemedStatusRole role = ThemedStatusRole::Info;
    double value = 0.0;
    bool indeterminate = true;
    bool finished = false;
    bool stopRequested = false;
};

struct ThemedTaskProgressDialogOptions {
    HWND owner = nullptr;
    HINSTANCE instance = nullptr;
    Theme theme;
    HICON icon = nullptr;
    HICON smallIcon = nullptr;
    std::wstring className;
    std::wstring title;
    std::wstring initialStatus = L"正在准备检查…";
    std::wstring initialDetail = L"正在读取路径信息。";
    std::wstring stopText = L"停止";
    std::wstring closeText = L"关闭";
    int progressBarId = 1;
    int stopButtonId = 2;
    int closeButtonId = 3;
    int clientWidth = 420;
    int clientHeight = 164;
    std::function<ThemedTaskProgressSnapshot()> readSnapshot;
    std::function<void()> requestStop;
};

// 无业务依赖的公共后台任务进度窗口。关闭窗口不会停止任务，停止行为由调用方回调决定。
class ThemedTaskProgressDialog final {
public:
    explicit ThemedTaskProgressDialog(ThemedTaskProgressDialogOptions options);
    ~ThemedTaskProgressDialog();

    bool Show();
    void Close();
    bool IsOpen() const;
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
    void CreateControls();
    void Refresh();

    ThemedTaskProgressDialogOptions options_;
    HWND hwnd_ = nullptr;
    std::unique_ptr<ThemedWindowUi> windowUi_;
    HWND status_ = nullptr;
    HWND detail_ = nullptr;
    HWND progress_ = nullptr;
    HWND stop_ = nullptr;
    HWND close_ = nullptr;
    bool stopEnabled_ = true;
};
