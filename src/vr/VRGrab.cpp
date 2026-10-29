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

#include "vr/VRGrab.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/gtc/quaternion.hpp>

#include "core/Config.h"
#include "core/Core.h"
#include "game/Entity.h"
#include "game/EntityManager.h"
#include "game/Equipment.h"
#include "game/Inventory.h"
#include "game/Player.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/Math.h"
#include "gui/Dragging.h"
#include "gui/Interface.h"
#include "gui/Menu.h"
#include "input/Input.h"
#include "physics/Physics.h"
#include "scene/GameSound.h"
#include "scene/Interactive.h"
#include "script/Script.h"
#include "vr/VRCamera.h"
#include "vr/VRControls.h"
#include "vr/VRSystem.h"


extern long LOOKING_FOR_SPELL_TARGET; // see Spells.cpp
extern Entity * LASTSPAWNED; // see ScriptedIOControl.cpp

namespace {

enum GrabState {
	GrabNone,
	GrabSelected, //!< A distant object, waiting for the hand to be flicked towards the player or upwards
	GrabFlying,   //!< Flying to the hand
	GrabHeld      //!< Attached to the hand
};

//! Grabbing state and motion of one hand
struct GrabHand {

	// Motion of the hand in the world
	bool valid = false;
	Vec3f position = Vec3f(0.f);
	glm::quat orientation = glm::quat(1.f, 0.f, 0.f, 0.f);
	Vec3f velocity = Vec3f(0.f); //!< World units (cm) per second

	// The grabbed entity (a handle: it may be destroyed by a script while held)
	GrabState state = GrabNone;
	EntityHandle entity;
	unsigned long lastFrame = 0;
	glm::quat rotationOffset = glm::quat(1.f, 0.f, 0.f, 0.f); //!< Object rotation relative to the hand
	Vec3f positionOffset = Vec3f(0.f); //!< Object position relative to the hand, in hand space
	Vec3f flightStart = Vec3f(0.f);
	float flightTime = 0.f;

	// Grabbing directly with the grip (not through the game drag): the object pointed at and the grip press on it
	bool direct = false;
	EntityHandle target;
	bool targetTouched = false; //!< The target touches the hand (otherwise it is pointed at)
	EntityHandle pressed;
	float pressTime = 0.f;
	float gripTime = 0.f; //!< Since the grip was pressed (to catch objects)

	// Eating a held item at the mouth
	float mouthTime = 0.f; //!< Since the held item reached the mouth
	bool mouthUsed = false; //!< One was used there: the item must leave the mouth before the next of a stack

