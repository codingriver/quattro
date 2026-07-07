cmake_minimum_required(VERSION 3.20)

set(QUATTRO_VERSION "0.1.0" CACHE STRING "Quattro package version")
project(Quattro VERSION ${QUATTRO_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(QUATTRO_BUILD_TESTS "Build local Quattro test tools" OFF)

find_package(CURL QUIET)
find_package(httplib CONFIG QUIET)

set(QUATTRO_WITH_CURL OFF)
if(CURL_FOUND)
    set(QUATTRO_WITH_CURL ON)
endif()

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

set(EMBEDDED_ASSET_INPUTS
    "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
)

file(GLOB_RECURSE EMBEDDED_ASSET_DIR_INPUTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/theme/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/icons/menu/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/icons/url/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/*"
)
list(APPEND EMBEDDED_ASSET_INPUTS ${EMBEDDED_ASSET_DIR_INPUTS})

set(EMBEDDED_ASSETS_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedAssets.cpp")
set(EMBEDDED_UPDATER_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedUpdater.cpp")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h.in" "${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/VersionResource.h.in" "${CMAKE_CURRENT_BINARY_DIR}/generated/VersionResource.h" @ONLY)

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
    src/AppLog.cpp
    src/BuiltinTools.cpp
    src/CatalogDialogLayout.cpp
    src/Config.cpp
    src/ConfigPackageService.cpp
    src/EmbeddedAssetInstaller.cpp
    src/Elevation.cpp
    src/HotKeyEditor.cpp
    src/IconService.cpp
    src/JsonValue.cpp
    src/Launcher.cpp
    src/LocalHttpServerService.cpp
    src/LinkEditDialog.cpp
    src/MainWindow.cpp
    src/MenuCatalog.cpp
    src/PluginRegistry.cpp
    src/QuickImportDialog.cpp
    src/QuickImportService.cpp
    src/ShellItemService.cpp
    src/SimpleDialogs.cpp
    src/StartupService.cpp
    src/Storage.cpp
    src/SystemFunctions.cpp
    src/Theme.cpp
    src/ThemedControls.cpp
    src/TodoEditDialog.cpp
    src/TodoSchedule.cpp
    src/UpdateCheckService.cpp
    src/UpdateInstaller.cpp
    src/UrlEditDialog.cpp
    src/UrlIconDownloadService.cpp
    src/Utilities.cpp
    src/WebDavBackupService.cpp
    src/WebDavClient.cpp
    src/WebDavCredentialService.cpp
    src/WebDavRecoveryService.cpp
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
    src/AppLog.cpp
    src/Config.cpp
    src/ConfigPackageService.cpp
    src/Launcher.cpp
    src/JsonValue.cpp
    src/MenuCatalog.cpp
    src/PluginRegistry.cpp
    src/ShellItemService.cpp
    src/Storage.cpp
    src/SystemFunctions.cpp
    src/Theme.cpp
    src/ThemedControls.cpp
    src/TodoSchedule.cpp
    src/Utilities.cpp
    src/WebDavBackupService.cpp
    src/WebDavClient.cpp
    src/WebDavCredentialService.cpp
    src/WebDavRecoveryService.cpp
)

add_executable(QuattroNoteTodoAcceptance
    tests/NoteTodoAcceptance.cpp
    src/AppLog.cpp
    src/Config.cpp
    src/ConfigPackageService.cpp
    src/JsonValue.cpp
    src/MenuCatalog.cpp
    src/Storage.cpp
    src/TodoSchedule.cpp
    src/Utilities.cpp
)

add_executable(QuattroDbProbe
    tests/DbProbe.cpp
    src/AppLog.cpp
    src/Storage.cpp
    src/TodoSchedule.cpp
    src/Utilities.cpp
)

add_executable(QuattroDbSeed
    tests/DbSeed.cpp
    src/AppLog.cpp
    src/Storage.cpp
    src/TodoSchedule.cpp
    src/Utilities.cpp
)

add_executable(QuattroThemeStateProbe
    tests/ThemeStateProbe.cpp
    src/Theme.cpp
    src/ThemedControls.cpp
    src/Utilities.cpp
)

add_executable(QuattroUiScreenshotAcceptance
    tests/UiScreenshotAcceptance.cpp
    src/AppLog.cpp
    src/BuiltinTools.cpp
    src/ConfigPackageService.cpp
    src/HotKeyEditor.cpp
    src/JsonValue.cpp
    src/Launcher.cpp
    src/LocalHttpServerService.cpp
    src/LinkEditDialog.cpp
    src/MenuCatalog.cpp
    src/PluginRegistry.cpp
    src/QuickImportDialog.cpp
    src/QuickImportService.cpp
    src/ShellItemService.cpp
    src/SimpleDialogs.cpp
    src/Storage.cpp
    src/SystemFunctions.cpp
    src/Theme.cpp
    src/ThemedControls.cpp
    src/TodoEditDialog.cpp
    src/TodoSchedule.cpp
    src/UrlEditDialog.cpp
    src/Utilities.cpp
    src/WebDavBackupService.cpp
    src/WebDavClient.cpp
    src/WebDavCredentialService.cpp
    src/WebDavRecoveryService.cpp
)

add_executable(QuattroAboutTrayExitAcceptance
    tests/AboutTrayExitAcceptance.cpp
)

add_executable(QuattroWebDavAcceptance
    tests/WebDavAcceptance.cpp
    src/AppLog.cpp
    src/Config.cpp
    src/ConfigPackageService.cpp
    src/Storage.cpp
    src/TodoSchedule.cpp
    src/Utilities.cpp
    src/WebDavBackupService.cpp
    src/WebDavClient.cpp
    src/WebDavCredentialService.cpp
    src/WebDavRecoveryService.cpp
)

target_compile_definitions(QuattroTests PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

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

target_compile_options(QuattroThemeStateProbe PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_compile_options(QuattroUiScreenshotAcceptance PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_compile_options(QuattroAboutTrayExitAcceptance PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_compile_options(QuattroWebDavAcceptance PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
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
    shell32
    shlwapi
    user32
    wininet
)

target_link_libraries(QuattroAboutTrayExitAcceptance PRIVATE
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
    src/JsonValue.cpp
    src/Utilities.cpp
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
    src/LocalHttpServerService.cpp
    src/Utilities.cpp
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
    src/AppLog.cpp
    src/LocalHttpServerService.cpp
    src/Utilities.cpp
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
    src/AppLog.cpp
    src/BuiltinTools.cpp
    src/ConfigPackageService.cpp
    src/HotKeyEditor.cpp
    src/JsonValue.cpp
    src/Launcher.cpp
    src/LocalHttpServerService.cpp
    src/LinkEditDialog.cpp
    src/MenuCatalog.cpp
    src/PluginRegistry.cpp
    src/QuickImportDialog.cpp
    src/QuickImportService.cpp
    src/ShellItemService.cpp
    src/SimpleDialogs.cpp
    src/Storage.cpp
    src/SystemFunctions.cpp
    src/Theme.cpp
    src/ThemedControls.cpp
    src/TodoEditDialog.cpp
    src/TodoSchedule.cpp
    src/UrlEditDialog.cpp
    src/Utilities.cpp
    src/WebDavBackupService.cpp
    src/WebDavClient.cpp
    src/WebDavCredentialService.cpp
    src/WebDavRecoveryService.cpp
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
    ole32
    oleaut32
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
    )
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/theme")
    add_custom_command(TARGET Quattro POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:Quattro>/theme"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/theme" "$<TARGET_FILE_DIR:Quattro>/theme"
    )
endif()
