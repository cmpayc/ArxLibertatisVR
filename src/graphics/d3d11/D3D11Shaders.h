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

#ifndef ARX_GRAPHICS_D3D11_D3D11SHADERS_H
#define ARX_GRAPHICS_D3D11_D3D11SHADERS_H

/*
 * HLSL emulation of the OpenGL fixed-function state used by the OpenGL backend:
 *  - pre-transformed (TL) and model-view-projection transformed vertices
 *  - up to three GL_COMBINE texture stages (replace / modulate 1x 2x 4x)
 *  - linear fog based on the eye-plane distance
 *  - alpha test
 *
 * Clip-space z is remapped from the OpenGL [-w, w] range to the Direct3D [0, w]
 * range so that clipping and depth values are identical to the OpenGL backend.
 */
static const char d3d11ShaderSource[] = R"HLSL(

cbuffer VSConstants : register(b0) {
	float4x4 transform;
	float4 jitter; // xy = sub-pixel offset in normalized device coordinates
	float4 camera; // World position of the camera
	float4x4 modelToWorld; // From pre-transformed model vertices to world space, for per-pixel highlights
	float4x4 worldToClip; // From world space to clip space in the 3D scene, for tessellated models
};

cbuffer PSConstants : register(b0) {
	int4 stages[3]; // x = color op, y = alpha op, z = has texture
	float4 fogColor;
	float4 fogParams; // x = start, y = end, z = enabled, w = alpha reference (< 0: no test)
	float4 reliefParams; // x = normal map slope scale, y = parallax depth relative to the texture size
	float4 specularParams; // x = highlight strength, y = exponent, z = metallic
	float4 surfaceParams; // x = reflectivity, y = 1 for blended draws, z = 1 for water surfaces, w = 1 for models
};

cbuffer ClearConstants : register(b1) {
	float4 clearColor;
	float4 clearDepth;
};

// Lights for per-pixel lighting of world geometry, see ApplyTileLights()
cbuffer LightConstants : register(b6) {
	int4 lightCount; // x = dynamic lights, y = debug view, z = all lights including the ones only for highlights
	float4 lights[256]; // Per light: position + fall start, color * intensity + fall end
};

Texture2D texture0 : register(t0);
Texture2D texture1 : register(t1);
Texture2D texture2 : register(t2);
Texture2D reliefMap : register(t3); // Slope and height of the stage 0 texture, see psNormalMap
SamplerState sampler0 : register(s0);
SamplerState sampler1 : register(s1);
SamplerState sampler2 : register(s2);

#ifdef SAMPLE_SHADING
#define INTERPOLATION sample
#else
#define INTERPOLATION
#endif

struct VSOutput {
	float4 position : SV_Position;
	float4 color : COLOR0;
	float2 uv0 : TEXCOORD0;
	float2 uv1 : TEXCOORD1;
	float2 uv2 : TEXCOORD2;
	float fog : TEXCOORD3;
	float4 clip : TEXCOORD4;     // Clip position without jitter
	float4 prevClip : TEXCOORD5; // Clip position in the previous frame, w <= 0 if unknown
	float3 world : TEXCOORD6;    // World position for per-pixel lighting
	float3 normal : TEXCOORD7;   // World normal for per-pixel lighting, zero if not lit
	float3 view : TEXCOORD8;     // From the world position to the camera
};

struct PSInput {
	float4 position : SV_Position;
	float4 color : COLOR0;
	INTERPOLATION float2 uv0 : TEXCOORD0;
	INTERPOLATION float2 uv1 : TEXCOORD1;
	INTERPOLATION float2 uv2 : TEXCOORD2;
	float fog : TEXCOORD3;
	float4 clip : TEXCOORD4;
	float4 prevClip : TEXCOORD5;
	INTERPOLATION float3 world : TEXCOORD6;
	INTERPOLATION float3 normal : TEXCOORD7;
	float3 view : TEXCOORD8;
};

struct PSOutput {
	float4 color : SV_Target0;
	float4 motion : SV_Target1; // xy = previous - current position in NDC, w = 1 if valid
	float4 surface : SV_Target2; // xy = encoded normal, z = reflectivity, w = 1 to replace the old value
};

VSOutput makeOutput(float4 clip, float4 prevClip, float4 color, float2 uv0, float2 uv1, float2 uv2) {
	VSOutput o;
	o.position = float4(clip.xy + jitter.xy * clip.w, (clip.z + clip.w) * 0.5, clip.w);
	o.color = color;
	o.uv0 = uv0;
	o.uv1 = uv1;
	o.uv2 = uv2;
	// The eye-plane distance - both the TL and the projection matrices put view.z into w
	o.fog = clip.w;
	o.clip = clip;
	o.prevClip = prevClip;
	o.world = float3(0, 0, 0);
	o.normal = float3(0, 0, 0);
	o.view = float3(0, 0, 0);
	return o;
}

VSOutput vsTL(float4 position : POSITION, float4 color : COLOR, float2 uv : TEXCOORD0,
              float4 prevPosition : PREVPOSITION, float3 normal : NORMAL) {
	float4 prevClip = prevPosition.w > 0 ? mul(transform, prevPosition) : float4(0, 0, 0, 0);
	VSOutput o = makeOutput(mul(transform, position), prevClip, color, uv, float2(0, 0), float2(0, 0));
	// Models provide a world space normal for per-pixel highlights
	if(dot(normal, normal) > 0) {
		float4 world = mul(modelToWorld, position);
		o.world = world.xyz / world.w;
		o.normal = normal;
		o.view = camera.xyz - o.world;
	}
	return o;
}

VSOutput vsWorld(float3 position : POSITION, float4 color : COLOR, float2 uv : TEXCOORD0,
                 float3 normal : NORMAL) {
	VSOutput o = makeOutput(mul(transform, float4(position, 1)), float4(0, 0, 0, 0), color, uv, float2(0, 0),
	                        float2(0, 0));
	o.world = position;
	o.normal = normal;
	o.view = camera.xyz - position;
	return o;
}

