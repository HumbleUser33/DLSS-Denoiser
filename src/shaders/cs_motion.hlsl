// Motion vectors for DLSS from NVIDIA Optical Flow, as compute passes.
//
// Taken from MPCVR-Plus (Shaders/d3d11/ps_dlss_stabilize.hlsl, PASS 0, 4 and 3, (C) 2026
// its authors, GPL v3 or later), where they were measured in real time on Optical Flow
// run at about 540 lines with a 4x4 grid: a 4K film had a vector per 16 source pixels.
// Offline the flow may run at the picture's own size with a vector per pixel, and every
// window here that counted vector blocks or flow pixels would then shrink sixteenfold.
// So the windows keep the reach they were measured with, in source pixels: the mean
// shift samples its neighbours `stride` blocks apart, and the search samples its window
// every `sampleStep` flow pixels from a first step `firstStep` flow pixels wide.
//
//   PASS 0  flow frame: luma of the picture, PQ code values, box-filtered by a whole
//           factor of 1 to 4 and a reach beyond it, into the R8 texture Optical Flow reads
//   PASS 4  block motion, a texel per vector block: each block drawn to the picture's
//           global motion (cs_dlss_global_motion.hlsl) where it lies within the noise of
//           it, and what keeps its own vector steadied across the blocks that move alike,
//           searched again on the frames, and blended with the last picture's
//   PASS 3  PASS 4's blocks -> motion at the picture's size, RG32F, pixels, current to
//           previous

#ifndef PASS
#define PASS 3
#endif

static const float3 kLuma = float3(0.2627, 0.6780, 0.0593);   // BT.2020

#if PASS == 0

Texture2D<float4>   picture : register(t0);   // PQ code values
RWTexture2D<unorm float> frame : register(u0);

cbuffer CS_MOTION_FRAME : register(b0)
{
	float factor;   // picture pixels per flow pixel: 1 to 4
	float reach;    // picture pixels the box reaches beyond its flow pixel on each side: 0..4
	float2 reserved;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint fw, fh;
	frame.GetDimensions(fw, fh);
	if (id.x >= fw || id.y >= fh) {
		return;
	}
	uint w, h;
	picture.GetDimensions(w, h);
	const int2 last = int2(w, h) - 1;
	const int f = clamp((int)factor, 1, 4);
	const int r = clamp((int)reach, 0, 4);
	const int n = f + 2 * r;
	const int2 base = int2(id.xy) * f - r;
	float3 c = 0;
	[loop] for (int y = 0; y < n; y++) {
		[loop] for (int x = 0; x < n; x++) {
			c += picture.Load(int3(clamp(base + int2(x, y), int2(0, 0), last), 0)).rgb;
		}
	}
	c /= (float)(n * n);
	frame[id.xy] = dot(saturate(c), kLuma);
}

#elif PASS == 4

Texture2D<int4>   flowForward  : register(t0);   // R16G16_SINT, 1/32 flow pixel, current -> previous
Texture2D<int4>   flowBackward : register(t1);   // previous -> current
Texture2D<uint4>  costForward  : register(t2);   // R8_UINT, higher is worse
Texture2D<float4> globalMotion : register(t3);   // 1x1: flow pixels
Texture2D<float4> lastField    : register(t4);   // this pass's target for the last picture
Texture2D<float>  frameNow     : register(t5);   // the flow frame the engine just ran on
Texture2D<float>  frameLast    : register(t6);   // and the one before it
Texture2D<float4> tileMotion   : register(t7);   // cs_tiles.hlsl: a texel per tile, flow pixels
RWTexture2D<float4> field      : register(u0);

