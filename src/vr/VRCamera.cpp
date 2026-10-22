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
#include "graphics/Raycast.h"
#include "graphics/StereoView.h"
#include "vr/VRBodyTracking.h"
#include "vr/VRControls.h"
#include "vr/VRMath.h"
#include "vr/VRSystem.h"


namespace {

/*
 * Tracking space: the OpenXR stage space relative to the recenter pose (yaw only), in meters.
 *
 * For the player camera, the tracking space is placed in the world at the player position with the heading of
 * the "play space". The play space turns with the player body when the game turns it (thumbstick, mouse,
 * scripts), while physical body turns affect only the avatar. The upright tracked position anchors the body;
 * room-scale movement follows changes in that absolute stance. The remaining displacement is a lean.
 */
bool g_recenterRequested = true;
Vec3f g_referencePosition(0.f); // OpenXR stage space
glm::quat g_referenceYawInverse(1.f, 0.f, 0.f, 0.f);

bool g_playSpaceValid = false;
float g_playSpaceYaw = 0.f; // degrees, Arx convention
float g_lastBodyYaw = 0.f; // player body yaw when it was last seen
Vec2f g_consumedOffset(0.f); // horizontal head offset already converted into player movement (x, z)
bool g_physicallyCrouching = false;
bool g_standingHeightValid = false;
float g_standingHeight = 0.f; // OpenXR stage space, meters
vr::BodyTracking g_bodyTracking;
bool g_bodyTrackingNeedsReset = true;
XrTime g_lastBodyTrackingTime = 0;
bool g_userEyeHeightValid = false;
float g_userEyeHeight = 0.f; // highest the head has been: the user's standing eye height (stage space, meters)
float g_bodyScale = 1.f; // scale of the player model to match the user's height
bool g_snapBodyScale = true; // set the scale at once (the first frame, after loading), else it adapts slowly
float g_crouchDrop = 0.f; // how far the view is lowered for crouching with a button (cm, y down)
const float ModelEyeHeight = 169.f; // eye height of the standing player model (human_base) above its feet, cm
const float ModelHeight = 180.f; // height of the standing player model (the top of its head), cm
const float MinCharacterHeight = 170.f; // cm, the player model is not scaled down further (seated play)
// Mapping from the tracking space to the game world for the last prepared frame
struct TrackingToWorld {
	bool valid = false;
	Vec3f origin = Vec3f(0.f);
	glm::mat3x3 body = glm::mat3x3(1.f);
	Vec3f consumed = Vec3f(0.f);
	float scale = 100.f;
	glm::mat4x4 worldToScreen = glm::mat4x4(1.f); //!< Center camera, window-sized screen
	Vec3f originFromPlayer = Vec3f(0.f); //!< origin - player.pos, to follow the player before the next frame
};
TrackingToWorld g_trackingToWorld;

Vec3f toVec(const XrVector3f & v) {
	return Vec3f(v.x, v.y, v.z);
}

glm::quat toQuat(const XrQuaternionf & q) {
	return glm::quat(q.w, q.x, q.y, q.z);
}

/*!
 * How far the body is crouched with the crouch button, 0 (standing) to 1: along the animations that crouch and stand
 * up, or the crouch movement (or a lowered collision cylinder, kept under a low ceiling)
 */
float getCrouchProgress() {

	bool crouching = (player.m_currentMovement & PLAYER_CROUCH) || player.physics.cyl.height > player.baseHeight();

	const Entity * io = entities.player();
	if(!io) {
		return crouching ? 1.f : 0.f;
	}
	const AnimLayer & layer = io->animlayer[0];
	bool starting = layer.cur_anim && layer.cur_anim == io->anims[ANIM_CROUCH_START];
	bool ending = layer.cur_anim && layer.cur_anim == io->anims[ANIM_CROUCH_END];
	if((starting || ending) && layer.currentAltAnim() && layer.currentAltAnim()->anim_time > 0ms) {
		float progress = glm::clamp(float(layer.ctime / layer.currentAltAnim()->anim_time), 0.f, 1.f);
		return starting ? progress : 1.f - progress;
	}

	return crouching ? 1.f : 0.f;
}

} // anonymous namespace

