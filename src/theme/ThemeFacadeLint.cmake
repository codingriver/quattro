set(WINDOWS_DIR "${SOURCE_DIR}/src/windows")
file(GLOB WINDOW_SOURCES "${WINDOWS_DIR}/*.cpp")
file(GLOB APP_LAUNCH_LOCKER_UI_SOURCES "${SOURCE_DIR}/src/applaunchlocker/*Window.cpp")
list(APPEND WINDOW_SOURCES ${APP_LAUNCH_LOCKER_UI_SOURCES})

set(FORBIDDEN_PATTERN
    "ThemedControls::Create(SingleLineEdit|MultiLineEdit|CheckBox|ComboBox|ListBox|StaticText|LabelText|ProgressBar|LinkText|GroupBox|TabControlFrame|ToolBarFrame)|ThemedControls::Draw(FieldFrame|PanelFrame|TabGroupFrame)|ThemedControls::ApplyListViewTheme|ThemedControls::RegisterTable|WC_LISTVIEW|ListView_|LVS_|LVCOLUMN|LVITEM|WC_TABCONTROL|TabCtrl_|TCM_|TOOLBARCLASSNAME|BS_GROUPBOX")

foreach(SOURCE_FILE IN LISTS WINDOW_SOURCES)
    file(READ "${SOURCE_FILE}" SOURCE_TEXT)
    string(REGEX MATCHALL "${FORBIDDEN_PATTERN}" MATCHES "${SOURCE_TEXT}")
    list(LENGTH MATCHES MATCH_COUNT)
    get_filename_component(FILE_NAME "${SOURCE_FILE}" NAME)
    if(MATCH_COUNT GREATER 0)
        message(FATAL_ERROR
            "Theme facade violation: ${FILE_NAME} introduces ${MATCH_COUNT} low-level control calls. Use ThemedUi.")
    endif()

    string(REGEX MATCH "[.]SingleLineEdit\\(|[.]MultiLineEdit\\(|[.]FramedStatic\\([^;]*SS_" LEGACY_FACADE_MATCH "${SOURCE_TEXT}")
    if(LEGACY_FACADE_MATCH)
        message(FATAL_ERROR
            "Theme facade violation: ${FILE_NAME} uses a legacy ThemedUi style-based API. Use semantic options.")
    endif()

    string(REGEX MATCH "BM_SETCHECK|PBM_SETPOS|PBM_SETSTATE|ThemedControls::SetTabButtonSelected|ThemedControls::SetProgressBarValue" DIRECT_STATE_MATCH "${SOURCE_TEXT}")
    if(DIRECT_STATE_MATCH)
        message(FATAL_ERROR
            "Theme facade violation: ${FILE_NAME} updates common control state through a low-level API. Use ThemedUi semantic state helpers.")
    endif()

    # User-visible custom painting in business windows must go through the
    # semantic ThemedUi/ThemedPaint facade. Keep Win32 paint lifecycle and
    # bitmap-handle interop available, but reject direct visual GDI primitives.
    set(GDI_SCAN_TEXT "${SOURCE_TEXT}")
    string(REPLACE "renderTarget_->DrawTextW" "D2D_RENDER_TARGET_DRAW_TEXT" GDI_SCAN_TEXT "${GDI_SCAN_TEXT}")
    string(REPLACE "ImageList/DrawIconEx" "IMAGE_LIST_ICON_BRIDGE" GDI_SCAN_TEXT "${GDI_SCAN_TEXT}")
    string(REPLACE "D2D1::Ellipse" "D2D_ELLIPSE" GDI_SCAN_TEXT "${GDI_SCAN_TEXT}")
    string(REPLACE "FillEllipse" "D2D_FILL_ELLIPSE" GDI_SCAN_TEXT "${GDI_SCAN_TEXT}")
    string(REPLACE "FillRoundedRectangle" "D2D_FILL_ROUNDED_RECT" GDI_SCAN_TEXT "${GDI_SCAN_TEXT}")
    string(REPLACE "DrawRoundedRectangle" "D2D_DRAW_ROUNDED_RECT" GDI_SCAN_TEXT "${GDI_SCAN_TEXT}")
    string(REPLACE "FillRectangle" "D2D_FILL_RECT" GDI_SCAN_TEXT "${GDI_SCAN_TEXT}")
    string(REPLACE "DrawRectangle" "D2D_DRAW_RECT" GDI_SCAN_TEXT "${GDI_SCAN_TEXT}")
    string(REPLACE "MainWindow::FillRect" "MAIN_WINDOW_D2D_FILL_RECT" GDI_SCAN_TEXT "${GDI_SCAN_TEXT}")
    string(REPLACE ".DrawPolyline" ".THEMED_DRAW_POLYLINE" GDI_SCAN_TEXT "${GDI_SCAN_TEXT}")
    string(REGEX MATCH
        "CreateSolidBrush[ ]*[(]|CreatePen[ ]*[(]|FrameRect[ ]*[(]|RoundRect[ ]*[(]|Rectangle[ ]*[(]|Ellipse[ ]*[(]|Polygon[ ]*[(]|Polyline[ ]*[(]|DrawTextW[ ]*[(]|TextOutW[ ]*[(]|ExtTextOutW[ ]*[(]|GradientFill[ ]*[(]|AlphaBlend[ ]*[(]|TransparentBlt[ ]*[(]|BitBlt[ ]*[(]|StretchBlt[ ]*[(]|DrawIconEx[ ]*[(]|DrawFrameControl[ ]*[(]|::FillRect[ ]*[(]|FillRect[ ]*[(][ ]*(dc|draw->hDC)"
        DIRECT_VISUAL_GDI_MATCH
        "${GDI_SCAN_TEXT}")
    if(DIRECT_VISUAL_GDI_MATCH)
        message(FATAL_ERROR
            "Theme facade violation: ${FILE_NAME} draws user-visible pixels through GDI (${DIRECT_VISUAL_GDI_MATCH}). Use ThemedPaint; GDI is allowed only inside ThemedGdiFallback or an approved interop adapter.")
    endif()
