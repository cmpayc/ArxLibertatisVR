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

#include "graphics/d3d11/D3D11Upscaler.h"

#include <sstream>
#include <string>

#include <upscalers/include/ffx_upscale.h>
#include <api/include/dx12/ffx_api_dx12.h>

#include "graphics/d3d11/D3D12Bridge.h"
#include "io/log/Logger.h"
#include "platform/WindowsUtils.h"

namespace {

// AMD FidelityFX libraries are not distributed with Arx Libertatis, newer SDKs use the loader
const wchar_t * const FidelityFXLibraries[] = {
	L"amd_fidelityfx_loader_dx12.dll",
	L"amd_fidelityfx_dx12.dll",
};

std::string returnCodeString(ffxReturnCode_t code) {
	std::ostringstream oss;
	oss << code;
	return oss.str();
}

class FSRUpscaler final : public D3D11Upscaler {
	
	HMODULE m_library;
	PfnFfxCreateContext m_createContext;
	PfnFfxDestroyContext m_destroyContext;
	PfnFfxQuery m_query;
	PfnFfxDispatch m_dispatch;
	
	D3D12Bridge m_bridge;
	D3D12Bridge::Texture m_color;
	D3D12Bridge::Texture m_depth;
	D3D12Bridge::Texture m_motion;
	D3D12Bridge::Texture m_output;
	
	ffxContext m_context;
	Vec2i m_renderSize;
	Vec2i m_outputSize;
	bool m_contextFailed; //!< Don't retry creating the context until the sizes change
	
	//! Directory of the FidelityFX loader, which loads the upscaler library from the DLL search path
	std::wstring m_libraryDirectory;
	
	void destroyContext() {
		if(m_context) {
			// The context may still be used by the GPU
			m_bridge.waitIdle();
			m_destroyContext(&m_context, nullptr);
			m_context = nullptr;
		}
	}
	
	bool createContext(const Inputs & inputs) {
		
		destroyContext();
		
		ffxCreateBackendDX12Desc backend = {};
		backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
		backend.device = m_bridge.device();
		
		ffxCreateContextDescUpscale desc = {};
		desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		desc.header.pNext = &backend.header;
		// Arx Libertatis renders low dynamic range colors in gamma space
		desc.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
		desc.maxRenderSize.width = uint32_t(inputs.renderSize.x);
		desc.maxRenderSize.height = uint32_t(inputs.renderSize.y);
		desc.maxUpscaleSize.width = uint32_t(inputs.outputSize.x);
		desc.maxUpscaleSize.height = uint32_t(inputs.outputSize.y);
		
		m_renderSize = inputs.renderSize;
		m_outputSize = inputs.outputSize;
		
		// The loader looks for amd_fidelityfx_upscaler_dx12.dll in the DLL search path
		SetDllDirectoryW(m_libraryDirectory.c_str());
		ffxReturnCode_t result = m_createContext(&m_context, &desc.header, nullptr);
		SetDllDirectoryW(nullptr);
		
		if(result != FFX_API_RETURN_OK) {
			LogError << "Failed to create FSR context for " << inputs.renderSize.x << "x" << inputs.renderSize.y
			         << " -> " << inputs.outputSize.x << "x" << inputs.outputSize.y << ": " << returnCodeString(result)
			         << (result == FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE
			             ? " - amd_fidelityfx_upscaler_dx12.dll must be next to the FidelityFX loader" : "");
			m_context = nullptr;
			m_contextFailed = true;
			return false;
		}
		m_contextFailed = false;
		
		ffxQueryGetProviderVersion version = {};
		version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
		const char * versionName = "(unknown)";
		if(m_query(&m_context, &version.header) == FFX_API_RETURN_OK && version.versionName) {
			versionName = version.versionName;
		}
		
		LogInfo << "FSR " << versionName << ": " << inputs.renderSize.x << "x" << inputs.renderSize.y << " -> "
		        << inputs.outputSize.x << "x" << inputs.outputSize.y;
		
		return true;
	}
	
public:
	
	FSRUpscaler()
		: m_library(nullptr)
		, m_createContext(nullptr)
		, m_destroyContext(nullptr)
		, m_query(nullptr)
		, m_dispatch(nullptr)
		, m_context(nullptr)
		, m_renderSize(0)
		, m_outputSize(0)
		, m_contextFailed(false)
	{ }
	
	~FSRUpscaler() override {
		destroyContext();
		m_color = m_depth = m_motion = m_output = D3D12Bridge::Texture();
		if(m_library) {
			FreeLibrary(m_library);
		}
	}
	