void syncVRTrackingToPlayer() {
	
	/*
	 * The player model (and its hands, see VRBody) is animated before the camera of the frame is prepared: bring the
	 * mapping of the last frame up to date with the turns (snap turn) and moves of the player since then, as
	 * prepareVRCamera will, so that the hands are not one frame behind
	 */
	if(!g_trackingToWorld.valid || !g_playSpaceValid) {
		return;
	}
	float bodyYaw = player.angle.getYaw();
	g_playSpaceYaw = MAKEANGLE(g_playSpaceYaw + vr::wrapDegrees(bodyYaw - g_lastBodyYaw));
	g_lastBodyYaw = bodyYaw;
	g_trackingToWorld.body = glm::transpose(glm::mat3x3(toRotationMatrix(Anglef(0.f, g_playSpaceYaw, 0.f))));
	g_trackingToWorld.origin = player.pos + g_trackingToWorld.originFromPlayer;
	g_trackingToWorld.consumed = Vec3f(g_consumedOffset.x, 0.f, g_consumedOffset.y);
}

void requestVRRecenter() {
	g_recenterRequested = true;
	if(g_vr) {
		g_vr->resetUIPlacement();
	}
}

void resetVRTracking() {
	/*
	 * After loading a save or changing level, measure the user again from the current pose (user request: an old
	 * standing height made a seated user crouch): height, size of the player model, the center and no lean
	 */
	g_recenterRequested = true;
	g_playSpaceValid = false;
	g_consumedOffset = Vec2f(0.f);
	g_standingHeightValid = false;
	g_userEyeHeightValid = false;
	g_snapBodyScale = true;
	g_physicallyCrouching = false;
	g_crouchDrop = 0.f;
	g_bodyTrackingNeedsReset = true;
	g_lastBodyTrackingTime = 0;
	g_trackingToWorld.valid = false;
	if(g_vr) {
		g_vr->resetUIPlacement();
	}
}

