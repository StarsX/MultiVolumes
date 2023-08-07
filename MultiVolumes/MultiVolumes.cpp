//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#include "SharedConsts.h"
#include "MultiVolumes.h"
#include "stb_image_write.h"
#include <DirectXColors.h>

using namespace std;
using namespace XUSG;
using namespace XUSG::RayTracing;

const float g_FOVAngleY = XM_PIDIV4;

const auto g_backFormat = Format::R8G8B8A8_UNORM;
const auto g_rtFormat = Format::R16G16B16A16_FLOAT;
const auto g_dsFormat = Format::D32_FLOAT;

MultiVolumes::MultiVolumes(uint32_t width, uint32_t height, std::wstring name) :
	DXFramework(width, height, name),
	m_frameIndex(0),
	m_oitMethod(MultiRayCaster::OIT_RAY_QUERY),
	m_useWorkGraph(false),
	m_animate(false),
	m_showMesh(false),
	m_showFPS(true),
	m_isPaused(false),
	m_tracking(false),
	m_gridSize(128),
	m_lightGridSize(96),
	m_maxRaySamples(256),
	m_maxLightSamples(96),
	m_numVolumes(2),
	m_radianceFile(L"Assets/LA_Radiance.dds"),
	m_meshFileName("Assets/bunny.obj"),
	m_volPosScale(0.0f, 0.0f, 0.0f, 10.0f),
	m_meshPosScale(0.0f, -9.0f, 0.0f, 1.8f),
	m_screenShot(0)
{
#if defined (_DEBUG)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	AllocConsole();
	FILE* stream;
	freopen_s(&stream, "CONIN$", "r+t", stdin);
	freopen_s(&stream, "CONOUT$", "w+t", stdout);
	freopen_s(&stream, "CONOUT$", "w+t", stderr);
#endif

	m_volumeFiles[0] = L"Assets/bunny.dds";
	m_volumeFiles[1] = L"Assets/buddha.dds";
	m_volumeFiles[2] = L"Assets/dragon.dds";
	m_volumeFiles[3] = L"Assets/Eagle.dds";
	m_volumeFiles[4] = L"Assets/Jacquemart.dds";
	m_volumeFiles[5] = L"Assets/lucy.dds";
	m_volumeFiles[6] = L"Assets/penelope.dds";
	m_volumeFiles[7] = L"Assets/Cloud1.dds";
	m_volumeFiles[8] = L"Assets/Cloud2.dds";
	m_volumeFiles[9] = L"Assets/Devil.dds";
}

MultiVolumes::~MultiVolumes()
{
#if defined (_DEBUG)
	FreeConsole();
#endif
}

void MultiVolumes::OnInit()
{
	LoadPipeline();
	LoadAssets();
}

// Load the rendering pipeline dependencies.
void MultiVolumes::LoadPipeline()
{
	auto dxgiFactoryFlags = 0u;

#if defined(_DEBUG)
	// Enable the debug layer (requires the Graphics Tools "optional feature").
	// NOTE: Enabling the debug layer after device creation will invalidate the active device.
	{
		com_ptr<ID3D12Debug1> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			debugController->EnableDebugLayer();
			//debugController->SetEnableGPUBasedValidation(TRUE);

			// Enable additional debug layers.
			dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
		}
	}
#endif

	ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&m_factory)));

	DXGI_ADAPTER_DESC1 dxgiAdapterDesc;
	com_ptr<IDXGIAdapter1> dxgiAdapter;
	auto hr = DXGI_ERROR_UNSUPPORTED;
	//const auto createDeviceFlags = EnableRootDescriptorsInShaderRecords;
	const auto createDeviceFlags = 0;
	for (auto i = 0u; hr == DXGI_ERROR_UNSUPPORTED; ++i)
	{
		dxgiAdapter = nullptr;
		ThrowIfFailed(m_factory->EnumAdapters1(i, &dxgiAdapter));
		EnableDirectXRaytracingAndWorkGraph(dxgiAdapter.get());

		m_device = RayTracing::Device::MakeUnique();
		hr = m_device->Create(dxgiAdapter.get(), D3D_FEATURE_LEVEL_11_0);
		XUSG_N_RETURN(m_device->CreateInterface(createDeviceFlags), ThrowIfFailed(E_FAIL));
	}

	dxgiAdapter->GetDesc1(&dxgiAdapterDesc);
	if (dxgiAdapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
		m_title += dxgiAdapterDesc.VendorId == 0x1414 && dxgiAdapterDesc.DeviceId == 0x8c ? L" (WARP)" : L" (Software)";
	ThrowIfFailed(hr);

	m_oitMethod = (m_dxrSupport & MultiRayCaster::RT_INLINE) ? MultiRayCaster::OIT_RAY_QUERY :
		((m_dxrSupport & MultiRayCaster::RT_PIPELINE) ? MultiRayCaster::OIT_RAY_TRACING : MultiRayCaster::OIT_K_BUFFER);

	// Create the command queue.
	m_commandQueue = CommandQueue::MakeUnique();
	XUSG_N_RETURN(m_commandQueue->Create(m_device.get(), CommandListType::DIRECT, CommandQueueFlag::NONE,
		0, 0, L"CommandQueue"), ThrowIfFailed(E_FAIL));

	// Create the swap chain.
	CreateSwapchain();

	// Reset the index to the current back buffer.
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// Create a command allocator for each frame.
	for (uint8_t n = 0; n < FrameCount; ++n)
	{
		m_commandAllocators[n] = CommandAllocator::MakeUnique();
		XUSG_N_RETURN(m_commandAllocators[n]->Create(m_device.get(), CommandListType::DIRECT,
			(L"CommandAllocator" + to_wstring(n)).c_str()), ThrowIfFailed(E_FAIL));
	}

	// Create descriptor-table lib.
	m_descriptorTableLib = DescriptorTableLib::MakeShared(m_device.get(), L"DescriptorTableLib");
}

