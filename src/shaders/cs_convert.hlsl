// The picture into DLSS and back.
//
//   PASS 0  in: the picture as it came, three planes of PQ code values, W x H, into DLSS's
//           input and into the picture the motion is measured on, both with a guard band
//           of `pad` pixels mirrored on every side. DLSS is given linear light, 1.0 = 100
//           nits, with IsHDR: without it DLSS quantizes the data to 8 bits (programming
//           guide 310.6.0, 3.1.2). With writeDlss at 0 only the motion's picture is
//           written: a clip given for the motion alone.
//   PASS 1  out: DLSS's output, guard band taken off, back to three planes of PQ code
//           values
//
// A guard band because DLSS keeps an edge effect of its own: MPCVR-Plus measured a
// 32-pixel mirrored band taking a further 7 to 10 % off the grain flicker in the
// outermost band (README-DLSS5.md, DLSS Super Resolution).

#ifndef PASS
#define PASS 0
#endif

cbuffer CS_CONVERT : register(b0)
{
	float  pad;        // guard band, pixels
	float  toLinear;   // 1: DLSS works on linear light
	float  scale;      // linear value of 1.0 in units of 10000 nits: 100 nits is 0.01
	float  writeDlss;  // 0: the motion's picture only
	float2 size;       // the picture's size without the band
	float2 reserved;
};

static const float kM1 = 2610.0 / 16384.0;
static const float kM2 = 2523.0 / 4096.0 * 128.0;
static const float kC1 = 3424.0 / 4096.0;
static const float kC2 = 2413.0 / 4096.0 * 32.0;
static const float kC3 = 2392.0 / 4096.0 * 32.0;

float3 PqToLinear(float3 e)
{
	const float3 p = pow(saturate(e), 1.0 / kM2);
	return pow(max(p - kC1, 0.0) / (kC2 - kC3 * p), 1.0 / kM1);
}

float3 LinearToPq(float3 y)
{
	const float3 m = pow(max(y, 0.0), kM1);
	return pow((kC1 + kC2 * m) / (1.0 + kC3 * m), kM2);
}

#if PASS == 0

Texture2D<float>    planeR  : register(t0);   // PQ code values, W x H
Texture2D<float>    planeG  : register(t1);
Texture2D<float>    planeB  : register(t2);
RWTexture2D<float4> dlssIn  : register(u0);   // (W + 2 pad) x (H + 2 pad), linear or PQ
RWTexture2D<float4> motion  : register(u1);   // the same size, PQ code values

uint Mirror(int x, uint n)
{
	// Reflected about the edge, the edge pixel not repeated: -1 -> 1, n -> n - 2.
	const uint period = 2 * n - 2;
	const uint m = (uint)abs(x) % period;
	return m < n ? m : period - m;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint w, h;
	motion.GetDimensions(w, h);
	if (id.x >= w || id.y >= h) {
		return;
	}
	const int3 p = int3(Mirror((int)id.x - (int)pad, (uint)size.x), Mirror((int)id.y - (int)pad, (uint)size.y), 0);
	const float3 pq = saturate(float3(planeR.Load(p), planeG.Load(p), planeB.Load(p)));
	motion[id.xy] = float4(pq, 1.0);
	if (writeDlss > 0.5) {
		dlssIn[id.xy] = float4(toLinear > 0.5 ? PqToLinear(pq) / scale : pq, 1.0);
	}
}

#else

Texture2D<float4>  dlssOut : register(t0);   // (W + 2 pad) x (H + 2 pad)
RWTexture2D<float> outR    : register(u0);   // W x H, PQ code values
RWTexture2D<float> outG    : register(u1);
RWTexture2D<float> outB    : register(u2);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= (uint)size.x || id.y >= (uint)size.y) {
		return;
	}
	const float3 c = dlssOut.Load(int3(id.xy + (uint)pad, 0)).rgb;
	const float3 pq = toLinear > 0.5 ? LinearToPq(c * scale) : saturate(c);
	outR[id.xy] = pq.r;
	outG[id.xy] = pq.g;
	outB[id.xy] = pq.b;
}

#endif
