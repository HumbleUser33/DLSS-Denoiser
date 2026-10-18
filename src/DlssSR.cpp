/*
 * This file is part of DLSS Denoiser.
 * Derived from MPCVR-Plus (https://github.com/HumbleUser33/MPCVR-Plus), Source/DLSS/DlssSR.cpp,
 * (C) 2026 its authors.
 *
 * DLSS Denoiser is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version. See LICENSE.txt.
 */

#include "DlssSR.h"
#include "NgxRuntime.h"
#include <shlwapi.h>
#include <shlobj.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

namespace {

const wchar_t kSnippetName[] = L"nvngx_dlss.dll";

// Newest first: a runtime older than the SDK answers OutOfDate. 310.9.16 is 0x15.
constexpr uint32_t kSdkVersions[] = { 0x15, 0x14, 0x13 };

// Our own application id. NGX only logs that a valid one is required in production
// builds and goes on; MPCVR-Plus's is its own business.
constexpr unsigned long long kAppId = 0x444C53534E31ull;   // "DLSSN1"

// The parameter keys of DLSS Super Resolution (310.9.16 nvsdk_ngx_defs.h).
#define P_WIDTH          "Width"
#define P_HEIGHT         "Height"
#define P_OUTWIDTH       "OutWidth"
#define P_OUTHEIGHT      "OutHeight"
#define P_PERFQUALITY    "PerfQualityValue"
#define P_CREATEFLAGS    "DLSS.Feature.Create.Flags"
#define P_OUTSUBRECTS    "DLSS.Enable.Output.Subrects"
#define P_COLOR          "Color"
#define P_OUTPUT         "Output"
#define P_DEPTH          "Depth"
#define P_MOTIONVECTORS  "MotionVectors"
#define P_EXPOSURE       "ExposureTexture"
#define P_JITTERX        "Jitter.Offset.X"
#define P_JITTERY        "Jitter.Offset.Y"
#define P_SHARPNESS      "Sharpness"
#define P_RESET          "Reset"
#define P_MVSCALEX       "MV.Scale.X"
#define P_MVSCALEY       "MV.Scale.Y"
#define P_RENDERWIDTH    "DLSS.Render.Subrect.Dimensions.Width"
#define P_RENDERHEIGHT   "DLSS.Render.Subrect.Dimensions.Height"
#define P_PREEXPOSURE    "DLSS.Pre.Exposure"
#define P_EXPOSURESCALE  "DLSS.Exposure.Scale"
#define P_FRAMETIME      "FrameTimeDeltaInMsec"

const char* const kPresetKeys[] = {
	"DLSS.Hint.Render.Preset.DLAA",
	"DLSS.Hint.Render.Preset.Quality",
	"DLSS.Hint.Render.Preset.Balanced",
	"DLSS.Hint.Render.Preset.Performance",
	"DLSS.Hint.Render.Preset.UltraPerformance",
	"DLSS.Hint.Render.Preset.UltraQuality",
};

// What NGX logs, from whichever thread it logs on. The callback carries no context,
// so the lines gather here for every instance.
std::mutex g_NgxLogLock;
std::deque<std::wstring> g_NgxLog;

// NGX sessions open in the process. The runtime's device-less Shutdown, the only one that
// does not crash after Init_Ext, ends every session of the process at once: a second
// instance of the filter (a preview beside an encode, two calls in one script) failed its
// next evaluation with InvalidParameter once the first had shut down. So only the last
// session to close calls it.
std::mutex g_SessionLock;
int g_Sessions = 0;

// The model DLSS picked for each mode, as its creation log says.
wchar_t g_PresetByMode[6] = {};

// "... NgxDltss::FillCreationParams ... Info: (DLAA) ... Preset K"
void ReadPresetLine(const std::wstring& line)
{
	if (line.find(L"NgxDltss::FillCreationParams") == std::wstring::npos) {
		return;
	}
	static const struct { const wchar_t* tag; int mode; } tags[] = {
		{ L"(Perf)", NGX_PERFQUALITY_MaxPerf }, { L"(Balanced)", NGX_PERFQUALITY_Balanced },
		{ L"(Quality)", NGX_PERFQUALITY_MaxQuality }, { L"(UltraPerf)", NGX_PERFQUALITY_UltraPerformance },
		{ L"(UltraQuality)", NGX_PERFQUALITY_UltraQuality }, { L"(DLAA)", NGX_PERFQUALITY_DLAA },
	};
	const size_t preset = line.rfind(L"Preset ");
	if (preset == std::wstring::npos || preset + 7 >= line.size()) {
		return;
	}
	for (const auto& t : tags) {
		if (line.find(t.tag) != std::wstring::npos) {
			g_PresetByMode[t.mode] = line[preset + 7];
			return;
		}
	}
}

void __cdecl NgxLogCallback(const char* message, NVSDK_NGX_Logging_Level, uint32_t)
{
	if (!message) {
		return;
	}
	std::wstring line;
	for (const char* p = message; *p; p++) {
		if (*p != '\r' && *p != '\n') {
			line += (wchar_t)(unsigned char)*p;
		}
	}
	ReadPresetLine(line);

	// The runtime's configuration dump and model searches say nothing about why
	// something failed.
	for (const wchar_t* noise : { L"NGXLoadConfig", L"NGXLoadFromPath", L"FillCreationParams", L"Container" }) {
		if (line.find(noise) != std::wstring::npos) {
			return;
		}
	}
	DLog(L"NGX: {}", line);
	std::lock_guard lock(g_NgxLogLock);
	g_NgxLog.push_back(std::move(line));
	while (g_NgxLog.size() > 64) {
		g_NgxLog.pop_front();
	}
}

// Every call into NGX runs in its own pipeline state: NGX binds compute shaders,
// views and samplers on the immediate context.
class CNgxStateScope
{
public:
	CNgxStateScope(ID3D11DeviceContext1* pContext, ID3DDeviceContextState* pState)
	{
		if (pContext && pState) {
			m_pContext = pContext;
			pContext->SwapDeviceContextState(pState, &m_pPrevious);
		}
	}
	~CNgxStateScope()
	{
		if (m_pContext && m_pPrevious) {
			m_pContext->SwapDeviceContextState(m_pPrevious, nullptr);
		}
	}

private:
	CComPtr<ID3D11DeviceContext1>   m_pContext;
	CComPtr<ID3DDeviceContextState> m_pPrevious;
};

HRESULT ClearTexture(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, ID3D11Texture2D* pTexture, float value)
{
	CComPtr<ID3D11RenderTargetView> pTarget;
	HRESULT hr = pDevice->CreateRenderTargetView(pTexture, nullptr, &pTarget);
	if (SUCCEEDED(hr)) {
		const FLOAT color[4] = { value, value, value, value };
		pContext->ClearRenderTargetView(pTarget, color);
	}
	return hr;
}

} // namespace

