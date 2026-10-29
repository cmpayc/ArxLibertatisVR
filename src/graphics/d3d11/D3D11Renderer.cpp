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
#include <limits>
#include <sstream>
#include <string>

#include <glm/gtc/constants.hpp>
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
#include "io/fs/FileStream.h"
#include "io/log/Logger.h"
#include "platform/CrashHandler.h"
#include "platform/WindowsUtils.h"
#include "window/RenderWindow.h"

using d3d11::ComPtr;

namespace {

struct VSConstants {
	float transform[16];
	float jitter[4];
	float camera[4]; // World position of the camera
	float modelToWorld[16]; // From pre-transformed model vertices to world space, for per-pixel highlights
	float worldToClip[16]; // From world space to clip space in the 3D scene, for tessellated models
	float prevTransform[16]; // Like transform for the positions of the previous frame (differs for VR eyes)
};

const size_t VSConstantsSize = sizeof(VSConstants) / sizeof(float);

struct MotionConstants {
	float reprojection[16];
	float targetSize[4];
	float region[4]; // The view in the scene target: xy = first pixel, zw = size
};

struct PSConstants {
	s32 stages[3][4];
	float fogColor[4];
	float fogParams[4];
	float relief[4]; // x = normal map slope scale, y = parallax depth, z = cavity darkening of models, both 0 without a relief map
	float specular[4]; // x = highlight strength, y = exponent, z = metallic, w = highlight setting, all 0 without highlights
	float surface[4]; // x = reflectivity, y = 1 for blended draws, z = 1 for water surfaces, w = 1 for models
	float effect[4]; // x = time in seconds (repeating), y = lava glow, z = ice, w = crystal, 0 for other surfaces
	float material[4]; // x = brightness of the steel parts of armor, y = leather sheen, 0 for other surfaces
};

// The lava animation repeats after this time, the frequencies of its waves are multiples of 0.1
const float LavaTimePeriod = 20.f * glm::pi<float>();

struct ReflectionConstants {
	float target[4];     // xy = size, zw = 1 / size
	float projection[4]; // x = 1 / projection[0][0], y = 1 / projection[1][1], z = near, w = far
	float view[3][4];    // Rows of the rotation from world space to the view space of the depth reconstruction
	float params[4];     // x = strength, y = maximum ray length, z = mip levels of the scene copy, w = debug view
	float region[4];     // The view in the scene target: xy = first pixel, zw = size
	float center[4];     // xy = projection[2][0], projection[2][1] (off-center projections of VR eyes)
};

struct BlitConstants {
	float rect[4]; // The part of the source textures covered by the viewport: offset and size
};

struct ClearConstants {
	float color[4];
	float depth[4];
};

struct AmbientOcclusionConstants {
	float target[4];
	float projection[4];
	float params[4];
	float region[4]; // See ReflectionConstants
	float center[4];
};

// Ambient occlusion settings: sample radius in world units (about centimeters), depth bias
const float AmbientOcclusionRadius = 60.f;
const float AmbientOcclusionBias = 1.f;

struct BloomConstants {
	float params[4];
	float source[4];
	float clamp[4]; // The source texture coordinates of the view: xy = minimum, zw = maximum
	float fade[4];  // x = distance where the bloom starts to fade (0 = no fade), y = fade length, z = near, w = far
};

struct GradingConstants {
	float params[4];
	float tint[4];
	float shadows[4];
	float highlights[4];
	float tone[4];
};

struct ColorGradingPreset {
	const char * name;
	float exposure;
	float gamma; //!< Below 1 brightens dark areas
	float contrast;
	float saturation;
	float vignette;
	float tint[3];
	float shadows[3];
	float highlights[3];
};

// Contrast pivot, low because Arx scenes are dark
const float ColorGradingPivot = 0.2f;

// The first preset leaves the colors unchanged, the others are slightly brighter than the original
const ColorGradingPreset ColorGradingPresets[] = {
	{ "off", 1.f, 1.f, 1.f, 1.f, 0.f, { 1.f, 1.f, 1.f }, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f } },
	{ "natural", 1.f, 0.95f, 1.06f, 1.15f, 0.f, { 1.f, 1.f, 1.f }, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f } },
	{ "warm", 1.f, 0.95f, 1.05f, 1.1f, 0.1f, { 1.06f, 1.f, 0.9f }, { 0.01f, 0.f, -0.01f },
	  { 0.02f, 0.01f, -0.01f } },
	{ "cinematic", 1.f, 0.95f, 1.1f, 0.95f, 0.2f, { 1.f, 1.f, 1.f }, { -0.012f, 0.004f, 0.02f },
	  { 0.03f, 0.01f, -0.02f } },
};

// Bloom settings: brightness where the bloom starts, width of the soft transition
const float BloomThreshold = 0.45f;
const float BloomKnee = 0.2f;
// Distance (world units, about centimeters) where the bloom starts to fade, and over which it fades to a half
const float BloomFadeStart = 300.f;
const float BloomFadeLength = 700.f;

const size_t MaxPixelLights = 128;

// Normal map slope at full strength, the generated slope is the brightness change per texel
const float NormalMapSlopeScale = 6.f;

// Models are small and seen up close in VR: their relief is stronger than the world's, so that it shows even at the
// low normal map setting
const float ModelReliefScale = 2.f;

// Parallax depth at full strength, relative to the texture size
const float ParallaxDepthScale = 0.05f;

// Highlight strength of the shiniest materials at full strength
const float SpecularScale = 1.5f;

// Highlights and reflections of water surfaces
static const D3D11Texture::Specular WaterSpecular = { 1.f, 96.f, 0.f, 1.f };

// Longest reflection ray in world units
const float ReflectionDistance = 2000.f;

struct LightConstants {
	s32 count[4];
	// Per light: position + fall start, color * intensity + fall end
	float lights[MaxPixelLights * 2][4];
};

const size_t MaxShadowMapLights = 6;

struct ShadowPassConstants {
	float face[16]; // From the position relative to the light to the clip space of a cube map face
	float light[4]; // xyz = light position, w = model triangles closer than this to the light are ignored
};

struct ShadowConstants {
	float target[4];     // xy = size, zw = 1 / size
	float projection[4]; // x = 1 / projection[0][0], y = 1 / projection[1][1], z = near, w = far
	float view[3][4];    // Rows of the rotation from the reconstructed view space to world space
	float params[4];     // x = light count, y = filter taps, z = shadow map size
	float fog[4];        // x = fog start, y = fog end
	float region[4];     // The view in the target (one eye in VR): xy = first pixel, zw = size
	float center[4];     // xy = projection[2][0], projection[2][1]: the center of off-center projections (VR)
	// Per light: position relative to the camera + fall start, brightness + fall end + strength + 1 if baked,
	// near plane + far plane + cube index
	float lights[MaxShadowMapLights * 3][4];
};

struct ShadowQuality {
	size_t lights;
	UINT size; //!< Size of each cube map face
	UINT taps; //!< Filter samples per pixel and light
};

// Off, low, medium, high
const ShadowQuality ShadowQualities[] = {
	{ 0, 0, 0 },
	{ 2, 512, 8 },
	{ 4, 1024, 12 },
	{ 6, 1024, 16 },
};

// Near plane of the cube map faces
const float ShadowNearPlane = 2.f;

// Model triangles closer than this to a light are not drawn into its cube map, lights carried by characters (or
// the player's torch at the eyes) are inside their models
const float ShadowCasterMinDistance = 40.f;

// Size of the cells that sort the static world geometry into chunks for culling by the light range
const float ShadowChunkSize = 1000.f;

// Debug view that shows the shadow factor instead of the scene
const int ShadowDebugView = 10;

static_assert(sizeof(ShadowPassConstants) % 16 == 0, "constant buffer size must be a multiple of 16");
static_assert(sizeof(ShadowConstants) % 16 == 0, "constant buffer size must be a multiple of 16");
static_assert(sizeof(VSConstants) % 16 == 0, "constant buffer size must be a multiple of 16");
static_assert(sizeof(PSConstants) % 16 == 0, "constant buffer size must be a multiple of 16");
static_assert(sizeof(ClearConstants) % 16 == 0, "constant buffer size must be a multiple of 16");
static_assert(sizeof(AmbientOcclusionConstants) % 16 == 0, "constant buffer size must be a multiple of 16");
static_assert(sizeof(ReflectionConstants) % 16 == 0, "constant buffer size must be a multiple of 16");
static_assert(sizeof(BloomConstants) % 16 == 0, "constant buffer size must be a multiple of 16");
static_assert(sizeof(GradingConstants) % 16 == 0, "constant buffer size must be a multiple of 16");
static_assert(sizeof(BlitConstants) % 16 == 0, "constant buffer size must be a multiple of 16");
static_assert(sizeof(LightConstants) % 16 == 0, "constant buffer size must be a multiple of 16");

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

// See d3d11::isDirect3D11()
const D3D11Renderer * g_d3d11Renderer = nullptr;

// The adapter the device must be created on (VR), see d3d11::setRequiredAdapter()
bool g_hasRequiredAdapter = false;
LUID g_requiredAdapter = { };

// MSAA samples of the stereo scene at most: both eyes at the headset resolution are expensive
const UINT StereoMaxSamples = 4;

/*!
 * From positions relative to a light to the clip space of a cube map face, with the face orientations of Direct3D
 * cube maps so that the faces can be sampled with the direction from the light
 */
glm::mat4x4 shadowFaceTransform(size_t face, float nearPlane, float farPlane) {

	// Right, up and forward axis of each face
	static const Vec3f axes[6][3] = {
		{ Vec3f(0.f, 0.f, -1.f), Vec3f(0.f, 1.f, 0.f), Vec3f(1.f, 0.f, 0.f) },
		{ Vec3f(0.f, 0.f, 1.f), Vec3f(0.f, 1.f, 0.f), Vec3f(-1.f, 0.f, 0.f) },
		{ Vec3f(1.f, 0.f, 0.f), Vec3f(0.f, 0.f, -1.f), Vec3f(0.f, 1.f, 0.f) },
		{ Vec3f(1.f, 0.f, 0.f), Vec3f(0.f, 0.f, 1.f), Vec3f(0.f, -1.f, 0.f) },
		{ Vec3f(1.f, 0.f, 0.f), Vec3f(0.f, 1.f, 0.f), Vec3f(0.f, 0.f, 1.f) },
		{ Vec3f(-1.f, 0.f, 0.f), Vec3f(0.f, 1.f, 0.f), Vec3f(0.f, 0.f, -1.f) },
	};

	// 90 degree field of view, depth in [0, 1]
	float a = farPlane / (farPlane - nearPlane);
	float b = -nearPlane * farPlane / (farPlane - nearPlane);

	glm::mat4x4 transform(0.f);
	for(glm::length_t column = 0; column < 3; column++) {
		transform[column][0] = axes[face][0][column];
		transform[column][1] = axes[face][1][column];
		transform[column][2] = axes[face][2][column] * a;
		transform[column][3] = axes[face][2][column];
	}
	transform[3][2] = b;

	return transform;
}

