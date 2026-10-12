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

#include "graphics/d3d11/D3D11Renderer.h"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

#include "Configure.h"

#include "core/Application.h"
#include "core/Config.h"
#include "gui/Credits.h"
#include "graphics/Vertex.h"
#include "graphics/d3d11/D3D11Shaders.h"
#include "graphics/d3d11/D3D11Texture.h"
#include "graphics/d3d11/D3D11TextureStage.h"
#include "graphics/d3d11/D3D11Upscaler.h"
#include "graphics/d3d11/D3D11VertexBuffer.h"
#include "graphics/image/Image.h"
#include "io/log/Logger.h"
#include "platform/CrashHandler.h"
#include "platform/WindowsUtils.h"
#include "window/RenderWindow.h"

using d3d11::ComPtr;

namespace {

struct VSConstants {
	float transform[16];
	float jitter[4];
};

struct MotionConstants {
	float reprojection[16];
	float targetSize[4];
};

struct PSConstants {
	s32 stages[3][4];
	float fogColor[4];
	float fogParams[4];
};

struct ClearConstants {
	float color[4];
	float depth[4];
};

static_assert(sizeof(VSConstants) % 16 == 0, "constant buffer size must be a multiple of 16");
static_assert(sizeof(PSConstants) % 16 == 0, "constant buffer size must be a multiple of 16");
static_assert(sizeof(ClearConstants) % 16 == 0, "constant buffer size must be a multiple of 16");

const size_t VertexRingSize = 4 * 1024 * 1024;
const size_t IndexRingSize = 1024 * 1024;

const UINT TextureStageCount = 3;

const DXGI_FORMAT ColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
const DXGI_FORMAT DepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

const D3D11_BLEND arxToD3DBlendFactor[] = {
	D3D11_BLEND_ZERO,          // BlendZero,
	D3D11_BLEND_ONE,           // BlendOne,
	D3D11_BLEND_SRC_COLOR,     // BlendSrcColor,
	D3D11_BLEND_SRC_ALPHA,     // BlendSrcAlpha,
	D3D11_BLEND_INV_SRC_COLOR, // BlendInvSrcColor,
	D3D11_BLEND_INV_SRC_ALPHA, // BlendInvSrcAlpha,
	D3D11_BLEND_SRC_ALPHA_SAT, // BlendSrcAlphaSaturate,
	D3D11_BLEND_DEST_COLOR,    // BlendDstColor,
	D3D11_BLEND_DEST_ALPHA,    // BlendDstAlpha,
	D3D11_BLEND_INV_DEST_COLOR, // BlendInvDstColor,
	D3D11_BLEND_INV_DEST_ALPHA // BlendInvDstAlpha
};

// Color factors are not allowed for the alpha channel
const D3D11_BLEND arxToD3DAlphaBlendFactor[] = {
	D3D11_BLEND_ZERO,          // BlendZero,
	D3D11_BLEND_ONE,           // BlendOne,
	D3D11_BLEND_SRC_ALPHA,     // BlendSrcColor,
	D3D11_BLEND_SRC_ALPHA,     // BlendSrcAlpha,
	D3D11_BLEND_INV_SRC_ALPHA, // BlendInvSrcColor,
	D3D11_BLEND_INV_SRC_ALPHA, // BlendInvSrcAlpha,
	D3D11_BLEND_SRC_ALPHA_SAT, // BlendSrcAlphaSaturate,
	D3D11_BLEND_DEST_ALPHA,    // BlendDstColor,
	D3D11_BLEND_DEST_ALPHA,    // BlendDstAlpha,
	D3D11_BLEND_INV_DEST_ALPHA, // BlendInvDstColor,
	D3D11_BLEND_INV_DEST_ALPHA // BlendInvDstAlpha
};

const D3D11_PRIMITIVE_TOPOLOGY arxToD3DPrimitive[] = {
	D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,  // TriangleList,
	D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, // TriangleStrip,
	D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,  // TriangleFan (converted to a list)
	D3D11_PRIMITIVE_TOPOLOGY_LINELIST,      // LineList,
	D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP      // LineStrip
};

std::string hresultString(HRESULT hr) {
	std::ostringstream oss;
	oss << "0x" << std::hex << u32(hr);
	return oss.str();
}

} // anonymous namespace

D3D11Renderer::D3D11Renderer()
	: m_window(nullptr)
	, m_featureLevel(D3D_FEATURE_LEVEL_10_0)
	, m_flipModel(false)
	, m_allowTearing(false)
	, m_vsync(1)
	, m_size(0)
	, m_msaaSamples(1)
	, m_sceneScale(1.f)
	, m_inScene(false)
	, m_sceneSize(0)
	, m_sceneSamples(1)
	, m_sceneLodBias(0.f)
	, m_sceneProjection(1.f)
	, m_previousProjection(1.f)
	, m_previousView(1.f)
	, m_hasPreviousScene(false)
	, m_sceneRenderedLastFrame(false)
	, m_sceneRenderedThisFrame(false)
	, m_resetHistory(true)
	, m_jitterEnabled(false)
	, m_jitterIndex(0)
	, m_jitter(0.f)
	, m_sceneDebugView(0)
	, m_upscalerCreated(false)
	, m_dlssFailed(false)
	, m_lastUpscaleTime(0)
	, m_maximumAnisotropy(1.f)
	, m_maximumSupportedAnisotropy(1.f)
	, m_wireframe(false)
	, m_hasMSAA(false)
	, m_projection(1.f)
	, m_view(1.f)
	, m_fogColor(Color4f::black)
	, m_fogStart(0.f)
	, m_fogEnd(1.f)
	, m_stateValid(false)
	, m_currentVertexType(VertexTL)
	, m_currentVertexBuffer(nullptr)
	, m_currentVertexOffset(0)
	, m_currentTopology(D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED)
	, m_currentRasterizer(nullptr)
	, m_currentBlend(nullptr)
	, m_currentDepth(nullptr)
	, m_currentPixelShader(nullptr)
	, m_currentViews()
	, m_currentSamplers()
	, m_vsConstantsData()
	, m_vsConstantsValid(false)
	, m_psConstantsData()
	, m_psConstantsValid(false)
{ }

D3D11Renderer::~D3D11Renderer() {
	
	if(isInitialized()) {
		shutdown();
	}
	
	if(m_context) {
		m_context->ClearState();
		m_context->Flush();
	}
	
	// TODO textures must be destructed before D3D11Renderer or not at all
	
}

bool D3D11Renderer::create(HWND window, int maxMSAALevel) {
	
	m_window = window;
	
	// Debug view for the scene: 1 = depth, 2 = motion vectors, 3 = per-object motion
	{
		char value[16];
		DWORD length = GetEnvironmentVariableA("ARX_SCENE_DEBUG", value, sizeof(value));
		if(length > 0 && length < sizeof(value)) {
			m_sceneDebugView = std::atoi(value);
		}
	}
	
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	
	D3D_FEATURE_LEVEL levels[] = {
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
	};
	
	HRESULT hr = E_FAIL;
	
	#ifdef ARX_DEBUG
	hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags | D3D11_CREATE_DEVICE_DEBUG,
	                       levels, UINT(std::size(levels)), D3D11_SDK_VERSION, &m_device, &m_featureLevel,
	                       &m_context);
	if(FAILED(hr)) {
		LogWarning << "Direct3D 11 debug layer not available";
	}
	#endif
	
	if(FAILED(hr)) {
		hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
		                       UINT(std::size(levels)), D3D11_SDK_VERSION, &m_device, &m_featureLevel,
		                       &m_context);
	}
	
	if(hr == E_INVALIDARG) {
		// Direct3D 11.0 runtimes do not know about D3D_FEATURE_LEVEL_11_1
		hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels + 1,
		                       UINT(std::size(levels) - 1), D3D11_SDK_VERSION, &m_device, &m_featureLevel,
		                       &m_context);
	}
	
	if(FAILED(hr)) {
		LogError << "Failed to create Direct3D 11 device: " << hresultString(hr);
		return false;
	}
	
	ComPtr<IDXGIDevice> dxgiDevice;
	ComPtr<IDXGIAdapter> adapter;
	ComPtr<IDXGIFactory2> factory;
	if(FAILED(m_device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter))
	   || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
		LogError << "Failed to get the DXGI 1.2 factory";
		return false;
	}
	
	ComPtr<IDXGIFactory5> factory5;
	if(SUCCEEDED(factory.As(&factory5))) {
		BOOL allowTearing = FALSE;
		if(SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing,
		                                           sizeof(allowTearing)))) {
			m_allowTearing = (allowTearing != FALSE);
		}
	}
	
	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.Format = ColorFormat;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = 2;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.Flags = m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
	
	m_flipModel = true;
	hr = factory->CreateSwapChainForHwnd(m_device.Get(), m_window, &desc, nullptr, nullptr, &m_swapChain);
	if(FAILED(hr)) {
		// Windows 8
		m_allowTearing = false;
		desc.Flags = 0;
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		hr = factory->CreateSwapChainForHwnd(m_device.Get(), m_window, &desc, nullptr, nullptr, &m_swapChain);
	}
	if(FAILED(hr)) {
		// Windows 7
		m_flipModel = false;
		desc.BufferCount = 1;
		desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		hr = factory->CreateSwapChainForHwnd(m_device.Get(), m_window, &desc, nullptr, nullptr, &m_swapChain);
	}
	if(FAILED(hr)) {
		LogError << "Failed to create Direct3D 11 swap chain: " << hresultString(hr);
		return false;
	}
	
	// Fullscreen is handled by SDL
	factory->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
	
	m_msaaSamples = 1;
	for(int samples = maxMSAALevel; samples > 1; samples--) {
		UINT colorQuality = 0, depthQuality = 0;
		if(SUCCEEDED(m_device->CheckMultisampleQualityLevels(ColorFormat, UINT(samples), &colorQuality))
		   && SUCCEEDED(m_device->CheckMultisampleQualityLevels(DepthFormat, UINT(samples), &depthQuality))
		   && colorQuality > 0 && depthQuality > 0) {
			m_msaaSamples = UINT(samples);
			break;
		}
	}
	
	if(!compileShaders()) {
		return false;
	}
	
	{
		D3D11_BUFFER_DESC bufferDesc = {};
		bufferDesc.Usage = D3D11_USAGE_DEFAULT;
		bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bufferDesc.ByteWidth = sizeof(VSConstants);
		if(FAILED(m_device->CreateBuffer(&bufferDesc, nullptr, &m_vsConstants))) {
			return false;
		}
		bufferDesc.ByteWidth = sizeof(PSConstants);
		if(FAILED(m_device->CreateBuffer(&bufferDesc, nullptr, &m_psConstants))) {
			return false;
		}
		bufferDesc.ByteWidth = sizeof(ClearConstants);
		if(FAILED(m_device->CreateBuffer(&bufferDesc, nullptr, &m_clearConstants))) {
			return false;
		}
		bufferDesc.ByteWidth = sizeof(MotionConstants);
		if(FAILED(m_device->CreateBuffer(&bufferDesc, nullptr, &m_motionConstants))) {
			return false;
		}
	}
	
	{
		D3D11_BUFFER_DESC bufferDesc = {};
		bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
		bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		bufferDesc.ByteWidth = UINT(VertexRingSize);
		if(FAILED(m_device->CreateBuffer(&bufferDesc, nullptr, &m_vertexRing.buffer))) {
			return false;
		}
		m_vertexRing.size = VertexRingSize;
		bufferDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
		bufferDesc.ByteWidth = UINT(IndexRingSize);
		if(FAILED(m_device->CreateBuffer(&bufferDesc, nullptr, &m_indexRing.buffer))) {
			return false;
		}
		m_indexRing.size = IndexRingSize;
	}
	
	return true;
}

