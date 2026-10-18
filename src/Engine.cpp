/*
 * This file is part of DLSS Denoiser.
 *
 * DLSS Denoiser is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version. See LICENSE.txt.
 */

#include "Engine.h"
#include "ShaderIds.h"
#include <dxgi1_6.h>
#include <DirectXPackedVector.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace {

constexpr float kLinearScale = 100.0f / 10000.0f;   // 1.0 = 100 nits

HRESULT LoadComputeShader(ID3D11Device* pDevice, int id, ID3D11ComputeShader** ppShader)
{
	const void* data = nullptr;
	DWORD size = 0;
	HRESULT hr = GetShaderResource(id, &data, &size);
	return SUCCEEDED(hr) ? pDevice->CreateComputeShader(data, size, nullptr, ppShader) : hr;
}

} // namespace

bool CEngine::Init(int deviceIndex, const wchar_t* dllPath)
{
	if (m_pDevice) {
		return true;
	}
	CComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
		return Fail(L"no DXGI factory");
	}
	CComPtr<IDXGIAdapter1> adapter;
	int nvidia = 0;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 desc = {};
		adapter->GetDesc1(&desc);
		if (desc.VendorId == 0x10DE && nvidia++ == deviceIndex) {
			m_adapter = desc.Description;
			break;
		}
		adapter.Release();
	}
	if (!adapter) {
		return Fail(std::format(L"no NVIDIA adapter number {}", deviceIndex));
	}
	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1 };
	if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 1, D3D11_SDK_VERSION,
			&m_pDevice, nullptr, &m_pContext))) {
		return Fail(std::format(L"cannot create a Direct3D 11.1 device on {}", m_adapter));
	}
	if (FAILED(LoadComputeShader(m_pDevice, IDS_CS_CONVERT_IN, &m_pCSIn))
			|| FAILED(LoadComputeShader(m_pDevice, IDS_CS_CONVERT_OUT, &m_pCSOut))) {
		Shutdown();
		return Fail(L"the conversion shaders are missing from the module");
	}
	D3D11_BUFFER_DESC bufferDesc = { 16 * sizeof(float), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
	if (FAILED(m_pDevice->CreateBuffer(&bufferDesc, nullptr, &m_pConstants))) {
		Shutdown();
		return Fail(L"no constant buffer");
	}
	if (!m_Dlss.Init(m_pDevice, dllPath)) {
		const std::wstring why = m_Dlss.GetStatusLine();
		Shutdown();
		return Fail(L"DLSS: " + why);
	}
	return true;
}

void CEngine::Shutdown()
{
	m_Motion[0].Release();
	m_Motion[1].Release();
	m_Dlss.Shutdown();
	for (int i = 0; i < 3; i++) {
		m_InPlanes[i].Release();
		m_RefPlanes[i].Release();
		m_OutPlanes[i].Release();
		m_pOutTargets[i].Release();
		m_pOutStaging[i].Release();
	}
	m_MotionPicture.Release();
	m_RefPicture.Release();
	m_pMotionPictureTarget.Release();
	m_pRefPictureTarget.Release();
	m_pDlssInTarget[0].Release();
	m_pDlssInTarget[1].Release();
	m_ExactMotion.Release();
	m_ZeroMotion.Release();
	m_pConstants.Release();
	m_pCSIn.Release();
	m_pCSOut.Release();
	m_pContext.Release();
	m_pDevice.Release();
	m_bConfigured = false;
	m_Config = Config();
}

