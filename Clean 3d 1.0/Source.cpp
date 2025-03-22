#define NOMINMAX
#include <windows.h>
#include <sal.h>
#include <gdiplus.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <string>
#include <memory>
#include <exception>
#include <d3dx12.h>
#include <dxgidebug.h>
#include <wrl.h>
#include <fstream>
#include <d3d11.h>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3d11.lib")

#define CHECK_HR(hr, msg) \
    if (FAILED(hr)) { \
        char debugMsg[256]; \
        sprintf_s(debugMsg, "%s failed, HRESULT: 0x%08X\n", msg, hr); \
        Log(debugMsg); \
        throw ToolException(msg, hr); \
    }

#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }

static UINT SCREEN_WIDTH = 4096;
static UINT SCREEN_HEIGHT = 2160;
const UINT TARGET_FPS = 60;
const UINT FRAME_COUNT = 2;

std::ofstream logFile("debug_log.txt", std::ios::app);

void Log(const char* msg) {
    logFile << msg;
    logFile.flush();
    OutputDebugStringA(msg);
}

#pragma pack(push, 1)
struct IllusionConfig {
    float depth_intensity;
    float parallax_strength;
    float alpha;
    float edge_depth_influence;
    float color_separation;
    float perspective_strength;
    uint8_t enable_gpu;
    int32_t processing_quality;
    uint8_t enable_chromatic;
    uint8_t enable_parallax;
    uint8_t enable_dof;
    uint8_t padding[96];
};
#pragma pack(pop)
static_assert(sizeof(IllusionConfig) == 128, "IllusionConfig must be 128 bytes");

static IllusionConfig config = {
    90.0f, 2.5f, 200.34f, 10.0f, 0.8f, 0.7f,
    1, 4, 1, 1, 1,
    {0}
};

class ToolException : public std::exception {
public:
    ToolException(const char* msg, HRESULT hr = S_OK) : m_message(msg), m_hr(hr) {
        char debugMsg[256];
        sprintf_s(debugMsg, "ToolException: %s (HRESULT: 0x%08X)\n", msg, hr);
        Log(debugMsg);
    }
    const char* what() const noexcept override { return m_message.c_str(); }
    HRESULT GetHR() const { return m_hr; }
private:
    std::string m_message;
    HRESULT m_hr;
};

class D3D12Renderer {
public:
    D3D12Renderer() : m_device(nullptr), m_commandQueue(nullptr), m_swapChain(nullptr),
        m_frameIndex(0), m_fenceValue(0), m_fence(nullptr), m_fenceEvent(nullptr),
        m_commandList(nullptr), m_computePso(nullptr), m_constantBuffer(nullptr),
        m_depthTexture(nullptr), m_graphicsPso(nullptr), m_rootSignature(nullptr),
        m_rtvHeap(nullptr), m_screenTexture(nullptr), m_srvHeap(nullptr),
        m_samplerHeap(nullptr), m_vertexBuffer(nullptr), m_rtvDescriptorSize(0),
        m_featureLevel(D3D_FEATURE_LEVEL_12_0), m_adapter(nullptr), m_factory(nullptr),
        m_d3d11Device(nullptr), m_d3d11Context(nullptr), m_d3d11Duplication(nullptr),
        m_d3d11StagingTexture(nullptr), m_d3d12UploadBuffer(nullptr) {
        for (UINT i = 0; i < FRAME_COUNT; i++) {
            m_renderTargets[i] = nullptr;
            m_commandAllocators[i] = nullptr;
        }
        Log("D3D12Renderer constructor called\n");
    }

    ~D3D12Renderer() { Cleanup(); }

    bool Initialize(HWND hwnd) {
        Log("Initializing D3D12Renderer - Using system DXGI with D3D11 capture\n");

        try {
            HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory));
            CHECK_HR(hr, "CreateDXGIFactory2");

            Log("Attempting to upgrade to IDXGIFactory6\n");
            IDXGIFactory6* factory6 = nullptr;
            hr = m_factory->QueryInterface(IID_PPV_ARGS(&factory6));
            if (SUCCEEDED(hr)) {
                SAFE_RELEASE(m_factory);
                m_factory = factory6;
                Log("Upgraded to IDXGIFactory6 successfully\n");
            }

#ifdef _DEBUG
            ID3D12Debug1* debugController;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
                debugController->EnableDebugLayer();
                debugController->SetEnableGPUBasedValidation(true);
                debugController->SetEnableSynchronizedCommandQueueValidation(true);
                SAFE_RELEASE(debugController);
                Log("D3D12 debug layer enabled\n");
            }
