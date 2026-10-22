/*
 * Copyright 2026 Arx Libertatis VR contributors <kazakov.s.fl@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include "vr/VRBodyTracking.h"

namespace {

void check(bool condition, const char * name) {
	if(!condition) {
		std::cerr << "FAIL: " << name << '\n';
		std::exit(1);
	}
}

vr::BodyObservation standing() {
	vr::BodyObservation pose;
	pose.head = glm::vec3(0.f, 1.7f, 0.f);
	pose.hands = { glm::vec3(-0.25f, 1.1f, -0.3f), glm::vec3(0.25f, 1.1f, -0.3f) };
	pose.rotations = { pose.orientation, pose.orientation };
	pose.tracked = { true, true };
	return pose;
}

template <class Motion>
vr::BodyTracking run(float seconds, int hz, Motion motion) {
	vr::BodyTracking body;
	body.reset(standing(), glm::vec2(0.f));
	for(int frame = 1; frame <= int(seconds * hz); frame++) {
		auto pose = standing();
		motion(pose, float(frame) / hz);
		body.update(pose, 1.f / hz);
	}
	return body;
}

glm::quat yaw(float degrees) {
	return glm::angleAxis(glm::radians(degrees), glm::vec3(0.f, 1.f, 0.f));
}

void translate(vr::BodyObservation & pose, glm::vec3 offset) {
	pose.head += offset;
	for(auto & hand : pose.hands) {
		hand += offset;
	}
}

} // namespace

int main() {
	for(int hz : { 72, 90, 120 }) {
		auto delayedHands = run(5.f, hz, [](auto & p, float t) {
			p.orientation = yaw(std::min(t, 1.f) * 90.f);
			auto q = yaw(std::clamp(t - 1.2f, 0.f, 1.f) * 90.f);
			p.rotations = { q, q };
			for(auto & h : p.hands) { h = q * h; }
		});
		check(std::abs(delayedHands.yaw() - 90.f) < 2.f, "recover a turn when the head leads the hands");
		auto lostHandTurn = run(5.f, hz, [](auto & p, float t) {
			auto q = yaw(std::min(t, 2.f) * 45.f);
			p.orientation = q;
			p.rotations = { q, q };
			for(auto & h : p.hands) { h = q * h; }
			p.tracked[0] = t > 2.2f;
		});
		check(std::abs(lostHandTurn.yaw() - 90.f) < 2.f, "recover the full turn after controller reacquisition");
		auto wristsFixed = run(5.f, hz, [](auto & p, float t) {
			auto q = yaw(std::min(t, 2.f) * 45.f);
			p.orientation = q;
			for(auto & h : p.hands) { h = q * h; }
		});
		check(std::abs(wristsFixed.yaw() - 90.f) < 2.f, "hand positions recover turns despite wrist orientation");
		auto handsOrbit = run(5.f, hz, [](auto & p, float t) {
			auto q = yaw(std::min(t, 2.f) * 45.f);
			p.rotations = { q, q };
			for(auto & h : p.hands) { h = q * h; }
		});
		check(std::abs(handsOrbit.yaw()) < 0.001f, "orbiting hands alone cannot recover body heading");
		auto crossedHands = run(5.f, hz, [](auto & p, float t) {
			p.orientation = yaw(std::min(t, 2.f) * 45.f);
			std::swap(p.hands[0], p.hands[1]);
		});
		check(std::abs(crossedHands.yaw()) < 0.001f, "crossed hands are not a reliable heading");
		auto tinyTurn = run(5.f, hz, [](auto & p, float t) {
			auto q = yaw(std::min(t, 2.f) * 2.f);
			p.orientation = q;
			p.rotations = { q, q };
			for(auto & h : p.hands) { h = q * h; }
		});
		check(std::abs(tinyTurn.yaw()) < 0.001f, "small shared sway retains the turn dead zone");
		// A reset carries the actual avatar heading, not the current head heading.
		// Check both directions, including recovery through the angle boundary.
		for(auto headings : { glm::vec2(90.f, 90.f), glm::vec2(0.f, 90.f),
		                      glm::vec2(90.f, 0.f), glm::vec2(170.f, 190.f),
		                      glm::vec2(-170.f, -190.f), glm::vec2(90.f, -90.f) }) {
			auto p = standing();
			p.orientation = yaw(headings.y);
			for(auto & h : p.hands) { h = p.orientation * h; }
			vr::BodyTracking resumed;
			resumed.reset(p, glm::vec2(0.f), headings.x);
			check(resumed.yaw() == headings.x, "reset preserves the actual body heading");
			for(int frame = 0; frame < hz * 4; frame++) {
				float previous = resumed.yaw();
				resumed.update(p, 1.f / hz);
				check(std::abs(resumed.yaw() - previous) <= 180.f / hz + 0.001f,
				      "heading recovery is smooth rather than an instantaneous snap");
			}
			check(std::abs(std::remainder(resumed.yaw() - headings.y, 360.f)) < 0.2f,
			      "absolute heading recovers after reset in either direction");
			check(glm::length(resumed.root()) < 0.02f, "nonzero heading does not rotate the neck offset twice");
			float previous = resumed.yaw();
			check(!resumed.update(p, 1.f), "long pause resets motion history");
			check(resumed.yaw() == previous, "discontinuity preserves heading");
		}
		// Regression: many individually small steps with pauses and imperfect hand
		// agreement must not leave the standing body at an old location.
		auto sidesteps = run(18.f, hz, [](auto & p, float t) {
			float cycle = std::min(t, 16.f) / 0.8f;
			float x = 0.04f * (std::floor(cycle) + std::min((cycle - std::floor(cycle)) * 4.f, 1.f));
			translate(p, glm::vec3(x, 0.f, 0.f));
			p.hands[0].x += 0.06f * std::sin(t * 3.f);
			p.hands[1].z += 0.08f * std::sin(t * 2.f);
		});
		check(glm::distance(sidesteps.root(), glm::vec2(0.8f, 0.f)) < 0.01f, "repeated short sidesteps stay centered");
		vr::BodyTracking staleRoot;
		staleRoot.reset(standing(), glm::vec2(1.2f, 0.f));
		for(int frame = 0; frame < hz; frame++) { staleRoot.update(standing(), 1.f / hz); }
		check(glm::length(staleRoot.root()) < 0.01f, "reset cannot calibrate a meter of body separation as neutral");
		// Check the error after every step, not just once at the end of a walk.
		vr::BodyTracking repeated;
		repeated.reset(standing(), glm::vec2(0.f));
		for(int frame = 1; frame <= hz * 16; frame++) {
			float t = float(frame) / hz;
			float cycle = t / 0.8f;
			float phase = cycle - std::floor(cycle);
			float x = 0.04f * (std::floor(cycle) + std::min(phase * 4.f, 1.f));
			auto p = standing();
			translate(p, glm::vec3(x, 0.f, 0.f));
			p.tracked[0] = std::fmod(t, 2.f) > 0.5f;
			p.hands[1].z += 0.05f * std::sin(t * 4.f);
			repeated.update(p, 1.f / hz);
			check(glm::distance(repeated.root(), glm::vec2(x, 0.f)) < 0.04f, "no accumulated separation during small steps");
			if(phase > 0.8f) {
				check(glm::distance(repeated.root(), glm::vec2(x, 0.f)) < 0.003f, "center recovered between small steps");
			}
		}
		auto hands = run(4.f, hz, [](auto & p, float t) {
			for(auto & h : p.hands) { h.x += 0.3f * std::sin(t * 3.f); }
			p.rotations = { yaw(t * 25.f), yaw(t * 25.f) };
		});
		check(glm::length(hands.root()) < 0.001f && std::abs(hands.yaw()) < 0.001f, "hands alone");
		auto lean = run(8.f, hz, [](auto & p, float t) {
			float progress = std::min(t, 1.f);
			p.head.x += progress * 0.3f;
			p.head.y -= progress * 0.08f;
		});
		check(glm::length(lean.root()) < 0.001f, "held head-only lean must never settle into a step");
		auto upright = run(3.f, hz, [](auto & p, float t) { p.head.x += std::min(t, 1.f) * 0.3f; });
		check(glm::distance(upright.root(), glm::vec2(0.3f, 0.f)) < 0.003f,
		      "upright stationary head and hands recover the user's center");
		auto straighten = run(4.f, hz, [](auto & p, float t) {
			float progress = std::min(t, 1.f);
			p.head.x += progress * 0.3f;
			p.head.y -= std::max(0.f, t < 2.f ? progress : 3.f - t) * 0.08f;
		});
		check(glm::distance(straighten.root(), glm::vec2(0.3f, 0.f)) < 0.003f, "standing upright after bending recenters the stance");
		auto opposite = run(2.f, hz, [](auto & p, float t) {
			p.head.x += t * 0.15f;
			for(auto & h : p.hands) { h.x -= t * 0.15f; }
		});
		check(glm::length(opposite.root()) < 0.001f, "opposed hands and head");
		auto sway = run(10.f, hz, [](auto & p, float t) {
			translate(p, glm::vec3(0.015f * std::sin(t * 2.f), 0.f, 0.f));
		});
		check(glm::length(sway.root()) < 0.001f, "small common sway");
		auto slight = run(8.f, hz, [](auto & p, float t) {
			translate(p, glm::vec3(std::min(t, 1.f) * 0.015f, 0.f, 0.f));
		});
		check(glm::length(slight.root()) < 0.001f, "small common shift held still");
		auto swayWithPause = run(6.f, hz, [](auto & p, float t) {
			float x = t < 1.f ? t * 0.015f : (t < 3.f ? 0.015f : std::max(-0.015f, 0.015f - (t - 3.f) * 0.03f));
			translate(p, glm::vec3(x, 0.f, 0.f));
		});
		check(glm::length(swayWithPause.root()) < 0.001f, "small shifts remain in stance dead zone after a hold");
		auto walk = run(4.f, hz, [](auto & p, float t) {
			translate(p, glm::vec3(std::min(t, 2.f) * 0.3f, 0.f, 0.f));
		});
		check(glm::distance(walk.root(), glm::vec2(0.6f, 0.f)) < 0.015f, "walk and stop without residual offset");
		auto slow = run(8.f, hz, [](auto & p, float t) { translate(p, glm::vec3(t * 0.025f, 0.f, 0.f)); });
		check(glm::distance(slow.root(), glm::vec2(0.2f, 0.f)) < 0.015f, "slow deliberate step");
		auto swinging = run(2.f, hz, [](auto & p, float t) {
			translate(p, glm::vec3(t * 0.3f, 0.f, 0.f));
			p.hands[0].z += 0.12f * std::sin(t * 6.f);
			p.hands[1].z -= 0.12f * std::sin(t * 6.f);
		});
		check(glm::distance(swinging.root(), glm::vec2(0.6f, 0.f)) < 0.015f, "walking with alternating arm swing");
		auto roundTrip = run(4.f, hz, [](auto & p, float t) {
			translate(p, glm::vec3((t < 2.f ? t : 4.f - t) * 0.3f, 0.f, 0.f));
		});
		check(glm::length(roundTrip.root()) < 0.015f, "walking back does not leave the body behind");
		auto look = run(8.f, hz, [](auto & p, float t) {
			p.orientation = yaw(std::min(t, 2.f) * 40.f);
			p.head = vr::BodyTracking::neck(standing()) - p.orientation * glm::vec3(0.f, -0.1f, 0.08f);
		});
		check(glm::length(look.root()) < 0.001f && std::abs(look.yaw()) < 0.001f, "head turn about neck");
		auto turn = run(4.f, hz, [](auto & p, float t) {
			auto q = yaw(std::min(t, 2.f) * 45.f);
			p.orientation = q;
			p.rotations = { q, q };
			for(auto & h : p.hands) { h = q * h; }
		});
		check(std::abs(turn.yaw() - 90.f) < 1.f, "shared turn catches its initial dead zone");
		check(glm::length(turn.root()) < 0.015f, "turning in place does not translate feet");
		auto slowTurn = run(10.f, hz, [](auto & p, float t) {
			p.orientation = yaw(t * 3.f);
			p.rotations = { p.orientation, p.orientation };
		});
		check(std::abs(slowTurn.yaw() - 30.f) < 1.f, "slow body turn");
		auto movingTurn = run(3.f, hz, [](auto & p, float t) {
			auto q = yaw(t * 30.f);
			p.orientation = q;
			p.rotations = { q, q };
			for(auto & h : p.hands) { h = q * h; }
			translate(p, glm::vec3(t * 0.3f, 0.f, 0.f));
		});
		check(glm::distance(movingTurn.root(), glm::vec2(0.9f, 0.f)) < 0.025f
		      && std::abs(movingTurn.yaw() - 90.f) < 1.f, "simultaneous walking and turning");
		auto wrap = run(8.f, hz, [](auto & p, float t) {
			auto q = yaw(t * 60.f);
			p.orientation = q;
			p.rotations = { q, q };
			for(auto & h : p.hands) { h = q * h; }
		});
		check(std::abs(wrap.yaw() - 480.f) < 1.f && glm::length(wrap.root()) < 0.015f, "yaw wrap and full turn");
		auto roll = run(2.f, hz, [](auto & p, float t) {
			p.orientation = yaw(t * 30.f);
			auto q = glm::angleAxis(t, glm::vec3(0.f, 0.f, 1.f));
			p.rotations = { q, q };
		});
		check(std::abs(roll.yaw()) < 0.001f, "controller roll does not count as body yaw");
		auto one = run(2.f, hz, [](auto & p, float t) {
			p.head.x += t * 0.15f;
			p.hands[0].x += t * 0.15f;
		});
		check(glm::distance(one.root(), glm::vec2(0.3f, 0.f)) < 0.01f, "upright movement is not vetoed by one hand");
		auto crouch = run(3.f, hz, [](auto & p, float t) {
			translate(p, glm::vec3(0.f, -0.5f * std::sin(t), 0.f));
		});
		check(glm::length(crouch.root()) < 0.001f, "crouching is not horizontal locomotion");
		auto untracked = run(2.f, hz, [](auto & p, float t) {
			p.tracked = { false, false };
			p.head.x += t * 0.5f;
		});
		check(glm::distance(untracked.root(), glm::vec2(1.f, 0.f)) < 0.025f, "upright movement without controllers");
		auto oppositeTurn = run(2.f, hz, [](auto & p, float t) {
			p.orientation = yaw(t * 30.f);
			p.rotations = { yaw(-t * 30.f), yaw(-t * 30.f) };
		});
		check(std::abs(oppositeTurn.yaw()) < 0.001f, "opposed hand/head turns");
		auto nod = run(3.f, hz, [](auto & p, float t) {
			p.orientation = glm::angleAxis(0.7f * std::sin(t * 2.f), glm::vec3(1.f, 0.f, 0.f));
			p.head = vr::BodyTracking::neck(standing()) - p.orientation * glm::vec3(0.f, -0.1f, 0.08f);
		});
		check(glm::length(nod.root()) < 0.001f && std::abs(nod.yaw()) < 0.001f, "nodding about the neck");
		auto farLean = run(4.f, hz, [](auto & p, float t) {
			p.head.x += std::min(t, 2.f) * 0.5f;
			p.head.y -= std::min(t, 1.f) * 0.15f;
		});
		check(glm::distance(farLean.root(), glm::vec2(0.65f, 0.f)) < 0.003f, "lean separation remains bounded");
	}
	vr::BodyTracking body;
	auto pose = standing();
	body.reset(pose, glm::vec2(0.f));
	translate(pose, glm::vec3(2.f, 0.f, 0.f));
	check(!body.update(pose, 1.f / 90.f), "tracking jump is rejected");
	check(body.update(pose, 1.f / 90.f), "tracking resumes without a delayed jump");
	check(!body.update(pose, 1.f), "pause is rejected");
	pose = standing();
	body.reset(pose, glm::vec2(0.f));
	pose.tracked[1] = false;
	body.update(pose, 1.f / 90.f);
	pose.tracked[1] = true;
	pose.hands[1].x += 1.f;
	body.update(pose, 1.f / 90.f);
	check(glm::length(body.root()) < 0.001f && std::abs(body.yaw()) < 0.001f, "controller reacquisition");
	std::cout << "VR body motion traces passed at 72, 90 and 120 Hz\n";
}
