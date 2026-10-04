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

#include "math/Rectangle.h"

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

#endif // ARX_VR_VRCAMERA_H
