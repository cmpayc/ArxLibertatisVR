/*
 * Copyright 2026 Arx Libertatis VR contributors <kazakov.s.fl@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARX_VR_VRBODYTRACKING_H
#define ARX_VR_VRBODYTRACKING_H

#include <array>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace vr {

// All observations stay in OpenXR space (meters, Y up). Game turns and collisions
// must never feed back into the evidence for physical movement.
struct BodyObservation {
	glm::vec3 head = glm::vec3(0.f);
	glm::quat orientation = glm::quat(1.f, 0.f, 0.f, 0.f);
	std::array<glm::vec3, 2> hands = { glm::vec3(0.f), glm::vec3(0.f) };
	std::array<glm::quat, 2> rotations = { glm::quat(1.f, 0.f, 0.f, 0.f), glm::quat(1.f, 0.f, 0.f, 0.f) };
	std::array<bool, 2> tracked = {};
};

class BodyTracking {
public:
	// Rebase after recenter, a tracking discontinuity, or resuming gameplay.
	void reset(const BodyObservation & pose, glm::vec2 root, float bodyYaw = 0.f) {
		*this = BodyTracking();
		m_previous = pose;
		m_root = root;
		// Keep the avatar's heading in tracking space, including across pauses.
		// A reset must not calibrate a missed physical turn as the new neutral.
		m_yaw = bodyYaw;
		m_uprightNeckHeight = neck(pose).y;
		m_uprightValid = isLevel(pose); // Looking down puts the neck estimate up to 7 cm too high (log)
		m_valid = true;
	}

	// False means the new root is a rebase, not displacement to apply to physics.
	bool update(const BodyObservation & pose, float dt) {
		if(!m_valid) {
			reset(pose, horizontal(pose.head));
			return false;
		}
		glm::vec2 delta = horizontal(neck(pose) - neck(m_previous));
		// Never interpret a runtime relocation or a long absence as walking.
		if(dt <= 0.f || dt > 0.25f || glm::length(delta) > 0.25f
		   || std::abs(yawChange(m_previous.orientation, pose.orientation)) > 45.f) {
			reset(pose, m_root + horizontal(pose.head - m_previous.head), m_yaw);
			return false;
		}
		const float filter = 1.f - std::exp(-dt / 0.08f);
		float headTurn = yawChange(m_previous.orientation, pose.orientation);
		float turnFilter = headTurn * m_headSpeed < 0.f ? 1.f : filter;
		m_headSpeed += (headTurn / dt - m_headSpeed) * turnFilter;
		bool both = true;
		bool turning = std::abs(headTurn / dt) > 0.5f && headTurn * m_headSpeed > 0.f;
		for(size_t hand = 0; hand < 2; hand++) {
			bool continuous = pose.tracked[hand] && m_previous.tracked[hand]
			                  && glm::distance(pose.hands[hand], m_previous.hands[hand]) < 0.35f;
			both = both && continuous;
			float speed = continuous ? yawChange(m_previous.rotations[hand], pose.rotations[hand]) / dt : 0.f;
			m_handSpeed[hand] += (speed - m_handSpeed[hand]) * turnFilter;
			float share = std::abs(m_headSpeed) > 0.5f ? m_handSpeed[hand] / m_headSpeed : 0.f;
			turning = turning && continuous && speed * headTurn > 0.f && share > 0.45f && share < 2.2f;
		}

		// Accumulate displacement, not speed: slow deliberate turns count too.
		// Shared angular motion gives immediate response during coordinated turns.
		if(turning) {
			m_turnIdle = 0.f;
			m_pendingYaw += headTurn;
			if(m_turning || std::abs(m_pendingYaw) >= 5.f) {
				m_yaw += m_pendingYaw;
				m_pendingYaw = 0.f;
				m_turning = true;
			}
		} else if((m_turnIdle += dt) > 0.12f || !both) {
			m_pendingYaw = 0.f;
			m_turning = false;
		}
		recoverHeading(pose, both, dt);

		// A stance is an absolute position, not an integral of whichever head
		// deltas happened to agree with the hands. In an upright posture the
		// user is the center even after short steps, pauses or hand gestures.
		glm::quat facing = glm::angleAxis(glm::radians(m_yaw), glm::vec3(0.f, 1.f, 0.f));
		glm::vec3 offset = facing * glm::vec3(0.f, 0.f, 0.08f);
		glm::vec2 center = horizontal(neck(pose) - offset);
		float distance = glm::length(center - m_root);
		float neckHeight = neck(pose).y;
		glm::vec3 right = pose.orientation * glm::vec3(1.f, 0.f, 0.f);
		/*
		 * Learn the relaxed height over the stance, never while a lean is held away from it, and only while looking
		 * about level: the neck estimate rises when looking down (60°: above the eyes), and a height learned so stayed
		 * 5 cm too high, every upright step was a bend and the feet stayed up to the maximum lean behind (log).
		 * Standing still and upright away from the feet for a while also counts as the stance, so that a height learned
		 * too high cannot keep them there.
		 */
		bool level = isLevel(pose);
		bool still = glm::length(delta) / dt < 0.03f && std::abs(right.y) < std::sin(glm::radians(8.f));
		m_stillTime = (level && still) ? m_stillTime + dt : 0.f;
		if(level && !m_uprightValid) {
			m_uprightNeckHeight = neckHeight;
			m_uprightValid = true;
		} else if(level && (distance < 0.02f || m_stillTime > 2.f)) {
			m_uprightNeckHeight += (neckHeight - m_uprightNeckHeight) * (1.f - std::exp(-dt * 2.f));
		}
		// While the head moves independently, start with a torso lean. Once the
		// motion stops, an upright posture must still recover the actual center.
		bool independentHeadMotion = both && glm::length(delta) / dt > 0.006f;
		for(size_t hand = 0; hand < 2; hand++) {
			glm::vec2 handDelta = horizontal(pose.hands[hand] - m_previous.hands[hand]);
			independentHeadMotion = independentHeadMotion
			                        && glm::dot(handDelta, delta) <= 0.25f * glm::dot(delta, delta);
		}
		/*
		 * The hands decide (user): when they came along with the head, relative to the neck where they were while the
		 * feet were under it, this is a step, not a bend - the neck being lower than the learned upright height
		 * alone kept the feet planted up to the maximum lean while the user stood 5 cm lower than at the rebase (log).
		 * The horizontal distance of each hand from the neck: it does not change when turning. Relative to the body
		 * heading, a heading lagging 20° behind the user moved the hands by 14 cm and every step was a bend (log).
		 */
		bool handsAlong = false;
		if(both) {
			glm::vec2 handDistance(glm::length(horizontal(pose.hands[0] - neck(pose))),
			                       glm::length(horizontal(pose.hands[1] - neck(pose))));
			if(m_handsFromNeckValid) {
				glm::vec2 change = glm::abs(handDistance - m_handsFromNeck);
				handsAlong = std::max(change.x, change.y) < 0.08f + 0.25f * distance;
			}
			// The arms move freely while the feet are under the neck
			if(distance < 0.02f || !m_handsFromNeckValid) {
				float rate = m_handsFromNeckValid ? 1.f - std::exp(-dt * 4.f) : 1.f;
				m_handsFromNeck += (handDistance - m_handsFromNeck) * rate;
				m_handsFromNeckValid = true;
			}
		} else {
			m_handsFromNeckValid = false;
		}
		/*
		 * Looking down says nothing about the height: the last level measurement holds. The hands always decide: with
		 * the measurement held, the user looking down most of the time stayed bent with the hands along (log).
		 */
		if(level && m_uprightValid) {
			m_lowered = m_uprightNeckHeight - neckHeight > 0.035f;
		}
		bool bent = m_lowered && !handsAlong;
		bool leaning = bent || std::abs(right.y) > std::sin(glm::radians(12.f)) || independentHeadMotion;
		if(leaning) {
			m_following = false;
		} else {
			// Two centimeters of sway do not shuffle the feet. Once a step is
			// apparent, settle all the way to the current center, not its old value.
			m_following = m_following || distance > 0.02f;
			if(m_following) {
				m_root += (center - m_root) * (1.f - std::exp(-dt * 20.f));
				if(glm::length(center - m_root) < 0.002f && glm::length(delta) / dt < 0.02f) {
					m_following = false;
				}
			}
		}

		// A reset must never absorb an old head/body gap into the neutral pose.
		// Keep a hard anatomical bound even during a bend or tracking recovery.
		glm::vec2 lean = center - m_root;
		const float maxLean = 0.35f;
		if(glm::length(lean) > maxLean) {
			m_root = center - lean * (maxLean / glm::length(lean));
		}
		m_previous = pose;
		return true;
	}

	[[nodiscard]] glm::vec2 root() const { return m_root; }
	[[nodiscard]] float yaw() const { return m_yaw; }
	/*!
	 * Shortest horizontal distance between the hands for their left-to-right span to give the heading (meters): the
	 * hands are often held close to the body and to each other, 18 to 20 cm apart did not turn a body 65° off (log)
	 */
	static constexpr float MinHandSpan = 0.08f;

	/*!
	 * Where the user faces now (degrees, like yaw()): the left-to-right span of the hands when they are held in front
	 * and agree with the head, else the head. For a rebase after the body was not followed (cutscenes, pauses).
	 */
	[[nodiscard]] static float heading(const BodyObservation & pose) {
		glm::vec3 forward = pose.orientation * glm::vec3(0.f, 0.f, -1.f);
		float headYaw = glm::degrees(std::atan2(-forward.x, -forward.z));
		if(pose.tracked[0] && pose.tracked[1]) {
			glm::vec3 span = pose.hands[1] - pose.hands[0];
			float width = glm::length(horizontal(span));
			float handYaw = glm::degrees(std::atan2(-span.z, span.x));
			if(width > MinHandSpan && width < 0.9f && std::abs(span.y) < 0.3f
			   && std::abs(std::remainder(headYaw - handYaw, 360.f)) < 25.f) {
				return handYaw;
			}
		}
		return headYaw;
	}

	// A rotating head moves the eyes around the neck even with planted feet.
	[[nodiscard]] static glm::vec3 neck(const BodyObservation & pose) {
		return pose.head + pose.orientation * glm::vec3(0.f, -0.10f, 0.08f);
	}

