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
};

struct PSInput {
	float4 position : SV_Position;
	float4 color : COLOR0;
	INTERPOLATION float2 uv0 : TEXCOORD0;
	INTERPOLATION float2 uv1 : TEXCOORD1;
	INTERPOLATION float2 uv2 : TEXCOORD2;
	float fog : TEXCOORD3;
};

VSOutput makeOutput(float4 clip, float4 color, float2 uv0, float2 uv1, float2 uv2) {
	VSOutput o;
	o.position = float4(clip.xy, (clip.z + clip.w) * 0.5, clip.w);
	o.color = color;
	o.uv0 = uv0;
	o.uv1 = uv1;
	o.uv2 = uv2;
	// The eye-plane distance - both the TL and the projection matrices put view.z into w
	o.fog = clip.w;
	return o;
}

VSOutput vsTL(float4 position : POSITION, float4 color : COLOR, float2 uv : TEXCOORD0) {
	return makeOutput(mul(transform, position), color, uv, float2(0, 0), float2(0, 0));
}

VSOutput vsWorld(float3 position : POSITION, float4 color : COLOR, float2 uv : TEXCOORD0) {
	return makeOutput(mul(transform, float4(position, 1)), color, uv, float2(0, 0), float2(0, 0));
}

VSOutput vsWorld3(float3 position : POSITION, float4 color : COLOR, float2 uv0 : TEXCOORD0,
                  float2 uv1 : TEXCOORD1, float2 uv2 : TEXCOORD2) {
	return makeOutput(mul(transform, float4(position, 1)), color, uv0, uv1, uv2);
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

float4 psMain(PSInput input
#ifdef SAMPLE_SHADING
              , uint sampleIndex : SV_SampleIndex
#endif
              ) : SV_Target {
	
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
	
	return color;
}

float4 vsClear(uint id : SV_VertexID) : SV_Position {
	float2 p = float2((id << 1) & 2, id & 2);
	return float4(p * float2(2, -2) + float2(-1, 1), clearDepth.x, 1);
}

float4 psClear() : SV_Target {
	return clearColor;
}

)HLSL";

#endif // ARX_GRAPHICS_D3D11_D3D11SHADERS_H