cbuffer CS_MOTION_BLOCK : register(b0)
{
	float2 flowScale;     // picture pixels per flow pixel, x and y
	float  grid;          // flow pixels per vector
	float  consistency;   // forward/backward mismatch (flow pixels) where confidence starts to fall
	float2 flowSize;      // flow frame size, pixels
	float  costLow;       // cost where confidence starts to fall
	float  costHigh;      // and where it reaches 0
	float  useBackward;   // 0 when the engine gave no backward flow
	float  useCost;       // 0 when it gave no cost
	float  snapLow;       // distance to the global motion, picture pixels, where a block starts to keep its vector
	float  snapHigh;      // and where it keeps it whole
	float  gate;          // unused: the mean shift decides
	float  deadZone;      // a global motion shorter than this, picture pixels, is no motion at all
	float  radius;        // window samples on each side of this block
	float  sigma;         // picture pixels: how alike two vectors must be to be averaged
	float  temporal;      // weight of this picture's vector against the last one's; 1: the last is not used
	float  history;       // 1 when lastField holds the last picture
	float  shift;         // mean shift steps
	float  supportLow;    // the share of the window's trust that must agree to begin keeping
	float  supportHigh;   // and to keep whole
	float  refine;        // halving search steps around a kept vector; 0: none
	float  reach;         // flow pixels on each side of the block centre the search compares
	float  textureMin;    // the least luma deviation over that window for a search to be trusted
	float  evidence;      // trusted blocks' worth that must share the motion to keep it whole
	float  stride;        // blocks between two window samples of the mean shift
	float  sampleStep;    // flow pixels between two samples of the search window
	float  firstStep;     // the search's first step, flow pixels
	float  tileSize;      // flow pixels per tile of tileMotion; 0: the global motion everywhere
	float3 reserved;
};

// The motion an untrusted block is drawn to: its tile's (cs_tiles.hlsl), a bilinear of the
// four tiles around it, or the picture's global motion.
float2 Background(float2 q)
{
	float2 result = globalMotion.Load(int3(0, 0, 0)).xy;
	if (tileSize > 0.0) {
		uint tw, th;
		tileMotion.GetDimensions(tw, th);
		const int2 last = int2(tw, th) - 1;
		const float2 f = q / tileSize - 0.5;
		const int2 i0 = (int2)floor(f);
		const float2 t = f - (float2)i0;
		const float2 a = tileMotion.Load(int3(clamp(i0, int2(0, 0), last), 0)).xy;
		const float2 b = tileMotion.Load(int3(clamp(i0 + int2(1, 0), int2(0, 0), last), 0)).xy;
		const float2 c = tileMotion.Load(int3(clamp(i0 + int2(0, 1), int2(0, 0), last), 0)).xy;
		const float2 d = tileMotion.Load(int3(clamp(i0 + int2(1, 1), int2(0, 0), last), 0)).xy;
		result = lerp(lerp(a, b, t.x), lerp(c, d, t.x), t.y);
	}
	return result;
}

bool Outside(float2 q)
{
	return any(q < 0.0) || any(q > flowSize);
}

// The confidence at a block, 0..1; none for a vector that leaves the picture.
float Confidence(int2 b, float2 v, int2 last)
{
	const float2 q = ((float2)b + 0.5) * grid + v;
	if (Outside(q)) {
		return 0.0;
	}
	float confidence = 1.0;
	if (useBackward > 0.5) {
		const int2 bb = clamp((int2)floor(q / grid), int2(0, 0), last);
		const float2 back = (float2)flowBackward.Load(int3(bb, 0)).xy / 32.0;
		confidence *= 1.0 - smoothstep(consistency, 2.0 * consistency, length(v + back));
	}
	if (useCost > 0.5) {
		confidence *= 1.0 - smoothstep(costLow, costHigh, (float)costForward.Load(int3(b, 0)).x);
	}
	return confidence;
}

// Bilinear of the last flow frame at a position in pixels (centres at +0.5), from Loads.
float LastAt(float2 pos, int2 lastPixel)
{
	const float2 f = pos - 0.5;
	const int2 i0 = (int2)floor(f);
	const float2 t = f - (float2)i0;
	const float a = frameLast.Load(int3(clamp(i0, int2(0, 0), lastPixel), 0));
	const float b = frameLast.Load(int3(clamp(i0 + int2(1, 0), int2(0, 0), lastPixel), 0));
	const float c = frameLast.Load(int3(clamp(i0 + int2(0, 1), int2(0, 0), lastPixel), 0));
	const float d = frameLast.Load(int3(clamp(i0 + int2(1, 1), int2(0, 0), lastPixel), 0));
	return lerp(lerp(a, b, t.x), lerp(c, d, t.x), t.y);
}

