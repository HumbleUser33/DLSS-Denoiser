/*
 * This file is part of DLSS Denoiser.
 *
 * DLSS Denoiser is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version. See LICENSE.txt.
 */

// vsdlss: DLSS Super Resolution in DLAA mode as a VapourSynth filter, to take the grain
// out of a film before it is encoded.
//
//   core.dlss.Denoise(clip clip[, clip ref, string preset="J", int linear=1, float exposure=1.0,
//                     int auto_exposure=0, int pad=32, int motion=1, int flow_factor=1, int grid=1,
//                     int perf=0, int flow_blur=1, int scenecut=3, int preroll=24, int passes=2,
//                     int chunk=24, int lookahead=24, string dll="", int device=0])
//
// clip: RGBS or RGBH, PQ code values (dlssdenoise.py converts from YUV and back).
// ref: the same format and size, the motion is measured on it instead (a copy with the
// grain taken out). scenecut: 1 the GPU's cut detection, 2 the _SceneChangePrev property,
// 3 both, 0 none.
//
// DLSS keeps a history: frames go through in order. Requests arrive several at a time and
// finish in any order (fmUnordered, vspipe's in-flight requests), so a request for a frame
// ahead processes every frame up to it, and the outputs of those in between wait for their
// own requests. A request that does not follow -- a seek in a preview -- starts the history
// over `preroll` frames before it.
//
// passes=2 runs the film backward as well, a chunk at a time, and gives out the mean of the
// two weighted by the history each held: no grain coming back after a cut, and less of it
// everywhere (bench --bidir, --twopass). It costs about three passes' work.

#include "Engine.h"
#include <map>
#include <memory>
#include "VapourSynth4.h"
#include "VSHelper4.h"

namespace {

const VSAPI* g_vsapi = nullptr;
VSCore* g_logCore = nullptr;
std::mutex g_logCoreLock;

void LogToVapourSynth(const wchar_t* line)
{
	std::lock_guard lock(g_logCoreLock);
	if (g_vsapi && g_logCore) {
		std::string s;
		for (const wchar_t* p = line; *p; p++) {
			s += (*p < 128) ? (char)*p : '?';
		}
		g_vsapi->logMessage(mtDebug, ("dlss: " + s).c_str(), g_logCore);
	}
}

std::string Narrow(const std::wstring& w)
{
	std::string s;
	for (wchar_t c : w) {
		s += (c < 128) ? (char)c : '?';
	}
	return s;
}

struct Instance {
	VSNode* node = nullptr;
	VSNode* ref = nullptr;
	VSVideoInfo vi = {};

	CEngine::Config config;
	std::wstring dll;
	int device = 0;
	int scenecut = 3;
	int preroll = 24;
	int window = 64;           // how far ahead a request may lie and still follow
	int inflight = 16;         // the core's threads: requests vspipe keeps in flight
	int passes = 2;            // 2: forward and backward, weighted by the history each holds
	int chunk = 24;            // frames the backward lane gives out per run
	int lookahead = 24;        // and the frames past them it starts from, for a history
	int debug = 0;             // 1: the motion vectors DLSS was given instead of the picture: R x, G y, picture pixels

	std::unique_ptr<CEngine> engine;
	bool engineFailed = false;
	std::string engineError;

	int next = 0;              // the frame the history is ready for
	bool fresh = true;         // nothing processed since the last reset
	std::map<int, const VSFrame*> pending;   // processed ahead, waiting for their own request

