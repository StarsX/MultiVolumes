//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConsts.h"

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
float3 ProjectToViewport(uint i, float4x4 worldViewProj, float2 viewport)
{
	float4 p;
	p.x = (i & 1) ? 1.0 : -1.0;
	p.y = ((i >> 1) & 1) ? 1.0 : -1.0;
	p.z = (i >> 2) ? 1.0 : -1.0;
	p.w = 1.0;

	p = mul(p, worldViewProj);
	p.xyz /= p.w;
	p.xy = p.xy * 0.5 + 0.5;
	p.y = 1.0 - p.y;

	return float3(p.xy * viewport, p.z);
}

//--------------------------------------------------------------------------------------
// Check the visibility of the face
//--------------------------------------------------------------------------------------
bool IsFaceVisible(uint face, float3 localSpaceEyePt)
{
	const float viewComp = localSpaceEyePt[face >> 1];

	return (face & 0x1) ? viewComp > -1.0 : viewComp < 1.0;
}

uint GenVisibilityMask(float4x3 worldI, float3 eyePt, uint wTidx, uint baseLaneId)
{
	// Per face processing
	bool isVisible = false;
	if (wTidx < 6)
	{
		const float3 localSpaceEyePt = mul(float4(eyePt, 1.0), worldI);
		isVisible = IsFaceVisible(wTidx, localSpaceEyePt);
	}

	const uint waveMask = WaveActiveBallot(isVisible).x;

	return (waveMask >> baseLaneId) & 0xff;
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
#ifdef _NO_ARRAY_INDEXING_
	uint4 m;
	switch (edgeId >> 1)
	{
	case 0:
		m = uint4(0, 1, 3, 2);
		break;
	case 1:
		m = uint4(1, 3, 2, 0);
		break;
	case 2:
		m = uint4(6, 7, 5, 4);
		break;
	case 3:
		m = uint4(4, 6, 7, 5);
		break;
	case 4:
		m = uint4(4, 0, 2, 6);
		break;
	case 5:
		m = uint4(7, 3, 1, 5);
		break;
	}

	const uint2 i = (edgeId & 1) ? m.zw : m.xy;
#else
	static const uint2 lanes[] =
	{
		uint2(0, 1),
		uint2(3, 2),
	
		uint2(1, 3),
		uint2(2, 0),
	
		uint2(6, 7),
		uint2(5, 4),
	
		uint2(4, 6),
		uint2(7, 5),
	
		uint2(4, 0),
		uint2(2, 6),
	
		uint2(7, 3),
		uint2(1, 5)
	};

#ifdef _USE_WATERFALLING_
	uint2 i;
	for (uint n = 0; n < 32; ++n)
	{
		const uint scalar = WaveReadLaneFirst(edgeId);
		if (scalar == edgeId)
		{
			i = lanes[scalar];
			break;
		}
	}
#else
	const uint2 i = lanes[edgeId];
#endif
#endif
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
#ifdef _NO_ARRAY_INDEXING_
	uint i;
	switch (faceId)
	{
	case 0:
		i = uint4( 8,  3,  9,  6)[edgeIdx];
		break;
	case 1:
		i = uint4(10,  2, 11,  7)[edgeIdx];
		break;
	case 2:
		i = uint4( 0,  8,  5, 11)[edgeIdx];
		break;
	case 3:
		i = uint4( 1, 10,  4,  9)[edgeIdx];
		break;
	case 4:
		i = uint4( 0,  2,  1,  3)[edgeIdx];
		break;
	case 5:
		i = uint4( 4,  6,  5,  7)[edgeIdx];
		break;
	}
#else
	static const uint4 lanes[] =
	{
		uint4( 8,  3,  9,  6),	// -X
		uint4(10,  2, 11,  7),	// +X

		uint4( 0,  8,  5, 11),	// -Y
		uint4( 1, 10,  4,  9),	// +Y

		uint4( 0,  2,  1,  3),	// -Z
		uint4( 4,  6,  5,  7)	// +Z
	};

#ifdef _USE_WATERFALLING_
	uint i;
	for (uint n = 0; n < 32; ++n)
	{
		const uint scalar = WaveReadLaneFirst(faceId);
		if (scalar == faceId)
		{
			i = lanes[scalar][edgeIdx];
			break;
		}
	}
#else
	const uint i = lanes[faceId][edgeIdx];
#endif
#endif
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
		uint i;
		float2 e[4];

		[unroll]
		for (i = 0; i < 4; ++i)
			e[i] = GetCubeFaceEdges(edgePair, wTid.x, i, baseLaneId);

		float faceArea = 0.0;
		if (faceMask & (1u << wTid.x)) faceArea = CalcQuadArea(e);

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
	const uint edgeLength = cubeMapSize >> mipLevel;
	const float visibleFaceCount = countbits(faceMask);
	const float faceArea = edgeLength * edgeLength;

	return faceArea * visibleFaceCount;
}
