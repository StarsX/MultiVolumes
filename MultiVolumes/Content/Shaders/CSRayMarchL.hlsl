//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "RayMarch.hlsli"

//--------------------------------------------------------------------------------------
// Buffers and textures
//--------------------------------------------------------------------------------------
RWTexture3D<float3> g_rwLightMap;

StructuredBuffer<PerObject>		g_roPerObject	: register (t0);
StructuredBuffer<VolumeDesc>	g_roVolumes		: register (t1);

//--------------------------------------------------------------------------------------
// Compute Shader
//--------------------------------------------------------------------------------------
[numthreads(4, 4, 4)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	float3 gridSize;
	g_rwLightMap.GetDimensions(gridSize.x, gridSize.y, gridSize.z);

	uint2 structInfo;
	g_roVolumes.GetDimensions(structInfo.x, structInfo.y);

	float4 rayOrigin;
	rayOrigin.xyz = (DTid + 0.5) / gridSize * 2.0 - 1.0;
	rayOrigin.w = 1.0;

	rayOrigin.xyz = mul(rayOrigin, g_lightMapWorld);					// Light-map space to world space

	min16float shadow = 1.0;

	for (uint n = 0; n < structInfo.x; ++n)
	{
		const PerObject perObject = g_roPerObject[n];
		VolumeDesc volume = g_roVolumes[n];

		const float3 localRayOrigin = mul(rayOrigin, perObject.WorldI);	// World space to volume space

		// Transmittance
#ifdef _HAS_SHADOW_MAP_
		shadow *= ShadowTest(localRayOrigin, g_txDepth, perObject.ShadowWVP);
#endif

		float3 uvw = localRayOrigin * 0.5 + 0.5;
		/*const min16float density = GetSample(uvw).w;
		if (density < ZERO_THRESHOLD)
		{
			g_rwLightMap[DTid] = 0.0;
			return;
		}*/

#ifdef _POINT_LIGHT_
		const float3 localSpaceLightPt = mul(g_lightPos, perObject.WorldI);
		const float3 rayDir = normalize(localSpaceLightPt - localRayOrigin);
#else
		const float3 localSpaceLightPt = mul(g_lightPos.xyz, (float3x3)perObject.WorldI);
		const float3 rayDir = normalize(localSpaceLightPt);
#endif

		volume.VolTexId = WaveReadLaneFirst(volume.VolTexId);

		if (shadow > 0.0)
		{
			float t = g_stepScale;
			for (uint i = 0; i < g_numSamples; ++i)
			{
				const float3 pos = localRayOrigin + rayDir * t;
				if (any(abs(pos) > 1.0)) break;
				uvw = LocalToTex3DSpace(pos);

				// Get a sample along light ray
				const min16float density = GetSample(volume.VolTexId, uvw).w;

				// Attenuate ray-throughput along light direction
				shadow *= 1.0 - GetOpacity(density, g_stepScale);
				if (shadow < ZERO_THRESHOLD) break;

				// Update position along light ray
				t += g_stepScale;
			}
		}
	}

	const min16float3 lightColor = min16float3(g_lightColor.xyz * g_lightColor.w);
	const min16float3 ambient = min16float3(g_ambient.xyz * g_ambient.w);
	g_rwLightMap[DTid] = lightColor * shadow + ambient;
}
