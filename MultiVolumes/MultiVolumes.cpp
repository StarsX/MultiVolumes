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

using namespace std;
using namespace XUSG;
using namespace XUSG::RayTracing;

enum RenderMethod
{
	RAY_MARCH_MERGED,
	RAY_MARCH_SEPARATE,
	RAY_MARCH_DIRECT_MERGED,
	RAY_MARCH_DIRECT_SEPARATE,

	NUM_RENDER_METHOD
};

const float g_FOVAngleY = XM_PIDIV4;

RenderMethod g_renderMethod = RAY_MARCH_SEPARATE;
const auto g_rtFormat = Format::B8G8R8A8_UNORM;
const auto g_dsFormat = Format::D32_FLOAT;

MultiVolumes::MultiVolumes(uint32_t width, uint32_t height, std::wstring name) :
	DXFramework(width, height, name),
	m_frameIndex(0),
	m_showFPS(true),
	m_isPaused(false),
	m_tracking(false),
	m_gridSize(128),
	m_lightGridSize(512),
	m_volumeFile(L""),
	m_meshFileName("Media/bunny.obj"),
	m_meshPosScale(0.0f, -10.0f, 0.0f, 1.5f)
{
#if defined (_DEBUG)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	AllocConsole();
	FILE* stream;
	freopen_s(&stream, "CONOUT$", "w+t", stdout);
	freopen_s(&stream, "CONIN$", "r+t", stdin);
#endif
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
		EnableDirectXRaytracing(dxgiAdapter.get());

		m_device = RayTracing::Device::MakeShared();
		hr = m_device->Create(dxgiAdapter.get(), D3D_FEATURE_LEVEL_11_0);
		N_RETURN(m_device->CreateInterface(createDeviceFlags), ThrowIfFailed(E_FAIL));
	}

	dxgiAdapter->GetDesc1(&dxgiAdapterDesc);
	if (dxgiAdapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
		m_title += dxgiAdapterDesc.VendorId == 0x1414 && dxgiAdapterDesc.DeviceId == 0x8c ? L" (WARP)" : L" (Software)";
	ThrowIfFailed(hr);

	// Create the command queue.
	m_commandQueue = CommandQueue::MakeUnique();
	N_RETURN(m_commandQueue->Create(m_device.get(), CommandListType::DIRECT, CommandQueueFlag::NONE,
		0, 0, L"CommandQueue"), ThrowIfFailed(E_FAIL));

	// Create the swap chain.
	CreateSwapchain();

	// Reset the index to the current back buffer.
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// Create a command allocator for each frame.
	for (uint8_t n = 0; n < FrameCount; ++n)
	{
		m_commandAllocators[n] = CommandAllocator::MakeUnique();
		N_RETURN(m_commandAllocators[n]->Create(m_device.get(), CommandListType::DIRECT,
			(L"CommandAllocator" + to_wstring(n)).c_str()), ThrowIfFailed(E_FAIL));
	}

	// Create descriptor table cache.
	m_descriptorTableCache = DescriptorTableCache::MakeShared(m_device.get(), L"DescriptorTableCache");
}

