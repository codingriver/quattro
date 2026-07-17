cmake_minimum_required(VERSION 3.20)

set(QUATTRO_VERSION "0.1.0" CACHE STRING "Quattro package version")
project(Quattro VERSION ${QUATTRO_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(
    $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/MP4>
)

option(QUATTRO_BUILD_TESTS "Build local Quattro test tools" OFF)
option(QUATTRO_DEFAULT_LOGGING_ENABLED "Enable logging by default for new configs" ON)
option(QUATTRO_DEFAULT_TOP_MOST_ENABLED "Keep the main window topmost by default for new configs" OFF)
option(QUATTRO_BUNDLE_OPTIONAL_EXECUTABLES "Embed AppLaunchLocker and QuattroUpdater in Quattro" OFF)
option(QUATTRO_OFFICIAL_BUILD "Build an official package without a development title marker" OFF)
option(QUATTRO_COMPRESS_EMBEDDED_ASSETS "Compress built-in theme and icon resources" OFF)

set(QUATTRO_BUILD_MARKER "")
set(QUATTRO_OFFICIAL_BUILD_VALUE 0)
if(QUATTRO_OFFICIAL_BUILD)
    set(QUATTRO_OFFICIAL_BUILD_VALUE 1)
elseif(QUATTRO_BUNDLE_OPTIONAL_EXECUTABLES)
    set(QUATTRO_BUILD_MARKER "DEBUG-All")
else()
    set(QUATTRO_BUILD_MARKER "DEBUG")
endif()

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

if(QUATTRO_COMPRESS_EMBEDDED_ASSETS)
    set(EMBEDDED_ASSET_MODE "xpress")
    set(QUATTRO_COMPRESS_EMBEDDED_ASSETS_VALUE 1)
else()
    set(EMBEDDED_ASSET_MODE "raw")
    set(QUATTRO_COMPRESS_EMBEDDED_ASSETS_VALUE 0)
endif()
set(EMBEDDED_ASSET_PACK "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedAssets.${EMBEDDED_ASSET_MODE}.pack")
set(EMBEDDED_ASSET_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedAssets.${EMBEDDED_ASSET_MODE}.rc")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h.in" "${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/VersionResource.h.in" "${CMAKE_CURRENT_BINARY_DIR}/generated/VersionResource.h" @ONLY)

function(quattro_register_embedded_executable)
    cmake_parse_arguments(COMPONENT "" "TARGET;SYMBOL;ID;FILE_NAME" "" ${ARGN})
    foreach(REQUIRED_ARGUMENT TARGET SYMBOL ID FILE_NAME)
        if(NOT COMPONENT_${REQUIRED_ARGUMENT})
            message(FATAL_ERROR "quattro_register_embedded_executable requires ${REQUIRED_ARGUMENT}")
        endif()
    endforeach()
    if(NOT TARGET ${COMPONENT_TARGET})
        message(FATAL_ERROR "Embedded executable target does not exist: ${COMPONENT_TARGET}")
    endif()
    if(NOT COMPONENT_SYMBOL MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "Invalid embedded executable symbol: ${COMPONENT_SYMBOL}")
    endif()

    set(GENERATED_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/Embedded${COMPONENT_SYMBOL}.cpp")
    add_custom_command(
        OUTPUT "${GENERATED_SOURCE}"
        COMMAND powershell -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-embedded-executable.ps1"
                -Executable "$<TARGET_FILE:${COMPONENT_TARGET}>"
                -Output "${GENERATED_SOURCE}"
                -Symbol "${COMPONENT_SYMBOL}"
                -Id "${COMPONENT_ID}"
                -FileName "${COMPONENT_FILE_NAME}"
                -ExpectedVersion "${QUATTRO_VERSION}"
        DEPENDS
            ${COMPONENT_TARGET}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-embedded-executable.ps1"
        COMMENT "Embedding ${COMPONENT_FILE_NAME}"
        VERBATIM
    )
    set_property(GLOBAL APPEND PROPERTY QUATTRO_EMBEDDED_EXECUTABLE_SOURCES "${GENERATED_SOURCE}")
    set_property(GLOBAL APPEND PROPERTY QUATTRO_EMBEDDED_EXECUTABLE_SYMBOLS "${COMPONENT_SYMBOL}")
endfunction()

function(quattro_generate_embedded_executable_catalog OUTPUT_VARIABLE)
    get_property(EMBEDDED_SOURCES GLOBAL PROPERTY QUATTRO_EMBEDDED_EXECUTABLE_SOURCES)
    get_property(EMBEDDED_SYMBOLS GLOBAL PROPERTY QUATTRO_EMBEDDED_EXECUTABLE_SYMBOLS)
    list(LENGTH EMBEDDED_SYMBOLS EMBEDDED_EXECUTABLE_COUNT)
    set(EMBEDDED_EXECUTABLE_DECLARATIONS "")
    set(EMBEDDED_EXECUTABLE_ENTRIES "")
    foreach(SYMBOL IN LISTS EMBEDDED_SYMBOLS)
        string(APPEND EMBEDDED_EXECUTABLE_DECLARATIONS
            "const EmbeddedExecutableDescriptor& EmbeddedExecutable_${SYMBOL}();\n")
        string(APPEND EMBEDDED_EXECUTABLE_ENTRIES
            "        &EmbeddedExecutable_${SYMBOL}(),\n")
    endforeach()

    set(GENERATED_CATALOG "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedExecutableCatalog.cpp")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/common/EmbeddedExecutableCatalog.cpp.in"
        "${GENERATED_CATALOG}"
        @ONLY
    )
    set(${OUTPUT_VARIABLE} ${EMBEDDED_SOURCES} "${GENERATED_CATALOG}" PARENT_SCOPE)
endfunction()

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
    resources/QuattroUpdater.rc
    src/updater/UpdaterMain.cpp
)
set_target_properties(QuattroUpdater PROPERTIES
    EXCLUDE_FROM_ALL TRUE
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

if(QUATTRO_BUNDLE_OPTIONAL_EXECUTABLES)
    quattro_register_embedded_executable(
        TARGET QuattroUpdater
        SYMBOL QuattroUpdater
        ID quattro-updater
        FILE_NAME QuattroUpdater.exe
    )
endif()

add_custom_command(
    OUTPUT "${EMBEDDED_ASSET_PACK}" "${EMBEDDED_ASSET_RESOURCE}"
    COMMAND powershell -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-embedded-asset-pack.ps1"
            -Root "${CMAKE_CURRENT_SOURCE_DIR}"
            -PackOutput "${EMBEDDED_ASSET_PACK}"
            -ResourceOutput "${EMBEDDED_ASSET_RESOURCE}"
            -ResourceHeader "${CMAKE_CURRENT_SOURCE_DIR}/resources/resource.h"
            -ResourceName IDR_QUATTRO_ASSET_PACK
            -Mode "${EMBEDDED_ASSET_MODE}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate-embedded-asset-pack.ps1"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/resource.h"
        ${EMBEDDED_ASSET_INPUTS}
    COMMENT "Generating ${EMBEDDED_ASSET_MODE} embedded assets"
    VERBATIM
)
set_source_files_properties("${EMBEDDED_ASSET_RESOURCE}" PROPERTIES GENERATED TRUE)
add_library(QuattroEmbeddedAssetResources OBJECT
    "${EMBEDDED_ASSET_RESOURCE}"
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

add_library(QuattroThemedUi STATIC
    src/theme/CatalogDialogLayout.cpp
    src/theme/Theme.cpp
    src/theme/ThemedFormLayout.cpp
    src/theme/ThemedD2D.cpp
    src/theme/ThemedGdiFallback.cpp
    src/theme/ThemedControls.cpp
    src/theme/ThemedMessageBox.cpp
    src/theme/ThemedUi.cpp
    src/theme/ThemedWindowUi.cpp
    src/common/Utilities.cpp
)

target_compile_definitions(QuattroThemedUi PUBLIC
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

target_compile_options(QuattroThemedUi PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_link_libraries(QuattroThemedUi PUBLIC
    pugixml
    user32
    gdi32
    d2d1
    dwrite
    comctl32
    msimg32
    shell32
    shlwapi
    uxtheme
)

add_library(AppLaunchLockerCore STATIC
    src/applaunchlocker/AppLaunchLockerCore.cpp
    src/common/JsonValue.cpp
)

target_compile_definitions(AppLaunchLockerCore PUBLIC
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

target_compile_options(AppLaunchLockerCore PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_link_libraries(AppLaunchLockerCore PUBLIC
    advapi32
    ole32
    oleaut32
    shell32
    taskschd
    uuid
    wbemuuid
    crypt32
    wintrust
)

add_executable(AppLaunchLocker WIN32
    resources/AppLaunchLocker.rc
    src/applaunchlocker/AppLaunchLockerMain.cpp
    src/applaunchlocker/AppLaunchLockerWindow.cpp
    src/applaunchlocker/AdBlockWindow.cpp
)
set_target_properties(AppLaunchLocker PROPERTIES
    EXCLUDE_FROM_ALL TRUE
)
set_property(SOURCE resources/AppLaunchLocker.rc APPEND PROPERTY
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/theme/default.xml")

target_compile_definitions(AppLaunchLocker PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

target_compile_options(AppLaunchLocker PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_link_libraries(AppLaunchLocker PRIVATE
    AppLaunchLockerCore
    QuattroThemedUi
)

target_link_options(AppLaunchLocker PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/MANIFEST:EMBED>
    $<$<CXX_COMPILER_ID:MSVC>:/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/resources/AppLaunchLocker.manifest>
)

if(QUATTRO_BUNDLE_OPTIONAL_EXECUTABLES)
    quattro_register_embedded_executable(
        TARGET AppLaunchLocker
        SYMBOL AppLaunchLocker
        ID app-launch-locker
        FILE_NAME AppLaunchLocker.exe
    )
endif()

quattro_generate_embedded_executable_catalog(EMBEDDED_EXECUTABLE_SOURCES)

add_library(QuattroEmbeddedExecutableCatalogProbe OBJECT
    ${EMBEDDED_EXECUTABLE_SOURCES}
)
set_target_properties(QuattroEmbeddedExecutableCatalogProbe PROPERTIES
    EXCLUDE_FROM_ALL TRUE
)
target_compile_options(QuattroEmbeddedExecutableCatalogProbe PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

add_executable(Quattro WIN32
    $<TARGET_OBJECTS:QuattroEmbeddedAssetResources>
    ${EMBEDDED_EXECUTABLE_SOURCES}
    resources/Quattro.rc
    src/main.cpp
    src/windows/AboutDialog.cpp
    src/common/AppLog.cpp
    src/common/EmbeddedAssets.cpp
    src/common/EmbeddedExecutableManager.cpp
    src/windows/BuiltinTools.cpp
    src/windows/ConfirmDialog.cpp
    src/domain/Config.cpp
    src/domain/TrackedContextMenuProviders.cpp
    src/services/ConfigPackageService.cpp
    src/services/ContextMenuProviderIconService.cpp
    src/common/FileDialog.cpp
    src/services/FileLockQueryService.cpp
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
    src/services/ShellContextMenuCacheService.cpp
    src/services/ShellContextMenuRefreshService.cpp
    src/services/ShellItemService.cpp
    src/services/SelectedPathCopyService.cpp
    src/services/TerminalContextMenuService.cpp
    src/windows/SimpleDialogs.cpp
    src/services/StartupService.cpp
    src/services/Storage.cpp
    src/services/SystemFunctions.cpp
    src/windows/TodoEditDialog.cpp
    src/domain/TodoSchedule.cpp
    src/services/UpdateCheckService.cpp
    src/windows/UpdateCheckDialog.cpp
    src/windows/UpdateDownloadDialog.cpp
    src/services/UpdateInstaller.cpp
    src/windows/UrlEditDialog.cpp
    src/services/UrlIconDownloadService.cpp
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
    QUATTRO_DEFAULT_TOP_MOST_ENABLED=$<IF:$<BOOL:${QUATTRO_DEFAULT_TOP_MOST_ENABLED}>,1,0>
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
    $<TARGET_OBJECTS:QuattroEmbeddedAssetResources>
    tests/UnitTests.cpp
    src/common/AppLog.cpp
    src/common/EmbeddedAssets.cpp
    src/common/EmbeddedAssetInstaller.cpp
    src/common/EmbeddedExecutableManager.cpp
    src/domain/Config.cpp
    src/domain/TrackedContextMenuProviders.cpp
    src/services/ConfigPackageService.cpp
    src/services/ContextMenuProviderIconService.cpp
    src/common/FileDialog.cpp
    src/services/FileLockQueryService.cpp
    src/services/Launcher.cpp
    src/common/JsonValue.cpp
    src/domain/LinkSorting.cpp
    src/domain/MenuCatalog.cpp
    src/domain/PluginRegistry.cpp
    src/services/ShellContextMenuCacheService.cpp
    src/services/ShellContextMenuRefreshService.cpp
    src/services/ShellItemService.cpp
    src/services/SelectedPathCopyService.cpp
    src/services/TerminalContextMenuService.cpp
    src/services/Storage.cpp
    src/services/SystemFunctions.cpp
    src/domain/TodoSchedule.cpp
    src/services/UpdateCheckService.cpp
    src/services/WebDavBackupService.cpp
    src/services/WebDavClient.cpp
    src/services/WebDavCredentialService.cpp
    src/services/WebDavRecoveryService.cpp
)

add_executable(QuattroNoteTodoAcceptance
    tests/NoteTodoAcceptance.cpp
    src/common/AppLog.cpp
    src/domain/Config.cpp
    src/domain/TrackedContextMenuProviders.cpp
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
)

add_executable(QuattroToastAcceptance
    tests/ToastAcceptance.cpp
)

add_executable(QuattroUiScreenshotAcceptance
    tests/UiScreenshotAcceptance.cpp
    src/common/AppLog.cpp
    src/windows/AboutDialog.cpp
    src/windows/BuiltinTools.cpp
    src/windows/ConfirmDialog.cpp
    src/services/ConfigPackageService.cpp
    src/services/ContextMenuProviderIconService.cpp
    src/common/FileDialog.cpp
    src/services/FileLockQueryService.cpp
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
    src/services/ShellContextMenuCacheService.cpp
    src/services/ShellContextMenuRefreshService.cpp
    src/services/ShellItemService.cpp
    src/services/TerminalContextMenuService.cpp
    src/domain/TrackedContextMenuProviders.cpp
    src/windows/SimpleDialogs.cpp
    src/services/Storage.cpp
    src/services/SystemFunctions.cpp
    src/windows/TodoEditDialog.cpp
    src/domain/TodoSchedule.cpp
    src/windows/UpdateCheckDialog.cpp
    src/windows/UrlEditDialog.cpp
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
    src/domain/TrackedContextMenuProviders.cpp
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

target_compile_options(QuattroToastAcceptance PRIVATE
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

target_include_directories(QuattroToastAcceptance PRIVATE
    src/theme
    src/common
)

target_link_libraries(QuattroToastAcceptance PRIVATE
    QuattroThemedUi
    gdiplus
)

target_link_libraries(QuattroTests PRIVATE
    QuattroThemedUi
    sqlite3
    ccronexpr
    advapi32
    bcrypt
    comctl32
    gdi32
    ole32
    oleaut32
    rstrtmgr
    shell32
    shlwapi
    uuid
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
    QuattroThemedUi
    comctl32
    gdiplus
    gdi32
    shell32
    user32
)

target_link_libraries(QuattroUiScreenshotAcceptance PRIVATE
    QuattroThemedUi
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
    uuid
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

if(QUATTRO_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/FileLockQueryServiceTests.cpp")
add_executable(QuattroFileLockQueryServiceTests
    tests/FileLockQueryServiceTests.cpp
    src/services/FileLockQueryService.cpp
    src/common/Utilities.cpp
)

target_compile_definitions(QuattroFileLockQueryServiceTests PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

target_compile_options(QuattroFileLockQueryServiceTests PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_include_directories(QuattroFileLockQueryServiceTests PRIVATE
    src
)

target_link_libraries(QuattroFileLockQueryServiceTests PRIVATE
    rstrtmgr
    shell32
    shlwapi
    user32
)
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

if(QUATTRO_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/FileDialogTests.cpp")
add_executable(QuattroFileDialogTests
    tests/FileDialogTests.cpp
    src/common/AppLog.cpp
    src/common/FileDialog.cpp
    src/common/Utilities.cpp
)

target_compile_definitions(QuattroFileDialogTests PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

target_compile_options(QuattroFileDialogTests PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_include_directories(QuattroFileDialogTests PRIVATE
    src
)

target_link_libraries(QuattroFileDialogTests PRIVATE
    ole32
    shell32
    shlwapi
    uuid
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
    src/services/ContextMenuProviderIconService.cpp
    src/common/FileDialog.cpp
    src/services/FileLockQueryService.cpp
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
    src/services/ShellContextMenuCacheService.cpp
    src/services/ShellContextMenuRefreshService.cpp
    src/services/ShellItemService.cpp
    src/services/TerminalContextMenuService.cpp
    src/domain/TrackedContextMenuProviders.cpp
    src/windows/SimpleDialogs.cpp
    src/services/Storage.cpp
    src/services/SystemFunctions.cpp
    src/windows/TodoEditDialog.cpp
    src/domain/TodoSchedule.cpp
    src/windows/UpdateCheckDialog.cpp
    src/windows/UrlEditDialog.cpp
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
    QuattroThemedUi
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
    uuid
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

if(QUATTRO_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/AppLaunchLockerTests.cpp")
add_executable(AppLaunchLockerTests
    tests/AppLaunchLockerTests.cpp
)
target_link_libraries(AppLaunchLockerTests PRIVATE AppLaunchLockerCore)
target_compile_definitions(AppLaunchLockerTests PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)
target_compile_options(AppLaunchLockerTests PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)
endif()

target_link_libraries(Quattro PRIVATE
    QuattroThemedUi
    sqlite3
    ccronexpr
    advapi32
    bcrypt
    d2d1
    dwrite
    iphlpapi
    mpr
    msimg32
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

target_compile_definitions(Quattro PRIVATE
    QUATTRO_ENABLE_ASSET_DECOMPRESSION=${QUATTRO_COMPRESS_EMBEDDED_ASSETS_VALUE}
)
if(TARGET QuattroTests)
    target_compile_definitions(QuattroTests PRIVATE
        QUATTRO_ENABLE_ASSET_DECOMPRESSION=${QUATTRO_COMPRESS_EMBEDDED_ASSETS_VALUE}
    )
endif()
if(QUATTRO_COMPRESS_EMBEDDED_ASSETS)
    target_link_libraries(Quattro PRIVATE cabinet)
    if(TARGET QuattroTests)
        target_link_libraries(QuattroTests PRIVATE cabinet)
    endif()
endif()

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

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/theme")
    add_custom_command(TARGET AppLaunchLocker POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:AppLaunchLocker>/theme"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/theme" "$<TARGET_FILE_DIR:AppLaunchLocker>/theme"
    )
endif()

foreach(TARGET_NAME Quattro AppLaunchLocker QuattroTests QuattroUiScreenshotAcceptance)
    if(TARGET ${TARGET_NAME})
        add_dependencies(${TARGET_NAME} QuattroThemeFacadeLint)
    endif()
endforeach()
