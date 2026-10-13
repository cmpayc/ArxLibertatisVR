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

#ifndef ARX_ANIMATION_MESHSMOOTHING_H
#define ARX_ANIMATION_MESHSMOOTHING_H

#include <stddef.h>

#include "math/Types.h"

struct ColorMod;
struct EERIE_3DOBJ;
struct ShaderLight;
struct TexturedVertex;

/*!
 * Models are drawn as curved point-normal triangles (Vlachos et al. 2001):
 * each triangle is split into segments^2 smaller ones that are placed on a cubic patch
 * through the corners, using smooth normals computed from the transformed vertex positions.
 * Neighboring patches share the same edge curves so no cracks appear between them.
 * Creases and non-manifold edges stay straight to keep sharp features intact.
 * The same normals are used to light the model so that the shading is smooth across triangles.
 */

//! Lights for shading the vertices of smoothed triangles
struct MeshLighting {
	
	ShaderLight * lights;
	size_t lightsCount;
	const ColorMod * colorMod;
	
};

//! Largest number of segments per edge
const int MeshSmoothingMaxSegments = 4;

//! Number of segments each edge is split into for a smoothing level (0 = off to 3) and a distance
int meshSmoothingSegments(int level, float distance);

//! Update the smooth normals of an animated object after its vertices have been transformed
void updateMeshSmoothing(EERIE_3DOBJ & eobj);

//! Smooth world space normal of a vertex of an object prepared with \ref updateMeshSmoothing
const Vec3f & meshSmoothingNormal(const EERIE_3DOBJ & eobj, size_t vertex);

/*!
 * Split a face of an object prepared with \ref updateMeshSmoothing into curved triangles
 *
 * \param corners  The face vertices as they would be drawn without smoothing
 * \param lighting Lights for the new vertices, or nullptr to interpolate the corner colors
 * \param out      Receives segments^2 * 3 vertices
 *
 * \return the number of triangles written
 */
size_t smoothFace(const EERIE_3DOBJ & eobj, size_t face, const TexturedVertex corners[3],
                  int segments, const MeshLighting * lighting, TexturedVertex * out);

#endif // ARX_ANIMATION_MESHSMOOTHING_H
