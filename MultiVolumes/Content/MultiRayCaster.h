//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#pragma once

#include "Core/XUSG.h"
#include "RayTracing/XUSGRayTracing.h"

class MultiRayCaster
{
public:
	enum RTSupport : uint8_t
	{
		RT_PIPELINE	= (1 << 0),
		RT_INLINE	= (1 << 1)
	};

	enum OITMethod : uint8_t
	{
		OIT_K_BUFFER,
		OIT_RAY_TRACING,
		OIT_RAY_QUERY,

		OIT_METHOD_COUNT
	};

	MultiRayCaster();
	virtual ~MultiRayCaster();

	bool Init(XUSG::RayTracing::CommandList* pCommandList, const XUSG::DescriptorTableLib::sptr& descriptorTableLib,
		XUSG::Format rtFormat, XUSG::Format dsFormat, uint32_t gridSize, uint32_t lightGridSize, uint32_t numVolumes,
		uint32_t numVolumeSrcs, std::vector<XUSG::Resource::uptr>& uploaders, XUSG::RayTracing::GeometryBuffer* pGeometry,
		uint8_t rtSupport);
	bool LoadVolumeData(XUSG::CommandList* pCommandList, uint32_t i,
		const wchar_t* fileName, std::vector<XUSG::Resource::uptr>& uploaders);
	bool SetRenderTargets(const XUSG::Device* pDevice, const XUSG::RenderTarget* pColorOut, const XUSG::DepthStencil::uptr* depths);
	bool SetViewport(const XUSG::Device* pDevice, uint32_t width, uint32_t height, const XUSG::Texture* pColorOut);

	void InitVolumeData(const XUSG::CommandList* pCommandList, uint32_t i);
	void SetSH(const XUSG::StructuredBuffer::sptr& coeffSH);
	void SetMaxSamples(uint32_t maxRaySamples, uint32_t maxLightSamples);
	void SetVolumesWorld(float size, const DirectX::XMFLOAT3& center);
	void SetVolumeWorld(uint32_t i, float size, const DirectX::XMFLOAT3& pos);
	void SetLight(const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& color, float intensity);
	void SetAmbient(const DirectX::XMFLOAT3& color, float intensity);
	void UpdateFrame(uint8_t frameIndex, DirectX::CXMMATRIX viewProj,
		const DirectX::XMFLOAT4X4& shadowVP, const DirectX::XMFLOAT3& eyePt);
	void Render(XUSG::RayTracing::CommandList* pCommandList, uint8_t frameIndex,
		XUSG::RenderTarget* pColorOut, OITMethod oitMethod = OIT_K_BUFFER);

	static const uint8_t FrameCount = 3;

protected:
	enum PipelineIndex : uint8_t
	{
		LOAD_VOLUME_DATA,
		INIT_VOLUME_DATA,
		VOLUME_CULL,
		RAY_MARCH_L,
		RAY_MARCH_V,
		CUBE_DEPTH_PEEL,
		DEPTH_PASS,
		RENDER_CUBE,
		RENDER_CUBE_RT,
		RESOLVE_OIT,
		RAY_TRACING,

		NUM_PIPELINE
	};

	enum CommandLayoutIndex : uint8_t
	{
		DISPATCH_LAYOUT,
		DRAW_LAYOUT,

		NUM_COMMAND_LAYOUT
	};

	enum SrvTable : uint8_t
	{
		SRV_TABLE_FILE_SRC,
		SRV_TABLE_VOLUME_DESCS,
		SRV_TABLE_VOLUME,
		SRV_TABLE_VIS_VOLUMES,
		SRV_TABLE_VOLUME_ATTRIBS,
		SRV_TABLE_CUBE_VOLUMES,
		SRV_TABLE_LIGHT_MAP,
		SRV_TABLE_DEPTH,
		SRV_TABLE_SHADOW,
		SRV_TABLE_CUBE_MAP,
		SRV_TABLE_CUBE_DEPTH,
		SRV_TABLE_K_COLORS,
		SRV_TABLE_K_DEPTHS,

		NUM_SRV_TABLE
	};

	enum UavMipTable : uint8_t
	{
		UAV_TABLE_CULL,
		UAV_TABLE_CUBE_MAP,
		UAV_TABLE_CUBE_DEPTH,
		UAV_TABLE_LIGHT_MAP,
		UAV_TABLE_K_COLORS,
		UAV_TABLE_K_DEPTHS,
		UAV_TABLE_OUT,

		NUM_UAV_TABLE
	};

	enum DepthIndex : uint8_t
	{
		DEPTH_MAP,
		SHADOW_MAP
	};

	bool createCubeVB(XUSG::CommandList* pCommandList, std::vector<XUSG::Resource::uptr>& uploaders);
	bool createCubeIB(XUSG::CommandList* pCommandList, std::vector<XUSG::Resource::uptr>& uploaders);
	bool createVolumeInfoBuffers(XUSG::CommandList* pCommandList, uint32_t numVolumes,
		uint32_t numVolumeSrcs, std::vector<XUSG::Resource::uptr>& uploaders);
	bool createPipelineLayouts(const XUSG::Device* pDevice);
	bool createPipelines(XUSG::Format rtFormat, XUSG::Format dsFormat);
	bool createCommandLayouts(const XUSG::Device* pDevice);
	bool createDescriptorTables(const XUSG::Texture* pColorOut);
	bool buildAccelerationStructures(XUSG::RayTracing::CommandList* pCommandList,
		XUSG::RayTracing::GeometryBuffer* pGeometries);
	bool buildShaderTables(const XUSG::RayTracing::Device* pDevice);

