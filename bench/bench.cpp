/*
 * This file is part of DLSS Denoiser.
 *
 * DLSS Denoiser is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version. See LICENSE.txt.
 */

// dlssdn_bench: what DLSS Super Resolution in DLAA mode does to a graded HDR picture,
// measured where the truth is known. Film frames are taken to PQ BT.2020 at 203 nits
// for diffuse white, grained anew on every frame, and run through DLSS with the exact
// motion; the output is measured against the clean frame in PQ code values, which is
// what an encoder sees.
//
//   --ramp      precision: a smooth PQ ramp, how many levels come out
//   --synth     grain left, detail kept, flicker, against the clean frame
//   --converge  the same, frame by frame after a reset
//   --cost      GPU time of one evaluation at film sizes
//   --refuse    whether DLSS takes an output smaller than its input
//
// The domains DLSS is fed with:
//   ldr_pq        PQ code values without IsHDR, as MPCVR-Plus does (8-bit inside, per the guide)
//   hdr_pq        PQ code values with IsHDR
//   hdr_lin       linear light with IsHDR, 1.0 = 100 nits, exposure 1
//   hdr_lin_shot  the same, the exposure from the sequence's mean luminance (guide 3.9)
//   hdr_lin_auto  the same, DLSS's own exposure

#include "../src/Engine.h"
#include <DirectXPackedVector.h>
#include <wincodec.h>
#include <dxgi1_6.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using DirectX::PackedVector::HALF;

namespace {

int g_failures = 0;
bool g_verbose = false;

void Check(bool ok, const char* what)
{
	if (!ok) {
		printf("  FAIL: %s\n", what);
		g_failures++;
	}
}

// ---------------------------------------------------------------- colour

constexpr double kM1 = 2610.0 / 16384.0;
constexpr double kM2 = 2523.0 / 4096.0 * 128.0;
constexpr double kC1 = 3424.0 / 4096.0;
constexpr double kC2 = 2413.0 / 4096.0 * 32.0;
constexpr double kC3 = 2392.0 / 4096.0 * 32.0;

// PQ code value -> luminance, in units of 10000 nits.
inline float PqToLinear(float e)
{
	e = std::clamp(e, 0.0f, 1.0f);
	const double p = std::pow((double)e, 1.0 / kM2);
	return (float)std::pow(std::max(p - kC1, 0.0) / (kC2 - kC3 * p), 1.0 / kM1);
}

inline float LinearToPq(float y)
{
	y = std::max(y, 0.0f);
	const double m = std::pow((double)y, kM1);
	return (float)std::pow((kC1 + kC2 * m) / (1.0 + kC3 * m), kM2);
}

// A lookup for PQ -> linear: exact enough (65536 entries, linear interpolation) and
// thousands of times faster than two pow per sample over 4K frames.
struct PqTable {
	std::vector<float> lut;
	PqTable() : lut(65537) { for (int i = 0; i <= 65536; i++) lut[i] = PqToLinear(i / 65536.0f); }
	float operator()(float e) const {
		const float x = std::clamp(e, 0.0f, 1.0f) * 65536.0f;
		const int i = std::min((int)x, 65535);
		return lut[i] + (lut[i + 1] - lut[i]) * (x - i);
	}
};
const PqTable& Pq2Lin() { static PqTable t; return t; }

// And back, on log2 of the luminance from 2^-24 to 1, where PQ is smooth.
struct InvPqTable {
	static constexpr int N = 65536;
	std::vector<float> lut;
	InvPqTable() : lut(N + 1) { for (int i = 0; i <= N; i++) lut[i] = LinearToPq(std::exp2(-24.0f + 24.0f * i / N)); }
	float operator()(float y) const {
		if (y <= 5.96e-8f) {
			return LinearToPq(y);
		}
		const float x = std::min((std::log2(y) + 24.0f) * (N / 24.0f), (float)N);
		const int i = std::min((int)x, N - 1);
		return lut[i] + (lut[i + 1] - lut[i]) * (x - i);
	}
};
const InvPqTable& Lin2Pq() { static InvPqTable t; return t; }

inline float LumaPq(const float* rgb)
{
	return 0.2627f * rgb[0] + 0.6780f * rgb[1] + 0.0593f * rgb[2];   // BT.2020 on R'G'B'
}

// ---------------------------------------------------------------- pictures

struct Picture {
	int w = 0, h = 0;
	std::vector<float> rgba;   // PQ BT.2020 R'G'B', alpha 1
	float* at(int x, int y) { return &rgba[((size_t)y * w + x) * 4]; }
	const float* at(int x, int y) const { return &rgba[((size_t)y * w + x) * 4]; }
};

// A film frame from a PNG (8 or 16 bits, sRGB-coded SDR) taken to HDR the way a grade
// would place it: BT.1886 to light, diffuse white at 203 nits, BT.709 to BT.2020
// primaries, PQ.
bool LoadReferenceAsPq(IWICImagingFactory* factory, const wchar_t* path, Picture& out, std::string& error)
{
	CComPtr<IWICBitmapDecoder> decoder;
	CComPtr<IWICBitmapFrameDecode> frame;
	CComPtr<IWICFormatConverter> converter;
	if (FAILED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder))
			|| FAILED(decoder->GetFrame(0, &frame))
			|| FAILED(factory->CreateFormatConverter(&converter))
			|| FAILED(converter->Initialize(frame, GUID_WICPixelFormat64bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
		error = "cannot decode";
		return false;
	}
	UINT w = 0, h = 0;
	converter->GetSize(&w, &h);
	std::vector<uint16_t> px((size_t)w * h * 4);
	if (FAILED(converter->CopyPixels(nullptr, w * 8, (UINT)(px.size() * 2), (BYTE*)px.data()))) {
		error = "cannot read pixels";
		return false;
	}
	static const float m[3][3] = {
		{ 0.627404f, 0.329283f, 0.043313f },
		{ 0.069097f, 0.919540f, 0.011362f },
		{ 0.016391f, 0.088013f, 0.895595f },
	};
	out.w = (int)w;
	out.h = (int)h;
	out.rgba.assign((size_t)w * h * 4, 1.0f);
	std::vector<float> gammaToLin(65536);
	for (int i = 0; i < 65536; i++) {
		gammaToLin[i] = std::pow(i / 65535.0f, 2.4f) * (203.0f / 10000.0f);
	}
#pragma omp parallel for
	for (int y = 0; y < (int)h; y++) {
		for (int x = 0; x < (int)w; x++) {
			const uint16_t* s = &px[((size_t)y * w + x) * 4];
			const float l[3] = { gammaToLin[s[0]], gammaToLin[s[1]], gammaToLin[s[2]] };
			float* d = out.at(x, y);
			for (int c = 0; c < 3; c++) {
				d[c] = Lin2Pq()(m[c][0] * l[0] + m[c][1] * l[1] + m[c][2] * l[2]);
			}
		}
	}
	return true;
}

Picture Crop(const Picture& src, int x0, int y0, int w, int h)
{
	Picture p;
	p.w = w;
	p.h = h;
	p.rgba.resize((size_t)w * h * 4);
	for (int y = 0; y < h; y++) {
		memcpy(p.at(0, y), src.at(x0, y0 + y), (size_t)w * 4 * sizeof(float));
	}
	return p;
}

// ---------------------------------------------------------------- grain

inline uint64_t SplitMix(uint64_t& s)
{
	uint64_t z = (s += 0x9E3779B97F4A7C15ull);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

inline float Gauss(uint64_t& s)
{
	const double u1 = ((SplitMix(s) >> 11) + 0.5) * (1.0 / 9007199254740992.0);
	const double u2 = ((SplitMix(s) >> 11) + 0.5) * (1.0 / 9007199254740992.0);
	return (float)(std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2));
}

// Film-like grain on PQ code values, new on every frame: mostly common to the three
// dye layers, partly each its own, and clumped over a pixel or two as a 4K scan's grain
// is (a Gaussian of 0.8 pixel, its variance put back to one). sigma is the rms per
// channel, in PQ code units.
void AddGrain(Picture& p, float sigma, uint32_t seed)
{
	const int w = p.w, h = p.h;
	std::vector<float> field[4];
	for (auto& f : field) {
		f.resize((size_t)w * h);
	}
#pragma omp parallel for
	for (int y = 0; y < h; y++) {
		for (int k = 0; k < 4; k++) {
			uint64_t s = ((uint64_t)seed << 40) ^ ((uint64_t)k << 32) ^ (uint64_t)y ^ 0x5DEECE66Dull;
			SplitMix(s);
			float* row = &field[k][(size_t)y * w];
			for (int x = 0; x < w; x++) {
				row[x] = Gauss(s);
			}
		}
	}
	// Separable blur, radius 2, sigma 0.8.
	float kernel[5];
	double sum = 0, sum2 = 0;
	for (int i = -2; i <= 2; i++) {
		kernel[i + 2] = (float)std::exp(-0.5 * (i / 0.8) * (i / 0.8));
		sum += kernel[i + 2];
	}
	for (float& k : kernel) {
		k = (float)(k / sum);
		sum2 += (double)k * k;
	}
	const float renorm = (float)(1.0 / sum2);   // the two passes take the deviation down by sum2 in all
	for (auto& f : field) {
		std::vector<float> tmp((size_t)w * h);
#pragma omp parallel for
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				float a = 0;
				for (int i = -2; i <= 2; i++) {
					a += kernel[i + 2] * f[(size_t)y * w + std::clamp(x + i, 0, w - 1)];
				}
				tmp[(size_t)y * w + x] = a;
			}
		}
#pragma omp parallel for
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				float a = 0;
				for (int i = -2; i <= 2; i++) {
					a += kernel[i + 2] * tmp[(size_t)std::clamp(y + i, 0, h - 1) * w + x];
				}
				f[(size_t)y * w + x] = a * renorm;
			}
		}
	}
	const float common = 0.8f, own = 0.6f;   // 0.8^2 + 0.6^2 = 1
