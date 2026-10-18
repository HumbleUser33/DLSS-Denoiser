/*
 * This file is part of DLSS Denoiser.
 * Derived from MPCVR-Plus (https://github.com/HumbleUser33/MPCVR-Plus), Source/DX11Helper.h,
 * (C) 2019-2026 its authors.
 *
 * DLSS Denoiser is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version. See LICENSE.txt.
 */

#pragma once

#include "Common.h"

enum Tex2DType {
	Tex2D_Default,
	Tex2D_DefaultShader,
	Tex2D_DefaultShaderRTarget,
	Tex2D_DefaultShaderRTargetUAV,   // NGX writes its output through a UAV
	Tex2D_DefaultShaderUAV,
	Tex2D_StagingRead,
	Tex2D_StagingWrite,
};

D3D11_TEXTURE2D_DESC CreateTex2DDesc(DXGI_FORMAT format, UINT width, UINT height, Tex2DType type);

struct Tex2D_t
{
	CComPtr<ID3D11Texture2D> pTexture;
	D3D11_TEXTURE2D_DESC desc = {};
	CComPtr<ID3D11ShaderResourceView> pShaderResource;
	Tex2DType type = Tex2D_Default;

	HRESULT CheckCreate(ID3D11Device* pDevice, DXGI_FORMAT format, UINT width, UINT height, Tex2DType kind) {
		if (!width || !height) {
			return E_FAIL;
		}
		if (format == desc.Format && width == desc.Width && height == desc.Height && kind == type && pTexture) {
			return S_OK;
		}
		return Create(pDevice, format, width, height, kind);
	}

	HRESULT Create(ID3D11Device* pDevice, DXGI_FORMAT format, UINT width, UINT height, Tex2DType kind) {
		Release();
		const D3D11_TEXTURE2D_DESC texdesc = CreateTex2DDesc(format, width, height, kind);
		HRESULT hr = pDevice->CreateTexture2D(&texdesc, nullptr, &pTexture);
		if (SUCCEEDED(hr)) {
			pTexture->GetDesc(&desc);
			type = kind;
			if (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
				hr = pDevice->CreateShaderResourceView(pTexture, nullptr, &pShaderResource);
				if (FAILED(hr)) {
					Release();
				}
			}
		}
		return hr;
	}

	void Release() {
		pShaderResource.Release();
		pTexture.Release();
		desc = {};
		type = Tex2D_Default;
	}
};

// A compiled shader embedded in this module as an RCDATA resource.
HRESULT GetShaderResource(int id, const void** ppData, DWORD* pSize);
