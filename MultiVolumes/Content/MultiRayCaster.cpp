//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConsts.h"
#include "MultiRayCaster.h"
#define _INDEPENDENT_DDS_LOADER_
#include "Advanced/XUSGDDSLoader.h"
#undef _INDEPENDENT_DDS_LOADER_

#define NUM_SAMPLES			256
#define NUM_LIGHT_SAMPLES	256

using namespace std;
using namespace DirectX;
using namespace XUSG;
using namespace XUSG::RayTracing;

const wchar_t* MultiRayCaster::HitGroupName = L"hitGroup";
const wchar_t* MultiRayCaster::RaygenShaderName = L"raygenMain";
const wchar_t* MultiRayCaster::ClosestHitShaderName = L"closestHitMain";
const wchar_t* MultiRayCaster::MissShaderName = L"missMain";

struct CBPerFrame
{
	XMFLOAT4 EyePos;
	XMFLOAT4 Viewport;
	XMFLOAT3X4 LightMapWorld;
	XMFLOAT4 LightPos;
	XMFLOAT4 LightColor;
	XMFLOAT4 Ambient;
};

struct PerObject
{
	XMFLOAT4X4 WorldViewProj;
	XMFLOAT4X4 WorldViewProjI;
	XMFLOAT4X4 ShadowWVP;
	XMFLOAT3X4 WorldI;
	XMFLOAT3X4 LocalToLight;
};

struct VolumeDesc
{
	uint32_t VolTexId;
	uint32_t NumMips;
	float CubeMapSize;
	float Padding;
};

struct VisibleVolume
{
	uint32_t VolumeId;
	uint32_t Mip_SCnt;
	uint32_t FaceMask;
	uint32_t VolTexId;
};

const uint8_t g_numCubeMips = NUM_CUBE_MIP;

MultiRayCaster::MultiRayCaster(const RayTracing::Device::sptr& device) :
	m_device(device),
	m_instances(),
	m_lightPt(75.0f, 75.0f, -75.0f),
	m_lightColor(1.0f, 0.7f, 0.3f, 1.0f),
	m_ambient(0.0f, 0.3f, 1.0f, 0.4f)
{
	m_shaderPool = ShaderPool::MakeUnique();
	m_rayTracingPipelineCache = RayTracing::PipelineCache::MakeUnique(device.get());
	m_graphicsPipelineCache = Graphics::PipelineCache::MakeUnique(device.get());
	m_computePipelineCache = Compute::PipelineCache::MakeUnique(device.get());
	m_pipelineLayoutCache = PipelineLayoutCache::MakeUnique(device.get());

	XMStoreFloat3x4(&m_volumeWorld, XMMatrixScaling(10.0f, 10.0f, 10.0f));
	m_lightMapWorld = m_volumeWorld;

	AccelerationStructure::SetUAVCount(2);
}

MultiRayCaster::~MultiRayCaster()
{
}