static ComPtr<ID3DBlob> compileShader(const char * entry, const char * target,
                                      const D3D_SHADER_MACRO * defines = nullptr) {
	
	ComPtr<ID3DBlob> code;
	ComPtr<ID3DBlob> errors;
	
	HRESULT hr = D3DCompile(d3d11ShaderSource, sizeof(d3d11ShaderSource) - 1, "arx-d3d11", defines, nullptr,
	                        entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
	if(FAILED(hr)) {
		LogError << "Failed to compile Direct3D 11 shader " << entry << ": "
		         << (errors ? static_cast<const char *>(errors->GetBufferPointer()) : hresultString(hr).c_str());
		return { };
	}
	
	return code;
}

bool D3D11Renderer::compileShaders() {
	
	const D3D11_INPUT_ELEMENT_DESC layoutTL[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, UINT(offsetof(TexturedVertex, p)),
		  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, UINT(offsetof(TexturedVertex, color)),
		  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, UINT(offsetof(TexturedVertex, uv)),
		  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "PREVPOSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, UINT(offsetof(TexturedVertex, prevP)),
		  D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	static_assert(offsetof(TexturedVertex, w) == offsetof(TexturedVertex, p) + 3 * sizeof(float),
	              "TexturedVertex position must be followed by w");
	
	const D3D11_INPUT_ELEMENT_DESC layoutWorld[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, UINT(offsetof(SMY_VERTEX, p)),
		  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, UINT(offsetof(SMY_VERTEX, color)),
		  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, UINT(offsetof(SMY_VERTEX, uv)),
		  D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	
	const D3D11_INPUT_ELEMENT_DESC layoutWorld3[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, UINT(offsetof(SMY_VERTEX3, p)),
		  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, UINT(offsetof(SMY_VERTEX3, color)),
		  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, UINT(offsetof(SMY_VERTEX3, uv)),
		  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, UINT(offsetof(SMY_VERTEX3, uv) + sizeof(Vec2f)),
		  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, UINT(offsetof(SMY_VERTEX3, uv) + 2 * sizeof(Vec2f)),
		  D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	
	struct VertexShaderInfo {
		const char * entry;
		const D3D11_INPUT_ELEMENT_DESC * layout;
		UINT layoutSize;
	};
	const VertexShaderInfo vertexShaders[VertexTypeCount] = {
		{ "vsTL", layoutTL, UINT(std::size(layoutTL)) },
		{ "vsWorld", layoutWorld, UINT(std::size(layoutWorld)) },
		{ "vsWorld3", layoutWorld3, UINT(std::size(layoutWorld3)) },
	};
	
	for(size_t i = 0; i < size_t(VertexTypeCount); i++) {
		ComPtr<ID3DBlob> code = compileShader(vertexShaders[i].entry, "vs_4_0");
		if(!code) {
			return false;
		}
		if(FAILED(m_device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
		                                       &m_vertexShaders[i]))
		   || FAILED(m_device->CreateInputLayout(vertexShaders[i].layout, vertexShaders[i].layoutSize,
		                                         code->GetBufferPointer(), code->GetBufferSize(),
		                                         &m_inputLayouts[i]))) {
			LogError << "Failed to create Direct3D 11 vertex shader " << vertexShaders[i].entry;
			return false;
		}
	}
	
	{
		ComPtr<ID3DBlob> code = compileShader("psMain", "ps_4_0");
		if(!code || FAILED(m_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
		                                               nullptr, &m_pixelShader))) {
			return false;
		}
	}
	
	if(m_featureLevel >= D3D_FEATURE_LEVEL_10_1) {
		const D3D_SHADER_MACRO defines[] = { { "SAMPLE_SHADING", "1" }, { nullptr, nullptr } };
		ComPtr<ID3DBlob> code = compileShader("psMain", "ps_4_1", defines);
		if(!code || FAILED(m_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
		                                               nullptr, &m_pixelShaderSampleShading))) {
			LogWarning << "Per-sample shading not available";
			m_pixelShaderSampleShading.Reset();
		}
	}
	
	{
		ComPtr<ID3DBlob> code = compileShader("vsClear", "vs_4_0");
		if(!code || FAILED(m_device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(),
		                                                nullptr, &m_clearVertexShader))) {
			return false;
		}
		code = compileShader("psClear", "ps_4_0");
		if(!code || FAILED(m_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
		                                               nullptr, &m_clearPixelShader))) {
			return false;
		}
	}
	
	{
		ComPtr<ID3DBlob> code = compileShader("vsBlit", "vs_4_0");
		if(!code || FAILED(m_device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(),
		                                                nullptr, &m_blitVertexShader))) {
			return false;
		}
		code = compileShader("psBlit", "ps_4_0");
		if(!code || FAILED(m_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
		                                               nullptr, &m_blitPixelShader))) {
			return false;
		}
		D3D11_SAMPLER_DESC desc = {};
		desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		desc.MaxAnisotropy = 1;
		desc.MaxLOD = D3D11_FLOAT32_MAX;
		if(FAILED(m_device->CreateSamplerState(&desc, &m_blitSampler))) {
			return false;
		}
	}
	
	{
		struct PixelShaderInfo {
			const char * entry;
			const D3D_SHADER_MACRO * defines;
			ComPtr<ID3D11PixelShader> * shader;
		};
		const D3D_SHADER_MACRO msaaDefines[] = { { "MSAA_DEPTH", "1" }, { nullptr, nullptr } };
		const PixelShaderInfo shaders[] = {
			{ "psMotion", nullptr, std::addressof(m_motionPixelShader) },
			{ "psMotion", msaaDefines, std::addressof(m_motionPixelShaderMS) },
			{ "psDebugDepth", nullptr, std::addressof(m_debugDepthPixelShader) },
			{ "psDebugMotion", nullptr, std::addressof(m_debugMotionPixelShader) },
			{ "psDebugObjectMotion", nullptr, std::addressof(m_debugObjectMotionPixelShader) },
		};
		for(const PixelShaderInfo & info : shaders) {
			if(m_featureLevel < D3D_FEATURE_LEVEL_10_1) {
				break;
			}
			ComPtr<ID3DBlob> code = compileShader(info.entry, "ps_4_1", info.defines);
			if(!code || FAILED(m_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
			                                               nullptr, &*info.shader))) {
				LogWarning << "Failed to create Direct3D 11 shader " << info.entry;
			}
		}
	}
	
	return true;
}