//! \return true if an axis-aligned box intersects a sphere
bool boxIntersectsSphere(const Vec3f & min, const Vec3f & max, const Vec3f & center, float radius) {
	Vec3f closest = glm::clamp(center, min, max);
	Vec3f offset = closest - center;
	return glm::dot(offset, offset) < radius * radius;
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
	, m_stereoSamples(1)
	, m_outputColor(nullptr)
	, m_outputDepth(nullptr)
	, m_premultipliedAlpha(false)
	, m_stereoSize(0)
	, m_stereoEyeSize(0)
	, m_stereoVSConstantsData()
	, m_stereoVSConstantsValid()
	, m_sceneScale(1.f)
	, m_inScene(false)
	, m_drawingScene(false)
	, m_sceneSize(0)
	, m_ambientOcclusion(false)
	, m_ambientOcclusionStrength(0.f)
	, m_reflectionStrength(0.f)
	, m_reflectionColorLevels(0)
	, m_shadowQuality(0)
	, m_shadowMapSize(0)
	, m_shadowMapLights(0)
	, m_shadowCastersMin(0.f)
	, m_shadowCastersMax(0.f)
	, m_shadowCasterCapacity(0)
	, m_bloom(false)
	, m_bloomStrength(0.f)
	, m_bloomSizes()
	, m_sceneEffectsExcluded(false)
	, m_colorGrading(0)
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
	, m_upscalerSharpness(0.f)
	, m_upscalerCreated(false)
	, m_dlssFailed(false)
	, m_fsrFailed(false)
	, m_lastUpscaleTime(0)
	, m_blitRect(0.f)
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
	, m_modelTessellation(false)
	, m_tessellationBound(false)
	, m_currentVertexBuffer(nullptr)
	, m_currentVertexOffset(0)
	, m_currentTopology(D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED)
	, m_currentRasterizer(nullptr)
	, m_currentBlend(nullptr)
	, m_currentDepth(nullptr)
	, m_currentPixelShader(nullptr)
	, m_currentViews()
	, m_currentSamplers()
	, m_currentNormalView(nullptr)
	, m_vsConstantsData()
	, m_vsConstantsValid(false)
	, m_psConstantsData()
	, m_psConstantsValid(false)
	, m_pixelLighting(false)
	, m_pixelLightCount(0)
	, m_normalMapStrength(0.f)
	, m_parallaxStrength(0.f)
	, m_specularStrength(0.f)
	, m_waterSurface(false)
	, m_effectTime(0.f)
	, m_pixelLightsWarned(false)
	, m_snapshotTime(0)
	, m_fpsLogStart(0)
	, m_fpsLogFrames(0)
	, m_fpsLogTessellatedDraws(0)
	, m_fpsLogModelDraws(0)
{
	g_d3d11Renderer = this;
}

D3D11Renderer::~D3D11Renderer() {
	
	if(g_d3d11Renderer == this) {
		g_d3d11Renderer = nullptr;
	}
	
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
	
	// Debug view for the scene: 1 = depth, 2 = motion vectors, 3 = per-object motion, 4 = ambient occlusion,
	// 5 = bloom, 6 = per-pixel dynamic light, 7 = reflections, 8 = highlights of models,
	// 9 = models as wireframes, 10 = dynamic shadows
	{
		char value[16];
		DWORD length = GetEnvironmentVariableA("ARX_SCENE_DEBUG", value, sizeof(value));
		if(length > 0 && length < sizeof(value)) {
			m_sceneDebugView = std::atoi(value);
		}
	}
	
	// Debug log of the average frame rate every few seconds to a file, for measuring the cost of the effects
	{
		WCHAR path[MAX_PATH];
		DWORD length = GetEnvironmentVariableW(L"ARX_D3D11_FPS_LOG", path, MAX_PATH);
		if(length > 0 && length < MAX_PATH) {
			m_fpsLogPath = platform::WideString::toUTF8(path, length);
		}
	}
	
	// Debug snapshot of the frame after a number of seconds, for testing without capturing the screen
	{
		WCHAR path[MAX_PATH];
		DWORD length = GetEnvironmentVariableW(L"ARX_D3D11_SNAPSHOT", path, MAX_PATH);
		if(length > 0 && length < MAX_PATH) {
			m_snapshotPath = platform::WideString::toUTF8(path, length);
			char value[16];
			length = GetEnvironmentVariableA("ARX_D3D11_SNAPSHOT_TIME", value, sizeof(value));
			float seconds = (length > 0 && length < sizeof(value)) ? float(std::atof(value)) : 20.f;
			LARGE_INTEGER now, frequency;
			QueryPerformanceCounter(&now);
			QueryPerformanceFrequency(&frequency);
			m_snapshotTime = now.QuadPart + LONGLONG(double(seconds) * double(frequency.QuadPart));
		}
	}
	
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	
	D3D_FEATURE_LEVEL levels[] = {
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
	};
	
	// Create the factory through the dxgi.dll exports and not from the device: tools that wrap DXGI
	// (e.g. the OptiScaler overlay) only see swap chains created by a factory they handed out
	ComPtr<IDXGIFactory2> factory;
	ComPtr<IDXGIAdapter1> defaultAdapter;
	if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
		// VR: the headset may be connected to another adapter than the primary one
		if(g_hasRequiredAdapter) {
			ComPtr<IDXGIAdapter1> adapter;
			for(UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
				DXGI_ADAPTER_DESC1 desc;
				if(SUCCEEDED(adapter->GetDesc1(&desc)) && desc.AdapterLuid.LowPart == g_requiredAdapter.LowPart
				   && desc.AdapterLuid.HighPart == g_requiredAdapter.HighPart) {
					defaultAdapter = adapter;
					break;
				}
				adapter.Reset();
			}
			if(!defaultAdapter) {
				LogWarning << "The adapter required by the VR runtime was not found";
			}
		}
		if(!defaultAdapter) {
			factory->EnumAdapters1(0, &defaultAdapter);
		}
	}
	
	auto createDevice = [&](UINT deviceFlags, const D3D_FEATURE_LEVEL * featureLevels, UINT count) {
		HRESULT result = E_FAIL;
		if(defaultAdapter) {
			result = D3D11CreateDevice(defaultAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, deviceFlags,
			                           featureLevels, count, D3D11_SDK_VERSION, &m_device, &m_featureLevel,
			                           &m_context);
		}
		if(FAILED(result) && result != E_INVALIDARG) {
			result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags, featureLevels, count,
			                           D3D11_SDK_VERSION, &m_device, &m_featureLevel, &m_context);
		}
		return result;
	};
	
	HRESULT hr = E_FAIL;
	
	#ifdef ARX_DEBUG
	hr = createDevice(flags | D3D11_CREATE_DEVICE_DEBUG, levels, UINT(std::size(levels)));
	if(FAILED(hr)) {
		LogWarning << "Direct3D 11 debug layer not available";
	}
	#endif
	
	if(FAILED(hr)) {
		hr = createDevice(flags, levels, UINT(std::size(levels)));
	}
	
	if(hr == E_INVALIDARG) {
		// Direct3D 11.0 runtimes do not know about D3D_FEATURE_LEVEL_11_1
		hr = createDevice(flags, levels + 1, UINT(std::size(levels) - 1));
	}
	
	if(FAILED(hr)) {
		LogError << "Failed to create Direct3D 11 device: " << hresultString(hr);
		return false;
	}
	
	// Fall back to the factory of the device if it was not created on the default adapter
	ComPtr<IDXGIDevice> dxgiDevice;
	ComPtr<IDXGIAdapter> adapter;
	DXGI_ADAPTER_DESC adapterDesc, defaultDesc;
	if(FAILED(m_device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter))) {
		LogError << "Failed to get the DXGI adapter";
		return false;
	}
	if(!factory || !defaultAdapter || FAILED(adapter->GetDesc(&adapterDesc))
	   || FAILED(defaultAdapter->GetDesc(&defaultDesc))
	   || adapterDesc.AdapterLuid.LowPart != defaultDesc.AdapterLuid.LowPart
	   || adapterDesc.AdapterLuid.HighPart != defaultDesc.AdapterLuid.HighPart) {
		factory.Reset();
		if(FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
			LogError << "Failed to get the DXGI 1.2 factory";
			return false;
		}
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
	// Use an explicit size: tools that hook DXGI (e.g. OptiScaler) take small swap chains for overlays and ignore them.
	// The window may not have its final size yet, the buffers are resized later.
	RECT client;
	if(GetClientRect(m_window, &client)) {
		desc.Width = UINT(std::max<LONG>(client.right - client.left, 0));
		desc.Height = UINT(std::max<LONG>(client.bottom - client.top, 0));
	}
	if(desc.Width < 640 || desc.Height < 480) {
		MONITORINFO monitor = {};
		monitor.cbSize = sizeof(monitor);
		if(GetMonitorInfoW(MonitorFromWindow(m_window, MONITOR_DEFAULTTONEAREST), &monitor)) {
			desc.Width = UINT(monitor.rcMonitor.right - monitor.rcMonitor.left);
			desc.Height = UINT(monitor.rcMonitor.bottom - monitor.rcMonitor.top);
		}
	}
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
	
	auto supportedSamples = [this](UINT maximum) {
		for(UINT samples = maximum; samples > 1; samples--) {
			UINT colorQuality = 0, depthQuality = 0;
			if(SUCCEEDED(m_device->CheckMultisampleQualityLevels(ColorFormat, samples, &colorQuality))
			   && SUCCEEDED(m_device->CheckMultisampleQualityLevels(DepthFormat, samples, &depthQuality))
			   && colorQuality > 0 && depthQuality > 0) {
				return samples;
			}
		}
		return UINT(1);
	};
	m_msaaSamples = supportedSamples(UINT(std::max(maxMSAALevel, 1)));
	// VR copies the stereo scene into the eye images, the window is not multisampled then
	m_stereoSamples = supportedSamples(StereoMaxSamples);
	
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
		bufferDesc.ByteWidth = sizeof(LightConstants);
		LightConstants noLights = {};
		D3D11_SUBRESOURCE_DATA initialData = { &noLights, 0, 0 };
		if(FAILED(m_device->CreateBuffer(&bufferDesc, &initialData, &m_lightConstants))) {
			return false;
		}
		m_pixelLightCount = 0;
		bufferDesc.ByteWidth = sizeof(MotionConstants);
		if(FAILED(m_device->CreateBuffer(&bufferDesc, nullptr, &m_motionConstants))) {
			return false;
		}
		bufferDesc.ByteWidth = sizeof(AmbientOcclusionConstants);
		if(FAILED(m_device->CreateBuffer(&bufferDesc, nullptr, &m_aoConstants))) {
			return false;
		}
		bufferDesc.ByteWidth = sizeof(ReflectionConstants);
		if(FAILED(m_device->CreateBuffer(&bufferDesc, nullptr, &m_reflectionConstants))) {
			return false;
		}
		bufferDesc.ByteWidth = sizeof(BloomConstants);
		if(FAILED(m_device->CreateBuffer(&bufferDesc, nullptr, &m_bloomConstants))) {
			return false;
		}
		bufferDesc.ByteWidth = sizeof(GradingConstants);
		if(FAILED(m_device->CreateBuffer(&bufferDesc, nullptr, &m_gradingConstants))) {
			return false;
		}
		bufferDesc.ByteWidth = sizeof(BlitConstants);
		if(FAILED(m_device->CreateBuffer(&bufferDesc, nullptr, &m_blitConstants))) {
			return false;
		}
		bufferDesc.ByteWidth = sizeof(ShadowPassConstants);
		if(FAILED(m_device->CreateBuffer(&bufferDesc, nullptr, &m_shadowPassConstants))) {
			return false;
		}
		bufferDesc.ByteWidth = sizeof(ShadowConstants);
		if(FAILED(m_device->CreateBuffer(&bufferDesc, nullptr, &m_shadowConstants))) {
			return false;
		}
		bufferDesc.ByteWidth = sizeof(VSConstants);
		for(ComPtr<ID3D11Buffer> & buffer : m_stereoVSConstants) {
			if(FAILED(m_device->CreateBuffer(&bufferDesc, nullptr, &buffer))) {
				return false;
			}
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
                                      const D3D_SHADER_MACRO * defines = nullptr,
                                      std::string_view source = d3d11ShaderSource) {
	
	ComPtr<ID3DBlob> code;
	ComPtr<ID3DBlob> errors;
	
	HRESULT hr = D3DCompile(source.data(), source.size(), "arx-d3d11", defines, nullptr,
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
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, UINT(offsetof(TexturedVertex, normal)),
		  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TESSELLATION", 0, DXGI_FORMAT_R32_UINT, 0, UINT(offsetof(TexturedVertex, tessellation)),
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
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, UINT(offsetof(SMY_VERTEX, normal)),
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
		const D3D11_INPUT_ELEMENT_DESC layout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, UINT(offsetof(OverlayVertex, position)),
			  D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, UINT(offsetof(OverlayVertex, uv)),
			  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		ComPtr<ID3DBlob> code = compileShader("vsOverlay", "vs_4_0");
		if(!code || FAILED(m_device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(),
		                                                nullptr, &m_overlayVertexShader))
		   || FAILED(m_device->CreateInputLayout(layout, UINT(std::size(layout)), code->GetBufferPointer(),
		                                         code->GetBufferSize(), &m_overlayLayout))) {
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
		const D3D_SHADER_MACRO verticalDefines[] = { { "BLUR_VERTICAL", "1" }, { nullptr, nullptr } };
		const PixelShaderInfo shaders[] = {
			{ "psMotion", nullptr, std::addressof(m_motionPixelShader) },
			{ "psMotion", msaaDefines, std::addressof(m_motionPixelShaderMS) },
			{ "psDebugDepth", nullptr, std::addressof(m_debugDepthPixelShader) },
			{ "psDebugMotion", nullptr, std::addressof(m_debugMotionPixelShader) },
			{ "psDebugObjectMotion", nullptr, std::addressof(m_debugObjectMotionPixelShader) },
			{ "psAmbientOcclusion", nullptr, std::addressof(m_aoPixelShader) },
			{ "psAmbientOcclusionBlur", nullptr, std::addressof(m_aoBlurPixelShader) },
			{ "psAmbientOcclusionBlur", verticalDefines, std::addressof(m_aoBlurVerticalPixelShader) },
			{ "psApplyAmbientOcclusion", nullptr, std::addressof(m_aoApplyPixelShader) },
			{ "psApplyAmbientOcclusion", msaaDefines, std::addressof(m_aoApplyPixelShaderMS) },
			{ "psDebugAmbientOcclusion", nullptr, std::addressof(m_debugAOPixelShader) },
			{ "psReflection", nullptr, std::addressof(m_reflectionPixelShader) },
			{ "psBloomPrefilter", nullptr, std::addressof(m_bloomPrefilterPixelShader) },
			{ "psBloomPrefilter", msaaDefines, std::addressof(m_bloomPrefilterPixelShaderMS) },
			{ "psBloomDownsample", nullptr, std::addressof(m_bloomDownsamplePixelShader) },
			{ "psBloomUpsample", nullptr, std::addressof(m_bloomUpsamplePixelShader) },
			{ "psBloomComposite", nullptr, std::addressof(m_bloomCompositePixelShader) },
			{ "psColorGrading", nullptr, std::addressof(m_gradingPixelShader) },
			{ "psNormalMap", nullptr, std::addressof(m_normalMapPixelShader) },
			{ "psShadowCaster", nullptr, std::addressof(m_shadowCasterPixelShader) },
			{ "psShadow", nullptr, std::addressof(m_shadowPixelShader) },
			{ "psShadow", msaaDefines, std::addressof(m_shadowPixelShaderMS) },
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
	
	// Shadow maps hold only depth, from world space positions
	if(m_featureLevel >= D3D_FEATURE_LEVEL_10_1) {
		const D3D11_INPUT_ELEMENT_DESC layout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		ComPtr<ID3DBlob> code = compileShader("vsShadow", "vs_4_1");
		if(!code || FAILED(m_device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
		                                                &m_shadowVertexShader))
		   || FAILED(m_device->CreateInputLayout(layout, UINT(std::size(layout)), code->GetBufferPointer(),
		                                         code->GetBufferSize(), &m_shadowInputLayout))) {
			LogWarning << "Dynamic shadows not available";
			m_shadowVertexShader.Reset();
			m_shadowInputLayout.Reset();
		}
	}

	// Models are tessellated with hull and domain shaders, which need feature level 11
	if(m_featureLevel >= D3D_FEATURE_LEVEL_11_0 && GetEnvironmentVariableA("ARX_D3D11_NO_TESSELLATION", nullptr, 0) == 0) {
		std::string source = std::string(d3d11ShaderSource) + d3d11ModelPatchSource + d3d11ModelPatchStagesSource;
		ComPtr<ID3DBlob> vertex = compileShader("vsModelPatch", "vs_5_0", nullptr, source);
		ComPtr<ID3DBlob> hull = compileShader("hsModelPatch", "hs_5_0", nullptr, d3d11ModelPatchSource);
		ComPtr<ID3DBlob> domain = compileShader("dsModelPatch", "ds_5_0", nullptr, source);
		if(!vertex || !hull || !domain
		   || FAILED(m_device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr,
		                                         &m_modelPatchVertexShader))
		   || FAILED(m_device->CreateHullShader(hull->GetBufferPointer(), hull->GetBufferSize(), nullptr,
		                                       &m_modelHullShader))
		   || FAILED(m_device->CreateDomainShader(domain->GetBufferPointer(), domain->GetBufferSize(), nullptr,
		                                         &m_modelDomainShader))) {
			LogWarning << "Model tessellation not available";
			m_modelPatchVertexShader.Reset();
			m_modelHullShader.Reset();
			m_modelDomainShader.Reset();
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

bool D3D11Renderer::createSceneTargets(const Vec2i & size, UINT samples) {
	
	releaseSceneTargets();
	
	m_sceneSamples = samples;
	
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
	
	// Surface normal and reflectivity for the screen-space reflections and the shadows
	if(surfaceTargetNeeded()) {
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | (m_sceneSamples > 1 ? 0 : D3D11_BIND_SHADER_RESOURCE);
		if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_surfaceTarget))
		   || FAILED(m_device->CreateRenderTargetView(m_surfaceTarget.Get(), nullptr, &m_surfaceView))) {
			LogError << "Failed to create " << size.x << "x" << size.y << " Direct3D 11 surface target";
			releaseSceneTargets();
			return false;
		}
	}

	// The light that reached each surface, which the shadows take away from
	if(shadowsEnabled()) {
		desc.Format = DXGI_FORMAT_R8G8_UNORM;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | (m_sceneSamples > 1 ? 0 : D3D11_BIND_SHADER_RESOURCE);
		if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_shadingTarget))
		   || FAILED(m_device->CreateRenderTargetView(m_shadingTarget.Get(), nullptr, &m_shadingView))) {
			LogError << "Failed to create " << size.x << "x" << size.y << " Direct3D 11 shading target";
			releaseSceneTargets();
			return false;
		}
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
		// The stencil marks the pixels without effects
		resourceDesc.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
		if(FAILED(m_device->CreateShaderResourceView(m_sceneDepthTarget.Get(), &resourceDesc,
		                                             &m_sceneStencilResource))) {
			m_sceneStencilResource.Reset();
		}
	}
	
	desc.SampleDesc.Count = 1;
	
	if(m_sceneSamples > 1) {
		desc.Format = ColorFormat;
		// Ambient occlusion is applied to the resolved scene
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_sceneResolved))
		   || FAILED(m_device->CreateRenderTargetView(m_sceneResolved.Get(), nullptr, &m_sceneResolvedTargetView))) {
			LogError << "Failed to create " << size.x << "x" << size.y << " Direct3D 11 scene resolve target";
			releaseSceneTargets();
			return false;
		}
	} else {
		m_sceneResolved = m_sceneColorTarget;
		m_sceneResolvedTargetView = m_sceneColorView;
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
	
	// Resolved surface target
	if(m_surfaceTarget) {
		if(m_sceneSamples > 1) {
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_surfaceResolved))) {
				LogError << "Failed to create " << size.x << "x" << size.y << " Direct3D 11 surface resolve target";
				releaseSceneTargets();
				return false;
			}
		} else {
			m_surfaceResolved = m_surfaceTarget;
		}
		if(FAILED(m_device->CreateShaderResourceView(m_surfaceResolved.Get(), nullptr, &m_surfaceResource))) {
			LogError << "Failed to create Direct3D 11 surface view";
			releaseSceneTargets();
			return false;
		}
	}

	// Resolved shading target
	if(m_shadingTarget) {
		if(m_sceneSamples > 1) {
			desc.Format = DXGI_FORMAT_R8G8_UNORM;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_shadingResolved))) {
				LogError << "Failed to create " << size.x << "x" << size.y << " Direct3D 11 shading resolve target";
				releaseSceneTargets();
				return false;
			}
		} else {
			m_shadingResolved = m_shadingTarget;
		}
		if(FAILED(m_device->CreateShaderResourceView(m_shadingResolved.Get(), nullptr, &m_shadingResource))) {
			LogError << "Failed to create Direct3D 11 shading view";
			releaseSceneTargets();
			return false;
		}
	}

	// Scene copy with mipmaps for the screen-space reflections
	if(m_surfaceTarget && m_reflectionStrength > 0.f) {
		D3D11_TEXTURE2D_DESC colorDesc = desc;
		colorDesc.Format = ColorFormat;
		colorDesc.MipLevels = 0;
		colorDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		colorDesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
		if(FAILED(m_device->CreateTexture2D(&colorDesc, nullptr, &m_reflectionColor))
		   || FAILED(m_device->CreateShaderResourceView(m_reflectionColor.Get(), nullptr, &m_reflectionColorResource))) {
			LogError << "Failed to create " << size.x << "x" << size.y << " Direct3D 11 reflection targets";
			releaseSceneTargets();
			return false;
		}
		m_reflectionColor->GetDesc(&colorDesc);
		m_reflectionColorLevels = colorDesc.MipLevels;
	}
	
	// Ambient occlusion and the intermediate blur result, at half the scene size
	if(m_ambientOcclusion) {
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		desc.Format = DXGI_FORMAT_R8_UNORM;
		desc.Width = UINT((size.x + 1) / 2);
		desc.Height = UINT((size.y + 1) / 2);
		if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_aoTarget))
		   || FAILED(m_device->CreateRenderTargetView(m_aoTarget.Get(), nullptr, &m_aoView))
		   || FAILED(m_device->CreateShaderResourceView(m_aoTarget.Get(), nullptr, &m_aoResource))
		   || FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_aoBlurTarget))
		   || FAILED(m_device->CreateRenderTargetView(m_aoBlurTarget.Get(), nullptr, &m_aoBlurView))
		   || FAILED(m_device->CreateShaderResourceView(m_aoBlurTarget.Get(), nullptr, &m_aoBlurResource))) {
			LogError << "Failed to create " << size.x << "x" << size.y << " Direct3D 11 ambient occlusion target";
			releaseSceneTargets();
			return false;
		}
	}
	
	// Bloom chain from half the scene size down
	if(m_bloom) {
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		desc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
		for(size_t i = 0; i < BloomLevels; i++) {
			m_bloomSizes[i] = Vec2i(std::max<s32>(size.x >> (i + 1), 1), std::max<s32>(size.y >> (i + 1), 1));
			desc.Width = UINT(m_bloomSizes[i].x);
			desc.Height = UINT(m_bloomSizes[i].y);
			if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_bloomTargets[i]))
			   || FAILED(m_device->CreateRenderTargetView(m_bloomTargets[i].Get(), nullptr, &m_bloomViews[i]))
			   || FAILED(m_device->CreateShaderResourceView(m_bloomTargets[i].Get(), nullptr, &m_bloomResources[i]))) {
				LogError << "Failed to create " << desc.Width << "x" << desc.Height << " Direct3D 11 bloom target";
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
	unbindTextureView(m_sceneStencilResource.Get());
	unbindTextureView(m_motionResource.Get());
	unbindTextureView(m_linearDepthResource.Get());
	unbindTextureView(m_objectMotionResource.Get());
	unbindTextureView(m_aoResource.Get());
	unbindTextureView(m_aoBlurResource.Get());
	unbindTextureView(m_surfaceResource.Get());
	unbindTextureView(m_reflectionColorResource.Get());
	unbindTextureView(m_shadingResource.Get());

	m_shadingResource.Reset();
	m_shadingResolved.Reset();
	m_shadingView.Reset();
	m_shadingTarget.Reset();
	m_aoResource.Reset();
	m_aoView.Reset();
	m_aoTarget.Reset();
	m_aoBlurResource.Reset();
	m_aoBlurView.Reset();
	m_aoBlurTarget.Reset();
	m_surfaceResource.Reset();
	m_surfaceResolved.Reset();
	m_surfaceView.Reset();
	m_surfaceTarget.Reset();
	m_reflectionColorResource.Reset();
	m_reflectionColor.Reset();
	m_reflectionColorLevels = 0;
	for(size_t i = 0; i < BloomLevels; i++) {
		unbindTextureView(m_bloomResources[i].Get());
		m_bloomResources[i].Reset();
		m_bloomViews[i].Reset();
		m_bloomTargets[i].Reset();
	}
	m_sceneResolvedTargetView.Reset();
	m_sceneResolvedView.Reset();
	m_sceneResolved.Reset();
	m_sceneColorView.Reset();
	m_sceneColorTarget.Reset();
	m_sceneDepthResource.Reset();
	m_sceneStencilResource.Reset();
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
	return m_sceneScale < 1.f || m_sceneDebugView != 0 || m_jitterEnabled || m_ambientOcclusion || m_bloom
	       || m_colorGrading != 0 || m_reflectionStrength > 0.f || shadowsEnabled();
}

