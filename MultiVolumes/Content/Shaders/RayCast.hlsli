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
min16float4 GetSampleNU(uint i, float3 uvw)
{
	min16float4 color = min16float4(g_txGrids[NonUniformResourceIndex(i)].SampleLevel(g_smpLinear, uvw, 0.0));
	//min16float4 color = min16float4(0.0, 0.5, 1.0, 0.5);
	color.w *= DENSITY_SCALE;

	return color;
}

//--------------------------------------------------------------------------------------
// Screen-space ray marching casting
//--------------------------------------------------------------------------------------
min16float4 RayCast(uint2 idx, float2 xy, float3 rayOrigin, float3 rayDir,
	uint volumeId, uint sampleCount, matrix worldViewProjI, float4x3 localToLight)
{
	if (!ComputeRayOrigin(rayOrigin, rayDir)) return 0.0;

#ifdef _HAS_DEPTH_MAP_
	// Calculate occluded end point
	const float3 pos = GetClipPos(idx, xy);
	const float tMax = GetTMax(pos, rayOrigin, rayDir, worldViewProjI);
#endif

	const min16float stepScale = g_maxDist / min16float(sampleCount);

	// Transmittance
	min16float transm = 1.0;

	// In-scattered radiance
	min16float3 scatter = 0.0;

	float t = 0.0;
	min16float step = stepScale;
	for (uint i = 0; i < sampleCount; ++i)
	{
		const float3 pos = rayOrigin + rayDir * t;
		if (any(abs(pos) > 1.0)) break;
		const float3 uvw = LocalToTex3DSpace(pos);

		// Get a sample
		min16float4 color = GetSampleNU(volumeId, uvw);

		// Skip empty space
		if (color.w > ZERO_THRESHOLD)
		{
			// Sample light
			const float3 light = GetLight(pos, localToLight);

			// Accumulate color
			color.w = GetOpacity(color.w, step);
			color.xyz *= transm;
#ifdef _PRE_MULTIPLIED_
			color.xyz = GetPremultiplied(color.xyz, step);
#else
			color.xyz *= color.w;
#endif

			//scatter += color.xyz;
			scatter += min16float3(light) * color.xyz;

			// Attenuate ray-throughput
			transm *= 1.0 - color.w;
			if (transm < ZERO_THRESHOLD) break;
		}

		// Update position along ray
		step = GetStep(transm, color.w, stepScale);
		t += step;
#ifdef _HAS_DEPTH_MAP_
		if (t > tMax) break;
#endif
	}

	//return min16float4(scatter * min16float3(1.0, 0.1.xx), 1.0 - transm); // Red mark
	return min16float4(scatter, 1.0 - transm);
}
