//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define _LIGHT_PASS_
#define _NO_ARRAY_INDEXING_
#include "RayMarch.hlsli"
#include "VolumeCull.hlsli"

#define DIV_UP(x, n) (((x) + (n) - 1) / (n))

struct VolumeCullRecord
{
	uint VolumeCount;
};

struct RayMarchRecord
{
	uint2 DispatchGrid : SV_DispatchGrid;
	uint VolumeId;
	uint MipLevel;	// Mip level
	uint SmpCount;	// Ray sample count
	uint MaskBits;	// Highest bit in the uint16: render scheme, lowest 6 bits: cube-face visibility mask 
	uint VolTexId;	// Volume texture Id
};

struct VolumeOutRecord
{
	uint VolumeId;
	uint MipLevel;	// Mip level
	uint SmpCount;	// Ray sample count
	uint MaskBits;	// Highest bit in the uint16: render scheme, lowest 6 bits: cube-face visibility mask 
	uint VolTexId;	// Volume texture Id
};

//--------------------------------------------------------------------------------------
// Main compute shader for volume culling
//--------------------------------------------------------------------------------------
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[numthreads(8, GROUP_VOLUME_COUNT, 1)]
void VolumeCull(uint Gid : SV_GroupID,
	DispatchNodeInputRecord<VolumeCullRecord> input,
	[MaxRecords(GROUP_VOLUME_COUNT)] NodeOutput<RayMarchRecord> RayMarch)
{
	uint2 wTid;
	wTid.x = WaveGetLaneIndex() % 8;
	wTid.y = WaveGetLaneIndex() / 8;

	const uint volumeId = Gid * GROUP_VOLUME_COUNT + wTid.y;

	PerObject perObject = (PerObject)0;
	float3 v = 0.0;

	uint volumeVis;
	if (volumeId < input.Get().VolumeCount)
	{
		perObject = g_roPerObject[volumeId];

		// Project vertex to viewport space
		v = ProjectToViewport(wTid.x, perObject.WorldViewProj, g_viewport);

		// If any vertices are inside viewport
		const bool isInView = all(and(v.xy <= g_viewport, v.xy >= 0.0)) && (v.z > 0.0 && v.z < 1.0);
		const uint waveMask = WaveActiveBallot(isInView).x;
		volumeVis = (waveMask >> (8 * wTid.y)) & 0xff;
	}
	else volumeVis = 0;

	VolumeIn volumeIn;
	uint cubeMapSize, mipLevel, raySampleCount, maskBits, volTexId;
	bool useCubeMap = true;

	if (volumeVis)
	{
		if (wTid.x == 0)
		{
			volumeIn = g_roVolumes[volumeId];
			raySampleCount = g_numSamples;
		}

		// Visiblity mask
		const uint baseLaneId = 8 * wTid.y;
		const uint faceMask = GenVisibilityMask(perObject.WorldI, g_eyePt, wTid.x, baseLaneId);

		// Get per-lane edges
		const float4 ep = GetCubeEdgePairPerLane(v.xy, wTid.x, baseLaneId);

		// Cube map LOD
		cubeMapSize = GetCubeMapSize(volumeIn);
		mipLevel = EstimateCubeMapLOD(raySampleCount, GetNumMips(volumeIn), cubeMapSize, ep, wTid);

		// Volume projection coverage
		const float projCov = EstimateProjCoverage(ep, wTid, faceMask, baseLaneId);

		if (wTid.x == 0)
		{
			// Visible pixels of the cube map
#if _ADAPTIVE_RAYMARCH_
			const float cubeMapPix = EstimateCubeMapVisiblePixels(faceMask, mipLevel, cubeMapSize);
			useCubeMap = cubeMapPix <= projCov;
#endif
			maskBits = useCubeMap ? (faceMask | CUBEMAP_RAYMARCH_BIT) : faceMask;
			volTexId = GetSourceTextureId(volumeIn);
		}
	}

	const bool needOutput = volumeVis && wTid.x == 0;
	//useCubeMap = needOutput && useCubeMap;
	//const uint recordCount = WaveActiveCountBits(useCubeMap);
	const uint recordCount = WaveActiveCountBits(needOutput && useCubeMap);
	GroupNodeOutputRecords<RayMarchRecord> outRecs = RayMarch.GetGroupNodeOutputRecords(recordCount);

	if (needOutput)
	{
		if (useCubeMap)
		{
			const uint i = WavePrefixCountBits(true);
			const uint mipCubeMapSize = cubeMapSize >> mipLevel;
			outRecs[i].DispatchGrid = DIV_UP(mipCubeMapSize, 8);
			outRecs[i].VolumeId = volumeId;
			outRecs[i].MipLevel = mipLevel;
			outRecs[i].SmpCount = raySampleCount;
			outRecs[i].MaskBits = maskBits;
			outRecs[i].VolTexId = volTexId;
		}

		g_rwVolumes[volumeId] = uint4(mipLevel, raySampleCount, maskBits, volTexId);
		g_rwVisibleVolumes.Append(volumeId);
	}

	outRecs.OutputComplete();
}

//--------------------------------------------------------------------------------------
// Buffers and textures
//--------------------------------------------------------------------------------------
RWTexture2DArray<float4> g_rwCubeMaps[]		: register (u0, space1);
#ifdef _HAS_DEPTH_MAP_
RWTexture2DArray<float> g_rwCubeDepths[]	: register (u0, space2);
#endif

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
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeMaxDispatchGrid(32, 32, 1)]
[numthreads(8, 8, 6)]
void RayMarch(uint2 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID,
	DispatchNodeInputRecord<RayMarchRecord> input)
{
	const uint volumeId = input.Get().VolumeId;
	VolumeInfo volumeInfo;
	volumeInfo.MipLevel = input.Get().MipLevel;
	volumeInfo.SmpCount = input.Get().SmpCount;
	volumeInfo.VolTexId = input.Get().VolTexId;
	volumeInfo.MaskBits = input.Get().MaskBits;

	const uint faceId = WaveReadLaneAt(GTid.z, 0);
	if ((volumeInfo.MaskBits & (1u << faceId)) == 0) return;

	const PerObject perObject = g_roPerObject[volumeId];
	float3 rayOrigin = mul(float4(g_eyePt, 1.0), perObject.WorldI);

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

	const min16float stepScale = g_maxDist / min16float(volumeInfo.SmpCount);

	// In-scattered radiance with inverted transmittance
	min16float4 scatter = 0.0;

	float t = 0.0;
	min16float step = stepScale;
	float prevDensity = 0.0;
	for (uint i = 0; i < volumeInfo.SmpCount; ++i)
	{
		const float3 pos = rayOrigin + rayDir * t;
		if (any(abs(pos) > 1.0)) break;
		const float3 uvw = LocalToTex3DSpace(pos);

		// Get a sample
		min16float4 color = GetSample(volumeInfo.VolTexId, uvw);
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

			if (transm < ZERO_THRESHOLD) break;
		}

		// Update position along ray
		step = newStep;
		t += step;
#ifdef _HAS_DEPTH_MAP_
		if (t > tMax) break;
#endif
	}

	scatter.xyz /= 2.0 * PI;

	g_rwCubeMaps[uavIdx][index] = scatter;
}