void D3D11Renderer::initialize() {
	
	arx_assert(m_device);
	
	std::string adapterName = "(unknown)";
	u64 totalVRAM = 0;
	{
		ComPtr<IDXGIDevice> dxgiDevice;
		ComPtr<IDXGIAdapter> adapter;
		DXGI_ADAPTER_DESC desc;
		if(SUCCEEDED(m_device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter))
		   && SUCCEEDED(adapter->GetDesc(&desc))) {
			adapterName = platform::WideString::toUTF8(desc.Description);
			totalVRAM = u64(desc.DedicatedVideoMemory);
		}
	}
	
	const char * level = "10.0";
	switch(m_featureLevel) {
		case D3D_FEATURE_LEVEL_11_1: level = "11.1"; break;
		case D3D_FEATURE_LEVEL_11_0: level = "11.0"; break;
		case D3D_FEATURE_LEVEL_10_1: level = "10.1"; break;
		default: break;
	}
	
	LogInfo << "Using Direct3D 11 with feature level " << level;
	CrashHandler::setVariable("Direct3D feature level", level);
	
	LogInfo << " ├─ Device: " << adapterName;
	CrashHandler::setVariable("Direct3D device", adapterName);
	
	LogInfo << " ├─ Swap chain: " << (m_flipModel ? "flip model" : "blit model")
	        << (m_allowTearing ? ", tearing supported" : "");
	
	if(totalVRAM != 0) {
		LogInfo << " └─ VRAM: " << (totalVRAM / 1024 / 1024) << " MiB";
		CrashHandler::setVariable("VRAM size", totalVRAM);
	} else {
		LogInfo << " └─ VRAM: (unknown)";
	}
	
	credits::setLibraryCredits("graphics", "Direct3D 11");
	
	m_maximumSupportedAnisotropy = float(D3D11_REQ_MAXANISOTROPY);
	setMaxAnisotropy(float(config.video.maxAnisotropicFiltering));
	
}

void D3D11Renderer::beforeResize(bool wasOrIsFullscreen) {
	// The swap chain is resized in afterResize(), no need to shut down
	ARX_UNUSED(wasOrIsFullscreen);
}

void D3D11Renderer::afterResize() {
	
	if(!isInitialized()) {
		reinit();
		return;
	}
	
	Vec2i size = mainApp->getWindow()->getSize();
	if(size == m_size || size.x <= 0 || size.y <= 0) {
		return;
	}
	
	releaseSizeDependentResources();
	
	createSizeDependentResources();
	
}

bool D3D11Renderer::createSizeDependentResources() {
	
	Vec2i size = mainApp->getWindow()->getSize();
	m_size = Vec2i(std::max<s32>(size.x, 1), std::max<s32>(size.y, 1));
	
	// The swap chain was created for the hidden window and is not resized when entering fullscreen
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc;
	if(SUCCEEDED(m_swapChain->GetDesc1(&swapChainDesc))
	   && (swapChainDesc.Width != UINT(m_size.x) || swapChainDesc.Height != UINT(m_size.y))) {
		HRESULT hr = m_swapChain->ResizeBuffers(0, UINT(m_size.x), UINT(m_size.y), DXGI_FORMAT_UNKNOWN,
		                                        swapChainDesc.Flags);
		if(FAILED(hr)) {
			LogError << "Failed to resize Direct3D 11 swap chain to " << m_size.x << "x" << m_size.y << ": "
			         << hresultString(hr);
		}
	}
	
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = UINT(m_size.x);
	desc.Height = UINT(m_size.y);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.SampleDesc.Count = m_msaaSamples;
	desc.Usage = D3D11_USAGE_DEFAULT;
	
	desc.Format = ColorFormat;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | (m_msaaSamples > 1 ? 0 : D3D11_BIND_SHADER_RESOURCE);
	if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_colorTarget))
	   || FAILED(m_device->CreateRenderTargetView(m_colorTarget.Get(), nullptr, &m_colorView))) {
		LogError << "Failed to create " << m_size.x << "x" << m_size.y << " Direct3D 11 color target";
		return false;
	}
	
	desc.Format = DepthFormat;
	desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_depthTarget))
	   || FAILED(m_device->CreateDepthStencilView(m_depthTarget.Get(), nullptr, &m_depthView))) {
		LogError << "Failed to create " << m_size.x << "x" << m_size.y << " Direct3D 11 depth target";
		return false;
	}
	
	invalidateCachedState();
	
	return true;
}

void D3D11Renderer::releaseSizeDependentResources() {
	
	if(m_context) {
		m_context->OMSetRenderTargets(0, nullptr, nullptr);
	}
	
	m_colorView.Reset();
	m_colorTarget.Reset();
	m_depthView.Reset();
	m_depthTarget.Reset();
	
	unbindTextureView(m_upscaledView.Get());
	m_upscaledView.Reset();
	m_upscaledTarget.Reset();
	
	releaseSceneTargets();
	
	invalidateCachedState();
	
}

bool D3D11Renderer::createSceneTargets(const Vec2i & size) {
	
	releaseSceneTargets();
	
	m_sceneSamples = m_upscaler ? 1 : m_msaaSamples;
	
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = UINT(size.x);
	desc.Height = UINT(size.y);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.SampleDesc.Count = m_sceneSamples;
	desc.Usage = D3D11_USAGE_DEFAULT;
	
	desc.Format = ColorFormat;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | (m_sceneSamples > 1 ? 0 : D3D11_BIND_SHADER_RESOURCE);
	if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_sceneColorTarget))
	   || FAILED(m_device->CreateRenderTargetView(m_sceneColorTarget.Get(), nullptr, &m_sceneColorView))) {
		LogError << "Failed to create " << size.x << "x" << size.y << " Direct3D 11 scene color target";
		releaseSceneTargets();
		return false;
	}
	
	// The scene depth is read by the motion vector pass
	{
		desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
		desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
		D3D11_DEPTH_STENCIL_VIEW_DESC depthDesc = {};
		depthDesc.Format = DepthFormat;
		depthDesc.ViewDimension = (m_sceneSamples > 1) ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
		D3D11_SHADER_RESOURCE_VIEW_DESC resourceDesc = {};
		resourceDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		resourceDesc.ViewDimension = (m_sceneSamples > 1) ? D3D11_SRV_DIMENSION_TEXTURE2DMS
		                                                  : D3D11_SRV_DIMENSION_TEXTURE2D;
		resourceDesc.Texture2D.MipLevels = 1;
		if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_sceneDepthTarget))
		   || FAILED(m_device->CreateDepthStencilView(m_sceneDepthTarget.Get(), &depthDesc, &m_sceneDepthView))
		   || FAILED(m_device->CreateShaderResourceView(m_sceneDepthTarget.Get(), &resourceDesc,
		                                                &m_sceneDepthResource))) {
			LogError << "Failed to create " << size.x << "x" << size.y << " Direct3D 11 scene depth target";
			releaseSceneTargets();
			return false;
		}
	}
	
	desc.SampleDesc.Count = 1;
	
	if(m_sceneSamples > 1) {
		desc.Format = ColorFormat;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_sceneResolved))) {
			LogError << "Failed to create " << size.x << "x" << size.y << " Direct3D 11 scene resolve target";
			releaseSceneTargets();
			return false;
		}
	} else {
		m_sceneResolved = m_sceneColorTarget;
	}
	
	if(FAILED(m_device->CreateShaderResourceView(m_sceneResolved.Get(), nullptr, &m_sceneResolvedView))) {
		LogError << "Failed to create Direct3D 11 scene view";
		releaseSceneTargets();
		return false;
	}
	
	// Upscaler inputs, only allocated while something uses them
	if(upscalerInputsNeeded()) {
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		desc.Format = DXGI_FORMAT_R16G16_FLOAT;
		if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_motionTarget))
		   || FAILED(m_device->CreateRenderTargetView(m_motionTarget.Get(), nullptr, &m_motionView))
		   || FAILED(m_device->CreateShaderResourceView(m_motionTarget.Get(), nullptr, &m_motionResource))) {
			LogError << "Failed to create " << size.x << "x" << size.y << " Direct3D 11 motion vector target";
			releaseSceneTargets();
			return false;
		}
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_linearDepthTarget))
		   || FAILED(m_device->CreateRenderTargetView(m_linearDepthTarget.Get(), nullptr, &m_linearDepthView))
		   || FAILED(m_device->CreateShaderResourceView(m_linearDepthTarget.Get(), nullptr, &m_linearDepthResource))) {
			LogError << "Failed to create " << size.x << "x" << size.y << " Direct3D 11 depth target";
			releaseSceneTargets();
			return false;
		}
		// Written by the scene shaders for objects that provide their own motion (needs the same sample count)
		if(m_sceneSamples == 1) {
			desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_objectMotionTarget))
			   || FAILED(m_device->CreateRenderTargetView(m_objectMotionTarget.Get(), nullptr, &m_objectMotionView))
			   || FAILED(m_device->CreateShaderResourceView(m_objectMotionTarget.Get(), nullptr,
			                                                &m_objectMotionResource))) {
				LogError << "Failed to create " << size.x << "x" << size.y << " Direct3D 11 object motion target";
				releaseSceneTargets();
				return false;
			}
		}
	}
	
	m_sceneSize = size;
	m_resetHistory = true;
	
	return true;
}

void D3D11Renderer::releaseSceneTargets() {
	
	if(m_inScene) {
		m_inScene = false;
		invalidateCachedState();
	}
	
	unbindTextureView(m_sceneResolvedView.Get());
	unbindTextureView(m_sceneDepthResource.Get());
	unbindTextureView(m_motionResource.Get());
	unbindTextureView(m_linearDepthResource.Get());
	unbindTextureView(m_objectMotionResource.Get());
	
	m_sceneResolvedView.Reset();
	m_sceneResolved.Reset();
	m_sceneColorView.Reset();
	m_sceneColorTarget.Reset();
	m_sceneDepthResource.Reset();
	m_sceneDepthView.Reset();
	m_sceneDepthTarget.Reset();
	m_motionResource.Reset();
	m_motionView.Reset();
	m_motionTarget.Reset();
	m_linearDepthResource.Reset();
	m_linearDepthView.Reset();
	m_linearDepthTarget.Reset();
	m_objectMotionResource.Reset();
	m_objectMotionView.Reset();
	m_objectMotionTarget.Reset();
	m_sceneSize = Vec2i(0);
	
}