// Distance range where the parallax mapping fades out
static const float ParallaxFadeStart = 1200;
static const float ParallaxFadeEnd = 2000;

// Diffuse light of the first diffuseCount lights and Blinn-Phong highlights of all lights if the exponent is not
// zero, with the same falloff as ApplyTileLights()
void pixelLighting(float3 position, float3 normal, float3 view, int diffuseCount, int count, float exponent,
                   out float3 diffuse, out float3 specular) {
	diffuse = float3(0, 0, 0);
	specular = float3(0, 0, 0);
	[loop] for(int i = 0; i < count; i++) {
		float4 a = lights[i * 2];
		float4 b = lights[i * 2 + 1];
		float3 toLight = a.xyz - position;
		float distance2 = dot(toLight, toLight);
		if(distance2 >= b.w * b.w) {
			continue;
		}
		float distance = sqrt(distance2);
		float3 direction = toLight / max(distance, 0.0001);
		float cosAngle = dot(normal, direction);
		if(cosAngle <= 0) {
			continue;
		}
		float falloff = (distance <= a.w) ? 1 : saturate((b.w - distance) / max(b.w - a.w, 0.0001));
		if(i < diffuseCount) {
			diffuse += b.rgb * (cosAngle * falloff);
		}
		if(exponent > 0) {
			float cosHalf = saturate(dot(normal, normalize(direction + view)));
			specular += b.rgb * (pow(cosHalf, exponent) * falloff);
		}
	}
	// Sharper highlights are brighter so that they keep about the same energy
	specular *= (exponent + 8) / 32;
}

VSOutput vsWorld3(float3 position : POSITION, float4 color : COLOR, float2 uv0 : TEXCOORD0,
                  float2 uv1 : TEXCOORD1, float2 uv2 : TEXCOORD2) {
	VSOutput o = makeOutput(mul(transform, float4(position, 1)), float4(0, 0, 0, 0), color, uv0, uv1, uv2);
	o.world = position;
	o.view = camera.xyz - position;
	return o;
}

float3 combineColor(int op, float3 previous, float3 texel) {
	if(op == 1) {
		return texel;
	} else if(op == 2) {
		return texel * previous;
	} else if(op == 3) {
		return texel * previous * 2;
	} else if(op == 4) {
		return texel * previous * 4;
	}
	return previous;
}

float combineAlpha(int op, float previous, float texel) {
	if(op == 1) {
		return texel;
	} else if(op == 2) {
		return texel * previous;
	} else if(op == 3) {
		return texel * previous * 2;
	} else if(op == 4) {
		return texel * previous * 4;
	}
	return previous;
}

float4 combine(int4 stage, float4 previous, float4 texel) {
	return saturate(float4(combineColor(stage.x, previous.rgb, texel.rgb),
	                       combineAlpha(stage.y, previous.a, texel.a)));
}

)HLSL"
R"HLSL(
// Octahedral encoding of a unit vector into [0, 1]^2
float2 encodeNormal(float3 n) {
	n /= abs(n.x) + abs(n.y) + abs(n.z);
	float2 e = n.xy;
	if(n.z < 0) {
		e = (1 - abs(n.yx)) * float2(n.x >= 0 ? 1 : -1, n.y >= 0 ? 1 : -1);
	}
	return e * 0.5 + 0.5;
}

float3 decodeNormal(float2 e) {
	e = e * 2 - 1;
	float3 n = float3(e, 1 - abs(e.x) - abs(e.y));
	float t = saturate(-n.z);
	n.xy += float2(n.x >= 0 ? -t : t, n.y >= 0 ? -t : t);
	return normalize(n);
}

