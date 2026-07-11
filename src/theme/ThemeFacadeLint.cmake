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

message(STATUS "Theme facade lint passed")
