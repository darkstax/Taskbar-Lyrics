cmake_minimum_required(VERSION 3.30)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(/utf-8 /D_UNICODE /DUNICODE)

project(taskbar-lyrics LANGUAGES CXX)

add_executable(taskbar-lyrics WIN32
    "src/main.cpp"
)

# C++20 模块源（CMake 3.28+ 的 CXX_MODULES FILE_SET，兼容 CMake 4.x）
target_sources(taskbar-lyrics PRIVATE
    FILE_SET CXX_MODULES BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
        "src/plugin/Plugin.cppm"
        "src/plugin/Config.cppm"
        "src/taskbar/Taskbar.cppm"
        "src/taskbar/Handler.cppm"
        "src/taskbar/Registry.cppm"
        "src/window/Window.cppm"
        "src/window/Renderer.cppm"
        "src/window/Lyrics.cppm"
        "src/pipe/LyricPipeServer.cppm"
)

target_include_directories(taskbar-lyrics PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

target_link_libraries(taskbar-lyrics PRIVATE
    d3d11
    d2d1
    dwrite
    dxgi
    dcomp
)

if(MSVC)
    # 注入应用清单：PerMonitorV2 DPI 感知 + Windows 11 兼容性声明
    target_link_options(taskbar-lyrics PRIVATE
        "/MANIFEST:EMBED"
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest"
    )
endif()
