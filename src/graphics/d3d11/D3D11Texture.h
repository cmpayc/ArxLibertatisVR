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

#ifndef ARX_GRAPHICS_D3D11_D3D11TEXTURE_H
#define ARX_GRAPHICS_D3D11_D3D11TEXTURE_H

#include "graphics/d3d11/D3D11ComPtr.h"

#include <boost/intrusive/list_hook.hpp>

#include "graphics/texture/Texture.h"
#include "graphics/texture/TextureMaterial.h"
#include "platform/Platform.h"

class D3D11Renderer;

typedef boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>> D3D11TextureListHook;

class D3D11Texture final : public Texture, public D3D11TextureListHook {

public:
	
	explicit D3D11Texture(D3D11Renderer * renderer);
	~D3D11Texture() override;
	
	bool create() override;
	void upload() override;
	void destroy() override;
	
	ID3D11ShaderResourceView * view() const { return m_view.Get(); }
	ID3D11Texture2D * texture() const { return m_texture.Get(); }

	//! Slope of the texture brightness for normal mapping, nullptr if not generated
	ID3D11ShaderResourceView * normalView() const { return m_normalView.Get(); }
	
	//! Generate the normal map if the texture can be used for normal mapping and does not have one yet
	void updateNormalMap();
	
	//! Release the normal map
	void releaseNormalMap();
	
	//! Specular highlights and reflections of the surface, guessed from the material in the file name
	typedef TextureMaterial Specular;
	const Specular & specular() const { return m_specular; }
	
	//! NPOT textures are always clamped to match the OpenGL backend.
	bool isNPOT() const { return m_isNPOT; }
	
private:
	
	D3D11Renderer * m_renderer;
	
	d3d11::ComPtr<ID3D11Texture2D> m_texture;
	d3d11::ComPtr<ID3D11ShaderResourceView> m_view;
	d3d11::ComPtr<ID3D11Texture2D> m_normalTexture;
	d3d11::ComPtr<ID3D11ShaderResourceView> m_normalView;
	Vec2i m_textureSize;
	bool m_textureMipmaps;
	
	bool m_isNPOT;
	
	Specular m_specular;
	
};

#endif // ARX_GRAPHICS_D3D11_D3D11TEXTURE_H
