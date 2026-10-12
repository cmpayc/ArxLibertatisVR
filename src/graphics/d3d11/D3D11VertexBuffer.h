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

#ifndef ARX_GRAPHICS_D3D11_D3D11VERTEXBUFFER_H
#define ARX_GRAPHICS_D3D11_D3D11VERTEXBUFFER_H

#include <algorithm>
#include <cstring>
#include <vector>

#include "graphics/Vertex.h"
#include "graphics/VertexBuffer.h"
#include "graphics/d3d11/D3D11Renderer.h"
#include "io/log/Logger.h"

template <class Vertex>
struct D3D11VertexType;

template <>
struct D3D11VertexType<TexturedVertex> {
	static constexpr D3D11Renderer::VertexType value = D3D11Renderer::VertexTL;
};

template <>
struct D3D11VertexType<SMY_VERTEX> {
	static constexpr D3D11Renderer::VertexType value = D3D11Renderer::VertexWorld;
};

template <>
struct D3D11VertexType<SMY_VERTEX3> {
	static constexpr D3D11Renderer::VertexType value = D3D11Renderer::VertexWorld3;
};

/*!
 * Vertex buffer with a CPU-side shadow copy.
 *
 * The shadow copy lets lock() return memory that keeps the previous buffer contents
 * like the OpenGL backend does, independent of how the data is sent to the GPU.
 * Static buffers are updated with UpdateSubresource(), dynamic and stream buffers
 * are mapped with discard / no-overwrite semantics.
 */
template <class Vertex>
class D3D11VertexBuffer final : public VertexBuffer<Vertex> {
	
	typedef VertexBuffer<Vertex> Base;
	
public:
	
	using Base::capacity;
	
	D3D11VertexBuffer(D3D11Renderer * renderer, size_t capacity, Renderer::BufferUsage usage)
		: Base(capacity)
		, m_renderer(renderer)
		, m_dynamic(usage != Renderer::Static)
		, m_shadow(capacity)
		, m_used(0)
		, m_discarded(false)
		, m_lockOffset(0)
		, m_lockCount(0)
		, m_lockFlags(0)
	{
		
		D3D11_BUFFER_DESC desc = {};
		desc.ByteWidth = UINT(capacity * sizeof(Vertex));
		desc.Usage = m_dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		desc.CPUAccessFlags = m_dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
		
		if(FAILED(m_renderer->device()->CreateBuffer(&desc, nullptr, &m_buffer))) {
			LogError << "Failed to create Direct3D 11 vertex buffer with " << capacity << " vertices";
		}
		
	}
	
	void setData(const Vertex * vertices, size_t count, size_t offset, BufferFlags flags) override {
		
		arx_assert(offset < capacity());
		arx_assert(offset + count <= capacity());
		
		if(count == 0) {
			return;
		}
		
		std::copy(vertices, vertices + count, m_shadow.data() + offset);
		
		upload(offset, count, flags);
	}
	
	Vertex * lock(BufferFlags flags, size_t offset, size_t count) override {
		
		arx_assert(offset < capacity());
		
		m_lockOffset = offset;
		m_lockCount = std::min(count, capacity() - offset);
		m_lockFlags = flags;
		
		return m_shadow.data() + offset;
	}
	
	void unlock() override {
		
		if(m_lockCount != 0) {
			upload(m_lockOffset, m_lockCount, m_lockFlags);
		}
		
		m_lockCount = 0;
	}
	
	void draw(Renderer::Primitive primitive, size_t count, size_t offset) const override {
		
		arx_assert(offset < capacity());
		arx_assert(offset + count <= capacity());
		
		if(!m_buffer || count == 0) {
			return;
		}
		
		m_renderer->drawBuffer(D3D11VertexType<Vertex>::value, primitive, m_buffer.Get(),
		                       UINT(sizeof(Vertex)), count, offset);
	}
	
	void drawIndexed(Renderer::Primitive primitive, size_t count, size_t offset,
	                 const unsigned short * indices, size_t nbindices) const override {
		
		ARX_UNUSED(count);
		
		arx_assert(offset < capacity());
		arx_assert(offset + count <= capacity());
		arx_assert(indices != nullptr);
		
		if(!m_buffer || nbindices == 0) {
			return;
		}
		
		m_renderer->drawBufferIndexed(D3D11VertexType<Vertex>::value, primitive, m_buffer.Get(),
		                              UINT(sizeof(Vertex)), offset, indices, nbindices);
	}
	
private:
	
	void upload(size_t offset, size_t count, BufferFlags flags) {
		
		if(!m_buffer) {
			return;
		}
		
		ID3D11DeviceContext * context = m_renderer->context();
		
		m_used = std::max(m_used, offset + count);
		
		if(!m_dynamic) {
			D3D11_BOX box = { UINT(offset * sizeof(Vertex)), 0, 0, UINT((offset + count) * sizeof(Vertex)), 1, 1 };
			context->UpdateSubresource(m_buffer.Get(), 0, &box, m_shadow.data() + offset, 0, 0);
			return;
		}
		
		D3D11_MAP mode;
		size_t start = offset;
		size_t end = offset + count;
		if((flags & DiscardBuffer) || !m_discarded) {
			// The first map of a dynamic buffer must discard
			mode = D3D11_MAP_WRITE_DISCARD;
			if(!(flags & DiscardBuffer)) {
				start = 0;
				end = m_used;
			}
		} else if(flags & NoOverwrite) {
			mode = D3D11_MAP_WRITE_NO_OVERWRITE;
		} else {
			// Other buffer contents must be preserved - re-upload them from the shadow copy
			mode = D3D11_MAP_WRITE_DISCARD;
			start = 0;
			end = m_used;
		}
		
		D3D11_MAPPED_SUBRESOURCE mapped;
		if(FAILED(context->Map(m_buffer.Get(), 0, mode, 0, &mapped))) {
			LogError << "Failed to map Direct3D 11 vertex buffer";
			return;
		}
		
		Vertex * dst = reinterpret_cast<Vertex *>(mapped.pData);
		std::memcpy(dst + start, m_shadow.data() + start, (end - start) * sizeof(Vertex));
		
		context->Unmap(m_buffer.Get(), 0);
		
		m_discarded = true;
	}
	
	D3D11Renderer * m_renderer;
	bool m_dynamic;
	D3D11Renderer::ComPtr<ID3D11Buffer> m_buffer;
	
	std::vector<Vertex> m_shadow;
	size_t m_used;
	bool m_discarded;
	
	size_t m_lockOffset;
	size_t m_lockCount;
	BufferFlags m_lockFlags;
	
};

#endif // ARX_GRAPHICS_D3D11_D3D11VERTEXBUFFER_H
