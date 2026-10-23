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

#include "graphics/vulkan/VulkanRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

#include <SDL.h>
#include <SDL_vulkan.h>

#include <vulkan/vk_enum_string_helper.h>

#include "core/Application.h"
#include "core/Config.h"
#include "gui/Credits.h"
#include "graphics/Vertex.h"
#include "graphics/vulkan/VulkanTexture.h"
#include "graphics/vulkan/VulkanTextureStage.h"
#include "graphics/vulkan/VulkanVertexBuffer.h"
#include "graphics/vulkan/VulkanWindow.h"
#include "graphics/image/Image.h"
#include "io/fs/FilePath.h"
#include "io/log/Logger.h"
#include "platform/CrashHandler.h"
#include "window/RenderWindow.h"

namespace {

const size_t TextureStageCount = 3;

VulkanRenderer * g_vulkanRenderer = nullptr;

bool checkVk(VkResult result, const char * call) {
	if(result == VK_SUCCESS) {
		return true;
	}
	LogError << call << " failed: " << string_VkResult(result);
	return false;
}

bool hasExtension(const std::vector<VkExtensionProperties> & extensions, const char * name) {
	return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties & extension) {
		return std::strcmp(extension.extensionName, name) == 0;
	});
}

std::string versionString(uint32_t version) {
	std::ostringstream oss;
	oss << VK_API_VERSION_MAJOR(version) << '.' << VK_API_VERSION_MINOR(version) << '.' << VK_API_VERSION_PATCH(version);
	return oss.str();
}

//! Validation layers: in debug builds, or with the ARX_VULKAN_VALIDATION=1 environment variable
bool validationRequested() {
	#ifdef ARX_DEBUG
	const bool defaultValue = true;
	#else
	const bool defaultValue = false;
	#endif
	const char * value = std::getenv("ARX_VULKAN_VALIDATION");
	return value ? (*value != '\0' && *value != '0') : defaultValue;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT types,
                                             const VkDebugUtilsMessengerCallbackDataEXT * data, void * user) {
	ARX_UNUSED(types), ARX_UNUSED(user);
	const char * id = data->pMessageIdName ? data->pMessageIdName : "";
	/*
	 * psMain writes the scene targets (SV_Target1-3) also when they are not bound, these writes are discarded by the
	 * attachments without an image (see VulkanRenderer::beginRendering)
	 */
	if(std::strcmp(id, "Undefined-Value-ShaderOutputNotConsumed-DynamicRendering") == 0) {
		return VK_FALSE;
	}
	if(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
		LogError << "Vulkan: [" << id << "] " << data->pMessage;
	} else {
		LogWarning << "Vulkan: [" << id << "] " << data->pMessage;
	}
	return VK_FALSE;
}

} // anonymous namespace

VulkanRenderer::VulkanRenderer()
	: m_window(nullptr)
	, m_instance(VK_NULL_HANDLE)
	, m_debugMessenger(VK_NULL_HANDLE)
	, m_surface(VK_NULL_HANDLE)
	, m_physicalDevice(VK_NULL_HANDLE)
	, m_properties()
	, m_apiVersion(0)
	, m_dynamicRenderingExtension(false)
	, m_features()
	, m_queueFamily(0)
	, m_device(VK_NULL_HANDLE)
	, m_queue(VK_NULL_HANDLE)
	, m_allocator(VK_NULL_HANDLE)
	, m_cmdBeginRendering(nullptr)
	, m_cmdEndRendering(nullptr)
	, m_swapchain(VK_NULL_HANDLE)
	, m_swapchainExtent()
	, m_presentMode(VK_PRESENT_MODE_FIFO_KHR)
	, m_swapchainDirty(false)
	, m_colorFormat(VK_FORMAT_UNDEFINED)
	, m_depthFormat(VK_FORMAT_UNDEFINED)
	, m_msaaSamples(VK_SAMPLE_COUNT_1_BIT)
	, m_size(0)
	, m_sceneScale(1.f)
	, m_inScene(false)
	, m_sceneSize(0)
	, m_frameSerial(0)
	, m_frameIndex(0)
	, m_frameActive(false)
	, m_renderingActive(false)
	, m_deviceLost(false)
	, m_vsync(1)
	, m_hasMSAA(false)
	, m_wireframe(false)
	, m_maximumAnisotropy(1.f)
	, m_maximumSupportedAnisotropy(1.f)
	, m_view(1.f)
	, m_projection(1.f)
	, m_fogColor(Color4f::black)
	, m_fogStart(0.f)
	, m_fogEnd(0.f)
	, m_vertexShaders()
	, m_pixelShader(VK_NULL_HANDLE)
	, m_pixelShaderSampleShading(VK_NULL_HANDLE)
	, m_descriptorSetLayout(VK_NULL_HANDLE)
	, m_pipelineLayout(VK_NULL_HANDLE)
	, m_pipelineCache(VK_NULL_HANDLE)
	, m_blitVertexShader(VK_NULL_HANDLE)
	, m_blitPixelShader(VK_NULL_HANDLE)
	, m_blitSetLayout(VK_NULL_HANDLE)
	, m_blitPipelineLayout(VK_NULL_HANDLE)
	, m_blitPipeline(VK_NULL_HANDLE)
	, m_lightConstants(VK_NULL_HANDLE)
	, m_lightConstantsAllocation(VK_NULL_HANDLE)
	, m_boundPipeline(VK_NULL_HANDLE)
	, m_boundVertexBuffer(VK_NULL_HANDLE)
	, m_boundVertexOffset(0)
	, m_boundIndexBuffer(VK_NULL_HANDLE)
	, m_boundIndexOffset(0)
	, m_dynamicStateValid(false)
	, m_boundDepthBias(1.f)
	, m_boundDescriptorSet(VK_NULL_HANDLE)
	, m_boundConstantOffsets()
{
	g_vulkanRenderer = this;

	for(Frame & frame : m_frames) {
		frame.vertices.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		frame.vertices.chunkSize = VkDeviceSize(4) * 1024 * 1024;
		frame.indices.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		frame.indices.chunkSize = VkDeviceSize(1) * 1024 * 1024;
		frame.constants.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		frame.constants.chunkSize = VkDeviceSize(1) * 1024 * 1024;
	}
}

VulkanRenderer::~VulkanRenderer() {

	if(g_vulkanRenderer == this) {
		g_vulkanRenderer = nullptr;
	}

	if(isInitialized()) {
		shutdown();
	}

	// Textures can outlive the renderer, their images are released with the device
	for(VulkanTexture & texture : m_textures) {
		texture.releaseImage();
		texture.m_renderer = nullptr;
	}
	m_textures.clear();

	if(m_device) {

		if(m_upload.recording) {
			flushUploads();
		}

		vkDeviceWaitIdle(m_device);

		releaseSizeDependentResources();
		releaseSwapchain();
		clearSamplers();
		releasePipelineResources();

		for(Frame & frame : m_frames) {
			for(std::function<void()> & release : frame.releases) {
				release();
			}
			frame.releases.clear();
		}

		for(Frame & frame : m_frames) {
			releaseRing(frame.vertices);
			releaseRing(frame.indices);
			releaseRing(frame.constants);
			for(VkDescriptorPool pool : frame.descriptorPools) {
				vkDestroyDescriptorPool(m_device, pool, nullptr);
			}
			frame.descriptorPools.clear();
			vkDestroySemaphore(m_device, frame.imageAcquired, nullptr);
			vkDestroyFence(m_device, frame.fence, nullptr);
			vkDestroyCommandPool(m_device, frame.commandPool, nullptr);
		}
		vkDestroyFence(m_device, m_upload.fence, nullptr);
		vkDestroyCommandPool(m_device, m_upload.commandPool, nullptr);

		if(m_allocator) {
			vmaDestroyAllocator(m_allocator);
		}

		vkDestroyDevice(m_device, nullptr);
	}

	if(m_instance) {
		if(m_surface) {
			vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
		}
		if(m_debugMessenger) {
			vkDestroyDebugUtilsMessengerEXT(m_instance, m_debugMessenger, nullptr);
		}
		vkDestroyInstance(m_instance, nullptr);
	}

}

