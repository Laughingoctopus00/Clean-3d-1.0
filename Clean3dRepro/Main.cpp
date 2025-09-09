#include <windows.h>
#include <wrl.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <gdiplus.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <chrono>
#include <string>
#include <shellapi.h>
#include <numeric>
#include <thread>
#include <atomic>
#include <d3dx12.h>
#include <dcomp.h>
#include <dxgi.h>
#include <CommCtrl.h>
#include <vector>
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "d3d11.lib")

using namespace Microsoft::WRL;

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    MessageBoxA(nullptr, "Placeholder build for Clean3dRepro", "Info", MB_OK);
    return 0;
}
