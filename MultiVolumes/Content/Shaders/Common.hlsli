//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define _HAS_DEPTH_MAP_
#define _HAS_SHADOW_MAP_

#define	INF				asfloat(0x7f800000)
#define	FLT_MAX			3.402823466e+38
#define DENSITY_SCALE	1.0

//--------------------------------------------------------------------------------------
// Struct
//--------------------------------------------------------------------------------------
struct VolumeDesc
{
	uint VolTexId;
	uint NumMips;
	float CubeMapSize;
	float Padding;
};

struct VisibleVolume
{
	uint VolumeId;
	uint Mip_SCnt;	// Mip level and ray sample count
	uint FaceMask;	// Cube-face visibility mask 
	uint VolTexId;	// Volume texture Id
};

struct Matrices
{
	float4x4 WorldViewProj;
	float4x4 WorldViewProjI;
	float4x3 WorldI;
	float4x4 ShadowWVP;
};

//--------------------------------------------------------------------------------------
// Constant buffers
//--------------------------------------------------------------------------------------
cbuffer cbPerFrame
{
	float3 g_eyePt;
	float2 g_viewport;
	float4x3 g_lightMapWorld;
	float4 g_lightPos;
	float4 g_lightColor;
	float4 g_ambient;
};

cbuffer cbSampleRes
{
	uint g_numSamples;
};

//--------------------------------------------------------------------------------------
// Texture sampler
//--------------------------------------------------------------------------------------
SamplerState g_smpLinear;
