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

#ifndef ARX_GRAPHICS_VULKAN_VULKANTEXTURE_H
#define ARX_GRAPHICS_VULKAN_VULKANTEXTURE_H

#include <volk.h>
#include <vk_mem_alloc.h>

#include <boost/intrusive/list_hook.hpp>

#include "graphics/texture/Texture.h"
#include "graphics/texture/TextureMaterial.h"
#include "platform/Platform.h"

class VulkanRenderer;

/*!
 * Record the box-filtered mip levels of an image from its first level, like glGenerateMipmap
 *
 * The other levels can have any contents. All levels end up in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
 */
void recordVulkanMipmaps(VkCommandBuffer commands, VkImage image, uint32_t width, uint32_t height, uint32_t levels,
                         VkImageLayout firstLayout, VkPipelineStageFlags firstStage, VkAccessFlags firstAccess);

//! Mip levels of a full mip chain
uint32_t vulkanMipLevels(uint32_t width, uint32_t height);

typedef boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>> VulkanTextureListHook;

/*!
 * Texture of the Vulkan renderer: a device-local RGBA8 image with mipmaps generated on the GPU
 *
 * Each upload goes into a new image, the previous one is released once the frames using it finished.
 * Like with the other backends, draws recorded before the upload still see the old contents.
 */
class VulkanTexture final : public Texture, public VulkanTextureListHook {

public:

	explicit VulkanTexture(VulkanRenderer * renderer);
	~VulkanTexture() override;

	bool create() override;
	void upload() override;
	void destroy() override;

	VkImageView view() const { return m_view; }

	//! Slope and height of the texture brightness for normal and parallax mapping, VK_NULL_HANDLE if not generated
	VkImageView reliefView() const { return m_reliefView; }

	//! Generate the relief map if the texture can be used for normal mapping and does not have one yet
	void updateReliefMap();
	void releaseReliefMap();

	//! Specular highlights and reflections of the surface, guessed from the material in the file name
	const TextureMaterial & material() const { return m_material; }

	//! NPOT textures are always clamped to match the OpenGL backend.
	bool isNPOT() const { return m_isNPOT; }

private:

	void releaseImage();

	VulkanRenderer * m_renderer;

	VkImage m_texture;
	VmaAllocation m_allocation;
	VkImageView m_view;
	Vec2i m_textureSize;
	bool m_textureMipmaps;

	VkImage m_relief;
	VmaAllocation m_reliefAllocation;
	VkImageView m_reliefView;

	TextureMaterial m_material;
	bool m_isNPOT;

	friend class VulkanRenderer;

};

#endif // ARX_GRAPHICS_VULKAN_VULKANTEXTURE_H