#pragma omp parallel for
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const size_t i = (size_t)y * w + x;
			float* d = p.at(x, y);
			for (int c = 0; c < 3; c++) {
				d[c] = std::clamp(d[c] + sigma * (common * field[3][i] + own * field[c][i]), 0.0f, 1.0f);
			}
		}
	}
}

// ---------------------------------------------------------------- domains

enum class Domain { LdrPq, HdrPq, HdrLin, HdrLinShot, HdrLinAuto };

struct DomainInfo { Domain d; const char* name; bool hdr; bool linear; bool autoExposure; };
const DomainInfo kDomains[] = {
	{ Domain::LdrPq,      "ldr_pq",       false, false, false },
	{ Domain::HdrPq,      "hdr_pq",       true,  false, false },
	{ Domain::HdrLin,     "hdr_lin",      true,  true,  false },
	{ Domain::HdrLinShot, "hdr_lin_shot", true,  true,  false },
	{ Domain::HdrLinAuto, "hdr_lin_auto", true,  true,  true  },
};

const DomainInfo* FindDomain(const std::string& name)
{
	for (const auto& d : kDomains) {
		if (name == d.name) {
			return &d;
		}
	}
	return nullptr;
}

constexpr float kLinearScale = 10000.0f / 100.0f;   // 1.0 = 100 nits

// What DLSS is given, from PQ code values.
void ToDomain(const DomainInfo& d, const Picture& pq, std::vector<float>& out)
{
	out.resize(pq.rgba.size());
	if (!d.linear) {
		out = pq.rgba;
		return;
	}
	const PqTable& lut = Pq2Lin();
#pragma omp parallel for
	for (long long i = 0; i < (long long)pq.rgba.size(); i += 4) {
		for (int c = 0; c < 3; c++) {
			out[i + c] = lut(pq.rgba[i + c]) * kLinearScale;
		}
		out[i + 3] = 1.0f;
	}
}

// And back to PQ code values.
void FromDomain(const DomainInfo& d, const std::vector<float>& in, Picture& pq, int w, int h)
{
	pq.w = w;
	pq.h = h;
	pq.rgba.resize(in.size());
	if (!d.linear) {
		pq.rgba = in;
		return;
	}
	const InvPqTable& lut = Lin2Pq();
#pragma omp parallel for
	for (long long i = 0; i < (long long)in.size(); i += 4) {
		for (int c = 0; c < 3; c++) {
			pq.rgba[i + c] = lut(in[i + c] / kLinearScale);
		}
		pq.rgba[i + 3] = 1.0f;
	}
}

// The exposure that brings middle grey to 0.18 (guide 3.9), from the mean luminance.
float ShotExposure(const Picture& pq)
{
	const PqTable& lut = Pq2Lin();
	double sum = 0;
	for (size_t i = 0; i < pq.rgba.size(); i += 4) {
		sum += lut(LumaPq(&pq.rgba[i])) * kLinearScale;
	}
	const double mean = std::max(sum / (pq.rgba.size() / 4), 1e-4);
	return (float)(0.18 / (mean * (1.0 - 0.18)));
}

// ---------------------------------------------------------------- GPU

struct Gpu {
	CComPtr<ID3D11Device> dev;
	CComPtr<ID3D11DeviceContext> ctx;
	std::wstring name;
};

bool CreateDevice(Gpu& g)
{
	CComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
		return false;
	}
	CComPtr<IDXGIAdapter1> adapter;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 desc = {};
		adapter->GetDesc1(&desc);
		if (desc.VendorId == 0x10DE) {
			g.name = desc.Description;
			break;
		}
		adapter.Release();
	}
	if (!adapter) {
		return false;
	}
	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1 };
	return SUCCEEDED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 1,
		D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx));
}

void Upload(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const std::vector<float>& rgba, int w)
{
	D3D11_TEXTURE2D_DESC desc = {};
	tex->GetDesc(&desc);
	if (desc.Format == DXGI_FORMAT_R32G32B32A32_FLOAT) {
		ctx->UpdateSubresource(tex, 0, nullptr, rgba.data(), 16 * w, 0);
		return;
	}
	std::vector<HALF> half(rgba.size());
	DirectX::PackedVector::XMConvertFloatToHalfStream(half.data(), sizeof(HALF), rgba.data(), sizeof(float), rgba.size());
	ctx->UpdateSubresource(tex, 0, nullptr, half.data(), 8 * w, 0);
}

bool Download(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, std::vector<float>& out)
{
	D3D11_TEXTURE2D_DESC sd = {};
	tex->GetDesc(&sd);
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.MiscFlags = 0;
	CComPtr<ID3D11Texture2D> stage;
	if (FAILED(dev->CreateTexture2D(&sd, nullptr, &stage))) {
		return false;
	}
	ctx->CopyResource(stage, tex);
	D3D11_MAPPED_SUBRESOURCE mr = {};
	if (FAILED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mr))) {
		return false;
	}
	out.resize((size_t)sd.Width * sd.Height * 4);
	for (UINT y = 0; y < sd.Height; y++) {
		DirectX::PackedVector::XMConvertHalfToFloatStream(&out[(size_t)y * sd.Width * 4], sizeof(float),
			(const HALF*)((const BYTE*)mr.pData + (size_t)y * mr.RowPitch), sizeof(HALF), sd.Width * 4);
	}
	ctx->Unmap(stage, 0);
	return true;
}

// A constant motion field: content at x now was at x + (dx, dy) one frame earlier.
bool MotionTexture(ID3D11Device* dev, ID3D11DeviceContext* ctx, int w, int h, float dx, float dy, Tex2D_t& tex)
{
	if (FAILED(tex.CheckCreate(dev, DXGI_FORMAT_R32G32_FLOAT, w, h, Tex2D_DefaultShaderRTarget))) {
		return false;
	}
	CComPtr<ID3D11RenderTargetView> rtv;
	if (FAILED(dev->CreateRenderTargetView(tex.pTexture, nullptr, &rtv))) {
		return false;
	}
	const FLOAT v[4] = { dx, dy, 0, 0 };
	ctx->ClearRenderTargetView(rtv, v);
	return true;
}

template <class F>
double GpuMs(ID3D11Device* dev, ID3D11DeviceContext* ctx, int runs, F&& work)
{
	CComPtr<ID3D11Query> disjoint, t0q, t1q;
	D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
	dev->CreateQuery(&qd, &disjoint);
	qd.Query = D3D11_QUERY_TIMESTAMP;
	dev->CreateQuery(&qd, &t0q);
	dev->CreateQuery(&qd, &t1q);
	if (!disjoint || !t0q || !t1q) {
		return -1;
	}
	for (int i = 0; i < 3; i++) {
		work();
	}
	ctx->Begin(disjoint);
	ctx->End(t0q);
	for (int i = 0; i < runs; i++) {
		work();
	}
	ctx->End(t1q);
	ctx->End(disjoint);
	D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
	UINT64 a = 0, b = 0;
	while (ctx->GetData(disjoint, &dj, sizeof(dj), 0) == S_FALSE) { Sleep(0); }
	while (ctx->GetData(t0q, &a, sizeof(a), 0) == S_FALSE) { Sleep(0); }
	while (ctx->GetData(t1q, &b, sizeof(b), 0) == S_FALSE) { Sleep(0); }
	return (!dj.Disjoint && dj.Frequency) ? double(b - a) * 1000.0 / dj.Frequency / runs : -1;
}

// ---------------------------------------------------------------- metrics

// Where the clean picture is flat, grain is all there is to see; where it has texture,
// detail is. Classified once on the clean frame by its 5x5 luma deviation.
struct Classes {
	std::vector<uint8_t> cls;   // 0 neither, 1 flat, 2 textured
	int margin = 32;
};

Classes Classify(const Picture& clean)
{
	Classes c;
	const int w = clean.w, h = clean.h;
	c.cls.assign((size_t)w * h, 0);
	std::vector<float> y((size_t)w * h);
	for (size_t i = 0; i < y.size(); i++) {
		y[i] = LumaPq(&clean.rgba[i * 4]);
	}
#pragma omp parallel for
	for (int py = c.margin; py < h - c.margin; py++) {
		for (int px = c.margin; px < w - c.margin; px++) {
			double s = 0, s2 = 0;
			for (int j = -2; j <= 2; j++) {
				for (int i = -2; i <= 2; i++) {
					const double v = y[(size_t)(py + j) * w + px + i];
					s += v;
					s2 += v * v;
				}
			}
			const double sd = std::sqrt(std::max(s2 / 25 - (s / 25) * (s / 25), 0.0));
			c.cls[(size_t)py * w + px] = sd < 0.0015 ? 1 : (sd > 0.012 ? 2 : 0);
		}
	}
	return c;
}