bool MultiRayCaster::Init(RayTracing::CommandList* pCommandList, const DescriptorTableCache::sptr& descriptorTableCache,
	Format rtFormat, uint32_t gridSize, uint32_t lightGridSize, uint32_t numVolumes, uint32_t numVolumeSrcs,
	const DepthStencil::uptr* depths, vector<Resource::uptr>& uploaders, RayTracing::GeometryBuffer* pGeometry)
{
	m_descriptorTableCache = descriptorTableCache;
	m_gridSize = gridSize;
	m_lightGridSize = lightGridSize;
	m_pDepths = depths;

	// Create resources
	N_RETURN(createVolumeInfoBuffers(pCommandList, numVolumes, numVolumeSrcs, uploaders), false);

	m_volumes.resize(numVolumeSrcs);
	for (auto i = 0u; i < numVolumeSrcs; ++i)
	{
		m_volumes[i] = Texture3D::MakeUnique();
		N_RETURN(m_volumes[i]->Create(m_device.get(), gridSize, gridSize, gridSize, Format::R16G16B16A16_FLOAT,
			ResourceFlag::ALLOW_UNORDERED_ACCESS | ResourceFlag::ALLOW_SIMULTANEOUS_ACCESS, 1,
			MemoryType::DEFAULT, (L"Volume" + to_wstring(i)).c_str()), false);
	}

	m_cubeMaps.resize(numVolumes);
	m_cubeDepths.resize(numVolumes);
	for (auto i = 0u; i < numVolumes; ++i)
	{
		m_cubeMaps[i] = Texture2D::MakeUnique();
		N_RETURN(m_cubeMaps[i]->Create(m_device.get(), gridSize, gridSize, Format::R8G8B8A8_UNORM, 6,
			ResourceFlag::ALLOW_UNORDERED_ACCESS, g_numCubeMips, 1, MemoryType::DEFAULT, true,
			(L"CubeMap" + to_wstring(i)).c_str()), false);

		m_cubeDepths[i] = Texture2D::MakeUnique();
		N_RETURN(m_cubeDepths[i]->Create(m_device.get(), gridSize, gridSize, Format::R32_FLOAT, 6,
			ResourceFlag::ALLOW_UNORDERED_ACCESS, g_numCubeMips, 1, MemoryType::DEFAULT, true,
			(L"CubeDepth" + to_wstring(i)).c_str()), false);
	}

	m_lightMap = Texture3D::MakeUnique();
	N_RETURN(m_lightMap->Create(m_device.get(), m_lightGridSize, m_lightGridSize, m_lightGridSize,
		Format::R11G11B10_FLOAT,ResourceFlag::ALLOW_UNORDERED_ACCESS | ResourceFlag::ALLOW_SIMULTANEOUS_ACCESS,
		1, MemoryType::DEFAULT, L"LightMap"), false);

	m_cbPerFrame = ConstantBuffer::MakeUnique();
	N_RETURN(m_cbPerFrame->Create(m_device.get(), sizeof(CBPerFrame[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, L"RayCaster.CBPerFrame"), false);

	/*m_cbPerObject = ConstantBuffer::MakeUnique();
	N_RETURN(m_cbPerObject->Create(m_device.get(), sizeof(CBPerObject[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, L"RayCaster.CBPerObject"), false);*/

	// Create pipelines
	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(rtFormat), false);

	// create command layout
	N_RETURN(createCommandLayouts(), false);

	const float aabb[] = { -1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
	m_vertexBuffer = VertexBuffer::MakeUnique();
	N_RETURN(m_vertexBuffer->Create(m_device.get(), 1, sizeof(float[6]), ResourceFlag::NONE,
		MemoryType::DEFAULT, 1, nullptr, 0, nullptr, 0, nullptr, L"AABB"), false);
	uploaders.emplace_back(Resource::MakeUnique());
	N_RETURN(m_vertexBuffer->Upload(pCommandList, uploaders.back().get(), aabb, sizeof(float[6])), false);

	N_RETURN(createCubeIB(pCommandList, uploaders), false);

	//N_RETURN(buildAccelerationStructures(pCommandList, pGeometry), false);
	N_RETURN(createDescriptorTables(), false);

	return true;
}

bool MultiRayCaster::LoadVolumeData(XUSG::CommandList* pCommandList, uint32_t i, const wchar_t* fileName, vector<Resource::uptr>& uploaders)
{
	// Load input image
	{
		DDS::Loader textureLoader;
		DDS::AlphaMode alphaMode;

		uploaders.emplace_back(Resource::MakeUnique());
		N_RETURN(textureLoader.CreateTextureFromFile(m_device.get(), pCommandList, fileName,
			8192, false, m_fileSrc, uploaders.back().get(), &alphaMode), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_fileSrc->GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_FILE_SRC], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	ResourceBarrier barrier;
	m_volumes[i]->SetBarrier(&barrier, ResourceState::UNORDERED_ACCESS);

	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
	};
	pCommandList->SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	// Set pipeline state
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[LOAD_VOLUME_DATA]);
	pCommandList->SetPipelineState(m_pipelines[LOAD_VOLUME_DATA]);

	// Set descriptor tables
	pCommandList->SetComputeDescriptorTable(0, m_srvTables[SRV_TABLE_FILE_SRC]);
	pCommandList->SetComputeDescriptorTable(1, m_uavInitTables[i]);
	pCommandList->SetComputeDescriptorTable(2, m_samplerTable);

	// Dispatch grid
	pCommandList->Dispatch(DIV_UP(m_gridSize, 4), DIV_UP(m_gridSize, 4), DIV_UP(m_gridSize, 4));

	return true;
}

bool MultiRayCaster::SetDepthMaps(const DepthStencil::uptr* depths)
{
	m_pDepths = depths;

	return SetViewport(depths[DEPTH_MAP]->GetWidth(), depths[DEPTH_MAP]->GetHeight());
}

bool MultiRayCaster::SetViewport(uint32_t width, uint32_t height)
{
	m_kDepths = Texture2D::MakeUnique();
	N_RETURN(m_kDepths->Create(m_device.get(), width, height, Format::R32_UINT, NUM_OIT_LAYERS,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, MemoryType::DEFAULT, false, L"ColorKBuffer"), false);

	m_kColors = Texture2D::MakeUnique();
	N_RETURN(m_kColors->Create(m_device.get(), width, height, Format::R16G16B16A16_FLOAT, NUM_OIT_LAYERS,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, MemoryType::DEFAULT, false, L"ColorKBuffer"), false);

	return createDescriptorTables();
}

void MultiRayCaster::InitVolumeData(const XUSG::CommandList* pCommandList, uint32_t i)
{
	ResourceBarrier barrier;
	m_volumes[i]->SetBarrier(&barrier, ResourceState::UNORDERED_ACCESS);

	const DescriptorPool pDescriptorPool[] =
	{ m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL) };
	pCommandList->SetDescriptorPools(1, pDescriptorPool);

	// Set pipeline state
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[INIT_VOLUME_DATA]);
	pCommandList->SetPipelineState(m_pipelines[INIT_VOLUME_DATA]);

	// Set descriptor tables
	pCommandList->SetComputeDescriptorTable(0, m_uavInitTables[i]);

	// Dispatch grid
	pCommandList->Dispatch(DIV_UP(m_gridSize, 4), DIV_UP(m_gridSize, 4), DIV_UP(m_gridSize, 4));
}

void MultiRayCaster::SetVolumeWorld(float size, const DirectX::XMFLOAT3& pos)
{
	size *= 0.5f;
	auto world = XMMatrixScaling(size, size, size);
	world = world * XMMatrixTranslation(pos.x, pos.y, pos.z);
	XMStoreFloat3x4(&m_volumeWorld, world);
}

void MultiRayCaster::SetLightMapWorld(float size, const DirectX::XMFLOAT3& pos)
{
	size *= 0.5f;
	auto world = XMMatrixScaling(size, size, size);
	world = world * XMMatrixTranslation(pos.x, pos.y, pos.z);
	XMStoreFloat3x4(&m_lightMapWorld, world);
}

void MultiRayCaster::SetLight(const XMFLOAT3& pos, const XMFLOAT3& color, float intensity)
{
	m_lightPt = pos;
	m_lightColor = XMFLOAT4(color.x, color.y, color.z, intensity);
}

void MultiRayCaster::SetAmbient(const XMFLOAT3& color, float intensity)
{
	m_ambient = XMFLOAT4(color.x, color.y, color.z, intensity);
}