PSOutput psMain(PSInput input
#ifdef SAMPLE_SHADING
                , uint sampleIndex : SV_SampleIndex
#endif
                ) {
	
	float4 color = input.color;
	
	// Gradients must be computed outside of flow control
	float2 dx0 = ddx(input.uv0), dy0 = ddy(input.uv0);
	float2 dx1 = ddx(input.uv1), dy1 = ddy(input.uv1);
	float2 dx2 = ddx(input.uv2), dy2 = ddy(input.uv2);
	float3 worldDx = ddx(input.world), worldDy = ddy(input.world);
	
	// The vertex color holds the static lighting
	float normalLength = dot(input.normal, input.normal);
	float3 normal = input.normal * rsqrt(max(normalLength, 1e-20));
	// Water surfaces face up, with ripples from the two animated layers of the water texture
	bool water = surfaceParams.z != 0;
	[branch] if(water) {
		float2 ripple = texture0.SampleGrad(sampler0, input.uv0, dx0, dy0).rg
		                - texture1.SampleGrad(sampler1, input.uv1, dx1, dy1).rg;
		normal = normalize(float3(ripple.x * 0.3, -1, ripple.y * 0.3));
		normalLength = 1;
	}
	bool relief = normalLength > 0.01 && stages[0].w != 0;
	
	// Tangent frame from the screen-space derivatives (Christian Schüler, "Normal Mapping Without Precomputed
	// Tangents"): the world-space gradients of the texture coordinates, multiplied by det
	float3 dp2perp = cross(worldDy, normal);
	float3 dp1perp = cross(normal, worldDx);
	float3 gradU = dp2perp * dx0.x + dp1perp * dy0.x;
	float3 gradV = dp2perp * dx0.y + dp1perp * dy0.y;
	
	// Parallax occlusion mapping: march the view ray through the height field below the surface, with the
	// average height at the surface
	float2 uv0 = input.uv0;
	[branch] if(relief && reliefParams.y > 0) {
		float det = dot(worldDx, dp2perp);
		float viewDistance = length(input.view);
		float3 view = input.view / max(viewDistance, 0.0001);
		float cosView = dot(view, normal);
		float fade = saturate((ParallaxFadeEnd - viewDistance) / (ParallaxFadeEnd - ParallaxFadeStart));
		float uvPerWorld = sqrt(length(gradU) * length(gradV)) / max(abs(det), 1e-20);
		[branch] if(cosView > 0.05 && fade > 0 && abs(det) > 1e-20 && uvPerWorld > 0) {
			// Texture coordinate change from the surface to the full depth along the view ray
			float depth = reliefParams.y * fade / uvPerWorld;
			float2 offset = -float2(dot(gradU, view), dot(gradV, view)) / det * (depth / max(cosView, 0.2));
			int steps = int(lerp(16, 6, cosView));
			float stepSize = 1.0 / steps;
			float2 uv = uv0 - offset * 0.5;
			float2 delta = offset * stepSize;
			float layer = 0;
			float surface = 1 - reliefMap.SampleGrad(sampler0, uv, dx0, dy0).b;
			float2 previousUV = uv;
			[loop] for(int i = 0; i < steps && layer < surface; i++) {
				previousUV = uv;
				uv += delta;
				layer += stepSize;
				surface = 1 - reliefMap.SampleGrad(sampler0, uv, dx0, dy0).b;
			}
			// Refine the intersection between the last two steps with a binary search
			float2 startUV = previousUV;
			float startLayer = layer - stepSize;
			float2 endUV = uv;
			float endLayer = layer;
			[unroll] for(int j = 0; j < 4; j++) {
				float2 middleUV = (startUV + endUV) * 0.5;
				float middleLayer = (startLayer + endLayer) * 0.5;
				if(middleLayer < 1 - reliefMap.SampleGrad(sampler0, middleUV, dx0, dy0).b) {
					startUV = middleUV;
					startLayer = middleLayer;
				} else {
					endUV = middleUV;
					endLayer = middleLayer;
				}
			}
			uv0 = (startUV + endUV) * 0.5;
		}
	}
	
	// Bumped normal for the lighting and the reflections
	[branch] if(relief && reliefParams.x > 0) {
		float scale = rsqrt(max(max(dot(gradU, gradU), dot(gradV, gradV)), 1e-20));
		// Height grows with the brightness, the surface normal leans against the slope
		float2 slope = (reliefMap.SampleGrad(sampler0, uv0, dx0, dy0).rg * 2 - 1) * reliefParams.x;
		float3 bumped = normalize(normal - (gradU * slope.x + gradV * slope.y) * scale);
		if(all(isfinite(bumped))) {
			normal = bumped;
		}
	}
	
	// Add the dynamic lights
	float3 dynamicLight = float3(0, 0, 0);
	float3 specular = float3(0, 0, 0);
	bool shiny = specularParams.x > 0 && lightCount.z > 0;
	[branch] if((lightCount.x > 0 || shiny) && normalLength > 0.01) {
		// Water is drawn on top of the lit ground and models are already lit, they only get the highlights
		int diffuseCount = (water || surfaceParams.w != 0) ? 0 : lightCount.x;
		pixelLighting(input.world, normal, normalize(input.view), diffuseCount, shiny ? lightCount.z : diffuseCount,
		              shiny ? specularParams.y : 0, dynamicLight, specular);
		specular *= specularParams.x;
		color.rgb = saturate(color.rgb + dynamicLight);
	}
	
	float4 texel0 = float4(1, 1, 1, 1);
	[branch] if(stages[0].z != 0) {
		texel0 = texture0.SampleGrad(sampler0, uv0, dx0, dy0);
		color = combine(stages[0], color, texel0);
	}
	[branch] if(stages[1].z != 0) {
		color = combine(stages[1], color, texture1.SampleGrad(sampler1, input.uv1, dx1, dy1));
	}
	[branch] if(stages[2].z != 0) {
		color = combine(stages[2], color, texture2.SampleGrad(sampler2, input.uv2, dx2, dy2));
	}
	
	// Metals tint the highlights with their color, darker parts of other surfaces are less shiny
	[branch] if(any(specular > 0)) {
		float luminance = dot(texel0.rgb, float3(0.299, 0.587, 0.114));
		float3 tint = lerp(saturate(luminance * 2).xxx, texel0.rgb * 1.5, specularParams.z);
		color.rgb = saturate(color.rgb + specular * tint);
	}
	
	if(fogParams.z != 0) {
		float f = saturate((fogParams.y - input.fog) / max(fogParams.y - fogParams.x, 0.0001));
		color.rgb = lerp(fogColor.rgb, color.rgb, f);
	}
	
	if(color.a <= fogParams.w) {
		discard;
	}
	
	// Debug view: only the dynamic light of world geometry lit per pixel
	if(lightCount.y == 1 && normalLength > 0.01) {
		color.rgb = saturate(dynamicLight);
	}
	// Debug view: only the highlights of models
	if(lightCount.y == 2) {
		color.rgb = (surfaceParams.w != 0) ? saturate(specular * 4) : float3(0, 0, 0);
	}
	
	PSOutput o;
	o.color = color;
	// Per-object motion, only written for opaque geometry while the motion target is bound
	o.motion = float4(0, 0, 0, 0);
	if(input.prevClip.w > 0 && input.clip.w > 0) {
		o.motion = float4(input.prevClip.xy / input.prevClip.w - input.clip.xy / input.clip.w, 0, 1);
	}
	// Normal and reflectivity for the screen-space reflections: opaque draws always replace them, blended draws
	// only for reflective surfaces like water
	float reflectivity = (normalLength > 0.01) ? surfaceParams.x : 0;
	o.surface = float4(encodeNormal(normal), reflectivity, (surfaceParams.y == 0 || reflectivity > 0) ? 1 : 0);
	return o;
}

float4 vsClear(uint id : SV_VertexID) : SV_Position {
	float2 p = float2((id << 1) & 2, id & 2);
	return float4(p * float2(2, -2) + float2(-1, 1), clearDepth.x, 1);
}

float4 psClear() : SV_Target {
	return clearColor;
}

