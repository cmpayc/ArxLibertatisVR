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
#include "vr/VRFunctions.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include "graphics/Draw.h"
#include "graphics/Renderer.h"
#include "graphics/font/Font.h"
#include "graphics/texture/Texture.h"

#ifdef __ANDROID__
#include <dlfcn.h>
#include <time.h>
#include <jni.h>
#include <SDL_system.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_TIMESPEC
#else
#include <windows.h>
#include <unknwn.h>
#define XR_USE_PLATFORM_WIN32
#endif
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include "core/Config.h"
#include "core/Core.h"
#include "core/Version.h"
#include "game/Entity.h"
#include "game/EntityManager.h"
#include "game/NPC.h"
#include "game/Player.h"
#include "game/Spells.h"
#include "gui/Dragging.h"
#include "gui/Interface.h"
#include "gui/Text.h"
#include "input/Input.h"
#include "input/Mouse.h"
#include "io/log/Logger.h"
#include "platform/ProgramOptions.h"
#include "scene/GameSound.h"
#include "scene/Interactive.h"
#include "vr/VRBody.h"
#include "vr/VRCamera.h"
#include "vr/VRCombat.h"
#include "vr/VRControls.h"
#include "vr/VRGrab.h"
#include "vr/VRGraphics.h"
#include "vr/VRHud.h"
#include "vr/VRPerf.h"


VRSystem * g_vr = nullptr;

#define ARX_XR_DEFINE_FUNCTION(name) PFN_##name name = nullptr;
PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
ARX_XR_GLOBAL_FUNCTIONS(ARX_XR_DEFINE_FUNCTION)
ARX_XR_INSTANCE_FUNCTIONS(ARX_XR_DEFINE_FUNCTION)
#undef ARX_XR_DEFINE_FUNCTION

extern long LOOKING_FOR_SPELL_TARGET; // see Spells.cpp

namespace {

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

#ifdef __ANDROID__

PFN_xrConvertTimespecTimeToTimeKHR g_convertTime = nullptr;
const char * const TimeConversionExtension = XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME;
const char * const TimeConversionFunction = "xrConvertTimespecTimeToTimeKHR";

//! \return the current OpenXR time, or 0 if not available
XrTime getCurrentTime(XrInstance instance) {
	timespec now;
	XrTime time = 0;
	if(!g_convertTime || clock_gettime(CLOCK_MONOTONIC, &now) != 0 || XR_FAILED(g_convertTime(instance, &now, &time))) {
		return 0;
	}
	return time;
}

//! \return a monotonic time in milliseconds
u64 getMilliseconds() {
	timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return u64(now.tv_sec) * 1000 + u64(now.tv_nsec) / 1000000;
}

//! The activity of the app for the OpenXR loader and runtime (a global reference)
jobject g_activity = nullptr;
JavaVM * g_javaVM = nullptr;

#else

PFN_xrConvertWin32PerformanceCounterToTimeKHR g_convertTime = nullptr;
const char * const TimeConversionExtension = XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME;
const char * const TimeConversionFunction = "xrConvertWin32PerformanceCounterToTimeKHR";

//! \return the current OpenXR time, or 0 if not available
XrTime getCurrentTime(XrInstance instance) {
	LARGE_INTEGER counter;
	XrTime time = 0;
	if(!g_convertTime || !QueryPerformanceCounter(&counter) || XR_FAILED(g_convertTime(instance, &counter, &time))) {
		return 0;
	}
	return time;
}

//! \return a monotonic time in milliseconds
u64 getMilliseconds() {
	LARGE_INTEGER counter, frequency;
	QueryPerformanceCounter(&counter);
	QueryPerformanceFrequency(&frequency);
	return u64(counter.QuadPart / (frequency.QuadPart / 1000));
}

#endif

//! Start of the stereo scene, for VRPerfScene
std::chrono::steady_clock::time_point g_stereoStart;

//! XR_EXT_performance_settings: standalone headsets run apps at low clocks unless they ask for more
PFN_xrPerfSettingsSetPerformanceLevelEXT g_setPerformanceLevel = nullptr;

//! The frame statistics in the interface layer with the "show_perf" option, empty otherwise
Rect g_perfArea;

//! Draw the frame statistics at the top left of the interface layer, one line per group
Rect drawPerfOverlay() {

	const std::string & summary = getVRPerfSummary();
	if(!config.vr.showPerf || summary.empty() || !hFontDebug) {
		return Rect();
	}

	std::vector<std::string_view> lines;
	std::string_view rest = summary;
	for(size_t separator; (separator = rest.find(" | ")) != std::string_view::npos; rest = rest.substr(separator + 3)) {
		lines.push_back(rest.substr(0, separator));
	}
	lines.push_back(rest);

	const s32 margin = 8;
	s32 lineHeight = hFontDebug->getLineHeight();
	s32 width = 0;
	for(std::string_view line : lines) {
		width = std::max(width, hFontDebug->getTextSize(line).width());
	}
	Rect area(0, 0, width + 2 * margin, s32(lines.size()) * lineHeight + 2 * margin);

	{
		// Darken behind the text, like the wrist icon backings
		UseRenderState state(render2D().blend(BlendZero, BlendSrcAlpha));
		EERIEDrawBitmap(Rectf(area), 0.f, nullptr, Color(0, 0, 0, 179));
	}
	for(size_t i = 0; i < lines.size(); i++) {
		hFontDebug->draw(Vec2i(margin, margin + s32(i) * lineHeight), lines[i], Color::white);
	}

	return area;
}

//! Rate limit for repeated per-frame errors: log the first few, then every 1000th
bool shouldLogRepeatedError(u64 & count) {
	count++;
	return count <= 5 || count % 1000 == 0;
}

//! VR turned on (1) or off (-1) on the command line, 0 to use the config
#ifdef __ANDROID__
int g_vrOverride = 1; // A standalone headset has no flat mode
#else
int g_vrOverride = 0;
#endif

void enableVR() {
	g_vrOverride = 1;
}

void disableVR() {
	g_vrOverride = -1;
}

ARX_PROGRAM_OPTION("vr", "", "Enable VR mode (OpenXR)", &enableVR)
ARX_PROGRAM_OPTION("no-vr", "", "Disable VR mode", &disableVR)

const char * resultString(XrInstance instance, XrResult result) {
	static char buffer[XR_MAX_RESULT_STRING_SIZE];
	if(instance == XR_NULL_HANDLE || !xrResultToString || XR_FAILED(xrResultToString(instance, result, buffer))) {
		std::snprintf(buffer, sizeof(buffer), "XrResult(%d)", int(result));
	}
	return buffer;
}

#ifndef __ANDROID__

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

//! Find the Meta (Oculus) OpenXR runtime manifest of the Meta Quest PC app
std::string findMetaRuntime() {

	std::string base = getEnv("OculusBase");
	if(base.empty()) {
		base = "C:\\Program Files\\Oculus\\";
	}
	if(base.back() != '\\' && base.back() != '/') {
		base += '\\';
	}

	std::string manifest = base + "Support\\oculus-runtime\\oculus_openxr_64.json";
	return readFile(manifest).empty() ? std::string() : manifest;
}

/*!
 * \return the runtime manifests to try in order, an empty string stands for the system default runtime
 *
 * With runtime=auto the system default is tried first, then the installed runtimes: the default may not support the
 * graphics API (e.g. Virtual Desktop does not support OpenGL).
 */
std::vector<std::string> getRuntimeCandidates() {

	const std::string & runtime = config.vr.runtime;

	if(runtime == "steamvr" || runtime == "meta") {
		std::string manifest = (runtime == "steamvr") ? findSteamVRRuntime() : findMetaRuntime();
		if(manifest.empty()) {
			LogWarning << "Could not locate the " << runtime << " OpenXR runtime, using the system default";
		}
		return { manifest };
	}

	if(!runtime.empty() && runtime != "auto") {
		return { runtime };
	}

	std::vector<std::string> candidates = { std::string() };
	for(const std::string & manifest : { findSteamVRRuntime(), findMetaRuntime() }) {
		if(!manifest.empty() && !readFile(manifest).empty()) {
			candidates.push_back(manifest);
		}
	}
	return candidates;
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

#endif // !__ANDROID__

#ifdef __ANDROID__

//! The system runtime of the headset (Meta Horizon OS) or of Lepton (SteamVR) through the Khronos loader
std::vector<std::string> getRuntimeCandidates() {
	return { std::string() };
}

//! Load the OpenXR loader packaged with the app and initialize it with the Java VM and the activity
bool loadOpenXR(const std::string & manifest) {

	ARX_UNUSED(manifest);

	void * loader = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
	if(!loader) {
		LogError << "OpenXR: could not load libopenxr_loader.so: " << dlerror();
		return false;
	}
	xrGetInstanceProcAddr = reinterpret_cast<PFN_xrGetInstanceProcAddr>(dlsym(loader, "xrGetInstanceProcAddr"));
	if(!xrGetInstanceProcAddr) {
		LogError << "OpenXR: libopenxr_loader.so has no xrGetInstanceProcAddr";
		return false;
	}

	if(!g_activity) {
		JNIEnv * env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
		jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
		if(!env || !activity || env->GetJavaVM(&g_javaVM) != JNI_OK) {
			LogError << "OpenXR: no Android activity";
			xrGetInstanceProcAddr = nullptr;
			return false;
		}
		g_activity = env->NewGlobalRef(activity);
		env->DeleteLocalRef(activity);
	}

	PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
	if(XR_FAILED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
	                                   reinterpret_cast<PFN_xrVoidFunction *>(&initializeLoader)))
	   || !initializeLoader) {
		LogError << "OpenXR: xrInitializeLoaderKHR not available";
		xrGetInstanceProcAddr = nullptr;
		return false;
	}
	XrLoaderInitInfoAndroidKHR initInfo = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
	initInfo.applicationVM = g_javaVM;
	initInfo.applicationContext = g_activity;
	if(XR_FAILED(initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&initInfo)))) {
		LogError << "OpenXR: xrInitializeLoaderKHR failed";
		xrGetInstanceProcAddr = nullptr;
		return false;
	}

	if(!loadFunctions(XR_NULL_HANDLE)) {
		xrGetInstanceProcAddr = nullptr;
		return false;
	}

	return true;
}

#else

//! Load a runtime from its manifest, or the system OpenXR loader (system default runtime) for an empty manifest
bool loadOpenXR(const std::string & manifest) {

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

#endif // __ANDROID__

std::vector<XrExtensionProperties> getInstanceExtensions() {

	uint32_t count = 0;
	if(XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr))) {
		return { };
	}
	std::vector<XrExtensionProperties> extensions(count, { XR_TYPE_EXTENSION_PROPERTIES });
	if(XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data()))) {
		return { };
	}
	extensions.resize(count);
	return extensions;
}

bool hasExtension(const std::vector<XrExtensionProperties> & extensions, const char * name) {
	return std::any_of(extensions.begin(), extensions.end(), [name](const XrExtensionProperties & e) {
		return std::strcmp(e.extensionName, name) == 0;
	});
}

} // anonymous namespace

bool checkXrResult(XrInstance instance, XrResult result, const char * call) {
	if(XR_FAILED(result)) {
		LogError << "OpenXR: " << call << " failed: " << resultString(instance, result);
		return false;
	}
	return true;
}

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
	, m_stereoTarget(false)
	, m_stereoActive(false)
	, m_stereoRendered(false)
	, m_uiSize(0)
	, m_uiActive(false)
	, m_uiSubmitted(false)
	, m_uiRendered(false)
	, m_uiPlaced(false)
	, m_uiFollowHead(false)
	, m_uiAwayTime(0.f)
	, m_uiYaw(0.f)
	, m_uiAnchor(0.f)
	, m_uiPose{ { 0.f, 0.f, 0.f, 1.f }, { 0.f, 0.f, 0.f } }
	, m_uiExtent{ 1.f, 1.f }
	, m_pointerOnUI(false)
	, m_lastFrameStereo(false)
	, m_imageRectFromTop(false)
	, m_triggerHoldTime(0.f)
	, m_clickPosition(0)
	, m_clickOnTarget(false)
	, m_clickOnUI(false)
	, m_clickByTrigger(false)
	, m_clickWasPressed(false)
	, m_gripIsClick(true)
	, m_clickUseOnTap(false)
	, m_useTapFrames(0)
	, m_useTapDelay(-1.f)
	, m_useTapPosition(0)
	, m_secondTap(false)
	, m_triggerOnItem(false)
	, m_magicDrawing(false)
	, m_magicMode(false)
	, m_bookOpen(false)
	, m_bookHand(VRHandLeft)
	, m_bookPixelSize(0.f)
	, m_bookCoverSize(0.f)
	, m_magicStroke(false)
	, m_magicPrecast(false)
	, m_magicCancelTime(-1.f)
	, m_magicPressTime(0.f)
	, m_magicPressPosition(0.f)
	, m_triggerItemTime(0.f)
	, m_lastItemTapAge(0.f)
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
	return g_vrOverride != 0 ? g_vrOverride > 0 : config.vr.enabled;
}

