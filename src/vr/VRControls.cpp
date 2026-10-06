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
#include "game/EntityManager.h"
#include "game/Equipment.h"
#include "game/Inventory.h"
#include "graphics/Draw.h"
#include "graphics/Raycast.h"
#include "physics/Collisions.h"
#include "graphics/Renderer.h"
#include "graphics/data/Mesh.h"
#include "graphics/data/TextureContainer.h"
#include "gui/Cursor.h"
#include "gui/Interface.h"
#include "gui/Menu.h"
#include "gui/hud/SecondaryInventory.h"
#include "input/Input.h"
#include "input/InputKey.h"
#include "input/Keyboard.h"
#include "input/Mouse.h"
#include "io/log/Logger.h"
#include "io/resource/PakReader.h"
#include "io/resource/ResourcePath.h"
#include "scene/Interactive.h"
#include "script/Script.h"
#include "vr/VRBody.h"
#include "vr/VRCamera.h"
#include "vr/VRCombat.h"
#include "vr/VRGrab.h"
#include "vr/VRMath.h"
#include "vr/VRSystem.h"


namespace {

float g_headYaw = 0.f; // world yaw of the head
float g_pendingTurn = 0.f;
bool g_snapTurnReady = true;
std::vector<InputKeyId> g_pressedKeys;
int g_weaponToggleFrames = 0; // Frames left of the ready weapon action from the sheath
bool g_sheatheArmed = false; // The drawn weapon left the sheath: bringing it back sheathes it
VRHand g_weaponHand = VRHandRight; // The hand that drew the weapon (set before the first use)

//! The laser pointer beam, in world space - shown when pointing at a 2D interface, the world ray only for debugging
struct PointerBeam {
	bool visible = false;
	bool hit = false; //!< The beam ends on something: draw a dot there
	Vec3f start = Vec3f(0.f);
	Vec3f end = Vec3f(0.f);
	TextureContainer * icon = nullptr; //!< Shown at the end (the item selected to combine)
};
PointerBeam g_beam;
PointerBeam g_offHandBeam; //!< Only for debugging

const float StickDeadZone = 0.2f;

VRHand dominantHand() {
	return config.vr.leftHanded ? VRHandLeft : VRHandRight;
}

VRHand offHand() {
	return config.vr.leftHanded ? VRHandRight : VRHandLeft;
}

const float SheathRadius = 30.f; // cm, at the model's size

/*!
 * \return the distance from a hand to the sheath, in cm at the model's size, or a large value if the weapon cannot be
 *         drawn or sheathed with it
 *
 * The sheath is on the back over the shoulder of the hand, where the game attaches the sheathed weapon, see
 * \ref getVRSheathHilt.
 */
float getSheathDistance(VRHand hand) {

	const float Far = 1e6f;
	bool holding = (hand == dominantHand()) ? isVRGrabbing() : isVROffHandGrabbing();
	if(!g_vr || ARXmenu.mode() != Mode_InGame || BLOCK_PLAYER_CONTROLS || holding
	   || !entities.get(player.equiped[EQUIP_SLOT_WEAPON])) {
		return Far;
	}

	Vec3f wrist;
	glm::quat grip;
	Vec3f hilt;
	if(!getVRWristPose(hand, wrist, grip) || !getVRSheathHilt(hand, hilt)) {
		return Far;
	}

	return glm::distance(wrist, hilt) / getVRBodyScale();
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

/*!
 * Find the interactive object that the hand points at most closely: within about 8 degrees or 15 cm of the ray,
 * in reach and not behind a wall
 */
Entity * findVRAimAssistTarget(const Vec3f & origin, const Vec3f & direction) {
	
	const float MaxAngle = 0.14f; // tan(8 degrees)
	const float MinRadius = 15.f; // cm
	const float Range = 360.f;    // cm, the pick range of the game
	
	Entity * best = nullptr;
	float bestScore = 0.f;
	for(Entity & entity : entities) {
		
		if(&entity == entities.player() || entity.show != SHOW_FLAG_IN_SCENE || !entity.obj
		   || !(entity.gameFlags & GFLAG_INTERACTIVITY) || !(entity.gameFlags & GFLAG_ISINTREATZONE)
		   || !(entity.ioflags & (IO_ITEM | IO_FIX | IO_NPC))) {
			continue;
		}
		
		const EERIE_3D_BBOX & bbox = entity.bbox3D;
		Vec3f center = (bbox.min.x <= bbox.max.x) ? (bbox.min + bbox.max) * 0.5f : entity.pos;
		Vec3f toCenter = center - origin;
		float along = glm::dot(toCenter, direction);
		if(along < 5.f || along > Range) {
			continue;
		}
		float offset = glm::length(toCenter - direction * along);
		if(offset > std::max(MinRadius, along * MaxAngle)) {
			continue;
		}
		
		float score = offset / along;
		if(best && score >= bestScore) {
			continue;
		}
		
		// Not behind a wall
		if(RaycastResult wall = raycastScene(origin, center, POLY_TRANS, RaycastIgnorePlayer)) {
			if(glm::distance(origin, wall.pos) < along - 20.f) {
				continue;
			}
		}
		
		best = &entity;
		bestScore = score;
	}
	
	return best;
}

} // anonymous namespace

bool isVRActive() {
	return g_vr && g_vr->hasSession();
}

void setVRHeadYaw(float yaw) {
	g_headYaw = yaw;
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
	 * Main hand (right): A = jump, B = inventory, stick click = crouch toggle, trigger = magic mode,
	 *                    grip = take, grab and attack (left click, see the laser pointer in VRSystem)
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
	/*
	 * Grabbing the weapon at the sheath (on the back, over the shoulder of the hand) with either hand draws it into that
	 * hand: hold the action, a key release only counts after two frames. A drawn weapon is sheathed by just bringing it
	 * back over the shoulder, once it has left the sheath.
	 */
	bool toggleWeapon = false;
	if(!(player.Interface & INTER_COMBATMODE)) {
		g_sheatheArmed = false;
		if(off.wasPressed(VRButtonStick)) {
			g_weaponHand = dominantHand(); // The ready weapon button draws into the main hand
		}
		for(VRHand hand : { dominantHand(), offHand() }) {
			if(input.getHand(hand).wasPressed(VRButtonGrip) && getSheathDistance(hand) < SheathRadius) {
				g_weaponHand = hand;
				toggleWeapon = true;
				break;
			}
		}
	} else if(float sheathDistance = getSheathDistance(g_weaponHand); sheathDistance > SheathRadius + 10.f) {
		g_sheatheArmed = true;
	} else if(g_sheatheArmed && getSheathDistance(g_weaponHand) < SheathRadius) {
		g_sheatheArmed = false;
		toggleWeapon = true;
	}
	if(toggleWeapon && g_weaponToggleFrames == 0) {
		g_weaponToggleFrames = 3;
		g_vr->getInput().vibrate(g_weaponHand, 0.6f, 0.08f);
	}
	press(CONTROLS_CUST_WEAPON, off.isPressed(VRButtonStick) || g_weaponToggleFrames > 0);
	if(g_weaponToggleFrames > 0) {
		g_weaponToggleFrames--;
	}
	// While the off hand holds an object its trigger stores it instead (see VRGrab)
	// A fist (grip and trigger) punches instead (see VRCombat)
	press(CONTROLS_CUST_USE, (off.isPressed(VRButtonTrigger) && !isVROffHandGrabbing() && !isVRFist(offHand())
	                          && !isVRArcheryActive())
	                         || g_vr->isUseTapPressed());
	// While holding an object the trigger stores it instead (see VRGrab)
	press(CONTROLS_CUST_MAGICMODE, main.isPressed(VRButtonTrigger) && !g_vr->isTriggerClicking()
	                               && !g_vr->isPointingAtUI() && !isVRGrabbing() && !isVRFist(dominantHand())
	                               && !isVRArcheryActive()); // The bow uses the triggers (see VRCombat)
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

bool getVRLocomotion(Vec2f & stick, float & yaw) {

	if(!g_vr) {
		return false;
	}

	const VRHandState & off = g_vr->getInput().getHand(offHand());
	stick = applyDeadZone(off.thumbstick);
	if(stick == Vec2f(0.f)) {
		return false;
	}

	yaw = g_headYaw;

	if(config.vr.handOrientedMovement && off.aim.valid) {
		// Move towards where the off hand points: its yaw relative to the head in the tracking space
		const XrQuaternionf & head = g_vr->getView(0).pose.orientation;
		float headYaw = yawDegrees(glm::quat(head.w, head.x, head.y, head.z));
		yaw += vr::wrapDegrees(yawDegrees(off.aim.orientation) - headYaw);
	}

	return true;
}

float takeVRTurn() {
	float turn = g_pendingTurn;
	g_pendingTurn = 0.f;
	return turn;
}

bool isVRFist(VRHand hand) {
	if(!g_vr) {
		return false;
	}
	const VRHandState & state = g_vr->getInput().getHand(hand);
	return state.isPressed(VRButtonGrip) && state.isPressed(VRButtonTrigger);
}

bool isVRHandAtSheath(VRHand hand) {
	return getSheathDistance(hand) < SheathRadius;
}

VRHand getVRWeaponHand() {
	return g_weaponHand;
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
	
	// The world ray is invisible, except for debugging and while an item is selected to combine (with its icon)
	const Vec3f start = origin;
	bool showRay = config.vr.debugRay || COMBINE;
	if(showRay) {
		g_beam.visible = true;
		g_beam.hit = false;
		g_beam.start = start;
		g_beam.end = origin + direction * 200.f;
		g_beam.icon = COMBINE ? COMBINE->m_icon : nullptr;
	}
	
	// Start a little in front of the controller so that the player's own hand and weapon are not hit
	origin += direction * 10.f;
	const float range = 1000.f;
	Vec3f target = origin + direction * range;
	
	bool hitSomething = false;
	if(RaycastResult hit = raycastScene(origin, target, POLY_TRANS, RaycastIgnorePlayer)) {
		target = hit.pos;
		hitSomething = true;
	}
	Entity * hitEntity = nullptr;
	if(EntityRaycastResult hit = raycastEntities(origin, target, POLY_TRANS, RaycastIgnorePlayer)) {
		target = hit.pos;
		hitSomething = true;
		hitEntity = hit.entity;
	}
	
	// Aim assist: without a direct hit on an interactive object, take the one closest to the ray
	if(!hitEntity || !(hitEntity->gameFlags & GFLAG_INTERACTIVITY)) {
		if(Entity * assisted = findVRAimAssistTarget(origin, direction)) {
			const EERIE_3D_BBOX & bbox = assisted->bbox3D;
			target = (bbox.min.x <= bbox.max.x) ? (bbox.min + bbox.max) * 0.5f : assisted->pos;
			hitSomething = true;
		}
	}
	if(showRay && hitSomething) {
		g_beam.end = target;
		g_beam.hit = true;
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
	g_beam.icon = nullptr;
	g_offHandBeam.visible = false;
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

static void drawPointerBeam(const PointerBeam & beam);

void drawVRPointerBeam() {
	drawPointerBeam(g_beam);
	drawPointerBeam(g_offHandBeam);
}

static void drawPointerBeam(const PointerBeam & beam) {

	if(!beam.visible || beam.start == beam.end) {
		return;
	}

	const Vec3f eye = Vec3f(g_preparedCamera.m_viewToWorld[3]);
	const Vec3f direction = glm::normalize(beam.end - beam.start);
	
	UseRenderState state(render3D().depthWrite(false).blendAdditive());
	GRenderer->ResetTexture(0);
	
	// A thin ribbon facing the viewer, brighter at the hand (not while combining: only the item icon is shown)
	Vec3f toEye = eye - beam.start;
	Vec3f side = glm::cross(direction, toEye);
	if(glm::length(side) > 0.001f && !beam.icon) {
		side = glm::normalize(side) * 0.3f;
		const Color startColor = Color::rgb(0.35f, 0.6f, 1.f);
		const Color endColor = Color::rgb(0.1f, 0.2f, 0.4f);
		TexturedVertex ribbon[4];
		worldToClipSpace(beam.start - side, ribbon[0]);
		worldToClipSpace(beam.start + side, ribbon[1]);
		worldToClipSpace(beam.end - side, ribbon[2]);
		worldToClipSpace(beam.end + side, ribbon[3]);
		ribbon[0].color = ribbon[1].color = startColor.toRGBA();
		ribbon[2].color = ribbon[3].color = endColor.toRGBA();
		EERIEDRAWPRIM(Renderer::TriangleStrip, ribbon, 4);
	}
	
	/*
	 * The icon of the item selected to combine at the end, kept about the same apparent size at any distance, with the
	 * game's combine cursor at its top left corner: a circle with a triangle over a target (an interactive object, like
	 * the game's cursor), empty and with a tinted icon elsewhere
	 */
	if(beam.icon) {
		bool onTarget = FlyingOverIO && FlyingOverIO != COMBINE;
		Vec3f center = beam.end - direction * 5.f;
		float size = 2.f + 0.03f * glm::distance(eye, center);
		Vec2f iconSize(beam.icon->m_size);
		float largest = std::max(std::max(iconSize.x, iconSize.y), 1.f);
		Vec3f right = Vec3f(g_preparedCamera.m_viewToWorld[0]) * (size * iconSize.x / largest);
		Vec3f down = Vec3f(g_preparedCamera.m_viewToWorld[1]) * (size * iconSize.y / largest);
		Color color = onTarget ? Color::white : Color(255, 170, 102);
		UseRenderState iconState(render3D().depthWrite(false).blend());
		auto drawQuad = [&](TextureContainer * texture, const Vec3f & middle, const Vec3f & x, const Vec3f & y) {
			TexturedVertex quad[4];
			worldToClipSpace(middle - x - y, quad[0]);
			worldToClipSpace(middle + x - y, quad[1]);
			worldToClipSpace(middle - x + y, quad[2]);
			worldToClipSpace(middle + x + y, quad[3]);
			quad[0].uv = Vec2f(0.f, 0.f);
			quad[1].uv = Vec2f(texture->uv.x, 0.f);
			quad[2].uv = Vec2f(0.f, texture->uv.y);
			quad[3].uv = texture->uv;
			quad[0].color = quad[1].color = quad[2].color = quad[3].color = color.toRGBA();
			GRenderer->SetTexture(0, texture);
			EERIEDRAWPRIM(Renderer::TriangleStrip, quad, 4);
		};
		drawQuad(beam.icon, center, right, down);
		if(TextureContainer * circle = cursorCombineTexture(onTarget)) {
			// The same size relative to the icon as on the screen, centered on its top left corner
			float scale = size * float(circle->m_size.x) / largest;
			Vec3f x = Vec3f(g_preparedCamera.m_viewToWorld[0]) * scale;
			Vec3f y = Vec3f(g_preparedCamera.m_viewToWorld[1]) * scale;
			color = onTarget ? Color::white : Color(160, 160, 160);
			drawQuad(circle, center - right - down - direction * 0.5f, x, y);
		}
		GRenderer->ResetTexture(0);
		return;
	}
	
	// A dot where the beam hits, kept about the same apparent size at any distance
	if(beam.hit) {
		Vec3f center = beam.end - direction * 1.f;
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
void applyVRRoomScale() {
	
	if(!entities.player()) {
		return;
	}
	
	// The game sets the collision radius once; keep it at the VR body size (e.g. after changing the config)
	player.physics.cyl.radius = player.baseRadius();

	Vec3f movement = takeVRRoomScaleMovement();
	Vec3f moved(0.f);
	if(movement != Vec3f(0.f)) {
		if(USE_PLAYERCOLLISIONS) {
			// Move the player cylinder with the same sliding collision as normal movement
			IO_PHYSICS physics = player.physics;
			physics.cyl.origin = player.basePosition();
			physics.startpos = physics.cyl.origin;
			physics.targetpos = physics.startpos + movement;
			ARX_COLLISION_Move_Cylinder(&physics, entities.player(), 40.f, CFLAG_EASY_SLIDING | CFLAG_PLAYER);
			moved = physics.cyl.origin - player.basePosition();
			player.pos = physics.cyl.origin + player.baseOffset();
			player.physics.cyl.origin = physics.cyl.origin;
		} else {
			moved = movement;
			player.pos += movement;
		}
	}
	
	/*
	 * Play the walk animation while physically stepping, unless the thumbstick already moves the player.
	 * Any noticeable movement plays at least about one step, so that even small steps are animated.
	 */
	static float animationTime = 0.f;
	static Vec3f direction(0.f);
	float dt = g_vr ? g_vr->getFrameDuration() : 0.f;
	if(dt > 0.f) {
		Vec3f horizontal(moved.x, 0.f, moved.z);
		if(glm::length(horizontal) / dt > 10.f) {
			animationTime = 0.4f;
			direction = horizontal;
		} else {
			animationTime -= dt;
		}
	}
	
	const PlayerMovement walking = PLAYER_MOVE_WALK_FORWARD | PLAYER_MOVE_WALK_BACKWARD
	                               | PLAYER_MOVE_STRAFE_LEFT | PLAYER_MOVE_STRAFE_RIGHT;
	if(animationTime > 0.f && !(player.m_currentMovement & walking)) {
		Vec3f forward = angleToVectorXZ(player.angle.getYaw());
		Vec3f right = angleToVectorXZ(player.angle.getYaw() - 90.f);
		float alongForward = glm::dot(direction, forward);
		float alongRight = glm::dot(direction, right);
		player.m_currentMovement |= PLAYER_MOVE_STEALTH;
		if(std::abs(alongForward) >= std::abs(alongRight)) {
			player.m_currentMovement |= (alongForward > 0.f) ? PLAYER_MOVE_WALK_FORWARD : PLAYER_MOVE_WALK_BACKWARD;
		} else {
			player.m_currentMovement |= (alongRight > 0.f) ? PLAYER_MOVE_STRAFE_RIGHT : PLAYER_MOVE_STRAFE_LEFT;
		}
	}
	
}

Entity * getVRPointedEntity(const VRPose & aim, bool debugRay) {
	
	Vec3f origin;
	Vec3f direction;
	if(!aim.valid || !trackingRayToWorld(aim.position, aim.orientation, origin, direction)) {
		return nullptr;
	}
	
	// The ray is invisible, except for debugging
	const Vec3f start = origin;
	
	// Start a little in front of the controller so that the player's own hand is not hit
	origin += direction * 10.f;
	Vec3f target = origin + direction * 1000.f;
	bool hitSomething = false;
	if(RaycastResult hit = raycastScene(origin, target, POLY_TRANS, RaycastIgnorePlayer)) {
		target = hit.pos;
		hitSomething = true;
	}
	Entity * result = nullptr;
	if(EntityRaycastResult hit = raycastEntities(origin, target, POLY_TRANS, RaycastIgnorePlayer)) {
		target = hit.pos;
		hitSomething = true;
		if(hit.entity->gameFlags & GFLAG_INTERACTIVITY) {
			result = hit.entity;
		}
	}
	if(!result) {
		result = findVRAimAssistTarget(origin, direction);
		if(result) {
			const EERIE_3D_BBOX & bbox = result->bbox3D;
			target = (bbox.min.x <= bbox.max.x) ? (bbox.min + bbox.max) * 0.5f : result->pos;
			hitSomething = true;
		}
	}
	
	if(debugRay && config.vr.debugRay) {
		g_offHandBeam.visible = true;
		g_offHandBeam.hit = hitSomething;
		g_offHandBeam.start = start;
		g_offHandBeam.end = hitSomething ? target : start + direction * 200.f;
	}
	
	return result;
}

void giveVRTestBow() {
	
	if(!entities.player() || ARXmenu.mode() != Mode_InGame) {
		return;
	}
	
	// The item names are not known here: take every weapon whose folder name contains "bow" or "arrow"
	const res::path weapons = "graph/obj3d/interactive/items/weapons";
	PakDirectory * directory = g_resources->getDirectory(weapons);
	if(!directory) {
		LogWarning << "VR test: no " << weapons;
		return;
	}
	
	for(const auto & entry : directory->dirs()) {
		
		std::string_view name = entry.name;
		bool bow = name.find("bow") != std::string_view::npos;
		bool arrows = name.find("arrow") != std::string_view::npos;
		if(!bow && !arrows) {
			continue;
		}
		
		res::path file = weapons / name / name;
		Entity * item = AddItem(file);
		if(!item) {
			LogWarning << "VR test: could not create " << file;
			continue;
		}
		item->scriptload = 1;
		SendInitScriptEvent(item);
		if(arrows && (item->ioflags & IO_ITEM)) {
			item->_itemdata->maxcount = 9999;
			item->_itemdata->count = 50;
		}
		giveToPlayer(item);
		LogInfo << "VR test: added " << file;
		
	}
	
}
