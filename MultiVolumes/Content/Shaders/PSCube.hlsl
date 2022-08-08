//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define _HAS_DEPTH_MAP_

#include "RayCast.hlsli"
#include "PSCube.hlsli"

//--------------------------------------------------------------------------------------
// Structure
//--------------------------------------------------------------------------------------
struct PSIn
{
	float4 Pos	: SV_POSITION;
	float3 UVW	: TEXCOORD;
	float3 LPt	: POSLOCAL;
	uint VolId	: VOLUMEID;
	uint SrvId	: SRVINDEX;
	uint TexId	: VOLTEXID;
	uint SmpCnt : SAMPLECOUNT;
};

RWTexture2DArray<float4>	g_rwKColors;
Texture2DArray<uint>		g_txKDepths : register (t1);

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
void main(PSIn input)
{
	const PerObject perObject = g_roPerObject[input.VolId];
	const float3 localSpaceEyePt = mul(float4(g_eyePt, 1.0), perObject.WorldI);
	const float3 rayDir = input.LPt - localSpaceEyePt;

	const uint2 uv = input.Pos.xy;
	const uint depth = asuint(input.Pos.z);
	float2 xy = input.Pos.xy / g_viewport;
	xy = xy * 2.0 - 1.0;
	xy.y = -xy.y;

	for (uint i = 0; i < NUM_OIT_LAYERS; ++i)
	{
		const uint3 uvw = { uv, i };

		if (g_txKDepths[uvw] == depth)
		{
			min16float4 color;
#if _ADAPTIVE_RAYMARCH_
			if (input.SmpCnt > 0)
				color = RayCast(uv, xy, localSpaceEyePt, normalize(rayDir), input.TexId,
					input.SmpCnt, perObject.WorldViewProjI, perObject.ToLightSpace);
			else
#endif
				color = CubeCast(uv, input.UVW, input.LPt, rayDir, input.SrvId);

			if (color.w > 0.0 && color.w <= 1.0) g_rwKColors[uvw] = color;
		}
	}
}
