Clean3D - Real-Time 3D Desktop Overlay
Clean3D is a lightweight, DirectX 12-based application that transforms your desktop into a real-time 3D experience using parallax barrier and lenticular effects. Designed for high-resolution displays (optimized for 4096x2160 @ 104 PPI), it captures your desktop and applies depth-based visual effects, creating an immersive pseudo-3D overlay without requiring specialized hardware.
Features
Parallax Barrier Effect: Adds thin vertical black strips (~0.25 pixels wide) to interleave left/right views, simulating depth as you move your head.

Lenticular Effect: Smoothly blends left/right views over ~1 pixel-wide lenses for a glasses-free 3D experience.

Desktop Capture: Uses DirectX Desktop Duplication API to mirror your screen in real-time.

Customizable: Toggle effects via a system tray menu or hotkeys (Ctrl+Alt+C for click-through, Ctrl+Alt+H to hide/show).

Performance: Targets 60 FPS with minimal resource overhead, leveraging NVIDIA RTX hardware acceleration (tested on RTX 2060).

Transparency & Click-Through: Runs as a topmost overlay with adjustable opacity and optional mouse passthrough.

Technical Details
Platform: Windows 10/11

Graphics API: DirectX 12 (with D3D11 fallback for desktop duplication)

Shader Language: HLSL (external VertexShader.hlsl and PixelShader.hlsl)

Resolution: Hardcoded for 4096x2160, easily adaptable to other resolutions

Dependencies: DirectX 12 SDK, Windows SDK, GDI+ for tray icon

Building & Running
Prerequisites: Visual Studio with C++ and DirectX support, Windows SDK.

Setup:
Clone the repo.

Place VertexShader.hlsl and PixelShader.hlsl in the output directory (e.g., bin/Debug/).

Build the solution in Debug or Release mode.

Run: Launch with admin privileges for desktop duplication to work.

Usage: Right-click the tray icon to toggle effects or use hotkeys.

Notes
Optimized for 104 PPI displays; adjust shader constants (stripWidth, barrierWidth, lensWidth) in PixelShader.hlsl for other DPIs.

Logs to debug_log.txt for troubleshooting (toggle via tray menu).

Requires an NVIDIA GPU with DirectX 12 support for best performance.

Multi-monitor support

Configurable resolution via UI

Additional 3D effects (e.g., chromatic aberration, DOF)
