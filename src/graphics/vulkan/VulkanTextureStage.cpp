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

#include "graphics/vulkan/VulkanTextureStage.h"

#include "graphics/vulkan/VulkanRenderer.h"
#include "graphics/vulkan/VulkanTexture.h"

VulkanTextureStage::VulkanTextureStage(VulkanRenderer * renderer, unsigned stage)
	: TextureStage(stage)
	, m_renderer(renderer)
	, m_texture(nullptr)
	, m_colorOp(stage == 0 ? OpModulate : OpDisable)
	, m_alphaOp(stage == 0 ? OpSelectArg1 : OpDisable)
	, m_lodBias(0.f)
{ }

VulkanTextureStage::~VulkanTextureStage() {
	resetTexture();
}

Texture * VulkanTextureStage::getTexture() const {
	return m_texture;
}

void VulkanTextureStage::setTexture(Texture * texture) {

	arx_assert(texture != nullptr);

	m_texture = static_cast<VulkanTexture *>(texture);
}

void VulkanTextureStage::resetTexture() {
	m_texture = nullptr;
}

void VulkanTextureStage::setColorOp(TextureOp op) {
	m_colorOp = op;
}

void VulkanTextureStage::setAlphaOp(TextureOp op) {
	m_alphaOp = op;
}

void VulkanTextureStage::setMipMapLODBias(float bias) {
	m_lodBias = bias;
}

VulkanTexture * VulkanTextureStage::getActiveTexture() const {

	// Like OpenGL, a stage without a valid texture passes through the previous color
	if(!isEnabled() || !m_texture || !m_texture->view()) {
		return nullptr;
	}

	return m_texture;
}

VkSampler VulkanTextureStage::getSampler(const VulkanTexture * texture) const {

	arx_assert(texture);

	// TODO: Fix callers and change this into an assert/error/warning (same as in the OpenGL backend)
	WrapMode wrapMode = texture->isNPOT() ? WrapClamp : getWrapMode();

	return m_renderer->getSampler(wrapMode, getMinFilter(), getMagFilter(), texture->hasMipmaps(), m_lodBias);
}
