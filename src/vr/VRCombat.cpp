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

#include <glm/glm.hpp>

#include "core/Config.h"
#include "game/Damage.h"
#include "game/Entity.h"
#include "game/EntityManager.h"
#include "game/Equipment.h"
#include "game/Player.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/particle/Spark.h"
#include "gui/Menu.h"
#include "gui/Speech.h"
#include "physics/Collisions.h"
#include "scene/GameSound.h"
#include "script/Script.h"
#include "vr/VRBody.h"
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

//! The equipped weapon once it is in the main hand of the player model (after the draw animation), or nullptr
Entity * getHeldWeapon() {

	Entity * weapon = entities.get(player.equiped[EQUIP_SLOT_WEAPON]);
	Entity * io = entities.player();
	if(!weapon || !weapon->obj || !io || !io->obj) {
		return nullptr;
	}

	VertexId hand = io->obj->fastaccess.primary_attach;
	for(const EERIE_LINKED & link : io->obj->linked) {
		if(hand && link.lidx == hand && link.io == weapon) {
			return weapon;
		}
	}

	return nullptr;
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

	const VRHand hand = mainHand();
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
		 * Every hand punches, in and outside of the combat mode (user request), except one holding an object; the
		 * grip on an item still grabs it (see VRGrab). The grip in the world is also a game click, which the game may
		 * take as a drag (isVRGrabbing): only objects really held count.
		 */
		bool free = !((hand == mainHand()) ? isVRGrabbingDirectly() : isVROffHandGrabbing());
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

} // anonymous namespace

bool isVRMeleeActive() {
	return isVRActive() && g_vr && ARX_EQUIPMENT_GetPlayerWeaponType() != WEAPON_BOW;
}

void updateVRCombat() {

	Entity * io = entities.player();
	bool canFight = ARXmenu.mode() == Mode_InGame && !BLOCK_PLAYER_CONTROLS && player.lifePool.current > 0.f;
	if(!isVRActive() || !g_vr || !io || !io->obj || !canFight) {
		resetSwings();
		return;
	}

	float dt = g_vr->getFrameDuration();
	WeaponType type = ARX_EQUIPMENT_GetPlayerWeaponType();

	// The fists punch whenever no weapon is drawn: without a weapon, or with it (or a bow) in its sheath
	bool drawn = (player.Interface & INTER_COMBATMODE) && type != WEAPON_BARE;
	if(!drawn) {
		updateFistSwings(*io, dt);
		return;
	}

	if(type == WEAPON_BOW) {
		resetSwings(); // The game handles bows
		return;
	}

	Entity * weapon = getHeldWeapon();
	if(!weapon) {
		resetSwings();
		return;
	}

	updateWeaponSwing(*io, *weapon, type, dt);

}
