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

#include "graphics/d3d11/D3D11TextureStage.h"

#include <cfloat>
#include <cstring>

#include "graphics/d3d11/D3D11Renderer.h"
#include "graphics/d3d11/D3D11Texture.h"
#include "io/log/Logger.h"

D3D11TextureStage::D3D11TextureStage(D3D11Renderer * renderer, unsigned stage)
	: TextureStage(stage)
	, m_renderer(renderer)
	, m_texture(nullptr)
	, m_colorOp(stage == 0 ? OpModulate : OpDisable)
	, m_alphaOp(stage == 0 ? OpSelectArg1 : OpDisable)
	, m_lodBias(0.f)
{ }

D3D11TextureStage::~D3D11TextureStage() {
	resetTexture();
}

Texture * D3D11TextureStage::getTexture() const {
	return m_texture;
}

void D3D11TextureStage::setTexture(Texture * texture) {
	
	arx_assert(texture != nullptr);
	
	m_texture = reinterpret_cast<D3D11Texture *>(texture);
}

void D3D11TextureStage::resetTexture() {
	m_texture = nullptr;
}

void D3D11TextureStage::setColorOp(TextureOp op) {
	m_colorOp = op;
}

void D3D11TextureStage::setAlphaOp(TextureOp op) {
	m_alphaOp = op;
}

void D3D11TextureStage::setMipMapLODBias(float bias) {
	m_lodBias = bias;
}

D3D11Texture * D3D11TextureStage::getActiveTexture() const {
	
	// Like OpenGL, a stage without a valid texture passes through the previous color
	if(!isEnabled() || !m_texture || !m_texture->view()) {
		return nullptr;
	}
	
	return m_texture;
}

ID3D11SamplerState * D3D11TextureStage::getSampler(const D3D11Texture * texture) {
	
	arx_assert(texture);
	
	// TODO: Fix callers and change this into an assert/error/warning (same as in the OpenGL backend)
	WrapMode wrapMode = texture->isNPOT() ? WrapClamp : getWrapMode();
	bool mipmaps = texture->hasMipmaps();
	
	unsigned anisotropy = 1;
	if(mipmaps && getMinFilter() == FilterLinear) {
		anisotropy = unsigned(m_renderer->getMaxAnisotropy());
	}
	
	u32 biasBits;
	std::memcpy(&biasBits, &m_lodBias, sizeof(biasBits));
	
	u64 key = u64(wrapMode) | (u64(getMinFilter()) << 2) | (u64(getMagFilter()) << 3)
	          | (u64(mipmaps) << 4) | (u64(anisotropy) << 8) | (u64(biasBits) << 32);
	
	auto it = m_samplers.find(key);
	if(it != m_samplers.end()) {
		return it->second.Get();
	}
	
	static const D3D11_TEXTURE_ADDRESS_MODE arxToD3DWrapMode[] = {
		D3D11_TEXTURE_ADDRESS_WRAP,   // WrapRepeat
		D3D11_TEXTURE_ADDRESS_MIRROR, // WrapMirror
		D3D11_TEXTURE_ADDRESS_CLAMP   // WrapClamp
	};
	
	D3D11_SAMPLER_DESC desc = {};
	if(anisotropy > 1) {
		desc.Filter = D3D11_FILTER_ANISOTROPIC;
		desc.MaxAnisotropy = anisotropy;
	} else {
		// OpenGL uses GL_*_MIPMAP_LINEAR for mipmapped textures
		D3D11_FILTER_TYPE min = getMinFilter() == FilterLinear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
		D3D11_FILTER_TYPE mag = getMagFilter() == FilterLinear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
		D3D11_FILTER_TYPE mip = mipmaps ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
		desc.Filter = D3D11_ENCODE_BASIC_FILTER(min, mag, mip, 0);
		desc.MaxAnisotropy = 1;
	}
	desc.AddressU = desc.AddressV = desc.AddressW = arxToD3DWrapMode[wrapMode];
	desc.MipLODBias = glm::clamp(m_lodBias, -16.f, 15.99f);
	desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	desc.MinLOD = 0.f;
	desc.MaxLOD = mipmaps ? D3D11_FLOAT32_MAX : 0.f;
	
	d3d11::ComPtr<ID3D11SamplerState> sampler;
	if(FAILED(m_renderer->device()->CreateSamplerState(&desc, &sampler))) {
		LogError << "Failed to create Direct3D 11 sampler state";
		return nullptr;
	}
	
	ID3D11SamplerState * result = sampler.Get();
	m_samplers.emplace(key, std::move(sampler));
	return result;
}