	void cullVolumes(XUSG::CommandList* pCommandList, uint8_t frameIndex);
	void rayMarchL(XUSG::CommandList* pCommandList, uint8_t frameIndex);
	void rayMarchV(XUSG::CommandList* pCommandList, uint8_t frameIndex);
	void cubeDepthPeel(XUSG::CommandList* pCommandList, uint8_t frameIndex);
	void renderDepth(XUSG::CommandList* pCommandList, uint8_t frameIndex);
	void renderCube(XUSG::CommandList* pCommandList, uint8_t frameIndex);
	void renderCubeRT(XUSG::CommandList* pCommandList, uint8_t frameIndex, XUSG::RenderTarget* pColorOut);
	void resolveOIT(XUSG::CommandList* pCommandList, uint8_t frameIndex);
	void traceCube(XUSG::RayTracing::CommandList* pCommandList, uint8_t frameIndex, XUSG::Texture* pColorOut);

	XUSG::RayTracing::BottomLevelAS::uptr m_bottomLevelAS;
	XUSG::RayTracing::TopLevelAS::uptr m_topLevelAS;

	XUSG::VertexBuffer::uptr	m_vertexBuffer;
	XUSG::IndexBuffer::uptr		m_indexBuffer;

	// Shader tables
	static const wchar_t* HitGroupName;
	static const wchar_t* RaygenShaderName;
	static const wchar_t* ClosestHitShaderName;
	static const wchar_t* MissShaderName;
	XUSG::RayTracing::ShaderTable::uptr	m_missShaderTable;
	XUSG::RayTracing::ShaderTable::uptr	m_hitGroupShaderTable;
	XUSG::RayTracing::ShaderTable::uptr	m_rayGenShaderTable;

	XUSG::ShaderLib::uptr				m_shaderLib;
	XUSG::RayTracing::PipelineLib::uptr m_rayTracingPipelineLib;
	XUSG::Graphics::PipelineLib::uptr	m_graphicsPipelineLib;
	XUSG::Compute::PipelineLib::uptr	m_computePipelineLib;
	XUSG::PipelineLayoutLib::uptr		m_pipelineLayoutLib;
	XUSG::DescriptorTableLib::sptr		m_descriptorTableLib;

	XUSG::PipelineLayout	m_pipelineLayouts[NUM_PIPELINE];
	XUSG::Pipeline			m_pipelines[NUM_PIPELINE];
	XUSG::CommandLayout::uptr m_commandLayouts[NUM_COMMAND_LAYOUT];

	std::vector<XUSG::DescriptorTable> m_uavInitTables;
	XUSG::DescriptorTable	m_cbvTables[FrameCount];
	XUSG::DescriptorTable	m_cbvSrvTables[FrameCount];
	XUSG::DescriptorTable	m_uavTables[NUM_UAV_TABLE];
	XUSG::DescriptorTable	m_srvTables[NUM_SRV_TABLE];

	std::vector<XUSG::Texture::sptr>	m_fileSrcs;
	std::vector<XUSG::Texture3D::uptr>	m_volumes;
	std::vector<XUSG::Texture2D::uptr>	m_cubeMaps;
	std::vector<XUSG::Texture2D::uptr>	m_cubeDepths;
	std::vector<XUSG::Texture3D::uptr>	m_lightMaps;
	XUSG::Texture::uptr		m_kDepths;
	XUSG::Texture::uptr		m_kColors;
	XUSG::ConstantBuffer::uptr m_cbPerFrame;
	XUSG::StructuredBuffer::uptr m_perObject;
	XUSG::StructuredBuffer::uptr m_volumeDescs;
	XUSG::StructuredBuffer::uptr m_visibleVolumes;
	XUSG::StructuredBuffer::uptr m_cubeMapVolumes;
	XUSG::StructuredBuffer::uptr m_counterReset;
	XUSG::StructuredBuffer::sptr m_visibleVolumeCounter;
	XUSG::StructuredBuffer::sptr m_cubeMapVolumeCounter;
	XUSG::TypedBuffer::uptr m_volumeAttribs;
	XUSG::RawBuffer::uptr	m_volumeDispatchArg;
	XUSG::RawBuffer::uptr	m_volumeDrawArg;

	const XUSG::DepthStencil::uptr* m_pDepths;
	XUSG::StructuredBuffer::sptr	m_coeffSH;
	XUSG::DepthStencil::uptr m_depth;

	XUSG::Resource::uptr	m_scratch;
	XUSG::Resource::uptr	m_instances;

	uint32_t				m_gridSize;
	uint32_t				m_lightGridSize;
	uint32_t				m_maxRaySamples;
	uint32_t				m_maxLightSamples;
	uint32_t				m_frameIdx;

	DirectX::XMFLOAT3		m_lightPt;
	DirectX::XMFLOAT4		m_lightColor;
	DirectX::XMFLOAT4		m_ambient;
	std::vector<DirectX::XMFLOAT3X4> m_volumeWorlds;

	DirectX::XMUINT2		m_viewport;

	uint8_t m_rtSupport;
};