bool VulkanRenderer::create(SDL_Window * window, int maxMSAALevel) {

	m_window = window;

	// Debug snapshot of the frame after a number of seconds, for testing without capturing the screen
	if(const char * path = std::getenv("ARX_VULKAN_SNAPSHOT")) {
		m_snapshotPath = path;
		const char * time = std::getenv("ARX_VULKAN_SNAPSHOT_TIME");
		float seconds = time ? float(std::atof(time)) : 20.f;
		m_snapshotTime = std::chrono::steady_clock::now()
		                 + std::chrono::milliseconds(static_cast<long long>(double(seconds) * 1000.0));
	}

	if(!createInstance(window) || !selectPhysicalDevice() || !createDevice() || !createFrames()) {
		return false;
	}

	// Multisampling of the color + depth target
	VkSampleCountFlags counts = m_properties.limits.framebufferColorSampleCounts
	                            & m_properties.limits.framebufferDepthSampleCounts;
	m_msaaSamples = VK_SAMPLE_COUNT_1_BIT;
	for(VkSampleCountFlagBits samples : { VK_SAMPLE_COUNT_8_BIT, VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_2_BIT }) {
		if(int(samples) <= maxMSAALevel && (counts & samples)) {
			m_msaaSamples = samples;
			break;
		}
	}

	// Depth with stencil for the scene effects
	for(VkFormat format : { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D16_UNORM_S8_UINT }) {
		VkFormatProperties properties;
		vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &properties);
		if(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
			m_depthFormat = format;
			break;
		}
	}
	if(m_depthFormat == VK_FORMAT_UNDEFINED) {
		LogError << "No supported Vulkan depth/stencil format";
		return false;
	}

	if(!createSwapchain() || !createPipelineResources()) {
		return false;
	}

	return true;
}

bool VulkanRenderer::createInstance(SDL_Window * window) {

	// Use the Vulkan loader that SDL loaded for the window
	PFN_vkGetInstanceProcAddr getInstanceProcAddr
		= reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
	if(!getInstanceProcAddr) {
		LogError << "Vulkan loader not available: " << SDL_GetError();
		return false;
	}
	volkInitializeCustom(getInstanceProcAddr);

	uint32_t instanceVersion = volkGetInstanceVersion();
	if(instanceVersion < VK_API_VERSION_1_1) {
		LogError << "Vulkan 1.1 not supported, the loader supports " << versionString(instanceVersion);
		return false;
	}
	m_apiVersion = std::min(instanceVersion, uint32_t(VK_API_VERSION_1_3));

	unsigned int count = 0;
	if(!SDL_Vulkan_GetInstanceExtensions(window, &count, nullptr)) {
		LogError << "Could not get the Vulkan surface extensions: " << SDL_GetError();
		return false;
	}
	std::vector<const char *> extensions(count);
	if(!SDL_Vulkan_GetInstanceExtensions(window, &count, extensions.data())) {
		LogError << "Could not get the Vulkan surface extensions: " << SDL_GetError();
		return false;
	}

	uint32_t available = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &available, nullptr);
	std::vector<VkExtensionProperties> availableExtensions(available);
	vkEnumerateInstanceExtensionProperties(nullptr, &available, availableExtensions.data());

	std::vector<const char *> layers;
	bool debugUtils = false;
	if(validationRequested()) {
		uint32_t layerCount = 0;
		vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
		std::vector<VkLayerProperties> availableLayers(layerCount);
		vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());
		const char * validation = "VK_LAYER_KHRONOS_validation";
		if(std::any_of(availableLayers.begin(), availableLayers.end(), [validation](const VkLayerProperties & layer) {
			return std::strcmp(layer.layerName, validation) == 0;
		})) {
			layers.push_back(validation);
			LogInfo << "Vulkan validation enabled";
		} else {
			LogWarning << "Vulkan validation layer not available";
		}
		if(hasExtension(availableExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
			extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			debugUtils = true;
		}
	}

	VkApplicationInfo application = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	application.pApplicationName = "Arx Libertatis";
	application.pEngineName = "Arx Libertatis";
	application.apiVersion = m_apiVersion;

	VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	info.pApplicationInfo = &application;
	info.enabledExtensionCount = uint32_t(extensions.size());
	info.ppEnabledExtensionNames = extensions.data();
	info.enabledLayerCount = uint32_t(layers.size());
	info.ppEnabledLayerNames = layers.data();

	if(!checkVk(vkCreateInstance(&info, nullptr, &m_instance), "vkCreateInstance")) {
		return false;
	}
	volkLoadInstance(m_instance);

	if(debugUtils) {
		VkDebugUtilsMessengerCreateInfoEXT messenger = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
		messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
		                            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
		                        | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
		                        | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		messenger.pfnUserCallback = debugCallback;
		checkVk(vkCreateDebugUtilsMessengerEXT(m_instance, &messenger, nullptr, &m_debugMessenger),
		        "vkCreateDebugUtilsMessengerEXT");
	}

	if(!SDL_Vulkan_CreateSurface(window, m_instance, &m_surface)) {
		LogError << "Could not create the Vulkan surface: " << SDL_GetError();
		return false;
	}

	return true;
}

bool VulkanRenderer::selectPhysicalDevice() {

	uint32_t count = 0;
	vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
	std::vector<VkPhysicalDevice> devices(count);
	vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

	int bestScore = 0;

	for(VkPhysicalDevice device : devices) {

		VkPhysicalDeviceProperties properties;
		vkGetPhysicalDeviceProperties(device, &properties);

		if(properties.apiVersion < VK_API_VERSION_1_1) {
			LogInfo << "Ignoring Vulkan device " << properties.deviceName << ": Vulkan 1.1 not supported";
			continue;
		}

		uint32_t extensionCount = 0;
		vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
		std::vector<VkExtensionProperties> extensions(extensionCount);
		vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());

		if(!hasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
			continue;
		}

		// Dynamic rendering: core in Vulkan 1.3, or the extension
		bool core13 = std::min(properties.apiVersion, m_apiVersion) >= VK_API_VERSION_1_3;
		bool extension = hasExtension(extensions, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)
		                 && hasExtension(extensions, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME)
		                 && hasExtension(extensions, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
		VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRendering
			= { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR };
		VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
		features.pNext = &dynamicRendering;
		if(core13 || extension) {
			vkGetPhysicalDeviceFeatures2(device, &features);
		}
		if(!dynamicRendering.dynamicRendering) {
			LogInfo << "Ignoring Vulkan device " << properties.deviceName << ": dynamic rendering not supported";
			continue;
		}

		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
		uint32_t family = familyCount;
		for(uint32_t i = 0; i < familyCount; i++) {
			VkBool32 present = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &present);
			if((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
				family = i;
				break;
			}
		}
		if(family == familyCount) {
			continue;
		}

		int score = 1;
		if(properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
			score = 3;
		} else if(properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
			score = 2;
		}
		if(score > bestScore) {
			bestScore = score;
			m_physicalDevice = device;
			m_properties = properties;
			m_queueFamily = family;
			m_dynamicRenderingExtension = !core13;
		}

	}

	if(!m_physicalDevice) {
		LogError << "No usable Vulkan device found";
		return false;
	}

	m_apiVersion = std::min(m_apiVersion, m_properties.apiVersion);

	return true;
}

bool VulkanRenderer::createDevice() {

	float priority = 1.f;
	VkDeviceQueueCreateInfo queue = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	queue.queueFamilyIndex = m_queueFamily;
	queue.queueCount = 1;
	queue.pQueuePriorities = &priority;

	std::vector<const char *> extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	if(m_dynamicRenderingExtension) {
		extensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
		extensions.push_back(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
		extensions.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
	}

	// Optional features used by the effects of the Direct3D 11 renderer
	VkPhysicalDeviceFeatures supported;
	vkGetPhysicalDeviceFeatures(m_physicalDevice, &supported);
	m_features = VkPhysicalDeviceFeatures();
	m_features.samplerAnisotropy = supported.samplerAnisotropy;
	m_features.fillModeNonSolid = supported.fillModeNonSolid;
	m_features.sampleRateShading = supported.sampleRateShading;
	m_features.independentBlend = supported.independentBlend;
	m_features.imageCubeArray = supported.imageCubeArray;
	m_features.tessellationShader = supported.tessellationShader;

	VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRendering
		= { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR };
	dynamicRendering.dynamicRendering = VK_TRUE;
	VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	features.pNext = &dynamicRendering;
	features.features = m_features;

	VkDeviceCreateInfo info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	info.pNext = &features;
	info.queueCreateInfoCount = 1;
	info.pQueueCreateInfos = &queue;
	info.enabledExtensionCount = uint32_t(extensions.size());
	info.ppEnabledExtensionNames = extensions.data();

	if(!checkVk(vkCreateDevice(m_physicalDevice, &info, nullptr, &m_device), "vkCreateDevice")) {
		return false;
	}
	volkLoadDevice(m_device);

	vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);

	if(m_dynamicRenderingExtension) {
		m_cmdBeginRendering = vkCmdBeginRenderingKHR;
		m_cmdEndRendering = vkCmdEndRenderingKHR;
	} else {
		m_cmdBeginRendering = vkCmdBeginRendering;
		m_cmdEndRendering = vkCmdEndRendering;
	}
	if(!m_cmdBeginRendering || !m_cmdEndRendering) {
		LogError << "Vulkan dynamic rendering functions not available";
		return false;
	}

	VmaVulkanFunctions functions = {};
	functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
	functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
	VmaAllocatorCreateInfo allocator = {};
	allocator.vulkanApiVersion = m_apiVersion;
	allocator.physicalDevice = m_physicalDevice;
	allocator.device = m_device;
	allocator.instance = m_instance;
	allocator.pVulkanFunctions = &functions;
	if(!checkVk(vmaCreateAllocator(&allocator, &m_allocator), "vmaCreateAllocator")) {
		return false;
	}

	return true;
}