bool D3D11Renderer::sceneEnabled() const {
	return m_sceneScale < 1.f || m_sceneDebugView != 0 || m_jitterEnabled;
}

bool D3D11Renderer::upscalerInputsNeeded() const {
	return m_sceneDebugView != 0 || m_jitterEnabled;
}

static float halton(u32 index, u32 base) {
	float f = 1.f;
	float result = 0.f;
	while(index > 0) {
		f /= float(base);
		result += f * float(index % base);
		index /= base;
	}
	return result;
}

void D3D11Renderer::updateSceneCamera() {
	
	m_sceneView = m_view;
	m_sceneProjection = m_projection;
	
	if(!m_hasPreviousScene || !m_sceneRenderedLastFrame) {
		m_resetHistory = true;
	} else {
		// Detect camera cuts - the view matrix maps the forward direction of the camera to +z
		glm::mat4x4 previous = glm::inverse(m_previousView);
		glm::mat4x4 current = glm::inverse(m_sceneView);
		Vec3f previousPosition(previous[3]);
		Vec3f currentPosition(current[3]);
		Vec3f previousForward = glm::normalize(Vec3f(previous[2]));
		Vec3f currentForward = glm::normalize(Vec3f(current[2]));
		if(glm::distance(previousPosition, currentPosition) > 200.f
		   || glm::dot(previousForward, currentForward) < 0.7f) {
			m_resetHistory = true;
		}
	}
	
	Vec2f scale = Vec2f(m_sceneSize) / Vec2f(m_size);
	
	if(m_jitterEnabled) {
		// Halton(2, 3) sequence with enough phases to cover each window pixel
		u32 phases = u32(std::ceil(8.f / (scale.x * scale.y)));
		m_jitterIndex = (m_jitterIndex + 1) % std::max(phases, u32(1));
		m_jitter = Vec2f(halton(m_jitterIndex + 1, 2) - 0.5f, halton(m_jitterIndex + 1, 3) - 0.5f);
	} else {
		m_jitter = Vec2f(0.f);
	}
	
	// Use the texture detail of the output resolution
	m_sceneLodBias = (scale.x < 1.f) ? std::log2(scale.x) : 0.f;
	
}

void D3D11Renderer::drawFullscreen(ID3D11PixelShader * shader) {
	
	m_context->IASetInputLayout(nullptr);
	m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_currentTopology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	m_context->VSSetShader(m_blitVertexShader.Get(), nullptr, 0);
	m_context->PSSetShader(shader, nullptr, 0);
	m_context->Draw(3, 0);
	
}

void D3D11Renderer::renderSceneMotion() {
	
	ID3D11PixelShader * shader = (m_sceneSamples > 1) ? m_motionPixelShaderMS.Get() : m_motionPixelShader.Get();
	if(!shader || !m_motionView || !m_linearDepthView) {
		return;
	}
	
	MotionConstants constants;
	glm::mat4x4 reprojection(1.f);
	if(!m_resetHistory) {
		// World coordinates are large (thousands of units) - single precision is not enough to cancel them out
		// and results in sub-pixel noise in the motion vectors. Compute the relative camera motion first.
		glm::dmat4 relative = glm::dmat4(m_previousView) * glm::inverse(glm::dmat4(m_sceneView));
		reprojection = glm::mat4x4(glm::dmat4(m_previousProjection) * relative * glm::inverse(glm::dmat4(m_sceneProjection)));
	}
	std::memcpy(constants.reprojection, &reprojection, sizeof(constants.reprojection));
	constants.targetSize[0] = float(m_sceneSize.x);
	constants.targetSize[1] = float(m_sceneSize.y);
	constants.targetSize[2] = 1.f / float(m_sceneSize.x);
	constants.targetSize[3] = 1.f / float(m_sceneSize.y);
	m_context->UpdateSubresource(m_motionConstants.Get(), 0, nullptr, &constants, 0, 0);
	
	ID3D11RenderTargetView * views[] = { m_motionView.Get(), m_linearDepthView.Get() };
	m_context->OMSetRenderTargets(UINT(std::size(views)), views, nullptr);
	
	D3D11_VIEWPORT viewport = { 0.f, 0.f, float(m_sceneSize.x), float(m_sceneSize.y), 0.f, 1.f };
	m_context->RSSetViewports(1, &viewport);
	m_context->RSSetState(getRasterizerState(false, 0, false, false));
	m_context->OMSetBlendState(getBlendState(BlendOne, BlendZero, false, true), nullptr, 0xffffffff);
	m_context->OMSetDepthStencilState(getDepthState(false, false), 0);
	
	ID3D11Buffer * buffer = m_motionConstants.Get();
	m_context->PSSetConstantBuffers(2, 1, &buffer);
	ID3D11ShaderResourceView * inputs[] = { m_sceneDepthResource.Get(), m_objectMotionResource.Get() };
	m_context->PSSetShaderResources(1, UINT(std::size(inputs)), inputs);
	
	drawFullscreen(shader);
	
	ID3D11ShaderResourceView * none[std::size(inputs)] = { };
	m_context->PSSetShaderResources(1, UINT(std::size(none)), none);
	m_currentViews[1] = nullptr;
	m_currentViews[2] = nullptr;
	
}

Vec2f D3D11Renderer::targetScale() const {
	
	if(!m_inScene || m_size.x <= 0 || m_size.y <= 0) {
		return Vec2f(1.f);
	}
	
	return Vec2f(m_sceneSize) / Vec2f(m_size);
}

D3D11_VIEWPORT D3D11Renderer::toTargetViewport(const Rect & rect) const {
	
	Vec2f scale = targetScale();
	
	D3D11_VIEWPORT viewport = {
		float(rect.left) * scale.x, float(rect.top) * scale.y,
		float(rect.width()) * scale.x, float(rect.height()) * scale.y,
		0.f, 1.f
	};
	
	return viewport;
}

D3D11_RECT D3D11Renderer::toTargetRect(const Rect & rect) const {
	
	Vec2f scale = targetScale();
	
	D3D11_RECT result = {
		LONG(std::floor(float(rect.left) * scale.x + 0.5f)), LONG(std::floor(float(rect.top) * scale.y + 0.5f)),
		LONG(std::floor(float(rect.right) * scale.x + 0.5f)), LONG(std::floor(float(rect.bottom) * scale.y + 0.5f))
	};
	
	return result;
}

void D3D11Renderer::bindRenderTargets() {
	
	ID3D11RenderTargetView * colorView = m_inScene ? m_sceneColorView.Get() : m_colorView.Get();
	ID3D11DepthStencilView * depthView = m_inScene ? m_sceneDepthView.Get() : m_depthView.Get();
	
	if(m_inScene && m_objectMotionView) {
		ID3D11RenderTargetView * views[] = { colorView, m_objectMotionView.Get() };
		m_context->OMSetRenderTargets(UINT(std::size(views)), views, depthView);
		return;
	}
	
	m_context->OMSetRenderTargets(1, &colorView, depthView);
	
}

void D3D11Renderer::setSceneScale(float scale) {
	
	scale = glm::clamp(scale, 0.25f, 1.f);
	if(scale == m_sceneScale) {
		return;
	}
	
	m_sceneScale = scale;
	
	if(!m_inScene) {
		releaseSceneTargets();
	}
	
}

void D3D11Renderer::beginScene() {
	
	if(m_inScene || !m_colorView) {
		return;
	}
	
	updateUpscaler();
	
	if(!sceneEnabled()) {
		return;
	}
	
	float scale = std::min(m_sceneScale, 1.f);
	Vec2i size(std::max<s32>(s32(float(m_size.x) * scale + 0.5f), 1),
	           std::max<s32>(s32(float(m_size.y) * scale + 0.5f), 1));
	UINT samples = m_upscaler ? 1 : m_msaaSamples;
	if((size != m_sceneSize || bool(m_motionView) != upscalerInputsNeeded() || samples != m_sceneSamples)
	   && !createSceneTargets(size)) {
		return;
	}
	
	// The scene textures are about to become render targets
	unbindTextureView(m_sceneResolvedView.Get());
	unbindTextureView(m_sceneDepthResource.Get());
	unbindTextureView(m_motionResource.Get());
	unbindTextureView(m_linearDepthResource.Get());
	unbindTextureView(m_objectMotionResource.Get());
	
	if(m_objectMotionView) {
		const float none[4] = { 0.f, 0.f, 0.f, 0.f };
		m_context->ClearRenderTargetView(m_objectMotionView.Get(), none);
	}
	
	updateSceneCamera();
	
	m_inScene = true;
	m_sceneRenderedThisFrame = true;
	invalidateCachedState();
	
}

