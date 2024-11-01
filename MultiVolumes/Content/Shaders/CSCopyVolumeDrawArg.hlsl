//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

RWByteAddressBuffer g_rwDst;
StructuredBuffer<uint> g_roSrc;

[numthreads(1, 1, 1)]
void main(uint DTid : SV_DispatchThreadID)
{
	g_rwDst.Store(sizeof(uint) * DTid, g_roSrc[DTid]);
}
