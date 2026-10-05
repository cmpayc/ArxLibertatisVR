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

#ifndef ARX_VR_VRCAMERA_H
#define ARX_VR_VRCAMERA_H

#include <glm/gtc/quaternion.hpp>

#include "math/Rectangle.h"
#include "math/Types.h"

struct Camera;

/*!
 * Prepare the active camera from the headset pose for the current VR frame
 *
 * The game camera position and yaw are used as the origin of the tracking space. Head movement relative to the
 * recenter pose is applied on top of that. A "center" camera covering both eyes is prepared for culling and
 * CPU-side projection, and the per-eye views are handed to \ref VRSystem for stereo rendering.
 *
 * \param cam      The game camera to use as the tracking origin
 * \param viewport The screen rectangle used for CPU-side projection (the window size)
 *
 * \return false if there is no VR frame to render, in which case the camera was not prepared
 */
bool prepareVRCamera(Camera & cam, const Rect & viewport);

//! Use the current headset pose as the neutral position and orientation
void requestVRRecenter();

//! \return the physical head movement since the last call, converted to world movement of the player
Vec3f takeVRRoomScaleMovement();

//! \return true while the user physically crouches (head well below the height at the last recenter)
bool isVRPhysicallyCrouching();

//! \return the world position between the eyes in the last prepared player frame
bool getVREyeCenter(Vec3f & position);

//! The eye center and the head orientation (Arx basis: x right, y down, z forward, to world) in the last frame
bool getVRHeadPose(Vec3f & position, glm::mat3x3 & rotation);

//! \return the scale of the player model, so that its eyes are at the user's real eye height
float getVRBodyScale();

/*!
 * Convert a ray in the tracking space (OpenXR stage space, e.g. a controller aim pose) to the game world,
 * using the mapping of the last frame prepared by \ref prepareVRCamera.
 *
 * \return false if no frame has been prepared with the player camera
 */
bool trackingRayToWorld(const Vec3f & position, const glm::quat & orientation, Vec3f & origin, Vec3f & direction);

//! Convert a point in the tracking space to the game world, see \ref trackingRayToWorld
bool trackingPointToWorld(const Vec3f & position, Vec3f & world);

/*!
 * Convert a pose in the tracking space to the game world
 *
 * The world orientation maps the pose's axes, converted to the Arx basis (x right, y down, z forward),
 * to world directions.
 */
bool trackingPoseToWorld(const Vec3f & position, const glm::quat & orientation,
                         Vec3f & worldPosition, glm::quat & worldOrientation);
//! Project a world position to the window-sized screen of the last prepared VR frame
bool worldToVRScreen(const Vec3f & position, Vec2f & screen);

//! Forget the mapping, e.g. when the game camera is not the player camera
void invalidateVRTracking();

#endif // ARX_VR_VRCAMERA_H
