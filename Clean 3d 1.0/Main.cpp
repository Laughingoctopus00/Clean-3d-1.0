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
#include <d3d11.h>
#include <fstream>
#include <cstdlib>

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
        char buffer[256]; \
        sprintf_s(buffer, "%s (HR: 0x%08X)\n", msg, hr); \
        Log(buffer); \
        throw ToolException(buffer, hr); \
    }

#define SAFE_RELEASE(p) if (p) { (p)->Release(); (p) = nullptr; }

static UINT SCREEN_WIDTH = 4096;  // Updated for your 4096x2160 screen
static UINT SCREEN_HEIGHT = 2160;
const UINT TARGET_FPS = 140;
const UINT FRAME_COUNT = 3;
const int MAX_RECOVERY_ATTEMPTS = 3;

static std::ofstream logFile("debug_log.txt", std::ios::app);
static bool enableLogging = true;

void Log(const char* msg) {
    if (enableLogging) {
        logFile << msg;
        logFile.flush();
        OutputDebugStringA(msg);
    }
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
    float time;
    float occlusion_strength;
    float wiggle_frequency;
    // Volumetric fog params
    float fog_density;
    float fog_color_r;
    float fog_color_g;
    float fog_color_b;
    float fog_scatter;
    float fog_anisotropy;
    float fog_height_falloff;
    float temporal_blend;
    // New outline controls
    float outline_width;
    float outline_intensity;
    uint8_t enable_parallax_barrier;
    uint8_t enable_lenticular;
    uint8_t enable_volumetric_fog;
    uint8_t padding[41];
};
#pragma pack(pop)
static_assert(sizeof(IllusionConfig) == 128, "IllusionConfig must be 128 bytes");

static const IllusionConfig defaultConfig = {
    1200.0f, 1260.0f, 0.95f, 1000.0f, 12.0f, 160.0f,
    1, 3, 1, 1, 1,
    0.016f, 0.75f, 12.0f,
    // fog defaults
    100.02f, 0.6f, 0.65f, 0.7f, 0.5f, 1000.0f, 1.0f, 0.9f,
    // outline defaults
    2.06f, 1000.85f,
    1, 1, 1,
    {0}
};

static IllusionConfig config = defaultConfig;

