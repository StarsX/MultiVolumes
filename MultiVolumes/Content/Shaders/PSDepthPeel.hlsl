//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConsts.h"

//--------------------------------------------------------------------------------------
// Unordered access textures
//--------------------------------------------------------------------------------------
RWTexture2DArray<uint>	g_rwKDepths;

void main(float4 Pos : SV_POSITION)
{
	const uint2 uv = Pos.xy;
	uint depth = asuint(Pos.z);
	uint depthPrev;

	for (uint i = 0; i < NUM_OIT_LAYERS; ++i)
	{
		const uint3 uvw = { uv, i };
		InterlockedMin(g_rwKDepths[uvw], depth, depthPrev);
		depth = max(depth, depthPrev);
	}
}