void MultiRayCaster::UpdateFrame(uint8_t frameIndex, CXMMATRIX viewProj, CXMMATRIX shadowVP, const XMFLOAT3& eyePt)
{
	const auto& depth = m_pDepths[DEPTH_MAP];
	const auto width = static_cast<float>(depth->GetWidth());
	const auto height = static_cast<float>(depth->GetHeight());

	// Per-frame
	{
		const auto pCbData = reinterpret_cast<CBPerFrame*>(m_cbPerFrame->Map(frameIndex));
		pCbData->EyePos = XMFLOAT4(eyePt.x, eyePt.y, eyePt.z, 1.0f);
		pCbData->Viewport = XMFLOAT4(width, height, 0.0f, 0.0f);
		pCbData->LightMapWorld = m_lightMapWorld;
		pCbData->LightPos = XMFLOAT4(m_lightPt.x, m_lightPt.y, m_lightPt.z, 1.0f);
		pCbData->LightColor = m_lightColor;
		pCbData->Ambient = m_ambient;
	}

	// Per-object
	{
		const auto lightWorld = XMLoadFloat3x4(&m_lightMapWorld);
		const auto lightWorldI = XMMatrixInverse(nullptr, lightWorld);

		const auto pMappedData = reinterpret_cast<PerObject*>(m_perObject->Map(frameIndex));
		for (auto i = 0u; i < 1; ++i)
		{
			const auto world = XMLoadFloat3x4(&m_volumeWorld);
			const auto worldI = XMMatrixInverse(nullptr, world);
			const auto worldViewProj = world * viewProj;
			const auto localToLight = world * lightWorldI;

			XMStoreFloat4x4(&pMappedData[i].WorldViewProj, XMMatrixTranspose(worldViewProj));
			XMStoreFloat4x4(&pMappedData[i].WorldViewProjI, XMMatrixTranspose(XMMatrixInverse(nullptr, worldViewProj)));
			XMStoreFloat4x4(&pMappedData[i].ShadowWVP, XMMatrixTranspose(world * shadowVP));
			XMStoreFloat3x4(&pMappedData[i].WorldI, worldI);
			XMStoreFloat3x4(&pMappedData[i].LocalToLight, localToLight);
		}
	}
}

void MultiRayCaster::Render(XUSG::CommandList* pCommandList, uint8_t frameIndex)
{
	static auto isFirstFrame = true;

	cullVolumes(pCommandList, frameIndex);

	if (isFirstFrame)
	{
		RayMarchL(pCommandList, frameIndex);
		isFirstFrame = false;
	}

	rayMarchV(pCommandList, frameIndex);
	cubeDepthPeel(pCommandList, frameIndex);
	renderCube(pCommandList, frameIndex);
	resolveOIT(pCommandList, frameIndex);
}

void MultiRayCaster::RayMarchL(const XUSG::CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barrier
	ResourceBarrier barrier;
	m_lightMap->SetBarrier(&barrier, ResourceState::UNORDERED_ACCESS);

	// Set pipeline state
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[RAY_MARCH_L]);
	pCommandList->SetPipelineState(m_pipelines[RAY_MARCH_L]);

	// Set descriptor tables
	pCommandList->SetComputeDescriptorTable(0, m_cbvSrvTables[frameIndex]);
	pCommandList->SetComputeDescriptorTable(1, m_srvTables[SRV_TABLE_VOLUME_DESCS]);
	pCommandList->SetComputeDescriptorTable(2, m_uavTables[UAV_TABLE_LIGHT_MAP]);
	pCommandList->SetComputeDescriptorTable(3, m_srvTables[SRV_TABLE_VOLUME]);
	pCommandList->SetComputeDescriptorTable(4, m_srvTables[SRV_TABLE_SHADOW]);
	pCommandList->SetComputeDescriptorTable(5, m_samplerTable);
	pCommandList->SetCompute32BitConstant(6, NUM_LIGHT_SAMPLES);

	// Dispatch grid
	pCommandList->Dispatch(DIV_UP(m_lightGridSize, 4), DIV_UP(m_lightGridSize, 4), DIV_UP(m_lightGridSize, 4));
}

const DescriptorTable& MultiRayCaster::GetLightSRVTable() const
{
	return m_srvTables[SRV_TABLE_LIGHT_MAP];
}

Resource* MultiRayCaster::GetLightMap() const
{
	return m_lightMap.get();
}

bool MultiRayCaster::createCubeIB(XUSG::CommandList* pCommandList, vector<Resource::uptr>& uploaders)
{
	uint16_t indices[] =
	{
		0, 1, 2, 3, 2, 1,
		4, 5, 6, 7, 6, 5,
		8, 9, 10, 11, 10, 9,
		12, 13, 14, 15, 14, 13,
		16, 17, 18, 19, 18, 17,
		20, 21, 22, 23, 22, 21
	};

	m_indexBuffer = IndexBuffer::MakeUnique();
	N_RETURN(m_indexBuffer->Create(m_device.get(), sizeof(indices), Format::R16_UINT, ResourceFlag::NONE,
		MemoryType::DEFAULT, 1, nullptr, 1, nullptr, 1, nullptr, L"CubeIB"), false);
	uploaders.emplace_back(Resource::MakeUnique());

	return m_indexBuffer->Upload(pCommandList, uploaders.back().get(), indices, sizeof(indices));
}

