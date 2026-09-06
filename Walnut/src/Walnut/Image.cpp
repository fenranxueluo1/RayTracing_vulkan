#include "Image.h"

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

#include "Application.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <iostream>

namespace Walnut {

	namespace Utils {

		static uint32_t GetVulkanMemoryType(VkMemoryPropertyFlags properties, uint32_t type_bits)
		{
			VkPhysicalDeviceMemoryProperties prop;
			vkGetPhysicalDeviceMemoryProperties(Application::GetPhysicalDevice(), &prop);
			for (uint32_t i = 0; i < prop.memoryTypeCount; i++)
			{
				if ((prop.memoryTypes[i].propertyFlags & properties) == properties && type_bits & (1 << i))
					return i;
			}
			
			return 0xffffffff;
		}

		static uint32_t BytesPerPixel(ImageFormat format)
		{
			switch (format)
			{
				case ImageFormat::RGBA:    return 4;
				case ImageFormat::RGBA32F: return 16;
			}
			return 0;
		}
		
		static VkFormat WalnutFormatToVulkanFormat(ImageFormat format)
		{
			switch (format)
			{
				case ImageFormat::RGBA:    return VK_FORMAT_R8G8B8A8_UNORM;
				case ImageFormat::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
			}
			return (VkFormat)0;
		}

	}

	Image::Image(std::string_view path)
		: m_Filepath(path)
	{
		// 用 SDL3_image 解码图片（PNG / JPG / TGA / BMP / HDR 等）
		SDL_Surface* surface = IMG_Load(m_Filepath.c_str());
		if (!surface)
		{
			std::cerr << "Failed to load image '" << m_Filepath << "': " << SDL_GetError() << "\n";
			return;
		}

		// HDR（如 .hdr / .pic）会被解码为浮点格式，保留为 RGBA32F
		if (SDL_ISPIXELFORMAT_FLOAT(surface->format))
		{
			SDL_Surface* floatSurface = surface;
			if (surface->format != SDL_PIXELFORMAT_RGBA128_FLOAT)
				floatSurface = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA128_FLOAT);
			SDL_DestroySurface(surface);
			if (!floatSurface)
			{
				std::cerr << "Failed to convert HDR image '" << m_Filepath << "': " << SDL_GetError() << "\n";
				return;
			}

			m_Format = ImageFormat::RGBA32F;
			m_Width = floatSurface->w;
			m_Height = floatSurface->h;
			AllocateMemory(m_Width * m_Height * Utils::BytesPerPixel(m_Format));
			SetData(floatSurface->pixels);
			SDL_DestroySurface(floatSurface);
			return;
		}

		// 其余格式统一转为 SDL_PIXELFORMAT_RGBA32 —— 即当前平台上"内存中按 R,G,B,A
		// 逐字节排列"的格式别名，与 VK_FORMAT_R8G8B8A8_UNORM 的字节序一致（避免红蓝颠倒）
		SDL_Surface* converted = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
		SDL_DestroySurface(surface);
		if (!converted)
		{
			std::cerr << "Failed to convert image '" << m_Filepath << "': " << SDL_GetError() << "\n";
			return;
		}

