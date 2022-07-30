//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SharedConsts.h"
#include "MultiRayCaster.h"
#define _INDEPENDENT_DDS_LOADER_
#include "Advanced/XUSGDDSLoader.h"
#undef _INDEPENDENT_DDS_LOADER_
#include <array>

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
	XMFLOAT4X4 ScreenToWorld;
	XMFLOAT3X4 LightMapWorld;
	XMFLOAT4X4 ShadowViewProj;
	XMFLOAT4 LightPos;
	XMFLOAT4 LightColor;
	XMFLOAT4 Ambient;
};

struct PerObject
{
	XMFLOAT4X4 WorldViewProj;
	XMFLOAT4X4 WorldViewProjI;
	XMFLOAT3X4 WorldI;
	XMFLOAT3X4 World;
	XMFLOAT3X4 LocalToLight;
};

struct VolumeDesc
{
	uint16_t VolTexId;
	uint16_t NumMips;
	float CubeMapSize;
};

struct VolumeInfo
{
	uint16_t MipLevel;
	uint16_t SmpCount;
	uint16_t FaceMask;
	uint16_t VolTexId;
};

const uint8_t g_numCubeMips = NUM_CUBE_MIP;

MultiRayCaster::MultiRayCaster() :
	m_pDepths(nullptr),
	m_coeffSH(nullptr),
	m_instances(),
	m_maxRaySamples(256),
	m_maxLightSamples(128),
	m_lightPt(75.0f, 75.0f, -75.0f),
	m_lightColor(1.0f, 0.7f, 0.3f, 1.0f),
	m_ambient(0.0f, 0.3f, 1.0f, 0.4f),
	m_rtSupport(0)
{
	m_shaderPool = ShaderPool::MakeUnique();

	XMStoreFloat3x4(&m_lightMapWorld, XMMatrixScaling(10.0f, 10.0f, 10.0f));

	AccelerationStructure::SetUAVCount(2);
}

MultiRayCaster::~MultiRayCaster()
{
}

