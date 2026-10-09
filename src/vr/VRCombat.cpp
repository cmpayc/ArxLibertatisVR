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

#include "vr/VRCombat.h"

#include <algorithm>
#include <string_view>

#include <boost/algorithm/string/predicate.hpp>

#include <glm/glm.hpp>

#include "animation/AnimationRender.h"
#include "core/Config.h"
#include "core/Core.h"
#include "game/Damage.h"
#include "game/Entity.h"
#include "game/EntityManager.h"
#include "game/Equipment.h"
#include "game/Inventory.h"
#include "game/Player.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/particle/ParticleEffects.h"
#include "graphics/particle/ParticleTextures.h"
#include "graphics/particle/Spark.h"
#include "gui/Menu.h"
#include "gui/Speech.h"
#include "io/log/Logger.h"
#include "math/Random.h"
#include "math/RandomVector.h"
#include "physics/Collisions.h"
#include "physics/Projectile.h"
#include "scene/LinkedObject.h"
#include "scene/GameSound.h"
#include "scene/Interactive.h"
#include "scene/Object.h"
#include "script/Script.h"
#include "vr/VRBody.h"
#include "vr/VRCamera.h"
#include "vr/VRControls.h"
#include "vr/VRGrab.h"
#include "vr/VRSystem.h"

