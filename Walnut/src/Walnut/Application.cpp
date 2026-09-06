// 让 Application.h -> Vulkan.h 中引入的 volk.h 在本编译单元内生成实现（仅此一处）
#define VOLK_IMPLEMENTATION

#include "Application.h"

//
// Vulkan 上下文：SDL3 + volk + Vulkan 1.4 + 动态渲染 + 同步2
// 写法参考 Nexus_GameEngine / NEXUS_RENDERING 的 VulkanContext
//

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_vulkan.h"

#include <stdio.h>          // printf, fprintf
#include <stdlib.h>         // abort
#include <glm/glm.hpp>
#include <iostream>

extern bool g_ApplicationRunning;

#ifdef WL_DEBUG
#define APP_USE_VULKAN_DEBUG
static VkDebugUtilsMessengerEXT g_DebugUtilsMessenger = VK_NULL_HANDLE;
#endif

static VkAllocationCallbacks* g_Allocator = nullptr;
static VkInstance             g_Instance = VK_NULL_HANDLE;
static VkPhysicalDevice       g_PhysicalDevice = VK_NULL_HANDLE;
static VkDevice               g_Device = VK_NULL_HANDLE;
static uint32_t               g_QueueFamily = (uint32_t)-1;
static VkQueue                g_Queue = VK_NULL_HANDLE;
static VkPipelineCache        g_PipelineCache = VK_NULL_HANDLE;
static VkDescriptorPool       g_DescriptorPool = VK_NULL_HANDLE;

static ImGui_ImplVulkanH_Window g_MainWindowData;
static uint32_t                 g_MinImageCount = 2;
static bool                     g_SwapChainRebuild = false;

// 每帧 in-flight 资源
static std::vector<std::vector<VkCommandBuffer>> s_AllocatedCommandBuffers;
static std::vector<std::vector<std::function<void()>>> s_ResourceFreeQueue;

// 与 g_MainWindowData.FrameIndex 不同，这里始终递增（0,1,2,0,1,2...）
static uint32_t s_CurrentFrameIndex = 0;

static Walnut::Application* s_Instance = nullptr;

static constexpr uint32_t s_VulkanApiVersion = VK_API_VERSION_1_4;

void check_vk_result(VkResult err)
{
	if (err == VK_SUCCESS)
		return;
	fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
	if (err < 0)
		abort();
}

#ifdef APP_USE_VULKAN_DEBUG
static VKAPI_ATTR VkBool32 VKAPI_CALL DebugUtilsMessengerCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT messageType,
	const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
	void* pUserData)
{
	(void)severity; (void)messageType; (void)pUserData;
	fprintf(stderr, "[vulkan] Validation layer: %s\n", pCallbackData->pMessage);
	return VK_FALSE;
}
#endif // APP_USE_VULKAN_DEBUG

static bool IsExtensionAvailable(const std::vector<VkExtensionProperties>& properties, const char* extension)
{
	for (const VkExtensionProperties& p : properties)
		if (strcmp(p.extensionName, extension) == 0)
			return true;
	return false;
}

