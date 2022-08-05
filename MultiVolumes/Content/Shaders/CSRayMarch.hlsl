//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConsts.h"
#include "RayMarch.hlsli"

//--------------------------------------------------------------------------------------
// Buffers and textures
//--------------------------------------------------------------------------------------
RWTexture2DArray<float4> g_rwCubeMaps[]		: register (u0, space0);
#ifdef _HAS_DEPTH_MAP_
RWTexture2DArray<float> g_rwCubeDepths[]	: register (u0, space1);
#endif

StructuredBuffer<PerObject>	g_roPerObject		: register (t0);
StructuredBuffer<uint>		g_roVisibleVolumes	: register (t1);
Buffer<uint4>				g_roVolumes			: register (t2);

//--------------------------------------------------------------------------------------
// Texture sampler
//--------------------------------------------------------------------------------------
SamplerState g_smpPoint;

//--------------------------------------------------------------------------------------
// Get the local-space position of the grid surface
//--------------------------------------------------------------------------------------
float3 GetLocalPos(float2 pos, uint slice, RWTexture2DArray<float4> rwCubeMap)
{
	float3 gridSize;
	rwCubeMap.GetDimensions(gridSize.x, gridSize.y, gridSize.z);
	
	pos = (pos + 0.5) / gridSize.xy * 2.0 - 1.0;
	pos.y = -pos.y;

	switch (slice)
	{
	case 0: // +X
		return float3(1.0, pos.y, -pos.x);
	case 1: // -X
		return float3(-1.0, pos.y, pos.x);
	case 2: // +Y
		return float3(pos.x, 1.0, -pos.y);
	case 3: // -Y
		return float3(pos.x, -1.0, pos.y);
	case 4: // +Z
		return float3(pos.x, pos.y, 1.0);
	case 5: // -Z
		return float3(-pos.x, pos.y, -1.0);
	default:
		return 0.0;
	}
}

//--------------------------------------------------------------------------------------
// Get clip-space position
//--------------------------------------------------------------------------------------
#ifdef _HAS_DEPTH_MAP_
float3 GetClipPos(float3 rayOrigin, float3 rayDir, matrix worldViewProj)
{
	float4 hPos = float4(rayOrigin + 0.01 * rayDir, 1.0);
	hPos = mul(hPos, worldViewProj);

	const float2 xy = hPos.xy / hPos.w;
	float2 uv = xy * 0.5 + 0.5;
	uv.y = 1.0 - uv.y;

	const float z = g_txDepth.SampleLevel(g_smpPoint, uv, 0.0);

	return float3(xy, z);
}
#endif

//--------------------------------------------------------------------------------------
// Compute Shader
//--------------------------------------------------------------------------------------
[numthreads(8, 4, 6)]
void main(uint2 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
	uint volumeId = g_roVisibleVolumes[Gid.z];
	VolumeInfo volumeInfo = (VolumeInfo)g_roVolumes[volumeId];
	volumeInfo.MaskBits = WaveReadLaneFirst(volumeInfo.MaskBits);

#if _ADAPTIVE_RAYMARCH_
	if (!(volumeInfo.MaskBits & CUBEMAP_RAYMARCH_BIT)) return;
#endif
	if ((volumeInfo.MaskBits & (1 << GTid.z)) == 0) return;

	//volumeId = WaveReadLaneFirst(volumeId);
	const PerObject perObject = g_roPerObject[volumeId];
	float3 rayOrigin = mul(float4(g_eyePt, 1.0), perObject.WorldI);

	//volumeInfo.MipLevel = WaveReadLaneFirst(volumeInfo.MipLevel);
	volumeInfo.SmpCount = WaveReadLaneFirst(volumeInfo.SmpCount);
	const uint uavIdx = NUM_CUBE_MIP * volumeId + volumeInfo.MipLevel;
	const float3 target = GetLocalPos(DTid, GTid.z, g_rwCubeMaps[uavIdx]);
	const float3 rayDir = normalize(target - rayOrigin);
	const bool isHit = ComputeRayOrigin(rayOrigin, rayDir);
	if (!isHit) return;

	const uint3 index = uint3(DTid, GTid.z);
#ifdef _HAS_DEPTH_MAP_
	// Calculate occluded end point
	const float3 pos = GetClipPos(rayOrigin, rayDir, perObject.WorldViewProj);
	g_rwCubeDepths[uavIdx][index] = pos.z;
	const float tMax = GetTMax(pos, rayOrigin, rayDir, perObject.WorldViewProjI);
#endif

	volumeInfo.VolTexId = WaveReadLaneFirst(volumeInfo.VolTexId);
	const min16float stepScale = g_maxDist / min16float(volumeInfo.SmpCount);

	// Transmittance
	min16float transm = 1.0;

	// In-scattered radiance
	min16float3 scatter = 0.0;

	float t = 0.0;
	min16float step = stepScale;
	for (uint i = 0; i < volumeInfo.SmpCount; ++i)
	{
		const float3 pos = rayOrigin + rayDir * t;
		if (any(abs(pos) > 1.0)) break;
		const float3 uvw = LocalToTex3DSpace(pos);

		// Get a sample
		min16float4 color = GetSample(volumeInfo.VolTexId, uvw);

		// Skip empty space
		if (color.w > ZERO_THRESHOLD)
		{
			// Sample light
			const float3 light = GetLight(pos, perObject.ToLightSpace);

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

	g_rwCubeMaps[uavIdx][index] = float4(scatter, 1.0 - transm);
}