bool prepareVRCamera(Camera & cam, const Rect & viewport) {

	if(!g_vr || !g_vr->isRenderingFrame()) {
		return false;
	}

	const XrView * views[VRSystem::EyeCount] = { &g_vr->getView(0), &g_vr->getView(1) };
	/*
	 * The player's own view, driven by the head and moving the body (room-scale, IK). Dead, the game's death camera
	 * (rising over the body) is a camera like the scripted ones instead (user request: the body is not controlled).
	 */
	const bool isPlayer = (&cam == &g_playerCamera) && player.lifePool.current > 0.f;

	const Vec3f headStage = (toVec(views[0]->pose.position) + toVec(views[1]->pose.position)) * 0.5f;
	
	/*
	 * Standing height: the highest the head has been recently, slowly adapting (e.g. to sitting down). It is not
	 * taken when recentering, as the user may be bent over the keyboard to press the key.
	 */
	if(!g_standingHeightValid) {
		g_standingHeight = headStage.y;
		g_standingHeightValid = true;
	} else {
		g_standingHeight = std::max(g_standingHeight - 0.002f * g_vr->getFrameDuration(), headStage.y);
	}
	// The user's height does not adapt to sitting down: the player model keeps its size
	if(!g_userEyeHeightValid || headStage.y > g_userEyeHeight) {
		g_userEyeHeight = headStage.y;
		g_userEyeHeightValid = true;
	}
	
	if(g_recenterRequested) {
		// Recenter establishes the user's current position as the stance. Do not
		// carry an old body/head separation into the new tracking origin.
		g_referencePosition = headStage;
		g_referenceYawInverse = glm::inverse(vr::yawOnly(toQuat(views[0]->pose.orientation)));
		g_consumedOffset = Vec2f(0.f);
		g_playSpaceValid = false;
		g_physicallyCrouching = false;
		g_bodyTrackingNeedsReset = true;
		g_lastBodyTrackingTime = 0;
		g_recenterRequested = false;
	}
	g_referencePosition.y = g_standingHeight;

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
		 * Scale the player model (from its feet) so that its eyes are at the user's real eye height: the arms and
		 * shoulders then match the user's reach. The view is at the eyes of the scaled model.
		 */
		// At least 170 cm tall (user request: playing seated made the player too short), the model being about 180 cm
		const float MinBodyScale = MinCharacterHeight / ModelHeight;
		const float MaxBodyScale = 1.25f;
		/*
		 * The eye height of the standing player model (measured, about 169 cm above its feet). It is a constant:
		 * measuring it from the animation picks up jumps, crouching and standing up.
		 */
		const float modelEyeHeight = ModelEyeHeight;
		float targetScale = 1.f;
		if(g_userEyeHeight > 0.5f) {
			targetScale = glm::clamp(g_userEyeHeight * scale / modelEyeHeight, MinBodyScale, MaxBodyScale);
		}
		if(g_snapBodyScale) {
			g_bodyScale = targetScale;
			g_snapBodyScale = false;
		} else {
			g_bodyScale += (targetScale - g_bodyScale) * (1.f - std::exp(-g_vr->getFrameDuration() * 2.f));
		}

		origin = Vec3f(player.pos.x, player.pos.y - player.baseHeight() - g_bodyScale * modelEyeHeight, player.pos.z);
		// Optionally raise the view further (y points down)
		origin.y -= config.vr.heightOffset * 100.f;

		/*
		 * Crouching with a button (or kept crouched under a low ceiling): lower the view like the crouching body,
		 * minus how far the user already bends down. A real crouch moves the view by itself.
		 * The game lowers the collision cylinder only when the crouch animation ends (the view followed about a
		 * second after the body, user), and a fixed speed was faster than the body: the view follows the progress of
		 * the crouch animations.
		 */
		float crouchDrop = 0.f;
		float crouched = getCrouchProgress();
		if(crouched > 0.f && !g_physicallyCrouching) {
			float physicalDrop = std::max(g_standingHeight - headStage.y, 0.f) * scale;
			float bodyDrop = (player.crouchHeight() - player.baseHeight()) * g_bodyScale;
			// Scaled as a whole: subtracting the user's drop along the way held the view still for the first 40%
			crouchDrop = std::max(bodyDrop - physicalDrop, 0.f) * crouched;
		}
		g_crouchDrop += (crouchDrop - g_crouchDrop) * (1.f - std::exp(-g_vr->getFrameDuration() * 20.f));
		origin.y += g_crouchDrop;
	} else {
		// Scripted cameras: the camera defines the origin and heading
		heading = cam.angle.getYaw();
		origin = cam.m_pos;
	}
	const glm::mat3x3 body = glm::transpose(glm::mat3x3(toRotationMatrix(Anglef(0.f, heading, 0.f))));

	// The body root was estimated and moved through collision before animation.
	// Subtract exactly the same physical offset from the head and both hands.
	Vec3f consumed = isPlayer ? Vec3f(g_consumedOffset.x, 0.f, g_consumedOffset.y) : Vec3f(0.f);
	if(!isPlayer) {
		g_bodyTrackingNeedsReset = true;
	}
	glm::mat3x3 eyeRotation[VRSystem::EyeCount];
	Vec3f eyePosition[VRSystem::EyeCount];
	for(size_t eye = 0; eye < VRSystem::EyeCount; eye++) {
		Vec3f position = g_referenceYawInverse * (toVec(views[eye]->pose.position) - g_referencePosition) - consumed;
		glm::quat orientation = g_referenceYawInverse * toQuat(views[eye]->pose.orientation);
		eyeRotation[eye] = body * glm::mat3_cast(vr::toArxBasis(orientation));
		eyePosition[eye] = origin + body * (vr::toArxBasis(position) * scale);
	}
	
	// Leaning must not put the head through walls: stop the view in front of them
	if(isPlayer) {
		Vec3f eyes = (eyePosition[0] + eyePosition[1]) * 0.5f;
		Vec3f lean = eyes - origin;
		float leanLength = glm::length(lean);
		if(leanLength > 1.f) {
			if(RaycastResult hit = raycastScene(origin, eyes + lean / leanLength * 10.f, POLY_TRANS, RaycastIgnorePlayer)) {
				float allowed = std::max(glm::distance(origin, hit.pos) - 10.f, 0.f);
				if(allowed < leanLength) {
					Vec3f correction = lean / leanLength * (leanLength - allowed);
					eyePosition[0] -= correction;
					eyePosition[1] -= correction;
				}
			}
		}
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
	g_trackingToWorld.originFromPlayer = origin - player.pos;
	g_trackingToWorld.body = body;
	g_trackingToWorld.consumed = consumed;
	g_trackingToWorld.scale = scale;
	g_trackingToWorld.worldToScreen = g_preparedCamera.m_worldToScreen;

	// Gameplay code (sound listener, portal culling) uses the camera position and angle
	cam.m_pos = center;
	cam.angle = Camera::getLookAtAngle(center, center + forward * 100.f);

	if(isPlayer) {
		setVRHeadYaw(cam.angle.getYaw());
	}

	// Views for the stereo renderer: re-project pre-transformed vertices from the center camera into each eye
	glm::dmat4 screenToWorld = glm::inverse(glm::dmat4(g_preparedCamera.m_worldToScreen));
	Vec2i eyeSize = g_vr->getEyeSize();
	/*
	 * Bow zoom (user request): the game zooms in while the bow is drawn (focal + 177.5 x the draw ratio, about 1.5x).
	 * The eyes are rendered with a narrower field of view, which the headset shows over its full field of view: the
	 * whole view is magnified. The game lowers the ratio smoothly after the shot.
	 */
	float zoom = isPlayer ? 1.f + 0.5f * glm::clamp(player.m_bowAimRatio, 0.f, 1.f) : 1.f;

	std::vector<StereoView> stereoViews(VRSystem::EyeCount);
	for(size_t eye = 0; eye < VRSystem::EyeCount; eye++) {
		const XrFovf & fov = views[eye]->fov;
		StereoView & view = stereoViews[eye];
		view.viewport = Rect(s32(eye) * eyeSize.x, 0, s32(eye + 1) * eyeSize.x, eyeSize.y);
		view.view = vr::worldToViewMatrix(eyeRotation[eye], eyePosition[eye]);
		view.projection = vr::projectionFromTangents(glm::tan(fov.angleLeft) / zoom, glm::tan(fov.angleRight) / zoom,
		                                             glm::tan(fov.angleUp) / zoom, glm::tan(fov.angleDown) / zoom,
		                                             nearDist, farDist);
		view.screenToClip = glm::mat4x4(glm::dmat4(view.projection) * glm::dmat4(view.view) * screenToWorld);
	}
	g_vr->setStereoViews(std::move(stereoViews));

	return true;
}