bool VRSystem::check(XrResult result, const char * call) const {
	if(result == XR_ERROR_SESSION_LOST) {
		m_sessionLost = true;
	} else if(result == XR_ERROR_INSTANCE_LOST) {
		m_instanceLost = true;
	}
	return checkXrResult(m_instance, result, call);
}

bool VRSystem::usesDirect3D11() const {
	return m_graphics && m_graphics->isDirect3D11();
}

bool VRSystem::usesVulkan() const {
	return m_graphics && m_graphics->isVulkan();
}

bool VRSystem::isPanelTextureFlipped() const {
	return m_graphics && m_graphics->isPanelCopyFlipped();
}

bool VRSystem::initInstance() {

	arx_assert(m_instance == XR_NULL_HANDLE);

	/*
	 * The graphics API of the renderer: Vulkan or Direct3D 11 if it is selected and a runtime supports it, else
	 * OpenGL (the window then creates an OpenGL renderer). The first runtime that supports it is used.
	 */
	std::vector<std::unique_ptr<VRGraphics>> apis;
	#if ARX_HAVE_VULKAN
	// Without OpenGL (Android), Vulkan is the only renderer
	if(config.video.renderer == "Vulkan" || !ARX_HAVE_OPENGL) {
		apis.push_back(createVRGraphicsVulkan());
	}
	#endif
	#if ARX_HAVE_D3D11
	// Also the automatic choice (see SDL2Window): Vulkan only when selected, it is meant for the standalone headsets
	if(config.video.renderer == "Direct3D 11" || config.video.renderer == "auto") {
		apis.push_back(createVRGraphicsD3D11());
	}
	#endif
	#if ARX_HAVE_OPENGL && ARX_PLATFORM == ARX_PLATFORM_WIN32
	apis.push_back(createVRGraphicsOpenGL());
	#endif
	std::vector<std::string> candidates = getRuntimeCandidates();
	std::vector<XrExtensionProperties> extensions;
	for(std::unique_ptr<VRGraphics> & api : apis) {
		for(const std::string & manifest : candidates) {
			if(!loadOpenXR(manifest)) {
				continue;
			}
			extensions = getInstanceExtensions();
			if(hasExtension(extensions, api->getExtension())) {
				m_graphics = std::move(api);
				break;
			}
			LogWarning << "OpenXR: " << (manifest.empty() ? "the system default runtime" : manifest)
			           << (extensions.empty() ? " is not available" : " does not support ")
			           << (extensions.empty() ? "" : api->getName());
			xrGetInstanceProcAddr = nullptr;
		}
		if(m_graphics) {
			break;
		}
	}
	if(!m_graphics) {
		LogError << "OpenXR: no runtime with Vulkan, OpenGL or Direct3D 11 support available - install SteamVR or the Meta"
		         << " Quest PC app, or select a runtime with runtime= in the [vr] config section";
		return false;
	}

	bool hasTimeConversion = hasExtension(extensions, TimeConversionExtension);
	bool hasPerformanceSettings = hasExtension(extensions, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);

	std::vector<const char *> enabledExtensions = { m_graphics->getExtension() };
	if(hasTimeConversion) {
		enabledExtensions.push_back(TimeConversionExtension);
	}
	if(hasPerformanceSettings) {
		enabledExtensions.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
	}

	XrInstanceCreateInfo createInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
	#ifdef __ANDROID__
	// Required on Android: the runtime needs the Java VM and the activity of the app
	enabledExtensions.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
	XrInstanceCreateInfoAndroidKHR androidInfo = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	androidInfo.applicationVM = g_javaVM;
	androidInfo.applicationActivity = g_activity;
	createInfo.next = &androidInfo;
	#endif
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
		xrGetInstanceProcAddr(m_instance, TimeConversionFunction,
		                      reinterpret_cast<PFN_xrVoidFunction *>(&g_convertTime));
	}
	g_setPerformanceLevel = nullptr;
	if(hasPerformanceSettings) {
		xrGetInstanceProcAddr(m_instance, "xrPerfSettingsSetPerformanceLevelEXT",
		                      reinterpret_cast<PFN_xrVoidFunction *>(&g_setPerformanceLevel));
	}

	if(!m_input.init(m_instance)) {
		LogWarning << "OpenXR: controller input is not available";
	}

	XrInstanceProperties instanceProperties = { XR_TYPE_INSTANCE_PROPERTIES };
	if(XR_SUCCEEDED(xrGetInstanceProperties(m_instance, &instanceProperties))) {
		std::ostringstream oss;
		oss << instanceProperties.runtimeName << ' '
		    << XR_VERSION_MAJOR(instanceProperties.runtimeVersion) << '.'
		    << XR_VERSION_MINOR(instanceProperties.runtimeVersion) << '.'
		    << XR_VERSION_PATCH(instanceProperties.runtimeVersion);
		m_runtimeName = oss.str();
		// The Meta runtime counts image rectangles of OpenGL swapchains from the top, SteamVR from the bottom
		m_imageRectFromTop = std::string_view(instanceProperties.runtimeName).substr(0, 6) == "Oculus";
	}

	/*
	 * The headset may not be connected yet (the Meta runtime before Quest Link or Air Link is started): wait for it
	 * like after it was disconnected, see recoverSession(). The game started without VR, and the headset showed only a
	 * black screen once it was connected (user).
	 */
	XrResult result = initSystem();
	if(result == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
		LogWarning << "OpenXR: no headset found - waiting for it, connect it to " << m_runtimeName;
		m_recoverSession = true;
		m_recoverAttempts = 0;
		m_nextRecoverTime = getMilliseconds() + 1000;
		return true;
	}
	if(XR_FAILED(result)) {
		LogError << "OpenXR: no headset found - is it connected and is " << m_runtimeName << " running?";
		shutdown();
		return false;
	}

	return true;
}

XrResult VRSystem::initSystem() {

	XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	XrResult result = xrGetSystem(m_instance, &systemInfo, &systemId);
	if(result == XR_ERROR_INSTANCE_LOST) {
		m_instanceLost = true;
	}
	if(XR_FAILED(result)) {
		return result; // Logged by the caller: it is repeated while waiting for the headset
	}
	m_systemId = systemId;

	XrSystemProperties systemProperties = { XR_TYPE_SYSTEM_PROPERTIES };
	if(XR_SUCCEEDED(xrGetSystemProperties(m_instance, m_systemId, &systemProperties))) {
		m_systemName = systemProperties.systemName;
	}

	// Direct3D 11: the renderer is created on the adapter of the headset
	if(!m_graphics->checkRequirements(m_instance, m_systemId)) {
		m_systemId = XR_NULL_SYSTEM_ID;
		return XR_ERROR_RUNTIME_FAILURE;
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
		m_systemId = XR_NULL_SYSTEM_ID;
		return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
	}
	// Sharper eyes above the recommended size (config.vr.resolutionScale), up to the maximum of the runtime
	const XrViewConfigurationView & configView = configViews[0];
	float resolutionScale = config.vr.resolutionScale;
	m_eyeSize = Vec2i(s32(float(configView.recommendedImageRectWidth) * resolutionScale + 0.5f),
	                  s32(float(configView.recommendedImageRectHeight) * resolutionScale + 0.5f));
	m_eyeSize = glm::clamp(m_eyeSize, Vec2i(16),
	                       Vec2i(s32(configView.maxImageRectWidth), s32(configView.maxImageRectHeight)));

	LogInfo << "OpenXR runtime: " << m_runtimeName << ", system: " << m_systemName
	        << ", eye resolution: " << m_eyeSize.x << 'x' << m_eyeSize.y << ", " << m_graphics->getName();

	return XR_SUCCESS;
}

void VRSystem::shutdown() {

	destroySession();

	if(m_instance != XR_NULL_HANDLE) {
		m_input.shutdown();
		xrDestroyInstance(m_instance);
		m_instance = XR_NULL_HANDLE;
	}
	m_systemId = XR_NULL_SYSTEM_ID;
	m_graphics.reset();

}

bool VRSystem::createSession() {

	arx_assert(m_instance != XR_NULL_HANDLE);
	arx_assert(m_session == XR_NULL_HANDLE);

	if(m_systemId == XR_NULL_SYSTEM_ID) {
		return false; // No headset yet, see recoverSession()
	}

	// The requirements must be queried before each xrCreateSession (SteamVR, after the headset reconnected)
	if(!m_graphics->checkRequirements(m_instance, m_systemId)) {
		return false;
	}
	const void * binding = m_graphics->getSessionBinding(m_instance, m_systemId);
	if(!binding) {
		return false;
	}

	XrSessionCreateInfo sessionInfo = { XR_TYPE_SESSION_CREATE_INFO };
	sessionInfo.next = binding;
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

	m_stereoTarget = m_graphics->createStereoTarget(m_eyeSize);
	if(!m_stereoTarget) {
		destroySession();
		return false;
	}

	m_input.attach(m_session);

	if(g_setPerformanceLevel) {
		// The highest level that can be kept up indefinitely (boost is only for loading)
		bool cpu = check(g_setPerformanceLevel(m_session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT,
		                                       XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT), "xrPerfSettingsSetPerformanceLevelEXT");
		bool gpu = check(g_setPerformanceLevel(m_session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT,
		                                       XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT), "xrPerfSettingsSetPerformanceLevelEXT");
		if(cpu && gpu) {
			LogInfo << "OpenXR: CPU and GPU performance level set to sustained high";
		}
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

	m_colorFormat = m_graphics->selectColorFormat(formats);
	if(m_colorFormat == 0) {
		LogError << "OpenXR: no supported swapchain color format";
		return false;
	}

	for(EyeSwapchain & swapchain : m_swapchains) {

		XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT
		                  | m_graphics->getExtraSwapchainUsage();
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

		if(!m_graphics->enumerateImages(swapchain.handle, swapchain.images)) {
			return false;
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

	arx_assert(!m_stereoActive);
	if(m_stereoTarget) {
		m_graphics->destroyStereoTarget();
		m_stereoTarget = false;
	}
	destroyUITarget();

	destroySwapchains();

	if(m_stageSpace != XR_NULL_HANDLE) {
		xrDestroySpace(m_stageSpace);
		m_stageSpace = XR_NULL_HANDLE;
	}

	m_input.detach();
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

			case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: {
				m_input.logInteractionProfiles();
				break;
			}

			// The user recentered with the headset (holding the system button): recenter the game too
			case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
				const auto & spaceEvent = reinterpret_cast<const XrEventDataReferenceSpaceChangePending &>(event);
				if(spaceEvent.session == m_session) {
					LogInfo << "OpenXR: reference space changed, recentering";
					requestVRRecenter();
				}
				break;
			}
			
			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
				LogWarning << "OpenXR: instance loss pending";
				m_instanceLost = true;
				break;
			}

			default: break;

		}

		event = { XR_TYPE_EVENT_DATA_BUFFER };
	}

	if(m_sessionLost || m_instanceLost) {
		handleLoss();
	}

}

void VRSystem::handleLoss() {

	LogWarning << "OpenXR: the " << (m_instanceLost ? "instance" : "session")
	           << " was lost (headset disconnected?), waiting for the headset";

	destroySession();
	m_sessionLost = false;

	if(m_instanceLost) {
		m_input.shutdown();
		xrDestroyInstance(m_instance);
		m_instance = XR_NULL_HANDLE;
		m_systemId = XR_NULL_SYSTEM_ID;
		m_instanceLost = false;
		m_sessionFailures = 0;
	}

	m_recoverSession = true;
	m_recoverAttempts = 0;
	m_nextRecoverTime = getMilliseconds() + 1000;
	// Back in the menu when the headset is back
	m_pauseRequested = true;

}

