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
#include <cmath>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "core/Config.h"
#include "game/Camera.h"
#include "game/Player.h"
#include "graphics/Math.h"
#include "graphics/opengl/OpenGLRenderer.h"
#include "vr/VRControls.h"
#include "vr/VRMath.h"
#include "vr/VRSystem.h"


namespace {

/*
 * Tracking space: the OpenXR stage space relative to the recenter pose (yaw only), in meters.
 *
 * For the player camera, the tracking space is placed in the world at the player position with the heading of
 * the "play space". The play space turns with the player body when the game turns it (thumbstick, mouse,
 * scripts), while the body itself follows the head. Horizontal head movement is converted into player movement
 * (room-scale) and accumulated in the consumed offset, so that the view stays above the player's body.
 */
bool g_recenterRequested = true;
Vec3f g_referencePosition(0.f); // OpenXR stage space
glm::quat g_referenceYawInverse(1.f, 0.f, 0.f, 0.f);

bool g_playSpaceValid = false;
float g_playSpaceYaw = 0.f; // degrees, Arx convention
float g_lastBodyYaw = 0.f; // player body yaw when it was last seen
bool g_bodyTurning = false;
Vec2f g_consumedOffset(0.f); // horizontal head offset already converted into player movement (x, z)
Vec3f g_roomScaleMovement(0.f); // world movement waiting to be applied to the player
bool g_physicallyCrouching = false;
bool g_standingEyeValid = false;
float g_standingEyeOffset = 0.f; // eye height above player.pos while standing (game units, y down)
Vec3f g_eyeCenter(0.f); // world position of the eyes in the last prepared player frame
bool g_eyeCenterValid = false;
Vec3f g_bodyOffset(0.f); // where the player model is drawn relative to the player position
// Mapping from the tracking space to the game world for the last prepared frame
struct TrackingToWorld {
	bool valid = false;
	Vec3f origin = Vec3f(0.f);
	glm::mat3x3 body = glm::mat3x3(1.f);
	Vec3f consumed = Vec3f(0.f);
	float scale = 100.f;
	glm::mat4x4 worldToScreen = glm::mat4x4(1.f); //!< Center camera, window-sized screen
};
TrackingToWorld g_trackingToWorld;

Vec3f toVec(const XrVector3f & v) {
	return Vec3f(v.x, v.y, v.z);
}

glm::quat toQuat(const XrQuaternionf & q) {
	return glm::quat(q.w, q.x, q.y, q.z);
}

//! Turn the player body towards the head once the head looks too far to the side, or while moving
void updateBodyYaw(float headYaw) {

	float bodyYaw = player.angle.getYaw();
	float difference = vr::wrapDegrees(headYaw - bodyYaw);

	Vec2f stick;
	float movementYaw;
	bool moving = getVRLocomotion(stick, movementYaw);

	if(std::abs(difference) > 45.f || moving) {
		g_bodyTurning = true;
	}
	if(g_bodyTurning) {
		float dt = g_vr->getFrameDuration();
		bodyYaw += difference * (1.f - std::exp(-dt * 6.f));
		if(std::abs(difference) < 3.f && !moving) {
			g_bodyTurning = false;
		}
		player.angle.setYaw(MAKEANGLE(bodyYaw));
		player.desiredangle.setYaw(player.angle.getYaw());
	}

	g_lastBodyYaw = player.angle.getYaw();
}

} // anonymous namespace

void requestVRRecenter() {
	g_recenterRequested = true;
	if(g_vr) {
		g_vr->resetUIPlacement();
	}
}

