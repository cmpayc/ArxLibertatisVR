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

/*!
 * Physical melee in VR: swinging the weapon (or fists) strikes, instead of the game's click and hold strikes.
 *
 * A swing starts when the weapon tip (or the fist) moves fast enough, measured from the controller in the tracking
 * space so walking and turning do not count. During a swing the game's hit check runs every frame with the damage
 * scaled by the speed. One swing damages a target once. Bows keep the game's handling.
 */

//! \return true if VR handles the melee of the current weapon (the game's strike animations are skipped)
[[nodiscard]] bool isVRMeleeActive();

//! Swing detection and hit checks, in place of ManageCombatModeAnimations - once per frame in combat mode
void updateVRCombat();

#endif // ARX_VR_VRCOMBAT_H
