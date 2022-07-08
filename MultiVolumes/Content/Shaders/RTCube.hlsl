#include "PSCube.hlsli"

struct GlobalCB
{
    float4 EyePos;
    matrix ScreenToWorld;
};

ConstantBuffer<GlobalCB> g_cb : register (b0);
RaytracingAccelerationStructure Scene : register(t0);
Buffer<uint4>	g_roVolumes	: register (t1);

RWTexture2D<float4> RenderTarget : register(u0);

typedef BuiltInTriangleIntersectionAttributes Attributes;
struct RayPayload
{
    float4 Color;
    float T;
};

// Retrieve hit world position.
float3 HitWorldPosition()
{
    return WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
}

// Retrieve attribute at a hit position interpolated from vertex attributes using the hit's barycentrics.
float3 HitAttribute(float3 vertexAttribute[3], Attributes attr)
{
    return vertexAttribute[0] +
        attr.barycentrics.x * (vertexAttribute[1] - vertexAttribute[0]) +
        attr.barycentrics.y * (vertexAttribute[2] - vertexAttribute[0]);
}

uint GetVertId(uint primId, uint i)
{
    return (primId % 2) ? i : (3 - i);
}

float2 GetVertUV(uint primId, uint i)
{
    uint vertId = GetVertId(primId, i);
    float2 uv = float2(vertId & 1, vertId >> 1);
    return float2(1.0 - uv.x, uv.y); // Exterior UV to Interior UV
}

float2 GetUV(uint primId, float2 baryc)
{
    float2 uv0 = GetVertUV(primId, 0);
    float2 uv1 = GetVertUV(primId, 1);
    float2 uv2 = GetVertUV(primId, 2);

    return uv0 * (1.0 - baryc.x - baryc.y) + uv1 * baryc.x + uv2 * baryc.y;
}

// Generate a ray in world space for a camera pixel corresponding to an index from the dispatched 2D grid.
inline void GenerateCameraRay(uint2 index, out float3 origin, out float3 direction)
{
    float2 xy = index + 0.5f; // center in the middle of the pixel.
    float2 screenPos = xy / DispatchRaysDimensions().xy * 2.0 - 1.0;

    // Invert Y for DirectX-style coordinates.
    screenPos.y = -screenPos.y;

    // Unproject the pixel coordinate into a ray.
    float4 world = mul(float4(screenPos, 0, 1), g_cb.ScreenToWorld);

    world.xyz /= world.w;
    origin = g_cb.EyePos.xyz;
    direction = normalize(world.xyz - origin);
}

[shader("raygeneration")]
void RaygenShader()
{
    float3 rayDir;
    float3 origin;

    // Generate a ray for a camera pixel corresponding to an index from the dispatched 2D grid.
    GenerateCameraRay(DispatchRaysIndex().xy, origin, rayDir);

    // Trace the ray.
    // Set the ray's extents.
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = rayDir;
    // Set TMin to a non-zero small value to avoid aliasing issues due to floating - point errors.
    // TMin should be kept small to prevent missing geometry at close contact areas.
    ray.TMin = 0.001;
    ray.TMax = 10000.0;
    RayPayload payload = { float4(0, 0, 0, 0), 0.0 };

    for (int i = 0; i < NUM_OIT_LAYERS; ++i)
    {
        TraceRay(Scene, RAY_FLAG_CULL_FRONT_FACING_TRIANGLES, ~0, 0, 1, 0, ray, payload);
        ray.TMin = payload.T;
    }

    // Write the raytraced color to the output texture.
    RenderTarget[DispatchRaysIndex().xy] = payload.Color;
}

[shader("closesthit")]
void ClosestHitShader(inout RayPayload payload, in Attributes attr)
{
    const float3 localRayDir = mul(WorldRayDirection(), (float3x3)WorldToObject4x3());
    const float3 localPos = mul(float4(HitWorldPosition(), 1.0), WorldToObject4x3());
    const uint primId = PrimitiveIndex();
    const uint faceId = primId / 2;
    const float3 uvw = float3(GetUV(primId, attr.barycentrics), faceId);

    const VolumeInfo volumeInfo = (VolumeInfo)g_roVolumes[InstanceID()];
    const uint uavIdx = NUM_CUBE_MIP * volumeInfo.VolTexId + volumeInfo.MipLevel;

    const float4 color = CubeCast(DispatchRaysIndex().xy, uvw, localPos, localRayDir, uavIdx);
    payload.Color += color * (1.0 - payload.Color.a);
    payload.T = RayTCurrent() + 0.001;
}

[shader("miss")]
void MissShader(inout RayPayload payload)
{
}