const wchar_t* NgxResultName(NVSDK_NGX_Result r)
{
	switch (r) {
	case NGX_Result_Success:                        return L"Success";
	case NGX_Result_Fail:                           return L"Fail";
	case NGX_Result_FAIL_FeatureNotSupported:       return L"FeatureNotSupported";
	case NGX_Result_FAIL_PlatformError:             return L"PlatformError";
	case NGX_Result_FAIL_FeatureAlreadyExists:      return L"FeatureAlreadyExists";
	case NGX_Result_FAIL_FeatureNotFound:           return L"FeatureNotFound";
	case NGX_Result_FAIL_InvalidParameter:          return L"InvalidParameter";
	case NGX_Result_FAIL_ScratchBufferTooSmall:     return L"ScratchBufferTooSmall";
	case NGX_Result_FAIL_NotInitialized:            return L"NotInitialized";
	case NGX_Result_FAIL_UnsupportedInputFormat:    return L"UnsupportedInputFormat";
	case NGX_Result_FAIL_RWFlagMissing:             return L"RWFlagMissing";
	case NGX_Result_FAIL_MissingInput:              return L"MissingInput";
	case NGX_Result_FAIL_UnableToInitializeFeature: return L"UnableToInitializeFeature";
	case NGX_Result_FAIL_OutOfDate:                 return L"OutOfDate";
	case NGX_Result_FAIL_OutOfGPUMemory:            return L"OutOfGPUMemory";
	case NGX_Result_FAIL_UnsupportedFormat:         return L"UnsupportedFormat";
	case NGX_Result_FAIL_UnableToWriteToAppDataPath:return L"UnableToWriteToAppDataPath";
	case NGX_Result_FAIL_UnsupportedParameter:      return L"UnsupportedParameter";
	case NGX_Result_FAIL_Denied:                    return L"Denied";
	case NGX_Result_FAIL_NotImplemented:            return L"NotImplemented";
	}
	return L"Unknown";
}