// Load the sample assets.
void MultiVolumes::LoadAssets()
{
	// Create the command list.
	m_commandList = RayTracing::CommandList::MakeUnique();
	const auto pCommandList = m_commandList.get();
	XUSG_N_RETURN(pCommandList->Create(m_device.get(), 0, CommandListType::DIRECT,
		m_commandAllocators[m_frameIndex].get(), nullptr), ThrowIfFailed(E_FAIL));

	// Create ray tracing interfaces
	XUSG_N_RETURN(m_commandList->CreateInterface(), ThrowIfFailed(E_FAIL));

	// Clear color setting
	m_clearColor = { 0.2f, 0.2f, 0.2f, 0.0f };
	m_clearColor = m_volumeFiles->empty() ? m_clearColor : DirectX::Colors::CornflowerBlue;
	m_clearColor.v = XMVectorPow(m_clearColor, XMVectorReplicate(1.0f / 1.25f));
	m_clearColor.v = 0.7f * m_clearColor / (XMVectorReplicate(1.25f) - m_clearColor);
	m_clearColor.f[3] = 0.0f;

	vector<Resource::uptr> uploaders(0);
	m_descriptorTableLib->AllocateDescriptorHeap(CBV_SRV_UAV_HEAP, 1600);

	if (!m_radianceFile.empty())
	{
		XUSG_X_RETURN(m_lightProbe, make_unique<LightProbe>(), ThrowIfFailed(E_FAIL));
		XUSG_N_RETURN(m_lightProbe->Init(pCommandList, m_descriptorTableLib, uploaders,
			m_radianceFile.c_str(), g_rtFormat, g_dsFormat), ThrowIfFailed(E_FAIL));
	}

	XUSG_X_RETURN(m_objectRenderer, make_unique<ObjectRenderer>(), ThrowIfFailed(E_FAIL));
	XUSG_N_RETURN(m_objectRenderer->Init(m_commandList.get(), m_descriptorTableLib, uploaders,
		m_meshFileName.c_str(), g_backFormat, g_rtFormat, g_dsFormat, m_meshPosScale), ThrowIfFailed(E_FAIL));

	const auto numVolumeSrcs = static_cast<uint32_t>(size(m_volumeFiles));

	GeometryBuffer geometry;
	m_rayCaster = make_unique<MultiRayCaster>();
	if (!m_rayCaster) ThrowIfFailed(E_FAIL);
	if (!m_rayCaster->Init(pCommandList, m_descriptorTableLib, g_rtFormat, g_dsFormat,
		m_gridSize, m_lightGridSize, m_numVolumes, numVolumeSrcs, uploaders,
		&geometry, m_dxrSupport, m_workGraphSupport)) ThrowIfFailed(E_FAIL);
	const auto volumeSize = m_volPosScale.w * 2.0f;
	const auto volumePos = XMFLOAT3(m_volPosScale.x, m_volPosScale.y, m_volPosScale.z);
	m_rayCaster->SetVolumesWorld(volumeSize, volumePos);
	m_rayCaster->SetMaxSamples(m_maxRaySamples, m_maxLightSamples);

	if (m_volumeFiles->empty())
	{
		for (auto i = 0u; i < numVolumeSrcs; ++i)
			m_rayCaster->InitVolumeData(pCommandList, i);
	}
	else
	{
		for (auto i = 0u; i < numVolumeSrcs; ++i)
			m_rayCaster->LoadVolumeData(pCommandList, i, m_volumeFiles[i].c_str(), uploaders);
	}

	// Close the command list and execute it to begin the initial GPU setup.
	XUSG_N_RETURN(pCommandList->Close(), ThrowIfFailed(E_FAIL));
	m_commandQueue->ExecuteCommandList(pCommandList);

	// Create synchronization objects and wait until assets have been uploaded to the GPU.
	{
		if (!m_fence)
		{
			m_fence = Fence::MakeUnique();
			XUSG_N_RETURN(m_fence->Create(m_device.get(), m_fenceValues[m_frameIndex]++, FenceFlag::NONE, L"Fence"), ThrowIfFailed(E_FAIL));
		}

		// Create an event handle to use for frame synchronization.
		m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (!m_fenceEvent) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));

		// Wait for the command list to execute; we are reusing the same command 
		// list in our main loop but for now, we just want to wait for setup to 
		// complete before continuing.
		WaitForGpu();
	}

	// Create window size dependent resources.
	//m_descriptorTableCache->ResetDescriptorPool(CBV_SRV_UAV_HEAP, 0);
	CreateResources();

	// Projection
	{
		const auto aspectRatio = m_width / static_cast<float>(m_height);
		const auto proj = XMMatrixPerspectiveFovLH(g_FOVAngleY, aspectRatio, g_zNear, g_zFar);
		XMStoreFloat4x4(&m_proj, proj);
	}

	// View initialization
	{
		m_focusPt = XMFLOAT3(0.0f, 0.0f, 0.0f);
		m_eyePt = XMFLOAT3(4.0f, 16.0f, -80.0f);
		const auto focusPt = XMLoadFloat3(&m_focusPt);
		const auto eyePt = XMLoadFloat3(&m_eyePt);
		const auto view = XMMatrixLookAtLH(eyePt, focusPt, XMVectorSet(0.0f, 1.0f, 0.0f, 1.0f));
		XMStoreFloat4x4(&m_view, view);
	}
}