bool MultiRayCaster::createVolumeInfoBuffers(XUSG::CommandList* pCommandList, uint32_t numVolumes,
	uint32_t numVolumeSrcs, vector<Resource::uptr>& uploaders)
{
	{
		uint32_t firstSRVElements[FrameCount];
		for (uint8_t i = 0; i < FrameCount; ++i) firstSRVElements[i] = numVolumes * i;

		m_perObject = StructuredBuffer::MakeUnique();
		N_RETURN(m_perObject->Create(m_device.get(), numVolumes * FrameCount,
			sizeof(PerObject), ResourceFlag::NONE, MemoryType::UPLOAD, FrameCount,
			firstSRVElements, 0, nullptr, L"RayCaster.Matrices"), false);
	}

	{
		vector<VolumeDesc> volumeDescs(numVolumes);
		for (auto& volume : volumeDescs)
		{
			volume.VolTexId = rand() % numVolumeSrcs;
			volume.NumMips = g_numCubeMips;
			volume.CubeMapSize = static_cast<float>(m_gridSize);
		}

		m_volumeDescs = StructuredBuffer::MakeUnique();
		N_RETURN(m_volumeDescs->Create(m_device.get(), numVolumes,
			sizeof(VolumeDesc), ResourceFlag::NONE, MemoryType::DEFAULT, 1,
			nullptr, 1, nullptr, L"RayCaster.VolumeDescs"), false);

		uploaders.emplace_back(Resource::MakeUnique());
		m_volumeDescs->Upload(pCommandList, uploaders.back().get(),
			volumeDescs.data(), sizeof(VolumeDesc) * volumeDescs.size());
	}

	{
		const auto conterOffset = sizeof(uint32_t[2]);
		m_visibleVolumeCounter = RawBuffer::MakeShared();
		N_RETURN(m_visibleVolumeCounter->Create(m_device.get(), sizeof(uint32_t),
			ResourceFlag::ALLOW_UNORDERED_ACCESS | ResourceFlag::DENY_SHADER_RESOURCE,
			MemoryType::DEFAULT, 0, nullptr, 0, nullptr, L"RayCaster.VisibleVolumeCounter"), false);

		m_visibleVolumes = StructuredBuffer::MakeUnique();
		m_visibleVolumes->SetCounter(m_visibleVolumeCounter);
		N_RETURN(m_visibleVolumes->Create(m_device.get(), numVolumes, sizeof(VolumeDesc),
			ResourceFlag::ALLOW_UNORDERED_ACCESS, MemoryType::DEFAULT, 1, nullptr,
			1, nullptr, L"RayCaster.VisibleVolumes"), false);

		m_counterReset = StructuredBuffer::MakeUnique();
		N_RETURN(m_counterReset->Create(m_device.get(), 1, sizeof(uint32_t),
			ResourceFlag::DENY_SHADER_RESOURCE, MemoryType::DEFAULT, 0, nullptr,
			0, nullptr, L"RayCaster.CounterReset"), false);

		m_volumeDispatchArg = RawBuffer::MakeUnique();
		N_RETURN(m_volumeDispatchArg->Create(m_device.get(), sizeof(uint32_t[3]),
			ResourceFlag::DENY_SHADER_RESOURCE, MemoryType::DEFAULT, 0, nullptr,
			0, nullptr, L"RayCaster.VisibleVolumeDispatchArg"), false);

		const uint32_t pDispatchReset[] = { DIV_UP(m_gridSize, 8), DIV_UP(m_gridSize, 4), 0 };
		uploaders.emplace_back(Resource::MakeUnique());
		N_RETURN(m_volumeDispatchArg->Upload(pCommandList, uploaders.back().get(), pDispatchReset, sizeof(uint32_t[3])), false);

		m_volumeDrawArg = RawBuffer::MakeUnique();
		N_RETURN(m_volumeDrawArg->Create(m_device.get(), sizeof(uint32_t[5]),
			ResourceFlag::DENY_SHADER_RESOURCE, MemoryType::DEFAULT, 0, nullptr,
			0, nullptr, L"RayCaster.VisibleVolumeDrawArg"), false);

		const uint32_t pDrawReset[] = { 36, 0, 0, 0, 0 };
		uploaders.emplace_back(Resource::MakeUnique());
		N_RETURN(m_volumeDrawArg->Upload(pCommandList, uploaders.back().get(), pDrawReset, sizeof(uint32_t[5])), false);

		const auto clear = 0u;
		uploaders.emplace_back(Resource::MakeUnique());
		N_RETURN(m_counterReset->Upload(pCommandList, uploaders.back().get(), &clear, sizeof(uint32_t)), false);
	}

	{
		m_volumeVis = TypedBuffer::MakeUnique();
		N_RETURN(m_volumeVis->Create(m_device.get(), numVolumes, sizeof(uint8_t),
			Format::R8_UINT, ResourceFlag::ALLOW_UNORDERED_ACCESS, MemoryType::DEFAULT,
			1, nullptr, 1, nullptr, L"RayCaster.VolumeVisibility"), false);
	}

	return true;
}

