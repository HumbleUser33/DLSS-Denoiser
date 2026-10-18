// A box blur of a flow frame, separable, into 32-bit floats, then its value and gradient:
// what cs_tiles.hlsl takes its Gauss-Newton steps on, first heavily blurred to follow what is
// out of focus, then lightly.
//
//   PASS 0  horizontal: the R8 flow frame -> R32F
//   PASS 1  vertical:   R32F -> R32F
//   PASS 2  value and gradient through a Gaussian of 0.8 pixel: R32F -> RGBA16F (value, d/dx,
//           d/dy); the steps read it bilinearly, four loads a pixel where sampling the Gaussian
//           there took 72

#ifndef PASS
#define PASS 0
#endif

Texture2D<float>   source : register(t0);
#if PASS == 2
RWTexture2D<float4> result : register(u0);
#else
RWTexture2D<float> result : register(u0);
#endif

cbuffer CS_BLUR : register(b0)
{
	float  radius;
	float3 reserved;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint w, h;
	source.GetDimensions(w, h);
	if (id.x >= w || id.y >= h) {
		return;
	}
	const int2 last = int2(w, h) - 1;
#if PASS == 2
	const float sigma = 0.8;
	float wsum = 0.0, s = 0.0;
	float2 d1 = 0.0;
	for (int j = -2; j <= 2; j++) {
		for (int i = -2; i <= 2; i++) {
			const float2 d = float2(i, j);
			const float wt = exp(-dot(d, d) * (0.5 / (sigma * sigma)));
			const float v = source.Load(int3(clamp(int2(id.xy) + int2(i, j), int2(0, 0), last), 0));
			wsum += wt;
			s += wt * v;
			d1 += wt * d * v;
		}
	}
	result[id.xy] = float4(s / wsum, d1 / (wsum * sigma * sigma), 0.0);
#else
	const int r = (int)radius;
#if PASS == 0
	const int2 axis = int2(1, 0);
#else
	const int2 axis = int2(0, 1);
#endif
	float s = 0.0;
	for (int i = -r; i <= r; i++) {
		s += source.Load(int3(clamp(int2(id.xy) + axis * i, int2(0, 0), last), 0));
	}
	result[id.xy] = s / (float)(2 * r + 1);
#endif
}