struct FrameMetrics {
	double psnr = 0;        // RGB, PQ code values, peak 1, inside the margin
	double noiseRms = 0;    // rms of luma error on flat pixels
	double detail = 0;      // high-pass projection on the clean high-pass, textured pixels: 1 all kept
	double bias = 0;        // mean luma error, x1000
};

FrameMetrics Measure(const Picture& out, const Picture& clean, const Classes& c)
{
	const int w = clean.w, h = clean.h, m = c.margin;
	auto Y = [](const Picture& p, int x, int y) { return LumaPq(p.at(x, y)); };
	auto HighPass = [&](const Picture& p, int x, int y) {
		float s = 0;
		for (int j = -1; j <= 1; j++) {
			for (int i = -1; i <= 1; i++) {
				s += Y(p, x + i, y + j);
			}
		}
		return Y(p, x, y) - s / 9.0f;
	};
	double se = 0, n = 0, flatSe = 0, flatN = 0, dot = 0, norm = 0, bias = 0;
#pragma omp parallel for reduction(+:se, n, flatSe, flatN, dot, norm, bias)
	for (int y = m; y < h - m; y++) {
		for (int x = m; x < w - m; x++) {
			const float* o = out.at(x, y);
			const float* r = clean.at(x, y);
			for (int k = 0; k < 3; k++) {
				se += (double)(o[k] - r[k]) * (o[k] - r[k]);
			}
			n += 3;
			const double e = LumaPq(o) - LumaPq(r);
			bias += e;
			const uint8_t cls = c.cls[(size_t)y * w + x];
			if (cls == 1) {
				flatSe += e * e;
				flatN += 1;
			} else if (cls == 2) {
				const double hc = HighPass(clean, x, y);
				dot += HighPass(out, x, y) * hc;
				norm += hc * hc;
			}
		}
	}
	FrameMetrics f;
	f.psnr = 10.0 * std::log10(1.0 / std::max(se / n, 1e-12));
	f.noiseRms = std::sqrt(flatSe / std::max(flatN, 1.0));
	f.detail = norm > 0 ? dot / norm : 0;
	f.bias = 1000.0 * bias / std::max(n / 3, 1.0);
	return f;
}

// Frame-to-frame change of luma, compensated by the exact motion, inside the margin.
double Flicker(const Picture& now, const Picture& before, int dx, int dy, int margin)
{
	double s = 0, n = 0;
	const int w = now.w, h = now.h;
#pragma omp parallel for reduction(+:s, n)
	for (int y = margin; y < h - margin; y++) {
		for (int x = margin; x < w - margin; x++) {
			s += std::abs(LumaPq(now.at(x, y)) - LumaPq(before.at(x + dx, y + dy)));
			n += 1;
		}
	}
	return s / std::max(n, 1.0);
}

// ---------------------------------------------------------------- sequences

struct Sequence {
	const char* name;
	int dx, dy;   // content motion per frame, pixels: the crop moves by this much
};

const Sequence kSequences[] = {
	{ "still", 0, 0 },
	{ "pan",   2, 1 },
};

struct RunResult {
	std::vector<FrameMetrics> frames;
	std::vector<double> flicker;
	FrameMetrics input;
	double inputFlicker = 0;
	double cleanFlicker = 0;
	bool ok = false;
};

struct Options {
	std::wstring dll;
	std::vector<std::wstring> refs;
	std::vector<std::string> domains;
	std::vector<unsigned> presets;
	std::vector<float> grains;
	int frames = 40;
	int measureFrom = 24;
	int cropW = 3200, cropH = 1800;
	bool fp32 = false;
	int pad = 32;
};

// One sequence through DLSS, the clean and the grained frame made anew for each frame.
RunResult RunSequence(Gpu& g, CDlssSR& sr, const Picture& ref, const Sequence& seq, const DomainInfo& d,
	unsigned preset, float grain, const Options& opt, uint32_t seedBase)
{
	RunResult r;
	const int w = opt.cropW, h = opt.cropH;
	CDlssSR::Config cfg;
	cfg.width = w;
	cfg.height = h;
	cfg.preset = preset;
	cfg.hdr = d.hdr;
	cfg.autoExposure = d.autoExposure;
	cfg.inputFormat = opt.fp32 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
	if (!sr.CreateFeature(cfg)) {
		printf("  feature: %S\n", sr.GetStatusLine().c_str());
		g_failures++;
		return r;
	}
	if (g_verbose) {
		printf("    %S\n", sr.GetStatusLine().c_str());
	}
	Tex2D_t motion;
	if (!MotionTexture(g.dev, g.ctx, w, h, (float)seq.dx, (float)seq.dy, motion)) {
		g_failures++;
		return r;
	}
	const int x0 = (ref.w - w - seq.dx * opt.frames) / 2, y0 = (ref.h - h - seq.dy * opt.frames) / 2;
	Picture prevOut, prevIn, prevClean;
	Classes classes;
	std::vector<float> dlssIn, dlssOut;
	float exposure = 1.0f;
	double inFlicker = 0, cleanFlicker = 0, flickerN = 0;
	FrameMetrics inSum;
	int inN = 0;
	for (int t = 0; t < opt.frames; t++) {
		const Picture clean = Crop(ref, x0 + seq.dx * t, y0 + seq.dy * t, w, h);
		if (t == 0) {
			classes = Classify(clean);
		}
		Picture noisy = clean;
		AddGrain(noisy, grain, seedBase + t);
		if (t == 0 && d.d == Domain::HdrLinShot) {
			exposure = ShotExposure(noisy);
		}
		ToDomain(d, noisy, dlssIn);
		Upload(g.ctx, sr.GetInput()->pTexture, dlssIn, w);
		if (!sr.Evaluate(t ? motion.pTexture.p : nullptr, exposure, t == 0)) {
			printf("  evaluate: %S\n", sr.GetStatusLine().c_str());
			g_failures++;
			return r;
		}
		if (!Download(g.dev, g.ctx, sr.GetOutput()->pTexture, dlssOut)) {
			g_failures++;
			return r;
		}
		Picture out;
		FromDomain(d, dlssOut, out, w, h);
		const Classes& c = (seq.dx || seq.dy) ? Classify(clean) : classes;
		r.frames.push_back(Measure(out, clean, c));
		if (t >= opt.measureFrom) {
			const FrameMetrics fi = Measure(noisy, clean, c);
			inSum.psnr += fi.psnr; inSum.noiseRms += fi.noiseRms; inSum.detail += fi.detail; inSum.bias += fi.bias;
			inN++;
		}
		if (t > 0) {
			r.flicker.push_back(Flicker(out, prevOut, seq.dx, seq.dy, 40));
			if (t >= opt.measureFrom) {
				inFlicker += Flicker(noisy, prevIn, seq.dx, seq.dy, 40);
				cleanFlicker += Flicker(clean, prevClean, seq.dx, seq.dy, 40);
				flickerN++;
			}
		} else {
			r.flicker.push_back(0);
		}
		prevOut = std::move(out);
		prevIn = std::move(noisy);
		prevClean = clean;
	}
	r.input.psnr = inSum.psnr / inN;
	r.input.noiseRms = inSum.noiseRms / inN;
	r.input.detail = inSum.detail / inN;
	r.input.bias = inSum.bias / inN;
	r.inputFlicker = inFlicker / std::max(flickerN, 1.0);
	r.cleanFlicker = cleanFlicker / std::max(flickerN, 1.0);
	r.ok = true;
	return r;
}

FrameMetrics Average(const std::vector<FrameMetrics>& f, int from)
{
	FrameMetrics a;
	int n = 0;
	for (int i = from; i < (int)f.size(); i++, n++) {
		a.psnr += f[i].psnr; a.noiseRms += f[i].noiseRms; a.detail += f[i].detail; a.bias += f[i].bias;
	}
	if (n) {
		a.psnr /= n; a.noiseRms /= n; a.detail /= n; a.bias /= n;
	}
	return a;
}

double AverageOf(const std::vector<double>& v, int from)
{
	double s = 0;
	int n = 0;
	for (int i = from; i < (int)v.size(); i++, n++) {
		s += v[i];
	}
	return n ? s / n : 0;
}

bool InitDlss(Gpu& g, CDlssSR& sr, const Options& opt)
{
	if (!sr.Init(g.dev, opt.dll.empty() ? nullptr : opt.dll.c_str())) {
		printf("%S", sr.GetInfoBlock().c_str());
		return false;
	}
	printf("%S", sr.GetInfoBlock().c_str());
	return true;
}