void MultiVolumes::CreateSwapchain()
{
	// Describe and create the swap chain.
	m_swapChain = SwapChain::MakeUnique();
	XUSG_N_RETURN(m_swapChain->Create(m_factory.get(), Win32Application::GetHwnd(), m_commandQueue->GetHandle(),
		FrameCount, m_width, m_height, g_backFormat, SwapChainFlag::ALLOW_TEARING), ThrowIfFailed(E_FAIL));

	// This class does not support exclusive full-screen mode and prevents DXGI from responding to the ALT+ENTER shortcut.
	ThrowIfFailed(m_factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));
}

void MultiVolumes::CreateResources()
{
	// Obtain the back buffers for this window which will be the final render targets
	// and create render target views for each of them.
	for (uint8_t n = 0; n < FrameCount; ++n)
	{
		m_renderTargets[n] = RenderTarget::MakeUnique();
		XUSG_N_RETURN(m_renderTargets[n]->CreateFromSwapChain(m_device.get(), m_swapChain.get(), n), ThrowIfFailed(E_FAIL));
	}

	if (m_lightProbe)
	{
		XUSG_N_RETURN(m_lightProbe->CreateDescriptorTables(m_device.get()), ThrowIfFailed(E_FAIL));
		XUSG_N_RETURN(m_objectRenderer->SetRadiance(m_lightProbe->GetRadiance()->GetSRV()), ThrowIfFailed(E_FAIL));
	}
	XUSG_N_RETURN(m_objectRenderer->SetViewport(m_device.get(), m_width, m_height,
		g_rtFormat, g_dsFormat, m_clearColor, true), ThrowIfFailed(E_FAIL));
	XUSG_N_RETURN(m_rayCaster->SetRenderTargets(m_device.get(), m_objectRenderer->GetRenderTarget(ObjectRenderer::RT_COLOR),
		m_objectRenderer->GetDepthMaps()), ThrowIfFailed(E_FAIL));
}