struct BlitOutput {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};

BlitOutput vsBlit(uint id : SV_VertexID) {
	BlitOutput o;
	float2 p = float2((id << 1) & 2, id & 2);
	o.position = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
	o.uv = p;
	return o;
}

float4 psBlit(BlitOutput input) : SV_Target {
	return texture0.Sample(sampler0, input.uv);
}

cbuffer MotionConstants : register(b2) {
	float4x4 reprojection; // current clip space to previous clip space
	float4 targetSize; // xy = size, zw = 1 / size
};

#ifdef MSAA_DEPTH
Texture2DMS<float> sceneDepth : register(t1);
#else
Texture2D<float> sceneDepth : register(t1);
#endif
Texture2D<float4> objectMotion : register(t2);

struct MotionOutput {
	float2 motion : SV_Target0;
	float depth : SV_Target1;
};

/*
 * Camera motion vectors reconstructed from the depth buffer.
 * Depth values are in the OpenGL window range, which maps the projection's [0, 1] depth to [0.5, 1].
 */
MotionOutput psMotion(float4 position : SV_Position) {
	
	int2 pixel = int2(position.xy);
	#ifdef MSAA_DEPTH
	float depth = sceneDepth.Load(pixel, 0);
	#else
	float depth = sceneDepth.Load(int3(pixel, 0));
	#endif
	
	float z = depth * 2 - 1;
	float2 uv = position.xy * targetSize.zw;
	float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
	
	float4 previous = mul(reprojection, float4(ndc, z, 1));
	float2 previousNdc = previous.xy / previous.w;
	float2 previousUV = float2(previousNdc.x * 0.5 + 0.5, 0.5 - previousNdc.y * 0.5);
	
	MotionOutput o;
	o.motion = (previousUV - uv) * targetSize.xy;
	o.depth = saturate(z);
	
	// Moving objects (characters, items) provide their own motion
	float4 object = objectMotion.Load(int3(pixel, 0));
	if(object.w > 0) {
		o.motion = object.xy * float2(0.5, -0.5) * targetSize.xy;
	}
	
	return o;
}

float4 psDebugDepth(BlitOutput input) : SV_Target {
	// Approximate view distance for a projection with the near plane at 1
	float z = texture0.Sample(sampler0, input.uv).r;
	float distance = 1 / max(1 - z, 0.000001);
	return float4((1 - saturate(distance / 3000)).xxx, 1);
}

float4 psDebugMotion(BlitOutput input) : SV_Target {
	float2 motion = texture0.Sample(sampler0, input.uv).rg;
	// Full red / green at one pixel of horizontal / vertical motion per frame, blue at ten pixels
	return float4(saturate(abs(motion)), saturate(length(motion) * 0.1), 1);
}

float4 psDebugObjectMotion(BlitOutput input) : SV_Target {
	// Green where objects provide their own motion, red for the amount (in normalized device coordinates)
	float4 motion = texture0.Sample(sampler0, input.uv);
	return float4(saturate(length(motion.xy) * 300), motion.w * 0.5, 0, 1);
}

)HLSL" // MSVC limits the length of a single string literal
R"HLSL(

/*
 * Screen-space ambient occlusion, based on Scalable Ambient Obscurance (McGuire et al. 2012).
 * Positions and normals are reconstructed from the scene depth written by psMotion.
 */

cbuffer AmbientOcclusionConstants : register(b3) {
	float4 aoTarget;     // xy = size, zw = 1 / size
	float4 aoProjection; // x = 1 / projection[0][0], y = 1 / projection[1][1], z = near, w = far
	float4 aoParams;     // x = radius in world units, y = strength, z = bias, w = projected radius scale
};

Texture2D<float> aoDepth : register(t0);
Texture2D<float> aoInput : register(t1);

// The ambient occlusion is computed at half the scene size from every second depth pixel
static const int AoScale = 2;

int2 aoHalfSize() {
	return (int2(aoTarget.xy) + AoScale - 1) / AoScale;
}

float aoViewDistance(float depth) {
	return aoProjection.z * aoProjection.w / (aoProjection.w - depth * (aoProjection.w - aoProjection.z));
}

float3 aoViewPosition(int2 pixel) {
	pixel = clamp(pixel, int2(0, 0), int2(aoTarget.xy) - 1);
	float distance = aoViewDistance(aoDepth.Load(int3(pixel, 0)));
	float2 ndc = (float2(pixel) + 0.5) * aoTarget.zw * float2(2, -2) + float2(-1, 1);
	return float3(ndc * aoProjection.xy * distance, distance);
}

float psAmbientOcclusion(float4 position : SV_Position) : SV_Target {
	
	int2 pixel = int2(position.xy) * AoScale;
	float depth = aoDepth.Load(int3(pixel, 0));
	if(depth >= 0.99999) {
		return 1; // Sky or nothing rendered
	}
	
	float3 center = aoViewPosition(pixel);
	
	// Normal from the neighbors with the smaller depth difference to avoid artifacts at edges
	float3 left = center - aoViewPosition(pixel - int2(1, 0));
	float3 right = aoViewPosition(pixel + int2(1, 0)) - center;
	float3 up = center - aoViewPosition(pixel - int2(0, 1));
	float3 down = aoViewPosition(pixel + int2(0, 1)) - center;
	float3 dx = (abs(left.z) < abs(right.z)) ? left : right;
	float3 dy = (abs(up.z) < abs(down.z)) ? up : down;
	float3 normal = normalize(cross(dy, dx));
	if(dot(normal, center) > 0) {
		normal = -normal;
	}
	
	// Interleaved gradient noise rotates the sample spiral per pixel, the blur removes the pattern
	float noise = frac(52.9829189 * frac(dot(position.xy, float2(0.06711056, 0.00583715))));
	float angle = noise * 6.28318531;
	
	float radius = aoParams.x;
	float screenRadius = aoParams.w * radius / center.z;
	if(screenRadius < 1) {
		return 1;
	}
	screenRadius = min(screenRadius, 256);
	
	const int samples = 16;
	const float turns = 7;
	float radius2 = radius * radius;
	float sum = 0;
	[unroll] for(int i = 0; i < samples; i++) {
		float alpha = (float(i) + 0.5) / float(samples);
		float a = alpha * turns * 6.28318531 + angle;
		int2 offset = int2(float2(cos(a), sin(a)) * alpha * screenRadius);
		float3 v = aoViewPosition(pixel + offset) - center;
		float vv = dot(v, v);
		float vn = dot(v, normal);
		float f = max(radius2 - vv, 0);
		sum += f * f * f * max((vn - aoParams.z * center.z * 0.01) / (vv + 0.01 * radius2), 0);
	}
	
	// Scaled by the radius so that the result does not depend on the world units
	float occlusion = sum * 5 * radius / (radius2 * radius2 * radius2 * float(samples));
	return saturate(1 - occlusion);
}