// How badly the window around a block centre matches the last frame moved by v: count
// samples a side, step flow pixels apart.
float Mismatch(float2 origin, int count, float step, float2 v, int2 lastPixel)
{
	float sum = 0.0;
	for (int y = 0; y < count; y++) {
		for (int x = 0; x < count; x++) {
			const int2 p = clamp((int2)(origin + float2(x, y) * step), int2(0, 0), lastPixel);
			const float e = frameNow.Load(int3(p, 0)) - LastAt((float2)p + 0.5 + v, lastPixel);
			sum += e * e;
		}
	}
	return sum;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint gw, gh;
	flowForward.GetDimensions(gw, gh);
	if (id.x >= gw || id.y >= gh) {
		return;
	}
	const int2 last = int2(gw, gh) - 1;
	const int2 b = int2(id.xy);
	float2 g = Background(((float2)b + 0.5) * grid);
	if (length(g * flowScale) < deadZone) {
		g = 0.0;
	}

	const float2 v = (float2)flowForward.Load(int3(b, 0)).xy / 32.0;
	if (shift < 0.5) {
		// No cleaning: Optical Flow's own vector, kept whole (a measure of the engine).
		field[b] = float4(v, 1.0, 1.0);
		return;
	}
	float keep = 0.0;
	float2 own = v;
	{
		// The window's vectors and their trust, once.
		const int r = min((int)radius, 3);
		const int s = max((int)stride, 1);
		const int count = (2 * r + 1) * (2 * r + 1);
		float2 vectors[49];
		float trust[49];
		float totalTrust = 0.0;
		int k = 0;
		for (int j = -r; j <= r; j++) {
			for (int i = -r; i <= r; i++) {
				// Beyond the grid there is no block: a clamped window would count the edge's
				// own blocks several times over, and they are the engine's worst.
				const int2 n = b + int2(i, j) * s;
				const bool inside = all(n >= int2(0, 0)) && all(n <= last);
				vectors[k] = inside ? (float2)flowForward.Load(int3(n, 0)).xy / 32.0 : float2(0.0, 0.0);
				trust[k] = inside ? Confidence(n, vectors[k], last) : 0.0;
				totalTrust += trust[k];
				k++;
			}
		}
		// From the block's own vector to the motion the trusted blocks around share.
		float weights = 0.0;
		for (int step = 0; step < (int)shift; step++) {
			float2 sum = 0.0;
			weights = 0.0;
			for (int m = 0; m < count; m++) {
				const float d = length((vectors[m] - own) * flowScale) / sigma;
				const float w = trust[m] * exp(-d * d);
				sum += w * vectors[m];
				weights += w;
			}
			if (weights > 1e-3) {
				own = sum / weights;
			}
		}
		// Kept if it is not the global motion and enough of the window's trust shares it,
		// with evidence blocks' worth of it at least.
		const float2 centre = ((float2)b + 0.5) * grid;
		if (!(Outside(centre + own) && !Outside(centre + g))) {
			keep = smoothstep(snapLow, snapHigh, length((own - g) * flowScale))
				* smoothstep(supportLow, supportHigh, weights / max(totalTrust, 1e-4))
				* smoothstep(0.5 * evidence, evidence, weights);
		}
	}
	if (keep > 0.0 && refine > 0.5) {
		// The kept vector searched again on the frames themselves, where they have texture.
		uint fw, fh;
		frameNow.GetDimensions(fw, fh);
		const int2 lastPixel = int2(fw, fh) - 1;
		const float sampleAt = max(sampleStep, 1.0);
		const int samples = max((int)round(2.0 * reach / sampleAt), 2);
		const float2 origin = ((float2)b + 0.5) * grid - reach;
		float mean = 0.0, square = 0.0;
		for (int y = 0; y < samples; y++) {
			for (int x = 0; x < samples; x++) {
				const int2 p = clamp((int2)(origin + float2(x, y) * sampleAt), int2(0, 0), lastPixel);
				const float a = frameNow.Load(int3(p, 0));
				mean += a;
				square += a * a;
			}
		}
		const float n = (float)(samples * samples);
		mean /= n;
		if (sqrt(max(square / n - mean * mean, 0.0)) >= textureMin) {
			float2 best = own;
			float bestCost = Mismatch(origin, samples, sampleAt, best, lastPixel);
			float step = firstStep;
			for (int level = 0; level < (int)refine; level++) {
				const float2 centre = best;
				for (int j = -1; j <= 1; j++) {
					for (int i = -1; i <= 1; i++) {
						if (i == 0 && j == 0) {
							continue;
						}
						const float2 candidate = centre + float2(i, j) * step;
						const float cost = Mismatch(origin, samples, sampleAt, candidate, lastPixel);
						if (cost < bestCost) {
							best = candidate;
							bestCost = cost;
						}
					}
				}
				step *= 0.5;
			}
			own = best;
		}
	}
	if (keep > 0.0 && history > 0.5 && temporal < 1.0) {
		// Where this content was one picture earlier, if its vector agrees.
		const float2 q = ((float2)b + 0.5) * grid + own;
		const int2 bp = clamp((int2)floor(q / grid), int2(0, 0), last);
		const float4 previous = lastField.Load(int3(bp, 0));
		if (previous.z > 0.5 && length((previous.xy - own) * flowScale) < snapHigh) {
			own = lerp(previous.xy, own, temporal);
		}
	}
	field[b] = float4(own, keep, 1.0);
}