bool D3D11Renderer::surfaceTargetNeeded() const {
	return m_reflectionStrength > 0.f || shadowsEnabled();
}

bool D3D11Renderer::shadowsEnabled() const {
	return m_shadowQuality > 0 && m_shadowWorldResource && m_shadowModelResource;
}

void D3D11Renderer::setColorGrading(std::string_view preset) {
	
	size_t index = 0;
	for(size_t i = 0; i < std::size(ColorGradingPresets); i++) {
		if(preset == ColorGradingPresets[i].name) {
			index = i;
		}
	}
	
	if(index != 0 && m_colorGrading == 0 && !m_inScene) {
		// The scene is rendered to separate targets from now on
		releaseSceneTargets();
	}
	
	m_colorGrading = index;
	
}

bool D3D11Renderer::upscalerInputsNeeded() const {
	// Ambient occlusion, reflections and shadows use the depth written by the motion pass
	return m_sceneDebugView != 0 || m_jitterEnabled || m_ambientOcclusion || m_reflectionStrength > 0.f
	       || shadowsEnabled();
}

void D3D11Renderer::setAmbientOcclusion(float strength) {
	
	m_ambientOcclusionStrength = glm::clamp(strength, 0.f, 1.f);
	
	bool enabled = (m_ambientOcclusionStrength > 0.f);
	if(enabled == m_ambientOcclusion) {
		return;
	}
	
	m_ambientOcclusion = enabled;
	
	if(!m_inScene) {
		releaseSceneTargets();
	}
	
}

void D3D11Renderer::setReflections(float strength) {
	
	bool wasEnabled = (m_reflectionStrength > 0.f);
	m_reflectionStrength = glm::clamp(strength, 0.f, 1.f);
	m_stateValid = false;
	
	if(wasEnabled != (m_reflectionStrength > 0.f) && !m_inScene) {
		releaseSceneTargets();
	}
	
}

void D3D11Renderer::setNormalMaps(float strength) {
	
	bool wasEnabled = reliefMapsEnabled();
	m_normalMapStrength = glm::clamp(strength, 0.f, 1.f);
	updateReliefMaps(wasEnabled);
	
}

void D3D11Renderer::setParallaxMapping(float strength) {
	
	bool wasEnabled = reliefMapsEnabled();
	m_parallaxStrength = glm::clamp(strength, 0.f, 1.f);
	updateReliefMaps(wasEnabled);
	
}

void D3D11Renderer::setSpecular(float strength) {
	
	m_specularStrength = glm::clamp(strength, 0.f, 1.f);
	m_stateValid = false;
	
}

void D3D11Renderer::updateReliefMaps(bool wasEnabled) {
	
	m_stateValid = false;
	
	bool enabled = reliefMapsEnabled();
	if(enabled == wasEnabled) {
		return;
	}
	
	// Generate the relief maps for the loaded textures, or free them
	for(D3D11Texture & texture : m_textures) {
		if(enabled) {
			texture.updateNormalMap();
		} else {
			texture.releaseNormalMap();
		}
	}
	
}

void D3D11Renderer::generateNormalMap(ID3D11ShaderResourceView * color, const Vec2i & size,
                                      ComPtr<ID3D11Texture2D> & texture, ComPtr<ID3D11ShaderResourceView> & view) {
	
	if(!m_normalMapPixelShader || !color || size.x <= 0 || size.y <= 0) {
		return;
	}
	
	D3D11_TEXTURE2D_DESC desc = { };
	desc.Width = UINT(size.x);
	desc.Height = UINT(size.y);
	desc.MipLevels = 0;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
	
	ComPtr<ID3D11RenderTargetView> target;
	D3D11_RENDER_TARGET_VIEW_DESC targetDesc = { };
	targetDesc.Format = desc.Format;
	targetDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	targetDesc.Texture2D.MipSlice = 0;
	if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &texture))
	   || FAILED(m_device->CreateRenderTargetView(texture.Get(), &targetDesc, &target))
	   || FAILED(m_device->CreateShaderResourceView(texture.Get(), nullptr, &view))) {
		LogError << "Failed to create " << size.x << "x" << size.y << " Direct3D 11 normal map";
		texture.Reset();
		view.Reset();
		return;
	}
	
	ID3D11ShaderResourceView * none = nullptr;
	m_context->PSSetShaderResources(0, 1, &none);
	ID3D11RenderTargetView * targets[] = { target.Get() };
	m_context->OMSetRenderTargets(1, targets, nullptr);
	D3D11_VIEWPORT viewport = { 0.f, 0.f, float(size.x), float(size.y), 0.f, 1.f };
	m_context->RSSetViewports(1, &viewport);
	m_context->RSSetState(getRasterizerState(false, 0, false, false));
	m_context->OMSetBlendState(getBlendState(BlendOne, BlendZero, false, true), nullptr, 0xffffffff);
	m_context->OMSetDepthStencilState(getDepthState(false, false), 0);
	m_context->PSSetShaderResources(0, 1, &color);
	
	drawFullscreen(m_normalMapPixelShader.Get());
	
	m_context->PSSetShaderResources(0, 1, &none);
	m_context->OMSetRenderTargets(0, nullptr, nullptr);
	m_context->GenerateMips(view.Get());
	
	// Restore the render targets and state for the next draw
	m_currentViews[0] = nullptr;
	invalidateCachedState();
	
}

void D3D11Renderer::setBloom(float strength) {
	
	m_bloomStrength = glm::clamp(strength, 0.f, 1.f);
	
	bool enabled = (m_bloomStrength > 0.f);
	if(enabled == m_bloom) {
		return;
	}
	
	m_bloom = enabled;
	
	if(!m_inScene) {
		releaseSceneTargets();
	}
	
}

ID3D11PixelShader * D3D11Renderer::sceneMaskShader(ID3D11PixelShader * shader, ID3D11PixelShader * multisampled,
                                                   ID3D11ShaderResourceView * & mask) const {

	mask = m_sceneStencilResource.Get();
	ID3D11PixelShader * result = (m_sceneSamples > 1) ? multisampled : shader;
	if(!mask || !result) {
		// The shader reads no stencil: no pixel is left out
		mask = nullptr;
		result = shader;
	}

	return result;
}

void D3D11Renderer::renderBloom(bool apply) {
	
	if(!m_bloomViews[0] || !m_sceneResolvedTargetView || !m_bloomPrefilterPixelShader || !m_bloomDownsamplePixelShader
	   || !m_bloomUpsamplePixelShader || !m_bloomCompositePixelShader) {
		return;
	}
	
	m_context->RSSetState(getRasterizerState(false, 0, false, false));
	m_context->OMSetDepthStencilState(getDepthState(false, false), 0);
	ID3D11SamplerState * sampler = m_blitSampler.Get();
	m_context->PSSetSamplers(0, 1, &sampler);
	ID3D11Buffer * buffer = m_bloomConstants.Get();
	m_context->PSSetConstantBuffers(4, 1, &buffer);
	
	ID3D11ShaderResourceView * none = nullptr;
	
	// Each eye of the stereo scene is blurred on its own: the same part of every level
	std::vector<Vec4f> regions;
	std::vector<Vec2f> depthRanges; // near and far plane of each view, for the distance fade
	for(const SceneRegion & region : sceneRegions()) {
		regions.emplace_back(float(region.pixels.left) / float(m_sceneSize.x),
		                     float(region.pixels.top) / float(m_sceneSize.y),
		                     float(region.pixels.width()) / float(m_sceneSize.x),
		                     float(region.pixels.height()) / float(m_sceneSize.y));
		// See renderAmbientOcclusion()
		float q = region.projection[2][2];
		float nearPlane = (q != 0.f) ? -region.projection[3][2] / q : 1.f;
		float farPlane = (q > 1.f) ? nearPlane * q / (q - 1.f) : nearPlane * 10000.f;
		depthRanges.emplace_back(nearPlane, farPlane);
	}

	auto pass = [&](ID3D11RenderTargetView * target, const Vec2i & targetSize, ID3D11ShaderResourceView * source,
	                const Vec2i & sourceSize, ID3D11PixelShader * shader, bool additive,
	                ID3D11ShaderResourceView * mask = nullptr, ID3D11ShaderResourceView * depth = nullptr) {

		m_context->PSSetShaderResources(0, 1, &none);
		m_context->OMSetRenderTargets(1, &target, nullptr);
		m_context->PSSetShaderResources(0, 1, &source);
		m_context->PSSetShaderResources(1, 1, &depth);
		m_context->PSSetShaderResources(4, 1, &mask);
		BlendingFactor dst = additive ? BlendOne : BlendZero;
		m_context->OMSetBlendState(getBlendState(BlendOne, dst, false, true), nullptr, 0xffffffff);

		for(size_t i = 0; i < regions.size(); i++) {

			const Vec4f & region = regions[i];
			BloomConstants constants = { };
			if(depth) {
				constants.fade[0] = BloomFadeStart;
				constants.fade[1] = BloomFadeLength;
				constants.fade[2] = depthRanges[i].x;
				constants.fade[3] = depthRanges[i].y;
			}
			constants.params[0] = BloomThreshold;
			constants.params[1] = BloomKnee;
			// The upsampled levels add up, normalize the result
			constants.params[2] = 6.f * m_bloomStrength / float(BloomLevels);
			constants.source[0] = 1.f / float(sourceSize.x);
			constants.source[1] = 1.f / float(sourceSize.y);
			constants.source[2] = float(sourceSize.x);
			constants.source[3] = float(sourceSize.y);
			constants.clamp[0] = region.x + 0.5f * constants.source[0];
			constants.clamp[1] = region.y + 0.5f * constants.source[1];
			constants.clamp[2] = region.x + region.z - 0.5f * constants.source[0];
			constants.clamp[3] = region.y + region.w - 0.5f * constants.source[1];
			m_context->UpdateSubresource(m_bloomConstants.Get(), 0, nullptr, &constants, 0, 0);
			
			D3D11_VIEWPORT viewport = {
				region.x * float(targetSize.x), region.y * float(targetSize.y),
				region.z * float(targetSize.x), region.w * float(targetSize.y), 0.f, 1.f
			};
			m_context->RSSetViewports(1, &viewport);
			drawFullscreen(shader, region);
			
		}
	};
	
	// Without the pixels marked in the stencil (see setSceneEffectsExcluded)
	ID3D11ShaderResourceView * mask = nullptr;
	ID3D11PixelShader * prefilter = sceneMaskShader(m_bloomPrefilterPixelShader.Get(),
	                                                m_bloomPrefilterPixelShaderMS.Get(), mask);
	// The depth for the distance fade, if the shader reads it the way it was created (multisampled or not)
	bool multisampled = (prefilter == m_bloomPrefilterPixelShaderMS.Get());
	ID3D11ShaderResourceView * depth = (multisampled == (m_sceneSamples > 1)) ? m_sceneDepthResource.Get() : nullptr;
	pass(m_bloomViews[0].Get(), m_bloomSizes[0], m_sceneResolvedView.Get(), m_sceneSize, prefilter, false, mask, depth);
	m_context->PSSetShaderResources(1, 1, &none);
	m_context->PSSetShaderResources(4, 1, &none);
	for(size_t i = 1; i < BloomLevels; i++) {
		pass(m_bloomViews[i].Get(), m_bloomSizes[i], m_bloomResources[i - 1].Get(), m_bloomSizes[i - 1],
		     m_bloomDownsamplePixelShader.Get(), false);
	}
	for(size_t i = BloomLevels - 1; i > 0; i--) {
		pass(m_bloomViews[i - 1].Get(), m_bloomSizes[i - 1], m_bloomResources[i].Get(), m_bloomSizes[i],
		     m_bloomUpsamplePixelShader.Get(), true);
	}
	
	if(apply) {
		pass(m_sceneResolvedTargetView.Get(), m_sceneSize, m_bloomResources[0].Get(), m_bloomSizes[0],
		     m_bloomCompositePixelShader.Get(), true);
	}
	
	m_context->PSSetShaderResources(0, 1, &none);
	m_context->OMSetRenderTargets(0, nullptr, nullptr);
	m_currentViews[0] = nullptr;
	m_currentSamplers[0] = sampler;
	
}

