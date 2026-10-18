/*
 * This file is part of DLSS Denoiser.
 * Derived from MPCVR-Plus (https://github.com/HumbleUser33/MPCVR-Plus), Source/DLSS/DlssStabilizer.cpp,
 * (C) 2026 its authors.
 *
 * DLSS Denoiser is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version. See LICENSE.txt.
 */

#include "MotionField.h"
#include "ShaderIds.h"
#include <cmath>

namespace {

// Measured in MPCVR-Plus (tools/dlssnr_probe --tstab, --tsrstill), on flow at about 540
// lines: distances in flow pixels of that flow, a quarter of a 4K picture's.
constexpr float kConsistency  = 1.0f;    // forward/backward mismatch where trust starts to fall
constexpr float kCostLow      = 48.0f;   // matching cost where trust starts to fall
constexpr float kCostHigh     = 160.0f;  // and where it is gone
constexpr float kTrustedShare = 0.05f;   // fewer trusted blocks than this share: no global motion

// The geometry the cleaning was measured with on a 4K film: flow at a quarter of the
// size, a vector per 4x4 flow pixels -- 4 picture pixels per flow pixel, 16 per vector.
constexpr float kRefFlowFactor = 4.0f;
constexpr float kRefBlockPixels = 16.0f;

// Gauss-Newton steps of the tile refinement (cs_tiles.hlsl) on each blur of the flow frames.
constexpr int kTileSteps = 32;
// The texture against the grain a tile's own motion needs (cs_tiles.hlsl): white grain on a
// clear sky measured 0.02 or less; a film's own grain, clumped, measures 0.06 to 0.08 on a
// sky, as much as on the trees it must follow, and passes.
constexpr float kTileTextureLow = 0.025f, kTileTextureHigh = 0.04f;

// A cut (cs_dlss_scene_cut.hlsl), chosen in MPCVR-Plus on 48 minutes of six films
// (--tcutscan) on frames of about 540 lines, where ffmpeg's scene score passed 0.3 on 296
// pictures: every one called, 571 in all.
constexpr float kCutHistogram   = 0.20f;
constexpr float kCutResidual    = 0.001f;
constexpr float kCutLightHist   = 0.25f;
constexpr float kCutLightRatio  = 30.0f;
constexpr float kCutCorrelation = 0.25f;
constexpr float kCutRatio       = 3.0f;

HRESULT LoadShader(ID3D11Device* pDevice, int id, ID3D11ComputeShader** ppShader)
{
	const void* data = nullptr;
	DWORD size = 0;
	HRESULT hr = GetShaderResource(id, &data, &size);
	if (SUCCEEDED(hr)) {
		hr = pDevice->CreateComputeShader(data, size, nullptr, ppShader);
	}
	DLogIf(FAILED(hr), L"CMotionField: shader {} failed: {}", id, dn::HR2Str(hr));
	return hr;
}

} // namespace

CMotionField::Settings CMotionField::Realtime()
{
	Settings s;
	s.flowFactor = 4;
	s.gridSize = 4;
	s.perfLevel = NV_OF_PERF_LEVEL_MEDIUM;
	s.flowBlur = 1;
	return s;
}

HRESULT CMotionField::CreateUavTex(ID3D11Device* pDevice, UINT width, UINT height, DXGI_FORMAT format, UavTex& tex)
{
	D3D11_TEXTURE2D_DESC desc = CreateTex2DDesc(format, width, height, Tex2D_DefaultShaderUAV);
	tex = UavTex{};
	HRESULT hr = pDevice->CreateTexture2D(&desc, nullptr, &tex.pTexture);
	if (SUCCEEDED(hr)) {
		hr = pDevice->CreateShaderResourceView(tex.pTexture, nullptr, &tex.pView);
	}
	if (SUCCEEDED(hr)) {
		hr = pDevice->CreateUnorderedAccessView(tex.pTexture, nullptr, &tex.pTarget);
	}
	return hr;
}

