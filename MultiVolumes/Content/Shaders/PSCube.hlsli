//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConsts.h"

//--------------------------------------------------------------------------------------
// Buffers and textures
//--------------------------------------------------------------------------------------
StructuredBuffer<PerObject> g_roPerObject	: register (t0);

TextureCube	g_txCubeMaps[]			: register (t0, space3);

#ifdef _HAS_DEPTH_MAP_
TextureCube<float> g_txCubeDepths[]	: register (t0, space4);
#endif

//--------------------------------------------------------------------------------------
// Unproject and return z in viewing space
//--------------------------------------------------------------------------------------
float UnprojectZ(float depth)
{
	static const float3 unproj = { g_zNear - g_zFar, g_zFar, g_zNear * g_zFar };

	return unproj.z / (depth * unproj.x + unproj.y);
}

//--------------------------------------------------------------------------------------
// Get domain location
//--------------------------------------------------------------------------------------
min16float2 GetDomain(float2 uv, float3 pos, float3 rayDir, float2 gridSize)
{
	uv *= gridSize;
	float2 domain = frac(uv + 0.5);

	const float bound = gridSize.x - 1.0;
	const float3 axes = pos * gridSize.x;
	if (any(abs(axes) > bound && axes * rayDir < 0.0))
	{
		// Need to clamp the exterior edge
		uv = min(uv, gridSize - 0.5);
		domain = uv < 0.5 ? 1.0 : 0.0;
	}

	return min16float2(domain);
}

//--------------------------------------------------------------------------------------
// Cube interior-surface casting
//--------------------------------------------------------------------------------------
min16float4 CubeCast(uint2 idx, float3 uvw, float3 pos, float3 rayDir, uint srvIdx)
{
	float2 gridSize;
	const TextureCube txCubeMap = g_txCubeMaps[NonUniformResourceIndex(srvIdx)];
	txCubeMap.GetDimensions(gridSize.x, gridSize.y);
	float2 uv = uvw.xy;
	uvw = pos;

	const float4 color = txCubeMap.SampleLevel(g_smpLinear, uvw, 0.0);
	const float4x4 gathers =
	{
		txCubeMap.GatherRed(g_smpLinear, uvw),
		txCubeMap.GatherGreen(g_smpLinear, uvw),
		txCubeMap.GatherBlue(g_smpLinear, uvw),
		txCubeMap.GatherAlpha(g_smpLinear, uvw)
	};

#ifdef _HAS_DEPTH_MAP_
	const float4 z = g_txCubeDepths[NonUniformResourceIndex(srvIdx)].Gather(g_smpLinear, uvw);
	float depth = g_txDepth[idx];
#endif

	const min16float2 domain = GetDomain(uv, pos, rayDir, gridSize);
	const min16float2 domainInv = 1.0 - domain;
	const min16float4 wb =
	{
		domainInv.x * domain.y,
		domain.x * domain.y,
		domain.x * domainInv.y,
		domainInv.x * domainInv.y
	};

	const min16float4x4 samples = transpose(min16float4x4(gathers));
#ifdef _HAS_DEPTH_MAP_
	depth = UnprojectZ(depth);
#endif
	min16float4 result = 0.0;
	min16float ws = 0.0;
	[unroll]
	for (uint i = 0; i < 4; ++i)
	{
#ifdef _HAS_DEPTH_MAP_
		const float zi = UnprojectZ(z[i]);
		min16float w = min16float(max(1.0 - 0.5 * abs(depth - zi), 0.0));
		w *= wb[i];
#else
		const min16float w = wb[i];
#endif

		result += samples[i] * w;
		ws += w;
	}

	//result = min16float4(color); // Reference
	result = ws > 0.0 ? result / ws : min16float4(color);

	return result;
}