// Load the sample assets.
void MultiVolumes::LoadAssets()
{
	// Create the command list.
	m_commandList = RayTracing::CommandList::MakeUnique();
	const auto pCommandList = m_commandList.get();
	N_RETURN(pCommandList->Create(m_device.get(), 0, CommandListType::DIRECT,
		m_commandAllocators[m_frameIndex].get(), nullptr), ThrowIfFailed(E_FAIL));

	// Create ray tracing interfaces
	N_RETURN(m_commandList->CreateInterface(m_device.get()), ThrowIfFailed(E_FAIL));

	vector<Resource::uptr> uploaders(0);
	//m_descriptorTableCache->AllocateDescriptorPool(CBV_SRV_UAV_POOL, 100, 0);
	m_objectRenderer = make_unique<ObjectRenderer>(m_device);
	if (!m_objectRenderer) ThrowIfFailed(E_FAIL);
	if (!m_objectRenderer->Init(m_commandList.get(), m_width, m_height, m_descriptorTableCache,
		uploaders, m_meshFileName.c_str(), g_rtFormat, g_dsFormat, m_meshPosScale))
		ThrowIfFailed(E_FAIL);

	const auto numVolumes = 1u;
	const auto numVolumeSrcs = 1u;

	GeometryBuffer geometry;
	m_rayCaster = make_unique<MultiRayCaster>(m_device);
	if (!m_rayCaster) ThrowIfFailed(E_FAIL);
	if (!m_rayCaster->Init(pCommandList, m_descriptorTableCache, g_rtFormat, m_gridSize, m_lightGridSize,
		numVolumes, numVolumeSrcs, m_objectRenderer->GetDepthMaps(), uploaders, &geometry))
		ThrowIfFailed(E_FAIL);

	if (m_volumeFile.empty())
	{
		for (auto i = 0u; i < numVolumeSrcs; ++i)
			m_rayCaster->InitVolumeData(pCommandList, i);
	}
	else
	{
		for (auto i = 0u; i < numVolumeSrcs; ++i)
			m_rayCaster->LoadVolumeData(pCommandList, i, m_volumeFile.c_str(), uploaders);
	}

	// Close the command list and execute it to begin the initial GPU setup.
	N_RETURN(pCommandList->Close(), ThrowIfFailed(E_FAIL));
	m_commandQueue->ExecuteCommandList(pCommandList);

	// Create synchronization objects and wait until assets have been uploaded to the GPU.
	{
		if (!m_fence)
		{
			m_fence = Fence::MakeUnique();
			N_RETURN(m_fence->Create(m_device.get(), m_fenceValues[m_frameIndex]++, FenceFlag::NONE, L"Fence"), ThrowIfFailed(E_FAIL));
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
	//m_descriptorTableCache->ResetDescriptorPool(CBV_SRV_UAV_POOL, 0);
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
		m_eyePt = XMFLOAT3(4.0f, 16.0f, -40.0f);
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
	N_RETURN(m_swapChain->Create(m_factory.Get(), Win32Application::GetHwnd(), m_commandQueue.get(),
		FrameCount, m_width, m_height, g_rtFormat), ThrowIfFailed(E_FAIL));

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
		N_RETURN(m_renderTargets[n]->CreateFromSwapChain(m_device.get(), m_swapChain.get(), n), ThrowIfFailed(E_FAIL));
	}

	N_RETURN(m_objectRenderer->SetViewport(m_width, m_height, g_dsFormat), ThrowIfFailed(E_FAIL));
	N_RETURN(m_rayCaster->SetDepthMaps(m_objectRenderer->GetDepthMaps()), ThrowIfFailed(E_FAIL));

	// Set the 3D rendering viewport and scissor rectangle to target the entire window.
	//m_viewport = Viewport(0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height));
	//m_scissorRect = RectRange(0, 0, m_width, m_height);
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

	m_rayCaster->SetLightMapWorld(40.0f, XMFLOAT3(0.0f, 0.0f, 0.0f));

	// View
	//const auto eyePt = XMLoadFloat3(&m_eyePt);
	const auto view = XMLoadFloat4x4(&m_view);
	const auto proj = XMLoadFloat4x4(&m_proj);
	const auto viewProj = view * proj;
	m_objectRenderer->UpdateFrame(m_frameIndex, viewProj, m_eyePt);
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
	N_RETURN(m_swapChain->Present(0, 0), ThrowIfFailed(E_FAIL));

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
	m_descriptorTableCache->ResetDescriptorPool(CBV_SRV_UAV_POOL, 0);
	//m_descriptorTableCache->ResetDescriptorPool(RTV_POOL, 0);

	// Determine the render target size in pixels.
	m_width = (max)(width, 1);
	m_height = (max)(height, 1);

	// If the swap chain already exists, resize it, otherwise create one.
	if (m_swapChain)
	{
		// If the swap chain already exists, resize it.
		const auto hr = m_swapChain->ResizeBuffers(FrameCount, m_width, m_height, g_rtFormat, 0);

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
	case VK_LEFT:
		g_renderMethod = static_cast<RenderMethod>((g_renderMethod + NUM_RENDER_METHOD - 1) % NUM_RENDER_METHOD);
		break;
	case VK_RIGHT:
		g_renderMethod = static_cast<RenderMethod>((g_renderMethod + 1) % NUM_RENDER_METHOD);
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
		if (_wcsnicmp(argv[i], L"-gridSize", wcslen(argv[i])) == 0 ||
			_wcsnicmp(argv[i], L"/gridSize", wcslen(argv[i])) == 0)
			m_gridSize = ++i < argc ? static_cast<uint32_t>(_wtof(argv[i])) : m_gridSize;
		else if (_wcsnicmp(argv[i], L"-volume", wcslen(argv[i])) == 0 ||
			_wcsnicmp(argv[i], L"/volume", wcslen(argv[i])) == 0)
		{
			m_volumeFile = ++i < argc ? argv[i] : m_volumeFile;
		}
	}
}

void MultiVolumes::PopulateCommandList()
{
	// Command list allocators can only be reset when the associated 
	// command lists have finished execution on the GPU; apps should use 
	// fences to determine GPU execution progress.
	const auto pCommandAllocator = m_commandAllocators[m_frameIndex].get();
	N_RETURN(pCommandAllocator->Reset(), ThrowIfFailed(E_FAIL));

	// However, when ExecuteCommandList() is called on a particular command 
	// list, that command list can then be reset at any time and must be before 
	// re-recording.
	const auto pCommandList = m_commandList.get();
	N_RETURN(pCommandList->Reset(pCommandAllocator, nullptr), ThrowIfFailed(E_FAIL));

	// Record commands.
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
	};
	pCommandList->SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	static auto isFirstFrame = true;
	if (isFirstFrame)
	{
		m_objectRenderer->RenderShadow(pCommandList, m_frameIndex);
		isFirstFrame = false;
	}

	ResourceBarrier barriers[3];
	const auto pDepth = m_objectRenderer->GetDepthMap(ObjectRenderer::DEPTH_MAP);
	const auto pShadow = m_objectRenderer->GetDepthMap(ObjectRenderer::SHADOW_MAP);
	auto numBarriers = m_renderTargets[m_frameIndex]->SetBarrier(barriers, ResourceState::RENDER_TARGET);
	numBarriers = pDepth->SetBarrier(barriers, ResourceState::DEPTH_WRITE, numBarriers);
	numBarriers = pShadow->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE | ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
	pCommandList->Barrier(numBarriers, barriers);

	// Clear render target
	const float clearColor[4] = { 0.2f, 0.2f, 0.2f, 0.0f };
	pCommandList->ClearRenderTargetView(m_renderTargets[m_frameIndex]->GetRTV(), clearColor);
	pCommandList->ClearDepthStencilView(pDepth->GetDSV(), ClearFlag::DEPTH, 1.0f);

	pCommandList->OMSetRenderTargets(1, &m_renderTargets[m_frameIndex]->GetRTV(), &pDepth->GetDSV());

	// Set viewport
	Viewport viewport(0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height));
	RectRange scissorRect(0, 0, m_width, m_height);
	pCommandList->RSSetViewports(1, &viewport);
	pCommandList->RSSetScissorRects(1, &scissorRect);

	m_objectRenderer->Render(pCommandList, m_frameIndex);
	m_rayCaster->Render(pCommandList, m_frameIndex);
	
	// Indicate that the back buffer will now be used to present.
	numBarriers = m_renderTargets[m_frameIndex]->SetBarrier(barriers, ResourceState::PRESENT);
	pCommandList->Barrier(numBarriers, barriers);

	N_RETURN(pCommandList->Close(), ThrowIfFailed(E_FAIL));
}

