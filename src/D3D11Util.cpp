/*
 * This file is part of DLSS Denoiser.
 * Derived from MPCVR-Plus (https://github.com/HumbleUser33/MPCVR-Plus), Source/DX11Helper.cpp,
 * (C) 2019-2026 its authors.
 *
 * DLSS Denoiser is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version. See LICENSE.txt.
 */

#include "D3D11Util.h"

namespace dn {

namespace {
std::mutex g_LogLock;
LogFn g_LogSink = nullptr;
}

void SetLogSink(LogFn fn)
{
	std::lock_guard lock(g_LogLock);
	g_LogSink = fn;
}

void LogLine(const std::wstring& line)
{
	std::lock_guard lock(g_LogLock);
	if (g_LogSink) {
		g_LogSink(line.c_str());
	} else {
		OutputDebugStringW((line + L"\n").c_str());
	}
}

std::wstring HR2Str(HRESULT hr)
{
	return std::format(L"0x{:08X}", (unsigned)hr);
}

} // namespace dn

D3D11_TEXTURE2D_DESC CreateTex2DDesc(DXGI_FORMAT format, UINT width, UINT height, Tex2DType type)
{
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = format;
	desc.SampleDesc = { 1, 0 };
	desc.Usage = D3D11_USAGE_DEFAULT;

	switch (type) {
	default:
	case Tex2D_Default:
		break;
	case Tex2D_DefaultShader:
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		break;
	case Tex2D_DefaultShaderRTarget:
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		break;
	case Tex2D_DefaultShaderRTargetUAV:
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
		break;
	case Tex2D_DefaultShaderUAV:
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		break;
	case Tex2D_StagingRead:
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		break;
	case Tex2D_StagingWrite:
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		break;
	}
	return desc;
}

static HMODULE ThisModule()
{
	HMODULE hMod = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCWSTR)&ThisModule, &hMod);
	return hMod;
}

HRESULT GetShaderResource(int id, const void** ppData, DWORD* pSize)
{
	const HMODULE hMod = ThisModule();
	HRSRC hrsrc = FindResourceW(hMod, MAKEINTRESOURCEW(id), RT_RCDATA);
	if (!hrsrc) {
		return HRESULT_FROM_WIN32(GetLastError());
	}
	HGLOBAL hGlobal = LoadResource(hMod, hrsrc);
	if (!hGlobal) {
		return HRESULT_FROM_WIN32(GetLastError());
	}
	*ppData = LockResource(hGlobal);
	*pSize = SizeofResource(hMod, hrsrc);
	return (*ppData && *pSize) ? S_OK : E_FAIL;
}
