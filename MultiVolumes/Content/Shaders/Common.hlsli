//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define _HAS_DEPTH_MAP_
#define _HAS_SHADOW_MAP_
#define _HAS_LIGHT_PROBE_

#define	INF				asfloat(0x7f800000)
#define	FLT_MAX			3.402823466e+38
#define DENSITY_SCALE	6.0

#define CUBEMAP_RAYMARCH_BIT	(1 << 15)
#define _ADAPTIVE_RAYMARCH_		1

typedef uint VolumeDesc;

//--------------------------------------------------------------------------------------
// Struct
//--------------------------------------------------------------------------------------
struct VolumeInfo
{
	uint MipLevel;	// Mip level
	uint SmpCount;	// Ray sample count
	uint MaskBits;	// Highest bit in the uint16: render scheme, lowest 6 bits: cube-face visibility mask 
	uint VolTexId;	// Volume texture Id
};

struct PerObject
{
	float4x4 WorldViewProj;
	float4x4 WorldViewProjI;
	float4x3 WorldI;
	float4x3 World;
};

//--------------------------------------------------------------------------------------
// Constant buffers
//--------------------------------------------------------------------------------------
cbuffer cbPerFrame
{
	float3 g_eyePt;
	float2 g_viewport;
	float4x4 g_screenToWorld;
	float4x4 g_shadowViewProj;
	float4 g_lightPos;
	float4 g_lightColor;
	float4 g_ambient;
	uint g_frameIdx;
};

cbuffer cbSampleRes
{
	uint g_numSamples;
#ifdef _HAS_LIGHT_PROBE_
	uint g_hasLightProbe;
#endif
};

//--------------------------------------------------------------------------------------
// Texture sampler
//--------------------------------------------------------------------------------------
SamplerState g_smpLinear : register (s0);

//--------------------------------------------------------------------------------------
// Helper functions for decoding
//--------------------------------------------------------------------------------------
uint GetSourceTextureId(VolumeDesc volume)
{
	return volume & 0x3fff;
}

uint GetNumMips(VolumeDesc volume)
{
	return (volume >> 14) & 0xf;
}

uint GetCubeMapSize(VolumeDesc volume)
{
	return volume >> 18;
}
