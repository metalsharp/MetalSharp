RWTexture2DMS<uint, 4> target : register(u0);
RWByteAddressBuffer output : register(u1);
RWTexture2DMSArray<uint, 4> target_array : register(u2);

struct VSOut { float4 position : SV_Position; };

VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  o.position = id == 0 ? float4(-3.0, -3.0, 0.0, 1.0)
             : (id == 1 ? float4(9.0, -3.0, 0.0, 1.0)
                        : float4(-3.0, 9.0, 0.0, 1.0));
  return o;
}

float4 ps_main(VSOut input, uint sample : SV_SampleIndex) : SV_Target0 {
  uint prior = target.Load(uint2(0, 0), sample);
  target.sample[sample][uint2(0, 0)] = prior + 1;
  uint array_prior = target_array.Load(uint3(0, 0, 1), sample);
  target_array.sample[sample][uint3(0, 0, 1)] = array_prior + 1;
  return float4(1.0, 0.0, 0.0, 1.0);
}

[numthreads(4, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
  uint value = target.Load(uint2(0, 0), id.x);
  uint array_value = target_array.Load(uint3(0, 0, 1), id.x);
  output.Store(id.x * 4, value);
  output.Store((id.x + 4) * 4, array_value);
}