bool CEngine::Configure(const Config& config)
{
	if (!m_pDevice) {
		return Fail(L"not initialised");
	}
	if (m_bConfigured && config == m_Config) {
		return true;
	}
	m_bConfigured = false;
	const UINT W = config.width, H = config.height;
	const UINT PW = W + 2 * config.pad, PH = H + 2 * config.pad;
	if (W < 64 || H < 64 || config.pad > std::min(W, H) / 2) {
		return Fail(std::format(L"{}x{} with a band of {} cannot be processed", W, H, config.pad));
	}

	CDlssSR::Config dlss;
	dlss.width = PW;
	dlss.height = PH;
	dlss.preset = config.preset;
	dlss.hdr = config.linear;
	dlss.autoExposure = config.autoExposure;
	dlss.inputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	const int lanes = config.passes >= 2 ? 2 : 1;
	if (lanes < 2) {
		m_Dlss.ReleaseFeature(1);
	}
	for (int lane = 0; lane < lanes; lane++) {
		if (!m_Dlss.CreateFeature(dlss, lane)) {
			return Fail(L"DLSS: " + m_Dlss.GetStatusLine(lane));
		}
		m_pDlssInTarget[lane].Release();
		if (FAILED(m_pDevice->CreateUnorderedAccessView(m_Dlss.GetInput(lane)->pTexture, nullptr, &m_pDlssInTarget[lane]))) {
			return Fail(L"DLSS's input takes no unordered access view");
		}
	}

	const DXGI_FORMAT planeFormat = config.half ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R32_FLOAT;
	HRESULT hr = S_OK;
	for (int i = 0; i < 3 && SUCCEEDED(hr); i++) {
		hr = m_InPlanes[i].CheckCreate(m_pDevice, planeFormat, W, H, Tex2D_DefaultShader);
		if (SUCCEEDED(hr)) {
			hr = m_RefPlanes[i].CheckCreate(m_pDevice, planeFormat, W, H, Tex2D_DefaultShader);
		}
		if (SUCCEEDED(hr)) {
			hr = m_OutPlanes[i].CheckCreate(m_pDevice, planeFormat, W, H, Tex2D_DefaultShaderUAV);
		}
		if (SUCCEEDED(hr)) {
			m_pOutTargets[i].Release();
			hr = m_pDevice->CreateUnorderedAccessView(m_OutPlanes[i].pTexture, nullptr, &m_pOutTargets[i]);
		}
		if (SUCCEEDED(hr)) {
			m_pOutStaging[i].Release();
			const D3D11_TEXTURE2D_DESC desc = CreateTex2DDesc(planeFormat, W, H, Tex2D_StagingRead);
			hr = m_pDevice->CreateTexture2D(&desc, nullptr, &m_pOutStaging[i]);
		}
	}
	if (SUCCEEDED(hr)) {
		hr = m_MotionPicture.CheckCreate(m_pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, PW, PH, Tex2D_DefaultShaderUAV);
	}
	if (SUCCEEDED(hr)) {
		m_pMotionPictureTarget.Release();
		hr = m_pDevice->CreateUnorderedAccessView(m_MotionPicture.pTexture, nullptr, &m_pMotionPictureTarget);
	}
	if (SUCCEEDED(hr)) {
		hr = m_RefPicture.CheckCreate(m_pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, PW, PH, Tex2D_DefaultShaderUAV);
	}
	if (SUCCEEDED(hr)) {
		m_pRefPictureTarget.Release();
		hr = m_pDevice->CreateUnorderedAccessView(m_RefPicture.pTexture, nullptr, &m_pRefPictureTarget);
	}
	if (SUCCEEDED(hr)) {
		hr = m_ExactMotion.CheckCreate(m_pDevice, DXGI_FORMAT_R32G32_FLOAT, PW, PH, Tex2D_DefaultShader);
	}
	if (FAILED(hr)) {
		return Fail(std::format(L"cannot create the textures for {}x{} ({})", W, H, dn::HR2Str(hr)));
	}

	for (int lane = 0; lane < 2; lane++) {
		m_Motion[lane].Release();
		if (config.motion && lane < lanes) {
			hr = m_Motion[lane].Create(m_pDevice, m_pContext, PW, PH, config.motionSettings);
			if (FAILED(hr)) {
				return Fail(std::format(L"cannot create the motion field ({})", dn::HR2Str(hr)));
			}
			DLog(L"CEngine: motion {}: {}", lane, m_Motion[lane].GetStatusLine());
		}
		m_bFirst[lane] = true;
		m_history[lane] = 0;
	}
	m_Config = config;
	m_bConfigured = true;
	DLog(L"CEngine: {}", GetStatus());
	return true;
}

std::wstring CEngine::GetStatus() const
{
	return std::format(L"{}, {}x{} + {} band, DLSS {}", m_adapter, m_Config.width, m_Config.height, m_Config.pad,
		m_Dlss.GetStatusLine());
}

void CEngine::Upload(ID3D11Texture2D* pTexture, const void* data, ptrdiff_t stride)
{
	m_pContext->UpdateSubresource(pTexture, 0, nullptr, data, (UINT)stride, 0);
}

