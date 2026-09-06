cmake_minimum_required(VERSION 4.3)
project(Walnut)

# C++ 标准在顶层 CMakeLists.txt 中统一设置，此处无需重复。

# ---------- 目标（静态库） ----------
add_library(Walnut STATIC)

# MSVC 编译器启用最新 C++ 特性
if(MSVC)
    target_compile_options(Walnut PRIVATE /std:c++latest)
endif()

# ---------- 源文件 ----------
# ImGui 核心 + 后端（SDL3 / Vulkan），后端通过 ImGuiBuild.cpp 以 #include 方式编入
target_sources(Walnut
    PRIVATE
        src/Walnut/Application.cpp
        src/Walnut/Image.cpp
        src/Walnut/Random.cpp
        src/Walnut/Input/Input.cpp
        src/Walnut/ImGui/ImGuiBuild.cpp

        ${CMAKE_CURRENT_SOURCE_DIR}/../Dependencies/imgui/imgui.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../Dependencies/imgui/imgui_demo.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../Dependencies/imgui/imgui_draw.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../Dependencies/imgui/imgui_tables.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../Dependencies/imgui/imgui_widgets.cpp
)

# ---------- 对外头文件 ----------
# PUBLIC：使用方通过 target_link_libraries(RayTracing ... Walnut) 即可获得这些头文件路径
target_include_directories(Walnut
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../Dependencies/imgui>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../Dependencies>
)

# ---------- 第三方库头文件（SYSTEM，避免内部告警污染构建输出） ----------
target_include_directories(Walnut
    SYSTEM PUBLIC
        ${RAYTRACING_VOLK_INCLUDE_DIR}
        ${Vulkan_INCLUDE_DIRS}
)

# ---------- 编译宏 ----------
# IMGUI_IMPL_VULKAN_USE_VOLK：让 imgui 的 Vulkan 后端使用 volk 函数指针
# WL_DEBUG / WL_RELEASE / WL_PLATFORM_WINDOWS：与原 Walnut 框架保持一致
target_compile_definitions(Walnut
    PUBLIC
        IMGUI_IMPL_VULKAN_USE_VOLK
        $<$<CONFIG:Debug>:WL_DEBUG>
        $<$<CONFIG:Release>:WL_RELEASE>
        $<$<BOOL:${WIN32}>:WL_PLATFORM_WINDOWS>
)

# ==============================================================================
#  SDL3（Windows / Linux 分别配置）
#
#  SDL 头文件需以 PUBLIC 暴露，因为 Application.h 公开了 SDL_Window 类型。
#  导入目标（SDL3::SDL3）只在调用 find_package 的目录及其子目录可见，
#  Walnut 与 RayTracing 是平级的兄弟目录，因此各自独立查找。
#  重复调用 find_package 与 pkg_check_modules 是安全的（内部有缓存）。
# ==============================================================================
if(WIN32)
    # Windows：使用 D:/library 下的预编译库（顶层已确定 RAYTRACING_SDL3_DIR）
    set(SDL3_DIR "${RAYTRACING_SDL3_DIR}")
    find_package(SDL3 CONFIG REQUIRED)
    target_link_libraries(Walnut
        PUBLIC
            SDL3::SDL3
    )

    # SDL3_image（Image.cpp 用它解码图片，仅内部使用，故 PRIVATE）
    set(SDL3_image_DIR "${RAYTRACING_SDL3_IMAGE_DIR}")
    find_package(SDL3_image CONFIG REQUIRED)
    target_link_libraries(Walnut
        PRIVATE
            SDL3_image::SDL3_image
    )
else()
    # Linux：优先用 SDL3 官方提供的 CMake Config 包（能正确处理非标准安装前缀），
    # 未安装 config 包时回退到 pkg-config
    find_package(SDL3 QUIET CONFIG)
    if(TARGET SDL3::SDL3)
        target_link_libraries(Walnut PUBLIC SDL3::SDL3)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
        target_link_libraries(Walnut PUBLIC PkgConfig::SDL3)
    endif()

    # --- SDL3_image ---（图片解码必需）
    find_package(SDL3_image QUIET CONFIG)
    if(TARGET SDL3_image::SDL3_image)
        target_link_libraries(Walnut PRIVATE SDL3_image::SDL3_image)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL3_image REQUIRED IMPORTED_TARGET sdl3-image)
        target_link_libraries(Walnut PRIVATE PkgConfig::SDL3_image)
    endif()
endif()

# ---------- Vulkan 依赖（平台无关） ----------
# Vulkan SDK 通过顶层 find_package(Vulkan ...) 引入，目标 Vulkan::Vulkan 提供头文件与链接库
target_link_libraries(Walnut
    PUBLIC
        Vulkan::Vulkan
)

# volk 实现已在 Walnut/src/Walnut/Application.cpp 中通过 VOLK_IMPLEMENTATION 生成