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

/*
 * All OpenXR functions are called through pointers so that a runtime can be loaded directly, bypassing the
 * system loader. The loader ignores XR_RUNTIME_JSON in elevated processes (e.g. with UAC disabled), which would
 * make it impossible to use SteamVR while another runtime (such as Virtual Desktop) is the system default.
 */
#ifndef XR_NO_PROTOTYPES
#define XR_NO_PROTOTYPES
#endif

#include "vr/VRSystem.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

#include "graphics/opengl/OpenGLUtil.h"
#include <epoxy/wgl.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include "core/Config.h"
#include "core/Version.h"
#include "io/log/Logger.h"
#include "platform/ProgramOptions.h"


VRSystem * g_vr = nullptr;

namespace {

#define ARX_XR_GLOBAL_FUNCTIONS(X) \
	X(xrEnumerateInstanceExtensionProperties) \
	X(xrCreateInstance)

#define ARX_XR_INSTANCE_FUNCTIONS(X) \
	X(xrResultToString) \
	X(xrDestroyInstance) \
	X(xrGetInstanceProperties) \
	X(xrGetSystem) \
	X(xrGetSystemProperties) \
	X(xrEnumerateViewConfigurationViews) \
	X(xrCreateSession) \
	X(xrDestroySession) \
	X(xrBeginSession) \
	X(xrEndSession) \
	X(xrCreateReferenceSpace) \
	X(xrDestroySpace) \
	X(xrEnumerateSwapchainFormats) \
	X(xrCreateSwapchain) \
	X(xrDestroySwapchain) \
	X(xrEnumerateSwapchainImages) \
	X(xrAcquireSwapchainImage) \
	X(xrWaitSwapchainImage) \
	X(xrReleaseSwapchainImage) \
	X(xrPollEvent) \
	X(xrWaitFrame) \
	X(xrBeginFrame) \
	X(xrEndFrame) \
	X(xrLocateViews)

#define ARX_XR_DECLARE_FUNCTION(name) PFN_##name name = nullptr;
PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
ARX_XR_GLOBAL_FUNCTIONS(ARX_XR_DECLARE_FUNCTION)
ARX_XR_INSTANCE_FUNCTIONS(ARX_XR_DECLARE_FUNCTION)
#undef ARX_XR_DECLARE_FUNCTION

bool loadFunctions(XrInstance instance) {
	bool success = true;
	#define ARX_XR_LOAD_FUNCTION(name) \
		if(XR_FAILED(xrGetInstanceProcAddr(instance, #name, reinterpret_cast<PFN_xrVoidFunction *>(&name))) \
		   || !name) { \
			LogError << "OpenXR: missing function " << #name; \
			success = false; \
		}
	if(instance == XR_NULL_HANDLE) {
		ARX_XR_GLOBAL_FUNCTIONS(ARX_XR_LOAD_FUNCTION)
	} else {
		ARX_XR_INSTANCE_FUNCTIONS(ARX_XR_LOAD_FUNCTION)
	}
	#undef ARX_XR_LOAD_FUNCTION
	return success;
}

PFN_xrConvertWin32PerformanceCounterToTimeKHR g_convertTime = nullptr;

//! \return the current OpenXR time, or 0 if not available
XrTime getCurrentTime(XrInstance instance) {
	LARGE_INTEGER counter;
	XrTime time = 0;
	if(!g_convertTime || !QueryPerformanceCounter(&counter) || XR_FAILED(g_convertTime(instance, &counter, &time))) {
		return 0;
	}
	return time;
}

//! Rate limit for repeated per-frame errors: log the first few, then every 1000th
bool shouldLogRepeatedError(u64 & count) {
	count++;
	return count <= 5 || count % 1000 == 0;
}

bool g_vrForced = false;

void enableVR() {
	g_vrForced = true;
}

ARX_PROGRAM_OPTION("vr", "", "Enable VR mode (OpenXR)", &enableVR)

const char * resultString(XrInstance instance, XrResult result) {
	static char buffer[XR_MAX_RESULT_STRING_SIZE];
	if(instance == XR_NULL_HANDLE || !xrResultToString || XR_FAILED(xrResultToString(instance, result, buffer))) {
		std::snprintf(buffer, sizeof(buffer), "XrResult(%d)", int(result));
	}
	return buffer;
}

std::string getEnv(const char * name) {
	char buffer[4096];
	DWORD length = GetEnvironmentVariableA(name, buffer, DWORD(sizeof(buffer)));
	if(length == 0 || length >= sizeof(buffer)) {
		return std::string();
	}
	return std::string(buffer, length);
}

std::string readFile(const std::string & path) {
	std::ifstream ifs(path, std::ios::binary);
	if(!ifs) {
		return std::string();
	}
	return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

/*!
 * Minimal JSON lookup: return the first string value following the given key.
 * Handles both "key" : "value" and "key" : [ "value", ... ]
 */
std::string findJsonString(const std::string & data, const char * key) {

	size_t pos = data.find(std::string("\"") + key + "\"");
	if(pos == std::string::npos || (pos = data.find(':', pos)) == std::string::npos
	   || (pos = data.find('"', pos)) == std::string::npos) {
		return std::string();
	}

	std::string value;
	for(pos++; pos < data.size() && data[pos] != '"'; pos++) {
		if(data[pos] == '\\' && pos + 1 < data.size()) {
			pos++;
		}
		value += data[pos];
	}

	return value;
}

//! Find the SteamVR OpenXR runtime manifest using the OpenVR path registry
std::string findSteamVRRuntime() {

	std::string localAppData = getEnv("LOCALAPPDATA");
	if(localAppData.empty()) {
		return std::string();
	}

	std::string path = findJsonString(readFile(localAppData + "\\openvr\\openvrpaths.vrpath"), "runtime");
	if(path.empty()) {
		return std::string();
	}

	return path + "\\steamxr_win64.json";
}

//! \return the runtime manifest selected in the config, or an empty string to use the system default
std::string getSelectedRuntimeManifest() {

	const std::string & runtime = config.vr.runtime;
	if(runtime.empty() || runtime == "auto") {
		return std::string();
	}

	if(runtime == "steamvr") {
		std::string manifest = findSteamVRRuntime();
		if(manifest.empty()) {
			LogWarning << "Could not locate the SteamVR OpenXR runtime, using the system default";
		}
		return manifest;
	}

	return runtime;
}

//! Load a runtime directly from its manifest and negotiate the loader interface with it
PFN_xrGetInstanceProcAddr loadRuntime(const std::string & manifest) {

	std::string libraryPath = findJsonString(readFile(manifest), "library_path");
	if(libraryPath.empty()) {
		LogError << "OpenXR: could not read library_path from " << manifest;
		return nullptr;
	}

	bool isAbsolute = (libraryPath.size() > 1 && libraryPath[1] == ':')
	                  || libraryPath[0] == '\\' || libraryPath[0] == '/';
	if(!isAbsolute) {
		size_t slash = manifest.find_last_of("\\/");
		if(slash != std::string::npos) {
			libraryPath = manifest.substr(0, slash + 1) + libraryPath;
		}
	}

	HMODULE library = LoadLibraryExA(libraryPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if(!library) {
		LogError << "OpenXR: could not load runtime library " << libraryPath << " (error " << GetLastError() << ')';
		return nullptr;
	}

	auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
		reinterpret_cast<void *>(GetProcAddress(library, "xrNegotiateLoaderRuntimeInterface")));
	if(!negotiate) {
		LogError << "OpenXR: " << libraryPath << " is not an OpenXR runtime";
		return nullptr;
	}

	XrNegotiateLoaderInfo loaderInfo = { };
	loaderInfo.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
	loaderInfo.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
	loaderInfo.structSize = sizeof(loaderInfo);
	loaderInfo.minInterfaceVersion = 1;
	loaderInfo.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
	loaderInfo.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
	loaderInfo.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);

	XrNegotiateRuntimeRequest runtimeRequest = { };
	runtimeRequest.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
	runtimeRequest.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
	runtimeRequest.structSize = sizeof(runtimeRequest);

	if(XR_FAILED(negotiate(&loaderInfo, &runtimeRequest)) || !runtimeRequest.getInstanceProcAddr) {
		LogError << "OpenXR: runtime interface negotiation with " << libraryPath << " failed";
		return nullptr;
	}

	LogInfo << "Loaded OpenXR runtime " << libraryPath;

	// The runtime library stays loaded until the process exits
	return runtimeRequest.getInstanceProcAddr;
}

//! Load the runtime selected in the config, or the system OpenXR loader if none was selected
bool loadOpenXR() {

	if(xrGetInstanceProcAddr) {
		return true;
	}

	std::string manifest = getSelectedRuntimeManifest();
	if(!manifest.empty()) {
		xrGetInstanceProcAddr = loadRuntime(manifest);
	} else {
		HMODULE loader = LoadLibraryA("openxr_loader.dll");
		if(!loader) {
			LogError << "OpenXR: could not load openxr_loader.dll";
			return false;
		}
		xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
			reinterpret_cast<void *>(GetProcAddress(loader, "xrGetInstanceProcAddr")));
	}

	if(!xrGetInstanceProcAddr || !loadFunctions(XR_NULL_HANDLE)) {
		xrGetInstanceProcAddr = nullptr;
		return false;
	}

	return true;
}

} // anonymous namespace

VRSystem::VRSystem()
	: m_instance(XR_NULL_HANDLE)
	, m_systemId(XR_NULL_SYSTEM_ID)
	, m_session(XR_NULL_HANDLE)
	, m_stageSpace(XR_NULL_HANDLE)
	, m_sessionState(XR_SESSION_STATE_UNKNOWN)
	, m_sessionRunning(false)
	, m_exitRequested(false)
	, m_eyeSize(0)
	, m_colorFormat(0)
	, m_blitFramebuffer(0)
	, m_resolveFramebuffer(0)
	, m_resolveRenderbuffer(0)
	, m_resolveSize(0)
	, m_stereoFramebuffer(0)
	, m_stereoColor(0)
	, m_stereoDepth(0)
	, m_stereoResolveFramebuffer(0)
	, m_stereoResolveColor(0)
	, m_stereoActive(false)
	, m_stereoRendered(false)
	, m_frameState{ XR_TYPE_FRAME_STATE }
	, m_frameActive(false)
	, m_shouldRender(false)
	, m_rendered(false)
{
	for(XrView & view : m_views) {
		view = { XR_TYPE_VIEW };
	}
	for(XrCompositionLayerProjectionView & view : m_layerViews) {
		view = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
	}
}

VRSystem::~VRSystem() {
	shutdown();
}

bool VRSystem::isRequested() {
	return g_vrForced || config.vr.enabled;
}

bool VRSystem::check(XrResult result, const char * call) const {
	if(XR_FAILED(result)) {
		LogError << "OpenXR: " << call << " failed: " << resultString(m_instance, result);
		return false;
	}
	return true;
}

bool VRSystem::initInstance() {

	arx_assert(m_instance == XR_NULL_HANDLE);

	if(!loadOpenXR()) {
		LogError << "OpenXR: no runtime available";
		return false;
	}

	uint32_t extensionCount = 0;
	if(!check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr),
	          "xrEnumerateInstanceExtensionProperties")) {
		LogError << "OpenXR: no runtime available";
		return false;
	}
	std::vector<XrExtensionProperties> extensions(extensionCount, { XR_TYPE_EXTENSION_PROPERTIES });
	xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data());

	bool hasOpenGL = std::any_of(extensions.begin(), extensions.end(), [](const XrExtensionProperties & e) {
		return std::strcmp(e.extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME) == 0;
	});
	if(!hasOpenGL) {
		LogError << "OpenXR: the active runtime does not support " << XR_KHR_OPENGL_ENABLE_EXTENSION_NAME
		         << " - select an OpenGL capable runtime such as SteamVR (set runtime=steamvr in the [vr] config section)";
		return false;
	}

	bool hasTimeConversion = std::any_of(extensions.begin(), extensions.end(), [](const XrExtensionProperties & e) {
		return std::strcmp(e.extensionName, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME) == 0;
	});
	
	std::vector<const char *> enabledExtensions = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
	if(hasTimeConversion) {
		enabledExtensions.push_back(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
	}

	XrInstanceCreateInfo createInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
	std::snprintf(createInfo.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "%s", "Arx Libertatis VR");
	createInfo.applicationInfo.applicationVersion = 1;
	std::snprintf(createInfo.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "%s", "Arx Libertatis");
	createInfo.applicationInfo.engineVersion = 1;
	createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	createInfo.enabledExtensionCount = uint32_t(enabledExtensions.size());
	createInfo.enabledExtensionNames = enabledExtensions.data();

	if(!check(xrCreateInstance(&createInfo, &m_instance), "xrCreateInstance")) {
		m_instance = XR_NULL_HANDLE;
		return false;
	}

	if(!loadFunctions(m_instance)) {
		shutdown();
		return false;
	}
	
	g_convertTime = nullptr;
	if(hasTimeConversion) {
		xrGetInstanceProcAddr(m_instance, "xrConvertWin32PerformanceCounterToTimeKHR",
		                      reinterpret_cast<PFN_xrVoidFunction *>(&g_convertTime));
	}

	XrInstanceProperties instanceProperties = { XR_TYPE_INSTANCE_PROPERTIES };
	if(XR_SUCCEEDED(xrGetInstanceProperties(m_instance, &instanceProperties))) {
		std::ostringstream oss;
		oss << instanceProperties.runtimeName << ' '
		    << XR_VERSION_MAJOR(instanceProperties.runtimeVersion) << '.'
		    << XR_VERSION_MINOR(instanceProperties.runtimeVersion) << '.'
		    << XR_VERSION_PATCH(instanceProperties.runtimeVersion);
		m_runtimeName = oss.str();
	}

	XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if(!check(xrGetSystem(m_instance, &systemInfo, &m_systemId), "xrGetSystem")) {
		LogError << "OpenXR: no headset found - is it connected and is " << m_runtimeName << " running?";
		shutdown();
		return false;
	}

	XrSystemProperties systemProperties = { XR_TYPE_SYSTEM_PROPERTIES };
	if(XR_SUCCEEDED(xrGetSystemProperties(m_instance, m_systemId, &systemProperties))) {
		m_systemName = systemProperties.systemName;
	}

	uint32_t viewCount = 0;
	xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	                                  0, &viewCount, nullptr);
	std::vector<XrViewConfigurationView> configViews(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
	if(viewCount != EyeCount
	   || !check(xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	                                               viewCount, &viewCount, configViews.data()),
	             "xrEnumerateViewConfigurationViews")) {
		LogError << "OpenXR: stereo view configuration not supported";
		shutdown();
		return false;
	}
	m_eyeSize = Vec2i(s32(configViews[0].recommendedImageRectWidth), s32(configViews[0].recommendedImageRectHeight));

	LogInfo << "OpenXR runtime: " << m_runtimeName << ", system: " << m_systemName
	        << ", eye resolution: " << m_eyeSize.x << 'x' << m_eyeSize.y;

	return true;
}

