#pragma once

#include "Config.h"
#include "ConfigPackageService.h"
#include "IconService.h"
#include "Launcher.h"
#include "LocalHttpServerService.h"
#include "MenuCatalog.h"
#include "Models.h"
#include "PluginRegistry.h"
#include "Storage.h"
#include "SystemFunctions.h"
#include "Theme.h"
#include "UrlIconDownloadService.h"

#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>
#include <commctrl.h>
#include <wincodec.h>

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class ThemedWindowUi;

constexpr UINT WM_QUATTRO_WAKEUP = WM_APP + 0x65;
constexpr UINT WM_QUATTRO_TRAY = WM_APP + 0x66;
constexpr UINT WM_QUATTRO_URL_ICON_DOWNLOADED = WM_APP + 0x67;
constexpr UINT WM_QUATTRO_DOCK_PEEK_ACTIVATE = WM_APP + 0x68;
constexpr UINT WM_QUATTRO_EXIT_INSTANCE = WM_APP + 0x69;
constexpr UINT WM_QUATTRO_STARTUP_ACTIVATE = WM_APP + 0x6A;
constexpr UINT WM_QUATTRO_STARTUP_DEFERRED = WM_APP + 0x6B;

class OleDropTarget;

class MainWindow {
public:
    MainWindow(
        HINSTANCE instance,
        std::filesystem::path appDirectory,
        std::filesystem::path httpRootBaseDirectory,
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
    class DockAutoHidePause {
    public:
        DockAutoHidePause(MainWindow& window, bool restoreHidden = true);
        ~DockAutoHidePause();
        DockAutoHidePause(const DockAutoHidePause&) = delete;
        DockAutoHidePause& operator=(const DockAutoHidePause&) = delete;

    private:
        MainWindow& window_;
    };

    friend class OleDropTarget;

    enum class HitKind {
        None,
        CloseButton,
        MenuButton,
        ToolButton,
        SkinButton,
        AddButton,
        Group,
        Tag,
        Link,
        Todo,
    };

    struct HitArea {
        HitKind kind = HitKind::None;
        int id = 0;
        D2D1_RECT_F rect{};
    };

    enum class MenuIconTone {
        Default,
        Active,
        Muted,
    };

    enum class LinkDragMode {
        None,
        Insert,
        Swap,
    };

    struct MenuItemData {
        std::wstring text;
        int icon = 0;
        int systemImageIndex = -1;
        int stockIcon = -1;
        bool disabled = false;
        bool submenu = false;
        bool separator = false;
        MenuIconTone iconTone = MenuIconTone::Default;
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
    void AddNoteTag();
    void AddTodoTag();
    void EditTag(int tagId);
    void DeleteTag(int tagId);
    void SetCurrentTagSort(int sort);
    void SetCurrentTodoSort(int sort);
    void SetCurrentTagLayout(int layout);
    void SetCurrentTagIconSize(int iconSize);
    void SetAllTagsSort(int sort);
    void SetAllTagsLayout(int layout);
    void SetAllTagsIconSize(int iconSize);
    void ToggleConfigVisibility(bool AppConfig::*field);
    int CommandGroupId() const;
    int CommandTagId() const;
    int CommandLinkId() const;
    void AddLink();
    void AddFile();
    void AddFolder();
    void AddUrl();
    void QuickImport();
    int EnsureQuickImportTargetTag();
    void AddSystemFunction(std::size_t index);
    void OpenSystemFunction(std::size_t index);
    void EditLink(int linkId);
    void DeleteLink(int linkId);
    void RunLink(int linkId);
    void RunLinkAsAdmin(int linkId);
    void RunUrlPrivate(int linkId);
    void CopyLinkInternal(int linkId, bool cut);
    void PasteLinkInternal();
    void MoveMenuContext(int direction);
    void MoveLinkWithinTag(int linkId, int direction);
    bool ApplyManualLinkOrder(int tagId, const std::vector<int>& orderedLinkIds, const wchar_t* title);
    void MoveGroupWithinParent(int groupId, int direction);
    void MoveTagToGroup(int tagId, int groupId);
    void MoveLinkToTag(int linkId, int tagId);
    void CopyLinkToTag(int linkId, int tagId);
    void OpenContainingFolder(int linkId);
    void CopyLinkPath(int linkId);
    bool LinkCenterScreenPoint(int linkId, POINT& screenPoint) const;
    void ShowWindowsContextMenu(int linkId, POINT screenPoint);
    void CreateDesktopShortcut(int linkId);
    void OpenSystemProperties(int linkId);
    void ClearCurrentTagLinks();
    void AddTodoItem();
    void EditTodoItem(int todoId);
    void DeleteTodoItem(int todoId);
    void ToggleTodoDone(int todoId);
    void ToggleTodoEnabled(int todoId);
    void ClearDoneTodos();
    void CheckTodoReminders();
    void ShowTodoReminder(const TodoItem& item);
    void ShowTodoReminderPanel(const TodoItem& item);
    void HideTodoReminderPanel();
    void ShowTodoSystemNotification(const TodoItem& item);
    void ShowClipboardImportNotification(int count, const std::wstring& pathDetail = L"");
    bool EnsureNotificationIcon();
    void OpenSettings();
    void CommitSettingsConfig(const AppConfig& next, bool importedData);
    void OpenBuiltinTool(std::size_t index);
    void ResetLayoutToDefaults();
    void ClearIconCache();
    void RefreshAllIcons();
    void RefreshLinkIcon(int linkId);
    void RequestInitialUrlIconDownload(const Link& link);
    void OnUrlIconDownloaded(int linkId, bool success);
    void ShowAbout();
    void CheckForUpdates();
    void OpenHelp();
    void OpenFaq();
    void OpenReward();
    bool OpenConfiguredUrl(const std::wstring& url, const wchar_t* title);
    void RestartWithOppositePrivilege();
    bool TryRepairLinkTarget(Link& link);
    void ShowThemeMenu(POINT screenPoint);
    void ApplyTheme(const std::wstring& themeName);
    void BeginDockAutoHidePause(bool restoreHidden);
    void EndDockAutoHidePause();
    bool DockAutoHidePaused() const;
    void UpdateDockState();
    bool DockHide();
    void DockRestore();
    void ShowDockPeek(const RECT& peekRect);
    void HideDockPeek();
    bool SnapDockWindowRect(RECT& window) const;
    bool IsNearDockEdge(POINT screenPoint) const;
    bool IsEffectivelyVisible() const;
    void HideMainWindowAfterLink();
    void HideMainWindow();
    void ImportPath(const std::wstring& path);
    void ImportClipboard();
    void ExportConfigPackage();
    void ImportConfigPackageMerge();
    bool ImportDropData(IDataObject* dataObject);
    bool StartHttpServer(bool showMessage);
    void StopHttpServer(bool showMessage);
    bool RestartHttpServer(bool showMessage);
    void SyncHttpServerRuntime(const AppConfig& previous);
    void ApplyConfigRuntimeChanges(const AppConfig& previous);
    void SyncAutoRun(const AppConfig& previous);
    void RegisterConfiguredHotKeys();
    void UnregisterConfiguredHotKeys();
    void InitializeTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu(POINT screenPoint);
    void ShowMainMenu(POINT screenPoint);
    void ShowToolMenu(POINT screenPoint);
    void ShowLinkMenu(int linkId, POINT screenPoint);
    void AppendLinkActionItems(HMENU menu, Link* link, bool includeNativeMenuItem);
    void AppendBuiltinSystemContextItems(HMENU menu, const Link& link);
    void ExecuteBuiltinSystemContextAction(int linkId, BuiltinSystemContextAction action);
    void RestoreLegacyBuiltinSystemFunctionKeys();
    void CreateTooltip();
    void ApplyTooltipTheme();
    void HideItemTooltip();
    void UpdateItemTooltip(const HitArea& hit, POINT screenPoint);
    std::wstring LinkTooltipText(const Link& link) const;
    std::wstring TodoTooltipText(const TodoItem& item) const;
    void ShowGroupMenu(int groupId, POINT screenPoint);
    void ShowGroupBlankMenu(POINT screenPoint);
    void ShowTagMenu(int tagId, POINT screenPoint);
    void ShowTagBlankMenu(POINT screenPoint);
    void ShowTodoMenu(int todoId, POINT screenPoint);
    void ShowBackgroundMenu(POINT screenPoint);
    void AppendThemeItemsToMenu(HMENU menu);
    void AppendAddLinkItems(HMENU menu);
    void AppendViewOptionItems(HMENU menu, const Group* tag);
    void AppendTodoSortItems(HMENU menu, const Group* tag);
    void AppendUnifiedViewOptionItems(HMENU menu);
    void AppendSystemFunctionItems(HMENU menu, UINT commandBase = ID_MENU_SYSTEM_FUNCTION_BASE);
    void AppendToolItems(HMENU menu);
    std::vector<int> GroupTargetIds(int excludedGroupId) const;
    std::vector<int> GroupedTagTargetIds(int excludedTagId) const;
    void AppendGroupTargetMenu(HMENU menu, UINT commandBase, std::vector<int>& targetIds, int excludedGroupId);
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
    void DrawMajorNavItem(D2D1_RECT_F rect, const std::wstring& text, bool selected, bool hovered, bool vertical);
    void DrawMinorNavItem(D2D1_RECT_F rect, const std::wstring& text, bool selected, bool hovered);
    void DrawLinks(D2D1_RECT_F rect);
    void DrawContentCard(const D2D1_RECT_F& fullRect);
    D2D1_RECT_F CardRectFor(const D2D1_RECT_F& regionRect) const;
    D2D1_RECT_F CardContentRectFor(const D2D1_RECT_F& regionRect) const;
    void DrawNotePage(D2D1_RECT_F rect, const Group& tag);
    void DrawTodoItems(D2D1_RECT_F rect, const Group& tag);
    void DrawEmptyState(const D2D1_RECT_F& contentRect, const std::wstring& title, const std::wstring& hint, const std::wstring& buttonLabel);
    void DrawEmptyAddButton(const D2D1_RECT_F& contentRect, float topY, const std::wstring& label);
    static std::array<HitKind, 4> TitleButtonsRightToLeft();
    bool IsTitleButtonVisible(HitKind kind) const;
    float TitleButtonsReserveWidth() const;
    void DrawButtonIcon(HitKind kind, D2D1_RECT_F rect, const Color& color);
    ID2D1Bitmap* LoadAppIconBitmap();
    void ClearUiBitmaps();
    void FillRect(const D2D1_RECT_F& rect, const Color& color);
    void DrawRect(const D2D1_RECT_F& rect, const Color& color, float strokeWidth = 1.0f);
    void FillRoundedRect(const D2D1_RECT_F& rect, const Color& color, float radius);
    void DrawRoundedRect(const D2D1_RECT_F& rect, const Color& color, float radius, float strokeWidth = 1.0f);
    void FillEllipse(float cx, float cy, float radius, const Color& color);
    void DrawScrollBar(const D2D1_RECT_F& rect, float offset, float maxOffset, bool horizontal);
    float MeasureTextWidth(const std::wstring& text, IDWriteTextFormat* format, float maxWidth = 1000.0f) const;
    void DrawTextBlock(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, const Color& color);
    void DrawLinkName(const std::wstring& text, IDWriteTextFormat* format, const D2D1_RECT_F& rect, const Color& color);
    void ResetMenuVisuals();
    void AppendThemedMenuItem(HMENU menu, UINT flags, UINT_PTR id, const std::wstring& text, bool submenu = false, int systemImageIndex = -1, int stockIcon = -1, int menuIcon = 0);
    void InsertThemedMenuItem(HMENU menu, UINT position, UINT flags, UINT_PTR id, const std::wstring& text, bool submenu = false, int systemImageIndex = -1, int stockIcon = -1, int menuIcon = 0);
    void AppendThemedStateMenuItem(HMENU menu, UINT flags, UINT_PTR id, const std::wstring& text, bool active, int menuIcon = 0, bool submenu = false);
    void AppendThemedSeparator(HMENU menu);
    const MenuItemData* ThemedMenuItemFromData(ULONG_PTR itemData) const;
    bool MeasureThemedMenuItem(MEASUREITEMSTRUCT* measure);
    bool DrawThemedMenuItem(const DRAWITEMSTRUCT* draw);
    void ClampScrollOffsets();
    void ScrollAtPoint(float x, float y, int wheelDelta, bool horizontal);
    float MaxGroupScrollOffset(const D2D1_RECT_F& rect) const;
    float MaxTagScrollOffset(const D2D1_RECT_F& rect) const;
    float MaxLinkScrollOffset(const D2D1_RECT_F& rect) const;
    float TodoContentHeight(const D2D1_RECT_F& rect) const;
    void EnsureGroupVisible(int groupId);
    void EnsureTagVisible(int tagId);
    void EnsureLinkVisible(int linkId);
    void EnsureTodoVisible(int todoId);

    void BeginLinkDragCandidate(int linkId, POINT point);
    void UpdateLinkDrag(POINT point);
    void CommitLinkDrag();
    void CancelLinkDrag();
    void UpdateLinkDragTarget(POINT point);
    void MoveLinkSelection(int dx, int dy);
    void MoveTodoSelection(int delta);
    void SelectAdjacentTag(int direction);
    void SelectAdjacentGroup(int direction);
    bool HandleKeyDown(WPARAM key);
    void SetDragOver(bool active);
    HitArea CursorHitArea() const;

    std::vector<Group> MajorGroups() const;
    std::vector<Group> TagsForCurrentGroup() const;
    std::wstring TagDisplayName(const Group& tag) const;
    std::vector<Link*> OrderedLinksForTag(int tagId);
    std::vector<Link*> LinksForCurrentTag();
    std::vector<TodoItem*> TodosForCurrentTag();
    Group* FindGroup(int id);
    const Group* FindGroup(int id) const;
    Link* FindLink(int id);
    TodoItem* FindTodoItem(int id);
    int CommandTodoId() const;
    NotePage* FindNotePage(int tagId);
    const NotePage* FindNotePage(int tagId) const;
    void SaveCurrentNotePage();
    void HideNoteEdit();
    void EnsureNoteEdit(const D2D1_RECT_F& rect, const Group& tag);
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
    std::filesystem::path httpRootBaseDirectory_;
    ConfigService& configService_;
    StorageService& storageService_;
    PluginRegistry pluginRegistry_;
    AppConfig config_;
    AppModel model_;
    Theme theme_;
    Launcher launcher_;
    IconService iconService_;
    UrlIconDownloadService urlIconDownloadService_;
    LocalHttpServerService httpServerService_;

    int currentGroupId_ = 0;
    int currentTagId_ = 0;
    int selectedLinkId_ = 0;
    int selectedTodoId_ = 0;
    bool selectionByKeyboard_ = false;
    HitKind menuContextKind_ = HitKind::None;
    int menuContextId_ = 0;
    HitArea hover_;
    std::vector<HitArea> hitAreas_;
    float groupScrollOffset_ = 0.0f;
    float tagScrollOffset_ = 0.0f;
    float linkScrollOffset_ = 0.0f;
    std::vector<int> menuMoveTargetIds_;
    std::vector<int> menuCopyTargetIds_;
    std::vector<int> menuGroupTargetIds_;
    std::vector<std::wstring> menuToolEngines_;
    std::vector<bool> menuToolEnabled_;
    std::vector<std::pair<int, int>> registeredLinkHotKeys_;
    Link clipboardLink_;
    bool hasClipboardLink_ = false;
    bool clipboardCut_ = false;
    int clipboardSourceId_ = 0;
    std::wstring tooltipText_;
    HitKind tooltipItemKind_ = HitKind::None;
    int tooltipItemId_ = 0;
    bool trackingMouse_ = false;
    bool dragOver_ = false;
    int linkDragCandidateId_ = 0;
    int linkDragId_ = 0;
    POINT linkDragStartPoint_{};
    POINT linkDragCurrentPoint_{};
    bool linkDragActive_ = false;
    LinkDragMode linkDragMode_ = LinkDragMode::None;
    int linkDragTargetLinkId_ = 0;
    int linkDragInsertIndex_ = -1;
    bool trayIconVisible_ = false;
    bool hotKeysRegistered_ = false;
    bool mainHotKeyRegistered_ = false;
    bool runningAsAdmin_ = false;
    bool exitingForPrivilegeRestart_ = false;
    bool startupFirstPaintLogged_ = false;
    bool startupActivationPosted_ = false;
    bool startupDeferredPosted_ = false;
    bool startupTopMostPending_ = false;
    bool windowStateSaveEnabled_ = false;
    bool updateDownloadActive_ = false;
    bool dockHidden_ = false;
    RECT dockRestoreRect_{};
    HWND dockPeek_ = nullptr;
    UINT_PTR dockTimerId_ = 0;
    ULONGLONG dockHideDueTick_ = 0;
    int dockAutoHidePauseDepth_ = 0;
    HitKind pendingHoverActivationKind_ = HitKind::None;
    int pendingHoverActivationId_ = 0;
    UINT_PTR hoverActivationTimerId_ = 0;
    OleDropTarget* oleDropTarget_ = nullptr;
    HWND noteEdit_ = nullptr;
    int noteEditTagId_ = 0;
    RECT noteEditFrame_{};
    std::unique_ptr<ThemedWindowUi> embeddedUi_;
    bool noteDirty_ = false;
    UINT_PTR noteSaveTimerId_ = 0;
    UINT_PTR reminderScanTimerId_ = 0;
    UINT_PTR reminderPanelTimerId_ = 0;
    HWND reminderPanel_ = nullptr;
    HFONT reminderPanelFont_ = nullptr;
    std::unordered_set<std::wstring> shownReminderKeys_;

    ID2D1Factory* d2dFactory_ = nullptr;
    IDWriteFactory* dwriteFactory_ = nullptr;
    IWICImagingFactory* uiWicFactory_ = nullptr;
    ID2D1HwndRenderTarget* renderTarget_ = nullptr;
    IDWriteTextFormat* titleFormat_ = nullptr;
    IDWriteTextFormat* textFormat_ = nullptr;
    IDWriteTextFormat* navSelectedFormat_ = nullptr;
    IDWriteTextFormat* smallFormat_ = nullptr;
    std::unordered_map<std::wstring, ID2D1Bitmap*> uiBitmapCache_;
    std::vector<std::unique_ptr<MenuItemData>> activeMenuItems_;
};
