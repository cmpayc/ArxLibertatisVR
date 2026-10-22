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

#include "vr/VRBody.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include "animation/AnimationRender.h"
#include "animation/Skeleton.h"
#include "core/Config.h"
#include "core/Core.h"
#include "game/Entity.h"
#include "game/Player.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/Math.h"
#include "graphics/Raycast.h"
#include "io/log/Logger.h"
#include "scene/Object.h"
#include "vr/VRCamera.h"
#include "vr/VRControls.h"
#include "vr/VRMath.h"
#include "vr/VRSystem.h"


namespace {

//! A middle, ring or little finger with its own bones, see splitFingers
struct FingerBones {
	VertexGroupId middle; //!< Middle phalanx, a child of the fingers tip bone
	VertexGroupId tip;    //!< Last phalanx
};

//! A slice of the forearm that turns with a part of the hand twist, see splitForearm
struct TwistBone {
	VertexGroupId bone;   //!< A child of the elbow or wrist bone, at its pivot
	float twist = 0.f;    //!< Part of the forearm twist: 0 at the elbow, 1 at the wrist
};

//! The bones of one arm of the player model
struct ArmBones {
	VertexGroupId shoulder;  //!< Shoulder joint, rotates the upper arm
	VertexGroupId elbow;     //!< Elbow joint, rotates the forearm
	VertexGroupId wrist;     //!< Wrist joint
	VertexGroupId hand;      //!< Hand, parent of the fingers
	VertexGroupId indexRoot; //!< Gives the direction of the fingers
	VertexGroupId thumb;     //!< Gives the side of the thumb
	VertexGroupId indexTip;
	VertexGroupId fingersRoot; //!< Middle, ring and little fingers
	VertexGroupId fingersTip;
	std::vector<FingerBones> fingers;
	std::vector<TwistBone> twists;
	[[nodiscard]] bool valid() const { return shoulder && elbow && wrist && hand && indexRoot && thumb; }
};

struct SpineBones {
	VertexGroupId belt;  //!< Lower spine
	VertexGroupId chest; //!< Upper spine
	VertexGroupId neck;
	[[nodiscard]] bool valid() const { return belt && chest && neck; }
};

struct LegBones {
	VertexGroupId hip;
	VertexGroupId knee;
	VertexGroupId ankle;
	[[nodiscard]] bool valid() const { return hip && knee && ankle; }
};

struct BodyBones {
	const EERIE_3DOBJ * object = nullptr;
	SpineBones spine;
	ArmBones arms[VRHandCount];
	LegBones legs[VRHandCount]; //!< Left and right
};
BodyBones g_bones;

VertexGroupId findBodyGroup(const EERIE_3DOBJ & object, std::string_view name) {
	for(VertexGroupId group : object.grouplist.handles()) {
		const std::string & groupName = object.grouplist[group].name;
		// Some group names have an index prefix, e.g. "28: right_hand_fingers_root"
		if(groupName == name || (groupName.size() > name.size()
		                         && groupName.compare(groupName.size() - name.size(), name.size(), name) == 0
		                         && groupName[groupName.size() - name.size() - 1] == ' ')) {
			return group;
		}
	}
	return VertexGroupId();
}

const BodyBones & getBodyBones(const EERIE_3DOBJ & object) {

	if(g_bones.object != &object) {
		g_bones = BodyBones();
		g_bones.object = &object;
		g_bones.spine.belt = findBodyGroup(object, "belt");
		g_bones.spine.chest = findBodyGroup(object, "chest");
		g_bones.spine.neck = findBodyGroup(object, "neck");
		const char * const legSides[VRHandCount] = { "left", "right" };
		for(size_t leg = 0; leg < VRHandCount; leg++) {
			g_bones.legs[leg].hip = findBodyGroup(object, std::string(legSides[leg]) + "_hip");
			g_bones.legs[leg].knee = findBodyGroup(object, std::string(legSides[leg]) + "_knee");
			g_bones.legs[leg].ankle = findBodyGroup(object, std::string(legSides[leg]) + "_ankle");
		}
		if(!g_bones.spine.valid()) {
			LogWarning << "VR: the player model has no complete spine, it will not lean";
		}
		const char * const sides[VRHandCount] = { "left", "right" };
		for(size_t hand = 0; hand < VRHandCount; hand++) {
			std::string side = sides[hand];
			ArmBones & arm = g_bones.arms[hand];
			arm.shoulder = findBodyGroup(object, side + "_arm");
			arm.elbow = findBodyGroup(object, side + "_elbow");
			arm.wrist = findBodyGroup(object, side + "_wrist");
			arm.hand = findBodyGroup(object, side + "_hand");
			arm.indexRoot = findBodyGroup(object, side + "_hand_index_root");
			arm.thumb = findBodyGroup(object, side + "_hand_thumb");
			arm.indexTip = findBodyGroup(object, side + "_hand_index_tip");
			arm.fingersRoot = findBodyGroup(object, side + "_hand_fingers_root");
			arm.fingersTip = findBodyGroup(object, side + "_hand_fingers_tip");
			if(!arm.fingersTip) {
				arm.fingersTip = findBodyGroup(object, side + "_hand_finger_tip"); // Spelled differently in the model
			}			if(!arm.valid()) {
				LogWarning << "VR: the player model has no complete " << side << " arm, it will not follow the controller";
			}
		}
	}

	return g_bones;
}

bool isBoneDescendant(const Skeleton & skeleton, VertexGroupId bone, VertexGroupId ancestor) {
	for(VertexGroupId parent = skeleton.bones[bone].father; parent; parent = skeleton.bones[parent].father) {
		if(parent == ancestor) {
			return true;
		}
	}
	return false;
}

/*
 * Bones made longer (the arms, see solveArm): a factor of their scale in their own axes that their children do not
 * inherit, so that only these bones and the positions of their children change, not the size of the hand
 */
std::vector<Vec3f> g_boneLengths;

Vec3f getBoneLength(VertexGroupId group) {
	return (size_t(group) < g_boneLengths.size()) ? g_boneLengths[size_t(group)] : Vec3f(1.f);
}

//! Make a bone longer along its own axis towards a child, by a factor
void lengthenBone(Skeleton & skeleton, VertexGroupId group, VertexGroupId child, float factor) {
	Vec3f towards = glm::abs(skeleton.bones[child].init.trans);
	int axis = (towards.x >= towards.y && towards.x >= towards.z) ? 0 : (towards.y >= towards.z ? 1 : 2);
	if(g_boneLengths.size() < skeleton.bones.size()) {
		g_boneLengths.resize(skeleton.bones.size(), Vec3f(1.f));
	}
	Vec3f length(1.f);
	length[axis] = factor;
	Bone & bone = skeleton.bones[group];
	bone.anim.scale = bone.anim.scale / getBoneLength(group) * length;
	g_boneLengths[size_t(group)] = length;
}

//! Recompute the world transforms of all descendants of a bone after changing it (same as Cedric_ConcatenateTM)
void updateBoneDescendants(Skeleton & skeleton, VertexGroupId root) {
	for(VertexGroupId group : skeleton.bones.handles()) {
		if(!isBoneDescendant(skeleton, group, root)) {
			continue;
		}
		Bone & bone = skeleton.bones[group];
		const Bone & parent = skeleton.bones[bone.father];
		bone.anim.quat = parent.anim.quat * bone.init.quat;
		bone.anim.trans = parent.anim.trans + parent.anim.quat * (bone.init.trans * parent.anim.scale);
		// A longer parent does not make its children longer (see lengthenBone)
		Vec3f parentScale = parent.anim.scale / getBoneLength(bone.father);
		bone.anim.scale = (bone.init.scale + Vec3f(1.f)) * parentScale * getBoneLength(group);
	}
}

/*!
 * The middle, ring and little fingers of the model share one root and one tip bone, and the tip bone holds the last two
 * phalanges of each finger: they could only bend at the knuckles and at one joint, all three around the same point.
 * Give each finger a bone for its middle phalanx (a child of the tip bone, where the root bone ends) and one for its
 * last phalanx. Each finger is a separate part of the mesh. The new bones come after the bones of the
 * model, so the animations of the game leave them alone, and the fingers follow the shared bones as before until
 * curlFingers bends them.
 */
void splitFingers(EERIE_3DOBJ & object, ArmBones & arm) {

	arm.fingers.clear();
	if(!object.m_skeleton || !arm.indexRoot || !arm.fingersRoot || !arm.fingersTip) {
		return;
	}
	Skeleton & skeleton = *object.m_skeleton;
	const Vec3f direction = glm::normalize(skeleton.bones[arm.indexRoot].transinit_global);

	// Each finger is a separate part of the mesh: find the vertices of both bones connected by faces
	std::map<size_t, size_t> parents;
	std::map<size_t, bool> isTip;
	for(VertexGroupId bone : { arm.fingersRoot, arm.fingersTip }) {
		for(VertexId vertex : object.m_boneVertices[bone]) {
			parents[size_t(vertex)] = size_t(vertex);
			isTip[size_t(vertex)] = (bone == arm.fingersTip);
		}
	}
	auto find = [&parents](size_t vertex) {
		while(parents[vertex] != vertex) {
			vertex = parents[vertex] = parents[parents[vertex]];
		}
		return vertex;
	};
	for(const EERIE_FACE & face : object.facelist) {
		for(size_t i = 0; i < IOPOLYVERT; i++) {
			size_t a = size_t(face.vid[i]);
			size_t b = size_t(face.vid[(i + 1) % IOPOLYVERT]);
			if(parents.count(a) && parents.count(b)) {
				parents[find(a)] = find(b);
			}
		}
	}
	std::map<size_t, std::vector<size_t>> parts;
	for(const auto & entry : parents) {
		parts[find(entry.first)].push_back(entry.first);
	}

	const Vec3f tipPivot = object.vertexlist[object.grouplist[arm.fingersTip].origin].v;
	std::vector<size_t> moved;

	for(auto & entry : parts) {

		std::vector<size_t> & vertices = entry.second;
		if(vertices.size() < 3) {
			continue; // The loose pivot vertex
		}
		auto along = [&](size_t vertex) { return glm::dot(object.vertexlist[VertexId(vertex)].v, direction); };
		std::sort(vertices.begin(), vertices.end(), [&](size_t a, size_t b) { return along(a) < along(b); });
		std::vector<size_t> tips;
		float rootEnd = -std::numeric_limits<float>::max();
		for(size_t vertex : vertices) {
			if(isTip[vertex]) {
				tips.push_back(vertex);
			} else {
				rootEnd = std::max(rootEnd, along(vertex));
			}
		}
		if(tips.size() < 2 || rootEnd == -std::numeric_limits<float>::max()) {
			continue;
		}

		/*
		 * The middle joint is where the root bone ends. The last joint is at the largest gap between the vertices of the
		 * tip bone 50% to 80% of the way from there to the tip: the three rings of human_base, or a denser mesh.
		 */
		const float middleJoint = (rootEnd + along(tips.front())) / 2.f;
		const float end = along(tips.back());
		if(end - middleJoint < 0.001f) {
			continue;
		}
		float lastJoint = 0.f;
		float largestGap = -1.f;
		for(size_t i = 0; i + 1 < tips.size(); i++) {
			float gap = along(tips[i + 1]) - along(tips[i]);
			float middle = (along(tips[i + 1]) + along(tips[i])) / 2.f;
			float fraction = (middle - middleJoint) / (end - middleJoint);
			if(fraction >= 0.5f && fraction <= 0.8f && gap > largestGap) {
				largestGap = gap;
				lastJoint = middle;
			}
		}
		if(largestGap < 0.f) {
			continue;
		}

		// A joint bends around the center of the finger: the vertices nearest to it along the finger
		auto center = [&](float position) {
			std::vector<size_t> nearest = vertices;
			size_t count = std::max<size_t>(3, vertices.size() / 6);
			std::partial_sort(nearest.begin(), nearest.begin() + count, nearest.end(), [&](size_t a, size_t b) {
				return std::abs(along(a) - position) < std::abs(along(b) - position);
			});
			Vec3f sum(0.f);
			for(size_t i = 0; i < count; i++) {
				sum += object.vertexlist[VertexId(nearest[i])].v;
			}
			sum /= float(count);
			return sum + direction * (position - glm::dot(sum, direction));
		};
		const Vec3f middlePivot = center(middleJoint);
		const Vec3f lastPivot = center(lastJoint);

		auto addBone = [&](VertexGroupId father, const Vec3f & pivot, const Vec3f & fatherPivot) {
			Bone bone;
			bone.father = father;
			bone.transinit_global = bone.init.trans = pivot - fatherPivot;
			bone.anim.trans = pivot;
			bone.anim.scale = Vec3f(1.f);
			skeleton.bones.push_back(bone);
			object.m_boneVertices.emplace_back();
			return skeleton.bones.last();
		};
		FingerBones finger;
		finger.middle = addBone(arm.fingersTip, middlePivot, tipPivot);
		finger.tip = addBone(finger.middle, lastPivot, middlePivot);
		for(size_t tip : tips) {
			const VertexId vertex(tip);
			bool last = along(tip) >= lastJoint;
			object.m_boneVertices[last ? finger.tip : finger.middle].push_back(vertex);
			object.vertexlocal[vertex] = object.vertexlist[vertex].v - (last ? lastPivot : middlePivot);
			moved.push_back(tip);
		}
		arm.fingers.push_back(finger);

	}

	auto & tipVertices = object.m_boneVertices[arm.fingersTip];
	tipVertices.erase(std::remove_if(tipVertices.begin(), tipVertices.end(), [&moved](VertexId vertex) {
		return std::find(moved.begin(), moved.end(), size_t(vertex)) != moved.end();
	}), tipVertices.end());

	updateBoneDescendants(skeleton, arm.fingersRoot);
}

/*!
 * The forearm of the model has two bones, and a few rings of vertices: with rigid skinning, the twist of the hand around
 * the forearm all went to the faces between two rings and stretched them (user: turning the palm up). Like the twist
 * bones of other games, give the vertices of both bones slices that turn with a part of the twist, from none at the
 * elbow to all of it at the wrist (see solveArm). The slices are children of the bone the vertices were in, at its pivot,
 * so the vertices keep their positions and follow it as before until solveArm turns them.
 */
void splitForearm(EERIE_3DOBJ & object, ArmBones & arm) {

	arm.twists.clear();
	if(!object.m_skeleton || !arm.elbow || !arm.wrist) {
		return;
	}
	Skeleton & skeleton = *object.m_skeleton;
	const Vec3f elbowPivot = object.vertexlist[object.grouplist[arm.elbow].origin].v;
	const Vec3f wristPivot = object.vertexlist[object.grouplist[arm.wrist].origin].v;
	if(glm::distance(elbowPivot, wristPivot) < 0.001f) {
		return;
	}
	const Vec3f axis = glm::normalize(wristPivot - elbowPivot);
	auto along = [&](VertexId vertex) { return glm::dot(object.vertexlist[vertex].v - elbowPivot, axis); };

	// From the ring nearest to the elbow (it meets the upper arm, which does not turn) to the wrist
	float start = std::numeric_limits<float>::max();
	float end = -std::numeric_limits<float>::max();
	for(VertexGroupId bone : { arm.elbow, arm.wrist }) {
		for(VertexId vertex : object.m_boneVertices[bone]) {
			start = std::min(start, along(vertex));
			end = std::max(end, along(vertex));
		}
	}
	if(end - start < 0.001f) {
		return;
	}

	const int Slices = 12;
	std::map<std::pair<size_t, int>, VertexGroupId> slices;
	for(VertexGroupId owner : { arm.elbow, arm.wrist }) {
		const std::vector<VertexId> vertices = object.m_boneVertices[owner]; // Adding bones moves the lists
		std::vector<VertexId> kept;
		for(VertexId vertex : vertices) {
			int slice = int(std::round((along(vertex) - start) / (end - start) * float(Slices)));
			if(slice == ((owner == arm.wrist) ? Slices : 0)) {
				kept.push_back(vertex); // Already turns as much as its bone
				continue;
			}
			VertexGroupId & bone = slices[{ size_t(owner), slice }];
			if(!bone) {
				Bone twistBone;
				twistBone.father = owner;
				twistBone.transinit_global = twistBone.init.trans = Vec3f(0.f);
				twistBone.anim.trans = (owner == arm.wrist) ? wristPivot : elbowPivot;
				twistBone.anim.scale = Vec3f(1.f);
				skeleton.bones.push_back(twistBone);
				object.m_boneVertices.emplace_back();
				bone = skeleton.bones.last();
				arm.twists.push_back({ bone, float(slice) / float(Slices) });
			}
			object.m_boneVertices[bone].push_back(vertex);
		}
		object.m_boneVertices[owner] = kept;
	}

	updateBoneDescendants(skeleton, arm.elbow);
}

//! Shortest rotation from one unit vector to another
glm::quat rotationBetween(const Vec3f & from, const Vec3f & to) {
	float d = glm::dot(from, to);
	if(d < -0.9999f) {
		// Opposite directions: turn half way around any perpendicular axis
		Vec3f axis = glm::cross(Vec3f(1.f, 0.f, 0.f), from);
		if(glm::length(axis) < 0.001f) {
			axis = glm::cross(Vec3f(0.f, 1.f, 0.f), from);
		}
		return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
	}
	Vec3f axis = glm::cross(from, to);
	return glm::normalize(glm::quat(1.f + d, axis.x, axis.y, axis.z));
}
//! Rotate a bone in world space so that the direction to one of its children points at a target
void aimBone(Skeleton & skeleton, VertexGroupId bone, VertexGroupId child, const Vec3f & target) {
	Bone & b = skeleton.bones[bone];
	Vec3f current = skeleton.bones[child].anim.trans - b.anim.trans;
	Vec3f wanted = target - b.anim.trans;
	if(glm::length(current) < 0.001f || glm::length(wanted) < 0.001f) {
		return;
	}
	b.anim.quat = glm::normalize(rotationBetween(glm::normalize(current), glm::normalize(wanted)) * b.anim.quat);
	updateBoneDescendants(skeleton, bone);
}

//! Rotate a bone part of the way towards aiming one of its descendants at a target
void aimBonePartially(Skeleton & skeleton, VertexGroupId bone, VertexGroupId child, const Vec3f & target,
                      float amount) {
	Bone & b = skeleton.bones[bone];
	Vec3f current = skeleton.bones[child].anim.trans - b.anim.trans;
	Vec3f wanted = target - b.anim.trans;
	if(glm::length(current) < 0.001f || glm::length(wanted) < 0.001f) {
		return;
	}
	glm::quat rotation = rotationBetween(glm::normalize(current), glm::normalize(wanted));
	rotation = glm::slerp(glm::quat(1.f, 0.f, 0.f, 0.f), rotation, amount);
	b.anim.quat = glm::normalize(rotation * b.anim.quat);
	updateBoneDescendants(skeleton, bone);
}

/*!
 * Lower the hips of the model (squatting) while the feet stay where the animation put them: two-bone IK for the
 * legs, with the knees bending forwards and a little outwards. The drop is limited to how far the legs can fold.
 */
void lowerHips(Skeleton & skeleton, const LegBones (&legs)[VRHandCount], float drop) {
	
	const float MinFold = 0.3f; // Shortest hip to ankle distance, relative to the length of the leg
	
	struct Foot {
		Vec3f position;
		glm::quat orientation;
	};
	Foot feet[VRHandCount];
	for(size_t leg = 0; leg < VRHandCount; leg++) {
		if(!legs[leg].valid()) {
			return;
		}
		const Bone & hip = skeleton.bones[legs[leg].hip];
		const Bone & knee = skeleton.bones[legs[leg].knee];
		const Bone & ankle = skeleton.bones[legs[leg].ankle];
		feet[leg] = { ankle.anim.trans, ankle.anim.quat };
		float length = glm::distance(hip.anim.trans, knee.anim.trans) + glm::distance(knee.anim.trans, ankle.anim.trans);
		drop = std::min(drop, std::max(ankle.anim.trans.y - hip.anim.trans.y - length * MinFold, 0.f)); // y down
	}
	if(drop <= 0.f) {
		return;
	}
	
	for(VertexGroupId bone : skeleton.bones.handles()) {
		if(!skeleton.bones[bone].father) {
			skeleton.bones[bone].anim.trans.y += drop;
			updateBoneDescendants(skeleton, bone);
		}
	}
	
	const Vec3f forward = angleToVectorXZ(player.angle.getYaw());
	const Vec3f right = angleToVectorXZ(player.angle.getYaw() - 90.f);
	for(size_t leg = 0; leg < VRHandCount; leg++) {
		const LegBones & bones = legs[leg];
		const Vec3f hip = skeleton.bones[bones.hip].anim.trans;
		float upper = glm::distance(hip, skeleton.bones[bones.knee].anim.trans);
		float lower = glm::distance(skeleton.bones[bones.knee].anim.trans, skeleton.bones[bones.ankle].anim.trans);
		Vec3f toFoot = feet[leg].position - hip;
		float distance = glm::length(toFoot);
		if(distance < 0.001f) {
			continue;
		}
		Vec3f direction = toFoot / distance;
		distance = glm::clamp(distance, std::abs(upper - lower) + 0.1f, upper + lower - 0.1f);
		Vec3f outwards = (leg == VRHandRight) ? right : -right;
		Vec3f pole = forward + outwards * 0.3f;
		Vec3f bend = pole - direction * glm::dot(pole, direction);
		if(glm::length(bend) < 0.001f) {
			bend = forward;
		}
		bend = glm::normalize(bend);
		float cosAngle = glm::clamp((upper * upper + distance * distance - lower * lower) / (2.f * upper * distance),
		                            -1.f, 1.f);
		float sinAngle = std::sqrt(1.f - cosAngle * cosAngle);
		Vec3f knee = hip + (direction * cosAngle + bend * sinAngle) * upper;
		aimBone(skeleton, bones.hip, bones.knee, knee);
		aimBone(skeleton, bones.knee, bones.ankle, hip + direction * distance);
		// Keep the foot flat on the floor
		skeleton.bones[bones.ankle].anim.quat = feet[leg].orientation;
		updateBoneDescendants(skeleton, bones.ankle);
	}
	
}

/*!
 * Bend the spine so that the neck of the model is below and behind the eyes of the user:
 * leaning and peeking tilt the torso instead of moving the feet. The lower spine and the chest share the bend.
 */
void solveSpine(Skeleton & skeleton, const SpineBones & spine, const LegBones (&legs)[VRHandCount]) {
	
	Vec3f eyes;
	glm::mat3x3 head;
	if(!getVRHeadPose(eyes, head)) {
		return;
	}

	/*
	 * The head turns around a pivot at the top of the neck, below and behind the eyes: looking down moves the
	 * eyes down and forwards, but not the neck. Place the neck relative to that pivot (real size, from the
	 * tracking) instead of the eyes, so that only moving the head lowers the body.
	 */
	const float trackingScale = 100.f * config.vr.worldScale;
	const Vec3f PivotFromEyes = Vec3f(0.f, 10.f, -8.f) * trackingScale / 100.f; // head space, cm
	Vec3f pivot = eyes + head * PivotFromEyes;

	/*
	 * The neck joint is 17 cm below the eyes of the (scaled) model, plus the height_offset the view is raised by.
	 * It is placed further behind the eyes than in the player model
	 * (6.6 cm) so that the view is not inside the neck opening of the hidden head.
	 */
	Vec3f forward = angleToVectorXZ(player.angle.getYaw());
	// Looking straight ahead, the pivot is level with the head: this gives the same neck as relative to the eyes
	Vec3f neck = pivot + Vec3f(0.f, 17.f * getVRBodyScale() - PivotFromEyes.y + config.vr.heightOffset * 100.f, 0.f)
	             - forward * (config.vr.bodyOffset * 100.f + PivotFromEyes.z);
	
	/*
	 * The crouch animation cannot go as low as a real squat: lower the hips (the whole model, before bending the
	 * spine, so that the torso does not tip down towards a neck target below the hips) and bend the knees so that
	 * the feet stay on the floor.
	 */
	const float Tolerance = 5.f;
	{
		// Leaning keeps the neck at the length of the spine from the hips: only drop when it is closer
		const Vec3f belt = skeleton.bones[spine.belt].anim.trans;
		float spineLength = glm::distance(belt, skeleton.bones[spine.neck].anim.trans);
		float horizontal = glm::length(Vec2f(neck.x - belt.x, neck.z - belt.z));
		float vertical = std::sqrt(std::max(spineLength * spineLength - horizontal * horizontal, 0.f));
		// Standing still the head is often a few cm below the highest it has been: do not squat for that
		const float SquatTolerance = 15.f;
		float drop = neck.y + vertical - belt.y - SquatTolerance; // y points down
		if(drop > 0.f) {
			lowerHips(skeleton, legs, drop);
		}
	}
	
	aimBonePartially(skeleton, spine.belt, spine.neck, neck, 0.5f);
	aimBone(skeleton, spine.chest, spine.neck, neck);

	/*
	 * Aiming does not change the length of the spine: when the animation puts the torso far from the head (e.g.
	 * crouching while the player sits), the shoulders would stay out of reach of the hands. Move the upper body
	 * so that the neck reaches its place, ignoring small differences so that standing looks the same.
	 */
	// Small residual corrections keep the shoulders reachable without detaching
	// the torso from the planted pelvis when the requested pose is unreachable.
	const float MaxShift = 8.f * getVRBodyScale();
	Vec3f error = neck - skeleton.bones[spine.neck].anim.trans;
	float length = glm::length(error);
	if(length > Tolerance) {
		Vec3f shift = error * (std::min(length - Tolerance, MaxShift) / length);
		skeleton.bones[spine.belt].anim.trans += shift;
		updateBoneDescendants(skeleton, spine.belt);
	}

}

//! Orthonormal frame from a primary direction and a secondary direction
glm::mat3x3 makeBodyFrame(const Vec3f & primary, const Vec3f & secondary) {
	Vec3f x = glm::normalize(primary);
	Vec3f z = glm::normalize(glm::cross(x, secondary));
	Vec3f y = glm::cross(z, x);
	return glm::mat3x3(x, y, z);
}

/*
 * How the hand sits on the controller, in the controller grip frame (OpenXR basis: x right, y up, z backward).
 * The wrist joint position relative to the grip pose is configurable: config.vr.handOffset.
 * TODO(vr) Calibrate per controller type
 */
bool g_handInWall[VRHandCount] = { false, false };
bool g_weaponInWall[VRHandCount] = { false, false };

//! Twist of the hand around the forearm in the last frame (degrees), to follow it past half a turn
float g_handTwist[VRHandCount] = { 0.f, 0.f };
bool g_handTwistValid[VRHandCount] = { false, false };

//! Signed angle (degrees) of the part of a rotation around an axis (swing-twist decomposition)
float twistAngle(const glm::quat & rotation, const Vec3f & axis) {
	float angle = glm::degrees(2.f * std::atan2(glm::dot(Vec3f(rotation.x, rotation.y, rotation.z), axis), rotation.w));
	return (angle > 180.f) ? angle - 360.f : ((angle <= -180.f) ? angle + 360.f : angle);
}

//! The angle plus or minus whole turns that is nearest to a reference (degrees)
float unwrapAngle(float angle, float reference) {
	return angle + 360.f * std::round((reference - angle) / 360.f);
}

// The wrist joint and the controller orientation of each hand of the posed model, in the world
struct WristPose {
	bool valid = false;
	Vec3f position = Vec3f(0.f);
	glm::quat gripOrientation = glm::quat(1.f, 0.f, 0.f, 0.f);
	Vec3f elbow = Vec3f(0.f);
};
WristPose g_wristPoses[VRHandCount];

// The hip joints of the posed model, in the world
bool g_hipsValid = false;
Vec3f g_hipPositions[VRHandCount];

const Vec3f FingerDirection(0.f, -0.5f, -1.f); //!< From the wrist to the knuckles
const Vec3f ThumbDirection(0.f, 1.f, 0.f);     //!< The thumb rests on top of the controller

/*!
 * Curl the fingers of the model with the controller: the grip closes the middle, ring and little fingers,
 * the trigger the index finger. Touch sensors lower the thumb and the index finger when they rest on the
 * controller. The finger animation of the game is replaced - except while holding something (see applyVRBodyIK).
 */
/*!
 * Give the fingers of one hand the pose of the other hand's fingers, mirrored (the model is symmetric across x, and its
 * bones have no rest rotation, so the mirrored rotation flips y and z)
 */
void copyMirroredFingers(Skeleton & skeleton, const ArmBones & from, const ArmBones & to) {
	
	auto copy = [&skeleton](VertexGroupId source, VertexGroupId target) {
		if(source && target) {
			const glm::quat & q = skeleton.bones[source].init.quat;
			skeleton.bones[target].init.quat = glm::quat(q.w, q.x, -q.y, -q.z);
		}
	};
	copy(from.fingersRoot, to.fingersRoot);
	copy(from.fingersTip, to.fingersTip);
	copy(from.indexRoot, to.indexRoot);
	copy(from.indexTip, to.indexTip);
	copy(from.thumb, to.thumb);
	
	updateBoneDescendants(skeleton, to.hand);
}

void curlFingers(Skeleton & skeleton, const ArmBones & arm, const VRHandState & state,
                 const ANIM_HANDLE * fistPose = nullptr, const ArmBones * fistArm = nullptr,
                 bool mirrorFist = false, bool pointing = false) {
	
	// In the rest pose of the model the palms face down (+y in model space, which points down)
	const Vec3f palm(0.f, 1.f, 0.f);
	const Vec3f fingers = glm::normalize(skeleton.bones[arm.indexRoot].transinit_global);
	const Vec3f thumb = skeleton.bones[arm.thumb].transinit_global;
	
	// Fingers bend around the line of the knuckles towards the palm, the thumb around the direction of the fingers
	Vec3f fingerAxis = glm::cross(fingers, palm);
	Vec3f thumbAxis = glm::cross(thumb - fingers * glm::dot(thumb, fingers), palm);
	if(glm::length(fingerAxis) < 0.001f || glm::length(thumbAxis) < 0.001f) {
		return;
	}
	fingerAxis = glm::normalize(fingerAxis);
	thumbAxis = glm::normalize(thumbAxis);

	// The index points straight on, unlike the line from the wrist to it (9° to the thumb): it curled to the thumb
	Vec3f indexAxis = fingerAxis;
	if(arm.indexTip) {
		Vec3f axis = glm::cross(skeleton.bones[arm.indexTip].transinit_global, palm);
		if(glm::length(axis) > 0.001f) {
			indexAxis = glm::normalize(axis);
		}
	}

	/*
	 * The thumb touching a button bends towards the tip of the curled index, like an OK sign: 56° around this axis,
	 * measured on human_base and the enhanced player mesh. It is built from cross products of the bone directions, so
	 * it mirrors for the left hand.
	 */
	Vec3f thumbOkAxis = 0.71f * glm::cross(fingers, palm) - 0.073f * glm::cross(palm, thumb)
	                    + 0.21f * glm::cross(thumb, fingers);
	if(glm::length(thumbOkAxis) < 0.001f) {
		return;
	}
	thumbOkAxis = glm::normalize(thumbOkAxis);

	const float relaxed = 0.12f; // A slight curl even when open
	float grip = glm::mix(relaxed, 1.f, state.squeeze);
	float index = glm::mix(relaxed, 1.f, state.trigger);
	if(state.hasIndexTouch && state.indexTouch) {
		// Resting on the trigger
		index = std::max(index, 0.175f);
	}

	// With touch sensors the thumb bends while touching the controller (less without the index) and is raised otherwise
	float thumbAngle = 25.f + 15.f * state.squeeze;
	Vec3f thumbBend = thumbAxis;
	if(state.hasThumbTouch) {
		thumbAngle = state.thumbTouch ? glm::mix(15.f, 56.f, state.trigger) : 0.f;
		thumbBend = thumbOkAxis;
	}

	// Casting (the game's pose for magic): the index straight, the other fingers bent with the thumb over them
	if(pointing) {
		grip = 1.f;
		index = 0.f;
		thumbAngle = 55.f;
		thumbBend = thumbAxis;
	}
	
	/*
	 * Grip and trigger together make a fist (it punches, see VRCombat): blend into the right hand grip of the game's
	 * one-handed weapon stance (its first frame), mirrored for the left hand like copyMirroredFingers. Bending the fingers
	 * further pointed the tips down, the bare hand stance hardly bends them.
	 */
	float fist = pointing ? 0.f : std::min(state.squeeze, state.trigger);
	const EERIE_ANIM * fistAnim = (fistPose && !fistPose->anims.empty()) ? fistPose->anims[0] : nullptr;
	
	auto curl = [&](VertexGroupId bone, VertexGroupId fistBone, const Vec3f & axis, float degrees,
	                const glm::quat & fistOffset = glm::quat(1.f, 0.f, 0.f, 0.f)) {
		if(!bone) {
			return;
		}
		glm::quat rotation = glm::angleAxis(glm::radians(degrees), axis);
		size_t group = size_t(fistBone);
		if(fist > 0.f && fistAnim && fistBone && group < fistAnim->nb_groups() && group < fistAnim->groups.size()
		   && !fistAnim->voidgroups[group]) {
			glm::quat pose = fistAnim->groups[group].quat;
			if(mirrorFist) {
				pose = glm::quat(pose.w, pose.x, -pose.y, -pose.z);
			}
			rotation = glm::slerp(rotation, fistOffset * pose, fist);
		}
		skeleton.bones[bone].init.quat = rotation;
	};
	const ArmBones & source = fistArm ? *fistArm : arm;
	/*
	 * The middle, ring and little fingers bend at all three joints when they have their own bones (splitFingers), into
	 * an arc like the index. Otherwise the tip bone holds two phalanges: each joint stays below 90°, more at the knuckle
	 * folded them into a sharp L. They do not take the game's fist: it bends the tip 142° and folds them flat under the
	 * palm. The index curls almost like in the game's fist (69° and 97°).
	 */
	/*
	 * Around a line across the fingers from their root to their tip bone: the axis of the index made them roll up to
	 * one side. The bones are the same for every mesh, the vertices are not: the armor gloves have them as one piece.
	 */
	// The left hand is not quite a mirror of the right one (its bones point 8° to the side, not 3°): mirror the right one
	Vec3f fingersAxis = fingerAxis;
	const ArmBones & right = g_bones.arms[VRHandRight];
	const bool mirrored = (&arm == &g_bones.arms[VRHandLeft] && right.fingersTip);
	if(VertexGroupId fingersTip = mirrored ? right.fingersTip : arm.fingersTip) {
		Vec3f direction = skeleton.bones[fingersTip].transinit_global;
		if(mirrored) {
			direction.x = -direction.x;
		}
		Vec3f axis = glm::cross(direction, palm);
		if(glm::length(axis) > 0.001f) {
			fingersAxis = glm::normalize(axis);
		}
	}
	if(arm.fingers.empty()) {
		curl(arm.fingersRoot, VertexGroupId(), fingersAxis, 60.f * grip);
		curl(arm.fingersTip, VertexGroupId(), fingersAxis, 75.f * grip);
	} else {
		// The first phalanx is long: less at the knuckle (like the index) and more at the other joints rolls them up
		curl(arm.fingersRoot, VertexGroupId(), fingersAxis, 65.f * grip);
		curl(arm.fingersTip, VertexGroupId(), fingersAxis, 0.f);
		for(const FingerBones & finger : arm.fingers) {
			curl(finger.middle, VertexGroupId(), fingersAxis, 100.f * grip);
			curl(finger.tip, VertexGroupId(), fingersAxis, 75.f * grip);
		}
	}
	curl(arm.indexRoot, source.indexRoot, indexAxis, 70.f * index);
	curl(arm.indexTip, source.indexTip, indexAxis, 90.f * index);
	// The thumb of the game's fist goes through the index: turned a bit out, away from it
	Vec3f thumbOut = glm::cross(fingers, thumb);
	glm::quat thumbOffset(1.f, 0.f, 0.f, 0.f);
	if(glm::length(thumbOut) > 0.001f) {
		thumbOffset = glm::angleAxis(glm::radians(12.f), glm::normalize(thumbOut));
	}
	curl(arm.thumb, source.thumb, thumbBend, thumbAngle, thumbOffset);
	
	updateBoneDescendants(skeleton, arm.hand);
}

/*!
 * \param reach Length of the held item (weapon) from the fist, 0 if the hand is empty: it is stopped by walls too
 */
void solveArm(Skeleton & skeleton, const ArmBones & arm, VRHand hand, const VRPose & grip, float reach) {

	Vec3f gripPosition;
	glm::quat gripOrientation;
	if(!grip.valid || !trackingPoseToWorld(grip.position, grip.orientation, gripPosition, gripOrientation)) {
		g_handTwistValid[hand] = false;
		return;
	}

	const float scale = 100.f * config.vr.worldScale;
	Vec3f target = gripPosition + gripOrientation * (vr::toArxBasis(config.vr.handOffset) * scale);

	/*
	 * A held weapon stops at walls: the hand is pulled back along the blade (which points through the fist, see
	 * alignHeldItem) so that the tip rests on the wall, with a short vibration on contact.
	 */
	bool weaponInWall = false;
	if(reach > 0.f) {
		const float WristToFist = 8.f; // cm, the item is held in front of the wrist
		Vec3f blade = glm::normalize(gripOrientation * Vec3f(0.f, 0.f, 1.f));
		Vec3f tip = target + blade * (WristToFist + reach);
		if(RaycastResult hit = raycastScene(target, tip, POLY_TRANS, RaycastIgnorePlayer)) {
			target -= blade * glm::distance(hit.pos, tip);
			weaponInWall = true;
		}
	}
	if(weaponInWall && !g_weaponInWall[hand]) {
		g_vr->getInput().vibrate(hand, 0.4f, 0.05f);
	}
	g_weaponInWall[hand] = weaponInWall;

	Vec3f shoulder = skeleton.bones[arm.shoulder].anim.trans;
	float upperLength = glm::distance(shoulder, skeleton.bones[arm.elbow].anim.trans);
	float lowerLength = glm::distance(skeleton.bones[arm.elbow].anim.trans, skeleton.bones[arm.wrist].anim.trans);

	Vec3f toTarget = target - shoulder;
	float distance = glm::length(toTarget);
	if(distance < 0.001f) {
		return;
	}
	Vec3f direction = toTarget / distance;

	/*
	 * Walls stop the hand: the wrist stays far enough in front of them for the palm and fingers, with a short
	 * vibration when the hand hits a wall. Objects do not stop the hand (they can be grabbed).
	 */
	const float HandLength = 5.f; // cm from the wrist to the wall: the palm usually faces the wall, not the fingers
	bool inWall = false;
	if(RaycastResult hit = raycastScene(shoulder, target + direction * HandLength, POLY_TRANS, RaycastIgnorePlayer)) {
		float allowed = std::max(glm::distance(shoulder, hit.pos) - HandLength, 0.f);
		if(allowed < distance) {
			distance = allowed;
			inWall = true;
		}
	}
	if(inWall && !g_handInWall[hand]) {
		g_vr->getInput().vibrate(hand, 0.2f, 0.03f);
	}
	g_handInWall[hand] = inWall;

	/*
	 * Out of reach (the user's arm may be longer than the model's): the shoulder moves towards the target, by up to
	 * 15% of the arm length (user request), stretching the mesh between the chest and the arm
	 */
	const float MaxStretch = 0.15f;
	float armLength = upperLength + lowerLength - 0.1f;
	if(distance > armLength) {
		float stretch = std::min(distance - armLength, MaxStretch * (upperLength + lowerLength));
		skeleton.bones[arm.shoulder].anim.trans += direction * stretch;
		updateBoneDescendants(skeleton, arm.shoulder);
		shoulder += direction * stretch;
		distance -= stretch;
	}

	/*
	 * Still out of reach: the upper arm and the forearm get longer evenly, by up to 35% (user request: with the
	 * shoulder, the model reaches as far as the user, measured 50% further than its arm), the hand keeps its size
	 */
	const float MaxLengthen = 0.35f;
	if(distance > armLength) {
		float factor = std::min(distance / armLength, 1.f + MaxLengthen);
		lengthenBone(skeleton, arm.shoulder, arm.elbow, factor);
		lengthenBone(skeleton, arm.elbow, arm.wrist, factor);
		for(const TwistBone & slice : arm.twists) {
			// The slices are longer with their bone (lengthenBone made room for all bones)
			g_boneLengths[size_t(slice.bone)] = getBoneLength(skeleton.bones[slice.bone].father);
		}
		updateBoneDescendants(skeleton, arm.shoulder);
		upperLength = glm::distance(shoulder, skeleton.bones[arm.elbow].anim.trans);
		lowerLength = glm::distance(skeleton.bones[arm.elbow].anim.trans, skeleton.bones[arm.wrist].anim.trans);
	}

	// Still out of reach: the arm points at the target
	distance = glm::clamp(distance, std::abs(upperLength - lowerLength) + 0.1f, upperLength + lowerLength - 0.1f);
	target = shoulder + direction * distance;

	// The elbow bends down, outwards and a little backwards (Arx world: y points down)
	Vec3f forward = angleToVectorXZ(player.angle.getYaw());
	Vec3f right = angleToVectorXZ(player.angle.getYaw() - 90.f);
	Vec3f outwards = (hand == VRHandRight) ? right : -right;
	Vec3f pole = Vec3f(0.f, 1.f, 0.f) + outwards * 0.6f - forward * 0.3f;
	Vec3f bend = pole - direction * glm::dot(pole, direction);
	if(glm::length(bend) < 0.001f) {
		bend = outwards;
	}
	bend = glm::normalize(bend);

	// Law of cosines: angle between the upper arm and the shoulder-to-wrist line
	float cosAngle = (upperLength * upperLength + distance * distance - lowerLength * lowerLength)
	                 / (2.f * upperLength * distance);
	cosAngle = glm::clamp(cosAngle, -1.f, 1.f);
	float sinAngle = std::sqrt(1.f - cosAngle * cosAngle);
	Vec3f elbow = shoulder + (direction * cosAngle + bend * sinAngle) * upperLength;

	aimBone(skeleton, arm.shoulder, arm.elbow, elbow);
	aimBone(skeleton, arm.elbow, arm.wrist, target);

	// Hand orientation: match the model's finger and thumb directions to the controller
	const Vec3f localFingers = skeleton.bones[arm.indexRoot].transinit_global;
	const Vec3f localThumb = skeleton.bones[arm.thumb].transinit_global;
	glm::mat3x3 local = makeBodyFrame(localFingers, localThumb);
	glm::mat3x3 world = makeBodyFrame(gripOrientation * vr::toArxBasis(FingerDirection),
	                              gripOrientation * vr::toArxBasis(ThumbDirection));
	glm::quat handOrientation = glm::normalize(glm::quat_cast(world * glm::transpose(local)));

	/*
	 * The twist of the hand around the forearm is shared out like in a real arm, so that no seam of the rigid skinning
	 * twists too far and stretches the textures (user request):
	 *  - 50% is a rotation of the shoulder: the whole arm turns around the line from the shoulder to the wrist (the
	 *    elbow swings, the wrist stays), up to 180° (user: more than 35%), but only when the elbow moves outwards
	 *    (user: not inwards, into the body),
	 *  - the rest goes along the forearm: the wrist bone turns with the hand (half of it made the hand turn faster)
	 *    and the slices of splitForearm in steps between none at the elbow and all at the wrist. Two seams there tore
	 *    or stretched the forearm at any share and limit (user: turning the palm up).
	 * The twist is followed from frame to frame, so it does not flip to the other side at half a turn.
	 */
	const float ShoulderShare = 0.5f;
	const float ShoulderTwist = 180.f;
	const float ElbowShare = 0.3f; // Without slices: of the whole, at the seam between the upper arm and the elbow bone
	const float MaxSwivel = 150.f;
	Bone & wrist = skeleton.bones[arm.wrist];
	auto forearmAxis = [&]() {
		Vec3f forearm = wrist.anim.trans - skeleton.bones[arm.elbow].anim.trans;
		return (glm::length(forearm) > 0.001f) ? glm::normalize(forearm) : Vec3f(0.f);
	};
	Vec3f forearm = forearmAxis();
	if(forearm != Vec3f(0.f)) {

		float twist = twistAngle(handOrientation * glm::inverse(wrist.anim.quat), forearm);
		if(g_handTwistValid[hand]) {
			twist = unwrapAngle(twist, g_handTwist[hand]);
			if(std::abs(twist) > 400.f) {
				twist -= std::copysign(360.f, twist); // More than a hand can turn: tracking was lost
			}
		}
		g_handTwist[hand] = twist;
		g_handTwistValid[hand] = true;

		float shoulderPart = glm::clamp(twist * ShoulderShare, -ShoulderTwist, ShoulderTwist);
		float along = glm::dot(direction, forearm);
		if(std::abs(shoulderPart) > 0.1f && std::abs(along) > 0.2f) {
			// Turning the arm around the shoulder to wrist line by an angle turns the forearm by about angle × along
			auto turnArm = [&](float angle) {
				// Both bones: updating the upper arm alone puts the forearm back where the animation had it
				glm::quat rotation = glm::angleAxis(glm::radians(angle), direction);
				Bone & upperArm = skeleton.bones[arm.shoulder];
				Bone & forearmBone = skeleton.bones[arm.elbow];
				glm::quat forearmOrientation = glm::normalize(rotation * forearmBone.anim.quat);
				upperArm.anim.quat = glm::normalize(rotation * upperArm.anim.quat);
				updateBoneDescendants(skeleton, arm.shoulder);
				forearmBone.anim.quat = forearmOrientation;
				updateBoneDescendants(skeleton, arm.elbow);
			};
			auto measure = [&](float expected) {
				forearm = forearmAxis();
				return unwrapAngle(twistAngle(handOrientation * glm::inverse(wrist.anim.quat), forearm), expected);
			};
			const Vec3f elbowBefore = skeleton.bones[arm.elbow].anim.trans;
			const Vec3f forearmBefore = forearm;
			float swivel = glm::clamp(shoulderPart / along, -MaxSwivel, MaxSwivel);
			turnArm(swivel);
			float turned = measure(twist - swivel * along);
			if(std::abs(turned) > std::abs(twist)) {
				turnArm(-2.f * swivel);
				turned = measure(twist + swivel * along);
				swivel = -swivel;
			}
			// The elbow does not swing inwards, towards the body (user): the forearm takes that twist
			if(glm::dot(skeleton.bones[arm.elbow].anim.trans - elbowBefore, outwards) < 0.f) {
				turnArm(-swivel);
				forearm = forearmBefore;
			} else {
				twist = turned;
			}
		}

		Bone & elbowBone = skeleton.bones[arm.elbow];
		if(arm.twists.empty()) {
			// What is left after the shoulder: 30% and 20% of the whole at the two seams of the forearm
			float elbowPart = twist * ElbowShare / (1.f - ShoulderShare);
			elbowBone.anim.quat = glm::normalize(glm::angleAxis(glm::radians(elbowPart), forearm) * elbowBone.anim.quat);
			updateBoneDescendants(skeleton, arm.elbow);
			wrist.anim.quat = glm::normalize(glm::angleAxis(glm::radians(twist - elbowPart), forearm) * wrist.anim.quat);
			updateBoneDescendants(skeleton, arm.wrist);
		} else {
			// The wrist bone turns with the hand, each slice by its part of that from its bone
			glm::quat wristOrientation = glm::normalize(glm::angleAxis(glm::radians(twist), forearm) * wrist.anim.quat);
			wrist.init.quat = glm::normalize(glm::inverse(elbowBone.anim.quat) * wristOrientation);
			for(const TwistBone & slice : arm.twists) {
				Bone & bone = skeleton.bones[slice.bone];
				bool lower = (bone.father == arm.wrist);
				Vec3f axis = glm::inverse(lower ? wristOrientation : elbowBone.anim.quat) * forearm;
				float part = lower ? slice.twist - 1.f : slice.twist;
				bone.init.quat = glm::angleAxis(glm::radians(part * twist), axis);
			}
			updateBoneDescendants(skeleton, arm.elbow);
		}

	}
	
	skeleton.bones[arm.hand].anim.quat = handOrientation;
	updateBoneDescendants(skeleton, arm.hand);
	
	WristPose & pose = g_wristPoses[hand];
	pose.valid = true;
	pose.position = skeleton.bones[arm.wrist].anim.trans;
	pose.gripOrientation = gripOrientation;
	pose.elbow = skeleton.bones[arm.elbow].anim.trans;

}

//! The attach vertex for objects held in a hand of the player model
VertexId getHoldVertex(const EERIE_3DOBJ & object, VRHand hand) {
	return (hand == VRHandRight) ? object.fastaccess.primary_attach : object.fastaccess.left_attach;
}

//! \return true if the player model holds something (weapon, torch) in a hand
bool isHoldingItem(const EERIE_3DOBJ & object, VRHand hand) {
	VertexId vertex = getHoldVertex(object, hand);
	for(const EERIE_LINKED & link : object.linked) {
		if(vertex && link.lidx == vertex && link.obj && link.obj != arrowobj.get()) { // Not the arrow to shoot
			return true;
		}
	}
	return false;
}

//! \return true if the player model holds the lit torch in a hand (see VRCombat)
bool isHoldingTorch(const EERIE_3DOBJ & object, VRHand hand) {
	VertexId vertex = getHoldVertex(object, hand);
	for(const EERIE_LINKED & link : object.linked) {
		if(vertex && link.lidx == vertex && player.torch && link.io == player.torch) {
			return true;
		}
	}
	return false;
}

//! The longest extent of an object from its attach point (the blade of a weapon), in object space
Vec3f getHeldItemExtent(const EERIE_3DOBJ & object, VertexId attach) {

	static std::map<const EERIE_3DOBJ *, Vec3f> s_extents;
	auto it = s_extents.find(&object);
	if(it != s_extents.end()) {
		return it->second;
	}

	Vec3f extent(0.f);
	if(attach && size_t(attach) < object.vertexlist.size()) {
		const Vec3f origin = object.vertexlist[attach].v;
		float farthest = 0.f;
		for(const EERIE_VERTEX & vertex : object.vertexlist) {
			float distance = glm::distance(vertex.v, origin);
			if(distance > farthest) {
				farthest = distance;
				extent = vertex.v - origin;
			}
		}
	}

	s_extents[&object] = extent;
	return extent;
}

//! Direction of the longest extent of an object from its attach point (the blade of a weapon), in object space
Vec3f getHeldItemAxis(const EERIE_3DOBJ & object, VertexId attach) {
	Vec3f extent = getHeldItemExtent(object, attach);
	return (glm::length(extent) > 0.001f) ? glm::normalize(extent) : Vec3f(0.f);
}

//! Length of the item held in a hand of the player model from the hand (its blade), 0 if the hand is empty
float getHeldItemReach(const EERIE_3DOBJ & object, VRHand hand) {
	VertexId vertex = getHoldVertex(object, hand);
	for(const EERIE_LINKED & link : object.linked) {
		if(vertex && link.lidx == vertex && link.obj && link.obj != arrowobj.get()) {
			return glm::length(getHeldItemExtent(*link.obj, link.lidx2)) * (link.io ? link.io->scale : 1.f);
		}
	}
	return 0.f;
}

/*!
 * \return true if an item held in the left hand of the model is drawn mirrored (bows)
 *
 * The left hand is a mirror image of the right hand (the model is symmetric across x and its bones have no rest
 * rotation, see copyMirroredFingers), so a bow attached to it would curve the wrong way. It is drawn as the mirror
 * image (across x in the bone's space) of how the right hand holds it, see adjustVRLinkedTransform.
 */
bool isMirroredInLeftHand(const EERIE_3DOBJ & object, const EERIE_LINKED & link) {
	return link.obj && link.io && object.fastaccess.left_attach && link.lidx == object.fastaccess.left_attach
	       && (link.io->type_flags & OBJECT_TYPE_BOW);
}

/*!
 * Turn the hand so that the object it holds (weapon, torch) points along the controller
 *
 * The object keeps the pose relative to the hand that the game gives it, with the handle inside the fist.
 * The grip pose points through the closed fist, like the blade of a sword.
 */
void alignHeldItem(Skeleton & skeleton, const EERIE_3DOBJ & object, const ArmBones & arm, VRHand hand,
                   const VRPose & grip) {
	
	VertexId vertex = getHoldVertex(object, hand);
	const EERIE_LINKED * held = nullptr;
	for(const EERIE_LINKED & link : object.linked) {
		if(vertex && link.lidx == vertex && link.obj && link.lgroup) {
			held = &link;
			break;
		}
	}
	if(!held) {
		return;
	}
	
	Vec3f axis = getHeldItemAxis(*held->obj, held->lidx2);
	Vec3f gripPosition;
	glm::quat gripOrientation;
	if(axis == Vec3f(0.f) || !trackingPoseToWorld(grip.position, grip.orientation, gripPosition, gripOrientation)) {
		return;
	}
	
	// Attached objects use the rotation of the bone they are attached to
	if(isMirroredInLeftHand(object, *held)) {
		axis.x = -axis.x;
	}
	Vec3f current = skeleton.bones[held->lgroup].anim.quat * axis;
	// A torch is held up and forwards (y is down), weapons point through the fist
	Vec3f direction = (player.torch && held->io == player.torch) ? Vec3f(0.f, -1.f, 1.f) : Vec3f(0.f, 0.f, 1.f);
	Vec3f wanted = gripOrientation * glm::normalize(direction);
	glm::quat change = rotationBetween(glm::normalize(current), glm::normalize(wanted));
	
	Bone & handBone = skeleton.bones[arm.hand];
	handBone.anim.quat = glm::normalize(change * handBone.anim.quat);
	updateBoneDescendants(skeleton, arm.hand);
}

} // anonymous namespace

