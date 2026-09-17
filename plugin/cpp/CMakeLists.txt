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
        "src/util/Log.cppm"
)

target_include_directories(taskbar-lyrics PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# 只链接实际用到的库：d2d1/dwrite（离屏渲染）、shell32（托盘 / 全屏状态查询）。
# d3d11/dxgi/dcomp 是早期 DComp 方案的遗留链接；现在走"内存 DIB + UpdateLayeredWindow
# 逐像素 alpha"，不需要图形设备（user32/gdi32 由 MSVC 默认链接集提供）。
target_link_libraries(taskbar-lyrics PRIVATE
    d2d1
    dwrite
    shell32 # SHQueryUserNotificationState / Shell_NotifyIconW
)

if(MSVC)
    # 注入应用清单：PerMonitorV2 DPI 感知 + Windows 11 兼容性声明
    target_link_options(taskbar-lyrics PRIVATE
        "/MANIFEST:EMBED"
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest"
    )
endif()

# ---------------------------------------------------------------------------
# 单元测试（审查修复 F1：测试入库为交付物）
# option 默认 OFF：发布构建产物清单不含 test_config.exe；本地/CI 开：
#   cmake --preset x64-release -DTL_BUILD_TESTS=ON && ctest --test-dir build/x64-release -C Release
# test_config 仅 import plugin.Config（纯解析逻辑 + 全局 config 状态），
# 不碰窗口/渲染/管道，console 程序可直接跑。
# ---------------------------------------------------------------------------
option(TL_BUILD_TESTS "Build unit tests (not part of release artifacts)" OFF)
if(TL_BUILD_TESTS)
    enable_testing()
    add_executable(test_config "tests/test_config.cpp")
    target_sources(test_config PRIVATE
        FILE_SET CXX_MODULES BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
        FILES
            "src/plugin/Config.cppm"
            "src/util/Log.cppm"
    )
    target_include_directories(test_config PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
    add_test(NAME config_logic COMMAND test_config)
endif()