Vec3f takeVRRoomScaleMovement() {
	if(!g_vr || !g_vr->isRenderingFrame() || !g_vr->isFocused()
	   || !g_trackingToWorld.valid || g_cameraEntity || player.lifePool.current <= 0.f || g_recenterRequested) {
		g_bodyTrackingNeedsReset = true;
		return Vec3f(0.f);
	}
	const XrTime time = g_vr->getTrackingTime();
	if(time == g_lastBodyTrackingTime && !g_bodyTrackingNeedsReset) {
		return Vec3f(0.f);
	}
	syncVRTrackingToPlayer();
	vr::BodyObservation pose;
	// Keep physical heights in stage space. The camera's standing-height origin
	// adapts separately; its updates must not look like bending to the estimator.
	const Vec3f reference(g_referencePosition.x, 0.f, g_referencePosition.z);
	pose.head = g_referenceYawInverse * ((toVec(g_vr->getView(0).pose.position)
	                                     + toVec(g_vr->getView(1).pose.position)) * 0.5f - reference);
	pose.orientation = g_referenceYawInverse * toQuat(g_vr->getView(0).pose.orientation);
	for(size_t hand = 0; hand < VRHandCount; hand++) {
		const VRHandState & state = g_vr->getInput().getHand(VRHand(hand));
		pose.tracked[hand] = state.active && state.grip.valid;
		pose.hands[hand] = g_referenceYawInverse * (state.grip.position - reference);
		pose.rotations[hand] = g_referenceYawInverse * state.grip.orientation;
	}
	float dt = g_lastBodyTrackingTime ? float(time - g_lastBodyTrackingTime) * 1e-9f : 0.f;
	bool reset = g_bodyTrackingNeedsReset || dt <= 0.f || dt > 0.25f;
	g_lastBodyTrackingTime = time;
	if(reset) {
		// Preserve the recentered root, but discard movement made during a pause.
		if(dt > 0.25f) {
			g_consumedOffset = Vec2f(pose.head.x, pose.head.z);
		}
		g_bodyTracking.reset(pose, g_consumedOffset,
		                     vr::wrapDegrees(player.angle.getYaw() - g_playSpaceYaw));
		g_bodyTrackingNeedsReset = false;
		return Vec3f(0.f);
	}
	float previousYaw = g_bodyTracking.yaw();
	bool continuous = g_bodyTracking.update(pose, dt);
	Vec2f step = g_bodyTracking.root() - g_consumedOffset;
	g_consumedOffset = g_bodyTracking.root();
	g_trackingToWorld.consumed = Vec3f(g_consumedOffset.x, 0.f, g_consumedOffset.y);
	if(!continuous) {
		return Vec3f(0.f);
	}
	// Physical turns rotate the avatar only; sync must not rotate tracking space again.
	player.angle.setYaw(MAKEANGLE(player.angle.getYaw() + g_bodyTracking.yaw() - previousYaw));
	player.desiredangle.setYaw(player.angle.getYaw());
	g_lastBodyYaw = player.angle.getYaw();
	float headDrop = pose.head.y - g_standingHeight;
	if(headDrop < -0.35f) {
		g_physicallyCrouching = true;
	} else if(headDrop > -0.25f) {
		g_physicallyCrouching = false;
	}
	return g_trackingToWorld.body * (vr::toArxBasis(Vec3f(step.x, 0.f, step.y)) * g_trackingToWorld.scale);
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
	g_bodyTrackingNeedsReset = true;
}

