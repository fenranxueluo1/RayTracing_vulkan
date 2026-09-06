# RayTracing（Vulkan）

跟随 [TheCherno 的《RayTracing》教程系列](https://www.youtube.com/watch?v=gfW1Fhd9u9Q) 学习光追渲染的项目框架。
基于 TheCherno 的 Walnut 模板移植而来：窗口系统由 GLFW 替换为 **SDL3**，Vulkan 写法升级为 **Vulkan 1.4**
（volk 函数加载 + 动态渲染 + 同步2），
构建系统由 Premake 改为 **CMake**，支持 **Windows 与 Linux** 双平台。

**新手请先阅读 [`学习文档.md`](学习文档.md)**，里面有面向零基础的 Vulkan 概念讲解与代码走读。

## 功能特性

- **ImGui 编辑器界面**：内置 Docking（停靠）布局与菜单栏，中文界面开箱即用
- **即时光追视口**：跟随教程逐集实现相机、场景、光线追踪着色器等（当前为随机像素渲染的起步版本）
- **SDL3 现代化封装**：窗口 / 事件 / 输入统一走 SDL3，`SDL_Vulkan_CreateSurface` 创建 Vulkan 表面
- **Vulkan 1.4 现代写法**：`VK_NO_PROTOTYPES` + volk 动态加载、指定初始化器、动态渲染（`vkCmdBeginRendering`）、同步2（`vkCmdPipelineBarrier2` / `vkQueueSubmit2`）、`VK_EXT_debug_utils` 校验（Debug 构建自动开启）
- **内置中文支持**：默认字体为随程序分发的 **Noto Sans SC**（SIL OFL，可免费商用），不依赖系统字体
- **图片加载**：基于 **SDL3_image** 解码 PNG / JPG / TGA / BMP / HDR 等格式，HDR 保留浮点数据
- **跨平台构建**：CMake Presets 提供 Windows（MinGW / MSVC）与 Linux 预设，构建产物统一输出到 `build/bin`

## 技术栈

| 组件 | 技术 |
| --- | --- |
| 语言标准 | C++23 |
| 构建系统 | CMake ≥ 4.3 |
| 窗口 / 事件 | SDL3（3.4.14） |
| 图片解码 | SDL3_image（3.4.4） |
| 图形 API | Vulkan 1.4 |
| Vulkan 函数加载 | volk（动态加载，`VK_NO_PROTOTYPES`） |
| GUI | Dear ImGui（docking 分支） |
| 数学库 | glm（header-only） |
| 字体 | Noto Sans SC（随仓库分发，SIL OFL） |

## 目录结构

```
RayTracing_vulkan/
├── CMakeLists.txt            # 顶层构建脚本（找 Vulkan / 配置依赖路径 / 引入子项目）
├── CMakePresets.json         # 构建预设（windows / linux，Debug / Release）
├── assets/fonts/             # Noto Sans SC 字体 + OFL 授权文件（构建时拷贝到 exe 旁）
├── Dependencies/             # 纯头文件依赖（glm / imgui，来自 D:/library）
├── Walnut/                   # 框架静态库（窗口 / Vulkan 上下文 / ImGui / 输入 / 图像）
└── RayTracing/               # 应用程序（可执行文件，你的图层写在这里）
```

## 环境要求

- **CMake** ≥ 4.3
- **C++23 编译器**：MSVC 19.40+ / GCC 13+ / Clang 16+
- **Vulkan SDK（1.4+）**：`find_package(Vulkan REQUIRED COMPONENTS volk)`，需 SDK 提供 `volk`
  - Windows：官方安装包会自动设置 `VULKAN_SDK` 环境变量
  - Linux：下载 SDK 后先 `source <VulkanSDK>/setup-env.sh`
- **SDL3 / SDL3_image**：
  - Windows：使用 `D:/library` 下的预编译包（通过缓存变量 `RAYTRACING_DEP_ROOT` 定位，默认 `D:/library`）
  - Linux：由系统包管理器提供
- **Vulkan 驱动**：需硬件驱动支持

### 各平台依赖

| 依赖 | Windows | Linux |
| --- | --- | --- |
| Vulkan SDK（含 volk） | 官方安装包（`VULKAN_SDK`） | 官方 SDK 或发行版包 |
| SDL3 | `D:/library/SDL3-3.4.14-mingw` 或 `-vc`（按编译器自动选择） | 系统库 `sdl3` |
| SDL3_image | `D:/library/SDL3_image-3.4.4-mingw` | 系统库 `sdl3-image` |
| glm / imgui | `Dependencies/`（来自 `D:/library`） | 同左 |
| Noto Sans SC | `assets/fonts/`（随仓库） | 同左 |

Linux（Debian / Ubuntu）安装系统依赖：

```bash
sudo apt install libsdl3-dev libsdl3-image-dev
```

## 构建与运行

### Windows（MinGW）

```bash
cd D:/Vulkan_Project/RayTracing_vulkan
cmake --preset windows-debug      # 配置
cmake --build  --preset windows-debug   # 编译
./build/bin/RayTracing.exe        # 运行
```

> 一键「编译并运行」：`cmake --build --preset windows-debug-run`
> 其它预设：`windows-release` / `windows-debug-run`

### Windows（Visual Studio / MSVC）

```bash
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Debug
```

> 注意：`D:/library` 目前只提供了 MinGW 版的 SDL3_image；用 MSVC 构建请先下载
> `SDL3_image-3.4.4-vc` 放入 `D:/library`。

### Linux

```bash
source /path/to/VulkanSDK/setup-env.sh   # 设置 VULKAN_SDK
cd RayTracing_vulkan
cmake --preset linux-debug
cmake --build --preset linux-debug
./build/bin/RayTracing
```

> 构建产物（exe / DLL / 字体）统一输出到 `build/bin`，随程序分发的 `fonts/` 会自动拷贝到 exe 旁。

## 常见问题

- **中文显示为方块？** 确认 `build/bin/fonts/NotoSansSC-VF.ttf` 存在（构建时会自动拷贝）。若被手动删除，程序会回退到不含中文的内置字体。
- **启动时出现 `image has not been acquired from VkSwapchainKHR` 校验警告？** 这是 ImGui docking 后端已知的无害 bug（[ocornut/imgui#9496](https://github.com/ocornut/imgui/issues/9496)），仅在启动 / 缩放窗口时出现，Release 构建（无校验层）不显示。
- 更多问题见 [`学习文档.md`](学习文档.md) 的 FAQ 章节。

## 致谢

- [TheCherno / RayTracing](https://github.com/TheCherno/RayTracing)：原始教程项目
- [Dear ImGui](https://github.com/ocornut/imgui)：GUI 库（docking 分支）
- [SDL](https://github.com/libsdl-org/SDL)：窗口与事件系统
- [Noto Sans SC](https://fonts.google.com/noto/specimen/Noto+Sans+SC)：字体（SIL OFL 1.1，见 `assets/fonts/OFL.txt`）