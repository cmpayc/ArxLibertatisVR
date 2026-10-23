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
#include <cstdint>

#include "graphics/Renderer.h"
#include "graphics/vulkan/VulkanExternalDevice.h"
#include "graphics/vulkan/VulkanRenderer.h"
#include "graphics/vulkan/VulkanWindow.h"
#include "io/log/Logger.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>

namespace {

VulkanRenderer * getVulkanRenderer() {
	// The window falls back to another renderer if the Vulkan device cannot be created
	return vulkan::isVulkan(GRenderer) ? static_cast<VulkanRenderer *>(GRenderer) : nullptr;
}

static_assert(sizeof(VkImage) == sizeof(u64), "Vulkan images are passed as u64");

VkImage toImage(u64 image) {
	return reinterpret_cast<VkImage>(static_cast<uintptr_t>(image));
}

//! XrVersion (16 bits major, 16 bits minor, 32 bits patch) to a Vulkan version without the patch
uint32_t toVulkanVersion(XrVersion version) {
	return VK_MAKE_API_VERSION(0, uint32_t(XR_VERSION_MAJOR(version)), uint32_t(XR_VERSION_MINOR(version)), 0);
}

/*!
 * OpenXR with the Vulkan renderer (XR_KHR_vulkan_enable2): the runtime creates the Vulkan instance and device of the
 * renderer, which draws the eyes side by side into its scene targets with all effects, like the Direct3D 11 backend.
 * The result is copied into the eye images.
 */
class VRGraphicsVulkan final : public VRGraphics, public vulkan::ExternalDevice {

public:

	~VRGraphicsVulkan() override {
		vulkan::setExternalDevice(nullptr);
	}

	const char * getName() const override { return "Vulkan"; }
	const char * getExtension() const override { return XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME; }
	bool isDirect3D11() const override { return false; }
	bool isVulkan() const override { return true; }

	bool checkRequirements(XrInstance instance, XrSystemId system) override;
	const void * getSessionBinding(XrInstance instance, XrSystemId system) override;

	s64 selectColorFormat(const std::vector<s64> & formats) const override;
	bool enumerateImages(XrSwapchain swapchain, std::vector<u64> & images) override;
	// Vulkan images start at the top
	bool areImageRectsFromTop(bool metaRuntime) const override { ARX_UNUSED(metaRuntime); return true; }
	bool isPanelCopyFlipped() const override { return false; }

	bool createStereoTarget(const Vec2i & eyeSize) override;
	void destroyStereoTarget() override { }
	bool beginStereo(const std::vector<StereoView> & views) override;
	bool endStereo(bool uiActive) override;
	void copyEyeToImage(size_t eye, u64 image) override;
	void drawEyeToWindow(const Vec2i & windowSize) override;

	bool createUITarget(const Vec2i & size) override;
	void destroyUITarget() override;
	bool hasUITarget() const override;
	void beginUI() override;
	void endUI() override;
	void setPremultipliedAlphaOutput(bool enable) override;
	void copyUIToImage(u64 image) override;
	bool copyUIToTexture(Texture * texture, const Rect & area) override;
	void copyWindowToImage(u64 image, const Vec2i & windowSize) override;
	void drawUIToWindow(const Vec2i & windowSize, bool clear, const std::vector<VRWindowQuad> & quads) override;

	// vulkan::ExternalDevice
	uint32_t getMaxApiVersion() const override { return m_maxApiVersion; }
	VkResult createInstance(PFN_vkGetInstanceProcAddr getInstanceProcAddr, const VkInstanceCreateInfo & info,
	                        VkInstance & instance) override;
	VkPhysicalDevice getPhysicalDevice(VkInstance instance) override;
	VkResult createDevice(PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkPhysicalDevice physicalDevice,
	                      const VkDeviceCreateInfo & info, VkDevice & device) override;

private:

	bool loadFunctions(XrInstance instance);

	XrInstance m_instance = XR_NULL_HANDLE;
	XrSystemId m_system = XR_NULL_SYSTEM_ID;
	PFN_xrGetVulkanGraphicsRequirements2KHR m_getRequirements = nullptr;
	PFN_xrCreateVulkanInstanceKHR m_createInstance = nullptr;
	PFN_xrGetVulkanGraphicsDevice2KHR m_getDevice = nullptr;
	PFN_xrCreateVulkanDeviceKHR m_createDevice = nullptr;
	uint32_t m_maxApiVersion = VK_API_VERSION_1_3;