void CEngine::Convert(ID3D11ComputeShader* pShader, std::initializer_list<ID3D11ShaderResourceView*> inputs,
	std::initializer_list<ID3D11UnorderedAccessView*> outputs, bool writeDlss, UINT groupsX, UINT groupsY)
{
	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (SUCCEEDED(m_pContext->Map(m_pConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
		const float constants[16] = {
			(float)m_Config.pad, m_Config.linear ? 1.0f : 0.0f, kLinearScale, writeDlss ? 1.0f : 0.0f,
			(float)m_Config.width, (float)m_Config.height
		};
		memcpy(mr.pData, constants, sizeof(constants));
		m_pContext->Unmap(m_pConstants, 0);
	}
	ID3D11ShaderResourceView* srvs[3] = {};
	std::copy_n(inputs.begin(), std::min<size_t>(inputs.size(), 3), srvs);
	ID3D11UnorderedAccessView* uavs[3] = {};
	std::copy_n(outputs.begin(), std::min<size_t>(outputs.size(), 3), uavs);
	ID3D11Buffer* pConstants = m_pConstants;
	m_pContext->CSSetShader(pShader, nullptr, 0);
	m_pContext->CSSetShaderResources(0, 3, srvs);
	m_pContext->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
	m_pContext->CSSetConstantBuffers(0, 1, &pConstants);
	m_pContext->Dispatch(groupsX, groupsY, 1);
	ID3D11ShaderResourceView* noViews[3] = {};
	ID3D11UnorderedAccessView* noTargets[3] = {};
	m_pContext->CSSetShaderResources(0, 3, noViews);
	m_pContext->CSSetUnorderedAccessViews(0, 3, noTargets, nullptr);
}

bool CEngine::Process(const Planes& in, const Planes* ref, const float* exactMotion, bool reset, const OutPlanes& out, int lane)
{
	m_bCut = false;
	if (!m_bConfigured) {
		return Fail(L"not configured");
	}
	if (lane < 0 || lane >= (m_Config.passes >= 2 ? 2 : 1)) {
		return Fail(std::format(L"no lane {}", lane));
	}
	CMotionField& motion = m_Motion[lane];
	const UINT W = m_Config.width, H = m_Config.height, P = m_Config.pad;
	const UINT PW = W + 2 * P, PH = H + 2 * P;

	for (int i = 0; i < 3; i++) {
		Upload(m_InPlanes[i].pTexture, in.data[i], in.stride[i]);
	}
	Convert(m_pCSIn, { m_InPlanes[0].pShaderResource, m_InPlanes[1].pShaderResource, m_InPlanes[2].pShaderResource },
		{ m_pDlssInTarget[lane], m_pMotionPictureTarget }, true, (PW + 7) / 8, (PH + 7) / 8);

	ID3D11Texture2D* pMotion = nullptr;
	if (exactMotion) {
		// The band takes the vector of the nearest picture pixel.
		std::vector<float> padded((size_t)PW * PH * 2);
		for (UINT y = 0; y < PH; y++) {
			const UINT sy = std::min(y >= P ? y - P : 0u, H - 1);
			for (UINT x = 0; x < PW; x++) {
				const UINT sx = std::min(x >= P ? x - P : 0u, W - 1);
				padded[((size_t)y * PW + x) * 2 + 0] = exactMotion[((size_t)sy * W + sx) * 2 + 0];
				padded[((size_t)y * PW + x) * 2 + 1] = exactMotion[((size_t)sy * W + sx) * 2 + 1];
			}
		}
		m_pContext->UpdateSubresource(m_ExactMotion.pTexture, 0, nullptr, padded.data(), PW * 8, 0);
		pMotion = m_ExactMotion.pTexture;
	} else if (m_Config.motion) {
		if (reset) {
			motion.Reset();
		}
		ID3D11ShaderResourceView* pSource = m_MotionPicture.pShaderResource;
		if (ref) {
			for (int i = 0; i < 3; i++) {
				Upload(m_RefPlanes[i].pTexture, ref->data[i], ref->stride[i]);
			}
			Convert(m_pCSIn, { m_RefPlanes[0].pShaderResource, m_RefPlanes[1].pShaderResource, m_RefPlanes[2].pShaderResource },
				{ nullptr, m_pRefPictureTarget }, false, (PW + 7) / 8, (PH + 7) / 8);
			pSource = m_RefPicture.pShaderResource;
		}
		if (motion.Process(m_pContext, pSource)) {
			pMotion = motion.GetMotion();
		}
		m_bCut = motion.WasCut();
	}

	const bool restart = reset || m_bCut || m_bFirst[lane];
	if (!m_Dlss.Evaluate(pMotion, m_Config.exposure, restart, lane)) {
		return Fail(L"DLSS: " + m_Dlss.GetStatusLine(lane));
	}
	m_bFirst[lane] = false;
	m_history[lane] = restart ? 0 : std::min(m_history[lane] + 1, 1 << 20);

	Convert(m_pCSOut, { m_Dlss.GetOutput(lane)->pShaderResource }, { m_pOutTargets[0], m_pOutTargets[1], m_pOutTargets[2] },
		false, (W + 7) / 8, (H + 7) / 8);

	const size_t bytes = m_Config.half ? 2 : 4;
	for (int i = 0; i < 3; i++) {
		m_pContext->CopyResource(m_pOutStaging[i], m_OutPlanes[i].pTexture);
	}
	for (int i = 0; i < 3; i++) {
		D3D11_MAPPED_SUBRESOURCE mr = {};
		if (FAILED(m_pContext->Map(m_pOutStaging[i], 0, D3D11_MAP_READ, 0, &mr))) {
			return Fail(L"cannot read the result back");
		}
		for (UINT y = 0; y < H; y++) {
			memcpy((BYTE*)out.data[i] + y * out.stride[i], (const BYTE*)mr.pData + (size_t)y * mr.RowPitch, W * bytes);
		}
		m_pContext->Unmap(m_pOutStaging[i], 0);
	}
	return true;
}

bool CEngine::ReadMotion(std::vector<float>& rg)
{
	ID3D11Texture2D* pMotion = m_Config.motion ? m_Motion[0].GetMotion() : nullptr;
	if (!pMotion) {
		return false;
	}
	const UINT W = m_Config.width, H = m_Config.height, P = m_Config.pad;
	D3D11_TEXTURE2D_DESC desc = {};
	pMotion->GetDesc(&desc);
	desc = CreateTex2DDesc(desc.Format, desc.Width, desc.Height, Tex2D_StagingRead);
	CComPtr<ID3D11Texture2D> stage;
	if (FAILED(m_pDevice->CreateTexture2D(&desc, nullptr, &stage))) {
		return false;
	}
	m_pContext->CopyResource(stage, pMotion);
	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (FAILED(m_pContext->Map(stage, 0, D3D11_MAP_READ, 0, &mr))) {
		return false;
	}
	rg.resize((size_t)W * H * 2);
	for (UINT y = 0; y < H; y++) {
		memcpy(&rg[(size_t)y * W * 2], (const BYTE*)mr.pData + (size_t)(y + P) * mr.RowPitch + P * 8, W * 8);
	}
	m_pContext->Unmap(stage, 0);
	return true;
}

void CEngine::Blend(const OutPlanes& a, float wa, const OutPlanes& b, float wb, UINT width, UINT height, bool half)
{
	const float ka = wa / (wa + wb), kb = 1.0f - ka;
	for (int p = 0; p < 3; p++) {
		for (UINT y = 0; y < height; y++) {
			if (half) {
				auto* pa = (DirectX::PackedVector::HALF*)((BYTE*)a.data[p] + y * a.stride[p]);
				const auto* pb = (const DirectX::PackedVector::HALF*)((const BYTE*)b.data[p] + y * b.stride[p]);
				for (UINT x = 0; x < width; x++) {
					pa[x] = DirectX::PackedVector::XMConvertFloatToHalf(ka * DirectX::PackedVector::XMConvertHalfToFloat(pa[x])
						+ kb * DirectX::PackedVector::XMConvertHalfToFloat(pb[x]));
				}
			} else {
				float* pa = (float*)((BYTE*)a.data[p] + y * a.stride[p]);
				const float* pb = (const float*)((const BYTE*)b.data[p] + y * b.stride[p]);
				for (UINT x = 0; x < width; x++) {
					pa[x] = ka * pa[x] + kb * pb[x];
				}
			}
		}
	}
}