void applyVRBodyIK(Entity & entity, Skeleton & skeleton) {

	if(!isVRActive() || !g_vr || !entity.obj) {
		return;
	}

	const BodyBones & bones = getBodyBones(*entity.obj);
	// Once for each player mesh (it is created again when the equipment changes), see splitFingers
	if(entity.obj->m_skeleton.get() == &skeleton && skeleton.bones.size() == entity.obj->grouplist.size()) {
		for(ArmBones & arm : g_bones.arms) {
			splitFingers(*entity.obj, arm);
			splitForearm(*entity.obj, arm);
		}
	}
	const VRInput & input = g_vr->getInput();
	g_boneLengths.assign(skeleton.bones.size(), Vec3f(1.f)); // The animation set the scales again
	for(WristPose & pose : g_wristPoses) {
		pose.valid = false;
	}
	g_hipsValid = false;

	// Cutscenes and other cameras: the game animates the body (the last head pose would freeze it bent)
	if(!isVRPlayerView()) {
		return;
	}

	// The spine first: it moves the shoulders
	if(bones.spine.valid()) {
		solveSpine(skeleton, bones.spine, bones.legs);
	}

	for(size_t hand = 0; hand < VRHandCount; hand++) {
		const ArmBones & arm = bones.arms[hand];
		if(arm.valid()) {
			const VRHandState & state = input.getHand(VRHand(hand));
			solveArm(skeleton, arm, VRHand(hand), state.grip, getHeldItemReach(*entity.obj, VRHand(hand)));
			if(state.grip.valid) {
				if(isHoldingItem(*entity.obj, VRHand(hand))) {
					// The animation of the game wraps the fingers around the handle: keep it, only aim the item
					alignHeldItem(skeleton, *entity.obj, arm, VRHand(hand), state.grip);
					if(isHoldingTorch(*entity.obj, VRHand(hand))) {
						// The game does not animate a hand holding the torch: a fist around it
						VRHandState fist = state;
						fist.squeeze = fist.trigger = 1.f;
						curlFingers(skeleton, arm, fist, entity.anims[ANIM_1H_WAIT], &bones.arms[VRHandRight],
						            hand == VRHandLeft);
					} else if(hand == VRHandLeft) {
						/*
						 * The game only holds items in the right hand, and animates its fingers around the handle: the
						 * left hand (done first, before the right hand fingers may be curled) gets that pose mirrored
						 */
						copyMirroredFingers(skeleton, bones.arms[VRHandRight], arm);
					}
				} else {
					// The fist pose of the game's right hand, mirrored for the left hand: closed around the book held
					// by its corner (see VRSystem::getBookPose)
					VRHandState fingers = state;
					if(g_vr && g_vr->isBookInHand() && g_vr->getBookHand() == VRHand(hand)) {
						fingers.squeeze = fingers.trigger = 1.f;
					}
					curlFingers(skeleton, arm, fingers, entity.anims[ANIM_1H_WAIT], &bones.arms[VRHandRight],
					            hand == VRHandLeft);
				}
				// In the magic mode the hand drawing the runes points like in the game, also holding a weapon (user)
				if(g_vr && g_vr->isMagicMode() && g_vr->getPointerHand() == VRHand(hand)) {
					curlFingers(skeleton, arm, state, nullptr, nullptr, false, true);
				}
			}
		}
	}
	
	if(bones.legs[0].valid() && bones.legs[1].valid()) {
		for(size_t leg = 0; leg < VRHandCount; leg++) {
			g_hipPositions[leg] = skeleton.bones[bones.legs[leg].hip].anim.trans;
		}
		g_hipsValid = true;
	}

}

