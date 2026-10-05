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
#include <map>
#include <sstream>
#include <string_view>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include "animation/Skeleton.h"
#include "core/Config.h"
#include "game/Entity.h"
#include "game/Player.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/Math.h"
#include "io/log/Logger.h"
#include "vr/VRCamera.h"
#include "vr/VRControls.h"
#include "vr/VRMath.h"
#include "vr/VRSystem.h"


namespace {

std::string formatBodyVector(const Vec3f & v) {
	std::ostringstream oss;
	oss.precision(1);
	oss << std::fixed << '(' << v.x << ", " << v.y << ", " << v.z << ')';
	return oss.str();
}

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
	[[nodiscard]] bool valid() const { return shoulder && elbow && wrist && hand && indexRoot && thumb; }
};

struct SpineBones {
	VertexGroupId belt;  //!< Lower spine
	VertexGroupId chest; //!< Upper spine
	VertexGroupId neck;
	[[nodiscard]] bool valid() const { return belt && chest && neck; }
};

struct BodyBones {
	const EERIE_3DOBJ * object = nullptr;
	SpineBones spine;
	ArmBones arms[VRHandCount];
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
		bone.anim.scale = (bone.init.scale + Vec3f(1.f)) * parent.anim.scale;
	}
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
 * Bend the spine so that the neck of the model is below and behind the eyes of the user:
 * leaning and peeking tilt the torso instead of moving the feet. The lower spine and the chest share the bend.
 */