	bool initialize(ID3D11Device * device, ID3D11DeviceContext * context, const std::string & searchPath) {
		
		for(const wchar_t * name : FidelityFXLibraries) {
			std::wstring path = d3d11::findUpscalerLibrary(name, searchPath);
			if(!path.empty()) {
				m_library = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
				if(m_library) {
					m_libraryDirectory = path.substr(0, path.find_last_of(L"\\/") + 1);
					break;
				}
			}
		}
		if(!m_library) {
			LogWarning << "FSR not available: amd_fidelityfx_loader_dx12.dll not found - copy it and"
			           << " amd_fidelityfx_upscaler_dx12.dll from the AMD FidelityFX SDK or another game next to arx.exe";
			return false;
		}
		
		m_createContext = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(m_library, "ffxCreateContext"));
		m_destroyContext = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(m_library, "ffxDestroyContext"));
		m_query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(m_library, "ffxQuery"));
		m_dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(m_library, "ffxDispatch"));
		if(!m_createContext || !m_destroyContext || !m_query || !m_dispatch) {
			LogWarning << "FSR not available: the FidelityFX library does not provide the expected functions";
			return false;
		}
		
		if(!m_bridge.initialize(device, context)) {
			LogWarning << "FSR not available: Direct3D 12 is required";
			return false;
		}
		
		LogInfo << "FSR available";
		
		return true;
	}
	
	bool evaluate(ID3D11DeviceContext * context, const Inputs & inputs) override {
		
		bool reset = inputs.reset;
		bool resized = (inputs.renderSize != m_renderSize || inputs.outputSize != m_outputSize);
		if(m_contextFailed && !resized) {
			return false;
		}
		if(!m_context || resized) {
			if(!createContext(inputs)) {
				return false;
			}
			reset = true;
		}
		
		D3D11_TEXTURE2D_DESC colorDesc;
		inputs.color->GetDesc(&colorDesc);
		D3D11_TEXTURE2D_DESC outputDesc;
		inputs.output->GetDesc(&outputDesc);
		
		if(!m_bridge.createTexture(m_color, inputs.renderSize, colorDesc.Format, false)
		   || !m_bridge.createTexture(m_depth, inputs.renderSize, DXGI_FORMAT_R32_FLOAT, false)
		   || !m_bridge.createTexture(m_motion, inputs.renderSize, DXGI_FORMAT_R16G16_FLOAT, false)
		   || !m_bridge.createTexture(m_output, inputs.outputSize, outputDesc.Format, true)) {
			return false;
		}
		
		context->CopyResource(m_color.texture.Get(), inputs.color);
		context->CopyResource(m_depth.texture.Get(), inputs.depth);
		context->CopyResource(m_motion.texture.Get(), inputs.motion);
		
		ID3D12GraphicsCommandList * list = m_bridge.begin();
		if(!list) {
			return false;
		}
		
		ffxDispatchDescUpscale desc = {};
		desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
		desc.commandList = list;
		// The shared textures are in the common state between frames
		desc.color = ffxApiGetResourceDX12(m_color.resource.Get(), FFX_API_RESOURCE_STATE_COMMON);
		desc.depth = ffxApiGetResourceDX12(m_depth.resource.Get(), FFX_API_RESOURCE_STATE_COMMON);
		desc.motionVectors = ffxApiGetResourceDX12(m_motion.resource.Get(), FFX_API_RESOURCE_STATE_COMMON);
		desc.output = ffxApiGetResourceDX12(m_output.resource.Get(), FFX_API_RESOURCE_STATE_COMMON);
		desc.jitterOffset.x = inputs.jitter.x;
		desc.jitterOffset.y = inputs.jitter.y;
		desc.motionVectorScale.x = 1.f;
		desc.motionVectorScale.y = 1.f;
		desc.renderSize.width = uint32_t(inputs.renderSize.x);
		desc.renderSize.height = uint32_t(inputs.renderSize.y);
		desc.upscaleSize.width = uint32_t(inputs.outputSize.x);
		desc.upscaleSize.height = uint32_t(inputs.outputSize.y);
		desc.enableSharpening = (inputs.sharpness > 0.f);
		desc.sharpness = inputs.sharpness;
		desc.frameTimeDelta = inputs.frameTime;
		desc.preExposure = 1.f;
		desc.reset = reset;
		desc.cameraNear = inputs.cameraNear;
		desc.cameraFar = inputs.cameraFar;
		desc.cameraFovAngleVertical = inputs.cameraFovVertical;
		desc.viewSpaceToMetersFactor = 0.01f; // One unit is about one centimeter
		desc.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
		
		ffxReturnCode_t result = m_dispatch(&m_context, &desc.header);
		
		if(!m_bridge.end() || result != FFX_API_RETURN_OK) {
			static bool warned = false;
			if(!warned) {
				LogError << "FSR dispatch failed: " << returnCodeString(result);
				warned = true;
			}
			return false;
		}
		
		context->CopyResource(inputs.output, m_output.texture.Get());
		
		return true;
	}
	
	const char * name() const override {
		return "FSR";
	}
	
};

} // anonymous namespace

namespace d3d11 {

std::unique_ptr<D3D11Upscaler> createFSRUpscaler(ID3D11Device * device, ID3D11DeviceContext * context,
                                                 const std::string & searchPath) {
	
	auto upscaler = std::make_unique<FSRUpscaler>();
	if(!upscaler->initialize(device, context, searchPath)) {
		return { };
	}
	
	return upscaler;
}

bool findFSRLibrary(const std::string & searchPath) {
	for(const wchar_t * name : FidelityFXLibraries) {
		if(!findUpscalerLibrary(name, searchPath).empty()) {
			return true;
		}
	}
	return false;
}

} // namespace d3d11