const std::vector<bool> * getVRUpperLayerBones(const EERIE_3DOBJ & object) {
	
	if(!isVRActive() || !object.m_skeleton || !isVRPlayerView()) {
		return nullptr;
	}
	
	static const EERIE_3DOBJ * s_object = nullptr;
	static std::vector<bool> s_mask;
	const Skeleton & skeleton = *object.m_skeleton;
	if(s_object != &object || s_mask.size() != skeleton.bones.size()) {
		const BodyBones & bones = getBodyBones(object);
		s_mask.assign(skeleton.bones.size(), false);
		for(VertexGroupId group : skeleton.bones.handles()) {
			for(const ArmBones & arm : bones.arms) {
				if(arm.hand && (group == arm.hand || isBoneDescendant(skeleton, group, arm.hand))) {
					s_mask[size_t(group)] = true;
				}
			}
		}
		s_object = &object;
	}
	
	return &s_mask;
}

bool isVRWeaponInWall(VRHand hand) {
	return isVRActive() && g_weaponInWall[hand];
}

bool getVRHipPosition(VRHand side, Vec3f & position) {
	
	if(!isVRActive() || !g_hipsValid) {
		return false;
	}
	
	position = g_hipPositions[side];
	return true;
}

bool getVRSheathHilt(VRHand hand, Vec3f & hilt) {
	
	// Where the game shows the sheathed weapon (on the back, seen in third person): its grip is at this vertex
	const Entity * io = entities.player();
	if(!isVRActive() || !io || !io->obj) {
		return false;
	}
	VertexId vertex = io->obj->fastaccess.weapon_attach;
	if(!vertex || size_t(vertex) >= io->obj->vertexWorldPositions.size()) {
		return false;
	}
	
	hilt = io->obj->vertexWorldPositions[vertex].v;
	
	// For the hand on the other side: the same spot over the other shoulder, mirrored across the chest
	const BodyBones & bones = getBodyBones(*io->obj);
	Vec3f center = io->pos;
	if(bones.spine.chest && io->obj->m_skeleton) {
		center = io->obj->m_skeleton->bones[bones.spine.chest].anim.trans;
	}
	Vec3f right = angleToVectorXZ(player.angle.getYaw() - 90.f);
	float side = glm::dot(hilt - center, right);
	if((side > 0.f) != (hand == VRHandRight)) {
		hilt -= right * (2.f * side);
	}
	
	return true;
}

