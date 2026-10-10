cmake_minimum_required(VERSION 3.20)

project(mkv_win_mcp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "mkv-win-mcp only supports Windows.")
endif()

# 静态链接 CRT：生成的 exe 不依赖 VC 运行时，MCP 客户端可直接拉起
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 产物固定落在 bin/<配置>/，让单配置生成器（Ninja）与多配置生成器（VS）路径一致。
# 用了 $<CONFIG> 之后 CMake 不会再自动追加一层配置子目录。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

add_executable(mkv_win_mcp
    src/main.cpp
    src/util.cpp
    src/tools.cpp
    src/screenshot.cpp
    src/input.cpp
    src/mouse.cpp
    src/keyboard.cpp
)

target_include_directories(mkv_win_mcp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
)

target_compile_definitions(mkv_win_mcp PRIVATE UNICODE _UNICODE)

if(MSVC)
    target_compile_options(mkv_win_mcp PRIVATE /utf-8 /bigobj /W3 /permissive-)
endif()

target_link_libraries(mkv_win_mcp PRIVATE
    user32
    gdi32
    gdiplus
    ole32
    shell32
    winmm    # timeBeginPeriod：拟人移动时把时钟粒度提到 1ms
)