	void resetGrab() {
		state = GrabNone;
		direct = false;
		entity = EntityHandle();
		rotationOffset = glm::quat(1.f, 0.f, 0.f, 0.f);
		positionOffset = Vec3f(0.f);
		flightTime = 0.f;
		mouthTime = 0.f;
		mouthUsed = false;
	}

};

GrabHand g_hands[VRHandCount];
unsigned long g_frame = 0;

const float ReachDistance = 60.f;   // cm: objects closer than this to the hand are grabbed directly
const float FlickSpeed = 100.f;     // cm/s towards the player or upwards to pull a selected object
const float FlightDuration = 0.6f;  // s
const float HoldDuration = 0.3f;    // s: holding the grip longer grabs, shorter takes into the inventory
const float MouthDistance = 12.f;   // cm: food or a drink held this close to the mouth is eaten
const float MouthLeave = 20.f;      // cm: and must be taken this far away before the next one of a stack
const float MouthDuration = 0.2f;   // s: held at the mouth this long (not only passing by the face)

VRHand grabMainHand() {
	return config.vr.leftHanded ? VRHandLeft : VRHandRight;
}

VRHand grabOffHand() {
	return config.vr.leftHanded ? VRHandRight : VRHandLeft;
}

//! Rotation of the model of an entity in the world (see setDraggedEntity)
glm::quat getEntityRotation(const Entity & entity) {
	Anglef angle = entity.angle;
	angle.setYaw(270.f - angle.getYaw());
	return toQuaternion(angle);
}

void setEntityRotation(Entity & entity, const glm::quat & rotation) {
	Anglef angle = toAngle(rotation);
	angle.setYaw(270.f - angle.getYaw());
	entity.angle = angle;
}

//! Center of the bounding box of the model of an entity, relative to its position in the world
Vec3f getEntityCenterOffset(const Entity & entity) {
	EERIE_3D_BBOX bbox;
	for(const EERIE_VERTEX & vertex : entity.obj->vertexlist) {
		bbox.add(vertex.v);
	}
	return getEntityRotation(entity) * ((bbox.min + bbox.max) * 0.5f);
}

//! Attach an entity to a hand with its center in the hand, keeping its orientation relative to the hand
void holdEntity(GrabHand & hand, Entity & entity) {
	glm::quat toHand = glm::inverse(hand.orientation);
	hand.positionOffset = toHand * -getEntityCenterOffset(entity);
	hand.rotationOffset = toHand * getEntityRotation(entity);
	hand.state = GrabHeld;
}

/*!
 * Start grabbing: objects within reach go into the hand, distant ones are selected for pulling
 *
 * \param fromInterface Taken out of an inventory or the equipment: put it into the hand
 */
void startGrab(GrabHand & hand, Entity & entity, bool fromInterface = false) {
	hand.resetGrab();
	hand.entity = entity.index();
	if(entity.obj && entity.obj->pbox) {
		entity.obj->pbox->active = 0;
	}
	Vec3f center = entity.pos + getEntityCenterOffset(entity);
	if(entity.show != SHOW_FLAG_IN_SCENE || fromInterface) {
		// Taken out of an inventory: put it into the hand
		entity.show = SHOW_FLAG_IN_SCENE;
		ARX_INTERACTIVE_Teleport(&entity, hand.position - getEntityCenterOffset(entity), true);
		holdEntity(hand, entity);
	} else if(glm::distance(center, hand.position) < ReachDistance) {
		holdEntity(hand, entity);
	} else {
		hand.state = GrabSelected;
	}
}

//! Let go of an entity with a velocity (dropping or throwing it)
void releaseEntity(GrabHand & hand, Entity & entity, const Vec3f & velocity, bool dragged) {

	ARX_PLAYER_Remove_Invisibility();
	entity.soundtime = 0;
	entity.soundcount = 0;
	entity.show = SHOW_FLAG_IN_SCENE;
	entity.gameFlags &= ~GFLAG_NOCOMPUTATION;
	if(dragged) {
		setDraggedEntity(nullptr);
	}

	if(entity.obj && entity.obj->pbox) {
		// The physics boxes are launched with a direction scaled by about 250 (see EERIE_PHYSICS_BOX_Launch)
		Vec3f launch = velocity * (config.vr.throwStrength / 250.f);
		if(glm::length(launch) > 4.f) {
			launch *= 4.f / glm::length(launch);
		}
		if(glm::length(launch) < 0.1f) {
			launch = Vec3f(0.f, 0.1f, 0.f); // Just drop it
		}
		EERIE_PHYSICS_BOX_Launch(entity.obj, entity.pos, entity.angle, launch);
	}

	if(glm::length(velocity) > FlickSpeed) {
		ARX_SOUND_PlaySFX(g_snd.WHOOSH, &entity.pos);
	}

	hand.resetGrab();
}

//! \return true if an entity can be put into the player inventory
bool canStore(const Entity & entity) {
	return (entity.ioflags & IO_ITEM) && !(entity.ioflags & IO_MOVABLE) && entities.player()
	       && entities.player()->inventory;
}

/*!
 * Put an entity into the player inventory
 *
 * \return false if it cannot be stored (movable objects, full inventory)
 */
bool storeEntity(VRHand handId, Entity & entity, bool dragged) {

	if(!canStore(entity)) {
		return false;
	}

	if(dragged) {
		setDraggedEntity(nullptr);
	}
	entity.gameFlags &= ~GFLAG_NOCOMPUTATION;

	if(entity.ioflags & IO_GOLD) {
		ARX_SOUND_PlayInterface(g_snd.GOLD);
		ARX_PLAYER_AddGold(&entity);
	} else if(entities.player()->inventory->insert(&entity)) {
		ARX_SOUND_PlayInterface(g_snd.INVSTD);
	} else {
		// No space
		if(dragged) {
			setDraggedEntity(&entity);
			entity.show = SHOW_FLAG_IN_SCENE;
		}
		return false;
	}

	g_vr->getInput().vibrate(handId, 0.4f, 0.05f);
	g_hands[handId].resetGrab();
	return true;
}

//! \return true if an item is eaten or drunk when used: food, drinks and potions (see their scripts)
bool isConsumable(const Entity & entity) {
	if(!(entity.ioflags & IO_ITEM) || !(entity.gameFlags & GFLAG_INTERACTIVITY)) {
		return false;
	}
	if(entity.groups.find("food") != entity.groups.end()) {
		return true;
	}
	const std::string & name = entity.className();
	// Beer: mug_full, and mug_full2 sold in the tavern
	return name.rfind("mug_full", 0) == 0 || name == "bottle_wine" || name == "bottle_water"
	       || name.rfind("potion_", 0) == 0;
}

//! \return the distance from the center of a held entity to the player's mouth, or a large value if unknown
float getMouthDistance(const Entity & entity) {
	Vec3f eyes;
	glm::mat3x3 head;
	if(!getVRHeadPose(eyes, head)) {
		return std::numeric_limits<float>::max();
	}
	// Below and in front of the eyes (Arx basis: y down, z forward)
	Vec3f mouth = eyes + head * Vec3f(0.f, 9.f, 6.f);
	return glm::distance(entity.pos + getEntityCenterOffset(entity), mouth);
}

/*!
 * Eat or drink a held item, like using it in the inventory: one of a stack. What replaces it (an empty bottle)
 * stays in the hand.
 */
void consumeEntity(VRHand handId, Entity & entity, bool dragged) {

	Entity * lastSpawned = LASTSPAWNED;
	SendIOScriptEvent(entities.player(), &entity, SM_INVENTORYUSE);
	Entity * replacement = (LASTSPAWNED != lastSpawned) ? LASTSPAWNED : nullptr;
	if(replacement && (!(replacement->ioflags & IO_ITEM) || replacement->show != SHOW_FLAG_IN_SCENE)) {
		replacement = nullptr;
	}
	g_vr->getInput().vibrate(handId, 0.4f, 0.05f);

	GrabHand & hand = g_hands[handId];
	hand.mouthUsed = true;
	hand.mouthTime = 0.f;
	bool gone = entity.show != SHOW_FLAG_IN_SCENE || ARX_INTERACTIVE_IsDestroyIOdelayed(&entity);
	if(!gone) {
		// One of a stack: the rest stays in the hand, what replaces the one used goes into the inventory
		if(replacement && canStore(*replacement) && entities.player()->inventory->insert(replacement)) {
			ARX_SOUND_PlayInterface(g_snd.INVSTD);
		}
	} else if(replacement) {
		hand.entity = replacement->index();
		if(dragged) {
			setDraggedEntity(replacement);
		}
	} else {
		if(dragged) {
			setDraggedEntity(nullptr);
		}
		hand.resetGrab();
	}

}

/*!
 * Move a grabbed entity: wait for the pull gesture, fly to the hand, or follow the hand
 *
 * \param release The grab button was released
 * \param store   The trigger was pressed to put the object into the inventory
 */
void updateGrab(VRHand handId, Entity & entity, bool release, bool store, bool dragged) {

	GrabHand & hand = g_hands[handId];
	entity.show = SHOW_FLAG_IN_SCENE;

	if(store && (hand.state == GrabHeld || hand.state == GrabFlying) && storeEntity(handId, entity, dragged)) {
		return;
	}

	switch(hand.state) {

		case GrabSelected: {
			if(release) {
				releaseEntity(hand, entity, Vec3f(0.f), dragged);
				return;
			}
			// Flicking the hand towards the player or upwards pulls the object (up is -y)
			Vec3f center = entity.pos + getEntityCenterOffset(entity);
			Vec3f away = center - hand.position;
			float towards = (glm::length(away) > 1.f) ? glm::dot(hand.velocity, -glm::normalize(away)) : 0.f;
			float upwards = -hand.velocity.y;
			if(std::max(towards, upwards) > FlickSpeed) {
				hand.state = GrabFlying;
				hand.flightStart = entity.pos;
				hand.flightTime = 0.f;
				ARX_SOUND_PlaySFX(g_snd.WHOOSH, &entity.pos);
			}
			return;
		}

		case GrabFlying: {
			hand.flightTime += g_vr->getFrameDuration();
			float t = std::min(hand.flightTime / FlightDuration, 1.f);
			// Arrive with the center of the object in the hand, in an arc (up is -y)
			Vec3f target = hand.position - getEntityCenterOffset(entity);
			Vec3f position = glm::mix(hand.flightStart, target, t);
			position.y -= glm::distance(hand.flightStart, target) * 0.3f * 4.f * t * (1.f - t);
			Vec3f previous = entity.pos;
			ARX_INTERACTIVE_Teleport(&entity, position, true);
			if(release) {
				// Let go during the flight: keep flying with the current velocity
				float dt = std::max(g_vr->getFrameDuration(), 0.001f);
				releaseEntity(hand, entity, (position - previous) / dt, dragged);
				return;
			}
			if(t >= 1.f) {
				holdEntity(hand, entity);
			}
			return;
		}

		case GrabHeld: {
			setEntityRotation(entity, hand.orientation * hand.rotationOffset);
			ARX_INTERACTIVE_Teleport(&entity, hand.position + hand.orientation * hand.positionOffset, true);
			if(release) {
				releaseEntity(hand, entity, hand.velocity, dragged);
				return;
			}
			// Food or a drink brought to the mouth is eaten or drunk (user request)
			if(isConsumable(entity)) {
				float distance = getMouthDistance(entity);
				if(hand.mouthUsed) {
					hand.mouthUsed = (distance < MouthLeave);
				} else if(distance < MouthDistance) {
					hand.mouthTime += g_vr->getFrameDuration();
					if(hand.mouthTime >= MouthDuration) {
						consumeEntity(handId, entity, dragged);
					}
				} else {
					hand.mouthTime = 0.f;
				}
			}
			return;
		}

		case GrabNone: break;

	}

}

//! \return true if a hand may grab an entity directly
bool canGrab(const Entity & entity) {
	for(const GrabHand & hand : g_hands) {
		if(hand.state != GrabNone && hand.entity == entity.index()) {
			return false; // Held by the other hand
		}
	}
	return (entity.ioflags & IO_ITEM) && entity.show == SHOW_FLAG_IN_SCENE && entity.obj
	       && (entity.gameFlags & GFLAG_INTERACTIVITY) && &entity != g_draggedEntity
	       && closerThan(player.pos, entity.pos, 360.f);
}

//! The grabbable object closest to the hand and the distance from the hand to the box around its model
Entity * findNearestEntity(const GrabHand & hand, float & nearest) {
	
	Entity * result = nullptr;
	nearest = std::numeric_limits<float>::max();
	for(Entity & entity : entities) {
		if(!canGrab(entity) || !closerThan(hand.position, entity.pos, 150.f)) {
			continue;
		}
		// Distance to the box around the model, in the model's own space (inside the box counts as touching)
		EERIE_3D_BBOX bbox;
		for(const EERIE_VERTEX & vertex : entity.obj->vertexlist) {
			bbox.add(vertex.v);
		}
		Vec3f local = glm::inverse(getEntityRotation(entity)) * (hand.position - entity.pos);
		float distance = glm::distance(local, glm::clamp(local, bbox.min, bbox.max));
		if(distance <= nearest) {
			nearest = distance;
			result = &entity;
		}
	}
	
	return result;
}

//! \return the grabbable object closest to the hand, if the hand touches it
Entity * findTouchedEntity(const GrabHand & hand) {
	const float TouchDistance = 8.f; // cm from the box around the model to the controller
	float distance;
	Entity * entity = findNearestEntity(hand, distance);
	return (distance <= TouchDistance) ? entity : nullptr;
}

} // anonymous namespace

