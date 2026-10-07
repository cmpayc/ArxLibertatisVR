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

#ifndef ARX_VR_VRCONTROLS_H
#define ARX_VR_VRCONTROLS_H

#include "math/Types.h"
#include "vr/VRInput.h"

/*!
 * Gameplay controls from the VR controllers
 *
 * Angles use the Arx yaw convention: degrees, positive turns left.
 */

//! \return true while a VR session is active
[[nodiscard]] bool isVRActive();

/*!
 * Map controller buttons to game actions and collect turning input
 *
 * Called once per VR frame after the controllers have been read.
 */
void updateVRControls();

/*!
 * Analog movement from the thumbstick of the off hand
 *
 * \param stick Movement in the controller frame: x right, y forward, length at most 1
 * \param yaw   World yaw of "forward": where the head (or the off hand) points
 *
 * \return true if the stick is outside the dead zone
 */
bool getVRLocomotion(Vec2f & stick, float & yaw);

//! \return the body rotation requested since the last call
float takeVRTurn();

//! Set by the VR camera: the world yaw of the head
void setVRHeadYaw(float yaw);

//! Move the player by the physical (room-scale) head movement, with collision - after the normal movement
void applyVRRoomScale();

//! \return true if a 2D interface (menu, inventory, book, note) is open and the laser should point at it
[[nodiscard]] bool isVRInterfaceOpen();

//! \return true if a weapon is equipped and the hand is at its sheath (on the back, over the hand's shoulder)
[[nodiscard]] bool isVRHandAtSheath(VRHand hand);

//! The hand that drew the weapon, which holds it while it is drawn (the main hand by default)
[[nodiscard]] VRHand getVRWeaponHand();

//! \return true while a hand makes a fist, grip and trigger held together: it punches (see VRCombat)
[[nodiscard]] bool isVRFist(VRHand hand);

/*!
 * Point into the game world with a controller
 *
 * \param aim   Controller aim pose in the tracking space
 * \param mouse Cursor position that makes the game pick what the controller points at
 *
 * \return false if the controller does not point at anything visible
 */
bool getVRWorldPointer(const struct VRPose & aim, Vec2s & mouse);

//! Hide the laser beam (called at the start of each VR frame)
void clearVRPointerBeam();

//! Show the laser beam from the controller to a point on the virtual screen (tracking space)
void setVRPointerBeamToUI(const Vec3f & start, const Vec3f & end);

//! Draw the laser beam into the 3D scene (only while it points at the virtual screen)
void drawVRPointerBeam();

//! \return the interactive object a controller points at (with aim assist), or nullptr; optionally shows the debug ray
class Entity * getVRPointedEntity(const struct VRPose & aim, bool debugRay);

//! The spell drawn is stored instead of cast (the grip held in the magic mode): \ref g_precastSpellHook
bool isVRPrecastRequested();

//! Spells of the player fly along the main hand's pointing ray, not the view: g_playerSpellAimHook
bool getVRSpellAim(Anglef & angle);

//! \return true while the game waits for the target of a spell and the pointer is on a valid one
[[nodiscard]] bool isVRSpellTargetValid();

//! End the player's latest spell that lasts (night vision, armor...), like the game's cancel spell key
bool endVRLastSpell();

#endif // ARX_VR_VRCONTROLS_H