void VRSystem::shutdown() {

	destroySession();

	if(m_instance != XR_NULL_HANDLE) {
		xrDestroyInstance(m_instance);
		m_instance = XR_NULL_HANDLE;
	}
	m_systemId = XR_NULL_SYSTEM_ID;

}

bool VRSystem::createSession() {

	arx_assert(m_instance != XR_NULL_HANDLE);
	arx_assert(m_session == XR_NULL_HANDLE);

	// Must be called before xrCreateSession
	PFN_xrGetOpenGLGraphicsRequirementsKHR getRequirements = nullptr;
	if(!check(xrGetInstanceProcAddr(m_instance, "xrGetOpenGLGraphicsRequirementsKHR",
	                                reinterpret_cast<PFN_xrVoidFunction *>(&getRequirements)),
	          "xrGetInstanceProcAddr(xrGetOpenGLGraphicsRequirementsKHR)")) {
		return false;
	}
	XrGraphicsRequirementsOpenGLKHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
	if(!check(getRequirements(m_instance, m_systemId, &requirements), "xrGetOpenGLGraphicsRequirementsKHR")) {
		return false;
	}

	XrVersion glVersion = XR_MAKE_VERSION(epoxy_gl_version() / 10, epoxy_gl_version() % 10, 0);
	if(glVersion < requirements.minApiVersionSupported) {
		LogError << "OpenXR: runtime requires OpenGL " << XR_VERSION_MAJOR(requirements.minApiVersionSupported)
		         << '.' << XR_VERSION_MINOR(requirements.minApiVersionSupported);
		return false;
	}

	if(!epoxy_has_gl_extension("GL_ARB_framebuffer_object") && epoxy_gl_version() < 30) {
		LogError << "OpenXR: OpenGL framebuffer objects are required";
		return false;
	}

	XrGraphicsBindingOpenGLWin32KHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
	binding.hDC = wglGetCurrentDC();
	binding.hGLRC = wglGetCurrentContext();
	if(!binding.hDC || !binding.hGLRC) {
		LogError << "OpenXR: no current OpenGL context";
		return false;
	}

	XrSessionCreateInfo sessionInfo = { XR_TYPE_SESSION_CREATE_INFO };
	sessionInfo.next = &binding;
	sessionInfo.systemId = m_systemId;
	if(!check(xrCreateSession(m_instance, &sessionInfo, &m_session), "xrCreateSession")) {
		m_session = XR_NULL_HANDLE;
		return false;
	}

	XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
	spaceInfo.poseInReferenceSpace.orientation.w = 1.f;
	if(XR_FAILED(xrCreateReferenceSpace(m_session, &spaceInfo, &m_stageSpace))) {
		LogWarning << "OpenXR: stage space not available, falling back to local space";
		spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		if(!check(xrCreateReferenceSpace(m_session, &spaceInfo, &m_stageSpace), "xrCreateReferenceSpace")) {
			destroySession();
			return false;
		}
	}

	if(!createSwapchains()) {
		destroySession();
		return false;
	}

	glGenFramebuffers(1, &m_blitFramebuffer);
	glGenFramebuffers(1, &m_resolveFramebuffer);

	if(!createStereoTarget()) {
		destroySession();
		return false;
	}

	LogInfo << "OpenXR session created";

	return true;
}

