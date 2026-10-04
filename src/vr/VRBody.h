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

#ifndef ARX_VR_VRBODY_H
#define ARX_VR_VRBODY_H

#include "math/Types.h"

class Entity;
struct Skeleton;

/*!
 * Player body in VR: the player model driven by the headset and controllers (inverse kinematics)
 */

/*!
 * Pose the player skeleton from the headset and controllers (inverse kinematics)
 *
 * Registered as \ref g_playerSkeletonHook: called after the animation, before the mesh is skinned.
 */
void applyVRBodyIK(Entity & player, Skeleton & skeleton);

//! \return where the player model is drawn relative to the player position in VR
Vec3f getVRModelOffset();

//! Write the player skeleton (bones, hierarchy, attach points) to the log once, for development
void logVRPlayerSkeleton(const Entity & player);

#endif // ARX_VR_VRBODY_H
