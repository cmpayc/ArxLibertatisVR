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
#include <vector>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include "io/log/Logger.h"
#include "platform/WindowsUtils.h"

namespace {

// Identifies the application to NGX, see NVSDK_NGX_D3D11_Init_with_ProjectID()
const char * const NGXProjectId = "6d1c2a4e-8f3b-4f7a-9b5e-3c2d1a0f4e7b";

std::string resultString(NVSDK_NGX_Result result) {
	std::ostringstream oss;
	oss << "0x" << std::hex << u32(result);
	return oss.str();
}

NVSDK_NGX_PerfQuality_Value qualityForScale(float scale) {
	if(scale >= 0.99f) {
		return NVSDK_NGX_PerfQuality_Value_DLAA;
	} else if(scale >= 0.66f) {
		return NVSDK_NGX_PerfQuality_Value_MaxQuality;
	} else if(scale >= 0.57f) {
		return NVSDK_NGX_PerfQuality_Value_Balanced;
	} else if(scale >= 0.49f) {
		return NVSDK_NGX_PerfQuality_Value_MaxPerf;
	}
	return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

NVSDK_NGX_DLSS_Hint_Render_Preset presetFromName(const std::string & name) {
	if(name.size() == 1) {
		switch(name[0]) {
			case 'J': case 'j': return NVSDK_NGX_DLSS_Hint_Render_Preset_J;
			case 'K': case 'k': return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
			case 'L': case 'l': return NVSDK_NGX_DLSS_Hint_Render_Preset_L;
			case 'M': case 'm': return NVSDK_NGX_DLSS_Hint_Render_Preset_M;
			default: break;
		}
	}
	return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
}

/*!
 * Shut down NGX, ignoring crashes: tools that intercept DLSS (e.g. OptiScaler, which runs it on Direct3D 12)
 * may crash in the Direct3D 11 shutdown, which would otherwise crash the game on exit.
 * This function must not have objects with destructors.
 */
void shutdownNGX(ID3D11Device * device) {
	#if defined(_MSC_VER)
	__try {
		NVSDK_NGX_D3D11_Shutdown1(device);
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		// Nothing left to clean up
	}
	#else
	NVSDK_NGX_D3D11_Shutdown1(device);
	#endif
}

class DLSSUpscaler final : public D3D11Upscaler {
	
	//! A DLSS feature with its own history for each view (VR eye)
	struct View {
		NVSDK_NGX_Handle * feature = nullptr;
		Vec2i renderSize = Vec2i(0);
		Vec2i outputSize = Vec2i(0);
		// Copies of the inputs and the output for views in parts of the textures (VR eyes)
		d3d11::ComPtr<ID3D11Texture2D> color;
		d3d11::ComPtr<ID3D11Texture2D> depth;
		d3d11::ComPtr<ID3D11Texture2D> motion;
		d3d11::ComPtr<ID3D11Texture2D> output;
	};
	
	//! Create the texture if it does not exist or has another size or format
	bool ensureTexture(d3d11::ComPtr<ID3D11Texture2D> & texture, const Vec2i & size, DXGI_FORMAT format,
	                   UINT bindFlags) {
		if(texture) {
			D3D11_TEXTURE2D_DESC desc;
			texture->GetDesc(&desc);
			if(desc.Width == UINT(size.x) && desc.Height == UINT(size.y) && desc.Format == format) {
				return true;
			}
			texture.Reset();
		}
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = UINT(size.x);
		desc.Height = UINT(size.y);
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = bindFlags;
		if(FAILED(m_device->CreateTexture2D(&desc, nullptr, &texture))) {
			LogError << "Failed to create a " << size.x << "x" << size.y << " texture for DLSS";
			texture.Reset();
			return false;
		}
		return true;
	}
	
	ID3D11Device * m_device;
	NVSDK_NGX_Parameter * m_parameters;
	View m_views[MaxViews];
	NVSDK_NGX_DLSS_Hint_Render_Preset m_preset;
	bool m_initialized;
	
	static void releaseFeature(View & view) {
		if(view.feature) {
			NVSDK_NGX_D3D11_ReleaseFeature(view.feature);
			view.feature = nullptr;
		}
	}
	
	bool createFeature(ID3D11DeviceContext * context, View & view, const Inputs & inputs) {
		
		releaseFeature(view);
		
		const char * const presetParameters[] = {
			NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
			NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
			NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
			NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
			NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
			NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
		};
		for(const char * parameter : presetParameters) {
			NVSDK_NGX_Parameter_SetUI(m_parameters, parameter, unsigned(m_preset));
		}
		
		float scale = float(inputs.renderSize.x) / float(inputs.outputSize.x);
		
		NVSDK_NGX_DLSS_Create_Params params = {};
		params.Feature.InWidth = unsigned(inputs.renderSize.x);
		params.Feature.InHeight = unsigned(inputs.renderSize.y);
		params.Feature.InTargetWidth = unsigned(inputs.outputSize.x);
		params.Feature.InTargetHeight = unsigned(inputs.outputSize.y);
		params.Feature.InPerfQualityValue = qualityForScale(scale);
		// Motion vectors are at the render resolution and do not include the jitter
		params.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
		
		NVSDK_NGX_Result result = NGX_D3D11_CREATE_DLSS_EXT(context, &view.feature, m_parameters, &params);
		if(NVSDK_NGX_FAILED(result)) {
			LogError << "Failed to create DLSS feature for " << inputs.renderSize.x << "x" << inputs.renderSize.y
			         << " -> " << inputs.outputSize.x << "x" << inputs.outputSize.y << ": " << resultString(result);
			view.feature = nullptr;
			return false;
		}
		
		LogInfo << "DLSS view " << inputs.view << ": " << inputs.renderSize.x << "x" << inputs.renderSize.y << " -> "
		        << inputs.outputSize.x << "x" << inputs.outputSize.y << ", preset "
		        << (m_preset == NVSDK_NGX_DLSS_Hint_Render_Preset_Default ? '-' : char('A' + int(m_preset) - 1));
		
		view.renderSize = inputs.renderSize;
		view.outputSize = inputs.outputSize;
		
		return true;
	}
	
public:
	
	DLSSUpscaler(ID3D11Device * device, NVSDK_NGX_DLSS_Hint_Render_Preset preset)
		: m_device(device)
		, m_parameters(nullptr)
		, m_preset(preset)
		, m_initialized(false)
	{ }
	
	~DLSSUpscaler() override {
		for(View & view : m_views) {
			releaseFeature(view);
		}
		if(m_parameters) {
			NVSDK_NGX_D3D11_DestroyParameters(m_parameters);
		}
		if(m_initialized) {
			shutdownNGX(m_device);
		}
	}
	
	bool initialize(const std::string & searchPath) {
		
		// NGX writes its logs and caches here
		std::wstring dataPath;
		{
			WCHAR temp[MAX_PATH];
			DWORD length = GetTempPathW(MAX_PATH, temp);
			if(length > 0 && length < MAX_PATH) {
				dataPath = std::wstring(temp, length) + L"ArxLibertatis";
				CreateDirectoryW(dataPath.c_str(), nullptr);
			} else {
				dataPath = L".";
			}
		}
		
		// nvngx_dlss.dll is searched in the executable directory and the configured path
		platform::WideString path(searchPath);
		std::vector<const wchar_t *> paths;
		if(!searchPath.empty()) {
			paths.push_back(path.c_str());
		}
		NVSDK_NGX_FeatureCommonInfo info = {};
		info.PathListInfo.Path = paths.data();
		info.PathListInfo.Length = unsigned(paths.size());
		info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
		
		NVSDK_NGX_Result result = NVSDK_NGX_D3D11_Init_with_ProjectID(NGXProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
		                                                              "1.3", dataPath.c_str(), m_device, &info);
		if(NVSDK_NGX_FAILED(result)) {
			LogWarning << "DLSS not available: NGX initialization failed with " << resultString(result);
			return false;
		}
		m_initialized = true;
		
		result = NVSDK_NGX_D3D11_GetCapabilityParameters(&m_parameters);
		if(NVSDK_NGX_FAILED(result)) {
			LogWarning << "DLSS not available: could not get NGX capabilities: " << resultString(result);
			m_parameters = nullptr;
			return false;
		}
		
		int needsUpdatedDriver = 0;
		if(NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(m_parameters, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
		                                              &needsUpdatedDriver)) && needsUpdatedDriver) {
			unsigned major = 0, minor = 0;
			NVSDK_NGX_Parameter_GetUI(m_parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
			NVSDK_NGX_Parameter_GetUI(m_parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
			LogWarning << "DLSS not available: driver version " << major << "." << minor << " or newer required";
			return false;
		}
		
		int available = 0;
		if(NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetI(m_parameters, NVSDK_NGX_Parameter_SuperSampling_Available,
		                                             &available)) || !available) {
			LogWarning << "DLSS not available: GPU or driver not supported";
			return false;
		}
		
		LogInfo << "DLSS Super Resolution available";
		
		return true;
	}
	
	bool evaluate(ID3D11DeviceContext * context, const Inputs & inputs) override {
		
		if(inputs.view >= MaxViews) {
			return false;
		}
		View & view = m_views[inputs.view];
		
		bool reset = inputs.reset;
		if(!view.feature || inputs.renderSize != view.renderSize || inputs.outputSize != view.outputSize) {
			if(!createFeature(context, view, inputs)) {
				return false;
			}
			reset = true;
		}
		
		/*
		 * DLSS rejects views in parts of larger textures with sub-rectangles (seen with the VR eyes side by side):
		 * those are copied into textures of their own
		 */
		D3D11_TEXTURE2D_DESC colorDesc;
		inputs.color->GetDesc(&colorDesc);
		D3D11_TEXTURE2D_DESC outputDesc;
		inputs.output->GetDesc(&outputDesc);
		bool copyInputs = inputs.renderOffset != Vec2i(0) || colorDesc.Width != UINT(inputs.renderSize.x)
		                  || colorDesc.Height != UINT(inputs.renderSize.y);
		bool copyOutput = inputs.outputOffset != Vec2i(0) || outputDesc.Width != UINT(inputs.outputSize.x)
		                  || outputDesc.Height != UINT(inputs.outputSize.y);
		
		ID3D11Texture2D * color = inputs.color;
		ID3D11Texture2D * depth = inputs.depth;
		ID3D11Texture2D * motion = inputs.motion;
		ID3D11Texture2D * output = inputs.output;
		if(copyInputs) {
			if(!ensureTexture(view.color, inputs.renderSize, colorDesc.Format, D3D11_BIND_SHADER_RESOURCE)
			   || !ensureTexture(view.depth, inputs.renderSize, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE)
			   || !ensureTexture(view.motion, inputs.renderSize, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE)) {
				return false;
			}
			D3D11_BOX box = {
				UINT(inputs.renderOffset.x), UINT(inputs.renderOffset.y), 0,
				UINT(inputs.renderOffset.x + inputs.renderSize.x), UINT(inputs.renderOffset.y + inputs.renderSize.y), 1
			};
			context->CopySubresourceRegion(view.color.Get(), 0, 0, 0, 0, inputs.color, 0, &box);
			context->CopySubresourceRegion(view.depth.Get(), 0, 0, 0, 0, inputs.depth, 0, &box);
			context->CopySubresourceRegion(view.motion.Get(), 0, 0, 0, 0, inputs.motion, 0, &box);
			color = view.color.Get();
			depth = view.depth.Get();
			motion = view.motion.Get();
		}
		if(copyOutput) {
			if(!ensureTexture(view.output, inputs.outputSize, outputDesc.Format,
			                  D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS)) {
				return false;
			}
			output = view.output.Get();
		}
		
		NVSDK_NGX_D3D11_DLSS_Eval_Params params = {};
		params.Feature.pInColor = color;
		params.Feature.pInOutput = output;
		params.pInDepth = depth;
		params.pInMotionVectors = motion;
		params.InJitterOffsetX = inputs.jitter.x;
		params.InJitterOffsetY = inputs.jitter.y;
		params.InRenderSubrectDimensions.Width = unsigned(inputs.renderSize.x);
		params.InRenderSubrectDimensions.Height = unsigned(inputs.renderSize.y);
		params.InReset = reset ? 1 : 0;
		params.InMVScaleX = 1.f;
		params.InMVScaleY = 1.f;
		params.InFrameTimeDeltaInMsec = inputs.frameTime;
		
		NVSDK_NGX_Result result = NGX_D3D11_EVALUATE_DLSS_EXT(context, view.feature, m_parameters, &params);
		if(NVSDK_NGX_FAILED(result)) {
			static bool warned = false;
			if(!warned) {
				LogError << "DLSS evaluation failed for view " << inputs.view << ": " << resultString(result);
				warned = true;
			}
			return false;
		}
		
		if(copyOutput) {
			context->CopySubresourceRegion(inputs.output, 0, UINT(inputs.outputOffset.x), UINT(inputs.outputOffset.y), 0,
			                               output, 0, nullptr);
		}
		
		return true;
	}
	
	const char * name() const override {
		return "DLSS";
	}
	
	bool supportsViews() const override {
		return true;
	}
	
};
	
} // anonymous namespace

namespace d3d11 {

bool findDLSSLibrary(const std::string & searchPath) {
	return !findUpscalerLibrary(L"nvngx_dlss.dll", searchPath).empty();
}

std::unique_ptr<D3D11Upscaler> createDLSSUpscaler(ID3D11Device * device, const std::string & preset,
                                                  const std::string & searchPath) {
	
	auto upscaler = std::make_unique<DLSSUpscaler>(device, presetFromName(preset));
	if(!upscaler->initialize(searchPath)) {
		return { };
	}
	
	return upscaler;
}
	
} // namespace d3d11
