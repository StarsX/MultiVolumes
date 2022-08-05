//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define _HAS_DEPTH_MAP_

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
	bool CubeRM : SCHEME;
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

	for (uint i = 0; i < NUM_OIT_LAYERS; ++i)
	{
		const uint3 uvw = { uv, i };

		if (g_txKDepths[uvw] == depth)
		{
			min16float4 color;
#if _ADAPTIVE_RAYMARCH_
			if (input.CubeRM)
#endif
				color = CubeCast(uv, input.UVW, input.LPt, rayDir, input.SrvId);
#if _ADAPTIVE_RAYMARCH_
			else color = 0.0; // color = RayCast(); TODO: screen-space ray marching
#endif
			if (color.w > 0.0 && color.w <= 1.0) g_rwKColors[uvw] = color;
		}
	}
}