Vec3f getVRArrowRaise() {
	// Just above the hand, straight up (the thumb side of the controller pointed down with some grips)
	const float Up = 6.f; // cm
	return Vec3f(0.f, -Up * getVRBodyScale(), 0.f); // y points down
}

bool getVRArrowRest(Vec3f & rest) {
	
	// Just above the grip of the bow as it is drawn (its grip vertex)
	Entity * bow = entities.get(player.equiped[EQUIP_SLOT_WEAPON]);
	if(!isVRActive() || !bow || !bow->obj) {
		return false;
	}
	VertexId grip = bow->obj->fastaccess.primary_attach;
	if(!grip || size_t(grip) >= bow->obj->vertexWorldPositions.size()) {
		return false;
	}
	
	/*
	 * The grip vertex is on the inside of the grip, beside the bow: move it horizontally to the center of the bow at the
	 * grip (its vertices close to the grip vertex), keeping the height. The center of the whole bow is far off, towards
	 * its curved limbs.
	 */
	const auto & vertices = bow->obj->vertexWorldPositions;
	const Vec3f gripPosition = vertices[grip].v;
	const float Near = 8.f * getVRBodyScale(); // cm
	Vec3f center(0.f);
	size_t count = 0;
	for(const auto & vertex : vertices) {
		if(glm::distance(vertex.v, gripPosition) < Near) {
			center += vertex.v;
			count++;
		}
	}
	Vec3f side(0.f);
	if(count > 0) {
		side = center / float(count) - gripPosition;
		side.y = 0.f;
	}

	rest = vertices[grip].v + side + getVRArrowRaise();
	return true;
}

