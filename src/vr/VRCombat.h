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

#ifndef ARX_VR_VRCOMBAT_H
#define ARX_VR_VRCOMBAT_H

#include "math/Types.h"
#include "vr/VRInput.h"

class Entity;
struct TransformInfo;

/*!
 * Physical melee in VR: swinging the weapon (or fists) strikes, instead of the game's click and hold strikes.
 *
 * A swing starts when the weapon tip (or the fist) moves fast enough, measured from the controller in the tracking
 * space so walking and turning do not count. During a swing the game's hit check runs every frame with the damage
 * scaled by the speed. One swing damages a target once.
 *
 * Archery: the hand without the bow takes an arrow near the bow, holding its trigger draws the string by pulling the
 * hand back, releasing it shoots along the line from that hand through the bow.
 */

//! \return true if VR handles the combat (the game's strike and bow animations are skipped)
[[nodiscard]] bool isVRMeleeActive();

//! \return true while the bow is drawn: both triggers are used for archery (not for magic or use)
[[nodiscard]] bool isVRArcheryActive();

/*!
 * Physical shield block ([vr] shield_block): a melee hit of an attacker that lands on the player is partly blocked if
 * the shield is held up towards the attacker, see VRCombat
 *
 * \param damage The damage of the hit, lowered by the blocked share
 * \return true if the shield blocked (it made the sound of the hit)
 */
bool blockVRShieldHit(const Entity & attacker, float & damage);

//! Where the light of the torch held in a hand is (its flame), false if there is none (VR: the torch is held, see
//! updateVRCombat; the game lights the player position)
bool getVRTorchLight(Vec3f & position);

//! The torch hanging on the belt (put away at the hip), drawn at the hip: \ref g_playerCarriedItemHook
Entity * getVRCarriedItem(const Entity & entity, TransformInfo & transform);

//! \return true while a hand is at the torch on the belt: its grip takes the torch (not a click or grab)
[[nodiscard]] bool isVRHandAtBeltTorch(VRHand hand);

//! Put the lit torch away, unlit, on the belt (the hands are needed, e.g. for the book)
void hangVRTorchOnBelt();

//! Swing detection and hit checks, in place of ManageCombatModeAnimations - once per frame in combat mode
void updateVRCombat();

//! Forget the held torch and the archery state when the level is cleared (loading a save, changing level)
void resetVRCombatLevelState();

#endif // ARX_VR_VRCOMBAT_H