// Depth-aware blur, horizontal or vertical
float psAmbientOcclusionBlur(float4 position : SV_Position) : SV_Target {
	
	int2 pixel = int2(position.xy);
	#ifdef BLUR_VERTICAL
	int2 axis = int2(0, 1);
	#else
	int2 axis = int2(1, 0);
	#endif
	
	float centerDistance = aoViewDistance(aoDepth.Load(int3(pixel * AoScale, 0)));
	float sum = 0;
	float weights = 0;
	[unroll] for(int i = -4; i <= 4; i++) {
		int2 p = clamp(pixel + axis * i, int2(0, 0), aoHalfSize() - 1);
		float distance = aoViewDistance(aoDepth.Load(int3(p * AoScale, 0)));
		float weight = exp(-float(i * i) / 12) * exp(-abs(distance - centerDistance) / (centerDistance * 0.02 + 1));
		sum += aoInput.Load(int3(p, 0)) * weight;
		weights += weight;
	}
	
	return sum / weights;
}

// Upsampled from the four nearest half size values with similar depth, multiplied into the scene color by the
// blend state
float4 psApplyAmbientOcclusion(float4 position : SV_Position) : SV_Target {
	int2 pixel = int2(position.xy);
	float centerDistance = aoViewDistance(aoDepth.Load(int3(pixel, 0)));
	float2 halfPosition = (float2(pixel) + 0.5) / AoScale - 0.5;
	int2 base = int2(floor(halfPosition));
	float2 f = halfPosition - float2(base);
	float sum = 0;
	float weights = 0;
	[unroll] for(int i = 0; i < 4; i++) {
		int2 offset = int2(i & 1, i >> 1);
		int2 p = clamp(base + offset, int2(0, 0), aoHalfSize() - 1);
		float2 bilinear = lerp(1 - f, f, float2(offset));
		float distance = aoViewDistance(aoDepth.Load(int3(p * AoScale, 0)));
		float weight = bilinear.x * bilinear.y * exp(-abs(distance - centerDistance) / (centerDistance * 0.02 + 1)) + 1e-4;
		sum += aoInput.Load(int3(p, 0)) * weight;
		weights += weight;
	}
	float ao = lerp(1, sum / weights, aoParams.y);
	return float4(ao.xxx, 1);
}

float4 psDebugAmbientOcclusion(BlitOutput input) : SV_Target {
	return float4(texture0.Sample(sampler0, input.uv).rrr, 1);
}

)HLSL"
R"HLSL(
/*
 * Screen-space reflections: the reflected view ray of reflective pixels is marched through the scene depth and the
 * scene color where it hits is blended in, blurrier for rougher surfaces. Positions are reconstructed like for the
 * ambient occlusion.
 */

cbuffer ReflectionConstants : register(b7) {
	float4 ssrTarget;     // xy = size, zw = 1 / size
	float4 ssrProjection; // x = 1 / projection[0][0], y = 1 / projection[1][1], z = near, w = far
	float4 ssrView[3];    // Rows of the rotation from world space to the reconstructed view space
	float4 ssrParams;     // x = strength, y = maximum ray length, z = mip levels of the scene copy, w = debug view
};

Texture2D<float> ssrDepth : register(t0);
Texture2D<float4> ssrSurface : register(t1);
Texture2D<float4> ssrColor : register(t2);

float ssrViewDistance(int2 pixel) {
	pixel = clamp(pixel, int2(0, 0), int2(ssrTarget.xy) - 1);
	float depth = ssrDepth.Load(int3(pixel, 0));
	return ssrProjection.z * ssrProjection.w / (ssrProjection.w - depth * (ssrProjection.w - ssrProjection.z));
}