	XrGraphicsBindingVulkan2KHR m_binding = { XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR };

	Vec2i m_eyeSize = Vec2i(0);

};

bool VRGraphicsVulkan::loadFunctions(XrInstance instance) {

	if(instance == m_instance && m_getRequirements) {
		return true;
	}

	auto load = [instance](const char * name, auto & function) {
		function = nullptr;
		return checkXrResult(instance, xrGetInstanceProcAddr(instance, name,
		                                                     reinterpret_cast<PFN_xrVoidFunction *>(&function)),
		                     name) && function;
	};
	if(!load("xrGetVulkanGraphicsRequirements2KHR", m_getRequirements)
	   || !load("xrCreateVulkanInstanceKHR", m_createInstance)
	   || !load("xrGetVulkanGraphicsDevice2KHR", m_getDevice)
	   || !load("xrCreateVulkanDeviceKHR", m_createDevice)) {
		m_getRequirements = nullptr;
		return false;
	}

	return true;
}

bool VRGraphicsVulkan::checkRequirements(XrInstance instance, XrSystemId system) {

	// Must be called before xrCreateSession, and before the renderer creates its device
	if(!loadFunctions(instance)) {
		return false;
	}

	XrGraphicsRequirementsVulkan2KHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
	if(!checkXrResult(instance, m_getRequirements(instance, system, &requirements),
	                  "xrGetVulkanGraphicsRequirements2KHR")) {
		return false;
	}

	uint32_t minVersion = toVulkanVersion(requirements.minApiVersionSupported);
	uint32_t maxVersion = toVulkanVersion(requirements.maxApiVersionSupported);
	if(minVersion > VK_API_VERSION_1_3) {
		LogError << "OpenXR: runtime requires Vulkan " << VK_API_VERSION_MAJOR(minVersion) << '.'
		         << VK_API_VERSION_MINOR(minVersion);
		return false;
	}
	// Newer versions than the runtime was tested with are allowed, but not needed
	m_maxApiVersion = std::clamp(maxVersion, uint32_t(VK_API_VERSION_1_1), uint32_t(VK_API_VERSION_1_3));

	m_instance = instance;
	m_system = system;

	VulkanRenderer * renderer = getVulkanRenderer();
	if(!renderer) {
		// The renderer is created next, through the runtime
		vulkan::setExternalDevice(this);
		return true;
	}

	// Again before each session: the device must still be the one of the runtime
	if(renderer->hasExternalDevice()) {
		VkPhysicalDevice device = getPhysicalDevice(renderer->instance());
		if(device != renderer->physicalDevice()) {
			LogWarning << "OpenXR: the runtime now requires another Vulkan device - restart the game";
		}
	}

	return true;
}

VkResult VRGraphicsVulkan::createInstance(PFN_vkGetInstanceProcAddr getInstanceProcAddr,
                                          const VkInstanceCreateInfo & info, VkInstance & instance) {

	XrVulkanInstanceCreateInfoKHR createInfo = { XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR };
	createInfo.systemId = m_system;
	createInfo.pfnGetInstanceProcAddr = getInstanceProcAddr;
	createInfo.vulkanCreateInfo = &info;

	VkResult result = VK_ERROR_INITIALIZATION_FAILED;
	if(!checkXrResult(m_instance, m_createInstance(m_instance, &createInfo, &instance, &result),
	                  "xrCreateVulkanInstanceKHR")) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	return result;
}

VkPhysicalDevice VRGraphicsVulkan::getPhysicalDevice(VkInstance instance) {

	XrVulkanGraphicsDeviceGetInfoKHR getInfo = { XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
	getInfo.systemId = m_system;
	getInfo.vulkanInstance = instance;

	VkPhysicalDevice device = VK_NULL_HANDLE;
	if(!checkXrResult(m_instance, m_getDevice(m_instance, &getInfo, &device), "xrGetVulkanGraphicsDevice2KHR")) {
		return VK_NULL_HANDLE;
	}

	return device;
}

VkResult VRGraphicsVulkan::createDevice(PFN_vkGetInstanceProcAddr getInstanceProcAddr, VkPhysicalDevice physicalDevice,
                                        const VkDeviceCreateInfo & info, VkDevice & device) {

	XrVulkanDeviceCreateInfoKHR createInfo = { XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR };
	createInfo.systemId = m_system;
	createInfo.pfnGetInstanceProcAddr = getInstanceProcAddr;
	createInfo.vulkanPhysicalDevice = physicalDevice;
	createInfo.vulkanCreateInfo = &info;

	VkResult result = VK_ERROR_INITIALIZATION_FAILED;
	if(!checkXrResult(m_instance, m_createDevice(m_instance, &createInfo, &device, &result),
	                  "xrCreateVulkanDeviceKHR")) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	return result;
}

const void * VRGraphicsVulkan::getSessionBinding(XrInstance instance, XrSystemId system) {

	ARX_UNUSED(instance);
	ARX_UNUSED(system);

	VulkanRenderer * renderer = getVulkanRenderer();
	if(!renderer || !renderer->device()) {
		LogError << "OpenXR: no Vulkan device";
		return nullptr;
	}
	if(!renderer->hasExternalDevice()) {
		LogError << "OpenXR: the Vulkan device was not created by the runtime (was the headset connected when the"
		         << " game started?) - restart the game with the headset connected";
		return nullptr;
	}

	m_binding = { XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR };
	m_binding.instance = renderer->instance();
	m_binding.physicalDevice = renderer->physicalDevice();
	m_binding.device = renderer->device();
	m_binding.queueFamilyIndex = renderer->queueFamily();
	m_binding.queueIndex = 0;

	return &m_binding;
}

s64 VRGraphicsVulkan::selectColorFormat(const std::vector<s64> & formats) const {

	VulkanRenderer * renderer = getVulkanRenderer();
	if(!renderer) {
		return 0;
	}

	/*
	 * The game renders gamma-encoded colors. Copying them unchanged into an sRGB image makes the compositor decode
	 * them correctly. The images are copied byte by byte, so the channel order must match the renderer.
	 */
	std::vector<s64> preferred;
	switch(renderer->colorFormat()) {
		case VK_FORMAT_B8G8R8A8_UNORM: preferred = { VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM }; break;
		case VK_FORMAT_R8G8B8A8_UNORM: preferred = { VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM }; break;
		default: preferred = { s64(renderer->colorFormat()) }; break;
	}
	for(s64 format : preferred) {
		if(std::find(formats.begin(), formats.end(), format) != formats.end()) {
			return format;
		}
	}

	return 0;
}

bool VRGraphicsVulkan::enumerateImages(XrSwapchain swapchain, std::vector<u64> & images) {

	uint32_t imageCount = 0;
	xrEnumerateSwapchainImages(swapchain, 0, &imageCount, nullptr);
	std::vector<XrSwapchainImageVulkan2KHR> vulkanImages(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR });
	if(XR_FAILED(xrEnumerateSwapchainImages(swapchain, imageCount, &imageCount,
	                                        reinterpret_cast<XrSwapchainImageBaseHeader *>(vulkanImages.data())))) {
		LogError << "OpenXR: xrEnumerateSwapchainImages failed";
		return false;
	}

	images.clear();
	for(const XrSwapchainImageVulkan2KHR & image : vulkanImages) {
		images.push_back(u64(reinterpret_cast<uintptr_t>(image.image)));
	}

	return true;
}

bool VRGraphicsVulkan::createStereoTarget(const Vec2i & eyeSize) {

	// The renderer creates its stereo scene targets when the scene is drawn
	m_eyeSize = eyeSize;

	LogInfo << "VR stereo render target: " << (m_eyeSize.x * 2) << 'x' << m_eyeSize.y << ", Vulkan";

	return true;
}

bool VRGraphicsVulkan::beginStereo(const std::vector<StereoView> & views) {

	VulkanRenderer * renderer = getVulkanRenderer();
	if(!renderer || m_eyeSize.x <= 0 || m_eyeSize.y <= 0) {
		return false;
	}

	renderer->setStereoViews(views, Vec2i(m_eyeSize.x * 2, m_eyeSize.y));

	return true;
}

bool VRGraphicsVulkan::endStereo(bool uiActive) {

	ARX_UNUSED(uiActive);

	VulkanRenderer * renderer = getVulkanRenderer();
	bool rendered = renderer->finishStereoScene();
	renderer->setStereoViews({ }, Vec2i(0));

	return rendered;
}

void VRGraphicsVulkan::copyEyeToImage(size_t eye, u64 image) {

	VulkanRenderer * renderer = getVulkanRenderer();
	renderer->copyStereoEyeToImage(eye, toImage(image));

	// The image is released next: the copy must be submitted before
	renderer->submitCommands();

}

void VRGraphicsVulkan::drawEyeToWindow(const Vec2i & windowSize) {

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
	const VulkanRenderer::OverlayVertex quad[4] = {
		{ { -1.f, -1.f, 0.f, 1.f }, { uvMin.x, uvMax.y } },
		{ { 1.f, -1.f, 0.f, 1.f }, { uvMax.x, uvMax.y } },
		{ { -1.f, 1.f, 0.f, 1.f }, { uvMin.x, uvMin.y } },
		{ { 1.f, 1.f, 0.f, 1.f }, { uvMax.x, uvMin.y } },
	};
	getVulkanRenderer()->drawOverlay(VulkanRenderer::StereoOutput, quad, 4, false);

}

bool VRGraphicsVulkan::createUITarget(const Vec2i & size) {
	VulkanRenderer * renderer = getVulkanRenderer();
	return renderer && renderer->createOutputTarget(size);
}

void VRGraphicsVulkan::destroyUITarget() {
	if(VulkanRenderer * renderer = getVulkanRenderer()) {
		renderer->destroyOutputTarget();
	}
}

bool VRGraphicsVulkan::hasUITarget() const {
	VulkanRenderer * renderer = getVulkanRenderer();
	return renderer && renderer->hasOutputTarget();
}

void VRGraphicsVulkan::beginUI() {

	VulkanRenderer * renderer = getVulkanRenderer();
	renderer->setOutputTargetActive(true);
	renderer->setPremultipliedAlphaOutput(true);
	renderer->Clear(Renderer::ColorBuffer | Renderer::DepthBuffer, Color(0, 0, 0, 0));

}

void VRGraphicsVulkan::endUI() {

	VulkanRenderer * renderer = getVulkanRenderer();
	renderer->setPremultipliedAlphaOutput(false);
	renderer->setOutputTargetActive(false);

}

void VRGraphicsVulkan::setPremultipliedAlphaOutput(bool enable) {
	getVulkanRenderer()->setPremultipliedAlphaOutput(enable);
}

void VRGraphicsVulkan::copyUIToImage(u64 image) {

	VulkanRenderer * renderer = getVulkanRenderer();
	renderer->copyOutputToImage(toImage(image));

	// The image is released next: the copy must be submitted before
	renderer->submitCommands();

}

bool VRGraphicsVulkan::copyUIToTexture(Texture * texture, const Rect & area) {
	return getVulkanRenderer()->copyOutputToTexture(texture, area);
}

void VRGraphicsVulkan::copyWindowToImage(u64 image, const Vec2i & windowSize) {

	ARX_UNUSED(windowSize);

	VulkanRenderer * renderer = getVulkanRenderer();
	renderer->copyWindowToImage(toImage(image));

	// The image is released next: the copy must be submitted before
	renderer->submitCommands();

}

void VRGraphicsVulkan::drawUIToWindow(const Vec2i & windowSize, bool clear,
                                      const std::vector<VRWindowQuad> & quads) {

	ARX_UNUSED(windowSize);

	VulkanRenderer * renderer = getVulkanRenderer();

	if(clear) {
		renderer->clearWindow();
	}

	for(const VRWindowQuad & quad : quads) {
		VulkanRenderer::OverlayVertex vertices[4];
		for(size_t i = 0; i < 4; i++) {
			vertices[i] = { { quad.position[i].x, quad.position[i].y, quad.position[i].z, quad.position[i].w },
			                { quad.uv[i].x, quad.uv[i].y } };
		}
		renderer->drawOverlay(VulkanRenderer::OutputTarget, vertices, 4, true);
	}

}

} // anonymous namespace

std::unique_ptr<VRGraphics> createVRGraphicsVulkan() {
	return std::make_unique<VRGraphicsVulkan>();
}
