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

	rayOrigin.xyz = mul(rayOrigin, g_lightMapWorld);	// Light-map space to world space

#ifdef _HAS_SHADOW_MAP_
	min16float shadow = ShadowTest(rayOrigin.xyz, g_txDepth);
#else
	min16float shadow = 1.0;
#endif

#ifdef _HAS_LIGHT_PROBE_
	min16float ao = 1.0;
	float3 irradiance = 0.0;
#endif

	// Find the volume of which the current position is nonempty
	bool hasDensity = false;
	float3 uvw = 0.0;
	PerObject perObject;
	uint volTexId;
	for (uint n = 0; n < structInfo.x; ++n)
	{
		perObject = g_roPerObject[n];
		const float3 localRayOrigin = mul(rayOrigin, perObject.WorldI);	// World space to volume space

		if (all(abs(localRayOrigin) <= 1.0))
		{
			volTexId = GetSourceTextureId(g_roVolumes[n]);
			uvw = LocalToTex3DSpace(localRayOrigin);
			//volTexId = WaveReadLaneFirst(volTexId);

			const min16float density = GetSample(volTexId, uvw).w;
			hasDensity = density >= ZERO_THRESHOLD;

			if (hasDensity) break;
		}
	}

	if (hasDensity)
	{
		float3 aoRayDir = 0.0;
#ifdef _HAS_LIGHT_PROBE_
		if (g_hasLightProbes)
		{
			float3 shCoeffs[SH_NUM_COEFF];
			LoadSH(shCoeffs, g_roSHCoeffs);
			aoRayDir = -GetDensityGradient(volTexId, uvw);
			aoRayDir = any(abs(aoRayDir) > 0.0) ? aoRayDir : rayOrigin.xyz; // Avoid 0-gradient caused by uniform density field
			irradiance = GetIrradiance(shCoeffs, mul(aoRayDir, (float3x3)perObject.World));
			aoRayDir = normalize(aoRayDir);
		}
#endif

		for (n = 0; n < structInfo.x; ++n)
		{
			const PerObject perObject = g_roPerObject[n];
			float3 localRayOrigin = mul(rayOrigin, perObject.WorldI);	// World space to volume space

			if (shadow >= ZERO_THRESHOLD)
			{
#ifdef _POINT_LIGHT_
				const float3 localSpaceLightPt = mul(g_lightPos, perObject.WorldI);
				const float3 rayDir = normalize(localSpaceLightPt - localRayOrigin);
#else
				const float3 localSpaceLightPt = mul(g_lightPos.xyz, (float3x3)perObject.WorldI);
				const float3 rayDir = normalize(localSpaceLightPt);
#endif
				// Transmittance
				if (!ComputeRayOrigin(localRayOrigin, rayDir)) continue;
				uint volTexId = GetSourceTextureId(g_roVolumes[n]);
				//volTexId = WaveReadLaneFirst(volTexId);

				float t = g_stepScale;
				min16float step = g_stepScale;
				for (uint i = 0; i < g_numSamples; ++i)
				{
					const float3 pos = localRayOrigin + rayDir * t;
					if (any(abs(pos) > 1.0)) break;
					const float3 uvw = LocalToTex3DSpace(pos);

					// Get a sample along light ray
					const min16float density = GetSample(volTexId, uvw).w;

					// Attenuate ray-throughput along light direction
					const min16float opacity = GetOpacity(density, step);
					shadow *= 1.0 - opacity;
					if (shadow < ZERO_THRESHOLD) break;

					// Update position along light ray
					step = GetStep(shadow, opacity, g_stepScale);
					t += step;
				}
			}

#ifdef _HAS_LIGHT_PROBE_
			if (g_hasLightProbes)
			{
				float t = g_stepScale;
				min16float step = g_stepScale;
				for (uint i = 0; i < g_numSamples; ++i)
				{
					const float3 pos = localRayOrigin.xyz + aoRayDir * t;
					if (any(abs(pos) > 1.0)) break;
					const float3 uvw = LocalToTex3DSpace(pos);

					// Get a sample along light ray
					const min16float density = GetSample(volTexId, uvw).w;

					// Attenuate ray-throughput along light direction
					const min16float opacity = GetOpacity(density, step);
					ao *= 1.0 - opacity;
					if (ao < ZERO_THRESHOLD) break;

					// Update position along light ray
					step = GetStep(ao, opacity, g_stepScale);
					t += step;
				}
			}
#endif
		}
	}

	const min16float3 lightColor = min16float3(g_lightColor.xyz * g_lightColor.w);
	min16float3 ambient = min16float3(g_ambient.xyz * g_ambient.w);

#ifdef _HAS_LIGHT_PROBE_
	ambient = g_hasLightProbes ? min16float3(irradiance) * ao : ambient;
#endif

	g_rwLightMap[DTid] = lightColor * shadow + ambient;
}
