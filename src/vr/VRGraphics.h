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

#ifndef ARX_VR_VRGRAPHICS_H
#define ARX_VR_VRGRAPHICS_H

#include <memory>
#include <vector>

#include <openxr/openxr.h>

#include "Configure.h"

#include "graphics/StereoView.h"
#include "math/Rectangle.h"
#include "math/Types.h"
#include "platform/Platform.h"

class Texture;

//! A part of the interface image drawn into the desktop window
struct VRWindowQuad {
	Vec4f position[4]; //!< Clip space, as a triangle strip: bottom left, bottom right, top left, top right
	Vec2f uv[4]; //!< In the interface image, with the origin at the top left
};

/*!
 * The graphics API specific part of the VR system: the OpenXR session binding, the swapchain images and the render
 * targets of the eyes and of the interface layer, for the renderer in use (OpenGL or Direct3D 11)
 *
 * Swapchain images are opaque handles: OpenGL texture names or ID3D11Texture2D pointers.
 */
class VRGraphics {

public:
	
	virtual ~VRGraphics() = default;
	
	[[nodiscard]] virtual const char * getName() const = 0;
	//! The OpenXR instance extension for the graphics API
	[[nodiscard]] virtual const char * getExtension() const = 0;
	[[nodiscard]] virtual bool isDirect3D11() const = 0;
	
	//! Check the requirements of the runtime once the instance exists, before the renderer is created
	virtual bool checkRequirements(XrInstance instance, XrSystemId system) = 0;
	//! The graphics binding for xrCreateSession, for the device or context of the renderer, nullptr on failure
	virtual const void * getSessionBinding(XrInstance instance, XrSystemId system) = 0;
	
	//! Pick a swapchain color format: the gamma-encoded colors stored unchanged in an sRGB image are decoded right
	[[nodiscard]] virtual s64 selectColorFormat(const std::vector<s64> & formats) const = 0;
	virtual bool enumerateImages(XrSwapchain swapchain, std::vector<u64> & images) = 0;
	//! Whether quad layer image rectangles count from the top of the image
	[[nodiscard]] virtual bool areImageRectsFromTop(bool metaRuntime) const = 0;
	//! Whether interface parts copied into textures are upside down (OpenGL images start at the bottom)
	[[nodiscard]] virtual bool isPanelCopyFlipped() const = 0;
	
	virtual bool createStereoTarget(const Vec2i & eyeSize) = 0;
	virtual void destroyStereoTarget() = 0;
	//! Redirect rendering into the stereo target, replicating draw calls into the views
	virtual bool beginStereo(const std::vector<StereoView> & views) = 0;
	/*!
	 * Finish the stereo scene and render into the previous target again
	 *
	 * \param uiActive The interface layer is the previous target
	 * \return false if nothing was rendered
	 */
	virtual bool endStereo(bool uiActive) = 0;
	virtual void copyEyeToImage(size_t eye, u64 image) = 0;
	//! Show the center of the left eye in the window, cropped to the window aspect ratio
	virtual void drawEyeToWindow(const Vec2i & windowSize) = 0;
	
	//! The interface layer: a transparent image of the window size with premultiplied alpha
	virtual bool createUITarget(const Vec2i & size) = 0;
	virtual void destroyUITarget() = 0;
	[[nodiscard]] virtual bool hasUITarget() const = 0;
	//! Render into the interface layer instead of the window, cleared
	virtual void beginUI() = 0;
	virtual void endUI() = 0;
	virtual void setPremultipliedAlphaOutput(bool enable) = 0;
	virtual void copyUIToImage(u64 image) = 0;
	//! Copy a part of the interface layer (top left origin) into a texture of that size
	virtual bool copyUIToTexture(Texture * texture, const Rect & area) = 0;
	//! Copy the window into an image of the interface size, for frames drawn directly to the window
	virtual void copyWindowToImage(u64 image, const Vec2i & windowSize) = 0;
	//! Draw parts of the interface layer into the window with premultiplied alpha
	virtual void drawUIToWindow(const Vec2i & windowSize, bool clear, const std::vector<VRWindowQuad> & quads) = 0;
	
};

//! Log a failed OpenXR call, see VRSystem.cpp
bool checkXrResult(XrInstance instance, XrResult result, const char * call);

std::unique_ptr<VRGraphics> createVRGraphicsOpenGL();
#if ARX_HAVE_D3D11
std::unique_ptr<VRGraphics> createVRGraphicsD3D11();
#endif

#endif // ARX_VR_VRGRAPHICS_H