void VRSystem::recoverSession() {

	if(!m_recoverSession || m_session != XR_NULL_HANDLE || getMilliseconds() < m_nextRecoverTime) {
		return;
	}
	m_nextRecoverTime = getMilliseconds() + 1000;

	bool created = false;
	if(m_instance == XR_NULL_HANDLE) {
		m_graphics.reset();
		if(!initInstance()) {
			return;
		}
		created = true;
	}

	// The system is not available while the headset is disconnected, or before it was connected at all
	XrResult result = (created && m_systemId != XR_NULL_SYSTEM_ID) ? XR_SUCCESS : initSystem();
	if(result == XR_ERROR_INSTANCE_LOST) {
		handleLoss();
		return;
	}
	if(XR_FAILED(result)) {
		if(shouldLogRepeatedError(m_recoverAttempts)) {
			LogInfo << "OpenXR: waiting for the headset (" << resultString(m_instance, result) << ')';
		}
		return;
	}

	if(!createSession()) {
		// The instance may not be usable any more: create it again
		if(++m_sessionFailures >= 3) {
			LogWarning << "OpenXR: creating the instance again";
			m_instanceLost = true;
			handleLoss();
		}
		return;
	}
	m_sessionFailures = 0;

	m_recoverSession = false;
	// The tracking origin may have changed
	m_uiPlaced = false;
	requestVRRecenter();
	LogInfo << "OpenXR: the session was created again";

}

void VRSystem::handleSessionStateChange(XrSessionState state) {

	LogDebug("OpenXR session state " << int(m_sessionState) << " -> " << int(state));
	if(m_sessionState == XR_SESSION_STATE_FOCUSED && state != XR_SESSION_STATE_FOCUSED) {
		// The headset was taken off or the runtime shows its own menu: the game no longer gets input
		m_pauseRequested = true;
	}
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
			// The headset was disconnected (the Meta runtime): a new session is created once it is back
			LogInfo << "OpenXR session ending";
			m_sessionRunning = false;
			m_sessionLost = true;
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
	m_uiSubmitted = false;
	m_uiRendered = false;
	m_uiPanels.clear();

	if(!m_sessionRunning) {
		return false;
	}

	{
		VRPerfTimer timer(VRPerfWait);
		XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
		m_frameState = { XR_TYPE_FRAME_STATE };
		if(!check(xrWaitFrame(m_session, &waitInfo, &m_frameState), "xrWaitFrame")) {
			return false;
		}
		XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
		if(!check(xrBeginFrame(m_session, &beginInfo), "xrBeginFrame")) {
			return false;
		}
	}
	m_frameActive = true;
	countVRPerfFrame();

	/*
	 * The time to locate the head and the controllers at. After the headset reconnects the runtime's prediction can be
	 * invalid (seen: a large negative time, so nothing could be located and the controllers stopped working): then
	 * one frame period from now, like the head already fell back to. The frame itself keeps the prediction.
	 */
	m_displayTime = m_frameState.predictedDisplayTime;
	XrTime now = getCurrentTime(m_instance);
	if(m_displayTime <= 0 && now > 0) {
		if(!m_displayTimeFixed) {
			LogWarning << "OpenXR: invalid predicted display time " << m_displayTime << ", locating at the current time";
			m_pauseRequested = true; // The headset was away
		}
		m_displayTime = now + std::max(m_frameState.predictedDisplayPeriod, XrDuration(0));
		m_displayTimeFixed = true;
	} else if(m_displayTimeFixed && m_displayTime > 0) {
		LogInfo << "OpenXR: predicted display time valid again";
		m_displayTimeFixed = false;
	}

	// Controllers are read for every frame, also when nothing is rendered
	m_input.update(m_displayTime, m_stageSpace);
	// The hand motion first: the pointer checks whether the main hand touches an object (see VRGrab)
	updateVRHandMotion();
	updatePointer();
	updateVRControls();

	bool located = m_frameState.shouldRender && locateViews(now);
	updatePresence(located, now);
	if(!located) {
		return false;
	}

	m_shouldRender = true;
	return true;
}

void VRSystem::updatePresence(bool headsetLocated, XrTime now) {

	/*
	 * The headset is away when it is not located or nothing is to be rendered, or no controller is tracked: seen when
	 * the headset disconnects (Virtual Desktop), which the runtime may not report as a focus loss. After a short while
	 * the game pauses (see consumePauseRequest), once until everything is back.
	 */
	const XrDuration HeadsetGrace = 500'000'000; // ns
	const XrDuration ControllersGrace = 1'500'000'000; // ns (reaching behind the back can lose both briefly)

	bool controllers = false;
	for(VRHand hand : { VRHandLeft, VRHandRight }) {
		const VRHandState & state = m_input.getHand(hand);
		controllers = controllers || (state.active && state.grip.valid);
	}

	if(now <= 0) {
		return;
	}
	if(headsetLocated) {
		m_headsetAwaySince = 0;
	} else if(!m_headsetAwaySince) {
		m_headsetAwaySince = now;
	}
	if(controllers) {
		m_controllersAwaySince = 0;
	} else if(!m_controllersAwaySince) {
		m_controllersAwaySince = now;
	}

	if(headsetLocated && controllers) {
		m_present = true;
		return;
	}

	bool away = (m_headsetAwaySince && now - m_headsetAwaySince > HeadsetGrace)
	            || (m_controllersAwaySince && now - m_controllersAwaySince > ControllersGrace);
	if(m_present && away) {
		m_present = false;
		m_pauseRequested = true;
	}

}

bool VRSystem::locateViews(XrTime now) {

	XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = m_displayTime;
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
	return (viewState.viewStateFlags & required) == required;
}

u64 VRSystem::acquireSwapchainImage(EyeSwapchain & swapchain) {

	arx_assert(m_shouldRender);
	arx_assert(!swapchain.acquired);

	VRPerfTimer timer(VRPerfImageAcquire);
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

void VRSystem::releaseSwapchainImage(EyeSwapchain & swapchain) {

	arx_assert(swapchain.acquired);

	VRPerfTimer timer(VRPerfImageRelease);
	XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	check(xrReleaseSwapchainImage(swapchain.handle, &releaseInfo), "xrReleaseSwapchainImage");
	swapchain.acquired = false;

}

u64 VRSystem::acquireEyeImage(size_t eye) {
	return acquireSwapchainImage(m_swapchains[eye]);
}

void VRSystem::releaseEyeImage(size_t eye) {

	EyeSwapchain & swapchain = m_swapchains[eye];
	if(!swapchain.acquired) {
		return;
	}

	releaseSwapchainImage(swapchain);

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

void VRSystem::endFrame() {

	if(!m_frameActive) {
		return;
	}

	arx_assert(!m_stereoActive);
	arx_assert(!m_uiActive);
	m_stereoViews.clear();

	for(size_t eye = 0; eye < EyeCount; eye++) {
		releaseEyeImage(eye);
	}
	if(m_uiSwapchain.acquired) {
		releaseSwapchainImage(m_uiSwapchain);
	}

	std::vector<const XrCompositionLayerBaseHeader *> layers;

	XrCompositionLayerProjection projection = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	if(m_shouldRender && m_rendered) {
		projection.space = m_stageSpace;
		projection.viewCount = uint32_t(m_layerViews.size());
		projection.views = m_layerViews.data();
		layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection));
	}

	std::vector<XrCompositionLayerQuad> quads;
	if(m_shouldRender && m_uiSubmitted) {
		quads.reserve(m_uiPanels.size());
		for(const UIPanel & panel : m_uiPanels) {
			if(panel.hidden) {
				continue; // Drawn in the world instead (the book)
			}
			XrCompositionLayerQuad & quad = quads.emplace_back();
			quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
			// The virtual screen contains premultiplied alpha
			quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			quad.space = m_stageSpace;
			quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			quad.subImage.swapchain = m_uiSwapchain.handle;
			// OpenGL images start at the bottom (except for the Meta runtime, see m_imageRectFromTop)
			bool fromTop = m_graphics->areImageRectsFromTop(m_imageRectFromTop);
			int top = fromTop ? panel.area.top : m_uiSize.y - panel.area.bottom;
			quad.subImage.imageRect.offset = { panel.area.left, top };
			quad.subImage.imageRect.extent = { panel.area.width(), panel.area.height() };
			quad.subImage.imageArrayIndex = 0;
			quad.pose = panel.pose;
			quad.size = panel.extent;
			layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quad));
		}
	}

	XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
	endInfo.displayTime = m_frameState.predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = uint32_t(layers.size());
	endInfo.layers = layers.data();
	check(xrEndFrame(m_session, &endInfo), "xrEndFrame");
	m_visiblePanels.clear();
	if(m_shouldRender && m_uiSubmitted) {
		m_visiblePanels.swap(m_uiPanels);
	}
	m_lastFrameStereo = m_shouldRender && m_stereoRendered;

	m_frameActive = false;
	m_shouldRender = false;
	m_rendered = false;
	m_uiSubmitted = false;
	m_uiPanels.clear();

}

void VRSystem::setStereoViews(std::vector<StereoView> views) {
	m_stereoViews = std::move(views);
}

bool VRSystem::beginStereoRender() {

	arx_assert(!m_stereoActive);

	if(!isRenderingFrame() || m_stereoViews.size() != EyeCount || !m_stereoTarget) {
		return false;
	}

	if(!m_graphics->beginStereo(m_stereoViews)) {
		return false;
	}

	m_stereoActive = true;
	g_stereoStart = std::chrono::steady_clock::now();

	return true;
}

void VRSystem::endStereoRender(const Vec2i & windowSize) {

	if(!m_stereoActive) {
		return;
	}

	m_stereoActive = false;
	VRPerfTimer timer(VRPerfEyes);
	if(!m_graphics->endStereo(m_uiActive)) {
		return;
	}

	for(size_t eye = 0; eye < EyeCount; eye++) {
		u64 image = acquireEyeImage(eye);
		if(!image) {
			continue;
		}
		{
			VRPerfTimer copyTimer(VRPerfImageCopy);
			m_graphics->copyEyeToImage(eye, image);
		}
		releaseEyeImage(eye);
	}

	// Show the center of the left eye in the window, cropped to the window aspect ratio
	if(windowSize.x > 0 && windowSize.y > 0 && isDesktopViewShown()) {
		m_graphics->drawEyeToWindow(windowSize);
	}

	m_stereoRendered = true;
	addVRPerfTime(VRPerfScene, std::chrono::steady_clock::now() - g_stereoStart);

}

bool VRSystem::createUITarget(const Vec2i & size) {

	destroyUITarget();

	if(!m_graphics->createUITarget(size)) {
		return false;
	}

	XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT
	                  | m_graphics->getExtraSwapchainUsage();
	info.format = m_colorFormat;
	info.sampleCount = 1;
	info.width = uint32_t(size.x);
	info.height = uint32_t(size.y);
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	if(!check(xrCreateSwapchain(m_session, &info, &m_uiSwapchain.handle), "xrCreateSwapchain")) {
		m_uiSwapchain.handle = XR_NULL_HANDLE;
		destroyUITarget();
		return false;
	}

	if(!m_graphics->enumerateImages(m_uiSwapchain.handle, m_uiSwapchain.images)) {
		destroyUITarget();
		return false;
	}

	m_uiSize = size;

	LogInfo << "VR interface layer: " << size.x << 'x' << size.y;

	return true;
}

void VRSystem::destroyUITarget() {

	arx_assert(!m_uiActive);

	if(m_uiSwapchain.handle != XR_NULL_HANDLE) {
		if(m_uiSwapchain.acquired) {
			releaseSwapchainImage(m_uiSwapchain);
		}
		xrDestroySwapchain(m_uiSwapchain.handle);
	}
	m_uiSwapchain = EyeSwapchain();
	m_uiPanels.clear();
	m_visiblePanels.clear();
	m_uiSubmitted = false;

	if(m_graphics) {
		m_graphics->destroyUITarget();
	}
	for(WorldPanel * panel : { &m_bookPanel, &m_inventoryPanel }) {
		delete panel->texture;
		*panel = WorldPanel();
	}
	m_uiSize = Vec2i(0);

}

