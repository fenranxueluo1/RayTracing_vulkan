cmake_minimum_required(VERSION 4.3)
project(RayTracing LANGUAGES C CXX)

# ---------- C++ 标准 ----------
# 注意：必须在 add_library 之前设置，CMake 只在目标创建时读取该变量。
# 必须 >= C++20 才能使用 Vulkan 结构体的指定初始化器（.sType = ... 这种写法，
# 与 Nexus_GameEngine 一致）；这里用 C++23。
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# MSVC 编译器启用最新 C++ 特性
if(MSVC)
    add_compile_options(/std:c++latest)
endif()

# ---------- 全局输出目录 ----------
# 把所有构建产物集中到 build/bin，方便运行和调试
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ---------- 默认构建类型 ----------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---------- 编译选项 ----------
if(MSVC)
    add_compile_options(/W4 /utf-8)
    add_compile_definitions(NOMINMAX)
else()
    # -Wno-missing-field-initializers：Vulkan 代码普遍用 C++ 指定初始化器写
    # VkXxxCreateInfo{ .sType = ..., .foo = ... }，省略的成员按标准会被零初始化，
    # 但 GCC/Clang 仍会逐个字段告警（GCC 16 依然如此），属于误报。
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
endif()

# ---------- Vulkan 开发包：修正新版 SDK 的目录布局 ----------
# 自 1.4.3xx 起，Linux 版 Vulkan SDK 把 loader 从 $VULKAN_SDK/lib 挪到了
# $VULKAN_SDK/lib/VulkanLoader/lib，而 CMake 的 FindVulkan 只搜索前者，
# 会导致 "Could NOT find Vulkan (missing: Vulkan_LIBRARY)"。这里补上搜索路径。
# Windows 的 SDK 仍沿用 lib/ 布局，无需处理。
if(UNIX AND DEFINED ENV{VULKAN_SDK})
    set(_RAYTRACING_VULKAN_LOADER_DIR "$ENV{VULKAN_SDK}/lib/VulkanLoader/lib")
    if(IS_DIRECTORY "${_RAYTRACING_VULKAN_LOADER_DIR}")
        list(APPEND CMAKE_LIBRARY_PATH "${_RAYTRACING_VULKAN_LOADER_DIR}")
    endif()
endif()

# ---------- Vulkan 开发包 ----------
# 查找 Vulkan SDK（volk 加载器）。当前示例没有着色器，无需 glslc；
# 若后续加入着色器，可改为 find_package(Vulkan REQUIRED COMPONENTS volk glslc)。
# 两平台都需要 Vulkan SDK：
#   Windows：安装官方 SDK 即可（Installer 会写入 VULKAN_SDK 环境变量）
#   Linux  ：下载 SDK 后需先 source setup-env.sh，使 VULKAN_SDK 指向 SDK 根目录
find_package(Vulkan REQUIRED COMPONENTS volk)

# volk 并非所有 Vulkan 运行时包都附带，缺失时给出可操作的提示
if(NOT Vulkan_volk_FOUND)
    message(FATAL_ERROR
        "未找到 volk（Vulkan 加载器）。请安装完整版 Vulkan SDK，\n"
        "Linux 下请先执行：source <VulkanSDK>/setup-env.sh")
endif()

# volk 头文件所在目录（不同来源布局不同，统一归一化后以 <volk.h> 方式包含）：
#   Vulkan SDK  Windows：<prefix>/Include/Volk/volk.h
#   Vulkan SDK  Linux  ：<prefix>/include/volk/volk.h
#   Linux 包管理器     ：<prefix>/include/volk.h
find_path(RAYTRACING_VOLK_INCLUDE_DIR
    NAMES volk.h
    HINTS $ENV{VULKAN_SDK}/Include $ENV{VULKAN_SDK}/include
    PATH_SUFFIXES include volk
    DOC "volk 头文件所在目录"
)
if(NOT RAYTRACING_VOLK_INCLUDE_DIR)
    message(FATAL_ERROR
        "未找到 volk 的头文件。\n"
        "Linux  ：请安装 volk 包\n"
        "Windows：请安装 Vulkan SDK，并使 VULKAN_SDK 指向 SDK 根目录")
