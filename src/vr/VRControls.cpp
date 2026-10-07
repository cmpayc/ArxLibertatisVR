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
#include "game/Inventory.h"
#include "game/Player.h"
#include "game/Camera.h"
#include "game/EntityManager.h"
#include "game/Equipment.h"
#include "game/Spells.h"
#include "game/magic/Precast.h"
#include "game/magic/SpellData.h"
#include "graphics/Draw.h"
#include "graphics/Math.h"
#include "graphics/Raycast.h"
#include "physics/Collisions.h"
#include "scene/GameSound.h"
#include "graphics/Renderer.h"
#include "graphics/data/Mesh.h"
#include "graphics/data/TextureContainer.h"
#include "gui/Cursor.h"
#include "gui/book/Book.h"
#include "gui/Interface.h"
#include "gui/Menu.h"
#include "gui/hud/SecondaryInventory.h"
#include "input/Input.h"
#include "input/InputKey.h"
#include "input/Keyboard.h"
#include "input/Mouse.h"
#include "math/Angle.h"
#include "vr/VRBody.h"
#include "vr/VRCamera.h"
#include "vr/VRCombat.h"
#include "vr/VRGrab.h"
#include "vr/VRMath.h"
#include "vr/VRSystem.h"

extern long LOOKING_FOR_SPELL_TARGET; // see Spells.cpp