void D3D11Renderer::endScene() {
	
	if(!m_inScene) {
		return;
	}
	
	m_inScene = false;
	
	if(m_sceneSamples > 1) {
		m_context->ResolveSubresource(m_sceneResolved.Get(), 0, m_sceneColorTarget.Get(), 0, ColorFormat);
	}
	
	renderSceneMotion();
	
	ID3D11ShaderResourceView * upscaled = runUpscaler();
	
	// Upscale the scene to the window
	
	ID3D11RenderTargetView * colorView = m_colorView.Get();
	m_context->OMSetRenderTargets(1, &colorView, nullptr);
	
	D3D11_VIEWPORT viewport = { 0.f, 0.f, float(m_size.x), float(m_size.y), 0.f, 1.f };
	m_context->RSSetViewports(1, &viewport);
	m_context->RSSetState(getRasterizerState(false, 0, false, false));
	m_context->OMSetBlendState(getBlendState(BlendOne, BlendZero, false, true), nullptr, 0xffffffff);
	m_context->OMSetDepthStencilState(getDepthState(false, false), 0);
	
	ID3D11ShaderResourceView * view = upscaled ? upscaled : m_sceneResolvedView.Get();
	ID3D11PixelShader * shader = m_blitPixelShader.Get();
	if(m_sceneDebugView == 1 && m_debugDepthPixelShader && m_linearDepthResource) {
		view = m_linearDepthResource.Get();
		shader = m_debugDepthPixelShader.Get();
	} else if(m_sceneDebugView == 2 && m_debugMotionPixelShader && m_motionResource) {
		view = m_motionResource.Get();
		shader = m_debugMotionPixelShader.Get();
	} else if(m_sceneDebugView == 3 && m_debugObjectMotionPixelShader && m_objectMotionResource) {
		view = m_objectMotionResource.Get();
		shader = m_debugObjectMotionPixelShader.Get();
	}
	
	m_context->PSSetShaderResources(0, 1, &view);
	ID3D11SamplerState * sampler = m_blitSampler.Get();
	m_context->PSSetSamplers(0, 1, &sampler);
	
	drawFullscreen(shader);
	
	// The interface is drawn on top of the scene, with its own depth
	m_context->ClearDepthStencilView(m_depthView.Get(), D3D11_CLEAR_DEPTH, 1.f, 0);
	
	// Remember the camera for the motion vectors of the next frame
	m_previousProjection = m_sceneProjection;
	m_previousView = m_sceneView;
	m_hasPreviousScene = true;
	m_resetHistory = false;
	
	invalidateCachedState();
	m_currentViews[0] = view;
	m_currentSamplers[0] = sampler;
	
}

bool D3D11Renderer::supportsUpscaler(std::string_view name) const {
	
	#if ARX_HAVE_DLSS
	if(name == "dlss") {
		return true;
	}
	#endif
	
	ARX_UNUSED(name);
	return false;
}

std::string D3D11Renderer::getUpscalerError(std::string_view name) const {
	
	if(!supportsUpscaler(name)) {
		return "not supported";
	}
	
	#if ARX_HAVE_DLSS
	if(name == "dlss") {
		if(!d3d11::findDLSSLibrary(config.video.upscalerPath)) {
			return "nvngx_dlss.dll not found";
		}
		if(m_dlssFailed) {
			return "GPU not supported";
		}
	}
	#endif
	
	return std::string();
}

void D3D11Renderer::setUpscaler(std::string_view name, std::string_view preset) {
	
	if(name == m_upscalerName && preset == m_upscalerPreset) {
		return;
	}
	
	m_upscalerName = name;
	m_upscalerPreset = preset;
	
	m_upscaler.reset();
	m_upscalerCreated = false;
	m_jitterEnabled = false;
	
	if(!m_inScene) {
		releaseSceneTargets();
	}
	
}

void D3D11Renderer::updateUpscaler() {
	
	if(m_upscalerCreated) {
		return;
	}
	m_upscalerCreated = true;
	
	if(m_upscalerName.empty() || m_upscalerName == "off") {
		return;
	}
	
	#if ARX_HAVE_DLSS
	if(m_upscalerName == "dlss") {
		if(!d3d11::findDLSSLibrary(config.video.upscalerPath)) {
			// nvngx_dlss.dll is not distributed with Arx Libertatis
			LogWarning << "DLSS not available: nvngx_dlss.dll not found - copy it from the NVIDIA DLSS SDK"
			           << " or another game next to arx.exe";
		} else {
			m_upscaler = d3d11::createDLSSUpscaler(m_device.Get(), m_upscalerPreset, config.video.upscalerPath);
			m_dlssFailed = !m_upscaler;
		}
	}
	#endif
	
	if(!m_upscaler) {
		LogWarning << "Upscaler \"" << m_upscalerName << "\" not available, using bilinear scaling";
	}
	
	// Temporal upscalers need sub-pixel jitter and a scene without MSAA
	m_jitterEnabled = (m_upscaler != nullptr);
	
}

ID3D11ShaderResourceView * D3D11Renderer::runUpscaler() {
	
	if(!m_upscaler || !m_motionTarget || !m_linearDepthTarget) {
		return nullptr;
	}
	
	if(!m_upscaledTarget) {
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = UINT(m_size.x);
		desc.Height = UINT(m_size.y);
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = ColorFormat;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_upscaledTarget))
		   || FAILED(m_device->CreateShaderResourceView(m_upscaledTarget.Get(), nullptr, &m_upscaledView))) {
			LogError << "Failed to create " << m_size.x << "x" << m_size.y << " Direct3D 11 upscaler output";
			m_upscaledTarget.Reset();
			m_upscaledView.Reset();
			return nullptr;
		}
	}
	
	// The upscaler reads the scene textures and writes the output
	m_context->OMSetRenderTargets(0, nullptr, nullptr);
	ID3D11ShaderResourceView * none[TextureStageCount] = { };
	m_context->PSSetShaderResources(0, TextureStageCount, none);
	for(UINT i = 0; i < TextureStageCount; i++) {
		m_currentViews[i] = nullptr;
	}
	
	LARGE_INTEGER now, frequency;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&frequency);
	float frameTime = 1000.f / 60.f;
	if(m_lastUpscaleTime != 0) {
		frameTime = float(double(now.QuadPart - m_lastUpscaleTime) * 1000.0 / double(frequency.QuadPart));
	}
	m_lastUpscaleTime = now.QuadPart;
	
	D3D11Upscaler::Inputs inputs;
	inputs.color = m_sceneResolved.Get();
	inputs.depth = m_linearDepthTarget.Get();
	inputs.motion = m_motionTarget.Get();
	inputs.output = m_upscaledTarget.Get();
	inputs.renderSize = m_sceneSize;
	inputs.outputSize = m_size;
	inputs.jitter = m_jitter;
	inputs.reset = m_resetHistory;
	inputs.frameTime = frameTime;
	
	bool success = m_upscaler->evaluate(m_context.Get(), inputs);
	
	// The upscaler changes the device state
	invalidateCachedState();
	
	return success ? m_upscaledView.Get() : nullptr;
}

void D3D11Renderer::reinit() {
	
	arx_assert(!isInitialized());
	
	if(!m_colorView) {
		createSizeDependentResources();
	}
	
	m_hasMSAA = false;
	
	m_TextureStages.resize(TextureStageCount);
	for(size_t i = 0; i < m_TextureStages.size(); ++i) {
		m_TextureStages[i] = std::make_unique<D3D11TextureStage>(this, unsigned(i));
	}
	
	invalidateCachedState();
	
	// Clear screen
	Clear(ColorBuffer | DepthBuffer);
	
	onRendererInit();
	
}

void D3D11Renderer::shutdown() {
	
	arx_assert(isInitialized());
	
	onRendererShutdown();
	
	m_TextureStages.clear();
	
	m_maximumAnisotropy = 1.f;
	m_maximumSupportedAnisotropy = 1.f;
	
}

void D3D11Renderer::invalidateCachedState() {
	m_stateValid = false;
	m_vsConstantsValid = false;
	m_psConstantsValid = false;
}

void D3D11Renderer::SetViewMatrix(const glm::mat4x4 & matView) {
	m_view = matView;
}

void D3D11Renderer::SetProjectionMatrix(const glm::mat4x4 & matProj) {
	m_projection = matProj;
}

void D3D11Renderer::ReleaseAllTextures() {
	for(D3D11Texture & texture : m_textures) {
		texture.destroy();
	}
}

void D3D11Renderer::RestoreAllTextures() {
	for(D3D11Texture & texture : m_textures) {
		texture.restore();
	}
}

void D3D11Renderer::reloadColorKeyTextures() {
	for(D3D11Texture & texture : m_textures) {
		if(texture.hasColorKey()) {
			texture.restore();
		}
	}
}

Texture * D3D11Renderer::createTexture() {
	D3D11Texture * texture = new D3D11Texture(this);
	m_textures.push_back(*texture);
	return texture;
}

void D3D11Renderer::onTextureDestroyed(D3D11Texture * texture) {
	for(size_t i = 0; i < getTextureStageCount(); i++) {
		D3D11TextureStage * stage = GetTextureStage(i);
		if(stage->m_texture == texture) {
			stage->m_texture = nullptr;
		}
	}
}

void D3D11Renderer::unbindTextureView(ID3D11ShaderResourceView * view) {
	
	if(!view || !m_context) {
		return;
	}
	
	// The view may be released - make sure a new view at the same address will be bound
	for(UINT i = 0; i < TextureStageCount; i++) {
		if(m_currentViews[i] == view) {
			ID3D11ShaderResourceView * none = nullptr;
			m_context->PSSetShaderResources(i, 1, &none);
			m_currentViews[i] = nullptr;
		}
	}
	
}