bool VulkanRenderer::createFrames() {

	for(Frame & frame : m_frames) {

		VkCommandPoolCreateInfo pool = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		pool.queueFamilyIndex = m_queueFamily;
		if(!checkVk(vkCreateCommandPool(m_device, &pool, nullptr, &frame.commandPool), "vkCreateCommandPool")) {
			return false;
		}

		VkCommandBufferAllocateInfo commands = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		commands.commandPool = frame.commandPool;
		commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		commands.commandBufferCount = 1;
		if(!checkVk(vkAllocateCommandBuffers(m_device, &commands, &frame.commands), "vkAllocateCommandBuffers")) {
			return false;
		}

		VkFenceCreateInfo fence = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		if(!checkVk(vkCreateFence(m_device, &fence, nullptr, &frame.fence), "vkCreateFence")) {
			return false;
		}

		VkSemaphoreCreateInfo semaphore = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		if(!checkVk(vkCreateSemaphore(m_device, &semaphore, nullptr, &frame.imageAcquired), "vkCreateSemaphore")) {
			return false;
		}

	}

	VkCommandPoolCreateInfo pool = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	pool.queueFamilyIndex = m_queueFamily;
	if(!checkVk(vkCreateCommandPool(m_device, &pool, nullptr, &m_upload.commandPool), "vkCreateCommandPool")) {
		return false;
	}

	VkCommandBufferAllocateInfo commands = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	commands.commandPool = m_upload.commandPool;
	commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commands.commandBufferCount = 1;
	if(!checkVk(vkAllocateCommandBuffers(m_device, &commands, &m_upload.commands), "vkAllocateCommandBuffers")) {
		return false;
	}

	VkFenceCreateInfo fence = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	if(!checkVk(vkCreateFence(m_device, &fence, nullptr, &m_upload.fence), "vkCreateFence")) {
		return false;
	}

	return true;
}

bool VulkanRenderer::createSwapchain() {

	VkSurfaceCapabilitiesKHR capabilities;
	if(!checkVk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &capabilities),
	            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
		return false;
	}

	VkExtent2D extent = capabilities.currentExtent;
	if(extent.width == std::numeric_limits<uint32_t>::max()) {
		int width = 0, height = 0;
		SDL_Vulkan_GetDrawableSize(m_window, &width, &height);
		extent.width = std::clamp(uint32_t(std::max(width, 1)), capabilities.minImageExtent.width,
		                          capabilities.maxImageExtent.width);
		extent.height = std::clamp(uint32_t(std::max(height, 1)), capabilities.minImageExtent.height,
		                           capabilities.maxImageExtent.height);
	}
	if(extent.width == 0 || extent.height == 0) {
		// Minimized - try again later
		m_swapchainDirty = true;
		return true;
	}

	if(!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
		LogError << "Vulkan swapchain images cannot be copied to";
		return false;
	}

	// The game renders gamma-encoded colors like the other backends
	uint32_t formatCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, nullptr);
	std::vector<VkSurfaceFormatKHR> formats(formatCount);
	vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, formats.data());
	if(formats.empty()) {
		LogError << "No Vulkan surface formats";
		return false;
	}
	VkSurfaceFormatKHR format = formats[0];
	for(VkFormat preferred : { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A8B8G8R8_UNORM_PACK32 }) {
		auto it = std::find_if(formats.begin(), formats.end(), [preferred](const VkSurfaceFormatKHR & candidate) {
			return candidate.format == preferred && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		});
		if(it != formats.end()) {
			format = *it;
			break;
		}
	}

	uint32_t modeCount = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &modeCount, nullptr);
	std::vector<VkPresentModeKHR> modes(modeCount);
	vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &modeCount, modes.data());
	auto hasMode = [&modes](VkPresentModeKHR mode) {
		return std::find(modes.begin(), modes.end(), mode) != modes.end();
	};
	m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
	if(m_vsync == 0) {
		if(hasMode(VK_PRESENT_MODE_MAILBOX_KHR)) {
			m_presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
		} else if(hasMode(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
			m_presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
		}
	} else if(m_vsync < 0 && hasMode(VK_PRESENT_MODE_FIFO_RELAXED_KHR)) {
		m_presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
	}

	uint32_t imageCount = std::max(capabilities.minImageCount + 1, uint32_t(2));
	if(capabilities.maxImageCount != 0) {
		imageCount = std::min(imageCount, capabilities.maxImageCount);
	}

	VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if(!(capabilities.supportedCompositeAlpha & compositeAlpha)) {
		for(VkCompositeAlphaFlagBitsKHR alpha : { VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
		                                          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
		                                          VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR }) {
			if(capabilities.supportedCompositeAlpha & alpha) {
				compositeAlpha = alpha;
				break;
			}
		}
	}

	VkSwapchainKHR oldSwapchain = m_swapchain;

	VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	info.surface = m_surface;
	info.minImageCount = imageCount;
	info.imageFormat = format.format;
	info.imageColorSpace = format.colorSpace;
	info.imageExtent = extent;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = capabilities.currentTransform;
	info.compositeAlpha = compositeAlpha;
	info.presentMode = m_presentMode;
	info.clipped = VK_TRUE;
	info.oldSwapchain = oldSwapchain;

	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	if(!checkVk(vkCreateSwapchainKHR(m_device, &info, nullptr, &swapchain), "vkCreateSwapchainKHR")) {
		return false;
	}

	releaseSwapchain();
	m_swapchain = swapchain;
	m_swapchainExtent = extent;
	m_swapchainDirty = false;

	uint32_t count = 0;
	vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, nullptr);
	m_swapchainImages.resize(count);
	vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, m_swapchainImages.data());

	m_renderFinished.resize(count);
	for(VkSemaphore & semaphore : m_renderFinished) {
		VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		if(!checkVk(vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &semaphore), "vkCreateSemaphore")) {
			return false;
		}
	}

	// The offscreen target uses the same format so that it can be resolved / copied directly
	if(m_colorFormat != format.format) {
		bool hadTargets = (m_colorTarget.image != VK_NULL_HANDLE);
		if(hadTargets) {
			releaseSizeDependentResources();
		}
		m_colorFormat = format.format;
		if(hadTargets) {
			createSizeDependentResources();
		}
	}

	return true;
}

void VulkanRenderer::releaseSwapchain() {

	if(!m_swapchain) {
		return;
	}

	// The swapchain images may still be used by submitted frames
	vkDeviceWaitIdle(m_device);

	for(VkSemaphore semaphore : m_renderFinished) {
		vkDestroySemaphore(m_device, semaphore, nullptr);
	}
	m_renderFinished.clear();
	m_swapchainImages.clear();

	vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
	m_swapchain = VK_NULL_HANDLE;

}

bool VulkanRenderer::createImageTarget(ImageTarget & target, VkFormat format, VkImageUsageFlags usage,
                                       VkImageAspectFlags aspect, const Vec2i & size, VkSampleCountFlagBits samples) {

	VkImageCreateInfo image = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = format;
	image.extent = { uint32_t(size.x), uint32_t(size.y), 1 };
	image.mipLevels = 1;
	image.arrayLayers = 1;
	image.samples = samples;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = usage;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo allocation = {};
	allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	allocation.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

	if(!checkVk(vmaCreateImage(m_allocator, &image, &allocation, &target.image, &target.allocation, nullptr),
	            "vmaCreateImage")) {
		return false;
	}

	VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	view.image = target.image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = format;
	view.subresourceRange = { aspect, 0, 1, 0, 1 };
	if(!checkVk(vkCreateImageView(m_device, &view, nullptr, &target.view), "vkCreateImageView")) {
		return false;
	}

	target.layout = VK_IMAGE_LAYOUT_UNDEFINED;

	return true;
}