static void SetupVulkan(const std::vector<const char*>& instanceExtensions, SDL_Window* window, VkSurfaceKHR& outSurface)
{
	if (volkInitialize() != VK_SUCCESS)
	{
		std::cerr << "[vulkan] volkInitialize() failed!\n";
		abort();
	}

	// 创建 Vulkan 实例
	{
		VkApplicationInfo appInfo
		{
			.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
			.pApplicationName = s_Instance ? s_Instance->GetSpecification().Name.c_str() : "RayTracing",
			.applicationVersion = VK_MAKE_VERSION(1, 0, 0),
			.pEngineName = "Walnut",
			.engineVersion = VK_MAKE_VERSION(1, 0, 0),
			.apiVersion = s_VulkanApiVersion,
		};

		// 枚举可用的实例扩展，补齐可选扩展
		uint32_t propertiesCount = 0;
		vkEnumerateInstanceExtensionProperties(nullptr, &propertiesCount, nullptr);
		std::vector<VkExtensionProperties> properties(propertiesCount);
		vkEnumerateInstanceExtensionProperties(nullptr, &propertiesCount, properties.data());

		std::vector<const char*> extensions = instanceExtensions;
#ifdef APP_USE_VULKAN_DEBUG
		if (IsExtensionAvailable(properties, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
			extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif

		std::vector<const char*> layers;
#ifdef APP_USE_VULKAN_DEBUG
		layers.push_back("VK_LAYER_KHRONOS_validation");
#endif

		VkInstanceCreateInfo instanceCreateInfo
		{
			.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
			.pApplicationInfo = &appInfo,
			.enabledLayerCount = static_cast<uint32_t>(layers.size()),
			.ppEnabledLayerNames = layers.data(),
			.enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
			.ppEnabledExtensionNames = extensions.data(),
		};

		VkResult err = vkCreateInstance(&instanceCreateInfo, g_Allocator, &g_Instance);
		check_vk_result(err);

		// volk 加载实例级函数（含扩展函数）
		volkLoadInstance(g_Instance);

#ifdef APP_USE_VULKAN_DEBUG
		VkDebugUtilsMessengerCreateInfoEXT debugMessengerCreateInfo
		{
			.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
			.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
			.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
			.pfnUserCallback = DebugUtilsMessengerCallback,
			.pUserData = nullptr,
		};
		check_vk_result(vkCreateDebugUtilsMessengerEXT(g_Instance, &debugMessengerCreateInfo, g_Allocator, &g_DebugUtilsMessenger));
#endif // APP_USE_VULKAN_DEBUG
	}

	// 创建窗口表面（队列族选择需要查询呈现支持，必须先创建表面）
	if (!SDL_Vulkan_CreateSurface(window, g_Instance, g_Allocator, &outSurface))
	{
		fprintf(stderr, "Failed to create Vulkan surface: %s\n", SDL_GetError());
		abort();
	}

	// 选择物理设备（优先独显）
	{
		uint32_t gpuCount = 0;
		check_vk_result(vkEnumeratePhysicalDevices(g_Instance, &gpuCount, nullptr));
		IM_ASSERT(gpuCount > 0);

		std::vector<VkPhysicalDevice> gpus(gpuCount);
		check_vk_result(vkEnumeratePhysicalDevices(g_Instance, &gpuCount, gpus.data()));

		VkPhysicalDevice selected = gpus[0];
		for (VkPhysicalDevice gpu : gpus)
		{
			VkPhysicalDeviceProperties props;
			vkGetPhysicalDeviceProperties(gpu, &props);
			if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
			{
				selected = gpu;
				break;
			}
		}
		g_PhysicalDevice = selected;
	}

	// 选择支持图形 + 呈现的队列族
	{
		uint32_t count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &count, nullptr);
		std::vector<VkQueueFamilyProperties> queues(count);
		vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &count, queues.data());
		for (uint32_t i = 0; i < count; i++)
		{
			VkBool32 presentSupport = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, i, outSurface, &presentSupport);
			if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presentSupport)
			{
				g_QueueFamily = i;
				break;
			}
		}
		IM_ASSERT(g_QueueFamily != (uint32_t)-1);
	}

	// 创建逻辑设备（Vulkan 1.4，启用动态渲染 + 同步2）
	{
		const float queuePriority[] = { 1.0f };
		VkDeviceQueueCreateInfo queueInfo
		{
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueFamilyIndex = g_QueueFamily,
			.queueCount = 1,
			.pQueuePriorities = queuePriority,
		};

		// 查询设备特性支持
		VkPhysicalDeviceVulkan14Features supported14 { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES };
		VkPhysicalDeviceVulkan13Features supported13 { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &supported14 };
		VkPhysicalDeviceFeatures2 supportedFeatures { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &supported13 };
		vkGetPhysicalDeviceFeatures2(g_PhysicalDevice, &supportedFeatures);
		IM_ASSERT(supported13.synchronization2 && supported13.dynamicRendering);

		// 需要启用的特性链
		VkPhysicalDeviceVulkan14Features features14
		{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
			.pNext = nullptr,
		};
		VkPhysicalDeviceVulkan13Features features13
		{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
			.pNext = &features14,
			.synchronization2 = VK_TRUE,
			.dynamicRendering = VK_TRUE,
		};
		VkPhysicalDeviceFeatures2 features
		{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
			.pNext = &features13,
		};

		const std::vector<const char*> deviceExtensions { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
		VkDeviceCreateInfo deviceCreateInfo
		{
			.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			.pNext = &features,
			.queueCreateInfoCount = 1,
			.pQueueCreateInfos = &queueInfo,
			.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size()),
			.ppEnabledExtensionNames = deviceExtensions.data(),
			.pEnabledFeatures = nullptr, // 特性统一放 pNext 链
		};

		check_vk_result(vkCreateDevice(g_PhysicalDevice, &deviceCreateInfo, g_Allocator, &g_Device));
		volkLoadDevice(g_Device);
		vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
	}

	// 创建描述符池（ImGui + Walnut::Image 纹理用）
	{
		VkDescriptorPoolSize poolSizes[]
		{
			{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
			{ VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
		};
		VkDescriptorPoolCreateInfo poolInfo
		{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
			.maxSets = 1000,
			.poolSizeCount = static_cast<uint32_t>(IM_ARRAYSIZE(poolSizes)),
			.pPoolSizes = poolSizes,
		};
		check_vk_result(vkCreateDescriptorPool(g_Device, &poolInfo, g_Allocator, &g_DescriptorPool));
	}
}

static void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height)
{
	wd->Surface = surface;
	wd->UseDynamicRendering = true;

	// 检查 WSI 支持
	VkBool32 res = VK_FALSE;
	vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, wd->Surface, &res);
	if (res != VK_TRUE)
	{
		fprintf(stderr, "Error no WSI support on physical device 0\n");
		exit(-1);
	}

	// 选择表面格式
	const VkFormat requestSurfaceImageFormat[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM };
	const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
	wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(g_PhysicalDevice, wd->Surface, requestSurfaceImageFormat, (size_t)IM_ARRAYSIZE(requestSurfaceImageFormat), requestSurfaceColorSpace);

	// 选择呈现模式（垂直同步）
	const VkPresentModeKHR presentModes[] = { VK_PRESENT_MODE_FIFO_KHR };
	wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(g_PhysicalDevice, wd->Surface, &presentModes[0], IM_ARRAYSIZE(presentModes));

	// 创建交换链（动态渲染路径，不再创建 RenderPass / Framebuffer）
	IM_ASSERT(g_MinImageCount >= 2);
	ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, width, height, g_MinImageCount, 0);
}