void updateVRHandMotion() {

	if(!g_vr) {
		return;
	}

	g_frame++;
	float dt = g_vr->getFrameDuration();

	for(size_t handId = 0; handId < VRHandCount; handId++) {

		GrabHand & hand = g_hands[handId];
		const VRPose & grip = g_vr->getInput().getHand(VRHand(handId)).grip;

		Vec3f position;
		glm::quat orientation;
		if(!grip.valid || !trackingPoseToWorld(grip.position, grip.orientation, position, orientation)) {
			hand.valid = false;
			hand.velocity = Vec3f(0.f);
			continue;
		}

		if(hand.valid && dt > 0.f) {
			Vec3f delta = position - hand.position;
			// Ignore jumps (teleports, turning the play space)
			if(glm::length(delta) < 50.f) {
				hand.velocity += (delta / dt - hand.velocity) * (1.f - std::exp(-dt * 20.f));
			} else {
				hand.velocity = Vec3f(0.f);
			}
		}

		hand.position = position;
		hand.orientation = orientation;
		hand.valid = true;
	}

}

bool updateVRDraggedEntity(Entity & entity) {

	GrabHand & hand = g_hands[grabMainHand()];
	if(!isVRMotionControlled() || !g_vr || !entity.obj || !hand.valid) {
		return false;
	}

	/*
	 * Items dragged out of an inventory or the equipment use the normal drag and drop while pointing at an interface.
	 * Leaving it they go into the hand: also an unequipped weapon, which is in the scene but still at the place where
	 * it was picked up.
	 */
	static EntityHandle s_draggedFromInterface;
	static unsigned long s_draggedFromInterfaceFrame = 0;
	bool newGrab = (hand.entity != entity.index() || hand.state == GrabNone || hand.lastFrame + 2 < g_frame);
	if(newGrab && g_vr->isPointingAtUI()) {
		s_draggedFromInterface = entity.index();
		s_draggedFromInterfaceFrame = g_frame;
		return false;
	}

	if(newGrab) {
		bool fromInterface = s_draggedFromInterface == entity.index() && s_draggedFromInterfaceFrame + 2 >= g_frame;
		startGrab(hand, entity, fromInterface);
		s_draggedFromInterface = EntityHandle();
	}
	hand.lastFrame = g_frame;

	bool store = g_vr->getInput().getHand(grabMainHand()).wasPressed(VRButtonTrigger);
	updateGrab(grabMainHand(), entity, eeMouseUp1(), store, true);

	return true;
}