#endif

            Log("Enumerating hardware adapters\n");
            IDXGIAdapter1* tempAdapter = nullptr;
            UINT adapterIndex = 0;
            while (m_factory->EnumAdapters1(adapterIndex, &tempAdapter) != DXGI_ERROR_NOT_FOUND) {
                DXGI_ADAPTER_DESC1 desc;
                tempAdapter->GetDesc1(&desc);
                char adapterMsg[256];
                sprintf_s(adapterMsg, "Found adapter: %S\n", desc.Description);
                Log(adapterMsg);
                if (desc.VendorId == 0x10DE && desc.DeviceId == 0x1F08) {
                    SAFE_RELEASE(m_adapter);
                    m_adapter = tempAdapter;
                    Log("Selected NVIDIA RTX 2060 adapter\n");
                    break;
                }
                if (!m_adapter && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    m_adapter = tempAdapter;
                }
                else {
                    SAFE_RELEASE(tempAdapter);
                }
                adapterIndex++;
            }
            if (!m_adapter) throw ToolException("No suitable hardware adapter found");

            D3D_FEATURE_LEVEL featureLevels[] = {
                D3D_FEATURE_LEVEL_12_1,
                D3D_FEATURE_LEVEL_12_0,
                D3D_FEATURE_LEVEL_11_1,
                D3D_FEATURE_LEVEL_11_0
            };
            for (auto level : featureLevels) {
                hr = D3D12CreateDevice(m_adapter, level, IID_PPV_ARGS(&m_device));
                if (SUCCEEDED(hr)) {
                    m_featureLevel = level;
                    char levelMsg[64];
                    sprintf_s(levelMsg, "Created device with feature level: %d\n", (int)level);
                    Log(levelMsg);
                    break;
                }
            }
            if (!m_device) throw ToolException("D3D12 device creation failed", hr);

            D3D12_COMMAND_QUEUE_DESC queueDesc = {};
            queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            CHECK_HR(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)), "CreateCommandQueue");

            DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
            swapChainDesc.Width = SCREEN_WIDTH;
            swapChainDesc.Height = SCREEN_HEIGHT;
            swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            swapChainDesc.BufferCount = FRAME_COUNT;
            swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            swapChainDesc.SampleDesc.Count = 1;
            IDXGISwapChain1* swapChain1 = nullptr;
            CHECK_HR(m_factory->CreateSwapChainForHwnd(m_commandQueue, hwnd, &swapChainDesc, nullptr, nullptr, &swapChain1), "CreateSwapChainForHwnd");
            CHECK_HR(swapChain1->QueryInterface(IID_PPV_ARGS(&m_swapChain)), "SwapChain QueryInterface");
            SAFE_RELEASE(swapChain1);

            D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
            rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            rtvHeapDesc.NumDescriptors = FRAME_COUNT;
            rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            CHECK_HR(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)), "Create RTV Heap");

            D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
            srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            srvHeapDesc.NumDescriptors = 3;
            srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            CHECK_HR(m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap)), "Create SRV Heap");

            D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc = {};
            samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            samplerHeapDesc.NumDescriptors = 1;
            samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            CHECK_HR(m_device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&m_samplerHeap)), "Create Sampler Heap");

            m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
            for (UINT i = 0; i < FRAME_COUNT; i++) {
                hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
                if (FAILED(hr) || !m_renderTargets[i]) {
                    char errMsg[64];
                    sprintf_s(errMsg, "GetSwapChainBuffer %u failed, HRESULT: 0x%08X\n", i, hr);
                    Log(errMsg);
                    throw ToolException(errMsg, hr);
                }
                m_device->CreateRenderTargetView(m_renderTargets[i], nullptr, rtvHandle);
                rtvHandle.Offset(1, m_rtvDescriptorSize);
            }

            for (UINT i = 0; i < FRAME_COUNT; i++) {
                CHECK_HR(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i])), "CreateCommandAllocator");
            }

            CHECK_HR(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[0], nullptr, IID_PPV_ARGS(&m_commandList)), "CreateCommandList");
            CHECK_HR(m_commandList->Close(), "Close initial CommandList");

            CHECK_HR(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "CreateFence");
            m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (!m_fenceEvent) throw ToolException("Fence event creation failed", HRESULT_FROM_WIN32(GetLastError()));

            if (!CreateResources()) {
                Log("CreateResources failed, aborting initialization\n");
                throw ToolException("Resource creation failed");
            }
            if (!CreatePipelines()) {
                Log("CreatePipelines failed, aborting initialization\n");
                throw ToolException("Pipeline creation failed");
            }

            SAFE_RELEASE(m_adapter);
            SAFE_RELEASE(m_factory);
            Log("D3D12Renderer initialized successfully\n");
            return true;
        }
        catch (const ToolException& e) {
            char debugMsg[256];
            sprintf_s(debugMsg, "ToolException in Initialize: %s (HRESULT: 0x%08X)\n", e.what(), e.GetHR());
            Log(debugMsg);
            logFile.flush();
            Cleanup();
            return false;
        }
    }

    void Cleanup() {
        if (m_commandQueue && m_fence) WaitForGPU();
        for (UINT i = 0; i < FRAME_COUNT; i++) {
            SAFE_RELEASE(m_renderTargets[i]);
            SAFE_RELEASE(m_commandAllocators[i]);
        }
        SAFE_RELEASE(m_screenTexture);
        SAFE_RELEASE(m_depthTexture);
        SAFE_RELEASE(m_constantBuffer);
        SAFE_RELEASE(m_vertexBuffer);
        SAFE_RELEASE(m_d3d11Duplication);
        SAFE_RELEASE(m_d3d11StagingTexture);
        SAFE_RELEASE(m_d3d12UploadBuffer);
        SAFE_RELEASE(m_computePso);
        SAFE_RELEASE(m_graphicsPso);
        SAFE_RELEASE(m_rootSignature);
        SAFE_RELEASE(m_commandList);
        SAFE_RELEASE(m_srvHeap);
        SAFE_RELEASE(m_samplerHeap);
        SAFE_RELEASE(m_rtvHeap);
        SAFE_RELEASE(m_swapChain);
        SAFE_RELEASE(m_commandQueue);
        SAFE_RELEASE(m_device);
        SAFE_RELEASE(m_fence);
        if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
        SAFE_RELEASE(m_adapter);
        SAFE_RELEASE(m_factory);
        SAFE_RELEASE(m_d3d11Device);
        SAFE_RELEASE(m_d3d11Context);
        Log("D3D12Renderer cleaned up\n");
    }

    bool CreateResources() {
        Log("Entering CreateResources\n");
        if (!m_device || !m_adapter) {
            Log("CreateResources: Device or adapter not initialized\n");
            return false;
        }

        Log("Creating screen texture\n");
        D3D12_HEAP_PROPERTIES defaultHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, SCREEN_WIDTH, SCREEN_HEIGHT, 1, 1);
        HRESULT hr = m_device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_screenTexture));
        if (FAILED(hr)) {
            char debugMsg[256];
            sprintf_s(debugMsg, "Create screen texture failed, HRESULT: 0x%08X\n", hr);
            Log(debugMsg);
            return false;
        }

        Log("Initializing D3D11 capture\n");
        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels, 1, D3D11_SDK_VERSION,
            &m_d3d11Device, nullptr, &m_d3d11Context);
        if (FAILED(hr)) {
            char debugMsg[256];
            sprintf_s(debugMsg, "D3D11 device creation failed, HRESULT: 0x%08X\n", hr);
            Log(debugMsg);
            return false;
        }

        IDXGIFactory1* factory = nullptr;
        hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            Log("DXGI factory creation for D3D11 failed\n");
            return false;
        }

        IDXGIAdapter* adapter = nullptr;
        factory->EnumAdapters(0, &adapter);
        IDXGIOutput* output = nullptr;
        adapter->EnumOutputs(0, &output);
        IDXGIOutput1* output1 = nullptr;
        hr = output->QueryInterface(IID_PPV_ARGS(&output1));
        if (FAILED(hr) || !output1) {
            char debugMsg[256];
            sprintf_s(debugMsg, "QueryInterface for IDXGIOutput1 failed, HRESULT: 0x%08X\n", hr);
            Log(debugMsg);
            SAFE_RELEASE(output);
            SAFE_RELEASE(adapter);
            SAFE_RELEASE(factory);
            return false;
        }

        hr = output1->DuplicateOutput(m_d3d11Device, &m_d3d11Duplication);
        if (FAILED(hr)) {
            char debugMsg[256];
            sprintf_s(debugMsg, "D3D11 DuplicateOutput failed, HRESULT: 0x%08X\n", hr);
            Log(debugMsg);
            Log("Falling back to checkerboard texture\n");
            SAFE_RELEASE(m_d3d11Duplication);
            SAFE_RELEASE(output1);
            SAFE_RELEASE(output);
            SAFE_RELEASE(adapter);
            SAFE_RELEASE(factory);

            D3D12_HEAP_PROPERTIES uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(SCREEN_WIDTH * SCREEN_HEIGHT * 4);
            hr = m_device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_d3d12UploadBuffer));
            if (FAILED(hr)) {
                char debugMsg[256];
                sprintf_s(debugMsg, "Create upload buffer failed, HRESULT: 0x%08X\n", hr);
                Log(debugMsg);
                return false;
            }

            UINT8* data;
            hr = m_d3d12UploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&data));
            if (SUCCEEDED(hr)) {
                CreateCheckerboardPattern(reinterpret_cast<uint32_t*>(data));
                m_d3d12UploadBuffer->Unmap(0, nullptr);
            }

            hr = m_commandList->Reset(m_commandAllocators[0], nullptr);
            CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_screenTexture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            m_commandList->ResourceBarrier(1, &barrier);
            D3D12_SUBRESOURCE_DATA subresourceData = { data, static_cast<LONG_PTR>(SCREEN_WIDTH * 4), static_cast<LONG_PTR>(SCREEN_WIDTH * SCREEN_HEIGHT * 4) };
            ::UpdateSubresources<1>(m_commandList, m_screenTexture, m_d3d12UploadBuffer, 0, 0, 1, &subresourceData);
            barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_screenTexture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            m_commandList->ResourceBarrier(1, &barrier);
            m_commandList->Close();
            ID3D12CommandList* cmdLists[] = { m_commandList };
            m_commandQueue->ExecuteCommandLists(1, cmdLists);
            WaitForGPU();
        }
        else {
            DXGI_OUTDUPL_DESC duplDesc;
            m_d3d11Duplication->GetDesc(&duplDesc);
            SCREEN_WIDTH = duplDesc.ModeDesc.Width;
            SCREEN_HEIGHT = duplDesc.ModeDesc.Height;
            char duplMsg[128];
            sprintf_s(duplMsg, "D3D11 desktop duplication initialized: %ux%u\n", SCREEN_WIDTH, SCREEN_HEIGHT);
            Log(duplMsg);

            // Create D3D11 staging texture
            D3D11_TEXTURE2D_DESC stagingDesc = {};
            stagingDesc.Width = SCREEN_WIDTH;
            stagingDesc.Height = SCREEN_HEIGHT;
            stagingDesc.MipLevels = 1;
            stagingDesc.ArraySize = 1;
            stagingDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            stagingDesc.SampleDesc.Count = 1;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            hr = m_d3d11Device->CreateTexture2D(&stagingDesc, nullptr, &m_d3d11StagingTexture);
            if (FAILED(hr)) {
                char debugMsg[256];
                sprintf_s(debugMsg, "Create D3D11 staging texture failed, HRESULT: 0x%08X\n", hr);
                Log(debugMsg);
                SAFE_RELEASE(output1);
                SAFE_RELEASE(output);
                SAFE_RELEASE(adapter);
                SAFE_RELEASE(factory);
                return false;
            }

            // Create D3D12 upload buffer
            D3D12_HEAP_PROPERTIES uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(SCREEN_WIDTH * SCREEN_HEIGHT * 4);
            hr = m_device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_d3d12UploadBuffer));
            if (FAILED(hr)) {
                char debugMsg[256];
                sprintf_s(debugMsg, "Create D3D12 upload buffer failed, HRESULT: 0x%08X\n", hr);
                Log(debugMsg);
                SAFE_RELEASE(m_d3d11StagingTexture);
                SAFE_RELEASE(output1);
                SAFE_RELEASE(output);
                SAFE_RELEASE(adapter);
                SAFE_RELEASE(factory);
                return false;
            }

            SAFE_RELEASE(output1);
            SAFE_RELEASE(output);
            SAFE_RELEASE(adapter);
            SAFE_RELEASE(factory);
        }

        Log("Creating depth texture\n");
        D3D12_RESOURCE_DESC depthTexDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, SCREEN_WIDTH, SCREEN_HEIGHT, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        CHECK_HR(m_device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &depthTexDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_depthTexture)), "Create depth texture");

        UINT descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart());
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_screenTexture, &srvDesc, srvHandle);

        srvHandle.Offset(1, descriptorSize);
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        m_device->CreateShaderResourceView(m_depthTexture, &srvDesc, srvHandle);

        srvHandle.Offset(1, descriptorSize);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        m_device->CreateUnorderedAccessView(m_depthTexture, nullptr, &uavDesc, srvHandle);

        CD3DX12_CPU_DESCRIPTOR_HANDLE samplerHandle(m_samplerHeap->GetCPUDescriptorHandleForHeapStart());
        D3D12_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        m_device->CreateSampler(&samplerDesc, samplerHandle);

        D3D12_HEAP_PROPERTIES uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC cbDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(IllusionConfig));
        CHECK_HR(m_device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_constantBuffer)), "Create constant buffer");
        void* mappedData;
        CHECK_HR(m_constantBuffer->Map(0, nullptr, &mappedData), "Map constant buffer");
        memcpy(mappedData, &config, sizeof(IllusionConfig));
        m_constantBuffer->Unmap(0, nullptr);

        struct Vertex { float x, y, z, u, v; };
        Vertex vertices[] = { {-1.0f, 1.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f, 1.0f, 0.0f}, {-1.0f, -1.0f, 0.0f, 0.0f, 1.0f}, {1.0f, -1.0f, 0.0f, 1.0f, 1.0f} };
        D3D12_RESOURCE_DESC vbDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(vertices));
        CHECK_HR(m_device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &vbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_vertexBuffer)), "Create vertex buffer");
        CHECK_HR(m_vertexBuffer->Map(0, nullptr, &mappedData), "Map vertex buffer");
        memcpy(mappedData, vertices, sizeof(vertices));
        m_vertexBuffer->Unmap(0, nullptr);

        Log("CreateResources completed successfully\n");
        return true;
    }

    void CreateCheckerboardPattern(uint32_t* data) {
        const uint32_t white = 0xFFFFFFFF;
        const uint32_t gray = 0xFF808080;
        const int squareSize = 32;

        for (UINT y = 0; y < SCREEN_HEIGHT; y++) {
            for (UINT x = 0; x < SCREEN_WIDTH; x++) {
                bool isWhiteSquare = ((x / squareSize) + (y / squareSize)) % 2 == 0;
                data[y * SCREEN_WIDTH + x] = isWhiteSquare ? white : gray;
            }
        }
    }

    bool CreatePipelines() {
        Log("Entering CreatePipelines\n");
        if (!m_device) {
            Log("CreatePipelines: Device not initialized\n");
            return false;
        }

        CD3DX12_ROOT_PARAMETER rootParams[4] = {};
        rootParams[0].InitAsConstantBufferView(0);
        CD3DX12_DESCRIPTOR_RANGE srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);
        rootParams[1].InitAsDescriptorTable(1, &srvRange);
        CD3DX12_DESCRIPTOR_RANGE uavRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
        rootParams[2].InitAsDescriptorTable(1, &uavRange);
        CD3DX12_DESCRIPTOR_RANGE samplerRange(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0);
        rootParams[3].InitAsDescriptorTable(1, &samplerRange);

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(4, rootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        ID3DBlob* signature = nullptr;
        ID3DBlob* error = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
        if (FAILED(hr)) {
            if (error) {
                Log((const char*)error->GetBufferPointer());
                SAFE_RELEASE(error);
            }
            SAFE_RELEASE(signature);
            throw ToolException("SerializeRootSignature failed", hr);
        }
        CHECK_HR(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)), "CreateRootSignature");
        SAFE_RELEASE(signature);

        ID3DBlob* csBlob = nullptr;
        hr = D3DCompileFromFile(L"DepthCompute.hlsl", nullptr, nullptr, "CSMain", "cs_5_0", D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION, 0, &csBlob, &error);
        if (FAILED(hr)) {
            if (error) {
                Log((const char*)error->GetBufferPointer());
                SAFE_RELEASE(error);
            }
            SAFE_RELEASE(csBlob);
            throw ToolException("Compile DepthCompute.hlsl failed", hr);
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
        computePsoDesc.pRootSignature = m_rootSignature;
        computePsoDesc.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };
        CHECK_HR(m_device->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&m_computePso)), "CreateComputePipelineState");
        SAFE_RELEASE(csBlob);

        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* psBlob = nullptr;
        hr = D3DCompileFromFile(L"VertexShader.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION, 0, &vsBlob, &error);
        if (FAILED(hr)) {
            if (error) {
                Log((const char*)error->GetBufferPointer());
                SAFE_RELEASE(error);
            }
            SAFE_RELEASE(vsBlob);
            throw ToolException("Compile VertexShader.hlsl failed", hr);
        }
        hr = D3DCompileFromFile(L"Effects.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION, 0, &psBlob, &error);
        if (FAILED(hr)) {
            if (error) {
                Log((const char*)error->GetBufferPointer());
                SAFE_RELEASE(error);
            }
            SAFE_RELEASE(psBlob);
            SAFE_RELEASE(vsBlob);
            throw ToolException("Compile Effects.hlsl failed", hr);
        }

        D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = m_rootSignature;
        psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
        psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
        psoDesc.InputLayout = { inputLayout, 2 };
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.NumRenderTargets = 1;
        psoDesc.SampleDesc.Count = 1;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        CHECK_HR(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_graphicsPso)), "CreateGraphicsPipelineState");
        SAFE_RELEASE(vsBlob);
        SAFE_RELEASE(psBlob);

        Log("CreatePipelines completed successfully\n");
        return true;
    }

    bool CaptureDesktop() {
        if (!m_d3d11Duplication || !m_commandList || !m_screenTexture || !m_commandQueue || !m_commandAllocators[m_frameIndex] || !m_d3d11StagingTexture || !m_d3d12UploadBuffer) {
            Log("CaptureDesktop: Missing resources\n");
            return false;
        }

        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        IDXGIResource* desktopResource = nullptr;
        HRESULT hr = m_d3d11Duplication->AcquireNextFrame(16, &frameInfo, &desktopResource);
        if (FAILED(hr)) {
            m_d3d11Duplication->ReleaseFrame();
            char debugMsg[256];
            sprintf_s(debugMsg, "CaptureDesktop: AcquireNextFrame failed, HRESULT: 0x%08X\n", hr);
            Log(debugMsg);
            return false;
        }

        ID3D11Texture2D* d3d11Texture = nullptr;
        hr = desktopResource->QueryInterface(IID_PPV_ARGS(&d3d11Texture));
        if (FAILED(hr) || !d3d11Texture) {
            SAFE_RELEASE(desktopResource);
            m_d3d11Duplication->ReleaseFrame();
            char debugMsg[256];
            sprintf_s(debugMsg, "CaptureDesktop: QueryInterface for D3D11 texture failed, HRESULT: 0x%08X\n", hr);
            Log(debugMsg);
            return false;
        }

        // Copy the captured texture to the staging texture
        m_d3d11Context->CopyResource(m_d3d11StagingTexture, d3d11Texture);

        // Map the staging texture to CPU memory
        D3D11_MAPPED_SUBRESOURCE mappedResource;
        hr = m_d3d11Context->Map(m_d3d11StagingTexture, 0, D3D11_MAP_READ, 0, &mappedResource);
        if (FAILED(hr)) {
            char debugMsg[256];
            sprintf_s(debugMsg, "CaptureDesktop: Map staging texture failed, HRESULT: 0x%08X\n", hr);
            Log(debugMsg);
            SAFE_RELEASE(d3d11Texture);
            SAFE_RELEASE(desktopResource);
            m_d3d11Duplication->ReleaseFrame();
            return false;
        }

        // Map the D3D12 upload buffer
        UINT8* uploadData;
        hr = m_d3d12UploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&uploadData));
        if (FAILED(hr)) {
            char debugMsg[256];
            sprintf_s(debugMsg, "CaptureDesktop: Map upload buffer failed, HRESULT: 0x%08X\n", hr);
            Log(debugMsg);
            m_d3d11Context->Unmap(m_d3d11StagingTexture, 0);
            SAFE_RELEASE(d3d11Texture);
            SAFE_RELEASE(desktopResource);
            m_d3d11Duplication->ReleaseFrame();
            return false;
        }

        // Copy data from staging texture to upload buffer
        for (UINT y = 0; y < SCREEN_HEIGHT; y++) {
            memcpy(uploadData + y * SCREEN_WIDTH * 4,
                static_cast<UINT8*>(mappedResource.pData) + y * mappedResource.RowPitch,
                SCREEN_WIDTH * 4);
        }

        // Unmap resources
        m_d3d12UploadBuffer->Unmap(0, nullptr);
        m_d3d11Context->Unmap(m_d3d11StagingTexture, 0);

        // Upload to D3D12 screen texture
        CHECK_HR(m_commandList->Reset(m_commandAllocators[m_frameIndex], nullptr), "Reset command list for capture");
        CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_screenTexture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        m_commandList->ResourceBarrier(1, &barrier);
        D3D12_SUBRESOURCE_DATA subresourceData = { uploadData, static_cast<LONG_PTR>(SCREEN_WIDTH * 4), static_cast<LONG_PTR>(SCREEN_WIDTH * SCREEN_HEIGHT * 4) };
        ::UpdateSubresources<1>(m_commandList, m_screenTexture, m_d3d12UploadBuffer, 0, 0, 1, &subresourceData);
        barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_screenTexture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_commandList->ResourceBarrier(1, &barrier);
        CHECK_HR(m_commandList->Close(), "Close command list for capture");

        ID3D12CommandList* cmdLists[] = { m_commandList };
        m_commandQueue->ExecuteCommandLists(1, cmdLists);
        WaitForGPU();

        SAFE_RELEASE(d3d11Texture);
        SAFE_RELEASE(desktopResource);
        m_d3d11Duplication->ReleaseFrame();
        Log("CaptureDesktop completed successfully\n");
        return true;
    }

    void ComputeDepth() {
        if (!m_device || !m_depthTexture || !m_commandList || !m_computePso || !m_srvHeap || !m_samplerHeap || !m_constantBuffer || !m_commandQueue || !m_commandAllocators[m_frameIndex]) {
            Log("ComputeDepth: Invalid resources\n");
            return;
        }

        HRESULT hr = m_commandList->Reset(m_commandAllocators[m_frameIndex], m_computePso);
        if (FAILED(hr)) {
            Log("ComputeDepth: Command list reset failed\n");
            return;
        }

        CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_depthTexture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_commandList->ResourceBarrier(1, &barrier);

        m_commandList->SetPipelineState(m_computePso);
        m_commandList->SetComputeRootSignature(m_rootSignature);
        m_commandList->SetComputeRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());

        ID3D12DescriptorHeap* heaps[] = { m_srvHeap, m_samplerHeap };
        m_commandList->SetDescriptorHeaps(2, heaps);

        m_commandList->SetComputeRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
        CD3DX12_GPU_DESCRIPTOR_HANDLE uavHandle(m_srvHeap->GetGPUDescriptorHandleForHeapStart());
        uavHandle.Offset(2, m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
        m_commandList->SetComputeRootDescriptorTable(2, uavHandle);
        m_commandList->SetComputeRootDescriptorTable(3, m_samplerHeap->GetGPUDescriptorHandleForHeapStart());

        m_commandList->Dispatch((SCREEN_WIDTH + 15) / 16, (SCREEN_HEIGHT + 15) / 16, 1);

        barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_depthTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_commandList->ResourceBarrier(1, &barrier);

        CHECK_HR(m_commandList->Close(), "Close command list for compute");
        ID3D12CommandList* cmdLists[] = { m_commandList };
        m_commandQueue->ExecuteCommandLists(1, cmdLists);
        WaitForGPU();
    }

    void Render() {
        if (!m_device || !m_swapChain || !m_commandQueue || !m_commandList || !m_graphicsPso || !m_srvHeap || !m_samplerHeap || !m_vertexBuffer || !m_rtvHeap || !m_constantBuffer) {
            Log("Render: Invalid resources\n");
            return;
        }

        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
        if (m_frameIndex >= FRAME_COUNT || !m_commandAllocators[m_frameIndex] || !m_renderTargets[m_frameIndex]) {
            Log("Render: Invalid frame resources\n");
            return;
        }

        HRESULT hr = m_commandAllocators[m_frameIndex]->Reset();
        if (FAILED(hr)) {
            Log("Render: Command allocator reset failed\n");
            return;
        }

        hr = m_commandList->Reset(m_commandAllocators[m_frameIndex], m_graphicsPso);
        if (FAILED(hr)) {
            Log("Render: Command list reset failed\n");
            return;
        }

        CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_commandList->ResourceBarrier(1, &barrier);

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_frameIndex, m_rtvDescriptorSize);
        m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        const float clearColor[] = { 0.2f, 0.3f, 0.4f, 1.0f };
        m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

        m_commandList->SetPipelineState(m_graphicsPso);
        m_commandList->SetGraphicsRootSignature(m_rootSignature);
        m_commandList->SetGraphicsRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());

        ID3D12DescriptorHeap* heaps[] = { m_srvHeap, m_samplerHeap };
        m_commandList->SetDescriptorHeaps(2, heaps);

        m_commandList->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
        m_commandList->SetGraphicsRootDescriptorTable(3, m_samplerHeap->GetGPUDescriptorHandleForHeapStart());

        CD3DX12_VIEWPORT viewport(0.0f, 0.0f, static_cast<float>(SCREEN_WIDTH), static_cast<float>(SCREEN_HEIGHT));
        CD3DX12_RECT scissorRect(0, 0, static_cast<LONG>(SCREEN_WIDTH), static_cast<LONG>(SCREEN_HEIGHT));
        m_commandList->RSSetViewports(1, &viewport);
        m_commandList->RSSetScissorRects(1, &scissorRect);

        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        D3D12_VERTEX_BUFFER_VIEW vbView = { m_vertexBuffer->GetGPUVirtualAddress(), sizeof(float) * 5 * 4, sizeof(float) * 5 };
        m_commandList->IASetVertexBuffers(0, 1, &vbView);
        m_commandList->DrawInstanced(4, 1, 0, 0);

        barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        m_commandList->ResourceBarrier(1, &barrier);

        hr = m_commandList->Close();
        if (FAILED(hr)) {
            Log("Render: Command list close failed\n");
            return;
        }

        ID3D12CommandList* commandLists[] = { m_commandList };
        m_commandQueue->ExecuteCommandLists(1, commandLists);
        hr = m_swapChain->Present(1, 0);
        if (FAILED(hr)) {
            char debugMsg[256];
            sprintf_s(debugMsg, "Render: Present failed, HRESULT: 0x%08X\n", hr);
            Log(debugMsg);
            return;
        }
        WaitForGPU();
    }