static void CleanupVulkan()
{
	vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);

#ifdef APP_USE_VULKAN_DEBUG
	if (g_DebugUtilsMessenger)
		vkDestroyDebugUtilsMessengerEXT(g_Instance, g_DebugUtilsMessenger, g_Allocator);
#endif // APP_USE_VULKAN_DEBUG

	vkDestroyDevice(g_Device, g_Allocator);
	vkDestroyInstance(g_Instance, g_Allocator);
}

static void CleanupVulkanWindow(ImGui_ImplVulkanH_Window* wd)
{
	ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, wd, g_Allocator);
	vkDestroySurfaceKHR(g_Instance, wd->Surface, g_Allocator);
}

static void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* drawData)
{
	VkSemaphore imageAcquiredSemaphore = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
	VkSemaphore renderCompleteSemaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;

	VkResult err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX, imageAcquiredSemaphore, VK_NULL_HANDLE, &wd->FrameIndex);
	if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
	{
		g_SwapChainRebuild = true;
		return;
	}
	check_vk_result(err);

	s_CurrentFrameIndex = (s_CurrentFrameIndex + 1) % g_MainWindowData.ImageCount;

	ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
	{
		err = vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
		check_vk_result(err);
		err = vkResetFences(g_Device, 1, &fd->Fence);
		check_vk_result(err);
	}

	{
		// 释放延迟销毁的资源
		for (auto& func : s_ResourceFreeQueue[s_CurrentFrameIndex])
			func();
		s_ResourceFreeQueue[s_CurrentFrameIndex].clear();
	}

	{
		// 释放由 Application::GetCommandBuffer 分配的命令缓冲
		auto& allocatedCommandBuffers = s_AllocatedCommandBuffers[wd->FrameIndex];
		if (allocatedCommandBuffers.size() > 0)
		{
			vkFreeCommandBuffers(g_Device, fd->CommandPool, (uint32_t)allocatedCommandBuffers.size(), allocatedCommandBuffers.data());
			allocatedCommandBuffers.clear();
		}

		err = vkResetCommandPool(g_Device, fd->CommandPool, 0);
		check_vk_result(err);
		VkCommandBufferBeginInfo beginInfo
		{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		err = vkBeginCommandBuffer(fd->CommandBuffer, &beginInfo);
		check_vk_result(err);
	}

	// 动态渲染：把交换链图像转为颜色附件布局
	{
		VkImageMemoryBarrier2 imageBarrier
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			.srcAccessMask = 0,
			.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = fd->Backbuffer,
			.subresourceRange
			{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1,
			},
		};
		VkDependencyInfo depInfo
		{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.imageMemoryBarrierCount = 1,
			.pImageMemoryBarriers = &imageBarrier,
		};
		vkCmdPipelineBarrier2(fd->CommandBuffer, &depInfo);
	}

	// 开始动态渲染并提交 ImGui 绘制命令
	{
		VkRenderingAttachmentInfo colorAttachment
		{
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			.imageView = fd->BackbufferView,
			.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.resolveMode = VK_RESOLVE_MODE_NONE,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.clearValue = wd->ClearValue,
		};
		VkRenderingInfo renderingInfo
		{
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.renderArea
			{
				.offset{ .x = 0, .y = 0 },
				.extent{ .width = static_cast<uint32_t>(wd->Width), .height = static_cast<uint32_t>(wd->Height) },
			},
			.layerCount = 1,
			.colorAttachmentCount = 1,
			.pColorAttachments = &colorAttachment,
		};
		vkCmdBeginRendering(fd->CommandBuffer, &renderingInfo);
	}

	ImGui_ImplVulkan_RenderDrawData(drawData, fd->CommandBuffer);

	vkCmdEndRendering(fd->CommandBuffer);

	// 转回呈现布局
	{
		VkImageMemoryBarrier2 imageBarrier
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_NONE,
			.dstAccessMask = 0,
			.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = fd->Backbuffer,
			.subresourceRange
			{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1,
			},
		};
		VkDependencyInfo depInfo
		{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.imageMemoryBarrierCount = 1,
			.pImageMemoryBarriers = &imageBarrier,
		};
		vkCmdPipelineBarrier2(fd->CommandBuffer, &depInfo);
	}

	// 提交命令（同步2）
	{
		VkCommandBufferSubmitInfo cmdSubmitInfo
		{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
			.commandBuffer = fd->CommandBuffer,
		};
		VkSemaphoreSubmitInfo waitSemaphoreInfo
		{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			.semaphore = imageAcquiredSemaphore,
			.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		};
		VkSemaphoreSubmitInfo signalSemaphoreInfo
		{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			.semaphore = renderCompleteSemaphore,
			.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		};
		VkSubmitInfo2 submitInfo
		{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
			.waitSemaphoreInfoCount = 1,
			.pWaitSemaphoreInfos = &waitSemaphoreInfo,
			.commandBufferInfoCount = 1,
			.pCommandBufferInfos = &cmdSubmitInfo,
			.signalSemaphoreInfoCount = 1,
			.pSignalSemaphoreInfos = &signalSemaphoreInfo,
		};

		err = vkEndCommandBuffer(fd->CommandBuffer);
		check_vk_result(err);
		err = vkQueueSubmit2(g_Queue, 1, &submitInfo, fd->Fence);
		check_vk_result(err);
	}
}