void adjustVRLinkedTransform(const Entity & entity, const EERIE_LINKED & link, TransformInfo & transform) {
	
	if(!isVRActive() || !isVRPlayerView() || !entity.obj) {
		return;
	}
	
	if(arrowobj && link.obj == arrowobj.get()) {
		/*
		 * The arrow held to shoot (see VRCombat) lies just above the drawing hand and points at its rest just above the
		 * bow's grip, along the drawn string: turn it around its nock (attach) so that its tip points there
		 */
		Vec3f axis = getHeldItemAxis(*link.obj, link.lidx2);
		Vec3f rest;
		if(axis == Vec3f(0.f) || !getVRArrowRest(rest)) {
			return;
		}
		transform.pos += getVRArrowRaise();
		Vec3f toBow = rest - transform.pos;
		if(glm::length(toBow) < 1.f) {
			return;
		}
		Vec3f current = glm::normalize(transform.rotation * axis);
		transform.rotation = glm::normalize(rotationBetween(current, glm::normalize(toBow)) * transform.rotation);
		return;
	}
	
	if(player.torch && link.io == player.torch && link.obj) {
		/*
		 * The torch is attached at the end of its handle, so the hand held it by the very end (user): it is moved down
		 * through the hand, away from its flame, so that the hand holds it a little further up (30% was too far)
		 */
		VertexId fire = link.obj->fastaccess.fire;
		if(fire && size_t(fire) < link.obj->vertexlist.size() && size_t(link.lidx2) < link.obj->vertexlist.size()) {
			const float Down = 0.15f; // Of the way from the attach point to the flame
			Vec3f toFire = link.obj->vertexlist[fire].v - link.obj->vertexlist[link.lidx2].v;
			transform.pos -= transform.rotation * (toFire * Down) * transform.scale;
		}
		return;
	}

	if(!isMirroredInLeftHand(*entity.obj, link)) {
		return;
	}
	
	/*
	 * The mirror across x in the bone's space: a half turn around x with a negative scale is the reflection
	 * diag(-1, 1, 1), the faces are then culled the other way (see the linked objects in AnimationRender)
	 */
	transform.rotation = glm::normalize(transform.rotation * glm::angleAxis(glm::pi<float>(), Vec3f(1.f, 0.f, 0.f)));
	transform.scale = -transform.scale;
}

