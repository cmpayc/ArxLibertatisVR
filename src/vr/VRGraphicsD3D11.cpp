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

#ifndef XR_NO_PROTOTYPES
#define XR_NO_PROTOTYPES
#endif

#include "vr/VRGraphics.h"
#include "vr/VRFunctions.h"

#include <algorithm>

#include <d3d11.h>

#include "graphics/Renderer.h"
#include "graphics/d3d11/D3D11Renderer.h"
#include "graphics/d3d11/D3D11Texture.h"
#include "graphics/d3d11/D3D11Window.h"
#include "io/log/Logger.h"

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr_platform.h>

namespace {

using d3d11::ComPtr;

D3D11Renderer * getD3D11Renderer() {
	// The window falls back to OpenGL if the Direct3D 11 device cannot be created
	return d3d11::isDirect3D11(GRenderer) ? static_cast<D3D11Renderer *>(GRenderer) : nullptr;
}

/*!
 * OpenXR with the Direct3D 11 renderer: the renderer draws the eyes side by side into its scene targets with all
 * effects, and the result is copied into the eye images
 */
class VRGraphicsD3D11 final : public VRGraphics {

public:
	
	const char * getName() const override { return "Direct3D 11"; }
	const char * getExtension() const override { return XR_KHR_D3D11_ENABLE_EXTENSION_NAME; }
	bool isDirect3D11() const override { return true; }
	
	bool checkRequirements(XrInstance instance, XrSystemId system) override;
	const void * getSessionBinding(XrInstance instance, XrSystemId system) override;
	
	s64 selectColorFormat(const std::vector<s64> & formats) const override;
	bool enumerateImages(XrSwapchain swapchain, std::vector<u64> & images) override;
	// Direct3D images start at the top
	bool areImageRectsFromTop(bool metaRuntime) const override { ARX_UNUSED(metaRuntime); return true; }
	bool isPanelCopyFlipped() const override { return false; }
	
	bool createStereoTarget(const Vec2i & eyeSize) override;
	void destroyStereoTarget() override;
	bool beginStereo(const std::vector<StereoView> & views) override;
	bool endStereo(bool uiActive) override;
	void copyEyeToImage(size_t eye, u64 image) override;
	void drawEyeToWindow(const Vec2i & windowSize) override;
	
	bool createUITarget(const Vec2i & size) override;
	void destroyUITarget() override;
	bool hasUITarget() const override { return m_uiColor.Get() != nullptr; }
	void beginUI() override;
	void endUI() override;
	void setPremultipliedAlphaOutput(bool enable) override;
	void copyUIToImage(u64 image) override;
	bool copyUIToTexture(Texture * texture, const Rect & area) override;
	void copyWindowToImage(u64 image, const Vec2i & windowSize) override;
	void drawUIToWindow(const Vec2i & windowSize, bool clear, const std::vector<VRWindowQuad> & quads) override;

private:
	
	static ID3D11Texture2D * toTexture(u64 image) {
		return reinterpret_cast<ID3D11Texture2D *>(static_cast<uintptr_t>(image));
	}
	
	XrGraphicsBindingD3D11KHR m_binding = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
	
	Vec2i m_eyeSize = Vec2i(0);
	// The stereo scene of the renderer, both eyes side by side
	ComPtr<ID3D11Texture2D> m_output;
	ComPtr<ID3D11ShaderResourceView> m_outputView;
	
	Vec2i m_uiSize = Vec2i(0);
	ComPtr<ID3D11Texture2D> m_uiColor;
	ComPtr<ID3D11RenderTargetView> m_uiColorTarget;
	ComPtr<ID3D11ShaderResourceView> m_uiColorView;
	ComPtr<ID3D11Texture2D> m_uiDepth;
	ComPtr<ID3D11DepthStencilView> m_uiDepthTarget;
	
};

bool VRGraphicsD3D11::checkRequirements(XrInstance instance, XrSystemId system) {
	
	// Must be called before xrCreateSession, the device is created on the adapter of the headset
	PFN_xrGetD3D11GraphicsRequirementsKHR getRequirements = nullptr;
	if(!checkXrResult(instance, xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR",
	                                                  reinterpret_cast<PFN_xrVoidFunction *>(&getRequirements)),
	                  "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)")) {
		return false;
	}
	XrGraphicsRequirementsD3D11KHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
	if(!checkXrResult(instance, getRequirements(instance, system, &requirements),
	                  "xrGetD3D11GraphicsRequirementsKHR")) {
		return false;
	}
	