std::vector<Picture> LoadRefs(const Options& opt)
{
	std::vector<Picture> refs;
	CComPtr<IWICImagingFactory> factory;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
		return refs;
	}
	for (const auto& path : opt.refs) {
		Picture p;
		std::string error;
		if (!LoadReferenceAsPq(factory, path.c_str(), p, error)) {
			printf("  %S: %s\n", path.c_str(), error.c_str());
			g_failures++;
			continue;
		}
		if (p.w < opt.cropW + 100 || p.h < opt.cropH + 50) {
			printf("  %S: %dx%d is too small for a %dx%d crop\n", path.c_str(), p.w, p.h, opt.cropW, opt.cropH);
			g_failures++;
			continue;
		}
		refs.push_back(std::move(p));
	}
	return refs;
}

// ---------------------------------------------------------------- suites

int RunSynth(Gpu& g, const Options& opt, bool converge)
{
	printf("\n== %s: DLAA on PQ film frames, fresh grain on every frame, exact motion ==\n", converge ? "Convergence" : "Quality");
	printf("   crop %dx%d, %d frames, measured from frame %d; PQ code values, luma BT.2020\n",
		opt.cropW, opt.cropH, opt.frames, opt.measureFrom);
	printf("   noise: rms luma error on flat areas, x1000 (10-bit code = 0.98); detail: 1.00 = the clean picture's\n");
	printf("   flicker: frame-to-frame luma change along the motion, x1000\n");
	CDlssSR sr;
	if (!InitDlss(g, sr, opt)) {
		return 2;
	}
	const std::vector<Picture> refs = LoadRefs(opt);
	if (refs.empty()) {
		printf("  no reference frame\n");
		return 1;
	}
	for (float grain : opt.grains) {
		for (const auto& seq : kSequences) {
			printf("\n-- grain %.4f (%.1f codes), %s (%d,%d px/frame), %zu frames averaged --\n",
				grain, grain * 1023, seq.name, seq.dx, seq.dy, refs.size());
			printf("%-14s %-6s %8s %8s %8s %8s %8s\n", "domain", "preset", "PSNR", "noise", "detail", "flicker", "bias");
			bool inputPrinted = false;
			for (const auto& dn : opt.domains) {
				const DomainInfo* d = FindDomain(dn);
				if (!d) {
					printf("  unknown domain %s\n", dn.c_str());
					continue;
				}
				for (unsigned preset : opt.presets) {
					FrameMetrics avg, inAvg;
					double flick = 0, inFlick = 0, cleanFlick = 0;
					std::vector<FrameMetrics> series(opt.frames);
					std::vector<double> flickSeries(opt.frames);
					int n = 0;
					for (size_t k = 0; k < refs.size(); k++) {
						const RunResult r = RunSequence(g, sr, refs[k], seq, *d, preset, grain, opt, 1000u * (uint32_t)k + 7);
						if (!r.ok) {
							continue;
						}
						const FrameMetrics a = Average(r.frames, opt.measureFrom);
						avg.psnr += a.psnr; avg.noiseRms += a.noiseRms; avg.detail += a.detail; avg.bias += a.bias;
						inAvg.psnr += r.input.psnr; inAvg.noiseRms += r.input.noiseRms; inAvg.detail += r.input.detail;
						flick += AverageOf(r.flicker, opt.measureFrom);
						inFlick += r.inputFlicker;
						cleanFlick += r.cleanFlicker;
						for (int t = 0; t < opt.frames; t++) {
							series[t].psnr += r.frames[t].psnr;
							series[t].noiseRms += r.frames[t].noiseRms;
							series[t].detail += r.frames[t].detail;
							flickSeries[t] += r.flicker[t];
						}
						n++;
					}
					if (!n) {
						continue;
					}
					if (!inputPrinted) {
						printf("%-14s %-6s %8.2f %8.3f %8.3f %8.3f %8s\n", "(input)", "", inAvg.psnr / n,
							1000 * inAvg.noiseRms / n, inAvg.detail / n, 1000 * inFlick / n, "");
						if (seq.dx || seq.dy) {
							printf("%-14s %-6s %8s %8s %8s %8.3f %8s\n", "(clean)", "", "", "", "", 1000 * cleanFlick / n, "");
						}
						inputPrinted = true;
					}
					printf("%-14s %-6c %8.2f %8.3f %8.3f %8.3f %8.3f\n", d->name, CDlssSR::PresetLetter(preset),
						avg.psnr / n, 1000 * avg.noiseRms / n, avg.detail / n, 1000 * flick / n, avg.bias / n);
					if (converge) {
						printf("    frame:  ");
						for (int t = 0; t < opt.frames; t += (t < 16 ? 1 : 4)) {
							printf("%6d", t);
						}
						printf("\n    noise:  ");
						for (int t = 0; t < opt.frames; t += (t < 16 ? 1 : 4)) {
							printf("%6.2f", 1000 * series[t].noiseRms / n);
						}
						printf("\n    detail: ");
						for (int t = 0; t < opt.frames; t += (t < 16 ? 1 : 4)) {
							printf("%6.3f", series[t].detail / n);
						}
						printf("\n");
					}
				}
			}
		}
	}
	return 0;
}

// A smooth PQ ramp, the same on every row, with no grain and no motion: how finely
// does what comes out follow it?
int RunRamp(Gpu& g, const Options& opt)
{
	printf("\n== Precision: a smooth PQ ramp, still, no grain ==\n");
	printf("   err: rms and max of output - input, in 10-bit codes; plateau: longest run of one output value, pixels\n");
	printf("   levels: distinct output values per 10-bit code the ramp crosses (1.00 = as fine as 10-bit)\n");
	CDlssSR sr;
	if (!InitDlss(g, sr, opt)) {
		return 2;
	}
	const int w = 3840, h = 256;
	struct Range { const char* name; float lo, hi; };
	const Range ranges[] = {
		{ "dark 0.05-0.30 (0.05-14 nits)", 0.05f, 0.30f },
		{ "mid 0.30-0.60 (14-240 nits)", 0.30f, 0.60f },
		{ "bright 0.60-0.80 (240-1500 nits)", 0.60f, 0.80f },
	};
	for (const auto& range : ranges) {
		printf("\n-- %s --\n", range.name);
		printf("%-14s %-6s %8s %8s %8s %8s\n", "domain", "preset", "err rms", "err max", "plateau", "levels");
		Picture ramp;
		ramp.w = w;
		ramp.h = h;
		ramp.rgba.resize((size_t)w * h * 4);
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				const float v = range.lo + (range.hi - range.lo) * (x + 0.5f) / w;
				float* p = ramp.at(x, y);
				p[0] = p[1] = p[2] = v;
				p[3] = 1;
			}
		}
		for (const auto& dn : opt.domains) {
			const DomainInfo* d = FindDomain(dn);
			if (!d) {
				continue;
			}
			for (unsigned preset : opt.presets) {
				CDlssSR::Config cfg;
				cfg.width = w;
				cfg.height = h;
				cfg.preset = preset;
				cfg.hdr = d->hdr;
				cfg.autoExposure = d->autoExposure;
				cfg.inputFormat = opt.fp32 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
				if (!sr.CreateFeature(cfg)) {
					printf("  feature: %S\n", sr.GetStatusLine().c_str());
					g_failures++;
					continue;
				}
				std::vector<float> in, outv;
				ToDomain(*d, ramp, in);
				const float exposure = (d->d == Domain::HdrLinShot) ? ShotExposure(ramp) : 1.0f;
				bool ok = true;
				for (int t = 0; t < 16 && ok; t++) {
					Upload(g.ctx, sr.GetInput()->pTexture, in, w);
					ok = sr.Evaluate(nullptr, exposure, t == 0);
				}
				if (!ok || !Download(g.dev, g.ctx, sr.GetOutput()->pTexture, outv)) {
					printf("  %S\n", sr.GetStatusLine().c_str());
					g_failures++;
					continue;
				}
				Picture out;
				FromDomain(*d, outv, out, w, h);
				const int y = h / 2;
				double se = 0, emax = 0;
				int plateau = 1, run = 1, distinct = 1;
				for (int x = 64; x < w - 64; x++) {
					const double e = (LumaPq(out.at(x, y)) - LumaPq(ramp.at(x, y))) * 1023.0;
					se += e * e;
					emax = std::max(emax, std::abs(e));
					if (x > 64) {
						if (LumaPq(out.at(x, y)) == LumaPq(out.at(x - 1, y))) {
							run++;
						} else {
							run = 1;
							distinct++;
						}
						plateau = std::max(plateau, run);
					}
				}
				const double codes = (range.hi - range.lo) * 1023.0 * (w - 128) / w;
				printf("%-14s %-6c %8.3f %8.3f %8d %8.2f\n", d->name, CDlssSR::PresetLetter(preset),
					std::sqrt(se / (w - 128)), emax, plateau, distinct / codes);
			}
		}
	}
	return 0;
}