float2 ssrProject(float3 position) {
	float2 ndc = position.xy / (position.z * ssrProjection.xy);
	return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

// How far behind the depth buffer a ray may be and still count as a hit
float ssrThickness(float rayLength, float distance) {
	return 20 + rayLength * 0.1 + distance * 0.01;
}

float4 psReflection(float4 position : SV_Position) : SV_Target {
	
	int2 pixel = int2(position.xy);
	float4 surface = ssrSurface.Load(int3(pixel, 0));
	float reflectivity = surface.z;
	if(reflectivity <= 0.004 || ssrDepth.Load(int3(pixel, 0)) >= 0.99999) {
		discard;
	}
	
	float distance = ssrViewDistance(pixel);
	float2 ndc = (float2(pixel) + 0.5) * ssrTarget.zw * float2(2, -2) + float2(-1, 1);
	float3 origin = float3(ndc * ssrProjection.xy * distance, distance);
	float3 view = normalize(origin);
	
	float3 worldNormal = decodeNormal(surface.xy);
	float3 normal = normalize(float3(dot(ssrView[0].xyz, worldNormal), dot(ssrView[1].xyz, worldNormal),
	                                 dot(ssrView[2].xyz, worldNormal)));
	float cosView = -dot(view, normal);
	if(cosView <= 0) {
		discard;
	}
	float3 direction = reflect(view, normal);
	
	// Start slightly above the surface and stop in front of the near plane
	origin += normal * (1 + distance * 0.002);
	float maxLength = ssrParams.y;
	if(direction.z < 0) {
		maxLength = min(maxLength, (origin.z - ssrProjection.z * 2) / -direction.z);
	}
	if(maxLength <= 0) {
		discard;
	}
	
	// The steps grow with the distance, interleaved gradient noise hides the banding
	const int steps = 32;
	float noise = frac(52.9829189 * frac(dot(position.xy, float2(0.06711056, 0.00583715))));
	float previousLength = 0;
	float hitLength = -1;
	[loop] for(int i = 1; i <= steps; i++) {
		float f = (float(i) - noise) / float(steps);
		float rayLength = f * f * maxLength;
		float3 p = origin + direction * rayLength;
		float2 uv = ssrProject(p);
		if(any(uv < 0) || any(uv > 1)) {
			break;
		}
		float behind = p.z - ssrViewDistance(int2(uv * ssrTarget.xy));
		if(behind > 0 && behind < ssrThickness(rayLength, p.z)) {
			hitLength = rayLength;
			break;
		}
		previousLength = rayLength;
	}
	if(hitLength < 0) {
		discard;
	}
	
	// Refine the hit between the last two steps
	float low = previousLength;
	float high = hitLength;
	[unroll] for(int j = 0; j < 5; j++) {
		float middle = (low + high) * 0.5;
		float3 p = origin + direction * middle;
		if(p.z > ssrViewDistance(int2(ssrProject(p) * ssrTarget.xy))) {
			high = middle;
		} else {
			low = middle;
		}
	}
	float2 hit = ssrProject(origin + direction * high);
	
	// Fade out near the screen edges, at the end of the ray and for rays coming back to the camera, which
	// often hit something that is hidden in the depth buffer
	float2 edge = min(hit, 1 - hit);
	float fade = saturate(min(edge.x, edge.y) * 10);
	fade *= 1 - saturate(high / ssrParams.y) * saturate(high / ssrParams.y);
	fade *= saturate(direction.z * 4 + 1);
	
	// Rough surfaces get blurry reflections, more so the further the reflected object is
	float roughness = 1 - reflectivity;
	float level = min(roughness * roughness * 2 * saturate(high / 400), ssrParams.z - 1);
	float3 color = ssrColor.SampleLevel(sampler0, hit, level).rgb;
	
	// More reflection at grazing angles (Schlick's approximation of the Fresnel factor)
	float fresnel = pow(1 - cosView, 5);
	float alpha = saturate(reflectivity * ssrParams.x * 3 * lerp(0.4, 1, fresnel)) * fade;
	
	// The reflection replaces only half as much of the surface color as it adds, so that reflections of darker
	// surroundings do not make wet surfaces darker
	return float4(color * alpha, (ssrParams.w != 0) ? 1 : alpha * 0.5);
}

/*
 * Bloom: bright parts of the scene are extracted, blurred through a chain of downsampled textures and
 * added back to the scene (similar to the Call of Duty: Advanced Warfare bloom).
 */

cbuffer BloomConstants : register(b4) {
	float4 bloomParams; // x = threshold, y = soft knee, z = intensity
	float4 bloomSource; // xy = 1 / source size
};

float3 bloomThreshold(float3 color) {
	float brightness = max(color.r, max(color.g, color.b));
	float knee = bloomParams.y;
	float soft = clamp(brightness - bloomParams.x + knee, 0, 2 * knee);
	soft = soft * soft / (4 * knee + 0.00001);
	float contribution = max(soft, brightness - bloomParams.x) / max(brightness, 0.00001);
	return color * contribution;
}

float bloomKarisWeight(float3 color) {
	// Averaging by luma weight avoids flickering from single bright pixels
	return 1 / (1 + dot(color, float3(0.2126, 0.7152, 0.0722)));
}

// First downsample from the scene with the threshold
float4 psBloomPrefilter(BlitOutput input) : SV_Target {
	float2 t = bloomSource.xy;
	float3 a = bloomThreshold(texture0.Sample(sampler0, input.uv + t * float2(-1, -1)).rgb);
	float3 b = bloomThreshold(texture0.Sample(sampler0, input.uv + t * float2(1, -1)).rgb);
	float3 c = bloomThreshold(texture0.Sample(sampler0, input.uv + t * float2(-1, 1)).rgb);
	float3 d = bloomThreshold(texture0.Sample(sampler0, input.uv + t * float2(1, 1)).rgb);
	float wa = bloomKarisWeight(a), wb = bloomKarisWeight(b), wc = bloomKarisWeight(c), wd = bloomKarisWeight(d);
	return float4((a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd), 1);
}

float4 psBloomDownsample(BlitOutput input) : SV_Target {
	float2 t = bloomSource.xy;
	float3 sum = texture0.Sample(sampler0, input.uv + t * float2(-1, -1)).rgb
	           + texture0.Sample(sampler0, input.uv + t * float2(1, -1)).rgb
	           + texture0.Sample(sampler0, input.uv + t * float2(-1, 1)).rgb
	           + texture0.Sample(sampler0, input.uv + t * float2(1, 1)).rgb;
	return float4(sum * 0.25, 1);
}

// 3x3 tent filter, added to the next larger level by the blend state
float4 psBloomUpsample(BlitOutput input) : SV_Target {
	float2 t = bloomSource.xy;
	float3 sum = texture0.Sample(sampler0, input.uv).rgb * 4;
	sum += (texture0.Sample(sampler0, input.uv + t * float2(-1, 0)).rgb
	        + texture0.Sample(sampler0, input.uv + t * float2(1, 0)).rgb
	        + texture0.Sample(sampler0, input.uv + t * float2(0, -1)).rgb
	        + texture0.Sample(sampler0, input.uv + t * float2(0, 1)).rgb) * 2;
	sum += texture0.Sample(sampler0, input.uv + t * float2(-1, -1)).rgb
	       + texture0.Sample(sampler0, input.uv + t * float2(1, -1)).rgb
	       + texture0.Sample(sampler0, input.uv + t * float2(-1, 1)).rgb
	       + texture0.Sample(sampler0, input.uv + t * float2(1, 1)).rgb;
	return float4(sum / 16, 1);
}

// Added to the scene by the blend state
float4 psBloomComposite(BlitOutput input) : SV_Target {
	return float4(psBloomUpsample(input).rgb * bloomParams.z, 1);
}

/*
 * Color grading, applied while copying the scene to the window.
 */

cbuffer GradingConstants : register(b5) {
	float4 gradeParams;    // x = contrast, y = saturation, z = vignette strength, w = window aspect ratio
	float4 gradeTint;      // rgb = color multiplier
	float4 gradeShadows;   // rgb = color offset in dark areas
	float4 gradeHighlights; // rgb = color offset in bright areas
	float4 gradeTone;      // x = exposure, y = gamma (< 1 brightens dark areas), z = contrast pivot
};

float4 psColorGrading(BlitOutput input) : SV_Target {

	float3 color = texture0.Sample(sampler0, input.uv).rgb * gradeTint.rgb * gradeTone.x;

	// Arx scenes are dark, lift the shadows and mid tones without changing white
	color = pow(max(color, 0), gradeTone.y);

	// Contrast around a dark pivot so that most of the image does not get darker
	color = (color - gradeTone.z) * gradeParams.x + gradeTone.z;
	
	float luma = dot(saturate(color), float3(0.2126, 0.7152, 0.0722));
	color = lerp(luma.xxx, color, gradeParams.y);
	
	// Split toning: separate tints for shadows and highlights
	color += gradeShadows.rgb * (1 - smoothstep(0, 0.5, luma));
	color += gradeHighlights.rgb * smoothstep(0.4, 1, luma);
	
	// Darken towards the corners
	float2 offset = (input.uv - 0.5) * float2(gradeParams.w, 1);
	float vignette = smoothstep(0.35, 1.1, length(offset) * 1.4);
	color *= 1 - gradeParams.z * vignette;
	
	return float4(saturate(color), 1);
}

// Brightness of a texture mip level with bilinear filtering and clamping at the edges
float coarseBrightness(float2 uv, int level) {
	uint width, height, levels;
	texture0.GetDimensions(level, width, height, levels);
	float2 p = uv * float2(width, height) - 0.5;
	int2 base = int2(floor(p));
	float2 f = p - base;
	int2 last = int2(width, height) - 1;
	float h[2][2];
	[unroll] for(int y = 0; y < 2; y++) {
		[unroll] for(int x = 0; x < 2; x++) {
			int2 texel = clamp(base + int2(x, y), 0, last);
			h[y][x] = dot(texture0.Load(int3(texel, level)).rgb, float3(0.2126, 0.7152, 0.0722));
		}
	}
	return lerp(lerp(h[0][0], h[0][1], f.x), lerp(h[1][0], h[1][1], f.x), f.y);
}

/*
 * Relief map generation: the brightness of a texture is used as height.
 *  - rg: the Sobel filter of the height gives the slope (height change per texel in u and v), scaled into [0, 1].
 *    Most textures are not seamless, so the samples are clamped to the edge instead of wrapping: comparing
 *    opposite edges would put a ridge around the border.
 *  - b: the height for parallax mapping, from a mip level of about 64 texels so that it follows the large shapes
 *    (stones and joints) instead of the fine detail, relative to the average brightness of the texture (its last
 *    mip level) with 0.5 at the average.
 */
float4 psNormalMap(float4 position : SV_Position) : SV_Target {
	
	uint width, height, levels;
	texture0.GetDimensions(0, width, height, levels);
	int2 size = int2(width, height);
	int2 pixel = int2(position.xy);
	
	float h[3][3];
	[unroll] for(int y = 0; y < 3; y++) {
		[unroll] for(int x = 0; x < 3; x++) {
			int2 p = clamp(pixel + int2(x - 1, y - 1), 0, size - 1);
			h[y][x] = dot(texture0.Load(int3(p, 0)).rgb, float3(0.2126, 0.7152, 0.0722));
		}
	}
	
	float du = (h[0][2] + 2 * h[1][2] + h[2][2]) - (h[0][0] + 2 * h[1][0] + h[2][0]);
	float dv = (h[2][0] + 2 * h[2][1] + h[2][2]) - (h[0][0] + 2 * h[0][1] + h[0][2]);
	
	// Flatten the outermost texels so that filtering across the border (wrap) blends equal slopes
	if(any(pixel == 0 || pixel == size - 1)) {
		du = dv = 0;
	}
	
	int level = clamp(int(log2(float(max(width, height)))) - 6, 0, int(levels) - 1);
	float coarse = coarseBrightness((float2(pixel) + 0.5) / float2(size), level);
	float average = dot(texture0.Load(int3(0, 0, levels - 1)).rgb, float3(0.2126, 0.7152, 0.0722));
	float relativeHeight = 0.5 + (coarse - average) * 2;
	
	return saturate(float4(du * 0.125 + 0.5, dv * 0.125 + 0.5, relativeHeight, 1));
}

)HLSL";