void D3D11Renderer::renderAmbientOcclusion(bool apply) {
	
	if(!m_aoView || !m_aoBlurView || !m_linearDepthResource || !m_sceneResolvedTargetView || !m_aoPixelShader
	   || !m_aoBlurPixelShader || !m_aoBlurVerticalPixelShader || !m_aoApplyPixelShader) {
		return;
	}
	
	ID3D11Buffer * buffer = m_aoConstants.Get();
	m_context->PSSetConstantBuffers(3, 1, &buffer);
	m_context->RSSetState(getRasterizerState(false, 0, false, false));
	m_context->OMSetDepthStencilState(getDepthState(false, false), 0);
	
	ID3D11ShaderResourceView * none[2] = { };
	
	auto pass = [&](ID3D11RenderTargetView * target, ID3D11ShaderResourceView * input, ID3D11PixelShader * shader) {
		m_context->PSSetShaderResources(0, 2, none);
		m_context->OMSetRenderTargets(1, &target, nullptr);
		ID3D11ShaderResourceView * inputs[] = { m_linearDepthResource.Get(), input };
		m_context->PSSetShaderResources(0, 2, inputs);
		drawFullscreen(shader);
	};
	
	// Each eye of the stereo scene on its own, with its projection
	for(const SceneRegion & region : sceneRegions()) {
		
		// See runUpscaler() for the projection layout
		const glm::mat4x4 & projection = region.projection;
		float q = projection[2][2];
		float nearPlane = (q != 0.f) ? -projection[3][2] / q : 1.f;
		float farPlane = (q > 1.f) ? nearPlane * q / (q - 1.f) : nearPlane * 10000.f;
		float scaleX = std::max(std::abs(projection[0][0]), 0.0001f);
		float scaleY = std::max(std::abs(projection[1][1]), 0.0001f);
		
		AmbientOcclusionConstants constants;
		constants.target[0] = float(m_sceneSize.x);
		constants.target[1] = float(m_sceneSize.y);
		constants.target[2] = 1.f / float(m_sceneSize.x);
		constants.target[3] = 1.f / float(m_sceneSize.y);
		constants.projection[0] = 1.f / scaleX;
		constants.projection[1] = 1.f / scaleY;
		constants.projection[2] = nearPlane;
		constants.projection[3] = farPlane;
		constants.params[0] = AmbientOcclusionRadius;
		constants.params[1] = m_ambientOcclusionStrength;
		constants.params[2] = AmbientOcclusionBias;
		// Pixels per world unit at distance 1
		constants.params[3] = 0.5f * float(region.pixels.height()) * scaleY;
		constants.region[0] = float(region.pixels.left);
		constants.region[1] = float(region.pixels.top);
		constants.region[2] = float(region.pixels.width());
		constants.region[3] = float(region.pixels.height());
		constants.center[0] = projection[2][0];
		constants.center[1] = projection[2][1];
		constants.center[2] = constants.center[3] = 0.f;
		m_context->UpdateSubresource(m_aoConstants.Get(), 0, nullptr, &constants, 0, 0);
		
		// The ambient occlusion is computed at half the size and upsampled when it is applied
		s32 halfLeft = region.pixels.left / 2;
		s32 halfTop = region.pixels.top / 2;
		D3D11_VIEWPORT halfViewport = {
			float(halfLeft), float(halfTop), float((region.pixels.right + 1) / 2 - halfLeft),
			float((region.pixels.bottom + 1) / 2 - halfTop), 0.f, 1.f
		};
		m_context->RSSetViewports(1, &halfViewport);
		m_context->OMSetBlendState(getBlendState(BlendOne, BlendZero, false, true), nullptr, 0xffffffff);
		
		pass(m_aoView.Get(), nullptr, m_aoPixelShader.Get());
		pass(m_aoBlurView.Get(), m_aoResource.Get(), m_aoBlurPixelShader.Get());
		pass(m_aoView.Get(), m_aoBlurResource.Get(), m_aoBlurVerticalPixelShader.Get());
		
		if(apply) {
			// Darken the scene by multiplying it with the ambient occlusion
			D3D11_VIEWPORT viewport = {
				float(region.pixels.left), float(region.pixels.top), float(region.pixels.width()),
				float(region.pixels.height()), 0.f, 1.f
			};
			m_context->RSSetViewports(1, &viewport);
			m_context->OMSetBlendState(getBlendState(BlendZero, BlendSrcColor, false, true), nullptr, 0xffffffff);
			// Not the pixels marked in the stencil (see setSceneEffectsExcluded)
			ID3D11ShaderResourceView * mask = nullptr;
			ID3D11PixelShader * shader = sceneMaskShader(m_aoApplyPixelShader.Get(), m_aoApplyPixelShaderMS.Get(), mask);
			m_context->PSSetShaderResources(4, 1, &mask);
			pass(m_sceneResolvedTargetView.Get(), m_aoResource.Get(), shader);
			m_context->PSSetShaderResources(4, 1, none);
		}
		
	}
	
	m_context->PSSetShaderResources(0, 2, none);
	m_context->OMSetRenderTargets(0, nullptr, nullptr);
	m_currentViews[0] = nullptr;
	m_currentViews[1] = nullptr;
	
}

void D3D11Renderer::renderReflections() {
	
	if(!m_reflectionPixelShader || !m_surfaceResource || !m_reflectionColorResource || !m_linearDepthResource
	   || !m_sceneResolvedTargetView) {
		return;
	}
	
	// The reflections show the scene drawn so far, blurred with the mipmaps for rough surfaces
	unbindTextureView(m_reflectionColorResource.Get());
	m_context->CopySubresourceRegion(m_reflectionColor.Get(), 0, 0, 0, 0, m_sceneResolved.Get(), 0, nullptr);
	m_context->GenerateMips(m_reflectionColorResource.Get());
	
	ID3D11Buffer * buffer = m_reflectionConstants.Get();
	m_context->PSSetConstantBuffers(7, 1, &buffer);
	m_context->RSSetState(getRasterizerState(false, 0, false, false));
	m_context->OMSetDepthStencilState(getDepthState(false, false), 0);
	// The debug view shows only the reflections
	bool debugView = (m_sceneDebugView == 7);
	// The shader returns the premultiplied reflection and how much of the scene color it replaces
	BlendingFactor src = BlendOne;
	BlendingFactor dst = debugView ? BlendZero : BlendInvSrcAlpha;
	m_context->OMSetBlendState(getBlendState(src, dst, false, true), nullptr, 0xffffffff);
	
	ID3D11RenderTargetView * target = m_sceneResolvedTargetView.Get();
	if(debugView) {
		const float black[4] = { 0.f, 0.f, 0.f, 1.f };
		m_context->ClearRenderTargetView(target, black);
	}
	m_context->OMSetRenderTargets(1, &target, nullptr);
	ID3D11ShaderResourceView * inputs[] = {
		m_linearDepthResource.Get(), m_surfaceResource.Get(), m_reflectionColorResource.Get()
	};
	m_context->PSSetShaderResources(0, UINT(std::size(inputs)), inputs);
	ID3D11SamplerState * sampler = m_blitSampler.Get();
	m_context->PSSetSamplers(0, 1, &sampler);
	
	// Each eye of the stereo scene on its own, with its projection
	for(const SceneRegion & region : sceneRegions()) {
		
		// See renderAmbientOcclusion() for the depth reconstruction
		const glm::mat4x4 & projection = region.projection;
		float q = projection[2][2];
		float nearPlane = (q != 0.f) ? -projection[3][2] / q : 1.f;
		float farPlane = (q > 1.f) ? nearPlane * q / (q - 1.f) : nearPlane * 10000.f;
		
		ReflectionConstants constants = {};
		constants.target[0] = float(m_sceneSize.x);
		constants.target[1] = float(m_sceneSize.y);
		constants.target[2] = 1.f / float(m_sceneSize.x);
		constants.target[3] = 1.f / float(m_sceneSize.y);
		constants.projection[0] = 1.f / std::max(std::abs(projection[0][0]), 0.0001f);
		constants.projection[1] = 1.f / std::max(std::abs(projection[1][1]), 0.0001f);
		constants.projection[2] = nearPlane;
		constants.projection[3] = farPlane;
		// The reconstructed view positions are the view space positions with the signs of the projection
		const float signs[3] = {
			glm::sign(projection[0][0]), glm::sign(projection[1][1]), glm::sign(projection[2][3])
		};
		for(size_t row = 0; row < 3; row++) {
			for(size_t column = 0; column < 3; column++) {
				constants.view[row][column] = signs[row] * region.view[column][row];
			}
		}
		constants.params[0] = m_reflectionStrength;
		constants.params[1] = ReflectionDistance;
		constants.params[2] = float(m_reflectionColorLevels);
		constants.params[3] = debugView ? 1.f : 0.f;
		constants.region[0] = float(region.pixels.left);
		constants.region[1] = float(region.pixels.top);
		constants.region[2] = float(region.pixels.width());
		constants.region[3] = float(region.pixels.height());
		constants.center[0] = projection[2][0];
		constants.center[1] = projection[2][1];
		m_context->UpdateSubresource(m_reflectionConstants.Get(), 0, nullptr, &constants, 0, 0);
		
		D3D11_VIEWPORT viewport = {
			float(region.pixels.left), float(region.pixels.top), float(region.pixels.width()),
			float(region.pixels.height()), 0.f, 1.f
		};
		m_context->RSSetViewports(1, &viewport);
		
		drawFullscreen(m_reflectionPixelShader.Get());
		
	}
	
	ID3D11ShaderResourceView * none[std::size(inputs)] = { };
	m_context->PSSetShaderResources(0, UINT(std::size(none)), none);
	m_context->OMSetRenderTargets(0, nullptr, nullptr);
	m_currentViews[0] = nullptr;
	m_currentViews[1] = nullptr;
	m_currentViews[2] = nullptr;
	m_currentSamplers[0] = sampler;
	
}

void D3D11Renderer::setShadows(int quality) {

	static_assert(MaxShadowMapLights == MaxShadowLights, "shadow light limits must match");

	quality = glm::clamp(quality, 0, int(std::size(ShadowQualities)) - 1);
	if(!m_shadowVertexShader || !m_shadowCasterPixelShader || !m_shadowPixelShader) {
		quality = 0;
	}
	if(quality == m_shadowQuality) {
		return;
	}

	bool wasEnabled = shadowsEnabled();

	m_shadowQuality = quality;
	if(!updateShadowMaps()) {
		m_shadowQuality = 0;
		releaseShadowMaps();
	}
	m_stateValid = false;

	if(wasEnabled != shadowsEnabled() && !m_inScene) {
		// The scene targets for the shadows are created or freed
		releaseSceneTargets();
	}

}

size_t D3D11Renderer::getShadowLightLimit() const {
	return shadowsEnabled() ? m_shadowMapLights : 0;
}

void D3D11Renderer::setShadowLights(const ShadowLight * lights, size_t count) {
	m_shadowLights.assign(lights, lights + std::min(count, m_shadowMapLights));
}

void D3D11Renderer::releaseShadowMaps() {

	m_shadowWorldResource.Reset();
	m_shadowModelResource.Reset();
	m_shadowWorldFaces.clear();
	m_shadowModelFaces.clear();
	m_shadowWorldMaps.Reset();
	m_shadowModelMaps.Reset();
	m_shadowMapSize = 0;
	m_shadowMapLights = 0;
	for(ShadowSlot & slot : m_shadowSlots) {
		slot = ShadowSlot();
	}

}

bool D3D11Renderer::updateShadowMaps() {

	const ShadowQuality & quality = ShadowQualities[m_shadowQuality];
	if(quality.lights == 0) {
		releaseShadowMaps();
		return true;
	}
	if(quality.lights == m_shadowMapLights && quality.size == m_shadowMapSize) {
		return true;
	}

	releaseShadowMaps();

	if(!m_shadowSampler) {
		D3D11_SAMPLER_DESC desc = {};
		desc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
		desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
		desc.MaxAnisotropy = 1;
		desc.MaxLOD = D3D11_FLOAT32_MAX;
		if(FAILED(m_device->CreateSamplerState(&desc, &m_shadowSampler))) {
			LogError << "Failed to create Direct3D 11 shadow sampler";
			return false;
		}
	}

	if(!m_shadowRasterizer) {
		// Models and the world have open and single-sided geometry, draw both sides
		D3D11_RASTERIZER_DESC desc = {};
		desc.FillMode = D3D11_FILL_SOLID;
		desc.CullMode = D3D11_CULL_NONE;
		desc.FrontCounterClockwise = TRUE;
		desc.SlopeScaledDepthBias = 1.5f;
		desc.DepthClipEnable = TRUE;
		if(FAILED(m_device->CreateRasterizerState(&desc, &m_shadowRasterizer))) {
			LogError << "Failed to create Direct3D 11 shadow rasterizer state";
			return false;
		}
	}

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = desc.Height = quality.size;
	desc.MipLevels = 1;
	desc.ArraySize = UINT(6 * quality.lights);
	desc.Format = DXGI_FORMAT_R32_TYPELESS;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
	desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

	D3D11_SHADER_RESOURCE_VIEW_DESC resourceDesc = {};
	resourceDesc.Format = DXGI_FORMAT_R32_FLOAT;
	resourceDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
	resourceDesc.TextureCubeArray.MostDetailedMip = 0;
	resourceDesc.TextureCubeArray.MipLevels = 1;
	resourceDesc.TextureCubeArray.First2DArrayFace = 0;
	resourceDesc.TextureCubeArray.NumCubes = UINT(quality.lights);

	auto createMaps = [&](ComPtr<ID3D11Texture2D> & texture, std::vector<ComPtr<ID3D11DepthStencilView>> & faces,
	                      ComPtr<ID3D11ShaderResourceView> & resource) {
		if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &texture))
		   || FAILED(m_device->CreateShaderResourceView(texture.Get(), &resourceDesc, &resource))) {
			return false;
		}
		faces.resize(desc.ArraySize);
		for(UINT i = 0; i < desc.ArraySize; i++) {
			D3D11_DEPTH_STENCIL_VIEW_DESC faceDesc = {};
			faceDesc.Format = DXGI_FORMAT_D32_FLOAT;
			faceDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
			faceDesc.Texture2DArray.MipSlice = 0;
			faceDesc.Texture2DArray.FirstArraySlice = i;
			faceDesc.Texture2DArray.ArraySize = 1;
			if(FAILED(m_device->CreateDepthStencilView(texture.Get(), &faceDesc, &faces[i]))) {
				return false;
			}
		}
		return true;
	};

	if(!createMaps(m_shadowWorldMaps, m_shadowWorldFaces, m_shadowWorldResource)
	   || !createMaps(m_shadowModelMaps, m_shadowModelFaces, m_shadowModelResource)) {
		LogError << "Failed to create " << quality.lights << " Direct3D 11 shadow cube maps of " << quality.size << "x"
		         << quality.size;
		releaseShadowMaps();
		return false;
	}

	m_shadowMapSize = quality.size;
	m_shadowMapLights = quality.lights;

	LogInfo << "Dynamic shadows for " << quality.lights << " lights with " << quality.size << "x" << quality.size
	        << " cube maps";

	return true;
}