bool MultiRayCaster::createPipelineLayouts()
{
	const auto numVolumes = static_cast<uint32_t>(m_cubeMaps.size());
	const auto numVolumeSrcs = static_cast<uint32_t>(m_volumes.size());

	// Load grid data
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(2, DescriptorType::SAMPLER, 1, 0);
		X_RETURN(m_pipelineLayouts[LOAD_VOLUME_DATA], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"LoadGridDataLayout"), false);
	}

	// Init grid data
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		X_RETURN(m_pipelineLayouts[INIT_VOLUME_DATA], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"InitGridDataLayout"), false);
	}

	// Volume culling
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::UAV, 2, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(2, DescriptorType::SRV, 1, 1, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetConstants(3, 1, 1);
		X_RETURN(m_pipelineLayouts[VOLUME_CULL], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"VolumeCullingLayout"), false);
	}

	// Light space ray marching
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 1, 1, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(2, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(3, DescriptorType::SRV, numVolumeSrcs, 0, 1);
		pipelineLayout->SetRange(4, DescriptorType::SRV, 1, 0, 2);
		pipelineLayout->SetRange(5, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout->SetConstants(6, 1, 1);
		X_RETURN(m_pipelineLayouts[RAY_MARCH_L], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"LightSpaceRayMarchingLayout"), false);
	}

	// View space ray marching
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 2, 1, 0);
		pipelineLayout->SetRange(2, DescriptorType::UAV, g_numCubeMips * numVolumes, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(3, DescriptorType::UAV, g_numCubeMips * numVolumes, 0, 1, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(4, DescriptorType::SRV, numVolumeSrcs, 0, 1);
		pipelineLayout->SetRange(5, DescriptorType::SRV, 1, 0, 2);
		pipelineLayout->SetRange(6, DescriptorType::SAMPLER, 2, 0);
		X_RETURN(m_pipelineLayouts[RAY_MARCH_V], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"ViewSpaceRayMarchingLayout"), false);
	}

	// Cube depth peeling
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 1, 1, 0);
		pipelineLayout->SetRange(2, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetShaderStage(0, Shader::Stage::VS);
		pipelineLayout->SetShaderStage(1, Shader::Stage::VS);
		pipelineLayout->SetShaderStage(2, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[CUBE_DEPTH_PEEL], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"CubeDepthPeelingLayout"), false);
	}

	// Cube rendering
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 1, 1, 0);
		pipelineLayout->SetRange(2, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(3, DescriptorType::SRV, 1, 1, 0);
		pipelineLayout->SetRange(4, DescriptorType::SRV, g_numCubeMips * numVolumes, 0, 1);
		pipelineLayout->SetRange(5, DescriptorType::SRV, g_numCubeMips * numVolumes, 0, 2);
		pipelineLayout->SetRange(6, DescriptorType::SRV, 1, 0, 3);
		pipelineLayout->SetRange(7, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout->SetShaderStage(1, Shader::Stage::VS);
		pipelineLayout->SetShaderStage(2, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(3, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(4, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(5, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(6, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(7, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[RENDER_CUBE], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"CubeRenderingLayout"), false);
	}

	// Resolve OIT
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetShaderStage(0, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[RESOLVE_OIT], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"ResolveOITLayout"), false);
	}

	return true;
}

bool MultiRayCaster::createPipelines(Format rtFormat)
{
	auto vsIndex = 0u;
	auto psIndex = 0u;
	auto csIndex = 0u;

	// Load grid data
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSR32FToRGBA16F.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[LOAD_VOLUME_DATA]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[LOAD_VOLUME_DATA], state->GetPipeline(m_computePipelineCache.get(), L"InitGridData"), false);
	}

	// Init grid data
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSInitGridData.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[INIT_VOLUME_DATA]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[INIT_VOLUME_DATA], state->GetPipeline(m_computePipelineCache.get(), L"InitGridData"), false);
	}

	// Volume culling
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSVolumeCull.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[VOLUME_CULL]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[VOLUME_CULL], state->GetPipeline(m_computePipelineCache.get(), L"VolumeCulling"), false);
	}

	// Light space ray marching
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSRayMarchL.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RAY_MARCH_L]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[RAY_MARCH_L], state->GetPipeline(m_computePipelineCache.get(), L"LightSpaceRayMarching"), false);
	}

	// View space ray marching
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSRayMarchV.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RAY_MARCH_V]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[RAY_MARCH_V], state->GetPipeline(m_computePipelineCache.get(), L"ViewSpaceRayMarching"), false);
	}

	// Cube depth peeling
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSCubeDP.cso"), false);
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSDepthPeel.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[CUBE_DEPTH_PEEL]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex++));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->RSSetState(Graphics::CULL_FRONT, m_graphicsPipelineCache.get()); // Front-face culling for interior surfaces
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		X_RETURN(m_pipelines[CUBE_DEPTH_PEEL], state->GetPipeline(m_graphicsPipelineCache.get(), L"CubeDepthPeeling"), false);
	}

	// Cube rendering
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSCube.cso"), false);
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSCube.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RENDER_CUBE]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex++));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->RSSetState(Graphics::CULL_FRONT, m_graphicsPipelineCache.get()); // Front-face culling for interior surfaces
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		//state->OMSetBlendState(Graphics::PREMULTIPLITED, m_graphicsPipelineCache.get());
		//state->OMSetRTVFormats(&rtFormat, 1);
		X_RETURN(m_pipelines[RENDER_CUBE], state->GetPipeline(m_graphicsPipelineCache.get(), L"CubeRendering"), false);
	}

	// Resolve OIT
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSResolveOIT.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RESOLVE_OIT]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex++));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		state->OMSetBlendState(Graphics::PREMULTIPLITED, m_graphicsPipelineCache.get());
		state->OMSetRTVFormats(&rtFormat, 1);
		X_RETURN(m_pipelines[RESOLVE_OIT], state->GetPipeline(m_graphicsPipelineCache.get(), L"ResolveOIT"), false);
	}

	return true;
}

bool MultiRayCaster::createCommandLayouts()
{
	{
		IndirectArgument arg;
		arg.Type = IndirectArgumentType::DISPATCH;
		m_commandLayouts[DISPATCH_LAYOUT] = CommandLayout::MakeUnique();
		N_RETURN(m_commandLayouts[DISPATCH_LAYOUT]->Create(m_device.get(), sizeof(uint32_t[3]), 1, &arg), false);
	}

	{
		IndirectArgument arg;
		arg.Type = IndirectArgumentType::DRAW_INDEXED;
		m_commandLayouts[DRAW_LAYOUT] = CommandLayout::MakeUnique();
		N_RETURN(m_commandLayouts[DRAW_LAYOUT]->Create(m_device.get(), sizeof(uint32_t[5]), 1, &arg), false);
	}

	return true;
}