// Wait for pending GPU work to complete.
void MultiVolumes::WaitForGpu()
{
	// Schedule a Signal command in the queue.
	N_RETURN(m_commandQueue->Signal(m_fence.get(), m_fenceValues[m_frameIndex]), ThrowIfFailed(E_FAIL));

	// Wait until the fence has been processed.
	N_RETURN(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent), ThrowIfFailed(E_FAIL));
	WaitForSingleObject(m_fenceEvent, INFINITE);

	// Increment the fence value for the current frame.
	m_fenceValues[m_frameIndex]++;
}

// Prepare to render the next frame.
void MultiVolumes::MoveToNextFrame()
{
	// Schedule a Signal command in the queue.
	const auto currentFenceValue = m_fenceValues[m_frameIndex];
	N_RETURN(m_commandQueue->Signal(m_fence.get(), currentFenceValue), ThrowIfFailed(E_FAIL));

	// Update the frame index.
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// If the next frame is not ready to be rendered yet, wait until it is ready.
	if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
	{
		N_RETURN(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent), ThrowIfFailed(E_FAIL));
		WaitForSingleObject(m_fenceEvent, INFINITE);
	}

	// Set the fence value for the next frame.
	m_fenceValues[m_frameIndex] = currentFenceValue + 1;
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

		windowText << L"    [\x2190][\x2192] ";
		switch (g_renderMethod)
		{
		case RAY_MARCH_MERGED:
			windowText << L"Cubemap-space ray marching without separate lighting pass";
			break;
		case RAY_MARCH_SEPARATE:
			windowText << L"Cubemap-space ray marching with separate lighting pass";
			break;
		case RAY_MARCH_DIRECT_MERGED:
			windowText << L"Direct screen-space ray marching without separate lighting pass";
			break;
		case RAY_MARCH_DIRECT_SEPARATE:
			windowText << L"Direct screen-space ray marching with separate lighting pass";
			break;
		}

		SetCustomWindowText(windowText.str().c_str());
	}

	if (pTimeStep)* pTimeStep = static_cast<float>(totalTime - previousTime);
	previousTime = totalTime;

	return totalTime;
}

