//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define _HAS_DEPTH_MAP_

#include "PSCube.hlsli"

#define ONE_THRESHOLD 0.99

typedef RaytracingAccelerationStructure RaytracingAS;
typedef BuiltInTriangleIntersectionAttributes TriAttributes;

//--------------------------------------------------------------------------------------
// Structure
//--------------------------------------------------------------------------------------

struct PSIn
{
	float4 Pos	: SV_POSITION;
	float3 UVW	: TEXCOORD;
	float3 LPt	: POSLOCAL;
	uint2 Ids	: INDICES;
};


//--------------------------------------------------------------------------------------
// Buffers and textures
//--------------------------------------------------------------------------------------
RaytracingAS g_scene : register (t1);
Buffer<uint4> g_roVolumes : register (t2);

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------

float3 HitObjectPosition(RayQuery<RAY_FLAG_CULL_FRONT_FACING_TRIANGLES> q)
{
	return q.CommittedObjectRayOrigin() + q.CommittedRayT() * q.CommittedObjectRayDirection();
}

uint GetVertId(uint primId, uint i)
{
	return primId % 2 ? 3 - i : i;
}

float2 GetVertUV(uint primId, uint i)
{
	const uint vertId = GetVertId(primId, i);
	const float2 uv = float2(vertId & 1, vertId >> 1);

	return float2(1.0 - uv.x, uv.y); // Exterior UV to Interior UV
}

float2 GetUV(uint primId, float2 baryc)
{
	const float2 uv0 = GetVertUV(primId, 0);
	const float2 uv1 = GetVertUV(primId, 1);
	const float2 uv2 = GetVertUV(primId, 2);

	return uv0 * (1.0 - baryc.x - baryc.y) + uv1 * baryc.x + uv2 * baryc.y;
}

float4 main(PSIn input) : SV_TARGET
{
	const uint volumeId = input.Ids.x;
	const uint uavIdx = input.Ids.y;

	const PerObject perObject = g_roPerObject[volumeId];
	const float3 localSpaceEyePt = mul(float4(g_eyePt, 1.0), perObject.WorldI);
	const float3 rayDir = input.LPt.xyz - localSpaceEyePt;
	min16float4 dstColor = CubeCast(input.Pos.xy, input.UVW.xyz, input.LPt.xyz, rayDir, uavIdx);

	RayQuery<RAY_FLAG_CULL_FRONT_FACING_TRIANGLES> q;
	// Set up a trace. No work is done yet.
	// Trace the ray.
	// Set the ray's extents.
	RayDesc ray;
	ray.Origin = mul(float4(input.LPt.xyz, 1.0), perObject.World);
	ray.Direction = normalize(ray.Origin - g_eyePt);
	// Set TMin to a non-zero small value to avoid aliasing issues due to floating - point errors.
	// TMin should be kept small to prevent missing geometry at close contact areas.
	ray.TMin = 0.001;
	ray.TMax = 1000.0;

	for (uint i = 1; i < NUM_OIT_LAYERS; ++i)
	{
		q.TraceRayInline(g_scene, RAY_FLAG_NONE, ~0, ray);
		q.Proceed();

		if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
		{
			const uint volumeId = q.CommittedInstanceIndex();
			const VolumeInfo volumeInfo = (VolumeInfo)g_roVolumes[volumeId];
			const uint uavIdx = NUM_CUBE_MIP * volumeId + volumeInfo.MipLevel;

			const float3 pos = HitObjectPosition(q);
			const float3 rayDir = q.CommittedObjectRayDirection();

			const uint primId = q.CommittedPrimitiveIndex();
			const uint faceId = primId / 2;
			const float3 uvw = float3(GetUV(primId, q.CommittedTriangleBarycentrics()), faceId);

			const min16float4 srcColor = CubeCast(input.Pos.xy, uvw, pos, rayDir, uavIdx);
			dstColor += srcColor * (1.0 - dstColor.w);
			ray.TMin = (dstColor.w < ONE_THRESHOLD) ? (q.CommittedRayT() + 0.001) : ray.TMax;
		}
		else
		{
			ray.TMin = ray.TMax;
		}
	}

	return dstColor;
}