private:
    void WaitForGPU() {
        if (!m_commandQueue || !m_fence || !m_fenceEvent) {
            Log("WaitForGPU: Missing resources\n");
            return;
        }
        HRESULT hr = m_commandQueue->Signal(m_fence, ++m_fenceValue);
        if (FAILED(hr)) {
            Log("WaitForGPU: Signal failed\n");
            return;
        }
        if (m_fence->GetCompletedValue() < m_fenceValue) {
            hr = m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
            if (FAILED(hr)) {
                Log("WaitForGPU: SetEventOnCompletion failed\n");
                return;
            }
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }

    ID3D12Device* m_device;
    ID3D12CommandQueue* m_commandQueue;
    IDXGISwapChain3* m_swapChain;
    ID3D12DescriptorHeap* m_rtvHeap;
    ID3D12DescriptorHeap* m_srvHeap;
    ID3D12DescriptorHeap* m_samplerHeap;
    ID3D12Resource* m_renderTargets[FRAME_COUNT];
    ID3D12CommandAllocator* m_commandAllocators[FRAME_COUNT];
    ID3D12GraphicsCommandList* m_commandList;
    ID3D12Resource* m_screenTexture;
    ID3D12Resource* m_depthTexture;
    ID3D12Resource* m_constantBuffer;
    ID3D12Resource* m_vertexBuffer;
    IDXGIOutputDuplication* m_d3d11Duplication;
    ID3D11Texture2D* m_d3d11StagingTexture;  // New staging texture for D3D11
    ID3D12Resource* m_d3d12UploadBuffer;    // New upload buffer for D3D12
    ID3D12RootSignature* m_rootSignature;
    ID3D12PipelineState* m_computePso;
    ID3D12PipelineState* m_graphicsPso;
    ID3D12Fence* m_fence;
    HANDLE m_fenceEvent;
    UINT m_frameIndex;
    UINT64 m_fenceValue;
    UINT m_rtvDescriptorSize;
    D3D_FEATURE_LEVEL m_featureLevel;
    IDXGIAdapter1* m_adapter;
    IDXGIFactory6* m_factory;
    ID3D11Device* m_d3d11Device;
    ID3D11DeviceContext* m_d3d11Context;
};

