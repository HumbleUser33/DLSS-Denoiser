// The picture's motion refined tile by tile: what the cleaning draws an untrusted block to.
//
// The cleaning taken from MPCVR-Plus draws every block it does not trust to the picture's
// global motion. On The Uprising's face shot the camera moves 5 to 11 pixels a frame, the
// face is what Optical Flow trusts, and the trees behind move 1 to 2 pixels a frame
// otherwise (parallax): given the face's motion, DLSS carried their history that far off on
// every frame, and drew them 2.7 pixels and more from where they were, blurred. Here each
// tile of 64 x 64 flow pixels starts from the global motion and takes Gauss-Newton steps on
// the two flow frames, blurred against the grain -- first heavily, to follow what is out of
// focus, then lightly.
//
// Steps taken on grain alone wander: where the picture is flat (a clear sky) the tiles strayed
// pixels from the true motion, to wherever the grain of the two frames happened to line up
// best (dlssdn_bench --motion). So the last dispatch weighs the texture the tile has, on the
// heavily blurred frames, against the grain's energy in the unblurred one: the smaller
// eigenvalue of its gradient matrix per unit of that energy. A flat tile is drawn back to the
// global motion, and so is one that matches the frames no better than the global motion does.
// A film's grain is clumped and makes a sky look as textured as out-of-focus trees: that much
// is left to the median of the tiles around (MEDIAN below).
//
//   one dispatch per step, a group per tile: the sums of g g^T and g e over the tile, g the
//   previous frame's gradient where the content came from, e the difference to it; the 2 x 2
//   system solved and the step taken, at most `maxStep`
//   then one with `verify`: the tile's motion kept, drawn back to the global motion, or in
//   between, by that measure

#ifndef MEDIAN

Texture2D<float4>   frameNow   : register(t0);   // the flow frame the engine just ran on, blurred: value, gradient (cs_blur.hlsl)
Texture2D<float4>   frameLast  : register(t1);   // and the one before it, blurred the same
Texture2D<float4>   estimate   : register(t2);   // a texel per tile: xy flow pixels, current -> previous
Texture2D<float4>   globalMotion : register(t3); // 1x1, flow pixels: the start
Texture2D<float>    frameRaw   : register(t4);   // verify: the flow frame unblurred
Texture2D<float>    frameFine  : register(t5);   // and lightly blurred (box): their difference is mostly grain
RWTexture2D<float4> result     : register(u0);

cbuffer CS_TILES : register(b0)
{
	float tile;        // flow pixels per tile side
	float first;       // 1: start from the global motion
	float maxStep;     // flow pixels
	float minEigen;    // the smaller eigenvalue of the tile's gradient matrix a step needs, per unit of
	                   // the squared differences left; 0: every step is taken
	float maxOffset;   // flow pixels a tile may stray from the global motion
	float verify;      // 1: weigh the estimate against the global motion instead of stepping
	float textureLow;  // texture against the grain where the tile's own motion starts to count
	float textureHigh; // and where it counts whole
};

#define T 16   // threads per side; each thread sums tile / T pixels each way

groupshared float4 sumA[T * T];
groupshared float3 sumB[T * T];

// A frame at a position in pixels (centres at +0.5): x the value, yz its gradient, as
// cs_blur.hlsl took them through a Gaussian of 0.8 pixel, bilinearly between pixels. (Sampling
// the Gaussian at the position itself, so as to smooth the grain alike at any position, moved
// the tiles no differently.)
float3 Sample(Texture2D<float4> tex, float2 q, int2 last)
{
	const float2 f = q - 0.5;
	const int2 i0 = (int2)floor(f);
	const float2 t = f - (float2)i0;
	const float3 a = tex.Load(int3(clamp(i0, int2(0, 0), last), 0)).xyz;
	const float3 b = tex.Load(int3(clamp(i0 + int2(1, 0), int2(0, 0), last), 0)).xyz;
	const float3 c = tex.Load(int3(clamp(i0 + int2(0, 1), int2(0, 0), last), 0)).xyz;
	const float3 d = tex.Load(int3(clamp(i0 + int2(1, 1), int2(0, 0), last), 0)).xyz;
	return lerp(lerp(a, b, t.x), lerp(c, d, t.x), t.y);
}

