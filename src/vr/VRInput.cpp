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

#include "vr/VRInput.h"

#include <cstdio>
#include <vector>

#include "io/log/Logger.h"
#include "vr/VRFunctions.h"


namespace {

const char * const g_handPathNames[VRHandCount] = { "/user/hand/left", "/user/hand/right" };

// Analog inputs are treated as pressed above the first and released below the second threshold
const float PressThreshold = 0.6f;
const float ReleaseThreshold = 0.4f;

bool analogPressed(float value, bool wasPressed) {
	return wasPressed ? value > ReleaseThreshold : value > PressThreshold;
}

VRPose locate(XrSpace space, XrSpace baseSpace, XrTime time) {

	VRPose pose;

	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
	if(space == XR_NULL_HANDLE || XR_FAILED(xrLocateSpace(space, baseSpace, time, &location))) {
		return pose;
	}

	const XrSpaceLocationFlags required = XR_SPACE_LOCATION_POSITION_VALID_BIT
	                                      | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	if((location.locationFlags & required) != required) {
		return pose;
	}

	pose.valid = true;
	pose.position = Vec3f(location.pose.position.x, location.pose.position.y, location.pose.position.z);
	pose.orientation = glm::quat(location.pose.orientation.w, location.pose.orientation.x,
	                             location.pose.orientation.y, location.pose.orientation.z);
	return pose;
}

} // anonymous namespace

VRInput::VRInput()
	: m_instance(XR_NULL_HANDLE)
	, m_session(XR_NULL_HANDLE)
	, m_actionSet(XR_NULL_HANDLE)
	, m_handPaths{ XR_NULL_PATH, XR_NULL_PATH }
	, m_gripPose(XR_NULL_HANDLE)
	, m_aimPose(XR_NULL_HANDLE)
	, m_trigger(XR_NULL_HANDLE)
	, m_squeeze(XR_NULL_HANDLE)
	, m_thumbstick(XR_NULL_HANDLE)
	, m_thumbstickClick(XR_NULL_HANDLE)
	, m_primary(XR_NULL_HANDLE)
	, m_secondary(XR_NULL_HANDLE)
	, m_menu(XR_NULL_HANDLE)
	, m_haptic(XR_NULL_HANDLE)
	, m_gripSpaces{ XR_NULL_HANDLE, XR_NULL_HANDLE }
	, m_aimSpaces{ XR_NULL_HANDLE, XR_NULL_HANDLE }
{ }

XrPath VRInput::path(const char * string) const {
	XrPath result = XR_NULL_PATH;
	if(XR_FAILED(xrStringToPath(m_instance, string, &result))) {
		LogWarning << "OpenXR: invalid path " << string;
	}
	return result;
}

