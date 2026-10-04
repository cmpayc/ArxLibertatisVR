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

#ifndef ARX_VR_VRMATH_H
#define ARX_VR_VRMATH_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "math/Types.h"

/*!
 * Conversions between OpenXR and Arx conventions
 *
 * OpenXR: right-handed, x right, y up, z backward, meters.
 * Arx view space: x right, y down, z forward, centimeters (world space also has y down).
 *
 * Both are right-handed, so the change of basis is a 180° rotation around the x axis: (x, y, z) -> (x, -y, -z).
 */
namespace vr {

[[nodiscard]] inline Vec3f toArxBasis(const Vec3f & v) {
	return Vec3f(v.x, -v.y, -v.z);
}

//! Conjugate a rotation by the change of basis (the rotation axis is converted like a vector)
[[nodiscard]] inline glm::quat toArxBasis(const glm::quat & q) {
	return glm::quat(q.w, q.x, -q.y, -q.z);
}

//! \return the rotation around the vertical (y) axis of an OpenXR orientation
[[nodiscard]] inline glm::quat yawOnly(const glm::quat & q) {
	Vec3f forward = q * Vec3f(0.f, 0.f, -1.f);
	float yaw = glm::atan(-forward.x, -forward.z);
	return glm::angleAxis(yaw, Vec3f(0.f, 1.f, 0.f));
}

/*!
 * Build a projection matrix for Arx view space from the tangents of the frustum half-angles
 *
 * Matches the conventions of the default Arx projection: clip w = view z and depth is mapped like Direct3D.
 *
 * \param left  tan of the left angle (negative)
 * \param right tan of the right angle (positive)
 * \param up    tan of the up angle (positive)
 * \param down  tan of the down angle (negative)
 */
[[nodiscard]] inline glm::mat4x4 projectionFromTangents(float left, float right, float up, float down,
                                                       float nearDist, float farDist) {

	float q = farDist / (farDist - nearDist);

	glm::mat4x4 m(0.f);
	m[0][0] = 2.f / (right - left);
	m[2][0] = -(right + left) / (right - left);
	// View y points down while clip y points up
	m[1][1] = -2.f / (up - down);
	m[2][1] = -(up + down) / (up - down);
	m[2][2] = q;
	m[3][2] = -q * nearDist;
	m[2][3] = 1.f;

	return m;
}

//! Build a world to view matrix from a view to world rotation and a position
[[nodiscard]] inline glm::mat4x4 worldToViewMatrix(const glm::mat3x3 & viewToWorld, const Vec3f & position) {
	glm::mat3x3 rotation = glm::transpose(viewToWorld);
	glm::mat4x4 m(rotation);
	m[3] = Vec4f(-(rotation * position), 1.f);
	return m;
}

} // namespace vr

#endif // ARX_VR_VRMATH_H