class ToolException : public std::exception {
public:
    ToolException(const char* msg, HRESULT hr = S_OK) : m_message(msg), m_hr(hr) {
        if (enableLogging) {
            Log(m_message.c_str());
        }
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
        m_commandList(nullptr), m_computePso(nullptr), m_constantBufferSize(0),
        m_depthTexture(nullptr), m_graphicsPso(nullptr), m_rootSignature(nullptr),
        m_rtvHeap(nullptr), m_screenTexture(nullptr), m_srvHeap(nullptr),
        m_samplerHeap(nullptr), m_vertexBuffer(nullptr), m_rtvDescriptorSize(0),
        m_featureLevel(D3D_FEATURE_LEVEL_12_0), m_adapter(nullptr), m_factory(nullptr),
        m_d3d11Device(nullptr), m_d3d11Context(nullptr), m_d3d11Duplication(nullptr),
        m_d3d11StagingTexture(nullptr), m_d3d12UploadBuffer(nullptr), m_time(0.0f),
        m_recoveryCount(0), m_fallbackMode(false), m_hwnd(nullptr) {
        for (UINT i = 0; i < FRAME_COUNT; i++) {
            m_renderTargets[i] = nullptr;
            m_commandAllocators[i] = nullptr;
            m_constantBuffers[i] = nullptr;
            m_mappedConstantData[i] = nullptr;
        }
    }

    ~D3D12Renderer() { Cleanup(); }

    bool Initialize(HWND hwnd) {
        m_hwnd = hwnd;
        try {
            Log("Initializing D3D12Renderer...\n");
            HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory));
            CHECK_HR(hr, "CreateDXGIFactory2 failed");

            // Select adapter using configurable selection logic
            if (!SelectAdapter()) {
                throw ToolException("No suitable hardware adapter found");
            }

            return CreateDeviceAndResources();
        }
        catch (const ToolException& e) {
            Log(e.what());
            return false;
        }
    }

    // SelectAdapter: configurable selection
    // If environment variable CLEAN3D_USE_SYSTEM_DEFAULT_ADAPTER=1 is set,
    // use system default by leaving m_adapter == nullptr and letting
    // D3D12CreateDevice select the adapter. Otherwise enumerate adapters
    // and pick the one with the highest D3D12 feature level support; tie-breaker by VRAM.
    bool SelectAdapter() {
        //const char* useDefaultEnv = std::getenv("CLEAN3D_USE_SYSTEM_DEFAULT_ADAPTER");
        char* useDefaultEnv = nullptr;
        size_t envLen = 0;
        if (_dupenv_s(&useDefaultEnv, &envLen, "CLEAN3D_USE_SYSTEM_DEFAULT_ADAPTER") != 0) {
            useDefaultEnv = nullptr;
        }
        if (useDefaultEnv && useDefaultEnv[0] == '1') {
            Log("Adapter selection: using system default adapter (D3D12CreateDevice fallback)\n");
            if (useDefaultEnv) free(useDefaultEnv);
            // leave m_adapter as nullptr to let D3D12CreateDevice pick
            return true;
        }
        if (useDefaultEnv) { free(useDefaultEnv); useDefaultEnv = nullptr; }

        IDXGIAdapter1* bestAdapter = nullptr;
        int bestFeatureRank = -1;
        uint64_t bestMemory = 0;

        UINT adapterIndex = 0;
        IDXGIAdapter1* adapter = nullptr;

        // Feature levels in descending order; lower index => better
        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0
        };

        while (m_factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);

            // skip software adapters
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                SAFE_RELEASE(adapter);
                adapterIndex++;
                continue;
            }

            // Determine highest supported feature level for this adapter
            int featureRank = -1;
            for (int i = 0; i < _countof(featureLevels); ++i) {
                ID3D12Device* tmpDevice = nullptr;
                HRESULT hr = D3D12CreateDevice(adapter, featureLevels[i], IID_PPV_ARGS(&tmpDevice));
                if (SUCCEEDED(hr)) {
                    featureRank = (int)i; // lower is better (0 == 12_1)
                    SAFE_RELEASE(tmpDevice);
                    break;
                }
            }

            if (featureRank == -1) {
                // adapter doesn't support minimum required feature levels
                Log("Adapter skipped: insufficient feature level\n");
                SAFE_RELEASE(adapter);
                adapterIndex++;
                continue;
            }

            uint64_t vram = desc.DedicatedVideoMemory;

            // Choose adapter: prefer better feature rank, then more VRAM
            bool better = false;
            if (bestAdapter == nullptr) better = true;
            else if (featureRank < bestFeatureRank) better = true;
            else if (featureRank == bestFeatureRank && vram > bestMemory) better = true;

            if (better) {
                SAFE_RELEASE(bestAdapter);
                bestAdapter = adapter; // keep reference
                bestFeatureRank = featureRank;
                bestMemory = vram;
                // do not release 'adapter' here; it's now bestAdapter
            } else {
                SAFE_RELEASE(adapter);
            }

            adapterIndex++;
        }

        if (bestAdapter) {
            m_adapter = bestAdapter; // adopt best adapter
            DXGI_ADAPTER_DESC1 chosenDesc;
            m_adapter->GetDesc1(&chosenDesc);
            char buf[512];
            sprintf_s(buf, "Selected adapter: %ls (VRAM: %llu MB), feature rank: %d\n", chosenDesc.Description, (unsigned long long)(chosenDesc.DedicatedVideoMemory / (1024 * 1024)), bestFeatureRank);
            Log(buf);
            return true;
        }

        Log("No suitable hardware adapter was found during enumeration\n");
        return false;
    }

    bool CreateDeviceAndResources() {
        HRESULT hr;
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
                char buffer[64];
                sprintf_s(buffer, "Created device with feature level %d.%d\n", level / 0x1000, (level % 0x1000) / 0x100);
                Log(buffer);
                break;
            }
        }
        if (!m_device) throw ToolException("Failed to create D3D12 device");

        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        CHECK_HR(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)), "CreateCommandQueue failed");

        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.Width = SCREEN_WIDTH;
        swapChainDesc.Height = SCREEN_HEIGHT;
        swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.BufferCount = FRAME_COUNT;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.SampleDesc.Count = 1;
        IDXGISwapChain1* swapChain1 = nullptr;
        hr = m_factory->CreateSwapChainForHwnd(m_commandQueue, m_hwnd, &swapChainDesc, nullptr, nullptr, &swapChain1);
        CHECK_HR(hr, "CreateSwapChainForHwnd failed");
        CHECK_HR(swapChain1->QueryInterface(IID_PPV_ARGS(&m_swapChain)), "SwapChain QueryInterface failed");
        SAFE_RELEASE(swapChain1);

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = FRAME_COUNT;
        CHECK_HR(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)), "Create RTV Heap failed");

        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.NumDescriptors = 3;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        CHECK_HR(m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap)), "Create SRV Heap failed");

        D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc = {};
        samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        samplerHeapDesc.NumDescriptors = 1;
        samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        CHECK_HR(m_device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&m_samplerHeap)), "Create Sampler Heap failed");

        m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
        for (UINT i = 0; i < FRAME_COUNT; i++) {
            CHECK_HR(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i])), "GetSwapChainBuffer failed");
            m_device->CreateRenderTargetView(m_renderTargets[i], NULL, rtvHandle);
            rtvHandle.Offset(1, m_rtvDescriptorSize);
        }

        for (UINT i = 0; i < FRAME_COUNT; i++) {
            CHECK_HR(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i])), "CreateCommandAllocator failed");
        }

        CHECK_HR(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[0], NULL, IID_PPV_ARGS(&m_commandList)), "CreateCommandList failed");
        CHECK_HR(m_commandList->Close(), "Close initial CommandList failed");

        CHECK_HR(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "CreateFence failed");
        m_fenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (!m_fenceEvent) throw ToolException("Fence event creation failed", HRESULT_FROM_WIN32(GetLastError()));

        if (!CreateResources() || !CreatePipelines()) {
            Log("Resource or pipeline creation failed\n");
            return false;
        }

        Log("Device and resources created successfully\n");
        return true;
    }

    void Cleanup(bool preserveEssentials = false) {
        if (m_commandQueue && m_fence) WaitForGPU();
        for (UINT i = 0; i < FRAME_COUNT; i++) {
            SAFE_RELEASE(m_renderTargets[i]);
            SAFE_RELEASE(m_commandAllocators[i]);
            if (m_constantBuffers[i]) {
                m_constantBuffers[i]->Unmap(0, NULL);
                SAFE_RELEASE(m_constantBuffers[i]);
                m_mappedConstantData[i] = NULL;
            }
        }
        SAFE_RELEASE(m_screenTexture);
        SAFE_RELEASE(m_depthTexture);
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
        SAFE_RELEASE(m_disparityTexture);
        SAFE_RELEASE(m_computeRootSignature);
        if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = NULL; }
        if (!preserveEssentials) {
            SAFE_RELEASE(m_adapter);
            SAFE_RELEASE(m_factory);
            m_hwnd = NULL;
        }
        Log("Cleanup completed\n");
    }

    bool RecoverDevice() {
        m_recoveryCount++;
        char buffer[64];
        sprintf_s(buffer, "Attempting device recovery (count: %d)\n", m_recoveryCount);
        Log(buffer);

        if (m_recoveryCount > MAX_RECOVERY_ATTEMPTS) {
            Log("Exceeded maximum recovery attempts, switching to fallback mode\n");
            m_fallbackMode = true;
            return false;
        }

        IDXGIFactory6* tempFactory = m_factory;
        IDXGIAdapter1* tempAdapter = m_adapter;
        HWND tempHwnd = m_hwnd;
        m_factory = NULL;
        m_adapter = NULL;

        Cleanup(true);

        m_factory = tempFactory;
        m_adapter = tempAdapter;
        m_hwnd = tempHwnd;

        if (!m_factory || !m_adapter || !m_hwnd) {
            SAFE_RELEASE(m_factory);
            SAFE_RELEASE(m_adapter);
            m_hwnd = NULL;
            Log("Device recovery failed: Essential components missing\n");
            return false;
        }

        try {
            bool success = CreateDeviceAndResources();
            if (success) {
                config = defaultConfig;
                Log("Device recovered, restored default config\n");
            }
            Log(success ? "Device recovered successfully\n" : "Device recovery failed\n");
            return success;
        }
        catch (const ToolException& e) {
            Log(("Recovery failed: " + std::string(e.what()) + "\n").c_str());
            return false;
        }
    }

    bool ValidateResources() {
        bool valid = m_device && m_swapChain && m_commandQueue && m_commandList &&
            m_graphicsPso && m_srvHeap && m_samplerHeap && m_vertexBuffer &&
            m_rtvHeap && m_screenTexture && (m_constantBuffers[0] != NULL);
        if (!valid) Log("Resource validation failed\n");
        return valid;
    }

    bool CreateResources() {
        if (!m_device) return false;

        Log("Creating resources...\n");
        D3D12_HEAP_PROPERTIES defaultHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, SCREEN_WIDTH, SCREEN_HEIGHT, 1, 1);
        CHECK_HR(m_device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COMMON, NULL, IID_PPV_ARGS(&m_screenTexture)), "Create screen texture failed");

        // Create D3D11 device for desktop duplication (unchanged)
        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
        HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels, 1, D3D11_SDK_VERSION,
            &m_d3d11Device, NULL, &m_d3d11Context);
        if (FAILED(hr)) {
            Log("Failed to create D3D11 device for desktop duplication\n");
            return false;
        }

        IDXGIFactory1* factory = NULL;
        hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        CHECK_HR(hr, "CreateDXGIFactory1 failed for D3D11");

        IDXGIAdapter* adapter = NULL;
        factory->EnumAdapters(0, &adapter);
        IDXGIOutput* output = NULL;
        adapter->EnumOutputs(0, &output);
        IDXGIOutput1* output1 = NULL;
        hr = output->QueryInterface(IID_PPV_ARGS(&output1));
        if (FAILED(hr) || !output1) {
            SAFE_RELEASE(output);
            SAFE_RELEASE(adapter);
            SAFE_RELEASE(factory);
            Log("Failed to get IDXGIOutput1, using fallback checkerboard\n");

            D3D12_HEAP_PROPERTIES uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(SCREEN_WIDTH * SCREEN_HEIGHT * 4);
            CHECK_HR(m_device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, IID_PPV_ARGS(&m_d3d12UploadBuffer)), "Create upload buffer failed");

            UINT8* data;
            CHECK_HR(m_d3d12UploadBuffer->Map(0, NULL, reinterpret_cast<void**>(&data)), "Map upload buffer failed");
            CreateCheckerboardPattern(reinterpret_cast<uint32_t*>(data));
            m_d3d12UploadBuffer->Unmap(0, NULL);

            CHECK_HR(m_commandList->Reset(m_commandAllocators[0], NULL), "Reset command list failed");
            CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_screenTexture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            m_commandList->ResourceBarrier(1, &barrier);
            D3D12_SUBRESOURCE_DATA subresourceData = { data, static_cast<LONG_PTR>(SCREEN_WIDTH * 4), static_cast<LONG_PTR>(SCREEN_WIDTH * SCREEN_HEIGHT * 4) };
            ::UpdateSubresources<1>(m_commandList, m_screenTexture, m_d3d12UploadBuffer, 0, 0, 1, &subresourceData);
            barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_screenTexture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            m_commandList->ResourceBarrier(1, &barrier);
            CHECK_HR(m_commandList->Close(), "Close command list failed");
            ID3D12CommandList* cmdLists[] = { m_commandList };
            m_commandQueue->ExecuteCommandLists(1, cmdLists);
            WaitForGPU();
        }
        else {
            DXGI_OUTDUPL_DESC duplDesc;
            m_d3d11Duplication = NULL;
            hr = output1->DuplicateOutput(m_d3d11Device, &m_d3d11Duplication);
            if (FAILED(hr)) {
                Log("DuplicateOutput failed, falling back to checkerboard\n");
            }
            else {
                m_d3d11Duplication->GetDesc(&duplDesc);
                SCREEN_WIDTH = duplDesc.ModeDesc.Width;
                SCREEN_HEIGHT = duplDesc.ModeDesc.Height;
            }

            D3D11_TEXTURE2D_DESC stagingDesc = {};
            stagingDesc.Width = SCREEN_WIDTH;
            stagingDesc.Height = SCREEN_HEIGHT;
            stagingDesc.MipLevels = 1;
            stagingDesc.ArraySize = 1;
            stagingDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            stagingDesc.SampleDesc.Count = 1;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            CHECK_HR(m_d3d11Device->CreateTexture2D(&stagingDesc, NULL, &m_d3d11StagingTexture), "Create D3D11 staging texture failed");

            D3D12_HEAP_PROPERTIES uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(SCREEN_WIDTH * SCREEN_HEIGHT * 4);
            CHECK_HR(m_device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, IID_PPV_ARGS(&m_d3d12UploadBuffer)), "Create D3D12 upload buffer failed");

            SAFE_RELEASE(output1);
            SAFE_RELEASE(output);
            SAFE_RELEASE(adapter);
            SAFE_RELEASE(factory);
        }

        // Create SRV and sampler (unchanged)
        UINT descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_srvHeap->GetCPUDescriptorHandleForHeapStart());
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_screenTexture, &srvDesc, srvHandle);

        CD3DX12_CPU_DESCRIPTOR_HANDLE samplerHandle(m_samplerHeap->GetCPUDescriptorHandleForHeapStart());
        D3D12_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        m_device->CreateSampler(&samplerDesc, samplerHandle);

        // Create per-frame constant buffers, 256-byte aligned
        m_constantBufferSize = (sizeof(IllusionConfig) + 255) & ~255ULL;
        D3D12_HEAP_PROPERTIES uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        for (UINT i = 0; i < FRAME_COUNT; ++i) {
            D3D12_RESOURCE_DESC cbDesc = CD3DX12_RESOURCE_DESC::Buffer(m_constantBufferSize);
            CHECK_HR(m_device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, IID_PPV_ARGS(&m_constantBuffers[i])), "Create constant buffer failed");
            // Persistently map
            CHECK_HR(m_constantBuffers[i]->Map(0, NULL, reinterpret_cast<void**>(&m_mappedConstantData[i])), "Map constant buffer failed");
            // Initialize with current config
            memcpy(m_mappedConstantData[i], &config, sizeof(IllusionConfig));
            // Do not Unmap for upload heaps (keep mapped)
        }

        // Create vertex buffer (unchanged)
        struct Vertex { float x, y, z, u, v; };
        Vertex vertices[] = { {-1.0f, 1.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f, 1.0f, 0.0f}, {-1.0f, -1.0f, 0.0f, 0.0f, 1.0f}, {1.0f, -1.0f, 0.0f, 1.0f, 1.0f} };
        D3D12_RESOURCE_DESC vbDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(vertices));
        CHECK_HR(m_device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &vbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, IID_PPV_ARGS(&m_vertexBuffer)), "Create vertex buffer failed");
        void* vbData = NULL;
        CHECK_HR(m_vertexBuffer->Map(0, NULL, &vbData), "Map vertex buffer failed");
        memcpy(vbData, vertices, sizeof(vertices));
        m_vertexBuffer->Unmap(0, NULL);

        Log("Resources created successfully\n");
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
        if (!m_device) return false;

        Log("Creating pipelines...\n");

        CD3DX12_ROOT_PARAMETER rootParams[3] = {};
        CD3DX12_DESCRIPTOR_RANGE srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        rootParams[0].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
        CD3DX12_DESCRIPTOR_RANGE samplerRange(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0);
        rootParams[1].InitAsDescriptorTable(1, &samplerRange, D3D12_SHADER_VISIBILITY_PIXEL);
        rootParams[2].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, rootParams, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        ID3DBlob* signature = nullptr;
        ID3DBlob* error = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
        if (FAILED(hr)) {
            if (error) {
                Log("Root signature serialization error: ");
                Log(static_cast<const char*>(error->GetBufferPointer()));
                SAFE_RELEASE(error);
            }
            SAFE_RELEASE(signature);
            return false;
        }
        CHECK_HR(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)), "CreateRootSignature failed");
        SAFE_RELEASE(signature);

        // Create compute root signature for disparity compute
        {
            CD3DX12_DESCRIPTOR_RANGE srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0); // t0-t1
            CD3DX12_DESCRIPTOR_RANGE uavRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0); // u0
            CD3DX12_ROOT_PARAMETER computeParams[3];
            computeParams[0].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);
            computeParams[1].InitAsDescriptorTable(1, &uavRange, D3D12_SHADER_VISIBILITY_ALL);
            computeParams[2].InitAsConstantBufferView(0);
            CD3DX12_ROOT_SIGNATURE_DESC computeRootDesc(3, computeParams);
            ID3DBlob* compSig = nullptr;
            ID3DBlob* compErr = nullptr;
            HRESULT hrSig = D3D12SerializeRootSignature(&computeRootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &compSig, &compErr);
            if (FAILED(hrSig)) {
                if (compErr) { Log((const char*)compErr->GetBufferPointer()); SAFE_RELEASE(compErr); }
                SAFE_RELEASE(compSig);
                return false;
            }
            CHECK_HR(m_device->CreateRootSignature(0, compSig->GetBufferPointer(), compSig->GetBufferSize(), IID_PPV_ARGS(&m_computeRootSignature)), "Create compute root signature failed");
            SAFE_RELEASE(compSig);
        }

        // Compile compute shader FogCompute.hlsl
        ID3DBlob* csBlob = nullptr;
        ID3DBlob* csCompiled = nullptr;
        Log("Loading FogCompute.hlsl...\n");
        hr = D3DReadFileToBlob(L"FogCompute.hlsl", &csBlob);
        if (FAILED(hr)) {
            Log("Failed to load FogCompute.hlsl, compute fog disabled\n");
            SAFE_RELEASE(csBlob);
        }
        else {
            ID3DBlob* error = nullptr;
            hr = D3DCompile(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), "FogCompute.hlsl", nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &csCompiled, &error);
            if (FAILED(hr)) {
                if (error) { Log((const char*)error->GetBufferPointer()); SAFE_RELEASE(error); }
                SAFE_RELEASE(csBlob);
            }
            else {
                SAFE_RELEASE(csBlob);
                D3D12_COMPUTE_PIPELINE_STATE_DESC cpsd = {};
                cpsd.pRootSignature = m_computeRootSignature;
                cpsd.CS = { csCompiled->GetBufferPointer(), csCompiled->GetBufferSize() };
                CHECK_HR(m_device->CreateComputePipelineState(&cpsd, IID_PPV_ARGS(&m_computePso)), "CreateComputePipelineState failed");
                SAFE_RELEASE(csCompiled);
            }
        }

        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* psBlob = nullptr;
        Log("Loading VertexShader.hlsl...\n");
        hr = D3DReadFileToBlob(L"VertexShader.hlsl", &vsBlob);
        if (FAILED(hr)) {
            char buffer[256];
            sprintf_s(buffer, "Failed to load VertexShader.hlsl (HR: 0x%08X)\n", hr);
            Log(buffer);
            SAFE_RELEASE(vsBlob);
            return false;
        }
        Log("VertexShader.hlsl loaded successfully\n");

        Log("Loading PixelShader.hlsl...\n");
        hr = D3DReadFileToBlob(L"PixelShader.hlsl", &psBlob);
        if (FAILED(hr)) {
            char buffer[256];
            sprintf_s(buffer, "Failed to load PixelShader.hlsl (HR: 0x%08X)\n", hr);
            Log(buffer);
            SAFE_RELEASE(vsBlob);
            SAFE_RELEASE(psBlob);
            return false;
        }
        Log("PixelShader.hlsl loaded successfully\n");

        ID3DBlob* vsCompiled = nullptr;
        ID3DBlob* psCompiled = nullptr;
        hr = D3DCompile(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), "VertexShader.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsCompiled, &error);
        if (FAILED(hr)) {
            if (error) {
                Log("Vertex shader compilation error: ");
                Log(static_cast<const char*>(error->GetBufferPointer()));
                SAFE_RELEASE(error);
            }
            SAFE_RELEASE(vsBlob);
            SAFE_RELEASE(psBlob);
            return false;
        }

        hr = D3DCompile(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), "PixelShader.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psCompiled, &error);
        if (FAILED(hr)) {
            if (error) {
                Log("Pixel shader compilation error: ");
                Log(static_cast<const char*>(error->GetBufferPointer()));
                SAFE_RELEASE(error);
            }
            SAFE_RELEASE(vsBlob);
            SAFE_RELEASE(psBlob);
            SAFE_RELEASE(vsCompiled);
            return false;
        }

        SAFE_RELEASE(vsBlob);
        SAFE_RELEASE(psBlob);

        D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };

        DXGI_SWAP_CHAIN_DESC1 swapChainDesc;
        m_swapChain->GetDesc1(&swapChainDesc);
        DXGI_FORMAT swapChainFormat = swapChainDesc.Format;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = m_rootSignature;
        psoDesc.VS = { vsCompiled->GetBufferPointer(), vsCompiled->GetBufferSize() };
        psoDesc.PS = { psCompiled->GetBufferPointer(), psCompiled->GetBufferSize() };
        psoDesc.InputLayout = { inputLayout, 2 };
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.RTVFormats[0] = swapChainFormat;
        psoDesc.NumRenderTargets = 1;
        psoDesc.SampleDesc.Count = 1;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;

        char buffer[256];
        sprintf_s(buffer, "PSO Descriptor Values:\nNumRenderTargets: %u\nRTVFormats[0]: %u\nSampleDesc.Count: %u\nSampleMask: %u\n",
            psoDesc.NumRenderTargets, psoDesc.RTVFormats[0], psoDesc.SampleDesc.Count, psoDesc.SampleMask);
        Log(buffer);

        Log("Creating Graphics PSO...\n");
        hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_graphicsPso));
        SAFE_RELEASE(vsCompiled);
        SAFE_RELEASE(psCompiled);
        CHECK_HR(hr, "CreateGraphicsPipelineState failed");

        Log("Pipelines created successfully\n");
        return true;
    }

    bool CaptureDesktop() {
        if (!ValidateResources() || !m_d3d11Duplication || !m_d3d11StagingTexture || !m_d3d12UploadBuffer) {
            Log("CaptureDesktop skipped due to invalid resources\n");
            return false;
        }

        Log("Capturing desktop...\n");
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        IDXGIResource* desktopResource = NULL;
        HRESULT hr = m_d3d11Duplication->AcquireNextFrame(16, &frameInfo, &desktopResource);
        if (FAILED(hr)) {
            // Do not call ReleaseFrame() here because AcquireNextFrame failed and no frame has been acquired
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                // No new frame available within timeout - this isn't fatal
                Log("AcquireNextFrame timed out (no new frame)\n");
                return false;
            }
            char buf[128];
            sprintf_s(buf, "AcquireNextFrame failed (HR: 0x%08X)\n", hr);
            Log(buf);
            return false;
        }

        if (!desktopResource) {
            Log("AcquireNextFrame returned no desktop resource\n");
            // Ensure we release the frame slot if we somehow acquired one
            m_d3d11Duplication->ReleaseFrame();
            return false;
        }

        ID3D11Texture2D* d3d11Texture = NULL;
        hr = desktopResource->QueryInterface(IID_PPV_ARGS(&d3d11Texture));
        if (FAILED(hr) || !d3d11Texture) {
            SAFE_RELEASE(desktopResource);
            m_d3d11Duplication->ReleaseFrame();
            Log("Desktop resource QueryInterface failed\n");
            return false;
        }

        m_d3d11Context->CopyResource(m_d3d11StagingTexture, d3d11Texture);

        D3D11_MAPPED_SUBRESOURCE mappedResource;
        hr = m_d3d11Context->Map(m_d3d11StagingTexture, 0, D3D11_MAP_READ, 0, &mappedResource);
        if (FAILED(hr)) {
            SAFE_RELEASE(d3d11Texture);
            SAFE_RELEASE(desktopResource);
            m_d3d11Duplication->ReleaseFrame();
            Log("Map staging texture failed\n");
            return false;
        }

        UINT8* uploadData;
        hr = m_d3d12UploadBuffer->Map(0, NULL, reinterpret_cast<void**>(&uploadData));
        if (FAILED(hr)) {
            m_d3d11Context->Unmap(m_d3d11StagingTexture, 0);
            SAFE_RELEASE(d3d11Texture);
            SAFE_RELEASE(desktopResource);
            m_d3d11Duplication->ReleaseFrame();
            Log("Map upload buffer failed\n");
            return false;
        }

        // Safe pitch-aware copy (use min of row sizes)
        size_t rowBytes = SCREEN_WIDTH * 4;
        for (UINT y = 0; y < SCREEN_HEIGHT; y++) {
            uint8_t* src = static_cast<uint8_t*>(mappedResource.pData) + static_cast<size_t>(y) * mappedResource.RowPitch;
            uint8_t* dst = uploadData + static_cast<size_t>(y) * rowBytes;
            size_t copyBytes = (mappedResource.RowPitch < static_cast<UINT>(rowBytes)) ? mappedResource.RowPitch : rowBytes;
            memcpy(dst, src, copyBytes);
            if (copyBytes < rowBytes) {
                // zero remaining bytes to avoid garbage
                memset(dst + copyBytes, 0, rowBytes - copyBytes);
            }
        }

        m_d3d12UploadBuffer->Unmap(0, NULL);
        m_d3d11Context->Unmap(m_d3d11StagingTexture, 0);

        CHECK_HR(m_commandList->Reset(m_commandAllocators[m_frameIndex], NULL), "Reset command list for capture failed");
        CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_screenTexture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        m_commandList->ResourceBarrier(1, &barrier);
        D3D12_SUBRESOURCE_DATA subresourceData = { uploadData, static_cast<LONG_PTR>(rowBytes), static_cast<LONG_PTR>(rowBytes * SCREEN_HEIGHT) };
        ::UpdateSubresources<1>(m_commandList, m_screenTexture, m_d3d12UploadBuffer, 0, 0, 1, &subresourceData);
        barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_screenTexture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_commandList->ResourceBarrier(1, &barrier);
        CHECK_HR(m_commandList->Close(), "Close command list for capture failed");

        ID3D12CommandList* cmdLists[] = { m_commandList };
        m_commandQueue->ExecuteCommandLists(1, cmdLists);
        WaitForGPU();

        SAFE_RELEASE(d3d11Texture);
        SAFE_RELEASE(desktopResource);
        // Release the frame now that processing is complete
        m_d3d11Duplication->ReleaseFrame();
        Log("Desktop captured successfully\n");
        return true;
    }

    bool Render() {
        if (!ValidateResources()) {
            Log("Render failed: Invalid resources\n");
            if (m_fallbackMode) return RenderFallback();
            return false;
        }

        Log("Rendering frame...\n");
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
        if (m_frameIndex >= FRAME_COUNT || !m_commandAllocators[m_frameIndex] || !m_renderTargets[m_frameIndex]) {
            Log("Invalid frame index or resources\n");
            return false;
        }

        HRESULT hr = m_commandAllocators[m_frameIndex]->Reset();
        CHECK_HR(hr, "Command allocator reset failed");

        hr = m_commandList->Reset(m_commandAllocators[m_frameIndex], m_graphicsPso);
        CHECK_HR(hr, "Command list reset failed");

        // Dispatch disparity compute if available and enabled
        if (m_computePso && config.enable_volumetric_fog && m_disparityTexture) {
            // Transition disparity to UAV
            CD3DX12_RESOURCE_BARRIER toUav = CD3DX12_RESOURCE_BARRIER::Transition(m_disparityTexture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_commandList->ResourceBarrier(1, &toUav);

            m_commandList->SetPipelineState(m_computePso);
            m_commandList->SetComputeRootSignature(m_computeRootSignature);

            ID3D12DescriptorHeap* heaps[] = { m_srvHeap, m_samplerHeap };
            m_commandList->SetDescriptorHeaps(2, heaps);

            // Root descriptor tables: SRV table at slot 0 (t0,t1), UAV table at slot1 (u0)
            CD3DX12_GPU_DESCRIPTOR_HANDLE gpuSrv(m_srvHeap->GetGPUDescriptorHandleForHeapStart());
            m_commandList->SetComputeRootDescriptorTable(0, gpuSrv);
            // UAV at descriptor index 2
            CD3DX12_GPU_DESCRIPTOR_HANDLE gpuUav(m_srvHeap->GetGPUDescriptorHandleForHeapStart(), 2, m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
            m_commandList->SetComputeRootDescriptorTable(1, gpuUav);

            // Dispatch compute at 16x16 threads
            UINT threadsX = (SCREEN_WIDTH + 15) / 16;
            UINT threadsY = (SCREEN_HEIGHT + 15) / 16;
            m_commandList->Dispatch(threadsX, threadsY, 1);

            // Transition disparity to SRV for pixel shader use
            CD3DX12_RESOURCE_BARRIER toSrv = CD3DX12_RESOURCE_BARRIER::Transition(m_disparityTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            m_commandList->ResourceBarrier(1, &toSrv);

            // Restore graphics pipeline state and root signature for subsequent draw
            m_commandList->SetPipelineState(m_graphicsPso);
            m_commandList->SetGraphicsRootSignature(m_rootSignature);
            // rebind constant buffer root slot
            m_commandList->SetGraphicsRootConstantBufferView(2, m_constantBuffers[m_frameIndex]->GetGPUVirtualAddress());
            // rebind descriptor heaps
            m_commandList->SetDescriptorHeaps(2, heaps);
            m_commandList->SetGraphicsRootDescriptorTable(0, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
            m_commandList->SetGraphicsRootDescriptorTable(1, m_samplerHeap->GetGPUDescriptorHandleForHeapStart());
        }

        m_time += 0.016f;
        IllusionConfig frameConfig = config;
        frameConfig.time = m_time;
        // Write into per-frame mapped constant buffer
        memcpy(m_mappedConstantData[m_frameIndex], &frameConfig, sizeof(IllusionConfig));

        CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_commandList->ResourceBarrier(1, &barrier);

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_frameIndex, m_rtvDescriptorSize);
        m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, NULL);
        const float clearColor[] = { 0.2f, 0.3f, 0.4f, 1.0f };
        m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, NULL);

        m_commandList->SetPipelineState(m_graphicsPso);
        m_commandList->SetGraphicsRootSignature(m_rootSignature);
        // Use GPU virtual address of the per-frame constant buffer (must be 256-byte aligned)
        m_commandList->SetGraphicsRootConstantBufferView(2, m_constantBuffers[m_frameIndex]->GetGPUVirtualAddress());

        ID3D12DescriptorHeap* heaps[] = { m_srvHeap, m_samplerHeap };
        m_commandList->SetDescriptorHeaps(2, heaps);

        m_commandList->SetGraphicsRootDescriptorTable(0, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
        m_commandList->SetGraphicsRootDescriptorTable(1, m_samplerHeap->GetGPUDescriptorHandleForHeapStart());

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

        CHECK_HR(m_commandList->Close(), "Command list close failed");

        ID3D12CommandList* commandLists[] = { m_commandList };
        m_commandQueue->ExecuteCommandLists(1, commandLists);

        hr = m_swapChain->Present(1, 0);
        if (FAILED(hr)) {
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG) {
                if (RecoverDevice()) {
                    Log("Device recovered, retrying render\n");
                    return Render();
                }
                Log("Recovery failed, switching to fallback\n");
                m_fallbackMode = true;
                return RenderFallback();
            }
            char buffer[128];
            sprintf_s(buffer, "Present failed with HRESULT: 0x%08X\n", hr);
            Log(buffer);
            return false;
        }
        WaitForGPU();

        Log("Frame rendered successfully\n");
        return true;
    }

    bool RenderFallback() {
        Log("Rendering in fallback mode...\n");
        if (!m_swapChain || !m_commandQueue || !m_commandList || !m_rtvHeap) return false;

        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
        HRESULT hr = m_commandAllocators[m_frameIndex]->Reset();
        CHECK_HR(hr, "Fallback allocator reset failed");

        hr = m_commandList->Reset(m_commandAllocators[m_frameIndex], NULL);
        CHECK_HR(hr, "Fallback command list reset failed");

        CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_commandList->ResourceBarrier(1, &barrier);

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_frameIndex, m_rtvDescriptorSize);
        m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, NULL);
        const float clearColor[] = { 0.5f, 0.0f, 0.0f, 1.0f };
        m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, NULL);

        barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        m_commandList->ResourceBarrier(1, &barrier);

        CHECK_HR(m_commandList->Close(), "Fallback command list close failed");

        ID3D12CommandList* commandLists[] = { m_commandList };
        m_commandQueue->ExecuteCommandLists(1, commandLists);

        hr = m_swapChain->Present(1, 0);
        CHECK_HR(hr, "Fallback Present failed");
        WaitForGPU();

        Log("Fallback frame rendered\n");
        return true;
    }

