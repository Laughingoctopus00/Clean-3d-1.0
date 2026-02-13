# Clean3DMinimal

A minimal Windows desktop app that captures the primary monitor with **DXGI Desktop Duplication** and renders it with **D3D11**. A runtime-compiled pixel shader applies a lightweight pseudo-depth parallax effect by deriving approximate depth from luminance + local edge contrast and shifting horizontal sampling.

## Features

- Win32 windowed application
- D3D11 rendering path (fullscreen quad)
- DXGI output duplication capture (primary display)
- Runtime HLSL compilation using `D3DCompile`
- Keyboard controls:
  - **Up/Down**: increase/decrease parallax strength
  - **Space**: toggle effect on/off
  - **Esc**: quit

## Requirements

- Windows 10 or Windows 11
- Visual Studio 2022 with:
  - **Desktop development with C++**
  - Windows 10/11 SDK

## Build (Visual Studio 2022)

1. Open `Clean3DMinimal.sln` in Visual Studio 2022.
2. Select configuration:
   - `Debug | x64` or `Release | x64`
3. Build: **Build > Build Solution**.
4. Run with **F5** (debug) or **Ctrl+F5** (without debugger).

## Notes

- The app uses only Windows SDK components (`d3d11`, `dxgi`, `d3dcompiler`).
- If desktop duplication access is lost (display mode switch, remote session changes), duplication is recreated automatically.