int RunCost(Gpu& g, const Options& opt)
{
	printf("\n== Cost: GPU time of one DLAA evaluation, constant motion ==\n");
	CDlssSR sr;
	if (!InitDlss(g, sr, opt)) {
		return 2;
	}
	const struct { int w, h; } sizes[] = { { 3840, 2160 }, { 3840, 1600 }, { 1920, 1080 } };
	printf("%-10s %-14s", "size", "domain");
	for (unsigned preset : opt.presets) {
		printf("   %c ms", CDlssSR::PresetLetter(preset));
	}
	printf("\n");
	for (const auto& s : sizes) {
		for (const auto& dn : opt.domains) {
			const DomainInfo* d = FindDomain(dn);
			if (!d) {
				continue;
			}
			printf("%4dx%-5d %-14s", s.w, s.h, d->name);
			for (unsigned preset : opt.presets) {
				CDlssSR::Config cfg;
				cfg.width = s.w;
				cfg.height = s.h;
				cfg.preset = preset;
				cfg.hdr = d->hdr;
				cfg.autoExposure = d->autoExposure;
				cfg.inputFormat = opt.fp32 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
				Tex2D_t motion;
				if (!sr.CreateFeature(cfg) || !MotionTexture(g.dev, g.ctx, s.w, s.h, 1.0f, 0.5f, motion)) {
					printf("   fail");
					g_failures++;
					continue;
				}
				bool ok = true;
				const double ms = GpuMs(g.dev, g.ctx, 10, [&]() { ok = ok && sr.Evaluate(motion.pTexture, 1.0f, false); });
				printf(ok ? " %6.1f" : "   fail", ms);
			}
			printf("\n");
		}
	}
	return 0;
}

int RunRefuse(Gpu& g, const Options& opt)
{
	printf("\n== An output smaller than the input ==\n");
	CDlssSR sr;
	if (!InitDlss(g, sr, opt)) {
		return 2;
	}
	CDlssSR::Config cfg;
	cfg.width = 3840;
	cfg.height = 2160;
	cfg.outWidth = 1920;
	cfg.outHeight = 1080;
	const bool made = sr.CreateFeature(cfg);
	printf("  3840x2160 -> 1920x1080: %s (%S)\n", made ? "accepted" : "refused", sr.GetStatusLine().c_str());
	return 0;
}

// ---------------------------------------------------------------- two passes

// DLSS run forward and backward over the same sequence with the exact motion, and the two
// outputs averaged, on the middle frames where both passes have as much history: what a
// filter that may read ahead could gain over one pass.
int RunBidir(Gpu& g, const Options& opt)
{
	printf("\n== Two passes: forward, backward, and their mean, exact motion ==\n");
	const int frames = opt.frames, from = frames / 3, to = frames - frames / 3;
	printf("   crop %dx%d, %d frames, measured on frames %d to %d; columns as --synth\n", opt.cropW, opt.cropH, frames, from, to - 1);
	CDlssSR sr;
	if (!InitDlss(g, sr, opt)) {
		return 2;
	}
	const std::vector<Picture> refs = LoadRefs(opt);
	if (refs.empty()) {
		return 1;
	}
	for (float grain : opt.grains) {
		for (const auto& seq : kSequences) {
			for (const auto& dn : opt.domains) {
				const DomainInfo* d = FindDomain(dn);
				if (!d) {
					continue;
				}
				for (unsigned preset : opt.presets) {
					printf("\n-- grain %.4f, %s, %s, preset %c --\n", grain, seq.name, d->name, CDlssSR::PresetLetter(preset));
					printf("%-10s %8s %8s %8s %8s\n", "pass", "PSNR", "noise", "detail", "flicker");
					CDlssSR::Config cfg;
					cfg.width = opt.cropW;
					cfg.height = opt.cropH;
					cfg.preset = preset;
					cfg.hdr = d->hdr;
					cfg.autoExposure = d->autoExposure;
					if (!sr.CreateFeature(cfg)) {
						printf("  %S\n", sr.GetStatusLine().c_str());
						g_failures++;
						continue;
					}
					FrameMetrics sum[3];
					double flick[3] = {};
					int n = 0, nf = 0;
					for (size_t k = 0; k < refs.size(); k++) {
						const Picture& ref = refs[k];
						const int w = opt.cropW, h = opt.cropH;
						const int x0 = (ref.w - w - seq.dx * frames) / 2, y0 = (ref.h - h - seq.dy * frames) / 2;
						std::vector<Picture> fwd(to - from), bwd(to - from);
						for (int pass = 0; pass < 2; pass++) {
							Tex2D_t motion;
							const float sign = pass ? -1.0f : 1.0f;
							MotionTexture(g.dev, g.ctx, w, h, sign * seq.dx, sign * seq.dy, motion);
							for (int i = 0; i < frames; i++) {
								const int t = pass ? frames - 1 - i : i;
								Picture noisy = Crop(ref, x0 + seq.dx * t, y0 + seq.dy * t, w, h);
								AddGrain(noisy, grain, 1000u * (uint32_t)k + 7 + t);
								std::vector<float> in, outv;
								ToDomain(*d, noisy, in);
								Upload(g.ctx, sr.GetInput()->pTexture, in, w);
								if (!sr.Evaluate(i ? motion.pTexture.p : nullptr, 1.0f, i == 0) || !Download(g.dev, g.ctx, sr.GetOutput()->pTexture, outv)) {
									g_failures++;
									return 1;
								}
								if (t >= from && t < to) {
									FromDomain(*d, outv, (pass ? bwd : fwd)[t - from], w, h);
								}
							}
						}
						for (int t = from; t < to; t++) {
							const Picture clean = Crop(ref, x0 + seq.dx * t, y0 + seq.dy * t, w, h);
							const Classes c = Classify(clean);
							Picture mean = fwd[t - from];
							for (size_t i = 0; i < mean.rgba.size(); i++) {
								mean.rgba[i] = 0.5f * (fwd[t - from].rgba[i] + bwd[t - from].rgba[i]);
							}
							const Picture* outs[3] = { &fwd[t - from], &bwd[t - from], &mean };
							for (int j = 0; j < 3; j++) {
								const FrameMetrics f = Measure(*outs[j], clean, c);
								sum[j].psnr += f.psnr; sum[j].noiseRms += f.noiseRms; sum[j].detail += f.detail;
							}
							n++;
							if (t > from) {
								const Picture* prevs[3] = { &fwd[t - from - 1], &bwd[t - from - 1], nullptr };
								Picture prevMean = fwd[t - from - 1];
								for (size_t i = 0; i < prevMean.rgba.size(); i++) {
									prevMean.rgba[i] = 0.5f * (fwd[t - from - 1].rgba[i] + bwd[t - from - 1].rgba[i]);
								}
								prevs[2] = &prevMean;
								for (int j = 0; j < 3; j++) {
									flick[j] += Flicker(*outs[j], *prevs[j], seq.dx, seq.dy, 40);
								}
								nf++;
							}
						}
					}
					const char* names[3] = { "forward", "backward", "mean" };
					for (int j = 0; j < 3; j++) {
						printf("%-10s %8.2f %8.3f %8.3f %8.3f\n", names[j], sum[j].psnr / n, 1000 * sum[j].noiseRms / n,
							sum[j].detail / n, nf ? 1000 * flick[j] / nf : 0.0);
					}
				}
			}
		}
	}
	return 0;
}

// ---------------------------------------------------------------- two lanes and a cut

void ToPlanes(const Picture& p, std::vector<float> planes[3]);
void FromPlanes(const std::vector<float> planes[3], int w, int h, Picture& p);

