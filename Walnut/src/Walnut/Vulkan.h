#pragma once

// 统一使用 volk 作为 Vulkan 加载器（写法参考 Nexus_GameEngine / NEXUS_RENDERING 的 VulkanContext）
// - 不引入 Vulkan 平台函数原型（VK_NO_PROTOTYPES），全部符号由 volk 动态加载
// - volk 的 IMPLEMENTATION 在 Application.cpp 中提供，这里只声明
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <volk.h>