void VulkanRenderer::releaseImageTarget(ImageTarget & target) {

	if(!target.image) {
		return;
	}

	VkDevice device = m_device;
	VmaAllocator allocator = m_allocator;
	VkImage image = target.image;
	VmaAllocation allocation = target.allocation;
	VkImageView view = target.view;
	deferRelease([device, allocator, image, allocation, view]() {
		vkDestroyImageView(device, view, nullptr);
		vmaDestroyImage(allocator, image, allocation);
	});

	target = ImageTarget();
}

bool VulkanRenderer::createSizeDependentResources() {

	Vec2i size = mainApp->getWindow()->getSize();
	m_size = Vec2i(std::max<s32>(size.x, 1), std::max<s32>(size.y, 1));

	if(m_colorFormat == VK_FORMAT_UNDEFINED) {
		return false;
	}

	if(!createImageTarget(m_colorTarget, m_colorFormat,
	                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	                      VK_IMAGE_ASPECT_COLOR_BIT, m_size, m_msaaSamples)) {
		LogError << "Failed to create " << m_size.x << "x" << m_size.y << " Vulkan color target";
		return false;
	}

	if(!createImageTarget(m_depthTarget, m_depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
	                      VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, m_size, m_msaaSamples)) {
		LogError << "Failed to create " << m_size.x << "x" << m_size.y << " Vulkan depth target";
		return false;
	}

	// The swapchain follows the window size
	if(m_swapchainExtent.width != uint32_t(m_size.x) || m_swapchainExtent.height != uint32_t(m_size.y)) {
		m_swapchainDirty = true;
	}

	return true;
}

void VulkanRenderer::releaseSizeDependentResources() {

	endRendering();

	releaseImageTarget(m_colorTarget);
	releaseImageTarget(m_depthTarget);

	releaseSceneTargets();

}

bool VulkanRenderer::createSceneTargets(const Vec2i & size) {

	releaseSceneTargets();

	// The multisampled scene is resolved at the end of rendering into an image that is sampled by the effects
	VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if(m_msaaSamples == VK_SAMPLE_COUNT_1_BIT) {
		usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	}
	if(!createImageTarget(m_sceneColor, m_colorFormat, usage, VK_IMAGE_ASPECT_COLOR_BIT, size, m_msaaSamples)
	   || !createImageTarget(m_sceneDepth, m_depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
	                         VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, size, m_msaaSamples)
	   || (m_msaaSamples != VK_SAMPLE_COUNT_1_BIT
	       && !createImageTarget(m_sceneResolved, m_colorFormat,
	                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	                             VK_IMAGE_ASPECT_COLOR_BIT, size, VK_SAMPLE_COUNT_1_BIT))) {
		LogError << "Failed to create " << size.x << "x" << size.y << " Vulkan scene targets";
		releaseSceneTargets();
		return false;
	}

	m_sceneSize = size;

	return true;
}

void VulkanRenderer::releaseSceneTargets() {

	if(m_inScene) {
		endRendering();
		m_inScene = false;
		invalidateBoundState();
	}

	releaseImageTarget(m_sceneColor);
	releaseImageTarget(m_sceneDepth);
	releaseImageTarget(m_sceneResolved);
	m_sceneSize = Vec2i(0);

}

Vec2f VulkanRenderer::targetScale() const {

	if(!m_inScene || m_size.x <= 0 || m_size.y <= 0) {
		return Vec2f(1.f);
	}

	return Vec2f(m_sceneSize) / Vec2f(m_size);
}

Rect VulkanRenderer::toTargetRect(const Rect & rect) const {

	Vec2f scale = targetScale();

	return Rect(s32(std::floor(float(rect.left) * scale.x + 0.5f)), s32(std::floor(float(rect.top) * scale.y + 0.5f)),
	            s32(std::floor(float(rect.right) * scale.x + 0.5f)),
	            s32(std::floor(float(rect.bottom) * scale.y + 0.5f)));
}

void VulkanRenderer::setSceneScale(float scale) {

	scale = std::clamp(scale, 0.25f, 1.f);
	if(scale == m_sceneScale) {
		return;
	}

	m_sceneScale = scale;

	if(!m_inScene) {
		releaseSceneTargets();
	}

}

void VulkanRenderer::beginScene() {

	if(m_inScene || !m_colorTarget.image) {
		return;
	}

	// TODO Phase 2: also for the effects
	if(m_sceneScale >= 1.f) {
		return;
	}

	Vec2i size(std::max<s32>(s32(float(m_size.x) * m_sceneScale + 0.5f), 1),
	           std::max<s32>(s32(float(m_size.y) * m_sceneScale + 0.5f), 1));
	if(size != m_sceneSize && !createSceneTargets(size)) {
		return;
	}

	endRendering();

	m_inScene = true;
	invalidateBoundState();

}