private:
    void WaitForGPU() {
        if (!m_commandQueue || !m_fence || !m_fenceEvent) return;
        HRESULT hr = m_commandQueue->Signal(m_fence, ++m_fenceValue);
        if (FAILED(hr)) {
            Log("Signal fence failed\n");
            return;
        }
        if (m_fence->GetCompletedValue() < m_fenceValue) {
            hr = m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
            if (FAILED(hr)) {
                Log("SetEventOnCompletion failed\n");
                return;
            }
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }

    HWND m_hwnd;
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
    ID3D12Resource* m_constantBuffers[FRAME_COUNT];
    uint8_t* m_mappedConstantData[FRAME_COUNT];
    UINT64 m_constantBufferSize;
    ID3D12Resource* m_vertexBuffer;
    IDXGIOutputDuplication* m_d3d11Duplication;
    ID3D11Texture2D* m_d3d11StagingTexture;
    ID3D12Resource* m_d3d12UploadBuffer;
    ID3D12RootSignature* m_rootSignature;
    ID3D12PipelineState* m_computePso;
    ID3D12PipelineState* m_graphicsPso;
    ID3D12Resource* m_disparityTexture; // added for compute output
    ID3D12RootSignature* m_computeRootSignature; // added compute root signature
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
    float m_time;
    int m_recoveryCount;
    bool m_fallbackMode;
};