bool getVRSheathTransform(const Entity & entity, const EERIE_LINKED & link, TransformInfo & transform) {
	
	if(!entity.obj || !entity.obj->m_skeleton || !link.obj || !link.lgroup) {
		return false;
	}
	
	// Like the game draws it in third person, attached to the back
	transform = TransformInfo(entity.obj->vertexWorldPositions[link.lidx].v,
	                          entity.obj->m_skeleton->bones[link.lgroup].anim.quat,
	                          link.io ? link.io->scale : 1.f);
	transform.pos = transform(link.obj->vertexlist[link.obj->origin].v - link.obj->vertexlist[link.lidx2].v);
	return true;
}

bool getVRBeltTransform(VRHand side, const EERIE_3DOBJ & item, float scale, TransformInfo & transform, Vec3f & centre) {
	
	if(!isVRActive() || !isVRPlayerView() || !g_hipsValid || item.vertexlist.empty()) {
		return false;
	}
	
	Vec3f across = g_hipPositions[VRHandRight] - g_hipPositions[VRHandLeft];
	across.y = 0.f;
	if(glm::length(across) < 1.f) {
		return false;
	}
	Vec3f outward = glm::normalize((side == VRHandRight) ? across : -across);
	
	// From the end to the flame (fire vertex), or along the longest extent from the origin
	VertexId bottom = item.origin;
	Vec3f axis = getHeldItemExtent(item, bottom);
	if(item.fastaccess.fire && size_t(item.fastaccess.fire) < item.vertexlist.size()) {
		Vec3f toFire = item.vertexlist[item.fastaccess.fire].v - item.vertexlist[bottom].v;
		if(glm::length(toFire) > glm::length(axis) * 0.5f) {
			axis = toFire;
		}
	}
	float length = glm::length(axis) * scale;
	if(length < 1.f) {
		return false;
	}
	
	// Tucked into the belt at the outside of the hip, the flame end up and tilted outwards
	const float BeltOffset = 9.f * getVRBodyScale(); // cm outwards from the hip joint
	const float tilt = glm::radians(25.f);
	Vec3f along = glm::normalize(Vec3f(0.f, -std::cos(tilt), 0.f) + outward * std::sin(tilt));
	glm::quat rotation = rotationBetween(glm::normalize(axis), along);
	centre = g_hipPositions[side] + outward * BeltOffset;
	Vec3f middle = item.vertexlist[bottom].v + axis * 0.5f; // Object space, drawn as rotation * vertex * scale + pos
	transform = TransformInfo(centre - (rotation * middle) * scale, rotation, scale);
	return true;
}

bool getVRForearm(VRHand hand, Vec3f & elbow, Vec3f & wrist) {
	
	const WristPose & pose = g_wristPoses[hand];
	if(!isVRActive() || !pose.valid || glm::distance(pose.elbow, pose.position) < 1.f) {
		return false;
	}
	
	elbow = pose.elbow;
	wrist = pose.position;
	return true;
}

bool getVRWristPose(VRHand hand, Vec3f & position, glm::quat & gripOrientation) {
	
	const WristPose & pose = g_wristPoses[hand];
	if(!isVRActive() || !pose.valid) {
		return false;
	}
	
	position = pose.position;
	gripOrientation = pose.gripOrientation;
	return true;
}

Vec3f getVRModelOffset() {
	if(!isVRPlayerView()) {
		return Vec3f(0.f); // Not posed from the headset, see applyVRBodyIK
	}
	// The neck of the player model is 5 cm behind its feet: move the model back so that the torso stays upright
	// when the neck is placed config.vr.bodyOffset behind the eyes, which are above the feet
	float back = std::max(config.vr.bodyOffset * 100.f - 5.f * getVRBodyScale(), 0.f);
	return -angleToVectorXZ(player.angle.getYaw()) * back;
}