void D3D11Renderer::setShadowOccluders(const Vec3f * vertices, size_t count) {

	m_shadowOccluders.Reset();
	m_shadowChunks.clear();
	for(ShadowSlot & slot : m_shadowSlots) {
		slot.worldValid = false;
	}

	size_t triangles = count / 3;
	if(!vertices || triangles == 0 || !m_device) {
		return;
	}

	// Sort the triangles into cells of the horizontal plane
	std::vector<std::pair<u64, u32>> cells(triangles);
	for(size_t i = 0; i < triangles; i++) {
		Vec3f center = (vertices[i * 3] + vertices[i * 3 + 1] + vertices[i * 3 + 2]) * (1.f / 3.f);
		u64 x = u64(u32(s32(std::floor(center.x / ShadowChunkSize)) + 0x8000));
		u64 z = u64(u32(s32(std::floor(center.z / ShadowChunkSize)) + 0x8000));
		cells[i] = { (x << 32) | z, u32(i) };
	}
	std::sort(cells.begin(), cells.end());

	std::vector<Vec3f> sorted;
	sorted.reserve(triangles * 3);
	for(size_t i = 0; i < triangles; i++) {
		if(i == 0 || cells[i].first != cells[i - 1].first) {
			ShadowChunk & chunk = m_shadowChunks.emplace_back();
			chunk.start = UINT(sorted.size());
			chunk.count = 0;
			chunk.min = Vec3f(std::numeric_limits<float>::max());
			chunk.max = Vec3f(-std::numeric_limits<float>::max());
		}
		ShadowChunk & chunk = m_shadowChunks.back();
		for(size_t j = 0; j < 3; j++) {
			const Vec3f & vertex = vertices[size_t(cells[i].second) * 3 + j];
			sorted.push_back(vertex);
			chunk.min = glm::min(chunk.min, vertex);
			chunk.max = glm::max(chunk.max, vertex);
		}
		chunk.count += 3;
	}

	D3D11_BUFFER_DESC desc = {};
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	desc.ByteWidth = UINT(sorted.size() * sizeof(Vec3f));
	D3D11_SUBRESOURCE_DATA data = { sorted.data(), 0, 0 };
	if(FAILED(m_device->CreateBuffer(&desc, &data, &m_shadowOccluders))) {
		LogError << "Failed to create the Direct3D 11 shadow occluders for " << triangles << " triangles";
		m_shadowChunks.clear();
		return;
	}

	LogDebug("Shadow occluders: " << triangles << " triangles in " << m_shadowChunks.size() << " chunks");

}

void D3D11Renderer::addShadowCasters(const Vec3f * vertices, size_t count) {

	if(!m_inScene || !shadowsEnabled() || m_shadowLights.empty() || !vertices || count < 3) {
		return;
	}

	if(m_shadowCasters.empty()) {
		m_shadowCastersMin = Vec3f(std::numeric_limits<float>::max());
		m_shadowCastersMax = Vec3f(-std::numeric_limits<float>::max());
	}

	count -= count % 3;
	m_shadowCasters.insert(m_shadowCasters.end(), vertices, vertices + count);
	for(size_t i = 0; i < count; i++) {
		m_shadowCastersMin = glm::min(m_shadowCastersMin, vertices[i]);
		m_shadowCastersMax = glm::max(m_shadowCastersMax, vertices[i]);
	}

}

