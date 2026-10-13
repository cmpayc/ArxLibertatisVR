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

#include "animation/MeshSmoothing.h"

#include <algorithm>
#include <array>
#include <map>
#include <utility>

#include "graphics/GraphicsTypes.h"
#include "graphics/Vertex.h"
#include "graphics/data/Mesh.h"
#include "scene/Light.h"

//! Edges between faces that meet at a sharper angle than this (cosine of 60 degrees) stay straight
static const float CreaseCosine = 0.5f;

//! Distances over which the number of segments is reduced
static const float SmoothingFullDistance = 1000.f;
static const float SmoothingEndDistance = 3000.f;

int meshSmoothingSegments(int level, float distance) {
	
	int segments = std::clamp(level, 0, MeshSmoothingMaxSegments - 1) + 1;
	
	if(segments > 1 && distance > SmoothingFullDistance) {
		float fade = (SmoothingEndDistance - distance) / (SmoothingEndDistance - SmoothingFullDistance);
		segments = 1 + int(float(segments - 1) * std::max(fade, 0.f) + 0.5f);
	}
	
	return segments;
}

int meshSmoothingSegmentsForSize(float pixels) {
	
	if(pixels < 48.f) {
		return 1;
	} else if(pixels < 128.f) {
		return 2;
	} else if(pixels < 256.f) {
		return 3;
	}
	
	return MeshSmoothingMaxSegments;
}

static Vec3f faceNormal(const Vec3f & a, const Vec3f & b, const Vec3f & c) {
	return glm::cross(b - a, c - a);
}

static void buildMeshSmoothing(EERIE_3DOBJ & eobj) {
	
	MeshSmoothingData & data = eobj.smoothing;
	
	data.faceCount = eobj.facelist.size();
	data.vertexCount = eobj.vertexlist.size();
	
	// Vertices at the same bind pose position share a normal so that their patches meet
	data.weld.resize(data.vertexCount);
	std::map<std::array<float, 3>, u16> positions;
	for(size_t i = 0; i < data.vertexCount; i++) {
		const Vec3f & p = eobj.vertexlist[VertexId(i)].v;
		data.weld[i] = positions.emplace(std::array<float, 3>{ p.x, p.y, p.z }, u16(i)).first->second;
	}
	data.normals.resize(data.vertexCount);
	
	// Find the edges where the surface is not smooth
	struct Edge {
		size_t count = 0;
		Vec3f normal = Vec3f(0.f);
		bool crease = false;
	};
	std::map<std::pair<u16, u16>, Edge> edges;
	auto edgeKey = [&](const EERIE_FACE & face, size_t k) {
		u16 a = data.weld[face.vid[k].handleData()];
		u16 b = data.weld[face.vid[(k + 1) % 3].handleData()];
		return std::make_pair(std::min(a, b), std::max(a, b));
	};
	for(const EERIE_FACE & face : eobj.facelist) {
		Vec3f normal = faceNormal(eobj.vertexlist[face.vid[0]].v, eobj.vertexlist[face.vid[1]].v,
		                          eobj.vertexlist[face.vid[2]].v);
		float length = glm::length(normal);
		normal = (length > 0.f) ? normal / length : Vec3f(0.f);
		for(size_t k = 0; k < 3; k++) {
			Edge & edge = edges[edgeKey(face, k)];
			if(edge.count == 1 && glm::dot(edge.normal, normal) < CreaseCosine) {
				edge.crease = true;
			}
			edge.normal = normal;
			edge.count++;
		}
	}
	
	data.creases.assign(data.faceCount, 0);
	for(size_t i = 0; i < data.faceCount; i++) {
		for(size_t k = 0; k < 3; k++) {
			const Edge & edge = edges[edgeKey(eobj.facelist[i], k)];
			if(edge.crease || edge.count > 2) {
				data.creases[i] |= u8(1 << k);
			}
		}
	}
	
}

void updateMeshSmoothing(EERIE_3DOBJ & eobj) {
	
	MeshSmoothingData & data = eobj.smoothing;
	
	if(data.faceCount != eobj.facelist.size() || data.vertexCount != eobj.vertexlist.size()) {
		buildMeshSmoothing(eobj);
	}
	
	std::fill(data.normals.begin(), data.normals.end(), Vec3f(0.f));
	
	// Area-weighted average of the normals of the faces around each vertex
	for(const EERIE_FACE & face : eobj.facelist) {
		Vec3f normal = faceNormal(eobj.vertexWorldPositions[face.vid[0]].v,
		                          eobj.vertexWorldPositions[face.vid[1]].v,
		                          eobj.vertexWorldPositions[face.vid[2]].v);
		for(size_t k = 0; k < 3; k++) {
			data.normals[data.weld[face.vid[k].handleData()]] += normal;
		}
	}
	
	for(Vec3f & normal : data.normals) {
		float length = glm::length(normal);
		normal = (length > 0.f) ? normal / length : Vec3f(0.f);
	}
	
}

const Vec3f & meshSmoothingNormal(const EERIE_3DOBJ & eobj, size_t vertex) {
	return eobj.smoothing.normals[eobj.smoothing.weld[vertex]];
}