// ============================================================================
// Housekeeping
// ============================================================================

CDlssSR::~CDlssSR()
{
	Shutdown();
}

void CDlssSR::Log(const std::wstring& line)
{
	DLog(L"CDlssSR: {}", line);
	m_Log.push_back(line);
	while (m_Log.size() > 16) {
		m_Log.pop_front();
	}
}

bool CDlssSR::Fail(State state, const std::wstring& why)
{
	m_State = state;
	m_Detail = why;
	Log(why);
	return false;
}

std::vector<std::wstring> CDlssSR::CandidateDllPaths(const wchar_t* pConfigured)
{
	std::vector<std::wstring> out;
	if (pConfigured && *pConfigured) {
		std::wstring c = pConfigured;
		if (PathIsDirectoryW(c.c_str())) {
			if (c.back() != L'\\' && c.back() != L'/') {
				c += L'\\';
			}
			c += kSnippetName;
		}
		out.push_back(c);
		return out;   // a path the user gave is the only one tried
	}

	// A subfolder first: StaxRip loads every DLL of its plugin folder as a plugin, so
	// NVIDIA's must not sit beside ours.
	const std::wstring dir = NgxRuntime::ModuleDir();
	out.push_back(dir + L"\\dlss\\" + kSnippetName);
	out.push_back(dir + L"\\" + kSnippetName);
	out.push_back(dir + L"\\..\\dlss\\" + kSnippetName);   // the project's root, from _build
	out.push_back(dir + L"\\..\\" + kSnippetName);
	return out;
}

int CDlssSR::QualityModeFor(UINT inWidth, UINT inHeight, UINT outWidth, UINT outHeight)
{
	if (!inWidth || !inHeight) {
		return NGX_PERFQUALITY_DLAA;
	}
	const double ratio = std::max((double)outWidth / inWidth, (double)outHeight / inHeight);
	return (ratio <= 1.0)  ? NGX_PERFQUALITY_DLAA
	     : (ratio < 1.6)   ? NGX_PERFQUALITY_MaxQuality
	     : (ratio < 1.85)  ? NGX_PERFQUALITY_Balanced
	     : (ratio < 2.5)   ? NGX_PERFQUALITY_MaxPerf
	     :                   NGX_PERFQUALITY_UltraPerformance;
}

const wchar_t* CDlssSR::QualityModeName(int mode)
{
	switch (mode) {
	case NGX_PERFQUALITY_MaxPerf:          return L"Performance";
	case NGX_PERFQUALITY_Balanced:         return L"Balanced";
	case NGX_PERFQUALITY_MaxQuality:       return L"Quality";
	case NGX_PERFQUALITY_UltraPerformance: return L"Ultra Performance";
	case NGX_PERFQUALITY_UltraQuality:     return L"Ultra Quality";
	case NGX_PERFQUALITY_DLAA:             return L"DLAA";
	}
	return L"?";
}