endforeach()

set(POPUP_MENU_CALL_SOURCES ${WINDOW_SOURCES} "${SOURCE_DIR}/src/services/ShellItemService.cpp")
foreach(SOURCE_FILE IN LISTS POPUP_MENU_CALL_SOURCES)
    if(NOT EXISTS "${SOURCE_FILE}")
        continue()
    endif()
    file(READ "${SOURCE_FILE}" POPUP_MENU_SOURCE_TEXT)
    string(REGEX MATCH "TrackPopupMenu(Ex)?[ ]*[(]" DIRECT_POPUP_MENU_MATCH "${POPUP_MENU_SOURCE_TEXT}")
    if(DIRECT_POPUP_MENU_MATCH)
        get_filename_component(FILE_NAME "${SOURCE_FILE}" NAME)
        message(FATAL_ERROR
            "Theme facade violation: ${FILE_NAME} bypasses ThemedUi::ShowPopupMenu.")
    endif()
endforeach()

foreach(SOURCE_FILE IN LISTS APP_LAUNCH_LOCKER_UI_SOURCES)
    file(READ "${SOURCE_FILE}" APP_LAUNCH_LOCKER_UI_TEXT)
    string(REGEX MATCH "CB_(ADDSTRING|RESETCONTENT|SETCURSEL|GETCURSEL)|MessageBoxW[(]" APP_LAUNCH_LOCKER_BYPASS "${APP_LAUNCH_LOCKER_UI_TEXT}")
    if(APP_LAUNCH_LOCKER_BYPASS)
        get_filename_component(FILE_NAME "${SOURCE_FILE}" NAME)
        message(FATAL_ERROR
            "Theme facade violation: ${FILE_NAME} bypasses AppLaunchLocker common UI state/dialog APIs: ${APP_LAUNCH_LOCKER_BYPASS}")
    endif()
endforeach()

# Keep the public 4px-grid scale enforceable. These checks intentionally target
# the shared theme and known legacy layout formulas instead of banning ordinary
# business numbers such as IDs, widths, or data-dependent visual content.
set(DEFAULT_THEME "${SOURCE_DIR}/theme/default.xml")
if(EXISTS "${DEFAULT_THEME}")
    file(READ "${DEFAULT_THEME}" DEFAULT_THEME_TEXT)
    foreach(REQUIRED_METRIC IN ITEMS
        "captionLineHeight\" value=\"16"
        "bodyLineHeight\" value=\"20"
        "titleLineHeight\" value=\"24"
        "smallControlHeight\" value=\"24"
        "mediumControlHeight\" value=\"28"
        "largeControlHeight\" value=\"32"
        "denseGap\" value=\"4"
        "titleInsetY\" value=\"4"
        "compactRowGap\" value=\"6"
        "standardRowGap\" value=\"8"
        "sectionGap\" value=\"12"
        "majorSectionGap\" value=\"16")
        string(FIND "${DEFAULT_THEME_TEXT}" "${REQUIRED_METRIC}" REQUIRED_METRIC_INDEX)
        if(REQUIRED_METRIC_INDEX LESS 0)
            message(FATAL_ERROR "Theme scale violation: missing canonical metric ${REQUIRED_METRIC}")
        endif()
    endforeach()