static void FramePresent(ImGui_ImplVulkanH_Window* wd)
{
	if (g_SwapChainRebuild)
		return;

	VkSemaphore renderCompleteSemaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
	VkPresentInfoKHR presentInfo
	{
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &renderCompleteSemaphore,
		.swapchainCount = 1,
		.pSwapchains = &wd->Swapchain,
		.pImageIndices = &wd->FrameIndex,
	};

	VkResult err = vkQueuePresentKHR(g_Queue, &presentInfo);
	if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
	{
		g_SwapChainRebuild = true;
		return;
	}
	check_vk_result(err);
	wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}

namespace Walnut {

	Application::Application(const ApplicationSpecification& specification)
		: m_Specification(specification)
	{
		s_Instance = this;

		Init();
	}

	Application::~Application()
	{
		Shutdown();

		s_Instance = nullptr;
	}

	Application& Application::Get()
	{
		return *s_Instance;
	}

	// 加载随程序分发的 Noto Sans SC 字体路径（位于可执行文件同目录 fonts/ 下，
// 由 CMake 构建时从 assets/fonts 拷贝，不依赖系统字体）
static std::string GetNotoSansSCPath()
{
	const char* basePath = SDL_GetBasePath();
	if (!basePath)
		return "fonts/NotoSansSC-VF.ttf";
	return std::string(basePath) + "fonts/NotoSansSC-VF.ttf";
}

void Application::Init()
	{
		// 初始化 SDL3
		if (!SDL_Init(SDL_INIT_VIDEO))
		{
			std::cerr << "Could not initialize SDL3: " << SDL_GetError() << "\n";
			return;
		}

		// 创建窗口（Vulkan 上下文）
		const SDL_WindowFlags windowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
		m_WindowHandle = SDL_CreateWindow(m_Specification.Name.c_str(), (int)m_Specification.Width, (int)m_Specification.Height, windowFlags);
		if (m_WindowHandle == nullptr)
		{
			std::cerr << "Could not create SDL3 window: " << SDL_GetError() << "\n";
			return;
		}

		// 收集 SDL 要求的 Vulkan 实例扩展
		std::vector<const char*> instanceExtensions;
		{
			uint32_t sdlExtensionCount = 0;
			const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
			for (uint32_t i = 0; i < sdlExtensionCount; i++)
				instanceExtensions.push_back(sdlExtensions[i]);
		}

		// 初始化 Vulkan（实例 / 窗口表面 / 物理设备 / 逻辑设备 / 描述符池）
		VkSurfaceKHR surface;
		SetupVulkan(instanceExtensions, m_WindowHandle, surface);

		// 创建交换链
		int width, height;
		SDL_GetWindowSizeInPixels(m_WindowHandle, &width, &height);
		ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
		SetupVulkanWindow(wd, surface, width, height);

		s_AllocatedCommandBuffers.resize(wd->ImageCount);
		s_ResourceFreeQueue.resize(wd->ImageCount);

		// 设置 Dear ImGui 上下文
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO(); (void)io;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

		ImGui::StyleColorsDark();

		ImGuiStyle& style = ImGui::GetStyle();
		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			style.WindowRounding = 0.0f;
			style.Colors[ImGuiCol_WindowBg].w = 1.0f;
		}