// Update frame-based values.
void MultiVolumes::OnUpdate()
{
	// Timer
	static auto time = 0.0, pauseTime = 0.0;

	m_timer.Tick();
	float timeStep;
	const auto totalTime = CalculateFrameStats(&timeStep);
	pauseTime = m_isPaused ? totalTime - time : pauseTime;
	timeStep = m_isPaused ? 0.0f : timeStep;
	time = totalTime - pauseTime;

	// Auto camera animation
	if (m_animate)
	{
		const auto tParam = static_cast<float>(time * 0.5);
		const auto r = 60.0f;
		m_eyePt = XMFLOAT3(sinf(tParam) * r, 6.0f, cosf(tParam) * r);
		const auto focusPt = XMLoadFloat3(&m_focusPt);
		const auto eyePt = XMLoadFloat3(&m_eyePt);
		const auto view = XMMatrixLookAtLH(eyePt, focusPt, XMVectorSet(0.0f, 1.0f, 0.0f, 1.0f));
		XMStoreFloat4x4(&m_view, view);
	}

	const XMFLOAT3 lightPt(75.0f, 75.0f, -75.0f);
	const XMFLOAT3 lightColor(1.0f, 0.7f, 0.3f);
	const XMFLOAT3 ambientColor(0.4f, 0.6f, 1.0f);
	const auto lightIntensity = 3.0f * XM_PI, ambientIntensity = 2.0f * XM_PI;
	m_objectRenderer->SetLight(lightPt, lightColor, lightIntensity);
	m_objectRenderer->SetAmbient(ambientColor, ambientIntensity);
	m_rayCaster->SetLight(lightPt, lightColor, lightIntensity);
	m_rayCaster->SetAmbient(ambientColor, ambientIntensity);

	// View
	//const auto eyePt = XMLoadFloat3(&m_eyePt);
	const auto view = XMLoadFloat4x4(&m_view);
	const auto proj = XMLoadFloat4x4(&m_proj);
	const auto viewProj = view * proj;
	m_objectRenderer->UpdateFrame(m_frameIndex, viewProj, m_eyePt);
	if (m_lightProbe) m_lightProbe->UpdateFrame(m_frameIndex, viewProj, m_eyePt);
	m_rayCaster->UpdateFrame(m_frameIndex, viewProj, m_objectRenderer->GetShadowVP(), m_eyePt);
}

// Render the scene.
void MultiVolumes::OnRender()
{
	// Record all the commands we need to render the scene into the command list.
	PopulateCommandList();

	// Execute the command list.
	m_commandQueue->ExecuteCommandList(m_commandList.get());

	// Present the frame.
	XUSG_N_RETURN(m_swapChain->Present(0, PresentFlag::ALLOW_TEARING), ThrowIfFailed(E_FAIL));

	MoveToNextFrame();
}

void MultiVolumes::OnDestroy()
{
	// Ensure that the GPU is no longer referencing resources that are about to be
	// cleaned up by the destructor.
	WaitForGpu();

	CloseHandle(m_fenceEvent);
}

void MultiVolumes::OnWindowSizeChanged(int width, int height)
{
	if (!Win32Application::GetHwnd())
	{
		throw std::exception("Call SetWindow with a valid Win32 window handle");
	}

	// Wait until all previous GPU work is complete.
	WaitForGpu();

	// Release resources that are tied to the swap chain and update fence values.
	for (uint8_t n = 0; n < FrameCount; ++n)
	{
		m_renderTargets[n].reset();
		m_fenceValues[n] = m_fenceValues[m_frameIndex];
	}
	m_descriptorTableLib->ResetDescriptorHeap(CBV_SRV_UAV_HEAP);
	//m_descriptorTableCache->ResetDescriptorHeap(RTV_HEAP);

	// Determine the render target size in pixels.
	m_width = (max)(width, 1);
	m_height = (max)(height, 1);

	// If the swap chain already exists, resize it, otherwise create one.
	if (m_swapChain)
	{
		// If the swap chain already exists, resize it.
		const auto hr = m_swapChain->ResizeBuffers(FrameCount, m_width, m_height, g_backFormat, SwapChainFlag::ALLOW_TEARING);

		if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
		{
#ifdef _DEBUG
			char buff[64] = {};
			sprintf_s(buff, "Device Lost on ResizeBuffers: Reason code 0x%08X\n", (hr == DXGI_ERROR_DEVICE_REMOVED) ? m_device->GetDeviceRemovedReason() : hr);
			OutputDebugStringA(buff);
#endif
			// If the device was removed for any reason, a new device and swap chain will need to be created.
			//HandleDeviceLost();

			// Everything is set up now. Do not continue execution of this method. HandleDeviceLost will reenter this method 
			// and correctly set up the new device.
			return;
		}
		else
		{
			ThrowIfFailed(hr);
		}
	}
	else CreateSwapchain();

	// Reset the index to the current back buffer.
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// Create window size dependent resources.
	CreateResources();

	// Projection
	{
		const auto aspectRatio = m_width / static_cast<float>(m_height);
		const auto proj = XMMatrixPerspectiveFovLH(g_FOVAngleY, aspectRatio, g_zNear, g_zFar);
		XMStoreFloat4x4(&m_proj, proj);
	}
}