wchar_t CDlssSR::PresetLetter(unsigned preset)
{
	return (preset >= 1 && preset <= 15) ? (wchar_t)(L'A' + preset - 1) : L'-';
}

// ============================================================================
// Bring-up
// ============================================================================

bool CDlssSR::FindSnippet(const wchar_t* pConfiguredDllPath)
{
	for (const auto& candidate : CandidateDllPaths(pConfiguredDllPath)) {
		if (!PathFileExistsW(candidate.c_str()) || PathIsDirectoryW(candidate.c_str())) {
			continue;
		}
		wchar_t full[MAX_PATH] = {};
		if (!GetFullPathNameW(candidate.c_str(), (DWORD)std::size(full), full, nullptr)) {
			continue;
		}
		// The runtime loads the feature by its name from the folder it is given.
		if (_wcsicmp(PathFindFileNameW(full), kSnippetName)) {
			return Fail(State::DllNotFound, std::format(L"the DLL must be named {}", kSnippetName));
		}
		m_DllPath = full;
		m_SnippetDir = NgxRuntime::DirOf(m_DllPath);
		Log(L"DLSS: " + m_DllPath);
		return true;
	}
	return Fail(State::DllNotFound, std::format(L"{} not found", kSnippetName));
}

bool CDlssSR::LoadCore()
{
	std::wstring path;
	m_hCore = NgxRuntime::Load(&path);
	if (!m_hCore) {
		return Fail(State::CoreNotFound, L"the driver's NGX runtime (_nvngx.dll) was not found");
	}
	Log(L"runtime: " + path);

	m_pfnInitExt  = (PFN_NGX_D3D11_Init_Ext)               GetProcAddress(m_hCore, "NVSDK_NGX_D3D11_Init_Ext");
	m_pfnGetCaps  = (PFN_NGX_D3D11_GetCapabilityParameters)GetProcAddress(m_hCore, "NVSDK_NGX_D3D11_GetCapabilityParameters");
	m_pfnDestroy  = (PFN_NGX_D3D11_DestroyParameters)      GetProcAddress(m_hCore, "NVSDK_NGX_D3D11_DestroyParameters");
	m_pfnCreate   = (PFN_NGX_D3D11_CreateFeature)          GetProcAddress(m_hCore, "NVSDK_NGX_D3D11_CreateFeature");
	m_pfnEvaluate = (PFN_NGX_D3D11_EvaluateFeature)        GetProcAddress(m_hCore, "NVSDK_NGX_D3D11_EvaluateFeature");
	m_pfnRelease  = (PFN_NGX_D3D11_ReleaseFeature)         GetProcAddress(m_hCore, "NVSDK_NGX_D3D11_ReleaseFeature");
	m_pfnShutdown = (PFN_NGX_D3D11_Shutdown)               GetProcAddress(m_hCore, "NVSDK_NGX_D3D11_Shutdown");

	if (!m_pfnInitExt || !m_pfnGetCaps || !m_pfnCreate || !m_pfnEvaluate || !m_pfnRelease || !m_pfnShutdown) {
		return Fail(State::CoreNotFound, L"the driver's NGX runtime lacks the Direct3D 11 entry points");
	}

	// From here on the runtime stays in the process, whatever is freed. Its Init loads
	// nvngx_dlss.dll, which nothing unloads again, and gives it a function of the
	// runtime to call on every Evaluate; the DLL keeps the first one it is ever given.
	// So the runtime it met first has to be the one still there (MPCVR-Plus saw the
	// second film's first Evaluate jump to where an unloaded runtime had been).
	HMODULE hPinned = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
			(LPCWSTR)m_hCore, &hPinned)) {
		Log(std::format(L"the NGX runtime could not be kept loaded ({})", GetLastError()));
	}
	return true;
}