bool VRSystem::createSwapchains() {

	uint32_t formatCount = 0;
	xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr);
	std::vector<s64> formats(formatCount);
	if(!check(xrEnumerateSwapchainFormats(m_session, formatCount, &formatCount, formats.data()),
	          "xrEnumerateSwapchainFormats")) {
		return false;
	}

	/*
	 * The game renders gamma-encoded colors. Storing them unchanged in an sRGB image (with GL_FRAMEBUFFER_SRGB
	 * disabled) makes the compositor decode them correctly.
	 */
	const s64 preferred[] = { GL_SRGB8_ALPHA8, GL_SRGB8, GL_RGBA8 };
	m_colorFormat = 0;
	for(s64 format : preferred) {
		if(std::find(formats.begin(), formats.end(), format) != formats.end()) {
			m_colorFormat = format;
			break;
		}
	}
	if(m_colorFormat == 0) {
		LogError << "OpenXR: no supported swapchain color format";
		return false;
	}

	for(EyeSwapchain & swapchain : m_swapchains) {

		XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		info.format = m_colorFormat;
		info.sampleCount = 1;
		info.width = uint32_t(m_eyeSize.x);
		info.height = uint32_t(m_eyeSize.y);
		info.faceCount = 1;
		info.arraySize = 1;
		info.mipCount = 1;
		if(!check(xrCreateSwapchain(m_session, &info, &swapchain.handle), "xrCreateSwapchain")) {
			swapchain.handle = XR_NULL_HANDLE;
			return false;
		}

		uint32_t imageCount = 0;
		xrEnumerateSwapchainImages(swapchain.handle, 0, &imageCount, nullptr);
		std::vector<XrSwapchainImageOpenGLKHR> images(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR });
		if(!check(xrEnumerateSwapchainImages(swapchain.handle, imageCount, &imageCount,
		                                     reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data())),
		          "xrEnumerateSwapchainImages")) {
			return false;
		}
		swapchain.images.clear();
		for(const XrSwapchainImageOpenGLKHR & image : images) {
			swapchain.images.push_back(image.image);
		}

	}

	return true;
}

