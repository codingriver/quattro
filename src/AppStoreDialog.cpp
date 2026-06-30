#include "AppStoreDialog.h"

#include "AppPackageService.h"
#include "AppStoreCredentialService.h"
#include "AppStoreRegistry.h"
#include "AppStoreService.h"
#include "AppTransferService.h"
#include "GitHubReleaseClient.h"
#include "SimpleDialogs.h"
#include "ThemedControls.h"
#include "Utilities.h"

#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <fstream>
#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
constexpr int ID_TAB_STORE = 7101;
constexpr int ID_TAB_DOWNLOADS = 7102;
constexpr int ID_TAB_UPLOADS = 7103;
constexpr int ID_LIST = 7104;
constexpr int ID_REFRESH = 7105;
constexpr int ID_PRIMARY = 7106;
constexpr int ID_PAUSE = 7107;
constexpr int ID_RESUME = 7108;
constexpr int ID_DELETE = 7109;
constexpr int ID_STATUS = 7110;
constexpr UINT_PTR ID_REFRESH_TIMER = 7111;
constexpr UINT WM_TASK_DONE = WM_APP + 71;
constexpr int ID_UPLOAD_NAME = 7201;
constexpr int ID_UPLOAD_APPID = 7202;
constexpr int ID_UPLOAD_VERSION = 7203;
constexpr int ID_UPLOAD_SUMMARY = 7204;
constexpr int ID_UPLOAD_DRAFT = 7205;
constexpr int ID_UPLOAD_CATEGORY = 7207;

constexpr int kDialogWidth = 760;
constexpr int kDialogHeight = 620;
constexpr const wchar_t* kDriveManagerTitle = L"网盘管理";

enum class AppStoreTab {
    Store = 0,
    Downloads,
    Uploads,
};

float ClampFloat(float value, float minValue, float maxValue) {
    return std::max(minValue, std::min(maxValue, value));
}

COLORREF ToColorRef(Color color) {
    const auto byte = [](float value) -> BYTE {
        return static_cast<BYTE>(ClampFloat(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return RGB(byte(color.r), byte(color.g), byte(color.b));
}

std::wstring FormatSpeed(double bytesPerSecond) {
    if (bytesPerSecond <= 0.0) {
        return {};
    }
    const wchar_t* unit = L"B/s";
    double value = bytesPerSecond;
    if (value >= 1024.0 * 1024.0) {
        value /= 1024.0 * 1024.0;
        unit = L"MiB/s";
    } else if (value >= 1024.0) {
        value /= 1024.0;
        unit = L"KiB/s";
    }
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%.1f %s", value, unit);
    return buffer;
}

std::wstring StatusText(const AppTransferTask& task, double bytesPerSecond = 0.0) {
    std::wstring text = task.name.empty() ? task.localPath : task.name;
    if (text.empty()) {
        text = task.id;
    }
    text += L"    " + task.status;
    if (!task.phase.empty()) {
        text += L" / " + task.phase;
    }
    if (task.totalBytes > 0) {
        const int percent = static_cast<int>(std::min<long long>(100, task.transferredBytes * 100 / task.totalBytes));
        text += L"    " + std::to_wstring(percent) + L"%";
    }
    const std::wstring speed = FormatSpeed(bytesPerSecond);
    if (!speed.empty()) {
        text += L"    " + speed;
    }
    if (!task.version.empty()) {
        text += L"    v" + task.version;
    }
    if (!task.errorMessage.empty()) {
        text += L"    " + task.errorMessage;
    }
    return text;
}

std::wstring CategoryLabel(const std::wstring& category) {
    return ToLower(category) == L"other" ? L"其他" : L"App";
}

std::wstring AppText(const AppStoreEntry& entry) {
    const std::wstring displayName = entry.manifest.displayName.empty() ? entry.manifest.name : entry.manifest.displayName;
    std::wstring text = L"[" + CategoryLabel(entry.manifest.category) + L"] " + displayName + L"    v" + entry.manifest.version;
    if (entry.manifest.draft) {
        text += L"    草稿";
    }
    if (entry.manifest.encrypted) {
        text += L"    已加密";
    }
    if (!entry.manifest.summary.empty()) {
        text += L"    " + entry.manifest.summary;
    }
    return text;
}

const AppStoreReleaseAsset* ManifestAsset(const AppStoreRelease& release) {
    for (const auto& asset : release.assets) {
        if (ToLower(asset.name) == L"manifest.json") {
            return &asset;
        }
    }
    return nullptr;
}

std::string WideToUtf8Local(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string output(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), length, nullptr, nullptr);
    return output;
}

bool WriteUtf8FileLocal(const std::filesystem::path& path, const std::wstring& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    const std::string utf8 = WideToUtf8Local(text);
    file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    return file.good();
}

std::wstring JsonEscapeLocal(const std::wstring& value) {
    std::wstring output;
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\\': output += L"\\\\"; break;
        case L'"': output += L"\\\""; break;
        case L'\n': output += L"\\n"; break;
        case L'\r': output += L"\\r"; break;
        case L'\t': output += L"\\t"; break;
        default: output.push_back(ch); break;
        }
    }
    return output;
}