HRESULT CMotionField::Create(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, UINT width, UINT height, const Settings& settings)
{
	CheckPointer(pDevice, E_POINTER);
	CheckPointer(pContext, E_POINTER);
	if (Matches(width, height, settings)) {
		return S_OK;
	}
	Release();
	if (width < 64 || height < 64) {
		return E_INVALIDARG;
	}

	HRESULT hr = S_OK;
	const struct { int id; ID3D11ComputeShader** pp; } shaders[] = {
		{ IDS_CS_MOTION_FRAME, &m_pCSFrame }, { IDS_CS_MOTION_BLOCK, &m_pCSBlock }, { IDS_CS_MOTION_EXPAND, &m_pCSExpand },
		{ IDS_CS_GLOBAL_MOTION, &m_pCSGlobalMotion }, { IDS_CS_GLOBAL_STEP, &m_pCSRefineStep }, { IDS_CS_GLOBAL_SOLVE, &m_pCSRefineSolve },
		{ IDS_CS_SCENE_SUMS, &m_pCSSceneSums }, { IDS_CS_SCENE_CUT, &m_pCSSceneCut },
		{ IDS_CS_TILES, &m_pCSTiles }, { IDS_CS_TILE_MEDIAN, &m_pCSTileMedian },
		{ IDS_CS_BLUR_H, &m_pCSBlurH }, { IDS_CS_BLUR_V, &m_pCSBlurV }, { IDS_CS_BLUR_GRAD, &m_pCSBlurGrad },
	};
	for (const auto& s : shaders) {
		if (FAILED(hr = LoadShader(pDevice, s.id, s.pp))) {
			Release();
			return hr;
		}
	}
	D3D11_BUFFER_DESC bufferDesc = { 32 * sizeof(float), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
	hr = pDevice->CreateBuffer(&bufferDesc, nullptr, &m_pConstants);

	if (SUCCEEDED(hr)) {
		hr = CreateUavTex(pDevice, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, m_GlobalMotion);
	}
	if (SUCCEEDED(hr)) {
		hr = m_TexMotion.CheckCreate(pDevice, DXGI_FORMAT_R32G32_FLOAT, width, height, Tex2D_DefaultShaderUAV);
	}
	if (SUCCEEDED(hr)) {
		hr = pDevice->CreateUnorderedAccessView(m_TexMotion.pTexture, nullptr, &m_pMotionTarget);
	}
	if (SUCCEEDED(hr)) {
		D3D11_TEXTURE2D_DESC desc = CreateTex2DDesc(DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1, Tex2D_StagingRead);
		hr = pDevice->CreateTexture2D(&desc, nullptr, &m_pGlobalReadback);
	}

	// The cut, on frames of its own at about 540 lines.
	m_cutFactor = std::clamp((height + 270) / 540, 1u, 4u);
	const UINT cutW = width / m_cutFactor, cutH = height / m_cutFactor;
	for (int i = 0; i < 2 && SUCCEEDED(hr) && settings.cutDetect; i++) {
		hr = CreateUavTex(pDevice, cutW, cutH, DXGI_FORMAT_R8_UNORM, m_CutFrames[i]);
		if (SUCCEEDED(hr)) {
			hr = CreateUavTex(pDevice, (cutW + 15) / 16, (cutH + 15) / 16, DXGI_FORMAT_R32G32B32A32_FLOAT, m_CutPartial[i]);
		}
	}
	if (SUCCEEDED(hr) && settings.cutDetect) {
		D3D11_BUFFER_DESC desc = {};
		desc.ByteWidth = 16 * sizeof(float);
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		hr = pDevice->CreateBuffer(&desc, nullptr, &m_pCutState);
		if (SUCCEEDED(hr)) {
			D3D11_SHADER_RESOURCE_VIEW_DESC view = {};
			view.Format = DXGI_FORMAT_R32_FLOAT;
			view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			view.Buffer.NumElements = 16;
			hr = pDevice->CreateShaderResourceView(m_pCutState, &view, &m_pCutStateView);
		}
		if (SUCCEEDED(hr)) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC target = {};
			target.Format = DXGI_FORMAT_R32_FLOAT;
			target.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			target.Buffer.NumElements = 16;
			hr = pDevice->CreateUnorderedAccessView(m_pCutState, &target, &m_pCutStateTarget);
		}
		if (SUCCEEDED(hr)) {
			D3D11_BUFFER_DESC staging = {};
			staging.ByteWidth = 16 * sizeof(float);
			staging.Usage = D3D11_USAGE_STAGING;
			staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			hr = pDevice->CreateBuffer(&staging, nullptr, &m_pCutReadback);
		}
	}
	if (FAILED(hr)) {
		DLog(L"CMotionField: {}x{} failed: {}", width, height, dn::HR2Str(hr));
		Release();
		return hr;
	}

	m_pDevice = pDevice;
	m_width = width;
	m_height = height;
	m_Settings = settings;
	m_Settings.flowFactor = std::clamp(settings.flowFactor, 1u, 4u);

	CDlssOpticalFlow::Options options;
	options.gridSize = settings.gridSize;
	options.perfLevel = settings.perfLevel;
	options.bidirectional = true;
	options.cost = true;
	options.temporalHints = true;
	m_bFlow = m_Flow.Init(pDevice, pContext, width / m_Settings.flowFactor, height / m_Settings.flowFactor, options);
	if (m_bFlow) {
		const UINT tilesW = (m_Flow.Width() + settings.tileSize - 1) / settings.tileSize;
		const UINT tilesH = (m_Flow.Height() + settings.tileSize - 1) / settings.tileSize;
		for (int i = 0; i < 2 && SUCCEEDED(hr) && settings.tiles; i++) {
			hr = CreateUavTex(pDevice, tilesW, tilesH, DXGI_FORMAT_R32G32B32A32_FLOAT, m_Tiles[i]);
		}
		if (SUCCEEDED(hr) && settings.tiles) {
			hr = CreateUavTex(pDevice, tilesW, tilesH, DXGI_FORMAT_R32G32B32A32_FLOAT, m_TileField);
		}
		for (int i = 0; i < 4 && SUCCEEDED(hr) && settings.tiles; i++) {
			hr = CreateUavTex(pDevice, m_Flow.Width(), m_Flow.Height(), DXGI_FORMAT_R16G16B16A16_FLOAT, m_Smooth[i]);
		}
		if (SUCCEEDED(hr) && settings.tiles) {
			hr = CreateUavTex(pDevice, m_Flow.Width(), m_Flow.Height(), DXGI_FORMAT_R32_FLOAT, m_Blur);
		}
		if (SUCCEEDED(hr) && settings.tiles) {
			hr = CreateUavTex(pDevice, m_Flow.Width(), m_Flow.Height(), DXGI_FORMAT_R32_FLOAT, m_BlurTemp);
		}
		for (int i = 0; i < 2 && SUCCEEDED(hr); i++) {
			hr = CreateUavTex(pDevice, m_Flow.OutputWidth(), m_Flow.OutputHeight(), DXGI_FORMAT_R32G32B32A32_FLOAT, m_BlockField[i]);
			if (SUCCEEDED(hr)) {
				hr = CreateUavTex(pDevice, (m_Flow.Width() + 15) / 16, (m_Flow.Height() + 15) / 16, DXGI_FORMAT_R32G32B32A32_FLOAT, m_RefinePartial[i]);
			}
		}
		if (FAILED(hr)) {
			Release();
			return hr;
		}
		m_status = std::format(L"Optical Flow {}, cut on {}x{}", m_Flow.GetStatusLine(), cutW, cutH);
	} else {
		m_status = std::format(L"no motion vectors (Optical Flow: {})", m_Flow.GetStatusLine());
	}
	m_bCreated = true;

	const FLOAT zero[4] = {};
	pContext->ClearUnorderedAccessViewFloat(m_pMotionTarget, zero);
	Reset();
	return S_OK;
}