void VulkanRenderer::endScene() {

	if(!m_inScene) {
		return;
	}

	// Resolves the multisampled scene, see beginRendering()
	endRendering();

	m_inScene = false;
	invalidateBoundState();

	ImageTarget & scene = m_sceneResolved.image ? m_sceneResolved : m_sceneColor;
	transition(scene, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

	// Upscale the scene to the window
	composeScene(scene.view);

	// The interface is drawn on top of the scene, with its own depth
	Clear(DepthBuffer);

}

void VulkanRenderer::deferRelease(std::function<void()> release) {

	if(!m_device) {
		return;
	}

	// Not recording: the resources were last used by the previously submitted frame
	size_t index = m_frameActive ? m_frameIndex : (m_frameIndex + FramesInFlight - 1) % FramesInFlight;
	m_frames[index].releases.push_back(std::move(release));

}

void VulkanRenderer::beginFrame() {

	if(m_frameActive) {
		return;
	}

	Frame & frame = m_frames[m_frameIndex];

	vkWaitForFences(m_device, 1, &frame.fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
	vkResetFences(m_device, 1, &frame.fence);

	for(std::function<void()> & release : frame.releases) {
		release();
	}
	frame.releases.clear();

	vkResetCommandPool(m_device, frame.commandPool, 0);

	for(RingBuffer * ring : { &frame.vertices, &frame.indices, &frame.constants }) {
		ring->current = 0;
		ring->pos = 0;
	}
	for(VkDescriptorPool pool : frame.descriptorPools) {
		vkResetDescriptorPool(m_device, pool, 0);
	}
	frame.descriptorPool = 0;

	VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(frame.commands, &begin);

	invalidateBoundState();

	// Copies of vertex buffers made in earlier frames are gone with the ring buffer contents
	m_frameSerial++;

	m_frameActive = true;

}

void VulkanRenderer::endFrame(VkSemaphore wait, VkPipelineStageFlags waitStage, VkSemaphore signal) {

	arx_assert(m_frameActive);

	endRendering();

	// Textures used by this frame may have been uploaded after its commands were recorded
	if(m_upload.recording) {
		flushUploads();
	}

	Frame & frame = m_frames[m_frameIndex];

	flushRing(frame.vertices);
	flushRing(frame.indices);
	flushRing(frame.constants);

	vkEndCommandBuffer(frame.commands);

	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.waitSemaphoreCount = wait ? 1 : 0;
	submit.pWaitSemaphores = &wait;
	submit.pWaitDstStageMask = &waitStage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &frame.commands;
	submit.signalSemaphoreCount = signal ? 1 : 0;
	submit.pSignalSemaphores = &signal;

	VkResult result = vkQueueSubmit(m_queue, 1, &submit, frame.fence);
	if(result == VK_ERROR_DEVICE_LOST) {
		if(!m_deviceLost) {
			LogError << "Vulkan device lost";
			m_deviceLost = true;
		}
	} else {
		checkVk(result, "vkQueueSubmit");
	}

	m_frameActive = false;
	m_frameIndex = (m_frameIndex + 1) % FramesInFlight;

}

void VulkanRenderer::transition(ImageTarget & target, VkImageLayout layout, VkImageAspectFlags aspect) {

	if(target.layout == layout) {
		return;
	}

	// TODO use precise stages and access masks once the renderer is complete
	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.oldLayout = target.layout;
	barrier.newLayout = layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = target.image;
	barrier.subresourceRange = { aspect, 0, 1, 0, 1 };

	vkCmdPipelineBarrier(m_frames[m_frameIndex].commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

	target.layout = layout;

}

void VulkanRenderer::beginRendering() {

	if(m_renderingActive || !m_colorTarget.image) {
		return;
	}

	beginFrame();

	// The 3D scene has its own targets, see beginScene()
	ImageTarget & colorTarget = m_inScene ? m_sceneColor : m_colorTarget;
	ImageTarget & depthTarget = m_inScene ? m_sceneDepth : m_depthTarget;
	Vec2i size = m_inScene ? m_sceneSize : m_size;

	transition(colorTarget, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
	transition(depthTarget, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
	           VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
	bool resolve = m_inScene && m_sceneResolved.image;
	if(resolve) {
		transition(m_sceneResolved, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
	}

	/*
	 * psMain also writes the motion, surface and shading targets of the 3D scene (SV_Target1-3), these writes are
	 * discarded by attachments without an image
	 */
	VkRenderingAttachmentInfoKHR colors[4];
	for(VkRenderingAttachmentInfoKHR & attachment : colors) {
		attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
	}
	VkRenderingAttachmentInfoKHR & color = colors[0];
	color.imageView = colorTarget.view;
	color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	if(resolve) {
		// Resolving while rendering keeps the samples in tile memory on tiled GPUs
		color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
		color.resolveImageView = m_sceneResolved.view;
		color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	VkRenderingAttachmentInfoKHR depth = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
	depth.imageView = depthTarget.view;
	depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

	VkRenderingInfoKHR info = { VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
	info.renderArea = { { 0, 0 }, { uint32_t(size.x), uint32_t(size.y) } };
	info.layerCount = 1;
	info.colorAttachmentCount = uint32_t(std::size(colors));
	info.pColorAttachments = colors;
	info.pDepthAttachment = &depth;
	info.pStencilAttachment = &depth;

	m_cmdBeginRendering(m_frames[m_frameIndex].commands, &info);

	m_renderingActive = true;

}

void VulkanRenderer::endRendering() {

	if(!m_renderingActive) {
		return;
	}

	m_cmdEndRendering(m_frames[m_frameIndex].commands);

	m_renderingActive = false;

}

void VulkanRenderer::initialize() {

	arx_assert(m_device);

	const char * type = "other";
	switch(m_properties.deviceType) {
		case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: type = "discrete"; break;
		case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: type = "integrated"; break;
		case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: type = "virtual"; break;
		case VK_PHYSICAL_DEVICE_TYPE_CPU: type = "CPU"; break;
		default: break;
	}

	std::string version = versionString(m_apiVersion);
	LogInfo << "Using Vulkan " << version << (m_dynamicRenderingExtension ? " with VK_KHR_dynamic_rendering" : "");
	CrashHandler::setVariable("Vulkan version", version);

	LogInfo << " ├─ Device: " << m_properties.deviceName << " (" << type << ", driver "
	        << versionString(m_properties.driverVersion) << ")";
	CrashHandler::setVariable("Vulkan device", m_properties.deviceName);

	const char * presentMode = "FIFO";
	switch(m_presentMode) {
		case VK_PRESENT_MODE_MAILBOX_KHR: presentMode = "mailbox"; break;
		case VK_PRESENT_MODE_IMMEDIATE_KHR: presentMode = "immediate"; break;
		case VK_PRESENT_MODE_FIFO_RELAXED_KHR: presentMode = "FIFO relaxed"; break;
		default: break;
	}
	LogInfo << " ├─ Swapchain: " << m_swapchainImages.size() << " images, " << string_VkFormat(m_colorFormat)
	        << ", " << presentMode << ", depth " << string_VkFormat(m_depthFormat);

	VkPhysicalDeviceMemoryProperties memory;
	vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memory);
	uint64_t totalVRAM = 0;
	for(uint32_t i = 0; i < memory.memoryHeapCount; i++) {
		if(memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
			totalVRAM += memory.memoryHeaps[i].size;
		}
	}
	LogInfo << " └─ VRAM: " << (totalVRAM / 1024 / 1024) << " MiB";
	CrashHandler::setVariable("VRAM size", totalVRAM);

	credits::setLibraryCredits("graphics", "Vulkan " + version);

	m_maximumSupportedAnisotropy = m_features.samplerAnisotropy ? m_properties.limits.maxSamplerAnisotropy : 1.f;
	setMaxAnisotropy(float(config.video.maxAnisotropicFiltering));

}

void VulkanRenderer::beforeResize(bool wasOrIsFullscreen) {
	// The targets are recreated in afterResize(), no need to shut down
	ARX_UNUSED(wasOrIsFullscreen);
}

void VulkanRenderer::afterResize() {

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

void VulkanRenderer::reinit() {

	arx_assert(!isInitialized());

	if(!m_colorTarget.image) {
		createSizeDependentResources();
	}

	m_hasMSAA = false;

	m_TextureStages.resize(TextureStageCount);
	for(size_t i = 0; i < m_TextureStages.size(); ++i) {
		m_TextureStages[i] = std::make_unique<VulkanTextureStage>(this, unsigned(i));
	}

	// Clear screen
	Clear(ColorBuffer | DepthBuffer);

	onRendererInit();

}

void VulkanRenderer::shutdown() {

	arx_assert(isInitialized());

	onRendererShutdown();

	m_TextureStages.clear();

	m_maximumAnisotropy = 1.f;
	m_maximumSupportedAnisotropy = 1.f;

}

void VulkanRenderer::SetViewMatrix(const glm::mat4x4 & matView) {
	m_view = matView;
}

void VulkanRenderer::SetProjectionMatrix(const glm::mat4x4 & matProj) {
	m_projection = matProj;
}

void VulkanRenderer::ReleaseAllTextures() {
	for(VulkanTexture & texture : m_textures) {
		texture.destroy();
	}
}

void VulkanRenderer::RestoreAllTextures() {
	for(VulkanTexture & texture : m_textures) {
		texture.restore();
	}
}

void VulkanRenderer::reloadColorKeyTextures() {
	for(VulkanTexture & texture : m_textures) {
		if(texture.hasColorKey()) {
			texture.restore();
		}
	}
}

Texture * VulkanRenderer::createTexture() {
	VulkanTexture * texture = new VulkanTexture(this);
	m_textures.push_back(*texture);
	return texture;
}

void VulkanRenderer::onTextureDestroyed(VulkanTexture * texture) {
	for(size_t i = 0; i < getTextureStageCount(); i++) {
		VulkanTextureStage * stage = GetTextureStage(i);
		if(stage->m_texture == texture) {
			stage->m_texture = nullptr;
		}
	}
}

void VulkanRenderer::SetViewport(const Rect & viewport) {
	m_viewport = viewport;
	m_dynamicStateValid = false;
}

void VulkanRenderer::SetScissor(const Rect & rect) {
	m_scissor = rect;
	m_dynamicStateValid = false;
}

void VulkanRenderer::Clear(BufferFlags bufferFlags, Color clearColor, float clearDepth, size_t nrects,
                           Rect * rect) {

	if(!m_colorTarget.image || !(bufferFlags & (ColorBuffer | DepthBuffer))) {
		return;
	}

	beginRendering();

	Color4f color(clearColor);

	VkClearAttachment attachments[2];
	uint32_t count = 0;
	if(bufferFlags & ColorBuffer) {
		VkClearAttachment & attachment = attachments[count++];
		attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		attachment.colorAttachment = 0;
		attachment.clearValue.color = { { color.r, color.g, color.b, color.a } };
	}
	if(bufferFlags & DepthBuffer) {
		VkClearAttachment & attachment = attachments[count++];
		attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		attachment.colorAttachment = 0;
		attachment.clearValue.depthStencil = { clearDepth, 0 };
	}

	Vec2i size = m_inScene ? m_sceneSize : m_size;
	Rect bounds(size.x, size.y);

	std::vector<VkClearRect> rects;
	if(nrects == 0) {
		rects.push_back({ { { 0, 0 }, { uint32_t(size.x), uint32_t(size.y) } }, 0, 1 });
	} else {
		for(size_t i = 0; i < nrects; i++) {
			// The rectangles are in window pixels, also for the scaled scene
			Rect r = toTargetRect(rect[i]) & bounds;
			if(r.isValid() && r.width() > 0 && r.height() > 0) {
				rects.push_back({ { { r.left, r.top }, { uint32_t(r.width()), uint32_t(r.height()) } }, 0, 1 });
			}
		}
	}

	if(!rects.empty()) {
		vkCmdClearAttachments(m_frames[m_frameIndex].commands, count, attachments, uint32_t(rects.size()), rects.data());
	}

}

void VulkanRenderer::SetFogColor(Color color) {
	m_fogColor = Color4f(color);
}

void VulkanRenderer::SetFogParams(float fogStart, float fogEnd) {
	m_fogStart = fogStart;
	m_fogEnd = fogEnd;
}

void VulkanRenderer::SetAntialiasing(bool enable) {

	if(enable && !config.video.antialiasing) {
		return;
	}

	if(m_msaaSamples == VK_SAMPLE_COUNT_1_BIT) {
		return;
	}

	// Like Direct3D 11, triangles are always multisampled when rendering to a multisampled target,
	// this only changes how alpha cutouts are handled.
	m_hasMSAA = enable;
}

void VulkanRenderer::SetFillMode(FillMode mode) {
	m_wireframe = (mode == FillWireframe) && m_features.fillModeNonSolid;
}

void VulkanRenderer::setMaxAnisotropy(float value) {

	float maxAnisotropy = std::clamp(value, 1.f, m_maximumSupportedAnisotropy);
	if(m_maximumAnisotropy == maxAnisotropy) {
		return;
	}

	m_maximumAnisotropy = maxAnisotropy;

	clearSamplers();

}

VkCommandBuffer VulkanRenderer::beginUpload() {

	if(!m_upload.recording) {
		vkResetCommandPool(m_device, m_upload.commandPool, 0);
		VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(m_upload.commands, &begin);
		m_upload.recording = true;
	}

	return m_upload.commands;
}

void * VulkanRenderer::createStagingBuffer(VkDeviceSize size, VkBuffer & buffer) {

	// Don't let the staging memory grow while loading levels without presenting frames
	if(m_upload.recording && m_upload.stagingSize + size > MaxPendingUploadSize) {
		flushUploads();
	}

	VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	info.size = size;
	info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocation = {};
	allocation.usage = VMA_MEMORY_USAGE_AUTO;
	allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

	VmaAllocation memory = VK_NULL_HANDLE;
	VmaAllocationInfo allocationInfo = {};
	if(!checkVk(vmaCreateBuffer(m_allocator, &info, &allocation, &buffer, &memory, &allocationInfo),
	            "vmaCreateBuffer")) {
		return nullptr;
	}

	m_upload.staging.emplace_back(buffer, memory);
	m_upload.stagingSize += size;

	return allocationInfo.pMappedData;
}

void VulkanRenderer::flushUploads() {

	if(!m_upload.recording) {
		return;
	}

	vkEndCommandBuffer(m_upload.commands);
	m_upload.recording = false;
	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &m_upload.commands;

	VkResult result = vkQueueSubmit(m_queue, 1, &submit, m_upload.fence);
	if(result == VK_SUCCESS) {
		vkWaitForFences(m_device, 1, &m_upload.fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
		vkResetFences(m_device, 1, &m_upload.fence);
	} else if(result == VK_ERROR_DEVICE_LOST) {
		if(!m_deviceLost) {
			LogError << "Vulkan device lost";
			m_deviceLost = true;
		}
		vkDeviceWaitIdle(m_device);
	} else {
		checkVk(result, "vkQueueSubmit");
		vkDeviceWaitIdle(m_device);
	}

	for(const auto & [buffer, memory] : m_upload.staging) {
		vmaDestroyBuffer(m_allocator, buffer, memory);
	}
	m_upload.staging.clear();
	m_upload.stagingSize = 0;

}

VkSampler VulkanRenderer::getSampler(TextureStage::WrapMode wrapMode, TextureStage::FilterMode minFilter,
                                     TextureStage::FilterMode magFilter, bool mipmaps, float lodBias) {

	unsigned anisotropy = 1;
	if(mipmaps && minFilter == TextureStage::FilterLinear) {
		anisotropy = unsigned(m_maximumAnisotropy);
	}

	uint32_t biasBits;
	std::memcpy(&biasBits, &lodBias, sizeof(biasBits));

	uint64_t key = uint64_t(wrapMode) | (uint64_t(minFilter) << 2) | (uint64_t(magFilter) << 3)
	               | (uint64_t(mipmaps) << 4) | (uint64_t(anisotropy) << 8) | (uint64_t(biasBits) << 32);

	auto it = m_samplers.find(key);
	if(it != m_samplers.end()) {
		return it->second;
	}

	static const VkSamplerAddressMode arxToVkWrapMode[] = {
		VK_SAMPLER_ADDRESS_MODE_REPEAT,          // WrapRepeat
		VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, // WrapMirror
		VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE    // WrapClamp
	};

	VkSamplerCreateInfo info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	info.magFilter = (magFilter == TextureStage::FilterLinear) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	info.minFilter = (minFilter == TextureStage::FilterLinear) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	// OpenGL uses GL_*_MIPMAP_LINEAR for mipmapped textures
	info.mipmapMode = mipmaps ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
	info.addressModeU = info.addressModeV = info.addressModeW = arxToVkWrapMode[wrapMode];
	float maxBias = m_properties.limits.maxSamplerLodBias;
	info.mipLodBias = std::clamp(lodBias, -maxBias, maxBias);
	info.anisotropyEnable = (anisotropy > 1) ? VK_TRUE : VK_FALSE;
	info.maxAnisotropy = float(anisotropy);
	info.compareOp = VK_COMPARE_OP_NEVER;
	info.minLod = 0.f;
	info.maxLod = mipmaps ? VK_LOD_CLAMP_NONE : 0.f;
	info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

	VkSampler sampler = VK_NULL_HANDLE;
	if(!checkVk(vkCreateSampler(m_device, &info, nullptr, &sampler), "vkCreateSampler")) {
		return VK_NULL_HANDLE;
	}

	m_samplers.emplace(key, sampler);
	return sampler;
}

void VulkanRenderer::clearSamplers() {

	if(m_samplers.empty()) {
		return;
	}

	VkDevice device = m_device;
	std::vector<VkSampler> samplers;
	samplers.reserve(m_samplers.size());
	for(const auto & entry : m_samplers) {
		samplers.push_back(entry.second);
	}
	m_samplers.clear();

	deferRelease([device, samplers]() {
		for(VkSampler sampler : samplers) {
			vkDestroySampler(device, sampler, nullptr);
		}
	});

}

Renderer::AlphaCutoutAntialising VulkanRenderer::getMaxSupportedAlphaCutoutAntialiasing() const {
	return m_features.sampleRateShading ? CrispAlphaCutoutAA : FuzzyAlphaCutoutAA;
}

std::unique_ptr<VertexBuffer<TexturedVertex>> VulkanRenderer::createVertexBufferTL(size_t capacity,
                                                                                   BufferUsage usage) {
	return std::make_unique<VulkanVertexBuffer<TexturedVertex>>(this, capacity, usage);
}

std::unique_ptr<VertexBuffer<SMY_VERTEX>> VulkanRenderer::createVertexBuffer(size_t capacity, BufferUsage usage) {
	return std::make_unique<VulkanVertexBuffer<SMY_VERTEX>>(this, capacity, usage);
}

std::unique_ptr<VertexBuffer<SMY_VERTEX3>> VulkanRenderer::createVertexBuffer3(size_t capacity,
                                                                               BufferUsage usage) {
	return std::make_unique<VulkanVertexBuffer<SMY_VERTEX3>>(this, capacity, usage);
}

bool VulkanRenderer::allocate(RingBuffer & ring, VkDeviceSize size, VkDeviceSize alignment,
                              RingAllocation & allocation) {

	VkDeviceSize offset = (ring.pos + alignment - 1) / alignment * alignment;
	if(ring.current >= ring.chunks.size() || offset + size > ring.chunks[ring.current].size) {

		// Continue in the next chunk, add one if needed
		if(ring.current < ring.chunks.size()) {
			ring.current++;
		}
		while(ring.current < ring.chunks.size() && ring.chunks[ring.current].size < size) {
			ring.current++;
		}
		if(ring.current == ring.chunks.size()) {
			RingBuffer::Chunk chunk;
			chunk.size = std::max(ring.chunkSize, size);
			VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			info.size = chunk.size;
			info.usage = ring.usage;
			info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VmaAllocationCreateInfo create = {};
			create.usage = VMA_MEMORY_USAGE_AUTO;
			create.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
			VmaAllocationInfo allocationInfo = {};
			if(!checkVk(vmaCreateBuffer(m_allocator, &info, &create, &chunk.buffer, &chunk.allocation,
			                            &allocationInfo), "vmaCreateBuffer")) {
				return false;
			}
			chunk.data = static_cast<u8 *>(allocationInfo.pMappedData);
			ring.chunks.push_back(chunk);
		}

		offset = 0;
	}

	RingBuffer::Chunk & chunk = ring.chunks[ring.current];
	allocation.buffer = chunk.buffer;
	allocation.offset = offset;
	allocation.data = chunk.data + offset;
	ring.pos = offset + size;

	return true;
}

void VulkanRenderer::flushRing(RingBuffer & ring) {
	// Does nothing for host-coherent memory
	for(size_t i = 0; i <= ring.current && i < ring.chunks.size(); i++) {
		vmaFlushAllocation(m_allocator, ring.chunks[i].allocation, 0, VK_WHOLE_SIZE);
	}
}

void VulkanRenderer::releaseRing(RingBuffer & ring) {
	for(const RingBuffer::Chunk & chunk : ring.chunks) {
		vmaDestroyBuffer(m_allocator, chunk.buffer, chunk.allocation);
	}
	ring.chunks.clear();
	ring.current = 0;
	ring.pos = 0;
}

VkDescriptorSet VulkanRenderer::allocateDescriptorSet(VkDescriptorSetLayout layout) {

	Frame & frame = m_frames[m_frameIndex];

	while(true) {

		if(frame.descriptorPool == frame.descriptorPools.size()) {
			const uint32_t sets = 1024;
			const VkDescriptorPoolSize sizes[] = {
				{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 * sets },
				{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sets },
				{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4 * sets },
				{ VK_DESCRIPTOR_TYPE_SAMPLER, 3 * sets },
			};
			VkDescriptorPoolCreateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
			info.maxSets = sets;
			info.poolSizeCount = uint32_t(std::size(sizes));
			info.pPoolSizes = sizes;
			VkDescriptorPool pool = VK_NULL_HANDLE;
			if(!checkVk(vkCreateDescriptorPool(m_device, &info, nullptr, &pool), "vkCreateDescriptorPool")) {
				return VK_NULL_HANDLE;
			}
			frame.descriptorPools.push_back(pool);
		}

		VkDescriptorSetAllocateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		info.descriptorPool = frame.descriptorPools[frame.descriptorPool];
		info.descriptorSetCount = 1;
		info.pSetLayouts = &layout;
		VkDescriptorSet set = VK_NULL_HANDLE;
		VkResult result = vkAllocateDescriptorSets(m_device, &info, &set);
		if(result == VK_SUCCESS) {
			return set;
		}
		if(result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) {
			checkVk(result, "vkAllocateDescriptorSets");
			return VK_NULL_HANDLE;
		}

		// The pool is full, continue with the next one
		frame.descriptorPool++;
	}

}

void VulkanRenderer::invalidateBoundState() {
	m_boundPipeline = VK_NULL_HANDLE;
	m_boundVertexBuffer = VK_NULL_HANDLE;
	m_boundIndexBuffer = VK_NULL_HANDLE;
	m_dynamicStateValid = false;
	// Never used: the bias is -depthOffset, and NaN comparisons don't work with fast floating-point math
	m_boundDepthBias = 1.f;
	m_boundDescriptorSet = VK_NULL_HANDLE;
	m_vsConstants = RingAllocation();
	m_psConstants = RingAllocation();
}

static size_t vertexStride(VulkanRenderer::VertexType type) {
	switch(type) {
		case VulkanRenderer::VertexTL: return sizeof(TexturedVertex);
		case VulkanRenderer::VertexWorld: return sizeof(SMY_VERTEX);
		case VulkanRenderer::VertexWorld3: return sizeof(SMY_VERTEX3);
		default: arx_unreachable();
	}
}

bool VulkanRenderer::bindVertices(VertexType type, const void * vertices, size_t count) {

	size_t stride = vertexStride(type);
	RingAllocation allocation;
	if(!allocate(m_frames[m_frameIndex].vertices, VkDeviceSize(count * stride), 16, allocation)) {
		return false;
	}
	std::memcpy(allocation.data, vertices, count * stride);

	bindVertexBuffer(allocation.buffer, allocation.offset);

	return true;
}

bool VulkanRenderer::bindFrameCopy(VertexType type, const void * vertices, size_t count, FrameCopy & copy) {

	if(copy.frame != m_frameSerial || !copy.buffer || copy.count < count) {
		size_t stride = vertexStride(type);
		RingAllocation allocation;
		if(!allocate(m_frames[m_frameIndex].vertices, VkDeviceSize(count * stride), 16, allocation)) {
			return false;
		}
		std::memcpy(allocation.data, vertices, count * stride);
		copy.frame = m_frameSerial;
		copy.buffer = allocation.buffer;
		copy.offset = allocation.offset;
		copy.count = count;
	}

	bindVertexBuffer(copy.buffer, copy.offset);

	return true;
}

void VulkanRenderer::bindVertexBuffer(VkBuffer buffer, VkDeviceSize offset) {
	if(buffer != m_boundVertexBuffer || offset != m_boundVertexOffset) {
		vkCmdBindVertexBuffers(m_frames[m_frameIndex].commands, 0, 1, &buffer, &offset);
		m_boundVertexBuffer = buffer;
		m_boundVertexOffset = offset;
	}
}

bool VulkanRenderer::bindIndices(const unsigned short * indices, size_t count) {

	RingAllocation allocation;
	if(!allocate(m_frames[m_frameIndex].indices, VkDeviceSize(count * sizeof(unsigned short)), 4, allocation)) {
		return false;
	}
	std::memcpy(allocation.data, indices, count * sizeof(unsigned short));

	if(allocation.buffer != m_boundIndexBuffer || allocation.offset != m_boundIndexOffset) {
		vkCmdBindIndexBuffer(m_frames[m_frameIndex].commands, allocation.buffer, allocation.offset,
		                     VK_INDEX_TYPE_UINT16);
		m_boundIndexBuffer = allocation.buffer;
		m_boundIndexOffset = allocation.offset;
	}

	return true;
}

void VulkanRenderer::drawBuffer(VertexType type, Primitive primitive, const void * vertices, size_t count,
                                size_t first, size_t drawCount, FrameCopy * copy) {

	if(drawCount == 0 || !prepareDraw(type, primitive)) {
		return;
	}

	if(copy) {
		if(!bindFrameCopy(type, vertices, std::max(count, first + drawCount), *copy)) {
			return;
		}
	} else {
		const u8 * data = static_cast<const u8 *>(vertices) + first * vertexStride(type);
		if(!bindVertices(type, data, drawCount)) {
			return;
		}
		first = 0;
	}

	vkCmdDraw(m_frames[m_frameIndex].commands, uint32_t(drawCount), 1, uint32_t(first), 0);

}

void VulkanRenderer::drawBufferIndexed(VertexType type, Primitive primitive, const void * vertices, size_t count,
                                       size_t baseVertex, const unsigned short * indices, size_t nindices,
                                       FrameCopy * copy) {

	if(nindices == 0 || !prepareDraw(type, primitive)) {
		return;
	}

	size_t used = size_t(*std::max_element(indices, indices + nindices)) + 1;
	if(copy) {
		if(!bindFrameCopy(type, vertices, std::max(count, baseVertex + used), *copy)) {
			return;
		}
	} else {
		// Only copy the vertices used by the indices
		const u8 * data = static_cast<const u8 *>(vertices) + baseVertex * vertexStride(type);
		if(!bindVertices(type, data, used)) {
			return;
		}
		baseVertex = 0;
	}

	if(!bindIndices(indices, nindices)) {
		return;
	}

	vkCmdDrawIndexed(m_frames[m_frameIndex].commands, uint32_t(nindices), 1, 0, int32_t(baseVertex), 0);

}

void VulkanRenderer::drawIndexed(Primitive primitive, const TexturedVertex * vertices, size_t nvertices,
                                 unsigned short * indices, size_t nindices) {

	if(nvertices == 0 || nindices == 0 || !prepareDraw(VertexTL, primitive)) {
		return;
	}

	// Models pass all their vertices with the indices of one texture: only copy the indexed range
	auto [first, last] = std::minmax_element(indices, indices + nindices);
	size_t begin = *first;
	size_t end = std::min(size_t(*last) + 1, nvertices);
	if(begin >= end || !bindVertices(VertexTL, vertices + begin, end - begin) || !bindIndices(indices, nindices)) {
		return;
	}

	vkCmdDrawIndexed(m_frames[m_frameIndex].commands, uint32_t(nindices), 1, 0, -int32_t(begin), 0);

}

bool VulkanRenderer::getSnapshot(Image & image) {

	if(!m_colorTarget.image || m_deviceLost) {
		return false;
	}

	/*
	 * The commands recorded so far are submitted together with the copy and waited for, the rest of the frame
	 * continues in the next command buffer. The target keeps its contents for present().
	 */
	beginFrame();
	endRendering();

	uint32_t width = uint32_t(m_size.x);
	uint32_t height = uint32_t(m_size.y);
	VkDeviceSize size = VkDeviceSize(width) * height * 4;

	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation memory = VK_NULL_HANDLE;
	VmaAllocationInfo memoryInfo = {};
	{
		VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		info.size = size;
		info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VmaAllocationCreateInfo allocation = {};
		allocation.usage = VMA_MEMORY_USAGE_AUTO;
		allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		if(!checkVk(vmaCreateBuffer(m_allocator, &info, &allocation, &buffer, &memory, &memoryInfo),
		            "vmaCreateBuffer")) {
			return false;
		}
	}

	VkCommandBuffer commands = m_frames[m_frameIndex].commands;

	transition(m_colorTarget, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

	// Multisampled images cannot be copied to buffers
	ImageTarget resolved;
	VkImage source = m_colorTarget.image;
	if(m_msaaSamples != VK_SAMPLE_COUNT_1_BIT) {
		VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		info.imageType = VK_IMAGE_TYPE_2D;
		info.format = m_colorFormat;
		info.extent = { width, height, 1 };
		info.mipLevels = 1;
		info.arrayLayers = 1;
		info.samples = VK_SAMPLE_COUNT_1_BIT;
		info.tiling = VK_IMAGE_TILING_OPTIMAL;
		info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VmaAllocationCreateInfo allocation = {};
		allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
		if(!checkVk(vmaCreateImage(m_allocator, &info, &allocation, &resolved.image, &resolved.allocation, nullptr),
		            "vmaCreateImage")) {
			vmaDestroyBuffer(m_allocator, buffer, memory);
			return false;
		}
		transition(resolved, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
		VkImageSubresourceLayers layers = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		VkImageResolve region = { layers, { 0, 0, 0 }, layers, { 0, 0, 0 }, { width, height, 1 } };
		vkCmdResolveImage(commands, m_colorTarget.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, resolved.image,
		                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		transition(resolved, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
		source = resolved.image;
	}

	VkBufferImageCopy copy = {};
	copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	copy.imageExtent = { width, height, 1 };
	vkCmdCopyImageToBuffer(commands, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);

	VkBufferMemoryBarrier barrier = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer = buffer;
	barrier.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
	                     &barrier, 0, nullptr);

	VkFence fence = m_frames[m_frameIndex].fence;
	endFrame(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
	bool success = !m_deviceLost
	               && vkWaitForFences(m_device, 1, &fence, VK_TRUE, std::numeric_limits<uint64_t>::max()) == VK_SUCCESS;

	if(success) {
		vmaInvalidateAllocation(m_allocator, memory, 0, VK_WHOLE_SIZE);
		bool bgra = (m_colorFormat == VK_FORMAT_B8G8R8A8_UNORM || m_colorFormat == VK_FORMAT_B8G8R8A8_SRGB);
		image.create(size_t(width), size_t(height), Image::Format_R8G8B8);
		const u8 * in = static_cast<const u8 *>(memoryInfo.pMappedData);
		u8 * out = image.getData();
		for(size_t i = 0; i < size_t(width) * height; i++, in += 4, out += 3) {
			out[0] = in[bgra ? 2 : 0];
			out[1] = in[1];
			out[2] = in[bgra ? 0 : 2];
		}
	}

	// The GPU is done with them
	if(resolved.image) {
		vmaDestroyImage(m_allocator, resolved.image, resolved.allocation);
	}
	vmaDestroyBuffer(m_allocator, buffer, memory);

	return success;
}

bool VulkanRenderer::getSnapshot(Image & image, size_t width, size_t height) {

	// TODO handle scaling on the GPU so we don't need to download the whole image

	Image fullsize;
	if(!getSnapshot(fullsize)) {
		return false;
	}

	image.resizeFrom(fullsize, width, height);

	return true;
}

void VulkanRenderer::setVSync(int vsync) {
	if(vsync != m_vsync) {
		m_vsync = vsync;
		m_swapchainDirty = true;
	}
}

void VulkanRenderer::present() {

	if(!m_device || m_deviceLost) {
		return;
	}

	if(!m_snapshotPath.empty() && std::chrono::steady_clock::now() >= m_snapshotTime) {
		Image image;
		if(getSnapshot(image) && image.save(fs::path(m_snapshotPath))) {
			LogInfo << "Saved debug snapshot to " << m_snapshotPath;
		} else {
			LogError << "Failed to save debug snapshot to " << m_snapshotPath;
		}
		m_snapshotPath.clear();
	}

	if(m_swapchainDirty || !m_swapchain) {
		vkDeviceWaitIdle(m_device);
		createSwapchain();
	}

	beginFrame();
	endRendering();

	if(!m_swapchain || m_swapchainDirty || !m_colorTarget.image) {
		// Nothing to present to (minimized), still submit the recorded commands
		endFrame(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
		return;
	}

	Frame & frame = m_frames[m_frameIndex];

	uint32_t index = 0;
	VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, std::numeric_limits<uint64_t>::max(),
	                                        frame.imageAcquired, VK_NULL_HANDLE, &index);
	if(result == VK_ERROR_OUT_OF_DATE_KHR) {
		m_swapchainDirty = true;
		endFrame(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
		return;
	} else if(result == VK_SUBOPTIMAL_KHR) {
		m_swapchainDirty = true;
	} else if(!checkVk(result, "vkAcquireNextImageKHR")) {
		endFrame(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
		return;
	}

	VkCommandBuffer commands = frame.commands;

	transition(m_colorTarget, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

	ImageTarget swapchainImage;
	swapchainImage.image = m_swapchainImages[index];
	transition(swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

	VkImageSubresourceLayers layers = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	if(m_swapchainExtent.width == uint32_t(m_size.x) && m_swapchainExtent.height == uint32_t(m_size.y)) {
		VkExtent3D extent = { uint32_t(m_size.x), uint32_t(m_size.y), 1 };
		if(m_msaaSamples != VK_SAMPLE_COUNT_1_BIT) {
			VkImageResolve region = { layers, { 0, 0, 0 }, layers, { 0, 0, 0 }, extent };
			vkCmdResolveImage(commands, m_colorTarget.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			                  swapchainImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		} else {
			VkImageCopy region = { layers, { 0, 0, 0 }, layers, { 0, 0, 0 }, extent };
			vkCmdCopyImage(commands, m_colorTarget.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			               swapchainImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		}
	} else {
		// The window is being resized
		VkClearColorValue black = { { 0.f, 0.f, 0.f, 1.f } };
		VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdClearColorImage(commands, swapchainImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
	}

	transition(swapchainImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT);

	VkSemaphore renderFinished = m_renderFinished[index];
	endFrame(frame.imageAcquired, VK_PIPELINE_STAGE_TRANSFER_BIT, renderFinished);
	if(m_deviceLost) {
		return;
	}

	VkPresentInfoKHR present = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &renderFinished;
	present.swapchainCount = 1;
	present.pSwapchains = &m_swapchain;
	present.pImageIndices = &index;
	result = vkQueuePresentKHR(m_queue, &present);
	if(result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
		m_swapchainDirty = true;
	} else if(result == VK_ERROR_DEVICE_LOST) {
		LogError << "Vulkan device lost";
		m_deviceLost = true;
	} else {
		checkVk(result, "vkQueuePresentKHR");
	}

}

namespace vulkan {

Renderer * createRenderer() {
	return new VulkanRenderer;
}

bool isVulkan(const Renderer * renderer) {
	return renderer && renderer == g_vulkanRenderer;
}

int createDevice(Renderer & renderer, void * window, int maxMSAALevel) {
	VulkanRenderer & vulkan = static_cast<VulkanRenderer &>(renderer);
	if(!vulkan.create(static_cast<SDL_Window *>(window), maxMSAALevel)) {
		return 0;
	}
	return std::max(vulkan.msaaSamples(), 1);
}

void setVSync(Renderer & renderer, int vsync) {
	static_cast<VulkanRenderer &>(renderer).setVSync(vsync);
}

void present(Renderer & renderer) {
	static_cast<VulkanRenderer &>(renderer).present();
}

} // namespace vulkan
