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

#ifndef ARX_GRAPHICS_STEREOVIEW_H
#define ARX_GRAPHICS_STEREOVIEW_H

#include "math/Rectangle.h"
#include "math/Types.h"

/*!
 * One eye of a stereo render target (VR): every draw call of the 3D scene is replicated into each view
 *
 * Pre-transformed vertices are projected with a single "center" camera and are re-projected into each view.
 */
struct StereoView {
	Rect viewport; //!< In render target coordinates (top-left origin)
	glm::mat4x4 view; //!< World to view space for transformed vertices
	glm::mat4x4 projection; //!< View to clip space for transformed vertices
	glm::mat4x4 screenToClip; //!< Screen to clip space for pre-transformed vertices
};

#endif // ARX_GRAPHICS_STEREOVIEW_H