class LightWeight3DApp {
public:
    LightWeight3DApp() : m_hwnd(nullptr), m_isRunning(false), m_isHidden(false), m_isClickThrough(true), m_frameCount(0) {
        Gdiplus::GdiplusStartupInput gdiplusStartupInput;
        Gdiplus::GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, nullptr);
    }

    ~LightWeight3DApp() {
        Cleanup();
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
    }

    bool Initialize() {
        try {
            WNDCLASSEX wc = {};
            wc.cbSize = sizeof(WNDCLASSEX);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = WindowProc;
            wc.hInstance = GetModuleHandle(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = L"LightWeight3DClass";
            if (!RegisterClassEx(&wc)) throw ToolException("Window class registration failed");

            HMONITOR hMonitor = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
            MONITORINFOEX monitorInfo = { sizeof(MONITORINFOEX) };
            GetMonitorInfo(hMonitor, &monitorInfo);
            SCREEN_WIDTH = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
            SCREEN_HEIGHT = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;

            m_hwnd = CreateWindowEx(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST, L"LightWeight3DClass",
                L"LightWeight3D", WS_POPUP, monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top,
                SCREEN_WIDTH, SCREEN_HEIGHT, nullptr, nullptr, GetModuleHandle(nullptr), this);
            if (!m_hwnd) throw ToolException("Window creation failed");
            SetWindowLongPtr(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

            if (!m_d3dRenderer.Initialize(m_hwnd)) {
                DestroyWindow(m_hwnd);
                throw ToolException("D3D12Renderer initialization failed");
            }

            SetWindowPos(m_hwnd, HWND_TOPMOST, monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top, SCREEN_WIDTH, SCREEN_HEIGHT, SWP_SHOWWINDOW);
            CreateTrayIcon();
            SetLayeredWindowAttributes(m_hwnd, 0, static_cast<BYTE>(config.alpha * 255), LWA_ALPHA);
            SetClickThrough(m_isClickThrough);
            ShowWindow(m_hwnd, SW_SHOW);
            return true;
        }
        catch (const ToolException& e) {
            Log(e.what());
            return false;
        }
    }

    void Cleanup() {
        Stop();
        RemoveTrayIcon();
        m_d3dRenderer.Cleanup();
        if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
    }

    void Run() {
        m_isRunning = true;
        m_renderThread = std::thread(&LightWeight3DApp::RenderLoop, this);
        MSG msg = {};
        while (GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) break;
        }
    }

    void Stop() {
        m_isRunning = false;
        if (m_renderThread.joinable()) m_renderThread.join();
    }

    void ToggleClickThrough() {
        m_isClickThrough = !m_isClickThrough;
        SetClickThrough(m_isClickThrough);
    }

    void ToggleVisibility() {
        m_isHidden = !m_isHidden;
        ShowWindow(m_hwnd, m_isHidden ? SW_HIDE : SW_SHOW);
    }

    void SetClickThrough(bool enabled) {
        LONG exStyle = GetWindowLong(m_hwnd, GWL_EXSTYLE);
        exStyle = enabled ? (exStyle | WS_EX_TRANSPARENT) : (exStyle & ~WS_EX_TRANSPARENT);
        SetWindowLong(m_hwnd, GWL_EXSTYLE, exStyle);
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        LightWeight3DApp* app = reinterpret_cast<LightWeight3DApp*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        switch (message) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_USER + 1:
            if (app) return app->HandleTrayMessage(wParam, lParam);
            break;
        case WM_HOTKEY:
            if (app) {
                switch (wParam) {
                case 1: app->ToggleClickThrough(); break;
                case 2: app->ToggleVisibility(); break;
                }
            }
            break;
        }
        return DefWindowProc(hwnd, message, wParam, lParam);
    }

    LRESULT HandleTrayMessage(WPARAM wParam, LPARAM lParam) {
        if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenu(menu, MF_STRING | (m_isClickThrough ? MF_CHECKED : MF_UNCHECKED), 1, L"Click-Through");
            AppendMenu(menu, MF_STRING | (m_isHidden ? MF_CHECKED : MF_UNCHECKED), 2, L"Hide Overlay");
            AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenu(menu, MF_STRING | (config.enable_parallax ? MF_CHECKED : MF_UNCHECKED), 3, L"Parallax Effect");
            AppendMenu(menu, MF_STRING | (config.enable_chromatic ? MF_CHECKED : MF_UNCHECKED), 4, L"Chromatic Aberration");
            AppendMenu(menu, MF_STRING | (config.enable_dof ? MF_CHECKED : MF_UNCHECKED), 5, L"Depth of Field");
            AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenu(menu, MF_STRING, 8, L"Exit");
            SetForegroundWindow(m_hwnd);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, m_hwnd, nullptr);
            DestroyMenu(menu);
            switch (cmd) {
            case 1: ToggleClickThrough(); break;
            case 2: ToggleVisibility(); break;
            case 3: config.enable_parallax = !config.enable_parallax; break;
            case 4: config.enable_chromatic = !config.enable_chromatic; break;
            case 5: config.enable_dof = !config.enable_dof; break;
            case 8: PostQuitMessage(0); break;
            }
        }
        return 0;
    }