void D3D11Renderer::SetViewport(const Rect & viewport) {
	
	m_viewport = viewport;
	
	if(m_context && m_stateValid) {
		D3D11_VIEWPORT vp = toTargetViewport(m_viewport);
		m_context->RSSetViewports(1, &vp);
	}
	
}

void D3D11Renderer::SetScissor(const Rect & rect) {
	
	m_scissor = rect;
	
	if(m_context && m_stateValid && m_scissor.isValid()) {
		D3D11_RECT r = toTargetRect(m_scissor);
		m_context->RSSetScissorRects(1, &r);
	}
	
}

void D3D11Renderer::Clear(BufferFlags bufferFlags, Color clearColor, float clearDepth, size_t nrects,
                          Rect * rect) {
	
	if(!m_colorView || !m_depthView) {
		return;
	}
	
	Color4f color(clearColor);
	
	ID3D11RenderTargetView * colorView = m_inScene ? m_sceneColorView.Get() : m_colorView.Get();
	ID3D11DepthStencilView * depthView = m_inScene ? m_sceneDepthView.Get() : m_depthView.Get();
	
	if(nrects == 0) {
		if(bufferFlags & ColorBuffer) {
			const float c[4] = { color.r, color.g, color.b, color.a };
			m_context->ClearRenderTargetView(colorView, c);
		}
		if(bufferFlags & DepthBuffer) {
			m_context->ClearDepthStencilView(depthView, D3D11_CLEAR_DEPTH, clearDepth, 0);
		}
		return;
	}
	
	// Partial clears are done by drawing a quad for each rectangle
	
	ClearConstants constants = { { color.r, color.g, color.b, color.a }, { clearDepth, 0.f, 0.f, 0.f } };
	m_context->UpdateSubresource(m_clearConstants.Get(), 0, nullptr, &constants, 0, 0);
	
	m_context->OMSetRenderTargets(1, &colorView, depthView);
	m_context->IASetInputLayout(nullptr);
	m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_context->VSSetShader(m_clearVertexShader.Get(), nullptr, 0);
	m_context->PSSetShader(m_clearPixelShader.Get(), nullptr, 0);
	ID3D11Buffer * buffer = m_clearConstants.Get();
	m_context->VSSetConstantBuffers(1, 1, &buffer);
	m_context->PSSetConstantBuffers(1, 1, &buffer);
	m_context->OMSetBlendState(getBlendState(BlendOne, BlendZero, false, (bufferFlags & ColorBuffer) != 0),
	                           nullptr, 0xffffffff);
	m_context->OMSetDepthStencilState(getDepthState(false, (bufferFlags & DepthBuffer) != 0), 0);
	m_context->RSSetState(getRasterizerState(false, 0, false, true));
	
	D3D11_VIEWPORT vp = toTargetViewport(Rect(m_size.x, m_size.y));
	m_context->RSSetViewports(1, &vp);
	
	for(size_t i = 0; i < nrects; i++) {
		D3D11_RECT r = toTargetRect(rect[i]);
		m_context->RSSetScissorRects(1, &r);
		m_context->Draw(3, 0);
	}
	
	invalidateCachedState();
	
}

void D3D11Renderer::SetFogColor(Color color) {
	m_fogColor = Color4f(color);
}

void D3D11Renderer::SetFogParams(float fogStart, float fogEnd) {
	m_fogStart = fogStart;
	m_fogEnd = fogEnd;
}

void D3D11Renderer::SetAntialiasing(bool enable) {
	
	if(m_msaaSamples <= 1) {
		return;
	}
	
	if(enable && !config.video.antialiasing) {
		return;
	}
	
	// Direct3D 11 always rasterizes triangles with multisampling when rendering to a multisampled target,
	// this only changes how alpha cutouts are handled.
	m_hasMSAA = enable;
}

void D3D11Renderer::SetFillMode(FillMode mode) {
	m_wireframe = (mode == FillWireframe);
}

void D3D11Renderer::setMaxAnisotropy(float value) {
	
	float maxAnisotropy = glm::clamp(value, 1.f, m_maximumSupportedAnisotropy);
	if(m_maximumAnisotropy == maxAnisotropy) {
		return;
	}
	
	m_maximumAnisotropy = maxAnisotropy;
	
	for(size_t i = 0; i < getTextureStageCount(); i++) {
		GetTextureStage(i)->clearSamplers();
	}
	
	invalidateCachedState();
	
}

Renderer::AlphaCutoutAntialising D3D11Renderer::getMaxSupportedAlphaCutoutAntialiasing() const {
	return m_pixelShaderSampleShading ? CrispAlphaCutoutAA : FuzzyAlphaCutoutAA;
}

std::unique_ptr<VertexBuffer<TexturedVertex>> D3D11Renderer::createVertexBufferTL(size_t capacity,
                                                                                  BufferUsage usage) {
	return std::make_unique<D3D11VertexBuffer<TexturedVertex>>(this, capacity, usage);
}

std::unique_ptr<VertexBuffer<SMY_VERTEX>> D3D11Renderer::createVertexBuffer(size_t capacity,
                                                                            BufferUsage usage) {
	return std::make_unique<D3D11VertexBuffer<SMY_VERTEX>>(this, capacity, usage);
}

std::unique_ptr<VertexBuffer<SMY_VERTEX3>> D3D11Renderer::createVertexBuffer3(size_t capacity,
                                                                              BufferUsage usage) {
	return std::make_unique<D3D11VertexBuffer<SMY_VERTEX3>>(this, capacity, usage);
}

size_t D3D11Renderer::RingBuffer::upload(ID3D11DeviceContext * context, const void * data, size_t bytes,
                                         size_t alignment) {
	
	if(bytes > size) {
		return size_t(-1);
	}
	
	size_t offset = (pos + alignment - 1) / alignment * alignment;
	D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
	if(offset == 0 || offset + bytes > size) {
		offset = 0;
		mode = D3D11_MAP_WRITE_DISCARD;
	}
	
	D3D11_MAPPED_SUBRESOURCE mapped;
	if(FAILED(context->Map(buffer.Get(), 0, mode, 0, &mapped))) {
		return size_t(-1);
	}
	std::memcpy(static_cast<char *>(mapped.pData) + offset, data, bytes);
	context->Unmap(buffer.Get(), 0);
	
	pos = offset + bytes;
	
	return offset;
}

bool D3D11Renderer::uploadIndices(const unsigned short * indices, size_t nindices, Primitive primitive,
                                  UINT & start, size_t & count) {
	
	if(primitive == TriangleFan) {
		if(nindices < 3) {
			return false;
		}
		m_indexScratch.resize((nindices - 2) * 3);
		for(size_t i = 1; i + 1 < nindices; i++) {
			m_indexScratch[(i - 1) * 3 + 0] = indices[0];
			m_indexScratch[(i - 1) * 3 + 1] = indices[i];
			m_indexScratch[(i - 1) * 3 + 2] = indices[i + 1];
		}
		indices = m_indexScratch.data();
		nindices = m_indexScratch.size();
	}
	
	size_t offset = m_indexRing.upload(m_context.Get(), indices, nindices * sizeof(unsigned short),
	                                   sizeof(unsigned short));
	if(offset == size_t(-1)) {
		static bool warned = false;
		if(!warned) {
			LogError << "Failed to upload " << nindices << " indices";
			warned = true;
		}
		return false;
	}
	
	start = UINT(offset / sizeof(unsigned short));
	count = nindices;
	
	return true;
}

void D3D11Renderer::setPrimitive(Primitive primitive) {
	D3D11_PRIMITIVE_TOPOLOGY topology = arxToD3DPrimitive[primitive];
	if(topology != m_currentTopology) {
		m_context->IASetPrimitiveTopology(topology);
		m_currentTopology = topology;
	}
}

void D3D11Renderer::bindVertexBuffer(VertexType type, ID3D11Buffer * buffer, UINT stride, UINT offset) {
	ARX_UNUSED(type);
	if(buffer != m_currentVertexBuffer || offset != m_currentVertexOffset) {
		m_context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
		m_currentVertexBuffer = buffer;
		m_currentVertexOffset = offset;
	}
}

bool D3D11Renderer::drawFan(size_t count, size_t baseVertex) {
	
	if(count < 3) {
		return false;
	}
	
	m_indexScratch.resize((count - 2) * 3);
	for(size_t i = 1; i + 1 < count; i++) {
		m_indexScratch[(i - 1) * 3 + 0] = 0;
		m_indexScratch[(i - 1) * 3 + 1] = static_cast<unsigned short>(i);
		m_indexScratch[(i - 1) * 3 + 2] = static_cast<unsigned short>(i + 1);
	}
	
	UINT start;
	size_t nindices;
	if(!uploadIndices(m_indexScratch.data(), m_indexScratch.size(), TriangleList, start, nindices)) {
		return false;
	}
	
	setPrimitive(TriangleList);
	m_context->DrawIndexed(UINT(nindices), start, INT(baseVertex));
	
	return true;
}

void D3D11Renderer::drawBuffer(VertexType type, Primitive primitive, ID3D11Buffer * buffer, UINT stride,
                               size_t count, size_t offset) {
	
	if(!m_colorView) {
		return;
	}
	
	flushState(type);
	
	bindVertexBuffer(type, buffer, stride, 0);
	
	if(primitive == TriangleFan) {
		drawFan(count, offset);
		return;
	}
	
	setPrimitive(primitive);
	m_context->Draw(UINT(count), UINT(offset));
	
}

