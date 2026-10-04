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

#endif // ARX_VR_VRCONTROLS_H