void VRSystem::destroySwapchains() {

	for(EyeSwapchain & swapchain : m_swapchains) {
		if(swapchain.handle != XR_NULL_HANDLE) {
			xrDestroySwapchain(swapchain.handle);
		}
		swapchain = EyeSwapchain();
	}

}

void VRSystem::destroySession() {

	if(m_session == XR_NULL_HANDLE) {
		return;
	}

	if(m_frameActive) {
		endFrame();
	}

	if(m_blitFramebuffer) {
		glDeleteFramebuffers(1, &m_blitFramebuffer);
		m_blitFramebuffer = 0;
	}
	if(m_resolveFramebuffer) {
		glDeleteFramebuffers(1, &m_resolveFramebuffer);
		m_resolveFramebuffer = 0;
	}
	if(m_resolveRenderbuffer) {
		glDeleteRenderbuffers(1, &m_resolveRenderbuffer);
		m_resolveRenderbuffer = 0;
	}
	m_resolveSize = Vec2i(0);

	destroyStereoTarget();

	destroySwapchains();

	if(m_stageSpace != XR_NULL_HANDLE) {
		xrDestroySpace(m_stageSpace);
		m_stageSpace = XR_NULL_HANDLE;
	}

	xrDestroySession(m_session);
	m_session = XR_NULL_HANDLE;
	m_sessionRunning = false;
	m_sessionState = XR_SESSION_STATE_UNKNOWN;

	LogInfo << "OpenXR session destroyed";

}

