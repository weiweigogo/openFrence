cmake_minimum_required(VERSION 3.20)
project(openFences VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT linking so the released .exe is self-contained; recipients do not
# need to install the VC++ redistributable.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
add_definitions(-DUNICODE -D_UNICODE)

# Sources are UTF-8 (Chinese menu strings); without this MSVC assumes the
# local ANSI code page and misparses multi-byte literals.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# nlohmann/json (header-only, auto-download)
include(FetchContent)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

add_executable(openfences
    src/main.cpp
    src/fence_window.cpp
    src/render.cpp
    src/settings_panel.cpp
    src/context_menu.cpp
    src/icon_extract.cpp
    src/menu_icons.cpp
    src/config.cpp
    src/explorer_sort.cpp
    src/zoom_overlay.cpp
    src/app.rc
)

# Merge our application manifest into the linker-generated one. The
# <compatibility> section is load-bearing: without it Windows treats the
# process as pre-Windows-8 and refuses to create WS_EX_LAYERED child windows,
# which the Progman-hosted fence windows are.
target_link_options(openfences PRIVATE
    "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest")

target_include_directories(openfences PRIVATE src)
target_link_libraries(openfences PRIVATE
    d2d1 dwrite windowscodecs
    ole32 shlwapi shell32 comctl32 comdlg32
    advapi32
    nlohmann_json::nlohmann_json
)

set_target_properties(openfences PROPERTIES WIN32_EXECUTABLE TRUE)

add_executable(diag desktop_diag.cpp)
target_link_libraries(diag PRIVATE user32 comctl32)

add_executable(icon_diag icon_diag.cpp src/icon_extract.cpp)
target_link_libraries(icon_diag PRIVATE user32 shell32 shlwapi ole32 uuid comctl32)

add_executable(region_spike region_spike.cpp)
target_link_libraries(region_spike PRIVATE user32)
# Layered CHILD windows (the fence analogue probed here) only exist on Win8+;
# without the supportedOS manifest CreateWindowExW(WS_EX_LAYERED, WS_CHILD) fails.
target_link_options(region_spike PRIVATE
    "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest")

add_executable(ctxmenu_spike ctxmenu_spike.cpp)
target_link_libraries(ctxmenu_spike PRIVATE user32 shell32 shlwapi ole32 uuid)
# Same supportedOS manifest as the app: without it the process is shimmed to
# pre-Windows-8 and the shell hides IExplorerCommandProvider (a Win8+ interface).
target_link_options(ctxmenu_spike PRIVATE
    "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest")

add_executable(vis_probe vis_probe.cpp)
target_link_libraries(vis_probe PRIVATE user32 shell32 advapi32)