void CMotionField::Release()
{
	m_Flow.Release();
	m_bFlow = false;
	m_pCSFrame.Release(); m_pCSBlock.Release(); m_pCSExpand.Release();
	m_pCSGlobalMotion.Release(); m_pCSRefineStep.Release(); m_pCSRefineSolve.Release();
	m_pCSSceneSums.Release(); m_pCSSceneCut.Release();
	m_pCSTiles.Release(); m_pCSTileMedian.Release();
	m_pCSBlurH.Release(); m_pCSBlurV.Release(); m_pCSBlurGrad.Release();
	for (auto& s : m_Smooth) {
		s = UavTex{};
	}
	m_Blur = UavTex{};
	m_BlurTemp = UavTex{};
	m_Tiles[0] = UavTex{};
	m_Tiles[1] = UavTex{};
	m_TileField = UavTex{};
	m_pConstants.Release();
	m_GlobalMotion = UavTex{};
	for (int i = 0; i < 2; i++) {
		m_RefinePartial[i] = UavTex{};
		m_BlockField[i] = UavTex{};
		m_CutFrames[i] = UavTex{};
		m_CutPartial[i] = UavTex{};
	}
	m_pCutStateTarget.Release();
	m_pCutStateView.Release();
	m_pCutState.Release();
	m_pMotionTarget.Release();
	m_TexMotion.Release();
	m_pGlobalReadback.Release();
	m_pCutReadback.Release();
	m_pDevice.Release();
	m_width = m_height = 0;
	m_bCreated = false;
	m_Settings = Settings();
	m_status.clear();
}

