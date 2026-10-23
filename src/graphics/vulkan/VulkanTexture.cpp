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

#include "graphics/vulkan/VulkanRenderer.h"

VulkanTexture::VulkanTexture(VulkanRenderer * renderer)
	: m_renderer(renderer)
{ }

VulkanTexture::~VulkanTexture() {
	destroy();
}

bool VulkanTexture::create() {

	// Vulkan supports non-power-of-two textures with mipmaps and wrapping
	m_storedSize = getSize();

	return true;
}

void VulkanTexture::upload() {
	// TODO T1.2 upload m_image into a device-local image
}

void VulkanTexture::destroy() {
	if(m_renderer) {
		m_renderer->onTextureDestroyed(this);
	}
}