		// 平台 / 渲染后端
		ImGui_ImplSDL3_InitForVulkan(m_WindowHandle);
		ImGui_ImplVulkan_InitInfo initInfo = {};
		initInfo.ApiVersion = s_VulkanApiVersion;
		initInfo.Instance = g_Instance;
		initInfo.PhysicalDevice = g_PhysicalDevice;
		initInfo.Device = g_Device;
		initInfo.QueueFamily = g_QueueFamily;
		initInfo.Queue = g_Queue;
		initInfo.DescriptorPool = g_DescriptorPool;
		initInfo.MinImageCount = g_MinImageCount;
		initInfo.ImageCount = wd->ImageCount;
		initInfo.PipelineCache = g_PipelineCache;
		initInfo.Allocator = g_Allocator;
		initInfo.CheckVkResultFn = check_vk_result;
		initInfo.UseDynamicRendering = true;
		initInfo.PipelineInfoMain.Subpass = 0;
		initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
		initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
		initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
		initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &wd->SurfaceFormat.format;
		initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
		ImGui_ImplVulkan_Init(&initInfo);

		// 加载随程序分发的 Noto Sans SC 作为默认字体。
		// 它同时包含拉丁与中文（GB2312 全集）字形，可免费商用（SIL OFL），不依赖系统字体
		const std::string fontPath = GetNotoSansSCPath();
		ImFont* defaultFont = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 20.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
		if (defaultFont)
		{
			io.FontDefault = defaultFont;
		}
		else
		{
			fprintf(stderr, "[imgui] 未找到字体 %s，将使用内置默认字体（不支持中文）\n", fontPath.c_str());
		}