bool VRSystem::beginUIRender(const Vec2i & windowSize) {

	if(!isRenderingFrame() || m_uiActive || windowSize.x <= 0 || windowSize.y <= 0) {
		return false;
	}

	if(m_uiSize != windowSize && !createUITarget(windowSize)) {
		return false;
	}

	m_graphics->beginUI();

	m_uiActive = true;
	m_heldCaptured = HeldNone;

	return true;
}

void VRSystem::drawWristIconBackings() {

	/*
	 * Each icon at the wrist (gauges, status icons) gets a round backing like the menus at the hands, and is 30%
	 * transparent (more visible than the menus). Painted after the interface: the backing under what is already drawn
	 * (where it does not cover yet), then the coverage of the whole icon area scaled to 70% (the image has premultiplied
	 * alpha).
	 */
	VRHudAreas areas = getVRHudAreas();
	const Rect * icons[] = { &areas.gauges, &areas.equipment, &areas.stealth, &areas.levelUp, &areas.changeLevel };

	m_graphics->setPremultipliedAlphaOutput(false); // The blending below applies to all channels
	GRenderer->ResetTexture(0);
	{
		UseRenderState state(render2D().blend(BlendInvDstAlpha, BlendOne));
		const Color backing(12, 12, 20, 200); // Premultiplied, like the menus at the hands
		const size_t Segments = 32;
		for(const Rect * icon : icons) {
			if(icon->empty()) {
				continue;
			}
			Vec2f center = Rectf(*icon).center();
			float radius = 0.5f * float(std::min(icon->width(), icon->height()));
			TexturedVertex fan[Segments + 2];
			fan[0] = TexturedVertex(Vec3f(center, 0.f), 1.f, backing.toRGBA(), Vec2f(0.f));
			for(size_t i = 0; i <= Segments; i++) {
				float angle = glm::two_pi<float>() * float(i) / float(Segments);
				Vec2f rim = center + Vec2f(std::cos(angle), std::sin(angle)) * radius;
				fan[i + 1] = TexturedVertex(Vec3f(rim, 0.f), 1.f, backing.toRGBA(), Vec2f(0.f));
			}
			EERIEDRAWPRIM(Renderer::TriangleFan, fan, Segments + 2);
		}
	}
	{
		UseRenderState state(render2D().blend(BlendZero, BlendSrcAlpha));
		for(const Rect * icon : icons) {
			if(!icon->empty()) {
				// The stealth icon in the view is half transparent (user request), see updateViewIconPanels
				u8 alpha = (icon == &areas.stealth) ? 128 : 179;
				EERIEDrawBitmap(Rectf(*icon), 0.f, nullptr, Color(0, 0, 0, alpha));
			}
		}
		// The map too (user request), without a backing
		if(!areas.map.empty()) {
			EERIEDrawBitmap(Rectf(areas.map), 0.f, nullptr, Color(0, 0, 0, 179));
		}
	}
	m_graphics->setPremultipliedAlphaOutput(true);

}

void VRSystem::endUIRender(const Vec2i & windowSize, bool overScene) {

	if(!m_uiActive) {
		return;
	}

	m_inventoryPanel.active = false; // Until it is shown again (see updateWristPanels)
	if(overScene && !config.vr.showHud) {
		drawWristIconBackings();
	}
	g_perfArea = drawPerfOverlay();

	m_graphics->endUI();
	m_uiActive = false;
	m_uiRendered = true;

	u64 image = acquireSwapchainImage(m_uiSwapchain);
	if(image) {
		{
			VRPerfTimer copyTimer(VRPerfImageCopy);
			m_graphics->copyUIToImage(image);
		}
		releaseSwapchainImage(m_uiSwapchain);
	}

	if(!overScene) {
		// Desktop view: the interface on its own for menus
		drawUIToWindow(windowSize, true);
		if(image) {
			updateUIPlacement(true); // Following the head (user request), like the panels in the game
			addScreenPanel(Rect(m_uiSize.x, m_uiSize.y), true);
		}
		return;
	}
	if(!image) {
		drawUIToWindow(windowSize, false);
		return;
	}

	// In-game the desktop view shows the panels over the left eye image, like the headset
	updateGamePanels();
	if(config.vr.showHud) {
		drawUIToWindow(windowSize, false);
	} else {
		drawPanelsToWindow(windowSize);
	}

}

void VRSystem::updateGamePanels() {

	/*
	 * In-game the HUD is attached to the wrists and only the open interface (book, notes, containers) is shown
	 * in front, see VRHud. While holding an object the panel in front is hidden, so that the object does not
	 * switch to inventory drag and drop.
	 */
	VRHudAreas areas = getVRHudAreas();
	m_otherInventoryArea = areas.otherInventory;
	updateBookPanel(areas);
	if(!g_perfArea.empty()) {
		// The frame statistics at the top of the view
		updateUIPlacement(true);
		addScreenPanel(g_perfArea, false, true);
	}
	// The texts above and below the view do not depend on the book or a note being held (user request)
	if(!areas.subtitles.empty()) {
		// Subtitles of in-game cutscenes: below the view, where they are on the screen
		updateUIPlacement(true);
		addScreenPanel(areas.subtitles, false, true);
	}
	if(!areas.info.empty()) {
		// Names of pointed objects and notifications: above the view, where they are on the screen
		updateUIPlacement(true);
		addScreenPanel(areas.info, false, true);
	}
	if(m_bookOpen) {
		return; // Otherwise only the book while it is open (user request)
	}
	if(!areas.screen.empty() && !isVRGrabbing()) {
		updateUIPlacement(true);
		// Only an open container in the game: twice as big, its items were too small to pick (user request)
		const float ContainerScale = 2.f;
		addScreenPanel(areas.screen, isVRInterfaceOpen(), false, config.vr.showHud ? 1.f : ContainerScale);
	}
	updateWristPanels(areas);
	updateLootPanel(areas);
	updateLabelPanel(areas);
	updateViewIconPanels(areas);

}

void VRSystem::addScreenPanel(const Rect & area, bool interactive, bool keepPosition, float scale) {

	// The same pixel size as the whole virtual screen, centered where the screen is
	float pixelSize = m_uiExtent.width / float(m_uiSize.x) * scale;
	glm::quat orientation(m_uiPose.orientation.w, m_uiPose.orientation.x,
	                      m_uiPose.orientation.y, m_uiPose.orientation.z);
	Vec3f position(m_uiPose.position.x, m_uiPose.position.y, m_uiPose.position.z);
	if(keepPosition) {
		// As high as the area is on the whole virtual screen (screen y points down), centered
		float offset = (float(area.center().y) - float(m_uiSize.y) * 0.5f) * pixelSize;
		position += orientation * Vec3f(0.f, -offset, 0.f);
	}
	addUIPanel(area, pixelSize, position, orientation, interactive);

}

bool VRSystem::addUIPanel(const Rect & area, float pixelSize, const Vec3f & position,
                          const glm::quat & orientation, bool interactive) {

	Rect visible = area & Rect(m_uiSize.x, m_uiSize.y);
	if(visible.empty() || pixelSize <= 0.f) {
		return false;
	}

	UIPanel & panel = m_uiPanels.emplace_back();
	panel.area = visible;
	panel.pose.orientation = { orientation.x, orientation.y, orientation.z, orientation.w };
	panel.pose.position = { position.x, position.y, position.z };
	panel.extent = { float(visible.width()) * pixelSize, float(visible.height()) * pixelSize };
	panel.interactive = interactive;
	m_uiSubmitted = true;
	return true;

}

void VRSystem::updateWristPanels(const VRHudAreas & areas) {

	const VRHand hudHand = config.vr.leftHanded ? VRHandRight : VRHandLeft;

	const XrPosef & left = m_views[0].pose;
	const XrPosef & right = m_views[1].pose;
	Vec3f head = (Vec3f(left.position.x, left.position.y, left.position.z)
	              + Vec3f(right.position.x, right.position.y, right.position.z)) * 0.5f;

	// Sizes in metres per interface pixel
	const float GaugePixelSize = 0.0003f;
	const float InventoryPixelSize = 0.0005f;
	const float MapPixelSize = 0.001f;
	const float RunePixelSize = 0.0006f; // A rune icon of 32 pixels is about 2 cm
	const float Gap = 0.005f;
	// Rotation of the panels on the left wrist, clockwise (mirrored on the right wrist)
	const float Tilt = glm::radians(45.f);
	// The health and mana gauges are turned back counter-clockwise by this (user request)
	const float GaugesTilt = glm::radians(-45.f);
	// The inventory is turned counter-clockwise by this (user request: its left side lower)
	const float InventoryTilt = glm::radians(-15.f);
	// The inventory is further out from the wrist than the panels were (user request)
	const float InventoryOut = 0.0175f;
	// The map is turned clockwise by this (user request; the tilt is mirrored on the right hand, see Mount)
	const float MapTilt = glm::radians(-45.f);
	// Sideways shift of the panels: to the left on the left wrist, to the right on the right wrist
	const float Shift = 0.025f;
	// Where the icons and the map are on the forearm: back from the wrist joint, out from the bone
	const float ForearmBack = 0.045f;
	const float ForearmAbove = 0.015f;
	// The inventory is above where the row of gauges was at the wrist (half the height of a gauge)
	const float InventoryBase = 0.5f * 80.f * GaugePixelSize;

	//! Where panels are attached: a point and the directions of the panels there, facing out of `normal`
	struct Mount {
		Vec3f position;
		Vec3f across; // Left to right when reading
		Vec3f up;
		Vec3f normal;
		float side; // 1 on the left hand, -1 on the right hand
		[[nodiscard]] glm::quat orientation(float extraTilt = 0.f) const {
			Vec3f turnedUp = up * std::cos(extraTilt * side) + across * std::sin(extraTilt * side);
			return glm::quat_cast(glm::mat3x3(glm::cross(turnedUp, normal), turnedUp, normal));
		}
	};

	for(size_t i = 0; i < VRHandCount; i++) {

		const VRHand hand = VRHand(i);
		const VRHandState & state = m_input.getHand(hand);
		bool isHudHand = (hand == hudHand);
		if(!state.grip.valid || (isHudHand ? (areas.gauges.empty() && areas.inventory.empty())
		                                   : (areas.map.empty() && areas.runes.empty()))) {
			continue;
		}

		/*
		 * Like a watch on the inside of the wrist, fixed to it (OpenXR grip frame: x points out of the left palm and
		 * into the right palm, z backwards along the forearm). The top of the panels points to the fingers, turned by
		 * the tilt.
		 */
		Vec3f wristJoint;
		glm::quat grip;
		if(!getWristFrame(hand, wristJoint, grip)) {
			continue;
		}
		float side = (hand == VRHandLeft) ? 1.f : -1.f;
		auto mountAt = [&](const Vec3f & joint, const Vec3f & fingers, const Vec3f & normal, const Vec3f & offset) {
			Vec3f fingersRight = glm::cross(fingers, normal);
			float tilt = Tilt * side;
			Mount mount;
			mount.up = fingers * std::cos(tilt) + fingersRight * std::sin(tilt);
			mount.across = glm::cross(mount.up, normal);
			mount.normal = normal;
			mount.position = joint + offset;
			mount.side = side;
			return mount;
		};
		Vec3f wristNormal = grip * Vec3f(side, 0.f, 0.f);
		Vec3f wristFingers = grip * Vec3f(0.f, 0.f, -1.f);
		Mount wrist = mountAt(wristJoint, wristFingers, wristNormal,
		                      grip * Vec3f(0.f, 0.f, 0.03f) + wristNormal * 0.025f);

		/*
		 * The icons and the map are on the forearm, a little back from the wrist (user request), so that they do not
		 * turn when the hand bends: along the forearm, facing the palm side (they turn with it). The inventory stays
		 * at the wrist.
		 */
		Mount forearm = wrist;
		Vec3f elbow;
		Vec3f joint;
		glm::quat unused;
		Vec3f elbowPosition;
		Vec3f jointPosition;
		if(getVRForearm(hand, elbow, joint)
		   && worldPoseToTracking(elbow, glm::quat(1.f, 0.f, 0.f, 0.f), elbowPosition, unused)
		   && worldPoseToTracking(joint, glm::quat(1.f, 0.f, 0.f, 0.f), jointPosition, unused)
		   && glm::distance(elbowPosition, jointPosition) > 0.05f) {
			Vec3f along = glm::normalize(jointPosition - elbowPosition);
			Vec3f palm = wristNormal - along * glm::dot(wristNormal, along);
			if(glm::length(palm) > 0.1f) {
				palm = glm::normalize(palm);
				forearm = mountAt(jointPosition, along, palm, -along * ForearmBack + palm * ForearmAbove);
			}
		}

		// Like the body, it can only be seen from the front (quad layers are not hidden by the arm)
		auto visible = [&](const Mount & mount) {
			return glm::dot(mount.normal, head - mount.position) > 0.f;
		};
		auto add = [&](const Mount & mount, const Rect & area, float pixelSize, Vec2f center, bool interactive,
		               float extraTilt = 0.f) {
			Vec3f position = mount.position + mount.across * (center.x - Shift * side) + mount.up * center.y;
			if(!addUIPanel(area, pixelSize, position, mount.orientation(extraTilt), interactive)) {
				return false;
			}
			m_uiPanels.back().noBeam = true;
			return true;
		};
		auto size = [](const Rect & area, float pixelSize) {
			return area.empty() ? Vec2f(0.f) : Vec2f(float(area.width()), float(area.height())) * pixelSize;
		};

		if(!isHudHand) {
			if(visible(forearm)) {
				add(forearm, areas.map, MapPixelSize / areas.mapScale, Vec2f(0.f), false, MapTilt);
			}
			// The runes of the spell being drawn at the wrist, like a watch, while they are shown (user request)
			if(visible(wrist)) {
				add(wrist, areas.runes, RunePixelSize / areas.hudScale, Vec2f(0.f), false);
			}
			continue;
		}

		/*
		 * The health and mana gauges (in one circle, turned back), the damaged equipment status and the stealth, level
		 * up and stairs icons in a row on the forearm, centered vertically
		 */
		float gaugePixelSize = GaugePixelSize / areas.hudScale;
		if(visible(forearm)) {
			// The stealth icon is in the view instead (see updateViewIconPanels)
			const Rect * row[] = { &areas.gauges, &areas.equipment, &areas.levelUp, &areas.changeLevel };
			float rowWidth = 0.f;
			for(const Rect * area : row) {
				Vec2f extent = size(*area, gaugePixelSize);
				if(extent.x > 0.f) {
					rowWidth += (rowWidth > 0.f ? Gap : 0.f) + extent.x;
				}
			}
			// Shifted to the right by half the gauges circle (user request)
			float x = -0.5f * rowWidth + 0.5f * size(areas.gauges, gaugePixelSize).x;
			for(const Rect * area : row) {
				Vec2f extent = size(*area, gaugePixelSize);
				if(extent.x > 0.f) {
					bool gauges = (area == &areas.gauges);
					// Not clicked (user request: only the inventory on the wrist is)
					add(forearm, *area, gaugePixelSize, Vec2f(x + 0.5f * extent.x, 0.f), false,
					    gauges ? GaugesTilt : 0.f);
					x += extent.x + Gap;
				}
			}
		}

		// The inventory at the wrist, as before the icons moved to the forearm
		Mount inventoryMount = wrist;
		inventoryMount.position += wrist.normal * InventoryOut;
		if(!areas.inventory.empty() && visible(inventoryMount)) {
			float inventoryPixelSize = InventoryPixelSize / areas.hudScale;
			Vec2f inventory = size(areas.inventory, inventoryPixelSize);
			Rect visibleArea = areas.inventory & Rect(m_uiSize.x, m_uiSize.y);
			if(add(inventoryMount, visibleArea, inventoryPixelSize, Vec2f(0.f, InventoryBase + Gap + 0.5f * inventory.y), true,
			       InventoryTilt)
			   && copyToWorldPanel(m_inventoryPanel, visibleArea, inventoryPixelSize)) {
				/*
				 * Drawn in the world (see drawVRHandMenus), so that the hands are in front of it (user request),
				 * where it is relative to the wrist now: the panel here is only pointed at
				 */
				UIPanel & panel = m_uiPanels.back();
				panel.useOnTap = true;
				panel.hidden = true;
				Vec3f position(panel.pose.position.x, panel.pose.position.y, panel.pose.position.z);
				glm::quat orientation(panel.pose.orientation.w, panel.pose.orientation.x, panel.pose.orientation.y,
				                      panel.pose.orientation.z);
				glm::quat toWrist = glm::inverse(grip);
				m_inventoryPanel.hand = hand;
				m_inventoryPanel.localPosition = toWrist * (position - wristJoint);
				m_inventoryPanel.localOrientation = toWrist * orientation;
			}
		}

	}

}

