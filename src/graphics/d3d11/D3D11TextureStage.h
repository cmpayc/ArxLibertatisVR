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

#ifndef ARX_GRAPHICS_D3D11_D3D11TEXTURESTAGE_H
#define ARX_GRAPHICS_D3D11_D3D11TEXTURESTAGE_H

#include <unordered_map>

#include "graphics/d3d11/D3D11ComPtr.h"

#include "graphics/texture/TextureStage.h"
#include "platform/Platform.h"

class D3D11Renderer;
class D3D11Texture;

class D3D11TextureStage final : public TextureStage {

public:
	
	D3D11TextureStage(D3D11Renderer * renderer, unsigned textureStage);
	~D3D11TextureStage() override;
	
	Texture * getTexture() const override;
	void setTexture(Texture * texture) override;
	void resetTexture() override;
	
	void setColorOp(TextureOp textureOp) override;
	void setAlphaOp(TextureOp textureOp) override;
	
	void setMipMapLODBias(float bias) override;
	
	TextureOp getColorOp() const { return m_colorOp; }
	TextureOp getAlphaOp() const { return m_alphaOp; }
	
	bool isEnabled() const { return m_colorOp != OpDisable || m_alphaOp != OpDisable; }
	
	//! \return the bound texture if it can be sampled, nullptr otherwise
	D3D11Texture * getActiveTexture() const;
	
	//! \return the sampler state matching the stage and texture settings
	ID3D11SamplerState * getSampler(const D3D11Texture * texture);
	
	//! Drop cached samplers (e.g. after the anisotropy setting changed)
	void clearSamplers() { m_samplers.clear(); }
	
private:
	
	D3D11Renderer * m_renderer;
	
	D3D11Texture * m_texture;
	
	TextureOp m_colorOp;
	TextureOp m_alphaOp;
	
	float m_lodBias;
	
	std::unordered_map<u64, d3d11::ComPtr<ID3D11SamplerState>> m_samplers;
	
	friend class D3D11Renderer;
	
};

#endif // ARX_GRAPHICS_D3D11_D3D11TEXTURESTAGE_H
