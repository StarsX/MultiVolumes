//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define THRESHOLD	0.12

typedef RaytracingAccelerationStructure RaytracingAS;
typedef BuiltInTriangleIntersectionAttributes TriAttributes;

//--------------------------------------------------------------------------------------
// Structs
//--------------------------------------------------------------------------------------
struct Vertex
{
	float3	Pos;
	float3	Nrm;
};

struct RayPayload
{
	float3	Normal;
	bool	IsInside;
};

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
RWTexture3D<float4>			RenderTarget	: register(u0);
RaytracingAS				g_scene : register(t0);

// IA buffers
Buffer<uint>				g_indexBuffers[]	: register(t0, space1);
StructuredBuffer<Vertex>	g_vertexBuffers[]	: register(t0, space2);

//--------------------------------------------------------------------------------------
// Samplers
//--------------------------------------------------------------------------------------
SamplerState g_sampler;

//--------------------------------------------------------------------------------------
// Generate a ray in grid space for a voxel corresponding to an index
// from the dispatched grid.
//--------------------------------------------------------------------------------------
void generateRay(uint3 index, out float3 origin, out float3 direction)
{
	float3 pos = (index + 0.5) / DispatchRaysDimensions().x * 2.0 - 1.0;

	// Invert Y for Y-up-style NDC.
	pos.y = -pos.y;

	origin = pos;
	direction = normalize(pos);
}

//--------------------------------------------------------------------------------------
// Ray generation
//--------------------------------------------------------------------------------------
[shader("raygeneration")]
void raygenMain()
{
	// Trace the ray.
	RayDesc ray;

	uint3 index = DispatchRaysIndex();
	// It seems Fallback layer has no depth
	index.z = index.y / DispatchRaysDimensions().x;
	index.y %= DispatchRaysDimensions().x;

	// Generate a ray for a voxel corresponding to an index from the dispatched grid.
	generateRay(index, ray.Origin, ray.Direction);

	RayPayload payload;

	// Set TMin to a zero value to avoid aliasing artifacts along contact areas.
	// Note: make sure to enable face culling so as to avoid surface face fighting.
	ray.TMin = 0.0;
	ray.TMax = 10000.0;
	payload.Normal = 0.0.xxx;
	payload.IsInside = false;
	TraceRay(g_scene, RAY_FLAG_NONE, ~0, 0, 1, 0, ray, payload);

	// Write the raytraced color to the output texture.
	if (payload.IsInside)
		RenderTarget[index] = float4(payload.Normal, 1.0);
}

//--------------------------------------------------------------------------------------
// Get IA-style inputs
//--------------------------------------------------------------------------------------
Vertex getInput(float2 barycentrics)
{
	const uint meshIdx = InstanceIndex();
	const uint baseIdx = PrimitiveIndex() * 3;
	const uint3 indices =
	{
		g_indexBuffers[NonUniformResourceIndex(meshIdx)][baseIdx],
		g_indexBuffers[NonUniformResourceIndex(meshIdx)][baseIdx + 1],
		g_indexBuffers[NonUniformResourceIndex(meshIdx)][baseIdx + 2]
	};

	// Retrieve corresponding vertex normals for the triangle vertices.
	Vertex vertices[3] =
	{
		g_vertexBuffers[NonUniformResourceIndex(meshIdx)][indices[0]],
		g_vertexBuffers[NonUniformResourceIndex(meshIdx)][indices[1]],
		g_vertexBuffers[NonUniformResourceIndex(meshIdx)][indices[2]]
	};

	Vertex input;
	input.Pos = vertices[0].Pos +
		barycentrics.x * (vertices[1].Pos - vertices[0].Pos) +
		barycentrics.y * (vertices[2].Pos - vertices[0].Pos);

	input.Nrm = vertices[0].Nrm +
		barycentrics.x * (vertices[1].Nrm - vertices[0].Nrm) +
		barycentrics.y * (vertices[2].Nrm - vertices[0].Nrm);

	return input;
}

//--------------------------------------------------------------------------------------
// Retrieve hit world position.
//--------------------------------------------------------------------------------------
float3 hitWorldPosition()
{
	return WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
}

//--------------------------------------------------------------------------------------
// Ray closest hit
//--------------------------------------------------------------------------------------
[shader("closesthit")]
void closestHitMain(inout RayPayload payload, TriAttributes attr)
{
	Vertex input = getInput(attr.barycentrics);

	payload.Normal = normalize(input.Nrm);
	payload.IsInside = dot(payload.Normal, WorldRayDirection()) > THRESHOLD;
	//payload.IsInside = HitKind() == HIT_KIND_TRIANGLE_BACK_FACE;
}

//--------------------------------------------------------------------------------------
// Ray miss
//--------------------------------------------------------------------------------------
[shader("miss")]
void missMain(inout RayPayload payload)
{
}