// The hull shader is compiled from this part alone, it crashes the shader compiler in the full source
static const char d3d11ModelPatchSource[] = R"HLSL(

/*
 * Tessellated models: model triangles from the CPU become curved point-normal triangles (Vlachos et al. 2001)
 * like in animation/MeshSmoothing.cpp. The corners stay where the CPU put them and the curved surface is
 * added as an offset, so that tessellated and other triangles of a model meet exactly.
 */

struct ModelPatchVertex {
	float3 world : WORLDPOS;
	float3 normal : NORMAL;
	float4 color : COLOR0;
	float2 uv : TEXCOORD0;
	float4 clip : TEXCOORD4;
	float4 prevClip : TEXCOORD5;
	uint tessellation : TESSELLATION;
};

struct ModelPatchConstants {
	float edges[3] : SV_TessFactor;
	float inside : SV_InsideTessFactor;
	// Control points of the cubic Bezier triangle, bijk has the weight a^i b^j c^k
	float3 b210 : CONTROLPOINT0;
	float3 b120 : CONTROLPOINT1;
	float3 b021 : CONTROLPOINT2;
	float3 b012 : CONTROLPOINT3;
	float3 b102 : CONTROLPOINT4;
	float3 b201 : CONTROLPOINT5;
	float3 b111 : CONTROLPOINT6;
};

