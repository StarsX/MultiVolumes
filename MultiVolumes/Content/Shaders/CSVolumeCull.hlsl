//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Common.hlsli"

#define GROUP_VOLUME_COUNT 8

//--------------------------------------------------------------------------------------
// Structs
//--------------------------------------------------------------------------------------
typedef VolumeDesc VolumeIn;
typedef VisibleVolume VolumeOut;

//--------------------------------------------------------------------------------------
// Buffers
//--------------------------------------------------------------------------------------
StructuredBuffer<PerObject>	g_roPerObject;
StructuredBuffer<VolumeIn>	g_roVolumes;

AppendStructuredBuffer<VolumeOut> g_rwVisibleVolumes;
RWBuffer<uint> g_rwVolumeVis;

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

		float4(-1.0f, 1.0f, -1.0f, 1.0f),
		float4(1.0f, 1.0f, -1.0f, 1.0f),
		float4(-1.0f, -1.0f, -1.0f, 1.0f),
		float4(1.0f, -1.0f, -1.0f, 1.0f),
	};

	float4 p = mul(v[i], worldViewProj);
	p.xyz /= p.w;
	p = p * 0.5 + 0.5;
	p.y = 1.0 - p.y;

	return float4(p.xy * viewport, p.zw);
}

float EstimateCubeEdgePixelSize(float2 v, uint edgeId, uint baseLaneId)
{
	static const uint ei[][2] =
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

	const uint i = edgeId;
	const float2 v0 = WaveReadLaneAt(v, baseLaneId + ei[i][0]);
	const float2 v1 = WaveReadLaneAt(v, baseLaneId + ei[i][1]);

	return length(v1 - v0);
}

float EstimateMaxCubeEdgePixelSize(float2 v, uint2 gTid)
{
	// Per edge processing
	if (gTid.x < 6)
	{
		const uint baseEdgeId = 2 * gTid.x;
		const uint baseLaneId = 8 * gTid.y;
		float2 s;

		[unroll]
		for (uint i = 0; i < 2; ++i)
			s[i] = EstimateCubeEdgePixelSize(v, baseEdgeId + i, baseLaneId);

		const float ms = max(s.x, s.y);

		[unroll]
		for (i = 0; i < GROUP_VOLUME_COUNT; ++i)
		{
			[branch]
			if (i == gTid.y) return WaveActiveMax(ms);
		}
	}

	return 0.0;
}

uint EstimateCubeMapLOD(inout uint raySampleCount, uint numMips, float cubeMapSize,
	float2 v, uint2 gTid, float upscale = 2.0, float raySampleCountScale = 2.0)
{
	// Calulate the ideal cube-map resolution
	float s = EstimateMaxCubeEdgePixelSize(v, gTid) / upscale;

	if (gTid.x == 0)
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
// Check the visibility of the face
//--------------------------------------------------------------------------------------
bool IsFaceVisible(uint face, float3 localSpaceEyePt)
{
	const float viewComp = localSpaceEyePt[face >> 1];

	return (face & 0x1) ? viewComp > -1.0 : viewComp < 1.0;
}

uint GenVisibilityMask(float4x3 worldI, float3 eyePt, uint2 gTid)
{
	// Per face processing
	if (gTid.x < 6)
	{
		const float3 localSpaceEyePt = mul(float4(eyePt, 1.0), worldI);
		const bool isVisible = IsFaceVisible(gTid.x, localSpaceEyePt);
		const uint waveMask = WaveActiveBallot(isVisible).x;

		return (waveMask >> gTid.y) & 0xff;
	}

	return 0;
}

[numthreads(8, GROUP_VOLUME_COUNT, 1)]
void main(uint2 GTid : SV_GroupThreadID, uint Gid : SV_GroupID)
{
	uint2 structInfo;
	g_roVolumes.GetDimensions(structInfo.x, structInfo.y);

	const uint volumeId = Gid * GROUP_VOLUME_COUNT + GTid.y;
	if (volumeId >= structInfo.x) return;

	const PerObject perObject = g_roPerObject[volumeId];

	// Project vertex to viewport space
	const float4 v = ProjectToViewport(GTid.x, perObject.WorldViewProj, g_viewport);

	// If any vertices are inside viewport
	const bool isInView = all(v.xy <= g_viewport && v.xy >= 0.0);
	const uint waveMask = WaveActiveBallot(isInView).x;
	const uint volumeVis = (waveMask >> GTid.y) & 0xff;
	//if (GTid.x == 0) g_rwVolumeVis[volumeId] = volumeVis;

	// Viewport-visibility culling
	if (volumeVis == 0) return;

	VolumeIn volumeIn;
	uint raySampleCount;
	if (GTid.x == 0)
	{
		volumeIn = g_roVolumes[volumeId];
		raySampleCount = g_numSamples;
	}

	const uint mipLevel = EstimateCubeMapLOD(raySampleCount,
		volumeIn.NumMips, volumeIn.CubeMapSize, v.xy, GTid);
	const uint faceMask = GenVisibilityMask(perObject.WorldI, g_eyePt, GTid);

	if (GTid.x == 0)
	{
		VolumeOut volumeOut;
		volumeOut.VolumeId = volumeId;
		volumeOut.Mip_SCnt = (mipLevel << 16) | raySampleCount;
		volumeOut.FaceMask = faceMask;
		volumeOut.VolTexId = volumeIn.VolTexId;

		g_rwVisibleVolumes.Append(volumeOut);
	}
}
