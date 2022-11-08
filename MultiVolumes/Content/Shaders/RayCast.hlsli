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
	min16float4 color = min16float4(g_txGrids[NonUniformResourceIndex(volumeId)].SampleLevel(g_smpLinear, uvw, 0.0));
	//min16float4 color = min16float4(0.0, 0.5, 1.0, 0.5);
	color.w *= DENSITY_SCALE;

	return color;
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

	// Transmittance
	min16float transm = 1.0;

	// In-scattered radiance
	min16float3 scatter = 0.0;

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
			// Sample light
			const float3 light = GetLightNU(volumeId, pos);

			// Update step
			dDensity = color.w - prevDensity;
			const min16float opacity = saturate(color.w * step);
			newStep = GetStep(dDensity, transm, opacity, stepScale);
			step = (step + newStep) * 0.5;
			prevDensity = color.w;

			// Accumulate color
			const min16float tansl = GetTranslucency(color.w, step);
			color.w = saturate(color.w * step);
#ifdef _PRE_MULTIPLIED_
			color.xyz = GetPremultiplied(color.xyz, step);
#else
			//color.xyz *= color.w;
			color.xyz *= color.w;
#endif
			color.xyz *= transm;

			//scatter += color.xyz;
			scatter += color.xyz * min16float3(light);

			// Attenuate ray-throughput
			transm *= 1.0 - tansl;
			if (transm < ZERO_THRESHOLD) break;
		}

		// Update position along ray
		step = newStep;
		t += step;
#ifdef _HAS_DEPTH_MAP_
		if (t > tMax) break;
#endif
	}

	scatter /= 2.0 * PI;

	//return min16float4(scatter * min16float3(1.0, 0.1.xx), 1.0 - transm); // Red mark
	return min16float4(scatter, 1.0 - transm);
}