bool CDlssSR::Init(ID3D11Device* pDevice, const wchar_t* pConfiguredDllPath)
{
	if (m_bInitialised) {
		return true;
	}
	if (!pDevice) {
		return false;
	}
	m_State = State::Off;
	m_LastResult = 0;
	m_Detail.clear();

	auto abandon = [this]() {
		const State state = m_State;
		const std::wstring detail = m_Detail;
		Shutdown();
		m_State = state;
		m_Detail = detail;
		return false;
	};

	CComQIPtr<ID3D11Device1> pDevice1(pDevice);
	CComPtr<ID3D11DeviceContext> pContext;
	pDevice->GetImmediateContext(&pContext);
	if (!pDevice1 || !pContext || FAILED(pContext->QueryInterface(IID_PPV_ARGS(&m_pContext)))) {
		Fail(State::NoContextState, L"Direct3D 11.1 is not available");
		return abandon();
	}
	const D3D_FEATURE_LEVEL level = pDevice->GetFeatureLevel();
	const UINT stateFlags = (pDevice->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
		? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
	HRESULT hr = pDevice1->CreateDeviceContextState(stateFlags, &level, 1, D3D11_SDK_VERSION,
		__uuidof(ID3D11Device), nullptr, &m_pNgxState);
	if (FAILED(hr)) {
		Fail(State::NoContextState, std::format(L"cannot create a device context state (0x{:08X})", (unsigned)hr));
		return abandon();
	}
	m_pDevice = pDevice;

	if (!FindSnippet(pConfiguredDllPath) || !LoadCore()) {
		return abandon();
	}

	// NGX writes its logs and caches here.
	wchar_t appData[MAX_PATH] = {};
	if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appData))) {
		m_DataPath = std::wstring(appData) + L"\\DLSS Denoise\\ngx\\";
		SHCreateDirectoryExW(nullptr, m_DataPath.c_str(), nullptr);
	} else {
		wchar_t tmp[MAX_PATH] = {};
		GetTempPathW((DWORD)std::size(tmp), tmp);
		m_DataPath = tmp;
	}

	NVSDK_NGX_FeatureCommonInfo fci = {};
	const wchar_t* paths[1] = { m_SnippetDir.c_str() };
	fci.PathListInfo.Path = paths;
	fci.PathListInfo.Length = 1;
	fci.LoggingInfo.LoggingCallback = NgxLogCallback;
	fci.LoggingInfo.MinimumLoggingLevel = NGX_LOGGING_ON;
	// The runtime's own sinks write to the console of whoever loaded it: vspipe's is the
	// encoder's pipe on stderr, and the bench's table is on stdout.
	fci.LoggingInfo.DisableOtherLoggingSinks = true;

	NVSDK_NGX_Result r = NGX_Result_Fail;
	{
		CNgxStateScope scope(m_pContext, m_pNgxState);
		for (const uint32_t version : kSdkVersions) {
			r = m_pfnInitExt(kAppId, m_DataPath.c_str(), pDevice, version, &fci);
			Log(std::format(L"NGX init, SDK 0x{:02X}: 0x{:08X} {}", version, r, NgxResultName(r)));
			m_SdkVersion = version;
			if (r != NGX_Result_FAIL_OutOfDate) {
				break;
			}
		}
	}
	m_LastResult = r;
	if (NGX_FAILED(r)) {
		Fail(State::InitFailed, std::format(L"NGX init failed: 0x{:08X} {}", r, NgxResultName(r)));
		return abandon();
	}
	m_bInitialised = true;
	{
		std::lock_guard lock(g_SessionLock);
		g_Sessions++;
	}

	r = m_pfnGetCaps(&m_pParams);
	if (NGX_FAILED(r) || !m_pParams) {
		m_pParams = nullptr;
		m_LastResult = r;
		Fail(State::InitFailed, std::format(L"no NGX parameters: 0x{:08X} {}", r, NgxResultName(r)));
		return abandon();
	}

	int available = 0;
	m_pParams->Get("SuperSampling.Available", &available);
	if (!available) {
		int initResult = 0;
		int needsDriver = 0;
		unsigned int minMajor = 0, minMinor = 0;
		m_pParams->Get("SuperSampling.FeatureInitResult", &initResult);
		m_pParams->Get("SuperSampling.NeedsUpdatedDriver", &needsDriver);
		m_pParams->Get("SuperSampling.MinDriverVersionMajor", &minMajor);
		m_pParams->Get("SuperSampling.MinDriverVersionMinor", &minMinor);
		if (needsDriver) {
			Fail(State::NotAvailable, std::format(L"DLSS Super Resolution needs driver {}.{} or newer", minMajor, minMinor));
		} else {
			Fail(State::NotAvailable, std::format(L"DLSS Super Resolution is not available: 0x{:08X} {}",
				(unsigned)initResult, NgxResultName((NVSDK_NGX_Result)initResult)));
		}
		return abandon();
	}

	m_State = State::Ready;
	Log(L"initialised");
	return true;
}