		// 字体纹理由后端在 ImGui_ImplVulkan_NewFrame() 首次调用时自动上传
	}

	void Application::Shutdown()
	{
		for (auto& layer : m_LayerStack)
			layer->OnDetach();

		m_LayerStack.clear();

		// 清理
		VkResult err = vkDeviceWaitIdle(g_Device);
		check_vk_result(err);

		// 释放延迟销毁的资源
		for (auto& queue : s_ResourceFreeQueue)
		{
			for (auto& func : queue)
				func();
		}
		s_ResourceFreeQueue.clear();

		ImGui_ImplVulkan_Shutdown();
		ImGui_ImplSDL3_Shutdown();
		ImGui::DestroyContext();

		CleanupVulkanWindow(&g_MainWindowData);
		CleanupVulkan();

		SDL_DestroyWindow(m_WindowHandle);
		SDL_Quit();

		g_ApplicationRunning = false;
	}

	void Application::Run()
	{
		m_Running = true;

		ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
		ImVec4 clearColor = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
		ImGuiIO& io = ImGui::GetIO();

		// 主循环
		while (m_Running)
		{
			// 处理事件（输入、窗口缩放等）
			SDL_Event event;
			while (SDL_PollEvent(&event))
			{
				ImGui_ImplSDL3_ProcessEvent(&event);
				if (event.type == SDL_EVENT_QUIT)
					m_Running = false;
				if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(m_WindowHandle))
					m_Running = false;
				if (event.type == SDL_EVENT_WINDOW_RESIZED && event.window.windowID == SDL_GetWindowID(m_WindowHandle))
					g_SwapChainRebuild = true;
			}

			for (auto& layer : m_LayerStack)
				layer->OnUpdate(m_TimeStep);

			// 重建交换链
			if (g_SwapChainRebuild)
			{
				int newWidth, newHeight;
				SDL_GetWindowSizeInPixels(m_WindowHandle, &newWidth, &newHeight);
				if (newWidth > 0 && newHeight > 0)
				{
					ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
					ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, &g_MainWindowData, g_QueueFamily, g_Allocator, newWidth, newHeight, g_MinImageCount, 0);
					g_MainWindowData.FrameIndex = 0;

					s_AllocatedCommandBuffers.clear();
					s_AllocatedCommandBuffers.resize(g_MainWindowData.ImageCount);

					g_SwapChainRebuild = false;
				}
			}

			// 开始一帧 ImGui
			ImGui_ImplVulkan_NewFrame();
			ImGui_ImplSDL3_NewFrame();
			ImGui::NewFrame();

			{
				static ImGuiDockNodeFlags dockspaceFlags = ImGuiDockNodeFlags_None;

				ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDocking;
				if (m_MenubarCallback)
					windowFlags |= ImGuiWindowFlags_MenuBar;

				const ImGuiViewport* viewport = ImGui::GetMainViewport();
				ImGui::SetNextWindowPos(viewport->WorkPos);
				ImGui::SetNextWindowSize(viewport->WorkSize);
				ImGui::SetNextWindowViewport(viewport->ID);
				ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
				ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
				windowFlags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
				windowFlags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

				if (dockspaceFlags & ImGuiDockNodeFlags_PassthruCentralNode)
					windowFlags |= ImGuiWindowFlags_NoBackground;

				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
				ImGui::Begin("DockSpace Demo", nullptr, windowFlags);
				ImGui::PopStyleVar();

				ImGui::PopStyleVar(2);

				ImGuiIO& ioRef = ImGui::GetIO();
				if (ioRef.ConfigFlags & ImGuiConfigFlags_DockingEnable)
				{
					ImGuiID dockspaceId = ImGui::GetID("VulkanAppDockspace");
					ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), dockspaceFlags);
				}

				if (m_MenubarCallback)
				{
					if (ImGui::BeginMenuBar())
					{
						m_MenubarCallback();
						ImGui::EndMenuBar();
					}
				}

				for (auto& layer : m_LayerStack)
					layer->OnUIRender();

				ImGui::End();
			}

			// 渲染
			ImGui::Render();
			ImDrawData* mainDrawData = ImGui::GetDrawData();
			const bool mainIsMinimized = (mainDrawData->DisplaySize.x <= 0.0f || mainDrawData->DisplaySize.y <= 0.0f);
			wd->ClearValue.color.float32[0] = clearColor.x * clearColor.w;
			wd->ClearValue.color.float32[1] = clearColor.y * clearColor.w;
			wd->ClearValue.color.float32[2] = clearColor.z * clearColor.w;
			wd->ClearValue.color.float32[3] = clearColor.w;
			if (!mainIsMinimized)
				FrameRender(wd, mainDrawData);

			// 多视口平台窗口
			if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
			{
				ImGui::UpdatePlatformWindows();
				ImGui::RenderPlatformWindowsDefault();
			}

			// 呈现主窗口
			if (!mainIsMinimized)
				FramePresent(wd);

			float time = GetTime();
			m_FrameTime = time - m_LastFrameTime;
			m_TimeStep = glm::min<float>(m_FrameTime, 0.0333f);
			m_LastFrameTime = time;
		}
	}

	void Application::Close()
	{
		m_Running = false;
	}

	float Application::GetTime()
	{
		return (float)SDL_GetTicksNS() * 0.000000001f;
	}

	VkInstance Application::GetInstance()
	{
		return g_Instance;
	}

	VkPhysicalDevice Application::GetPhysicalDevice()
	{
		return g_PhysicalDevice;
	}

	VkDevice Application::GetDevice()
	{
		return g_Device;
	}

	VkCommandBuffer Application::GetCommandBuffer(bool begin)
	{
		(void)begin;

		ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;

		VkCommandPool commandPool = wd->Frames[wd->FrameIndex].CommandPool;

		VkCommandBufferAllocateInfo allocInfo
		{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = commandPool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};

		VkCommandBuffer& commandBuffer = s_AllocatedCommandBuffers[wd->FrameIndex].emplace_back();
		VkResult err = vkAllocateCommandBuffers(g_Device, &allocInfo, &commandBuffer);
		check_vk_result(err);

		VkCommandBufferBeginInfo beginInfo
		{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		err = vkBeginCommandBuffer(commandBuffer, &beginInfo);
		check_vk_result(err);

		return commandBuffer;
	}

	void Application::FlushCommandBuffer(VkCommandBuffer commandBuffer)
	{
		constexpr uint64_t DEFAULT_FENCE_TIMEOUT = 100000000000;

		VkCommandBufferSubmitInfo cmdSubmitInfo
		{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
			.commandBuffer = commandBuffer,
		};
		VkSubmitInfo2 submitInfo
		{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
			.commandBufferInfoCount = 1,
			.pCommandBufferInfos = &cmdSubmitInfo,
		};
		VkResult err = vkEndCommandBuffer(commandBuffer);
		check_vk_result(err);

		// 创建 fence 保证命令缓冲执行完毕
		VkFenceCreateInfo fenceCreateInfo
		{
			.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
			.flags = 0,
		};
		VkFence fence;
		err = vkCreateFence(g_Device, &fenceCreateInfo, nullptr, &fence);
		check_vk_result(err);

		err = vkQueueSubmit2(g_Queue, 1, &submitInfo, fence);
		check_vk_result(err);

		err = vkWaitForFences(g_Device, 1, &fence, VK_TRUE, DEFAULT_FENCE_TIMEOUT);
		check_vk_result(err);

		vkDestroyFence(g_Device, fence, nullptr);
	}

	void Application::SubmitResourceFree(std::function<void()>&& func)
	{
		s_ResourceFreeQueue[s_CurrentFrameIndex].emplace_back(func);
	}

}