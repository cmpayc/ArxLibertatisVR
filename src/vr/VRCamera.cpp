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
Vec2f g_consumedOffset(0.f); // horizontal head offset already converted into player movement (x, z)
Vec3f g_roomScaleMovement(0.f); // world movement waiting to be applied to the player
bool g_physicallyCrouching = false;
bool g_standingHeightValid = false;
float g_standingHeight = 0.f; // OpenXR stage space, meters
Vec2f g_previousHead(0.f);
bool g_previousHeadValid = false;
Vec2f g_previousHandsXZ(0.f);
int g_previousHands = 0;
Vec2f g_headVelocity(0.f); // horizontal, m/s
Vec2f g_handsVelocity(0.f);
float g_settleTime = 0.f; // how long the head has been offset without leaning
bool g_uprightHeightValid = false;
float g_uprightHeight = 0.f; // head height when standing upright (stage space, meters, slowly averaged)
bool g_userEyeHeightValid = false;
float g_userEyeHeight = 0.f; // highest the head has been: the user's standing eye height (stage space, meters)
float g_bodyScale = 1.f; // scale of the player model to match the user's height
bool g_snapBodyScale = true; // set the scale at once (the first frame, after loading), else it adapts slowly
float g_crouchDrop = 0.f; // how far the view is lowered for crouching with a button (cm, y down)
const float ModelEyeHeight = 169.f; // eye height of the standing player model (human_base) above its feet, cm
const float ModelHeight = 180.f; // height of the standing player model (the top of its head), cm
const float MinCharacterHeight = 170.f; // cm, the player model is not scaled down further (seated play)
Vec3f g_eyeCenter(0.f); // world position of the eyes in the last prepared player frame
bool g_eyeCenterValid = false;
glm::mat3x3 g_headRotation(1.f); // head orientation (Arx basis: x right, y down, z forward) to world
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

/*!
 * The player body faces where the controllers point (user request, as in other VR games): turning only the head
 * looks around without turning the body. The neck only twists so far: past that the body is turned with the head.
 * Without tracked controllers the body faces where the head looks.
 */
