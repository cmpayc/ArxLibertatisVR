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

#include "graphics/d3d11/D3D12Bridge.h"

#include <algorithm>
#include <sstream>
#include <string>

#include "graphics/d3d11/D3D11Upscaler.h"
#include "io/log/Logger.h"
#include "platform/WindowsUtils.h"

namespace {

std::string hresultString(HRESULT hr) {
	std::ostringstream oss;
	oss << "0x" << std::hex << u32(hr);
	return oss.str();
}

} // anonymous namespace

D3D12Bridge::D3D12Bridge()
	: m_allocatorFences()
	, m_frame(0)
	, m_recording(false)
	, m_fenceValue(0)
	, m_event(nullptr)
{ }

D3D12Bridge::~D3D12Bridge() {
	
	// Resources may still be in use by the GPU
	waitIdle();
	
	if(m_event) {
		CloseHandle(m_event);
	}
	
}

void D3D12Bridge::waitIdle() {
	
	if(!m_queue || !m_fence || !m_event) {
		return;
	}
	
	m_fenceValue++;
	if(SUCCEEDED(m_queue->Signal(m_fence.Get(), m_fenceValue))) {
		waitForGpu(m_fenceValue);
	}
	
}

bool D3D12Bridge::initialize(ID3D11Device * device, ID3D11DeviceContext * context) {
	
	// Shared fences need Direct3D 11.4 (Windows 10 1703)
	if(FAILED(device->QueryInterface(IID_PPV_ARGS(&m_device11)))
	   || FAILED(context->QueryInterface(IID_PPV_ARGS(&m_context11)))) {
		LogWarning << "Direct3D 12 bridge not available: Direct3D 11.4 required";
		return false;
	}
	
	// Use the same adapter as the Direct3D 11 device
	ComPtr<IDXGIDevice> dxgiDevice;
	ComPtr<IDXGIAdapter> adapter;
	if(FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter))) {
		LogWarning << "Direct3D 12 bridge not available: could not get the adapter";
		return false;
	}
	
	HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
	if(FAILED(hr)) {
		LogWarning << "Direct3D 12 bridge not available: device creation failed with " << hresultString(hr);
		return false;
	}
	
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if(FAILED(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue)))) {
		LogWarning << "Direct3D 12 bridge not available: could not create the command queue";
		return false;
	}
	
	for(ComPtr<ID3D12CommandAllocator> & allocator : m_allocators) {
		if(FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) {
			LogWarning << "Direct3D 12 bridge not available: could not create a command allocator";
			return false;
		}
	}
	
	if(FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocators[0].Get(), nullptr,
	                                      IID_PPV_ARGS(&m_list)))
	   || FAILED(m_list->Close())) {
		LogWarning << "Direct3D 12 bridge not available: could not create the command list";
		return false;
	}
	
	// Shared fence
	{
		if(FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fence)))) {
			LogWarning << "Direct3D 12 bridge not available: could not create the shared fence";
			return false;
		}
		HANDLE handle = nullptr;
		hr = m_device->CreateSharedHandle(m_fence.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
		if(SUCCEEDED(hr)) {
			hr = m_device11->OpenSharedFence(handle, IID_PPV_ARGS(&m_fence11));
			CloseHandle(handle);
		}
		if(FAILED(hr)) {
			LogWarning << "Direct3D 12 bridge not available: could not share the fence: " << hresultString(hr);
			return false;
		}
	}
	
	m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if(!m_event) {
		return false;
	}
	
	LogInfo << "Direct3D 12 bridge created";
	
	return true;
}

bool D3D12Bridge::createTexture(Texture & texture, const Vec2i & size, DXGI_FORMAT format, bool unorderedAccess) {
	
	if(texture.resource && texture.size == size && texture.format == format) {
		return true;
	}
	
	texture = Texture();
	
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	
	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = UINT64(size.x);
	desc.Height = UINT(size.y);
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	// Simultaneous access lets both devices use the texture without explicit state transitions
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	if(unorderedAccess) {
		desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	}
	
	HRESULT hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON,
	                                               nullptr, IID_PPV_ARGS(&texture.resource));
	if(FAILED(hr)) {
		LogError << "Failed to create " << size.x << "x" << size.y << " shared Direct3D 12 texture: "
		         << hresultString(hr);
		texture = Texture();
		return false;
	}
	
	HANDLE handle = nullptr;
	hr = m_device->CreateSharedHandle(texture.resource.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
	if(SUCCEEDED(hr)) {
		hr = m_device11->OpenSharedResource1(handle, IID_PPV_ARGS(&texture.texture));
		CloseHandle(handle);
	}
	if(FAILED(hr)) {
		LogError << "Failed to share " << size.x << "x" << size.y << " Direct3D 12 texture: " << hresultString(hr);
		texture = Texture();
		return false;
	}
	
	texture.size = size;
	texture.format = format;
	
	return true;
}