// User hot-key interactions.
void MultiVolumes::OnKeyUp(uint8_t key)
{
	switch (key)
	{
	case VK_SPACE:
		m_isPaused = !m_isPaused;
		break;
	case VK_F1:
		m_showFPS = !m_showFPS;
		break;
	case VK_F11:
		m_screenShot = 1;
		break;
	case 'A':
		m_animate = !m_animate;
		break;
	case 'M':
		m_showMesh = !m_showMesh;
		break;
	case 'O':
	{
		auto inc = (m_oitMethod + 1) % MultiRayCaster::OIT_METHOD_COUNT == MultiRayCaster::OIT_RAY_QUERY &&
			!(m_dxrSupport & MultiRayCaster::RT_INLINE) ? 2 : 1;
		inc = (m_oitMethod + 1) % MultiRayCaster::OIT_METHOD_COUNT == MultiRayCaster::OIT_RAY_TRACING &&
			!(m_dxrSupport & MultiRayCaster::RT_PIPELINE) ? 0 : inc;
		m_oitMethod = static_cast<MultiRayCaster::OITMethod>((m_oitMethod + inc) % MultiRayCaster::OIT_METHOD_COUNT);
		break;
	}
	case 'W':
		m_useWorkGraph = m_workGraphSupport ? !m_useWorkGraph : false;
		break;
	}
}

// User camera interactions.
void MultiVolumes::OnLButtonDown(float posX, float posY)
{
	m_tracking = true;
	m_mousePt = XMFLOAT2(posX, posY);
}

void MultiVolumes::OnLButtonUp(float posX, float posY)
{
	m_tracking = false;
}

void MultiVolumes::OnMouseMove(float posX, float posY)
{
	if (m_tracking)
	{
		const auto dPos = XMFLOAT2(m_mousePt.x - posX, m_mousePt.y - posY);

		XMFLOAT2 radians;
		radians.x = XM_2PI * dPos.y / m_height;
		radians.y = XM_2PI * dPos.x / m_width;

		const auto focusPt = XMLoadFloat3(&m_focusPt);
		auto eyePt = XMLoadFloat3(&m_eyePt);

		const auto len = XMVectorGetX(XMVector3Length(focusPt - eyePt));
		auto transform = XMMatrixTranslation(0.0f, 0.0f, -len);
		transform *= XMMatrixRotationRollPitchYaw(radians.x, radians.y, 0.0f);
		transform *= XMMatrixTranslation(0.0f, 0.0f, len);

		const auto view = XMLoadFloat4x4(&m_view) * transform;
		const auto viewInv = XMMatrixInverse(nullptr, view);
		eyePt = viewInv.r[3];

		XMStoreFloat3(&m_eyePt, eyePt);
		XMStoreFloat4x4(&m_view, view);

		m_mousePt = XMFLOAT2(posX, posY);
	}
}

void MultiVolumes::OnMouseWheel(float deltaZ, float posX, float posY)
{
	const auto focusPt = XMLoadFloat3(&m_focusPt);
	auto eyePt = XMLoadFloat3(&m_eyePt);

	const auto len = XMVectorGetX(XMVector3Length(focusPt - eyePt));
	const auto transform = XMMatrixTranslation(0.0f, 0.0f, -len * deltaZ / 16.0f);

	const auto view = XMLoadFloat4x4(&m_view) * transform;
	const auto viewInv = XMMatrixInverse(nullptr, view);
	eyePt = viewInv.r[3];

	XMStoreFloat3(&m_eyePt, eyePt);
	XMStoreFloat4x4(&m_view, view);
}

void MultiVolumes::OnMouseLeave()
{
	m_tracking = false;
}

