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

#include "graphics/d3d11/D3D11Texture.h"

#include <string_view>
#include <vector>

#include "graphics/Math.h"
#include "graphics/d3d11/D3D11Renderer.h"
#include "io/log/Logger.h"

// Highlights and reflections for the materials named in texture file names, the first matching tag wins
static const struct {
	std::string_view tag;
	D3D11Texture::Specular specular;
} surfaceMaterials[] = {
	{ "lava",    { 0.f, 1.f, 0.f, 0.f, 1.f } },
	{ "water",   { 1.f, 96.f, 0.f, 1.f } },
	{ "wet",     { 0.8f, 64.f, 0.f, 0.f } },
	{ "blood",   { 0.7f, 64.f, 0.3f, 0.f } },
	{ "mud",     { 0.5f, 48.f, 0.f, 0.f } },
	// Crystals and gems reflect (only level geometry) and look clear and deep, before "glass" for "[glass]_quartz"
	{ "quartz",  { 0.6f, 128.f, 0.f, 0.3f, 0.f, 0.f, 1.f } },
	{ "crystal", { 0.6f, 128.f, 0.f, 0.3f, 0.f, 0.f, 1.f } },
	{ "diamond", { 0.6f, 128.f, 0.f, 0.3f, 0.f, 0.f, 1.f } },
	{ "gems",    { 0.6f, 128.f, 0.f, 0.3f, 0.f, 0.f, 1.f } },
	// The crystal mirrors of the temple of illusion ("[glass]_mirror"), polished: sharper reflections
	{ "mirror",  { 0.6f, 128.f, 0.f, 0.6f, 0.f, 0.f, 1.f } },
	{ "glass",   { 1.f, 128.f, 0.f, 0.f } },
	{ "vitrail", { 1.f, 128.f, 0.f, 0.f } },
	{ "ice",     { 0.3f, 96.f, 0.f, 0.2f, 0.f, 1.f } },
	{ "marble",  { 0.6f, 64.f, 0.f, 0.f } },
	{ "rust",    { 0.3f, 16.f, 1.f, 0.f } },
	{ "metal",   { 0.9f, 40.f, 1.f, 0.f } },
	{ "iron",    { 0.8f, 32.f, 1.f, 0.f } },
	{ "weapon",  { 0.9f, 40.f, 1.f, 0.f } },
	{ "gold",    { 1.f, 48.f, 1.f, 0.f } },
	{ "mithril", { 1.f, 48.f, 1.f, 0.f } },
	{ "silver",  { 1.f, 48.f, 1.f, 0.f } },
	{ "plate",   { 0.9f, 40.f, 1.f, 0.f } },
	{ "chainmail", { 0.7f, 24.f, 1.f, 0.f } },
	{ "sword",   { 0.9f, 40.f, 1.f, 0.f } },
	{ "shield",  { 0.6f, 32.f, 1.f, 0.f } },
	{ "stone",   { 0.6f, 48.f, 0.f, 0.f } },
	{ "rock",    { 0.5f, 40.f, 0.f, 0.f } },
	{ "bone",    { 0.4f, 32.f, 0.f, 0.f } },
	{ "flesh",   { 0.2f, 16.f, 0.f, 0.f } },
	{ "wood",    { 0.15f, 10.f, 0.f, 0.f } },
	{ "woodl",   { 0.15f, 10.f, 0.f, 0.f } },
	{ "fabric",  { 0.f, 1.f, 0.f, 0.f } },
	{ "sand",    { 0.f, 1.f, 0.f, 0.f } },
	{ "soil",    { 0.f, 1.f, 0.f, 0.f } },
	{ "gravel",  { 0.f, 1.f, 0.f, 0.f } },
	{ "earth",   { 0.f, 1.f, 0.f, 0.f } },
	{ "dust",    { 0.f, 1.f, 0.f, 0.f } },
	{ "straw",   { 0.f, 1.f, 0.f, 0.f } },
	{ "plant",   { 0.f, 1.f, 0.f, 0.f } },
	{ "hair",    { 0.f, 1.f, 0.f, 0.f } },
	{ "fur",     { 0.f, 1.f, 0.f, 0.f } },
	{ "clothe",  { 0.f, 1.f, 0.f, 0.f } },
};

// Highlights and reflections of textures without a known material
static const D3D11Texture::Specular defaultSpecular = { 0.3f, 32.f, 0.f, 0.f };

