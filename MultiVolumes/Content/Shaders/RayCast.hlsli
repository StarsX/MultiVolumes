//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen & ZENG, Wei. All rights reserved.
//--------------------------------------------------------------------------------------

#include "RayMarch.hlsli"

//--------------------------------------------------------------------------------------
// Get clip-space position
//--------------------------------------------------------------------------------------
#ifdef _HAS_DEPTH_MAP_
float3 GetClipPos(uint2 idx, float2 xy)
{
	const float z = g_txDepth[idx];

	return float3(xy, z);
}
#endif

//--------------------------------------------------------------------------------------
// Sample density field
//--------------------------------------------------------------------------------------
min16float4 GetSampleNU(uint volumeId, float3 uvw)
{
	const float4 color = g_txGrids[NonUniformResourceIndex(volumeId)].SampleLevel(g_smpLinear, uvw, 0.0);
	//const min16float4 color = min16float4(0.0, 0.5, 1.0, 0.5);

	return min16float4(color);
}

//--------------------------------------------------------------------------------------
// Get light
//--------------------------------------------------------------------------------------
float3 GetLightNU(uint volumeId, float3 pos)
{
	const float3 uvw = pos * 0.5 + 0.5;

	return g_txLightMaps[NonUniformResourceIndex(volumeId)].SampleLevel(g_smpLinear, uvw, 0.0);
}

//--------------------------------------------------------------------------------------
// Screen-space ray marching casting
//--------------------------------------------------------------------------------------
min16float4 RayCast(uint2 idx, float2 xy, float3 rayOrigin, float3 rayDir,
	uint volumeId, uint volTexId, uint sampleCount, matrix worldViewProjI)
{
	if (!ComputeRayOrigin(rayOrigin, rayDir)) return 0.0;

#ifdef _HAS_DEPTH_MAP_
	// Calculate occluded end point
	const float3 pos = GetClipPos(idx, xy);
	const float tMax = GetTMax(pos, rayOrigin, rayDir, worldViewProjI);
#endif

	const min16float stepScale = g_maxDist / min16float(sampleCount);

	// In-scattered radiance with inverted transmittance
	min16float4 scatter = 0.0;

	float t = 0.0;
	min16float step = stepScale;
	float prevDensity = 0.0;
	for (uint i = 0; i < sampleCount; ++i)
	{
		const float3 pos = rayOrigin + rayDir * t;
		if (any(abs(pos) > 1.0)) break;
		const float3 uvw = LocalToTex3DSpace(pos);

		// Get a sample
		min16float4 color = GetSampleNU(volTexId, uvw);
		min16float newStep = stepScale;
		float dDensity = 1.0;

		// Skip empty space
		if (color.w > ZERO_THRESHOLD)
		{
			const float3 light = GetLight(volumeId, pos); // Sample light

			// Update step
			const min16float transm = 1.0 - scatter.w;
			const float dDensity = color.w - prevDensity;
			newStep = GetStep(dDensity, transm, color.w, stepScale);
			step = (step + newStep) * 0.5;
			prevDensity = color.w;

			// Accumulate color
#ifndef _PRE_MULTIPLIED_
			color.xyz *= color.w;
#endif
			color.xyz *= min16float3(light);
			scatter += color * ABSORPTION * transm;

			if (transm < ZERO_THRESHOLD)
				break;
		}

		// Update position along ray
		step = newStep;
		t += step;
#ifdef _HAS_DEPTH_MAP_
		if (t > tMax) break;
#endif
	}

	scatter.xyz /= 2.0 * PI;

	//return scatter * min16float4(1.0, 0.1.xx, 1.0); // Red mark
	return scatter;
}
