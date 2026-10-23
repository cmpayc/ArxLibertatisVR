/*
 * Copyright 2026 Arx Libertatis VR contributors <kazakov.s.fl@gmail.com>
 *
 * This file is part of Arx Libertatis.
 *
 * Arx Libertatis is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Arx Libertatis is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Arx Libertatis.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "graphics/vulkan/VulkanTexture.h"

#include <algorithm>
#include <cstring>

#include "graphics/Math.h"
#include "graphics/vulkan/VulkanRenderer.h"
#include "io/log/Logger.h"

VulkanTexture::VulkanTexture(VulkanRenderer * renderer)
	: m_renderer(renderer)
	, m_texture(VK_NULL_HANDLE)
	, m_allocation(VK_NULL_HANDLE)
	, m_view(VK_NULL_HANDLE)
	, m_textureSize(0)
	, m_textureMipmaps(false)
	, m_relief(VK_NULL_HANDLE)
	, m_reliefAllocation(VK_NULL_HANDLE)
	, m_reliefView(VK_NULL_HANDLE)
	, m_isNPOT(false)
{ }

VulkanTexture::~VulkanTexture() {
	destroy();
}

bool VulkanTexture::create() {

	// Vulkan supports non-power-of-two textures with mipmaps and wrapping
	m_storedSize = getSize();

	Vec2i nextPowerOfTwo(GetNextPowerOf2(unsigned(getSize().x)), GetNextPowerOf2(unsigned(getSize().y)));
	m_isNPOT = (getSize() != nextPowerOfTwo);

	m_material = guessTextureMaterial(getFileName().string());

	return true;
}

/*!
 * Expand an image to RGBA8 using the same component mapping as the OpenGL
 * fixed-function pipeline uses for the corresponding texture base format.
 */
static void convertToRGBA(const Image & image, bool intensity, u8 * out) {

	size_t pixels = image.getWidth() * image.getHeight();

	const u8 * in = image.getData();
	u8 * o = out;

	switch(image.getFormat()) {

		case Image::Format_L8: {
			for(size_t i = 0; i < pixels; i++, o += 4) {
				o[0] = o[1] = o[2] = in[i];
				o[3] = intensity ? in[i] : 255;
			}
			break;
		}

		case Image::Format_A8: {
			for(size_t i = 0; i < pixels; i++, o += 4) {
				o[0] = o[1] = o[2] = 0;
				o[3] = in[i];
			}
			break;
		}

		case Image::Format_L8A8: {
			for(size_t i = 0; i < pixels; i++, o += 4, in += 2) {
				o[0] = o[1] = o[2] = in[0];
				o[3] = in[1];
			}
			break;
		}

		case Image::Format_R8G8B8: {
			for(size_t i = 0; i < pixels; i++, o += 4, in += 3) {
				o[0] = in[0];
				o[1] = in[1];
				o[2] = in[2];
				o[3] = 255;
			}
			break;
		}

		case Image::Format_B8G8R8: {
			for(size_t i = 0; i < pixels; i++, o += 4, in += 3) {
				o[0] = in[2];
				o[1] = in[1];
				o[2] = in[0];
				o[3] = 255;
			}
			break;
		}

		case Image::Format_R8G8B8A8: {
			std::copy(in, in + pixels * 4, o);
			break;
		}

		case Image::Format_B8G8R8A8: {
			for(size_t i = 0; i < pixels; i++, o += 4, in += 4) {
				o[0] = in[2];
				o[1] = in[1];
				o[2] = in[0];
				o[3] = in[3];
			}
			break;
		}

		default: {
			arx_assert_msg(false, "Unsupported image format: %ld", long(image.getFormat()));
			std::fill(out, out + pixels * 4, u8(255));
		}

	}

}