namespace {

const float SwingStartSpeed = 1.5f;  // m/s
const float SwingEndSpeed = 0.8f;    // m/s
const float SwingEndTime = 0.15f;    // s below the end speed
const float SwingTurnRatio = 0.3f;   // of the peak speed: the turning point of quick strikes
const float SwingTurnTime = 0.05f;   // s below the turning speed
const float HardHitSpeed = 2.5f;     // m/s for wear and sounds when the weapon hits walls and objects

/*
 * How hard a swing is: from its peak speed and the distance it travelled so far (a short flick is a light hit,
 * a big fast swing a heavy one), in meters at the tracked point
 */
struct SwingScale {
	float lightSpeed;    // m/s, up to which a swing is a quick strike of the game
	float heavySpeed;    // m/s for a fully charged strike (a heavy hit)
	float lightDistance; // m, up to which a swing is a quick strike
	float heavyDistance; // m for a fully charged strike
};
const SwingScale WeaponSwing = { 9.f, 18.f, 0.8f, 2.f }; // Near the tip of the blade (measured: normal 7-10 m/s, 0.7-1 m)
const SwingScale FistSwing = { 5.f, 11.f, 0.4f, 1.2f };

struct Swing {
	bool tracked = false;
	Vec3f previous = Vec3f(0.f); // Tracking space, meters
	float speed = 0.f;
	bool active = false;
	bool hit = false;
	bool hard = false; // Fast enough for wear and sounds on walls and objects
	float slowTime = 0.f;
	float peakSpeed = 0.f; // Of the current swing
	float distance = 0.f;  // Travelled in the current swing
};

Swing g_swings[VRHandCount];

VRHand mainHand() {
	return config.vr.leftHanded ? VRHandLeft : VRHandRight;
}

VRHand secondaryHand() {
	return config.vr.leftHanded ? VRHandRight : VRHandLeft;
}

/*!
 * The equipped weapon once it is in a hand of the player model (after the draw animation), or nullptr
 *
 * The game always puts it into the right hand: drawn with the left hand (see VRControls) it is moved to the left hand.
 */
Entity * getHeldWeapon() {

	Entity * weapon = entities.get(player.equiped[EQUIP_SLOT_WEAPON]);
	Entity * io = entities.player();
	if(!weapon || !weapon->obj || !io || !io->obj) {
		return nullptr;
	}

	const VertexId right = io->obj->fastaccess.primary_attach;
	const VertexId left = io->obj->fastaccess.left_attach;
	VertexId held;
	for(const EERIE_LINKED & link : io->obj->linked) {
		if(link.io == weapon && link.lidx && (link.lidx == right || link.lidx == left)) {
			held = link.lidx;
			break;
		}
	}
	if(!held) {
		return nullptr;
	}

	VertexId wanted = (getVRWeaponHand() == VRHandLeft) ? left : right;
	if(wanted && held != wanted) {
		linkEntities(*io, (wanted == left) ? "left_attach" : "primary_attach", *weapon, "primary_attach");
	}

	return weapon;
}

//! Distance from the grip of an object to its farthest vertex (the blade length), in object units (cm)
float getReach(const EERIE_3DOBJ & object) {
	VertexId attach = object.fastaccess.primary_attach;
	float reach = 0.f;
	if(attach && size_t(attach) < object.vertexlist.size()) {
		const Vec3f origin = object.vertexlist[attach].v;
		for(const EERIE_VERTEX & vertex : object.vertexlist) {
			reach = std::max(reach, glm::distance(vertex.v, origin));
		}
	}
	return reach;
}

/*!
 * Strike ratio (damage multiplier), like the charge of the game's strikes
 *
 * The game has no stamina: a strike is quick (ratio 0.1, about 0.2 for a normal click) or charged up to 1.5 s (ratio 1, a
 * heavy hit). Here normal swings are normal clicks, short taps weaker, only big and fast swings are heavy hits.
 */
float strikeRatio(const Swing & swing, const SwingScale & scale) {
	float speed = glm::clamp((swing.peakSpeed - scale.lightSpeed) / (scale.heavySpeed - scale.lightSpeed), 0.f, 1.f);
	float distance = glm::clamp((swing.distance - scale.lightDistance) / (scale.heavyDistance - scale.lightDistance),
	                            0.f, 1.f);
	// A normal swing is like a normal click of the game (about 0.2: the strike start animation charges a little),
	// only taps are weaker
	float normal = 0.1f + 0.1f * glm::clamp(swing.peakSpeed / scale.lightSpeed, 0.f, 1.f);
	return normal + (1.f - normal) * speed * speed * distance * distance;
}

void strikeSpeak(Entity & io) {

	// Like the game: only strong strikes shout
	if(player.m_strikeAimRatio <= 0.8f) {
		return;
	}

	std::string_view speech = io.strikespeech;
	Entity * weapon = entities.get(player.equiped[EQUIP_SLOT_WEAPON]);
	if(weapon && !weapon->strikespeech.empty()) {
		speech = weapon->strikespeech;
	}

	ARX_SPEECH_AddSpeech(io, speech, ANIM_TALK_NEUTRAL, ARX_SPEECH_FLAG_NOTEXT);
}

/*!
 * Update the swing of a tracked point (tracking space, meters)
 *
 * \return true when a swing starts
 */
bool updateSwing(Swing & swing, const Vec3f & position, float dt) {

	if(!swing.tracked || dt <= 0.f) {
		swing = Swing();
		swing.tracked = true;
		swing.previous = position;
		return false;
	}

	float step = glm::distance(position, swing.previous);
	swing.previous = position;
	swing.speed += (step / dt - swing.speed) * 0.5f; // A little smoothing against tracking jitter

	if(!swing.active) {
		if(swing.speed > SwingStartSpeed) {
			swing.active = true;
			swing.hit = false;
			swing.hard = false;
			swing.slowTime = 0.f;
			swing.peakSpeed = swing.speed;
			swing.distance = step;
			return true;
		}
		return false;
	}

	swing.peakSpeed = std::max(swing.peakSpeed, swing.speed);
	swing.distance += step;

	/*
	 * The swing ends when the hand stops, or when it slows down a lot compared to the peak: at the turning point of
	 * quick strikes (jab, pull back, jab), which do not stop long enough, so that each one can hit
	 */
	if(swing.speed < SwingEndSpeed || swing.speed < swing.peakSpeed * SwingTurnRatio) {
		swing.slowTime += dt;
		float endTime = (swing.speed < SwingEndSpeed) ? SwingEndTime : SwingTurnTime;
		if(swing.slowTime > endTime) {
			swing.active = false;
		}
	} else {
		swing.slowTime = 0.f;
	}

	return false;
}

void resetSwings() {
	for(Swing & swing : g_swings) {
		swing = Swing();
	}
}

std::string_view strikeName(WeaponType type) {
	switch(type) {
		case WEAPON_DAGGER: return "dagger";
		case WEAPON_1H: return "1h";
		case WEAPON_2H: return "2h";
		default: return "bare";
	}
}

//! Damage factor of the player's attack without the bonus of the equipped weapon, for punches while it is sheathed
float getBareHandRatio() {

	Entity * weapon = entities.get(player.equiped[EQUIP_SLOT_WEAPON]);
	float attack = player.m_miscFull.damages;
	if(!weapon || !(weapon->ioflags & IO_ITEM) || !weapon->_itemdata->equipitem || attack <= 0.f) {
		return 1.f;
	}

	const IO_EQUIPITEM_ELEMENT & bonus = weapon->_itemdata->equipitem->elements[IO_EQUIPITEM_ELEMENT_Damages];
	float bare = attack - bonus.value;
	if(bonus.flags.has(IO_ELEMENT_FLAG_PERCENT)) {
		bare = attack / (1.f + bonus.value * 0.01f);
	}

	return glm::clamp(bare / attack, 0.05f, 1.f);
}

void startStrike(Entity & io, WeaponType type, VRHand hand) {
	/*
	 * The game plays the hit sound and wears the weapon once per strike that touches a wall or an object (isHit).
	 * In VR the weapon touches walls much more often (it rests on them, and any fast movement is a swing): this only
	 * counts once the swing is fast, see updateWeaponSwing.
	 */
	io.isHit = true;
	SendIOScriptEvent(nullptr, &io, SM_STRIKE, strikeName(type));
	g_vr->getInput().vibrate(hand, 0.1f, 0.02f);
}

//! A strike lands: heavy hits shout and shake harder
void landStrike(Entity & io, VRHand hand) {
	strikeSpeak(io);
	float strength = player.m_strikeAimRatio;
	g_vr->getInput().vibrate(hand, 0.5f + 0.5f * strength, 0.08f + 0.1f * strength);
}

void updateWeaponSwing(Entity & io, Entity & weapon, WeaponType type, float dt) {

	const VRHand hand = getVRWeaponHand(); // The hand holding the weapon
	const VRHandState & state = g_vr->getInput().getHand(hand);
	Swing & swing = g_swings[hand];
	if(!state.grip.valid) {
		swing = Swing();
		return;
	}

	// Near the tip of the blade, which points through the fist (OpenXR grip: -z)
	float reach = getReach(*weapon.obj) * weapon.scale * 0.8f / (100.f * config.vr.worldScale);
	Vec3f tip = state.grip.position + state.grip.orientation * (config.vr.handOffset + Vec3f(0.f, 0.f, -reach));

	if(updateSwing(swing, tip, dt)) {
		startStrike(io, type, hand);
	}
	if(!swing.active) {
		return;
	}

	// A hard swing that did not start against a wall: its first contact with a wall or an object counts
	if(!swing.hard && swing.speed > HardHitSpeed && !isVRWeaponInWall(hand)) {
		swing.hard = true;
		io.isHit = false;
	}

	player.m_strikeAimRatio = strikeRatio(swing, WeaponSwing);
	// Damage once per swing, then only the effects (sparks, blood) like the game
	if(ARX_EQUIPMENT_Strike_Check(&io, &weapon, player.m_strikeAimRatio, swing.hit ? 1 : 0) && !swing.hit) {
		swing.hit = true;
		landStrike(io, hand);
	}

}

void updateFistSwings(Entity & io, float dt) {

	for(size_t i = 0; i < VRHandCount; i++) {

		const VRHand hand = VRHand(i);
		const VRHandState & state = g_vr->getInput().getHand(hand);
		Swing & swing = g_swings[i];
		if(!state.grip.valid) {
			swing = Swing();
			continue;
		}

		/*
		 * Only a fist punches, in and outside of the combat mode: grip and trigger held together (user request: swinging
		 * the open hand hit NPCs by accident), and not holding an object. The fist does not grab, click or cast (see
		 * VRGrab, VRSystem, VRControls). The grip in the world is also a game click, which the game may take as a drag
		 * (isVRGrabbing): only objects really held count.
		 */
		bool holding = (hand == mainHand()) ? isVRGrabbingDirectly() : isVROffHandGrabbing();
		bool free = isVRFist(hand) && !holding && !g_vr->isBookInHand(); // The book needs both hands
		if(updateSwing(swing, state.grip.position, dt)) {
			if(free) {
				startStrike(io, WEAPON_BARE, hand);
			} else {
				swing.hit = true; // No punch in this swing
			}
		}
		if(!swing.active || swing.hit || !free) {
			continue;
		}

		VertexId fist = (hand == mainHand()) ? io.obj->fastaccess.primary_attach : io.obj->fastaccess.left_attach;
		if(!fist) {
			continue;
		}

		// Like the game's punches
		Sphere sphere;
		sphere.origin = io.obj->vertexWorldPositions[fist].v;
		sphere.radius = 15.f;
		Entity * target = nullptr;
		if(!CheckAnythingInSphere(sphere, &io, 0, &target)) {
			continue;
		}

		player.m_strikeAimRatio = strikeRatio(swing, FistSwing);
		float damage = (player.m_miscFull.damages + 1.f) * player.m_strikeAimRatio;
		bool hit = false;
		if(entities.get(player.equiped[EQUIP_SLOT_WEAPON])) {
			// The weapon is in its sheath: punch without its damage bonus (the game's punches use the full attack)
			if(target && (target->ioflags & IO_NPC)) {
				Vec3f position = sphere.origin;
				ARX_EQUIPMENT_ComputeDamages(&io, target, getBareHandRatio(), &position);
				hit = true;
			}
		} else {
			hit = tryToDoDamage(sphere.origin, damage, 40.f, io);
		}
		if(hit) {
			swing.hit = true;
			ParticleSparkSpawnContinous(sphere.origin, unsigned(damage), Color3f(0.45f, 0.1f, 0.f).toRGB());
			if(target) {
				ARX_SOUND_PlayCollision(target->material, MATERIAL_FLESH, 1.f, 1.f, sphere.origin, nullptr);
			}
			landStrike(io, hand);
		}

	}

}

/*
 * Archery: the hand without the bow takes an arrow near the bow, holding its trigger draws the string by pulling the
 * hand back, releasing it shoots along the line from that hand through the bow (the game's own launch: arrows from the
 * inventory, damage, poison, the draw ratio for the speed and gravity)
 */
struct Archery {
	bool nocked = false;  // An arrow is in the string hand
	bool drawing = false; // The trigger pulls the string
	float startDistance = 0.f; // m between the hands when the draw started
	float pulseTime = 0.f;
};

Archery g_archery;

const float NockDistance = 0.3f;  // m between the hands to take an arrow and start drawing
const float PutAwayDistance = 0.45f; // m between the hands to put the arrow away while not drawing
const float FullDraw = 0.55f;    // m pulled back for the full draw

VRHand stringHand() {
	return (getVRWeaponHand() == VRHandLeft) ? VRHandRight : VRHandLeft;
}

VertexId getArrowAttach() {
	VertexId attach = getNamedVertex(arrowobj.get(), "attach");
	return attach ? attach : arrowobj->origin;
}

//! The tip of the arrow: its hit point farthest from the nock, like the game
VertexId getArrowTip(VertexId attach) {

	VertexId tip;
	float farthest = 0.f;
	for(const EERIE_ACTIONLIST & action : arrowobj->actionlist) {
		if(!boost::starts_with(action.name, "hit_")) {
			continue;
		}
		float distance = arx::distance2(arrowobj->vertexlist[attach].v, arrowobj->vertexlist[action.idx].v);
		if(distance > farthest) {
			tip = action.idx;
			farthest = distance;
		}
	}
	if(tip) {
		return tip;
	}

	for(VertexId vertex : arrowobj->vertexlist.handles()) {
		float distance = arx::distance2(arrowobj->vertexlist[attach].v, arrowobj->vertexlist[vertex].v);
		if(vertex != VertexId(0) && distance > farthest) {
			tip = vertex;
			farthest = distance;
		}
	}
	return tip ? tip : attach;
}

void dropArrow(Entity & io) {
	if(arrowobj) {
		EERIE_LINKEDOBJ_UnLinkObjectFromObject(io.obj, arrowobj.get());
	}
	g_archery = Archery();
}

//! Shoot the nocked arrow from the bow along a direction, like the game does when the bow string is released
void shootArrow(Entity & io, const Vec3f & position, const Vec3f & direction, float ratio) {

	dropArrow(io);

	SendIOScriptEvent(nullptr, &io, SM_STRIKE, "bow");
	player.m_bowAimRatio = ratio;
	player.m_strikeAimRatio = ratio;

	Entity * quiver = getInventoryItemWithLowestDurability("arrows", 1.f);
	float poisonous = 0.f;
	if(quiver) {
		poisonous = quiver->poisonous;
		if(quiver->poisonous_count > 0) {
			quiver->poisonous_count--;
			if(quiver->poisonous_count <= 0) {
				quiver->poisonous = 0;
			}
		}
		ARX_DAMAGES_DurabilityLoss(quiver, 1.f);
		if(ValidIOAddress(quiver) && quiver->durability <= 0.f) {
			ARX_INTERACTIVE_DestroyIOdelayed(quiver);
		}
	}

	if(!arrowobj || arrowobj->vertexlist.size() < 2) {
		return;
	}

	// The arrow model points along its tip
	VertexId attach = getArrowAttach();
	VertexId tip = getArrowTip(attach);
	glm::quat quat = glm::inverse(getProjectileQuatFromVector(arrowobj->vertexlist[tip].v
	                                                          - arrowobj->vertexlist[attach].v));

	float velocity = std::max(ratio + 0.3f, 0.9f);
	float gravity = 0.0002f * glm::clamp(1.f - ratio, 0.f, 1.f); // Not fully drawn arrows drop
	float weaponDamage = getEquipmentBaseModifier(IO_EQUIPITEM_ELEMENT_Damages);
	float damages = weaponDamage * (1.f + (player.m_skillFull.projectile + player.m_attributeFull.dexterity) * 0.02f);

	ARX_THROWN_OBJECT_Throw(EntityHandle_Player, position, direction * velocity, gravity, arrowobj.get(), attach, quat,
	                        damages, poisonous);
}

void updateArchery(Entity & io, float dt) {

	const VRHand bowHand = getVRWeaponHand();
	const VRHand hand = stringHand();
	const VRHandState & bow = g_vr->getInput().getHand(bowHand);
	const VRHandState & string = g_vr->getInput().getHand(hand);
	if(!bow.grip.valid || !string.grip.valid || !arrowobj) {
		return;
	}

	float distance = glm::distance(string.grip.position, bow.grip.position);
	if(!getInventoryItemWithLowestDurability("arrows", 1.f)) {
		if(g_archery.nocked) {
			dropArrow(io); // No arrows left
		}
		return;
	}

	// Take an arrow with the free hand near the bow
	auto attachArrow = [&]() {
		EERIE_LINKEDOBJ_LinkObjectToObject(io.obj, arrowobj.get(),
		                                   (hand == VRHandLeft) ? "left_attach" : "primary_attach", "attach");
	};
	if(!g_archery.nocked) {
		if(distance < NockDistance) {
			attachArrow();
			g_archery.nocked = true;
			g_vr->getInput().vibrate(hand, 0.3f, 0.03f);
		}
		return;
	}

	// The game unlinks the arrow at the end of the bow's draw animation: keep it in the hand
	bool linked = std::any_of(io.obj->linked.begin(), io.obj->linked.end(), [](const EERIE_LINKED & link) {
		return link.obj == arrowobj.get();
	});
	if(!linked) {
		attachArrow();
	}

	// Moving the hand away from the bow without drawing puts the arrow away
	if(!g_archery.drawing && distance > PutAwayDistance) {
		dropArrow(io);
		return;
	}
	
	// Hold the trigger near the bow to grab the string
	if(!g_archery.drawing) {
		if(string.wasPressed(VRButtonTrigger) && distance < NockDistance + 0.05f) {
			g_archery.drawing = true;
			g_archery.startDistance = distance;
			g_archery.pulseTime = 0.f;
			g_vr->getInput().vibrate(hand, 0.4f, 0.03f);
		}
		return;
	}

	// Pull back: the tension rises with the distance, felt as faster and stronger pulses
	float ratio = glm::clamp((distance - g_archery.startDistance) / FullDraw, 0.f, 1.f);
	player.m_bowAimRatio = ratio;
	g_archery.pulseTime -= dt;
	if(g_archery.pulseTime <= 0.f && ratio > 0.05f) {
		g_vr->getInput().vibrate(hand, 0.1f + 0.4f * ratio, 0.02f);
		g_vr->getInput().vibrate(bowHand, 0.05f + 0.2f * ratio, 0.02f);
		g_archery.pulseTime = 0.12f - 0.08f * ratio;
	}

	if(string.isPressed(VRButtonTrigger)) {
		return;
	}

	// Released
	g_archery.drawing = false;
	if(ratio < 0.1f) {
		return; // Hardly drawn: keep the arrow
	}

	// Along the arrow as it is drawn: from just above the drawing hand to its rest on the bow (adjustVRLinkedTransform)
	VertexId stringVertex = (hand == VRHandLeft) ? io.obj->fastaccess.left_attach : io.obj->fastaccess.primary_attach;
	Vec3f bowPosition;
	if(!stringVertex || !getVRArrowRest(bowPosition)) {
		return;
	}
	Vec3f stringPosition = io.obj->vertexWorldPositions[stringVertex].v + getVRArrowRaise();
	if(glm::distance(bowPosition, stringPosition) < 1.f) {
		return;
	}
	Vec3f direction = glm::normalize(bowPosition - stringPosition);
	shootArrow(io, bowPosition + direction * 10.f, direction, ratio);
	g_vr->getInput().vibrate(hand, 0.8f, 0.08f);
	g_vr->getInput().vibrate(bowHand, 0.6f, 0.1f);

}

/*!
 * The lit torch is held in a hand (the game only shows its icon and lights the player position): the off hand, or the
 * main hand while a weapon is drawn in the off hand. Its flame burns at its fire vertex.
 */
EntityHandle g_heldTorch; // The torch linked to a hand
VRHand g_torchHand = VRHandCount; // The hand that took the torch from the belt, VRHandCount: the off hand
bool g_torchStowArmed = false; // The torch hand was away from the hip, see updateTorch
const float TorchStowRadius = 16.f; // cm from the hip joint to the wrist
const float TorchIgniteRadius = 15.f; // cm around the flame

/*!
 * The torch put away at the hip hangs on the belt there, unlit. It stays in the inventory (saved games keep it there)
 * and is drawn at the hip (getVRCarriedItem) until it is taken back with a grip or leaves the inventory.
 * Which torch hangs there, and on which side, is a local script variable of the torch: saved games and level changes
 * keep it, while entity handles change when they are loaded.
 */
EntityHandle g_beltTorch;
VRHand g_beltSide = VRHandLeft;
Vec3f g_beltCentre(0.f);
bool g_beltCentreValid = false;
bool g_beltTaking[VRHandCount] = { }; // The grip that took the torch is still held (not a click or grab)
const float BeltTakeRadius = 15.f; // cm from the wrist to the middle of the torch
const std::string_view BeltVariable = "\xA7vr_on_belt"; // Local long: 0 not on the belt, 1 + the side (VRHand)

void setOnBelt(Entity & torch, long side) {
	SETVarValueLong(torch.m_variables, BeltVariable, side);
}

Entity * getBeltTorch() {

	Entity * torch = entities.get(g_beltTorch);
	if(!torch || GETVarValueLong(torch->m_variables, BeltVariable) <= 0) {
		// After loading a save or changing level: find the torch marked as on the belt again
		torch = nullptr;
		for(Entity & item : entities(IO_ITEM)) {
			if(GETVarValueLong(item.m_variables, BeltVariable) > 0) {
				torch = &item;
				break;
			}
		}
		g_beltTorch = torch ? torch->index() : EntityHandle();
	}
	if(!torch) {
		g_beltCentreValid = false;
		return nullptr;
	}

	if(torch == player.torch || !torch->obj || !torch->obj->fastaccess.fire || torch->owner() != entities.player()
	   || !locateInInventories(torch)) {
		// Taken in hand, dropped, given away... it is no longer on the belt
		setOnBelt(*torch, 0);
		g_beltTorch = EntityHandle();
		g_beltCentreValid = false;
		return nullptr;
	}

	long side = GETVarValueLong(torch->m_variables, BeltVariable) - 1;
	g_beltSide = (side == VRHandRight) ? VRHandRight : VRHandLeft;
	return torch;
}

void updateBeltTorch() {

	for(size_t hand = 0; hand < VRHandCount; hand++) {
		if(!g_vr->getInput().getHand(VRHand(hand)).isPressed(VRButtonGrip)) {
			g_beltTaking[hand] = false;
		}
	}

	Entity * torch = getBeltTorch();
	if(!torch || player.torch) {
		return;
	}
	TransformInfo transform;
	g_beltCentreValid = getVRBeltTransform(g_beltSide, *torch->obj, torch->scale, transform, g_beltCentre);
	if(!g_beltCentreValid) {
		return;
	}

	if(g_vr->isBookInHand()) {
		return; // The hands hold the book
	}

	// A grip near the torch takes it (and lights it) with a hand that holds no weapon
	bool weaponDrawn = (player.Interface & INTER_COMBATMODE) && ARX_EQUIPMENT_GetPlayerWeaponType() != WEAPON_BARE;
	for(size_t i = 0; i < VRHandCount; i++) {
		VRHand hand = VRHand(i);
		Vec3f wrist;
		glm::quat orientation;
		if(!g_vr->getInput().getHand(hand).wasPressed(VRButtonGrip) || (weaponDrawn && getVRWeaponHand() == hand)
		   || !getVRWristPose(hand, wrist, orientation) || glm::distance(wrist, g_beltCentre) > BeltTakeRadius) {
			continue;
		}
		setOnBelt(*torch, 0);
		g_beltTorch = EntityHandle();
		g_beltCentreValid = false;
		g_beltTaking[hand] = true;
		g_torchHand = hand;
		g_torchStowArmed = false;
		ARX_PLAYER_ClickedOnTorch(torch); // Equips and lights it
		g_vr->getInput().vibrate(hand, 0.5f, 0.06f);
		return;
	}
}

//! Put the lit torch away, unlit, on the belt on the side of the hand holding it
void hangTorchOnBelt(Entity & torch, VRHand hand) {
	g_torchStowArmed = false;
	g_heldTorch = EntityHandle();
	g_torchHand = VRHandCount;
	ARX_PLAYER_ClickedOnTorch(&torch); // Unequips it into the inventory
	// Only one torch hangs on the belt
	if(Entity * previous = getBeltTorch(); previous && previous != &torch) {
		setOnBelt(*previous, 0);
	}
	setOnBelt(torch, 1 + long(hand));
	g_beltTorch = torch.index();
	g_beltSide = hand;
}

/*!
 * The flame of the torch held in a hand: the game's fire particles (createFireParticles) are drawn larger the closer
 * they are, and a hand is close to the eyes, so the flame took half of the view (user). Smaller and shorter particles.
 */
void createTorchFlame(const Vec3f & position) {

	const float Scale = 0.4f; // Of the game's fire
	for(int i = 0; i < 2; i++) {
		if(Random::getf() >= 0.4f) {
			continue;
		}
		PARTICLE_DEF * pd = createParticle();
		if(!pd) {
			break;
		}
		pd->ov = position;
		pd->move = (Vec3f(2.f, 2.f, 2.f) - Vec3f(4.f, 22.f, 4.f) * arx::randomVec3f()) * Scale;
		pd->size = 7.f * Scale;
		pd->duration = Random::get(500ms, 1500ms);
		pd->m_flags = FIRE_TO_SMOKE | ROTATING;
		pd->tc = g_particleTextures.fire2;
		pd->m_rotation = Random::getf(-0.1f, 0.1f);
		pd->sizeDelta = -8.f * Scale;
		pd->rgb = Color3f(0.71f, 0.43f, 0.29f);
		pd->elapsed = 2ms * -i;
	}
}

//! The hand holding the lit torch: the hand that took it from the belt or the off hand, the other hand while a weapon
//! is drawn in it
VRHand getTorchHand() {
	bool weaponDrawn = (player.Interface & INTER_COMBATMODE) && ARX_EQUIPMENT_GetPlayerWeaponType() != WEAPON_BARE;
	VRHand hand = (g_torchHand < VRHandCount) ? g_torchHand : secondaryHand();
	if(weaponDrawn && getVRWeaponHand() == hand) {
		hand = (hand == VRHandLeft) ? VRHandRight : VRHandLeft;
	}
	return hand;
}

/*!
 * The lit torch is held in a hand (the game only shows its icon and lights the player position), see getTorchHand.
 * Its flame burns at its fire vertex.
 */
void updateTorch(Entity & io) {

	updateBeltTorch();

	// A torch that went out (e.g. a douse spell) is not unlinked by the game: let go of it
	Entity * torch = player.torch;
	if(Entity * held = entities.get(g_heldTorch); held && held != torch) {
		unlinkEntity(*held);
		g_heldTorch = EntityHandle();
	}
	if(!torch || !torch->obj) {
		g_torchStowArmed = false;
		g_torchHand = VRHandCount;
		return;
	}

	VRHand hand = getTorchHand();

	VertexId wanted = (hand == VRHandLeft) ? io.obj->fastaccess.left_attach : io.obj->fastaccess.primary_attach;
	bool linked = false;
	for(const EERIE_LINKED & link : io.obj->linked) {
		if(link.io == torch) {
			linked = (link.lidx == wanted);
			break;
		}
	}
	if(!linked && wanted) {
		// The torch model has no attach vertex: held at its origin
		VertexId attach = getNamedVertex(torch->obj, "primary_attach");
		if(!attach) {
			attach = getNamedVertex(torch->obj, "attach");
		}
		if(!attach) {
			attach = torch->obj->origin;
		}
		if(VertexGroupId group = getGroupForVertex(io.obj, wanted)) {
			unlinkEntity(*torch);
			EERIE_LINKED link;
			link.lidx = wanted;
			link.lgroup = group;
			link.lidx2 = attach;
			link.obj = torch->obj;
			link.io = torch;
			io.obj->linked.push_back(link);
			torch->setOwner(&io);
			g_heldTorch = torch->index();
		}
	}

	// Bringing the torch hand to the hip on its side hangs the torch on the belt there (unlit, in the inventory), once
	// the hand has been away from the hip since the torch was taken
	Vec3f wrist;
	Vec3f hip;
	glm::quat orientation;
	if(getVRWristPose(hand, wrist, orientation) && getVRHipPosition(hand, hip)) {
		float distance = glm::distance(wrist, hip);
		if(distance > TorchStowRadius + 15.f) {
			g_torchStowArmed = true;
		} else if(g_torchStowArmed && distance < TorchStowRadius) {
			hangTorchOnBelt(*torch, hand);
			g_vr->getInput().vibrate(hand, 0.5f, 0.06f);
			return;
		}
	}

	VertexId fire = torch->obj->fastaccess.fire;
	if(fire && size_t(fire) < torch->obj->vertexWorldPositions.size()) {
		createTorchFlame(torch->obj->vertexWorldPositions[fire].v);
		// Touching something that can burn (another torch, a candle, a fireplace) with the flame lights it, like fire arrows
		Sphere flame(torch->obj->vertexWorldPositions[fire].v, TorchIgniteRadius);
		igniteLights(flame);
		igniteEntities(flame);
	}
}

//! A faint blue glow (sparkles) around the casting hand while the magic mode is on (see VRSystem::updatePointer)
void updateMagicGlow(const Entity & io) {

	if(!g_vr->isMagicMode()) {
		return;
	}
	VertexId vertex = (mainHand() == VRHandLeft) ? io.obj->fastaccess.left_attach : io.obj->fastaccess.primary_attach;
	if(!vertex || size_t(vertex) >= io.obj->vertexWorldPositions.size()) {
		return;
	}
	Vec3f hand = io.obj->vertexWorldPositions[vertex].v;

	const float SparklesPerSecond = 40.f;
	float count = SparklesPerSecond * g_vr->getFrameDuration();
	for(; count > 0.f; count -= 1.f) {
		if(count < 1.f && Random::getf() > count) {
			break;
		}
		PARTICLE_DEF * pd = createParticle();
		if(!pd) {
			break;
		}
		pd->ov = hand + arx::randomVec(-2.5f, 2.5f);
		pd->move = Vec3f(0.f, -4.f, 0.f) + arx::randomVec(-2.f, 2.f); // Rising slowly (y points down)
		pd->size = Random::getf(0.4f, 0.8f);
		pd->sizeDelta = -0.5f;
		pd->duration = Random::get(400ms, 800ms);
		pd->tc = g_particleTextures.healing;
		pd->rgb = Color3f(0.06f, 0.11f, 0.25f); // Additive: dim is more transparent
	}
}

} // anonymous namespace

