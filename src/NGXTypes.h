/*
 * This file is part of DLSS Denoiser.
 * Derived from MPCVR-Plus (https://github.com/HumbleUser33/MPCVR-Plus), Source/DLSS/NGXTypes.h,
 * (C) 2026 its authors.
 *
 * DLSS Denoiser is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version. See LICENSE.txt.
 */

#pragma once

// The part of the NGX ABI that DLSS Super Resolution needs, declared by hand rather
// than taken from NVIDIA's headers and static library, as MPCVR-Plus does. The values
// are those of the 310.9.16 SDK headers (include/nvsdk_ngx_defs.h).

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D12Resource;

// ---------------------------------------------------------------- results

typedef uint32_t NVSDK_NGX_Result;

enum : NVSDK_NGX_Result {
	NGX_Result_Success                        = 0x00000001,
	NGX_Result_Fail                           = 0xBAD00000,
	NGX_Result_FAIL_FeatureNotSupported       = 0xBAD00001,
	NGX_Result_FAIL_PlatformError             = 0xBAD00002,
	NGX_Result_FAIL_FeatureAlreadyExists      = 0xBAD00003,
	NGX_Result_FAIL_FeatureNotFound           = 0xBAD00004,
	NGX_Result_FAIL_InvalidParameter          = 0xBAD00005,
	NGX_Result_FAIL_ScratchBufferTooSmall     = 0xBAD00006,
	NGX_Result_FAIL_NotInitialized            = 0xBAD00007,
	NGX_Result_FAIL_UnsupportedInputFormat    = 0xBAD00008,
	NGX_Result_FAIL_RWFlagMissing             = 0xBAD00009,
	NGX_Result_FAIL_MissingInput              = 0xBAD0000A,
	NGX_Result_FAIL_UnableToInitializeFeature = 0xBAD0000B,
	NGX_Result_FAIL_OutOfDate                 = 0xBAD0000C,
	NGX_Result_FAIL_OutOfGPUMemory            = 0xBAD0000D,
	NGX_Result_FAIL_UnsupportedFormat         = 0xBAD0000E,
	NGX_Result_FAIL_UnableToWriteToAppDataPath= 0xBAD0000F,
	NGX_Result_FAIL_UnsupportedParameter      = 0xBAD00010,
	NGX_Result_FAIL_Denied                    = 0xBAD00011,
	NGX_Result_FAIL_NotImplemented            = 0xBAD00012,
};

#define NGX_SUCCEED(r) (((r) & 0xFFF00000) != 0xBAD00000)
#define NGX_FAILED(r)  (((r) & 0xFFF00000) == 0xBAD00000)

const wchar_t* NgxResultName(NVSDK_NGX_Result r);

// ---------------------------------------------------------------- constants

// DLSS Super Resolution.
constexpr uint32_t NGX_FEATURE_SUPERSAMPLING = 1;

// NVSDK_NGX_PerfQuality_Value.
enum : int {
	NGX_PERFQUALITY_MaxPerf          = 0,   // 2x
	NGX_PERFQUALITY_Balanced         = 1,   // 1.72x
	NGX_PERFQUALITY_MaxQuality       = 2,   // 1.5x
	NGX_PERFQUALITY_UltraPerformance = 3,   // 3x
	NGX_PERFQUALITY_UltraQuality     = 4,
	NGX_PERFQUALITY_DLAA             = 5,   // 1x: input and output the same size
};

// NVSDK_NGX_DLSS_Feature_Flags.
enum : int {
	NGX_DLSS_FLAG_IsHDR         = 1 << 0,   // linear or high-range colour, high precision
	NGX_DLSS_FLAG_MVLowRes      = 1 << 1,   // vectors at the input size
	NGX_DLSS_FLAG_MVJittered    = 1 << 2,
	NGX_DLSS_FLAG_DepthInverted = 1 << 3,
	NGX_DLSS_FLAG_AutoExposure  = 1 << 6,
};

// NVSDK_NGX_DLSS_Hint_Render_Preset, as 310.9.16 leaves it: A to D removed, E and F
// deprecated, J and K the first transformer models (K the default for DLAA), L and M
// the second (DLSS 4.5). Default may change with an over-the-air update, so the
// filter always names one.
enum : unsigned int {
	NGX_DLSS_PRESET_Default = 0,
	NGX_DLSS_PRESET_E = 5,
	NGX_DLSS_PRESET_F = 6,
	NGX_DLSS_PRESET_J = 10,
	NGX_DLSS_PRESET_K = 11,
	NGX_DLSS_PRESET_L = 12,
	NGX_DLSS_PRESET_M = 13,
};