void VRSystem::pollEvents() {

	if(m_instance == XR_NULL_HANDLE) {
		return;
	}

	XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
	while(xrPollEvent(m_instance, &event) == XR_SUCCESS) {

		switch(event.type) {

			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
				const auto & stateEvent = reinterpret_cast<const XrEventDataSessionStateChanged &>(event);
				if(stateEvent.session == m_session) {
					handleSessionStateChange(stateEvent.state);
				}
				break;
			}

			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
				LogWarning << "OpenXR: instance loss pending";
				m_exitRequested = true;
				break;
			}

			default: break;

		}

		event = { XR_TYPE_EVENT_DATA_BUFFER };
	}

}

void VRSystem::handleSessionStateChange(XrSessionState state) {

	LogDebug("OpenXR session state " << int(m_sessionState) << " -> " << int(state));
	m_sessionState = state;

	switch(state) {

		case XR_SESSION_STATE_READY: {
			XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
			beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			if(check(xrBeginSession(m_session, &beginInfo), "xrBeginSession")) {
				m_sessionRunning = true;
				LogInfo << "OpenXR session running";
			}
			break;
		}

		case XR_SESSION_STATE_STOPPING: {
			m_sessionRunning = false;
			check(xrEndSession(m_session), "xrEndSession");
			break;
		}

		case XR_SESSION_STATE_EXITING:
		case XR_SESSION_STATE_LOSS_PENDING: {
			LogInfo << "OpenXR session ending";
			m_sessionRunning = false;
			m_exitRequested = true;
			break;
		}

		default: break;

	}

}