void resetVRCombatLevelState() {
	g_archery = Archery();
	g_heldTorch = EntityHandle();
	g_torchHand = VRHandCount;
	g_torchStowArmed = false;
	// The torch on the belt is found again by its variable (see getBeltTorch)
	g_beltTorch = EntityHandle();
	g_beltCentreValid = false;
	std::fill(std::begin(g_beltTaking), std::end(g_beltTaking), false);
}

Entity * getVRCarriedItem(const Entity & entity, TransformInfo & transform) {
	
	ARX_UNUSED(entity);
	
	Entity * torch = (isVRActive() && !player.torch) ? getBeltTorch() : nullptr;
	Vec3f centre;
	if(!torch || !getVRBeltTransform(g_beltSide, *torch->obj, torch->scale, transform, centre)) {
		return nullptr;
	}
	return torch;
}

bool isVRHandAtBeltTorch(VRHand hand) {
	if(!isVRActive() || hand >= VRHandCount) {
		return false;
	}
	if(g_beltTaking[hand]) {
		return true;
	}
	Vec3f wrist;
	glm::quat orientation;
	return g_beltCentreValid && !player.torch && entities.get(g_beltTorch) && getVRWristPose(hand, wrist, orientation)
	       && glm::distance(wrist, g_beltCentre) < BeltTakeRadius;
}