void VRSystem::updateLootPanel(const VRHudAreas & areas) {

	if(areas.loot.empty() || !areas.lootEntity) {
		return;
	}

	// On top of the body (Arx world: y points down)
	const Entity & body = *areas.lootEntity;
	Vec3f top = body.pos;
	if(body.bbox3D.min.x <= body.bbox3D.max.x) {
		top = (body.bbox3D.min + body.bbox3D.max) * 0.5f;
		top.y = body.bbox3D.min.y;
	}
	top.y -= 5.f;
	Vec3f position;
	glm::quat unused;
	if(!worldPoseToTracking(top, glm::quat(1.f, 0.f, 0.f, 0.f), position, unused)) {
		return;
	}

	const XrPosef & left = m_views[0].pose;
	const XrPosef & right = m_views[1].pose;
	Vec3f head = (Vec3f(left.position.x, left.position.y, left.position.z)
	              + Vec3f(right.position.x, right.position.y, right.position.z)) * 0.5f;

	// Lying flat with its top away from the user, tilted up a little towards the user to be readable
	Vec3f away = position - head;
	away.y = 0.f;
	if(glm::length(away) < 0.01f) {
		glm::quat orientation(left.orientation.w, left.orientation.x, left.orientation.y, left.orientation.z);
		away = orientation * Vec3f(0.f, 0.f, -1.f);
		away.y = 0.f;
		if(glm::length(away) < 0.01f) {
			return;
		}
	}
	away = glm::normalize(away);
	const float Tilt = glm::radians(30.f);
	Vec3f normal = Vec3f(0.f, 1.f, 0.f) * std::cos(Tilt) - away * std::sin(Tilt);
	Vec3f up = glm::normalize(away - normal * glm::dot(away, normal));
	Vec3f across = glm::cross(up, normal);
	glm::quat orientation = glm::quat_cast(glm::mat3x3(across, up, normal));

	// Meters per interface pixel: a sheet of about 60 cm (30 cm was too small to pick the items, user request)
	const float LootPixelSize = 0.0024f;
	if(addUIPanel(areas.loot, LootPixelSize / areas.hudScale, position, orientation, true)) {
		m_uiPanels.back().noBeam = true; // The cursor on it is enough (user request)
	}

}

bool VRSystem::copyToWorldPanel(WorldPanel & panel, const Rect & area, float pixelSize) {

	panel.active = false;
	Vec2i size(area.width(), area.height());
	if(area.empty() || !m_graphics->hasUITarget()) {
		return false;
	}
	if(!panel.texture || panel.textureSize != size) {
		delete panel.texture;
		panel.texture = GRenderer->createTexture();
		if(!panel.texture->create(size_t(size.x), size_t(size.y), Image::Format_R8G8B8A8)) {
			delete panel.texture, panel.texture = nullptr;
			return false;
		}
		panel.texture->upload(); // Allocates it
		panel.textureSize = size;
	}

	// Upside down with OpenGL, see isPanelTextureFlipped()
	if(!m_graphics->copyUIToTexture(panel.texture, area)) {
		return false;
	}
	panel.size = Vec2f(size) * pixelSize;
	panel.active = true;
	return true;
}

bool VRSystem::getWristFrame(VRHand hand, Vec3f & joint, glm::quat & grip) const {

	const VRHandState & state = m_input.getHand(hand);
	if(!state.grip.valid) {
		return false;
	}
	joint = state.grip.position + state.grip.orientation * config.vr.handOffset;
	grip = state.grip.orientation;
	// Attached to the visible body: the model wrist lags behind the controller while walking
	Vec3f modelWrist;
	glm::quat modelGrip;
	if(getVRWristPose(hand, modelWrist, modelGrip)) {
		Vec3f position;
		glm::quat orientation;
		if(worldPoseToTracking(modelWrist, modelGrip, position, orientation)) {
			joint = position;
			grip = orientation;
		}
	}
	return true;
}

bool VRSystem::getInventoryPanel(Texture *& texture, Vec3f & position, glm::quat & orientation,
                                 Vec2f & size) const {

	Vec3f joint;
	glm::quat grip;
	if(!m_inventoryPanel.active || !m_inventoryPanel.texture || !getWristFrame(m_inventoryPanel.hand, joint, grip)) {
		return false;
	}

	// Rigidly attached to the wrist, as it was placed at the last update (see updateWristPanels)
	position = joint + grip * m_inventoryPanel.localPosition;
	orientation = grip * m_inventoryPanel.localOrientation;

	// Like the body, it can only be seen from the front
	const XrPosef & left = m_views[0].pose;
	const XrPosef & right = m_views[1].pose;
	Vec3f head = (Vec3f(left.position.x, left.position.y, left.position.z)
	              + Vec3f(right.position.x, right.position.y, right.position.z)) * 0.5f;
	if(glm::dot(orientation * Vec3f(0.f, 0.f, 1.f), head - position) <= 0.f) {
		return false;
	}

	texture = m_inventoryPanel.texture;
	size = m_inventoryPanel.size;
	return true;
}

bool VRSystem::isDesktopViewShown() {
	// As the game started: changing the setting takes effect after a restart (like the window it was created with)
	#ifdef __ANDROID__
	return false; // Only the OpenXR layers are shown, see PresentToWindow in VulkanRenderer
	#else
	static const bool shown = (config.vr.desktopView != "off");
	return shown;
	#endif
}

VRHand VRSystem::getPointerHand() const {
	VRHand main = config.vr.leftHanded ? VRHandLeft : VRHandRight;
	if(m_bookOpen && m_bookHand == main) {
		return (main == VRHandLeft) ? VRHandRight : VRHandLeft;
	}
	return main;
}

bool VRSystem::getBookPose(Vec3f & position, glm::quat & orientation, Vec2f & coverSize) const {

	// m from the corner of the cover to the controller: the hand holds the very edge, a little up from the bottom
	const float InsetAcross = -0.015f;
	const float InsetUp = 0.035f;
	// m along the facing direction (its z axis, partly towards the thumb, user request)
	const float Depth = 0.032f;

	const VRHandState & hand = m_input.getHand(m_bookHand);
	if(!m_bookOpen || !hand.grip.valid || m_bookPixelSize <= 0.f) {
		return false;
	}

	/*
	 * Held by its bottom corner on the side of the hand (user request) and turning with the hand: the left hand at the
	 * bottom left, the right hand at the bottom right. Upright against the controller's y axis (user request), the
	 * pages facing along its forward axis (-z): turned half around its x axis. The top leans towards the user (user
	 * request).
	 */
	const float Lean = glm::radians(40.f);

	// Attached to the visible hand: the model wrist lags behind the controller while walking or reaching far
	Vec3f gripPosition = hand.grip.position;
	glm::quat gripOrientation = hand.grip.orientation;
	Vec3f modelWrist;
	glm::quat modelGrip;
	if(getVRWristPose(m_bookHand, modelWrist, modelGrip)) {
		Vec3f wrist;
		glm::quat wristOrientation;
		if(worldPoseToTracking(modelWrist, modelGrip, wrist, wristOrientation)) {
			gripOrientation = wristOrientation;
			gripPosition = wrist - wristOrientation * config.vr.handOffset; // The wrist is at this offset from the grip
		}
	}

	orientation = gripOrientation * glm::angleAxis(glm::pi<float>(), Vec3f(1.f, 0.f, 0.f))
	              * glm::angleAxis(Lean, Vec3f(1.f, 0.f, 0.f));
	Vec3f across = orientation * Vec3f(1.f, 0.f, 0.f);
	Vec3f up = orientation * Vec3f(0.f, 1.f, 0.f);
	float side = (m_bookHand == VRHandLeft) ? 1.f : -1.f;
	position = gripPosition + across * (side * (0.5f * m_bookCoverSize.x - InsetAcross))
	           + up * (0.5f * m_bookCoverSize.y - InsetUp) + orientation * Vec3f(0.f, 0.f, Depth);
	coverSize = m_bookCoverSize;
	return true;
}

