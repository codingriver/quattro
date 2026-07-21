#include "WebDavTransferProgressController.h"

#include "ThemedTaskProgressDialog.h"
#include "Utilities.h"
#include "../../resources/resource.h"

#include <algorithm>
#include <chrono>

namespace {
constexpr UINT WM_WEBDAV_TRANSFER_DONE = WM_APP + 0xC7;

std::wstring PhaseStatus(WebDavFileTransferPhase phase, bool upload) {
    switch (phase) {
    case WebDavFileTransferPhase::Preparing: return upload ? L"正在准备（1 / 4）" : L"正在准备下载";
    case WebDavFileTransferPhase::UploadingMeta: return L"正在上传 META（2 / 4）";
    case WebDavFileTransferPhase::UploadingContent: return L"正在上传（3 / 4）";
    case WebDavFileTransferPhase::FinalizingMeta: return L"正在确认（4 / 4）";
    case WebDavFileTransferPhase::DownloadingContent: return L"步骤 1 / 2：下载文件内容";
    case WebDavFileTransferPhase::Verifying: return L"步骤 2 / 2：校验并替换文件";
    }
    return L"正在处理";
}

std::wstring BytesText(std::uint64_t current, std::uint64_t total) {
    return FormatByteSizeForDisplay(current) + L" / " + FormatByteSizeForDisplay(total);
}

double UploadPhaseFraction(
    WebDavFileTransferPhase phase,
    std::uint64_t transferred,
    std::uint64_t total) {
    switch (phase) {
    case WebDavFileTransferPhase::Preparing:
        return 0.05;
    case WebDavFileTransferPhase::UploadingMeta: {
        const double meta = total > 0
            ? std::clamp(static_cast<double>(transferred) / static_cast<double>(total), 0.0, 1.0)
            : 0.5;
        return 0.10 + meta * 0.10;
    }
    case WebDavFileTransferPhase::UploadingContent: {
        const double content = total > 0
            ? std::clamp(static_cast<double>(transferred) / static_cast<double>(total), 0.0, 1.0)
            : 0.0;
        return 0.20 + content * 0.75;
    }
    case WebDavFileTransferPhase::FinalizingMeta: {
        const double confirm = total > 0
            ? std::clamp(static_cast<double>(transferred) / static_cast<double>(total), 0.0, 1.0)
            : 0.5;
        return 0.95 + confirm * 0.05;
    }
    default:
        return 0.0;
    }
}
}

WebDavTransferProgressController::WebDavTransferProgressController(
    HWND owner, HINSTANCE instance, const Theme& theme, AppConfig config)
    : owner_(owner), instance_(instance), theme_(theme), config_(std::move(config)) {}

WebDavTransferProgressController::~WebDavTransferProgressController() {
    RequestStop();
    JoinWorker();
    if (progressDialog_) progressDialog_->Close();
}

bool WebDavTransferProgressController::StartUpload(std::vector<std::filesystem::path> paths) {
    if (IsRunning() || paths.empty()) return false;
    uploadPaths_ = std::move(paths);
    uploadMode_ = true;
    downloadMode_ = false;
    {
        std::lock_guard lock(mutex_);
        state_ = {};
        state_.fileCount = uploadPaths_.size();
        for (const auto& path : uploadPaths_) {
            std::error_code ec;
            state_.batchTotal += std::filesystem::file_size(path, ec);
        }
    }
    StartWorker();
    return true;
}

bool WebDavTransferProgressController::StartDownload(WebDavFileRecord record) {
    if (IsRunning()) return false;
    downloadRecord_ = std::move(record);
    uploadMode_ = false;
    downloadMode_ = true;
    {
        std::lock_guard lock(mutex_);
        state_ = {};
        state_.fileCount = 1;
        state_.currentFileName = downloadRecord_->displayName;
        state_.batchTotal = downloadRecord_->size;
        state_.currentTotal = downloadRecord_->size;
    }
    StartWorker();
    return true;
}

