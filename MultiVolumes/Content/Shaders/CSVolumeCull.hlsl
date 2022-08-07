//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConsts.h"
#include "Common.hlsli"

//--------------------------------------------------------------------------------------
// Structs
//--------------------------------------------------------------------------------------
typedef VolumeDesc VolumeIn;
typedef VolumeInfo VolumeOut;

//--------------------------------------------------------------------------------------
// Buffers
//--------------------------------------------------------------------------------------
StructuredBuffer<PerObject>	g_roPerObject;
StructuredBuffer<VolumeIn>	g_roVolumes;

AppendStructuredBuffer<uint> g_rwVisibleVolumes;
RWBuffer<uint4> g_rwVolumes;

static const uint g_waveVolumeCount = WaveGetLaneCount() / 8;

//--------------------------------------------------------------------------------------
// Project to viewport space
//--------------------------------------------------------------------------------------
float4 ProjectToViewport(uint i, float4x4 worldViewProj, float2 viewport)
{
	static const float4 v[] =
	{
		float4(1.0, 1.0, 1.0, 1.0),
		float4(-1.0, 1.0, 1.0, 1.0),
		float4(1.0, -1.0, 1.0, 1.0),
		float4(-1.0, -1.0, 1.0, 1.0),

		float4(-1.0, 1.0, -1.0, 1.0),
		float4(1.0, 1.0, -1.0, 1.0),
		float4(-1.0, -1.0, -1.0, 1.0),
		float4(1.0, -1.0, -1.0, 1.0),
	};

	float4 p = mul(v[i], worldViewProj);
	p.xyz /= p.w;
	p = p * 0.5 + 0.5;
	p.y = 1.0 - p.y;

	return float4(p.xy * viewport, p.zw);
}

//--------------------------------------------------------------------------------------
// Check the visibility of the face
//--------------------------------------------------------------------------------------
bool IsFaceVisible(uint face, float3 localSpaceEyePt)
{
	const float viewComp = localSpaceEyePt[face >> 1];

	return (face & 0x1) ? viewComp > -1.0 : viewComp < 1.0;
}

uint GenVisibilityMask(float4x3 worldI, float3 eyePt, uint2 wTid)
{
	// Per face processing
	bool isVisible = false;
	if (wTid.x < 6)
	{
		const float3 localSpaceEyePt = mul(float4(eyePt, 1.0), worldI);
		isVisible = IsFaceVisible(wTid.x, localSpaceEyePt);
	}

	const uint waveMask = WaveActiveBallot(isVisible).x;

	return (waveMask >> wTid.y) & 0xff;
}

//--------------------------------------------------------------------------------------
// Calculate triangle area
//--------------------------------------------------------------------------------------
float CalcTriangleArea(float2 edge1, float2 edge2)
{
	return 0.5 * abs(determinant(float2x2(edge1, edge2)));
}

//--------------------------------------------------------------------------------------
// Calculate quad area
//--------------------------------------------------------------------------------------
float CalcQuadArea(float2 edges[4])
{
	return CalcTriangleArea(edges[0], edges[1])
		+ CalcTriangleArea(edges[2], edges[3]);
}

//--------------------------------------------------------------------------------------
// Get cube edge
//--------------------------------------------------------------------------------------
float2 GetCubeEdge(float2 vertPos, uint edgeId, uint baseLaneId)
{
	// Lane-index map from unique edge Ids to vertex positions
	static const uint2 lanes[] =
	{
		{ 0, 1 },
		{ 3, 2 },

		{ 1, 3 },
		{ 2, 0 },

		{ 4, 5 },
		{ 7, 6 },

		{ 5, 7 },
		{ 6, 4 },

		{ 1, 4 },
		{ 6, 3 },

		{ 5, 0 },
		{ 2, 7 }
	};

	const uint2 i = lanes[edgeId];
	const float2 v0 = WaveReadLaneAt(vertPos, baseLaneId + i.x);
	const float2 v1 = WaveReadLaneAt(vertPos, baseLaneId + i.y);

	return v1 - v0;
}

//--------------------------------------------------------------------------------------
// Get cube edges per lane
//--------------------------------------------------------------------------------------
float4 GetCubeEdgePairPerLane(float2 vertPos, uint wTidx, uint baseLaneId)
{
	// Per edge processing
	if (wTidx < 6)
	{
		// Each lane stores 2 edges
		const uint baseEdgeId = 2 * wTidx;
		float2 e[2];

		[unroll]
		for (uint i = 0; i < 2; ++i)
			e[i] = GetCubeEdge(vertPos, baseEdgeId + i, baseLaneId);

		return float4(e[0], e[1]);
	}

	return 0.0;
}