bool MultiRayCaster::Init(RayTracing::CommandList* pCommandList, const DescriptorTableCache::sptr& descriptorTableCache,
	Format rtFormat, Format dsFormat, uint32_t gridSize, uint32_t lightGridSize, uint32_t numVolumes, uint32_t numVolumeSrcs,
	vector<Resource::uptr>& uploaders, RayTracing::GeometryBuffer* pGeometry, uint8_t rtSupport)
{
	const auto pDevice = pCommandList->GetRTDevice();
	m_rayTracingPipelineCache = RayTracing::PipelineCache::MakeUnique(pDevice);
	m_graphicsPipelineCache = Graphics::PipelineCache::MakeUnique(pDevice);
	m_computePipelineCache = Compute::PipelineCache::MakeUnique(pDevice);
	m_pipelineLayoutCache = PipelineLayoutCache::MakeUnique(pDevice);
	m_descriptorTableCache = descriptorTableCache;
	m_rtSupport = rtSupport;

	m_gridSize = gridSize;
	m_lightGridSize = lightGridSize;

	// Create resources
	XUSG_N_RETURN(createVolumeInfoBuffers(pCommandList, numVolumes, numVolumeSrcs, uploaders), false);

	m_volumes.resize(numVolumeSrcs);
	for (auto i = 0u; i < numVolumeSrcs; ++i)
	{
		m_volumes[i] = Texture3D::MakeUnique();
		XUSG_N_RETURN(m_volumes[i]->Create(pDevice, gridSize, gridSize, gridSize, Format::R16G16B16A16_FLOAT,
			ResourceFlag::ALLOW_UNORDERED_ACCESS | ResourceFlag::ALLOW_SIMULTANEOUS_ACCESS, 1,
			MemoryFlag::NONE, (L"Volume" + to_wstring(i)).c_str()), false);
	}

	m_cubeMaps.resize(numVolumes);
	m_cubeDepths.resize(numVolumes);
	for (auto i = 0u; i < numVolumes; ++i)
	{
		m_cubeMaps[i] = Texture2D::MakeUnique();
		XUSG_N_RETURN(m_cubeMaps[i]->Create(pDevice, gridSize, gridSize, Format::R16G16B16A16_FLOAT, 6,
			ResourceFlag::ALLOW_UNORDERED_ACCESS, g_numCubeMips, 1, true, MemoryFlag::NONE,
			(L"RadianceCubeMap" + to_wstring(i)).c_str()), false);

		m_cubeDepths[i] = Texture2D::MakeUnique();
		XUSG_N_RETURN(m_cubeDepths[i]->Create(pDevice, gridSize, gridSize, Format::R32_FLOAT, 6,
			ResourceFlag::ALLOW_UNORDERED_ACCESS, g_numCubeMips, 1, true, MemoryFlag::NONE,
			(L"DepthCubeMap" + to_wstring(i)).c_str()), false);
	}

	m_lightMap = Texture3D::MakeUnique();
	XUSG_N_RETURN(m_lightMap->Create(pDevice, m_lightGridSize, m_lightGridSize, m_lightGridSize,
		Format::R11G11B10_FLOAT,ResourceFlag::ALLOW_UNORDERED_ACCESS | ResourceFlag::ALLOW_SIMULTANEOUS_ACCESS,
		1, MemoryFlag::NONE, L"LightMap"), false);

	m_cbPerFrame = ConstantBuffer::MakeUnique();
	XUSG_N_RETURN(m_cbPerFrame->Create(pDevice, sizeof(CBPerFrame[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, MemoryFlag::NONE, L"RayCaster.CBPerFrame"), false);

	/*m_cbPerObject = ConstantBuffer::MakeUnique();
	XUSG_N_RETURN(m_cbPerObject->Create(pDevice, sizeof(CBPerObject[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, L"RayCaster.CBPerObject"), false);*/

	// Create pipelines
	XUSG_N_RETURN(createPipelineLayouts(pDevice), false);
	XUSG_N_RETURN(createPipelines(rtFormat, dsFormat), false);

	// create command layout
	XUSG_N_RETURN(createCommandLayouts(pDevice), false);

	XUSG_N_RETURN(createCubeVB(pCommandList, uploaders), false);
	XUSG_N_RETURN(createCubeIB(pCommandList, uploaders), false);

	// Set world transforms
	m_volumeWorlds.resize(numVolumes);
	SetVolumesWorld(20.0f, XMFLOAT3(0.0f, 0.0f, 0.0f));

	// Build acceleration structures
	if (m_rtSupport)
	{
		XUSG_N_RETURN(buildAccelerationStructures(pCommandList, pGeometry), false);
		//XUSG_N_RETURN(createDescriptorTables(nullptr), false); // included in buildAccelerationStructures()
		XUSG_N_RETURN(buildShaderTables(pDevice), false);
	}
	else XUSG_N_RETURN(createDescriptorTables(nullptr), false);

	return true;
}

bool MultiRayCaster::LoadVolumeData(XUSG::CommandList* pCommandList, uint32_t i, const wchar_t* fileName, vector<Resource::uptr>& uploaders)
{
	// Load input image
	{
		DDS::Loader textureLoader;
		DDS::AlphaMode alphaMode;

		if (i >= m_fileSrcs.size()) m_fileSrcs.resize(i + 1);
		uploaders.emplace_back(Resource::MakeUnique());
		XUSG_N_RETURN(textureLoader.CreateTextureFromFile(pCommandList, fileName,
			8192, false, m_fileSrcs[i], uploaders.back().get(), &alphaMode), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_fileSrcs[i]->GetSRV());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_FILE_SRC], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
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
	pCommandList->Dispatch(XUSG_DIV_UP(m_gridSize, 4), XUSG_DIV_UP(m_gridSize, 4), XUSG_DIV_UP(m_gridSize, 4));

	return true;
}

bool MultiRayCaster::SetRenderTargets(const XUSG::Device* pDevice, const RenderTarget* pColorOut, const DepthStencil::uptr* depths)
{
	m_pDepths = depths;

	const auto width = static_cast<uint32_t>(depths[DEPTH_MAP]->GetWidth());
	const auto height = depths[DEPTH_MAP]->GetHeight();

	m_depth = DepthStencil::MakeUnique();
	XUSG_N_RETURN(m_depth->Create(pDevice, width, height, Format::D32_FLOAT, ResourceFlag::NONE,
		1, 1, 1, 1.0f, 0, false, MemoryFlag::NONE, L"DepthIncCubes"), false);

	return SetViewport(pDevice, width, height, pColorOut);
}

bool MultiRayCaster::SetViewport(const XUSG::Device* pDevice, uint32_t width, uint32_t height, const Texture* pColorOut)
{
	m_viewport.x = width;
	m_viewport.y = height;

	m_kDepths = Texture2D::MakeUnique();
	XUSG_N_RETURN(m_kDepths->Create(pDevice, width, height, Format::R32_UINT, NUM_OIT_LAYERS,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, false, MemoryFlag::NONE, L"ColorKBuffer"), false);

	m_kColors = Texture2D::MakeUnique();
	XUSG_N_RETURN(m_kColors->Create(pDevice, width, height, Format::R16G16B16A16_FLOAT, NUM_OIT_LAYERS,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, false, MemoryFlag::NONE, L"ColorKBuffer"), false);

	XUSG_N_RETURN(createDescriptorTables(pColorOut),false);

	return true;
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
	pCommandList->Dispatch(XUSG_DIV_UP(m_gridSize, 4), XUSG_DIV_UP(m_gridSize, 4), XUSG_DIV_UP(m_gridSize, 4));
}

void MultiRayCaster::SetSH(const StructuredBuffer::sptr& coeffSH)
{
	m_coeffSH = coeffSH;
}

void MultiRayCaster::SetMaxSamples(uint32_t maxRaySamples, uint32_t maxLightSamples)
{
	m_maxRaySamples = maxRaySamples;
	m_maxLightSamples = maxLightSamples;
}

void MultiRayCaster::SetVolumesWorld(float size, const XMFLOAT3& center)
{
	const auto numVolumes = static_cast<uint32_t>(m_cubeMaps.size());
	const auto rowLength = static_cast<uint32_t>(ceilf(sqrtf(static_cast<float>(numVolumes))));
	const auto colLength = static_cast<uint32_t>(ceilf(static_cast<float>(numVolumes / rowLength)));

	auto pos = center;
	pos.z -= (colLength / 2.0f - 0.5f) * size * 1.5f;
	for (auto m = 0u; m < colLength; ++m)
	{
		pos.x = center.x - (rowLength / 2.0f - 0.5f) * size * 1.5f;
		for (auto n = 0u; n < rowLength; ++n)
		{
			SetVolumeWorld(rowLength * m + n, size, pos);
			pos.x += size * 1.5f;
		}
		pos.z += size * 1.5f;
	}
}

void MultiRayCaster::SetVolumeWorld(uint32_t i, float size, const XMFLOAT3& pos)
{
	size *= 0.5f;
	auto world = XMMatrixScaling(size, size, size);
	world = world * XMMatrixTranslation(pos.x, pos.y, pos.z);
	XMStoreFloat3x4(&m_volumeWorlds[i], world);
}

void MultiRayCaster::SetLightMapWorld(float size, const XMFLOAT3& pos)
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

void MultiRayCaster::UpdateFrame(uint8_t frameIndex, CXMMATRIX viewProj,
	const XMFLOAT4X4& shadowVP, const XMFLOAT3& eyePt)
{
	const auto& depth = m_pDepths[DEPTH_MAP];
	const auto width = static_cast<float>(depth->GetWidth());
	const auto height = static_cast<float>(depth->GetHeight());

	// Per-frame
	{
		const auto projToWorld = XMMatrixInverse(nullptr, viewProj);
		const auto pCbData = reinterpret_cast<CBPerFrame*>(m_cbPerFrame->Map(frameIndex));
		pCbData->EyePos = XMFLOAT4(eyePt.x, eyePt.y, eyePt.z, 1.0f);
		pCbData->Viewport = XMFLOAT4(width, height, 0.0f, 0.0f);
		pCbData->LightMapWorld = m_lightMapWorld;
		pCbData->ShadowViewProj = shadowVP;
		pCbData->LightPos = XMFLOAT4(m_lightPt.x, m_lightPt.y, m_lightPt.z, 1.0f);
		pCbData->LightColor = m_lightColor;
		pCbData->Ambient = m_ambient;
		XMStoreFloat4x4(&pCbData->ScreenToWorld, XMMatrixTranspose(projToWorld));
	}

	// Per-object
	{
		const auto numVolumes = static_cast<uint32_t>(m_cubeMaps.size());
		const auto lightWorld = XMLoadFloat3x4(&m_lightMapWorld);
		const auto lightWorldI = XMMatrixInverse(nullptr, lightWorld);

		const auto pMappedData = reinterpret_cast<PerObject*>(m_perObject->Map(frameIndex));
		for (auto i = 0u; i < numVolumes; ++i)
		{
			const auto world = XMLoadFloat3x4(&m_volumeWorlds[i]);
			const auto worldI = XMMatrixInverse(nullptr, world);
			const auto worldViewProj = world * viewProj;
			const auto localToLight = world * lightWorldI;

			XMStoreFloat4x4(&pMappedData[i].WorldViewProj, XMMatrixTranspose(worldViewProj));
			XMStoreFloat4x4(&pMappedData[i].WorldViewProjI, XMMatrixTranspose(XMMatrixInverse(nullptr, worldViewProj)));
			XMStoreFloat3x4(&pMappedData[i].WorldI, worldI);
			XMStoreFloat3x4(&pMappedData[i].World, world);
			XMStoreFloat3x4(&pMappedData[i].LocalToLight, localToLight);
		}
	}
}

void MultiRayCaster::Render(RayTracing::CommandList* pCommandList, uint8_t frameIndex,
	RenderTarget* pColorOut, bool updateLight, OITMethod oitMethod)
{
	if (updateLight) RayMarchL(pCommandList, frameIndex);
	cullVolumes(pCommandList, frameIndex);
	rayMarchV(pCommandList, frameIndex);

	switch (oitMethod)
	{
	case OIT_RAY_TRACING:
		traceCube(pCommandList, frameIndex, pColorOut);
		break;
	case OIT_RAY_QUERY:
		renderDepth(pCommandList, frameIndex);
		renderCubeRT(pCommandList, frameIndex, pColorOut);
		break;
	default:
		cubeDepthPeel(pCommandList, frameIndex);
		renderCube(pCommandList, frameIndex);
		resolveOIT(pCommandList, frameIndex);
	}
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
	pCommandList->SetCompute32BitConstant(6, m_maxLightSamples);
	pCommandList->SetCompute32BitConstant(6, m_coeffSH ? 1 : 0, 1);
	if (m_coeffSH) pCommandList->SetComputeRootShaderResourceView(7, m_coeffSH.get());

	// Dispatch grid
	pCommandList->Dispatch(XUSG_DIV_UP(m_lightGridSize, 4), XUSG_DIV_UP(m_lightGridSize, 4), XUSG_DIV_UP(m_lightGridSize, 4));
}

const DescriptorTable& MultiRayCaster::GetLightSRVTable() const
{
	return m_srvTables[SRV_TABLE_LIGHT_MAP];
}

Resource* MultiRayCaster::GetLightMap() const
{
	return m_lightMap.get();
}

bool MultiRayCaster::createCubeVB(XUSG::CommandList* pCommandList, vector<Resource::uptr>& uploaders)
{
	static const auto CubeVertices = []()
	{
		static const XMFLOAT3X3 planes[] =
		{
			// back plane
			XMFLOAT3X3(-1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f),
			// left plane
			XMFLOAT3X3(0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 0.0f),
			// front plane
			XMFLOAT3X3(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f),
			// right plane
			XMFLOAT3X3(0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f),
			// top plane
			XMFLOAT3X3(-1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f),
			// bottom plane
			XMFLOAT3X3(-1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f)
		};

		const uint8_t cubeVertexCount = 24;
		array<XMFLOAT3, cubeVertexCount> cubeVertices;
		for (uint8_t i = 0; i < cubeVertexCount; ++i)
		{
			const uint8_t faceId = i / 4;
			const uint8_t vertId = i % 4;
			const XMFLOAT2 uv(1.0f * (vertId & 1), 1.0f * (vertId >> 1));
			const XMFLOAT2 pos2D(2.0f * uv.x - 1.0f, 2.0f * uv.y - 1.0f);
			const XMFLOAT3 pos(pos2D.x, -pos2D.y, 1.0);
			const auto result = XMVector3TransformNormal(XMLoadFloat3(&pos), XMLoadFloat3x3(&planes[faceId]));
			XMStoreFloat3(&cubeVertices[i], result);
		}

		return cubeVertices;
	};
	static const auto vertices = CubeVertices();

	m_vertexBuffer = VertexBuffer::MakeUnique();
	XUSG_N_RETURN(m_vertexBuffer->Create(pCommandList->GetDevice(), static_cast<uint32_t>(vertices.size()), 
		static_cast<uint32_t>(sizeof(XMFLOAT3)), ResourceFlag::NONE, MemoryType::DEFAULT, 1,
		nullptr, 0, nullptr, 0, nullptr, MemoryFlag::NONE, L"CubeVB"), false);
	uploaders.emplace_back(Resource::MakeUnique());

	return m_vertexBuffer->Upload(pCommandList, uploaders.back().get(), vertices.data(),
		vertices.size() * sizeof(XMFLOAT3), 0, ResourceState::NON_PIXEL_SHADER_RESOURCE);
}

bool MultiRayCaster::createCubeIB(XUSG::CommandList* pCommandList, vector<Resource::uptr>& uploaders)
{
	static const uint16_t indices[] =
	{
		0, 1, 2, 3, 2, 1,
		4, 5, 6, 7, 6, 5,
		8, 9, 10, 11, 10, 9,
		12, 13, 14, 15, 14, 13,
		16, 17, 18, 19, 18, 17,
		20, 21, 22, 23, 22, 21
	};

	m_indexBuffer = IndexBuffer::MakeUnique();
	XUSG_N_RETURN(m_indexBuffer->Create(pCommandList->GetDevice(), sizeof(indices), Format::R16_UINT, ResourceFlag::NONE,
		MemoryType::DEFAULT, 1, nullptr, 1, nullptr, 1, nullptr, MemoryFlag::NONE, L"CubeIB"), false);
	uploaders.emplace_back(Resource::MakeUnique());

	return m_indexBuffer->Upload(pCommandList, uploaders.back().get(), indices,
		sizeof(indices), 0, ResourceState::NON_PIXEL_SHADER_RESOURCE | ResourceState::INDEX_BUFFER);
}

bool MultiRayCaster::createVolumeInfoBuffers(XUSG::CommandList* pCommandList, uint32_t numVolumes,
	uint32_t numVolumeSrcs, vector<Resource::uptr>& uploaders)
{
	const auto pDevice = pCommandList->GetDevice();

	{
		uint32_t firstSRVElements[FrameCount];
		for (uint8_t i = 0; i < FrameCount; ++i) firstSRVElements[i] = numVolumes * i;

		m_perObject = StructuredBuffer::MakeUnique();
		XUSG_N_RETURN(m_perObject->Create(pDevice, numVolumes * FrameCount,
			sizeof(PerObject), ResourceFlag::NONE, MemoryType::UPLOAD, FrameCount,
			firstSRVElements, 0, nullptr, MemoryFlag::NONE, L"RayCaster.Matrices"), false);
	}

	{
		vector<VolumeDesc> volumeDescs(numVolumes);
		for (auto i = 0u; i < volumeDescs.size(); ++i)
		{
			auto& volume = volumeDescs[i];
			//volume.VolTexId = static_cast<uint16_t>(rand() % numVolumeSrcs);
			volume.VolTexId = static_cast<uint16_t>(i % numVolumeSrcs);
			volume.NumMips = g_numCubeMips;
			volume.CubeMapSize = static_cast<float>(m_gridSize);
		}

		m_volumeDescs = StructuredBuffer::MakeUnique();
		XUSG_N_RETURN(m_volumeDescs->Create(pDevice, numVolumes,
			sizeof(VolumeDesc), ResourceFlag::NONE, MemoryType::DEFAULT, 1,
			nullptr, 1, nullptr, MemoryFlag::NONE, L"RayCaster.VolumeDescs"), false);

		uploaders.emplace_back(Resource::MakeUnique());
		m_volumeDescs->Upload(pCommandList, uploaders.back().get(),
			volumeDescs.data(), sizeof(VolumeDesc) * volumeDescs.size());
	}

	{
		const auto conterOffset = sizeof(uint32_t[2]);
		m_visibleVolumeCounter = RawBuffer::MakeShared();
		XUSG_N_RETURN(m_visibleVolumeCounter->Create(pDevice, sizeof(uint32_t),
			ResourceFlag::ALLOW_UNORDERED_ACCESS | ResourceFlag::DENY_SHADER_RESOURCE,
			MemoryType::DEFAULT, 0, nullptr, 0, nullptr, MemoryFlag::NONE,
			L"RayCaster.VisibleVolumeCounter"), false);

		m_visibleVolumes = StructuredBuffer::MakeUnique();
		m_visibleVolumes->SetCounter(m_visibleVolumeCounter);
		XUSG_N_RETURN(m_visibleVolumes->Create(pDevice, numVolumes, sizeof(uint32_t),
			ResourceFlag::ALLOW_UNORDERED_ACCESS, MemoryType::DEFAULT, 1, nullptr,
			1, nullptr, MemoryFlag::NONE, L"RayCaster.VisibleVolumes"), false);

		m_counterReset = StructuredBuffer::MakeUnique();
		XUSG_N_RETURN(m_counterReset->Create(pDevice, 1, sizeof(uint32_t),
			ResourceFlag::DENY_SHADER_RESOURCE, MemoryType::DEFAULT, 0, nullptr,
			0, nullptr, MemoryFlag::NONE, L"RayCaster.CounterReset"), false);

		m_volumeAttribs = TypedBuffer::MakeUnique();
		XUSG_N_RETURN(m_volumeAttribs->Create(pDevice, numVolumes, sizeof(VolumeInfo),
			Format::R16G16B16A16_UINT, ResourceFlag::ALLOW_UNORDERED_ACCESS, MemoryType::DEFAULT,
			1, nullptr, 1, nullptr, MemoryFlag::NONE, L"RayCaster.VolumeAttributes"), false);

		m_volumeDispatchArg = RawBuffer::MakeUnique();
		XUSG_N_RETURN(m_volumeDispatchArg->Create(pDevice, sizeof(uint32_t[3]),
			ResourceFlag::DENY_SHADER_RESOURCE, MemoryType::DEFAULT, 0, nullptr,
			0, nullptr, MemoryFlag::NONE, L"RayCaster.VisibleVolumeDispatchArg"), false);

		const uint32_t pDispatchReset[] = { XUSG_DIV_UP(m_gridSize, 8), XUSG_DIV_UP(m_gridSize, 4), 0 };
		uploaders.emplace_back(Resource::MakeUnique());
		XUSG_N_RETURN(m_volumeDispatchArg->Upload(pCommandList, uploaders.back().get(), pDispatchReset, sizeof(uint32_t[3])), false);

		m_volumeDrawArg = RawBuffer::MakeUnique();
		XUSG_N_RETURN(m_volumeDrawArg->Create(pDevice, sizeof(uint32_t[5]),
			ResourceFlag::DENY_SHADER_RESOURCE, MemoryType::DEFAULT, 0, nullptr,
			0, nullptr, MemoryFlag::NONE, L"RayCaster.VisibleVolumeDrawArg"), false);

		const uint32_t pDrawReset[] = { 36, 0, 0, 0, 0 };
		uploaders.emplace_back(Resource::MakeUnique());
		XUSG_N_RETURN(m_volumeDrawArg->Upload(pCommandList, uploaders.back().get(), pDrawReset, sizeof(uint32_t[5])), false);

		const auto clear = 0u;
		uploaders.emplace_back(Resource::MakeUnique());
		XUSG_N_RETURN(m_counterReset->Upload(pCommandList, uploaders.back().get(), &clear, sizeof(uint32_t)), false);
	}

	return true;
}

bool MultiRayCaster::createPipelineLayouts(const XUSG::Device* pDevice)
{
	const auto numVolumes = static_cast<uint32_t>(m_cubeMaps.size());
	const auto numVolumeSrcs = static_cast<uint32_t>(m_volumes.size());

	// Load grid data
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(2, DescriptorType::SAMPLER, 1, 0);
		XUSG_X_RETURN(m_pipelineLayouts[LOAD_VOLUME_DATA], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"LoadGridDataLayout"), false);
	}

	// Init grid data
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		XUSG_X_RETURN(m_pipelineLayouts[INIT_VOLUME_DATA], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
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
		XUSG_X_RETURN(m_pipelineLayouts[VOLUME_CULL], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
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
		pipelineLayout->SetConstants(6, 2, 1);
		pipelineLayout->SetRootSRV(7, 1, 2);
		XUSG_X_RETURN(m_pipelineLayouts[RAY_MARCH_L], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"LightSpaceRayMarchingLayout"), false);
	}

	// View space ray marching
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 3, 1, 0);
		pipelineLayout->SetRange(2, DescriptorType::UAV, g_numCubeMips * numVolumes, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(3, DescriptorType::UAV, g_numCubeMips * numVolumes, 0, 1, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(4, DescriptorType::SRV, numVolumeSrcs, 0, 1);
		pipelineLayout->SetRange(5, DescriptorType::SRV, 1, 0, 2);
		pipelineLayout->SetRange(6, DescriptorType::SAMPLER, 2, 0);
		XUSG_X_RETURN(m_pipelineLayouts[RAY_MARCH_V], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"ViewSpaceRayMarchingLayout"), false);
	}

	// Cube depth peeling
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 2, 1, 0);
		pipelineLayout->SetRange(2, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetShaderStage(0, Shader::Stage::VS);
		pipelineLayout->SetShaderStage(1, Shader::Stage::VS);
		pipelineLayout->SetShaderStage(2, Shader::Stage::PS);
		XUSG_X_RETURN(m_pipelineLayouts[CUBE_DEPTH_PEEL], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"CubeDepthPeelingLayout"), false);
	}

	// Depth prepass
	if (m_rtSupport & RT_INLINE)
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 2, 1, 0);
		pipelineLayout->SetShaderStage(0, Shader::Stage::VS);
		pipelineLayout->SetShaderStage(1, Shader::Stage::VS);
		XUSG_X_RETURN(m_pipelineLayouts[DEPTH_PASS], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"DepthPrepassLayout"), false);
	}

	// Cube rendering
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 2, 1, 0);
		pipelineLayout->SetRange(2, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE); // g_rwKColors
		pipelineLayout->SetRange(3, DescriptorType::SRV, 1, 1, 0); // g_txKDepths
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
		XUSG_X_RETURN(m_pipelineLayouts[RENDER_CUBE], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"CubeRenderingLayout"), false);
	}

	// Cube rendering RT
	if (m_rtSupport & RT_INLINE)
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::SRV, 2, 1, 0);
		pipelineLayout->SetRootSRV(2, 1, 0, DescriptorFlag::DATA_STATIC, Shader::Stage::PS);
		pipelineLayout->SetRange(3, DescriptorType::SRV, 1, 2, 0);
		pipelineLayout->SetRange(4, DescriptorType::SRV, g_numCubeMips* numVolumes, 0, 1);
		pipelineLayout->SetRange(5, DescriptorType::SRV, g_numCubeMips* numVolumes, 0, 2);
		pipelineLayout->SetRange(6, DescriptorType::SRV, 1, 0, 3);
		pipelineLayout->SetRange(7, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout->SetShaderStage(1, Shader::Stage::VS);
		pipelineLayout->SetShaderStage(3, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(4, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(5, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(6, Shader::Stage::PS);
		pipelineLayout->SetShaderStage(7, Shader::Stage::PS);
		XUSG_X_RETURN(m_pipelineLayouts[RENDER_CUBE_RT], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"CubeRenderingRTLayout"), false);
	}

	// Resolve OIT
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetShaderStage(0, Shader::Stage::PS);
		XUSG_X_RETURN(m_pipelineLayouts[RESOLVE_OIT], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"ResolveOITLayout"), false);
	}

	// Ray Tracing
	if (m_rtSupport & RT_PIPELINE)
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRootSRV(0, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(1, DescriptorType::CBV, 1, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(2, DescriptorType::SRV, 1, 1);
		pipelineLayout->SetRange(3, DescriptorType::UAV, 1, 0);
		pipelineLayout->SetRange(4, DescriptorType::SRV, g_numCubeMips * numVolumes, 0, 1);
		pipelineLayout->SetRange(5, DescriptorType::SRV, g_numCubeMips * numVolumes, 0, 2);
		pipelineLayout->SetRange(6, DescriptorType::SRV, 1, 0, 3);
		pipelineLayout->SetRange(7, DescriptorType::SAMPLER, 1, 0);
		XUSG_X_RETURN(m_pipelineLayouts[RAY_TRACING], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"RayTracingLayout"), false);
	}

	return true;
}

