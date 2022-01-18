//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConsts.h"

//--------------------------------------------------------------------------------------
// Texture
//--------------------------------------------------------------------------------------
Texture2DArray	g_txKColors;

min16float4 main(float4 Pos : SV_POSITION) : SV_TARGET
{
	const uint2 uv = Pos.xy;
	min16float4 result = 0.0;

	for (uint i = 0; i < NUM_OIT_LAYERS; ++i)
	{
		const float4 src = g_txKColors[uint3(uv, i)];
		result += min16float4(src) * (1.0 - result.w);
	}

	return result;
}