std::wstring SafeUploadAppId(std::wstring value) {
    value = ToLower(Trim(value));
    std::wstring output;
    bool previousDash = false;
    for (wchar_t ch : value) {
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L'_' || ch == L'-') {
            output.push_back(ch);
            previousDash = false;
        } else if (!previousDash) {
            output.push_back(L'-');
            previousDash = true;
        }
    }
    while (!output.empty() && (output.front() == L'-' || output.front() == L'.' || output.front() == L'_')) {
        output.erase(output.begin());
    }
    while (!output.empty() && (output.back() == L'-' || output.back() == L'.' || output.back() == L'_')) {
        output.pop_back();
    }
    return output.empty() ? L"app-" + Hex8(StablePathHash(value)) : output;
}

std::wstring FileNameNoExtension(const std::filesystem::path& path) {
    if (DirectoryExists(path)) {
        const std::wstring folderName = path.filename().wstring();
        return folderName.empty() ? L"App" : folderName;
    }
    std::wstring value = path.stem().wstring();
    if (value.empty()) {
        value = path.filename().wstring();
    }
    return value.empty() ? L"App" : value;
}

struct UploadManifestDraft {
    std::filesystem::path sourcePath;
    std::wstring category = L"app";
    std::wstring sourceKind;
    std::wstring originalName;
    std::wstring appId;
    std::wstring name;
    std::wstring version = L"1.0.0";
    std::wstring summary;
    bool draft = true;
};

std::wstring GeneratedUploadManifest(const UploadManifestDraft& draft) {
    std::wstringstream out;
    out << L"{\n";
    out << L"  \"schema\": 1,\n";
    out << L"  \"appId\": \"" << JsonEscapeLocal(draft.appId) << L"\",\n";
    out << L"  \"name\": \"" << JsonEscapeLocal(draft.name) << L"\",\n";
    out << L"  \"displayName\": \"" << JsonEscapeLocal(draft.name) << L"\",\n";
    out << L"  \"version\": \"" << JsonEscapeLocal(draft.version) << L"\",\n";
    out << L"  \"category\": \"" << JsonEscapeLocal(draft.category.empty() ? L"app" : draft.category) << L"\",\n";
    out << L"  \"sourceKind\": \"" << JsonEscapeLocal(draft.sourceKind) << L"\",\n";
    out << L"  \"originalName\": \"" << JsonEscapeLocal(draft.originalName) << L"\",\n";
    out << L"  \"summary\": \"" << JsonEscapeLocal(draft.summary) << L"\",\n";
    out << L"  \"draft\": " << (draft.draft ? L"true" : L"false") << L"\n";
    out << L"}\n";
    return out.str();
}

UploadManifestDraft DefaultUploadDraft(const std::filesystem::path& source) {
    UploadManifestDraft draft;
    draft.sourcePath = source;
    draft.sourceKind = DirectoryExists(source) ? L"folder" : L"file";
    draft.originalName = source.filename().wstring();
    draft.name = FileNameNoExtension(source);
    draft.appId = SafeUploadAppId(draft.name);
    draft.summary = draft.name;
    draft.category = DirectoryExists(source) ? L"app" : L"other";
    return draft;
}

std::wstring GetWindowTextString(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring value(static_cast<std::size_t>(std::max(0, length)) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, value.data(), length + 1);
    }
    value.resize(static_cast<std::size_t>(std::max(0, length)));
    return value;
}

std::wstring PickUploadFolderPath(HWND owner) {
    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        return {};
    }
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PICKFOLDERS);
    }
    dialog->SetTitle(L"选择应用目录");
    std::wstring path;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR value = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &value)) && value) {
                path = value;
                CoTaskMemFree(value);
            }
            item->Release();
        }
    }
    dialog->Release();
    return path;
}

std::wstring PickUploadFilePath(HWND owner) {
    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        return {};
    }
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    }
    const COMDLG_FILTERSPEC filters[] = {
        {L"所有文件", L"*.*"},
    };
    dialog->SetFileTypes(static_cast<UINT>(sizeof(filters) / sizeof(filters[0])), filters);
    dialog->SetTitle(L"选择要上传的文件");
    std::wstring filePath;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR value = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &value)) && value) {
                filePath = value;
                CoTaskMemFree(value);
            }
            item->Release();
        }
    }
    dialog->Release();
    if (filePath.empty()) {
        return {};
    }
    return filePath;
}