private:
    void CreateTrayIcon() {
        NOTIFYICONDATA nid = {};
        nid.cbSize = sizeof(NOTIFYICONDATA);
        nid.hWnd = m_hwnd;
        nid.uID = 1;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_USER + 1;
        nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wcscpy_s(nid.szTip, L"LightWeight3D");
        Shell_NotifyIcon(NIM_ADD, &nid);
        RegisterHotKey(m_hwnd, 1, MOD_CONTROL | MOD_ALT, 'C');
        RegisterHotKey(m_hwnd, 2, MOD_CONTROL | MOD_ALT, 'H');
    }

    void RemoveTrayIcon() {
        NOTIFYICONDATA nid = {};
        nid.cbSize = sizeof(NOTIFYICONDATA);
        nid.hWnd = m_hwnd;
        nid.uID = 1;
        Shell_NotifyIcon(NIM_DELETE, &nid);
        UnregisterHotKey(m_hwnd, 1);
        UnregisterHotKey(m_hwnd, 2);
    }

    void RenderLoop() {
        auto lastFrameTime = std::chrono::high_resolution_clock::now();
        while (m_isRunning) {
            auto frameStart = std::chrono::high_resolution_clock::now();

            if (!m_d3dRenderer.CaptureDesktop()) {
                Log("RenderLoop: CaptureDesktop failed, continuing with previous frame\n");
            }
            m_d3dRenderer.ComputeDepth();
            m_d3dRenderer.Render();

            auto frameEnd = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(frameEnd - frameStart);
            auto targetFrameTime = std::chrono::microseconds(1000000 / TARGET_FPS);
            if (elapsed < targetFrameTime) {
                std::this_thread::sleep_for(targetFrameTime - elapsed);
            }

            if (++m_frameCount % 100 == 0) {
                float fps = 1000000.0f / std::chrono::duration_cast<std::chrono::microseconds>(frameEnd - lastFrameTime).count() * 100;
                char debugMsg[256];
                sprintf_s(debugMsg, "FPS: %.1f\n", fps);
                Log(debugMsg);
            }
            lastFrameTime = frameEnd;
        }
    }

    HWND m_hwnd;
    ULONG_PTR m_gdiplusToken;
    D3D12Renderer m_d3dRenderer;
    std::thread m_renderThread;
    bool m_isRunning, m_isHidden, m_isClickThrough;
    size_t m_frameCount;
};

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow) {
    try {
        LightWeight3DApp app;
        if (!app.Initialize()) {
            Log("Application initialization failed\n");
            return 1;
        }
        app.Run();
        return 0;
    }
    catch (const ToolException& e) {
        Log(e.what());
        return 1;
    }
}