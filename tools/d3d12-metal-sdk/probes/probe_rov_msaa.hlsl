#ifndef SAMPLES
#define SAMPLES 4
#endif

RWTexture2DMS<uint, SAMPLES> target : register(u0);
RWByteAddressBuffer output : register(u1);
RWTexture2DMSArray<uint, SAMPLES> target_array : register(u2);

struct VSOut { float4 position : SV_Position; };

VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  o.position = id == 0 ? float4(-3.0, -3.0, 0.0, 1.0)
             : (id == 1 ? float4(9.0, -3.0, 0.0, 1.0)
                        : float4(-3.0, 9.0, 0.0, 1.0));
  return o;
}

float4 ps_main(VSOut input, uint sample : SV_SampleIndex) : SV_Target0 {
#if SAMPLES > 4
  // Apple M4 has no native 8x render target.  Execute the flattened 8x ROV
  // provider once from sample zero of a 4x pass so every logical ROV sample is
  // updated exactly once per draw without pretending native 8x rasterization.
  if (sample == 0) {
    for (uint i = 0; i < SAMPLES; ++i) {
      uint prior = target.Load(uint2(0, 0), i);
      target.sample[i][uint2(0, 0)] = prior + 1;
      uint array_prior = target_array.Load(uint3(0, 0, 1), i);
      target_array.sample[i][uint3(0, 0, 1)] = array_prior + 1;
    }
  }
#else
  uint prior = target.Load(uint2(0, 0), sample);
  target.sample[sample][uint2(0, 0)] = prior + 1;
  uint array_prior = target_array.Load(uint3(0, 0, 1), sample);
  target_array.sample[sample][uint3(0, 0, 1)] = array_prior + 1;
#endif
  return float4(1.0, 0.0, 0.0, 1.0);
}

[numthreads(SAMPLES, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
  uint value = target.Load(uint2(0, 0), id.x);
  uint array_value = target_array.Load(uint3(0, 0, 1), id.x);
  output.Store(id.x * 4, value);
  output.Store((id.x + SAMPLES) * 4, array_value);
}
