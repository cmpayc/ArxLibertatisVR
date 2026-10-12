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

class DLSSUpscaler final : public D3D11Upscaler {
	
	ID3D11Device * m_device;
	NVSDK_NGX_Parameter * m_parameters;
	NVSDK_NGX_Handle * m_feature;
	Vec2i m_renderSize;
	Vec2i m_outputSize;
	NVSDK_NGX_DLSS_Hint_Render_Preset m_preset;
	bool m_initialized;
	
	void releaseFeature() {
		if(m_feature) {
			NVSDK_NGX_D3D11_ReleaseFeature(m_feature);
			m_feature = nullptr;
		}
	}
	
	bool createFeature(ID3D11DeviceContext * context, const Inputs & inputs) {
		
		releaseFeature();
		
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
		
		NVSDK_NGX_Result result = NGX_D3D11_CREATE_DLSS_EXT(context, &m_feature, m_parameters, &params);
		if(NVSDK_NGX_FAILED(result)) {
			LogError << "Failed to create DLSS feature for " << inputs.renderSize.x << "x" << inputs.renderSize.y
			         << " -> " << inputs.outputSize.x << "x" << inputs.outputSize.y << ": " << resultString(result);
			m_feature = nullptr;
			return false;
		}
		
		LogInfo << "DLSS: " << inputs.renderSize.x << "x" << inputs.renderSize.y << " -> "
		        << inputs.outputSize.x << "x" << inputs.outputSize.y << ", preset "
		        << (m_preset == NVSDK_NGX_DLSS_Hint_Render_Preset_Default ? '-' : char('A' + int(m_preset) - 1));
		
		m_renderSize = inputs.renderSize;
		m_outputSize = inputs.outputSize;
		
		return true;
	}
	
public:
	
	DLSSUpscaler(ID3D11Device * device, NVSDK_NGX_DLSS_Hint_Render_Preset preset)
		: m_device(device)
		, m_parameters(nullptr)
		, m_feature(nullptr)
		, m_renderSize(0)
		, m_outputSize(0)
		, m_preset(preset)
		, m_initialized(false)
	{ }
	
	~DLSSUpscaler() override {
		releaseFeature();
		if(m_parameters) {
			NVSDK_NGX_D3D11_DestroyParameters(m_parameters);
		}
		if(m_initialized) {
			NVSDK_NGX_D3D11_Shutdown1(m_device);
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
		
		bool reset = inputs.reset;
		if(!m_feature || inputs.renderSize != m_renderSize || inputs.outputSize != m_outputSize) {
			if(!createFeature(context, inputs)) {
				return false;
			}
			reset = true;
		}
		
		NVSDK_NGX_D3D11_DLSS_Eval_Params params = {};
		params.Feature.pInColor = inputs.color;
		params.Feature.pInOutput = inputs.output;
		params.pInDepth = inputs.depth;
		params.pInMotionVectors = inputs.motion;
		params.InJitterOffsetX = inputs.jitter.x;
		params.InJitterOffsetY = inputs.jitter.y;
		params.InRenderSubrectDimensions.Width = unsigned(inputs.renderSize.x);
		params.InRenderSubrectDimensions.Height = unsigned(inputs.renderSize.y);
		params.InReset = reset ? 1 : 0;
		params.InMVScaleX = 1.f;
		params.InMVScaleY = 1.f;
		params.InFrameTimeDeltaInMsec = inputs.frameTime;
		
		NVSDK_NGX_Result result = NGX_D3D11_EVALUATE_DLSS_EXT(context, m_feature, m_parameters, &params);
		if(NVSDK_NGX_FAILED(result)) {
			static bool warned = false;
			if(!warned) {
				LogError << "DLSS evaluation failed: " << resultString(result);
				warned = true;
			}
			return false;
		}
		
		return true;
	}
	
	const char * name() const override {
		return "DLSS";
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
