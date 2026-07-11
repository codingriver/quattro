cmake_minimum_required(VERSION 3.20)

set(QUATTRO_VERSION "0.1.0" CACHE STRING "Quattro package version")
project(Quattro VERSION ${QUATTRO_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(QUATTRO_BUILD_TESTS "Build local Quattro test tools" OFF)
option(QUATTRO_DEFAULT_LOGGING_ENABLED "Enable logging by default for new configs" ON)

find_package(CURL QUIET)
find_package(httplib CONFIG QUIET)

set(QUATTRO_WITH_CURL OFF)
if(CURL_FOUND)
    set(QUATTRO_WITH_CURL ON)
endif()

add_custom_target(QuattroThemeFacadeLint
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/src/theme/ThemeFacadeLint.cmake
    VERBATIM
)

set(QUATTRO_WITH_HTTPLIB OFF)
if(httplib_FOUND)
    set(QUATTRO_WITH_HTTPLIB ON)
else()
    message(WARNING "cpp-httplib was not found. Local HTTP server UI will build, but the server backend will report that the dependency is unavailable.")
endif()

set(QUATTRO_WITH_WEBDAV_LIBS OFF)
if(CURL_FOUND)
    set(QUATTRO_WITH_WEBDAV_LIBS ON)
else()
    message(WARNING "CURL was not found. WebDAV backup UI will build, but network operations will report that the third-party backend is unavailable.")
endif()

set(EMBEDDED_ASSET_INPUTS)

file(GLOB_RECURSE EMBEDDED_ASSET_DIR_INPUTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/theme/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/icons/menu/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/icons/url/*"
)
list(APPEND EMBEDDED_ASSET_INPUTS ${EMBEDDED_ASSET_DIR_INPUTS})

set(EMBEDDED_ASSETS_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedAssets.cpp")
set(EMBEDDED_UPDATER_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedUpdater.cpp")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h.in" "${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/VersionResource.h.in" "${CMAKE_CURRENT_BINARY_DIR}/generated/VersionResource.h" @ONLY)

set(QUATTRO_SOURCE_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/domain"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/services"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/theme"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/windows"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

include_directories(${QUATTRO_SOURCE_INCLUDE_DIRS})

add_executable(QuattroUpdater WIN32
    src/updater/UpdaterMain.cpp
)

target_compile_definitions(QuattroUpdater PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

target_compile_options(QuattroUpdater PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_link_libraries(QuattroUpdater PRIVATE
    user32
)

target_link_options(QuattroUpdater PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/MANIFEST:EMBED>
)

add_custom_command(
    OUTPUT "${EMBEDDED_UPDATER_CPP}"
    COMMAND powershell -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-embedded-updater.ps1"
            -Updater "$<TARGET_FILE:QuattroUpdater>"
            -Output "${EMBEDDED_UPDATER_CPP}"
    DEPENDS
        QuattroUpdater
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-embedded-updater.ps1"
    COMMENT "Embedding updater executable"
    VERBATIM
)

add_custom_command(
    OUTPUT "${EMBEDDED_ASSETS_CPP}"
    COMMAND powershell -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-embedded-assets.ps1"
            -Root "${CMAKE_CURRENT_SOURCE_DIR}"
            -Output "${EMBEDDED_ASSETS_CPP}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-embedded-assets.ps1"
        ${EMBEDDED_ASSET_INPUTS}
    COMMENT "Generating embedded default assets"
    VERBATIM
)

add_library(sqlite3 STATIC
    third_party/sqlite/sqlite3.c
)

add_library(ccronexpr STATIC
    third_party/ccronexpr/ccronexpr.c
)

add_library(pugixml STATIC
    third_party/pugixml/pugixml.cpp
)

target_include_directories(sqlite3 PUBLIC
    third_party/sqlite
)

target_include_directories(ccronexpr PUBLIC
    third_party/ccronexpr
)

target_include_directories(pugixml PUBLIC
    third_party/pugixml
)

target_compile_definitions(sqlite3 PRIVATE
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_THREADSAFE=1
)

target_compile_definitions(ccronexpr PRIVATE
    CRON_USE_LOCAL_TIME
    _CRT_SECURE_NO_WARNINGS
)

target_compile_options(sqlite3 PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/W0 /utf-8>
)

target_compile_options(ccronexpr PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/W0 /utf-8>
)

target_compile_options(pugixml PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W0 /utf-8>
)

add_executable(Quattro WIN32
    ${EMBEDDED_ASSETS_CPP}
    ${EMBEDDED_UPDATER_CPP}
    resources/Quattro.rc
    src/main.cpp
    src/windows/AboutDialog.cpp
    src/common/AppLog.cpp
    src/windows/BuiltinTools.cpp
    src/theme/CatalogDialogLayout.cpp
    src/windows/ConfirmDialog.cpp
    src/domain/Config.cpp
    src/services/ConfigPackageService.cpp
    src/common/EmbeddedAssetInstaller.cpp
    src/common/Elevation.cpp
    src/windows/HotKeyEditor.cpp
    src/services/IconService.cpp
    src/common/JsonValue.cpp
    src/domain/LinkSorting.cpp
    src/services/Launcher.cpp
    src/services/LocalHttpServerService.cpp
    src/windows/LinkEditDialog.cpp
    src/windows/MainHotKey.cpp
    src/windows/MainWindow.cpp
    src/domain/MenuCatalog.cpp
    src/domain/PluginRegistry.cpp
    src/windows/QuickImportDialog.cpp
    src/services/QuickImportService.cpp
    src/services/ShellItemService.cpp
    src/windows/SimpleDialogs.cpp
    src/services/StartupService.cpp
    src/services/Storage.cpp
    src/services/SystemFunctions.cpp
    src/theme/Theme.cpp
    src/theme/ThemedFormLayout.cpp
    src/theme/ThemedControls.cpp
    src/theme/ThemedUi.cpp
    src/theme/ThemedWindowUi.cpp
    src/windows/TodoEditDialog.cpp
    src/domain/TodoSchedule.cpp
    src/services/UpdateCheckService.cpp
    src/windows/UpdateCheckDialog.cpp
    src/windows/UpdateDownloadDialog.cpp
    src/services/UpdateInstaller.cpp
    src/windows/UrlEditDialog.cpp
    src/services/UrlIconDownloadService.cpp
    src/common/Utilities.cpp
    src/services/WebDavBackupService.cpp
    src/services/WebDavClient.cpp
    src/services/WebDavCredentialService.cpp
    src/services/WebDavRecoveryService.cpp
)

set_target_properties(Quattro PROPERTIES
    OUTPUT_NAME "Quattro"
)

target_compile_definitions(Quattro PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)
target_compile_definitions(Quattro PRIVATE
    QUATTRO_DEFAULT_LOGGING_ENABLED=$<IF:$<BOOL:${QUATTRO_DEFAULT_LOGGING_ENABLED}>,1,0>
)

if(QUATTRO_WITH_WEBDAV_LIBS)
    target_compile_definitions(Quattro PRIVATE QUATTRO_WITH_WEBDAV_LIBS)
endif()
if(QUATTRO_WITH_CURL)
    target_compile_definitions(Quattro PRIVATE QUATTRO_WITH_CURL)
endif()
if(QUATTRO_WITH_HTTPLIB)
    target_compile_definitions(Quattro PRIVATE QUATTRO_WITH_HTTPLIB)
endif()

target_compile_options(Quattro PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_include_directories(Quattro PRIVATE
    src
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

if(QUATTRO_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/UnitTests.cpp")
add_executable(QuattroTests
    tests/UnitTests.cpp
    src/common/AppLog.cpp
    src/domain/Config.cpp
    src/services/ConfigPackageService.cpp
    src/services/Launcher.cpp
    src/common/JsonValue.cpp
    src/domain/LinkSorting.cpp
    src/domain/MenuCatalog.cpp
    src/domain/PluginRegistry.cpp
    src/services/ShellItemService.cpp
    src/services/Storage.cpp
    src/services/SystemFunctions.cpp
    src/theme/Theme.cpp
    src/theme/ThemedControls.cpp
    src/theme/ThemedUi.cpp
    src/domain/TodoSchedule.cpp
    src/services/UpdateCheckService.cpp
    src/common/Utilities.cpp
    src/services/WebDavBackupService.cpp
    src/services/WebDavClient.cpp
    src/services/WebDavCredentialService.cpp
    src/services/WebDavRecoveryService.cpp
)

add_executable(QuattroNoteTodoAcceptance
    tests/NoteTodoAcceptance.cpp
    src/common/AppLog.cpp
    src/domain/Config.cpp
    src/services/ConfigPackageService.cpp
    src/common/JsonValue.cpp
    src/domain/MenuCatalog.cpp
    src/services/Storage.cpp
    src/domain/TodoSchedule.cpp
    src/common/Utilities.cpp
)

add_executable(QuattroDbProbe
    tests/DbProbe.cpp
    src/common/AppLog.cpp
    src/services/Storage.cpp
    src/domain/TodoSchedule.cpp
    src/common/Utilities.cpp
)

add_executable(QuattroDbSeed
    tests/DbSeed.cpp
    src/common/AppLog.cpp
    src/services/Storage.cpp
    src/domain/TodoSchedule.cpp
    src/common/Utilities.cpp
)

add_executable(QuattroSortScreenshotSeed
    tests/SortScreenshotSeed.cpp
    src/common/AppLog.cpp
    src/services/Storage.cpp
    src/domain/TodoSchedule.cpp
    src/common/Utilities.cpp
)

add_executable(QuattroThemeStateProbe
    tests/ThemeStateProbe.cpp
    src/theme/Theme.cpp
    src/theme/ThemedControls.cpp
    src/theme/ThemedUi.cpp
    src/common/Utilities.cpp
)

add_executable(QuattroUiScreenshotAcceptance
    tests/UiScreenshotAcceptance.cpp
    src/common/AppLog.cpp
    src/windows/AboutDialog.cpp
    src/windows/BuiltinTools.cpp
    src/windows/ConfirmDialog.cpp
    src/services/ConfigPackageService.cpp
    src/windows/HotKeyEditor.cpp
    src/common/JsonValue.cpp
    src/services/Launcher.cpp
    src/services/LocalHttpServerService.cpp
    src/windows/LinkEditDialog.cpp
    src/windows/MainHotKey.cpp
    src/domain/MenuCatalog.cpp
    src/domain/PluginRegistry.cpp
    src/windows/QuickImportDialog.cpp
    src/services/QuickImportService.cpp
    src/services/ShellItemService.cpp
    src/windows/SimpleDialogs.cpp
    src/services/Storage.cpp
    src/services/SystemFunctions.cpp
    src/theme/Theme.cpp
    src/theme/ThemedControls.cpp
    src/theme/ThemedFormLayout.cpp
    src/theme/ThemedUi.cpp
    src/theme/ThemedWindowUi.cpp
    src/windows/TodoEditDialog.cpp
    src/domain/TodoSchedule.cpp
    src/windows/UpdateCheckDialog.cpp
    src/windows/UrlEditDialog.cpp
    src/common/Utilities.cpp
    src/services/WebDavBackupService.cpp
    src/services/WebDavClient.cpp
    src/services/WebDavCredentialService.cpp
    src/services/WebDavRecoveryService.cpp
)

add_executable(QuattroAboutTrayExitAcceptance
    tests/AboutTrayExitAcceptance.cpp
)

add_executable(QuattroBuiltinSystemContextMenuAcceptance
    tests/BuiltinSystemContextMenuAcceptance.cpp
    src/common/AppLog.cpp
    src/services/Storage.cpp
    src/domain/TodoSchedule.cpp
    src/common/Utilities.cpp
)

add_executable(QuattroWebDavAcceptance
    tests/WebDavAcceptance.cpp
    src/common/AppLog.cpp
    src/domain/Config.cpp
    src/services/ConfigPackageService.cpp
    src/services/Storage.cpp
    src/domain/TodoSchedule.cpp
    src/common/Utilities.cpp
    src/services/WebDavBackupService.cpp
    src/services/WebDavClient.cpp
    src/services/WebDavCredentialService.cpp
    src/services/WebDavRecoveryService.cpp
)

target_compile_definitions(QuattroTests PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

if(QUATTRO_WITH_CURL)
    target_compile_definitions(QuattroTests PRIVATE QUATTRO_WITH_CURL)
endif()

target_compile_definitions(QuattroNoteTodoAcceptance PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

target_compile_definitions(QuattroDbProbe PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

target_compile_definitions(QuattroDbSeed PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

target_compile_definitions(QuattroSortScreenshotSeed PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

target_compile_definitions(QuattroThemeStateProbe PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

target_compile_definitions(QuattroUiScreenshotAcceptance PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

target_compile_definitions(QuattroAboutTrayExitAcceptance PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

target_compile_definitions(QuattroBuiltinSystemContextMenuAcceptance PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

target_compile_definitions(QuattroWebDavAcceptance PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

if(QUATTRO_WITH_WEBDAV_LIBS)
    target_compile_definitions(QuattroTests PRIVATE QUATTRO_WITH_WEBDAV_LIBS)
    target_compile_definitions(QuattroUiScreenshotAcceptance PRIVATE QUATTRO_WITH_WEBDAV_LIBS)
    target_compile_definitions(QuattroWebDavAcceptance PRIVATE QUATTRO_WITH_WEBDAV_LIBS)
endif()

target_compile_options(QuattroTests PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_compile_options(QuattroNoteTodoAcceptance PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_compile_options(QuattroDbProbe PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_compile_options(QuattroDbSeed PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_compile_options(QuattroSortScreenshotSeed PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_compile_options(QuattroThemeStateProbe PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_compile_options(QuattroUiScreenshotAcceptance PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_compile_options(QuattroAboutTrayExitAcceptance PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_compile_options(QuattroBuiltinSystemContextMenuAcceptance PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_compile_options(QuattroWebDavAcceptance PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_include_directories(QuattroTests PRIVATE
    src
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_include_directories(QuattroThemeStateProbe PRIVATE
    src
)

target_link_libraries(QuattroTests PRIVATE
    sqlite3
    ccronexpr
    advapi32
    bcrypt
    comctl32
    gdi32
    ole32
    rstrtmgr
    shell32
    shlwapi
    user32
    wininet
)

if(QUATTRO_WITH_WEBDAV_LIBS)
    target_link_libraries(QuattroTests PRIVATE CURL::libcurl pugixml)
endif()

target_link_libraries(QuattroNoteTodoAcceptance PRIVATE
    sqlite3
    ccronexpr
    bcrypt
    shell32
    shlwapi
    user32
    wininet
)

target_link_libraries(QuattroDbProbe PRIVATE
    sqlite3
    ccronexpr
    user32
)

target_link_libraries(QuattroDbSeed PRIVATE
    sqlite3
    ccronexpr
    user32
)

target_link_libraries(QuattroSortScreenshotSeed PRIVATE
    sqlite3
    ccronexpr
    user32
)

target_link_libraries(QuattroThemeStateProbe PRIVATE
    comctl32
    gdiplus
    gdi32
    shell32
    user32
)

target_link_libraries(QuattroUiScreenshotAcceptance PRIVATE
    sqlite3
    ccronexpr
    advapi32
    bcrypt
    comctl32
    gdiplus
    gdi32
    iphlpapi
    ole32
    rstrtmgr
    shell32
    shlwapi
    user32
    wininet
)

target_link_libraries(QuattroAboutTrayExitAcceptance PRIVATE
    user32
)

target_link_libraries(QuattroBuiltinSystemContextMenuAcceptance PRIVATE
    sqlite3
    ccronexpr
    user32
)

if(QUATTRO_WITH_WEBDAV_LIBS)
    target_link_libraries(QuattroUiScreenshotAcceptance PRIVATE CURL::libcurl pugixml)
endif()

target_link_libraries(QuattroWebDavAcceptance PRIVATE
    sqlite3
    ccronexpr
    advapi32
    user32
)

if(QUATTRO_WITH_WEBDAV_LIBS)
    target_link_libraries(QuattroWebDavAcceptance PRIVATE CURL::libcurl pugixml)
endif()
elseif(QUATTRO_BUILD_TESTS)
    message(WARNING "QUATTRO_BUILD_TESTS is ON, but local tests/ sources were not found. Test targets will be skipped.")
endif()

if(QUATTRO_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/TodoBackupJsonCompatibility.cpp")
add_executable(QuattroTodoBackupJsonCompatibility
    tests/TodoBackupJsonCompatibility.cpp
    src/common/JsonValue.cpp
    src/common/Utilities.cpp
)

target_compile_definitions(QuattroTodoBackupJsonCompatibility PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

target_compile_options(QuattroTodoBackupJsonCompatibility PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_include_directories(QuattroTodoBackupJsonCompatibility PRIVATE
    src
)
endif()

if(QUATTRO_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/HttpServerConfigAcceptance.cpp")
add_executable(QuattroHttpServerConfigAcceptance
    tests/HttpServerConfigAcceptance.cpp
    src/services/LocalHttpServerService.cpp
    src/common/Utilities.cpp
)

target_compile_definitions(QuattroHttpServerConfigAcceptance PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

target_compile_options(QuattroHttpServerConfigAcceptance PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_include_directories(QuattroHttpServerConfigAcceptance PRIVATE
    src
)

target_link_libraries(QuattroHttpServerConfigAcceptance PRIVATE
    shell32
    user32
)
endif()

if(QUATTRO_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/HttpServerRuntimeAcceptance.cpp")
add_executable(QuattroHttpServerRuntimeAcceptance
    tests/HttpServerRuntimeAcceptance.cpp
    src/common/AppLog.cpp
    src/services/LocalHttpServerService.cpp
    src/common/Utilities.cpp
)

target_compile_definitions(QuattroHttpServerRuntimeAcceptance PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

if(QUATTRO_WITH_HTTPLIB)
    target_compile_definitions(QuattroHttpServerRuntimeAcceptance PRIVATE QUATTRO_WITH_HTTPLIB)
endif()

target_compile_options(QuattroHttpServerRuntimeAcceptance PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_include_directories(QuattroHttpServerRuntimeAcceptance PRIVATE
    src
)

target_link_libraries(QuattroHttpServerRuntimeAcceptance PRIVATE
    shell32
    user32
)

if(QUATTRO_WITH_HTTPLIB)
    target_link_libraries(QuattroHttpServerRuntimeAcceptance PRIVATE httplib::httplib)
endif()
endif()

if(QUATTRO_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/SettingsHttpButtonsAcceptance.cpp")
add_executable(QuattroSettingsHttpButtonsAcceptance
    tests/SettingsHttpButtonsAcceptance.cpp
    src/common/AppLog.cpp
    src/windows/AboutDialog.cpp
    src/windows/BuiltinTools.cpp
    src/windows/ConfirmDialog.cpp
    src/services/ConfigPackageService.cpp
    src/windows/HotKeyEditor.cpp
    src/common/JsonValue.cpp
    src/services/Launcher.cpp
    src/services/LocalHttpServerService.cpp
    src/windows/LinkEditDialog.cpp
    src/windows/MainHotKey.cpp
    src/domain/MenuCatalog.cpp
    src/domain/PluginRegistry.cpp
    src/windows/QuickImportDialog.cpp
    src/services/QuickImportService.cpp
    src/services/ShellItemService.cpp
    src/windows/SimpleDialogs.cpp
    src/services/Storage.cpp
    src/services/SystemFunctions.cpp
    src/theme/Theme.cpp
    src/theme/ThemedControls.cpp
    src/theme/ThemedFormLayout.cpp
    src/theme/ThemedUi.cpp
    src/theme/ThemedWindowUi.cpp
    src/windows/TodoEditDialog.cpp
    src/domain/TodoSchedule.cpp
    src/windows/UpdateCheckDialog.cpp
    src/windows/UrlEditDialog.cpp
    src/common/Utilities.cpp
    src/services/WebDavBackupService.cpp
    src/services/WebDavClient.cpp
    src/services/WebDavCredentialService.cpp
    src/services/WebDavRecoveryService.cpp
)

target_compile_definitions(QuattroSettingsHttpButtonsAcceptance PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

if(QUATTRO_WITH_WEBDAV_LIBS)
    target_compile_definitions(QuattroSettingsHttpButtonsAcceptance PRIVATE QUATTRO_WITH_WEBDAV_LIBS)
endif()
if(QUATTRO_WITH_HTTPLIB)
    target_compile_definitions(QuattroSettingsHttpButtonsAcceptance PRIVATE QUATTRO_WITH_HTTPLIB)
endif()

target_compile_options(QuattroSettingsHttpButtonsAcceptance PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_include_directories(QuattroSettingsHttpButtonsAcceptance PRIVATE
    src
)

target_link_libraries(QuattroSettingsHttpButtonsAcceptance PRIVATE
    sqlite3
    ccronexpr
    advapi32
    bcrypt
    comctl32
    gdiplus
    gdi32
    iphlpapi
    ole32
    rstrtmgr
    shell32
    shlwapi
    user32
    wininet
)

if(QUATTRO_WITH_WEBDAV_LIBS)
    target_link_libraries(QuattroSettingsHttpButtonsAcceptance PRIVATE CURL::libcurl pugixml)
endif()
if(QUATTRO_WITH_HTTPLIB)
    target_link_libraries(QuattroSettingsHttpButtonsAcceptance PRIVATE httplib::httplib)
endif()
endif()

target_link_libraries(Quattro PRIVATE
    sqlite3
    ccronexpr
    advapi32
    bcrypt
    d2d1
    dwrite
    iphlpapi
    mpr
    ole32
    oleaut32
    rstrtmgr
    shell32
    shlwapi
    uuid
    wininet
    windowscodecs
    user32
    gdi32
    comctl32
)

if(QUATTRO_WITH_WEBDAV_LIBS)
    target_link_libraries(Quattro PRIVATE CURL::libcurl pugixml)
endif()
if(QUATTRO_WITH_HTTPLIB)
    target_link_libraries(Quattro PRIVATE httplib::httplib)
endif()

target_link_options(Quattro PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/MANIFEST:EMBED>
    $<$<CXX_COMPILER_ID:MSVC>:/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest>
)

add_custom_command(TARGET Quattro POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Quattro>/db"
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/icons")
    add_custom_command(TARGET Quattro POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/icons" "$<TARGET_FILE_DIR:Quattro>/icons"
        COMMAND ${CMAKE_COMMAND} -E rm -f "$<TARGET_FILE_DIR:Quattro>/icons/menu/tabler/README.md"
        COMMAND ${CMAKE_COMMAND} -E rm -f "$<TARGET_FILE_DIR:Quattro>/icons/menu/tabler/LICENSE"
    )
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/theme")
    add_custom_command(TARGET Quattro POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:Quattro>/theme"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/theme" "$<TARGET_FILE_DIR:Quattro>/theme"
    )
endif()

foreach(TARGET_NAME Quattro QuattroTests QuattroUiScreenshotAcceptance)
    if(TARGET ${TARGET_NAME})
        add_dependencies(${TARGET_NAME} QuattroThemeFacadeLint)
    endif()
endforeach()