	if(requirements.minFeatureLevel > D3D_FEATURE_LEVEL_11_1) {
		LogError << "OpenXR: runtime requires Direct3D feature level 0x" << std::hex << int(requirements.minFeatureLevel);
		return false;
	}
	
	unsigned long long luid = (unsigned long long)(u32(requirements.adapterLuid.LowPart))
	                          | ((unsigned long long)(u32(requirements.adapterLuid.HighPart)) << 32);
	d3d11::setRequiredAdapter(luid);
	
	return true;
}

const void * VRGraphicsD3D11::getSessionBinding(XrInstance instance, XrSystemId system) {
	
	ARX_UNUSED(instance);
	ARX_UNUSED(system);
	
	D3D11Renderer * renderer = getD3D11Renderer();
	if(!renderer || !renderer->device()) {
		LogError << "OpenXR: no Direct3D 11 device";
		return nullptr;
	}
	
	m_binding = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
	m_binding.device = renderer->device();
	
	return &m_binding;
}

s64 VRGraphicsD3D11::selectColorFormat(const std::vector<s64> & formats) const {
	
	/*
	 * The game renders gamma-encoded colors. Copying them unchanged into an sRGB image makes the compositor decode
	 * them correctly. Only formats that the RGBA8 images of the renderer can be copied into.
	 */
	const s64 preferred[] = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM };
	for(s64 format : preferred) {
		if(std::find(formats.begin(), formats.end(), format) != formats.end()) {
			return format;
		}
	}
	
	return 0;
}

