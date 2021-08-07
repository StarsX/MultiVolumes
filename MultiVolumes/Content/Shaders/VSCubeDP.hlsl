//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define main VSCube
#include "VSCube.hlsl"
#undef main

//--------------------------------------------------------------------------------------
// Vertex shader used for cube rendering
//--------------------------------------------------------------------------------------
float4 main(uint vid : SV_VertexID, uint iid : SV_InstanceID) : SV_POSITION
{
	return VSCube(vid, iid).Pos;
}