XrAction VRInput::createAction(const char * name, const char * localizedName, XrActionType type) {

	XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
	info.actionType = type;
	std::snprintf(info.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", name);
	std::snprintf(info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", localizedName);
	info.countSubactionPaths = uint32_t(m_handPaths.size());
	info.subactionPaths = m_handPaths.data();

	XrAction action = XR_NULL_HANDLE;
	if(XR_FAILED(xrCreateAction(m_actionSet, &info, &action))) {
		LogError << "OpenXR: could not create action " << name;
		return XR_NULL_HANDLE;
	}
	return action;
}

void VRInput::suggestBindings(const char * profile,
                              std::initializer_list<std::pair<XrAction, const char *>> bindings) {

	std::vector<XrActionSuggestedBinding> suggested;
	for(const auto & binding : bindings) {
		XrPath bindingPath = path(binding.second);
		if(binding.first != XR_NULL_HANDLE && bindingPath != XR_NULL_PATH) {
			suggested.push_back({ binding.first, bindingPath });
		}
	}

	XrInteractionProfileSuggestedBinding info = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	info.interactionProfile = path(profile);
	info.countSuggestedBindings = uint32_t(suggested.size());
	info.suggestedBindings = suggested.data();

	XrResult result = xrSuggestInteractionProfileBindings(m_instance, &info);
	if(XR_FAILED(result)) {
		LogWarning << "OpenXR: could not suggest bindings for " << profile << " (" << int(result) << ')';
	}

}

bool VRInput::init(XrInstance instance) {

	m_instance = instance;

	XrActionSetCreateInfo setInfo = { XR_TYPE_ACTION_SET_CREATE_INFO };
	std::snprintf(setInfo.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "%s", "gameplay");
	std::snprintf(setInfo.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "%s", "Gameplay");
	if(XR_FAILED(xrCreateActionSet(m_instance, &setInfo, &m_actionSet))) {
		LogError << "OpenXR: could not create the action set";
		m_actionSet = XR_NULL_HANDLE;
		return false;
	}

	for(size_t hand = 0; hand < VRHandCount; hand++) {
		m_handPaths[hand] = path(g_handPathNames[hand]);
	}

	m_gripPose = createAction("grip_pose", "Hand pose", XR_ACTION_TYPE_POSE_INPUT);
	m_aimPose = createAction("aim_pose", "Pointer pose", XR_ACTION_TYPE_POSE_INPUT);
	m_trigger = createAction("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT);
	m_squeeze = createAction("grip", "Grip", XR_ACTION_TYPE_FLOAT_INPUT);
	m_thumbstick = createAction("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT);
	m_thumbstickClick = createAction("thumbstick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT);
	m_primary = createAction("primary", "Primary button (A/X)", XR_ACTION_TYPE_BOOLEAN_INPUT);
	m_secondary = createAction("secondary", "Secondary button (B/Y)", XR_ACTION_TYPE_BOOLEAN_INPUT);
	m_menu = createAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
	m_haptic = createAction("haptic", "Vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT);

	suggestBindings("/interaction_profiles/oculus/touch_controller", {
		{ m_gripPose, "/user/hand/left/input/grip/pose" },
		{ m_gripPose, "/user/hand/right/input/grip/pose" },
		{ m_aimPose, "/user/hand/left/input/aim/pose" },
		{ m_aimPose, "/user/hand/right/input/aim/pose" },
		{ m_trigger, "/user/hand/left/input/trigger/value" },
		{ m_trigger, "/user/hand/right/input/trigger/value" },
		{ m_squeeze, "/user/hand/left/input/squeeze/value" },
		{ m_squeeze, "/user/hand/right/input/squeeze/value" },
		{ m_thumbstick, "/user/hand/left/input/thumbstick" },
		{ m_thumbstick, "/user/hand/right/input/thumbstick" },
		{ m_thumbstickClick, "/user/hand/left/input/thumbstick/click" },
		{ m_thumbstickClick, "/user/hand/right/input/thumbstick/click" },
		{ m_primary, "/user/hand/left/input/x/click" },
		{ m_primary, "/user/hand/right/input/a/click" },
		{ m_secondary, "/user/hand/left/input/y/click" },
		{ m_secondary, "/user/hand/right/input/b/click" },
		{ m_menu, "/user/hand/left/input/menu/click" },
		{ m_haptic, "/user/hand/left/output/haptic" },
		{ m_haptic, "/user/hand/right/output/haptic" },
	});

	suggestBindings("/interaction_profiles/valve/index_controller", {
		{ m_gripPose, "/user/hand/left/input/grip/pose" },
		{ m_gripPose, "/user/hand/right/input/grip/pose" },
		{ m_aimPose, "/user/hand/left/input/aim/pose" },
		{ m_aimPose, "/user/hand/right/input/aim/pose" },
		{ m_trigger, "/user/hand/left/input/trigger/value" },
		{ m_trigger, "/user/hand/right/input/trigger/value" },
		{ m_squeeze, "/user/hand/left/input/squeeze/value" },
		{ m_squeeze, "/user/hand/right/input/squeeze/value" },
		{ m_thumbstick, "/user/hand/left/input/thumbstick" },
		{ m_thumbstick, "/user/hand/right/input/thumbstick" },
		{ m_thumbstickClick, "/user/hand/left/input/thumbstick/click" },
		{ m_thumbstickClick, "/user/hand/right/input/thumbstick/click" },
		{ m_primary, "/user/hand/left/input/a/click" },
		{ m_primary, "/user/hand/right/input/a/click" },
		{ m_secondary, "/user/hand/left/input/b/click" },
		{ m_secondary, "/user/hand/right/input/b/click" },
		{ m_haptic, "/user/hand/left/output/haptic" },
		{ m_haptic, "/user/hand/right/output/haptic" },
	});

	suggestBindings("/interaction_profiles/htc/vive_controller", {
		{ m_gripPose, "/user/hand/left/input/grip/pose" },
		{ m_gripPose, "/user/hand/right/input/grip/pose" },
		{ m_aimPose, "/user/hand/left/input/aim/pose" },
		{ m_aimPose, "/user/hand/right/input/aim/pose" },
		{ m_trigger, "/user/hand/left/input/trigger/value" },
		{ m_trigger, "/user/hand/right/input/trigger/value" },
		{ m_squeeze, "/user/hand/left/input/squeeze/click" },
		{ m_squeeze, "/user/hand/right/input/squeeze/click" },
		{ m_thumbstick, "/user/hand/left/input/trackpad" },
		{ m_thumbstick, "/user/hand/right/input/trackpad" },
		{ m_thumbstickClick, "/user/hand/left/input/trackpad/click" },
		{ m_thumbstickClick, "/user/hand/right/input/trackpad/click" },
		{ m_menu, "/user/hand/left/input/menu/click" },
		{ m_secondary, "/user/hand/right/input/menu/click" },
		{ m_haptic, "/user/hand/left/output/haptic" },
		{ m_haptic, "/user/hand/right/output/haptic" },
	});

	suggestBindings("/interaction_profiles/microsoft/motion_controller", {
		{ m_gripPose, "/user/hand/left/input/grip/pose" },
		{ m_gripPose, "/user/hand/right/input/grip/pose" },
		{ m_aimPose, "/user/hand/left/input/aim/pose" },
		{ m_aimPose, "/user/hand/right/input/aim/pose" },
		{ m_trigger, "/user/hand/left/input/trigger/value" },
		{ m_trigger, "/user/hand/right/input/trigger/value" },
		{ m_squeeze, "/user/hand/left/input/squeeze/click" },
		{ m_squeeze, "/user/hand/right/input/squeeze/click" },
		{ m_thumbstick, "/user/hand/left/input/thumbstick" },
		{ m_thumbstick, "/user/hand/right/input/thumbstick" },
		{ m_thumbstickClick, "/user/hand/left/input/thumbstick/click" },
		{ m_thumbstickClick, "/user/hand/right/input/thumbstick/click" },
		{ m_primary, "/user/hand/left/input/trackpad/click" },
		{ m_primary, "/user/hand/right/input/trackpad/click" },
		{ m_menu, "/user/hand/left/input/menu/click" },
		{ m_secondary, "/user/hand/right/input/menu/click" },
		{ m_haptic, "/user/hand/left/output/haptic" },
		{ m_haptic, "/user/hand/right/output/haptic" },
	});

	suggestBindings("/interaction_profiles/khr/simple_controller", {
		{ m_gripPose, "/user/hand/left/input/grip/pose" },
		{ m_gripPose, "/user/hand/right/input/grip/pose" },
		{ m_aimPose, "/user/hand/left/input/aim/pose" },
		{ m_aimPose, "/user/hand/right/input/aim/pose" },
		{ m_trigger, "/user/hand/left/input/select/click" },
		{ m_trigger, "/user/hand/right/input/select/click" },
		{ m_menu, "/user/hand/left/input/menu/click" },
		{ m_secondary, "/user/hand/right/input/menu/click" },
		{ m_haptic, "/user/hand/left/output/haptic" },
		{ m_haptic, "/user/hand/right/output/haptic" },
	});

	return true;
}

void VRInput::shutdown() {

	detach();

	// Destroying the action set also destroys its actions
	if(m_actionSet != XR_NULL_HANDLE) {
		xrDestroyActionSet(m_actionSet);
	}
	*this = VRInput();

}

bool VRInput::attach(XrSession session) {

	if(m_actionSet == XR_NULL_HANDLE) {
		return false;
	}

	m_session = session;

	XrSessionActionSetsAttachInfo attachInfo = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &m_actionSet;
	if(XR_FAILED(xrAttachSessionActionSets(m_session, &attachInfo))) {
		LogError << "OpenXR: could not attach the action set";
		m_session = XR_NULL_HANDLE;
		return false;
	}

	for(size_t hand = 0; hand < VRHandCount; hand++) {
		XrActionSpaceCreateInfo spaceInfo = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
		spaceInfo.subactionPath = m_handPaths[hand];
		spaceInfo.poseInActionSpace.orientation.w = 1.f;
		spaceInfo.action = m_gripPose;
		xrCreateActionSpace(m_session, &spaceInfo, &m_gripSpaces[hand]);
		spaceInfo.action = m_aimPose;
		xrCreateActionSpace(m_session, &spaceInfo, &m_aimSpaces[hand]);
	}

	return true;
}

void VRInput::detach() {

	for(size_t hand = 0; hand < VRHandCount; hand++) {
		if(m_gripSpaces[hand] != XR_NULL_HANDLE) {
			xrDestroySpace(m_gripSpaces[hand]);
			m_gripSpaces[hand] = XR_NULL_HANDLE;
		}
		if(m_aimSpaces[hand] != XR_NULL_HANDLE) {
			xrDestroySpace(m_aimSpaces[hand]);
			m_aimSpaces[hand] = XR_NULL_HANDLE;
		}
		m_hands[hand] = VRHandState();
	}

	m_session = XR_NULL_HANDLE;

}

void VRInput::update(XrTime time, XrSpace space) {

	if(m_session == XR_NULL_HANDLE) {
		return;
	}

	XrActiveActionSet activeSet = { m_actionSet, XR_NULL_PATH };
	XrActionsSyncInfo syncInfo = { XR_TYPE_ACTIONS_SYNC_INFO };
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &activeSet;
	XrResult result = xrSyncActions(m_session, &syncInfo);

	// Inputs are inactive while the application is not focused (e.g. the system menu is open)
	bool focused = (result == XR_SUCCESS);

	for(size_t hand = 0; hand < VRHandCount; hand++) {

		VRHandState & state = m_hands[hand];
		state.previous = state.pressed;

		XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
		getInfo.subactionPath = m_handPaths[hand];

		auto getFloat = [&](XrAction action) {
			XrActionStateFloat value = { XR_TYPE_ACTION_STATE_FLOAT };
			getInfo.action = action;
			return (focused && XR_SUCCEEDED(xrGetActionStateFloat(m_session, &getInfo, &value)) && value.isActive)
			       ? value.currentState : 0.f;
		};
		auto getBoolean = [&](XrAction action) {
			XrActionStateBoolean value = { XR_TYPE_ACTION_STATE_BOOLEAN };
			getInfo.action = action;
			return focused && XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &getInfo, &value)) && value.isActive
			       && value.currentState;
		};

		XrActionStatePose poseState = { XR_TYPE_ACTION_STATE_POSE };
		getInfo.action = m_gripPose;
		state.active = XR_SUCCEEDED(xrGetActionStatePose(m_session, &getInfo, &poseState)) && poseState.isActive;

		state.grip = locate(m_gripSpaces[hand], space, time);
		state.aim = locate(m_aimSpaces[hand], space, time);

		state.trigger = getFloat(m_trigger);
		state.squeeze = getFloat(m_squeeze);

		XrActionStateVector2f stick = { XR_TYPE_ACTION_STATE_VECTOR2F };
		getInfo.action = m_thumbstick;
		bool stickActive = focused && XR_SUCCEEDED(xrGetActionStateVector2f(m_session, &getInfo, &stick))
		                   && stick.isActive;
		state.thumbstick = stickActive ? Vec2f(stick.currentState.x, stick.currentState.y) : Vec2f(0.f);

		state.pressed[VRButtonTrigger] = analogPressed(state.trigger, state.previous[VRButtonTrigger]);
		state.pressed[VRButtonGrip] = analogPressed(state.squeeze, state.previous[VRButtonGrip]);
		state.pressed[VRButtonPrimary] = getBoolean(m_primary);
		state.pressed[VRButtonSecondary] = getBoolean(m_secondary);
		state.pressed[VRButtonStick] = getBoolean(m_thumbstickClick);
		state.pressed[VRButtonMenu] = getBoolean(m_menu);

	}

}

void VRInput::vibrate(VRHand hand, float amplitude, float duration) {

	if(m_session == XR_NULL_HANDLE) {
		return;
	}

	XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };
	vibration.amplitude = amplitude;
	vibration.duration = XrDuration(double(duration) * 1e9);
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;

	XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };
	info.action = m_haptic;
	info.subactionPath = m_handPaths[hand];
	XrResult result = xrApplyHapticFeedback(m_session, &info, reinterpret_cast<const XrHapticBaseHeader *>(&vibration));
	static bool logged = false;
	if(XR_FAILED(result) && !logged) {
		LogWarning << "OpenXR: xrApplyHapticFeedback failed (" << int(result) << ')';
		logged = true;
	}

}

void VRInput::logInteractionProfiles() const {
	
	if(m_session == XR_NULL_HANDLE) {
		return;
	}
	
	for(size_t hand = 0; hand < VRHandCount; hand++) {
		XrInteractionProfileState state = { XR_TYPE_INTERACTION_PROFILE_STATE };
		if(XR_FAILED(xrGetCurrentInteractionProfile(m_session, m_handPaths[hand], &state))) {
			continue;
		}
		char name[XR_MAX_PATH_LENGTH] = "none";
		uint32_t length = 0;
		if(state.interactionProfile != XR_NULL_PATH) {
			xrPathToString(m_instance, state.interactionProfile, sizeof(name), &length, name);
		}
		LogInfo << "OpenXR: " << g_handPathNames[hand] << " uses " << name;
	}
	
}