void CDlssSR::Shutdown()
{
	ReleaseFeatures();

	if (m_hCore) {
		CNgxStateScope scope(m_pContext, m_pNgxState);
		if (m_pParams && m_pfnDestroy) {
			m_pfnDestroy(m_pParams);
		}
		// Shutdown1(device) crashes inside the runtime after an Init_Ext session; the
		// device-less Shutdown ends it cleanly and a later Init works again.
		if (m_bInitialised) {
			std::lock_guard lock(g_SessionLock);
			if (--g_Sessions == 0 && m_pfnShutdown) {
				m_pfnShutdown();
			}
		}
	}
	m_pParams = nullptr;
	m_bInitialised = false;

	if (m_hCore) {
		// LoadCore's reference, given back. The module itself stays: see there.
		FreeLibrary(m_hCore);
		m_hCore = nullptr;
	}
	m_pfnInitExt = nullptr; m_pfnGetCaps = nullptr; m_pfnDestroy = nullptr;
	m_pfnCreate = nullptr; m_pfnEvaluate = nullptr; m_pfnRelease = nullptr; m_pfnShutdown = nullptr;

	m_pNgxState.Release();
	m_pContext.Release();
	m_pDevice.Release();
	m_DllPath.clear();
	m_SnippetDir.clear();
	m_SdkVersion = 0;

	if (m_State == State::Ready) {
		m_State = State::Off;
	}
}

// ============================================================================
// Feature and evaluation
// ============================================================================

