//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConsts.h"
#include "Common.hlsli"

//--------------------------------------------------------------------------------------
// Structure
//--------------------------------------------------------------------------------------
struct VSOut
{
	float4 Pos	: SV_POSITION;
	float3 UVW	: TEXCOORD;
	float3 LPt	: POSLOCAL;
	uint2 Ids	: INDICES;
};

//--------------------------------------------------------------------------------------
// Constants
//--------------------------------------------------------------------------------------
static const float3x3 planes[6] =
{
	// back plane
	float3x3(-1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f),
	// left plane
	float3x3(0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 0.0f),
	// front plane
	float3x3(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f),
	// right plane
	float3x3(0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f),
	// top plane
	float3x3(-1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f),
	// bottom plane
	float3x3(-1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f)
};

//--------------------------------------------------------------------------------------
// Buffers and textures
//--------------------------------------------------------------------------------------
StructuredBuffer<Matrices>		g_roMatrices		: register (t0);
StructuredBuffer<VisibleVolume>	g_roVisibleVolumes	: register (t1);

//--------------------------------------------------------------------------------------
// Vertex shader used for cube rendering
//--------------------------------------------------------------------------------------
VSOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
	VSOut output;

	const uint faceId = vid / 4;
	const uint vertId = vid % 4;

	const VisibleVolume volumeInfo = g_roVisibleVolumes[iid];
	const Matrices matrices = g_roMatrices[volumeInfo.VolumeId];

	const uint mipLevel = volumeInfo.Mip_SCnt >> 16;
	const uint uavIdx = NUM_CUBE_MIP * volumeInfo.VolumeId + mipLevel;

	const float2 uv = float2(vertId & 1, vertId >> 1);
	const float2 pos2D = uv * 2.0 - 1.0;
	float3 pos = float3(pos2D.x, -pos2D.y, 1.0);
	pos = mul(pos, planes[faceId]);

	output.Pos = mul(float4(pos, 1.0), matrices.WorldViewProj);
	output.UVW = float3(1.0 - uv.x, uv.y, faceId); // Exterior UV to interior UV
	output.LPt = pos;
	output.Ids.x = volumeInfo.VolumeId;
	output.Ids.y = uavIdx;

	return output;
}
