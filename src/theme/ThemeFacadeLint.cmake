set(WINDOWS_DIR "${SOURCE_DIR}/src/windows")
file(GLOB WINDOW_SOURCES "${WINDOWS_DIR}/*.cpp")

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
endforeach()

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