// Two shots of a pan, a cut between them, through the engine: lane 0 forward, lane 1
// backward, and their mean -- plain, or weighted by the history each lane holds -- frame
// by frame around the cut, where one pass alone starts over with all its grain.
int RunTwoPass(const Options& opt, bool estimated)
{
	const int frames = opt.frames, cut = frames / 2, w = opt.cropW, h = opt.cropH;
	printf("\n== Two lanes through the engine, a cut at frame %d, %s motion ==\n", cut, estimated ? "measured" : "exact");
	printf("   crop %dx%d, %d frames, hdr_lin, preset %c; noise as --synth, x1000\n", w, h, frames, CDlssSR::PresetLetter(opt.presets[0]));
	const std::vector<Picture> refs = LoadRefs(opt);
	if (refs.size() < 2) {
		printf("  two reference frames are needed (--ref twice)\n");
		return 1;
	}
	CEngine engine;
	if (!engine.Init(0, opt.dll.empty() ? nullptr : opt.dll.c_str())) {
		printf("  %S\n", engine.GetError().c_str());
		return 2;
	}
	CEngine::Config cfg;
	cfg.width = w;
	cfg.height = h;
	cfg.pad = opt.pad;
	cfg.preset = opt.presets[0];
	cfg.motion = estimated;
	cfg.passes = 2;
	if (!engine.Configure(cfg)) {
		printf("  %S\n", engine.GetError().c_str());
		return 2;
	}
	const int dx = 2, dy = 1;
	auto clean = [&](int t) {
		const Picture& ref = refs[t < cut ? 0 : 1];
		const int x0 = (ref.w - w - dx * frames) / 2, y0 = (ref.h - h - dy * frames) / 2;
		return Crop(ref, x0 + dx * t, y0 + dy * t, w, h);
	};
	for (float grain : opt.grains) {
		std::vector<std::vector<float>> outs[2];
		std::vector<int> history[2];
		std::vector<int> cuts[2];
		for (int lane = 0; lane < 2; lane++) {
			outs[lane].resize(frames);
			history[lane].resize(frames);
			cuts[lane].resize(frames);
			std::vector<float> mv((size_t)w * h * 2);
			for (size_t i = 0; i < (size_t)w * h; i++) {
				mv[i * 2] = lane ? -(float)dx : (float)dx;
				mv[i * 2 + 1] = lane ? -(float)dy : (float)dy;
			}
			for (int i = 0; i < frames; i++) {
				const int t = lane ? frames - 1 - i : i;
				Picture noisy = clean(t);
				AddGrain(noisy, grain, 77u + t);
				std::vector<float> in[3], out[3];
				ToPlanes(noisy, in);
				CEngine::Planes ip;
				CEngine::OutPlanes op;
				for (int c = 0; c < 3; c++) {
					out[c].resize((size_t)w * h);
					ip.data[c] = in[c].data();
					ip.stride[c] = w * sizeof(float);
					op.data[c] = out[c].data();
					op.stride[c] = w * sizeof(float);
				}
				// Exact motion knows nothing of the cut: the lane is told, as the GPU's detection
				// would tell it, on the first picture of the new shot in its own direction.
				const bool shotStart = lane ? (t == cut - 1) : (t == cut);
				const bool useExact = !estimated && i > 0 && !shotStart;
				if (!engine.Process(ip, nullptr, useExact ? mv.data() : nullptr, i == 0 || (!estimated && shotStart), op, lane)) {
					printf("  %S\n", engine.GetError().c_str());
					return 2;
				}
				history[lane][t] = engine.GetHistory(lane);
				cuts[lane][t] = engine.WasCut() ? 1 : 0;
				Picture result;
				FromPlanes(out, w, h, result);
				outs[lane][t] = std::move(result.rgba);
			}
		}
		printf("\n-- grain %.4f (%.1f codes) --\n", grain, grain * 1023);
		printf("%5s %4s %4s %9s %9s %9s %9s\n", "frame", "h fw", "h bw", "forward", "backward", "mean", "weighted");
		double sum[4] = {};
		for (int t = 0; t < frames; t++) {
			const Picture c = clean(t);
			const Classes cls = Classify(c);
			Picture p[4];
			for (int k = 0; k < 2; k++) {
				p[k].w = w; p[k].h = h; p[k].rgba = outs[k][t];
			}
			p[2] = p[0];
			p[3] = p[0];
			const float wf = CEngine::Weight(history[0][t]), wb = CEngine::Weight(history[1][t]);
			for (size_t i = 0; i < p[2].rgba.size(); i++) {
				p[2].rgba[i] = 0.5f * (p[0].rgba[i] + p[1].rgba[i]);
				p[3].rgba[i] = (wf * p[0].rgba[i] + wb * p[1].rgba[i]) / (wf + wb);
			}
			double noise[4];
			for (int k = 0; k < 4; k++) {
				noise[k] = Measure(p[k], c, cls).noiseRms;
				sum[k] += noise[k];
			}
			if (std::abs(t - cut) <= 8 || t == 0 || t == frames - 1) {
				printf("%5d %4d %4d %9.3f %9.3f %9.3f %9.3f%s\n", t, history[0][t], history[1][t], 1000 * noise[0], 1000 * noise[1],
					1000 * noise[2], 1000 * noise[3], (cuts[0][t] || cuts[1][t]) ? "  cut seen" : "");
			}
		}
		printf("%5s %4s %4s %9.3f %9.3f %9.3f %9.3f  (mean over all frames)\n", "all", "", "",
			1000 * sum[0] / frames, 1000 * sum[1] / frames, 1000 * sum[2] / frames, 1000 * sum[3] / frames);
	}
	return 0;
}

// ---------------------------------------------------------------- the motion suite

// A crop at a fractional origin, Lanczos 3 on PQ code values: a pan slower than a pixel a
// frame, which no integer crop gives.
Picture CropFrac(const Picture& src, double x0, double y0, int w, int h)
{
	auto lanczos = [](double x) {
		if (std::abs(x) < 1e-9) return 1.0;
		if (std::abs(x) >= 3.0) return 0.0;
		const double px = 3.14159265358979 * x;
		return 3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px);
	};
	const int ix = (int)std::floor(x0), iy = (int)std::floor(y0);
	const double fx = x0 - ix, fy = y0 - iy;
	double kx[6], ky[6], sx = 0, sy = 0;
	for (int i = 0; i < 6; i++) {
		kx[i] = lanczos(i - 2 - fx);
		ky[i] = lanczos(i - 2 - fy);
		sx += kx[i];
		sy += ky[i];
	}
	Picture tmp;
	tmp.w = w;
	tmp.h = h + 6;
	tmp.rgba.resize((size_t)tmp.w * tmp.h * 4);
#pragma omp parallel for
	for (int y = 0; y < tmp.h; y++) {
		const int sy0 = std::clamp(iy - 2 + y, 0, src.h - 1);
		for (int x = 0; x < w; x++) {
			for (int c = 0; c < 4; c++) {
				double a = 0;
				for (int i = 0; i < 6; i++) {
					a += kx[i] * src.at(std::clamp(ix + x + i - 2, 0, src.w - 1), sy0)[c];
				}
				tmp.at(x, y)[c] = (float)(a / sx);
			}
		}
	}
	Picture out;
	out.w = w;
	out.h = h;
	out.rgba.resize((size_t)w * h * 4);
#pragma omp parallel for
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			for (int c = 0; c < 4; c++) {
				double a = 0;
				for (int i = 0; i < 6; i++) {
					a += ky[i] * tmp.at(x, y + i)[c];
				}
				out.at(x, y)[c] = std::clamp((float)(a / sy), 0.0f, 1.0f);
			}
		}
	}
	return out;
}

// A sequence with its exact motion: content at x in frame t was at x + mv in frame t-1.
struct MotionSequence {
	const char* name;
	double dx, dy;        // the whole picture's content motion, per frame (the crop moves by -mv)
	bool object;          // a patch of another part of the frame moving over a still one
	int ox, oy;           // its motion on screen, pixels per frame
};

const MotionSequence kMotionSequences[] = {
	{ "still",   0.0, 0.0, false, 0, 0 },
	{ "pan",     2.0, 1.0, false, 0, 0 },
	{ "slowpan", 0.7, 0.3, false, 0, 0 },
	{ "object",  0.0, 0.0, true,  4, 2 },
};

void MakeFrame(const Picture& ref, const MotionSequence& seq, int t, int frames, int w, int h,
	Picture& clean, std::vector<float>& mv, std::vector<uint8_t>* objectMask)
{
	const double span = std::max(std::abs(seq.dx), std::abs(seq.dy)) * frames + 8;
	const double x0 = (ref.w - w - span) / 2 + seq.dx * t, y0 = (ref.h - h - span) / 2 + seq.dy * t;
	clean = (seq.dx == std::floor(seq.dx) && seq.dy == std::floor(seq.dy))
		? Crop(ref, (int)x0, (int)y0, w, h) : CropFrac(ref, x0, y0, w, h);
	mv.assign((size_t)w * h * 2, 0.0f);
	for (size_t i = 0; i < (size_t)w * h; i++) {
		mv[i * 2 + 0] = (float)seq.dx;
		mv[i * 2 + 1] = (float)seq.dy;
	}
	if (objectMask) {
		objectMask->assign((size_t)w * h, 0);
	}
	if (seq.object) {
		const int pw = w * 3 / 10, ph = h * 4 / 10;              // the patch, 30 x 40 % of the frame
		const int srcX = 40, srcY = ref.h - ph - 40;              // taken from the reference's lower left
		const int px = w / 5 + seq.ox * t, py = h / 4 + seq.oy * t;
		for (int y = 0; y < ph; y++) {
			const int sy = py + y;
			if (sy < 0 || sy >= h) continue;
			for (int x = 0; x < pw; x++) {
				const int sx = px + x;
				if (sx < 0 || sx >= w) continue;
				memcpy(clean.at(sx, sy), ref.at(srcX + x, srcY + y), 4 * sizeof(float));
				mv[((size_t)sy * w + sx) * 2 + 0] = (float)-seq.ox;
				mv[((size_t)sy * w + sx) * 2 + 1] = (float)-seq.oy;
				if (objectMask) {
					(*objectMask)[(size_t)sy * w + sx] = 1;
				}
			}
		}
	}
}

void ToPlanes(const Picture& p, std::vector<float> planes[3])
{
	for (int c = 0; c < 3; c++) {
		planes[c].resize((size_t)p.w * p.h);
	}
	for (size_t i = 0; i < (size_t)p.w * p.h; i++) {
		planes[0][i] = p.rgba[i * 4 + 0];
		planes[1][i] = p.rgba[i * 4 + 1];
		planes[2][i] = p.rgba[i * 4 + 2];
	}
}

void FromPlanes(const std::vector<float> planes[3], int w, int h, Picture& p)
{
	p.w = w;
	p.h = h;
	p.rgba.resize((size_t)w * h * 4);
	for (size_t i = 0; i < (size_t)w * h; i++) {
		p.rgba[i * 4 + 0] = planes[0][i];
		p.rgba[i * 4 + 1] = planes[1][i];
		p.rgba[i * 4 + 2] = planes[2][i];
		p.rgba[i * 4 + 3] = 1.0f;
	}
}