bool MultiRayCaster::createDescriptorTables()
{
	const auto numVolumes = static_cast<uint32_t>(m_cubeMaps.size());
	const auto numVolumeSrcs = static_cast<uint32_t>(m_volumes.size());

	// Create CBV and SRV tables
	for (uint8_t i = 0; i < FrameCount; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		const Descriptor descriptors[] =
		{
			m_cbPerFrame->GetCBV(i),
			m_perObject->GetSRV(i)
		};
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_cbvSrvTables[i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Create UAV tables
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		vector<Descriptor> descriptors(g_numCubeMips * numVolumes);
		for (auto i = 0u; i < numVolumes; ++i)
			for (uint8_t j = 0; j < g_numCubeMips; ++j)
				descriptors[g_numCubeMips * i + j] = m_cubeMaps[i]->GetUAV(j);
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(descriptors.size()), descriptors.data());
		X_RETURN(m_uavTables[UAV_TABLE_CUBE_MAP], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	for (uint8_t i = 0; i < g_numCubeMips; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		vector<Descriptor> descriptors(g_numCubeMips * numVolumes);
		for (auto i = 0u; i < numVolumes; ++i)
			for (uint8_t j = 0; j < g_numCubeMips; ++j)
				descriptors[g_numCubeMips * i + j] = m_cubeDepths[i]->GetUAV(j);
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(descriptors.size()), descriptors.data());
		X_RETURN(m_uavTables[UAV_TABLE_CUBE_DEPTH], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_lightMap->GetUAV());
		X_RETURN(m_uavTables[UAV_TABLE_LIGHT_MAP], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	if (m_kDepths)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_kDepths->GetUAV());
		X_RETURN(m_uavTables[UAV_TABLE_K_DEPTHS], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	if (m_kColors)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_kColors->GetUAV());
		X_RETURN(m_uavTables[UAV_TABLE_K_COLORS], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Create SRV tables
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_volumeDescs->GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_VOLUME_DESCS], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		vector<Descriptor> descriptors(numVolumeSrcs);
		for (auto i = 0u; i < numVolumeSrcs; ++i) descriptors[i] = m_volumes[i]->GetSRV();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(descriptors.size()), descriptors.data());
		X_RETURN(m_srvTables[SRV_TABLE_VOLUME], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_visibleVolumes->GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_VIS_VOLUMES], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_lightMap->GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_LIGHT_MAP], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	if (m_kDepths)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_kDepths->GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_K_DEPTHS], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	if (m_kColors)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_kColors->GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_K_COLORS], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	if (m_pDepths[DEPTH_MAP])
	{
		{
			const auto descriptorTable = Util::DescriptorTable::MakeUnique();
			descriptorTable->SetDescriptors(0, 1, &m_pDepths[DEPTH_MAP]->GetSRV());
			X_RETURN(m_srvTables[SRV_TABLE_DEPTH], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
		}

		{
			const auto descriptorTable = Util::DescriptorTable::MakeUnique();
			descriptorTable->SetDescriptors(0, 1, &m_pDepths[SHADOW_MAP]->GetSRV());
			X_RETURN(m_srvTables[SRV_TABLE_SHADOW], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
		}
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		vector<Descriptor> descriptors(g_numCubeMips * numVolumes);
		for (auto i = 0u; i < numVolumes; ++i)
			for (uint8_t j = 0; j < g_numCubeMips; ++j)
				descriptors[g_numCubeMips * i + j] = m_cubeMaps[i]->GetSRV(j);
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(descriptors.size()), descriptors.data());
		X_RETURN(m_srvTables[SRV_TABLE_CUBE_MAP], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	for (uint8_t i = 0; i < g_numCubeMips; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		vector<Descriptor> descriptors(g_numCubeMips * numVolumes);
		for (auto i = 0u; i < numVolumes; ++i)
			for (uint8_t j = 0; j < g_numCubeMips; ++j)
				descriptors[g_numCubeMips * i + j] = m_cubeDepths[i]->GetSRV(j);
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(descriptors.size()), descriptors.data());
		X_RETURN(m_srvTables[SRV_TABLE_CUBE_DEPTH], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Create UAV tables
	m_uavInitTables.resize(numVolumeSrcs);
	for (auto i = 0u; i < numVolumeSrcs; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_volumes[i]->GetUAV());
		X_RETURN(m_uavInitTables[i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		const Descriptor descriptors[] =
		{
			m_visibleVolumes->GetUAV(),
			m_volumeVis->GetUAV()
		};
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_uavTables[UAV_TABLE_CULL], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Create the sampler
	const auto descriptorTable = Util::DescriptorTable::MakeUnique();
	const SamplerPreset samplers[] = { SamplerPreset::LINEAR_CLAMP, SamplerPreset::POINT_CLAMP };
	descriptorTable->SetSamplers(0, static_cast<uint32_t>(size(samplers)), samplers, m_descriptorTableCache.get());
	X_RETURN(m_samplerTable, descriptorTable->GetSamplerTable(m_descriptorTableCache.get()), false);

	return true;
}

bool MultiRayCaster::buildAccelerationStructures(const RayTracing::CommandList* pCommandList, GeometryBuffer* pGeometry)
{
	AccelerationStructure::SetFrameCount(FrameCount);

	// Set geometries
	BottomLevelAS::SetAABBGeometries(*pGeometry, 1, &m_vertexBuffer->GetVBV());

	// Descriptor index in descriptor pool
	const auto bottomLevelASIndex = 0u;
	const auto topLevelASIndex = bottomLevelASIndex + 1;

	// Prebuild
	m_bottomLevelAS = BottomLevelAS::MakeUnique();
	m_topLevelAS = TopLevelAS::MakeUnique();
	N_RETURN(m_bottomLevelAS->PreBuild(m_device.get(), 1, *pGeometry, bottomLevelASIndex), false);
	N_RETURN(m_topLevelAS->PreBuild(m_device.get(), 1, topLevelASIndex), false);

	// Create scratch buffer
	auto scratchSize = m_topLevelAS->GetScratchDataMaxSize();
	scratchSize = (max)(m_bottomLevelAS->GetScratchDataMaxSize(), scratchSize);
	m_scratch = Resource::MakeUnique();
	N_RETURN(AccelerationStructure::AllocateUAVBuffer(m_device.get(), m_scratch.get(), scratchSize), false);

	// Get descriptor pool and create descriptor tables
	N_RETURN(createDescriptorTables(), false);
	const auto& descriptorPool = m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL);

	// Set instance
	float* const pTransform[] = { reinterpret_cast<float*>(&m_volumeWorld) };
	m_instances = Resource::MakeUnique();
	const BottomLevelAS* ppBottomLevelAS[] = { m_bottomLevelAS.get() };
	TopLevelAS::SetInstances(m_device.get(), m_instances.get(), 1, ppBottomLevelAS, pTransform);

	// Build bottom level ASs
	m_bottomLevelAS->Build(pCommandList, m_scratch.get(), descriptorPool);

	// Build top level AS
	m_topLevelAS->Build(pCommandList, m_scratch.get(), m_instances.get(), descriptorPool);

	return true;
}

bool MultiRayCaster::buildShaderTables()
{
	// Get shader identifiers.
	const auto shaderIDSize = ShaderRecord::GetShaderIDSize(m_device.get());

	// Ray gen shader table
	m_rayGenShaderTable = ShaderTable::MakeUnique();
	N_RETURN(m_rayGenShaderTable->Create(m_device.get(), 1, shaderIDSize, L"RayGenShaderTable"), false);
	N_RETURN(m_rayGenShaderTable->AddShaderRecord(ShaderRecord::MakeUnique(m_device.get(), m_pipelines[RAY_TRACING], RaygenShaderName).get()), false);

	// Hit group shader table
	m_hitGroupShaderTable = ShaderTable::MakeUnique();
	N_RETURN(m_hitGroupShaderTable->Create(m_device.get(), 1, shaderIDSize, L"HitGroupShaderTable"), false);
	N_RETURN(m_hitGroupShaderTable->AddShaderRecord(ShaderRecord::MakeUnique(m_device.get(), m_pipelines[RAY_TRACING], HitGroupName).get()), false);

	// Miss shader table
	m_missShaderTable = ShaderTable::MakeUnique();
	N_RETURN(m_missShaderTable->Create(m_device.get(), 1, shaderIDSize, L"MissShaderTable"), false);
	N_RETURN(m_missShaderTable->AddShaderRecord(ShaderRecord::MakeUnique(m_device.get(), m_pipelines[RAY_TRACING], MissShaderName).get()), false);

	return true;
}

void MultiRayCaster::cullVolumes(const XUSG::CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barrier
	ResourceBarrier barriers[2];
	auto numBarriers = m_visibleVolumeCounter->SetBarrier(barriers, ResourceState::COPY_DEST);
	//pCommandList->Barrier(numBarriers, barriers); // Promotion

	// Reset counter
	pCommandList->CopyResource(m_visibleVolumeCounter.get(), m_counterReset.get());

	// Set barriers
	numBarriers = m_visibleVolumes->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS);
	//numBarriers = m_visibleVolumeCounter->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS, numBarriers);
	numBarriers = m_volumeVis->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS, numBarriers);
	pCommandList->Barrier(numBarriers, barriers);

	// Set pipeline state
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[VOLUME_CULL]);
	pCommandList->SetPipelineState(m_pipelines[VOLUME_CULL]);

	// Set descriptor tables
	pCommandList->SetComputeDescriptorTable(0, m_cbvSrvTables[frameIndex]);
	pCommandList->SetComputeDescriptorTable(1, m_uavTables[UAV_TABLE_CULL]);
	pCommandList->SetComputeDescriptorTable(2, m_srvTables[SRV_TABLE_VOLUME_DESCS]);
	pCommandList->SetCompute32BitConstant(3, NUM_SAMPLES);

	// Dispatch cube
	const uint32_t numVolumes = m_volumeDescs->GetWidth() / sizeof(VolumeDesc);
	pCommandList->Dispatch(DIV_UP(numVolumes, 4), 1, 1);
}

void MultiRayCaster::rayMarchV(XUSG::CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barriers
	vector<ResourceBarrier> barriers(m_cubeMaps.size() + m_cubeDepths.size() + 6);
	auto numBarriers = m_visibleVolumes->SetBarrier(barriers.data(), ResourceState::NON_PIXEL_SHADER_RESOURCE);
	numBarriers = m_lightMap->SetBarrier(barriers.data(), ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers);
	numBarriers = m_pDepths[DEPTH_MAP]->SetBarrier(barriers.data(), ResourceState::NON_PIXEL_SHADER_RESOURCE |
		ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
	for (auto& cubeMap : m_cubeMaps)
		numBarriers = cubeMap->SetBarrier(barriers.data(), ResourceState::UNORDERED_ACCESS, numBarriers);
	for (auto& cubeDepth : m_cubeDepths)
		numBarriers = cubeDepth->SetBarrier(barriers.data(), ResourceState::UNORDERED_ACCESS, numBarriers);
	numBarriers = m_volumeDispatchArg->SetBarrier(barriers.data(), ResourceState::COPY_DEST, numBarriers);
	numBarriers = m_volumeDrawArg->SetBarrier(barriers.data(), ResourceState::COPY_DEST, numBarriers);
	numBarriers = m_visibleVolumeCounter->SetBarrier(barriers.data(), ResourceState::COPY_SOURCE, numBarriers);
	pCommandList->Barrier(numBarriers, barriers.data());

	// Copy counter to dispatch arg.z
	pCommandList->CopyBufferRegion(m_volumeDispatchArg.get(), sizeof(uint32_t[2]), m_visibleVolumeCounter.get(), 0, sizeof(uint32_t));
	pCommandList->CopyBufferRegion(m_volumeDrawArg.get(), sizeof(uint32_t[1]), m_visibleVolumeCounter.get(), 0, sizeof(uint32_t));

	numBarriers = m_volumeDispatchArg->SetBarrier(barriers.data(), ResourceState::INDIRECT_ARGUMENT);
	pCommandList->Barrier(numBarriers, barriers.data());

	// Set pipeline state
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[RAY_MARCH_V]);
	pCommandList->SetPipelineState(m_pipelines[RAY_MARCH_V]);

	// Set descriptor tables
	pCommandList->SetComputeDescriptorTable(0, m_cbvSrvTables[frameIndex]);
	pCommandList->SetComputeDescriptorTable(1, m_srvTables[SRV_TABLE_VIS_VOLUMES]);
	pCommandList->SetComputeDescriptorTable(2, m_uavTables[UAV_TABLE_CUBE_MAP]);
	pCommandList->SetComputeDescriptorTable(3, m_uavTables[UAV_TABLE_CUBE_DEPTH]);
	pCommandList->SetComputeDescriptorTable(4, m_srvTables[SRV_TABLE_VOLUME]);
	pCommandList->SetComputeDescriptorTable(5, m_srvTables[SRV_TABLE_DEPTH]);
	pCommandList->SetComputeDescriptorTable(6, m_samplerTable);

	// Dispatch cube
	pCommandList->ExecuteIndirect(m_commandLayouts[DISPATCH_LAYOUT].get(), 1, m_volumeDispatchArg.get());
}

void MultiRayCaster::cubeDepthPeel(XUSG::CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barrier
	ResourceBarrier barriers[2];
	auto numBarriers = m_kDepths->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS);
	numBarriers = m_volumeDispatchArg->SetBarrier(barriers, ResourceState::INDIRECT_ARGUMENT, numBarriers);
	pCommandList->Barrier(numBarriers, barriers);

	// Clear depths
	const auto maxDepth = 1.0f;
	const auto maxDepthU = reinterpret_cast<const uint32_t&>(maxDepth);
	const uint32_t clearDepth[4] = { maxDepthU };
	pCommandList->ClearUnorderedAccessViewUint(m_uavTables[UAV_TABLE_K_DEPTHS], m_kDepths->GetUAV(), m_kDepths.get(), clearDepth);

	// Set pipeline state
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[CUBE_DEPTH_PEEL]);
	pCommandList->SetPipelineState(m_pipelines[CUBE_DEPTH_PEEL]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLESTRIP);

	// Set descriptor tables
	pCommandList->SetGraphicsDescriptorTable(0, m_cbvSrvTables[frameIndex]);
	pCommandList->SetGraphicsDescriptorTable(1, m_srvTables[SRV_TABLE_VIS_VOLUMES]);
	pCommandList->SetGraphicsDescriptorTable(2, m_uavTables[UAV_TABLE_K_DEPTHS]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);
	pCommandList->IASetIndexBuffer(m_indexBuffer->GetIBV());
	pCommandList->ExecuteIndirect(m_commandLayouts[DRAW_LAYOUT].get(), 1, m_volumeDrawArg.get());
}