static D3D11Texture::Specular guessSpecular(std::string_view name) {
	
	if(name.empty()) {
		return D3D11Texture::Specular();
	}
	
	// Only match whole words like "[metal]" or "_wet01", not "ice" in "notice"
	auto isLetter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
	for(const auto & material : surfaceMaterials) {
		for(size_t pos = name.find(material.tag); pos != std::string_view::npos;
		    pos = name.find(material.tag, pos + 1)) {
			size_t end = pos + material.tag.size();
			if((pos == 0 || !isLetter(name[pos - 1])) && (end == name.size() || !isLetter(name[end]))) {
				return material.specular;
			}
		}
	}
	
	return defaultSpecular;
}

D3D11Texture::D3D11Texture(D3D11Renderer * renderer)
	: m_renderer(renderer)
	, m_textureSize(0)
	, m_textureMipmaps(false)
	, m_isNPOT(false)
{ }

D3D11Texture::~D3D11Texture() {
	destroy();
}

bool D3D11Texture::create() {
	
	// Direct3D 11 supports non-power-of-two textures with mipmaps and wrapping
	m_storedSize = getSize();
	
	Vec2i nextPowerOfTwo(GetNextPowerOf2(unsigned(getSize().x)), GetNextPowerOf2(unsigned(getSize().y)));
	m_isNPOT = (getSize() != nextPowerOfTwo);
	
	m_specular = guessSpecular(getFileName().string());
	
	return true;
}

/*!
 * Expand an image to RGBA8 using the same component mapping as the OpenGL
 * fixed-function pipeline uses for the corresponding texture base format.
 */
static void convertToRGBA(const Image & image, bool intensity, std::vector<u8> & out) {
	
	size_t pixels = image.getWidth() * image.getHeight();
	out.resize(pixels * 4);
	
	const u8 * in = image.getData();
	u8 * o = out.data();
	
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
			std::fill(out.begin(), out.end(), u8(255));
		}
		
	}
	
}

void D3D11Texture::upload() {
	
	Vec2i size = getSize();
	if(size.x <= 0 || size.y <= 0 || !m_image.isValid()) {
		return;
	}
	
	bool mipmaps = hasMipmaps();
	
	if(!m_texture || m_textureSize != size || m_textureMipmaps != mipmaps) {
		
		m_renderer->unbindTextureView(m_view.Get());
		m_view.Reset();
		m_texture.Reset();
		
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = UINT(size.x);
		desc.Height = UINT(size.y);
		desc.MipLevels = mipmaps ? 0 : 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (mipmaps ? D3D11_BIND_RENDER_TARGET : 0);
		desc.MiscFlags = mipmaps ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
		
		if(FAILED(m_renderer->device()->CreateTexture2D(&desc, nullptr, &m_texture))) {
			LogError << "Failed to create " << size.x << "x" << size.y << " Direct3D 11 texture "
			         << getFileName();
			m_texture.Reset();
			return;
		}
		
		if(FAILED(m_renderer->device()->CreateShaderResourceView(m_texture.Get(), nullptr, &m_view))) {
			LogError << "Failed to create Direct3D 11 texture view for " << getFileName();
			m_texture.Reset();
			m_view.Reset();
			return;
		}
		
		m_textureSize = size;
		m_textureMipmaps = mipmaps;
	}
	
	std::vector<u8> data;
	convertToRGBA(m_image, isIntensity(), data);
	
	m_renderer->context()->UpdateSubresource(m_texture.Get(), 0, nullptr, data.data(), UINT(size.x) * 4, 0);
	
	if(mipmaps) {
		m_renderer->context()->GenerateMips(m_view.Get());
	}
	
	// The contents changed
	releaseNormalMap();
	updateNormalMap();
	
}

void D3D11Texture::updateNormalMap() {
	
	// Only textures for the 3D scene have mipmaps
	if(m_normalView || !m_view || !m_textureMipmaps || !m_renderer->reliefMapsEnabled()) {
		return;
	}
	
	m_renderer->generateNormalMap(m_view.Get(), m_textureSize, m_normalTexture, m_normalView);
	
}

void D3D11Texture::releaseNormalMap() {
	m_renderer->unbindTextureView(m_normalView.Get());
	m_normalView.Reset();
	m_normalTexture.Reset();
}

void D3D11Texture::destroy() {
	
	releaseNormalMap();
	m_renderer->unbindTextureView(m_view.Get());
	m_view.Reset();
	m_texture.Reset();
	m_textureSize = Vec2i(0);
	
	m_renderer->onTextureDestroyed(this);
	
}