bool MultiRayCaster::createPipelines(Format rtFormat, Format dsFormat)
{
	auto vsIndex = 0u;
	auto psIndex = 0u;
	auto csIndex = 0u;

	// Load grid data
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSR32FToRGBA16F.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[LOAD_VOLUME_DATA]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[LOAD_VOLUME_DATA], state->GetPipeline(m_computePipelineCache.get(), L"InitGridData"), false);
	}

	// Init grid data
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSInitGridData.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[INIT_VOLUME_DATA]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[INIT_VOLUME_DATA], state->GetPipeline(m_computePipelineCache.get(), L"InitGridData"), false);
	}

	// Volume culling
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSVolumeCull.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[VOLUME_CULL]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[VOLUME_CULL], state->GetPipeline(m_computePipelineCache.get(), L"VolumeCulling"), false);
	}

	// Light space ray marching
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSRayMarchL.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RAY_MARCH_L]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[RAY_MARCH_L], state->GetPipeline(m_computePipelineCache.get(), L"LightSpaceRayMarching"), false);
	}

	// View space ray marching
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSRayMarchV.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RAY_MARCH_V]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[RAY_MARCH_V], state->GetPipeline(m_computePipelineCache.get(), L"ViewSpaceRayMarching"), false);
	}

	XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSCubeDP.cso"), false);

	// Cube depth peeling
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSDepthPeel.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[CUBE_DEPTH_PEEL]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex++));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->RSSetState(Graphics::CULL_FRONT, m_graphicsPipelineCache.get()); // Front-face culling for interior surfaces
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		XUSG_X_RETURN(m_pipelines[CUBE_DEPTH_PEEL], state->GetPipeline(m_graphicsPipelineCache.get(), L"CubeDepthPeeling"), false);
	}

	// Depth prepass
	if (m_rtSupport & RT_INLINE)
	{
		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[DEPTH_PASS]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex - 1));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->RSSetState(Graphics::CULL_FRONT, m_graphicsPipelineCache.get()); // Front-face culling for interior surfaces
		state->DSSetState(Graphics::DEFAULT_LESS, m_graphicsPipelineCache.get());
		state->OMSetDSVFormat(dsFormat);
		XUSG_X_RETURN(m_pipelines[DEPTH_PASS], state->GetPipeline(m_graphicsPipelineCache.get(), L"DepthPrepass"), false);
	}

	XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSCube.cso"), false);

	// Cube rendering
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSCube.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RENDER_CUBE]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex++));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->RSSetState(Graphics::CULL_FRONT, m_graphicsPipelineCache.get()); // Front-face culling for interior surfaces
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		XUSG_X_RETURN(m_pipelines[RENDER_CUBE], state->GetPipeline(m_graphicsPipelineCache.get(), L"CubeRendering"), false);
	}

	// Cube rendering RT
	if (m_rtSupport & RT_INLINE)
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSCubeRT.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RENDER_CUBE_RT]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex - 1));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->RSSetState(Graphics::CULL_FRONT, m_graphicsPipelineCache.get()); // Front-face culling for interior surfaces
		state->DSSetState(Graphics::DEPTH_READ_EQUAL, m_graphicsPipelineCache.get());
		state->OMSetBlendState(Graphics::PREMULTIPLITED, m_graphicsPipelineCache.get());
		state->OMSetRTVFormats(&rtFormat, 1);
		state->OMSetDSVFormat(dsFormat);
		XUSG_X_RETURN(m_pipelines[RENDER_CUBE_RT], state->GetPipeline(m_graphicsPipelineCache.get(), L"CubeRenderingRT"), false);
	}

	// Resolve OIT
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSResolveOIT.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RESOLVE_OIT]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex++));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		state->OMSetBlendState(Graphics::PREMULTIPLITED, m_graphicsPipelineCache.get());
		state->OMSetRTVFormats(&rtFormat, 1);
		XUSG_X_RETURN(m_pipelines[RESOLVE_OIT], state->GetPipeline(m_graphicsPipelineCache.get(), L"ResolveOIT"), false);
	}

	// Ray Tracing
	if (m_rtSupport & RT_PIPELINE)
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"RTCube.cso"), false);

		const auto state = RayTracing::State::MakeUnique();
		state->SetShaderLibrary(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		state->SetHitGroup(0, HitGroupName, ClosestHitShaderName);
		state->SetShaderConfig(sizeof(float[5]), sizeof(float[2]));
		state->SetGlobalPipelineLayout(m_pipelineLayouts[RAY_TRACING]);
		state->SetMaxRecursionDepth(1);
		XUSG_X_RETURN(m_pipelines[RAY_TRACING], state->GetPipeline(m_rayTracingPipelineCache.get(), L"Raytracing"), false);
	}

	return true;
}