void WebDavTransferProgressController::ShowProgress() {
    if (!progressDialog_) {
        ThemedTaskProgressDialogOptions options{};
        options.owner = owner_;
        options.instance = instance_;
        options.theme = theme_;
        options.icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        options.className = L"QuattroWebDavTransferProgress_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        options.title = uploadMode_ ? L"上传到 WebDAV" : L"从 WebDAV 下载";
        options.initialStatus = uploadMode_ ? L"正在准备上传" : L"正在准备下载";
        options.initialDetail = L"正在读取文件信息。";
        options.stopText = L"停止";
        options.closeText = L"关闭";
        options.clientWidth = 560;
        options.clientHeight = 180;
        options.readSnapshot = [this]() { return Snapshot(); };
        options.requestStop = [this]() { RequestStop(); };
        progressDialog_ = std::make_unique<ThemedTaskProgressDialog>(std::move(options));
    }
    progressDialog_->Show();
}

void WebDavTransferProgressController::RequestStop() {
    std::lock_guard lock(mutex_);
    state_.stopRequested = true;
}

bool WebDavTransferProgressController::IsRunning() const {
    std::lock_guard lock(mutex_);
    return worker_.joinable() && !state_.finished;
}

int WebDavTransferProgressController::RunUntilFinished() {
    messageThreadId_ = GetCurrentThreadId();
    ShowProgress();
    MSG message{};
    while (IsRunning() || (progressDialog_ && progressDialog_->IsOpen())) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) break;
        if (message.message == WM_WEBDAV_TRANSFER_DONE) {
            if (progressDialog_ && progressDialog_->hwnd()) {
                InvalidateRect(progressDialog_->hwnd(), nullptr, FALSE);
            }
            continue;
        }
        if (!ThemedUi::PreTranslateMessage(message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    JoinWorker();
    return failed_ > 0 ? 2 : 0;
}

void WebDavTransferProgressController::StartWorker() {
    worker_ = std::jthread([this](std::stop_token stopToken) {
        WebDavFileService service(config_);
        if (uploadMode_) {
            for (std::size_t index = 0; index < uploadPaths_.size(); ++index) {
                std::error_code sizeError;
                const std::uint64_t currentFileSize = std::filesystem::file_size(uploadPaths_[index], sizeError);
                {
                    std::lock_guard lock(mutex_);
                    state_.currentFileIndex = index;
                    state_.currentFileName = uploadPaths_[index].filename().wstring();
                    state_.currentBytes = 0;
                    state_.currentTotal = sizeError ? 0 : currentFileSize;
                    state_.currentFileSize = sizeError ? 0 : currentFileSize;
                    state_.phase = WebDavFileTransferPhase::Preparing;
                    state_.batchBytes = state_.completedBytes;
                }
                const auto result = service.Upload(uploadPaths_[index],
                    [this, index](WebDavFileTransferPhase phase, std::uint64_t transferred, std::uint64_t total) {
                        UpdatePhase(phase, transferred, total);
                        std::lock_guard lock(mutex_);
                        return !state_.stopRequested;
                    }, stopToken);
                std::lock_guard lock(mutex_);
                if (result.ok) {
                    ++state_.succeeded;
                    state_.completedBytes += sizeError ? 0 : currentFileSize;
                    state_.batchBytes = state_.completedBytes;
                } else {
                    ++state_.failed;
                    state_.lastError = result.message;
                }
                if (state_.stopRequested) break;
            }
        } else if (downloadMode_ && downloadRecord_) {
            {
                std::lock_guard lock(mutex_);
                state_.phase = WebDavFileTransferPhase::Preparing;
                state_.currentFileName = downloadRecord_->displayName;
            }
            downloadResult_ = service.Download(*downloadRecord_,
                [this](WebDavFileTransferPhase phase, std::uint64_t transferred, std::uint64_t total) {
                    UpdatePhase(phase, transferred, total);
                    std::lock_guard lock(mutex_);
                    return !state_.stopRequested;
                }, stopToken);
            std::lock_guard lock(mutex_);
            if (downloadResult_.ok) ++state_.succeeded; else { ++state_.failed; state_.lastError = downloadResult_.message; }
        }
        {
            std::lock_guard lock(mutex_);
            succeeded_ = state_.succeeded;
            failed_ = state_.failed;
            state_.finished = true;
        }
        PostFinished();
    });
}

void WebDavTransferProgressController::UpdatePhase(
    WebDavFileTransferPhase phase, std::uint64_t transferred, std::uint64_t total) {
    std::lock_guard lock(mutex_);
    state_.phase = phase;
    state_.currentBytes = transferred;
    state_.currentTotal = total;
    if (phase == WebDavFileTransferPhase::UploadingContent || phase == WebDavFileTransferPhase::DownloadingContent) {
        state_.batchBytes = state_.completedBytes + transferred;
    }
}

ThemedTaskProgressSnapshot WebDavTransferProgressController::Snapshot() const {
    std::lock_guard lock(mutex_);
    ThemedTaskProgressSnapshot snapshot{};
    const bool upload = uploadMode_;
    if (upload && state_.fileCount > 0) {
        snapshot.title = L"上传文件（" + std::to_wstring(state_.currentFileIndex + 1) + L" / " +
            std::to_wstring(state_.fileCount) + L"）";
        if (!state_.currentFileName.empty()) snapshot.title += L" · " + state_.currentFileName;
    } else if (!upload && !state_.currentFileName.empty()) {
        snapshot.title = L"下载文件 · " + state_.currentFileName;
    }
    if (state_.finished) {
        if (state_.stopRequested) { snapshot.status = L"已停止"; snapshot.role = ThemedStatusRole::Warning; }
        else if (state_.failed > 0) { snapshot.status = upload ? L"上传部分失败" : L"下载失败"; snapshot.role = ThemedStatusRole::Danger; }
        else { snapshot.status = upload ? L"上传完成" : L"下载完成"; snapshot.role = ThemedStatusRole::Success; }
        snapshot.detail = L"成功 " + std::to_wstring(state_.succeeded) + L" 个，失败 " + std::to_wstring(state_.failed) + L" 个";
        if (!state_.lastError.empty()) snapshot.detail += L" · " + state_.lastError;
        snapshot.finished = true;
        if (state_.failed == 0 && !state_.stopRequested) {
            snapshot.value = 1.0;
        } else if (upload) {
            const double phase = UploadPhaseFraction(state_.phase, state_.currentBytes, state_.currentTotal);
            snapshot.value = state_.batchTotal > 0
                ? std::clamp(
                    (static_cast<double>(state_.completedBytes) +
                        static_cast<double>(state_.currentFileSize) * phase) /
                        static_cast<double>(state_.batchTotal),
                    0.0, 1.0)
                : std::clamp(
                    (static_cast<double>(state_.currentFileIndex) + phase) /
                        static_cast<double>(state_.fileCount),
                    0.0, 1.0);
        } else {
            snapshot.value = state_.currentTotal > 0
                ? std::clamp(static_cast<double>(state_.currentBytes) /
                    static_cast<double>(state_.currentTotal), 0.0, 1.0)
                : 0.0;
        }
        snapshot.indeterminate = false;
        return snapshot;
    }
    snapshot.status = PhaseStatus(state_.phase, upload);
    snapshot.detail = state_.currentFileName;
    if (upload && state_.currentFileSize > 0) {
        snapshot.detail += L" · " + FormatByteSizeForDisplay(state_.currentFileSize);
    } else if (!upload && state_.currentTotal > 0) {
        snapshot.detail += L" · " + BytesText(state_.currentBytes, state_.currentTotal);
    }
    if (upload) {
        const double phase = UploadPhaseFraction(state_.phase, state_.currentBytes, state_.currentTotal);
        if (state_.batchTotal > 0) {
            snapshot.value = std::clamp(
                (static_cast<double>(state_.completedBytes) +
                    static_cast<double>(state_.currentFileSize) * phase) /
                    static_cast<double>(state_.batchTotal),
                0.0, 1.0);
        } else if (state_.fileCount > 0) {
            snapshot.value = std::clamp(
                (static_cast<double>(state_.currentFileIndex) + phase) /
                    static_cast<double>(state_.fileCount),
                0.0, 1.0);
        }
        snapshot.indeterminate = false;
    } else {
        snapshot.value = state_.batchTotal > 0
            ? static_cast<double>(state_.batchBytes) / static_cast<double>(state_.batchTotal)
            : 0.0;
        snapshot.indeterminate = state_.phase != WebDavFileTransferPhase::DownloadingContent;
    }
    snapshot.stopRequested = state_.stopRequested;
    return snapshot;
}

void WebDavTransferProgressController::PostFinished() {
    if (progressDialog_ && progressDialog_->hwnd()) PostMessageW(progressDialog_->hwnd(), WM_WEBDAV_TRANSFER_DONE, 0, 0);
    if (messageThreadId_) PostThreadMessageW(messageThreadId_, WM_WEBDAV_TRANSFER_DONE, 0, 0);
}

void WebDavTransferProgressController::JoinWorker() {
    if (worker_.joinable()) worker_.join();
}
