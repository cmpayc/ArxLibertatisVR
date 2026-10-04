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

#ifndef ARX_VR_VRINPUT_H
#define ARX_VR_VRINPUT_H

#include <array>

#include <glm/gtc/quaternion.hpp>
#include <openxr/openxr.h>

#include "math/Types.h"

enum VRHand {
	VRHandLeft = 0,
	VRHandRight = 1,
	VRHandCount = 2
};

enum VRButton {
	VRButtonTrigger,   //!< Index trigger (analog, with hysteresis)
	VRButtonGrip,      //!< Grip / squeeze (analog, with hysteresis)
	VRButtonPrimary,   //!< A / X
	VRButtonSecondary, //!< B / Y
	VRButtonStick,     //!< Thumbstick click
	VRButtonMenu,      //!< Menu button (left hand on Touch controllers)
	VRButtonCount
};

struct VRPose {
	bool valid = false;
	Vec3f position = Vec3f(0.f); //!< OpenXR stage space, meters
	glm::quat orientation = glm::quat(1.f, 0.f, 0.f, 0.f);
};

struct VRHandState {

	bool active = false; //!< A controller is bound to this hand

	VRPose grip; //!< Where the controller is held
	VRPose aim;  //!< Pointing ray: origin and forward (-z)

	float trigger = 0.f;
	float squeeze = 0.f;
	Vec2f thumbstick = Vec2f(0.f);

	std::array<bool, VRButtonCount> pressed = { };
	std::array<bool, VRButtonCount> previous = { };

	[[nodiscard]] bool isPressed(VRButton button) const { return pressed[button]; }
	[[nodiscard]] bool wasPressed(VRButton button) const { return pressed[button] && !previous[button]; }
	[[nodiscard]] bool wasReleased(VRButton button) const { return !pressed[button] && previous[button]; }

};

/*!
 * OpenXR actions for the motion controllers
 *
 * Bindings are suggested for Oculus Touch, Valve Index, HTC Vive, Windows Mixed Reality and the KHR simple
 * controller. Runtimes can remap these for other controllers.
 */
class VRInput {

public:

	VRInput();

	//! Create the actions and suggest bindings - after instance creation, before the session
	bool init(XrInstance instance);
	void shutdown();

	//! Attach the actions to a new session
	bool attach(XrSession session);
	void detach();

	//! Read the controller state for the given predicted display time
	void update(XrTime time, XrSpace space);

	[[nodiscard]] const VRHandState & getHand(VRHand hand) const { return m_hands[hand]; }

	//! Log the interaction profile (controller type) bound to each hand
	void logInteractionProfiles() const;
	
	//! Vibrate a controller: amplitude in [0, 1], duration in seconds
	void vibrate(VRHand hand, float amplitude, float duration);

private:

	XrAction createAction(const char * name, const char * localizedName, XrActionType type);
	void suggestBindings(const char * profile, std::initializer_list<std::pair<XrAction, const char *>> bindings);
	XrPath path(const char * string) const;

	XrInstance m_instance;
	XrSession m_session;
	XrActionSet m_actionSet;
	std::array<XrPath, VRHandCount> m_handPaths;

	XrAction m_gripPose;
	XrAction m_aimPose;
	XrAction m_trigger;
	XrAction m_squeeze;
	XrAction m_thumbstick;
	XrAction m_thumbstickClick;
	XrAction m_primary;
	XrAction m_secondary;
	XrAction m_menu;
	XrAction m_haptic;

	std::array<XrSpace, VRHandCount> m_gripSpaces;
	std::array<XrSpace, VRHandCount> m_aimSpaces;

	std::array<VRHandState, VRHandCount> m_hands;

};

#endif // ARX_VR_VRINPUT_H