//! Control point next to corner p on the edge towards q, projected onto the tangent plane at p
static Vec3f edgeControlPoint(const Vec3f & p, const Vec3f & q, const Vec3f & n, bool straight) {
	Vec3f point = (2.f * p + q) / 3.f;
	if(!straight) {
		point -= glm::dot(q - p, n) * n / 3.f;
	}
	return point;
}

size_t smoothFace(const EERIE_3DOBJ & eobj, size_t faceIndex, const TexturedVertex corners[3],
                  int segments, const MeshLighting * lighting, TexturedVertex * out) {
	
	const MeshSmoothingData & data = eobj.smoothing;
	const EERIE_FACE & face = eobj.facelist[faceIndex];
	
	Vec3f p[3];
	Vec3f n[3];
	for(size_t k = 0; k < 3; k++) {
		p[k] = eobj.vertexWorldPositions[face.vid[k]].v;
		n[k] = data.normals[data.weld[face.vid[k].handleData()]];
	}
	
	u8 creases = data.creases[faceIndex];
	bool straight01 = (creases & 1) != 0;
	bool straight12 = (creases & 2) != 0;
	bool straight20 = (creases & 4) != 0;
	
	// Cubic Bezier triangle, bijk is the control point with weight a^i b^j c^k
	Vec3f b210 = edgeControlPoint(p[0], p[1], n[0], straight01);
	Vec3f b120 = edgeControlPoint(p[1], p[0], n[1], straight01);
	Vec3f b021 = edgeControlPoint(p[1], p[2], n[1], straight12);
	Vec3f b012 = edgeControlPoint(p[2], p[1], n[2], straight12);
	Vec3f b102 = edgeControlPoint(p[2], p[0], n[2], straight20);
	Vec3f b201 = edgeControlPoint(p[0], p[2], n[0], straight20);
	Vec3f e = (b210 + b120 + b021 + b012 + b102 + b201) / 6.f;
	Vec3f v = (p[0] + p[1] + p[2]) / 3.f;
	Vec3f b111 = e + (e - v) * 0.5f;
	
	Vec4f clip[3];
	bool hasPrevious = true;
	for(size_t k = 0; k < 3; k++) {
		clip[k] = Vec4f(corners[k].p, corners[k].w);
		hasPrevious = hasPrevious && corners[k].prevP.w > 0.f;
	}
	
	std::array<TexturedVertex, (MeshSmoothingMaxSegments + 1) * (MeshSmoothingMaxSegments + 2) / 2> grid;
	auto gridIndex = [segments](int r, int s) {
		return size_t(r * (2 * segments + 3 - r) / 2 + s);
	};
	
	for(int r = 0; r <= segments; r++) {
		for(int s = 0; s <= segments - r; s++) {
			
			float b = float(r) / float(segments);
			float c = float(s) / float(segments);
			float a = 1.f - b - c;
			
			TexturedVertex & vertex = grid[gridIndex(r, s)];
			
			if(r + s == 0 || r == segments || s == segments) {
				vertex = corners[(r == segments) ? 1 : (s == segments) ? 2 : 0];
				continue;
			}
			
			Vec3f position = a * a * a * p[0] + b * b * b * p[1] + c * c * c * p[2]
			                 + 3.f * a * a * b * b210 + 3.f * a * b * b * b120
			                 + 3.f * b * b * c * b021 + 3.f * b * c * c * b012
			                 + 3.f * c * c * a * b102 + 3.f * c * a * a * b201
			                 + 6.f * a * b * c * b111;
			
			Vec4f projected = worldToClipSpace(position);
			vertex.p = Vec3f(projected);
			vertex.w = projected.w;
			
			// The curved surface moves with the flat triangle between frames
			if(hasPrevious) {
				Vec4f flat = a * clip[0] + b * clip[1] + c * clip[2];
				vertex.prevP = a * corners[0].prevP + b * corners[1].prevP + c * corners[2].prevP
				               + (projected - flat);
			} else {
				vertex.prevP = Vec4f(0.f);
			}
			
			vertex.uv = a * corners[0].uv + b * corners[1].uv + c * corners[2].uv;
			Vec3f normal = a * corners[0].normal + b * corners[1].normal + c * corners[2].normal;
			float normalLength = glm::length(normal);
			vertex.normal = (normalLength > 0.f) ? normal / normalLength : Vec3f(0.f);
			
			if(lighting) {
				vertex.color = ApplyLight(lighting->lights, lighting->lightsCount, position, vertex.normal,
				                          *lighting->colorMod);
			} else {
				Color4f color = Color4f::fromRGBA(corners[0].color) * a
				                + Color4f::fromRGBA(corners[1].color) * b
				                + Color4f::fromRGBA(corners[2].color) * c;
				vertex.color = color.toRGBA();
			}
			
		}
	}
	
	size_t count = 0;
	for(int r = 0; r < segments; r++) {
		for(int s = 0; s < segments - r; s++) {
			*out++ = grid[gridIndex(r, s)];
			*out++ = grid[gridIndex(r + 1, s)];
			*out++ = grid[gridIndex(r, s + 1)];
			count++;
			if(r + s + 2 <= segments) {
				*out++ = grid[gridIndex(r + 1, s)];
				*out++ = grid[gridIndex(r + 1, s + 1)];
				*out++ = grid[gridIndex(r, s + 1)];
				count++;
			}
		}
	}
	
	return count;
}