	~Instance()
	{
		for (auto& [n, frame] : pending) {
			g_vsapi->freeFrame(frame);
		}
		engine.reset();
		g_vsapi->freeNode(node);
		g_vsapi->freeNode(ref);
	}
};

bool EnsureEngine(Instance* d)
{
	if (d->engine) {
		return true;
	}
	if (d->engineFailed) {
		return false;
	}
	auto engine = std::make_unique<CEngine>();
	if (!engine->Init(d->device, d->dll.empty() ? nullptr : d->dll.c_str()) || !engine->Configure(d->config)) {
		d->engineFailed = true;
		d->engineError = "dlss.Denoise: " + Narrow(engine->GetError());
		return false;
	}
	d->engine = std::move(engine);
	return true;
}

// The frame starts a shot: _SceneChangePrev, when the property is to be read.
bool CutBefore(const Instance* d, const VSFrame* frame)
{
	if (!(d->scenecut & 2) || !frame) {
		return false;
	}
	int err = 0;
	const int64_t cut = g_vsapi->mapGetInt(g_vsapi->getFramePropertiesRO(frame), "_SceneChangePrev", 0, &err);
	return !err && cut;
}

// One frame through one lane of the engine, into out.
bool RunLane(Instance* d, int lane, const VSFrame* src, const VSFrame* ref, bool reset, const CEngine::OutPlanes& out, std::string& error)
{
	CEngine::Planes in, rp;
	for (int p = 0; p < 3; p++) {
		in.data[p] = g_vsapi->getReadPtr(src, p);
		in.stride[p] = g_vsapi->getStride(src, p);
		if (ref) {
			rp.data[p] = g_vsapi->getReadPtr(ref, p);
			rp.stride[p] = g_vsapi->getStride(ref, p);
		}
	}
	if (!d->engine->Process(in, ref ? &rp : nullptr, nullptr, reset, out, lane)) {
		error = "dlss.Denoise: " + Narrow(d->engine->GetError());
		return false;
	}
	return true;
}

CEngine::OutPlanes FramePlanes(VSFrame* frame)
{
	CEngine::OutPlanes out;
	for (int p = 0; p < 3; p++) {
		out.data[p] = g_vsapi->getWritePtr(frame, p);
		out.stride[p] = g_vsapi->getStride(frame, p);
	}
	return out;
}

// A picture's three planes in memory of its own.
struct Buffer {
	std::vector<uint8_t> bytes;
	CEngine::OutPlanes planes;
	void Allocate(int width, int height, int sampleBytes)
	{
		const size_t plane = (size_t)width * height * sampleBytes;
		bytes.resize(plane * 3);
		for (int p = 0; p < 3; p++) {
			planes.data[p] = bytes.data() + plane * p;
			planes.stride[p] = (ptrdiff_t)width * sampleBytes;
		}
	}
};

const VSFrame* ProcessOne(Instance* d, int k, VSFrameContext* frameCtx, VSCore* core, bool reset, std::string& error)
{
	const VSFrame* src = g_vsapi->getFrameFilter(k, d->node, frameCtx);
	const VSFrame* ref = d->ref ? g_vsapi->getFrameFilter(k, d->ref, frameCtx) : nullptr;
	if (!src) {
		error = "dlss.Denoise: a source frame is missing";
		return nullptr;
	}
	reset = reset || CutBefore(d, src);
	VSFrame* dst = g_vsapi->newVideoFrame(&d->vi.format, d->vi.width, d->vi.height, src, core);
	bool ok = RunLane(d, 0, src, ref, reset, FramePlanes(dst), error);
	if (ok && d->debug == 1) {
		// The vectors themselves, for looking at: R and G in picture pixels, B zero.
		std::vector<float> rg;
		if (d->engine->ReadMotion(rg)) {
			for (int y = 0; y < d->vi.height; y++) {
				float* r = (float*)(g_vsapi->getWritePtr(dst, 0) + y * g_vsapi->getStride(dst, 0));
				float* g = (float*)(g_vsapi->getWritePtr(dst, 1) + y * g_vsapi->getStride(dst, 1));
				float* b = (float*)(g_vsapi->getWritePtr(dst, 2) + y * g_vsapi->getStride(dst, 2));
				for (int x = 0; x < d->vi.width; x++) {
					r[x] = rg[((size_t)y * d->vi.width + x) * 2];
					g[x] = rg[((size_t)y * d->vi.width + x) * 2 + 1];
					b[x] = 0.0f;
				}
			}
		}
	}
	if (ok) {
		VSMap* props = g_vsapi->getFramePropertiesRW(dst);
		g_vsapi->mapSetInt(props, "DLSSDenoiseCut", d->engine->WasCut() || reset ? 1 : 0, maReplace);
	}
	g_vsapi->freeFrame(src);
	g_vsapi->freeFrame(ref);
	if (!ok) {
		g_vsapi->freeFrame(dst);
		return nullptr;
	}
	return dst;
}

void RequestRange(Instance* d, int start, int n, VSFrameContext* frameCtx);

void KeepPending(Instance* d, int k, const VSFrame* frame)
{
	auto& slot = d->pending[k];
	if (slot) {
		g_vsapi->freeFrame(slot);
	}
	slot = frame;
}

// Outputs whose requests never came: a stopped encode, a preview that moved on.
void EvictPending(Instance* d)
{
	const int keep = std::max(2 * d->window, 2 * d->chunk);
	while (!d->pending.empty() && (d->pending.begin()->first < d->next - keep || (int)d->pending.size() > keep)) {
		g_vsapi->freeFrame(d->pending.begin()->second);
		d->pending.erase(d->pending.begin());
	}
}

// Two passes. Frames go out a chunk at a time: the backward lane runs from `lookahead`
// frames past the chunk down to its first frame, starting over there, then the forward
// lane takes the chunk on from where it stands, and each frame is the mean of the two
// weighted by the history each held (CEngine::Weight). After a cut the forward lane
// starts over with all its grain, and the backward lane, which has the shot behind it,
// carries the frame; before a cut the other way round (bench --twopass: a pan cut in two,
// heavy grain, 3.26 thousandths of grain left by one pass, 1.96 by the two).
const VSFrame* GetFrameTwoPass(int n, int activationReason, Instance* d, void** frameData,
	VSFrameContext* frameCtx, VSCore* core)
{
	const auto it = d->pending.find(n);
	if (it != d->pending.end()) {
		const VSFrame* frame = it->second;
		d->pending.erase(it);
		return frame;
	}
	const int total = d->vi.numFrames;
	const int c0 = n / d->chunk * d->chunk, c1 = std::min(c0 + d->chunk, total), end = std::min(c1 + d->lookahead, total);

	if (activationReason == arInitial) {
		const bool follows = !d->fresh && d->next == c0;
		const int start = follows ? c0 : std::max(0, c0 - d->preroll);
		frameData[0] = (void*)(intptr_t)(start + 1);   // + 1: zero is "no data"
		RequestRange(d, start, end - 1, frameCtx);
		return nullptr;
	}
	if (activationReason != arAllFramesReady) {
		return nullptr;
	}
	if (!EnsureEngine(d)) {
		g_vsapi->setFilterError(d->engineError.c_str(), frameCtx);
		return nullptr;
	}
	const int start = (int)(intptr_t)frameData[0] - 1;
	const bool follows = !d->fresh && d->next == c0;
	const int from = follows ? c0 : start;
	const int sampleBytes = d->vi.format.bytesPerSample;
	std::string error;
	auto fail = [&]() -> const VSFrame* {
		g_vsapi->setFilterError(error.c_str(), frameCtx);
		d->fresh = true;
		return nullptr;
	};

	// Backward, from the end of the look-ahead down to the chunk's first frame.
	std::vector<Buffer> back(c1 - c0);
	std::vector<int> backHistory(c1 - c0);
	Buffer scratch;
	scratch.Allocate(d->vi.width, d->vi.height, sampleBytes);
	bool cutAfter = false;   // the frame after this one starts a shot
	for (int k = end - 1; k >= c0; k--) {
		const VSFrame* src = g_vsapi->getFrameFilter(k, d->node, frameCtx);
		const VSFrame* ref = d->ref ? g_vsapi->getFrameFilter(k, d->ref, frameCtx) : nullptr;
		if (!src) {
			error = "dlss.Denoise: a source frame is missing";
			return fail();
		}
		Buffer* out = &scratch;
		if (k < c1) {
			back[k - c0].Allocate(d->vi.width, d->vi.height, sampleBytes);
			out = &back[k - c0];
		}
		const bool ok = RunLane(d, 1, src, ref, k == end - 1 || cutAfter, out->planes, error);
		cutAfter = CutBefore(d, src);
		g_vsapi->freeFrame(src);
		g_vsapi->freeFrame(ref);
		if (!ok) {
			return fail();
		}
		if (k < c1) {
			backHistory[k - c0] = d->engine->GetHistory(1);
		}
	}

	// Forward, from where the lane stands (or the preroll after a seek) to the chunk's end.
	const VSFrame* result = nullptr;
	for (int k = from; k < c1; k++) {
		const VSFrame* src = g_vsapi->getFrameFilter(k, d->node, frameCtx);
		const VSFrame* ref = d->ref ? g_vsapi->getFrameFilter(k, d->ref, frameCtx) : nullptr;
		if (!src) {
			error = "dlss.Denoise: a source frame is missing";
			return fail();
		}
		const bool reset = (k == from && !follows) || CutBefore(d, src);
		VSFrame* dst = k >= c0 ? g_vsapi->newVideoFrame(&d->vi.format, d->vi.width, d->vi.height, src, core) : nullptr;
		const CEngine::OutPlanes out = dst ? FramePlanes(dst) : scratch.planes;
		const bool ok = RunLane(d, 0, src, ref, reset, out, error);
		g_vsapi->freeFrame(src);
		g_vsapi->freeFrame(ref);
		if (!ok) {
			g_vsapi->freeFrame(dst);
			return fail();
		}
		if (!dst) {
			continue;   // a preroll frame: nobody asked for it
		}
		const int fw = d->engine->GetHistory(0), bw = backHistory[k - c0];
		CEngine::Blend(out, CEngine::Weight(fw), back[k - c0].planes, CEngine::Weight(bw),
			d->vi.width, d->vi.height, sampleBytes == 2);
		VSMap* props = g_vsapi->getFramePropertiesRW(dst);
		g_vsapi->mapSetInt(props, "DLSSDenoiseCut", fw == 0 ? 1 : 0, maReplace);
		g_vsapi->mapSetInt(props, "DLSSDenoiseHistory", fw, maReplace);
		g_vsapi->mapSetInt(props, "DLSSDenoiseHistory", bw, maAppend);
		if (k == n) {
			result = dst;
		} else {
			KeepPending(d, k, dst);   // its own request is on its way, or will be
		}
	}
	d->next = c1;
	d->fresh = false;
	EvictPending(d);
	return result;
}
void RequestRange(Instance* d, int start, int n, VSFrameContext* frameCtx)
{
	for (int k = start; k <= n; k++) {
		g_vsapi->requestFrameFilter(k, d->node, frameCtx);
		if (d->ref) {
			g_vsapi->requestFrameFilter(k, d->ref, frameCtx);
		}
	}
}

const VSFrame* VS_CC DenoiseGetFrame(int n, int activationReason, void* instanceData, void** frameData,
	VSFrameContext* frameCtx, VSCore* core, const VSAPI* vsapi)
{
	Instance* d = (Instance*)instanceData;
	if (d->passes >= 2) {
		return GetFrameTwoPass(n, activationReason, d, frameData, frameCtx, core);
	}

	if (activationReason == arInitial) {
		// Processed already, ahead of its own request.
		const auto it = d->pending.find(n);
		if (it != d->pending.end()) {
			const VSFrame* frame = it->second;
			d->pending.erase(it);
			return frame;
		}
		const bool follows = n >= d->next && n - d->next <= d->window && !d->fresh;
		const int start = follows ? d->next : std::max(0, n - d->preroll);
		frameData[0] = (void*)(intptr_t)(start + 1);   // + 1: zero is "no data"
		RequestRange(d, start, n, frameCtx);
		return nullptr;
	}

	if (activationReason == arAllFramesReady) {
		const auto it = d->pending.find(n);
		if (it != d->pending.end()) {
			const VSFrame* frame = it->second;
			d->pending.erase(it);
			return frame;
		}
		if (!EnsureEngine(d)) {
			vsapi->setFilterError(d->engineError.c_str(), frameCtx);
			return nullptr;
		}
		const int start = (int)(intptr_t)frameData[0] - 1;
		// It follows what was processed, and this request holds every frame from there; or
		// it does not -- a seek, or a request another overtook -- and the history starts over
		// at the first frame this request holds.
		const bool follows = !d->fresh && d->next >= start && d->next <= n;
		const int from = follows ? d->next : start;
		const VSFrame* result = nullptr;
		std::string error;
		for (int k = from; k <= n; k++) {
			const VSFrame* out = ProcessOne(d, k, frameCtx, core, !follows && k == from, error);
			if (!out) {
				vsapi->setFilterError(error.c_str(), frameCtx);
				d->fresh = true;
				return nullptr;
			}
			if (k == n) {
				result = out;
			} else if (follows || k >= n - d->inflight) {
				// A request of its own may be on its way: vspipe asks for as many frames at
				// once as the core has threads, and they finish in any order.
				KeepPending(d, k, out);
			} else {
				vsapi->freeFrame(out);   // a preroll frame: nobody asked for it
			}
		}
		// The history now stands at this frame, whatever came before.
		d->next = n + 1;
		d->fresh = false;
		EvictPending(d);
		return result;
	}
	return nullptr;
}

void VS_CC DenoiseFree(void* instanceData, VSCore* core, const VSAPI* vsapi)
{
	delete (Instance*)instanceData;
}

unsigned PresetFromString(const char* s, bool& ok)
{
	ok = true;
	if (!s || !*s) {
		return NGX_DLSS_PRESET_J;
	}
	const char c = (char)toupper((unsigned char)s[0]);
	if (!s[1]) {
		switch (c) {
		case 'J': return NGX_DLSS_PRESET_J;
		case 'K': return NGX_DLSS_PRESET_K;
		case 'L': return NGX_DLSS_PRESET_L;
		case 'M': return NGX_DLSS_PRESET_M;
		case 'E': return NGX_DLSS_PRESET_E;
		case 'F': return NGX_DLSS_PRESET_F;
		}
	}
	ok = false;
	return 0;
}

void VS_CC DenoiseCreate(const VSMap* in, VSMap* out, void* userData, VSCore* core, const VSAPI* vsapi)
{
	g_vsapi = vsapi;
	auto d = std::make_unique<Instance>();
	int err = 0;
	auto fail = [&](const std::string& msg) {
		vsapi->mapSetError(out, ("dlss.Denoise: " + msg).c_str());
	};

	d->node = vsapi->mapGetNode(in, "clip", 0, nullptr);
	d->vi = *vsapi->getVideoInfo(d->node);
	const VSVideoFormat& f = d->vi.format;
	if (!vsh::isConstantVideoFormat(&d->vi) || f.colorFamily != cfRGB || f.sampleType != stFloat
			|| (f.bitsPerSample != 32 && f.bitsPerSample != 16)) {
		return fail("the clip must be RGBS or RGBH of constant size: PQ code values (use dlssdenoise.py)");
	}
	d->ref = vsapi->mapGetNode(in, "ref", 0, &err);
	if (d->ref) {
		const VSVideoInfo* rvi = vsapi->getVideoInfo(d->ref);
		if (!vsh::isSameVideoInfo(rvi, &d->vi) || rvi->numFrames < d->vi.numFrames) {
			return fail("ref must have the clip's format, size and length");
		}
	}

	bool presetOk = true;
	d->config.preset = PresetFromString(vsapi->mapGetData(in, "preset", 0, &err), presetOk);
	if (!presetOk) {
		return fail("preset must be one of J, K, L, M (E and F are deprecated)");
	}
	d->config.width = d->vi.width;
	d->config.height = d->vi.height;
	d->config.half = f.bitsPerSample == 16;
	auto intArg = [&](const char* name, int64_t def) { int e = 0; const int64_t v = vsapi->mapGetInt(in, name, 0, &e); return e ? def : v; };
	auto floatArg = [&](const char* name, double def) { int e = 0; const double v = vsapi->mapGetFloat(in, name, 0, &e); return e ? def : v; };
	d->config.linear = intArg("linear", 1) != 0;
	d->config.exposure = (float)floatArg("exposure", 1.0);
	d->config.autoExposure = intArg("auto_exposure", 0) != 0;
	d->config.pad = (UINT)std::clamp<int64_t>(intArg("pad", 32), 0, 256);
	d->config.motion = intArg("motion", 1) != 0;
	d->config.motionSettings.flowFactor = (UINT)std::clamp<int64_t>(intArg("flow_factor", 1), 1, 4);
	d->config.motionSettings.gridSize = (UINT)intArg("grid", 1);
	const int64_t perf = intArg("perf", 0);
	d->config.motionSettings.perfLevel = perf <= 0 ? NV_OF_PERF_LEVEL_SLOW : perf == 1 ? NV_OF_PERF_LEVEL_MEDIUM : NV_OF_PERF_LEVEL_FAST;
	d->config.motionSettings.flowBlur = (UINT)std::clamp<int64_t>(intArg("flow_blur", 1), 0, 4);
	// tiles: an untrusted block takes its tile's motion, the global motion refined there, and
	// not the whole picture's (parallax: trees behind a face the camera follows).
	// 1: tiles of 64 flow pixels; 16 to 512: that size.
	const int64_t tiles = intArg("tiles", 1);
	d->config.motionSettings.tiles = tiles != 0;
	if (tiles >= 16) {
		d->config.motionSettings.tileSize = (UINT)std::min<int64_t>(tiles, 512);
	}
	// vectors: "clean" draws the blocks to the picture's global motion unless they move on
	// their own (MPCVR-Plus's cleaning); "raw" gives DLSS Optical Flow's own vectors, as
	// measured (best on a prefiltered ref); "global" the global motion everywhere.
	if (const char* vectors = vsapi->mapGetData(in, "vectors", 0, &err); vectors && *vectors) {
		const std::string v = vectors;
		if (v == "raw") {
			d->config.motionSettings.vectorShift = 0;
		} else if (v == "global") {
			d->config.motionSettings.globalOnly = true;
		} else if (v != "clean") {
			return fail("vectors must be clean, raw or global");
		}
	}
	d->scenecut = (int)std::clamp<int64_t>(intArg("scenecut", 3), 0, 3);
	d->config.motionSettings.cutDetect = (d->scenecut & 1) != 0;
	d->preroll = (int)std::clamp<int64_t>(intArg("preroll", 24), 0, 1000);
	d->passes = (int)std::clamp<int64_t>(intArg("passes", 2), 1, 2);
	d->chunk = (int)std::clamp<int64_t>(intArg("chunk", 24), 1, 1000);
	d->lookahead = (int)std::clamp<int64_t>(intArg("lookahead", 24), 0, 1000);
	d->config.passes = d->passes;
	d->debug = (int)std::clamp<int64_t>(intArg("debug", 0), 0, 1);
	if (d->debug) {
		if (d->vi.format.bitsPerSample != 32) {
			return fail("debug needs RGBS");
		}
		d->passes = 1;
		d->config.passes = 1;
	}
	d->device = (int)intArg("device", 0);
	if (const char* dll = vsapi->mapGetData(in, "dll", 0, &err); dll && *dll) {
		const int length = MultiByteToWideChar(CP_UTF8, 0, dll, -1, nullptr, 0);
		d->dll.resize(length > 0 ? length - 1 : 0);
		MultiByteToWideChar(CP_UTF8, 0, dll, -1, d->dll.data(), length);
	}
	if (d->config.motionSettings.gridSize != 1 && d->config.motionSettings.gridSize != 2 && d->config.motionSettings.gridSize != 4) {
		return fail("grid must be 1, 2 or 4");
	}
	if (d->config.pad * 2 >= (UINT)std::min(d->vi.width, d->vi.height)) {
		return fail("pad is too large for the picture");
	}

	{
		std::lock_guard lock(g_logCoreLock);
		g_logCore = core;
	}
	dn::SetLogSink(LogToVapourSynth);

	std::vector<VSFilterDependency> deps = { { d->node, rpGeneral } };
	if (d->ref) {
		deps.push_back({ d->ref, rpGeneral });
	}
	Instance* raw = d.release();
	VSNode* node = vsapi->createVideoFilter2("Denoise", &raw->vi, DenoiseGetFrame, DenoiseFree, fmUnordered,
		deps.data(), (int)deps.size(), raw, core);
	if (!node) {
		delete raw;
		return fail("cannot create the filter");
	}
	VSCoreInfo info = {};
	vsapi->getCoreInfo(core, &info);
	raw->inflight = std::max(info.numThreads, 1);
	raw->window = std::max(raw->inflight * 2 + 16, 32);
	vsapi->mapConsumeNode(out, "clip", node, maReplace);
}

} // namespace

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin* plugin, const VSPLUGINAPI* vspapi)
{
	vspapi->configPlugin("com.dlssdenoise.dlss", "dlss", "DLSS Super Resolution in DLAA mode as a degrainer",
		VS_MAKE_VERSION(0, 1), VAPOURSYNTH_API_VERSION, 0, plugin);
	vspapi->registerFunction("Denoise",
		"clip:vnode;ref:vnode:opt;preset:data:opt;linear:int:opt;exposure:float:opt;auto_exposure:int:opt;"
		"pad:int:opt;motion:int:opt;flow_factor:int:opt;grid:int:opt;perf:int:opt;flow_blur:int:opt;"
		"scenecut:int:opt;preroll:int:opt;passes:int:opt;chunk:int:opt;lookahead:int:opt;vectors:data:opt;tiles:int:opt;debug:int:opt;dll:data:opt;device:int:opt;",
		"clip:vnode;", DenoiseCreate, nullptr, plugin);
}