bool prepareVRCamera(Camera & cam, const Rect & viewport) {

	if(!g_vr || !g_vr->isRenderingFrame()) {
		return false;
	}

	const XrView * views[VRSystem::EyeCount] = { &g_vr->getView(0), &g_vr->getView(1) };
	const bool isPlayer = (&cam == &g_playerCamera);

	if(g_recenterRequested) {
		g_referencePosition = (toVec(views[0]->pose.position) + toVec(views[1]->pose.position)) * 0.5f;
		g_referenceYawInverse = glm::inverse(vr::yawOnly(toQuat(views[0]->pose.orientation)));
		g_consumedOffset = Vec2f(0.f);
		g_playSpaceValid = false;
		g_physicallyCrouching = false;
		g_recenterRequested = false;
	}

	const float scale = 100.f * config.vr.worldScale; // meters to centimeters

	// Place the tracking space in the world
	float heading;
	Vec3f origin;
	if(isPlayer) {
		float bodyYaw = player.angle.getYaw();
		if(!g_playSpaceValid) {
			g_playSpaceYaw = bodyYaw;
			g_lastBodyYaw = bodyYaw;
			g_playSpaceValid = true;
		}
		// Turns applied to the body by the game (thumbstick, mouse, scripts) also turn the play space
		g_playSpaceYaw = MAKEANGLE(g_playSpaceYaw + vr::wrapDegrees(bodyYaw - g_lastBodyYaw));
		heading = g_playSpaceYaw;
		/*
		 * Above the body center at eye height: the head vertex of the player model swings around when it turns.
		 * While crouching, keep the standing eye height: the real head already lowers the view.
		 */
		float eyeOffset = g_playerCameraStablePos.y - player.pos.y;
		if(!(player.m_currentMovement & PLAYER_CROUCH)) {
			if(!g_standingEyeValid || std::abs(eyeOffset - g_standingEyeOffset) > 50.f) {
				// First frame or a large change (level load, teleport): take the value directly
				g_standingEyeOffset = eyeOffset;
				g_standingEyeValid = true;
			} else {
				// Smoothed, so that the stand-up animation does not cause a dip
				g_standingEyeOffset += (eyeOffset - g_standingEyeOffset) * (1.f - std::exp(-g_vr->getFrameDuration() * 2.f));
			}
		}
		origin = Vec3f(player.pos.x, player.pos.y + (g_standingEyeValid ? g_standingEyeOffset : eyeOffset), player.pos.z);
	} else {
		// Scripted cameras: the camera defines the origin and heading
		heading = cam.angle.getYaw();
		origin = cam.m_pos;
	}
	const glm::mat3x3 body = glm::transpose(glm::mat3x3(toRotationMatrix(Anglef(0.f, heading, 0.f))));

	// Room-scale: horizontal head movement moves the player body, the view stays above it
	Vec3f consumed(0.f);
	if(isPlayer) {
		Vec3f head = g_referenceYawInverse * ((toVec(views[0]->pose.position) + toVec(views[1]->pose.position)) * 0.5f
		                                      - g_referencePosition);
		Vec2f pending = Vec2f(head.x, head.z) - g_consumedOffset;
		// Large jumps (e.g. walking around while a menu was open) are not converted into movement
		if(glm::length(pending) < 0.5f) {
			g_roomScaleMovement += body * (vr::toArxBasis(Vec3f(pending.x, 0.f, pending.y)) * scale);
		}
		g_consumedOffset += pending;
		consumed = Vec3f(g_consumedOffset.x, 0.f, g_consumedOffset.y);
		
		// Real crouching (with hysteresis) relative to the head height when recentering standing up
		if(head.y < -0.35f) {
			g_physicallyCrouching = true;
		} else if(head.y > -0.25f) {
			g_physicallyCrouching = false;
		}
	}

	glm::mat3x3 eyeRotation[VRSystem::EyeCount];
	Vec3f eyePosition[VRSystem::EyeCount];
	for(size_t eye = 0; eye < VRSystem::EyeCount; eye++) {
		Vec3f position = g_referenceYawInverse * (toVec(views[eye]->pose.position) - g_referencePosition) - consumed;
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

	// Pointing into the world only makes sense from the player's own point of view
	g_trackingToWorld.valid = isPlayer;
	g_trackingToWorld.origin = origin;
	g_trackingToWorld.body = body;
	g_trackingToWorld.consumed = consumed;
	g_trackingToWorld.scale = scale;
	g_trackingToWorld.worldToScreen = g_preparedCamera.m_worldToScreen;

	// Gameplay code (sound listener, portal culling) uses the camera position and angle
	cam.m_pos = center;
	cam.angle = Camera::getLookAtAngle(center, center + forward * 100.f);

	if(isPlayer) {
		g_eyeCenter = center;
		g_eyeCenterValid = true;
		setVRHeadYaw(cam.angle.getYaw());
		updateBodyYaw(cam.angle.getYaw());
	}

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

Vec3f takeVRRoomScaleMovement() {
	Vec3f movement = g_roomScaleMovement;
	g_roomScaleMovement = Vec3f(0.f);
	return movement;
}

bool trackingPointToWorld(const Vec3f & position, Vec3f & world) {

	if(!g_trackingToWorld.valid) {
		return false;
	}

	const TrackingToWorld & t = g_trackingToWorld;
	Vec3f relative = g_referenceYawInverse * (position - g_referencePosition) - t.consumed;
	world = t.origin + t.body * (vr::toArxBasis(relative) * t.scale);

	return true;
}

bool trackingRayToWorld(const Vec3f & position, const glm::quat & orientation, Vec3f & origin, Vec3f & direction) {

	if(!trackingPointToWorld(position, origin)) {
		return false;
	}

	Vec3f forward = g_referenceYawInverse * (orientation * Vec3f(0.f, 0.f, -1.f));
	direction = glm::normalize(g_trackingToWorld.body * vr::toArxBasis(forward));

	return true;
}

bool worldToVRScreen(const Vec3f & position, Vec2f & screen) {

	if(!g_trackingToWorld.valid) {
		return false;
	}

	Vec4f p = g_trackingToWorld.worldToScreen * Vec4f(position, 1.f);
	if(p.w <= 0.f) {
		return false;
	}

	screen = Vec2f(p) / p.w;
	return true;
}

void invalidateVRTracking() {
	g_trackingToWorld.valid = false;
}

bool isVRPhysicallyCrouching() {
	return g_physicallyCrouching;
}
Vec3f getVRBodyOffset() {
	return g_bodyOffset;
}

void updateVRBodyOffset(const Vec3f & headVertex) {
	
	if(!g_eyeCenterValid) {
		g_bodyOffset = Vec3f(0.f);
		return;
	}
	
	// Keep the head of the model about 10 cm behind the eyes (horizontally), where the head of the user is
	Vec3f forward = angleToVectorXZ(player.angle.getYaw());
	Vec3f target = g_eyeCenter - forward * 10.f;
	Vec3f error = target - headVertex;
	error.y = 0.f;
	
	g_bodyOffset += error;
	
	// After teleports or with a bad pose, fall back to the normal model placement
	if(glm::length(g_bodyOffset) > 80.f) {
		g_bodyOffset = Vec3f(0.f);
	}
	
}