private:
	void recoverHeading(const BodyObservation & pose, bool both, float dt) {
		// Wrist rotations alone cannot provide a lasting body heading: gestures,
		// delayed hands and tracking loss discard part of an integrated turn.
		// Use the left-to-right hand span when it agrees with the head instead.
		glm::vec3 span = pose.hands[1] - pose.hands[0];
		glm::vec3 forward = pose.orientation * glm::vec3(0.f, 0.f, -1.f);
		float width = glm::length(horizontal(span));
		float headYaw = glm::degrees(std::atan2(-forward.x, -forward.z));
		float handYaw = glm::degrees(std::atan2(-span.z, span.x));
		float handError = std::remainder(handYaw - m_yaw, 360.f);
		// Resolve both errors along the same arc at the +/-180 degree boundary.
		float headError = handError + std::remainder(headYaw - handYaw, 360.f);
		bool credible = both && width > MinHandSpan && width < 0.9f
		                && std::abs(span.y) < 0.3f
		                && glm::length(horizontal(forward)) > 0.5f
		                && std::abs(std::remainder(headYaw - handYaw, 360.f)) < 25.f;
		// Head-only looks or hand-only gestures do not start a correction. Once
		// started, settle fully instead of leaving the activation dead zone behind.
		bool agrees = headError * handError > 0.f
		              && std::abs(headError) > 12.f && std::abs(handError) > 12.f;
		if(!credible || (!m_recovering && !agrees)
		   || (headError * handError < 0.f && std::abs(headError) > 3.f)) {
			m_headingEvidence = 0.f;
			m_recovering = false;
			return;
		}
		m_headingEvidence += dt;
		if(m_headingEvidence < 0.2f) {
			return;
		}
		m_recovering = true;
		float correction = handError * (1.f - std::exp(-dt * 8.f));
		m_yaw += glm::clamp(correction, -180.f * dt, 180.f * dt);
		// The correction already includes any uncommitted shared turn.
		m_pendingYaw = 0.f;
		if(std::abs(handError) < 0.1f) {
			m_recovering = false;
			m_headingEvidence = 0.f;
		}
	}

	[[nodiscard]] static glm::vec2 horizontal(glm::vec3 v) { return glm::vec2(v.x, v.z); }
	//! Looking within 25° of level
	[[nodiscard]] static bool isLevel(const BodyObservation & pose) {
		return std::abs((pose.orientation * glm::vec3(0.f, 0.f, -1.f)).y) < std::sin(glm::radians(25.f));
	}
	[[nodiscard]] static float yawChange(glm::quat from, glm::quat to) {
		// World-space swing/twist: no choice of controller axis, hence no axis
		// switching discontinuity when a controller points down or rolls over.
		glm::quat change = glm::normalize(to * glm::inverse(from));
		if(change.w * change.w + change.y * change.y < 0.00001f) {
			return 0.f;
		}
		return std::remainder(glm::degrees(2.f * std::atan2(change.y, change.w)), 360.f);
	}

	bool m_valid = false;
	BodyObservation m_previous;
	glm::vec2 m_root = glm::vec2(0.f);
	float m_headSpeed = 0.f;
	std::array<float, 2> m_handSpeed = {};
	float m_pendingYaw = 0.f;
	float m_turnIdle = 0.f;
	float m_headingEvidence = 0.f;
	bool m_recovering = false;
	float m_uprightNeckHeight = 0.f;
	bool m_uprightValid = false;
	bool m_lowered = false; //!< The neck was below the upright height when last looking level
	float m_stillTime = 0.f;
	bool m_following = false;
	glm::vec2 m_handsFromNeck = glm::vec2(0.f); //!< Horizontal distance of each hand from the neck while the feet are under it
	bool m_handsFromNeckValid = false;
	bool m_turning = false;
	float m_yaw = 0.f;
};

} // namespace vr

#endif // ARX_VR_VRBODYTRACKING_H
