/*
 * This file is part of DLSS Denoiser.
 * Derived from MPCVR-Plus (https://github.com/HumbleUser33/MPCVR-Plus), Source/DLSS/DlssStabilizer.h,
 * (C) 2026 its authors.
 *
 * DLSS Denoiser is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version. See LICENSE.txt.
 */

#pragma once

#include "D3D11Util.h"
#include "DlssOpticalFlow.h"

// CMotionField
//
// The motion vectors DLSS is given, from NVIDIA Optical Flow, cleaned the way MPCVR-Plus
// cleans them for DLSS Super Resolution (CDlssStabilizer::ForDlssSR, measured there in
// --tsrstill, --tsrclip and --tsrcut): the picture's global motion, the median of the
// trusted blocks refined on the flow frames; each block drawn to it unless it moves on
// its own and the trusted blocks around it agree; what keeps its own vector searched
// again on the frames and blended with the last picture's.
//
// MPCVR-Plus runs the flow at about 540 lines with a vector per 4x4 flow pixels, for
// time. Here the flow may run at the picture's own size with a vector per pixel, and
// every window keeps the reach in source pixels it was measured with at 4K (a vector
// per 16 source pixels): see cs_motion.hlsl. The cut is told on frames of its own at
// about 540 lines, where its thresholds were chosen.

class CMotionField
{
public:
	struct Settings {
		UINT flowFactor = 1;                                  // picture pixels per flow pixel, 1 to 4
		UINT gridSize = 1;                                    // flow pixels per vector: 1, 2 or 4
		NV_OF_PERF_LEVEL perfLevel = NV_OF_PERF_LEVEL_SLOW;
		UINT flowBlur = 1;                                    // picture pixels the flow frame's box reaches beyond its own
		bool cutDetect = true;
		bool globalOnly = false;                              // every pixel the global motion: a test of the blocks' worth
		// The cleaning, as ForDlssSR set it; distances in picture pixels.
		float snapLow = 0.75f, snapHigh = 1.5f;
		int   globalRefine = 2;
		float globalRobust = 2.5f;
		int   vectorRadius = 3, vectorShift = 3, vectorRefine = 3;
		float vectorSigma = 1.0f, vectorTemporal = 0.3f;
		float vectorSupportLow = 0.1f, vectorSupportHigh = 0.3f, vectorEvidence = 4.0f;
		float vectorTexture = 0.02f;
		float costScale = 1.0f;                               // the matching cost thresholds, times this; 0: cost not used
		// An untrusted block is drawn to the motion of its tile of tileSize flow pixels, the
		// global motion refined there (cs_tiles.hlsl), instead of the global motion itself.
		bool tiles = false;
		UINT tileSize = 64;
		bool operator==(const Settings&) const = default;
	};

	// What MPCVR-Plus runs in real time for a 4K film: flow at a quarter of the size,
	// a vector per 4x4 flow pixels, medium effort.
	static Settings Realtime();

	CMotionField() = default;
	~CMotionField() { Release(); }
	CMotionField(const CMotionField&) = delete;
	CMotionField& operator=(const CMotionField&) = delete;

	HRESULT Create(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, UINT width, UINT height, const Settings& settings);
	void Release();
	bool Matches(UINT width, UINT height, const Settings& settings) const {
		return m_width == width && m_height == height && m_Settings == settings && m_bCreated;
	}

	// The next picture starts over: a seek, a cut known beforehand.
	void Reset();

	// A new picture, PQ code values at the picture's size: its motion against the
	// previous one into GetMotion(). False when there is none (the first picture after
	// a reset, or Optical Flow down); the motion is then zero.
	bool Process(ID3D11DeviceContext* pContext, ID3D11ShaderResourceView* pPicture);

	// RG32F at the picture's size: picture pixels, current -> previous.
	ID3D11Texture2D* GetMotion() const { return m_TexMotion.pTexture; }
	// The last Process saw a cut (cs_dlss_scene_cut.hlsl). Read back from the GPU.
	bool WasCut() const { return m_bCut; }
	// The global motion of the last picture, picture pixels, and the median distance of the
	// trusted blocks to it; read back from the GPU.
	void GetGlobalMotion(float& x, float& y, float& spread) const { x = m_global[0]; y = m_global[1]; spread = m_global[2]; }

