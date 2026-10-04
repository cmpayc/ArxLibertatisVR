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

#include "vr/VRControls.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <glm/glm.hpp>

#include "core/Config.h"
#include "core/Core.h"
#include "game/Player.h"
#include "game/Camera.h"
#include "graphics/Draw.h"
#include "graphics/Raycast.h"
#include "graphics/Renderer.h"
#include "graphics/data/Mesh.h"
#include "gui/Interface.h"
#include "gui/Menu.h"
#include "gui/hud/SecondaryInventory.h"
#include "input/Input.h"
#include "input/InputKey.h"
#include "input/Keyboard.h"
#include "input/Mouse.h"
#include "vr/VRCamera.h"
#include "vr/VRSystem.h"


namespace {

float g_headYawOffset = 0.f;
float g_pendingTurn = 0.f;
bool g_snapTurnReady = true;
std::vector<InputKeyId> g_pressedKeys;

//! The laser pointer beam, in world space - only shown when pointing at a 2D interface, the world ray is invisible
struct PointerBeam {
	bool visible = false;
	bool hit = false; //!< The beam ends on something: draw a dot there
	Vec3f start = Vec3f(0.f);
	Vec3f end = Vec3f(0.f);
};
PointerBeam g_beam;
const float StickDeadZone = 0.2f;

VRHand dominantHand() {
	return config.vr.leftHanded ? VRHandLeft : VRHandRight;
}

VRHand offHand() {
	return config.vr.leftHanded ? VRHandRight : VRHandLeft;
}

float wrapDegrees(float angle) {
	return std::remainder(angle, 360.f);
}

//! Yaw of an OpenXR orientation in degrees, positive turns left like the Arx convention
float yawDegrees(const glm::quat & orientation) {
	Vec3f forward = orientation * Vec3f(0.f, 0.f, -1.f);
	return glm::degrees(glm::atan(-forward.x, -forward.z));
}

//! Apply a radial dead zone and rescale the remaining range to [0, 1]
Vec2f applyDeadZone(const Vec2f & stick) {
	float length = glm::length(stick);
	if(length <= StickDeadZone) {
		return Vec2f(0.f);
	}
	float scaled = std::min((length - StickDeadZone) / (1.f - StickDeadZone), 1.f);
	return stick * (scaled / length);
}

//! \return the keyboard key bound to a game action, or -1 if it is only bound to the mouse or a key combination
InputKeyId getActionKey(ControlAction action) {
	for(InputKeyId key : config.actions[action].key) {
		if(key >= 0 && !(key & Mouse::ButtonBase) && !(key & Mouse::WheelBase) && !(key & INPUT_COMBINATION_MASK)) {
			return key;
		}
	}
	return -1;
}

} // anonymous namespace

bool isVRActive() {
	return g_vr && g_vr->hasSession();
}

void setVRHeadYawOffset(float yaw) {
	g_headYawOffset = wrapDegrees(yaw);
}

void updateVRControls() {

	if(!g_vr || !GInput) {
		return;
	}

	const VRInput & input = g_vr->getInput();
	const VRHandState & main = input.getHand(dominantHand());
	const VRHandState & off = input.getHand(offHand());

	/*
	 * Buttons are mapped to the keys bound to the game actions, so they also work in places that read keys.
	 * Main hand (right): A = jump, B = inventory, stick click = crouch toggle, grip = magic mode,
	 *                    trigger = left click (see the laser pointer in VRSystem)
	 * Off hand (left):   X = book, Y = game menu, stick click = ready weapon,
	 *                    trigger = use (interaction cursor), grip = right click
	 */
	std::vector<InputKeyId> keys;
	auto press = [&keys](ControlAction action, bool pressed) {
		InputKeyId key = getActionKey(action);
		if(pressed && key >= 0) {
			keys.push_back(key);
		}
	};
	press(CONTROLS_CUST_JUMP, main.isPressed(VRButtonPrimary));
	press(CONTROLS_CUST_INVENTORY, main.isPressed(VRButtonSecondary));
	press(CONTROLS_CUST_CROUCHTOGGLE, main.isPressed(VRButtonStick));
	press(CONTROLS_CUST_BOOK, off.isPressed(VRButtonPrimary));
	press(CONTROLS_CUST_WEAPON, off.isPressed(VRButtonStick));
	press(CONTROLS_CUST_USE, off.isPressed(VRButtonTrigger));
	press(CONTROLS_CUST_MAGICMODE, main.isPressed(VRButtonGrip));
	// The menu button is often reserved by the system (e.g. Quest through Virtual Desktop)
	if(off.isPressed(VRButtonSecondary) || off.isPressed(VRButtonMenu) || main.isPressed(VRButtonMenu)) {
		keys.push_back(Keyboard::Key_Escape);
	}

	for(InputKeyId key : g_pressedKeys) {
		if(std::find(keys.begin(), keys.end(), key) == keys.end()) {
			GInput->setVirtualKey(key, false);
		}
	}
	for(InputKeyId key : keys) {
		GInput->setVirtualKey(key, true);
	}
	g_pressedKeys = keys;

	// Turning with the main hand thumbstick
	float turn = applyDeadZone(Vec2f(main.thumbstick.x, 0.f)).x;
	if(config.vr.snapTurn) {
		if(g_snapTurnReady && std::abs(main.thumbstick.x) > 0.7f) {
			g_pendingTurn += (main.thumbstick.x > 0.f ? -1.f : 1.f) * float(config.vr.snapTurnAngle);
			g_snapTurnReady = false;
		} else if(std::abs(main.thumbstick.x) < 0.3f) {
			g_snapTurnReady = true;
		}
	} else if(turn != 0.f) {
		g_pendingTurn -= turn * config.vr.smoothTurnSpeed * g_vr->getFrameDuration();
	}

}

