#pragma once

#include "Config.h"
#include "IconService.h"
#include "Launcher.h"
#include "Models.h"
#include "Storage.h"
#include "Theme.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <windows.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

constexpr UINT WM_QUATTRO_WAKEUP = WM_APP + 0x65;
constexpr UINT WM_QUATTRO_TRAY = WM_APP + 0x66;

class OleDropTarget;

class MainWindow {
public:
    MainWindow(
        HINSTANCE instance,
        std::filesystem::path appDirectory,
        ConfigService& configService,
        StorageService& storageService,
        AppConfig config,
        AppModel model,
        Theme theme);
    ~MainWindow();

    bool Create();
    int RunMessageLoop();
    HWND hwnd() const { return hwnd_; }
    const AppConfig& config() const { return config_; }

private:
    friend class OleDropTarget;

    enum class HitKind {
        None,
        CloseButton,
        SearchButton,
        MenuButton,
        SkinButton,
        AddButton,
        Group,
        Tag,
        Link,
    };

    struct HitArea {
        HitKind kind = HitKind::None;
        int id = 0;
        D2D1_RECT_F rect{};
    };

    struct MenuItemData {
        std::wstring text;
        int icon = 0;
        bool checked = false;
        bool disabled = false;
        bool submenu = false;
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void SelectInitialItems();
    void SelectGroup(int groupId);
    void SelectTag(int tagId);
    void AddGroup();
    void EditGroup(int groupId);
    void DeleteGroup(int groupId);
    void AddTag();
    void EditTag(int tagId);
    void DeleteTag(int tagId);
    void SetCurrentTagSort(int sort);
    void SetCurrentTagLayout(int layout);
    void SetCurrentTagIconSize(int iconSize);
    int CommandGroupId() const;
    int CommandTagId() const;
    int CommandLinkId() const;
    void AddLink();
    void AddFile();
    void AddFolder();
    void AddUrl();
    void AddSystemFunction();
    void EditLink(int linkId);
    void DeleteLink(int linkId);
    void RunLink(int linkId);
    void RunLinkAsAdmin(int linkId);
    void RunUrlPrivate(int linkId);
    void CopyLinkInternal(int linkId, bool cut);
    void PasteLinkInternal();
    void MoveMenuContext(int direction);
    void MoveLinkWithinTag(int linkId, int direction);
    void MoveGroupWithinParent(int groupId, int direction);
    void MoveLinkToTag(int linkId, int tagId);
    void CopyLinkToTag(int linkId, int tagId);
    void OpenContainingFolder(int linkId);
    void CopyLinkPath(int linkId);
    void ShowWindowsContextMenu(int linkId, POINT screenPoint);
    void CreateDesktopShortcut(int linkId);
    void OpenSystemProperties(int linkId);
    void ClearCurrentTagLinks();
    void OpenSearch();
    void OpenSettings();
    void ClearIconCache();
    void RefreshLinkIcon(int linkId);
    void ShowAbout();
    void OpenHelp();
    void OpenFaq();
    void OpenReward();
    void ShowUpdateInfo();
    bool OpenConfiguredUrl(const std::wstring& url, const wchar_t* title);
    bool TryRepairLinkTarget(Link& link);
    void ShowThemeMenu(POINT screenPoint);
    void ApplyTheme(const std::wstring& themeName);
    void UpdateDockState();
    void DockHide();
    void DockRestore();
    bool IsNearDockEdge(POINT screenPoint) const;
    bool IsEffectivelyVisible() const;
    void HideMainWindow();
    void ImportPath(const std::wstring& path);
    void ImportClipboard();
    bool ImportDropData(IDataObject* dataObject);
    void ApplyConfigRuntimeChanges(const AppConfig& previous);
    void SyncAutoRun(const AppConfig& previous);
    void RegisterConfiguredHotKeys();
    void UnregisterConfiguredHotKeys();
    void InitializeTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu(POINT screenPoint);
    void ShowMainMenu(POINT screenPoint);
    void ShowLinkMenu(int linkId, POINT screenPoint);
    void ShowGroupMenu(int groupId, POINT screenPoint);
    void ShowGroupBlankMenu(POINT screenPoint);
    void ShowTagMenu(int tagId, POINT screenPoint);
    void ShowTagBlankMenu(POINT screenPoint);
    void ShowBackgroundMenu(POINT screenPoint);
    void AppendThemeItemsToMenu(HMENU menu);
    void AppendAddLinkItems(HMENU menu);
    void AppendViewOptionItems(HMENU menu, const Group* tag);
    void AppendTagTargetMenu(HMENU menu, UINT commandBase, std::vector<int>& targetIds, int excludedTagId);
    void AppendGroupedTagTargetMenu(HMENU menu, UINT commandBase, std::vector<int>& targetIds, int excludedTagId);
    void SaveWindowState();
    void WakeUp();

