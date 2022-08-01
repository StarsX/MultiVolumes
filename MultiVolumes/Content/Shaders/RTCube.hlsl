//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define _HAS_DEPTH_MAP_

#include "PSCube.hlsli"

#define ONE_THRESHOLD 0.99
#define T_MAX 1000.0

typedef RaytracingAccelerationStructure RaytracingAS;
typedef BuiltInTriangleIntersectionAttributes Attributes;

//--------------------------------------------------------------------------------------
// Structure
//--------------------------------------------------------------------------------------
struct RayPayload
{
	float4 Color;
	float T;
};

//--------------------------------------------------------------------------------------
// Buffers and textures
//--------------------------------------------------------------------------------------
RaytracingAS g_scene : register (t0);
Buffer<uint4> g_roVolumes : register (t1);

RWTexture2D<float4> g_renderTarget;

// Retrieve hit object position.
float3 HitObjectPosition()
{
	return ObjectRayOrigin() + RayTCurrent() * ObjectRayDirection();
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

// Generate a ray in world space for a camera pixel corresponding to an index from the dispatched 2D grid.
inline void GenerateCameraRay(uint2 index, out float3 origin, out float3 direction)
{
	const float2 xy = index + 0.5; // center in the middle of the pixel.
	float2 screenPos = xy / DispatchRaysDimensions().xy * 2.0 - 1.0;

	// Invert Y for DirectX-style coordinates.
	screenPos.y = -screenPos.y;

	// Unproject the pixel coordinate into a ray.
	float4 world = mul(float4(screenPos, 0.0, 1.0), g_screenToWorld);

	world.xyz /= world.w;
	origin = g_eyePt;
	direction = normalize(world.xyz - origin);
}

[shader("raygeneration")]
void raygenMain()
{
	const uint2 index = DispatchRaysIndex().xy;
	const float4 bgColor = g_renderTarget[index];

	// Generate a ray for a camera pixel corresponding to an index from the dispatched 2D grid.
	RayDesc ray;
	GenerateCameraRay(index, ray.Origin, ray.Direction);

	// Trace the ray.
	// Set the ray's extents.
	// Set TMin to a non-zero small value to avoid aliasing issues due to floating - point errors.
	// TMin should be kept small to prevent missing geometry at close contact areas.
	ray.TMin = 0.001;
	ray.TMax = 1000.0;

	RayPayload payload = (RayPayload)0;
	for (uint i = 0; i < NUM_OIT_LAYERS; ++i)
	{
		TraceRay(g_scene, RAY_FLAG_CULL_FRONT_FACING_TRIANGLES, ~0, 0, 1, 0, ray, payload);
		ray.TMin = payload.Color.w < ONE_THRESHOLD ? payload.T : ray.TMax;
	}

	// Write the raytraced color to the output texture.
	g_renderTarget[index] = payload.Color + bgColor * (1.0 - payload.Color.w);
}

[shader("closesthit")]
void closestHitMain(inout RayPayload payload, in Attributes attr)
{
	const uint volumeId = InstanceIndex();
	const VolumeInfo volumeInfo = (VolumeInfo)g_roVolumes[volumeId];
	const uint srvIdx = NUM_CUBE_MIP * volumeId + volumeInfo.MipLevel;

	const float3 pos = HitObjectPosition();
	const float3 rayDir = ObjectRayDirection();

	const uint primId = PrimitiveIndex();
	const uint faceId = primId / 2;
	const float3 uvw = float3(GetUV(primId, attr.barycentrics), faceId);

	const min16float4 color = CubeCast(DispatchRaysIndex().xy, uvw, pos, rayDir, srvIdx);

	payload.Color += color * (1.0 - payload.Color.w);
	payload.T = RayTCurrent() + 0.001;
}

[shader("miss")]
void missMain(inout RayPayload payload)
{
	payload.T = T_MAX;
}