bool VRSystem::beginFrame() {

	arx_assert(!m_frameActive);

	m_shouldRender = false;
	m_rendered = false;
	m_stereoRendered = false;
	m_stereoViews.clear();

	if(!m_sessionRunning) {
		return false;
	}

	XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
	m_frameState = { XR_TYPE_FRAME_STATE };
	if(!check(xrWaitFrame(m_session, &waitInfo, &m_frameState), "xrWaitFrame")) {
		return false;
	}

	XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
	if(!check(xrBeginFrame(m_session, &beginInfo), "xrBeginFrame")) {
		return false;
	}
	m_frameActive = true;

	if(!m_frameState.shouldRender) {
		return false;
	}

	XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = m_frameState.predictedDisplayTime;
	locateInfo.space = m_stageSpace;
	XrViewState viewState = { XR_TYPE_VIEW_STATE };
	uint32_t viewCount = 0;
	for(XrView & view : m_views) {
		view = { XR_TYPE_VIEW };
	}
	XrResult result = xrLocateViews(m_session, &locateInfo, &viewState, uint32_t(m_views.size()), &viewCount,
	                                m_views.data());
	if(XR_FAILED(result)) {
		
		static u64 failures = 0;
		XrTime now = getCurrentTime(m_instance);
		if(shouldLogRepeatedError(failures)) {
			LogError << "OpenXR: xrLocateViews failed (#" << failures << "): " << resultString(m_instance, result)
			         << ", predicted display time " << m_frameState.predictedDisplayTime
			         << ", period " << m_frameState.predictedDisplayPeriod << ", now " << now;
		}
		
		// Fall back to predicting one frame ahead from the current time
		if(now <= 0) {
			return false;
		}
		locateInfo.displayTime = now + std::max(m_frameState.predictedDisplayPeriod, XrDuration(0));
		for(XrView & view : m_views) {
			view = { XR_TYPE_VIEW };
		}
		viewState = { XR_TYPE_VIEW_STATE };
		if(XR_FAILED(xrLocateViews(m_session, &locateInfo, &viewState, uint32_t(m_views.size()), &viewCount,
		                           m_views.data()))) {
			return false;
		}
		
	}

	const XrViewStateFlags required = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
	if((viewState.viewStateFlags & required) != required) {
		return false;
	}

	m_shouldRender = true;
	return true;
}

unsigned VRSystem::acquireEyeImage(size_t eye) {

	arx_assert(m_shouldRender);

	EyeSwapchain & swapchain = m_swapchains[eye];
	arx_assert(!swapchain.acquired);

	XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t index = 0;
	if(!check(xrAcquireSwapchainImage(swapchain.handle, &acquireInfo, &index), "xrAcquireSwapchainImage")) {
		return 0;
	}

	XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = XR_INFINITE_DURATION;
	if(!check(xrWaitSwapchainImage(swapchain.handle, &waitInfo), "xrWaitSwapchainImage")) {
		XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(swapchain.handle, &releaseInfo);
		return 0;
	}

	swapchain.acquired = true;
	return swapchain.images[index];
}

void VRSystem::releaseEyeImage(size_t eye) {

	EyeSwapchain & swapchain = m_swapchains[eye];
	if(!swapchain.acquired) {
		return;
	}

	XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	check(xrReleaseSwapchainImage(swapchain.handle, &releaseInfo), "xrReleaseSwapchainImage");
	swapchain.acquired = false;

	XrCompositionLayerProjectionView & layerView = m_layerViews[eye];
	layerView = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
	layerView.pose = m_views[eye].pose;
	layerView.fov = m_views[eye].fov;
	layerView.subImage.swapchain = swapchain.handle;
	layerView.subImage.imageRect.offset = { 0, 0 };
	layerView.subImage.imageRect.extent = { m_eyeSize.x, m_eyeSize.y };
	layerView.subImage.imageArrayIndex = 0;

	m_rendered = true;
}

