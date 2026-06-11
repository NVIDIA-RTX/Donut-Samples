/*
* Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#include <donut/shaders/binding_helpers.hlsli>

void main_vs(
    uint i_vertex: SV_VertexID,
    out float4 o_position: SV_Position,
    out float2 o_uv: TEXCOORD0
)
{
    o_uv = float2(i_vertex % 2, i_vertex / 2);

    o_position = float4(o_uv * 2.0f - 1.0f, 0.0f, 1.0f);
    o_position.y *= -1;
}

Texture2D texture0 : REGISTER_SRV(0, 0);
SamplerState sampler0 : REGISTER_SAMPLER(0, 0);

float4 main_ps(
    float4 i_position: SV_Position,
    float2 i_uv: TEXCOORD0,
    in bool i_front: SV_IsFrontFace) : SV_Target0
{
    return texture0.Sample(sampler0, i_uv);
}

struct Constants
{
    uint counter;
};
DECLARE_PUSH_CONSTANTS(Constants, g_Constants, 0, 0);
RWTexture2D<float4> rwTexture0 : REGISTER_UAV(0, 0);

//--------------------------------------------------------------------------------------
// Simplex Noise https://en.wikipedia.org/wiki/Simplex_noise
//--------------------------------------------------------------------------------------
// Simplex 2D noise
//

#define mod(x, y) (x - y * floor(x / y))

float permute(float x) { return floor(mod(((x * 34.0) + 1.0) * x, 289.0)); }
float3 permute(float3 x) { return mod(((x * 34.0) + 1.0) * x, 289.0); }
float4 permute(float4 x) { return mod(((x * 34.0) + 1.0) * x, 289.0); }
float taylorInvSqrt(float r) { return 1.79284291400159 - 0.85373472095314 * r;}
float4 taylorInvSqrt(float4 r) { return float4(taylorInvSqrt(r.x), taylorInvSqrt(r.y), taylorInvSqrt(r.z), taylorInvSqrt(r.w)); }

float snoise(float2 v) {
    const float4 C = float4(0.211324865405187, 0.366025403784439, -0.577350269189626, 0.024390243902439);
    float2 i = floor(v + dot(v, C.yy));
    float2 x0 = v - i + dot(i, C.xx);
    float2 i1;
    i1 = (x0.x > x0.y) ? float2(1.0, 0.0) : float2(0.0, 1.0);
    float4 x12 = x0.xyxy + C.xxzz;
    x12.xy -= i1;
    i = mod(i, 289.0);
    float3 p = permute(permute(i.y + float3(0.0, i1.y, 1.0)) + i.x + float3(0.0, i1.x, 1.0));
    float3 m = max(0.5 - float3(dot(x0, x0), dot(x12.xy, x12.xy), dot(x12.zw, x12.zw)), 0.0);
    m = m * m;
    m = m * m;
    float3 x = 2.0 * frac(p * C.www) - 1.0;
    float3 h = abs(x) - 0.5;
    float3 ox = floor(x + 0.5);
    float3 a0 = x - ox;
    m *= 1.79284291400159 - 0.85373472095314 * (a0 * a0 + h * h);
    float3 g;
    g.x = a0.x * x0.x + h.x * x0.y;
    g.yz = a0.yz * x12.xz + h.yz * x12.yw;
    return 130.0 * dot(m, g);
}


[numthreads(8, 8, 1)]
void main_cs(
	uint3 dtid : SV_DispatchThreadID
)
{
    float2 uv = dtid.xy / float2(512, 512);
    float offset = g_Constants.counter * 0.005f;

    rwTexture0[dtid.xy] = float4(
		snoise(uv + offset * float2(0.7f, 0.1f)),
		snoise(uv + offset * float2(0.2f, -0.6f)),
		snoise(uv + offset * float2(-0.5f, 0.3f)),
		1
	);
}