void MultiVolumes::ParseCommandLineArgs(wchar_t* argv[], int argc)
{
	DXFramework::ParseCommandLineArgs(argv, argc);

	for (auto i = 1; i < argc; ++i)
	{
		if (wcsncmp(argv[i], L"-mesh", wcslen(argv[i])) == 0 ||
			wcsncmp(argv[i], L"/mesh", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc)
			{
				m_meshFileName.resize(wcslen(argv[++i]));
				for (size_t j = 0; j < m_meshFileName.size(); ++j)
					m_meshFileName[j] = static_cast<char>(argv[i][j]);
			}
			if (i + 1 < argc) m_meshPosScale.x = stof(argv[++i]);
			if (i + 1 < argc) m_meshPosScale.y = stof(argv[++i]);
			if (i + 1 < argc) m_meshPosScale.z = stof(argv[++i]);
			if (i + 1 < argc) m_meshPosScale.w = stof(argv[++i]);
		}
		else if (wcsncmp(argv[i], L"-gridSize", wcslen(argv[i])) == 0 ||
			wcsncmp(argv[i], L"/gridSize", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc) m_gridSize = stoul(argv[++i]);
		}
		else if (wcsncmp(argv[i], L"-lightGridSize", wcslen(argv[i])) == 0 ||
			wcsncmp(argv[i], L"/lightGridSize", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc) m_lightGridSize = stoul(argv[++i]);
		}
		else if (wcsncmp(argv[i], L"-volume", wcslen(argv[i])) == 0 ||
			wcsncmp(argv[i], L"/volume", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc) m_volumeFiles[0] = argv[++i];
			if (i + 1 < argc) m_volPosScale.x = stof(argv[++i]);
			if (i + 1 < argc) m_volPosScale.y = stof(argv[++i]);
			if (i + 1 < argc) m_volPosScale.z = stof(argv[++i]);
			if (i + 1 < argc) m_volPosScale.w = stof(argv[++i]);
		}
		else if (wcsncmp(argv[i], L"-maxRaySamples", wcslen(argv[i])) == 0 ||
			wcsncmp(argv[i], L"/maxRaySamples", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc) m_maxRaySamples = stoul(argv[++i]);
		}
		else if (wcsncmp(argv[i], L"-maxLightSamples", wcslen(argv[i])) == 0 ||
			wcsncmp(argv[i], L"/maxLightSamples", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc) m_maxLightSamples = stoul(argv[++i]);
		}
		else if (wcsncmp(argv[i], L"-numVolumes", wcslen(argv[i])) == 0 ||
			wcsncmp(argv[i], L"/numVolumes", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc) m_numVolumes = stoul(argv[++i]);
		}
		else if (wcsncmp(argv[i], L"-radiance", wcslen(argv[i])) == 0 ||
			wcsncmp(argv[i], L"/radiance", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc) m_radianceFile = argv[++i];
		}
	}
}

void MultiVolumes::PopulateCommandList()
{
	// Command list allocators can only be reset when the associated 
	// command lists have finished execution on the GPU; apps should use 
	// fences to determine GPU execution progress.
	const auto pCommandAllocator = m_commandAllocators[m_frameIndex].get();
	XUSG_N_RETURN(pCommandAllocator->Reset(), ThrowIfFailed(E_FAIL));

	// However, when ExecuteCommandList() is called on a particular command 
	// list, that command list can then be reset at any time and must be before 
	// re-recording.
	const auto pCommandList = m_commandList.get();
	XUSG_N_RETURN(pCommandList->Reset(pCommandAllocator, nullptr), ThrowIfFailed(E_FAIL));

	// Record commands.
	const auto descriptorHeap = m_descriptorTableLib->GetDescriptorHeap(CBV_SRV_UAV_HEAP);
	pCommandList->SetDescriptorHeaps(1, &descriptorHeap);

	if (m_lightProbe)
	{
		static auto isFirstFrame = true;
		if (isFirstFrame)
		{
			m_lightProbe->TransformSH(pCommandList);
			m_objectRenderer->SetSH(m_lightProbe->GetSH());
			m_rayCaster->SetSH(m_lightProbe->GetSH());
			isFirstFrame = false;
		}
	}

	m_objectRenderer->RenderShadow(pCommandList, m_frameIndex, m_showMesh);

	ResourceBarrier barriers[4];
	const auto pColor = m_objectRenderer->GetRenderTarget(ObjectRenderer::RT_COLOR);
	const auto pVelocity = m_objectRenderer->GetRenderTarget(ObjectRenderer::RT_VELOCITY);
	const auto pDepth = m_objectRenderer->GetDepthMap(ObjectRenderer::DEPTH_MAP);
	const auto pShadow = m_objectRenderer->GetDepthMap(ObjectRenderer::SHADOW_MAP);
	auto numBarriers = pColor->SetBarrier(barriers, ResourceState::RENDER_TARGET);
	numBarriers = pVelocity->SetBarrier(barriers, ResourceState::RENDER_TARGET, numBarriers);
	numBarriers = pDepth->SetBarrier(barriers, ResourceState::DEPTH_WRITE, numBarriers);
	numBarriers = pShadow->SetBarrier(barriers, ResourceState::ALL_SHADER_RESOURCE, numBarriers);
	pCommandList->Barrier(numBarriers, barriers);

	// Clear render targets
	const float clear[4] = {};
	const Descriptor pRTVs[] = { pColor->GetRTV(), pVelocity->GetRTV() };
	pCommandList->ClearRenderTargetView(pColor->GetRTV(), m_clearColor);
	pCommandList->ClearRenderTargetView(pVelocity->GetRTV(), clear);
	pCommandList->ClearDepthStencilView(pDepth->GetDSV(), ClearFlag::DEPTH, 1.0f);
	pCommandList->OMSetRenderTargets(static_cast<uint32_t>(size(pRTVs)), pRTVs, &pDepth->GetDSV());

	// Set viewport
	Viewport viewport(0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height));
	RectRange scissorRect(0, 0, m_width, m_height);
	pCommandList->RSSetViewports(1, &viewport);
	pCommandList->RSSetScissorRects(1, &scissorRect);

	m_objectRenderer->Render(pCommandList, m_frameIndex, m_showMesh);
	if (m_lightProbe) m_lightProbe->RenderEnvironment(pCommandList, m_frameIndex);
	m_rayCaster->Render(pCommandList, m_frameIndex, pColor, m_oitMethod, m_useWorkGraph);

	const auto pRenderTarget = m_renderTargets[m_frameIndex].get();
	m_objectRenderer->Postprocess(pCommandList, pRenderTarget);
	
	// Indicate that the back buffer will now be used to present.
	numBarriers = pRenderTarget->SetBarrier(barriers, ResourceState::PRESENT);
	pCommandList->Barrier(numBarriers, barriers);

	// Screen-shot helper
	if (m_screenShot == 1)
	{
		if (!m_readBuffer) m_readBuffer = Buffer::MakeUnique();
		pRenderTarget->ReadBack(pCommandList, m_readBuffer.get(), &m_rowPitch);
		m_screenShot = 2;
	}

	XUSG_N_RETURN(pCommandList->Close(), ThrowIfFailed(E_FAIL));
}

