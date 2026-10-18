/*
 * This file is part of DLSS Denoiser.
 *
 * DLSS Denoiser is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version. See LICENSE.txt.
 */

#pragma once

#include "DlssSR.h"
#include "MotionField.h"

// CEngine
//
// One picture after another through DLSS in DLAA mode, on a Direct3D 11 device of its
// own: three planes of PQ code values in, three out, the same size.
//
//   upload -> the guard band mirrored and the picture taken to linear light (cs_convert)
//          -> the motion measured on it, or on a clip given for the motion alone
//          -> DLSS, reset on a cut -> back to PQ code values -> readback
//
// Nothing here is thread-safe: NGX is not (programming guide, 5.2.4). The owner calls it
// from one thread.

class CEngine
{
public:
	struct Config {
		UINT width = 0, height = 0;
		UINT pad = 32;                          // guard band, pixels on each side
		unsigned preset = NGX_DLSS_PRESET_K;
		bool linear = true;                     // linear light with IsHDR; false: PQ code values, LDR
		bool autoExposure = false;
		float exposure = 1.0f;                  // with linear light, 1.0 = 100 nits
		bool half = false;                      // planes as 16-bit floats instead of 32-bit
		bool motion = true;                     // Optical Flow; false: zero motion unless given
		CMotionField::Settings motionSettings;
		int passes = 1;                         // 2: a second lane, fed the film backward
		bool operator==(const Config&) const = default;
	};

	struct Planes {
		const void* data[3] = {};
		ptrdiff_t stride[3] = {};               // bytes between rows
	};
	struct OutPlanes {
		void* data[3] = {};
		ptrdiff_t stride[3] = {};
	};

	CEngine() = default;
	~CEngine() { Shutdown(); }
	CEngine(const CEngine&) = delete;
	CEngine& operator=(const CEngine&) = delete;

	// The NVIDIA adapter of that index among NVIDIA's, and DLSS on it.
	bool Init(int deviceIndex, const wchar_t* dllPath);
	void Shutdown();
	bool Configure(const Config& config);
	const Config& GetConfig() const { return m_Config; }

	// One picture through one lane. Lane 0 takes the film forward; lane 1, with passes 2,
	// takes it backward -- its "previous" picture is the next one in the film -- with a
	// history and a motion field of its own. ref: a clip the motion is measured on
	// instead, the same size (a copy with the grain taken out). exactMotion: RG32F, W x H,
	// picture pixels, current to the lane's previous, instead of any measured (the
	// bench's). reset: the history starts over on this picture, a cut known beforehand,
	// a seek, the start of a backward run.
	bool Process(const Planes& in, const Planes* ref, const float* exactMotion, bool reset, const OutPlanes& out, int lane = 0);

	bool WasCut() const { return m_bCut; }          // the motion field saw a cut on the last picture
	// Pictures the lane's history held before its last picture: 0 when it started over on it.
	int GetHistory(int lane) const { return m_history[lane]; }
	void GetGlobalMotion(float& x, float& y, float& spread) const { m_Motion[0].GetGlobalMotion(x, y, spread); }

	// How much a lane's picture is worth in the mean of the two: as the inverse of the
	// variance of the grain it leaves, which falls as one over the pictures in its history
	// and stops falling after about thirty (bench, --converge: 9.1, 6.45, 4.57, 3.09, 2.30,
	// 1.95, 1.67 after 0, 1, 3, 8, 16, 24 and 36 pictures, heavy grain).
	static float Weight(int history) { return (float)std::min(history + 1, kHistoryCap); }
	static constexpr int kHistoryCap = 30;
	// The weighted mean of the two lanes' pictures, into a.
	static void Blend(const OutPlanes& a, float wa, const OutPlanes& b, float wb, UINT width, UINT height, bool half);
	// The motion DLSS was given for the last picture, W x H, RG pixels; for the bench.
	bool ReadMotion(std::vector<float>& rg);

	const std::wstring& GetError() const { return m_error; }
	std::wstring GetStatus() const;
	ID3D11Device* GetDevice() const { return m_pDevice; }
	ID3D11DeviceContext* GetContext() const { return m_pContext; }

private:
	bool Fail(const std::wstring& why) { m_error = why; DLog(L"CEngine: {}", why); return false; }
	void Upload(ID3D11Texture2D* pTexture, const void* data, ptrdiff_t stride);
	void Convert(ID3D11ComputeShader* pShader, std::initializer_list<ID3D11ShaderResourceView*> inputs,
		std::initializer_list<ID3D11UnorderedAccessView*> outputs, bool writeDlss, UINT groupsX, UINT groupsY);

	CComPtr<ID3D11Device>        m_pDevice;
	CComPtr<ID3D11DeviceContext> m_pContext;
	std::wstring m_adapter;
	CDlssSR      m_Dlss;
	CMotionField m_Motion[2];
	Config       m_Config;
	bool         m_bConfigured = false;

	CComPtr<ID3D11ComputeShader> m_pCSIn, m_pCSOut;
	CComPtr<ID3D11Buffer> m_pConstants;
	Tex2D_t m_InPlanes[3], m_RefPlanes[3];    // W x H, R32F or R16F
	Tex2D_t m_OutPlanes[3];                   // W x H, R32F with UAVs
	CComPtr<ID3D11UnorderedAccessView> m_pOutTargets[3];
	CComPtr<ID3D11Texture2D> m_pOutStaging[3];
	Tex2D_t m_MotionPicture, m_RefPicture;    // padded, RGBA16F with UAVs: what the motion is measured on
	CComPtr<ID3D11UnorderedAccessView> m_pMotionPictureTarget, m_pRefPictureTarget;
	CComPtr<ID3D11UnorderedAccessView> m_pDlssInTarget[2];
	Tex2D_t m_ExactMotion;                    // padded, RG32F
	Tex2D_t m_ZeroMotion;

	bool m_bCut = false;
	bool m_bFirst[2] = { true, true };
	int  m_history[2] = {};
	std::wstring m_error;
};
