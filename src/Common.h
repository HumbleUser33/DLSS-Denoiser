/*
 * This file is part of DLSS Denoiser.
 *
 * DLSS Denoiser is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version. See LICENSE.txt.
 */

#pragma once

// What every source of the engine needs: Windows, ATL's smart pointers, Direct3D 11
// and the standard library. Nothing of DirectShow or MFC: the classes taken from
// MPCVR-Plus only ever used CComPtr from ATL.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <atlbase.h>
#include <d3d11_4.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#ifndef CheckPointer
#define CheckPointer(p, ret) { if (!(p)) return (ret); }
#endif

// One sink for every log line of the engine: the bench prints them, the plugin hands
// them to VapourSynth. Lines are wide, as the NGX and Optical Flow messages are.
namespace dn {

using LogFn = void (*)(const wchar_t* line);
void SetLogSink(LogFn fn);
void LogLine(const std::wstring& line);

std::wstring HR2Str(HRESULT hr);

} // namespace dn

#define DLog(...) dn::LogLine(std::format(__VA_ARGS__))
#define DLogIf(cond, ...) { if (cond) { DLog(__VA_ARGS__); } }