bool getVRTorchLight(Vec3f & position) {
	
	// At the flame of the torch held in a hand, see updateTorch
	Entity * torch = player.torch;
	if(!isVRActive() || !isVRPlayerView() || !torch || !torch->obj || torch->show != SHOW_FLAG_ON_PLAYER) {
		return false;
	}
	VertexId fire = torch->obj->fastaccess.fire;
	if(!fire || size_t(fire) >= torch->obj->vertexWorldPositions.size()) {
		return false;
	}
	
	position = torch->obj->vertexWorldPositions[fire].v;
	return true;
}

bool isVRMeleeActive() {
	return isVRActive() && g_vr;
}

bool isVRArcheryActive() {
	return isVRActive() && (player.Interface & INTER_COMBATMODE) && ARX_EQUIPMENT_GetPlayerWeaponType() == WEAPON_BOW;
}

void updateVRCombat() {

	Entity * io = entities.player();
	bool canFight = ARXmenu.mode() == Mode_InGame && !BLOCK_PLAYER_CONTROLS && player.lifePool.current > 0.f;
	if(!isVRActive() || !g_vr || !io || !io->obj || !canFight) {
		resetSwings();
		return;
	}

	updateTorch(*io);
	updateMagicGlow(*io);
	
	float dt = g_vr->getFrameDuration();
	WeaponType type = ARX_EQUIPMENT_GetPlayerWeaponType();

	// The fists punch whenever no weapon is drawn: without a weapon, or with it (or a bow) in its sheath
	bool drawn = (player.Interface & INTER_COMBATMODE) && type != WEAPON_BARE;
	if(!drawn || type != WEAPON_BOW) {
		if(g_archery.nocked) {
			dropArrow(*io); // The bow was put away (the game also unlinks the arrow)
		}
	}
	if(!drawn) {
		updateFistSwings(*io, dt);
		return;
	}

	// Also moves the weapon (or bow) to the hand that drew it
	Entity * weapon = getHeldWeapon();
	if(!weapon) {
		resetSwings();
		return;
	}

	if(type == WEAPON_BOW) {
		resetSwings();
		updateArchery(*io, dt);
		return;
	}

	updateWeaponSwing(*io, *weapon, type, dt);

}

void hangVRTorchOnBelt() {
	if(Entity * torch = player.torch; torch && isVRActive()) {
		hangTorchOnBelt(*torch, getTorchHand());
	}
}