//--------------------------------------------------------------------------------------
// Ray tracing
//--------------------------------------------------------------------------------------

// Enable experimental features required for compute-based raytracing fallback.
// This will set active D3D12 devices to DEVICE_REMOVED state.
// Returns bool whether the call succeeded and the device supports the feature.
inline bool EnableComputeRaytracingFallback(IDXGIAdapter1* adapter)
{
	ComPtr<ID3D12Device> testDevice;
	UUID experimentalFeatures[] = { D3D12ExperimentalShaderModels };

	return SUCCEEDED(D3D12EnableExperimentalFeatures(1, experimentalFeatures, nullptr, nullptr))
		&& SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&testDevice)));
}

// Returns bool whether the device supports DirectX Raytracing tier.
inline bool IsDirectXRaytracingSupported(IDXGIAdapter1* adapter)
{
	ComPtr<ID3D12Device> testDevice;
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 featureSupportData = {};

	return SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&testDevice)))
		&& SUCCEEDED(testDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &featureSupportData, sizeof(featureSupportData)))
		&& featureSupportData.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
}

void MultiVolumes::EnableDirectXRaytracing(IDXGIAdapter1* adapter)
{
	// Fallback Layer uses an experimental feature and needs to be enabled before creating a D3D12 device.
	bool isFallbackSupported = EnableComputeRaytracingFallback(adapter);

	if (!isFallbackSupported)
	{
		OutputDebugString(
			L"Warning: Could not enable Compute Raytracing Fallback (D3D12EnableExperimentalFeatures() failed).\n" \
			L"         Possible reasons: your OS is not in developer mode.\n\n");
	}

	m_isDxrSupported = IsDirectXRaytracingSupported(adapter);

	if (!m_isDxrSupported)
	{
		OutputDebugString(L"Warning: DirectX Raytracing is not supported by your GPU and driver.\n\n");

		if (!isFallbackSupported)
			OutputDebugString(L"Could not enable compute based fallback raytracing support (D3D12EnableExperimentalFeatures() failed).\n"\
				L"Possible reasons: your OS is not in developer mode.\n\n");
		ThrowIfFailed(isFallbackSupported ? S_OK : E_FAIL);
	}
}