namespace {

float g_headYaw = 0.f; // world yaw of the head
float g_pendingTurn = 0.f;
bool g_snapTurnReady = true;
std::vector<InputKeyId> g_pressedKeys;
int g_weaponToggleFrames = 0; // Frames left of the ready weapon action from the sheath
bool g_sneaking = false; // Toggled with the off stick click
float g_bookHoldTime = -1.f; // s the B button is held, negative after a long press opened the spells
int g_bookTapFrames = 0; // Frames left of the book action from a short press of B
bool g_sheatheArmed = false; // The drawn weapon left the sheath: bringing it back sheathes it
VRHand g_weaponHand = VRHandRight; // The hand that drew the weapon (set before the first use)

//! The laser pointer beam, in world space - shown when pointing at a 2D interface, the world ray only for debugging
struct PointerBeam {
	bool visible = false;
	bool hit = false; //!< The beam ends on something: draw a dot there
	Vec3f start = Vec3f(0.f);
	Vec3f end = Vec3f(0.f);
	TextureContainer * icon = nullptr; //!< Shown at the end (the item selected to combine)
	bool spellTarget = false; //!< The game waits for the target of a spell: its target cursor at the end
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
 * \return how far an interactive object is from a ray (offset / distance along the ray), or a negative value if it is
 *         farther than maxAngle (tangent) or minRadius (cm), out of reach or behind a wall
 */
float getAimOffset(const Entity & entity, const Vec3f & origin, const Vec3f & direction, float maxAngle,
                   float minRadius) {
	
	const float Range = 360.f; // cm, the pick range of the game
	
	if(&entity == entities.player() || entity.show != SHOW_FLAG_IN_SCENE || !entity.obj
	   || !(entity.gameFlags & GFLAG_INTERACTIVITY) || !(entity.gameFlags & GFLAG_ISINTREATZONE)
	   || !(entity.ioflags & (IO_ITEM | IO_FIX | IO_NPC))) {
		return -1.f;
	}
	
	const EERIE_3D_BBOX & bbox = entity.bbox3D;
	Vec3f center = (bbox.min.x <= bbox.max.x) ? (bbox.min + bbox.max) * 0.5f : entity.pos;
	Vec3f toCenter = center - origin;
	float along = glm::dot(toCenter, direction);
	if(along < 5.f || along > Range) {
		return -1.f;
	}
	float offset = glm::length(toCenter - direction * along);
	if(offset > std::max(minRadius, along * maxAngle)) {
		return -1.f;
	}
	
	// Not behind a wall
	if(RaycastResult wall = raycastScene(origin, center, POLY_TRANS, RaycastIgnorePlayer)) {
		if(glm::distance(origin, wall.pos) < along - 20.f) {
			return -1.f;
		}
	}
	
	return offset / along;
}

/*!
 * Find the interactive object that the hand points at most closely: within about 8 degrees or 15 cm of the ray,
 * in reach and not behind a wall
 */
Entity * findVRAimAssistTarget(const Vec3f & origin, const Vec3f & direction) {
	
	const float MaxAngle = 0.14f; // tan(8 degrees)
	const float MinRadius = 15.f; // cm
	
	Entity * best = nullptr;
	float bestScore = 0.f;
	for(Entity & entity : entities) {
		float score = getAimOffset(entity, origin, direction, MaxAngle, MinRadius);
		if(score >= 0.f && (!best || score < bestScore)) {
			best = &entity;
			bestScore = score;
		}
	}
	
	return best;
}

EntityHandle g_aimTargets[VRHandCount]; // The last object each hand pointed at

/*!
 * Keep pointing at the same object until the ray moves clearly away from it (about 16 degrees or 25 cm), so that the
 * target does not flicker between objects or drop at the edge of the aim assist. Pointing directly at another object
 * switches to it at once.
 */
Entity * keepVRAimTarget(const VRPose & aim, Entity * target, bool direct, const Vec3f & origin,
                         const Vec3f & direction) {
	
	if(!g_vr) {
		return target;
	}
	size_t hand = VRHandCount;
	for(size_t i = 0; i < VRHandCount; i++) {
		if(&aim == &g_vr->getInput().getHand(VRHand(i)).aim) {
			hand = i;
		}
	}
	if(hand == VRHandCount) {
		return target;
	}
	
	const float KeepAngle = 0.29f; // tan(16 degrees)
	const float KeepRadius = 25.f; // cm
	Entity * previous = entities.get(g_aimTargets[hand]);
	if(!direct && previous && previous != target
	   && getAimOffset(*previous, origin, direction, KeepAngle, KeepRadius) >= 0.f) {
		target = previous;
	}
	
	g_aimTargets[hand] = target ? target->index() : EntityHandle();
	return target;
}


/*!
 * Stored spells are cast with the main hand: a fist (grip and trigger) moved up casts the first one, to the left the
 * second one, to the right the third one (like their icons on the back of the hand, see VRSystem). Moving forward is
 * a punch and casts nothing. Not in the magic mode, where the grip stores the spell drawn instead.
 */
bool g_castGesture = false; // The main hand makes a fist that may cast a stored spell
bool g_castGestureDone = false;
Vec3f g_castGestureStart(0.f);

void updatePrecastGestures() {

	const float GestureDistance = 0.18f; // m the fist moves
	const float Dominance = 1.5f; // How much more than the other directions it moves in the gesture direction

	VRHand hand = dominantHand();
	const VRHandState & state = g_vr->getInput().getHand(hand);
	bool fist = state.isPressed(VRButtonGrip) && state.isPressed(VRButtonTrigger) && state.grip.valid;
	if(!fist || g_precast.empty() || g_vr->isMagicMode() || ARXmenu.mode() != Mode_InGame || BLOCK_PLAYER_CONTROLS
	   || isVRArcheryActive() || isVRGrabbing() || g_vr->isPointingAtUI()) {
		g_castGesture = false;
		g_castGestureDone = false;
		return;
	}
	if(!g_castGesture) {
		g_castGesture = true;
		g_castGestureDone = false;
		g_castGestureStart = state.grip.position;
		return;
	}
	if(g_castGestureDone) {
		return; // One spell per fist
	}

	// In the directions of the head, horizontally (tracking space: y up)
	const XrQuaternionf & q = g_vr->getView(0).pose.orientation;
	Vec3f right = glm::quat(q.w, q.x, q.y, q.z) * Vec3f(1.f, 0.f, 0.f);
	right.y = 0.f;
	if(glm::length(right) < 0.01f) {
		return;
	}
	right = glm::normalize(right);
	Vec3f up(0.f, 1.f, 0.f);
	Vec3f forward = glm::cross(up, right);
	Vec3f moved = state.grip.position - g_castGestureStart;
	float x = glm::dot(moved, right);
	float y = glm::dot(moved, up);
	float z = std::abs(glm::dot(moved, forward));

	size_t slot = size_t(-1);
	if(y > GestureDistance && y > Dominance * std::max(std::abs(x), z)) {
		slot = 0;
	} else if(std::abs(x) > GestureDistance && std::abs(x) > Dominance * std::max(std::abs(y), z)) {
		slot = (x < 0.f) ? 1 : 2;
	}
	if(slot == size_t(-1)) {
		return;
	}

	g_castGestureDone = true;
	if(slot < g_precast.size()) {
		ARX_SPELLS_Precast_Launch(PrecastHandle(slot));
		g_vr->getInput().vibrate(hand, 0.8f, 0.12f);
	} else {
		g_vr->getInput().vibrate(hand, 0.2f, 0.04f); // No spell stored there
	}
}

/*!
 * The potion wheel: the off trigger alone opens it where the hand is (with the grip it is a fist, which punches, see
 * VRCombat). Moving the hand picks a part (the health potion to the left, the mana potion to the right, the antidote
 * up, cancel down), releasing the trigger drinks the potion picked, like the game's potion keys. The grip cancels it.
 * While the main hand points at a panel (inventory) the off trigger is the game's use action instead.
 */
struct ItemWheelSlot {
	std::string_view className; //!< Empty: cancel
	Vec2f direction; //!< Right and up, as seen from the head
};
const ItemWheelSlot g_wheelSlots[] = {
	{ "potion_life", Vec2f(-1.f, 0.f) },
	{ "potion_mana", Vec2f(1.f, 0.f) },
	{ "potion_purple", Vec2f(0.f, 1.f) },
	{ "", Vec2f(0.f, -1.f) },
};
bool g_wheelOpen = false;
Vec3f g_wheelCenter(0.f); // Where the hand was when the wheel opened, in the tracking space
int g_wheelSelected = -1;

void updateItemWheel() {

	const float DeadZone = 0.04f; // m the hand moves before a part is picked

	const VRHand hand = offHand();
	const VRHandState & state = g_vr->getInput().getHand(hand);
	bool trigger = state.isPressed(VRButtonTrigger) && !state.isPressed(VRButtonGrip) && state.grip.valid;
	bool weaponInHand = (player.Interface & INTER_COMBATMODE) && ARX_EQUIPMENT_GetPlayerWeaponType() != WEAPON_BARE
	                    && getVRWeaponHand() == hand;
	bool usable = ARXmenu.mode() == Mode_InGame && !BLOCK_PLAYER_CONTROLS && !isVROffHandGrabbing()
	              && !isVRArcheryActive() && !weaponInHand;

	if(!g_wheelOpen) {
		if(trigger && usable && state.wasPressed(VRButtonTrigger) && !g_vr->isPointingAtUI()) {
			g_wheelOpen = true;
			g_wheelCenter = state.grip.position;
			g_wheelSelected = -1;
			g_vr->getInput().vibrate(hand, 0.3f, 0.04f);
		}
		return;
	}

	if(!trigger || !usable) {
		g_wheelOpen = false;
		// Released: drink the potion picked (the grip cancels)
		if(usable && !state.isPressed(VRButtonGrip) && g_wheelSelected >= 0 && !g_wheelSlots[g_wheelSelected].className.empty()) {
			std::string_view className = g_wheelSlots[g_wheelSelected].className;
			bool have = getInventoryItemWithLowestDurability(className) != nullptr;
			if(have) {
				useInventoryItemWithLowestDurability(className);
			}
			g_vr->getInput().vibrate(hand, have ? 0.6f : 0.15f, have ? 0.1f : 0.04f);
		}
		g_wheelSelected = -1;
		return;
	}

	// The part in the direction the hand moved, as seen from the head (tracking space: y up)
	const XrQuaternionf & q = g_vr->getView(0).pose.orientation;
	glm::quat head(q.w, q.x, q.y, q.z);
	Vec3f moved = state.grip.position - g_wheelCenter;
	Vec2f offset(glm::dot(moved, head * Vec3f(1.f, 0.f, 0.f)), glm::dot(moved, head * Vec3f(0.f, 1.f, 0.f)));
	int selected = -1;
	if(glm::length(offset) > DeadZone) {
		float best = -2.f;
		for(size_t i = 0; i < std::size(g_wheelSlots); i++) {
			float alignment = glm::dot(g_wheelSlots[i].direction, glm::normalize(offset));
			if(alignment > best) {
				best = alignment;
				selected = int(i);
			}
		}
	}
	if(selected != g_wheelSelected) {
		g_wheelSelected = selected;
		if(selected >= 0) {
			g_vr->getInput().vibrate(hand, 0.2f, 0.02f);
		}
	}
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

	updatePrecastGestures();
	updateItemWheel();

	/*
	 * Buttons are mapped to the keys bound to the game actions, so they also work in places that read keys.
	 * Main hand (right): A = jump, B = book, stick click = crouch toggle, trigger = magic mode,
	 *                    grip = take, grab and attack (left click, see the laser pointer in VRSystem)
	 * Off hand (left):   X = inventory, Y = game menu, stick click = sneak toggle,
	 *                    trigger = potion wheel (use on panels), grip = right click
	 */
	std::vector<InputKeyId> keys;
	auto press = [&keys](ControlAction action, bool pressed) {
		InputKeyId key = getActionKey(action);
		if(pressed && key >= 0) {
			keys.push_back(key);
		}
	};
	press(CONTROLS_CUST_JUMP, main.isPressed(VRButtonPrimary));
	press(CONTROLS_CUST_INVENTORY, off.isPressed(VRButtonPrimary));
	press(CONTROLS_CUST_CROUCHTOGGLE, main.isPressed(VRButtonStick));
	// B: a short press opens or closes the book, held it opens the spells page of the book
	const float BookHoldTime = 0.5f; // s
	if(main.isPressed(VRButtonSecondary)) {
		if(g_bookHoldTime >= 0.f) {
			g_bookHoldTime += g_vr->getFrameDuration();
			if(g_bookHoldTime >= BookHoldTime) {
				g_bookHoldTime = -1.f;
				if(player.rune_flags && ARXmenu.mode() == Mode_InGame) {
					g_playerBook.openPage(BOOKMODE_SPELLS);
					g_vr->getInput().vibrate(dominantHand(), 0.5f, 0.06f);
				}
			}
		}
	} else {
		if(g_bookHoldTime >= 0.f && main.wasReleased(VRButtonSecondary)) {
			g_bookTapFrames = 3;
		}
		g_bookHoldTime = 0.f;
	}
	press(CONTROLS_CUST_BOOK, g_bookTapFrames > 0);
	if(g_bookTapFrames > 0) {
		g_bookTapFrames--;
	}
	/*
	 * Grabbing the weapon at the sheath (on the back, over the shoulder of the hand) with either hand draws it into that
	 * hand: hold the action, a key release only counts after two frames. A drawn weapon is sheathed by just bringing it
	 * back over the shoulder, once it has left the sheath.
	 */
	bool toggleWeapon = false;
	if(!(player.Interface & INTER_COMBATMODE)) {
		g_sheatheArmed = false;
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
	press(CONTROLS_CUST_WEAPON, g_weaponToggleFrames > 0);
	// Off stick click: sneaking on or off (like holding the stealth key, see isVRSneaking)
	if(off.wasPressed(VRButtonStick) && ARXmenu.mode() == Mode_InGame) {
		g_sneaking = !g_sneaking;
		g_vr->getInput().vibrate(offHand(), g_sneaking ? 0.5f : 0.25f, g_sneaking ? 0.1f : 0.05f);
	}
	if(g_weaponToggleFrames > 0) {
		g_weaponToggleFrames--;
	}
	// While the off hand holds an object its trigger stores it instead (see VRGrab)
	// A fist (grip and trigger) punches instead (see VRCombat)
	// In the world the off trigger opens the potion wheel instead (see updateItemWheel)
	press(CONTROLS_CUST_USE, (off.isPressed(VRButtonTrigger) && !isVROffHandGrabbing() && !isVRFist(offHand())
	                          && !g_wheelOpen && !isVRArcheryActive())
	                         || g_vr->isUseTapPressed());
	// While holding an object the trigger stores it instead (see VRGrab)
	/*
	 * Magic: a tap of the main trigger in the world toggles the magic mode, in it the trigger held draws a rune with the
	 * hand (see VRSystem::updatePointer). Leaving the mode casts the spell (the game casts when its magic key is
	 * released).
	 */
	press(CONTROLS_CUST_MAGICMODE, g_vr->isMagicMode());
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
	if(hand == dominantHand() && g_vr->isMagicMode()) {
		return false; // The grip and trigger draw a stored spell (see VRSystem::updatePointer)
	}
	const VRHandState & state = g_vr->getInput().getHand(hand);
	return state.isPressed(VRButtonGrip) && state.isPressed(VRButtonTrigger);
}

bool isVRHandAtSheath(VRHand hand) {
	return getSheathDistance(hand) < SheathRadius || isVRHandAtBeltTorch(hand);
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
	bool showRay = config.vr.debugRay || COMBINE || LOOKING_FOR_SPELL_TARGET;
	if(showRay) {
		g_beam.visible = true;
		g_beam.hit = false;
		g_beam.start = start;
		g_beam.end = origin + direction * 200.f;
		g_beam.icon = COMBINE ? COMBINE->m_icon : nullptr;
		g_beam.spellTarget = LOOKING_FOR_SPELL_TARGET != 0;
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
	
	// Aim assist: without a direct hit on an interactive object, take the one closest to the ray (kept while near)
	bool direct = hitEntity && (hitEntity->gameFlags & GFLAG_INTERACTIVITY);
	Entity * assisted = keepVRAimTarget(aim, direct ? hitEntity : findVRAimAssistTarget(origin, direction), direct,
	                                    origin, direction);
	if(!direct && assisted) {
		const EERIE_3D_BBOX & bbox = assisted->bbox3D;
		target = (bbox.min.x <= bbox.max.x) ? (bbox.min + bbox.max) * 0.5f : assisted->pos;
		hitSomething = true;
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
	g_beam.spellTarget = false;
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
	if(glm::length(side) > 0.001f && !beam.icon && !beam.spellTarget) {
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
	
	// The game's target cursor of a spell waiting for its target: lit over a valid target, dim elsewhere
	if(beam.spellTarget) {
		bool onTarget = isVRSpellTargetValid();
		if(TextureContainer * cursor = cursorCombineTexture(onTarget)) {
			Vec3f center = beam.end - direction * 5.f;
			float size = 2.f + 0.03f * glm::distance(eye, center);
			Vec3f right = Vec3f(g_preparedCamera.m_viewToWorld[0]) * size;
			Vec3f down = Vec3f(g_preparedCamera.m_viewToWorld[1]) * size;
			TexturedVertex quad[4];
			worldToClipSpace(center - right - down, quad[0]);
			worldToClipSpace(center + right - down, quad[1]);
			worldToClipSpace(center - right + down, quad[2]);
			worldToClipSpace(center + right + down, quad[3]);
			quad[0].uv = Vec2f(0.f, 0.f);
			quad[1].uv = Vec2f(cursor->uv.x, 0.f);
			quad[2].uv = Vec2f(0.f, cursor->uv.y);
			quad[3].uv = cursor->uv;
			Color color = onTarget ? Color::white : Color(160, 160, 160);
			quad[0].color = quad[1].color = quad[2].color = quad[3].color = color.toRGBA();
			UseRenderState cursorState(render3D().depthWrite(false).blend());
			GRenderer->SetTexture(0, cursor);
			EERIEDRAWPRIM(Renderer::TriangleStrip, quad, 4);
			GRenderer->ResetTexture(0);
		}
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
	bool direct = (result != nullptr);
	result = keepVRAimTarget(aim, direct ? result : findVRAimAssistTarget(origin, direction), direct,
	                         origin, direction);
	if(!direct) {
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

bool isVRPrecastRequested() {
	return g_vr && g_vr->isMagicPrecast();
}

bool getVRSpellAim(Anglef & angle) {
	
	// Along the pointing ray of the casting (main) hand
	if(!isVRActive() || !isVRPlayerView()) {
		return false;
	}
	const VRPose & aim = g_vr->getInput().getHand(dominantHand()).aim;
	Vec3f origin;
	Vec3f direction;
	if(!aim.valid || !trackingRayToWorld(aim.position, aim.orientation, origin, direction)
	   || glm::length(Vec2f(direction.x, direction.z)) < 0.01f) {
		return false;
	}
	
	angle = unitVectorToAngle(glm::normalize(direction));
	return true;
}

bool isVRSpellTargetValid() {
	// Like the game's target cursor (SelectSpellTargetCursorRender): NPCs, or items for enchant weapon
	return LOOKING_FOR_SPELL_TARGET && FlyingOverIO
	       && (((LOOKING_FOR_SPELL_TARGET & 1) && (FlyingOverIO->ioflags & IO_NPC))
	           || ((LOOKING_FOR_SPELL_TARGET & 2) && (FlyingOverIO->ioflags & IO_ITEM)));
}

bool endVRLastSpell() {
	
	// Like the game's cancel spell key: the latest spell of the player that lasts
	Spell * last = nullptr;
	for(Spell & spell : spells.byCaster(EntityHandle_Player)) {
		if(spellicons[spell.m_type].m_hasDuration && (!last || last->m_timcreation < spell.m_timcreation)) {
			last = &spell;
		}
	}
	if(!last) {
		return false;
	}
	ARX_SOUND_PlaySFX(g_snd.MAGIC_FIZZLE);
	spells.endSpell(last);
	return true;
}

bool isVRSneaking() {
	return g_sneaking && ARXmenu.mode() == Mode_InGame;
}

bool isVRItemWheelOpen() {
	return g_wheelOpen;
}

void drawVRItemWheel() {
	
	Vec3f center;
	if(!g_wheelOpen || !isVRPlayerView() || !trackingPointToWorld(g_wheelCenter, center)) {
		return;
	}
	
	// Facing the eyes, the parts around the hand: the picked one bright, potions not carried dim
	const float Radius = 7.f; // cm
	const float Size = 2.6f; // cm, half the width of a part
	const Vec3f right = Vec3f(g_preparedCamera.m_viewToWorld[0]);
	const Vec3f down = Vec3f(g_preparedCamera.m_viewToWorld[1]);
	const Vec3f toEye = -Vec3f(g_preparedCamera.m_viewToWorld[2]);
	
	UseRenderState state(render3D().depthWrite(false).blend());
	auto drawQuad = [&](TextureContainer * texture, const Vec3f & middle, float size, Color color) {
		TexturedVertex quad[4];
		worldToClipSpace(middle - (right + down) * size, quad[0]);
		worldToClipSpace(middle + (right - down) * size, quad[1]);
		worldToClipSpace(middle + (down - right) * size, quad[2]);
		worldToClipSpace(middle + (right + down) * size, quad[3]);
		Vec2f uv = texture ? texture->uv : Vec2f(1.f);
		quad[0].uv = Vec2f(0.f, 0.f);
		quad[1].uv = Vec2f(uv.x, 0.f);
		quad[2].uv = Vec2f(0.f, uv.y);
		quad[3].uv = uv;
		quad[0].color = quad[1].color = quad[2].color = quad[3].color = color.toRGBA();
		if(texture) {
			GRenderer->SetTexture(0, texture);
		} else {
			GRenderer->ResetTexture(0);
		}
		EERIEDRAWPRIM(Renderer::TriangleStrip, quad, 4);
	};
	
	for(size_t i = 0; i < std::size(g_wheelSlots); i++) {
		const ItemWheelSlot & slot = g_wheelSlots[i];
		bool selected = (int(i) == g_wheelSelected);
		Vec3f middle = center + (right * slot.direction.x - down * slot.direction.y) * Radius;
		bool cancel = slot.className.empty();
		Entity * item = cancel ? nullptr : getInventoryItemWithLowestDurability(slot.className);
		Color background = selected ? (cancel ? Color(150, 40, 30, 210) : Color(170, 140, 70, 210))
		                            : Color(15, 15, 25, 150);
		drawQuad(nullptr, middle, Size, background);
		if(cancel) {
			if(TextureContainer * circle = cursorCombineTexture(false)) {
				drawQuad(circle, middle + toEye * 0.1f, Size * 0.7f, selected ? Color::white : Color(170, 170, 170));
			}
		} else if(item && item->m_icon) {
			drawQuad(item->m_icon, middle + toEye * 0.1f, Size * 0.85f, Color::white);
		}
	}
	GRenderer->ResetTexture(0);
}