void D3D11Renderer::drawBufferIndexed(VertexType type, Primitive primitive, ID3D11Buffer * buffer,
                                      UINT stride, size_t baseVertex, const unsigned short * indices,
                                      size_t nindices) {
	
	if(!m_colorView) {
		return;
	}
	
	flushState(type);
	
	bindVertexBuffer(type, buffer, stride, 0);
	
	UINT start;
	size_t count;
	if(!uploadIndices(indices, nindices, primitive, start, count)) {
		return;
	}
	
	setPrimitive(primitive);
	m_context->DrawIndexed(UINT(count), start, INT(baseVertex));
	
}

void D3D11Renderer::drawIndexed(Primitive primitive, const TexturedVertex * vertices, size_t nvertices,
                                unsigned short * indices, size_t nindices) {
	
	if(!m_colorView || nvertices == 0 || nindices == 0) {
		return;
	}
	
	flushState(VertexTL);
	
	size_t offset = m_vertexRing.upload(m_context.Get(), vertices, nvertices * sizeof(TexturedVertex),
	                                    sizeof(TexturedVertex));
	if(offset == size_t(-1)) {
		static bool warned = false;
		if(!warned) {
			LogError << "Failed to upload " << nvertices << " vertices";
			warned = true;
		}
		return;
	}
	
	bindVertexBuffer(VertexTL, m_vertexRing.buffer.Get(), UINT(sizeof(TexturedVertex)), UINT(offset));
	
	UINT start;
	size_t count;
	if(!uploadIndices(indices, nindices, primitive, start, count)) {
		return;
	}
	
	setPrimitive(primitive);
	m_context->DrawIndexed(UINT(count), start, 0);
	
}

bool D3D11Renderer::getSnapshot(Image & image) {
	
	if(!m_colorTarget) {
		return false;
	}
	
	ComPtr<ID3D11Texture2D> source = m_colorTarget;
	
	D3D11_TEXTURE2D_DESC desc;
	m_colorTarget->GetDesc(&desc);
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.BindFlags = 0;
	desc.MiscFlags = 0;
	
	if(m_msaaSamples > 1) {
		ComPtr<ID3D11Texture2D> resolved;
		desc.Usage = D3D11_USAGE_DEFAULT;
		if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &resolved))) {
			return false;
		}
		m_context->ResolveSubresource(resolved.Get(), 0, m_colorTarget.Get(), 0, ColorFormat);
		source = resolved;
	}
	
	ComPtr<ID3D11Texture2D> staging;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &staging))) {
		return false;
	}
	m_context->CopyResource(staging.Get(), source.Get());
	
	D3D11_MAPPED_SUBRESOURCE mapped;
	if(FAILED(m_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
		return false;
	}
	
	image.create(size_t(desc.Width), size_t(desc.Height), Image::Format_R8G8B8);
	u8 * out = image.getData();
	for(UINT y = 0; y < desc.Height; y++) {
		const u8 * in = static_cast<const u8 *>(mapped.pData) + size_t(y) * mapped.RowPitch;
		for(UINT x = 0; x < desc.Width; x++, in += 4, out += 3) {
			out[0] = in[0];
			out[1] = in[1];
			out[2] = in[2];
		}
	}
	
	m_context->Unmap(staging.Get(), 0);
	
	return true;
}

bool D3D11Renderer::getSnapshot(Image & image, size_t width, size_t height) {
	
	// TODO handle scaling on the GPU so we don't need to download the whole image
	
	Image fullsize;
	if(!getSnapshot(fullsize)) {
		return false;
	}
	
	image.resizeFrom(fullsize, width, height);
	
	return true;
}