// Wait for pending GPU work to complete.
void MultiVolumes::WaitForGpu()
{
	// Schedule a Signal command in the queue.
	XUSG_N_RETURN(m_commandQueue->Signal(m_fence.get(), m_fenceValues[m_frameIndex]), ThrowIfFailed(E_FAIL));

	// Wait until the fence has been processed.
	XUSG_N_RETURN(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent), ThrowIfFailed(E_FAIL));
	WaitForSingleObject(m_fenceEvent, INFINITE);

	// Increment the fence value for the current frame.
	m_fenceValues[m_frameIndex]++;
}

// Prepare to render the next frame.
void MultiVolumes::MoveToNextFrame()
{
	// Schedule a Signal command in the queue.
	const auto currentFenceValue = m_fenceValues[m_frameIndex];
	XUSG_N_RETURN(m_commandQueue->Signal(m_fence.get(), currentFenceValue), ThrowIfFailed(E_FAIL));

	// Update the frame index.
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// If the next frame is not ready to be rendered yet, wait until it is ready.
	if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
	{
		XUSG_N_RETURN(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent), ThrowIfFailed(E_FAIL));
		WaitForSingleObject(m_fenceEvent, INFINITE);
	}

	// Set the fence value for the next frame.
	m_fenceValues[m_frameIndex] = currentFenceValue + 1;

	// Screen-shot helper
	if (m_screenShot)
	{
		if (m_screenShot > FrameCount)
		{
			char timeStr[15];
			tm dateTime;
			const auto now = time(nullptr);
			if (!localtime_s(&dateTime, &now) && strftime(timeStr, sizeof(timeStr), "%Y%m%d%H%M%S", &dateTime))
				SaveImage((string("MultiVolumes_") + timeStr + ".png").c_str(), m_readBuffer.get(), m_width, m_height, m_rowPitch);
			m_screenShot = 0;
		}
		else ++m_screenShot;
	}
}

void MultiVolumes::SaveImage(char const* fileName, Buffer* pImageBuffer, uint32_t w, uint32_t h, uint32_t rowPitch, uint8_t comp)
{
	assert(comp == 3 || comp == 4);
	const auto pData = static_cast<const uint8_t*>(pImageBuffer->Map(nullptr));

	//stbi_write_png_compression_level = 1024;
	vector<uint8_t> imageData(comp * w * h);
	const auto sw = rowPitch / 4; // Byte to pixel
	for (auto i = 0u; i < h; ++i)
		for (auto j = 0u; j < w; ++j)
		{
			const auto s = sw * i + j;
			const auto d = w * i + j;
			for (uint8_t k = 0; k < comp; ++k)
				imageData[comp * d + k] = pData[4 * s + k];
		}

	stbi_write_png(fileName, w, h, comp, imageData.data(), 0);

	pImageBuffer->Unmap();
}