void D3D11Renderer::renderShadowMaps(size_t * cubes) {

	size_t lightCount = std::min(m_shadowLights.size(), m_shadowMapLights);

	// Lights that did not move keep their cube map, so that the world geometry is drawn only once for static lights
	bool taken[MaxShadowLights] = { };
	for(size_t i = 0; i < lightCount; i++) {
		cubes[i] = size_t(-1);
		const ShadowLight & light = m_shadowLights[i];
		for(size_t slot = 0; slot < m_shadowMapLights; slot++) {
			const ShadowSlot & cube = m_shadowSlots[slot];
			if(!taken[slot] && cube.used && cube.light.position == light.position
			   && cube.light.fallEnd == light.fallEnd) {
				cubes[i] = slot;
				taken[slot] = true;
				break;
			}
		}
	}
	for(size_t i = 0; i < lightCount; i++) {
		if(cubes[i] != size_t(-1)) {
			continue;
		}
		// Prefer slots that are not used by any light, they may be needed again for a light that comes back
		size_t best = size_t(-1);
		for(size_t slot = 0; slot < m_shadowMapLights; slot++) {
			if(!taken[slot] && (best == size_t(-1) || (m_shadowSlots[best].used && !m_shadowSlots[slot].used))) {
				best = slot;
			}
		}
		arx_assert(best != size_t(-1));
		cubes[i] = best;
		taken[best] = true;
		m_shadowSlots[best].used = true;
		m_shadowSlots[best].light = m_shadowLights[i];
		m_shadowSlots[best].worldValid = false;
	}

	// Upload the model triangles of this frame
	bool hasCasters = false;
	if(!m_shadowCasters.empty()) {
		if(m_shadowCasters.size() > m_shadowCasterCapacity) {
			m_shadowCasterBuffer.Reset();
			m_shadowCasterCapacity = 0;
			size_t capacity = std::max<size_t>(m_shadowCasters.size() * 3 / 2, 64 * 1024);
			D3D11_BUFFER_DESC desc = {};
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
			desc.ByteWidth = UINT(capacity * sizeof(Vec3f));
			if(SUCCEEDED(m_device->CreateBuffer(&desc, nullptr, &m_shadowCasterBuffer))) {
				m_shadowCasterCapacity = capacity;
			}
		}
		D3D11_MAPPED_SUBRESOURCE mapped;
		if(m_shadowCasterBuffer
		   && SUCCEEDED(m_context->Map(m_shadowCasterBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			std::memcpy(mapped.pData, m_shadowCasters.data(), m_shadowCasters.size() * sizeof(Vec3f));
			m_context->Unmap(m_shadowCasterBuffer.Get(), 0);
			hasCasters = true;
		}
	}

	bindTessellation(false);
	m_context->IASetInputLayout(m_shadowInputLayout.Get());
	m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_context->VSSetShader(m_shadowVertexShader.Get(), nullptr, 0);
	ID3D11Buffer * constants = m_shadowPassConstants.Get();
	m_context->VSSetConstantBuffers(8, 1, &constants);
	m_context->PSSetConstantBuffers(8, 1, &constants);
	m_context->RSSetState(m_shadowRasterizer.Get());
	m_context->OMSetBlendState(getBlendState(BlendOne, BlendZero, false, false), nullptr, 0xffffffff);
	m_context->OMSetDepthStencilState(getDepthState(true, true), 0);
	D3D11_VIEWPORT viewport = { 0.f, 0.f, float(m_shadowMapSize), float(m_shadowMapSize), 0.f, 1.f };
	m_context->RSSetViewports(1, &viewport);
	const UINT stride = sizeof(Vec3f);
	const UINT offset = 0;

	for(size_t i = 0; i < lightCount; i++) {

		ShadowSlot & slot = m_shadowSlots[cubes[i]];
		const ShadowLight & light = m_shadowLights[i];
		float farPlane = std::max(light.fallEnd, ShadowNearPlane * 2.f);
		bool drawWorld = !slot.worldValid;
		bool drawModels = hasCasters && boxIntersectsSphere(m_shadowCastersMin, m_shadowCastersMax, light.position,
		                                                    farPlane);

		for(size_t face = 0; face < 6; face++) {

			size_t index = cubes[i] * 6 + face;

			ShadowPassConstants pass;
			glm::mat4x4 transform = shadowFaceTransform(face, ShadowNearPlane, farPlane);
			std::memcpy(pass.face, &transform, sizeof(pass.face));
			pass.light[0] = light.position.x;
			pass.light[1] = light.position.y;
			pass.light[2] = light.position.z;
			pass.light[3] = ShadowCasterMinDistance;
			m_context->UpdateSubresource(m_shadowPassConstants.Get(), 0, nullptr, &pass, 0, 0);

			if(drawWorld) {
				ID3D11DepthStencilView * target = m_shadowWorldFaces[index].Get();
				m_context->ClearDepthStencilView(target, D3D11_CLEAR_DEPTH, 1.f, 0);
				if(m_shadowOccluders) {
					m_context->OMSetRenderTargets(0, nullptr, target);
					m_context->PSSetShader(nullptr, nullptr, 0);
					ID3D11Buffer * buffer = m_shadowOccluders.Get();
					m_context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
					for(const ShadowChunk & chunk : m_shadowChunks) {
						if(boxIntersectsSphere(chunk.min, chunk.max, light.position, farPlane)) {
							m_context->Draw(chunk.count, chunk.start);
						}
					}
				}
			}

			ID3D11DepthStencilView * target = m_shadowModelFaces[index].Get();
			m_context->ClearDepthStencilView(target, D3D11_CLEAR_DEPTH, 1.f, 0);
			if(drawModels) {
				m_context->OMSetRenderTargets(0, nullptr, target);
				m_context->PSSetShader(m_shadowCasterPixelShader.Get(), nullptr, 0);
				ID3D11Buffer * buffer = m_shadowCasterBuffer.Get();
				m_context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
				m_context->Draw(UINT(m_shadowCasters.size()), 0);
			}

		}

		slot.worldValid = true;
	}

	m_context->OMSetRenderTargets(0, nullptr, nullptr);
	invalidateCachedState();
	m_currentVertexBuffer = nullptr;
	m_currentPixelShader = nullptr;

}

void D3D11Renderer::renderShadows(bool apply) {

	if(!m_shadowPixelShader || !m_linearDepthResource || !m_surfaceResource || !m_shadingResource
	   || !m_sceneResolvedTargetView) {
		return;
	}

	if(m_shadowLights.empty() && apply) {
		return;
	}

	if(m_shadingResolved.Get() != m_shadingTarget.Get()) {
		m_context->ResolveSubresource(m_shadingResolved.Get(), 0, m_shadingTarget.Get(), 0, DXGI_FORMAT_R8G8_UNORM);
	}

	size_t cubes[MaxShadowLights];
	renderShadowMaps(cubes);

	ID3D11Buffer * buffer = m_shadowConstants.Get();
	m_context->PSSetConstantBuffers(9, 1, &buffer);
	m_context->RSSetState(getRasterizerState(false, 0, false, false));
	m_context->OMSetDepthStencilState(getDepthState(false, false), 0);
	// The shadow factor darkens the scene, or replaces it in the debug view
	BlendingFactor src = apply ? BlendZero : BlendOne;
	BlendingFactor dst = apply ? BlendSrcColor : BlendZero;
	m_context->OMSetBlendState(getBlendState(src, dst, false, true), nullptr, 0xffffffff);

	ID3D11RenderTargetView * target = m_sceneResolvedTargetView.Get();
	m_context->OMSetRenderTargets(1, &target, nullptr);
	ID3D11ShaderResourceView * inputs[] = {
		m_linearDepthResource.Get(), m_surfaceResource.Get(), m_shadingResource.Get()
	};
	m_context->PSSetShaderResources(0, UINT(std::size(inputs)), inputs);
	ID3D11ShaderResourceView * maps[] = { m_shadowWorldResource.Get(), m_shadowModelResource.Get() };
	m_context->PSSetShaderResources(5, UINT(std::size(maps)), maps);
	ID3D11SamplerState * sampler = m_shadowSampler.Get();
	m_context->PSSetSamplers(3, 1, &sampler);
	// Not the pixels marked in the stencil (see setSceneEffectsExcluded)
	ID3D11ShaderResourceView * mask = nullptr;
	ID3D11PixelShader * shader = sceneMaskShader(m_shadowPixelShader.Get(), m_shadowPixelShaderMS.Get(), mask);
	m_context->PSSetShaderResources(4, 1, &mask);

	// Each eye of the stereo scene on its own, with its camera, the cube maps are shared
	size_t lightCount = std::min(m_shadowLights.size(), m_shadowMapLights);
	for(const SceneRegion & region : sceneRegions()) {

		// See renderAmbientOcclusion() and renderReflections() for the depth reconstruction
		const glm::mat4x4 & projection = region.projection;
		float q = projection[2][2];
		float nearPlane = (q != 0.f) ? -projection[3][2] / q : 1.f;
		float farPlane = (q > 1.f) ? nearPlane * q / (q - 1.f) : nearPlane * 10000.f;

		ShadowConstants constants = {};
		constants.target[0] = float(m_sceneSize.x);
		constants.target[1] = float(m_sceneSize.y);
		constants.target[2] = 1.f / float(m_sceneSize.x);
		constants.target[3] = 1.f / float(m_sceneSize.y);
		constants.projection[0] = 1.f / std::max(std::abs(projection[0][0]), 0.0001f);
		constants.projection[1] = 1.f / std::max(std::abs(projection[1][1]), 0.0001f);
		constants.projection[2] = nearPlane;
		constants.projection[3] = farPlane;
		// The inverse of the rotation in renderReflections()
		const float signs[3] = {
			glm::sign(projection[0][0]), glm::sign(projection[1][1]), glm::sign(projection[2][3])
		};
		for(size_t row = 0; row < 3; row++) {
			for(size_t column = 0; column < 3; column++) {
				constants.view[row][column] = region.view[row][column] * signs[column];
			}
		}
		constants.params[0] = float(lightCount);
		constants.params[1] = float(ShadowQualities[m_shadowQuality].taps);
		constants.params[2] = float(m_shadowMapSize);
		constants.fog[0] = m_fogStart;
		constants.fog[1] = m_fogEnd;
		constants.region[0] = float(region.pixels.left);
		constants.region[1] = float(region.pixels.top);
		constants.region[2] = float(region.pixels.width());
		constants.region[3] = float(region.pixels.height());
		constants.center[0] = projection[2][0];
		constants.center[1] = projection[2][1];
		// Light positions relative to the camera, the world coordinates are too large for single precision
		glm::dvec3 camera = glm::dvec3(glm::inverse(glm::dmat4(region.view))[3]);
		for(size_t i = 0; i < lightCount; i++) {
			const ShadowLight & light = m_shadowLights[i];
			float * a = constants.lights[i * 3];
			float * b = constants.lights[i * 3 + 1];
			float * c = constants.lights[i * 3 + 2];
			glm::dvec3 position = glm::dvec3(light.position) - camera;
			a[0] = float(position.x);
			a[1] = float(position.y);
			a[2] = float(position.z);
			a[3] = light.fallStart;
			b[0] = (light.color.r * 0.299f + light.color.g * 0.587f + light.color.b * 0.114f) * light.intensity;
			b[1] = light.fallEnd;
			b[2] = light.strength;
			b[3] = light.baked ? 1.f : 0.f;
			c[0] = ShadowNearPlane;
			c[1] = std::max(light.fallEnd, ShadowNearPlane * 2.f);
			c[2] = float(cubes[i]);
		}
		m_context->UpdateSubresource(m_shadowConstants.Get(), 0, nullptr, &constants, 0, 0);

		D3D11_VIEWPORT viewport = {
			float(region.pixels.left), float(region.pixels.top), float(region.pixels.width()),
			float(region.pixels.height()), 0.f, 1.f
		};
		m_context->RSSetViewports(1, &viewport);

		drawFullscreen(shader);

	}

	ID3D11ShaderResourceView * none[std::size(inputs)] = { };
	m_context->PSSetShaderResources(0, UINT(std::size(none)), none);
	m_context->PSSetShaderResources(4, 1, none);
	m_context->PSSetShaderResources(5, UINT(std::size(maps)), none);
	m_context->OMSetRenderTargets(0, nullptr, nullptr);
	m_currentViews[0] = nullptr;
	m_currentViews[1] = nullptr;
	m_currentViews[2] = nullptr;

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
	
	if(!m_hasPreviousScene || !m_sceneRenderedLastFrame || m_lastSceneStereo != !m_stereoViews.empty()) {
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
	
	// The stereo scene is scaled relative to the eye images
	Vec2f scale = Vec2f(m_sceneSize) / Vec2f(m_stereoViews.empty() ? m_size : m_stereoSize);
	
	// The same jitter for both eyes of the stereo scene, upscaled on their own
	if(m_jitterEnabled && (m_stereoViews.empty() || stereoUpscaling())) {
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

void D3D11Renderer::drawFullscreen(ID3D11PixelShader * shader, const Vec4f & uvRect) {
	
	if(uvRect != m_blitRect) {
		BlitConstants constants = { { uvRect.x, uvRect.y, uvRect.z, uvRect.w } };
		m_context->UpdateSubresource(m_blitConstants.Get(), 0, nullptr, &constants, 0, 0);
		m_blitRect = uvRect;
	}
	ID3D11Buffer * buffer = m_blitConstants.Get();
	m_context->VSSetConstantBuffers(8, 1, &buffer);
	
	bindTessellation(false);
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
	
	ID3D11RenderTargetView * views[] = { m_motionView.Get(), m_linearDepthView.Get() };
	m_context->OMSetRenderTargets(UINT(std::size(views)), views, nullptr);
	
	m_context->RSSetState(getRasterizerState(false, 0, false, false));
	m_context->OMSetBlendState(getBlendState(BlendOne, BlendZero, false, true), nullptr, 0xffffffff);
	m_context->OMSetDepthStencilState(getDepthState(false, false), 0);
	
	ID3D11Buffer * buffer = m_motionConstants.Get();
	m_context->PSSetConstantBuffers(2, 1, &buffer);
	ID3D11ShaderResourceView * inputs[] = { m_sceneDepthResource.Get(), m_objectMotionResource.Get() };
	m_context->PSSetShaderResources(1, UINT(std::size(inputs)), inputs);
	
	// Each eye of the stereo scene with its own camera motion
	for(const SceneRegion & region : sceneRegions()) {
		
		MotionConstants constants;
		glm::mat4x4 reprojection(1.f);
		if(!m_resetHistory) {
			// World coordinates are large (thousands of units) - single precision is not enough to cancel them out
			// and results in sub-pixel noise in the motion vectors. Compute the relative camera motion first.
			glm::dmat4 relative = glm::dmat4(region.previousView) * glm::inverse(glm::dmat4(region.view));
			reprojection = glm::mat4x4(glm::dmat4(region.previousProjection) * relative
			                           * glm::inverse(glm::dmat4(region.projection)));
		}
		std::memcpy(constants.reprojection, &reprojection, sizeof(constants.reprojection));
		constants.targetSize[0] = float(m_sceneSize.x);
		constants.targetSize[1] = float(m_sceneSize.y);
		constants.targetSize[2] = 1.f / float(m_sceneSize.x);
		constants.targetSize[3] = 1.f / float(m_sceneSize.y);
		constants.region[0] = float(region.pixels.left);
		constants.region[1] = float(region.pixels.top);
		constants.region[2] = float(region.pixels.width());
		constants.region[3] = float(region.pixels.height());
		m_context->UpdateSubresource(m_motionConstants.Get(), 0, nullptr, &constants, 0, 0);
		
		D3D11_VIEWPORT viewport = {
			float(region.pixels.left), float(region.pixels.top), float(region.pixels.width()),
			float(region.pixels.height()), 0.f, 1.f
		};
		m_context->RSSetViewports(1, &viewport);
		
		drawFullscreen(shader);
		
	}
	
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
	
	ID3D11RenderTargetView * colorView = m_inScene ? m_sceneColorView.Get() : outputColorView();
	ID3D11DepthStencilView * depthView = m_inScene ? m_sceneDepthView.Get() : outputDepthView();
	
	if(m_inScene && (m_objectMotionView || m_surfaceView || m_shadingView)) {
		ID3D11RenderTargetView * views[] = {
			colorView, m_objectMotionView.Get(), m_surfaceView.Get(), m_shadingView.Get()
		};
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
	
	m_drawingScene = true;

	{
		LARGE_INTEGER now, frequency;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&frequency);
		double seconds = double(now.QuadPart) / double(frequency.QuadPart);
		m_effectTime = float(std::fmod(seconds, double(LavaTimePeriod)));
	}

	updateUpscaler();

	m_shadowCasters.clear();

	float scale = std::min(m_sceneScale, 1.f);
	Vec2i size(std::max<s32>(s32(float(m_size.x) * scale + 0.5f), 1),
	           std::max<s32>(s32(float(m_size.y) * scale + 0.5f), 1));
	UINT samples = m_upscaler ? 1 : m_msaaSamples;
	
	if(!m_stereoViews.empty()) {
		// The stereo scene always has its own targets, the eyes side by side with an even width each
		Vec2i eye(m_stereoSize.x / s32(m_stereoViews.size()), m_stereoSize.y);
		m_stereoEyeSize = Vec2i(std::max<s32>((s32(float(eye.x) * scale + 0.5f) + 1) / 2 * 2, 2),
		                        std::max<s32>(s32(float(eye.y) * scale + 0.5f), 1));
		size = Vec2i(m_stereoEyeSize.x * s32(m_stereoViews.size()), m_stereoEyeSize.y);
		// Temporal upscalers need a scene without MSAA
		samples = (config.video.antialiasing && !stereoUpscaling())
		          ? std::min(m_stereoSamples, UINT(config.vr.msaaSamples)) : 1;
	} else if(!sceneEnabled()) {
		return;
	}
	
	if((size != m_sceneSize || bool(m_motionView) != upscalerInputsNeeded() || bool(m_aoView) != m_ambientOcclusion
	    || bool(m_bloomViews[0]) != m_bloom || bool(m_surfaceView) != surfaceTargetNeeded()
	    || bool(m_reflectionColor) != (m_reflectionStrength > 0.f) || bool(m_shadingView) != shadowsEnabled()
	    || samples != m_sceneSamples)
	   && !createSceneTargets(size, samples)) {
		return;
	}

	// The scene textures are about to become render targets
	unbindTextureView(m_sceneResolvedView.Get());
	unbindTextureView(m_sceneDepthResource.Get());
	unbindTextureView(m_sceneStencilResource.Get());
	unbindTextureView(m_motionResource.Get());
	unbindTextureView(m_linearDepthResource.Get());
	unbindTextureView(m_objectMotionResource.Get());
	unbindTextureView(m_surfaceResource.Get());
	unbindTextureView(m_reflectionColorResource.Get());
	unbindTextureView(m_shadingResource.Get());

	// No pixels without effects yet (see setSceneEffectsExcluded)
	if(m_sceneDepthView) {
		m_context->ClearDepthStencilView(m_sceneDepthView.Get(), D3D11_CLEAR_STENCIL, 1.f, 0);
	}

	const float none[4] = { 0.f, 0.f, 0.f, 0.f };
	if(m_objectMotionView) {
		m_context->ClearRenderTargetView(m_objectMotionView.Get(), none);
	}
	if(m_surfaceView) {
		m_context->ClearRenderTargetView(m_surfaceView.Get(), none);
	}
	if(m_shadingView) {
		m_context->ClearRenderTargetView(m_shadingView.Get(), none);
	}
	
	updateSceneCamera();
	
	m_inScene = true;
	m_sceneRenderedThisFrame = true;
	invalidateCachedState();
	
}

void D3D11Renderer::endScene() {
	
	m_drawingScene = false;
	
	// The stereo scene stays bound for the overlays drawn in the world, see finishStereoScene()
	if(!m_inScene || !m_stereoViews.empty()) {
		return;
	}
	
	m_inScene = false;
	
	postProcessScene();
	
	ID3D11ShaderResourceView * upscaled = runUpscaler();
	
	// Upscale the scene to the window
	composeScene(outputColorView(), m_size, upscaled, true);
	
	// The interface is drawn on top of the scene, with its own depth
	if(ID3D11DepthStencilView * depthView = outputDepthView()) {
		m_context->ClearDepthStencilView(depthView, D3D11_CLEAR_DEPTH, 1.f, 0);
	}
	
	// Remember the camera for the motion vectors of the next frame
	m_previousProjection = m_sceneProjection;
	m_previousView = m_sceneView;
	m_previousStereoViews.clear();
	m_hasPreviousScene = true;
	m_lastSceneStereo = false;
	m_resetHistory = false;
	
}

ID3D11Texture2D * D3D11Renderer::finishStereoScene() {
	
	m_drawingScene = false;
	
	if(!m_inScene || m_stereoViews.empty()) {
		return nullptr;
	}
	
	m_inScene = false;
	
	postProcessScene();
	
	if(!m_stereoOutput) {
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = UINT(m_stereoSize.x);
		desc.Height = UINT(m_stereoSize.y);
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = ColorFormat;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_stereoOutput))
		   || FAILED(m_device->CreateRenderTargetView(m_stereoOutput.Get(), nullptr, &m_stereoOutputView))) {
			LogError << "Failed to create " << m_stereoSize.x << "x" << m_stereoSize.y << " Direct3D 11 stereo output";
			m_stereoOutput.Reset();
			m_stereoOutputView.Reset();
			invalidateCachedState();
			return nullptr;
		}
	}
	
	ID3D11ShaderResourceView * upscaled = runStereoUpscaler();
	
	// The lenses darken the edges of the view already
	composeScene(m_stereoOutputView.Get(), m_stereoSize, upscaled, false);
	
	// Remember the cameras for the motion vectors of the next frame
	m_previousProjection = m_sceneProjection;
	m_previousView = m_sceneView;
	m_previousStereoViews = m_stereoViews;
	m_hasPreviousScene = true;
	m_lastSceneStereo = true;
	m_resetHistory = false;
	
	return m_stereoOutput.Get();
}

bool D3D11Renderer::stereoUpscaling() const {
	return m_upscaler && m_upscaler->supportsViews();
}

ID3D11ShaderResourceView * D3D11Renderer::runStereoUpscaler() {
	
	if(!stereoUpscaling() || !m_motionTarget || !m_linearDepthTarget || m_stereoViews.empty()) {
		return nullptr;
	}
	
	if(!m_stereoUpscaled) {
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = UINT(m_stereoSize.x);
		desc.Height = UINT(m_stereoSize.y);
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = ColorFormat;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_stereoUpscaled))
		   || FAILED(m_device->CreateShaderResourceView(m_stereoUpscaled.Get(), nullptr, &m_stereoUpscaledView))) {
			LogError << "Failed to create " << m_stereoSize.x << "x" << m_stereoSize.y
			         << " Direct3D 11 stereo upscaler output";
			m_stereoUpscaled.Reset();
			m_stereoUpscaledView.Reset();
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
	float frameTime = 1000.f / 90.f;
	if(m_lastUpscaleTime != 0) {
		frameTime = float(double(now.QuadPart - m_lastUpscaleTime) * 1000.0 / double(frequency.QuadPart));
	}
	m_lastUpscaleTime = now.QuadPart;
	
	// Each eye with its own history, from its part of the scene into its part of the output
	Vec2i outputSize(m_stereoSize.x / s32(m_stereoViews.size()), m_stereoSize.y);
	bool success = true;
	for(size_t eye = 0; eye < m_stereoViews.size() && success; eye++) {
		
		D3D11Upscaler::Inputs inputs;
		inputs.color = m_sceneResolved.Get();
		inputs.depth = m_linearDepthTarget.Get();
		inputs.motion = m_motionTarget.Get();
		inputs.output = m_stereoUpscaled.Get();
		inputs.renderSize = m_stereoEyeSize;
		inputs.outputSize = outputSize;
		inputs.view = eye;
		inputs.renderOffset = Vec2i(s32(eye) * m_stereoEyeSize.x, 0);
		inputs.outputOffset = Vec2i(s32(eye) * outputSize.x, 0);
		inputs.jitter = m_jitter;
		inputs.reset = m_resetHistory;
		inputs.frameTime = frameTime;
		inputs.sharpness = m_upscalerSharpness;
		
		// See runUpscaler()
		const glm::mat4x4 & projection = m_stereoViews[eye].projection;
		float q = projection[2][2];
		float nearPlane = (q != 0.f) ? -projection[3][2] / q : 1.f;
		inputs.cameraNear = nearPlane;
		inputs.cameraFar = (q > 1.f) ? nearPlane * q / (q - 1.f) : nearPlane * 10000.f;
		inputs.cameraFovVertical = 2.f * std::atan(1.f / std::max(std::abs(projection[1][1]), 0.0001f));
		
		success = m_upscaler->evaluate(m_context.Get(), inputs);
	}
	
	// The upscaler changes the device state
	invalidateCachedState();
	
	return success ? m_stereoUpscaledView.Get() : nullptr;
}

void D3D11Renderer::postProcessScene() {
	
	if(m_sceneSamples > 1) {
		m_context->ResolveSubresource(m_sceneResolved.Get(), 0, m_sceneColorTarget.Get(), 0, ColorFormat);
	}
	
	renderSceneMotion();

	if(m_surfaceTarget && m_surfaceResolved.Get() != m_surfaceTarget.Get()) {
		m_context->ResolveSubresource(m_surfaceResolved.Get(), 0, m_surfaceTarget.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
	}

	if(shadowsEnabled()) {
		// The debug view shows the shadows instead of the scene
		renderShadows(m_sceneDebugView != ShadowDebugView);
	}

	if(m_ambientOcclusion) {
		// The debug view shows the ambient occlusion without applying it
		renderAmbientOcclusion(m_sceneDebugView != 4);
	}

	if(m_surfaceView && m_reflectionStrength > 0.f) {
		renderReflections();
	}
	
	if(m_bloom) {
		renderBloom(m_sceneDebugView != 5);
	}
	
}

void D3D11Renderer::composeScene(ID3D11RenderTargetView * target, const Vec2i & size,
                                 ID3D11ShaderResourceView * upscaled, bool vignette) {
	
	m_context->OMSetRenderTargets(1, &target, nullptr);
	
	D3D11_VIEWPORT viewport = { 0.f, 0.f, float(size.x), float(size.y), 0.f, 1.f };
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
	} else if(m_sceneDebugView == 5 && m_bloomResources[0]) {
		view = m_bloomResources[0].Get();
	} else if(m_sceneDebugView == 4 && m_debugAOPixelShader && m_aoResource) {
		view = m_aoResource.Get();
		shader = m_debugAOPixelShader.Get();
	} else if(m_colorGrading != 0 && m_gradingPixelShader) {
		const ColorGradingPreset & preset = ColorGradingPresets[m_colorGrading];
		GradingConstants constants = { };
		constants.params[0] = preset.contrast;
		constants.params[1] = preset.saturation;
		constants.params[2] = vignette ? preset.vignette : 0.f;
		constants.params[3] = float(size.x) / float(std::max<s32>(size.y, 1));
		constants.tone[0] = preset.exposure;
		constants.tone[1] = preset.gamma;
		constants.tone[2] = ColorGradingPivot;
		for(size_t i = 0; i < 3; i++) {
			constants.tint[i] = preset.tint[i];
			constants.shadows[i] = preset.shadows[i];
			constants.highlights[i] = preset.highlights[i];
		}
		m_context->UpdateSubresource(m_gradingConstants.Get(), 0, nullptr, &constants, 0, 0);
		ID3D11Buffer * buffer = m_gradingConstants.Get();
		m_context->PSSetConstantBuffers(5, 1, &buffer);
		shader = m_gradingPixelShader.Get();
	}
	
	m_context->PSSetShaderResources(0, 1, &view);
	ID3D11SamplerState * sampler = m_blitSampler.Get();
	m_context->PSSetSamplers(0, 1, &sampler);
	
	drawFullscreen(shader);
	
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
	
	#if ARX_HAVE_FSR
	if(name == "fsr") {
		return true;
	}
	#endif
	
	// Copies the scene through the Direct3D 12 bridge, only selectable in the config file
	if(name == "bridge-test") {
		return true;
	}
	
	return false;
}

std::string D3D11Renderer::getUpscalerError(std::string_view name) const {
	
	if(!supportsUpscaler(name)) {
		return "unsupported";
	}
	
	#if ARX_HAVE_DLSS
	if(name == "dlss") {
		if(!d3d11::findDLSSLibrary(config.video.upscalerPath)) {
			return "dll_missing";
		}
		if(m_dlssFailed) {
			return "unsupported";
		}
	}
	#endif
	
	#if ARX_HAVE_FSR
	if(name == "fsr") {
		if(!d3d11::findFSRLibrary(config.video.upscalerPath)) {
			return "dll_missing";
		}
		if(m_fsrFailed) {
			return "unsupported";
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

void D3D11Renderer::setPixelLighting(bool enabled) {
	
	m_pixelLighting = enabled;
	
	if(!enabled) {
		setPixelLights(nullptr, 0);
	}
	
}

void D3D11Renderer::setPixelLights(const PixelLight * lights, size_t count) {
	
	// Debug views 6 and 8 show only the dynamic light or the highlights of models
	bool debugView = (m_sceneDebugView == 6 || m_sceneDebugView == 8);
	
	if(!m_lightConstants || !m_context || (count == 0 && m_pixelLightCount == 0 && !debugView)) {
		return;
	}
	
	// Lights that only add highlights come last and are dropped first
	size_t diffuseCount = 0;
	while(diffuseCount < count && !lights[diffuseCount].specularOnly) {
		diffuseCount++;
	}
	if(diffuseCount > MaxPixelLights && !m_pixelLightsWarned) {
		LogWarning << "Too many lights for per-pixel lighting: " << diffuseCount << " > " << MaxPixelLights;
		m_pixelLightsWarned = true;
	}
	count = std::min(count, MaxPixelLights);
	diffuseCount = std::min(diffuseCount, count);
	
	LightConstants constants = {};
	constants.count[0] = s32(diffuseCount);
	constants.count[1] = (m_sceneDebugView == 6) ? 1 : (m_sceneDebugView == 8) ? 2 : 0;
	constants.count[2] = s32(count);
	for(size_t i = 0; i < count; i++) {
		const PixelLight & light = lights[i];
		float * a = constants.lights[i * 2];
		float * b = constants.lights[i * 2 + 1];
		a[0] = light.position.x;
		a[1] = light.position.y;
		a[2] = light.position.z;
		a[3] = light.fallStart;
		b[0] = light.color.r * light.intensity;
		b[1] = light.color.g * light.intensity;
		b[2] = light.color.b * light.intensity;
		b[3] = light.fallEnd;
	}
	
	m_context->UpdateSubresource(m_lightConstants.Get(), 0, nullptr, &constants, 0, 0);
	if(count > 0 && m_pixelLightCount == 0) {
		LogDebug("Per-pixel lighting with " << count << " dynamic lights");
	}
	m_pixelLightCount = count;
	
}

void D3D11Renderer::updateUpscaler() {
	
	if(m_upscalerCreated) {
		return;
	}
	m_upscalerCreated = true;
	
	if(m_upscalerName.empty() || m_upscalerName == "off") {
		return;
	}
	
	if(m_upscalerName == "bridge-test") {
		m_upscaler = d3d11::createBridgeTestUpscaler(m_device.Get(), m_context.Get());
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
	
	#if ARX_HAVE_FSR
	if(m_upscalerName == "fsr") {
		// The AMD FidelityFX libraries are not distributed with Arx Libertatis, the upscaler logs why it failed
		m_upscaler = d3d11::createFSRUpscaler(m_device.Get(), m_context.Get(), config.video.upscalerPath);
		m_fsrFailed = !m_upscaler && d3d11::findFSRLibrary(config.video.upscalerPath);
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
	inputs.sharpness = m_upscalerSharpness;
	
	// Camera parameters from the projection matrix (see createProjectionMatrix() in Camera.cpp):
	// [2][2] = far / (far - near), [3][2] = -near * far / (far - near), [1][1] = -cot(fov / 2) for the height
	{
		float q = m_sceneProjection[2][2];
		float nearPlane = (q != 0.f) ? -m_sceneProjection[3][2] / q : 1.f;
		inputs.cameraNear = nearPlane;
		inputs.cameraFar = (q > 1.f) ? nearPlane * q / (q - 1.f) : nearPlane * 10000.f;
		inputs.cameraFovVertical = 2.f * std::atan(1.f / std::max(std::abs(m_sceneProjection[1][1]), 0.0001f));
	}
	
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
	if(m_currentNormalView == view) {
		ID3D11ShaderResourceView * none = nullptr;
		m_context->PSSetShaderResources(NormalMapSlot, 1, &none);
		m_currentNormalView = nullptr;
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
	
	ID3D11RenderTargetView * colorView = m_inScene ? m_sceneColorView.Get() : outputColorView();
	ID3D11DepthStencilView * depthView = m_inScene ? m_sceneDepthView.Get() : outputDepthView();
	
	if(nrects != 0 && stereoActive()) {
		/*
		 * Screen rectangles do not map to the stereo views, and clearing the whole target instead would erase the
		 * scene (cinematic borders clear the depth buffer to 0). These are only used for letterboxing.
		 */
		return;
	}
	
	if(nrects == 0) {
		if(bufferFlags & ColorBuffer) {
			const float c[4] = { color.r, color.g, color.b, color.a };
			m_context->ClearRenderTargetView(colorView, c);
		}
		if((bufferFlags & DepthBuffer) && depthView) {
			m_context->ClearDepthStencilView(depthView, D3D11_CLEAR_DEPTH, clearDepth, 0);
		}
		return;
	}
	
	// Partial clears are done by drawing a quad for each rectangle
	
	ClearConstants constants = { { color.r, color.g, color.b, color.a }, { clearDepth, 0.f, 0.f, 0.f } };
	m_context->UpdateSubresource(m_clearConstants.Get(), 0, nullptr, &constants, 0, 0);
	
	m_context->OMSetRenderTargets(1, &colorView, depthView);
	bindTessellation(false);
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
	
	if(enable && !config.video.antialiasing) {
		return;
	}
	
	// The window is not multisampled in VR, but the stereo scene is
	if(m_msaaSamples <= 1 && m_stereoSamples <= 1) {
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

glm::mat4x4 D3D11Renderer::screenTransform() const {
	
	// Direct3D doesn't apply any transform for D3DTLVERTEX
	// Change coordinate system from [0, width] x [0, height] to [-1, 1] x [-1, 1] and flip the y axis
	glm::mat4x4 transform = glm::translate(glm::mat4x4(1.f), Vec3f(-1.f, 1.f, 0.f));
	transform = glm::scale(transform, Vec3f(2.f / float(std::max<s32>(m_viewport.width(), 1)),
	                                        -2.f / float(std::max<s32>(m_viewport.height(), 1)), 1.f));
	// Change pixel origins
	return glm::translate(transform, Vec3f(0.5f, 0.5f, 0.f));
}

void D3D11Renderer::fillVSConstants(float * data, VertexType type, const glm::mat4x4 & transform,
                                    const glm::mat4x4 & view, const glm::mat4x4 & projection,
                                    const glm::mat4x4 & prevTransform, const Vec2i & viewSize) const {
	
	VSConstants constants;
	std::memcpy(constants.transform, &transform, sizeof(constants.transform));
	std::memcpy(constants.prevTransform, &prevTransform, sizeof(constants.prevTransform));
	// The jitter is in pixels of the view (one eye in VR)
	constants.jitter[0] = m_inScene ? 2.f * m_jitter.x / float(std::max<s32>(viewSize.x, 1)) : 0.f;
	constants.jitter[1] = m_inScene ? -2.f * m_jitter.y / float(std::max<s32>(viewSize.y, 1)) : 0.f;
	constants.jitter[2] = constants.jitter[3] = 0.f;
	// The view matrix is a rotation and translation
	Vec3f camera = -(glm::transpose(glm::mat3(view)) * Vec3f(view[3]));
	constants.camera[0] = camera.x;
	constants.camera[1] = camera.y;
	constants.camera[2] = camera.z;
	constants.camera[3] = 0.f;
	// Pre-transformed vertices of models in the 3D scene go back to world space with the scene camera (the center
	// camera in VR, which projected them)
	glm::mat4x4 modelToWorld(1.f);
	if(type == VertexTL && m_drawingScene) {
		modelToWorld = glm::mat4x4(glm::inverse(glm::dmat4(m_projection) * glm::dmat4(m_view))
		                           * glm::dmat4(screenTransform()));
	}
	std::memcpy(constants.modelToWorld, &modelToWorld, sizeof(constants.modelToWorld));
	glm::mat4x4 worldToClip = projection * view;
	std::memcpy(constants.worldToClip, &worldToClip, sizeof(constants.worldToClip));
	static_assert(sizeof(constants) == sizeof(m_vsConstantsData), "unexpected constant buffer size");
	std::memcpy(data, &constants, sizeof(constants));
	
}

template <class DrawFunction>
void D3D11Renderer::drawViews(VertexType type, const DrawFunction & draw) {
	
	if(!stereoActive()) {
		draw();
		return;
	}
	
	for(size_t eye = 0; eye < m_stereoViews.size(); eye++) {
		applyStereoView(type, eye);
		draw();
	}
	
}

void D3D11Renderer::applyStereoView(VertexType type, size_t eye) {
	
	const StereoView & view = m_stereoViews[eye];
	
	D3D11_VIEWPORT viewport = {
		float(s32(eye) * m_stereoEyeSize.x), 0.f, float(m_stereoEyeSize.x), float(m_stereoEyeSize.y), 0.f, 1.f
	};
	m_context->RSSetViewports(1, &viewport);
	
	// Pre-transformed vertices are re-projected from the center camera into the eye
	glm::mat4x4 transform = (type == VertexTL) ? view.screenToClip : view.projection * view.view;
	// Positions of the last frame were projected by the center camera of the last frame
	glm::mat4x4 prevTransform = transform;
	if(type == VertexTL && eye < m_previousStereoViews.size()) {
		prevTransform = m_previousStereoViews[eye].screenToClip;
	}
	float constants[VSConstantsSize];
	fillVSConstants(constants, type, transform, view.view, view.projection, prevTransform, m_stereoEyeSize);
	if(!m_stereoVSConstantsValid[eye] || std::memcmp(m_stereoVSConstantsData[eye], constants, sizeof(constants)) != 0) {
		std::memcpy(m_stereoVSConstantsData[eye], constants, sizeof(constants));
		m_context->UpdateSubresource(m_stereoVSConstants[eye].Get(), 0, nullptr, constants, 0, 0);
		m_stereoVSConstantsValid[eye] = true;
	}
	
	ID3D11Buffer * buffer = m_stereoVSConstants[eye].Get();
	m_context->VSSetConstantBuffers(0, 1, &buffer);
	m_context->HSSetConstantBuffers(0, 1, &buffer);
	m_context->DSSetConstantBuffers(0, 1, &buffer);
	
}

std::vector<D3D11Renderer::SceneRegion> D3D11Renderer::sceneRegions() const {
	
	std::vector<SceneRegion> regions;
	
	if(m_stereoViews.empty()) {
		regions.push_back({ Rect(m_sceneSize.x, m_sceneSize.y), m_sceneProjection, m_sceneView, m_previousProjection,
		                    m_previousView });
		return regions;
	}
	
	for(size_t eye = 0; eye < m_stereoViews.size(); eye++) {
		s32 left = s32(eye) * m_stereoEyeSize.x;
		Rect pixels(left, 0, left + m_stereoEyeSize.x, m_stereoEyeSize.y);
		const StereoView & view = m_stereoViews[eye];
		const StereoView & previous = (eye < m_previousStereoViews.size()) ? m_previousStereoViews[eye] : view;
		regions.push_back({ pixels, view.projection, view.view, previous.projection, previous.view });
	}
	
	return regions;
}

void D3D11Renderer::setStereoViews(std::vector<StereoView> views, const Vec2i & size) {
	
	if(views.size() > std::size(m_stereoVSConstants)) {
		views.resize(std::size(m_stereoVSConstants));
	}
	
	if(views.empty() && stereoActive()) {
		// The stereo scene was not finished, it is not shown
		m_inScene = false;
		m_drawingScene = false;
	}
	
	m_stereoViews = std::move(views);
	
	if(!m_stereoViews.empty() && size != m_stereoSize) {
		m_stereoOutputView.Reset();
		m_stereoOutput.Reset();
		unbindTextureView(m_stereoUpscaledView.Get());
		m_stereoUpscaledView.Reset();
		m_stereoUpscaled.Reset();
		m_stereoSize = size;
	}
	
	invalidateCachedState();
	
}

void D3D11Renderer::setOutputTarget(ID3D11RenderTargetView * color, ID3D11DepthStencilView * depth) {
	
	m_outputColor = color;
	m_outputDepth = color ? depth : nullptr;
	
	invalidateCachedState();
	
}

void D3D11Renderer::setPremultipliedAlphaOutput(bool enable) {
	m_premultipliedAlpha = enable;
}

void D3D11Renderer::drawOverlay(ID3D11RenderTargetView * target, const Vec2i & size,
                                ID3D11ShaderResourceView * texture, const OverlayVertex * vertices, size_t count,
                                bool blend) {
	
	if(!target || !texture || !vertices || count < 3 || !m_overlayVertexShader) {
		return;
	}
	
	size_t offset = m_vertexRing.upload(m_context.Get(), vertices, count * sizeof(OverlayVertex),
	                                    sizeof(OverlayVertex));
	if(offset == size_t(-1)) {
		return;
	}
	
	bindTessellation(false);
	
	ID3D11ShaderResourceView * none = nullptr;
	m_context->PSSetShaderResources(0, 1, &none);
	m_context->OMSetRenderTargets(1, &target, nullptr);
	D3D11_VIEWPORT viewport = { 0.f, 0.f, float(size.x), float(size.y), 0.f, 1.f };
	m_context->RSSetViewports(1, &viewport);
	m_context->RSSetState(getRasterizerState(false, 0, false, false));
	m_context->OMSetDepthStencilState(getDepthState(false, false), 0);
	BlendingFactor dst = blend ? BlendInvSrcAlpha : BlendZero;
	m_context->OMSetBlendState(getBlendState(BlendOne, dst, false, true), nullptr, 0xffffffff);
	
	ID3D11Buffer * buffer = m_vertexRing.buffer.Get();
	UINT stride = UINT(sizeof(OverlayVertex));
	UINT byteOffset = UINT(offset);
	m_context->IASetVertexBuffers(0, 1, &buffer, &stride, &byteOffset);
	m_context->IASetInputLayout(m_overlayLayout.Get());
	m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	m_context->VSSetShader(m_overlayVertexShader.Get(), nullptr, 0);
	m_context->PSSetShader(m_blitPixelShader.Get(), nullptr, 0);
	m_context->PSSetShaderResources(0, 1, &texture);
	ID3D11SamplerState * sampler = m_blitSampler.Get();
	m_context->PSSetSamplers(0, 1, &sampler);
	
	m_context->Draw(UINT(count), 0);
	
	m_context->PSSetShaderResources(0, 1, &none);
	m_context->OMSetRenderTargets(0, nullptr, nullptr);
	
	// Restore the renderer state for the next draw
	m_currentViews[0] = nullptr;
	m_currentSamplers[0] = sampler;
	m_currentVertexBuffer = nullptr;
	m_currentTopology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
	m_currentVertexType = VertexTypeCount;
	invalidateCachedState();
	
}

bool D3D11Renderer::supportsModelTessellation() const {
	return m_drawingScene && m_modelHullShader && m_modelDomainShader && m_modelPatchVertexShader;
}

void D3D11Renderer::bindTessellation(bool enable) {
	
	if(enable == m_tessellationBound) {
		return;
	}
	
	m_context->HSSetShader(enable ? m_modelHullShader.Get() : nullptr, nullptr, 0);
	m_context->DSSetShader(enable ? m_modelDomainShader.Get() : nullptr, nullptr, 0);
	if(enable) {
		m_context->VSSetShader(m_modelPatchVertexShader.Get(), nullptr, 0);
	} else if(m_currentVertexType < VertexTypeCount) {
		m_context->VSSetShader(m_vertexShaders[m_currentVertexType].Get(), nullptr, 0);
	}
	m_tessellationBound = enable;
	
}

void D3D11Renderer::setPrimitive(Primitive primitive, bool allowTessellation) {
	
	if(m_modelTessellation) {
		m_fpsLogModelDraws++;
	}
	bool tessellate = allowTessellation && primitive == TriangleList && m_modelTessellation
	                  && m_currentVertexType == VertexTL && supportsModelTessellation();
	bindTessellation(tessellate);
	if(tessellate) {
		m_fpsLogTessellatedDraws++;
	}
	
	D3D11_PRIMITIVE_TOPOLOGY topology = tessellate ? D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
	                                               : arxToD3DPrimitive[primitive];
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
	
	setPrimitive(TriangleList, false);
	drawViews(m_currentVertexType, [&]() {
		m_context->DrawIndexed(UINT(nindices), start, INT(baseVertex));
	});
	
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
	drawViews(type, [&]() {
		m_context->Draw(UINT(count), UINT(offset));
	});
	
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
	drawViews(type, [&]() {
		m_context->DrawIndexed(UINT(count), start, INT(baseVertex));
	});
	
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
	drawViews(VertexTL, [&]() {
		m_context->DrawIndexed(UINT(count), start, 0);
	});
	
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
	
	if(!m_fpsLogPath.empty()) {
		LARGE_INTEGER now, frequency;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&frequency);
		if(m_fpsLogFrames == 0) {
			m_fpsLogStart = now.QuadPart;
		}
		m_fpsLogFrames++;
		double seconds = double(now.QuadPart - m_fpsLogStart) / double(frequency.QuadPart);
		if(seconds >= 5.0) {
			double frames = double(m_fpsLogFrames - 1);
			fs::ofstream log(fs::path(m_fpsLogPath), std::ios_base::app);
			log << int(frames / seconds + 0.5) << " fps, " << float(seconds * 1000.0 / frames) << " ms, "
			    << float(double(m_fpsLogTessellatedDraws) / frames) << " of " << float(double(m_fpsLogModelDraws) / frames)
			    << " model draws tessellated" << std::endl;
			m_fpsLogTessellatedDraws = 0;
			m_fpsLogModelDraws = 0;
			m_fpsLogStart = now.QuadPart;
			m_fpsLogFrames = 1;
		}
	}
	
	if(!m_snapshotPath.empty()) {
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		if(now.QuadPart >= m_snapshotTime) {
			Image image;
			if(getSnapshot(image) && image.save(fs::path(m_snapshotPath))) {
				LogInfo << "Saved debug snapshot to " << m_snapshotPath;
			} else {
				LogError << "Failed to save debug snapshot to " << m_snapshotPath;
			}
			m_snapshotPath.clear();
		}
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
	
	// Overlays that hook Present (e.g. OptiScaler) may draw with our context and leave their state bound
	invalidateCachedState();
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
                                                bool colorWrite, bool premultipliedAlpha) {
	
	u32 key = u32(src) | (u32(dst) << 4) | (u32(alphaToCoverage) << 8) | (u32(colorWrite) << 9)
	          | (u32(premultipliedAlpha) << 10);
	
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
	// The third target (surface normal and reflectivity) is replaced where its alpha is 1, which the shader sets
	// for opaque draws and for reflective surfaces like water
	D3D11_RENDER_TARGET_BLEND_DESC & surface = desc.RenderTarget[2];
	surface.BlendEnable = TRUE;
	surface.SrcBlend = D3D11_BLEND_SRC_ALPHA;
	surface.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	surface.SrcBlendAlpha = D3D11_BLEND_ONE;
	surface.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	surface.BlendOp = surface.BlendOpAlpha = D3D11_BLEND_OP_ADD;
	surface.RenderTargetWriteMask = colorWrite ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
	// The fourth target (light at the surface for the shadows) is replaced by opaque draws in the same way
	desc.RenderTarget[3] = surface;
	D3D11_RENDER_TARGET_BLEND_DESC & target = desc.RenderTarget[0];
	target.BlendEnable = (src != BlendOne || dst != BlendZero) ? TRUE : FALSE;
	target.SrcBlend = arxToD3DBlendFactor[src];
	target.DestBlend = arxToD3DBlendFactor[dst];
	target.BlendOp = D3D11_BLEND_OP_ADD;
	target.SrcBlendAlpha = arxToD3DAlphaBlendFactor[src];
	target.DestBlendAlpha = arxToD3DAlphaBlendFactor[dst];
	target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
	target.RenderTargetWriteMask = colorWrite ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
	if(premultipliedAlpha) {
		/*
		 * Produce a premultiplied alpha image that can be composited over another one, like the OpenGL backend:
		 * the color channels are blended as usual while the alpha channel accumulates coverage.
		 * Additive and multiplicative blending do not add coverage.
		 */
		bool coverage = (dst != BlendOne && src != BlendZero);
		target.BlendEnable = TRUE;
		target.SrcBlendAlpha = coverage ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO;
		target.DestBlendAlpha = coverage ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_ONE;
	}
	
	ComPtr<ID3D11BlendState> state;
	if(FAILED(m_device->CreateBlendState(&desc, &state))) {
		LogError << "Failed to create Direct3D 11 blend state";
		return nullptr;
	}
	
	ID3D11BlendState * result = state.Get();
	m_blendStates.emplace(key, std::move(state));
	return result;
}

ID3D11DepthStencilState * D3D11Renderer::getDepthState(bool test, bool write, bool stencil) {

	u32 key = u32(test) | (u32(write) << 1) | (u32(stencil) << 2);
	
	auto it = m_depthStates.find(key);
	if(it != m_depthStates.end()) {
		return it->second.Get();
	}
	
	// Like the OpenGL backend, the depth test is always enabled so that depth writes work
	D3D11_DEPTH_STENCIL_DESC desc = {};
	desc.DepthEnable = TRUE;
	desc.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
	desc.DepthFunc = test ? D3D11_COMPARISON_LESS_EQUAL : D3D11_COMPARISON_ALWAYS;
	desc.StencilEnable = stencil ? TRUE : FALSE;
	if(stencil) {
		// The drawn pixels get the reference value (1)
		desc.StencilReadMask = desc.StencilWriteMask = 0xff;
		desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
		desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
		desc.FrontFace.StencilFailOp = desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		desc.BackFace = desc.FrontFace;
	}

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
		m_context->HSSetConstantBuffers(0, 1, &vsConstants);
		m_context->DSSetConstantBuffers(0, 1, &vsConstants);
		ID3D11Buffer * psConstants = m_psConstants.Get();
		m_context->PSSetConstantBuffers(0, 1, &psConstants);
		ID3D11Buffer * lightConstants = m_lightConstants.Get();
		m_context->PSSetConstantBuffers(6, 1, &lightConstants);
		
		m_currentVertexBuffer = nullptr;
		m_currentTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		
	}
	
	// Vertex shader
	
	if(force || type != m_currentVertexType) {
		m_context->IASetInputLayout(m_inputLayouts[type].Get());
		m_context->VSSetShader(m_vertexShaders[type].Get(), nullptr, 0);
		bindTessellation(false);
		m_currentVertexType = type;
	}
	
	// The eyes of the stereo scene have their own constants, see applyStereoView()
	if(!stereoActive()) {
		glm::mat4x4 transform = (type == VertexTL) ? screenTransform() : m_projection * m_view;
		float constants[VSConstantsSize];
		fillVSConstants(constants, type, transform, m_view, m_projection, transform, m_sceneSize);
		if(!m_vsConstantsValid || std::memcmp(m_vsConstantsData, constants, sizeof(constants)) != 0) {
			std::memcpy(m_vsConstantsData, constants, sizeof(constants));
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
	
	ID3D11BlendState * blend = getBlendState(blendSrc, blendDst, alphaTest == TestA2C, true,
	                                         m_premultipliedAlpha && !m_inScene);
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
	
	// Debug view 9 shows models as wireframes
	bool wireframe = m_wireframe || (m_sceneDebugView == 9 && m_modelTessellation && type == VertexTL);
	// Screen-space scissor rectangles do not map to the stereo views
	ID3D11RasterizerState * rasterizer = getRasterizerState(m_state.getCull(), m_state.getDepthOffset(),
	                                                        wireframe, m_scissor.isValid() && !stereoActive());
	if(force || rasterizer != m_currentRasterizer) {
		m_context->RSSetState(rasterizer);
		m_currentRasterizer = rasterizer;
	}
	
	ID3D11DepthStencilState * depth = getDepthState(m_state.getDepthTest(), m_state.getDepthWrite(),
	                                                m_sceneEffectsExcluded && m_inScene);
	if(force || depth != m_currentDepth) {
		m_context->OMSetDepthStencilState(depth, 1);
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
		
		// Relief map of the main texture for normal mapping and parallax mapping of world geometry, and for
		// normal mapping of models
		if(i == 0) {
			ID3D11ShaderResourceView * normalView = nullptr;
			bool model = (type == VertexTL && m_modelTessellation && m_drawingScene);
			if(texture && (type == VertexWorld || model) && reliefMapsEnabled()) {
				normalView = texture->normalView();
			}
			if(force || normalView != m_currentNormalView) {
				m_context->PSSetShaderResources(NormalMapSlot, 1, &normalView);
				m_currentNormalView = normalView;
			}
			constants.stages[0][3] = normalView ? 1 : 0;
			if(normalView) {
				float strength = model ? m_normalMapStrength * ModelReliefScale : m_normalMapStrength;
				constants.relief[0] = strength * NormalMapSlopeScale * (model ? texture->specular().relief : 1.f);
				constants.relief[1] = model ? 0.f : m_parallaxStrength * ParallaxDepthScale;
				constants.relief[2] = model ? std::min(strength, 1.f) : 0.f;
			}
			
			// Highlights from the material of the main texture
			if(texture && (type == VertexWorld || (type == VertexTL && m_drawingScene)) && m_specularStrength > 0.f) {
				const D3D11Texture::Specular & specular = texture->specular();
				constants.specular[0] = specular.strength * m_specularStrength * SpecularScale;
				constants.specular[1] = specular.exponent;
				constants.specular[2] = specular.metallic;
				constants.specular[3] = m_specularStrength * SpecularScale;
				// Metal and leather parts of armor in the 3D scene
				if(type == VertexTL) {
					constants.material[0] = specular.metalParts;
					constants.material[1] = specular.leather;
				}
			}
			if(texture && type == VertexWorld && m_surfaceView && m_reflectionStrength > 0.f) {
				constants.surface[0] = texture->specular().reflectivity;
			}
			// Lava glows and flows in the 3D scene
			if(texture && (type == VertexWorld || type == VertexTL) && m_drawingScene && texture->specular().glow > 0.f) {
				constants.effect[0] = m_effectTime;
				constants.effect[1] = texture->specular().glow;
			}
			// Ice looks deep, cold and sparkles in the 3D scene, crystals and gems (also items) look clear and deep
			if(texture && type == VertexWorld && m_drawingScene) {
				constants.effect[2] = texture->specular().ice;
			}
			if(texture && (type == VertexWorld || type == VertexTL) && m_drawingScene) {
				constants.effect[3] = texture->specular().crystal;
			}
			// Water is drawn without normals, with an animated texture
			if(type == VertexWorld3 && m_waterSurface) {
				const D3D11Texture::Specular & water = WaterSpecular;
				constants.specular[0] = water.strength * m_specularStrength * SpecularScale;
				constants.specular[1] = water.exponent;
				constants.specular[2] = water.metallic;
				constants.surface[0] = (m_surfaceView && m_reflectionStrength > 0.f) ? water.reflectivity : 0.f;
				constants.surface[2] = 1.f;
			}
		}
		
	}
	
	constants.fogColor[0] = m_fogColor.r;
	constants.fogColor[1] = m_fogColor.g;
	constants.fogColor[2] = m_fogColor.b;
	constants.fogColor[3] = m_fogColor.a;
	constants.fogParams[0] = m_fogStart;
	constants.fogParams[1] = m_fogEnd;
	constants.fogParams[2] = m_state.getFog() ? 1.f : 0.f;
	constants.fogParams[3] = alphaReference;
	constants.surface[1] = (m_state.getBlendSrc() != BlendOne || m_state.getBlendDst() != BlendZero) ? 1.f : 0.f;
	// Models are already lit in their vertex colors and only get highlights
	constants.surface[3] = (type == VertexTL) ? 1.f : 0.f;
	
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

bool isDirect3D11(const Renderer * renderer) {
	return renderer && renderer == g_d3d11Renderer;
}

void setRequiredAdapter(unsigned long long luid) {
	g_hasRequiredAdapter = true;
	g_requiredAdapter.LowPart = DWORD(luid & 0xffffffffu);
	g_requiredAdapter.HighPart = LONG(luid >> 32);
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