void CMotionField::Reset()
{
	m_Flow.Reset();
	m_bBlockHistory = false;
	m_bCutStateStale = true;   // no cut across a seek: the residuals before it are not this shot's
	m_cutFrames = 0;
	m_bCut = false;
	m_global[0] = m_global[1] = m_global[2] = 0;
}

void CMotionField::Dispatch(ID3D11DeviceContext* pContext, ID3D11ComputeShader* pShader,
	std::initializer_list<ID3D11ShaderResourceView*> inputs, std::initializer_list<ID3D11UnorderedAccessView*> outputs,
	const float* constants, size_t count, UINT groupsX, UINT groupsY)
{
	if (constants && count) {
		D3D11_MAPPED_SUBRESOURCE mr = {};
		if (SUCCEEDED(pContext->Map(m_pConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr))) {
			float block[32] = {};
			std::copy_n(constants, std::min<size_t>(count, std::size(block)), block);
			memcpy(mr.pData, block, sizeof(block));
			pContext->Unmap(m_pConstants, 0);
		}
	}
	ID3D11ShaderResourceView* srvs[8] = {};
	std::copy_n(inputs.begin(), std::min<size_t>(inputs.size(), std::size(srvs)), srvs);
	ID3D11UnorderedAccessView* uavs[2] = {};
	std::copy_n(outputs.begin(), std::min<size_t>(outputs.size(), std::size(uavs)), uavs);
	ID3D11Buffer* pConstants = m_pConstants;

	pContext->CSSetShader(pShader, nullptr, 0);
	pContext->CSSetShaderResources(0, (UINT)std::size(srvs), srvs);
	pContext->CSSetUnorderedAccessViews(0, (UINT)std::size(uavs), uavs, nullptr);
	pContext->CSSetConstantBuffers(0, 1, &pConstants);
	pContext->Dispatch(groupsX, groupsY, 1);

	// Unbound, so the next pass can read what this one wrote.
	ID3D11ShaderResourceView* noViews[8] = {};
	ID3D11UnorderedAccessView* noTargets[2] = {};
	pContext->CSSetShaderResources(0, (UINT)std::size(noViews), noViews);
	pContext->CSSetUnorderedAccessViews(0, (UINT)std::size(noTargets), noTargets, nullptr);
}

