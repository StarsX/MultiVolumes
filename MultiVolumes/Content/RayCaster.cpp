//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConsts.h"
#include "RayCaster.h"
#define _INDEPENDENT_DDS_LOADER_
#include "Advanced/XUSGDDSLoader.h"
#undef _INDEPENDENT_DDS_LOADER_

#define NUM_SAMPLES			256
#define NUM_LIGHT_SAMPLES	64

using namespace std;
using namespace DirectX;
using namespace XUSG;

struct CBPerFrame
{
	XMFLOAT4 EyePos;
	XMFLOAT4 Viewport;
	XMFLOAT3X4 LightMapWorld;
	XMFLOAT4 LightPos;
	XMFLOAT4 LightColor;
	XMFLOAT4 Ambient;
};

struct CBPerObject
{
	XMFLOAT4X4 WorldViewProjI;
	XMFLOAT4X4 WorldViewProj;
	XMFLOAT4X4 ShadowWVP;
	XMFLOAT3X4 WorldI;
	XMFLOAT3X4 LightMapWorld;
	XMFLOAT4 EyePos;
	XMFLOAT4 LightPos;
	XMFLOAT4 LightColor;
	XMFLOAT4 Ambient;
};

struct Matrices
{
	XMFLOAT4X4 WorldViewProj;
	XMFLOAT4X4 WorldViewProjI;
	XMFLOAT3X4 WorldI;
	XMFLOAT4X4 ShadowWVP;
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

RayCaster::RayCaster(const Device::sptr& device) :
	m_device(device),
	m_lightPt(75.0f, 75.0f, -75.0f),
	m_lightColor(1.0f, 0.7f, 0.3f, 1.0f),
	m_ambient(0.0f, 0.3f, 1.0f, 0.4f)
{
	m_shaderPool = ShaderPool::MakeUnique();
	m_graphicsPipelineCache = Graphics::PipelineCache::MakeUnique(device.get());
	m_computePipelineCache = Compute::PipelineCache::MakeUnique(device.get());
	m_pipelineLayoutCache = PipelineLayoutCache::MakeUnique(device.get());

	XMStoreFloat3x4(&m_volumeWorld, XMMatrixScaling(10.0f, 10.0f, 10.0f));
	m_lightMapWorld = m_volumeWorld;
}

RayCaster::~RayCaster()
{
}

bool RayCaster::Init(CommandList* pCommandList, const DescriptorTableCache::sptr& descriptorTableCache,
	Format rtFormat, uint32_t gridSize, uint32_t numVolumes, uint32_t numVolumeSrcs,
	const DepthStencil::uptr* depths, vector<Resource::uptr>& uploaders)
{
	m_descriptorTableCache = descriptorTableCache;
	m_gridSize = gridSize;
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

	m_lightGridSize = gridSize >> 1;
	m_lightMap = Texture3D::MakeUnique();
	N_RETURN(m_lightMap->Create(m_device.get(), m_lightGridSize, m_lightGridSize, m_lightGridSize,
		Format::R11G11B10_FLOAT,ResourceFlag::ALLOW_UNORDERED_ACCESS | ResourceFlag::ALLOW_SIMULTANEOUS_ACCESS,
		1, MemoryType::DEFAULT, L"LightMap"), false);

	m_cbPerFrame = ConstantBuffer::MakeUnique();
	N_RETURN(m_cbPerFrame->Create(m_device.get(), sizeof(CBPerFrame[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, L"RayCaster.CBPerFrame"), false);

	m_cbPerObject = ConstantBuffer::MakeUnique();
	N_RETURN(m_cbPerObject->Create(m_device.get(), sizeof(CBPerObject[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, L"RayCaster.CBPerObject"), false);

	// Create pipelines
	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(rtFormat), false);
	N_RETURN(createDescriptorTables(), false);

	return true;
}

bool RayCaster::LoadVolumeData(CommandList* pCommandList, uint32_t i, const wchar_t* fileName, vector<Resource::uptr>& uploaders)
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

bool RayCaster::SetDepthMaps(const DepthStencil::uptr* depths)
{
	m_pDepths = depths;

	return createDescriptorTables();
}

void RayCaster::InitVolumeData(const CommandList* pCommandList, uint32_t i)
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

void RayCaster::SetVolumeWorld(float size, const DirectX::XMFLOAT3& pos)
{
	size *= 0.5f;
	auto world = XMMatrixScaling(size, size, size);
	world = world * XMMatrixTranslation(pos.x, pos.y, pos.z);
	XMStoreFloat3x4(&m_volumeWorld, world);
}

void RayCaster::SetLightMapWorld(float size, const DirectX::XMFLOAT3& pos)
{
	size *= 0.5f;
	auto world = XMMatrixScaling(size, size, size);
	world = world * XMMatrixTranslation(pos.x, pos.y, pos.z);
	XMStoreFloat3x4(&m_lightMapWorld, world);
}

void RayCaster::SetLight(const XMFLOAT3& pos, const XMFLOAT3& color, float intensity)
{
	m_lightPt = pos;
	m_lightColor = XMFLOAT4(color.x, color.y, color.z, intensity);
}

void RayCaster::SetAmbient(const XMFLOAT3& color, float intensity)
{
	m_ambient = XMFLOAT4(color.x, color.y, color.z, intensity);
}

void RayCaster::UpdateFrame(uint8_t frameIndex, CXMMATRIX viewProj, CXMMATRIX shadowVP, const XMFLOAT3& eyePt)
{
	const auto& depth = m_pDepths[DEPTH_MAP];
	const auto width = static_cast<float>(depth->GetWidth());
	const auto height = static_cast<float>(depth->GetHeight());

	// General matrices
	const auto world = XMLoadFloat3x4(&m_volumeWorld);
	const auto worldI = XMMatrixInverse(nullptr, world);
	const auto worldViewProj = world * viewProj;

	{
		// Screen space matrices
		const auto pCbData = reinterpret_cast<CBPerObject*>(m_cbPerObject->Map(frameIndex));
		XMStoreFloat4x4(&pCbData->WorldViewProj, XMMatrixTranspose(worldViewProj));
		XMStoreFloat4x4(&pCbData->WorldViewProjI, XMMatrixTranspose(XMMatrixInverse(nullptr, worldViewProj)));
		XMStoreFloat4x4(&pCbData->ShadowWVP, XMMatrixTranspose(world * shadowVP));
		XMStoreFloat3x4(&pCbData->WorldI, worldI);

		// Lighting
		pCbData->LightMapWorld = m_lightMapWorld;
		pCbData->EyePos = XMFLOAT4(eyePt.x, eyePt.y, eyePt.z, 1.0f);
		pCbData->LightPos = XMFLOAT4(m_lightPt.x, m_lightPt.y, m_lightPt.z, 1.0f);
		pCbData->LightColor = m_lightColor;
		pCbData->Ambient = m_ambient;
	}

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
		const auto pMappedData = reinterpret_cast<Matrices*>(m_matrices->Map(frameIndex));

		for (auto i = 0u; i < 1; ++i)
		{
			XMStoreFloat4x4(&pMappedData[i].WorldViewProj, XMMatrixTranspose(worldViewProj));
			XMStoreFloat4x4(&pMappedData[i].WorldViewProjI, XMMatrixTranspose(XMMatrixInverse(nullptr, worldViewProj)));
			XMStoreFloat3x4(&pMappedData[i].WorldI, worldI);
			XMStoreFloat4x4(&pMappedData[i].ShadowWVP, XMMatrixTranspose(world * shadowVP));
		}
	}
}

void RayCaster::Render(const CommandList* pCommandList, uint8_t frameIndex)
{
	static auto isFirstFrame = true;

	cullVolumes(pCommandList, frameIndex);

	if (isFirstFrame)
	{
		RayMarchL(pCommandList, frameIndex);
		isFirstFrame = false;
	}

	rayMarchV(pCommandList, frameIndex);
	renderCube(pCommandList, frameIndex);
}

void RayCaster::RayMarchL(const CommandList* pCommandList, uint8_t frameIndex)
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

const DescriptorTable& RayCaster::GetLightSRVTable() const
{
	return m_srvTables[SRV_TABLE_LIGHT_MAP];
}

Resource* RayCaster::GetLightMap() const
{
	return m_lightMap.get();
}

bool RayCaster::createVolumeInfoBuffers(CommandList* pCommandList, uint32_t numVolumes,
	uint32_t numVolumeSrcs, vector<Resource::uptr>& uploaders)
{
	{
		uint32_t firstSRVElements[FrameCount];
		for (uint8_t i = 0; i < FrameCount; ++i) firstSRVElements[i] = numVolumes * i;

		m_matrices = StructuredBuffer::MakeUnique();
		N_RETURN(m_matrices->Create(m_device.get(), numVolumes * FrameCount,
			sizeof(Matrices), ResourceFlag::NONE, MemoryType::UPLOAD, FrameCount,
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
			MemoryType::DEFAULT, 0, nullptr, 1, nullptr, L"RayCaster.VisibleVolumeCounter"), false);

		m_visibleVolumes = StructuredBuffer::MakeUnique();
		m_visibleVolumes->SetCounter(m_visibleVolumeCounter);
		N_RETURN(m_visibleVolumes->Create(m_device.get(), numVolumes, sizeof(VolumeDesc),
			ResourceFlag::ALLOW_UNORDERED_ACCESS, MemoryType::DEFAULT, 1, nullptr,
			1, nullptr, L"RayCaster.VisibleVolumes"), false);

		m_counterReset = StructuredBuffer::MakeUnique();
		N_RETURN(m_counterReset->Create(m_device.get(), 1, sizeof(uint32_t), ResourceFlag::NONE,
			MemoryType::DEFAULT, 1, nullptr, 1, nullptr, L"RayCaster.CounterReset"), false);

		m_volumeDispatchArg = RawBuffer::MakeUnique();
		N_RETURN(m_volumeDispatchArg->Create(m_device.get(), sizeof(uint32_t[3]),
			ResourceFlag::DENY_SHADER_RESOURCE, MemoryType::DEFAULT, 0, nullptr,
			0, nullptr, L"RayCaster.VisibleVolumeDispatchArg"), false);

		const uint32_t pDataReset[] = { DIV_UP(m_gridSize, 8), DIV_UP(m_gridSize, 4), 0 };
		uploaders.emplace_back(Resource::MakeUnique());
		N_RETURN(m_volumeDispatchArg->Upload(pCommandList, uploaders.back().get(), pDataReset, sizeof(uint32_t[3])), false);

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

bool RayCaster::createPipelineLayouts()
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
		pipelineLayout->SetRange(6, DescriptorType::SAMPLER, 1, 0);
		X_RETURN(m_pipelineLayouts[RAY_MARCH_V], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"ViewSpaceRayMarchingLayout"), false);
	}

	// Cube rendering
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 2, 0);
		pipelineLayout->SetRange(2, DescriptorType::SRV, 1, 2);
		pipelineLayout->SetRange(3, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout->SetShaderStage(1, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(2, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(3, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[RENDER_CUBE], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"CubeLayout"), false);
	}

	return true;
}

bool RayCaster::createPipelines(Format rtFormat)
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
		state->OMSetBlendState(Graphics::PREMULTIPLITED, m_graphicsPipelineCache.get());
		state->OMSetRTVFormats(&rtFormat, 1);
		X_RETURN(m_pipelines[RENDER_CUBE], state->GetPipeline(m_graphicsPipelineCache.get(), L"RayCasting"), false);
	}

