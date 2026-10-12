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
};

cbuffer PSConstants : register(b0) {
	int4 stages[3]; // x = color op, y = alpha op, z = has texture
	float4 fogColor;
	float4 fogParams; // x = start, y = end, z = enabled, w = alpha reference (< 0: no test)
};

cbuffer ClearConstants : register(b1) {
	float4 clearColor;
	float4 clearDepth;
};

Texture2D texture0 : register(t0);
Texture2D texture1 : register(t1);
Texture2D texture2 : register(t2);
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
};

struct PSOutput {
	float4 color : SV_Target0;
	float4 motion : SV_Target1; // xy = previous - current position in NDC, w = 1 if valid
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
	return o;
}

VSOutput vsTL(float4 position : POSITION, float4 color : COLOR, float2 uv : TEXCOORD0,
              float4 prevPosition : PREVPOSITION) {
	float4 prevClip = prevPosition.w > 0 ? mul(transform, prevPosition) : float4(0, 0, 0, 0);
	return makeOutput(mul(transform, position), prevClip, color, uv, float2(0, 0), float2(0, 0));
}

VSOutput vsWorld(float3 position : POSITION, float4 color : COLOR, float2 uv : TEXCOORD0) {
	return makeOutput(mul(transform, float4(position, 1)), float4(0, 0, 0, 0), color, uv, float2(0, 0),
	                  float2(0, 0));
}

VSOutput vsWorld3(float3 position : POSITION, float4 color : COLOR, float2 uv0 : TEXCOORD0,
                  float2 uv1 : TEXCOORD1, float2 uv2 : TEXCOORD2) {
	return makeOutput(mul(transform, float4(position, 1)), float4(0, 0, 0, 0), color, uv0, uv1, uv2);
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
	
	[branch] if(stages[0].z != 0) {
		color = combine(stages[0], color, texture0.SampleGrad(sampler0, input.uv0, dx0, dy0));
	}
	[branch] if(stages[1].z != 0) {
		color = combine(stages[1], color, texture1.SampleGrad(sampler1, input.uv1, dx1, dy1));
	}
	[branch] if(stages[2].z != 0) {
		color = combine(stages[2], color, texture2.SampleGrad(sampler2, input.uv2, dx2, dy2));
	}
	
	if(fogParams.z != 0) {
		float f = saturate((fogParams.y - input.fog) / max(fogParams.y - fogParams.x, 0.0001));
		color.rgb = lerp(fogColor.rgb, color.rgb, f);
	}
	
	if(color.a <= fogParams.w) {
		discard;
	}
	
	PSOutput o;
	o.color = color;
	// Per-object motion, only written for opaque geometry while the motion target is bound
	o.motion = float4(0, 0, 0, 0);
	if(input.prevClip.w > 0 && input.clip.w > 0) {
		o.motion = float4(input.prevClip.xy / input.prevClip.w - input.clip.xy / input.clip.w, 0, 1);
	}
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
	
	int2 pixel = int2(position.xy);
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
	
	float centerDistance = aoViewDistance(aoDepth.Load(int3(pixel, 0)));
	float sum = 0;
	float weights = 0;
	[unroll] for(int i = -4; i <= 4; i++) {
		int2 p = clamp(pixel + axis * i, int2(0, 0), int2(aoTarget.xy) - 1);
		float distance = aoViewDistance(aoDepth.Load(int3(p, 0)));
		float weight = exp(-float(i * i) / 12) * exp(-abs(distance - centerDistance) / (centerDistance * 0.02 + 1));
		sum += aoInput.Load(int3(p, 0)) * weight;
		weights += weight;
	}
	
	return sum / weights;
}

// Multiplied into the scene color by the blend state
float4 psApplyAmbientOcclusion(float4 position : SV_Position) : SV_Target {
	float ao = lerp(1, aoInput.Load(int3(int2(position.xy), 0)), aoParams.y);
	return float4(ao.xxx, 1);
}

float4 psDebugAmbientOcclusion(BlitOutput input) : SV_Target {
	return float4(texture0.Sample(sampler0, input.uv).rrr, 1);
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

)HLSL";

#endif // ARX_GRAPHICS_D3D11_D3D11SHADERS_H