void CMotionField::MeasureGlobalMotion(ID3D11DeviceContext* pContext)
{
	const float s = kRefFlowFactor / m_Settings.flowFactor;   // this flow's pixels per reference flow pixel
	const float globalConstants[] = {
		(float)m_Flow.GridSize(), kConsistency * s, kCostLow * m_Settings.costScale, kCostHigh * m_Settings.costScale,
		(float)m_Flow.Width(), (float)m_Flow.Height(), m_Flow.Bidirectional() ? 1.0f : 0.0f,
		(m_Flow.HasCost() && m_Settings.costScale > 0) ? 1.0f : 0.0f, kTrustedShare
	};
	Dispatch(pContext, m_pCSGlobalMotion, { m_Flow.ForwardFlow(), m_Flow.BackwardFlow(), m_Flow.ForwardCost() },
		{ m_GlobalMotion.pTarget }, globalConstants, std::size(globalConstants), 1, 1);

	// Then refined on the flow frames themselves (cs_dlss_global_refine.hlsl).
	const float refineConstants[] = { (float)m_Flow.GridSize(), 1.0f * s, 2000.0f, 0.5f * s, m_Settings.globalRobust };
	const UINT groupsX = (m_Flow.Width() + 15) / 16, groupsY = (m_Flow.Height() + 15) / 16;
	for (int step = 0; step < m_Settings.globalRefine; step++) {
		Dispatch(pContext, m_pCSRefineStep, { m_Flow.CurrentFrame(), m_Flow.PreviousFrame(), m_GlobalMotion.pView, m_Flow.ForwardFlow() },
			{ m_RefinePartial[0].pTarget, m_RefinePartial[1].pTarget }, refineConstants, std::size(refineConstants), groupsX, groupsY);
		Dispatch(pContext, m_pCSRefineSolve, { m_RefinePartial[0].pView, m_RefinePartial[1].pView },
			{ m_GlobalMotion.pTarget }, refineConstants, std::size(refineConstants), 1, 1);
	}
}

void CMotionField::DetectCut(ID3D11DeviceContext* pContext)
{
	if (m_bCutStateStale) {
		const FLOAT zero[4] = {};
		pContext->ClearUnorderedAccessViewFloat(m_pCutStateTarget, zero);
		m_bCutStateStale = false;
	}
	const UavTex& now = m_CutFrames[m_iCutFrame];
	const UavTex& last = m_CutFrames[1 - m_iCutFrame];
	D3D11_TEXTURE2D_DESC desc = {};
	now.pTexture->GetDesc(&desc);
	const float sumConstants[] = { (float)m_Settings.flowFactor / m_cutFactor };
	Dispatch(pContext, m_pCSSceneSums, { now.pView, last.pView, m_GlobalMotion.pView },
		{ m_CutPartial[0].pTarget, m_CutPartial[1].pTarget }, sumConstants, std::size(sumConstants),
		(desc.Width + 15) / 16, (desc.Height + 15) / 16);
	const float cutConstants[] = { kCutHistogram, kCutRatio, kCutResidual, kCutCorrelation, kCutLightHist, kCutLightRatio };
	Dispatch(pContext, m_pCSSceneCut, { now.pView, last.pView, m_CutPartial[0].pView, m_CutPartial[1].pView },
		{ m_pCutStateTarget }, cutConstants, std::size(cutConstants), 1, 1);
}

