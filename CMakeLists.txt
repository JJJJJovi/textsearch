cmake_minimum_required(VERSION 3.15)
project(TextSearch CXX)

# ---- 全局设置 ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 默认 Release 构建
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Windows 通用宏定义
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

# 让资源编译器 windres 能找到 gui/app.manifest（ComCtl32 v6 主题清单）
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I${CMAKE_CURRENT_SOURCE_DIR}/gui")

# Release 构建优化：-O2
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2)
endif()

# ---- 源文件收集 ----
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp")
# C1 移植层：port_win.cpp（Windows）与 port_stub.cpp（其他平台 / 编译面验证）
# 各自整体 #if 守卫，两个都编进来在任何平台都只有一个生效。
file(GLOB PORT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/core/port/*.cpp")
list(APPEND CORE_SOURCES ${PORT_SOURCES})
file(GLOB GUI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gui/*.cpp")
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")

# bench_search 有独立 main()，不编入 tests target
list(REMOVE_ITEM TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/bench_search.cpp")

# ---- 主程序 target: Win32 GUI ----
add_executable(textsearch WIN32
    ${GUI_SOURCES}
    ${CORE_SOURCES}
    gui/app.rc          # ComCtl32 v6 主题清单（启用现代控件样式）
)

target_include_directories(textsearch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(textsearch PRIVATE cxx_std_17)
target_compile_options(textsearch PRIVATE
    -Wall -Wextra -finput-charset=UTF-8
)

# MinGW 链接为 Windows 子系统（-mwindows）+ GUI 所需系统库
# -municode：使用 wWinMain（Unicode 入口）
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(textsearch PRIVATE -mwindows -municode)
endif()
target_link_libraries(textsearch PRIVATE
    comctl32     # ListView / ProgressBar / 子类化
    comdlg32     # GetSaveFileName
    riched20     # RichEdit 查看器
    ole32        # COM / IFileDialog
    oleaut32
    uuid
    shlwapi      # 路径工具
    shell32      # SHBrowseForFolder 回退
    dwmapi       # S5.1：DwmSetWindowAttribute 深色标题栏（此前仅手动命令行链接带了它）
)


# ---- 测试 target: console ----
add_executable(tests
    ${TEST_SOURCES}
    ${CORE_SOURCES}
)

target_include_directories(tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
target_compile_features(tests PRIVATE cxx_std_17)
target_compile_options(tests PRIVATE
    -Wall -Wextra -finput-charset=UTF-8
)

# ---- Benchmark target: console ----
add_executable(bench
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/bench_search.cpp
    ${CORE_SOURCES}
)

target_include_directories(bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
target_compile_features(bench PRIVATE cxx_std_17)
target_compile_options(bench PRIVATE
    -Wall -Wextra -finput-charset=UTF-8
)
# psapi: GetProcessMemoryInfo（内存峰值测量）
target_link_libraries(bench PRIVATE psapi)

# ---- CLI target: 跨平台命令行入口（C2）----
add_executable(textsearch-cli
    cli/main.cpp
    ${CORE_SOURCES}
)
target_include_directories(textsearch-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(textsearch-cli PRIVATE cxx_std_17)
target_compile_options(textsearch-cli PRIVATE -Wall -Wextra)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # wmain 入口（Windows）；POSIX 侧用 main
    target_link_options(textsearch-cli PRIVATE -municode)
endif()

# ---- 测试注册 ----
enable_testing()
add_test(NAME textsearch_tests COMMAND tests)

# ---- 静态链接（默认开；必须在所有 add_executable 之后）----
# 2026-10 起本机构建统一 -static：C:\Windows 下的 libstdc++-6 等运行时 DLL 是
# 老版本残留，与 GCC 16.x 不兼容（动态链接启动即 0xC0000139），勿依赖动态。
# 单文件分发（约 2MB）也用同一口径。确要动态链接（约 650KB）时配置
# -DTEXTSEARCH_STATIC=OFF，并把工具链 bin 下配套的 3 个运行时 DLL 与 exe 同目录。
option(TEXTSEARCH_STATIC "Static-link executables (zero runtime DLL deps)" ON)
if(TEXTSEARCH_STATIC AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    foreach(tgt textsearch textsearch-cli tests bench)
        target_link_options(${tgt} PRIVATE -static -static-libgcc -static-libstdc++)
    endforeach()
endif()

# ---- UPX 压缩（可选，取消注释启用）----
# find_program(UPX upx)
# if(UPX AND CMAKE_BUILD_TYPE STREQUAL "Release")
#     add_custom_command(TARGET textsearch POST_BUILD
#         COMMAND ${UPX} --best $<TARGET_FILE:textsearch>
#         COMMENT "Compressing textsearch.exe with UPX")
# endif()
