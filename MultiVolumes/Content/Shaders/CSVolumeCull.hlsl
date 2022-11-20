//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Common.hlsli"
#include "VolumeCull.hlsli"

AppendStructuredBuffer<uint> g_rwCubeMapVolumes;

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
[numthreads(8, GROUP_VOLUME_COUNT, 1)]
void main(uint2 GTid : SV_GroupThreadID, uint Gid : SV_GroupID)
{
	uint2 structInfo;
	g_roVolumes.GetDimensions(structInfo.x, structInfo.y);

	const uint volumeId = Gid * GROUP_VOLUME_COUNT + GTid.y;
	if (volumeId >= structInfo.x) return;

	const PerObject perObject = g_roPerObject[volumeId];

	uint2 wTid;
	wTid.x = WaveGetLaneIndex() % 8;
	wTid.y = WaveGetLaneIndex() / 8;

	// Project vertex to viewport space
	const float4 v = ProjectToViewport(wTid.x, perObject.WorldViewProj, g_viewport);

	// If any vertices are inside viewport
	const bool isInView = all(v.xy <= g_viewport && v.xy >= 0.0) && v.z > 0.0 && v.z < 1.0;
	const uint waveMask = WaveActiveBallot(isInView).x;
	const uint volumeVis = (waveMask >> (8 * wTid.y)) & 0xff;
	//if (wTid.x == 0) g_rwVolumeVis[volumeId] = volumeVis;

	// Viewport-visibility culling
	if (volumeVis == 0) return;

	VolumeIn volumeIn = (VolumeIn)0;
	uint raySampleCount;
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
	const uint cubeMapSize = GetCubeMapSize(volumeIn);
	const uint mipLevel = EstimateCubeMapLOD(raySampleCount, GetNumMips(volumeIn), cubeMapSize, ep, wTid);

	// Volume projection coverage
	const float projCov = EstimateProjCoverage(ep, wTid, faceMask, baseLaneId);

	if (wTid.x == 0)
	{
		// Visible pixels of the cube map
		const float cubeMapPix = EstimateCubeMapVisiblePixels(faceMask, mipLevel, cubeMapSize);
		const bool useCubeMap = cubeMapPix <= projCov;
		const uint maskBits = useCubeMap ? (faceMask | CUBEMAP_RAYMARCH_BIT) : faceMask;

		const uint volTexId = GetSourceTextureId(volumeIn);
		g_rwVolumes[volumeId] = uint4(mipLevel, raySampleCount, maskBits, volTexId);
		g_rwVisibleVolumes.Append(volumeId);
#if _ADAPTIVE_RAYMARCH_
		if (useCubeMap) g_rwCubeMapVolumes.Append(volumeId);
#else
		g_rwCubeMapVolumes.Append(volumeId);
#endif
			
	}
}
