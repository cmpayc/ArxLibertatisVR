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

	//! Acquire the current swapchain image for an eye and return its OpenGL texture name
	unsigned acquireEyeImage(size_t eye);
	void releaseEyeImage(size_t eye);

	//! Debug output: blit the window back buffer into both eye images (\ref beginFrame must have returned true)
	void mirrorWindowToEyes(const Vec2i & windowSize);

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
	unsigned m_resolveFramebuffer;
	unsigned m_resolveRenderbuffer;
	Vec2i m_resolveSize;

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