void D3D12Bridge::waitForGpu(UINT64 value) {
	if(m_fence->GetCompletedValue() < value && SUCCEEDED(m_fence->SetEventOnCompletion(value, m_event))) {
		WaitForSingleObject(m_event, INFINITE);
	}
}

ID3D12GraphicsCommandList * D3D12Bridge::begin() {
	
	arx_assert(!m_recording);
	
	// Direct3D 11 → Direct3D 12
	m_fenceValue++;
	if(FAILED(m_context11->Signal(m_fence11.Get(), m_fenceValue))) {
		return nullptr;
	}
	m_context11->Flush();
	if(FAILED(m_queue->Wait(m_fence.Get(), m_fenceValue))) {
		return nullptr;
	}
	
	// Reuse the allocator from FrameCount frames ago once the GPU is done with it
	size_t index = m_frame % FrameCount;
	waitForGpu(m_allocatorFences[index]);
	if(FAILED(m_allocators[index]->Reset()) || FAILED(m_list->Reset(m_allocators[index].Get(), nullptr))) {
		return nullptr;
	}
	
	m_recording = true;
	
	return m_list.Get();
}

bool D3D12Bridge::end() {
	
	arx_assert(m_recording);
	m_recording = false;
	
	if(FAILED(m_list->Close())) {
		return false;
	}
	
	ID3D12CommandList * lists[] = { m_list.Get() };
	m_queue->ExecuteCommandLists(UINT(std::size(lists)), lists);
	
	// Direct3D 12 → Direct3D 11
	m_fenceValue++;
	if(FAILED(m_queue->Signal(m_fence.Get(), m_fenceValue))) {
		return false;
	}
	m_allocatorFences[m_frame % FrameCount] = m_fenceValue;
	m_frame++;
	
	return SUCCEEDED(m_context11->Wait(m_fence11.Get(), m_fenceValue));
}

namespace {

/*!
 * Copies the scene through Direct3D 12 to verify the bridge (upscaler="bridge-test").
 * The image should look exactly like without an upscaler at 100% render scale.
 */
class BridgeTestUpscaler final : public D3D11Upscaler {
	
	D3D12Bridge m_bridge;
	D3D12Bridge::Texture m_color;
	D3D12Bridge::Texture m_output;
	
public:
	
	bool initialize(ID3D11Device * device, ID3D11DeviceContext * context) {
		return m_bridge.initialize(device, context);
	}
	
	bool evaluate(ID3D11DeviceContext * context, const Inputs & inputs) override {
		
		D3D11_TEXTURE2D_DESC colorDesc;
		inputs.color->GetDesc(&colorDesc);
		D3D11_TEXTURE2D_DESC outputDesc;
		inputs.output->GetDesc(&outputDesc);
		
		if(!m_bridge.createTexture(m_color, inputs.renderSize, colorDesc.Format, false)
		   || !m_bridge.createTexture(m_output, inputs.outputSize, outputDesc.Format, true)) {
			return false;
		}
		
		context->CopyResource(m_color.texture.Get(), inputs.color);
		
		ID3D12GraphicsCommandList * list = m_bridge.begin();
		if(!list) {
			return false;
		}
		
		// Copy the overlapping area - the whole image at 100% render scale
		D3D12_TEXTURE_COPY_LOCATION destination = {};
		destination.pResource = m_output.resource.Get();
		destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		D3D12_TEXTURE_COPY_LOCATION source = {};
		source.pResource = m_color.resource.Get();
		source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		D3D12_BOX box = { 0, 0, 0, UINT(std::min(inputs.renderSize.x, inputs.outputSize.x)),
		                  UINT(std::min(inputs.renderSize.y, inputs.outputSize.y)), 1 };
		list->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
		
		if(!m_bridge.end()) {
			return false;
		}
		
		context->CopyResource(inputs.output, m_output.texture.Get());
		
		return true;
	}
	
	const char * name() const override {
		return "bridge-test";
	}
	
};

} // anonymous namespace

namespace d3d11 {

std::wstring findUpscalerLibrary(const wchar_t * name, const std::string & searchPath) {
	
	// The executable directory is searched first, like NGX does
	WCHAR module[MAX_PATH];
	DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
	if(length > 0 && length < MAX_PATH) {
		std::wstring path(module, length);
		size_t separator = path.find_last_of(L"\\/");
		if(separator != std::wstring::npos) {
			path.resize(separator + 1);
			path += name;
			if(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
				return path;
			}
		}
	}
	
	if(!searchPath.empty()) {
		std::wstring path = platform::WideString(searchPath).c_str();
		if(path.back() != L'\\' && path.back() != L'/') {
			path += L'\\';
		}
		path += name;
		if(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
			return path;
		}
	}
	
	return std::wstring();
}

std::unique_ptr<D3D11Upscaler> createBridgeTestUpscaler(ID3D11Device * device, ID3D11DeviceContext * context) {
	
	auto upscaler = std::make_unique<BridgeTestUpscaler>();
	if(!upscaler->initialize(device, context)) {
		return { };
	}
	
	return upscaler;
}

} // namespace d3d11