bool getVRLocomotion(Vec2f & stick, float & yawOffset) {

	if(!g_vr) {
		return false;
	}

	const VRHandState & off = g_vr->getInput().getHand(offHand());
	stick = applyDeadZone(off.thumbstick);
	if(stick == Vec2f(0.f)) {
		return false;
	}

	yawOffset = g_headYawOffset;

	if(config.vr.handOrientedMovement && off.aim.valid) {
		// Move towards where the off hand points: its yaw relative to the head, then relative to the body
		const XrQuaternionf & head = g_vr->getView(0).pose.orientation;
		float headYaw = yawDegrees(glm::quat(head.w, head.x, head.y, head.z));
		yawOffset += wrapDegrees(yawDegrees(off.aim.orientation) - headYaw);
	}

	return true;
}

float takeVRTurn() {
	float turn = g_pendingTurn;
	g_pendingTurn = 0.f;
	return turn;
}

bool isVRInterfaceOpen() {
	
	if(ARXmenu.mode() != Mode_InGame) {
		return true;
	}
	
	return (player.Interface & (INTER_INVENTORY | INTER_INVENTORYALL | INTER_PLAYERBOOK | INTER_NOTE))
	       || g_secondaryInventoryHud.isOpen() || g_note.isOpen();
}

bool getVRWorldPointer(const VRPose & aim, Vec2s & mouse) {
	
	Vec3f origin;
	Vec3f direction;
	if(!aim.valid || !trackingRayToWorld(aim.position, aim.orientation, origin, direction)) {
		return false;
	}
	
	// Start a little in front of the controller so that the player's own hand and weapon are not hit
	origin += direction * 10.f;
	const float range = 1000.f;
	Vec3f target = origin + direction * range;
	
	if(RaycastResult hit = raycastScene(origin, target, POLY_TRANS, RaycastIgnorePlayer)) {
		target = hit.pos;
	}
	if(EntityRaycastResult hit = raycastEntities(origin, target, POLY_TRANS, RaycastIgnorePlayer)) {
		target = hit.pos;
	}	
	// The game picks objects by casting a ray from the camera through the cursor: put the cursor on the target
	Vec2f screen;
	if(!worldToVRScreen(target, screen)) {
		return false;
	}
	if(screen.x < float(g_size.left) || screen.x >= float(g_size.right)
	   || screen.y < float(g_size.top) || screen.y >= float(g_size.bottom)) {
		return false;
	}
	
	mouse = Vec2s(s16(screen.x), s16(screen.y));
	return true;
}
void clearVRPointerBeam() {
	g_beam.visible = false;
}

void setVRPointerBeamToUI(const Vec3f & start, const Vec3f & end) {
	
	Vec3f worldStart;
	Vec3f worldEnd;
	if(!trackingPointToWorld(start, worldStart) || !trackingPointToWorld(end, worldEnd)) {
		g_beam.visible = false;
		return;
	}
	
	g_beam.visible = true;
	g_beam.hit = true;
	g_beam.start = worldStart;
	g_beam.end = worldEnd;
}

void drawVRPointerBeam() {
	
	if(!g_beam.visible || g_beam.start == g_beam.end) {
		return;
	}
	
	const Vec3f eye = Vec3f(g_preparedCamera.m_viewToWorld[3]);
	const Vec3f direction = glm::normalize(g_beam.end - g_beam.start);
	
	UseRenderState state(render3D().depthWrite(false).blendAdditive());
	GRenderer->ResetTexture(0);
	
	// A thin ribbon facing the viewer, brighter at the hand
	Vec3f toEye = eye - g_beam.start;
	Vec3f side = glm::cross(direction, toEye);
	if(glm::length(side) > 0.001f) {
		side = glm::normalize(side) * 0.3f;
		const Color startColor = Color::rgb(0.35f, 0.6f, 1.f);
		const Color endColor = Color::rgb(0.1f, 0.2f, 0.4f);
		TexturedVertex beam[4];
		worldToClipSpace(g_beam.start - side, beam[0]);
		worldToClipSpace(g_beam.start + side, beam[1]);
		worldToClipSpace(g_beam.end - side, beam[2]);
		worldToClipSpace(g_beam.end + side, beam[3]);
		beam[0].color = beam[1].color = startColor.toRGBA();
		beam[2].color = beam[3].color = endColor.toRGBA();
		EERIEDRAWPRIM(Renderer::TriangleStrip, beam, 4);
	}
	
	// A dot where the beam hits, kept about the same apparent size at any distance
	if(g_beam.hit) {
		Vec3f center = g_beam.end - direction * 1.f;
		float size = 0.6f + 0.006f * glm::distance(eye, center);
		Vec3f right = Vec3f(g_preparedCamera.m_viewToWorld[0]) * size;
		Vec3f down = Vec3f(g_preparedCamera.m_viewToWorld[1]) * size;
		TexturedVertex dot[4];
		worldToClipSpace(center - right - down, dot[0]);
		worldToClipSpace(center + right - down, dot[1]);
		worldToClipSpace(center - right + down, dot[2]);
		worldToClipSpace(center + right + down, dot[3]);
		dot[0].color = dot[1].color = dot[2].color = dot[3].color = Color::rgb(0.6f, 0.85f, 1.f).toRGBA();
		EERIEDRAWPRIM(Renderer::TriangleStrip, dot, 4);
	}
	
}