double MultiVolumes::CalculateFrameStats(float* pTimeStep)
{
	static int frameCnt = 0;
	static double elapsedTime = 0.0;
	static double previousTime = 0.0;
	const auto totalTime = m_timer.GetTotalSeconds();
	++frameCnt;

	const auto timeStep = static_cast<float>(totalTime - elapsedTime);

	// Compute averages over one second period.
	if ((totalTime - elapsedTime) >= 1.0f)
	{
		float fps = static_cast<float>(frameCnt) / timeStep;	// Normalize to an exact second.

		frameCnt = 0;
		elapsedTime = totalTime;

		wstringstream windowText;
		windowText << L"    fps: ";
		if (m_showFPS) windowText << setprecision(2) << fixed << fps;
		else windowText << L"[F1]";

		windowText << L"    [A] " << (m_animate ? "Auto-animation" : "Interaction");
		windowText << L"    [M] Show/hide mesh";

		windowText << L"    [O] ";
		switch (m_oitMethod)
		{
		case MultiRayCaster::OIT_RAY_TRACING:
			windowText << L"Ray-traced OIT";
			break;
		case MultiRayCaster::OIT_RAY_QUERY:
			windowText << L"Hybrid ray-traced OIT (ray query)";
			break;
		default:
			windowText << L"K-buffer OIT";
		}

		windowText << L"    [W] " << (m_useWorkGraph ? "Work graph" : "Execute indirect");

		SetCustomWindowText(windowText.str().c_str());
	}

	if (pTimeStep)* pTimeStep = static_cast<float>(totalTime - previousTime);
	previousTime = totalTime;

	return totalTime;
}

//--------------------------------------------------------------------------------------
// Ray tracing and work graph
//--------------------------------------------------------------------------------------

// Enable experimental features required for compute-based raytracing fallback and work graph.
// This will set active D3D12 devices to DEVICE_REMOVED state.
// Returns bool whether the call succeeded and the device supports the feature.
inline bool EnableExperimentalFeatures(IDXGIAdapter1* pAdapter)
{
	ComPtr<ID3D12Device> testDevice;
	UUID experimentalFeatures[] = { D3D12ExperimentalShaderModels, D3D12StateObjectsExperiment };

	return SUCCEEDED(D3D12EnableExperimentalFeatures(static_cast<uint32_t>(size(experimentalFeatures)), experimentalFeatures, nullptr, nullptr))
		&& SUCCEEDED(D3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&testDevice)));
}

// Returns uint8_t for the device supports DirectX Raytracing tier.
inline uint8_t GetDirectXRaytracingSupportLevel(ID3D12Device* pDevice)
{
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 featureSupportData = {};

	uint8_t dxrSupport = 0;
	if (SUCCEEDED(pDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &featureSupportData, sizeof(featureSupportData))))
	{
		dxrSupport |= featureSupportData.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0 ? MultiRayCaster::RT_PIPELINE : 0;
		dxrSupport |= featureSupportData.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1 ? MultiRayCaster::RT_INLINE : 0;
	}

	return dxrSupport;
}

// Returns bool whether the device supports DirectX work-graph tier.
inline bool GetDirectXWorkGraphSupport(ID3D12Device* pDevice)
{
	D3D12_FEATURE_DATA_D3D12_OPTIONS_EXPERIMENTAL featureSupportData = {};

	auto workGraphSupport = false;
	if (SUCCEEDED(pDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS_EXPERIMENTAL, &featureSupportData, sizeof(featureSupportData))))
		workGraphSupport = featureSupportData.WorkGraphsTier;

	return workGraphSupport;
}

void MultiVolumes::EnableDirectXRaytracingAndWorkGraph(IDXGIAdapter1* pAdapter)
{
	// Fallback Layer uses an experimental feature and needs to be enabled before creating a D3D12 device.
	const auto isExperimentalFeaturesSupported = EnableExperimentalFeatures(pAdapter);

	if (!isExperimentalFeaturesSupported)
	{
		OutputDebugString(
			L"Warning: Could not enable Compute Raytracing Fallback (D3D12EnableExperimentalFeatures() failed).\n" \
			L"         Possible reasons: your OS is not in developer mode.\n\n");
	}

	ComPtr<ID3D12Device> testDevice;
	if (SUCCEEDED(D3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&testDevice))))
	{
		m_dxrSupport = GetDirectXRaytracingSupportLevel(testDevice.Get());
		m_workGraphSupport = GetDirectXWorkGraphSupport(testDevice.Get());
	}
	else
	{
		m_dxrSupport = 0;
		m_workGraphSupport = false;
	}

	if (!m_dxrSupport)
	{
		OutputDebugString(L"Warning: DirectX Raytracing is not supported by your GPU and driver.\n\n");

		if (!isExperimentalFeaturesSupported)
			OutputDebugString(L"Could not enable compute based fallback raytracing support (D3D12EnableExperimentalFeatures() failed).\n"\
				L"Possible reasons: your OS is not in developer mode.\n\n");
		ThrowIfFailed(isExperimentalFeaturesSupported ? S_OK : E_FAIL);
	}
}
