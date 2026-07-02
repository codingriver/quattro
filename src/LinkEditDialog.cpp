#include "LinkEditDialog.h"

#include "../resources/resource.h"

#include "AppLog.h"
#include "HotKeyEditor.h"
#include "ShellItemService.h"
#include "ThemedControls.h"
#include "Utilities.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

namespace {
constexpr int kDialogWidth = 560;
constexpr int kDialogHeight = 540;
constexpr int kLabelX = 28;
constexpr int kFieldX = 108;
constexpr int kFieldWidth = 416;

enum ControlId {
    IdTag = 1001,
    IdType,
    IdName,
    IdPath,
    IdBrowseFile,
    IdBrowseFolder,
    IdParameter,
    IdWorkDir,
    IdBrowseWorkDir,
    IdIcon,
    IdShowCmd,
    IdHotKeyCapture,
    IdHotKeyClear,
    IdRemark,
    IdAdmin,
    IdCustomColor,
    IdCustomColorEdit,
    IdOk,
    IdCancel,
    IdPickColor = 1020,
};

struct TagChoice {
    int id = 0;
    std::wstring name;
};

std::wstring GetWindowTextString(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

float ClampFloat(float value, float minValue, float maxValue) {
    return std::max(minValue, std::min(maxValue, value));
}

COLORREF ToColorRef(Color color) {
    const auto byte = [](float value) -> BYTE {
        return static_cast<BYTE>(ClampFloat(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return RGB(byte(color.r), byte(color.g), byte(color.b));
}

bool LooksLikeUrl(const Link& link) {
    const std::wstring lower = ToLower(Trim(link.path));
    return link.type == 2 ||
           lower.rfind(L"http://", 0) == 0 ||
           lower.rfind(L"https://", 0) == 0 ||
           lower.rfind(L"ftp://", 0) == 0 ||
           lower.rfind(L"www.", 0) == 0;
}

std::wstring DialogTitleForLink(const Link& link, bool isNew) {
    if (isNew) {
        return LooksLikeUrl(link) ? L"添加网址" : L"添加启动项";
    }
    if (LooksLikeUrl(link)) {
        return L"编辑<超链接>";
    }
    if (link.type == 1) {
        return L"编辑<文件夹>";
    }
    if (link.type == 3) {
        return L"编辑<系统功能>";
    }
    return L"编辑<程序>";
}

std::wstring LinkHotKeyText(int key) {
    return key == 0 ? L"无" : FormatHotKeyText(key);
}

bool IsValidColorHex(const std::wstring& value) {
    std::wstring text = Trim(value);
    if (text.empty()) {
        return true;
    }
    if (!text.empty() && text.front() == L'#') {
        text.erase(text.begin());
    }
    if (text.size() != 6 && text.size() != 8) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](wchar_t ch) {
        return std::iswxdigit(ch) != 0;
    });
}

class DialogWindow {
public:
    DialogWindow(HWND owner, HINSTANCE instance, const Theme& theme, Link& link, const std::vector<Group>& groups, bool isNew)
        : owner_(owner), instance_(instance), theme_(theme), link_(link), groups_(groups), isNew_(isNew) {
        capturedHotKey_ = link.hotKey;
    }

    bool Run() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DialogWindow::WindowProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        wc.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"QuattroLinkEditDialog";
        RegisterClassExW(&wc);

        RECT ownerRect{};
        GetWindowRect(owner_, &ownerRect);
        const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - kDialogWidth) / 2;
        const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - kDialogHeight) / 2;

        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
            wc.lpszClassName,
            DialogTitleForLink(link_, isNew_).c_str(),
            WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_CLIPCHILDREN,
            x,
            y,
            kDialogWidth,
            kDialogHeight,
            owner_,
            nullptr,
            instance_,
            this);
        if (!hwnd_) {
            WriteAppLog(L"启动项编辑窗口创建失败: " + FormatLastError(GetLastError()));
            return false;
        }