void solveSpine(Skeleton & skeleton, const SpineBones & spine) {
	
	Vec3f eyes;
	if(!getVREyeCenter(eyes)) {
		return;
	}
	
	/*
	 * The neck joint is 17 cm below the eyes. It is placed further behind the eyes than in the player model
	 * (6.6 cm) so that the view is not inside the neck opening of the hidden head.
	 */
	Vec3f forward = angleToVectorXZ(player.angle.getYaw());
	Vec3f neck = eyes + Vec3f(0.f, 17.f, 0.f) - forward * (config.vr.bodyOffset * 100.f);
	
	aimBonePartially(skeleton, spine.belt, spine.neck, neck, 0.5f);
	aimBone(skeleton, spine.chest, spine.neck, neck);
	
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
const Vec3f FingerDirection(0.f, -0.5f, -1.f); //!< From the wrist to the knuckles
const Vec3f ThumbDirection(0.f, 1.f, 0.f);     //!< The thumb rests on top of the controller

/*!
 * Curl the fingers of the model with the controller: the grip closes the middle, ring and little fingers,
 * the trigger the index finger. Touch sensors lower the thumb and the index finger when they rest on the
 * controller. The finger animation of the game is replaced - except while holding something (see applyVRBodyIK).
 */
void curlFingers(Skeleton & skeleton, const ArmBones & arm, const VRHandState & state) {
	
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
	
	const float relaxed = 0.12f; // A slight curl even when open
	float grip = glm::mix(relaxed, 1.f, state.squeeze);
	float index = glm::mix(relaxed, 1.f, state.trigger);
	if(state.hasIndexTouch && state.indexTouch) {
		// Resting on the trigger
		index = std::max(index, 0.35f);
	}
	
	// With touch sensors the thumb lies on the controller while touching it and is raised otherwise
	float thumbAngle = 25.f + 15.f * state.squeeze;
	if(state.hasThumbTouch) {
		thumbAngle = state.thumbTouch ? 40.f + 15.f * state.squeeze : 0.f;
	}
	
	auto curl = [&](VertexGroupId bone, const Vec3f & axis, float degrees) {
		if(bone) {
			skeleton.bones[bone].init.quat = glm::angleAxis(glm::radians(degrees), axis);
		}
	};
	curl(arm.fingersRoot, fingerAxis, 80.f * grip);
	curl(arm.fingersTip, fingerAxis, 60.f * grip);
	curl(arm.indexRoot, fingerAxis, 75.f * index);
	curl(arm.indexTip, fingerAxis, 55.f * index);
	curl(arm.thumb, thumbAxis, thumbAngle);
	
	updateBoneDescendants(skeleton, arm.hand);
}

void solveArm(Skeleton & skeleton, const ArmBones & arm, VRHand hand, const VRPose & grip) {

	Vec3f gripPosition;
	glm::quat gripOrientation;
	if(!grip.valid || !trackingPoseToWorld(grip.position, grip.orientation, gripPosition, gripOrientation)) {
		return;
	}

	const float scale = 100.f * config.vr.worldScale;
	Vec3f target = gripPosition + gripOrientation * (vr::toArxBasis(config.vr.handOffset) * scale);

	const Vec3f shoulder = skeleton.bones[arm.shoulder].anim.trans;
	const float upperLength = glm::distance(shoulder, skeleton.bones[arm.elbow].anim.trans);
	const float lowerLength = glm::distance(skeleton.bones[arm.elbow].anim.trans, skeleton.bones[arm.wrist].anim.trans);

	// Out of reach: stretch the arm towards the target
	Vec3f toTarget = target - shoulder;
	float distance = glm::length(toTarget);
	if(distance < 0.001f) {
		return;
	}
	Vec3f direction = toTarget / distance;
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
	 * The wrist bone carries the lower half of the forearm (rigid skinning), so it must stay aligned with the
	 * forearm: only give it half of the hand's twist around the forearm axis, like a real forearm.
	 */
	Bone & wrist = skeleton.bones[arm.wrist];
	Vec3f forearm = wrist.anim.trans - skeleton.bones[arm.elbow].anim.trans;
	if(glm::length(forearm) > 0.001f) {
		forearm = glm::normalize(forearm);
		glm::quat change = handOrientation * glm::inverse(wrist.anim.quat);
		if(change.w < 0.f) {
			change = -change;
		}
		Vec3f axis = forearm * glm::dot(Vec3f(change.x, change.y, change.z), forearm);
		glm::quat twist(change.w, axis.x, axis.y, axis.z);
		if(glm::length(twist) > 0.001f) {
			twist = glm::normalize(twist);
			wrist.anim.quat = glm::normalize(glm::slerp(glm::quat(1.f, 0.f, 0.f, 0.f), twist, 0.5f) * wrist.anim.quat);
			updateBoneDescendants(skeleton, arm.wrist);
		}
	}
	
	skeleton.bones[arm.hand].anim.quat = handOrientation;
	updateBoneDescendants(skeleton, arm.hand);

}

//! The attach vertex for objects held in a hand of the player model
VertexId getHoldVertex(const EERIE_3DOBJ & object, VRHand hand) {
	return (hand == VRHandRight) ? object.fastaccess.primary_attach : object.fastaccess.left_attach;
}

//! \return true if the player model holds something (weapon, torch) in a hand
bool isHoldingItem(const EERIE_3DOBJ & object, VRHand hand) {
	VertexId vertex = getHoldVertex(object, hand);
	for(const EERIE_LINKED & link : object.linked) {
		if(vertex && link.lidx == vertex && link.obj) {
			return true;
		}
	}
	return false;
}

//! Direction of the longest extent of an object from its attach point (the blade of a weapon), in object space
Vec3f getHeldItemAxis(const EERIE_3DOBJ & object, VertexId attach) {
	
	static std::map<const EERIE_3DOBJ *, Vec3f> s_axes;
	auto it = s_axes.find(&object);
	if(it != s_axes.end()) {
		return it->second;
	}
	
	Vec3f axis(0.f);
	if(attach && size_t(attach) < object.vertexlist.size()) {
		const Vec3f origin = object.vertexlist[attach].v;
		float farthest = 0.f;
		for(const EERIE_VERTEX & vertex : object.vertexlist) {
			float distance = glm::distance(vertex.v, origin);
			if(distance > farthest) {
				farthest = distance;
				axis = vertex.v - origin;
			}
		}
		if(farthest > 0.001f) {
			axis = glm::normalize(axis);
		}
	}
	
	s_axes[&object] = axis;
	return axis;
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
	Vec3f current = skeleton.bones[held->lgroup].anim.quat * axis;
	Vec3f wanted = gripOrientation * Vec3f(0.f, 0.f, 1.f);
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
	const VRInput & input = g_vr->getInput();
	
	// The spine first: it moves the shoulders
	if(bones.spine.valid()) {
		solveSpine(skeleton, bones.spine);
	}

	for(size_t hand = 0; hand < VRHandCount; hand++) {
		const ArmBones & arm = bones.arms[hand];
		if(arm.valid()) {
			const VRHandState & state = input.getHand(VRHand(hand));
			solveArm(skeleton, arm, VRHand(hand), state.grip);
			if(state.grip.valid) {
				if(isHoldingItem(*entity.obj, VRHand(hand))) {
					// The animation of the game wraps the fingers around the handle: keep it, only aim the item
					alignHeldItem(skeleton, *entity.obj, arm, VRHand(hand), state.grip);
				} else {
					curlFingers(skeleton, arm, state);
				}
			}
		}
	}

}

void logVRPlayerSkeleton(const Entity & entity) {

	static bool logged = false;
	if(logged || !entity.obj || !entity.obj->m_skeleton) {
		return;
	}
	logged = true;

	const EERIE_3DOBJ & obj = *entity.obj;
	const Skeleton & skeleton = *obj.m_skeleton;

	// Positions in the body frame: x right, y down, z forward, relative to the player position
	glm::mat3x3 toBody = glm::mat3x3(toRotationMatrix(Anglef(0.f, entity.angle.getYaw(), 0.f)));
	auto bodyPosition = [&](VertexId vertex) {
		return formatBodyVector(toBody * (obj.vertexWorldPositions[vertex].v - entity.pos));
	};

	LogInfo << "VR skeleton of " << obj.file << ": " << obj.grouplist.size() << " groups, "
	        << obj.vertexlist.size() << " vertices, " << obj.facelist.size() << " faces";

	for(VertexGroupId group : obj.grouplist.handles()) {
		const VertexGroup & info = obj.grouplist[group];
		const Bone & bone = skeleton.bones[group];
		LogInfo << "VR skeleton:  group " << size_t(group) << " \"" << info.name << "\" parent "
		        << (bone.father ? std::to_string(size_t(bone.father)) : std::string("-"))
		        << ", " << info.indexes.size() << " vertices, rest " << formatBodyVector(bone.transinit_global)
		        << ", now " << bodyPosition(info.origin);
	}

	for(VertexSelectionId selection : obj.selections.handles()) {
		LogInfo << "VR skeleton:  selection " << size_t(selection) << " \"" << obj.selections[selection].name
		        << "\", " << obj.selections[selection].selected.size() << " vertices";
	}

	for(const EERIE_ACTIONLIST & action : obj.actionlist) {
		LogInfo << "VR skeleton:  action point \"" << action.name << "\" vertex " << size_t(action.idx)
		        << " now " << bodyPosition(action.idx);
	}

	const EERIE_FASTACCESS & fast = obj.fastaccess;
	auto logAttach = [&](const char * name, VertexId vertex) {
		if(vertex) {
			LogInfo << "VR skeleton:  " << name << " vertex " << size_t(vertex) << " now " << bodyPosition(vertex);
		}
	};
	logAttach("view_attach", fast.view_attach);
	logAttach("primary_attach", fast.primary_attach);
	logAttach("left_attach", fast.left_attach);
	logAttach("weapon_attach", fast.weapon_attach);
	logAttach("head_group_origin", fast.head_group_origin);

}

Vec3f getVRModelOffset() {
	// The neck of the player model is 5 cm behind its feet: move the model back so that the torso stays upright
	// when the neck is placed config.vr.bodyOffset behind the eyes, which are above the feet
	float back = std::max(config.vr.bodyOffset * 100.f - 5.f, 0.f);
	return -angleToVectorXZ(player.angle.getYaw()) * back;
}