		m_Format = ImageFormat::RGBA;
		m_Width = converted->w;
		m_Height = converted->h;
		AllocateMemory(m_Width * m_Height * Utils::BytesPerPixel(m_Format));
		SetData(converted->pixels);
		SDL_DestroySurface(converted);
	}

	Image::Image(uint32_t width, uint32_t height, ImageFormat format, const void* data)
		: m_Width(width), m_Height(height), m_Format(format)
	{
		AllocateMemory(m_Width * m_Height * Utils::BytesPerPixel(m_Format));
		if (data)
			SetData(data);
	}

	Image::~Image()
	{
		Release();
	}

	void Image::AllocateMemory(uint64_t size)
	{
		VkDevice device = Application::GetDevice();

		VkResult err;
		
		VkFormat vulkanFormat = Utils::WalnutFormatToVulkanFormat(m_Format);

		// 创建图像
		{
			VkImageCreateInfo info
			{
				.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
				.imageType = VK_IMAGE_TYPE_2D,
				.format = vulkanFormat,
				.extent{ .width = m_Width, .height = m_Height, .depth = 1 },
				.mipLevels = 1,
				.arrayLayers = 1,
				.samples = VK_SAMPLE_COUNT_1_BIT,
				.tiling = VK_IMAGE_TILING_OPTIMAL,
				.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
				.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			};
			err = vkCreateImage(device, &info, nullptr, &m_Image);
			check_vk_result(err);

			VkMemoryRequirements req;
			vkGetImageMemoryRequirements(device, m_Image, &req);
			VkMemoryAllocateInfo allocInfo
			{
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.allocationSize = req.size,
				.memoryTypeIndex = Utils::GetVulkanMemoryType(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, req.memoryTypeBits),
			};
			err = vkAllocateMemory(device, &allocInfo, nullptr, &m_Memory);
			check_vk_result(err);
			err = vkBindImageMemory(device, m_Image, m_Memory, 0);
			check_vk_result(err);
		}

		// 创建图像视图
		{
			VkImageViewCreateInfo info
			{
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.image = m_Image,
				.viewType = VK_IMAGE_VIEW_TYPE_2D,
				.format = vulkanFormat,
				.subresourceRange
				{
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.levelCount = 1,
					.layerCount = 1,
				},
			};
			err = vkCreateImageView(device, &info, nullptr, &m_ImageView);
			check_vk_result(err);
		}

		// 注册到 ImGui（使用后端的采样器，SAMPLED_IMAGE 描述符）
		m_DescriptorSet = ImGui_ImplVulkan_AddTexture(m_ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}

	void Image::Release()
	{
		Application::SubmitResourceFree([imageView = m_ImageView, image = m_Image,
			memory = m_Memory, stagingBuffer = m_StagingBuffer, stagingBufferMemory = m_StagingBufferMemory]()
		{
			VkDevice device = Application::GetDevice();

			vkDestroyImageView(device, imageView, nullptr);
			vkDestroyImage(device, image, nullptr);
			vkFreeMemory(device, memory, nullptr);
			vkDestroyBuffer(device, stagingBuffer, nullptr);
			vkFreeMemory(device, stagingBufferMemory, nullptr);
		});

		m_ImageView = nullptr;
		m_Image = nullptr;
		m_Memory = nullptr;
		m_StagingBuffer = nullptr;
		m_StagingBufferMemory = nullptr;
	}

	void Image::SetData(const void* data)
	{
		VkDevice device = Application::GetDevice();

		size_t upload_size = m_Width * m_Height * Utils::BytesPerPixel(m_Format);

		VkResult err;

		if (!m_StagingBuffer)
		{
			// 创建暂存缓冲
			{
				VkBufferCreateInfo bufferInfo
				{
					.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
					.size = upload_size,
					.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
					.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
				};
				err = vkCreateBuffer(device, &bufferInfo, nullptr, &m_StagingBuffer);
				check_vk_result(err);

				VkMemoryRequirements req;
				vkGetBufferMemoryRequirements(device, m_StagingBuffer, &req);
				m_AlignedSize = req.size;
				VkMemoryAllocateInfo allocInfo
				{
					.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
					.allocationSize = req.size,
					.memoryTypeIndex = Utils::GetVulkanMemoryType(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, req.memoryTypeBits),
				};
				err = vkAllocateMemory(device, &allocInfo, nullptr, &m_StagingBufferMemory);
				check_vk_result(err);
				err = vkBindBufferMemory(device, m_StagingBuffer, m_StagingBufferMemory, 0);
				check_vk_result(err);
			}

		}

		// 上传到暂存缓冲
		{
			char* map = NULL;
			err = vkMapMemory(device, m_StagingBufferMemory, 0, m_AlignedSize, 0, (void**)(&map));
			check_vk_result(err);
			memcpy(map, data, upload_size);
			VkMappedMemoryRange range
			{
				.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
				.memory = m_StagingBufferMemory,
				.size = m_AlignedSize,
			};
			err = vkFlushMappedMemoryRanges(device, 1, &range);
			check_vk_result(err);
			vkUnmapMemory(device, m_StagingBufferMemory);
		}

		// 拷贝到图像（同步2 屏障）
		{
			VkCommandBuffer commandBuffer = Application::GetCommandBuffer(true);

			VkImageMemoryBarrier2 copyBarrier
			{
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
				.srcAccessMask = 0,
				.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
				.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
				.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = m_Image,
				.subresourceRange
				{
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.levelCount = 1,
					.layerCount = 1,
				},
			};
			VkDependencyInfo copyDepInfo
			{
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.imageMemoryBarrierCount = 1,
				.pImageMemoryBarriers = &copyBarrier,
			};
			vkCmdPipelineBarrier2(commandBuffer, &copyDepInfo);

			VkBufferImageCopy region
			{
				.imageSubresource{ .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
				.imageExtent{ .width = m_Width, .height = m_Height, .depth = 1 },
			};
			vkCmdCopyBufferToImage(commandBuffer, m_StagingBuffer, m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

			VkImageMemoryBarrier2 useBarrier
			{
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
				.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
				.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
				.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
				.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = m_Image,
				.subresourceRange
				{
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.levelCount = 1,
					.layerCount = 1,
				},
			};
			VkDependencyInfo useDepInfo
			{
				.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				.imageMemoryBarrierCount = 1,
				.pImageMemoryBarriers = &useBarrier,
			};
			vkCmdPipelineBarrier2(commandBuffer, &useDepInfo);

			Application::FlushCommandBuffer(commandBuffer);
		}
	}

	void Image::Resize(uint32_t width, uint32_t height)
	{
		if (m_Image && m_Width == width && m_Height == height)
			return;

		m_Width = width;
		m_Height = height;

		Release();
		AllocateMemory(m_Width * m_Height * Utils::BytesPerPixel(m_Format));
	}

}