void VRSystem::updateBookPanel(const VRHudAreas & areas) {

	/*
	 * The open book is held in a hand like a real book (user request): the left hand when it opens, the other hand
	 * points at it. A grip of the pointing hand close to the book takes it into that hand (see updatePointer).
	 */
	const float Width = 0.34f; // m, with the bookmarks

	/*
	 * An open note (or a book of the game world) is held the same way (user request), as a sheet without a cover, at
	 * the scale of the book: the sizes relative to the book are kept
	 */
	bool note = areas.book.empty() && !areas.note.empty();
	const Rect & area = note ? areas.note : areas.book;
	bool open = !area.empty() && (!note || areas.bookWidth > 0);
	if(open && (!m_bookOpen || note != m_heldNote)) {
		m_bookHand = config.vr.leftHanded ? VRHandRight : VRHandLeft;
		stowVRHeldItems(); // Both hands free: the weapon to the back, the torch to the belt
	}
	m_bookOpen = open;
	m_heldNote = note;
	if(!open) {
		return;
	}

	if(note) {
		m_bookPixelSize = Width / float(areas.bookWidth);
		m_bookCoverSize = Vec2f(float(area.width()), float(area.height())) * m_bookPixelSize;
	} else {
		m_bookPixelSize = Width / float(area.width());
		float margin = 2.f * 24.f * areas.hudScale; // The bookmarks around the cover, see getVRHudAreas
		m_bookCoverSize = Vec2f(float(area.width()) - margin, float(area.height()) - margin) * m_bookPixelSize;
	}

	/*
	 * The book is drawn in the world (see drawVRHandMenus), so that the hand holding it is in front of it: a quad
	 * layer would cover the hand. Its part of the interface is copied into a texture for that, and its panel is only
	 * pointed at.
	 */
	Rect visible = area & Rect(m_uiSize.x, m_uiSize.y);
	if(m_heldCaptured != (note ? HeldNote : HeldBook) && !copyToWorldPanel(m_bookPanel, visible, m_bookPixelSize)) {
		return;
	}

	Vec3f position;
	glm::quat orientation;
	Vec2f coverSize;
	if(getBookPose(position, orientation, coverSize)) {
		if(addUIPanel(visible, m_bookPixelSize, position, orientation, true)) {
			m_uiPanels.back().noBeam = true; // The cursor on it is enough (user request)
			m_uiPanels.back().hidden = true;
		}
	}

}

Entity * VRSystem::getGripTarget(VRHand handId, Entity * target) const {

	/*
	 * The grip activates what it points at like the game's double click: levers, doors, containers, the loot of dead
	 * bodies, talking to NPCs (not with a drawn weapon). Items are taken and grabbed instead (see VRGrab).
	 */
	const float UseDistance = 360.f; // Like the game's clicks on objects
	if(!target) {
		target = getVRPointedEntity(m_input.getHand(handId).aim, false);
		if(target && glm::distance(player.pos, target->pos) >= UseDistance) {
			target = nullptr;
		}
	}
	if(!target || (target->ioflags & IO_ITEM)) {
		return nullptr;
	}
	bool alive = (target->ioflags & IO_NPC) && target->_npcdata->lifePool.current > 0.f;
	bool drawn = (player.Interface & INTER_COMBATMODE) != 0;
	return (alive && drawn) ? nullptr : target;
}

bool VRSystem::isUseTarget(const Entity & entity) const {
	return m_offHandUseTarget != EntityHandle() && m_offHandUseTarget == entity.index();
}

bool VRSystem::activateWithGrip(VRHand handId, Entity * target) {

	target = getGripTarget(handId, target);
	if(!target) {
		return false;
	}
	bool drawn = (player.Interface & INTER_COMBATMODE) != 0;
	// The game closes containers in combat mode: out of it, with the weapon kept in the hand
	if(drawn && !suspendVRCombatMode()) {
		ARX_INTERFACE_setCombatMode(COMBAT_MODE_OFF);
	}
	ARX_INTERFACE_ActivateEntity(*target);
	m_input.vibrate(handId, 0.4f, 0.05f);
	return true;
}

bool VRSystem::getImprovedRunePoint(const Vec3f & tip, Vec2f & screen) {

	/*
	 * Improved rune drawing (experimental, [vr] improved_rune_drawing, user request): the stroke is seen through the
	 * current view, so moving or tilting the head bent or turned a rune, the tracking jitter added zigzags and the
	 * release of the trigger a tail. Instead the stroke is drawn on a canvas fixed when it starts (the view direction
	 * then, without the head roll), smoothed, and passed to the game 50 ms late, so that the release does not reach it.
	 * All in the tracking space: walking or turning while drawing does not matter either.
	 */
	const float SmoothingRate = 40.f; // 1/s: about 25 ms
	const float Delay = 0.05f; // s
	const float ScaleProbe = 0.1f; // m

	RuneCanvas & canvas = m_runeCanvas;
	float dt = getFrameDuration();

	if(!canvas.valid) {
		// The canvas: the view direction now, level (no roll), and the screen scale at the hand
		const XrPosef & view = m_views[0].pose;
		glm::quat head(view.orientation.w, view.orientation.x, view.orientation.y, view.orientation.z);
		Vec3f forward = head * Vec3f(0.f, 0.f, -1.f);
		Vec3f right = glm::cross(forward, Vec3f(0.f, 1.f, 0.f));
		if(glm::length(right) < 0.1f) {
			return false; // Looking straight up or down
		}
		right = glm::normalize(right);
		Vec3f up = glm::normalize(glm::cross(right, forward));
		Vec3f world, worldProbe;
		Vec2f start, probe;
		if(!trackingPointToWorld(tip, world) || !worldToVRScreen(world, start)
		   || !trackingPointToWorld(tip + right * ScaleProbe, worldProbe) || !worldToVRScreen(worldProbe, probe)) {
			return false;
		}
		float pixelsPerMeter = glm::length(probe - start) / ScaleProbe;
		if(pixelsPerMeter <= 0.f) {
			return false;
		}
		canvas.valid = true;
		canvas.right = right;
		canvas.up = up;
		canvas.origin = tip;
		canvas.screenOrigin = start;
		canvas.pixelsPerMeter = pixelsPerMeter;
		canvas.smoothed = tip;
		canvas.time = 0.f;
		canvas.history.clear();
	}

	canvas.smoothed += (tip - canvas.smoothed) * (1.f - std::exp(-dt * SmoothingRate));
	canvas.time += dt;
	Vec3f offset = canvas.smoothed - canvas.origin;
	Vec2f point = canvas.screenOrigin
	              + Vec2f(glm::dot(offset, canvas.right), -glm::dot(offset, canvas.up)) * canvas.pixelsPerMeter;
	canvas.history.emplace_back(canvas.time, point);

	// The newest point at least the delay old (the first one until then)
	while(canvas.history.size() > 1 && canvas.history[1].first <= canvas.time - Delay) {
		canvas.history.pop_front();
	}
	screen = canvas.history.front().second;
	return true;
}

void VRSystem::captureHeldPanel(bool book) {

	/*
	 * The interface is one picture cut into panels: what the game draws after the book or a note over its area (texts,
	 * the mini map, the cursor) appeared in the held sheet, and the panels for those showed parts of the sheet (user).
	 * Right after the book (with its rune flares) or a note is drawn, its area is copied into the texture of the held
	 * sheet and cleared, for the rest to be drawn alone.
	 */
	if(!m_uiActive || !m_graphics->hasUITarget()) {
		return;
	}
	VRHudAreas areas = getVRHudAreas();
	const Rect & area = book ? areas.book : areas.note;
	if(area.empty() || (!book && (!areas.book.empty() || areas.bookWidth <= 0))) {
		return;
	}

	const float Width = 0.34f; // m, as in updateBookPanel
	float pixelSize = Width / float(book ? area.width() : areas.bookWidth);
	Rect visible = area & Rect(m_uiSize.x, m_uiSize.y);
	if(visible.empty() || !copyToWorldPanel(m_bookPanel, visible, pixelSize)) {
		return;
	}
	m_heldCaptured = book ? HeldBook : HeldNote;

	GRenderer->Clear(Renderer::ColorBuffer, Color(0, 0, 0, 0), 1.f, 1, &visible);
}

void VRSystem::updateViewIconPanels(const VRHudAreas & areas) {

	// The stealth icon in the lower left of the view, following the head (user request)
	if(areas.stealth.empty()) {
		return;
	}

	const Vec3f Offset(-0.38f, -0.32f, -1.f); // m in the view: left, down and ahead
	const float Width = 0.05f; // m

	const XrPosef & left = m_views[0].pose;
	const XrPosef & right = m_views[1].pose;
	Vec3f head = (Vec3f(left.position.x, left.position.y, left.position.z)
	              + Vec3f(right.position.x, right.position.y, right.position.z)) * 0.5f;
	glm::quat orientation(left.orientation.w, left.orientation.x, left.orientation.y, left.orientation.z);
	addUIPanel(areas.stealth, Width / float(areas.stealth.width()), head + orientation * Offset, orientation, false);

}

void VRSystem::updateLabelPanel(const VRHudAreas & areas) {

	if(areas.label.empty() || !areas.labelEntity || m_uiSize.x <= 0) {
		return;
	}

	// Just above the object (Arx world: y points down)
	const Entity & entity = *areas.labelEntity;
	Vec3f top = entity.pos;
	if(entity.bbox3D.min.x <= entity.bbox3D.max.x) {
		top = (entity.bbox3D.min + entity.bbox3D.max) * 0.5f;
		top.y = entity.bbox3D.min.y;
	}
	top.y -= 10.f;
	Vec3f position;
	glm::quat unused;
	if(!worldPoseToTracking(top, glm::quat(1.f, 0.f, 0.f, 0.f), position, unused)) {
		return;
	}

	const XrPosef & left = m_views[0].pose;
	const XrPosef & right = m_views[1].pose;
	Vec3f head = (Vec3f(left.position.x, left.position.y, left.position.z)
	              + Vec3f(right.position.x, right.position.y, right.position.z)) * 0.5f;

	// Facing the user, upright
	Vec3f toHead = head - position;
	float distance = glm::length(toHead);
	if(distance < 0.05f) {
		return;
	}
	Vec3f normal = toHead / distance;
	Vec3f up = Vec3f(0.f, 1.f, 0.f) - normal * normal.y;
	if(glm::length(up) < 0.01f) {
		return;
	}
	up = glm::normalize(up);
	Vec3f across = glm::cross(up, normal);
	glm::quat orientation = glm::quat_cast(glm::mat3x3(across, up, normal));

	// The same angular size as on the virtual screen (60° wide), at any distance
	float pixelSize = distance * 2.f * std::tan(glm::radians(30.f)) / float(m_uiSize.x);
	Vec3f center = position + up * (0.5f * float(areas.label.height()) * pixelSize);
	addUIPanel(areas.label, pixelSize, center, orientation, false);

}

void VRSystem::submitWindowAsUI(const Vec2i & windowSize) {

	// Only for frames drawn directly to the window (logos), not when the virtual screen was hidden on purpose
	if(!isRenderingFrame() || m_uiSubmitted || m_uiRendered || windowSize.x <= 0 || windowSize.y <= 0) {
		return;
	}

	if(m_uiSize != windowSize && !createUITarget(windowSize)) {
		return;
	}

	u64 image = acquireSwapchainImage(m_uiSwapchain);
	if(!image) {
		return;
	}

	m_graphics->copyWindowToImage(image, windowSize);

	releaseSwapchainImage(m_uiSwapchain);

	updateUIPlacement(true); // Following the head like the menus
	addScreenPanel(Rect(m_uiSize.x, m_uiSize.y), true);

}

