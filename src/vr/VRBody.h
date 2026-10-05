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

#include <vector>

#include <glm/gtc/quaternion.hpp>

#include "math/Types.h"
#include "vr/VRInput.h"

class Entity;
struct EERIE_LINKED;
struct TransformInfo;
struct Skeleton;
struct EERIE_3DOBJ;

/*!
 * Player body in VR: the player model driven by the headset and controllers (inverse kinematics)
 */

/*!
 * Pose the player skeleton from the headset and controllers (inverse kinematics)
 *
 * Registered as \ref g_playerSkeletonHook: called after the animation, before the mesh is skinned.
 */
void applyVRBodyIK(Entity & player, Skeleton & skeleton);

/*!
 * The wrist joint of the posed player model and the controller orientation it follows (Arx basis, to world),
 * in the last frame - to attach things to the visible body
 */
bool getVRWristPose(VRHand hand, Vec3f & position, glm::quat & gripOrientation);

//! The hip joint of the posed player model on the given side, in the world, in the last frame
bool getVRHipPosition(VRHand side, Vec3f & position);

/*!
 * The bones that animation layers above the base layer (combat, spells) may move: only the hands and fingers, so that
 * the body keeps its normal pose and the fingers still hold the weapon. The arms follow the controllers.
 *
 * Registered as \ref g_playerUpperLayerBones.
 */
const std::vector<bool> * getVRUpperLayerBones(const EERIE_3DOBJ & object);

//! Where the hilt of the sheathed weapon is, in the world: at the hip opposite to the main hand
bool getVRSheathHilt(Vec3f & hilt);

/*!
 * Draw the sheathed weapon at the hip opposite to the main hand (where it is drawn from, see VRControls)
 *
 * Registered as \ref g_playerSheathHook.
 */
bool getVRSheathTransform(const Entity & entity, const EERIE_LINKED & link, TransformInfo & transform);

//! \return where the player model is drawn relative to the player position in VR
Vec3f getVRModelOffset();

#endif // ARX_VR_VRBODY_H