        ownerWasEnabled_ = ShowModalWindow(owner_, hwnd_);
        UpdateWindow(hwnd_);
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);

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
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        DialogWindow* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<DialogWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<DialogWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (dialog) {
            return dialog->HandleMessage(message, wParam, lParam);
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            CreateControls();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC hdc = BeginPaint(hwnd_, &paint);
            PaintBackground(hdc);
            PaintFields(hdc);
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintBackground(reinterpret_cast<HDC>(wParam));
            PaintFields(reinterpret_cast<HDC>(wParam));
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            SetTextColor(reinterpret_cast<HDC>(wParam), ToColorRef(theme_.color(L"label", L"normal", L"text")));
            return reinterpret_cast<LRESULT>(backgroundBrush_ ? backgroundBrush_ : GetStockObject(WHITE_BRUSH));
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            SetBkColor(reinterpret_cast<HDC>(wParam), ToColorRef(theme_.color(L"edit", L"normal", L"bg")));
            SetTextColor(reinterpret_cast<HDC>(wParam), ToColorRef(theme_.color(L"edit", L"normal", L"text")));
            return reinterpret_cast<LRESULT>(fieldBrush_ ? fieldBrush_ : GetStockObject(WHITE_BRUSH));
        case WM_DRAWITEM:
            if (ThemedControls::Draw(theme_, reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
                return TRUE;
            }
            return 0;
        case WM_COMMAND:
            if (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS) {
                InvalidateField(reinterpret_cast<HWND>(lParam));
            }
            switch (LOWORD(wParam)) {
            case IdBrowseFile:
                if (!LooksLikeUrl(link_)) {
                    if (link_.type == 1) {
                        BrowseFolder(pathEdit_);
                    } else {
                        BrowseFile();
                    }
                }
                return 0;
            case IdBrowseFolder:
                BrowseIcon();
                return 0;
            case IdBrowseWorkDir:
                BrowseFolder(workDirEdit_);
                return 0;
            case IdHotKeyCapture:
                capturedHotKey_ = ShowHotKeyCaptureDialog(hwnd_, instance_, theme_, capturedHotKey_);
                UpdateHotKeyLabel();
                return 0;
            case IdHotKeyClear:
                capturedHotKey_ = 0;
                UpdateHotKeyLabel();
                return 0;
            case IdPickColor:
                PickColor();
                return 0;
            case IdOk:
                Accept();
                return 0;
            case IdCancel:
                Close(false);
                return 0;
            default:
                return 0;
            }
        case WM_CLOSE:
            Close(false);
            return 0;
        case WM_DESTROY:
            RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
            if (font_ && ownsFont_) {
                DeleteObject(font_);
            }
            font_ = nullptr;
            ownsFont_ = false;
            if (editFont_) {
                DeleteObject(editFont_);
                editFont_ = nullptr;
            }
            if (backgroundBrush_) {
                DeleteObject(backgroundBrush_);
                backgroundBrush_ = nullptr;
            }
            if (fieldBrush_) {
                DeleteObject(fieldBrush_);
                fieldBrush_ = nullptr;
            }
            done_ = true;
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    struct FieldFrame {
        RECT rect{};
        HWND child = nullptr;
        bool readOnly = false;
    };

    HWND Label(const wchar_t* text, int x, int y, int width = 74) {
        return ThemedControls::CreateLabelText(instance_, hwnd_, text, x, y + 7, width, theme_, editFont_ ? editFont_ : font_, SS_LEFT);
    }

    HWND Edit(int id, int x, int y, int width, const std::wstring& value) {
        const int fieldHeight = FieldHeight();
        const RECT frame{x, y, x + width, y + fieldHeight};
        HWND hwnd = ThemedControls::CreateSingleLineEdit(instance_, hwnd_, id, theme_, frame, value, editFont_ ? editFont_ : font_);
        fieldFrames_.push_back(FieldFrame{frame, hwnd, false});
        return hwnd;
    }

    HWND Button(int id, const wchar_t* text, int x, int y, int width, int height = 0) {
        if (height <= 0) {
            height = ButtonHeight();
        }
        if (id == IdOk) {
            return ThemedControls::CreatePrimaryButton(instance_, hwnd_, id, text, x, y, width, height, font_, true);
        }
        return ThemedControls::CreateButton(instance_, hwnd_, id, text, x, y, width, height, font_, false);
    }

    void PaintBackground(HDC hdc) {
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        FillRect(hdc, &rect, backgroundBrush_ ? backgroundBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    }

    void PaintFields(HDC hdc) {
        for (const auto& frame : fieldFrames_) {
            ThemedControls::DrawFieldFrame(theme_, hdc, frame.rect, frame.child, frame.readOnly);
        }
    }

    void InvalidateField(HWND child) {
        for (const auto& frame : fieldFrames_) {
            if (frame.child == child) {
                InvalidateRect(hwnd_, &frame.rect, TRUE);
                return;
            }
        }
    }

    void CreateControls() {
        backgroundBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
        fieldBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"edit", L"normal", L"bg")));
        font_ = ThemedControls::CreateDialogFont();
        ownsFont_ = font_ != nullptr;
        if (!font_) {
            font_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        }
        editFont_ = ThemedControls::CreateEditFont(theme_);

        const bool isUrl = LooksLikeUrl(link_);
        const int rowStep = RowStep();
        const int buttonHeight = ButtonHeight();
        int y = 24;
        Label(L"名称 *", kLabelX, y);
        nameEdit_ = Edit(IdName, kFieldX, y, kFieldWidth, link_.name);

        y += rowStep;
        Label(isUrl ? L"网址 *" : L"路径 *", kLabelX, y);
        pathEdit_ = Edit(IdPath, kFieldX, y, 374, link_.path);
        Button(IdBrowseFile, L"...", 492, y + 1, 32, buttonHeight);

        y += rowStep;
        Label(L"图标", kLabelX, y);
        iconEdit_ = Edit(IdIcon, kFieldX, y, 374, link_.icon.empty() ? (isUrl ? L"#url" : L"默认系统缓存图标") : link_.icon);
        Button(IdBrowseFolder, L"...", 492, y + 1, 32, buttonHeight);

        y += rowStep;
        Label(L"参数", kLabelX, y);
        parameterEdit_ = Edit(IdParameter, kFieldX, y, kFieldWidth, link_.parameter);

        y += rowStep;
        Label(L"工作目录", kLabelX, y);
        workDirEdit_ = Edit(IdWorkDir, kFieldX, y, kFieldWidth, link_.workDir);

        y += rowStep;
        adminCheck_ = ThemedControls::CreateCheckBox(
            instance_, hwnd_, IdAdmin, isUrl ? L"以隐私模式运行" : L"以管理员身份运行",
            kFieldX, y + 4, 220, ThemedControls::CheckBoxHeight(theme_), font_, link_.isAdmin);
        SendMessageW(adminCheck_, BM_SETCHECK, link_.isAdmin ? BST_CHECKED : BST_UNCHECKED, 0);

        y += rowStep;
        Label(L"颜色", kLabelX, y);
        std::wstring colorText = link_.isCustomColor && !link_.customColor.empty() ? link_.customColor : L"#ff000000";
        customColorEdit_ = Edit(IdCustomColorEdit, kFieldX, y, 374, colorText);
        Button(IdPickColor, L"...", 492, y + 1, 32, buttonHeight);

        y += rowStep;
        Label(L"快捷键", kLabelX, y);
        hotKeyText_ = ThemedControls::CreateButton(instance_, hwnd_, IdHotKeyCapture, LinkHotKeyText(capturedHotKey_).c_str(),
                                                   kFieldX, y + 1, 326, buttonHeight, font_);
        Button(IdHotKeyClear, L"清除", 448, y + 1, 76, buttonHeight);

        y += rowStep;
        Label(L"备注", kLabelX, y);
        const int remarkHeight = FieldHeight() * 2 + static_cast<int>(theme_.metric(L"global", L"sectionGap", 16.0f)) + 8;
        const RECT remarkFrame{kFieldX, y, kFieldX + kFieldWidth, y + remarkHeight};
        remarkEdit_ = ThemedControls::CreateMultiLineEdit(instance_, hwnd_, IdRemark, theme_, remarkFrame, link_.remark, font_);
        fieldFrames_.push_back(FieldFrame{remarkFrame, remarkEdit_, false});

        const int footerY = remarkFrame.bottom + static_cast<int>(theme_.metric(L"global", L"sectionGap", 16.0f));
        Button(IdOk, L"确定", 356, footerY, 76, buttonHeight);
        Button(IdCancel, L"取消", 448, footerY, 76, buttonHeight);
    }

    int FieldHeight() const {
        return ThemedControls::EditFrameHeight(theme_);
    }

    int ButtonHeight() const {
        return ThemedControls::ButtonHeight(theme_);
    }

    int RowStep() const {
        return FieldHeight() + static_cast<int>(theme_.metric(L"global", L"itemGap", 8.0f));
    }

    void AddType(const wchar_t* text, int type) {
        const LRESULT index = SendMessageW(typeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        SendMessageW(typeCombo_, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(type));
    }

    void SelectType(int type) {
        int fallback = 0;
        const int count = static_cast<int>(SendMessageW(typeCombo_, CB_GETCOUNT, 0, 0));
        for (int i = 0; i < count; ++i) {
            const int itemType = static_cast<int>(SendMessageW(typeCombo_, CB_GETITEMDATA, i, 0));
            if (itemType == type) {
                SendMessageW(typeCombo_, CB_SETCURSEL, i, 0);
                return;
            }
            if (itemType == 0) {
                fallback = i;
            }
        }
        SendMessageW(typeCombo_, CB_SETCURSEL, fallback, 0);
    }

    void AddShowCmd(const wchar_t* text, int showCmd) {
        const LRESULT index = SendMessageW(showCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        SendMessageW(showCombo_, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(showCmd));
    }

    void SelectShowCmd(int showCmd) {
        if (showCmd <= 0) {
            showCmd = SW_SHOWNORMAL;
        }
        int fallback = 0;
        const int count = static_cast<int>(SendMessageW(showCombo_, CB_GETCOUNT, 0, 0));
        for (int i = 0; i < count; ++i) {
            const int itemShowCmd = static_cast<int>(SendMessageW(showCombo_, CB_GETITEMDATA, i, 0));
            if (itemShowCmd == showCmd) {
                SendMessageW(showCombo_, CB_SETCURSEL, i, 0);
                return;
            }
            if (itemShowCmd == SW_SHOWNORMAL) {
                fallback = i;
            }
        }
        SendMessageW(showCombo_, CB_SETCURSEL, fallback, 0);
    }

    void BuildTagChoices() {
        tags_.clear();
        for (const auto& tag : groups_) {
            if (tag.parentGroup == 0) {
                continue;
            }
            std::wstring parentName;
            for (const auto& parent : groups_) {
                if (parent.id == tag.parentGroup) {
                    parentName = parent.name;
                    break;
                }
            }
            TagChoice choice;
            choice.id = tag.id;
            choice.name = parentName.empty() ? tag.name : (parentName + L" / " + tag.name);
            tags_.push_back(std::move(choice));
        }
    }

    void BrowseFile() {
        std::wstring buffer(32768, L'\0');
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFile = buffer.data();
        ofn.nMaxFile = static_cast<DWORD>(buffer.size());
        ofn.lpstrFilter = L"所有文件\0*.*\0";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) {
            SetWindowTextW(pathEdit_, buffer.c_str());
            SetNameFromPathIfEmpty(buffer.c_str());
            if (typeCombo_) {
                SelectType(0);
            }
        }
    }

    void BrowseIcon() {
        std::wstring buffer(32768, L'\0');
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFile = buffer.data();
        ofn.nMaxFile = static_cast<DWORD>(buffer.size());
        ofn.lpstrFilter = L"图标或程序\0*.ico;*.exe;*.dll\0所有文件\0*.*\0";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) {
            SetWindowTextW(iconEdit_, buffer.c_str());
        }
    }

    void PickColor() {
        static COLORREF customColors[16]{};
        CHOOSECOLORW choose{};
        choose.lStructSize = sizeof(choose);
        choose.hwndOwner = hwnd_;
        choose.lpCustColors = customColors;
        choose.Flags = CC_FULLOPEN | CC_RGBINIT;
        choose.rgbResult = RGB(0, 0, 0);
        if (ChooseColorW(&choose)) {
            wchar_t buffer[16]{};
            swprintf_s(buffer, L"#ff%02x%02x%02x", GetRValue(choose.rgbResult), GetGValue(choose.rgbResult), GetBValue(choose.rgbResult));
            SetWindowTextW(customColorEdit_, buffer);
        }
    }

    void BrowseFolder(HWND targetEdit) {
        IFileDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
            return;
        }

        DWORD options = 0;
        dialog->GetOptions(&options);
        DWORD requested = options | FOS_PICKFOLDERS;
        if (targetEdit != pathEdit_) {
            requested |= FOS_FORCEFILESYSTEM;
        }
        dialog->SetOptions(requested);
        if (SUCCEEDED(dialog->Show(hwnd_))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
                if (FAILED(hr) && targetEdit == pathEdit_) {
                    hr = item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &path);
                }
                if (SUCCEEDED(hr)) {
                    SetWindowTextW(targetEdit, path);
                    if (targetEdit == pathEdit_) {
                        SetNameFromShellItemIfEmpty(item, path);
                        if (typeCombo_) {
                            SelectType(ShellItemService::IsShellParseName(path) ? 3 : 1);
                        }
                    }
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }

    void SetNameFromShellItemIfEmpty(IShellItem* item, const std::wstring& fallback) {
        if (!Trim(GetWindowTextString(nameEdit_)).empty()) {
            return;
        }
        PWSTR displayName = nullptr;
        if (item && SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &displayName)) && displayName && *displayName) {
            SetWindowTextW(nameEdit_, displayName);
            CoTaskMemFree(displayName);
            return;
        }
        if (displayName) {
            CoTaskMemFree(displayName);
        }
        SetNameFromPathIfEmpty(fallback);
    }

    void SetNameFromPathIfEmpty(const std::wstring& path) {
        if (!Trim(GetWindowTextString(nameEdit_)).empty()) {
            return;
        }
        std::filesystem::path fsPath(path);
        std::wstring name = fsPath.stem().wstring();
        if (name.empty()) {
            name = fsPath.filename().wstring();
        }
        SetWindowTextW(nameEdit_, name.c_str());
    }

    void Accept() {
        Link next = link_;
        next.name = Trim(GetWindowTextString(nameEdit_));
        next.path = Trim(GetWindowTextString(pathEdit_));
        next.parameter = Trim(GetWindowTextString(parameterEdit_));
        next.workDir = Trim(GetWindowTextString(workDirEdit_));
        next.icon = Trim(GetWindowTextString(iconEdit_));
        if (next.icon == L"默认系统缓存图标") {
            next.icon.clear();
        }
        next.remark = Trim(GetWindowTextString(remarkEdit_));
        next.hotKey = capturedHotKey_;
        next.isAdmin = SendMessageW(adminCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        next.customColor = Trim(GetWindowTextString(customColorEdit_));
        next.isCustomColor = !next.customColor.empty() && ToLower(next.customColor) != L"#ff000000";

        if (tagCombo_) {
            const LRESULT tagIndex = SendMessageW(tagCombo_, CB_GETCURSEL, 0, 0);
            if (tagIndex != CB_ERR) {
                next.parentGroup = static_cast<int>(SendMessageW(tagCombo_, CB_GETITEMDATA, static_cast<WPARAM>(tagIndex), 0));
            }
        }
        if (typeCombo_) {
            const LRESULT typeIndex = SendMessageW(typeCombo_, CB_GETCURSEL, 0, 0);
            if (typeIndex != CB_ERR) {
                next.type = static_cast<int>(SendMessageW(typeCombo_, CB_GETITEMDATA, static_cast<WPARAM>(typeIndex), 0));
            }
        } else {
            const std::wstring lowerPath = ToLower(next.path);
            std::error_code ec;
            if (LooksLikeUrl(next)) {
                next.type = 2;
                next.path = NormalizeUrl(next.path);
                next.pidl.clear();
                if (next.icon.empty()) {
                    next.icon = L"#url";
                }
            } else if (ShellItemService::IsShellParseName(next.path)) {
                next.type = 3;
            } else if (std::filesystem::is_directory(ExpandEnvironmentStringsSafe(next.path), ec)) {
                next.type = 1;
            } else if (next.type != 3 && next.type != 4) {
                next.type = 0;
            }
            (void)lowerPath;
        }
        if (showCombo_) {
            const LRESULT showIndex = SendMessageW(showCombo_, CB_GETCURSEL, 0, 0);
            if (showIndex != CB_ERR) {
                next.showCmd = static_cast<int>(SendMessageW(showCombo_, CB_GETITEMDATA, static_cast<WPARAM>(showIndex), 0));
            }
        }

        if (next.name.empty()) {
            MessageBoxW(hwnd_, L"请输入名称。", L"启动项", MB_OK | MB_ICONWARNING);
            SetFocus(nameEdit_);
            return;
        }
        if (next.path.empty()) {
            MessageBoxW(hwnd_, L"请输入路径或网址。", L"启动项", MB_OK | MB_ICONWARNING);
            SetFocus(pathEdit_);
            return;
        }
        if (!IsValidColorHex(next.customColor)) {
            MessageBoxW(hwnd_, L"颜色必须是 #RRGGBB 或 #AARRGGBB 格式。", L"启动项", MB_OK | MB_ICONWARNING);
            SetFocus(customColorEdit_);
            return;
        }
        if (next.parentGroup <= 0) {
            MessageBoxW(hwnd_, L"请选择标签。", L"启动项", MB_OK | MB_ICONWARNING);
            if (tagCombo_) {
                SetFocus(tagCombo_);
            }
            return;
        }
        if (next.showCmd <= 0) {
            next.showCmd = SW_SHOWNORMAL;
        }
        const bool targetChanged = next.path != link_.path || next.type != link_.type;
        ShellItemService::RefreshLinkShellData(next, targetChanged);

        link_ = std::move(next);
        Close(true);
    }

    void UpdateHotKeyLabel() {
        if (hotKeyText_) {
            SetWindowTextW(hotKeyText_, LinkHotKeyText(capturedHotKey_).c_str());
        }
    }

    void Close(bool accepted) {
        accepted_ = accepted;
        done_ = true;
        DestroyWindow(hwnd_);
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    const Theme& theme_;
    Link& link_;
    const std::vector<Group>& groups_;
    bool isNew_ = false;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
    bool accepted_ = false;
    bool done_ = false;

    HFONT font_ = nullptr;
    HFONT editFont_ = nullptr;
    bool ownsFont_ = false;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH fieldBrush_ = nullptr;
    std::vector<FieldFrame> fieldFrames_;
    HWND tagCombo_ = nullptr;
    HWND typeCombo_ = nullptr;
    HWND showCombo_ = nullptr;
    HWND nameEdit_ = nullptr;
    HWND pathEdit_ = nullptr;
    HWND parameterEdit_ = nullptr;
    HWND workDirEdit_ = nullptr;
    HWND iconEdit_ = nullptr;
    HWND hotKeyText_ = nullptr;
    HWND remarkEdit_ = nullptr;
    HWND adminCheck_ = nullptr;
    HWND customColorCheck_ = nullptr;
    HWND customColorEdit_ = nullptr;
    int capturedHotKey_ = 0;
    std::vector<TagChoice> tags_;
};
}

bool LinkEditDialog::Show(HWND owner, HINSTANCE instance, const Theme& theme, Link& link, const std::vector<Group>& groups, bool isNew) {
    DialogWindow dialog(owner, instance, theme, link, groups, isNew);
    return dialog.Run();
}

