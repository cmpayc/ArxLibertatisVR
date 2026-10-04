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

#include "vr/VRCamera.h"

#include <algorithm>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "core/Config.h"
#include "game/Camera.h"
#include "graphics/Math.h"
#include "graphics/opengl/OpenGLRenderer.h"
#include "vr/VRMath.h"
#include "vr/VRSystem.h"


namespace {

bool g_recenterRequested = true;
Vec3f g_referencePosition(0.f); // OpenXR stage space, meters
glm::quat g_referenceYawInverse(1.f, 0.f, 0.f, 0.f);

Vec3f toVec(const XrVector3f & v) {
	return Vec3f(v.x, v.y, v.z);
}

glm::quat toQuat(const XrQuaternionf & q) {
	return glm::quat(q.w, q.x, q.y, q.z);
}

} // anonymous namespace

void requestVRRecenter() {
	g_recenterRequested = true;
}

bool prepareVRCamera(Camera & cam, const Rect & viewport) {

	if(!g_vr || !g_vr->isRenderingFrame()) {
		return false;
	}

	const XrView * views[VRSystem::EyeCount] = { &g_vr->getView(0), &g_vr->getView(1) };

	if(g_recenterRequested) {
		g_referencePosition = (toVec(views[0]->pose.position) + toVec(views[1]->pose.position)) * 0.5f;
		g_referenceYawInverse = glm::inverse(vr::yawOnly(toQuat(views[0]->pose.orientation)));
		g_recenterRequested = false;
	}

	// The game camera defines the origin and heading of the tracking space
	const float scale = 100.f * config.vr.worldScale; // meters to centimeters
	const glm::mat3x3 body = glm::transpose(glm::mat3x3(toRotationMatrix(Anglef(0.f, cam.angle.getYaw(), 0.f))));
	const Vec3f origin = cam.m_pos;

	glm::mat3x3 eyeRotation[VRSystem::EyeCount];
	Vec3f eyePosition[VRSystem::EyeCount];
	for(size_t eye = 0; eye < VRSystem::EyeCount; eye++) {
		Vec3f position = g_referenceYawInverse * (toVec(views[eye]->pose.position) - g_referencePosition);
		glm::quat orientation = g_referenceYawInverse * toQuat(views[eye]->pose.orientation);
		eyeRotation[eye] = body * glm::mat3_cast(vr::toArxBasis(orientation));
		eyePosition[eye] = origin + body * (vr::toArxBasis(position) * scale);
	}

	const float nearDist = 1.f;
	const float farDist = cam.cdepth;

	/*
	 * Center camera used for culling and CPU-side projection. It must see everything that either eye sees:
	 * use the union of both frustums and move it back so that the eye offsets are covered.
	 */
	float left = std::min(glm::tan(views[0]->fov.angleLeft), glm::tan(views[1]->fov.angleLeft));
	float right = std::max(glm::tan(views[0]->fov.angleRight), glm::tan(views[1]->fov.angleRight));
	float up = std::max(glm::tan(views[0]->fov.angleUp), glm::tan(views[1]->fov.angleUp));
	float down = std::min(glm::tan(views[0]->fov.angleDown), glm::tan(views[1]->fov.angleDown));

	// Keep square pixels on the virtual screen so that screen-space sprite sizes are not distorted
	float aspect = float(viewport.width()) / float(viewport.height());
	float width = right - left;
	float height = up - down;
	if(width / height < aspect) {
		float extra = (height * aspect - width) / 2.f;
		left -= extra;
		right += extra;
	} else {
		float extra = (width / aspect - height) / 2.f;
		up += extra;
		down -= extra;
	}

	Vec3f center = (eyePosition[0] + eyePosition[1]) * 0.5f;
	Vec3f forward = eyeRotation[0] * Vec3f(0.f, 0.f, 1.f);
	float halfIpd = glm::distance(eyePosition[0], eyePosition[1]) * 0.5f;
	float pullback = halfIpd / std::min(-left, right) + nearDist;

	glm::mat4x4 centerWorldToView = vr::worldToViewMatrix(eyeRotation[0], center - forward * pullback);
	glm::mat4x4 centerViewToClip = vr::projectionFromTangents(left, right, up, down, nearDist, farDist);
	PrepareCamera(&cam, centerWorldToView, centerViewToClip, viewport);

	// Gameplay code (sound listener, portal culling) uses the camera position and angle
	cam.m_pos = center;
	cam.angle = Camera::getLookAtAngle(center, center + forward * 100.f);

	// Views for the stereo renderer: re-project pre-transformed vertices from the center camera into each eye
	glm::dmat4 screenToWorld = glm::inverse(glm::dmat4(g_preparedCamera.m_worldToScreen));
	Vec2i eyeSize = g_vr->getEyeSize();
	std::vector<OpenGLRenderer::StereoView> stereoViews(VRSystem::EyeCount);
	for(size_t eye = 0; eye < VRSystem::EyeCount; eye++) {
		const XrFovf & fov = views[eye]->fov;
		OpenGLRenderer::StereoView & view = stereoViews[eye];
		view.viewport = Rect(s32(eye) * eyeSize.x, 0, s32(eye + 1) * eyeSize.x, eyeSize.y);
		view.view = vr::worldToViewMatrix(eyeRotation[eye], eyePosition[eye]);
		view.projection = vr::projectionFromTangents(glm::tan(fov.angleLeft), glm::tan(fov.angleRight),
		                                             glm::tan(fov.angleUp), glm::tan(fov.angleDown),
		                                             nearDist, farDist);
		view.screenToClip = glm::mat4x4(glm::dmat4(view.projection) * glm::dmat4(view.view) * screenToWorld);
	}
	g_vr->setStereoViews(std::move(stereoViews));

	return true;
}
