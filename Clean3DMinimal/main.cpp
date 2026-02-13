#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

struct Vertex
{
    float px, py;
    float u, v;
};

struct ParamsCB
{
    float strength;
    float enabled;
    float texelSizeX;
    float texelSizeY;
};

static const char* kVS = R"(
struct VSIn
{
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut main(VSIn i)
{
    VSOut o;
    o.pos = float4(i.pos, 0.0, 1.0);
    o.uv = i.uv;
    return o;
}
)";

static const char* kPS = R"(
Texture2D srcTex : register(t0);
SamplerState sampLinear : register(s0);

cbuffer Params : register(b0)
{
    float strength;
    float enabled;
    float texelSizeX;
    float texelSizeY;
}

float luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    float4 base = srcTex.Sample(sampLinear, uv);
    if (enabled < 0.5)
    {
        return base;
    }

    float lum = luma(base.rgb);

    // Tiny edge estimate for more local contrast in pseudo-depth.
    float lumR = luma(srcTex.Sample(sampLinear, uv + float2(texelSizeX, 0)).rgb);
    float lumL = luma(srcTex.Sample(sampLinear, uv - float2(texelSizeX, 0)).rgb);
    float edge = saturate(abs(lumR - lumL) * 2.0);

    // Brighter -> farther, darker/edge-heavy -> nearer.
    float depth = saturate((1.0 - lum) * 0.8 + edge * 0.2);
    float shift = (depth - 0.5) * strength;

    float2 shiftedUV = float2(saturate(uv.x + shift), uv.y);
    return srcTex.Sample(sampLinear, shiftedUV);
}
)";

class App
{
public:
    bool Init(HINSTANCE hInst);
    int Run();

private:
    bool InitWindow(HINSTANCE hInst);
    bool InitD3D();
    bool InitDuplication();
    bool CreateRenderTargets();
    bool CreatePipeline();
    bool CreateCaptureResources(UINT w, UINT h);
    void UpdateTitle() const;
    void OnResize(UINT w, UINT h);
    void CaptureFrame();
    void Render();
    void Cleanup();

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    HWND m_hwnd = nullptr;
    UINT m_winW = 1280;
    UINT m_winH = 720;

    bool m_running = true;
    bool m_effectEnabled = true;
    float m_strength = 0.03f;

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_ctx;
    ComPtr<IDXGISwapChain> m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_rtv;

    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_ps;
    ComPtr<ID3D11InputLayout> m_layout;
    ComPtr<ID3D11Buffer> m_vb;
    ComPtr<ID3D11Buffer> m_cb;
    ComPtr<ID3D11SamplerState> m_sampler;

    ComPtr<IDXGIOutputDuplication> m_duplication;
    UINT m_captureW = 0;
    UINT m_captureH = 0;
    ComPtr<ID3D11Texture2D> m_captureTex;
    ComPtr<ID3D11ShaderResourceView> m_captureSRV;
    bool m_frameAcquired = false;
};

bool CompileShader(const char* src, const char* entry, const char* profile, ComPtr<ID3DBlob>& blob)
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, profile, flags, 0, &blob, &errors);
    if (FAILED(hr))
    {
        if (errors)
        {
            OutputDebugStringA((const char*)errors->GetBufferPointer());
        }
        return false;
    }
    return true;
}

bool App::InitWindow(HINSTANCE hInst)
{
    WNDCLASSW wc = {};
    wc.hInstance = hInst;
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = L"Clean3DMinimalWnd";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

    if (!RegisterClassW(&wc))
    {
        return false;
    }

    RECT rc = { 0, 0, (LONG)m_winW, (LONG)m_winH };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowW(
        wc.lpszClassName,
        L"Clean3DMinimal",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rc.right - rc.left,
        rc.bottom - rc.top,
        nullptr,
        nullptr,
        hInst,
        this);

    if (!m_hwnd)
    {
        return false;
    }

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateTitle();
    return true;
}

bool App::CreateRenderTargets()
{
    m_rtv.Reset();

    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return false;

    hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv);
    return SUCCEEDED(hr);
}

bool App::CreatePipeline()
{
    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;

    if (!CompileShader(kVS, "main", "vs_5_0", vsBlob)) return false;
    if (!CompileShader(kPS, "main", "ps_5_0", psBlob)) return false;

    HRESULT hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vs);
    if (FAILED(hr)) return false;

    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_ps);
    if (FAILED(hr)) return false;

    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    hr = m_device->CreateInputLayout(il, ARRAYSIZE(il), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_layout);
    if (FAILED(hr)) return false;

    Vertex quad[] = {
        { -1.0f, -1.0f, 0.0f, 1.0f },
        { -1.0f,  1.0f, 0.0f, 0.0f },
        {  1.0f, -1.0f, 1.0f, 1.0f },
        {  1.0f,  1.0f, 1.0f, 0.0f },
    };

    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = sizeof(quad);
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbData = { quad };
    hr = m_device->CreateBuffer(&vbDesc, &vbData, &m_vb);
    if (FAILED(hr)) return false;

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(ParamsCB);
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_cb);
    if (FAILED(hr)) return false;

    D3D11_SAMPLER_DESC samp = {};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = m_device->CreateSamplerState(&samp, &m_sampler);
    return SUCCEEDED(hr);
}

bool App::CreateCaptureResources(UINT w, UINT h)
{
    m_captureSRV.Reset();
    m_captureTex.Reset();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &m_captureTex);
    if (FAILED(hr)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = td.Format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    hr = m_device->CreateShaderResourceView(m_captureTex.Get(), &sd, &m_captureSRV);
    if (FAILED(hr)) return false;

    m_captureW = w;
    m_captureH = h;
    return true;
}