namespace {

//! \return true if a hand may touch and grab objects directly (instead of the game's cursor and drag)
bool canGrabDirectly(VRHand handId) {

	if(g_vr->isBookInHand()) {
		return false; // One hand holds the book, the other points at it
	}

	// The hand holding a drawn weapon attacks with it (fists also grab, see VRCombat)
	bool holdsWeapon = (player.Interface & INTER_COMBATMODE) && ARX_EQUIPMENT_GetPlayerWeaponType() != WEAPON_BARE
	                   && getVRWeaponHand() == handId;
	if(handId == grabOffHand()) {
		return !holdsWeapon;
	}

	// The main hand cursor is needed to drop dragged items, combine items and select spell targets. In the magic mode
	// its grip stores the spell drawn instead (see VRSystem::updatePointer).
	return !holdsWeapon && !g_draggedEntity && !COMBINE && !COMBINEGOLD && !LOOKING_FOR_SPELL_TARGET
	       && !g_vr->isMagicMode();
}

//! \return true if a hand may also point at objects to grab them: the main hand points at panels instead
bool canPointToGrab(VRHand handId) {
	// At the sheath the grip draws the weapon (see VRControls)
	return !isVRHandAtSheath(handId) && (handId == grabOffHand() || !g_vr->isPointingAtUI());
}

//! Point at, take, grab, pull and throw objects with a hand, without the game's drag
void updateDirectGrab(VRHand handId) {
	
	GrabHand & hand = g_hands[handId];
	const VRHandState & input = g_vr->getInput().getHand(handId);

	if(input.wasPressed(VRButtonGrip)) {
		hand.gripTime = 0.f;
	} else if(input.isPressed(VRButtonGrip)) {
		hand.gripTime += g_vr->getFrameDuration();
	}

	// A held object follows the hand
	if(hand.state != GrabNone) {
		hand.target = EntityHandle();
		if(!hand.direct) {
			return; // Grabbed through the game's drag (see updateVRDraggedEntity)
		}
		Entity * entity = entities.get(hand.entity);
		if(!entity || !entity->obj || !hand.valid) {
			hand.resetGrab();
		} else {
			updateGrab(handId, *entity, !input.isPressed(VRButtonGrip), input.wasPressed(VRButtonTrigger), false);
		}
		return;
	}
	
	if(!hand.valid || ARXmenu.mode() != Mode_InGame || BLOCK_PLAYER_CONTROLS || !canGrabDirectly(handId)) {
		hand.target = hand.pressed = EntityHandle();
		return;
	}
	
	// Touch objects with the hand, or point at them, with a short vibration on a new one
	Entity * target = findTouchedEntity(hand);
	bool touching = (target != nullptr);
	if(!target && canPointToGrab(handId)) {
		target = getVRPointedEntity(input.aim, handId == grabOffHand());
		if(target && !canGrab(*target)) {
			target = nullptr;
		}
	} else if(handId == grabOffHand()) {
		getVRPointedEntity(input.aim, true); // Only for the debug ray
	}
	EntityHandle targetHandle = target ? target->index() : EntityHandle();
	if(target && targetHandle != hand.target) {
		g_vr->getInput().vibrate(handId, touching ? 0.3f : 0.15f, 0.02f);
	}
	hand.target = targetHandle;
	hand.targetTouched = touching;

	/*
	 * Grip: a touched object is grabbed at once, for a pointed one a short press takes it and holding grabs it.
	 * To catch thrown objects, one that touches the hand shortly after pressing the grip is grabbed too.
	 */
	const float CatchWindow = 0.3f;
	bool fist = input.isPressed(VRButtonTrigger); // With the grip: punches instead (see VRCombat)
	bool catching = touching && input.isPressed(VRButtonGrip) && hand.gripTime < CatchWindow
	                && hand.pressed == EntityHandle() && !fist;
	if((input.wasPressed(VRButtonGrip) && target && !fist) || catching) {
		if(touching) {
			startGrab(hand, *target);
			hand.direct = true;
			hand.target = EntityHandle();
			return;
		}
		hand.pressed = targetHandle;
		hand.pressTime = 0.f;
	}
	Entity * pressed = entities.get(hand.pressed);
	if(!pressed || !canGrab(*pressed)) {
		hand.pressed = EntityHandle();
		return;
	}
	if(input.isPressed(VRButtonGrip)) {
		hand.pressTime += g_vr->getFrameDuration();
		if(hand.pressTime >= HoldDuration) {
			hand.pressed = EntityHandle();
			startGrab(hand, *pressed);
			hand.direct = true;
		}
	} else {
		hand.pressed = EntityHandle();
		storeEntity(handId, *pressed, false);
	}
	
}

} // anonymous namespace