endif()

set(BUILTIN_TOOLS_SOURCE "${WINDOWS_DIR}/BuiltinTools.cpp")
if(EXISTS "${BUILTIN_TOOLS_SOURCE}")
    file(READ "${BUILTIN_TOOLS_SOURCE}" BUILTIN_TOOLS_TEXT)
    string(FIND "${BUILTIN_TOOLS_TEXT}" "max(40, ThemedControls::ListBoxItemHeight" LEGACY_RESULT_ROW)
    if(LEGACY_RESULT_ROW GREATER_EQUAL 0)
        message(FATAL_ERROR "Theme layout violation: BuiltinTools must use the shared two-line list item height.")
    endif()
endif()

set(TODO_SOURCE "${WINDOWS_DIR}/TodoEditDialog.cpp")
if(EXISTS "${TODO_SOURCE}")
    file(READ "${TODO_SOURCE}" TODO_TEXT)
    string(FIND "${TODO_TEXT}" "MetricInt(L\"global\", L\"rowGap\"" LEGACY_TODO_ROW_GAP)
    if(LEGACY_TODO_ROW_GAP GREATER_EQUAL 0)
        message(FATAL_ERROR "Theme layout violation: TodoEditDialog must use DialogLayoutMetrics row spacing.")
    endif()
endif()

set(SETTINGS_SOURCE "${WINDOWS_DIR}/SimpleDialogs.cpp")
if(EXISTS "${SETTINGS_SOURCE}")
    file(READ "${SETTINGS_SOURCE}" SIMPLE_DIALOGS_TEXT)
    string(FIND "${SIMPLE_DIALOGS_TEXT}" "class SettingsDialog" SETTINGS_BEGIN)
    string(FIND "${SIMPLE_DIALOGS_TEXT}" "bool ShowTextInputDialog" SETTINGS_END)
    if(SETTINGS_BEGIN GREATER_EQUAL 0 AND SETTINGS_END GREATER SETTINGS_BEGIN)
        math(EXPR SETTINGS_LENGTH "${SETTINGS_END} - ${SETTINGS_BEGIN}")
        string(SUBSTRING "${SIMPLE_DIALOGS_TEXT}" ${SETTINGS_BEGIN} ${SETTINGS_LENGTH} SETTINGS_TEXT)
        string(REGEX MATCH
            "theme_[.](color|metric)|ThemedControls::|CreateSolidBrush|SetTextColor|SetBkColor|BM_GETCHECK|BM_SETCHECK|RegisterClassExW|CreateWindowExW|ES_(AUTOHSCROLL|NUMBER|PASSWORD|READONLY)|EnableWindow[(]|InvalidateRect[(]|UsePanelBackground|PaintTabs[(]|PaintSectionFrames[(]"
            SETTINGS_VISUAL_MATCH
            "${SETTINGS_TEXT}")
        if(SETTINGS_VISUAL_MATCH)
            message(FATAL_ERROR
                "Theme facade violation: SettingsDialog bypasses public window, component, state, or visual APIs: ${SETTINGS_VISUAL_MATCH}")
        endif()
        string(REGEX MATCH
            "(CheckBox|Label|StatusBadge|Button|FramedEdit|FramedStatic|NumberEdit|Toggle)[(]Tab[A-Za-z]+,[^\n]*,[ ]*[0-9]+,[ ]*[0-9]+"
            SETTINGS_LITERAL_GEOMETRY_MATCH
            "${SETTINGS_TEXT}")
        if(SETTINGS_LITERAL_GEOMETRY_MATCH)
            message(FATAL_ERROR
                "Theme facade violation: SettingsDialog uses literal page geometry instead of public layout results: ${SETTINGS_LITERAL_GEOMETRY_MATCH}")
        endif()
        string(FIND "${SETTINGS_TEXT}" "ThemedWindowUi::HandleCommonMessage" SETTINGS_COMMON_MESSAGE)
        if(SETTINGS_COMMON_MESSAGE LESS 0)
            message(FATAL_ERROR
                "Theme facade violation: SettingsDialog must use ThemedWindowUi::HandleCommonMessage.")
        endif()
    endif()
endif()

message(STATUS "Theme facade lint passed")
