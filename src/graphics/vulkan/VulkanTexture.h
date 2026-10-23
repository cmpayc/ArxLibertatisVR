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
#include "platform/Platform.h"

class VulkanRenderer;

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

	//! NPOT textures are always clamped to match the OpenGL backend.
	bool isNPOT() const { return m_isNPOT; }

private:

	void releaseImage();

	VulkanRenderer * m_renderer;

	VkImage m_texture;
	VmaAllocation m_allocation;
	VkImageView m_view;

	bool m_isNPOT;

	friend class VulkanRenderer;

};

#endif // ARX_GRAPHICS_VULKAN_VULKANTEXTURE_H
