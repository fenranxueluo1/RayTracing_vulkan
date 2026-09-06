cmake_minimum_required(VERSION 4.3)
project(RayTracing)

# C++ 标准在顶层 CMakeLists.txt 中统一设置，此处无需重复。

# ---------- 目标（可执行程序） ----------
add_executable(RayTracing "src/WalnutApp.cpp")

# MSVC 编译器启用最新 C++ 特性
if(MSVC)
    target_compile_options(RayTracing PRIVATE /std:c++latest)
    # shaderc 等第三方预编译库不附带 PDB，忽略 LNK4099 警告（不影响链接结果）
    target_link_options(RayTracing PRIVATE /ignore:4099)
endif()

# ---------- 编译宏 ----------
target_compile_definitions(RayTracing
    PRIVATE
        $<$<CONFIG:Debug>:WL_DEBUG>
        $<$<CONFIG:Release>:WL_RELEASE>
        $<$<BOOL:${WIN32}>:WL_PLATFORM_WINDOWS>
)

# ---------- 链接依赖 ----------
# 链接 Walnut 即可级联获得其头文件路径与 Vulkan::Vulkan / SDL3 依赖
target_link_libraries(RayTracing
    PRIVATE
        Walnut
        Vulkan::Vulkan
)

# ==============================================================================
#  SDL3（Windows / Linux 分别配置）
#
#  最终可执行文件需要这些符号，故在此显式链接。
#  注意：导入目标只在调用 find_package 的目录及其子目录可见，
#  这里与 Walnut 是兄弟目录，必须各自独立查找。
# ==============================================================================
if(WIN32)
    # Windows：使用 D:/library 下的预编译库（顶层已确定 RAYTRACING_SDL3_DIR）
    set(SDL3_DIR "${RAYTRACING_SDL3_DIR}")
    find_package(SDL3 CONFIG REQUIRED)
    target_link_libraries(RayTracing
        PRIVATE
            SDL3::SDL3
    )

    # SDL3_image（Walnut::Image 依赖，最终可执行文件需要其符号）
    set(SDL3_image_DIR "${RAYTRACING_SDL3_IMAGE_DIR}")
    find_package(SDL3_image CONFIG REQUIRED)
    target_link_libraries(RayTracing
        PRIVATE
            SDL3_image::SDL3_image
    )
else()
    # Linux：优先 CMake Config 包，未安装时回退 pkg-config
    find_package(SDL3 QUIET CONFIG)
    if(TARGET SDL3::SDL3)
        target_link_libraries(RayTracing PRIVATE SDL3::SDL3)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
        target_link_libraries(RayTracing PRIVATE PkgConfig::SDL3)
    endif()

    # --- SDL3_image ---
    find_package(SDL3_image QUIET CONFIG)
    if(TARGET SDL3_image::SDL3_image)
        target_link_libraries(RayTracing PRIVATE SDL3_image::SDL3_image)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL3_image REQUIRED IMPORTED_TARGET sdl3-image)
        target_link_libraries(RayTracing PRIVATE PkgConfig::SDL3_image)
    endif()
endif()

# ==============================================================================
#  Windows 专有部分：把随附的运行时 DLL 拷贝到可执行文件目录
#  Linux 下 SDL3 由系统包管理器提供，位于默认动态库搜索路径，无需拷贝。
# ==============================================================================
if(WIN32)
    # ---------- 拷贝 SDL3.dll 到输出目录 ----------
    add_custom_command(TARGET RayTracing POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${RAYTRACING_SDL3_DLL}
            $<TARGET_FILE_DIR:RayTracing>
    )

    # ---------- 拷贝 SDL3_image.dll 到输出目录 ----------
    add_custom_command(TARGET RayTracing POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${RAYTRACING_SDL3_IMAGE_DLL}
            $<TARGET_FILE_DIR:RayTracing>
    )

    # ---------- 拷贝 MinGW 运行时 DLL 到 exe 目录 ----------
    # 避免程序从 PATH 里误加载 Git 自带的 libstdc++-6.dll（版本不匹配会崩溃）。
    # Windows 优先从 exe 所在目录加载 DLL，因此把编译工具链的运行时 DLL 拷贝过去即可。
    if(MINGW)
        get_filename_component(MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        foreach(runtime_dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
            add_custom_command(TARGET RayTracing POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${MINGW_BIN_DIR}/${runtime_dll}
                    $<TARGET_FILE_DIR:RayTracing>
            )
        endforeach()
    endif()
endif()

# ==============================================================================
#  拷贝字体资源到输出目录（Windows / Linux 通用）
#  运行时从可执行文件同目录 fonts/ 加载 Noto Sans SC，不依赖系统字体
# ==============================================================================
add_custom_command(TARGET RayTracing POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:RayTracing>/fonts
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/../assets/fonts/NotoSansSC-VF.ttf
        $<TARGET_FILE_DIR:RayTracing>/fonts/NotoSansSC-VF.ttf
)

# ---------- 运行目标 ----------
# 使 `cmake --build <构建目录> --target RayTracing_RUN` 等价于「先构建，再启动程序」
add_custom_target(RayTracing_RUN
    COMMAND $<TARGET_FILE:RayTracing>
    WORKING_DIRECTORY $<TARGET_FILE_DIR:RayTracing>
)
add_dependencies(RayTracing_RUN RayTracing)