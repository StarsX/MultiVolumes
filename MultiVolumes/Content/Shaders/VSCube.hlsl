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
	uint VolId	: VOLUMEID;
	uint SrvId	: SRVINDEX;
	uint TexId	: VOLTEXID;
	uint SmpCnt : SAMPLECOUNT;
};

//--------------------------------------------------------------------------------------
// Constants
//--------------------------------------------------------------------------------------
static const float3x3 planes[6] =
{
	// back plane
	float3x3(-1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0),
	// left plane
	float3x3(0.0, 0.0, -1.0, 0.0, 1.0, 0.0, -1.0, 0.0, 0.0),
	// front plane
	float3x3(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, -1.0),
	// right plane
	float3x3(0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0),
	// top plane
	float3x3(-1.0, 0.0, 0.0, 0.0, 0.0, -1.0, 0.0, 1.0, 0.0),
	// bottom plane
	float3x3(-1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, -1.0, 0.0)
};

//--------------------------------------------------------------------------------------
// Buffers and textures
//--------------------------------------------------------------------------------------
StructuredBuffer<PerObject>	g_roPerObject		: register (t0);
StructuredBuffer<uint>		g_roVisibleVolumes	: register (t1);
Buffer<uint4>				g_roVolumes			: register (t2);

//--------------------------------------------------------------------------------------
// Vertex shader used for cube rendering
//--------------------------------------------------------------------------------------
VSOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
	VSOut output;

	const uint faceId = vid / 4;
	const uint vertId = vid % 4;

	const uint volumeId = g_roVisibleVolumes[iid];
	const VolumeInfo volumeInfo = (VolumeInfo)g_roVolumes[volumeId];
	const PerObject perObject = g_roPerObject[volumeId];

	const float2 uv = float2(vertId & 1, vertId >> 1);
	const float2 pos2D = uv * 2.0 - 1.0;
	float3 pos = float3(pos2D.x, -pos2D.y, 1.0);
	pos = mul(pos, planes[faceId]);

	output.Pos = mul(float4(pos, 1.0), perObject.WorldViewProj);
	output.UVW = float3(1.0 - uv.x, uv.y, faceId); // Exterior UV to interior UV
	output.LPt = pos;
	output.VolId = volumeId;
	output.SrvId = NUM_CUBE_MIP * volumeId + volumeInfo.MipLevel;
	output.TexId = volumeInfo.VolTexId;
	output.SmpCnt = (volumeInfo.MaskBits & CUBEMAP_RAYMARCH_BIT) ? 0 : volumeInfo.SmpCount;

	return output;
}