bool VRGraphicsD3D11::enumerateImages(XrSwapchain swapchain, std::vector<u64> & images) {
	
	uint32_t imageCount = 0;
	xrEnumerateSwapchainImages(swapchain, 0, &imageCount, nullptr);
	std::vector<XrSwapchainImageD3D11KHR> d3dImages(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
	if(XR_FAILED(xrEnumerateSwapchainImages(swapchain, imageCount, &imageCount,
	                                        reinterpret_cast<XrSwapchainImageBaseHeader *>(d3dImages.data())))) {
		LogError << "OpenXR: xrEnumerateSwapchainImages failed";
		return false;
	}
	
	images.clear();
	for(const XrSwapchainImageD3D11KHR & image : d3dImages) {
		images.push_back(u64(reinterpret_cast<uintptr_t>(image.texture)));
	}
	
	return true;
}

bool VRGraphicsD3D11::createStereoTarget(const Vec2i & eyeSize) {
	
	// The renderer creates its stereo scene targets when the scene is drawn
	m_eyeSize = eyeSize;
	
	LogInfo << "VR stereo render target: " << (m_eyeSize.x * 2) << 'x' << m_eyeSize.y << ", Direct3D 11";
	
	return true;
}

void VRGraphicsD3D11::destroyStereoTarget() {
	m_outputView.Reset();
	m_output.Reset();
}

bool VRGraphicsD3D11::beginStereo(const std::vector<StereoView> & views) {
	
	D3D11Renderer * renderer = getD3D11Renderer();
	if(!renderer || m_eyeSize.x <= 0 || m_eyeSize.y <= 0) {
		return false;
	}
	
	renderer->setStereoViews(views, Vec2i(m_eyeSize.x * 2, m_eyeSize.y));
	
	return true;
}

bool VRGraphicsD3D11::endStereo(bool uiActive) {
	
	ARX_UNUSED(uiActive);
	
	D3D11Renderer * renderer = getD3D11Renderer();
	ID3D11Texture2D * output = renderer->finishStereoScene();
	renderer->setStereoViews({ }, Vec2i(0));
	
	if(output != m_output.Get()) {
		m_outputView.Reset();
		m_output.Reset();
		if(output) {
			// Keep it alive, a new output of the renderer must not be mistaken for it
			output->AddRef();
			*&m_output = output;
		}
		if(m_output && FAILED(renderer->device()->CreateShaderResourceView(m_output.Get(), nullptr, &m_outputView))) {
			LogError << "Failed to create the Direct3D 11 view of the VR stereo output";
		}
	}
	
	return output != nullptr;
}

void VRGraphicsD3D11::copyEyeToImage(size_t eye, u64 image) {
	
	if(!m_output || !image) {
		return;
	}
	
	UINT left = UINT(eye) * UINT(m_eyeSize.x);
	D3D11_BOX box = { left, 0, 0, left + UINT(m_eyeSize.x), UINT(m_eyeSize.y), 1 };
	getD3D11Renderer()->context()->CopySubresourceRegion(toTexture(image), 0, 0, 0, 0, m_output.Get(), 0, &box);
	
}

void VRGraphicsD3D11::drawEyeToWindow(const Vec2i & windowSize) {
	
	D3D11Renderer * renderer = getD3D11Renderer();
	if(!m_outputView || !renderer->windowView()) {
		return;
	}
	
	Vec2f crop(m_eyeSize);
	if(m_eyeSize.x * windowSize.y > windowSize.x * m_eyeSize.y) {
		crop.x = float(m_eyeSize.y) * float(windowSize.x) / float(windowSize.y);
	} else {
		crop.y = float(m_eyeSize.x) * float(windowSize.y) / float(windowSize.x);
	}
	Vec2f offset = (Vec2f(m_eyeSize) - crop) * 0.5f;
	
	// The left half of the output
	Vec2f size(float(m_eyeSize.x * 2), float(m_eyeSize.y));
	Vec2f uvMin = offset / size;
	Vec2f uvMax = (offset + crop) / size;
	const D3D11Renderer::OverlayVertex quad[4] = {
		{ { -1.f, -1.f, 0.f, 1.f }, { uvMin.x, uvMax.y } },
		{ { 1.f, -1.f, 0.f, 1.f }, { uvMax.x, uvMax.y } },
		{ { -1.f, 1.f, 0.f, 1.f }, { uvMin.x, uvMin.y } },
		{ { 1.f, 1.f, 0.f, 1.f }, { uvMax.x, uvMin.y } },
	};
	renderer->drawOverlay(renderer->windowView(), renderer->windowSize(), m_outputView.Get(), quad, 4, false);
	
}

bool VRGraphicsD3D11::createUITarget(const Vec2i & size) {
	
	destroyUITarget();
	
	ID3D11Device * device = getD3D11Renderer()->device();
	
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = UINT(size.x);
	desc.Height = UINT(size.y);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	if(FAILED(device->CreateTexture2D(&desc, nullptr, &m_uiColor))
	   || FAILED(device->CreateRenderTargetView(m_uiColor.Get(), nullptr, &m_uiColorTarget))
	   || FAILED(device->CreateShaderResourceView(m_uiColor.Get(), nullptr, &m_uiColorView))) {
		LogError << "OpenXR: could not create the interface render target";
		destroyUITarget();
		return false;
	}
	
	// The interface draws the 3D player model in the book, so it needs a depth buffer
	desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	if(FAILED(device->CreateTexture2D(&desc, nullptr, &m_uiDepth))
	   || FAILED(device->CreateDepthStencilView(m_uiDepth.Get(), nullptr, &m_uiDepthTarget))) {
		LogError << "OpenXR: could not create the interface depth target";
		destroyUITarget();
		return false;
	}
	
	m_uiSize = size;
	
	return true;
}

void VRGraphicsD3D11::destroyUITarget() {
	
	if(D3D11Renderer * renderer = getD3D11Renderer()) {
		renderer->setOutputTarget(nullptr, nullptr);
	}
	
	m_uiDepthTarget.Reset();
	m_uiDepth.Reset();
	m_uiColorView.Reset();
	m_uiColorTarget.Reset();
	m_uiColor.Reset();
	m_uiSize = Vec2i(0);
	
}

void VRGraphicsD3D11::beginUI() {
	
	D3D11Renderer * renderer = getD3D11Renderer();
	renderer->setOutputTarget(m_uiColorTarget.Get(), m_uiDepthTarget.Get());
	renderer->setPremultipliedAlphaOutput(true);
	renderer->Clear(Renderer::ColorBuffer | Renderer::DepthBuffer, Color(0, 0, 0, 0));
	
}

void VRGraphicsD3D11::endUI() {
	
	D3D11Renderer * renderer = getD3D11Renderer();
	renderer->setPremultipliedAlphaOutput(false);
	renderer->setOutputTarget(nullptr, nullptr);
	
}

void VRGraphicsD3D11::setPremultipliedAlphaOutput(bool enable) {
	getD3D11Renderer()->setPremultipliedAlphaOutput(enable);
}

void VRGraphicsD3D11::copyUIToImage(u64 image) {
	
	if(m_uiColor && image) {
		getD3D11Renderer()->context()->CopyResource(toTexture(image), m_uiColor.Get());
	}
	
}

bool VRGraphicsD3D11::copyUIToTexture(Texture * texture, const Rect & area) {
	
	ID3D11Texture2D * target = texture ? static_cast<D3D11Texture *>(texture)->texture() : nullptr;
	if(!m_uiColor || !target || area.empty()) {
		return false;
	}
	
	D3D11_BOX box = { UINT(area.left), UINT(area.top), 0, UINT(area.right), UINT(area.bottom), 1 };
	getD3D11Renderer()->context()->CopySubresourceRegion(target, 0, 0, 0, 0, m_uiColor.Get(), 0, &box);
	
	return true;
}

void VRGraphicsD3D11::copyWindowToImage(u64 image, const Vec2i & windowSize) {
	
	D3D11Renderer * renderer = getD3D11Renderer();
	ID3D11Texture2D * window = renderer->windowTexture();
	if(!window || !image || !m_uiColor || windowSize != m_uiSize || renderer->windowSize() != m_uiSize) {
		return;
	}
	
	D3D11_TEXTURE2D_DESC desc;
	window->GetDesc(&desc);
	if(desc.SampleDesc.Count > 1) {
		// Resolved through the interface layer, which is not used in this frame
		renderer->context()->ResolveSubresource(m_uiColor.Get(), 0, window, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
		renderer->context()->CopyResource(toTexture(image), m_uiColor.Get());
	} else {
		renderer->context()->CopyResource(toTexture(image), window);
	}
	
}

void VRGraphicsD3D11::drawUIToWindow(const Vec2i & windowSize, bool clear,
                                     const std::vector<VRWindowQuad> & quads) {
	
	D3D11Renderer * renderer = getD3D11Renderer();
	ID3D11RenderTargetView * window = renderer->windowView();
	if(!window || !m_uiColorView) {
		return;
	}
	
	if(clear) {
		const float black[4] = { 0.f, 0.f, 0.f, 1.f };
		renderer->context()->ClearRenderTargetView(window, black);
	}
	
	for(const VRWindowQuad & quad : quads) {
		D3D11Renderer::OverlayVertex vertices[4];
		for(size_t i = 0; i < 4; i++) {
			vertices[i] = { { quad.position[i].x, quad.position[i].y, quad.position[i].z, quad.position[i].w },
			                { quad.uv[i].x, quad.uv[i].y } };
		}
		renderer->drawOverlay(window, windowSize, m_uiColorView.Get(), vertices, 4, true);
	}
	
}
	
} // anonymous namespace

std::unique_ptr<VRGraphics> createVRGraphicsD3D11() {
	return std::make_unique<VRGraphicsD3D11>();
}