    void DiscardDeviceResources();
    HRESULT CreateDeviceResources();
    void OnPaint();
    void OnResize(UINT width, UINT height);
    void Draw();
    void DrawTitle(D2D1_RECT_F rect);
    void DrawGroups(D2D1_RECT_F rect);
    void DrawTags(D2D1_RECT_F rect);
    void DrawLinks(D2D1_RECT_F rect);
    void DrawButtonIcon(HitKind kind, D2D1_RECT_F rect, const Color& color);
    void DrawUiBitmap(const std::wstring& name, const D2D1_RECT_F& rect, float opacity = 1.0f);
    ID2D1Bitmap* LoadAppIconBitmap();
    ID2D1Bitmap* LoadUiBitmap(const std::wstring& name);
    void ClearUiBitmaps();
    void FillRect(const D2D1_RECT_F& rect, const Color& color);
    void DrawRect(const D2D1_RECT_F& rect, const Color& color, float strokeWidth = 1.0f);
    void FillRoundedRect(const D2D1_RECT_F& rect, const Color& color, float radius);
    void DrawRoundedRect(const D2D1_RECT_F& rect, const Color& color, float radius, float strokeWidth = 1.0f);
    void DrawSoftShadow(const D2D1_RECT_F& rect, float radius);
    void DrawScrollBar(const D2D1_RECT_F& rect, float offset, float maxOffset, bool horizontal);
    void DrawTextBlock(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, const Color& color);
    void ResetMenuVisuals();
    void AppendThemedMenuItem(HMENU menu, UINT flags, UINT_PTR id, const std::wstring& text, bool submenu = false);
    void AppendThemedSeparator(HMENU menu);
    bool MeasureThemedMenuItem(MEASUREITEMSTRUCT* measure);
    bool DrawThemedMenuItem(const DRAWITEMSTRUCT* draw);
    void ClampScrollOffsets();
    void ScrollAtPoint(float x, float y, int wheelDelta, bool horizontal);
    float MaxGroupScrollOffset(const D2D1_RECT_F& rect) const;
    float MaxTagScrollOffset(const D2D1_RECT_F& rect) const;
    float MaxLinkScrollOffset(const D2D1_RECT_F& rect) const;
    void EnsureGroupVisible(int groupId);
    void EnsureTagVisible(int tagId);
    void EnsureLinkVisible(int linkId);

    std::vector<Group> MajorGroups() const;
    std::vector<Group> TagsForCurrentGroup() const;
    std::wstring TagDisplayName(const Group& tag) const;
    std::vector<Link*> LinksForCurrentTag();
    Group* FindGroup(int id);
    const Group* FindGroup(int id) const;
    Link* FindLink(int id);
    int LinkIdFromHotKeyId(int hotKeyId) const;
    bool IsUrlLink(const Link& link) const;
    int EnsureCurrentTag();

    void BuildLayout(float width, float height, D2D1_RECT_F& title, D2D1_RECT_F& groups, D2D1_RECT_F& tags, D2D1_RECT_F& links) const;
    HitArea HitTest(float x, float y) const;
    bool IsHover(HitKind kind, int id) const;
    static bool Contains(const D2D1_RECT_F& rect, float x, float y);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    std::filesystem::path appDirectory_;
    ConfigService& configService_;
    StorageService& storageService_;
    AppConfig config_;
    AppModel model_;
    Theme theme_;
    Launcher launcher_;
    IconService iconService_;

    int currentGroupId_ = 0;
    int currentTagId_ = 0;
    int selectedLinkId_ = 0;
    HitKind menuContextKind_ = HitKind::None;
    int menuContextId_ = 0;
    HitArea hover_;
    std::vector<HitArea> hitAreas_;
    float groupScrollOffset_ = 0.0f;
    float tagScrollOffset_ = 0.0f;
    float linkScrollOffset_ = 0.0f;
    std::vector<int> menuMoveTargetIds_;
    std::vector<int> menuCopyTargetIds_;
    std::vector<std::pair<int, int>> registeredLinkHotKeys_;
    Link clipboardLink_;
    bool hasClipboardLink_ = false;
    bool clipboardCut_ = false;
    int clipboardSourceId_ = 0;
    bool trackingMouse_ = false;
    bool trayIconVisible_ = false;
    bool hotKeysRegistered_ = false;
    bool dockHidden_ = false;
    RECT dockRestoreRect_{};
    UINT_PTR dockTimerId_ = 0;
    ULONGLONG dockHideDueTick_ = 0;
    HitKind pendingHoverActivationKind_ = HitKind::None;
    int pendingHoverActivationId_ = 0;
    UINT_PTR hoverActivationTimerId_ = 0;
    OleDropTarget* oleDropTarget_ = nullptr;

    ID2D1Factory* d2dFactory_ = nullptr;
    IDWriteFactory* dwriteFactory_ = nullptr;
    IWICImagingFactory* uiWicFactory_ = nullptr;
    ID2D1HwndRenderTarget* renderTarget_ = nullptr;
    IDWriteTextFormat* titleFormat_ = nullptr;
    IDWriteTextFormat* textFormat_ = nullptr;
    IDWriteTextFormat* smallFormat_ = nullptr;
    std::unordered_map<std::wstring, ID2D1Bitmap*> uiBitmapCache_;
    std::vector<std::unique_ptr<MenuItemData>> activeMenuItems_;
};