// A cheap prefilter for the motion's clip: a Gaussian of the grained frame.
Picture Blur(const Picture& src, double sigma)
{
	const int r = (int)std::ceil(3 * sigma);
	std::vector<float> k(2 * r + 1);
	double sum = 0;
	for (int i = -r; i <= r; i++) {
		k[i + r] = (float)std::exp(-0.5 * i * i / (sigma * sigma));
		sum += k[i + r];
	}
	for (float& v : k) v = (float)(v / sum);
	Picture tmp = src, out = src;
#pragma omp parallel for
	for (int y = 0; y < src.h; y++) {
		for (int x = 0; x < src.w; x++) {
			for (int c = 0; c < 3; c++) {
				float a = 0;
				for (int i = -r; i <= r; i++) a += k[i + r] * src.at(std::clamp(x + i, 0, src.w - 1), y)[c];
				tmp.at(x, y)[c] = a;
			}
		}
	}
#pragma omp parallel for
	for (int y = 0; y < src.h; y++) {
		for (int x = 0; x < src.w; x++) {
			for (int c = 0; c < 3; c++) {
				float a = 0;
				for (int i = -r; i <= r; i++) a += k[i + r] * tmp.at(x, std::clamp(y + i, 0, src.h - 1))[c];
				out.at(x, y)[c] = a;
			}
		}
	}
	return out;
}

struct MotionMethod {
	const char* name;
	enum Kind { Exact, Zero, Flow } kind;
	CMotionField::Settings settings;
	enum Ref { None, Clean, Blurred, Dlss } ref;   // Dlss: a first DLSS pass, real-time vectors, as the motion's clip
};

std::vector<MotionMethod> MotionMethods()
{
	std::vector<MotionMethod> m;
	const CMotionField::Settings realtime = CMotionField::Realtime();
	CMotionField::Settings max;                          // the defaults: flow at the picture's size, a vector a pixel, slow
	CMotionField::Settings half = max;
	half.flowFactor = 2;
	half.gridSize = 2;
	CMotionField::Settings grid4 = max;
	grid4.gridSize = 4;
	CMotionField::Settings global = max;
	global.globalOnly = true;
	m.push_back({ "exact", MotionMethod::Exact, max, MotionMethod::None });
	m.push_back({ "zero", MotionMethod::Zero, max, MotionMethod::None });
	m.push_back({ "realtime f4g4", MotionMethod::Flow, realtime, MotionMethod::None });
	m.push_back({ "f2g2 slow", MotionMethod::Flow, half, MotionMethod::None });
	m.push_back({ "f1g4 slow", MotionMethod::Flow, grid4, MotionMethod::None });
	m.push_back({ "max f1g1 slow", MotionMethod::Flow, max, MotionMethod::None });
	m.push_back({ "max global only", MotionMethod::Flow, global, MotionMethod::None });
	m.push_back({ "max ref blur", MotionMethod::Flow, max, MotionMethod::Blurred });
	m.push_back({ "realtime ref blur", MotionMethod::Flow, realtime, MotionMethod::Blurred });
	m.push_back({ "max ref clean", MotionMethod::Flow, max, MotionMethod::Clean });
	// Under heavy grain a moving subject's own vectors, a vector a pixel, fall short of the
	// trust the cleaning asks for: what makes them pass.
	CMotionField::Settings v = max;
	v.flowBlur = 2;
	m.push_back({ "max blur2", MotionMethod::Flow, v, MotionMethod::None });
	v.flowBlur = 4;
	m.push_back({ "max blur4", MotionMethod::Flow, v, MotionMethod::None });
	v = max;
	v.costScale = 2.0f;
	m.push_back({ "max cost x2", MotionMethod::Flow, v, MotionMethod::None });
	v.costScale = 0.0f;
	m.push_back({ "max no cost", MotionMethod::Flow, v, MotionMethod::None });
	v = max;
	v.vectorEvidence = 1.5f;
	m.push_back({ "max evidence 1.5", MotionMethod::Flow, v, MotionMethod::None });
	v = max;
	v.gridSize = 2;
	m.push_back({ "f1g2 slow", MotionMethod::Flow, v, MotionMethod::None });
	v = max;
	v.flowBlur = 4;
	v.costScale = 0.0f;
	m.push_back({ "max blur4 no cost", MotionMethod::Flow, v, MotionMethod::None });
	// Optical Flow's own vectors, every block keeping its vector whole: what the engine
	// itself measures.
	auto raw = [](CMotionField::Settings s) {
		s.snapLow = 0.0f;
		s.snapHigh = 1e-6f;
		s.vectorShift = 0;
		s.vectorSupportLow = 0.0f;
		s.vectorSupportHigh = 1e-6f;
		s.vectorEvidence = 1e-6f;
		s.vectorRefine = 0;
		s.vectorTemporal = 1.0f;
		return s;
	};
	m.push_back({ "raw f1g1", MotionMethod::Flow, raw(max), MotionMethod::None });
	m.push_back({ "raw f1g4", MotionMethod::Flow, raw(grid4), MotionMethod::None });
	m.push_back({ "raw f4g4", MotionMethod::Flow, raw(realtime), MotionMethod::None });
	m.push_back({ "raw f1g1 ref clean", MotionMethod::Flow, raw(max), MotionMethod::Clean });
	m.push_back({ "raw f1g1 ref dlss", MotionMethod::Flow, raw(max), MotionMethod::Dlss });
	m.push_back({ "max ref dlss", MotionMethod::Flow, max, MotionMethod::Dlss });
	m.push_back({ "f1g4 ref dlss", MotionMethod::Flow, grid4, MotionMethod::Dlss });
	m.push_back({ "f2g2 ref dlss", MotionMethod::Flow, half, MotionMethod::Dlss });
	CMotionField::Settings tiled = max;
	tiled.tiles = true;
	m.push_back({ "max tiles", MotionMethod::Flow, tiled, MotionMethod::None });
	return m;
}

