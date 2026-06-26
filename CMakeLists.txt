cmake_minimum_required(VERSION 3.20)

project(Quattro LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL QUIET)
find_package(pugixml CONFIG QUIET)

set(QUATTRO_WITH_WEBDAV_LIBS OFF)
if(CURL_FOUND AND TARGET pugixml::pugixml)
    set(QUATTRO_WITH_WEBDAV_LIBS ON)
else()
    message(WARNING "CURL and/or pugixml were not found. WebDAV backup UI will build, but network operations will report that the third-party backend is unavailable.")
endif()

set(EMBEDDED_ASSET_INPUTS
    "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
)

file(GLOB_RECURSE EMBEDDED_ASSET_DIR_INPUTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/theme/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/icons/menu/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/icons/url/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins/store/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/*"
)
list(APPEND EMBEDDED_ASSET_INPUTS ${EMBEDDED_ASSET_DIR_INPUTS})

set(EMBEDDED_ASSETS_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedAssets.cpp")
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

target_include_directories(sqlite3 PUBLIC
    third_party/sqlite
)

target_compile_definitions(sqlite3 PRIVATE
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_THREADSAFE=1
)

target_compile_options(sqlite3 PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/W0 /utf-8>
)

add_executable(Quattro WIN32
    ${EMBEDDED_ASSETS_CPP}
    resources/Quattro.rc
    src/main.cpp
    src/AppLog.cpp
    src/BuiltinTools.cpp
    src/Config.cpp
    src/ConfigPackageService.cpp
    src/EmbeddedAssetInstaller.cpp
    src/HotKeyEditor.cpp
    src/IconService.cpp
    src/JsonValue.cpp
    src/Launcher.cpp
    src/LinkEditDialog.cpp
    src/MainWindow.cpp
    src/MenuCatalog.cpp
    src/PluginManagerDialog.cpp
    src/PluginRegistry.cpp
    src/PluginStoreService.cpp
    src/SearchDialog.cpp
    src/ShellItemService.cpp
    src/SimpleDialogs.cpp
    src/StartupService.cpp
    src/Storage.cpp
    src/SystemFunctionDialog.cpp
    src/Theme.cpp
    src/ThemedControls.cpp
    src/TodoEditDialog.cpp
    src/TodoSchedule.cpp
    src/UrlEditDialog.cpp
    src/Utilities.cpp
    src/WebDavBackupService.cpp
    src/WebDavClient.cpp
    src/WebDavCredentialService.cpp
)

set_target_properties(Quattro PROPERTIES
    OUTPUT_NAME "Quattro"
)

add_executable(QuattroTests
    tests/UnitTests.cpp
    src/Config.cpp
    src/ConfigPackageService.cpp
    src/Launcher.cpp
    src/JsonValue.cpp
    src/MenuCatalog.cpp
    src/PluginRegistry.cpp
    src/PluginStoreService.cpp
    src/ShellItemService.cpp
    src/Storage.cpp
    src/SystemFunctionDialog.cpp
    src/Theme.cpp
    src/ThemedControls.cpp
    src/TodoSchedule.cpp
    src/Utilities.cpp
    src/WebDavBackupService.cpp
    src/WebDavClient.cpp
    src/WebDavCredentialService.cpp
)

add_executable(QuattroNoteTodoAcceptance
    tests/NoteTodoAcceptance.cpp
    src/Config.cpp
    src/ConfigPackageService.cpp
    src/JsonValue.cpp
    src/MenuCatalog.cpp
    src/PluginRegistry.cpp
    src/Storage.cpp
    src/TodoSchedule.cpp
    src/Utilities.cpp
)

add_executable(QuattroDbProbe
    tests/DbProbe.cpp
    src/Storage.cpp
    src/TodoSchedule.cpp
    src/Utilities.cpp
)

add_executable(QuattroDbSeed
    tests/DbSeed.cpp
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

add_executable(QuattroWebDavAcceptance
    tests/WebDavAcceptance.cpp
    src/Config.cpp
    src/ConfigPackageService.cpp
    src/PluginRegistry.cpp
    src/Storage.cpp
    src/TodoSchedule.cpp
    src/Utilities.cpp
    src/WebDavBackupService.cpp
    src/WebDavClient.cpp
    src/WebDavCredentialService.cpp
)

target_compile_definitions(Quattro PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
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

target_compile_definitions(QuattroWebDavAcceptance PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

if(QUATTRO_WITH_WEBDAV_LIBS)
    target_compile_definitions(Quattro PRIVATE QUATTRO_WITH_WEBDAV_LIBS)
    target_compile_definitions(QuattroTests PRIVATE QUATTRO_WITH_WEBDAV_LIBS)
    target_compile_definitions(QuattroWebDavAcceptance PRIVATE QUATTRO_WITH_WEBDAV_LIBS)
endif()

target_compile_options(Quattro PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

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

target_compile_options(QuattroWebDavAcceptance PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_include_directories(Quattro PRIVATE
    src
)

target_include_directories(QuattroThemeStateProbe PRIVATE
    src
)

target_link_libraries(QuattroTests PRIVATE
    sqlite3
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
    target_link_libraries(QuattroTests PRIVATE CURL::libcurl pugixml::pugixml)
endif()

target_link_libraries(QuattroNoteTodoAcceptance PRIVATE
    sqlite3
    bcrypt
    shell32
    shlwapi
    user32
    wininet
)

target_link_libraries(QuattroDbProbe PRIVATE
    sqlite3
    user32
)

target_link_libraries(QuattroDbSeed PRIVATE
    sqlite3
    user32
)

target_link_libraries(QuattroThemeStateProbe PRIVATE
    comctl32
    gdiplus
    gdi32
    shell32
    user32
)

target_link_libraries(QuattroWebDavAcceptance PRIVATE
    sqlite3
    advapi32
    user32
)

if(QUATTRO_WITH_WEBDAV_LIBS)
    target_link_libraries(QuattroWebDavAcceptance PRIVATE CURL::libcurl pugixml::pugixml)
endif()

target_link_libraries(Quattro PRIVATE
    sqlite3
    advapi32
    bcrypt
    d2d1
    dwrite
    ole32
    shell32
    shlwapi
    wininet
    windowscodecs
    user32
    gdi32
    comctl32
)

if(QUATTRO_WITH_WEBDAV_LIBS)
    target_link_libraries(Quattro PRIVATE CURL::libcurl pugixml::pugixml)
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