void VRSystem::mirrorWindowToEyes(const Vec2i & windowSize) {

	arx_assert(m_shouldRender);

	if(windowSize.x <= 0 || windowSize.y <= 0) {
		return;
	}

	// Save state that the renderer caches
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	GLfloat clearColor[4];
	glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
	glDisable(GL_SCISSOR_TEST);

	/*
	 * The window back buffer may be multisampled, and a multisampled blit cannot be scaled.
	 * Resolve it into a single-sampled buffer of the same size first.
	 */
	if(m_resolveSize != windowSize) {
		if(!m_resolveRenderbuffer) {
			glGenRenderbuffers(1, &m_resolveRenderbuffer);
		}
		glBindRenderbuffer(GL_RENDERBUFFER, m_resolveRenderbuffer);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, windowSize.x, windowSize.y);
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, m_resolveFramebuffer);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_resolveRenderbuffer);
		m_resolveSize = windowSize;
	}

	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolveFramebuffer);
	glBlitFramebuffer(0, 0, windowSize.x, windowSize.y, 0, 0, windowSize.x, windowSize.y,
	                  GL_COLOR_BUFFER_BIT, GL_NEAREST);

	// Show the window contents as a screen in the center of each eye
	Vec2i size(m_eyeSize.x / s32(2), m_eyeSize.x / s32(2) * windowSize.y / windowSize.x);
	Vec2i offset = (m_eyeSize - size) / s32(2);

	for(size_t eye = 0; eye < EyeCount; eye++) {

		GLuint image = acquireEyeImage(eye);
		if(!image) {
			continue;
		}

		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_blitFramebuffer);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image, 0);

		glClearColor(0.f, 0.f, 0.f, 1.f);
		glClear(GL_COLOR_BUFFER_BIT);

		// OpenGL images are bottom-up, which matches what OpenXR expects for OpenGL swapchains
		glBindFramebuffer(GL_READ_FRAMEBUFFER, m_resolveFramebuffer);
		glBlitFramebuffer(0, 0, windowSize.x, windowSize.y,
		                  offset.x, offset.y, offset.x + size.x, offset.y + size.y,
		                  GL_COLOR_BUFFER_BIT, GL_LINEAR);

		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

		releaseEyeImage(eye);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
	if(scissor) {
		glEnable(GL_SCISSOR_TEST);
	}

	GLenum error = glGetError();
	if(error != GL_NO_ERROR) {
		static bool logged = false;
		if(!logged) {
			LogError << "OpenXR: OpenGL error 0x" << std::hex << error << " while mirroring the window to the eyes";
			logged = true;
		}
	}

}

void VRSystem::endFrame() {

	if(!m_frameActive) {
		return;
	}

	arx_assert(!m_stereoActive);
	m_stereoViews.clear();

	for(size_t eye = 0; eye < EyeCount; eye++) {
		releaseEyeImage(eye);
	}

	XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	layer.space = m_stageSpace;
	layer.viewCount = uint32_t(m_layerViews.size());
	layer.views = m_layerViews.data();
	const XrCompositionLayerBaseHeader * layers[] = {
		reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer)
	};

	XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
	endInfo.displayTime = m_frameState.predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = (m_shouldRender && m_rendered) ? 1 : 0;
	endInfo.layers = layers;
	check(xrEndFrame(m_session, &endInfo), "xrEndFrame");

	m_frameActive = false;
	m_shouldRender = false;
	m_rendered = false;

}