static void imageBarrier(VkCommandBuffer commands, VkImage image, uint32_t level, uint32_t levelCount,
                         VkImageLayout oldLayout, VkImageLayout newLayout,
                         VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                         VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {

	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	barrier.srcAccessMask = srcAccess;
	barrier.dstAccessMask = dstAccess;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level, levelCount, 0, 1 };

	vkCmdPipelineBarrier(commands, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

}

void VulkanTexture::upload() {

	Vec2i size = getSize();
	if(!m_renderer || size.x <= 0 || size.y <= 0 || !m_image.isValid()) {
		return;
	}

	// Draws recorded before this upload may still use the previous image
	releaseImage();

	uint32_t width = uint32_t(size.x);
	uint32_t height = uint32_t(size.y);
	uint32_t levels = hasMipmaps() ? vulkanMipLevels(width, height) : 1;

	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = VK_FORMAT_R8G8B8A8_UNORM;
	info.extent = { width, height, 1 };
	info.mipLevels = levels;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
	             | (levels > 1 ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo allocation = {};
	allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

	VkResult result = vmaCreateImage(m_renderer->allocator(), &info, &allocation, &m_texture, &m_allocation, nullptr);
	if(result != VK_SUCCESS) {
		LogError << "Failed to create " << size.x << "x" << size.y << " Vulkan texture " << getFileName();
		m_texture = VK_NULL_HANDLE;
		m_allocation = VK_NULL_HANDLE;
		return;
	}

	VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	view.image = m_texture;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = info.format;
	view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
	if(vkCreateImageView(m_renderer->device(), &view, nullptr, &m_view) != VK_SUCCESS) {
		LogError << "Failed to create Vulkan texture view for " << getFileName();
		m_view = VK_NULL_HANDLE;
		releaseImage();
		return;
	}

	VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
	VkBuffer staging = VK_NULL_HANDLE;
	void * data = m_renderer->createStagingBuffer(bytes, staging);
	if(!data) {
		releaseImage();
		return;
	}
	convertToRGBA(m_image, isIntensity(), static_cast<u8 *>(data));

	VkCommandBuffer commands = m_renderer->beginUpload();

	imageBarrier(commands, m_texture, 0, levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

	VkBufferImageCopy copy = {};
	copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	copy.imageExtent = { width, height, 1 };
	vkCmdCopyBufferToImage(commands, staging, m_texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

	recordVulkanMipmaps(commands, m_texture, width, height, levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

	m_textureSize = size;
	m_textureMipmaps = (levels > 1);

	// The contents changed
	updateReliefMap();

}

void VulkanTexture::updateReliefMap() {

	// Only textures for the 3D scene have mipmaps
	if(m_reliefView || !m_view || !m_textureMipmaps || !m_renderer || !m_renderer->reliefMapsEnabled()) {
		return;
	}

	m_renderer->createReliefMap(m_view, m_textureSize, m_relief, m_reliefAllocation, m_reliefView);

}

void VulkanTexture::releaseReliefMap() {

	if(!m_relief || !m_renderer) {
		return;
	}

	VkDevice device = m_renderer->device();
	VmaAllocator allocator = m_renderer->allocator();
	VkImage image = m_relief;
	VmaAllocation allocation = m_reliefAllocation;
	VkImageView view = m_reliefView;
	m_renderer->deferRelease([device, allocator, image, allocation, view]() {
		if(view) {
			vkDestroyImageView(device, view, nullptr);
		}
		vmaDestroyImage(allocator, image, allocation);
	});

	m_relief = VK_NULL_HANDLE;
	m_reliefAllocation = VK_NULL_HANDLE;
	m_reliefView = VK_NULL_HANDLE;

}

uint32_t vulkanMipLevels(uint32_t width, uint32_t height) {
	uint32_t levels = 1;
	while((std::max(width, height) >> levels) != 0) {
		levels++;
	}
	return levels;
}

void recordVulkanMipmaps(VkCommandBuffer commands, VkImage image, uint32_t width, uint32_t height, uint32_t levels,
                         VkImageLayout firstLayout, VkPipelineStageFlags firstStage, VkAccessFlags firstAccess) {

	if(levels <= 1) {
		imageBarrier(commands, image, 0, 1, firstLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, firstStage,
		             firstAccess, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
		return;
	}

	imageBarrier(commands, image, 1, levels - 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

	// Box-filtered mipmaps like glGenerateMipmap and Direct3D 11 GenerateMips
	int32_t levelWidth = int32_t(width);
	int32_t levelHeight = int32_t(height);
	for(uint32_t level = 1; level < levels; level++) {

		if(level == 1) {
			imageBarrier(commands, image, 0, 1, firstLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, firstStage,
			             firstAccess, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		} else {
			imageBarrier(commands, image, level - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
			             VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		}

		int32_t nextWidth = std::max(levelWidth / 2, 1);
		int32_t nextHeight = std::max(levelHeight / 2, 1);

		VkImageBlit blit = {};
		blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1 };
		blit.srcOffsets[1] = { levelWidth, levelHeight, 1 };
		blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
		blit.dstOffsets[1] = { nextWidth, nextHeight, 1 };
		vkCmdBlitImage(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		               1, &blit, VK_FILTER_LINEAR);

		levelWidth = nextWidth;
		levelHeight = nextHeight;
	}

	imageBarrier(commands, image, 0, levels - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
	             VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	imageBarrier(commands, image, levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
	             VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

}

void VulkanTexture::releaseImage() {

	releaseReliefMap();

	if(!m_texture || !m_renderer) {
		return;
	}

	VkDevice device = m_renderer->device();
	VmaAllocator allocator = m_renderer->allocator();
	VkImage image = m_texture;
	VmaAllocation allocation = m_allocation;
	VkImageView view = m_view;
	m_renderer->deferRelease([device, allocator, image, allocation, view]() {
		if(view) {
			vkDestroyImageView(device, view, nullptr);
		}
		vmaDestroyImage(allocator, image, allocation);
	});

	m_texture = VK_NULL_HANDLE;
	m_allocation = VK_NULL_HANDLE;
	m_view = VK_NULL_HANDLE;

}

void VulkanTexture::destroy() {

	releaseImage();

	if(m_renderer) {
		m_renderer->onTextureDestroyed(this);
	}

}