bool MultiRayCaster::createCommandLayouts(const XUSG::Device* pDevice)
{
	{
		IndirectArgument arg;
		arg.Type = IndirectArgumentType::DISPATCH;
		m_commandLayouts[DISPATCH_LAYOUT] = CommandLayout::MakeUnique();
		XUSG_N_RETURN(m_commandLayouts[DISPATCH_LAYOUT]->Create(pDevice, sizeof(uint32_t[3]), 1, &arg), false);
	}

	{
		IndirectArgument arg;
		arg.Type = IndirectArgumentType::DRAW_INDEXED;
		m_commandLayouts[DRAW_LAYOUT] = CommandLayout::MakeUnique();
		XUSG_N_RETURN(m_commandLayouts[DRAW_LAYOUT]->Create(pDevice, sizeof(uint32_t[5]), 1, &arg), false);
	}

	return true;
}

bool MultiRayCaster::createDescriptorTables(const Texture* pColorOut)
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
		XUSG_X_RETURN(m_cbvSrvTables[i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Create UAV tables
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		vector<Descriptor> descriptors(g_numCubeMips * numVolumes);
		for (auto i = 0u; i < numVolumes; ++i)
			for (uint8_t j = 0; j < g_numCubeMips; ++j)
				descriptors[g_numCubeMips * i + j] = m_cubeMaps[i]->GetUAV(j);
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(descriptors.size()), descriptors.data());
		XUSG_X_RETURN(m_uavTables[UAV_TABLE_CUBE_MAP], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	for (uint8_t i = 0; i < g_numCubeMips; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		vector<Descriptor> descriptors(g_numCubeMips * numVolumes);
		for (auto i = 0u; i < numVolumes; ++i)
			for (uint8_t j = 0; j < g_numCubeMips; ++j)
				descriptors[g_numCubeMips * i + j] = m_cubeDepths[i]->GetUAV(j);
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(descriptors.size()), descriptors.data());
		XUSG_X_RETURN(m_uavTables[UAV_TABLE_CUBE_DEPTH], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_lightMap->GetUAV());
		XUSG_X_RETURN(m_uavTables[UAV_TABLE_LIGHT_MAP], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	if (m_kDepths)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_kDepths->GetUAV());
		XUSG_X_RETURN(m_uavTables[UAV_TABLE_K_DEPTHS], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	if (m_kColors)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_kColors->GetUAV());
		XUSG_X_RETURN(m_uavTables[UAV_TABLE_K_COLORS], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Create SRV tables
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_volumeDescs->GetSRV());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_VOLUME_DESCS], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		vector<Descriptor> descriptors(numVolumeSrcs);
		for (auto i = 0u; i < numVolumeSrcs; ++i) descriptors[i] = m_volumes[i]->GetSRV();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(descriptors.size()), descriptors.data());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_VOLUME], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_visibleVolumes->GetSRV());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_VIS_VOLUMES], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_volumeAttribs->GetSRV());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_VOLUME_ATTRIBS], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_lightMap->GetSRV());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_LIGHT_MAP], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	if (m_kDepths)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_kDepths->GetSRV());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_K_DEPTHS], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	if (m_kColors)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_kColors->GetSRV());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_K_COLORS], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	if (m_pDepths)
	{
		if (m_pDepths[DEPTH_MAP])
		{
			const auto descriptorTable = Util::DescriptorTable::MakeUnique();
			descriptorTable->SetDescriptors(0, 1, &m_pDepths[DEPTH_MAP]->GetSRV());
			XUSG_X_RETURN(m_srvTables[SRV_TABLE_DEPTH], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
		}

		if (m_pDepths[SHADOW_MAP])
		{
			const auto descriptorTable = Util::DescriptorTable::MakeUnique();
			descriptorTable->SetDescriptors(0, 1, &m_pDepths[SHADOW_MAP]->GetSRV());
			XUSG_X_RETURN(m_srvTables[SRV_TABLE_SHADOW], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
		}
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		vector<Descriptor> descriptors(g_numCubeMips * numVolumes);
		for (auto i = 0u; i < numVolumes; ++i)
			for (uint8_t j = 0; j < g_numCubeMips; ++j)
				descriptors[g_numCubeMips * i + j] = m_cubeMaps[i]->GetSRV(j);
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(descriptors.size()), descriptors.data());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_CUBE_MAP], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	for (uint8_t i = 0; i < g_numCubeMips; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		vector<Descriptor> descriptors(g_numCubeMips * numVolumes);
		for (auto i = 0u; i < numVolumes; ++i)
			for (uint8_t j = 0; j < g_numCubeMips; ++j)
				descriptors[g_numCubeMips * i + j] = m_cubeDepths[i]->GetSRV(j);
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(descriptors.size()), descriptors.data());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_CUBE_DEPTH], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Create UAV tables
	m_uavInitTables.resize(numVolumeSrcs);
	for (auto i = 0u; i < numVolumeSrcs; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_volumes[i]->GetUAV());
		XUSG_X_RETURN(m_uavInitTables[i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		const Descriptor descriptors[] =
		{
			m_visibleVolumes->GetUAV(),
			m_volumeAttribs->GetUAV()
		};
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		XUSG_X_RETURN(m_uavTables[UAV_TABLE_CULL], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	if (pColorOut)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &pColorOut->GetUAV());
		XUSG_X_RETURN(m_uavTables[UAV_TABLE_OUT], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Create the sampler
	const auto descriptorTable = Util::DescriptorTable::MakeUnique();
	const SamplerPreset samplers[] = { SamplerPreset::LINEAR_CLAMP, SamplerPreset::POINT_CLAMP };
	descriptorTable->SetSamplers(0, static_cast<uint32_t>(size(samplers)), samplers, m_descriptorTableCache.get());
	XUSG_X_RETURN(m_samplerTable, descriptorTable->GetSamplerTable(m_descriptorTableCache.get()), false);

	return true;
}

bool MultiRayCaster::buildAccelerationStructures(RayTracing::CommandList* pCommandList, GeometryBuffer* pGeometry)
{
	AccelerationStructure::SetFrameCount(FrameCount);
	const auto pDevice = pCommandList->GetRTDevice();

	// Set geometries
	BottomLevelAS::SetTriangleGeometries(*pGeometry, 1, Format::R32G32B32_FLOAT,
		&m_vertexBuffer->GetVBV(), &m_indexBuffer->GetIBV());

	// Descriptor index in descriptor pool
	const auto bottomLevelASIndex = 0u;
	const auto topLevelASIndex = bottomLevelASIndex + 1;

	assert(m_volumeWorlds.size() == m_cubeMaps.size());
	const auto numVolumes = static_cast<uint32_t>(m_volumeWorlds.size());

	// Prebuild
	m_bottomLevelAS = BottomLevelAS::MakeUnique();
	m_topLevelAS = TopLevelAS::MakeUnique();
	XUSG_N_RETURN(m_bottomLevelAS->PreBuild(pDevice, 1, *pGeometry, bottomLevelASIndex), false);
	XUSG_N_RETURN(m_topLevelAS->PreBuild(pDevice, numVolumes, topLevelASIndex), false);

	// Create scratch buffer
	auto scratchSize = m_topLevelAS->GetScratchDataMaxSize();
	scratchSize = (max)(m_bottomLevelAS->GetScratchDataMaxSize(), scratchSize);
	m_scratch = Resource::MakeUnique();
	XUSG_N_RETURN(AccelerationStructure::AllocateUAVBuffer(pDevice, m_scratch.get(), scratchSize), false);

	// Get descriptor pool and create descriptor tables
	XUSG_N_RETURN(createDescriptorTables(nullptr), false);
	const auto& descriptorPool = m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL);

	// Set instance
	vector<float*> transforms(numVolumes);
	for (auto i = 0u; i < numVolumes; ++i)
		transforms[i] = reinterpret_cast<float*>(&m_volumeWorlds[i]);
	m_instances = Resource::MakeUnique();
	vector<const BottomLevelAS*> pBottomLevelASs(numVolumes, m_bottomLevelAS.get());
	TopLevelAS::SetInstances(pDevice, m_instances.get(), numVolumes, pBottomLevelASs.data(), transforms.data());

	// Build bottom level ASs
	m_bottomLevelAS->Build(pCommandList, m_scratch.get(), descriptorPool);

	// Build top level AS
	m_topLevelAS->Build(pCommandList, m_scratch.get(), m_instances.get(), descriptorPool);

	return true;
}

bool MultiRayCaster::buildShaderTables(const RayTracing::Device* pDevice)
{
	// Get shader identifiers.
	const auto shaderIDSize = ShaderRecord::GetShaderIDSize(pDevice);

	// Ray gen shader table
	m_rayGenShaderTable = ShaderTable::MakeUnique();
	XUSG_N_RETURN(m_rayGenShaderTable->Create(pDevice, 1, shaderIDSize, L"RayGenShaderTable"), false);
	XUSG_N_RETURN(m_rayGenShaderTable->AddShaderRecord(ShaderRecord::MakeUnique(pDevice, m_pipelines[RAY_TRACING], RaygenShaderName).get()), false);

	// Hit group shader table
	m_hitGroupShaderTable = ShaderTable::MakeUnique();
	XUSG_N_RETURN(m_hitGroupShaderTable->Create(pDevice, 1, shaderIDSize, L"HitGroupShaderTable"), false);
	XUSG_N_RETURN(m_hitGroupShaderTable->AddShaderRecord(ShaderRecord::MakeUnique(pDevice, m_pipelines[RAY_TRACING], HitGroupName).get()), false);

	// Miss shader table
	m_missShaderTable = ShaderTable::MakeUnique();
	XUSG_N_RETURN(m_missShaderTable->Create(pDevice, 1, shaderIDSize, L"MissShaderTable"), false);
	XUSG_N_RETURN(m_missShaderTable->AddShaderRecord(ShaderRecord::MakeUnique(pDevice, m_pipelines[RAY_TRACING], MissShaderName).get()), false);

	return true;
}

void MultiRayCaster::cullVolumes(XUSG::CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barrier
	ResourceBarrier barriers[1];
	m_visibleVolumeCounter->SetBarrier(barriers, ResourceState::COPY_DEST);
	//pCommandList->Barrier(numBarriers, barriers); // Promotion

	// Reset counter
	pCommandList->CopyResource(m_visibleVolumeCounter.get(), m_counterReset.get());

	// Set barriers
	m_visibleVolumes->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS);	// Promotion
	m_volumeAttribs->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS);		// Promotion
	//pCommandList->Barrier(numBarriers, barriers);

	// Set pipeline state
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[VOLUME_CULL]);
	pCommandList->SetPipelineState(m_pipelines[VOLUME_CULL]);

	// Set descriptor tables
	pCommandList->SetComputeDescriptorTable(0, m_cbvSrvTables[frameIndex]);
	pCommandList->SetComputeDescriptorTable(1, m_uavTables[UAV_TABLE_CULL]);
	pCommandList->SetComputeDescriptorTable(2, m_srvTables[SRV_TABLE_VOLUME_DESCS]);
	pCommandList->SetCompute32BitConstant(3, m_maxRaySamples);

	// Dispatch cube
	const uint32_t numVolumes = static_cast<uint32_t>(m_volumeDescs->GetWidth() / sizeof(VolumeDesc));
	pCommandList->Dispatch(XUSG_DIV_UP(numVolumes, GROUP_VOLUME_COUNT), 1, 1);
}