bool VRSystem::createStereoTarget() {

	const Vec2i size(m_eyeSize.x * s32(EyeCount), m_eyeSize.y);

	GLint maxSamples = 0;
	glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
	GLsizei samples = config.video.antialiasing ? std::min(GLint(4), maxSamples) : 0;

	glGenRenderbuffers(1, &m_stereoColor);
	glBindRenderbuffer(GL_RENDERBUFFER, m_stereoColor);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, size.x, size.y);

	glGenRenderbuffers(1, &m_stereoDepth);
	glBindRenderbuffer(GL_RENDERBUFFER, m_stereoDepth);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, size.x, size.y);

	glGenRenderbuffers(1, &m_stereoResolveColor);
	glBindRenderbuffer(GL_RENDERBUFFER, m_stereoResolveColor);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, size.x, size.y);

	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	glGenFramebuffers(1, &m_stereoFramebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, m_stereoFramebuffer);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_stereoColor);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_stereoDepth);
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

	glGenFramebuffers(1, &m_stereoResolveFramebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, m_stereoResolveFramebuffer);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_stereoResolveColor);
	GLenum resolveStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if(status != GL_FRAMEBUFFER_COMPLETE || resolveStatus != GL_FRAMEBUFFER_COMPLETE) {
		LogError << "OpenXR: could not create the stereo render target (status 0x" << std::hex << status
		         << ", 0x" << resolveStatus << ')';
		destroyStereoTarget();
		return false;
	}

	LogInfo << "VR stereo render target: " << size.x << 'x' << size.y << ", " << samples << "x MSAA";

	return true;
}

void VRSystem::destroyStereoTarget() {

	arx_assert(!m_stereoActive);

	GLuint framebuffers[] = { m_stereoFramebuffer, m_stereoResolveFramebuffer };
	glDeleteFramebuffers(GLsizei(std::size(framebuffers)), framebuffers);
	GLuint renderbuffers[] = { m_stereoColor, m_stereoDepth, m_stereoResolveColor };
	glDeleteRenderbuffers(GLsizei(std::size(renderbuffers)), renderbuffers);

	m_stereoFramebuffer = m_stereoResolveFramebuffer = 0;
	m_stereoColor = m_stereoDepth = m_stereoResolveColor = 0;

}

void VRSystem::setStereoViews(std::vector<OpenGLRenderer::StereoView> views) {
	m_stereoViews = std::move(views);
}

bool VRSystem::beginStereoRender() {

	arx_assert(!m_stereoActive);

	if(!isRenderingFrame() || m_stereoViews.size() != EyeCount || !m_stereoFramebuffer) {
		return false;
	}

	OpenGLRenderer * renderer = static_cast<OpenGLRenderer *>(GRenderer);
	renderer->setRenderTarget(m_stereoFramebuffer, Vec2i(m_eyeSize.x * s32(EyeCount), m_eyeSize.y));
	renderer->setStereoViews(m_stereoViews);

	m_stereoActive = true;

	return true;
}

void VRSystem::endStereoRender(const Vec2i & windowSize) {

	if(!m_stereoActive) {
		return;
	}

	OpenGLRenderer * renderer = static_cast<OpenGLRenderer *>(GRenderer);
	renderer->setStereoViews({ });
	renderer->setRenderTarget(0, Vec2i(0));
	m_stereoActive = false;

	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	glDisable(GL_SCISSOR_TEST);

	const Vec2i size(m_eyeSize.x * s32(EyeCount), m_eyeSize.y);

	// Resolve multisampling - this cannot be combined with partial or scaled blits
	glBindFramebuffer(GL_READ_FRAMEBUFFER, m_stereoFramebuffer);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_stereoResolveFramebuffer);
	glBlitFramebuffer(0, 0, size.x, size.y, 0, 0, size.x, size.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);

	glBindFramebuffer(GL_READ_FRAMEBUFFER, m_stereoResolveFramebuffer);
	for(size_t eye = 0; eye < EyeCount; eye++) {

		GLuint image = acquireEyeImage(eye);
		if(!image) {
			continue;
		}

		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_blitFramebuffer);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image, 0);

		GLint x = GLint(eye) * m_eyeSize.x;
		glBlitFramebuffer(x, 0, x + m_eyeSize.x, m_eyeSize.y, 0, 0, m_eyeSize.x, m_eyeSize.y,
		                  GL_COLOR_BUFFER_BIT, GL_NEAREST);

		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

		releaseEyeImage(eye);
	}

	// Show the center of the left eye in the window, cropped to the window aspect ratio
	if(windowSize.x > 0 && windowSize.y > 0) {
		Vec2i crop = m_eyeSize;
		if(m_eyeSize.x * windowSize.y > windowSize.x * m_eyeSize.y) {
			crop.x = m_eyeSize.y * windowSize.x / windowSize.y;
		} else {
			crop.y = m_eyeSize.x * windowSize.y / windowSize.x;
		}
		Vec2i offset = (m_eyeSize - crop) / s32(2);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		glBlitFramebuffer(offset.x, offset.y, offset.x + crop.x, offset.y + crop.y,
		                  0, 0, windowSize.x, windowSize.y, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if(scissor) {
		glEnable(GL_SCISSOR_TEST);
	}

	m_stereoRendered = true;

}