	return true;
}

bool RayCaster::createDescriptorTables()
{
	const auto numVolumes = static_cast<uint32_t>(m_cubeMaps.size());
	const auto numVolumeSrcs = static_cast<uint32_t>(m_volumes.size());

	// Create CBV tables
	for (uint8_t i = 0; i < FrameCount; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_cbPerObject->GetCBV(i));
		X_RETURN(m_cbvTables[i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Create CBV and SRV tables
	for (uint8_t i = 0; i < FrameCount; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		const Descriptor descriptors[] =
		{
			m_cbPerFrame->GetCBV(i),
			m_matrices->GetSRV(i)
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

	// Create SRV tables
	m_srvMipTables.resize(g_numCubeMips);
	for (uint8_t i = 0; i < g_numCubeMips; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		const Descriptor descriptors[] =
		{
			m_cubeMaps[0]->GetSRV(i),
			m_cubeDepths[0]->GetSRV(i)
		};
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvMipTables[i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
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
	const auto samplerLinearClamp = SamplerPreset::LINEAR_CLAMP;
	descriptorTable->SetSamplers(0, 1, &samplerLinearClamp, m_descriptorTableCache.get());
	X_RETURN(m_samplerTable, descriptorTable->GetSamplerTable(m_descriptorTableCache.get()), false);

	return true;
}

void RayCaster::cullVolumes(const CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barrier
	ResourceBarrier barriers[3];
	auto numBarriers = m_visibleVolumeCounter->SetBarrier(barriers, ResourceState::COPY_DEST);
	pCommandList->Barrier(numBarriers, barriers);

	// Reset counter
	pCommandList->CopyResource(m_visibleVolumeCounter.get(), m_counterReset.get());

	// Set barriers
	numBarriers = m_visibleVolumes->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS);
	numBarriers = m_visibleVolumeCounter->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS, numBarriers);
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

void RayCaster::rayMarchV(const CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barriers
	vector<ResourceBarrier> barriers(m_cubeMaps.size() + m_cubeDepths.size() + 2);
	auto numBarriers = m_lightMap->SetBarrier(barriers.data(), ResourceState::NON_PIXEL_SHADER_RESOURCE);
	numBarriers = m_pDepths[DEPTH_MAP]->SetBarrier(barriers.data(), ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers);
	for (auto& cubeMap : m_cubeMaps)
		numBarriers = cubeMap->SetBarrier(barriers.data(), ResourceState::UNORDERED_ACCESS, numBarriers);
	for (auto& cubeDepth : m_cubeDepths)
		numBarriers = cubeDepth->SetBarrier(barriers.data(), ResourceState::UNORDERED_ACCESS, numBarriers);
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
	pCommandList->Dispatch(DIV_UP(m_gridSize, 8), DIV_UP(m_gridSize, 4), 1);
}

void RayCaster::renderCube(const CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barriers
	ResourceBarrier barriers[2];
	auto numBarriers = m_cubeMaps[0]->SetBarrier(barriers, ResourceState::PIXEL_SHADER_RESOURCE);
	numBarriers = m_cubeDepths[0]->SetBarrier(barriers, ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
	pCommandList->Barrier(numBarriers, barriers);

	// Set pipeline state
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[RENDER_CUBE]);
	pCommandList->SetPipelineState(m_pipelines[RENDER_CUBE]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLESTRIP);

	// Set descriptor tables
	pCommandList->SetGraphicsDescriptorTable(0, m_cbvTables[frameIndex]);
	pCommandList->SetGraphicsDescriptorTable(1, m_srvMipTables[0]);
	pCommandList->SetGraphicsDescriptorTable(2, m_srvTables[SRV_TABLE_DEPTH]);
	pCommandList->SetGraphicsDescriptorTable(3, m_samplerTable);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLESTRIP);
	pCommandList->Draw(4, 6, 0, 0);
}