bool CDlssSR::CreateFeature(const Config& requested, int slot)
{
	if (slot < 0 || slot >= kSlots) {
		return false;
	}
	Slot& s = m_Slots[slot];
	Config config = requested;
	if (!config.outWidth || !config.outHeight) {
		config.outWidth = config.width;
		config.outHeight = config.height;
	}
	if (!m_bInitialised || !config.width || !config.height) {
		return false;
	}
	if (s.hFeature && config == s.config) {
		return true;
	}
	ReleaseFeature(slot);

	// The input takes an unordered access view: the engine writes it with a compute pass.
	HRESULT hr = s.texIn.CheckCreate(m_pDevice, config.inputFormat, config.width, config.height, Tex2D_DefaultShaderRTargetUAV);
	if (SUCCEEDED(hr)) {
		hr = s.texOut.CheckCreate(m_pDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, config.outWidth, config.outHeight, Tex2D_DefaultShaderRTargetUAV);
	}
	if (SUCCEEDED(hr)) {
		hr = s.texDepth.CheckCreate(m_pDevice, DXGI_FORMAT_R32_FLOAT, config.width, config.height, Tex2D_DefaultShaderRTarget);
	}
	if (SUCCEEDED(hr)) {
		hr = s.texNoMotion.CheckCreate(m_pDevice, DXGI_FORMAT_R32G32_FLOAT, config.width, config.height, Tex2D_DefaultShaderRTarget);
	}
	if (SUCCEEDED(hr)) {
		hr = s.texExposure.CheckCreate(m_pDevice, DXGI_FORMAT_R32_FLOAT, 1, 1, Tex2D_DefaultShaderRTarget);
		s.exposureSet = -1.0f;
	}
	// A video has no depth: one plane halfway, so nothing reads as sky or as a surface
	// right in front of the camera.
	if (SUCCEEDED(hr)) {
		hr = ClearTexture(m_pDevice, m_pContext, s.texDepth.pTexture, 0.5f);
	}
	if (SUCCEEDED(hr)) {
		hr = ClearTexture(m_pDevice, m_pContext, s.texNoMotion.pTexture, 0.0f);
	}
	if (FAILED(hr)) {
		ReleaseFeature(slot);
		return Fail(State::TexturesFailed, std::format(L"cannot create the textures for {}x{} (0x{:08X})",
			config.width, config.height, (unsigned)hr));
	}

	const int mode = QualityModeFor(config.width, config.height, config.outWidth, config.outHeight);
	int flags = NGX_DLSS_FLAG_MVLowRes;
	if (config.hdr) {
		flags |= NGX_DLSS_FLAG_IsHDR;
	}
	if (config.autoExposure) {
		flags |= NGX_DLSS_FLAG_AutoExposure;
	}
	m_pParams->Set(P_WIDTH, (unsigned int)config.width);
	m_pParams->Set(P_HEIGHT, (unsigned int)config.height);
	m_pParams->Set(P_OUTWIDTH, (unsigned int)config.outWidth);
	m_pParams->Set(P_OUTHEIGHT, (unsigned int)config.outHeight);
	m_pParams->Set(P_PERFQUALITY, (int)mode);
	m_pParams->Set(P_CREATEFLAGS, flags);
	m_pParams->Set(P_OUTSUBRECTS, (int)0);
	m_pParams->Set("CreationNodeMask", (unsigned int)1);
	m_pParams->Set("VisibilityNodeMask", (unsigned int)1);
	for (const char* key : kPresetKeys) {
		m_pParams->Set(key, (unsigned int)config.preset);
	}

	NVSDK_NGX_Result r = NGX_Result_Fail;
	g_PresetByMode[mode] = 0;
	{
		CNgxStateScope scope(m_pContext, m_pNgxState);
		r = m_pfnCreate(m_pContext, NGX_FEATURE_SUPERSAMPLING, m_pParams, &s.hFeature);
	}
	m_LastResult = r;
	if (NGX_FAILED(r) || !s.hFeature) {
		s.hFeature = nullptr;
		ReleaseFeature(slot);
		return Fail(State::FeatureCreateFailed, std::format(L"cannot create the feature {}x{} -> {}x{}: 0x{:08X} {}",
			config.width, config.height, config.outWidth, config.outHeight, r, NgxResultName(r)));
	}
	s.presetUsed = g_PresetByMode[mode];
	s.config = config;
	s.mode = mode;
	m_State = State::Ready;
	m_Detail.clear();
	Log(std::format(L"feature {}: {}", slot, GetStatusLine(slot).substr(6)));   // past "ready "
	return true;
}

void CDlssSR::ReleaseFeature(int slot)
{
	if (slot < 0 || slot >= kSlots) {
		return;
	}
	Slot& s = m_Slots[slot];
	if (s.hFeature && m_pfnRelease) {
		CNgxStateScope scope(m_pContext, m_pNgxState);
		m_pfnRelease(s.hFeature);
	}
	s = Slot();
}

void CDlssSR::ReleaseFeatures()
{
	for (int i = 0; i < kSlots; i++) {
		ReleaseFeature(i);
	}
}