	const std::wstring& GetStatusLine() const { return m_status; }
	const CDlssOpticalFlow& GetFlow() const { return m_Flow; }

private:
	struct UavTex {
		CComPtr<ID3D11Texture2D>           pTexture;
		CComPtr<ID3D11ShaderResourceView>  pView;
		CComPtr<ID3D11UnorderedAccessView> pTarget;
	};
	static HRESULT CreateUavTex(ID3D11Device* pDevice, UINT width, UINT height, DXGI_FORMAT format, UavTex& tex);

	void Dispatch(ID3D11DeviceContext* pContext, ID3D11ComputeShader* pShader,
		std::initializer_list<ID3D11ShaderResourceView*> inputs, std::initializer_list<ID3D11UnorderedAccessView*> outputs,
		const float* constants, size_t count, UINT groupsX, UINT groupsY);
	void MeasureGlobalMotion(ID3D11DeviceContext* pContext);
	void DetectCut(ID3D11DeviceContext* pContext);

	CComPtr<ID3D11Device> m_pDevice;
	CComPtr<ID3D11ComputeShader> m_pCSFrame;
	CComPtr<ID3D11ComputeShader> m_pCSBlock;
	CComPtr<ID3D11ComputeShader> m_pCSExpand;
	CComPtr<ID3D11ComputeShader> m_pCSGlobalMotion;
	CComPtr<ID3D11ComputeShader> m_pCSRefineStep;
	CComPtr<ID3D11ComputeShader> m_pCSRefineSolve;
	CComPtr<ID3D11ComputeShader> m_pCSSceneSums;
	CComPtr<ID3D11ComputeShader> m_pCSSceneCut;
	CComPtr<ID3D11ComputeShader> m_pCSTiles;
	CComPtr<ID3D11ComputeShader> m_pCSTileMedian;
	CComPtr<ID3D11ComputeShader> m_pCSBlurH, m_pCSBlurV, m_pCSBlurGrad;
	CComPtr<ID3D11Buffer>        m_pConstants;

	UavTex m_GlobalMotion;          // RGBA32F 1x1
	UavTex m_Tiles[2];              // RGBA32F, a texel per tile: the refinement's steps
	UavTex m_TileField;             // and its 3x3 median, what the blocks are drawn to
	UavTex m_Smooth[4];             // RGBA16F at the flow's size: this frame and the last, blurred heavily, then
	                                // lightly, value and gradient (cs_blur.hlsl)
	UavTex m_Blur, m_BlurTemp;      // R32F at the flow's size: the box blur's two passes
	UavTex m_RefinePartial[2];      // RGBA32F, a texel per 16 x 16 group of the flow frame
	UavTex m_BlockField[2];         // RGBA32F, a texel per vector block: this picture and the last
	UavTex m_CutFrames[2];          // R8, about 540 lines: this picture and the last
	UavTex m_CutPartial[2];         // RGBA32F, a texel per 16 x 16 group of the cut frame
	CComPtr<ID3D11Buffer>              m_pCutState;   // R32F x 16
	CComPtr<ID3D11ShaderResourceView>  m_pCutStateView;
	CComPtr<ID3D11UnorderedAccessView> m_pCutStateTarget;
	Tex2D_t m_TexMotion;            // RG32F with a UAV
	CComPtr<ID3D11UnorderedAccessView> m_pMotionTarget;
	CComPtr<ID3D11Texture2D> m_pGlobalReadback;   // staging, 1x1
	CComPtr<ID3D11Buffer>    m_pCutReadback;      // staging, 16 floats

	CDlssOpticalFlow m_Flow;
	Settings m_Settings;
	UINT m_width = 0, m_height = 0;
	UINT m_cutFactor = 1;
	bool m_bCreated = false;
	bool m_bFlow = false;
	int  m_iBlockField = 0;
	bool m_bBlockHistory = false;
	int  m_iCutFrame = 0;
	int  m_cutFrames = 0;           // cut frames written since the reset
	bool m_bCutStateStale = true;
	bool m_bCut = false;
	float m_global[3] = {};
	std::wstring m_status;
};
