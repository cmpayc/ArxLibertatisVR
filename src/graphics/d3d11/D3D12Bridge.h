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

#ifndef ARX_GRAPHICS_D3D11_D3D12BRIDGE_H
#define ARX_GRAPHICS_D3D11_D3D12BRIDGE_H

#include "graphics/d3d11/D3D11ComPtr.h"
#include "math/Types.h"

/*!
 * Runs Direct3D 12 work (e.g. upscalers that only support Direct3D 12) on the frames of the
 * Direct3D 11 renderer.
 *
 * A private Direct3D 12 device is created on the same adapter. Textures are created as shared
 * resources that both devices can access, and a shared fence orders the work of both devices on the GPU:
 * D3D12 work recorded between \ref begin() and \ref end() runs after all D3D11 work submitted before
 * \ref begin(), and D3D11 work submitted after \ref end() runs after it.
 */
class D3D12Bridge {

public:
	
	template <class T>
	using ComPtr = d3d11::ComPtr<T>;
	
	//! A texture that can be used by both devices
	struct Texture {
		ComPtr<ID3D12Resource> resource; //!< Direct3D 12 side, may be used in any state (simultaneous access)
		ComPtr<ID3D11Texture2D> texture; //!< Direct3D 11 side
		Vec2i size = Vec2i(0);
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	};
	
	D3D12Bridge();
	~D3D12Bridge();
	
	D3D12Bridge(const D3D12Bridge &) = delete;
	D3D12Bridge & operator=(const D3D12Bridge &) = delete;
	
	//! Create the Direct3D 12 device, returns false if the bridge is not supported
	bool initialize(ID3D11Device * device, ID3D11DeviceContext * context);
	
	/*!
	 * Create or resize a shared texture.
	 * \param unorderedAccess allow unordered access, e.g. for upscaler outputs
	 */
	bool createTexture(Texture & texture, const Vec2i & size, DXGI_FORMAT format, bool unorderedAccess);
	
	//! Start recording Direct3D 12 commands that run after the Direct3D 11 work submitted so far
	ID3D12GraphicsCommandList * begin();
	
	//! Submit the recorded commands, Direct3D 11 work submitted afterwards waits for them
	bool end();
	
	ID3D12Device * device() const { return m_device.Get(); }
	ID3D12CommandQueue * queue() const { return m_queue.Get(); }
	
private:
	
	static constexpr size_t FrameCount = 3;
	
	void waitForGpu(UINT64 value);
	
	ComPtr<ID3D11Device5> m_device11;
	ComPtr<ID3D11DeviceContext4> m_context11;
	
	ComPtr<ID3D12Device> m_device;
	ComPtr<ID3D12CommandQueue> m_queue;
	ComPtr<ID3D12CommandAllocator> m_allocators[FrameCount];
	UINT64 m_allocatorFences[FrameCount];
	ComPtr<ID3D12GraphicsCommandList> m_list;
	size_t m_frame;
	bool m_recording;
	
	// Shared between both devices, all signals use increasing values from m_fenceValue
	ComPtr<ID3D12Fence> m_fence;
	ComPtr<ID3D11Fence> m_fence11;
	UINT64 m_fenceValue;
	HANDLE m_event;
	
};

#endif // ARX_GRAPHICS_D3D11_D3D12BRIDGE_H