#elif PASS == 3

Texture2D<float4> blocks       : register(t0);   // PASS 4
Texture2D<float4> globalMotion : register(t1);   // 1x1: flow pixels
Texture2D<float4> tileMotion   : register(t2);   // cs_tiles.hlsl, as PASS 4
RWTexture2D<float2> motion     : register(u0);   // RG32F, picture pixels, current -> previous

cbuffer CS_MOTION_EXPAND : register(b0)
{
	float2 flowScale;     // picture pixels per flow pixel, x and y
	float  grid;          // flow pixels per vector
	float  deadZone;      // as PASS 4
	float  useBlocks;     // 0: the global motion everywhere
	float  tileSize;      // as PASS 4
	float2 reserved;
};

float2 Background(float2 q)
{
	float2 result = globalMotion.Load(int3(0, 0, 0)).xy;
	if (tileSize > 0.0) {
		uint tw, th;
		tileMotion.GetDimensions(tw, th);
		const int2 last = int2(tw, th) - 1;
		const float2 f = q / tileSize - 0.5;
		const int2 i0 = (int2)floor(f);
		const float2 t = f - (float2)i0;
		const float2 a = tileMotion.Load(int3(clamp(i0, int2(0, 0), last), 0)).xy;
		const float2 b = tileMotion.Load(int3(clamp(i0 + int2(1, 0), int2(0, 0), last), 0)).xy;
		const float2 c = tileMotion.Load(int3(clamp(i0 + int2(0, 1), int2(0, 0), last), 0)).xy;
		const float2 d = tileMotion.Load(int3(clamp(i0 + int2(1, 1), int2(0, 0), last), 0)).xy;
		result = lerp(lerp(a, b, t.x), lerp(c, d, t.x), t.y);
	}
	return result;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint w, h;
	motion.GetDimensions(w, h);
	if (id.x >= w || id.y >= h) {
		return;
	}
	uint gw, gh;
	blocks.GetDimensions(gw, gh);
	const int2 last = int2(gw, gh) - 1;
	const float2 q0 = ((float2)id.xy + 0.5) / flowScale;
	float2 f = Background(q0);
	if (useBlocks > 0.5) {
		const float2 q = ((float2)id.xy + 0.5) / flowScale;
		const float2 bf = q / grid - 0.5;
		const int2 b0 = (int2)floor(bf);
		const float2 t = bf - (float2)b0;
		f = 0;
		for (int j = 0; j < 2; j++) {
			for (int i = 0; i < 2; i++) {
				const int2 b = clamp(b0 + int2(i, j), int2(0, 0), last);
				const float weight = (i ? t.x : 1.0 - t.x) * (j ? t.y : 1.0 - t.y);
				const float4 block = blocks.Load(int3(b, 0));
				f += weight * lerp(Background(((float2)b + 0.5) * grid), block.xy, block.z);
			}
		}
	}
	motion[id.xy] = f * flowScale;
}

#endif