[numthreads(T, T, 1)]
void main(uint3 group : SV_GroupID, uint3 tid3 : SV_GroupThreadID, uint tid : SV_GroupIndex)
{
	uint w, h;
	frameNow.GetDimensions(w, h);
	const int2 last = int2(w, h) - 1;
	const float2 g0 = globalMotion.Load(int3(0, 0, 0)).xy;
	const float2 v = first > 0.5 ? g0 : estimate.Load(int3(group.xy, 0)).xy;

	float4 a = 0;
	float3 b = 0;
	const int per = max((int)((uint)tile / (uint)T), 1);
	const int2 base = int2(group.xy) * (int)tile + int2(tid3.xy) * per;
	if (verify > 0.5) {
		// a: the gradient matrix at the estimate, and the grain's energy; b: the squared
		// differences at the global motion and at the estimate.
		for (int y = 0; y < per; y++) {
			for (int x = 0; x < per; x++) {
				const int2 p = base + int2(x, y);
				const float2 qg = (float2)p + 0.5 + g0, qv = (float2)p + 0.5 + v;
				if (any(p < 3) || any(p > last - 3) || any(min(qg, qv) < 3.0) || any(max(qg, qv) > (float2)last - 2.0)) {
					continue;
				}
				const float now = frameNow.Load(int3(p, 0)).x;
				const float3 atV = Sample(frameLast, qv, last);
				const float eg = now - Sample(frameLast, qg, last).x;
				const float ev = now - atV.x;
				const float hp = frameRaw.Load(int3(p, 0)) - frameFine.Load(int3(p, 0));
				a += float4(atV.y * atV.y, atV.y * atV.z, atV.z * atV.z, hp * hp);
				b += float3(eg * eg, ev * ev, 1.0);
			}
		}
		sumA[tid] = a;
		sumB[tid] = b;
		GroupMemoryBarrierWithGroupSync();
		for (uint s = T * T / 2; s > 0; s >>= 1) {
			if (tid < s) {
				sumA[tid] += sumA[tid + s];
				sumB[tid] += sumB[tid + s];
			}
			GroupMemoryBarrierWithGroupSync();
		}
		if (tid == 0) {
			const float4 A = sumA[0];
			const float3 e = sumB[0];
			const float tr = A.x + A.z;
			const float lmin = 0.5 * (tr - sqrt(max(tr * tr - 4.0 * (A.x * A.z - A.y * A.y), 0.0)));
			const float textured = lmin / max(A.w, 1e-12);
			const float w = e.y < e.x ? smoothstep(textureLow, textureHigh, textured) : 0.0;
			result[group.xy] = float4(lerp(g0, v, w), textured, w);
		}
		return;
	}
	for (int y = 0; y < per; y++) {
		for (int x = 0; x < per; x++) {
			const int2 p = base + int2(x, y);
			if (any(p < 3) || any(p > last - 3)) {
				continue;
			}
			const float2 q = (float2)p + 0.5 + v;
			if (any(q < 3.0) || any(q > (float2)last - 2.0)) {
				continue;
			}
			const float3 previous = Sample(frameLast, q, last);
			const float2 grad = previous.yz;
			const float e = frameNow.Load(int3(p, 0)).x - previous.x;
			a += float4(grad.x * grad.x, grad.x * grad.y, grad.y * grad.y, 1.0);
			b += float3(grad * e, e * e);
		}
	}
	sumA[tid] = a;
	sumB[tid] = b;
	GroupMemoryBarrierWithGroupSync();
	for (uint s = T * T / 2; s > 0; s >>= 1) {
		if (tid < s) {
			sumA[tid] += sumA[tid + s];
			sumB[tid] += sumB[tid + s];
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (tid == 0) {
		const float4 A = sumA[0];
		const float3 B = sumB[0];
		float2 next = v;
		const float det = A.x * A.z - A.y * A.y;
		const float tr = A.x + A.z;
		const float lmin = 0.5 * (tr - sqrt(max(tr * tr - 4.0 * det, 0.0)));
		if (A.w > 0.0 && lmin > minEigen * B.z) {
			float2 step = float2(A.z * B.x - A.y * B.y, A.x * B.y - A.y * B.x) / det;
			const float len = length(step);
			if (len > maxStep) {
				step *= maxStep / len;
			}
			next = v + step;
			const float2 off = next - g0;
			if (length(off) > maxOffset) {
				next = g0 + off * (maxOffset / length(off));
			}
		}
		result[group.xy] = float4(next, lmin / max(A.w, 1.0), 1.0);
	}
}

#else

// MEDIAN: the tiles' field, each tile the median of the (2 r + 1)^2 tiles around it, x and y
// apart. Grain alone moves each tile its own way; a motion of the picture's own, the
// trees' parallax, is shared by the tiles around it.

Texture2D<float4>   source : register(t0);
RWTexture2D<float4> result : register(u0);

cbuffer CS_TILE_MEDIAN : register(b0)
{
	float radius;   // 1 or 2
	float3 reserved;
};

float Median(float a[25], int n)
{
	// Partial selection: the middle one of n.
	const int m = (int)((uint)n / 2u);
	for (int i = 0; i <= m; i++) {
		int k = i;
		for (int j = i + 1; j < n; j++) {
			if (a[j] < a[k]) {
				k = j;
			}
		}
		const float s = a[i];
		a[i] = a[k];
		a[k] = s;
	}
	return a[m];
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint w, h;
	source.GetDimensions(w, h);
	if (id.x >= w || id.y >= h) {
		return;
	}
	const int2 last = int2(w, h) - 1;
	const int r = clamp((int)radius, 1, 2);
	float xs[25], ys[25];
	int n = 0;
	for (int j = -r; j <= r; j++) {
		for (int i = -r; i <= r; i++) {
			const int2 q = int2(id.xy) + int2(i, j);
			if (any(q < 0) || any(q > last)) {
				continue;
			}
			const float4 s = source.Load(int3(q, 0));
			xs[n] = s.x;
			ys[n] = s.y;
			n++;
		}
	}
	const float4 centre = source.Load(int3(id.xy, 0));
	result[id.xy] = float4(Median(xs, n), Median(ys, n), centre.z, centre.w);
}

#endif