int RunMotion(const Options& opt, const std::vector<std::string>& only)
{
	printf("\n== Motion: what the vectors cost DLSS, against the exact motion ==\n");
	printf("   crop %dx%d, %d frames, measured from frame %d, hdr_lin, preset %c, band %d\n",
		opt.cropW, opt.cropH, opt.frames, opt.measureFrom, CDlssSR::PresetLetter(opt.presets[0]), opt.pad);
	printf("   mv err: rms of the vectors' error, pixels (object: inside it); noise/detail/flicker as --synth\n");
	const std::vector<Picture> refs = LoadRefs(opt);
	if (refs.empty()) {
		printf("  no reference frame\n");
		return 1;
	}
	CEngine engine;
	if (!engine.Init(0, opt.dll.empty() ? nullptr : opt.dll.c_str())) {
		printf("  %S\n", engine.GetError().c_str());
		return 2;
	}
	const int w = opt.cropW, h = opt.cropH;
	// The first pass for the methods that measure the motion on DLSS's own output: real-time
	// vectors, the same preset.
	CEngine first;
	bool firstReady = false;
	for (float grain : opt.grains) {
		for (const auto& seq : kMotionSequences) {
			printf("\n-- grain %.4f (%.1f codes), %s, %zu frames averaged --\n", grain, grain * 1023, seq.name, refs.size());
			printf("%-20s %8s %8s %8s %8s %8s %8s\n", "vectors", "mv err", "PSNR", "noise", "detail", "flicker", "ms");
			for (const auto& method : MotionMethods()) {
				if (!only.empty() && std::find(only.begin(), only.end(), std::string(method.name)) == only.end()) {
					continue;
				}
				CEngine::Config cfg;
				cfg.width = w;
				cfg.height = h;
				cfg.pad = opt.pad;
				cfg.preset = opt.presets[0];
				cfg.linear = true;
				cfg.motion = method.kind == MotionMethod::Flow;
				cfg.motionSettings = method.settings;
				if (!engine.Configure(cfg)) {
					printf("  %S\n", engine.GetError().c_str());
					g_failures++;
					continue;
				}
				double mvErr = 0, mvN = 0, psnr = 0, noise = 0, detail = 0, flick = 0, ms = 0;
				int n = 0;
				for (size_t k = 0; k < refs.size(); k++) {
					Picture prevOut;
					for (int t = 0; t < opt.frames; t++) {
						Picture clean;
						std::vector<float> mv;
						std::vector<uint8_t> mask;
						MakeFrame(refs[k], seq, t, opt.frames, w, h, clean, mv, seq.object ? &mask : nullptr);
						Picture noisy = clean;
						AddGrain(noisy, grain, 1000u * (uint32_t)k + 7 + t);
						std::vector<float> in[3], refPlanes[3], out[3];
						ToPlanes(noisy, in);
						CEngine::Planes ip, rp;
						for (int c = 0; c < 3; c++) {
							ip.data[c] = in[c].data();
							ip.stride[c] = w * sizeof(float);
							out[c].resize((size_t)w * h);
						}
						if (method.ref == MotionMethod::Dlss) {
							if (!firstReady) {
								CEngine::Config fc;
								fc.width = w;
								fc.height = h;
								fc.pad = opt.pad;
								fc.preset = opt.presets[0];
								fc.motionSettings = CMotionField::Realtime();
								if (!first.Init(0, opt.dll.empty() ? nullptr : opt.dll.c_str()) || !first.Configure(fc)) {
									printf("  first pass: %S\n", first.GetError().c_str());
									return 2;
								}
								firstReady = true;
							}
							CEngine::OutPlanes fo;
							for (int c = 0; c < 3; c++) {
								refPlanes[c].resize((size_t)w * h);
								fo.data[c] = refPlanes[c].data();
								fo.stride[c] = w * sizeof(float);
							}
							if (!first.Process(ip, nullptr, nullptr, t == 0, fo)) {
								printf("  first pass: %S\n", first.GetError().c_str());
								return 2;
							}
							for (int c = 0; c < 3; c++) {
								rp.data[c] = refPlanes[c].data();
								rp.stride[c] = w * sizeof(float);
							}
						} else if (method.ref != MotionMethod::None) {
							ToPlanes(method.ref == MotionMethod::Clean ? clean : Blur(noisy, 1.2), refPlanes);
							for (int c = 0; c < 3; c++) {
								rp.data[c] = refPlanes[c].data();
								rp.stride[c] = w * sizeof(float);
							}
						}
						CEngine::OutPlanes op;
						for (int c = 0; c < 3; c++) {
							op.data[c] = out[c].data();
							op.stride[c] = w * sizeof(float);
						}
						const auto start = std::chrono::steady_clock::now();
						const bool ok = engine.Process(ip, method.ref != MotionMethod::None ? &rp : nullptr,
							method.kind == MotionMethod::Exact && t > 0 ? mv.data() : nullptr, t == 0, op);
						const double frameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
						if (!ok) {
							printf("  %S\n", engine.GetError().c_str());
							g_failures++;
							break;
						}
						if (g_verbose && method.kind == MotionMethod::Flow) {
							float gx = 0, gy = 0, gs = 0;
							engine.GetGlobalMotion(gx, gy, gs);
							printf("    t=%2d global %+7.3f %+7.3f spread %.3f cut %d (exact %+.2f %+.2f)\n", t, gx, gy, gs, engine.WasCut() ? 1 : 0, seq.dx, seq.dy);
						}
						Picture result;
						FromPlanes(out, w, h, result);
						if (t >= opt.measureFrom) {
							const Classes c = Classify(clean);
							const FrameMetrics f = Measure(result, clean, c);
							psnr += f.psnr;
							noise += f.noiseRms;
							detail += f.detail;
							if (seq.dx == std::floor(seq.dx) && seq.dy == std::floor(seq.dy)) {
								flick += Flicker(result, prevOut, (int)seq.dx, (int)seq.dy, 40);
							}
							ms += frameMs;
							n++;
							std::vector<float> got;
							if (method.kind == MotionMethod::Flow && engine.ReadMotion(got)) {
								for (int y = 48; y < h - 48; y++) {
									for (int x = 48; x < w - 48; x++) {
										const size_t i = (size_t)y * w + x;
										if (seq.object && !mask[i]) continue;
										const double ex = got[i * 2] - mv[i * 2], ey = got[i * 2 + 1] - mv[i * 2 + 1];
										mvErr += ex * ex + ey * ey;
										mvN++;
									}
								}
							}
						}
						prevOut = std::move(result);
					}
				}
				if (!n) {
					continue;
				}
				const std::string err = mvN ? std::format("{:8.3f}", std::sqrt(mvErr / mvN)) : std::string("       -");
				printf("%-20s %s %8.2f %8.3f %8.3f %8.3f %8.0f\n", method.name, err.c_str(), psnr / n, 1000 * noise / n,
					detail / n, 1000 * flick / n, ms / n);
			}
		}
	}
	return 0;
}

void Usage()
{
	printf(
		"dlssdn_bench [--ramp] [--synth] [--converge] [--cost] [--refuse] [options]\n"
		"  --dll <path>        nvngx_dlss.dll or its folder (default: dlss\\ beside or above the bench)\n"
		"  --ref <png>         a film frame, 4K; several allowed\n"
		"  --domain <list>     ldr_pq,hdr_pq,hdr_lin,hdr_lin_shot,hdr_lin_auto (default hdr_lin)\n"
		"  --preset <list>     letters among E,F,J,K,L,M, or 0 for DLSS's default (default K)\n"
		"  --grain <list>      rms per channel in PQ code units (default 0.0035,0.010)\n"
		"  --frames <n>        frames per sequence (default 40), --from <n> first measured (default 24)\n"
		"  --crop <WxH>        crop of the reference (default 3200x1800)\n"
		"  --fp32              RGBA32F input instead of RGBA16F\n"
		"  --motion            the vectors against the exact motion (--methods <list> picks rows by name)\n"
		"  --bidir             forward, backward and their mean, exact motion, on the middle third\n"
		"  --twopass           the engine's two lanes across a cut, exact motion (--twopass-mv: measured)\n"
		"  --pad <n>           guard band of the engine, pixels (default 32)\n"
		"  -v                  every NGX line\n");
}

template <class T, class F>
std::vector<T> SplitList(const std::string& s, F&& conv)
{
	std::vector<T> out;
	size_t start = 0;
	while (start <= s.size()) {
		const size_t end = s.find(',', start);
		const std::string item = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (!item.empty()) {
			out.push_back(conv(item));
		}
		if (end == std::string::npos) {
			break;
		}
		start = end + 1;
	}
	return out;
}

unsigned PresetFromText(const std::string& s)
{
	if (s.size() == 1 && s[0] >= 'A' && s[0] <= 'O') {
		return (unsigned)(s[0] - 'A' + 1);
	}
	return (unsigned)std::stoul(s);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	Options opt;
	bool ramp = false, synth = false, converge = false, cost = false, refuse = false, motion = false, bidir = false;
	int twopass = 0;
	std::vector<std::string> methods;
	auto narrow = [](const wchar_t* w) { std::string s; for (; *w; w++) s += (char)*w; return s; };
	for (int i = 1; i < argc; i++) {
		const std::wstring a = argv[i];
		auto next = [&]() -> std::wstring { return (i + 1 < argc) ? argv[++i] : L""; };
		if (a == L"--ramp") ramp = true;
		else if (a == L"--synth") synth = true;
		else if (a == L"--converge") converge = true;
		else if (a == L"--cost") cost = true;
		else if (a == L"--refuse") refuse = true;
		else if (a == L"--motion") motion = true;
		else if (a == L"--bidir") bidir = true;
		else if (a == L"--twopass") twopass |= 1;
		else if (a == L"--twopass-mv") twopass |= 2;
		else if (a == L"--methods") { const std::string m = narrow(next().c_str()); methods = SplitList<std::string>(m, [](const std::string& s) { return s; }); }
		else if (a == L"--pad") opt.pad = _wtoi(next().c_str());
		else if (a == L"--dll") opt.dll = next();
		else if (a == L"--ref") opt.refs.push_back(next());
		else if (a == L"--domain") opt.domains = SplitList<std::string>(narrow(next().c_str()), [](const std::string& s) { return s; });
		else if (a == L"--preset") opt.presets = SplitList<unsigned>(narrow(next().c_str()), PresetFromText);
		else if (a == L"--grain") opt.grains = SplitList<float>(narrow(next().c_str()), [](const std::string& s) { return std::stof(s); });
		else if (a == L"--frames") opt.frames = _wtoi(next().c_str());
		else if (a == L"--from") opt.measureFrom = _wtoi(next().c_str());
		else if (a == L"--crop") { const std::wstring c = next(); swscanf_s(c.c_str(), L"%dx%d", &opt.cropW, &opt.cropH); }
		else if (a == L"--fp32") opt.fp32 = true;
		else if (a == L"-v") g_verbose = true;
		else { Usage(); return 1; }
	}
	if (!ramp && !synth && !converge && !cost && !refuse && !motion && !bidir && !twopass) {
		Usage();
		return 1;
	}
	if (opt.domains.empty()) opt.domains = { "hdr_lin" };
	if (opt.presets.empty()) opt.presets = { NGX_DLSS_PRESET_K };
	if (opt.grains.empty()) opt.grains = { 0.0035f, 0.010f };
	opt.measureFrom = std::clamp(opt.measureFrom, 1, std::max(opt.frames - 1, 1));

	dn::SetLogSink([](const wchar_t* line) {
		if (g_verbose || !wcsstr(line, L"NGX: ")) {
			printf("    | %S\n", line);
		}
	});

	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	Gpu g;
	if (!CreateDevice(g)) {
		printf("No NVIDIA Direct3D 11.1 device.\n");
		return 2;
	}
	printf("GPU: %S\n", g.name.c_str());

	int rc = 0;
	if (refuse) rc |= RunRefuse(g, opt);
	if (ramp) rc |= RunRamp(g, opt);
	if (cost) rc |= RunCost(g, opt);
	if (synth) rc |= RunSynth(g, opt, false);
	if (converge) rc |= RunSynth(g, opt, true);
	if (motion) rc |= RunMotion(opt, methods);
	if (bidir) rc |= RunBidir(g, opt);
	if (twopass & 1) rc |= RunTwoPass(opt, false);
	if (twopass & 2) rc |= RunTwoPass(opt, true);

	printf("\n%d failure(s)\n", g_failures);
	return rc ? rc : (g_failures ? 1 : 0);
}
