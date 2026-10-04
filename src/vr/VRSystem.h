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

#ifndef ARX_VR_VRSYSTEM_H
#define ARX_VR_VRSYSTEM_H

#include <array>
#include <string>
#include <vector>

#include <openxr/openxr.h>

#include "graphics/opengl/OpenGLRenderer.h"
#include "math/Types.h"


/*!
 * OpenXR integration: instance, OpenGL session, stereo swapchains and the frame loop.
 *
 * Lifetime:
 *  - initInstance() once at startup (no GL context needed)
 *  - createSession() / destroySession() follow the OpenGL context (onRendererInit / onRendererShutdown)
 *  - each frame: pollEvents(), beginFrame(), render into the eye images, endFrame()
 */
class VRSystem {

public:

	static const size_t EyeCount = 2;

	VRSystem();
	~VRSystem();

	VRSystem(const VRSystem &) = delete;
	VRSystem & operator=(const VRSystem &) = delete;

	//! \return true if VR was enabled in the config or on the command line
	static bool isRequested();

	bool initInstance();
	void shutdown();

	//! Create the session for the OpenGL context that is current on the calling thread
	bool createSession();
	void destroySession();

	//! Process pending OpenXR events, must be called once per frame
	void pollEvents();

	[[nodiscard]] bool hasSession() const { return m_session != XR_NULL_HANDLE; }
	[[nodiscard]] bool isSessionRunning() const { return m_sessionRunning; }
	[[nodiscard]] bool isFocused() const { return m_sessionState == XR_SESSION_STATE_FOCUSED; }
	[[nodiscard]] bool isExitRequested() const { return m_exitRequested; }

	/*!
	 * Wait for and begin the next frame and locate the eye views.
	 * endFrame() must be called after this returns, even if it returns false.
	 *
	 * \return true if the application should render the eye images this frame
	 */
	bool beginFrame();
	void endFrame();

	[[nodiscard]] bool isFrameActive() const { return m_frameActive; }
	[[nodiscard]] Vec2i getEyeSize() const { return m_eyeSize; }
	[[nodiscard]] const XrView & getView(size_t eye) const { return m_views[eye]; }

	//! \return true between beginFrame() and endFrame() if the eye images should be rendered
	[[nodiscard]] bool isRenderingFrame() const { return m_frameActive && m_shouldRender; }

	//! Set the eye views for the current frame, see \ref prepareVRCamera
	void setStereoViews(std::vector<OpenGLRenderer::StereoView> views);

	/*!
	 * Redirect rendering into the stereo render target, replicating draw calls into both eyes
	 *
	 * \return false if there are no eye views for this frame, in which case nothing was changed
	 */
	bool beginStereoRender();

	//! Resolve the stereo render target into the eye images and show the left eye in the window
	void endStereoRender(const Vec2i & windowSize);

	[[nodiscard]] bool hasRenderedStereo() const { return m_stereoRendered; }

	/*!
	 * Redirect rendering into the virtual screen: a transparent window-sized layer shown as a quad in VR
	 *
	 * All 2D rendering (interface, menus, cinematics) should happen between beginUIRender() and endUIRender().
	 * The stereo 3D scene can be rendered in between (\ref beginStereoRender restores this target).
	 *
	 * \return false if there is no VR frame to render
	 */
	bool beginUIRender(const Vec2i & windowSize);

	/*!
	 * Submit the virtual screen to the headset and composite it into the window
	 *
	 * \param overScene true if the virtual screen is drawn over the 3D scene (in-game HUD) - it then follows
	 *                  the head, otherwise it stays in place like a cinema screen
	 */
	void endUIRender(const Vec2i & windowSize, bool overScene);

	[[nodiscard]] bool isUIActive() const { return m_uiActive; }
	[[nodiscard]] bool hasSubmittedUI() const { return m_uiSubmitted; }

	//! Show the window contents as the virtual screen, for frames rendered without \ref beginUIRender
	void submitWindowAsUI(const Vec2i & windowSize);

	//! Place the virtual screen in front of the user again
	void resetUIPlacement() { m_uiPlaced = false; }

	//! Acquire the current swapchain image for an eye and return its OpenGL texture name
	unsigned acquireEyeImage(size_t eye);
	void releaseEyeImage(size_t eye);

	[[nodiscard]] const std::string & getRuntimeName() const { return m_runtimeName; }
	[[nodiscard]] const std::string & getSystemName() const { return m_systemName; }

private:

	struct EyeSwapchain {
		XrSwapchain handle = XR_NULL_HANDLE;
		std::vector<unsigned> images;
		bool acquired = false;
	};

	bool createSwapchains();
	void destroySwapchains();
	bool createStereoTarget();
	void destroyStereoTarget();
	bool createUITarget(const Vec2i & size);
	void destroyUITarget();
	void updateUIPlacement(bool followHead);
	void drawTextureToWindow(unsigned texture, const Vec2i & windowSize, bool clear);
	unsigned acquireSwapchainImage(EyeSwapchain & swapchain);
	void releaseSwapchainImage(EyeSwapchain & swapchain);
	void handleSessionStateChange(XrSessionState state);
	bool check(XrResult result, const char * call) const;

	XrInstance m_instance;
	XrSystemId m_systemId;
	XrSession m_session;
	XrSpace m_stageSpace;
	XrSessionState m_sessionState;
	bool m_sessionRunning;
	bool m_exitRequested;

	std::array<EyeSwapchain, EyeCount> m_swapchains;
	std::array<XrView, EyeCount> m_views;
	std::array<XrCompositionLayerProjectionView, EyeCount> m_layerViews;
	Vec2i m_eyeSize;
	s64 m_colorFormat;
	unsigned m_blitFramebuffer;

	// Double-wide (left eye | right eye) multisampled render target and its resolve target
	std::vector<OpenGLRenderer::StereoView> m_stereoViews;
	unsigned m_stereoFramebuffer;
	unsigned m_stereoColor;
	unsigned m_stereoDepth;
	unsigned m_stereoResolveFramebuffer;
	unsigned m_stereoResolveColor;
	bool m_stereoActive;
	bool m_stereoRendered;
	unsigned m_stereoPreviousTarget;
	Vec2i m_stereoPreviousTargetSize;

	// Virtual screen (interface layer) render target and quad layer
	EyeSwapchain m_uiSwapchain;
	unsigned m_uiFramebuffer;
	unsigned m_uiColor;
	unsigned m_uiDepth;
	Vec2i m_uiSize;
	bool m_uiActive;
	bool m_uiSubmitted;
	bool m_uiPlaced;
	bool m_uiFollowHead;
	bool m_uiTurning;
	float m_uiYaw; // radians, OpenXR stage space
	Vec3f m_uiAnchor; // head position the screen is placed relative to, OpenXR stage space
	XrPosef m_uiPose;
	XrExtent2Df m_uiExtent;

	XrFrameState m_frameState;
	bool m_frameActive;
	bool m_shouldRender;
	bool m_rendered;

	std::string m_runtimeName;
	std::string m_systemName;

};

//! Non-null while VR is enabled
extern VRSystem * g_vr;

#endif // ARX_VR_VRSYSTEM_H
