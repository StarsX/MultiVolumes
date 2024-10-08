//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define _LIGHT_PASS_
#include "RayMarch.hlsli"
#include "VolumeCull.hlsli"

#define DIV_UP(x, n) (((x) - 1) / (n) + 1)

struct VolumeCullRecord
{
	uint BaseVolumeId;
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
// Estimate visible pixels of the cube map
//--------------------------------------------------------------------------------------
float EstimateCubeMapVisiblePixels(uint faceMask, uint mipLevel, uint cubeMapSize)
{
	return EstimateCubeMapVisiblePixels(faceMask, cubeMapSize >> mipLevel);
}

//--------------------------------------------------------------------------------------
// Main compute shader for volume culling
//--------------------------------------------------------------------------------------
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[numthreads(8, GROUP_VOLUME_COUNT, 1)]
void VolumeCull(uint Gid : SV_GroupID,
	DispatchNodeInputRecord<VolumeCullRecord> input,
	[MaxRecords(1)] NodeOutput<RayMarchRecord> RayMarch)
{
	uint2 structInfo;
	g_roVolumes.GetDimensions(structInfo.x, structInfo.y);

	uint2 wTid;
	wTid.x = WaveGetLaneIndex() % 8;
	wTid.y = WaveGetLaneIndex() / 8;

	const uint volumeId = Gid * GROUP_VOLUME_COUNT + wTid.y;

	PerObject perObject = (PerObject)0;
	float4 v = 0.0;

	uint volumeVis = 0;
	if (volumeId < structInfo.x)
	{
		perObject = g_roPerObject[volumeId];

		// Project vertex to viewport space
		v = ProjectToViewport(wTid.x, perObject.WorldViewProj, g_viewport);

		// If any vertices are inside viewport
		const bool isInView = all(and(v.xy <= g_viewport, v.xy >= 0.0)) && (v.z > 0.0 && v.z < 1.0);
		const uint waveMask = WaveActiveBallot(isInView).x;
		volumeVis = (waveMask >> (8 * wTid.y)) & 0xff;
		//if (wTid.x == 0) g_rwVolumeVis[volumeId] = volumeVis;
	}
	//else volumeVis = 0;

	VolumeIn volumeIn = (VolumeIn)0;
	uint raySampleCount = 0, faceMask = 0, cubeMapSize = 0, mipLevel = 0;
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
		faceMask = GenVisibilityMask(perObject.WorldI, g_eyePt, wTid.x, baseLaneId);

		// Get per-lane edges
		const float4 ep = GetCubeEdgePairPerLane(v.xy, wTid.x, baseLaneId);

		// Cube map LOD
		cubeMapSize = GetCubeMapSize(volumeIn);
		mipLevel = EstimateCubeMapLOD(raySampleCount, GetNumMips(volumeIn), cubeMapSize, ep, wTid);

		// Volume projection coverage
		const float projCov = EstimateProjCoverage(ep, wTid, faceMask, baseLaneId);

#if _ADAPTIVE_RAYMARCH_
		if (wTid.x == 0)
		{
			// Visible pixels of the cube map
			const float cubeMapPix = EstimateCubeMapVisiblePixels(faceMask, mipLevel, cubeMapSize);
			useCubeMap = cubeMapPix <= projCov;
		}
#endif
	}

	const bool needOutput = volumeVis != 0 && wTid.x == 0;
	ThreadNodeOutputRecords<RayMarchRecord> rayMarchOutRec = RayMarch.GetThreadNodeOutputRecords(needOutput && useCubeMap ? 1 : 0);

	if (needOutput)
	{
		const uint maskBits = useCubeMap ? (faceMask | CUBEMAP_RAYMARCH_BIT) : faceMask;
		const uint volTexId = GetSourceTextureId(volumeIn);

		if (useCubeMap)
		{
			const uint mipCubeMapSize = cubeMapSize >> mipLevel;
			rayMarchOutRec.Get().DispatchGrid = uint2(DIV_UP(mipCubeMapSize, 8), DIV_UP(mipCubeMapSize, 4));
			rayMarchOutRec.Get().VolumeId = volumeId;
			rayMarchOutRec.Get().MipLevel = mipLevel;
			rayMarchOutRec.Get().SmpCount = raySampleCount;
			rayMarchOutRec.Get().MaskBits = maskBits;
			rayMarchOutRec.Get().VolTexId = volTexId;
		}

		g_rwVolumes[volumeId] = uint4(mipLevel, raySampleCount, maskBits, volTexId);
		g_rwVisibleVolumes.Append(volumeId);
	}

	rayMarchOutRec.OutputComplete();
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
[NodeMaxDispatchGrid(128, 256, 1)]
[numthreads(8, 4, 6)]
void RayMarch(uint2 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID,
	DispatchNodeInputRecord<RayMarchRecord> input)
{
	uint volumeId = input.Get().VolumeId;
	VolumeInfo volumeInfo;
	volumeInfo.MipLevel = input.Get().MipLevel;
	volumeInfo.SmpCount = input.Get().SmpCount;
	volumeInfo.VolTexId = input.Get().VolTexId;
	volumeInfo.MaskBits = WaveReadLaneAt(input.Get().MaskBits, 0);

	if ((volumeInfo.MaskBits & (1u << GTid.z)) == 0) return;

	//volumeId = WaveReadLaneAt(volumeId, 0);
	const PerObject perObject = g_roPerObject[volumeId];
	float3 rayOrigin = mul(float4(g_eyePt, 1.0), perObject.WorldI);

	//volumeInfo.MipLevel = WaveReadLaneAt(volumeInfo.MipLevel, 0);
	volumeInfo.SmpCount = WaveReadLaneAt(volumeInfo.SmpCount, 0);
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

	volumeInfo.VolTexId = WaveReadLaneAt(volumeInfo.VolTexId, 0);
	const min16float stepScale = g_maxDist / min16float(volumeInfo.SmpCount);

	// Transmittance
	min16float transm = 1.0;

	// In-scattered radiance
	min16float3 scatter = 0.0;

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
			// Sample light
			const float3 light = GetLight(volumeId, pos);

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

	scatter.xyz /= 2.0 * PI;

	g_rwCubeMaps[uavIdx][index] = float4(scatter, 1.0 - transm);
}