void updateBodyYaw(float headYaw) {

	const float MaxNeckTwist = 45.f; // degrees between the head and the body

	// The horizontal directions of both controllers, each weighted by how level it points
	Vec2f sum(0.f);
	for(VRHand hand : { VRHandLeft, VRHandRight }) {
		const VRHandState & state = g_vr->getInput().getHand(hand);
		Vec3f origin, direction;
		if(state.active && state.aim.valid
		   && trackingRayToWorld(state.aim.position, state.aim.orientation, origin, direction)) {
			sum += Vec2f(direction.x, direction.z);
		}
	}
	float targetYaw = headYaw;
	if(glm::length(sum) > 0.3f) {
		float handsYaw = Camera::getLookAtAngle(Vec3f(0.f), Vec3f(sum.x, 0.f, sum.y)).getYaw();
		targetYaw = headYaw + glm::clamp(vr::wrapDegrees(handsYaw - headYaw), -MaxNeckTwist, MaxNeckTwist);
	}

	float bodyYaw = player.angle.getYaw();
	float difference = vr::wrapDegrees(targetYaw - bodyYaw);

	// The neck limit is kept at once, the rest is smoothed (controllers are less steady than the head)
	float twist = vr::wrapDegrees(headYaw - bodyYaw);
	if(std::abs(twist) > MaxNeckTwist) {
		float overshoot = twist - glm::clamp(twist, -MaxNeckTwist, MaxNeckTwist);
		bodyYaw += overshoot;
		difference -= overshoot;
	}

	float dt = g_vr->getFrameDuration();
	bodyYaw += difference * (1.f - std::exp(-dt * 8.f));
	player.angle.setYaw(MAKEANGLE(bodyYaw));
	player.desiredangle.setYaw(player.angle.getYaw());
	
	g_lastBodyYaw = player.angle.getYaw();
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
	g_roomScaleMovement = Vec3f(0.f);
	g_standingHeightValid = false;
	g_uprightHeightValid = false;
	g_userEyeHeightValid = false;
	g_snapBodyScale = true;
	g_physicallyCrouching = false;
	g_crouchDrop = 0.f;
	g_previousHeadValid = false;
	g_headVelocity = g_handsVelocity = Vec2f(0.f);
	g_settleTime = 0.f;
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
		// Keep the current lean: the feet of the player do not move when recentering
		Vec3f leanStage(0.f);
		if(g_playSpaceValid) {
			Vec3f head = g_referenceYawInverse * (headStage - g_referencePosition);
			Vec2f lean = Vec2f(head.x, head.z) - g_consumedOffset;
			leanStage = glm::inverse(g_referenceYawInverse) * Vec3f(lean.x, 0.f, lean.y);
		}
		g_referencePosition = headStage;
		g_referenceYawInverse = glm::inverse(vr::yawOnly(toQuat(views[0]->pose.orientation)));
		Vec3f lean = g_referenceYawInverse * leanStage;
		g_consumedOffset = -Vec2f(lean.x, lean.z);
		g_playSpaceValid = false;
		g_physicallyCrouching = false;
		g_previousHeadValid = false;
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

	/*
	 * Room-scale: the feet of the player (the body root, g_consumedOffset in the tracking space) stay planted
	 * while the head leans away from them - the spine bends to follow (VRBody). The feet follow the head when
	 * stepping or walking (the controllers move along with the head) or when leaning too far.
	 */
	Vec3f consumed(0.f);
	if(isPlayer) {
		Vec3f head = g_referenceYawInverse * ((toVec(views[0]->pose.position) + toVec(views[1]->pose.position)) * 0.5f
		                                      - g_referencePosition);
		Vec2f headXZ(head.x, head.z);
		const float dt = std::max(g_vr->getFrameDuration(), 0.001f);
		
		/*
		 * Step or lean: when stepping or walking, the controllers move along with the head. When leaning, the
		 * hands stay where they are or move differently. The feet follow the part of the head movement that the
		 * hands share.
		 */
		Vec2f handsXZ(0.f);
		int hands = 0;
		for(size_t hand = 0; hand < VRHandCount; hand++) {
			const VRPose & grip = g_vr->getInput().getHand(VRHand(hand)).grip;
			if(grip.valid) {
				Vec3f position = g_referenceYawInverse * (grip.position - g_referencePosition);
				handsXZ += Vec2f(position.x, position.z);
				hands++;
			}
		}
		if(hands > 0) {
			handsXZ /= float(hands);
		}
		
		Vec2f headDelta = g_previousHeadValid ? headXZ - g_previousHead : Vec2f(0.f);
		// A hand appearing or disappearing is not movement
		Vec2f handsDelta = (g_previousHeadValid && hands > 0 && hands == g_previousHands) ? handsXZ - g_previousHandsXZ
		                                                                                   : Vec2f(0.f);
		float smoothing = 1.f - std::exp(-dt * 7.f);
		g_headVelocity += (headDelta / dt - g_headVelocity) * smoothing;
		g_handsVelocity += (handsDelta / dt - g_handsVelocity) * smoothing;
		g_previousHead = headXZ;
		g_previousHandsXZ = handsXZ;
		g_previousHands = hands;
		g_previousHeadValid = true;
		
		const float MaxLean = 0.3f;    // m
		const float WalkSpeed = 0.45f; // m/s, without tracked controllers
		const float HipToEyes = 0.75f; // m, pivot of a lean
		float headSpeed = glm::length(g_headVelocity);
		Vec2f root = g_consumedOffset;
		
		// Only movement away from the feet can be a step: returning from a lean must never move the feet
		bool movingAway = glm::dot(headDelta, headXZ - root) > 0.f;
		float follow = 0.f;
		if(headSpeed > 0.05f && movingAway) {
			if(hands > 0) {
				follow = glm::clamp(glm::dot(g_handsVelocity, g_headVelocity) / (headSpeed * headSpeed), 0.f, 1.f);
			} else {
				follow = (headSpeed > WalkSpeed) ? 1.f : 0.f;
			}
		}
		root += headDelta * follow;
		
		/*
		 * A lean rotates around the hips and lowers the head, and leaning to the side usually tilts it. An offset
		 * with the head upright (shifting the shoulders, the feet having followed the hands during a lean that has
		 * ended) settles: the feet move back under the head. The drop is measured from the recent standing height,
		 * not the height at the last recenter (standing a little lower than then kept the feet from settling).
		 */
		Vec2f lean = headXZ - root;
		float leanLength = glm::length(lean);
		float expectedDrop = HipToEyes - std::sqrt(std::max(HipToEyes * HipToEyes - leanLength * leanLength, 0.f));
		Vec3f headRight = toQuat(views[0]->pose.orientation) * Vec3f(1.f, 0.f, 0.f);
		bool tilted = std::abs(headRight.y) > std::sin(glm::radians(10.f));
		/*
		 * The upright head height: a slow average (3 s) while the head is level, not the highest the head has been
		 * (seen: 14 cm above the relaxed stance, so every stance counted as a lean). A forward lean held for long
		 * becomes the new upright height and settles.
		 */
		if(!g_uprightHeightValid) {
			g_uprightHeight = headStage.y;
			g_uprightHeightValid = true;
		} else if(!tilted) {
			g_uprightHeight += (headStage.y - g_uprightHeight) * (1.f - std::exp(-dt / 3.f));
		}
		float drop = std::max(g_uprightHeight - headStage.y, 0.f);
		bool leaning = tilted || drop > expectedDrop * 0.5f + 0.02f;
		// Also while still straightening up (user request: the body came back only after a while); once started, all
		// the way under the head
		float settleFrom = (g_settleTime > 0.f) ? 0.01f : 0.04f;
		if(leanLength > settleFrom && !leaning) {
			g_settleTime += dt;
		} else {
			g_settleTime = 0.f;
		}
		if(g_settleTime > 0.15f) {
			root += lean * (1.f - std::exp(-dt * 5.f));
		}
		
		lean = headXZ - root;
		leanLength = glm::length(lean);
		if(leanLength > MaxLean) {
			root = headXZ - lean * (MaxLean / leanLength);
		}		
		Vec2f step = root - g_consumedOffset;
		// Large jumps (e.g. walking around while a menu was open) are not converted into movement
		if(glm::length(step) < 0.5f) {
			g_roomScaleMovement += body * (vr::toArxBasis(Vec3f(step.x, 0.f, step.y)) * scale);
		}
		g_consumedOffset = root;
		consumed = Vec3f(root.x, 0.f, root.y);
		
		// Real crouching (with hysteresis) relative to the standing height
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
		g_eyeCenter = center;
		g_headRotation = eyeRotation[0];
		g_eyeCenterValid = true;
		setVRHeadYaw(cam.angle.getYaw());
		updateBodyYaw(cam.angle.getYaw());
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
	position = g_eyeCenter;
	return g_eyeCenterValid;
}

bool getVRHeadPose(Vec3f & position, glm::mat3x3 & rotation) {
	position = g_eyeCenter;
	rotation = g_headRotation;
	return g_eyeCenterValid;
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