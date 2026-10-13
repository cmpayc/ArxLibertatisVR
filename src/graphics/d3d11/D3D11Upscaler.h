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

#ifndef ARX_GRAPHICS_D3D11_D3D11UPSCALER_H
#define ARX_GRAPHICS_D3D11_D3D11UPSCALER_H

#include <memory>
#include <string>

#include "graphics/d3d11/D3D11ComPtr.h"
#include "math/Types.h"

/*!
 * Temporal upscaler for the 3D scene.
 *
 * Inputs are rendered at the render size with sub-pixel jitter, the output has the window size.
 */
class D3D11Upscaler {

public:
	
	struct Inputs {
		ID3D11Texture2D * color = nullptr;  //!< Scene color without MSAA
		ID3D11Texture2D * depth = nullptr;  //!< R32_FLOAT depth in [0, 1], 1 = far plane
		ID3D11Texture2D * motion = nullptr; //!< R16G16_FLOAT motion from the current to the previous frame in pixels
		ID3D11Texture2D * output = nullptr; //!< Output with unordered access, at the window size
		Vec2i renderSize = Vec2i(0);
		Vec2i outputSize = Vec2i(0);
		//! The view (VR eye): each view has its own history, see supportsViews()
		size_t view = 0;
		Vec2i renderOffset = Vec2i(0);      //!< Where the view is in the color, depth and motion textures
		Vec2i outputOffset = Vec2i(0);      //!< Where the view goes in the output
		Vec2f jitter = Vec2f(0.f);          //!< Jitter applied to the projection, in render pixels
		bool reset = false;                 //!< Discard the history, e.g. after a camera cut
		float frameTime = 0.f;              //!< Time since the last frame in milliseconds
		float cameraNear = 1.f;             //!< Near plane distance
		float cameraFar = 1.f;              //!< Far plane distance
		float cameraFovVertical = 1.f;      //!< Vertical field of view in radians
		float sharpness = 0.f;              //!< Strength of an additional sharpening pass in [0, 1], 0 = off
	};
	
	virtual ~D3D11Upscaler() = default;
	
	//! Upscale the inputs, returns false if the output was not written
	virtual bool evaluate(ID3D11DeviceContext * context, const Inputs & inputs) = 0;
	
	//! Short name used in log messages
	virtual const char * name() const = 0;

	//! Maximum number of views
	static constexpr size_t MaxViews = 2;

	//! \return true if the upscaler supports several views (VR eyes) in parts of the textures
	virtual bool supportsViews() const { return false; }
	
};

namespace d3d11 {

/*!
 * Create the NVIDIA DLSS Super Resolution upscaler.
 *
 * \param preset DLSS preset letter (e.g. "K" for the DLSS 4 transformer model) or empty for the default.
 * \param searchPath additional directory to search for nvngx_dlss.dll
 *
 * \return nullptr if DLSS is not available
 */
std::unique_ptr<D3D11Upscaler> createDLSSUpscaler(ID3D11Device * device, const std::string & preset,
                                                  const std::string & searchPath);

/*!
 * Create the AMD FidelityFX Super Resolution upscaler (FSR 3.1 or 4, chosen by the FidelityFX library).
 *
 * \param searchPath additional directory to search for the FidelityFX libraries
 *
 * \return nullptr if FSR is not available
 */
std::unique_ptr<D3D11Upscaler> createFSRUpscaler(ID3D11Device * device, ID3D11DeviceContext * context,
                                                 const std::string & searchPath);

/*!
 * Check if the AMD FidelityFX library, which is not distributed with Arx Libertatis, can be found.
 */
bool findFSRLibrary(const std::string & searchPath);

/*!
 * Find an upscaler library in the executable directory or in searchPath.
 *
 * \return the full path or an empty string if the library was not found
 */
std::wstring findUpscalerLibrary(const wchar_t * name, const std::string & searchPath);

/*!
 * Create an upscaler that only copies the scene through the Direct3D 12 bridge, for testing the bridge.
 *
 * \return nullptr if Direct3D 12 is not available
 */
std::unique_ptr<D3D11Upscaler> createBridgeTestUpscaler(ID3D11Device * device, ID3D11DeviceContext * context);

/*!
 * Check if nvngx_dlss.dll, which is not distributed with Arx Libertatis, can be found.
 *
 * \return true if the library exists in the executable directory or in searchPath
 */
bool findDLSSLibrary(const std::string & searchPath);

} // namespace d3d11

#endif // ARX_GRAPHICS_D3D11_D3D11UPSCALER_H
