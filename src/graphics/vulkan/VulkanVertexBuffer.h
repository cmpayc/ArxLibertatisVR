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

#ifndef ARX_GRAPHICS_VULKAN_VULKANVERTEXBUFFER_H
#define ARX_GRAPHICS_VULKAN_VULKANVERTEXBUFFER_H

#include <algorithm>
#include <vector>

#include "graphics/Vertex.h"
#include "graphics/VertexBuffer.h"
#include "graphics/vulkan/VulkanRenderer.h"

template <class Vertex>
struct VulkanVertexType;

template <>
struct VulkanVertexType<TexturedVertex> {
	static constexpr VulkanRenderer::VertexType value = VulkanRenderer::VertexTL;
};

template <>
struct VulkanVertexType<SMY_VERTEX> {
	static constexpr VulkanRenderer::VertexType value = VulkanRenderer::VertexWorld;
};

template <>
struct VulkanVertexType<SMY_VERTEX3> {
	static constexpr VulkanRenderer::VertexType value = VulkanRenderer::VertexWorld3;
};

/*!
 * Vertex buffer with a CPU-side shadow copy.
 *
 * The shadow copy lets lock() return memory that keeps the previous buffer contents like the other backends.
 * T1.1: the vertices are only kept on the CPU, the GPU buffers are added in T1.4.
 */
template <class Vertex>
class VulkanVertexBuffer final : public VertexBuffer<Vertex> {

	typedef VertexBuffer<Vertex> Base;

public:

	using Base::capacity;

	VulkanVertexBuffer(VulkanRenderer * renderer, size_t capacity, Renderer::BufferUsage usage)
		: Base(capacity)
		, m_renderer(renderer)
		, m_usage(usage)
		, m_shadow(capacity)
		, m_lockOffset(0)
		, m_lockCount(0)
		, m_lockFlags(0)
	{ }

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

		if(count == 0) {
			return;
		}

		m_renderer->drawBuffer(VulkanVertexType<Vertex>::value, primitive, m_shadow.data() + offset, count);
	}

	void drawIndexed(Renderer::Primitive primitive, size_t count, size_t offset,
	                 const unsigned short * indices, size_t nbindices) const override {

		ARX_UNUSED(count);

		arx_assert(offset < capacity());
		arx_assert(offset + count <= capacity());
		arx_assert(indices != nullptr);

		if(nbindices == 0) {
			return;
		}

		m_renderer->drawBufferIndexed(VulkanVertexType<Vertex>::value, primitive, m_shadow.data() + offset,
		                              indices, nbindices);
	}

private:

	void upload(size_t offset, size_t count, BufferFlags flags) {
		// TODO T1.4 copy static buffers into device-local memory
		ARX_UNUSED(offset), ARX_UNUSED(count), ARX_UNUSED(flags);
	}

	VulkanRenderer * m_renderer;
	Renderer::BufferUsage m_usage;

	std::vector<Vertex> m_shadow;

	size_t m_lockOffset;
	size_t m_lockCount;
	BufferFlags m_lockFlags;

};

#endif // ARX_GRAPHICS_VULKAN_VULKANVERTEXBUFFER_H