bool CMotionField::Process(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pPicture)
{
	m_bCut = false;
	if (!m_bCreated || !pContext || !pPicture) {
		return false;
	}

	// The flow frame, and the cut's.
	bool bHaveMotion = false;
	if (m_bFlow) {
		CComPtr<ID3D11UnorderedAccessView> pFrameTarget;
		if (FAILED(m_pDevice->CreateUnorderedAccessView(m_Flow.FrameTexture(), nullptr, &pFrameTarget))) {
			return false;
		}
		const float frameConstants[] = { (float)m_Settings.flowFactor, (float)m_Settings.flowBlur };
		Dispatch(pContext, m_pCSFrame, { pPicture }, { pFrameTarget }, frameConstants, std::size(frameConstants),
			(m_Flow.Width() + 7) / 8, (m_Flow.Height() + 7) / 8);
		bHaveMotion = m_Flow.Execute();
	}
	const bool bCut = m_Settings.cutDetect && m_CutFrames[0].pTexture;
	if (bCut) {
		D3D11_TEXTURE2D_DESC desc = {};
		m_CutFrames[m_iCutFrame].pTexture->GetDesc(&desc);
		const float frameConstants[] = { (float)m_cutFactor, 1.0f };
		Dispatch(pContext, m_pCSFrame, { pPicture }, { m_CutFrames[m_iCutFrame].pTarget }, frameConstants, std::size(frameConstants),
			(desc.Width + 7) / 8, (desc.Height + 7) / 8);
		m_cutFrames = std::min(m_cutFrames + 1, 2);
	}

	if (bHaveMotion) {
		MeasureGlobalMotion(pContext);
		if (bCut && m_cutFrames >= 2) {
			DetectCut(pContext);
		}

		const float s = kRefFlowFactor / m_Settings.flowFactor;
		const bool bTiles = m_TileField.pTexture != nullptr;
		if (bTiles) {
			// The global motion refined tile by tile from it, then a 3x3 median.
			D3D11_TEXTURE2D_DESC td = {};
			m_TileField.pTexture->GetDesc(&td);
			// The two flow frames blurred, heavily (radius 4) and lightly (radius 1), with their
			// gradients.
			const UINT fw = m_Flow.Width(), fh = m_Flow.Height();
			ID3D11ShaderResourceView* frames[2] = { m_Flow.CurrentFrame(), m_Flow.PreviousFrame() };
			const float radii[2] = { 4.0f, 1.0f };
			for (int r = 0; r < 2; r++) {
				for (int f = 1; f >= 0; f--) {   // the current frame last: m_Blur keeps its light blur
					const float blur[] = { radii[r] };
					Dispatch(pContext, m_pCSBlurH, { frames[f] }, { m_BlurTemp.pTarget }, blur, 1, (fw + 7) / 8, (fh + 7) / 8);
					Dispatch(pContext, m_pCSBlurV, { m_BlurTemp.pView }, { m_Blur.pTarget }, blur, 1, (fw + 7) / 8, (fh + 7) / 8);
					Dispatch(pContext, m_pCSBlurGrad, { m_Blur.pView }, { m_Smooth[r * 2 + f].pTarget }, nullptr, 0, (fw + 7) / 8, (fh + 7) / 8);
				}
			}
			// Under the grain each step moves only part of the way (the gradients are noisy):
			// eight steps left the trees behind the face of The Uprising 1.6 pixels off in
			// DLSS's picture, 64 left them 0.3 pixel off (vs\shiftregions.py).
			int read = 0;
			for (int step = 0; step < 2 * kTileSteps; step++) {
				const UavTex* pair = &m_Smooth[step < kTileSteps ? 0 : 2];
				const float tileConstants[] = { (float)m_Settings.tileSize, step == 0 ? 1.0f : 0.0f, 2.0f * s, 0.0f, 4.0f * s };
				Dispatch(pContext, m_pCSTiles, { pair[0].pView, pair[1].pView, m_Tiles[read].pView, m_GlobalMotion.pView },
					{ m_Tiles[1 - read].pTarget }, tileConstants, std::size(tileConstants), td.Width, td.Height);
				read = 1 - read;
			}
			// Drawn back to the global motion where the picture is flat against the grain, on
			// the heavily blurred frames.
			const float verifyConstants[] = { (float)m_Settings.tileSize, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, kTileTextureLow, kTileTextureHigh };
			Dispatch(pContext, m_pCSTiles, { m_Smooth[0].pView, m_Smooth[1].pView, m_Tiles[read].pView, m_GlobalMotion.pView,
				m_Flow.CurrentFrame(), m_Blur.pView },
				{ m_Tiles[1 - read].pTarget }, verifyConstants, std::size(verifyConstants), td.Width, td.Height);
			read = 1 - read;
			// Grain moves each tile its own way, a parallax moves the tiles around alike: the
			// median of 5 x 5 tiles took the tiles' error on the bench's grained still and pans
			// from 1 pixel to 0.5 (3x3: 1), and kept the trees within 0.7 pixel.
			const float medianConstants[] = { 2.0f };
			Dispatch(pContext, m_pCSTileMedian, { m_Tiles[read].pView }, { m_TileField.pTarget }, medianConstants, std::size(medianConstants),
				(td.Width + 7) / 8, (td.Height + 7) / 8);
		}
		const float tileSize = bTiles ? (float)m_Settings.tileSize : 0.0f;
		const float scaleX = (float)m_width / m_Flow.Width(), scaleY = (float)m_height / m_Flow.Height();
		const float stride = std::max(1.0f, std::round(kRefBlockPixels / (m_Settings.flowFactor * m_Flow.GridSize())));
		const int levels = m_Settings.vectorRefine + (int)std::lround(std::log2(s));
		const int write = 1 - m_iBlockField;
		if (!m_Settings.globalOnly) {
			const float blockConstants[] = {
				scaleX, scaleY, (float)m_Flow.GridSize(), kConsistency * s,
				(float)m_Flow.Width(), (float)m_Flow.Height(), kCostLow * m_Settings.costScale, kCostHigh * m_Settings.costScale,
				m_Flow.Bidirectional() ? 1.0f : 0.0f, (m_Flow.HasCost() && m_Settings.costScale > 0) ? 1.0f : 0.0f,
				m_Settings.snapLow, m_Settings.snapHigh,
				2.0f, 0.0f, (float)m_Settings.vectorRadius, m_Settings.vectorSigma,
				m_Settings.vectorTemporal, m_bBlockHistory ? 1.0f : 0.0f, (float)m_Settings.vectorShift, m_Settings.vectorSupportLow,
				m_Settings.vectorSupportHigh, (float)levels, 4.0f * s, m_Settings.vectorTexture,
				m_Settings.vectorEvidence, stride, s, 0.5f * s, tileSize
			};
			Dispatch(pContext, m_pCSBlock,
				{ m_Flow.ForwardFlow(), m_Flow.BackwardFlow(), m_Flow.ForwardCost(), m_GlobalMotion.pView,
				  m_BlockField[m_iBlockField].pView, m_Flow.CurrentFrame(), m_Flow.PreviousFrame(), m_TileField.pView },
				{ m_BlockField[write].pTarget }, blockConstants, std::size(blockConstants),
				(m_Flow.OutputWidth() + 7) / 8, (m_Flow.OutputHeight() + 7) / 8);
		}
		const float expandConstants[] = { scaleX, scaleY, (float)m_Flow.GridSize(), 0.0f, m_Settings.globalOnly ? 0.0f : 1.0f, tileSize };
		Dispatch(pContext, m_pCSExpand, { m_BlockField[write].pView, m_GlobalMotion.pView, m_TileField.pView }, { m_pMotionTarget },
			expandConstants, std::size(expandConstants), (m_width + 7) / 8, (m_height + 7) / 8);
		m_iBlockField = write;
		m_bBlockHistory = true;

		// Offline nothing waits for a frame ahead: the global motion and the cut are read
		// back now, and the cut becomes DLSS's Reset on this very picture.
		pContext->CopyResource(m_pGlobalReadback, m_GlobalMotion.pTexture);
		D3D11_MAPPED_SUBRESOURCE mr = {};
		if (SUCCEEDED(pContext->Map(m_pGlobalReadback, 0, D3D11_MAP_READ, 0, &mr))) {
			const float* g = (const float*)mr.pData;
			m_global[0] = g[0] * scaleX;
			m_global[1] = g[1] * scaleY;
			m_global[2] = g[2] * scaleX;
			pContext->Unmap(m_pGlobalReadback, 0);
		}
		if (bCut && m_cutFrames >= 2) {
			pContext->CopyResource(m_pCutReadback, m_pCutState);
			if (SUCCEEDED(pContext->Map(m_pCutReadback, 0, D3D11_MAP_READ, 0, &mr))) {
				m_bCut = ((const float*)mr.pData)[0] > 0.5f;
				pContext->Unmap(m_pCutReadback, 0);
			}
		}
	} else {
		// No flow for this picture: no motion, no history for the blocks.
		m_bBlockHistory = false;
		const FLOAT zero[4] = {};
		pContext->ClearUnorderedAccessViewFloat(m_pMotionTarget, zero);
	}
	if (bCut) {
		m_iCutFrame = 1 - m_iCutFrame;
	}
	return bHaveMotion;
}
