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

#include <boost/intrusive/list_hook.hpp>

#include "graphics/texture/Texture.h"
#include "platform/Platform.h"

class VulkanRenderer;

typedef boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>> VulkanTextureListHook;

//! Texture of the Vulkan renderer (T1.1: no GPU image yet, the image data is kept for the upload in T1.2)
class VulkanTexture final : public Texture, public VulkanTextureListHook {

public:

	explicit VulkanTexture(VulkanRenderer * renderer);
	~VulkanTexture() override;

	bool create() override;
	void upload() override;
	void destroy() override;

private:

	VulkanRenderer * m_renderer;

	friend class VulkanRenderer;

};

#endif // ARX_GRAPHICS_VULKAN_VULKANTEXTURE_H