void updateVRHandGrabs() {
	
	if(!isVRMotionControlled() || !g_vr) {
		return;
	}
	
	updateDirectGrab(grabMainHand());
	updateDirectGrab(grabOffHand());
}

bool isVRGrabTarget(const Entity & entity) {
	for(const GrabHand & hand : g_hands) {
		if(hand.state == GrabNone && hand.target == entity.index()) {
			return true;
		}
	}
	return false;
}

bool isVRGripGrabbing() {
	const GrabHand & hand = g_hands[grabMainHand()];
	bool targetValid = hand.target != EntityHandle() && (hand.targetTouched || canPointToGrab(grabMainHand()));
	if(targetValid || hand.pressed != EntityHandle() || (hand.state != GrabNone && hand.direct)) {
		return true;
	}
	// The target is from the last game frame: also check for an object that has just reached the hand
	return g_vr && hand.valid && hand.state == GrabNone && ARXmenu.mode() == Mode_InGame
	       && canGrabDirectly(grabMainHand()) && findTouchedEntity(hand);
}

bool isVRGrabbingDirectly() {
	const GrabHand & hand = g_hands[grabMainHand()];
	return hand.state != GrabNone && hand.direct;
}

void resetVRGrabs() {
	for(GrabHand & hand : g_hands) {
		hand.resetGrab();
		hand.target = EntityHandle();
		hand.targetTouched = false;
		hand.pressed = EntityHandle();
	}
}

bool isVRGrabbing() {
	const GrabHand & hand = g_hands[grabMainHand()];
	return hand.state != GrabNone && (hand.direct || hand.lastFrame + 2 >= g_frame);
}

bool isVROffHandGrabbing() {
	return g_hands[grabOffHand()].state != GrabNone;
}