bool isVRPlayerView() {
	return g_trackingToWorld.valid;
}

bool isVRPhysicallyCrouching() {
	return g_physicallyCrouching;
}

float getVRBodyScale() {
	return g_bodyScale;
}

bool getVREyeCenter(Vec3f & position) {
	glm::mat3x3 rotation;
	return getVRHeadPose(position, rotation);
}

bool getVRHeadPose(Vec3f & position, glm::mat3x3 & rotation) {
	if(!g_vr || !g_vr->isRenderingFrame() || !g_trackingToWorld.valid) {
		return false;
	}
	Vec3f eyes = (toVec(g_vr->getView(0).pose.position) + toVec(g_vr->getView(1).pose.position)) * 0.5f;
	glm::quat orientation;
	if(!trackingPoseToWorld(eyes, toQuat(g_vr->getView(0).pose.orientation), position, orientation)) {
		return false;
	}
	rotation = glm::mat3_cast(orientation);
	// Match the camera's wall constraint, including before the camera is prepared.
	Vec3f lean = position - g_trackingToWorld.origin;
	float distance = glm::length(lean);
	if(distance > 1.f) {
		if(RaycastResult hit = raycastScene(g_trackingToWorld.origin, position + lean / distance * 10.f,
		                                   POLY_TRANS, RaycastIgnorePlayer)) {
			float allowed = std::max(glm::distance(g_trackingToWorld.origin, hit.pos) - 10.f, 0.f);
			position = g_trackingToWorld.origin + lean * (std::min(distance, allowed) / distance);
		}
	}
	return true;
}

bool trackingPoseToWorld(const Vec3f & position, const glm::quat & orientation,
                         Vec3f & worldPosition, glm::quat & worldOrientation) {
	
	if(!trackingPointToWorld(position, worldPosition)) {
		return false;
	}
	
	glm::quat relative = g_referenceYawInverse * orientation;
	worldOrientation = glm::quat_cast(g_trackingToWorld.body * glm::mat3_cast(vr::toArxBasis(relative)));

	return true;
}

bool worldPoseToTracking(const Vec3f & worldPosition, const glm::quat & worldOrientation,
                         Vec3f & position, glm::quat & orientation) {

	const TrackingToWorld & t = g_trackingToWorld;
	if(!t.valid || t.scale <= 0.f) {
		return false;
	}

	glm::mat3x3 worldToBody = glm::inverse(t.body);
	glm::quat referenceYaw = glm::inverse(g_referenceYawInverse);

	Vec3f relative = vr::toArxBasis(worldToBody * (worldPosition - t.origin) / t.scale);
	position = referenceYaw * (relative + t.consumed) + g_referencePosition;

	glm::quat relativeOrientation = vr::toArxBasis(glm::quat_cast(worldToBody * glm::mat3_cast(worldOrientation)));
	orientation = glm::normalize(referenceYaw * relativeOrientation);

	return true;
}