void VRSystem::updateUIPlacement(bool followHead) {

	const XrPosef & left = m_views[0].pose;
	const XrPosef & right = m_views[1].pose;
	Vec3f head = (Vec3f(left.position.x, left.position.y, left.position.z)
	              + Vec3f(right.position.x, right.position.y, right.position.z)) * 0.5f;
	glm::quat orientation(left.orientation.w, left.orientation.x, left.orientation.y, left.orientation.z);
	Vec3f forward = orientation * Vec3f(0.f, 0.f, -1.f);
	float headYaw = glm::atan(-forward.x, -forward.z);

	if(!m_uiPlaced || followHead != m_uiFollowHead) {

		m_uiYaw = headYaw;
		m_uiAnchor = head;
		m_uiPlaced = true;
		m_uiFollowHead = followHead;
		m_uiAwayTime = 0.f;

	} else if(followHead) {

		/*
		 * Follow the head in steps: the screen stays in place, and once the head has looked or moved away from it for a
		 * moment, it jumps in front of the head at once. Gliding after the head made users dizzy.
		 */
		const float AwayAngle = glm::radians(30.f);
		const float AwayDistance = 0.25f; // m
		const float AwayDelay = 1.f; // s
		float dt = glm::clamp(float(m_frameState.predictedDisplayPeriod) * 1e-9f, 0.f, 0.1f);

		float difference = std::remainder(headYaw - m_uiYaw, glm::two_pi<float>());
		if(glm::abs(difference) > AwayAngle || glm::length(head - m_uiAnchor) > AwayDistance) {
			m_uiAwayTime += dt;
		} else {
			m_uiAwayTime = 0.f;
		}
		if(m_uiAwayTime >= AwayDelay) {
			m_uiYaw = headYaw;
			m_uiAnchor = head;
			m_uiAwayTime = 0.f;
		}

	}

	// A screen 60° wide at the configured distance
	float distance = config.vr.uiDistance;
	float width = 2.f * distance * glm::tan(glm::radians(30.f));
	m_uiExtent.width = width;
	m_uiExtent.height = width * float(m_uiSize.y) / float(m_uiSize.x);

	glm::quat rotation = glm::angleAxis(m_uiYaw, Vec3f(0.f, 1.f, 0.f));
	Vec3f position = m_uiAnchor + rotation * Vec3f(0.f, 0.f, -distance);
	m_uiPose.orientation = { rotation.x, rotation.y, rotation.z, rotation.w };
	m_uiPose.position = { position.x, position.y, position.z };

}

void VRSystem::drawUIToWindow(const Vec2i & windowSize, bool clear) {

	if(!isDesktopViewShown()) {
		return; // Nothing is drawn to the window (see config.vr.desktopView)
	}

	// The whole interface over the whole window
	VRWindowQuad quad;
	const Vec2f corners[4] = { Vec2f(-1.f, -1.f), Vec2f(1.f, -1.f), Vec2f(-1.f, 1.f), Vec2f(1.f, 1.f) };
	for(size_t i = 0; i < 4; i++) {
		quad.position[i] = Vec4f(corners[i], 0.f, 1.f);
		quad.uv[i] = Vec2f(corners[i].x * 0.5f + 0.5f, 0.5f - corners[i].y * 0.5f);
	}
	m_graphics->drawUIToWindow(windowSize, clear, { quad });

}

void VRSystem::drawPanelsToWindow(const Vec2i & windowSize) {

	/*
	 * The desktop view shows what the headset shows (user request): the panels of this frame (wrist, book, screen,
	 * labels...) seen from the left eye, cropped like its image (see endStereoRender), not the flat interface
	 */
	if(m_uiPanels.empty() || m_uiSize.x <= 0 || m_uiSize.y <= 0 || windowSize.x <= 0 || windowSize.y <= 0
	   || !isDesktopViewShown()) {
		return;
	}
	const XrView & eye = m_views[0];
	Vec2f crop(m_eyeSize);
	if(m_eyeSize.x * windowSize.y > windowSize.x * m_eyeSize.y) {
		crop.x = float(m_eyeSize.y) * float(windowSize.x) / float(windowSize.y);
	} else {
		crop.y = float(m_eyeSize.x) * float(windowSize.y) / float(windowSize.x);
	}
	Vec2f zoom = Vec2f(m_eyeSize) / crop;

	// The projection of the left eye, zoomed like its cropped image (the depth is not used): OpenXR views look along -z
	float tanLeft = std::tan(eye.fov.angleLeft);
	float tanRight = std::tan(eye.fov.angleRight);
	float tanUp = std::tan(eye.fov.angleUp);
	float tanDown = std::tan(eye.fov.angleDown);
	glm::mat4 projection(0.f);
	projection[0][0] = 2.f / (tanRight - tanLeft) * zoom.x;
	projection[2][0] = (tanRight + tanLeft) / (tanRight - tanLeft) * zoom.x;
	projection[1][1] = 2.f / (tanUp - tanDown) * zoom.y;
	projection[2][1] = (tanUp + tanDown) / (tanUp - tanDown) * zoom.y;
	projection[2][3] = -1.f;
	glm::quat eyeOrientation(eye.pose.orientation.w, eye.pose.orientation.x, eye.pose.orientation.y,
	                         eye.pose.orientation.z);
	glm::mat4 eyeToTracking = glm::translate(glm::mat4(1.f), Vec3f(eye.pose.position.x, eye.pose.position.y,
	                                                               eye.pose.position.z))
	                          * glm::mat4_cast(eyeOrientation);
	glm::mat4 trackingToClip = projection * glm::inverse(eyeToTracking);

	std::vector<VRWindowQuad> quads;
	for(const UIPanel & panel : m_uiPanels) {
		if(panel.hidden) {
			continue; // Drawn in the world, so in the eye image already
		}
		glm::quat orientation(panel.pose.orientation.w, panel.pose.orientation.x, panel.pose.orientation.y,
		                      panel.pose.orientation.z);
		Vec3f center(panel.pose.position.x, panel.pose.position.y, panel.pose.position.z);
		Vec3f across = orientation * Vec3f(0.5f * panel.extent.width, 0.f, 0.f);
		Vec3f up = orientation * Vec3f(0.f, 0.5f * panel.extent.height, 0.f);
		float uvLeft = float(panel.area.left) / float(m_uiSize.x);
		float uvRight = float(panel.area.right) / float(m_uiSize.x);
		float uvTop = float(panel.area.top) / float(m_uiSize.y);
		float uvBottom = float(panel.area.bottom) / float(m_uiSize.y);
		const Vec3f corners[4] = { center - across - up, center + across - up, center - across + up, center + across + up };
		const Vec2f uvs[4] = {
			Vec2f(uvLeft, uvBottom), Vec2f(uvRight, uvBottom), Vec2f(uvLeft, uvTop), Vec2f(uvRight, uvTop)
		};
		VRWindowQuad & quad = quads.emplace_back();
		for(size_t i = 0; i < 4; i++) {
			quad.position[i] = trackingToClip * Vec4f(corners[i], 1.f);
			quad.uv[i] = uvs[i];
		}
	}

	m_graphics->drawUIToWindow(windowSize, false, quads);

}