// ------------------------------------------------------------------ structs

struct NVSDK_NGX_Handle { unsigned int Id; };

struct NVSDK_NGX_PathListInfo {
	const wchar_t* const* Path;
	unsigned int          Length;
};

enum NVSDK_NGX_Logging_Level : uint32_t {
	NGX_LOGGING_OFF = 0, NGX_LOGGING_ON = 1, NGX_LOGGING_VERBOSE = 2
};

typedef void (__cdecl* NVSDK_NGX_AppLogCallback)(const char* message,
	NVSDK_NGX_Logging_Level level, uint32_t sourceComponent);

struct NVSDK_NGX_LoggingInfo {
	NVSDK_NGX_AppLogCallback LoggingCallback;
	NVSDK_NGX_Logging_Level  MinimumLoggingLevel;
	bool                     DisableOtherLoggingSinks;
};

struct NVSDK_NGX_FeatureCommonInfo {
	NVSDK_NGX_PathListInfo PathListInfo;
	void*                  InternalData;
	NVSDK_NGX_LoggingInfo  LoggingInfo;
};

// --------------------------------------------------- the parameter interface
// The runtime's own object; the vtable order is the SDK's. Do not reorder, and do not
// add a virtual destructor, which would take slot 0 under MSVC.

class NVSDK_NGX_Parameter
{
public:
	virtual void Set(const char* n, unsigned long long v) = 0;                    //  0
	virtual void Set(const char* n, float v) = 0;                                 //  1
	virtual void Set(const char* n, double v) = 0;                                //  2
	virtual void Set(const char* n, unsigned int v) = 0;                          //  3
	virtual void Set(const char* n, int v) = 0;                                   //  4
	virtual void Set(const char* n, ID3D11Resource* v) = 0;                       //  5
	virtual void Set(const char* n, ID3D12Resource* v) = 0;                       //  6
	virtual void Set(const char* n, void* v) = 0;                                 //  7
	virtual NVSDK_NGX_Result Get(const char* n, unsigned long long* o) const = 0; //  8
	virtual NVSDK_NGX_Result Get(const char* n, float* o) const = 0;              //  9
	virtual NVSDK_NGX_Result Get(const char* n, double* o) const = 0;             // 10
	virtual NVSDK_NGX_Result Get(const char* n, unsigned int* o) const = 0;       // 11
	virtual NVSDK_NGX_Result Get(const char* n, int* o) const = 0;                // 12
	virtual NVSDK_NGX_Result Get(const char* n, ID3D11Resource** o) const = 0;    // 13
	virtual NVSDK_NGX_Result Get(const char* n, ID3D12Resource** o) const = 0;    // 14
	virtual NVSDK_NGX_Result Get(const char* n, void** o) const = 0;              // 15
	virtual void Reset() = 0;                                                     // 16
};

// ------------------------------------------------------------ entry points
// The driver's runtime, _nvngx.dll. Its Init_Ext takes the version 4th and the feature
// information 5th; its plain Init answers OutOfDate once a current DLSS is on the path,
// and its Init_ProjectID with the documented argument order crashed (MPCVR-Plus,
// tools/dlssnr_probe --tsr). Shutdown1(device) crashes after an Init_Ext session, the
// device-less Shutdown ends it cleanly.

typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D11_Init_Ext)(
	unsigned long long appId, const wchar_t* appDataPath, ID3D11Device* dev,
	uint32_t sdkVersion, const void* featureInfo);
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D11_GetCapabilityParameters)(NVSDK_NGX_Parameter** out);
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D11_DestroyParameters)(NVSDK_NGX_Parameter* p);
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D11_CreateFeature)(
	ID3D11DeviceContext* ctx, uint32_t featureId,
	NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** out);
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D11_EvaluateFeature)(
	ID3D11DeviceContext* ctx, const NVSDK_NGX_Handle* h,
	const NVSDK_NGX_Parameter* p, void* progressCallback);
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D11_ReleaseFeature)(NVSDK_NGX_Handle* h);
typedef NVSDK_NGX_Result (__cdecl* PFN_NGX_D3D11_Shutdown)(void);