class LightWeight3DApp {
public:
    LightWeight3DApp() : m_hwnd(nullptr), m_isRunning(false), m_isHidden(false), m_isClickThrough(true), m_frameCount(0) {
        Gdiplus::GdiplusStartupInput gdiplusStartupInput;
        Gdiplus::GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, NULL);
    }

    ~LightWeight3DApp() {
        Cleanup();
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
        if (logFile.is_open()) logFile.close();
    }

    bool Initialize() {
        try {
            Log("Initializing LightWeight3DApp...\n");
            WNDCLASSEX wc = {};
            wc.cbSize = sizeof(WNDCLASSEX);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = WindowProc;
            wc.hInstance = GetModuleHandle(NULL);
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.lpszClassName = L"LightWeight3DClass";
            if (!RegisterClassEx(&wc)) throw ToolException("Window class registration failed");

            HMONITOR hMonitor = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
            MONITORINFOEX monitorInfo = { sizeof(MONITORINFOEX) };
            GetMonitorInfo(hMonitor, &monitorInfo);
            SCREEN_WIDTH = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
            SCREEN_HEIGHT = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;

            m_hwnd = CreateWindowEx(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST, L"LightWeight3DClass",
                L"LightWeight3D", WS_POPUP, monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top,
                SCREEN_WIDTH, SCREEN_HEIGHT, NULL, NULL, GetModuleHandle(NULL), this);
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
            UpdateWindow(m_hwnd);
            Log("App initialized successfully\n");
            return true;
        }
        catch (const ToolException& e) {
            Log(("App initialization failed: " + std::string(e.what()) + "\n").c_str());
            return false;
        }
    }

    void Cleanup() {
        Stop();
        RemoveTrayIcon();
        m_d3dRenderer.Cleanup();
        if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = NULL; }
        Log("App cleanup completed\n");
    }

    void Run() {
        m_isRunning = true;
        m_renderThread = std::thread(&LightWeight3DApp::RenderLoop, this);

        MSG msg = {};
        while (m_isRunning) {
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                if (msg.message == WM_QUIT) {
                    m_isRunning = false;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (m_renderThread.joinable()) m_renderThread.join();
    }

    void Stop() {
        m_isRunning = false;
    }

    void ToggleClickThrough() {
        m_isClickThrough = !m_isClickThrough;
        SetClickThrough(m_isClickThrough);
        Log(m_isClickThrough ? "Click-through enabled\n" : "Click-through disabled\n");
    }

    void ToggleVisibility() {
        m_isHidden = !m_isHidden;
        ShowWindow(m_hwnd, m_isHidden ? SW_HIDE : SW_SHOW);
        Log(m_isHidden ? "Overlay hidden\n" : "Overlay shown\n");
    }

    void ToggleLogging() {
        enableLogging = !enableLogging;
        Log(enableLogging ? "Logging enabled\n" : "Logging disabled\n");
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
            if (app) app->Stop();
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
            AppendMenu(menu, MF_SEPARATOR, 0, NULL);
            AppendMenu(menu, MF_STRING | (config.enable_parallax ? MF_CHECKED : MF_UNCHECKED), 3, L"Parallax Effect");
            AppendMenu(menu, MF_STRING | (config.enable_parallax_barrier ? MF_CHECKED : MF_UNCHECKED), 6, L"Parallax Barrier");
            AppendMenu(menu, MF_STRING | (config.enable_lenticular ? MF_CHECKED : MF_UNCHECKED), 7, L"Lenticular Sheet");
            AppendMenu(menu, MF_STRING | (enableLogging ? MF_CHECKED : MF_UNCHECKED), 8, L"Logging");
            AppendMenu(menu, MF_SEPARATOR, 0, NULL);

            // Outline presets
            UINT outlineState = 0; // 0=off,1=subtle,2=strong
            if (config.outline_intensity <= 0.0001f) outlineState = 0;
            else if (config.outline_intensity < 0.8f) outlineState = 1;
            else outlineState = 2;
            AppendMenu(menu, MF_STRING | (outlineState == 0 ? MF_CHECKED : MF_UNCHECKED), 10, L"Outline: Off");
            AppendMenu(menu, MF_STRING | (outlineState == 1 ? MF_CHECKED : MF_UNCHECKED), 11, L"Outline: Subtle");
            AppendMenu(menu, MF_STRING | (outlineState == 2 ? MF_CHECKED : MF_UNCHECKED), 12, L"Outline: Strong");

            AppendMenu(menu, MF_SEPARATOR, 0, NULL);
            AppendMenu(menu, MF_STRING, 9, L"Exit");
            SetForegroundWindow(m_hwnd);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, m_hwnd, NULL);
            DestroyMenu(menu);
            switch (cmd) {
            case 1: ToggleClickThrough(); break;
            case 2: ToggleVisibility(); break;
            case 3:
                config.enable_parallax = !config.enable_parallax;
                Log(config.enable_parallax ? "Parallax enabled\n" : "Parallax disabled\n");
                break;
            case 6:
                config.enable_parallax_barrier = !config.enable_parallax_barrier;
                config.enable_lenticular = config.enable_parallax_barrier ? 0 : config.enable_lenticular;
                Log(config.enable_parallax_barrier ? "Parallax barrier enabled\n" : "Parallax barrier disabled\n");
                break;
            case 7:
                config.enable_lenticular = !config.enable_lenticular;
                config.enable_parallax_barrier = config.enable_lenticular ? 0 : config.enable_parallax_barrier;
                Log(config.enable_lenticular ? "Lenticular enabled\n" : "Lenticular disabled\n");
                break;
            case 8: ToggleLogging(); break;
            case 10: // Outline Off
                config.outline_width = 0.0f;
                config.outline_intensity = 0.0f;
                Log("Outline disabled (Off)\n");
                break;
            case 11: // Outline Subtle
                config.outline_width = 1.5f;
                config.outline_intensity = 0.6f;
                Log("Outline set to Subtle (width=1.5, intensity=0.6)\n");
                break;
            case 12: // Outline Strong
                config.outline_width = 5.5f;
                config.outline_intensity = 1.0f;
                Log("Outline set to Strong (width=5.5, intensity=1.0)\n");
                break;
             case 9: PostQuitMessage(0); break;
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
        nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        wcscpy_s(nid.szTip, _countof(nid.szTip), L"LightWeight3D");
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
        while (m_isRunning) {
            auto frameStart = std::chrono::high_resolution_clock::now();

            try {
                if (!m_isHidden) {
                    if (!m_d3dRenderer.CaptureDesktop()) {
                        Log("CaptureDesktop failed, using last frame\n");
                    }
                    if (!m_d3dRenderer.Render()) {
                        Log("Render failed\n");
                        if (!m_d3dRenderer.RecoverDevice()) {
                            Log("Unrecoverable error, stopping render loop\n");
                            m_isRunning = false;
                            PostQuitMessage(1);
                            break;
                        }
                    }
                }
            }
            catch (const ToolException& e) {
                Log(("Render loop exception: " + std::string(e.what()) + "\n").c_str());
                if (!m_d3dRenderer.RecoverDevice()) {
                    Log("Unrecoverable error, stopping render loop\n");
                    m_isRunning = false;
                    PostQuitMessage(1);
                    break;
                }
            }

            auto frameEnd = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(frameEnd - frameStart);
            auto targetFrameTime = std::chrono::microseconds(1000000 / TARGET_FPS);
            if (elapsed < targetFrameTime) {
                std::this_thread::sleep_for(targetFrameTime - elapsed);
            }

            if (enableLogging) {
                char buffer[64];
                sprintf_s(buffer, "Frame %zu: %lld us\n", m_frameCount++, elapsed.count());
                Log(buffer);
            }
            else {
                m_frameCount++;
            }
        }
        Log("Render loop stopped\n");
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
        Log("Starting application...\n");
        LightWeight3DApp app;
        if (!app.Initialize()) {
            Log("Initialization failed, exiting\n");
            return 1;
        }
        app.Run();
        Log("Application exited normally\n");
        return 0;
    }
    catch (const ToolException& e) {
        Log(("Application exited with exception: " + std::string(e.what()) + "\n").c_str());
        return 1;
    }
}

// Shaders are stored in external .hlsl files (VertexShader.hlsl, PixelShader.hlsl, DepthCompute.hlsl, etc.)
// Embedded HLSL removed to keep this translation unit valid C++.