// Control point next to corner p on the edge towards q, projected onto the tangent plane at p if curve is 1
float3 modelEdgeControlPoint(float3 p, float3 q, float3 n, float curve) {
	return (2 * p + q - dot(q - p, n) * n * curve) / 3;
}

ModelPatchConstants hsModelPatchConstants(InputPatch<ModelPatchVertex, 3> patch) {
	
	// All corners have the same tessellation value
	uint tessellation = patch[0].tessellation;
	float segments = max(float(tessellation >> 3), 1);
	
	// The shader compiler crashes for conditionally straight edges, use weights instead
	float curve01 = 1 - float(tessellation & 1);
	float curve12 = 1 - float((tessellation >> 1) & 1);
	float curve20 = 1 - float((tessellation >> 2) & 1);
	
	float3 p0 = patch[0].world;
	float3 p1 = patch[1].world;
	float3 p2 = patch[2].world;
	float3 n0 = patch[0].normal;
	float3 n1 = patch[1].normal;
	float3 n2 = patch[2].normal;
	float3 b210 = modelEdgeControlPoint(p0, p1, n0, curve01);
	float3 b120 = modelEdgeControlPoint(p1, p0, n1, curve01);
	float3 b021 = modelEdgeControlPoint(p1, p2, n1, curve12);
	float3 b012 = modelEdgeControlPoint(p2, p1, n2, curve12);
	float3 b102 = modelEdgeControlPoint(p2, p0, n2, curve20);
	float3 b201 = modelEdgeControlPoint(p0, p2, n0, curve20);
	float3 e = (b210 + b120 + b021 + b012 + b102 + b201) / 6;
	float3 v = (p0 + p1 + p2) / 3;
	
	ModelPatchConstants c;
	c.edges[0] = segments;
	c.edges[1] = segments;
	c.edges[2] = segments;
	c.inside = segments;
	c.b210 = b210;
	c.b120 = b120;
	c.b021 = b021;
	c.b012 = b012;
	c.b102 = b102;
	c.b201 = b201;
	c.b111 = e + (e - v) * 0.5;
	
	return c;
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_ccw")]
[outputcontrolpoints(3)]
[patchconstantfunc("hsModelPatchConstants")]
ModelPatchVertex hsModelPatch(InputPatch<ModelPatchVertex, 3> patch, uint id : SV_OutputControlPointID) {
	return patch[id];
}

)HLSL";

// Compiled after d3d11ShaderSource and d3d11ModelPatchSource
static const char d3d11ModelPatchStagesSource[] = R"HLSL(

ModelPatchVertex vsModelPatch(float4 position : POSITION, float4 color : COLOR, float2 uv : TEXCOORD0,
                              float4 prevPosition : PREVPOSITION, float3 normal : NORMAL,
                              uint tessellation : TESSELLATION) {
	ModelPatchVertex o;
	float4 world = mul(modelToWorld, position);
	o.world = world.xyz / world.w;
	o.normal = normal;
	o.color = color;
	o.uv = uv;
	o.clip = mul(transform, position);
	o.prevClip = prevPosition.w > 0 ? mul(transform, prevPosition) : float4(0, 0, 0, 0);
	o.tessellation = tessellation;
	return o;
}

[domain("tri")]
VSOutput dsModelPatch(ModelPatchConstants c, float3 location : SV_DomainLocation,
                      const OutputPatch<ModelPatchVertex, 3> patch) {
	
	float a = location.x;
	float b = location.y;
	float d = location.z;
	
	float3 flat = a * patch[0].world + b * patch[1].world + d * patch[2].world;
	float3 world = a * a * a * patch[0].world + b * b * b * patch[1].world + d * d * d * patch[2].world
	               + 3 * a * a * b * c.b210 + 3 * a * b * b * c.b120
	               + 3 * b * b * d * c.b021 + 3 * b * d * d * c.b012
	               + 3 * d * d * a * c.b102 + 3 * d * a * a * c.b201
	               + 6 * a * b * d * c.b111;
	
	// The curved surface is an offset to the flat triangle, which moves with it between frames
	float4 offset = mul(worldToClip, float4(world - flat, 0));
	float4 clip = a * patch[0].clip + b * patch[1].clip + d * patch[2].clip + offset;
	float4 prevClip = float4(0, 0, 0, 0);
	if(patch[0].prevClip.w > 0 && patch[1].prevClip.w > 0 && patch[2].prevClip.w > 0) {
		prevClip = a * patch[0].prevClip + b * patch[1].prevClip + d * patch[2].prevClip + offset;
	}
	
	float4 color = a * patch[0].color + b * patch[1].color + d * patch[2].color;
	float2 uv = a * patch[0].uv + b * patch[1].uv + d * patch[2].uv;
	VSOutput o = makeOutput(clip, prevClip, color, uv, float2(0, 0), float2(0, 0));
	
	float3 normal = a * patch[0].normal + b * patch[1].normal + d * patch[2].normal;
	if(dot(normal, normal) > 0) {
		o.world = world;
		o.normal = normalize(normal);
		o.view = camera.xyz - world;
	}
	
	return o;
}

)HLSL";

#endif // ARX_GRAPHICS_D3D11_D3D11SHADERS_H