void MultiRayCaster::rayMarchV(XUSG::CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barriers
	vector<ResourceBarrier> barriers(m_cubeMaps.size() + m_cubeDepths.size() + 5);
	m_volumeDispatchArg->SetBarrier(barriers.data(), ResourceState::COPY_DEST);	// Promotion
	m_volumeDrawArg->SetBarrier(barriers.data(), ResourceState::COPY_DEST);		// Promotion
	auto numBarriers = m_visibleVolumes->SetBarrier(barriers.data(), ResourceState::NON_PIXEL_SHADER_RESOURCE);
	numBarriers = m_volumeAttribs->SetBarrier(barriers.data(), ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers);
	numBarriers = m_lightMap->SetBarrier(barriers.data(), ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers);
	numBarriers = m_pDepths[DEPTH_MAP]->SetBarrier(barriers.data(), ResourceState::NON_PIXEL_SHADER_RESOURCE |
		ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
	for (auto& cubeMap : m_cubeMaps)
		numBarriers = cubeMap->SetBarrier(barriers.data(), ResourceState::UNORDERED_ACCESS, numBarriers);
	for (auto& cubeDepth : m_cubeDepths)
		numBarriers = cubeDepth->SetBarrier(barriers.data(), ResourceState::UNORDERED_ACCESS, numBarriers);
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
	numBarriers = m_volumeDrawArg->SetBarrier(barriers, ResourceState::INDIRECT_ARGUMENT, numBarriers);
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

void MultiRayCaster::renderDepth(XUSG::CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barrier
	ResourceBarrier barriers[2];
	auto numBarriers = m_depth->SetBarrier(barriers, ResourceState::DEPTH_WRITE);
	numBarriers = m_volumeDrawArg->SetBarrier(barriers, ResourceState::INDIRECT_ARGUMENT, numBarriers);
	pCommandList->Barrier(numBarriers, barriers);

	// Clear depth
	const auto& pDSV = m_depth->GetDSV();
	pCommandList->ClearDepthStencilView(pDSV, ClearFlag::DEPTH, 1.0f);
	pCommandList->OMSetRenderTargets(0, nullptr, &pDSV);

	// Set pipeline state
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[DEPTH_PASS]);
	pCommandList->SetPipelineState(m_pipelines[DEPTH_PASS]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLESTRIP);

	// Set descriptor tables
	pCommandList->SetGraphicsDescriptorTable(0, m_cbvSrvTables[frameIndex]);
	pCommandList->SetGraphicsDescriptorTable(1, m_srvTables[SRV_TABLE_VIS_VOLUMES]);

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

void MultiRayCaster::renderCubeRT(XUSG::CommandList* pCommandList, uint8_t frameIndex, RenderTarget* pOutView)
{
	// Set barriers
	vector<ResourceBarrier> barriers(m_cubeMaps.size() + m_cubeDepths.size() + 1);
	auto numBarriers = m_depth->SetBarrier(barriers.data(), ResourceState::DEPTH_READ);
	for (auto& cubeMap : m_cubeMaps)
		numBarriers = cubeMap->SetBarrier(barriers.data(), ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
	for (auto& cubeDepth : m_cubeDepths)
		numBarriers = cubeDepth->SetBarrier(barriers.data(), ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
	pCommandList->Barrier(numBarriers, barriers.data());

	// Set render target
	pCommandList->OMSetRenderTargets(1, &pOutView->GetRTV(), &m_depth->GetDSV());

	// Set pipeline state
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[RENDER_CUBE_RT]);
	pCommandList->SetPipelineState(m_pipelines[RENDER_CUBE_RT]);

	// Set descriptor tables
	pCommandList->SetGraphicsDescriptorTable(0, m_cbvSrvTables[frameIndex]);
	pCommandList->SetGraphicsDescriptorTable(1, m_srvTables[SRV_TABLE_VIS_VOLUMES]);
	pCommandList->SetGraphicsRootShaderResourceView(2, m_topLevelAS->GetResult().get());
	pCommandList->SetGraphicsDescriptorTable(3, m_srvTables[SRV_TABLE_VOLUME_ATTRIBS]);
	pCommandList->SetGraphicsDescriptorTable(4, m_srvTables[SRV_TABLE_CUBE_MAP]);
	pCommandList->SetGraphicsDescriptorTable(5, m_srvTables[SRV_TABLE_CUBE_DEPTH]);
	pCommandList->SetGraphicsDescriptorTable(6, m_srvTables[SRV_TABLE_DEPTH]);
	pCommandList->SetGraphicsDescriptorTable(7, m_samplerTable);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);
	pCommandList->IASetIndexBuffer(m_indexBuffer->GetIBV());
	pCommandList->ExecuteIndirect(m_commandLayouts[DRAW_LAYOUT].get(), 1, m_volumeDrawArg.get());
}

void MultiRayCaster::resolveOIT(XUSG::CommandList* pCommandList, uint8_t frameIndex)
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

void MultiRayCaster::traceCube(RayTracing::CommandList* pCommandList, uint8_t frameIndex, Texture* pColorOut)
{
	// Set barriers
	vector<ResourceBarrier> barriers(m_cubeMaps.size() + m_cubeDepths.size() + 1);
	auto numBarriers = pColorOut->SetBarrier(barriers.data(), ResourceState::UNORDERED_ACCESS);
	for (auto& cubeMap : m_cubeMaps)
		numBarriers = cubeMap->SetBarrier(barriers.data(), ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers);
	for (auto& cubeDepth : m_cubeDepths)
		numBarriers = cubeDepth->SetBarrier(barriers.data(), ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers);
	pCommandList->Barrier(numBarriers, barriers.data());

	// Set pipeline state
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[RAY_TRACING]);
	pCommandList->SetRayTracingPipeline(m_pipelines[RAY_TRACING]);

	// Set descriptor tables
	pCommandList->SetTopLevelAccelerationStructure(0, m_topLevelAS.get());
	pCommandList->SetComputeDescriptorTable(1, m_cbvSrvTables[frameIndex]);
	pCommandList->SetComputeDescriptorTable(2, m_srvTables[SRV_TABLE_VOLUME_ATTRIBS]);
	pCommandList->SetComputeDescriptorTable(3, m_uavTables[UAV_TABLE_OUT]);
	pCommandList->SetComputeDescriptorTable(4, m_srvTables[SRV_TABLE_CUBE_MAP]);
	pCommandList->SetComputeDescriptorTable(5, m_srvTables[SRV_TABLE_CUBE_DEPTH]);
	pCommandList->SetComputeDescriptorTable(6, m_srvTables[SRV_TABLE_DEPTH]);
	pCommandList->SetComputeDescriptorTable(7, m_samplerTable);

	pCommandList->DispatchRays(m_viewport.x, m_viewport.y, 1, m_hitGroupShaderTable.get(),
		m_missShaderTable.get(), m_rayGenShaderTable.get());
}
