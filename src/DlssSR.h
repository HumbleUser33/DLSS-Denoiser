/*
 * This file is part of DLSS Denoiser.
 * Derived from MPCVR-Plus (https://github.com/HumbleUser33/MPCVR-Plus), Source/DLSS/DlssSR.h,
 * (C) 2026 its authors.
 *
 * DLSS Denoiser is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version. See LICENSE.txt.
 */

#pragma once

#include "D3D11Util.h"
#include "NGXTypes.h"

// CDlssSR
//
// DLSS Super Resolution (NGX feature 1) in DLAA mode -- the output the size of the
// input -- through the display driver's NGX runtime (_nvngx.dll), which runs it on
// Direct3D 11 and loads nvngx_dlss.dll from the folder it is given.
//
// A video has no jitter and no depth. The input is the picture at its own size, the
// depth a constant, and the motion vectors whatever the caller measured. Without
// jitter a still picture brings DLSS nothing new: what is left of it is a temporal
// filter along the vectors, which is what takes the grain out.
//
// HDR goes in as games give it, before any tone mapping: linear light with IsHDR,
// which DLSS processes "with high range, high precision colors". Without IsHDR, DLSS
// "quantizes the data to 8 bits" (programming guide 310.6.0, 3.1.2), which is what
// MPCVR-Plus gave it with PQ code values.
//
// NGX binds its own shaders and views on the immediate context. Every call into it
// runs in a separate device context state, so none of that leaks out.

class CDlssSR
{
public:
	enum class State {
		Off, CoreNotFound, DllNotFound, NoContextState, InitFailed, NotAvailable,
		TexturesFailed, FeatureCreateFailed, EvaluateFailed, Ready
	};

	struct Config {
		UINT width = 0, height = 0;
		UINT outWidth = 0, outHeight = 0;    // 0: the input's, DLAA
		unsigned preset = NGX_DLSS_PRESET_K;
		bool hdr = true;                     // IsHDR: linear light; false, LDR: perceptual 0..1, 8-bit inside
		bool autoExposure = false;           // DLSS measures the exposure itself (preset L always does)
		DXGI_FORMAT inputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
		bool operator==(const Config&) const = default;
	};

	CDlssSR() = default;
	~CDlssSR();
	CDlssSR(const CDlssSR&) = delete;
	CDlssSR& operator=(const CDlssSR&) = delete;

	// The NGX session on the device. pConfiguredDllPath is nvngx_dlss.dll or its
	// folder; empty looks next to this module. False on any failure, with the reason
	// in GetStatusLine().
	bool Init(ID3D11Device* pDevice, const wchar_t* pConfiguredDllPath);
	void Shutdown();
	bool IsInitialised() const { return m_bInitialised; }

	// Features of one session, each with its own history: the second runs the film backward.
	static constexpr int kSlots = 2;

	// The feature and its textures; nothing is rebuilt when the config already matches.
	bool CreateFeature(const Config& config, int slot = 0);
	void ReleaseFeature(int slot);
	void ReleaseFeatures();
	bool IsFeatureReady(int slot = 0) const { return m_Slots[slot].hFeature != nullptr; }
	const Config& GetConfig(int slot = 0) const { return m_Slots[slot].config; }

	// Where the picture goes before Evaluate: input size, Config::inputFormat.
	Tex2D_t* GetInput(int slot = 0) { return m_Slots[slot].hFeature ? &m_Slots[slot].texIn : nullptr; }
	// What Evaluate wrote: RGBA16F, output size.
	Tex2D_t* GetOutput(int slot = 0) { return m_Slots[slot].hFeature ? &m_Slots[slot].texOut : nullptr; }

	// One frame. pMotion: RG16F or RG32F at the input size, input pixels from the
	// current picture to the previous one, or null for none. exposure: the value that
	// brings middle grey where DLSS expects it, read by presets J and K in HDR mode.
	// reset: the history starts over on this frame (a cut, a seek).
	bool Evaluate(ID3D11Texture2D* pMotion, float exposure, bool reset, int slot = 0);

	State GetState() const { return m_State; }
	std::wstring GetStatusLine(int slot = 0) const;
	std::wstring GetInfoBlock() const;
	const std::wstring& GetDllPath() const { return m_DllPath; }
	wchar_t GetPresetUsed(int slot = 0) const { return m_Slots[slot].presetUsed; }   // the model DLSS says it runs, 0 unknown

	static std::vector<std::wstring> CandidateDllPaths(const wchar_t* pConfigured);
	static int QualityModeFor(UINT inWidth, UINT inHeight, UINT outWidth, UINT outHeight);
	static const wchar_t* QualityModeName(int mode);
	static wchar_t PresetLetter(unsigned preset);

private:
	bool LoadCore();
	bool FindSnippet(const wchar_t* pConfiguredDllPath);
	bool Fail(State state, const std::wstring& why);
	void Log(const std::wstring& line);

	HMODULE m_hCore = nullptr;   // _nvngx.dll, the driver's NGX runtime

	PFN_NGX_D3D11_Init_Ext                m_pfnInitExt  = nullptr;
	PFN_NGX_D3D11_GetCapabilityParameters m_pfnGetCaps  = nullptr;
	PFN_NGX_D3D11_DestroyParameters       m_pfnDestroy  = nullptr;
	PFN_NGX_D3D11_CreateFeature           m_pfnCreate   = nullptr;
	PFN_NGX_D3D11_EvaluateFeature         m_pfnEvaluate = nullptr;
	PFN_NGX_D3D11_ReleaseFeature          m_pfnRelease  = nullptr;
	PFN_NGX_D3D11_Shutdown                m_pfnShutdown = nullptr;

	CComPtr<ID3D11Device>           m_pDevice;
	CComPtr<ID3D11DeviceContext1>   m_pContext;
	CComPtr<ID3DDeviceContextState> m_pNgxState;   // the pipeline state NGX works in

	NVSDK_NGX_Parameter* m_pParams = nullptr;      // owned by the runtime

	struct Slot {
		NVSDK_NGX_Handle* hFeature = nullptr;
		Tex2D_t texIn;             // input size, Config::inputFormat
		Tex2D_t texOut;            // RGBA16F with a UAV, output size
		Tex2D_t texDepth;          // R32F, input size, constant
		Tex2D_t texNoMotion;       // RG32F, input size, zero
		Tex2D_t texExposure;       // R32F 1x1
		float   exposureSet = -1.0f;
		Config  config;
		int     mode = NGX_PERFQUALITY_DLAA;
		wchar_t presetUsed = 0;
	};
	Slot m_Slots[kSlots];

	std::wstring m_DllPath;       // the nvngx_dlss.dll that was found
	std::wstring m_SnippetDir;    // its folder, given to the runtime
	std::wstring m_DataPath;
	uint32_t     m_SdkVersion = 0;

	bool m_bInitialised = false;

	State            m_State = State::Off;
	NVSDK_NGX_Result m_LastResult = 0;
	std::wstring     m_Detail;
	std::deque<std::wstring> m_Log;
};