//--------------------------------------------------------------------------------------
// Get cube-face edges
//--------------------------------------------------------------------------------------
float2 GetCubeFaceEdges(float4 edgePair, uint faceId, uint edgeIdx, uint baseLaneId)
{
	// Lane-index map from per-face edge indices to unique edge Ids
	static const uint lanes[][4] =
	{
		{ 10,  3, 11,  6 },	// +X
		{  8,  7,  9,  2 },	// -X

		{  0, 10,  4,  8 },	// +Y
		{  5, 11,  1,  9 },	// -Y

		{  0,  2,  1,  3 },	// +Z
		{  4,  6,  5,  7 }	// -Z
	};

	const uint i = lanes[faceId][edgeIdx];
	edgePair = WaveReadLaneAt(edgePair, baseLaneId + (i >> 1));

	return (i & 1) ? edgePair.zw : edgePair.xy;
}

//--------------------------------------------------------------------------------------
// Estimate max edge length and projected pixel converage of the cube in pixels
//--------------------------------------------------------------------------------------
float EstimateCubeMaxEdgeLength(float4 edgePair, uint2 wTid)
{
	// Per edge processing
	if (wTid.x < 6)
	{
		const float ms = max(length(edgePair.xy), length(edgePair.zw));
		for (uint i = 0; i < g_waveVolumeCount; ++i)
		{
			[branch]
			if (i == wTid.y) return WaveActiveMax(ms);
		}
	}

	return 0.0;
}

//--------------------------------------------------------------------------------------
// Estimate cube map LOD
//--------------------------------------------------------------------------------------
uint EstimateCubeMapLOD(inout uint raySampleCount, uint numMips, float cubeMapSize,
	float4 edgePair, uint2 wTid, float upscale = 2.0, float raySampleCountScale = 2.0)
{
	// Calulate the ideal cube-map resolution
	float s = EstimateCubeMaxEdgeLength(edgePair, wTid) / upscale;

	if (wTid.x == 0)
	{
		// Get the ideal ray sample amount
		float raySampleAmt = raySampleCountScale * s / sqrt(3.0);

		// Clamp the ideal ray sample amount using the user-specified upper bound of ray sample count
		const uint raySampleCnt = ceil(raySampleAmt);
		raySampleCount = min(raySampleCnt, raySampleCount);

		// Inversely derive the cube-map resolution from the clamped ray sample amount
		raySampleAmt = min(raySampleAmt, raySampleCount);
		s = raySampleAmt / raySampleCountScale * sqrt(3.0);

		// Use the more detailed integer level for conservation
		//const auto level = static_cast<uint8_t>(floorf((max)(log2f(cubeMapSize / s), 0.0f)));
		const uint level = max(log2(cubeMapSize / s), 0.0);

		return min(level, numMips - 1);
	}

	return 0;
}

//--------------------------------------------------------------------------------------
// Estimate volume projection coverage in pixels
//--------------------------------------------------------------------------------------
float EstimateProjCoverage(float4 edgePair, uint2 wTid, uint faceMask, uint baseLaneId)
{
	// Per face processing
	if (wTid.x < 6)
	{
		float2 e[4];

		[unroll]
		for (uint i = 0; i < 4; ++i)
			e[i] = GetCubeFaceEdges(edgePair, wTid.x, i, baseLaneId);

		float faceArea = 0.0;
		if (faceMask & (1 << wTid.x)) faceArea = CalcQuadArea(e);

		for (i = 0; i < g_waveVolumeCount; ++i)
		{
			[branch]
			if (i == wTid.y) return WaveActiveSum(faceArea);
		}
	}

	return 0.0;
}

//--------------------------------------------------------------------------------------
// Estimate visible pixels of the cube map
//--------------------------------------------------------------------------------------
float EstimateCubeMapVisiblePixels(uint faceMask, uint mipLevel, uint cubeMapSize)
{
	const float visibleFaceCount = countbits(faceMask);
	const float edgeLength = cubeMapSize >> mipLevel;
	const float faceArea = edgeLength * edgeLength;

	return faceArea * visibleFaceCount;
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
	const bool isInView = all(v.xy <= g_viewport && v.xy >= 0.0);
	const uint waveMask = WaveActiveBallot(isInView).x;
	const uint volumeVis = (waveMask >> wTid.y) & 0xff;
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
	const uint faceMask = GenVisibilityMask(perObject.WorldI, g_eyePt, wTid);

	// Get per-lane edges
	const uint baseLaneId = 8 * wTid.y;
	const float4 ep = GetCubeEdgePairPerLane(v.xy, wTid.x, baseLaneId);

	// Cube map LOD
	const uint mipLevel = EstimateCubeMapLOD(raySampleCount,
		GetNumMips(volumeIn), volumeIn.CubeMapSize, ep, wTid);

	// Volume projection coverage
	const float projCov = EstimateProjCoverage(ep, wTid, faceMask, baseLaneId);

	if (wTid.x == 0)
	{
		// Visible pixels of the cube map
		const float cubeMapPix = EstimateCubeMapVisiblePixels(faceMask, mipLevel, volumeIn.CubeMapSize);
		const uint maskBits = cubeMapPix <= projCov ? (faceMask | CUBEMAP_RAYMARCH_BIT) : faceMask;

		const uint volTexId = GetSourceTextureId(volumeIn);
		g_rwVolumes[volumeId] = uint4(mipLevel, raySampleCount, maskBits, volTexId);
		g_rwVisibleVolumes.Append(volumeId);
	}
}