bool App::InitDuplication()
{
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = m_device.As(&dxgiDevice);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(0, &output); // primary
    if (FAILED(hr)) return false;

    DXGI_OUTPUT_DESC outDesc = {};
    output->GetDesc(&outDesc);

    ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    if (FAILED(hr)) return false;

    hr = output1->DuplicateOutput(m_device.Get(), &m_duplication);
    if (FAILED(hr)) return false;

    DXGI_OUTDUPL_DESC dupDesc = {};
    m_duplication->GetDesc(&dupDesc);
    return CreateCaptureResources(dupDesc.ModeDesc.Width, dupDesc.ModeDesc.Height);
}

bool App::InitD3D()
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL flOut = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = m_winW;
    sd.BufferDesc.Height = m_winH;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels,
        ARRAYSIZE(levels),
        D3D11_SDK_VERSION,
        &sd,
        &m_swapChain,
        &m_device,
        &flOut,
        &m_ctx);
    if (FAILED(hr)) return false;

    if (!CreateRenderTargets()) return false;
    if (!CreatePipeline()) return false;
    if (!InitDuplication()) return false;

    return true;
}

void App::OnResize(UINT w, UINT h)
{
    if (!m_swapChain || w == 0 || h == 0) return;

    m_winW = w;
    m_winH = h;

    m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    m_rtv.Reset();

    if (SUCCEEDED(m_swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0)))
    {
        CreateRenderTargets();
    }
}

void App::CaptureFrame()
{
    if (!m_duplication) return;

    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
    ComPtr<IDXGIResource> desktopResource;
    HRESULT hr = m_duplication->AcquireNextFrame(0, &frameInfo, &desktopResource);
    m_frameAcquired = false;

    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        return;
    }

    if (hr == DXGI_ERROR_ACCESS_LOST)
    {
        m_duplication.Reset();
        InitDuplication();
        return;
    }

    if (FAILED(hr))
    {
        return;
    }

    ComPtr<ID3D11Texture2D> desktopTex;
    hr = desktopResource.As(&desktopTex);
    if (SUCCEEDED(hr) && desktopTex && m_captureTex)
    {
        D3D11_TEXTURE2D_DESC srcDesc = {};
        desktopTex->GetDesc(&srcDesc);

        if (srcDesc.Width != m_captureW || srcDesc.Height != m_captureH)
        {
            CreateCaptureResources(srcDesc.Width, srcDesc.Height);
        }

        m_ctx->CopyResource(m_captureTex.Get(), desktopTex.Get());
    }

    m_frameAcquired = true;
    m_duplication->ReleaseFrame();
    m_frameAcquired = false;
}

void App::Render()
{
    CaptureFrame();

    float clear[4] = { 0.02f, 0.02f, 0.02f, 1.0f };
    m_ctx->ClearRenderTargetView(m_rtv.Get(), clear);

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = (FLOAT)m_winW;
    vp.Height = (FLOAT)m_winH;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_ctx->RSSetViewports(1, &vp);

    m_ctx->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    m_ctx->IASetInputLayout(m_layout.Get());
    m_ctx->IASetVertexBuffers(0, 1, m_vb.GetAddressOf(), &stride, &offset);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    m_ctx->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    m_ctx->PSSetShaderResources(0, 1, m_captureSRV.GetAddressOf());

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(m_ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        ParamsCB* cb = (ParamsCB*)mapped.pData;
        cb->strength = m_strength;
        cb->enabled = m_effectEnabled ? 1.0f : 0.0f;
        cb->texelSizeX = (m_captureW > 0) ? (1.0f / (float)m_captureW) : 0.0f;
        cb->texelSizeY = (m_captureH > 0) ? (1.0f / (float)m_captureH) : 0.0f;
        m_ctx->Unmap(m_cb.Get(), 0);
    }

    m_ctx->PSSetConstantBuffers(0, 1, m_cb.GetAddressOf());
    m_ctx->Draw(4, 0);

    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
    m_ctx->PSSetShaderResources(0, 1, nullSrv);

    m_swapChain->Present(1, 0);
}

void App::UpdateTitle() const
{
    wchar_t title[256] = {};
    swprintf_s(title, L"Clean3DMinimal - Strength: %.3f - Effect: %s", m_strength, m_effectEnabled ? L"ON" : L"OFF");
    SetWindowTextW(m_hwnd, title);
}

bool App::Init(HINSTANCE hInst)
{
    if (!InitWindow(hInst)) return false;
    if (!InitD3D()) return false;
    return true;
}

int App::Run()
{
    MSG msg = {};
    while (m_running)
    {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                m_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (!m_running) break;
        Render();
    }

    Cleanup();
    return (int)msg.wParam;
}

void App::Cleanup()
{
    if (m_duplication && m_frameAcquired)
    {
        m_duplication->ReleaseFrame();
        m_frameAcquired = false;
    }
}

LRESULT CALLBACK App::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }

    if (!self)
    {
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    switch (msg)
    {
    case WM_SIZE:
        self->OnResize(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_KEYDOWN:
        switch (wParam)
        {
        case VK_ESCAPE:
            PostQuitMessage(0);
            return 0;
        case VK_SPACE:
            self->m_effectEnabled = !self->m_effectEnabled;
            self->UpdateTitle();
            return 0;
        case VK_UP:
            self->m_strength = std::min(0.25f, self->m_strength + 0.005f);
            self->UpdateTitle();
            return 0;
        case VK_DOWN:
            self->m_strength = std::max(0.0f, self->m_strength - 0.005f);
            self->UpdateTitle();
            return 0;
        default:
            break;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    App app;
    if (!app.Init(hInstance))
    {
        MessageBoxW(nullptr, L"Initialization failed.", L"Clean3DMinimal", MB_ICONERROR);
        return -1;
    }

    return app.Run();
}
