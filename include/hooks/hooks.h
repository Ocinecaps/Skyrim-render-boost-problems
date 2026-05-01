#pragma once
#include <windows.h>
#include <d3d9.h>

bool InstallHooks();
void RemoveHooks();
extern "C" IDirect3D9* WINAPI HookedDirect3DCreate9(UINT SDKVersion);
