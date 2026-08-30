SamplerState samp : register(s0);

Texture1D<float4> tex1 : register(t0);
Texture1DArray<float4> tex1a : register(t0);
Texture2D<float4> tex2 : register(t0);
Texture2DArray<float4> tex2a : register(t0);
Texture3D<float4> tex3 : register(t0);
TextureCube<float4> texcube : register(t0);
TextureCubeArray<float4> texcubea : register(t0);
Texture2DMS<float4> texms : register(t0);
Texture2DMSArray<float4> texmsa : register(t0);

RWStructuredBuffer<uint> output : register(u0);

[numthreads(4,1,1)]
void cs_texture_1d(uint3 id : SV_DispatchThreadID) {
  output[0] = uint(tex1.Sample(samp, 0.5).r * 255.0 + 0.5);
}

[numthreads(4,1,1)]
void cs_texture_1d_array(uint3 id : SV_DispatchThreadID) {
  output[0] = uint(tex1a.Sample(samp, float2(0.5, 1)).r * 255.0 + 0.5);
}

[numthreads(4,1,1)]
void cs_texture_2d(uint3 id : SV_DispatchThreadID) {
  output[0] = uint(tex2.SampleLevel(samp, float2(0.5, 0.5), 0).r * 255.0 + 0.5);
}

[numthreads(4,1,1)]
void cs_texture_2d_array(uint3 id : SV_DispatchThreadID) {
  output[0] = uint(tex2a.SampleLevel(samp, float3(0.5, 0.5, 1), 0).r * 255.0 + 0.5);
}

[numthreads(4,1,1)]
void cs_texture_3d(uint3 id : SV_DispatchThreadID) {
  output[0] = uint(tex3.SampleLevel(samp, float3(0.5, 0.5, 0.375), 0).r * 255.0 + 0.5);
}

[numthreads(4,1,1)]
void cs_texture_cube(uint3 id : SV_DispatchThreadID) {
  output[0] = uint(texcube.SampleLevel(samp, float3(0, 0, 1), 0).r * 255.0 + 0.5);
}

[numthreads(4,1,1)]
void cs_texture_cube_array(uint3 id : SV_DispatchThreadID) {
  output[0] = uint(texcubea.SampleLevel(samp, float4(0, 0, 1, 1), 0).r * 255.0 + 0.5);
}

[numthreads(4,1,1)]
void cs_texture_2d_ms(uint3 id : SV_DispatchThreadID) {
  output[0] = uint(texms.Load(int2(0, 0), 0).r * 255.0 + 0.5);
}

[numthreads(4,1,1)]
void cs_texture_2d_ms_array(uint3 id : SV_DispatchThreadID) {
  output[0] = uint(texmsa.Load(int3(0, 0, 1), 0).r * 255.0 + 0.5);
}

RWTexture1D<float4> rw1 : register(u0);
RWTexture1DArray<float4> rw1a : register(u0);
RWTexture2D<float4> rw2 : register(u0);
RWTexture2DArray<float4> rw2a : register(u0);
RWTexture3D<float4> rw3 : register(u0);
RWTexture2DMS<float4> rwms : register(u0);
RWTexture2DMSArray<float4> rwmsa : register(u0);

[numthreads(4,1,1)]
void cs_store_1d(uint3 id : SV_DispatchThreadID) { rw1[0] = float4(0.25, 0, 0, 1); }
[numthreads(4,1,1)]
void cs_store_1d_array(uint3 id : SV_DispatchThreadID) { rw1a[uint2(0, 1)] = float4(0.25, 0, 0, 1); }
[numthreads(4,1,1)]
void cs_store_2d(uint3 id : SV_DispatchThreadID) { rw2[uint2(0, 0)] = float4(0.25, 0, 0, 1); }
[numthreads(4,1,1)]
void cs_store_2d_array(uint3 id : SV_DispatchThreadID) { rw2a[uint3(0, 0, 1)] = float4(0.25, 0, 0, 1); }
[numthreads(4,1,1)]
void cs_store_3d(uint3 id : SV_DispatchThreadID) { rw3[uint3(0, 0, 1)] = float4(0.25, 0, 0, 1); }
[numthreads(4,1,1)]
void cs_store_2d_ms(uint3 id : SV_DispatchThreadID) { rwms[uint2(0, 0)] = float4(0.25, 0, 0, 1); }
[numthreads(4,1,1)]
void cs_store_2d_ms_array(uint3 id : SV_DispatchThreadID) { rwmsa[uint3(0, 0, 0)] = float4(0.25, 0, 0, 1); }