void D3D11Renderer::present() {
	
	if(!m_swapChain || !m_colorTarget) {
		return;
	}
	
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc;
	if(SUCCEEDED(m_swapChain->GetDesc1(&swapChainDesc))
	   && (swapChainDesc.Width != UINT(m_size.x) || swapChainDesc.Height != UINT(m_size.y))) {
		m_swapChain->ResizeBuffers(0, UINT(m_size.x), UINT(m_size.y), DXGI_FORMAT_UNKNOWN, swapChainDesc.Flags);
	}
	
	ComPtr<ID3D11Texture2D> backBuffer;
	if(SUCCEEDED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
		D3D11_TEXTURE2D_DESC desc;
		backBuffer->GetDesc(&desc);
		if(desc.Width == UINT(m_size.x) && desc.Height == UINT(m_size.y)) {
			if(m_msaaSamples > 1) {
				m_context->ResolveSubresource(backBuffer.Get(), 0, m_colorTarget.Get(), 0, ColorFormat);
			} else {
				m_context->CopyResource(backBuffer.Get(), m_colorTarget.Get());
			}
		}
	}
	
	m_sceneRenderedLastFrame = m_sceneRenderedThisFrame;
	m_sceneRenderedThisFrame = false;
	
	UINT interval = (m_vsync != 0) ? 1 : 0;
	UINT flags = (interval == 0 && m_allowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
	
	HRESULT hr = m_swapChain->Present(interval, flags);
	if(hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
		static bool warned = false;
		if(!warned) {
			LogError << "Direct3D 11 device lost: " << hresultString(m_device->GetDeviceRemovedReason());
			warned = true;
		}
	}
	
}

ID3D11RasterizerState * D3D11Renderer::getRasterizerState(bool cull, unsigned depthOffset, bool wireframe,
                                                          bool scissor) {
	
	u32 key = u32(cull) | (u32(depthOffset) << 1) | (u32(wireframe) << 8) | (u32(scissor) << 9);
	
	auto it = m_rasterizerStates.find(key);
	if(it != m_rasterizerStates.end()) {
		return it->second.Get();
	}
	
	D3D11_RASTERIZER_DESC desc = {};
	desc.FillMode = wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
	desc.CullMode = cull ? D3D11_CULL_BACK : D3D11_CULL_NONE;
	// Same winding as the OpenGL default
	desc.FrontCounterClockwise = TRUE;
	desc.DepthBias = -INT(depthOffset);
	desc.DepthBiasClamp = 0.f;
	desc.SlopeScaledDepthBias = -float(depthOffset);
	desc.DepthClipEnable = TRUE;
	desc.ScissorEnable = scissor ? TRUE : FALSE;
	desc.MultisampleEnable = (m_msaaSamples > 1) ? TRUE : FALSE;
	desc.AntialiasedLineEnable = FALSE;
	
	ComPtr<ID3D11RasterizerState> state;
	if(FAILED(m_device->CreateRasterizerState(&desc, &state))) {
		LogError << "Failed to create Direct3D 11 rasterizer state";
		return nullptr;
	}
	
	ID3D11RasterizerState * result = state.Get();
	m_rasterizerStates.emplace(key, std::move(state));
	return result;
}

ID3D11BlendState * D3D11Renderer::getBlendState(BlendingFactor src, BlendingFactor dst, bool alphaToCoverage,
                                                bool colorWrite) {
	
	u32 key = u32(src) | (u32(dst) << 4) | (u32(alphaToCoverage) << 8) | (u32(colorWrite) << 9);
	
	auto it = m_blendStates.find(key);
	if(it != m_blendStates.end()) {
		return it->second.Get();
	}
	
	D3D11_BLEND_DESC desc = {};
	desc.AlphaToCoverageEnable = alphaToCoverage ? TRUE : FALSE;
	desc.IndependentBlendEnable = TRUE;
	// The second target (object motion or depth) is never blended and only written by opaque draws
	D3D11_RENDER_TARGET_BLEND_DESC & motion = desc.RenderTarget[1];
	motion.BlendEnable = FALSE;
	motion.SrcBlend = motion.SrcBlendAlpha = D3D11_BLEND_ONE;
	motion.DestBlend = motion.DestBlendAlpha = D3D11_BLEND_ZERO;
	motion.BlendOp = motion.BlendOpAlpha = D3D11_BLEND_OP_ADD;
	motion.RenderTargetWriteMask = (colorWrite && src == BlendOne && dst == BlendZero) ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
	D3D11_RENDER_TARGET_BLEND_DESC & target = desc.RenderTarget[0];
	target.BlendEnable = (src != BlendOne || dst != BlendZero) ? TRUE : FALSE;
	target.SrcBlend = arxToD3DBlendFactor[src];
	target.DestBlend = arxToD3DBlendFactor[dst];
	target.BlendOp = D3D11_BLEND_OP_ADD;
	target.SrcBlendAlpha = arxToD3DAlphaBlendFactor[src];
	target.DestBlendAlpha = arxToD3DAlphaBlendFactor[dst];
	target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
	target.RenderTargetWriteMask = colorWrite ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
	
	ComPtr<ID3D11BlendState> state;
	if(FAILED(m_device->CreateBlendState(&desc, &state))) {
		LogError << "Failed to create Direct3D 11 blend state";
		return nullptr;
	}
	
	ID3D11BlendState * result = state.Get();
	m_blendStates.emplace(key, std::move(state));
	return result;
}

ID3D11DepthStencilState * D3D11Renderer::getDepthState(bool test, bool write) {
	
	u32 key = u32(test) | (u32(write) << 1);
	
	auto it = m_depthStates.find(key);
	if(it != m_depthStates.end()) {
		return it->second.Get();
	}
	
	// Like the OpenGL backend, the depth test is always enabled so that depth writes work
	D3D11_DEPTH_STENCIL_DESC desc = {};
	desc.DepthEnable = TRUE;
	desc.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
	desc.DepthFunc = test ? D3D11_COMPARISON_LESS_EQUAL : D3D11_COMPARISON_ALWAYS;
	desc.StencilEnable = FALSE;
	
	ComPtr<ID3D11DepthStencilState> state;
	if(FAILED(m_device->CreateDepthStencilState(&desc, &state))) {
		LogError << "Failed to create Direct3D 11 depth stencil state";
		return nullptr;
	}
	
	ID3D11DepthStencilState * result = state.Get();
	m_depthStates.emplace(key, std::move(state));
	return result;
}

void D3D11Renderer::flushState(VertexType type) {
	
	bool force = !m_stateValid;
	
	if(force) {
		
		bindRenderTargets();
		
		D3D11_VIEWPORT vp = toTargetViewport(m_viewport);
		m_context->RSSetViewports(1, &vp);
		
		if(m_scissor.isValid()) {
			D3D11_RECT r = toTargetRect(m_scissor);
			m_context->RSSetScissorRects(1, &r);
		}
		
		m_context->IASetIndexBuffer(m_indexRing.buffer.Get(), DXGI_FORMAT_R16_UINT, 0);
		
		ID3D11Buffer * vsConstants = m_vsConstants.Get();
		m_context->VSSetConstantBuffers(0, 1, &vsConstants);
		ID3D11Buffer * psConstants = m_psConstants.Get();
		m_context->PSSetConstantBuffers(0, 1, &psConstants);
		
		m_currentVertexBuffer = nullptr;
		m_currentTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		
	}
	
	// Vertex shader
	
	if(force || type != m_currentVertexType) {
		m_context->IASetInputLayout(m_inputLayouts[type].Get());
		m_context->VSSetShader(m_vertexShaders[type].Get(), nullptr, 0);
		m_currentVertexType = type;
	}
	
	{
		glm::mat4x4 transform;
		if(type == VertexTL) {
			// Direct3D doesn't apply any transform for D3DTLVERTEX
			// Change coordinate system from [0, width] x [0, height] to [-1, 1] x [-1, 1] and flip the y axis
			transform = glm::translate(glm::mat4x4(1.f), Vec3f(-1.f, 1.f, 0.f));
			transform = glm::scale(transform, Vec3f(2.f / float(std::max<s32>(m_viewport.width(), 1)),
			                                        -2.f / float(std::max<s32>(m_viewport.height(), 1)), 1.f));
			// Change pixel origins
			transform = glm::translate(transform, Vec3f(0.5f, 0.5f, 0.f));
		} else {
			transform = m_projection * m_view;
		}
		VSConstants constants;
		std::memcpy(constants.transform, &transform, sizeof(constants.transform));
		constants.jitter[0] = m_inScene ? 2.f * m_jitter.x / float(m_sceneSize.x) : 0.f;
		constants.jitter[1] = m_inScene ? -2.f * m_jitter.y / float(m_sceneSize.y) : 0.f;
		constants.jitter[2] = constants.jitter[3] = 0.f;
		static_assert(sizeof(constants) == sizeof(m_vsConstantsData), "unexpected constant buffer size");
		if(!m_vsConstantsValid || std::memcmp(m_vsConstantsData, &constants, sizeof(constants)) != 0) {
			std::memcpy(m_vsConstantsData, &constants, sizeof(constants));
			m_context->UpdateSubresource(m_vsConstants.Get(), 0, nullptr, m_vsConstantsData, 0, 0);
			m_vsConstantsValid = true;
		}
	}
	
	// Blending and alpha cutouts - same logic as in the OpenGL backend
	
	enum TestMode {
		TestSS,
		TestA2C,
		TestStrict,
		TestConservative,
		TestNone,
	};
	
	// The scene has no MSAA when using a temporal upscaler
	bool hasMSAA = m_hasMSAA && !(m_inScene && m_sceneSamples <= 1);
	bool useSS = hasMSAA && m_pixelShaderSampleShading
	             && config.video.alphaCutoutAntialiasing == int(CrispAlphaCutoutAA);
	bool useA2C = hasMSAA && config.video.alphaCutoutAntialiasing == int(FuzzyAlphaCutoutAA);
	
	/* When rendering alpha cutouts with alpha blending enabled we still
	 * need to 'discard' transparent texels, as blending might not use the src alpha!
	 * On the other hand, we can't use alpha to coverage when blending
	 * as that could result in the src alpha being applied twice (e.g. for text).
	 * So we must toggle between alpha to coverage and alpha test when toggling blending.
	 */
	BlendingFactor blendSrc = m_state.getBlendSrc();
	BlendingFactor blendDst = m_state.getBlendDst();
	TestMode alphaTest = TestNone;
	if(m_state.getAlphaCutout()) {
		arx_assert_msg(blendSrc != BlendInvSrcAlpha && blendDst != BlendSrcAlpha,
		               "inverted alpha blending combined with alpha test makes no sense");
		if(blendSrc == BlendOne && blendDst == BlendZero) {
			alphaTest = useSS ? TestSS : useA2C ? TestA2C : TestStrict;
		} else if(blendSrc == BlendOne && blendDst == BlendOne) {
			blendSrc = BlendSrcAlpha;
			alphaTest = TestConservative; // optimization only, could use TestNone
		} else if(blendSrc == BlendSrcAlpha || blendDst == BlendInvSrcAlpha) {
			alphaTest = TestConservative;
		} else {
			alphaTest = useSS ? TestSS : TestStrict;
		}
	}
	
	float alphaReference = -1.f;
	if(alphaTest == TestConservative || alphaTest == TestA2C) {
		alphaReference = 0.f;
	} else if(alphaTest == TestStrict || alphaTest == TestSS) {
		alphaReference = 0.5f;
	}
	
	ID3D11BlendState * blend = getBlendState(blendSrc, blendDst, alphaTest == TestA2C, true);
	if(force || blend != m_currentBlend) {
		m_context->OMSetBlendState(blend, nullptr, 0xffffffff);
		m_currentBlend = blend;
	}
	
	ID3D11PixelShader * pixelShader = (alphaTest == TestSS) ? m_pixelShaderSampleShading.Get()
	                                                        : m_pixelShader.Get();
	if(force || pixelShader != m_currentPixelShader) {
		m_context->PSSetShader(pixelShader, nullptr, 0);
		m_currentPixelShader = pixelShader;
	}
	
	ID3D11RasterizerState * rasterizer = getRasterizerState(m_state.getCull(), m_state.getDepthOffset(),
	                                                        m_wireframe, m_scissor.isValid());
	if(force || rasterizer != m_currentRasterizer) {
		m_context->RSSetState(rasterizer);
		m_currentRasterizer = rasterizer;
	}
	
	ID3D11DepthStencilState * depth = getDepthState(m_state.getDepthTest(), m_state.getDepthWrite());
	if(force || depth != m_currentDepth) {
		m_context->OMSetDepthStencilState(depth, 0);
		m_currentDepth = depth;
	}
	
	// Texture stages
	
	PSConstants constants = {};
	
	for(UINT i = 0; i < TextureStageCount; i++) {
		
		D3D11TextureStage * stage = GetTextureStage(i);
		D3D11Texture * texture = stage->getActiveTexture();
		
		ID3D11ShaderResourceView * view = texture ? texture->view() : nullptr;
		if(force || view != m_currentViews[i]) {
			m_context->PSSetShaderResources(i, 1, &view);
			m_currentViews[i] = view;
		}
		
		if(texture) {
			ID3D11SamplerState * sampler = stage->getSampler(texture);
			if(force || sampler != m_currentSamplers[i]) {
				m_context->PSSetSamplers(i, 1, &sampler);
				m_currentSamplers[i] = sampler;
			}
		}
		
		constants.stages[i][0] = s32(stage->getColorOp());
		constants.stages[i][1] = s32(stage->getAlphaOp());
		constants.stages[i][2] = texture ? 1 : 0;
		
	}
	
	constants.fogColor[0] = m_fogColor.r;
	constants.fogColor[1] = m_fogColor.g;
	constants.fogColor[2] = m_fogColor.b;
	constants.fogColor[3] = m_fogColor.a;
	constants.fogParams[0] = m_fogStart;
	constants.fogParams[1] = m_fogEnd;
	constants.fogParams[2] = m_state.getFog() ? 1.f : 0.f;
	constants.fogParams[3] = alphaReference;
	
	static_assert(sizeof(constants) <= sizeof(m_psConstantsData), "constant buffer cache too small");
	if(!m_psConstantsValid || std::memcmp(m_psConstantsData, &constants, sizeof(constants)) != 0) {
		std::memcpy(m_psConstantsData, &constants, sizeof(constants));
		m_context->UpdateSubresource(m_psConstants.Get(), 0, nullptr, &constants, 0, 0);
		m_psConstantsValid = true;
	}
	
	m_stateValid = true;
	
}

namespace d3d11 {

Renderer * createRenderer() {
	return new D3D11Renderer;
}

int createDevice(Renderer & renderer, void * window, int maxMSAALevel) {
	D3D11Renderer & d3d11 = static_cast<D3D11Renderer &>(renderer);
	if(!d3d11.create(static_cast<HWND>(window), maxMSAALevel)) {
		return 0;
	}
	return std::max(int(d3d11.msaaSamples()), 1);
}

void setVSync(Renderer & renderer, int vsync) {
	static_cast<D3D11Renderer &>(renderer).setVSync(vsync);
}

void present(Renderer & renderer) {
	static_cast<D3D11Renderer &>(renderer).present();
}

} // namespace d3d11