void VRSystem::updatePointer() {

	if(!GInput) {
		return;
	}

	// The main hand, or the other one while the main hand holds the book
	const VRHand pointing = getPointerHand();
	const float MagicTapTime = 0.25f; // s, a shorter trigger press without moving the hand is a tap
	const float MagicStrokeDistance = 0.03f; // m the fingertips move before a rune is drawn
	const float MagicDoubleTapTime = 0.6f; // s from the end of the tap entering the magic mode to the end of the next
	const VRHandState & hand = m_input.getHand(pointing);

	/*
	 * In-game the laser points into the world, unless it points at an interface panel (book, containers, the HUD
	 * and inventory on the wrist, ...). Outside the game (menus) it always points at the virtual screen.
	 */
	bool inGame = m_lastFrameStereo;
	clearVRPointerBeam();
	// While holding an object, panels are ignored so that the object does not switch to inventory drag and drop
	bool panelsActive = !inGame || !isVRGrabbing();

	// Intersect the pointing hand's aim ray with the panels shown in the last frame, the nearest hit wins
	bool onUI = false;
	Vec2s mouse(0);
	bool useOnTap = false;
	bool noBeam = false;
	float nearest = std::numeric_limits<float>::max();
	for(const UIPanel & panel : m_visiblePanels) {

		if(!panelsActive || !hand.aim.valid) {
			break;
		}
		if(!panel.interactive) {
			continue;
		}

		glm::quat panelRotation(panel.pose.orientation.w, panel.pose.orientation.x,
		                        panel.pose.orientation.y, panel.pose.orientation.z);
		glm::quat toPanel = glm::inverse(panelRotation);
		Vec3f panelPosition(panel.pose.position.x, panel.pose.position.y, panel.pose.position.z);

		// Ray in panel space: the panel is the z = 0 plane, visible from +z
		Vec3f origin = toPanel * (hand.aim.position - panelPosition);
		Vec3f direction = toPanel * (hand.aim.orientation * Vec3f(0.f, 0.f, -1.f));
		if(direction.z > -1e-4f || origin.z <= 0.f) {
			continue;
		}

		float distance = -origin.z / direction.z;
		Vec3f hit = origin + direction * distance;
		float u = hit.x / panel.extent.width + 0.5f;
		float v = 0.5f - hit.y / panel.extent.height;
		if(distance >= nearest || u < 0.f || u > 1.f || v < 0.f || v > 1.f) {
			continue;
		}

		nearest = distance;
		onUI = true;
		useOnTap = panel.useOnTap;
		noBeam = panel.noBeam;
		mouse = Vec2s(s16(float(panel.area.left) + u * float(panel.area.width() - 1)),
		              s16(float(panel.area.top) + v * float(panel.area.height() - 1)));

	}
	// No beam to the panels at the wrists, the book and other inventories (user request), the cursor is enough
	if(onUI && m_otherInventoryArea.contains(Vec2i(mouse))) {
		noBeam = true;
	}
	if(onUI && inGame && !noBeam) {
		Vec3f aimDirection = hand.aim.orientation * Vec3f(0.f, 0.f, -1.f);
		setVRPointerBeamToUI(hand.aim.position, hand.aim.position + aimDirection * nearest);
	}

	if(onUI && !m_pointerOnUI) {
		m_input.vibrate(pointing, 0.25f, 0.04f);
	}
	m_pointerOnUI = onUI;

	bool onWorld = !onUI && inGame && getVRWorldPointer(hand.aim, mouse);
	
	/*
	 * The "click" (left mouse button): in the world the grip uses and activates (levers, doors, containers, talking, see
	 * below), on an interface panel the trigger or the grip click. Items are taken and grabbed with the grip (VRGrab).
	 * The trigger is otherwise used for magic (see VRControls). A trigger click started on a panel stays a click
	 * until the trigger is released, e.g. to drag an item out of the inventory.
	 */
	if(hand.wasPressed(VRButtonTrigger)) {
		/*
		 * While the game waits for the target of a spell, the trigger on a valid target clicks (the game casts the
		 * spell on it), elsewhere it cancels the spell. Neither taps items nor toggles the magic mode.
		 */
		bool spellTarget = inGame && !onUI && LOOKING_FOR_SPELL_TARGET;
		bool spellTargetValid = spellTarget && isVRSpellTargetValid();
		if(spellTarget && !spellTargetValid) {
			ARX_SOUND_PlaySFX(g_snd.MAGIC_FIZZLE, &player.pos);
			ARX_SPELLS_CancelSpellTarget();
		}
		// While an item is selected to combine, the trigger clicks everywhere: on the target, or elsewhere to cancel
		m_clickByTrigger = onUI || ((COMBINE || COMBINEGOLD || spellTargetValid) && onWorld && !isVRGrabbing());
		// A tap uses the item under it, unless an item is on the cursor: then it is a click that drops it there
		m_clickUseOnTap = onUI && useOnTap && !COMBINE && !COMBINEGOLD && !g_draggedEntity;
		if(m_clickUseOnTap && m_useTapDelay >= 0.f) {
			m_secondTap = true; // A double tap: select instead of using
			m_useTapDelay = -1.f;
		}
		// On an item in the world the trigger taps (a double tap selects it to combine) instead of starting magic
		m_triggerOnItem = false;
		if(!onUI && inGame && !COMBINE && !COMBINEGOLD && !spellTarget && !isVRGrabbing() && !m_bookOpen) {
			Entity * item = getVRPointedEntity(hand.aim, false);
			if(item && (item->ioflags & IO_ITEM) && item->show == SHOW_FLAG_IN_SCENE) {
				m_triggerOnItem = true;
				m_triggerItem = item->index();
				m_triggerItemTime = 0.f;
			}
		}
		// Elsewhere in the world a tap toggles the magic mode, in it the trigger held draws runes with the hand
		m_magicDrawing = inGame && !onUI && !spellTarget && !m_clickByTrigger && !m_triggerOnItem && !m_bookOpen
		                 && !isVRGrabbing() && !isVRFist(pointing) && !isVRArcheryActive();
		m_magicStroke = false;
		m_magicPressTime = 0.f;
		m_magicPressPosition = hand.grip.position + hand.grip.orientation * Vec3f(0.f, 0.f, -0.08f);
	} else if(!hand.isPressed(VRButtonTrigger)) {
		if(m_magicDrawing && !m_magicStroke && m_magicPressTime < MagicTapTime) {
			if(m_magicMode && m_magicCancelTime >= 0.f) {
				/*
				 * A double tap (the second right after entering the magic mode, nothing drawn) ends the latest
				 * lasting spell instead, e.g. night vision. Leaving the mode without runes casts nothing.
				 */
				m_magicMode = false;
				m_magicCancelTime = -1.f;
				bool ended = endVRLastSpell();
				m_input.vibrate(pointing, ended ? 0.6f : 0.2f, ended ? 0.1f : 0.04f);
			} else {
				// Leaving the magic mode casts the spell of the runes drawn (the game casts when its magic key is
				// released)
				m_magicMode = !m_magicMode;
				if(m_magicMode) {
					m_magicPrecast = false;
				}
				m_magicCancelTime = m_magicMode ? MagicDoubleTapTime : -1.f;
				m_input.vibrate(pointing, m_magicMode ? 0.7f : 0.4f, m_magicMode ? 0.12f : 0.06f);
			}
		}
		m_clickByTrigger = false;
		m_magicDrawing = false;
		m_magicStroke = false;
	}
	if(m_magicDrawing && hand.isPressed(VRButtonTrigger)) {
		// A rune is drawn once the hand moves or the trigger is held, so that taps (toggling the mode) draw nothing
		m_magicPressTime += getFrameDuration();
		Vec3f fingertips = hand.grip.position + hand.grip.orientation * Vec3f(0.f, 0.f, -0.08f);
		if(m_magicMode && (m_magicPressTime >= MagicTapTime
		                   || glm::distance(fingertips, m_magicPressPosition) > MagicStrokeDistance)) {
			m_magicStroke = true;
			m_magicCancelTime = -1.f; // Drawing: the next tap casts
		}
		if(m_magicStroke && hand.isPressed(VRButtonGrip) && !m_magicPrecast) {
			// The grip held while drawing stores the spell instead of casting it (see isMagicPrecast)
			m_magicPrecast = true;
			m_input.vibrate(pointing, 0.3f, 0.05f);
		}
	}
	if(m_magicCancelTime >= 0.f) {
		m_magicCancelTime -= getFrameDuration();
		if(m_magicCancelTime < 0.f) {
			m_magicCancelTime = -1.f;
		}
	}
	if(!inGame || isVRArcheryActive() || m_bookOpen) {
		m_magicMode = false; // Menus, the book, cutscenes, the bow
	}
	if(hand.wasPressed(VRButtonGrip)) {
		// Touching an object grabs it even while pointing at a panel
		m_gripIsClick = !isVRGripGrabbing() && !isVRHandAtSheath(pointing); // Not at the sheath (draws the weapon)
		Vec3f bookPosition;
		glm::quat bookOrientation;
		Vec2f bookSize;
		const float TakeDistance = 0.1f; // m from the corner
		bool atCorner = false;
		if(hand.grip.valid && getBookPose(bookPosition, bookOrientation, bookSize)) {
			// The bottom corner on the side of this hand, where it would hold the book
			float side = (pointing == VRHandLeft) ? -1.f : 1.f;
			Vec3f corner = bookPosition + bookOrientation * Vec3f(side * 0.5f * bookSize.x, -0.5f * bookSize.y, 0.f);
			atCorner = glm::distance(hand.grip.position, corner) < TakeDistance;
		}
		if(atCorner) {
			// A grip at the corner of the book takes it into this hand, the other hand points then
			m_bookHand = pointing;
			m_gripIsClick = false;
			m_input.vibrate(pointing, 0.5f, 0.08f);
		}
		if(m_magicMode) {
			m_gripIsClick = false; // In the magic mode the grip stores the spell (precast)
		}
		/*
		 * The grip activates what it points at like the game's double click: levers, doors, containers, the loot of
		 * dead bodies, talking to NPCs (not with a drawn weapon). Items are taken and grabbed instead (see VRGrab).
		 */
		if(inGame && !onUI && hand.isPressed(VRButtonTrigger)) {
			m_gripIsClick = false; // A fist punches (see VRCombat)
		}
		if(m_gripIsClick && inGame && !onUI && !m_bookOpen) {
			// The game's own pick at the pointer (like its double click finds objects), else the hand ray
			Entity * target = onWorld ? InterClick(mouse) : nullptr;
			if(activateWithGrip(pointing, target)) {
				m_gripIsClick = false;
			}
		}
	} else if(isVRGrabbingDirectly()) {
		m_gripIsClick = false; // Caught an object after pressing the grip
	}
	bool fist = inGame && !onUI && isVRFist(pointing); // Punches, it does not click (see VRCombat)
	/*
	 * In the character creation and the book, the grip is the right click (user request): the trigger raises
	 * attributes and skills, the grip lowers them, like the mouse buttons
	 */
	bool gripRightClick = onUI && (ARXmenu.mode() == Mode_CharacterCreation || m_bookOpen);
	bool gripPressed = hand.isPressed(VRButtonGrip) && m_gripIsClick && !fist;
	bool clickPressed = (gripPressed && !gripRightClick) || m_clickByTrigger;
	bool clickStarted = clickPressed && !m_clickWasPressed;
	bool clickEnded = !clickPressed && m_clickWasPressed;
	m_clickWasPressed = clickPressed;
	
	/*
	 * Pressing a button tilts the controller, which moves the cursor a lot at a distance and would turn every
	 * click into a drag. Freeze the cursor where the click started: releasing quickly is a click there,
	 * holding longer lets the cursor follow the hand again to drag.
	 */
	const float ClickDuration = 0.15f; // Longer to drag felt slow (user request)
	if(clickStarted) {
		m_triggerHoldTime = 0.f;
		m_clickPosition = mouse;
		m_clickOnTarget = onUI || onWorld;
		m_clickOnUI = onUI;
	} else if(clickPressed || clickEnded) {
		m_triggerHoldTime += getFrameDuration();
	}
	if((clickPressed || clickEnded) && m_triggerHoldTime < ClickDuration && m_clickOnTarget) {
		mouse = m_clickPosition;
		onUI = m_clickOnUI;
		onWorld = !m_clickOnUI;
	}
	
	/*
	 * In the inventory a trigger tap uses the item (e.g. to eat), holding the trigger drags it: the left button is
	 * only pressed once the click is held long enough. The use action of the game (see VRControls) is pressed for
	 * three frames (a key release only counts after two) and released, with the cursor kept on the item - a right
	 * click would also toggle free look, which closes the inventory.
	 */
	bool tapClick = m_clickByTrigger || (clickEnded && m_clickUseOnTap);
	bool tapPending = m_clickUseOnTap && tapClick && m_triggerHoldTime < ClickDuration;
	if(m_useTapFrames > 0) {
		m_useTapFrames--;
	}
	/*
	 * A double tap (like the game's double click) selects the item to combine it with the next target. A single tap
	 * only uses the item once no second tap followed.
	 */
	const float DoubleTapTime = 0.3f;
	if(tapPending && clickEnded) {
		Rect purse = getVRPurseArea();
		if(!purse.empty() && purse.contains(m_clickPosition.x, m_clickPosition.y)) {
			// A tap on the purse selects gold to give, like the game's double click on it (user request)
			if(!COMBINE && !COMBINEGOLD) {
				COMBINEGOLD = true;
				ARX_SOUND_PlayInterface(g_snd.GOLD);
				m_input.vibrate(pointing, 0.5f, 0.08f);
			}
		} else if(m_secondTap) {
			if(FlyingOverIO && (FlyingOverIO->ioflags & IO_ITEM) && ARX_INTERFACE_StartCombine(*FlyingOverIO)) {
				m_input.vibrate(pointing, 0.5f, 0.08f);
			}
		} else {
			m_useTapDelay = DoubleTapTime;
			m_useTapPosition = m_clickPosition;
		}
	}
	if(clickEnded) {
		m_clickUseOnTap = false;
		m_secondTap = false;
	}
	// Taps on items in the world: a double tap selects the item to combine
	m_lastItemTapAge += getFrameDuration();
	if(m_triggerOnItem) {
		if(hand.isPressed(VRButtonTrigger)) {
			m_triggerItemTime += getFrameDuration();
		} else {
			if(m_triggerItemTime < ClickDuration) {
				Entity * item = entities.get(m_triggerItem);
				if(item && m_lastItemTap == m_triggerItem && m_lastItemTapAge < DoubleTapTime) {
					if(ARX_INTERFACE_StartCombine(*item)) {
						m_input.vibrate(pointing, 0.5f, 0.08f);
					}
					m_lastItemTap = EntityHandle();
				} else {
					m_lastItemTap = m_triggerItem;
					m_lastItemTapAge = 0.f;
				}
			}
			m_triggerOnItem = false;
		}
	}
	if(m_useTapDelay >= 0.f) {
		m_useTapDelay -= getFrameDuration();
		if(m_useTapDelay < 0.f) {
			m_useTapFrames = 5;
		}
	}
	if(m_useTapFrames > 0) {
		mouse = m_useTapPosition;
		onUI = true;
		onWorld = false;
	}

	/*
	 * Drawing runes: the cursor follows the fingertips of the main hand as seen from the eyes, with the left button
	 * held. The game draws the flares in the world along the line through the cursor, so they appear at the hand, and
	 * recognizes the runes from these screen positions.
	 */
	bool improvedStroke = false;
	if(m_magicDrawing && m_magicMode && hand.isPressed(VRButtonTrigger) && hand.grip.valid) {
		Vec3f tip = hand.grip.position + hand.grip.orientation * Vec3f(0.f, 0.f, -0.08f);
		Vec2f screen;
		if(config.vr.improvedRuneDrawing && m_magicStroke && getImprovedRunePoint(tip, screen)) {
			improvedStroke = true;
			mouse = Vec2s(s16(screen.x), s16(screen.y));
			onWorld = true;
			onUI = false;
		} else {
			Vec3f fingertips;
			if(trackingPointToWorld(tip, fingertips) && worldToVRScreen(fingertips, screen)) {
				mouse = Vec2s(s16(screen.x), s16(screen.y));
				onWorld = true;
				onUI = false;
			}
		}
	}
	if(!improvedStroke) {
		m_runeCanvas.valid = false;
	}
	
	GInput->setVirtualMousePosition(onUI || onWorld, mouse);

	bool drawing = m_magicDrawing && m_magicStroke && hand.isPressed(VRButtonTrigger);
	GInput->setVirtualMouseButton(Mouse::Button_0, (clickPressed && !tapPending) || drawing);
	
	// Off hand grip = right click on a panel, in the world it grabs (see VRGrab). Not the hand holding the book.
	const VRHand offHand = (pointing == VRHandLeft) ? VRHandRight : VRHandLeft;
	const VRHandState & off = m_input.getHand(offHand);

	/*
	 * The off hand grip also activates what that hand points at, like the pointing hand (user request): not on a
	 * panel (right click), not grabbing items (VRGrab only takes items), drawing a weapon or taking the torch, not as
	 * a fist (punches) or holding the book
	 */
	m_offHandUseTarget = EntityHandle();
	if(inGame && !onUI && !m_bookOpen && !off.isPressed(VRButtonTrigger) && off.aim.valid
	   && !isVROffHandGrabbing() && !isVRHandAtSheath(offHand) && !isVRHandAtBeltTorch(offHand)) {
		// Highlighted like the objects the hands would grab (see isVRUseTarget)
		if(Entity * target = getGripTarget(offHand, nullptr)) {
			m_offHandUseTarget = target->index();
			if(off.wasPressed(VRButtonGrip)) {
				activateWithGrip(offHand, target);
			}
		}
	}
	bool rightClick = (off.isPressed(VRButtonGrip) && onUI && !m_bookOpen) || (gripPressed && gripRightClick);
	GInput->setVirtualMouseButton(Mouse::Button_1, rightClick);
	bool rightClickStarted = (off.wasPressed(VRButtonGrip) && !m_bookOpen)
	                         || (gripRightClick && gripPressed && hand.wasPressed(VRButtonGrip));

	if(onUI && (clickStarted || rightClickStarted)) {
		m_input.vibrate(pointing, 0.5f, 0.05f);
	}
	
}
