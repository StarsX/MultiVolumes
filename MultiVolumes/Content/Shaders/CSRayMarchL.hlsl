//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "RayMarch.hlsli"

//--------------------------------------------------------------------------------------
// Buffers and textures
//--------------------------------------------------------------------------------------
RWTexture3D<float3> g_rwLightMaps[];

StructuredBuffer<PerObject>		g_roPerObject	: register (t0);
StructuredBuffer<VolumeDesc>	g_roVolumes		: register (t1);
StructuredBuffer<uint>		g_roVisibleVolumes	: register (t2);
StructuredBuffer<uint>	g_roVisibleVolumeCount	: register (t3);

//--------------------------------------------------------------------------------------
// Compute Shader
//--------------------------------------------------------------------------------------
[numthreads(4, 4, 4)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	float3 gridSize;
	g_rwLightMaps[0].GetDimensions(gridSize.x, gridSize.y, gridSize.z);

	uint2 structInfo;
	g_roVolumes.GetDimensions(structInfo.x, structInfo.y);

	const uint visibleVolumeCount = g_roVisibleVolumeCount[0];
	uint volumeId;
	if (visibleVolumeCount) volumeId = g_roVisibleVolumes[g_frameIdx % visibleVolumeCount];
	else volumeId = g_frameIdx % structInfo.x;
	volumeId = WaveReadLaneAt(volumeId, 0);

	float4 rayOrigin;
	rayOrigin.xyz = (DTid + 0.5) / gridSize * 2.0 - 1.0;
	rayOrigin.w = 1.0;

	// Identify if the current position is nonempty
	uint volTexId = GetSourceTextureId(g_roVolumes[volumeId]);
	const float3 uvw = LocalToTex3DSpace(rayOrigin.xyz);
	volTexId = WaveReadLaneAt(volTexId, 0);

	const PerObject perObject = g_roPerObject[volumeId];
	const min16float density = GetSample(volTexId, uvw).w;
	const bool hasDensity = density >= ZERO_THRESHOLD;

	rayOrigin.xyz = mul(rayOrigin, perObject.World);	// Light-map space to world space

#ifdef _HAS_SHADOW_MAP_
	min16float shadow = ShadowTest(rayOrigin.xyz, g_txDepth);
#else
	min16float shadow = 1.0;
#endif

#ifdef _HAS_LIGHT_PROBE_
	min16float ao = 1.0;
	float3 irradiance = 0.0;
#endif

	if (hasDensity)
	{
		float3 aoRayDir = 0.0;
#ifdef _HAS_LIGHT_PROBE_
		if (g_hasLightProbe)
		{
			float3 shCoeffs[SH_NUM_COEFF];
			LoadSH(shCoeffs, g_roSHCoeffs);
			aoRayDir = -GetDensityGradient(volTexId, uvw);
			aoRayDir = any(abs(aoRayDir) > 0.0) ? aoRayDir : rayOrigin.xyz; // Avoid 0-gradient caused by uniform density field
			aoRayDir = mul(aoRayDir, (float3x3)perObject.World);
			aoRayDir = normalize(aoRayDir);
			irradiance = GetIrradiance(shCoeffs, aoRayDir);
		}
#endif

		for (uint n = 0; n < structInfo.x; ++n)
		{
			uint volTexId = GetSourceTextureId(g_roVolumes[n]);
			volTexId = WaveReadLaneFirst(volTexId);

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
				CastLightRay(shadow, volTexId, localRayOrigin, rayDir, g_step, g_numSamples);
			}

#ifdef _HAS_LIGHT_PROBE_
			if (g_hasLightProbe)
			{
				const float3 rayDir = normalize(mul(aoRayDir, (float3x3)perObject.WorldI));
				if (!ComputeRayOrigin(localRayOrigin, rayDir)) continue;

				min16float transm = 1.0;
				CastLightRay(transm, volTexId, localRayOrigin, rayDir, g_step, g_numSamples);
				ao *= n == volumeId ? transm : pow(saturate(transm + 0.5), 0.25);
			}
#endif
		}
	}

	const min16float3 lightColor = min16float3(g_lightColor.xyz * g_lightColor.w);
	min16float3 ambient = min16float3(g_ambient.xyz * g_ambient.w);

#ifdef _HAS_LIGHT_PROBE_
	ambient = g_hasLightProbe ? ao * min16float3(irradiance) : ambient;
#endif

	g_rwLightMaps[volumeId][DTid] = shadow * lightColor + ambient;
}
