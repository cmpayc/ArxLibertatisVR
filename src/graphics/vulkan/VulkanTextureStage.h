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

#ifndef ARX_GRAPHICS_VULKAN_VULKANTEXTURESTAGE_H
#define ARX_GRAPHICS_VULKAN_VULKANTEXTURESTAGE_H

#include <volk.h>

#include "graphics/texture/TextureStage.h"
#include "platform/Platform.h"

class VulkanRenderer;
class VulkanTexture;

class VulkanTextureStage final : public TextureStage {

public:

	VulkanTextureStage(VulkanRenderer * renderer, unsigned textureStage);
	~VulkanTextureStage() override;

	Texture * getTexture() const override;
	void setTexture(Texture * texture) override;
	void resetTexture() override;

	void setColorOp(TextureOp textureOp) override;
	void setAlphaOp(TextureOp textureOp) override;

	void setMipMapLODBias(float bias) override;

	TextureOp getColorOp() const { return m_colorOp; }
	TextureOp getAlphaOp() const { return m_alphaOp; }
	float getMipMapLODBias() const { return m_lodBias; }

	bool isEnabled() const { return m_colorOp != OpDisable || m_alphaOp != OpDisable; }

	//! \return the bound texture if it can be sampled, nullptr otherwise
	VulkanTexture * getActiveTexture() const;

	//! \return the sampler matching the stage and texture settings
	VkSampler getSampler(const VulkanTexture * texture) const;

private:

	VulkanRenderer * m_renderer;

	VulkanTexture * m_texture;

	TextureOp m_colorOp;
	TextureOp m_alphaOp;

	float m_lodBias;

	friend class VulkanRenderer;

};

#endif // ARX_GRAPHICS_VULKAN_VULKANTEXTURESTAGE_H