std::wstring PickUploadPath(HWND owner) {
    const int choice = MessageBoxW(
        owner,
        L"请选择上传来源：\n\n是：选择文件夹\n否：选择文件\n取消：返回",
        L"添加上传",
        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (choice == IDYES) {
        return PickUploadFolderPath(owner);
    }
    if (choice == IDNO) {
        return PickUploadFilePath(owner);
    }
    return {};
}

class UploadConfigDialog {
public:
    UploadConfigDialog(HWND owner, HINSTANCE instance, const Theme& theme, UploadManifestDraft& draft)
        : owner_(owner), instance_(instance), theme_(theme), draft_(draft) {}

    bool Run() {
        const std::wstring className = L"QuattroUploadConfigDialog_" + std::to_wstring(GetCurrentProcessId());
        WNDCLASSW wc{};
        wc.lpfnWndProc = UploadConfigDialog::Proc;
        wc.hInstance = instance_;
        wc.lpszClassName = className.c_str();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);

        RECT ownerRect{};
        GetWindowRect(owner_, &ownerRect);
        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
            className.c_str(),
            L"上传应用信息",
            WS_CAPTION | WS_SYSMENU | WS_POPUP,
            ownerRect.left + 70,
            ownerRect.top + 70,
            520,
            360,
            owner_,
            nullptr,
            instance_,
            this);
        if (!hwnd_) {
            return false;
        }
        ownerWasEnabled_ = ShowModalWindow(owner_, hwnd_);
        UpdateWindow(hwnd_);
        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
        return accepted_;
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        UploadConfigDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<UploadConfigDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<UploadConfigDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    HWND AddEdit(int id, const wchar_t* label, int x, int y, int width, const std::wstring& value) {
        ThemedControls::CreateLabelText(instance_, hwnd_, label, x, y, width, theme_, font_);
        RECT frame{x, y + 22, x + width, y + 22 + ThemedControls::EditFrameHeight(theme_)};
        editFrames_.push_back(frame);
        HWND edit = ThemedControls::CreateSingleLineEdit(instance_, hwnd_, id, theme_, frame, value, editFont_ ? editFont_ : font_);
        editControls_.push_back(edit);
        return edit;
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE: {
            font_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            editFont_ = ThemedControls::CreateEditFont(theme_);
            backgroundBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
            editBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"edit", L"normal", L"bg")));
            ThemedControls::CreateLabelText(instance_, hwnd_, (L"来源: " + draft_.sourcePath.wstring()).c_str(), 24, 18, 456, theme_, font_, SS_LEFTNOWORDWRAP);
            nameEdit_ = AddEdit(ID_UPLOAD_NAME, L"名称", 24, 52, 210, draft_.name);
            appIdEdit_ = AddEdit(ID_UPLOAD_APPID, L"App ID", 258, 52, 210, draft_.appId);
            versionEdit_ = AddEdit(ID_UPLOAD_VERSION, L"版本", 24, 124, 110, draft_.version);
            summaryEdit_ = AddEdit(ID_UPLOAD_SUMMARY, L"简介", 158, 124, 310, draft_.summary);
            ThemedControls::CreateLabelText(instance_, hwnd_, L"分类", 24, 196, 110, theme_, font_);
            categoryFrame_ = RECT{24, 218, 148, 218 + ThemedControls::ComboBoxHeight(theme_)};
            categoryCombo_ = ThemedControls::CreateComboBox(instance_, hwnd_, ID_UPLOAD_CATEGORY, categoryFrame_.left, categoryFrame_.top, categoryFrame_.right - categoryFrame_.left, 120, font_, theme_);
            SendMessageW(categoryCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"App"));
            SendMessageW(categoryCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"其他"));
            SendMessageW(categoryCombo_, CB_SETCURSEL, draft_.category == L"other" ? 1 : 0, 0);
            draftCheck_ = ThemedControls::CreateCheckBox(instance_, hwnd_, ID_UPLOAD_DRAFT, L"作为 Draft Release 上传", 170, 222, 190, ThemedControls::CheckBoxHeight(theme_), font_, draft_.draft);
            const int buttonHeight = ThemedControls::ButtonHeight(theme_);
            ThemedControls::CreateButton(instance_, hwnd_, IDOK, L"添加", 324, 274, 72, buttonHeight, font_, true);
            ThemedControls::CreateButton(instance_, hwnd_, IDCANCEL, L"取消", 412, 274, 72, buttonHeight, font_);
            SetFocus(nameEdit_);
            SendMessageW(nameEdit_, EM_SETSEL, 0, -1);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            RECT rect{};
            GetClientRect(hwnd_, &rect);
            FillRect(dc, &rect, backgroundBrush_ ? backgroundBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            for (std::size_t i = 0; i < editFrames_.size(); ++i) {
                ThemedControls::DrawFieldFrame(theme_, dc, editFrames_[i], i < editControls_.size() ? editControls_[i] : nullptr);
            }
            ThemedControls::DrawComboFrame(theme_, dc, categoryFrame_, categoryCombo_);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, ToColorRef(theme_.color(L"edit", L"normal", L"text")));
            SetBkColor(dc, ToColorRef(theme_.color(L"edit", L"normal", L"bg")));
            return reinterpret_cast<LRESULT>(editBrush_ ? editBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, ToColorRef(theme_.color(L"label", L"normal", L"text")));
            return reinterpret_cast<LRESULT>(backgroundBrush_ ? backgroundBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_DRAWITEM:
            if (ThemedControls::Draw(theme_, reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
                return TRUE;
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                return Accept();
            }
            if (LOWORD(wParam) == IDCANCEL) {
                DestroyWindow(hwnd_);
                return 0;
            }
            return 0;
        case WM_DESTROY:
            done_ = true;
            if (editFont_) {
                DeleteObject(editFont_);
            }
            if (backgroundBrush_) {
                DeleteObject(backgroundBrush_);
            }
            if (editBrush_) {
                DeleteObject(editBrush_);
            }
            RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    LRESULT Accept() {
        draft_.name = Trim(GetWindowTextString(nameEdit_));
        draft_.appId = SafeUploadAppId(GetWindowTextString(appIdEdit_));
        draft_.version = Trim(GetWindowTextString(versionEdit_));
        draft_.summary = Trim(GetWindowTextString(summaryEdit_));
        draft_.category = SendMessageW(categoryCombo_, CB_GETCURSEL, 0, 0) == 1 ? L"other" : L"app";
        draft_.draft = SendMessageW(draftCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (draft_.name.empty() || draft_.appId.empty() || draft_.version.empty()) {
            MessageBoxW(hwnd_, L"名称、App ID、版本不能为空。", L"上传应用信息", MB_OK | MB_ICONWARNING);
            return 0;
        }
        accepted_ = true;
        DestroyWindow(hwnd_);
        return 0;
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    UploadManifestDraft& draft_;
    HWND hwnd_ = nullptr;
    HWND nameEdit_ = nullptr;
    HWND appIdEdit_ = nullptr;
    HWND versionEdit_ = nullptr;
    HWND summaryEdit_ = nullptr;
    HWND categoryCombo_ = nullptr;
    HWND draftCheck_ = nullptr;
    HFONT font_ = nullptr;
    HFONT editFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    RECT categoryFrame_{};
    std::vector<RECT> editFrames_;
    std::vector<HWND> editControls_;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
    bool done_ = false;
    bool accepted_ = false;
};

bool CreateUploadStaging(const std::filesystem::path& appDirectory, const UploadManifestDraft& draft, std::filesystem::path& stagingPath, std::wstring& error) {
    error.clear();
    if (!FileExists(draft.sourcePath) && !DirectoryExists(draft.sourcePath)) {
        error = L"上传来源不存在。";
        return false;
    }
    stagingPath = appDirectory / L"app-store" / L"upload-staging" /
        (draft.appId + L"-" + Hex8(StablePathHash(draft.sourcePath.wstring() + std::to_wstring(GetTickCount64()))));
    std::error_code ec;
    std::filesystem::remove_all(stagingPath, ec);
    std::filesystem::create_directories(stagingPath, ec);
    if (ec) {
        error = L"创建上传暂存目录失败。";
        return false;
    }
    if (FileExists(draft.sourcePath)) {
        std::filesystem::copy_file(draft.sourcePath, stagingPath / draft.sourcePath.filename(), std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            error = L"复制上传文件失败。";
            return false;
        }
    } else {
        for (const auto& entry : std::filesystem::directory_iterator(draft.sourcePath, ec)) {
            if (ec) {
                error = L"读取上传目录失败。";
                return false;
            }
            const std::filesystem::path target = stagingPath / entry.path().filename();
            std::filesystem::copy(entry.path(), target, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                error = L"复制上传目录内容失败。";
                return false;
            }
        }
    }
    if (!WriteUtf8FileLocal(stagingPath / L"manifest.json", GeneratedUploadManifest(draft))) {
        error = L"生成上传 manifest.json 失败。";
        return false;
    }
    return true;
}

class AppStoreDialog {
public:
    AppStoreDialog(HWND owner, HINSTANCE instance, const Theme& theme, std::filesystem::path appDirectory, AppConfig config)
        : owner_(owner),
          instance_(instance),
          theme_(theme),
          appDirectory_(std::move(appDirectory)),
          config_(std::move(config)) {}

    bool Run() {
        const std::wstring className = L"QuattroAppStoreDialog_" + std::to_wstring(GetCurrentProcessId());
        WNDCLASSW wc{};
        wc.lpfnWndProc = AppStoreDialog::WndProcSetup;
        wc.hInstance = instance_;
        wc.lpszClassName = className.c_str();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);

        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME,
            className.c_str(),
            kDriveManagerTitle,
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            kDialogWidth,
            kDialogHeight,
            owner_,
            nullptr,
            instance_,
            this);
        if (!hwnd_) {
            RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
            return false;
        }
        ownerWasEnabled_ = ShowModalWindow(owner_, hwnd_);
        UpdateWindow(hwnd_);
        AppStoreRegistry registry(appDirectory_);
        registry.Initialize();
        registry.MarkInterruptedTasksPaused();
        RefreshStore(false);
        LoadCurrentTab();

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
        return true;
    }

private:
    static LRESULT CALLBACK WndProcSetup(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* self = static_cast<AppStoreDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(AppStoreDialog::WndProcThunk));
            return self->WndProc(message, wParam, lParam);
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<AppStoreDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return self ? self->WndProc(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            font_ = ThemedControls::CreateDialogFont();
            backgroundBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
            fieldBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"list", L"normal", L"bg")));
            CreateControls();
            return 0;
        case WM_SIZE:
            LayoutControls();
            return 0;
        case WM_PAINT:
            Paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, ToColorRef(theme_.color(L"label", L"normal", L"text")));
            HWND child = reinterpret_cast<HWND>(lParam);
            if (child == list_ && fieldBrush_) {
                SetBkColor(dc, ToColorRef(theme_.color(L"list", L"normal", L"bg")));
                return reinterpret_cast<LRESULT>(fieldBrush_);
            }
            return reinterpret_cast<LRESULT>(backgroundBrush_);
        }
        case WM_DRAWITEM:
            if (ThemedControls::Draw(theme_, reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
                return TRUE;
            }
            return 0;
        case WM_COMMAND:
            return HandleCommand(LOWORD(wParam));
        case WM_TIMER:
            if (wParam == ID_REFRESH_TIMER && tab_ != AppStoreTab::Store) {
                LoadCurrentTab();
                return 0;
            }
            return 0;
        case WM_TASK_DONE: {
            activeTask_.clear();
            auto* messageText = reinterpret_cast<wchar_t*>(lParam);
            if (messageText) {
                SetWindowTextW(status_, messageText);
                delete[] messageText;
            }
            LoadCurrentTab();
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            done_ = true;
            RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
            if (font_) {
                DeleteObject(font_);
            }
            if (backgroundBrush_) {
                DeleteObject(backgroundBrush_);
            }
            if (fieldBrush_) {
                DeleteObject(fieldBrush_);
            }
            KillTimer(hwnd_, ID_REFRESH_TIMER);
            if (worker_.joinable()) {
                worker_.detach();
            }
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    void CreateControls() {
        const int tabHeight = ThemedControls::TabButtonHeight(theme_);
        tabStore_ = ThemedControls::CreateTabButton(instance_, hwnd_, ID_TAB_STORE, L"网盘文件", 24, 18, 90, tabHeight, font_, true);
        tabDownloads_ = ThemedControls::CreateTabButton(instance_, hwnd_, ID_TAB_DOWNLOADS, L"下载队列", 114, 18, 90, tabHeight, font_, false);
        tabUploads_ = ThemedControls::CreateTabButton(instance_, hwnd_, ID_TAB_UPLOADS, L"上传队列", 204, 18, 90, tabHeight, font_, false);
        status_ = ThemedControls::CreateStaticText(instance_, hwnd_, L"", 24, 48, 500, 22, font_, SS_LEFT | SS_CENTERIMAGE);
        list_ = CreateWindowExW(
            0,
            L"LISTBOX",
            nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL,
            24,
            78,
            700,
            400,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LIST)),
            instance_,
            nullptr);
        SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);

        const int buttonHeight = ThemedControls::ButtonHeight(theme_);
        refresh_ = ThemedControls::CreateButton(instance_, hwnd_, ID_REFRESH, L"刷新", 24, 520, 76, buttonHeight, font_);
        primary_ = ThemedControls::CreateButton(instance_, hwnd_, ID_PRIMARY, L"下载", 112, 520, 88, buttonHeight, font_);
        pause_ = ThemedControls::CreateButton(instance_, hwnd_, ID_PAUSE, L"暂停", 212, 520, 76, buttonHeight, font_);
        resume_ = ThemedControls::CreateButton(instance_, hwnd_, ID_RESUME, L"继续", 300, 520, 76, buttonHeight, font_);
        delete_ = ThemedControls::CreateButton(instance_, hwnd_, ID_DELETE, L"删除", 388, 520, 76, buttonHeight, font_);
        close_ = ThemedControls::CreateButton(instance_, hwnd_, IDCANCEL, L"关闭", 640, 520, 76, buttonHeight, font_);
        SetTimer(hwnd_, ID_REFRESH_TIMER, 1000, nullptr);
        LayoutControls();
    }

    void LayoutControls() {
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        const int tabHeight = ThemedControls::TabButtonHeight(theme_);
        MoveWindow(tabStore_, 24, 18, 90, tabHeight, TRUE);
        MoveWindow(tabDownloads_, 114, 18, 90, tabHeight, TRUE);
        MoveWindow(tabUploads_, 204, 18, 90, tabHeight, TRUE);
        MoveWindow(status_, 24, 48, std::max(220, width - 48), 22, TRUE);
        listFrame_ = RECT{24, 78, std::max(260, width - 24), std::max(160, height - 74)};
        MoveWindow(list_, listFrame_.left + 1, listFrame_.top + 1, listFrame_.right - listFrame_.left - 2, listFrame_.bottom - listFrame_.top - 2, TRUE);
        const int buttonY = std::max(120, height - 52);
        const int buttonHeight = ThemedControls::ButtonHeight(theme_);
        MoveWindow(refresh_, 24, buttonY, 76, buttonHeight, TRUE);
        MoveWindow(primary_, 112, buttonY, 88, buttonHeight, TRUE);
        MoveWindow(pause_, 212, buttonY, 76, buttonHeight, TRUE);
        MoveWindow(resume_, 300, buttonY, 76, buttonHeight, TRUE);
        MoveWindow(delete_, 388, buttonY, 76, buttonHeight, TRUE);
        MoveWindow(close_, std::max(480, width - 100), buttonY, 76, buttonHeight, TRUE);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void Paint() {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT client{};
        GetClientRect(hwnd_, &client);
        FillRect(dc, &client, backgroundBrush_);
        ThemedControls::DrawListFrame(theme_, dc, listFrame_, list_);
        EndPaint(hwnd_, &ps);
    }

    LRESULT HandleCommand(int id) {
        switch (id) {
        case ID_TAB_STORE:
            SwitchTab(AppStoreTab::Store);
            return 0;
        case ID_TAB_DOWNLOADS:
            SwitchTab(AppStoreTab::Downloads);
            return 0;
        case ID_TAB_UPLOADS:
            SwitchTab(AppStoreTab::Uploads);
            return 0;
        case ID_REFRESH:
            if (tab_ == AppStoreTab::Store) {
                RefreshStore(true);
            }
            LoadCurrentTab();
            return 0;
        case ID_PRIMARY:
            PrimaryAction();
            return 0;
        case ID_PAUSE:
            UpdateSelectedTask(L"paused");
            return 0;
        case ID_RESUME:
            if (tab_ == AppStoreTab::Store) {
                RenameSelectedStoreEntry();
            } else {
                UpdateSelectedTask(L"queued");
            }
            return 0;
        case ID_DELETE:
            DeleteSelectedTask();
            return 0;
        case IDCANCEL:
            DestroyWindow(hwnd_);
            return 0;
        default:
            return 0;
        }
    }

    void SwitchTab(AppStoreTab tab) {
        tab_ = tab;
        SendMessageW(tabStore_, BM_SETCHECK, tab == AppStoreTab::Store ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(tabDownloads_, BM_SETCHECK, tab == AppStoreTab::Downloads ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(tabUploads_, BM_SETCHECK, tab == AppStoreTab::Uploads ? BST_CHECKED : BST_UNCHECKED, 0);
        LoadCurrentTab();
    }

    void RefreshStore(bool showResult) {
        AppStoreService service(appDirectory_, config_);
        if (!service.Refresh()) {
            if (showResult) {
                MessageBoxW(hwnd_, service.lastError().c_str(), kDriveManagerTitle, MB_OK | MB_ICONWARNING);
            }
            SetWindowTextW(status_, service.lastError().c_str());
            return;
        }
        apps_ = service.apps();
        if (showResult) {
            MessageBoxW(hwnd_, L"网盘列表已刷新。", kDriveManagerTitle, MB_OK | MB_ICONINFORMATION);
        }
    }

    void LoadCurrentTab() {
        SendMessageW(list_, LB_RESETCONTENT, 0, 0);
        AppStoreRegistry registry(appDirectory_);
        if (tab_ == AppStoreTab::Store) {
            for (const auto& app : apps_) {
                const std::wstring text = AppText(app);
                SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            }
            SetWindowTextW(status_, (L"文件 " + std::to_wstring(apps_.size()) + L" 个，来源 " + config_.appStoreOwner + L"/" + config_.appStoreRepo).c_str());
            SetWindowTextW(primary_, L"下载");
            SetWindowTextW(resume_, L"重命名");
            EnableWindow(pause_, FALSE);
            EnableWindow(resume_, TRUE);
            EnableWindow(delete_, FALSE);
            return;
        }

        const std::wstring direction = tab_ == AppStoreTab::Downloads ? L"download" : L"upload";
        tasks_ = registry.LoadTasks(direction);
        const ULONGLONG now = GetTickCount64();
        for (const auto& task : tasks_) {
            double speed = 0.0;
            const auto sample = speedSamples_.find(task.id);
            if (sample != speedSamples_.end() && now > sample->second.tick && task.transferredBytes >= sample->second.bytes) {
                const double seconds = static_cast<double>(now - sample->second.tick) / 1000.0;
                if (seconds > 0.0 && (task.status == L"running" || task.status == L"canceling")) {
                    speed = static_cast<double>(task.transferredBytes - sample->second.bytes) / seconds;
                }
            }
            speedSamples_[task.id] = SpeedSample{task.transferredBytes, now};
            const std::wstring text = StatusText(task, speed);
            SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        }
        SetWindowTextW(status_, (std::wstring(tab_ == AppStoreTab::Downloads ? L"下载任务 " : L"上传任务 ") + std::to_wstring(tasks_.size()) + L" 个").c_str());
        SetWindowTextW(primary_, tab_ == AppStoreTab::Downloads ? L"重试" : L"添加上传");
        SetWindowTextW(resume_, L"继续");
        EnableWindow(pause_, TRUE);
        EnableWindow(resume_, TRUE);
        EnableWindow(delete_, TRUE);
    }

    int SelectedIndex() const {
        return static_cast<int>(SendMessageW(list_, LB_GETCURSEL, 0, 0));
    }

    void PrimaryAction() {
        if (tab_ == AppStoreTab::Store) {
            const int index = SelectedIndex();
            if (index < 0 || index >= static_cast<int>(apps_.size())) {
                MessageBoxW(hwnd_, L"请选择一个文件条目。", kDriveManagerTitle, MB_OK | MB_ICONINFORMATION);
                return;
            }
            AppStoreRegistry registry(appDirectory_);
            const long long splitSize = static_cast<long long>(std::max(16, config_.appStoreSplitSizeMiB)) * 1024LL * 1024LL;
            std::wstring taskId;
            if (!registry.AddDownloadTask(apps_[static_cast<std::size_t>(index)], config_.appStoreOwner, config_.appStoreRepo, splitSize, taskId)) {
                MessageBoxW(hwnd_, registry.lastError().c_str(), kDriveManagerTitle, MB_OK | MB_ICONWARNING);
                return;
            }
            RunTaskById(taskId);
            SwitchTab(AppStoreTab::Downloads);
            return;
        }
        if (tab_ == AppStoreTab::Uploads) {
            const std::wstring selectedPath = PickUploadPath(hwnd_);
            if (selectedPath.empty()) {
                return;
            }
            UploadManifestDraft draft = DefaultUploadDraft(std::filesystem::path(selectedPath));
            UploadConfigDialog configDialog(hwnd_, instance_, theme_, draft);
            if (!configDialog.Run()) {
                return;
            }
            std::filesystem::path uploadPath;
            std::wstring stagingError;
            if (!CreateUploadStaging(appDirectory_, draft, uploadPath, stagingError)) {
                MessageBoxW(hwnd_, stagingError.c_str(), kDriveManagerTitle, MB_OK | MB_ICONWARNING);
                return;
            }
            AppStoreRegistry registry(appDirectory_);
            const long long splitSize = static_cast<long long>(std::max(16, config_.appStoreSplitSizeMiB)) * 1024LL * 1024LL;
            std::wstring taskId;
            if (!registry.AddUploadTaskPlaceholder(uploadPath.wstring(), config_.appStoreOwner, config_.appStoreRepo, splitSize, taskId)) {
                MessageBoxW(hwnd_, registry.lastError().c_str(), kDriveManagerTitle, MB_OK | MB_ICONWARNING);
                return;
            }
            RunTaskById(taskId);
            LoadCurrentTab();
            return;
        }
        const int index = SelectedIndex();
        if (index >= 0 && index < static_cast<int>(tasks_.size())) {
            RunTaskById(tasks_[static_cast<std::size_t>(index)].id);
            LoadCurrentTab();
        }
    }

    void RenameSelectedStoreEntry() {
        const int index = SelectedIndex();
        if (index < 0 || index >= static_cast<int>(apps_.size())) {
            MessageBoxW(hwnd_, L"请选择一个网盘条目。", kDriveManagerTitle, MB_OK | MB_ICONINFORMATION);
            return;
        }
        AppStoreEntry entry = apps_[static_cast<std::size_t>(index)];
        AppStoreManifest manifest = entry.manifest;
        std::wstring newName = manifest.displayName.empty() ? manifest.name : manifest.displayName;
        if (!ShowTextInputDialog(hwnd_, instance_, theme_, L"重命名", L"显示名称", newName)) {
            return;
        }
        newName = Trim(newName);
        if (newName.empty()) {
            MessageBoxW(hwnd_, L"显示名称不能为空。", kDriveManagerTitle, MB_OK | MB_ICONWARNING);
            return;
        }

        std::wstring githubToken;
        std::wstring error;
        if (!AppStoreCredentialService::LoadSecret(config_, AppStoreCredentialService::SecretKind::GitHubToken, githubToken, error) || Trim(githubToken).empty()) {
            MessageBoxW(hwnd_, (error.empty() ? L"重命名需要 GitHub Token。" : error).c_str(), kDriveManagerTitle, MB_OK | MB_ICONWARNING);
            return;
        }

        manifest.displayName = newName;
        manifest.manifestJson = SerializeAppStoreManifest(manifest);
        const std::filesystem::path manifestPath = appDirectory_ / L"app-store" / L"tmp" / (manifest.appId + L"-rename-manifest.json");
        if (!WriteUtf8FileLocal(manifestPath, manifest.manifestJson)) {
            MessageBoxW(hwnd_, L"写入临时 manifest.json 失败。", kDriveManagerTitle, MB_OK | MB_ICONWARNING);
            return;
        }

        GitHubReleaseClient client(config_, githubToken);
        if (const AppStoreReleaseAsset* oldManifest = ManifestAsset(entry.release)) {
            if (!client.DeleteAsset(oldManifest->id)) {
                MessageBoxW(hwnd_, client.lastError().c_str(), kDriveManagerTitle, MB_OK | MB_ICONWARNING);
                return;
            }
        }
        AppStoreReleaseAsset uploaded;
        if (!client.UploadReleaseAsset(entry.release, manifestPath, L"manifest.json", uploaded)) {
            MessageBoxW(hwnd_, client.lastError().c_str(), kDriveManagerTitle, MB_OK | MB_ICONWARNING);
            return;
        }
        client.UpdateReleaseMetadata(entry.release.id, newName + L" " + manifest.version, manifest.summary);
        RefreshStore(false);
        LoadCurrentTab();
    }

    void RunTaskById(const std::wstring& taskId) {
        if (!activeTask_.empty()) {
            MessageBoxW(hwnd_, L"已有队列任务正在运行。", kDriveManagerTitle, MB_OK | MB_ICONINFORMATION);
            return;
        }
        AppStoreRegistry registry(appDirectory_);
        const auto allTasks = registry.LoadTasks();
        auto found = std::find_if(allTasks.begin(), allTasks.end(), [&](const AppTransferTask& task) {
            return task.id == taskId;
        });
        if (found == allTasks.end()) {
            MessageBoxW(hwnd_, L"队列任务不存在。", kDriveManagerTitle, MB_OK | MB_ICONWARNING);
            return;
        }
        activeTask_ = taskId;
        AppTransferTask task = *found;
        const HWND target = hwnd_;
        const std::filesystem::path appDirectory = appDirectory_;
        const AppConfig config = config_;
        if (worker_.joinable()) {
            worker_.detach();
        }
        worker_ = std::thread([target, appDirectory, config, task]() {
            AppTransferService service(appDirectory, config);
            const AppTransferRunReport report = service.RunTask(task);
            std::wstring text = report.message;
            auto* heapText = new wchar_t[text.size() + 1];
            std::copy(text.begin(), text.end(), heapText);
            heapText[text.size()] = L'\0';
            if (IsWindow(target)) {
                PostMessageW(target, WM_TASK_DONE, report.ok ? 1 : 0, reinterpret_cast<LPARAM>(heapText));
            } else {
                delete[] heapText;
            }
        });
        LoadCurrentTab();
    }

    void UpdateSelectedTask(const std::wstring& status) {
        if (tab_ == AppStoreTab::Store) {
            return;
        }
        const int index = SelectedIndex();
        if (index < 0 || index >= static_cast<int>(tasks_.size())) {
            return;
        }
        AppStoreRegistry registry(appDirectory_);
        const std::wstring nextStatus = status == L"paused" && tasks_[static_cast<std::size_t>(index)].status == L"running" ? L"canceling" : status;
        registry.SetTaskStatus(tasks_[static_cast<std::size_t>(index)].id, nextStatus, tasks_[static_cast<std::size_t>(index)].phase);
        LoadCurrentTab();
    }

    void DeleteSelectedTask() {
        if (tab_ == AppStoreTab::Store) {
            return;
        }
        const int index = SelectedIndex();
        if (index < 0 || index >= static_cast<int>(tasks_.size())) {
            return;
        }
        if (MessageBoxW(hwnd_, L"确定删除选中的队列任务？", kDriveManagerTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
            return;
        }
        if (tasks_[static_cast<std::size_t>(index)].status == L"running" ||
            tasks_[static_cast<std::size_t>(index)].status == L"canceling") {
            AppStoreRegistry registry(appDirectory_);
            registry.SetTaskStatus(tasks_[static_cast<std::size_t>(index)].id, L"canceling", tasks_[static_cast<std::size_t>(index)].phase);
            MessageBoxW(hwnd_, L"任务正在后台执行，已请求取消。任务暂停后可再次删除。", kDriveManagerTitle, MB_OK | MB_ICONINFORMATION);
            LoadCurrentTab();
            return;
        }
        if (tasks_[static_cast<std::size_t>(index)].direction == L"upload") {
            AppTransferService service(appDirectory_, config_);
            const AppTransferRunReport report = service.DeleteRemoteForUploadTask(tasks_[static_cast<std::size_t>(index)]);
            if (!report.ok && MessageBoxW(hwnd_, (report.message + L"\n\n仍然删除本地任务？").c_str(), kDriveManagerTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                return;
            }
        }
        AppStoreRegistry registry(appDirectory_);
        if (!registry.DeleteTask(tasks_[static_cast<std::size_t>(index)].id)) {
            MessageBoxW(hwnd_, registry.lastError().c_str(), kDriveManagerTitle, MB_OK | MB_ICONWARNING);
            return;
        }
        LoadCurrentTab();
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    std::filesystem::path appDirectory_;
    AppConfig config_;
    HWND hwnd_ = nullptr;
    HWND tabStore_ = nullptr;
    HWND tabDownloads_ = nullptr;
    HWND tabUploads_ = nullptr;
    HWND list_ = nullptr;
    HWND status_ = nullptr;
    HWND refresh_ = nullptr;
    HWND primary_ = nullptr;
    HWND pause_ = nullptr;
    HWND resume_ = nullptr;
    HWND delete_ = nullptr;
    HWND close_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH fieldBrush_ = nullptr;
    RECT listFrame_{};
    AppStoreTab tab_ = AppStoreTab::Store;
    std::vector<AppStoreEntry> apps_;
    std::vector<AppTransferTask> tasks_;
    std::wstring activeTask_;
    std::thread worker_;
    struct SpeedSample {
        long long bytes = 0;
        ULONGLONG tick = 0;
    };
    std::map<std::wstring, SpeedSample> speedSamples_;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
    bool done_ = false;
};
}

bool ShowAppStoreDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const std::filesystem::path& appDirectory,
    const AppConfig& config) {
    AppStoreDialog dialog(owner, instance, theme, appDirectory, config);
    return dialog.Run();
}