bool CDlssSR::Evaluate(ID3D11Texture2D* pMotion, float exposure, bool reset, int slot)
{
	if (slot < 0 || slot >= kSlots || !m_Slots[slot].hFeature) {
		return false;
	}
	Slot& s = m_Slots[slot];
	if (exposure != s.exposureSet) {
		m_pContext->UpdateSubresource(s.texExposure.pTexture, 0, nullptr, &exposure, sizeof(float), 0);
		s.exposureSet = exposure;
	}

	m_pParams->Set(P_COLOR,         (ID3D11Resource*)s.texIn.pTexture.p);
	m_pParams->Set(P_OUTPUT,        (ID3D11Resource*)s.texOut.pTexture.p);
	m_pParams->Set(P_DEPTH,         (ID3D11Resource*)s.texDepth.pTexture.p);
	m_pParams->Set(P_MOTIONVECTORS, (ID3D11Resource*)(pMotion ? pMotion : s.texNoMotion.pTexture.p));
	m_pParams->Set(P_EXPOSURE,      (ID3D11Resource*)((s.config.hdr && !s.config.autoExposure) ? s.texExposure.pTexture.p : nullptr));
	m_pParams->Set(P_JITTERX, 0.0f);
	m_pParams->Set(P_JITTERY, 0.0f);
	m_pParams->Set(P_SHARPNESS, 0.0f);
	m_pParams->Set(P_RESET, (int)(reset ? 1 : 0));
	m_pParams->Set(P_MVSCALEX, 1.0f);
	m_pParams->Set(P_MVSCALEY, 1.0f);
	m_pParams->Set(P_RENDERWIDTH, (unsigned int)s.config.width);
	m_pParams->Set(P_RENDERHEIGHT, (unsigned int)s.config.height);
	m_pParams->Set(P_PREEXPOSURE, 1.0f);
	m_pParams->Set(P_EXPOSURESCALE, 1.0f);
	m_pParams->Set(P_FRAMETIME, 41.7f);

	NVSDK_NGX_Result r = NGX_Result_Fail;
	{
		CNgxStateScope scope(m_pContext, m_pNgxState);
		r = m_pfnEvaluate(m_pContext, s.hFeature, m_pParams, nullptr);
	}
	m_LastResult = r;
	if (NGX_FAILED(r)) {
		return Fail(State::EvaluateFailed, std::format(L"evaluation failed: 0x{:08X} {}", r, NgxResultName(r)));
	}
	return true;
}

// ============================================================================
// Reporting
// ============================================================================

std::wstring CDlssSR::GetStatusLine(int slot) const
{
	const Slot& s = m_Slots[std::clamp(slot, 0, kSlots - 1)];
	switch (m_State) {
	case State::Off:
		return L"disabled";
	case State::Ready:
		if (!s.hFeature) {
			return L"initialised";
		}
		return std::format(L"ready {}x{} -> {}x{}, {}, preset {}{}, {}{}", s.config.width, s.config.height,
			s.config.outWidth, s.config.outHeight, QualityModeName(s.mode), PresetLetter(s.config.preset),
			s.presetUsed ? std::format(L" (DLSS says {})", s.presetUsed) : L"",
			s.config.hdr ? L"HDR" : L"LDR", s.config.autoExposure ? L", auto exposure" : L"");
	default:
		return m_Detail.empty() ? std::format(L"unavailable: 0x{:08X} {}", m_LastResult, NgxResultName(m_LastResult)) : m_Detail;
	}
}

std::wstring CDlssSR::GetInfoBlock() const
{
	std::wstring s = L"DLSS Super Resolution:\n";
	s += std::format(L"  DLL       : {}\n", m_DllPath.empty() ? L"<not found>" : m_DllPath);
	if (m_SdkVersion) {
		s += std::format(L"  Session   : Direct3D 11, driver NGX runtime, SDK 0x{:02X}\n", m_SdkVersion);
	}
	s += std::format(L"  State     : {}\n", GetStatusLine());
	return s;
}