void MultiRayCaster::renderCube(XUSG::CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barriers
	vector<ResourceBarrier> barriers(m_cubeMaps.size() + m_cubeDepths.size() + 2);
	auto numBarriers = m_kColors->SetBarrier(barriers.data(), ResourceState::UNORDERED_ACCESS);
	numBarriers = m_kDepths->SetBarrier(barriers.data(), ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
	for (auto& cubeMap : m_cubeMaps)
		numBarriers = cubeMap->SetBarrier(barriers.data(), ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
	for (auto& cubeDepth : m_cubeDepths)
		numBarriers = cubeDepth->SetBarrier(barriers.data(), ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
	pCommandList->Barrier(numBarriers, barriers.data());

	// Clear colors
	const float clearColor[4] = { 0.0f };
	pCommandList->ClearUnorderedAccessViewFloat(m_uavTables[UAV_TABLE_K_COLORS], m_kColors->GetUAV(), m_kColors.get(), clearColor);

	// Set pipeline state
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[RENDER_CUBE]);
	pCommandList->SetPipelineState(m_pipelines[RENDER_CUBE]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLESTRIP);

	// Set descriptor tables
	pCommandList->SetGraphicsDescriptorTable(0, m_cbvSrvTables[frameIndex]);
	pCommandList->SetGraphicsDescriptorTable(1, m_srvTables[SRV_TABLE_VIS_VOLUMES]);
	pCommandList->SetGraphicsDescriptorTable(2, m_uavTables[UAV_TABLE_K_COLORS]);
	pCommandList->SetGraphicsDescriptorTable(3, m_srvTables[SRV_TABLE_K_DEPTHS]);
	pCommandList->SetGraphicsDescriptorTable(4, m_srvTables[SRV_TABLE_CUBE_MAP]);
	pCommandList->SetGraphicsDescriptorTable(5, m_srvTables[SRV_TABLE_CUBE_DEPTH]);
	pCommandList->SetGraphicsDescriptorTable(6, m_srvTables[SRV_TABLE_DEPTH]);
	pCommandList->SetGraphicsDescriptorTable(7, m_samplerTable);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);
	pCommandList->IASetIndexBuffer(m_indexBuffer->GetIBV());
	pCommandList->ExecuteIndirect(m_commandLayouts[DRAW_LAYOUT].get(), 1, m_volumeDrawArg.get());
}

void MultiRayCaster::resolveOIT(const XUSG::CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barrier
	ResourceBarrier barrier;
	const auto numBarriers = m_kColors->SetBarrier(&barrier, ResourceState::PIXEL_SHADER_RESOURCE);
	pCommandList->Barrier(numBarriers, &barrier);

	// Set pipeline state
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[RESOLVE_OIT]);
	pCommandList->SetPipelineState(m_pipelines[RESOLVE_OIT]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLESTRIP);

	// Set descriptor table
	pCommandList->SetGraphicsDescriptorTable(0, m_srvTables[SRV_TABLE_K_COLORS]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLESTRIP);
	pCommandList->Draw(3, 1, 0, 0);
}