endif()

# ==============================================================================
#  第三方依赖（Windows / Linux 分别配置）
#
#  Windows：使用 D:/library 下的预编译库（SDL3 / SDL3_image）与仓库内 Dependencies/ 的
#           纯头文件库（glm / imgui，来自 D:/library）。
#  Linux  ：SDL3 / SDL3_image 由系统包管理器提供，其余头文件库仍来自仓库内 Dependencies/。
# ==============================================================================
if(WIN32)
    # 第三方依赖根目录（用户已将依赖下载到 D:/library）
    if(NOT DEFINED RAYTRACING_DEP_ROOT)
        set(RAYTRACING_DEP_ROOT "D:/library" CACHE PATH "第三方依赖根目录（已下载到 D:/library）")
    endif()

    # 按编译器选择对应的 SDL3 预编译包：
    #   MinGW / MSYS2：SDL3-3.4.14-mingw（提供 libSDL3.dll.a 与 SDL3Config.cmake）
    #   MSVC          ：SDL3-3.4.14-vc  （提供 SDL3.lib 与 SDL3Config.cmake）
    if(MINGW)
        set(RAYTRACING_SDL3_DIR "${RAYTRACING_DEP_ROOT}/SDL3-3.4.14-mingw/x86_64-w64-mingw32/lib/cmake/SDL3" CACHE PATH "SDL3 的 CMake 配置目录")
        set(RAYTRACING_SDL3_DLL "${RAYTRACING_DEP_ROOT}/SDL3-3.4.14-mingw/x86_64-w64-mingw32/bin/SDL3.dll" CACHE PATH "SDL3 运行时 DLL")

        # SDL3_image（图片解码，替代 stb_image）
        set(RAYTRACING_SDL3_IMAGE_DIR "${RAYTRACING_DEP_ROOT}/SDL3_image-3.4.4-mingw/x86_64-w64-mingw32/lib/cmake/SDL3_image" CACHE PATH "SDL3_image 的 CMake 配置目录")
        set(RAYTRACING_SDL3_IMAGE_DLL "${RAYTRACING_DEP_ROOT}/SDL3_image-3.4.4-mingw/x86_64-w64-mingw32/bin/SDL3_image.dll" CACHE PATH "SDL3_image 运行时 DLL")
    elseif(MSVC)
        set(RAYTRACING_SDL3_DIR "${RAYTRACING_DEP_ROOT}/SDL3-3.4.14-vc/cmake" CACHE PATH "SDL3 的 CMake 配置目录")
        set(RAYTRACING_SDL3_DLL "${RAYTRACING_DEP_ROOT}/SDL3-3.4.14-vc/lib/x64/SDL3.dll" CACHE PATH "SDL3 运行时 DLL")

        # 注意：D:/library 下目前没有 SDL3_image 的 MSVC 预编译包，
        # 如需用 MSVC 构建请下载 SDL3_image-3.4.4-vc 并放到 D:/library
        set(RAYTRACING_SDL3_IMAGE_DIR "${RAYTRACING_DEP_ROOT}/SDL3_image-3.4.4-vc/cmake" CACHE PATH "SDL3_image 的 CMake 配置目录")
        set(RAYTRACING_SDL3_IMAGE_DLL "${RAYTRACING_DEP_ROOT}/SDL3_image-3.4.4-vc/lib/x64/SDL3_image.dll" CACHE PATH "SDL3_image 运行时 DLL")
    endif()
endif()

# ---------- 引入子项目 ----------
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/Walnut
                 ${CMAKE_CURRENT_BINARY_DIR}/Walnut)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/RayTracing
                 ${CMAKE_CURRENT_BINARY_DIR}/RayTracing)