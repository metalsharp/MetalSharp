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
#if defined(M12_TYPED_UINT)
Texture2D<uint> tex_uint : register(t0);
#elif defined(M12_TYPED_SINT)
Texture2D<int> tex_sint : register(t0);
#elif defined(M12_TYPED_UINT2)
Texture2D<uint2> tex_uint2 : register(t0);
#elif defined(M12_TYPED_UINT4)
Texture2D<uint4> tex_uint4 : register(t0);
#elif defined(M12_TYPED_SINT4)
Texture2D<int4> tex_sint4 : register(t0);
#elif defined(M12_TYPED_FLOAT16)
Texture2D<float> tex_float : register(t0);
#elif defined(M12_TYPED_UINT64)
Texture2D<uint64_t> tex_uint64 : register(t0);
#elif defined(M12_TYPED_SINT64)
Texture2D<int64_t> tex_sint64 : register(t0);
#endif

RWStructuredBuffer<uint> output : register(u0);

[numthreads(4,1,1)]
void cs_texture_1d(uint3 id : SV_DispatchThreadID) {
  output[0] = uint(tex1.SampleLevel(samp, 0.5, 0).r * 255.0 + 0.5);
  uint w;
  tex1.GetDimensions(w);
  output[1] = w;
}

[numthreads(4,1,1)]
void cs_texture_1d_array(uint3 id : SV_DispatchThreadID) {
  output[0] = uint(tex1a.Sample(samp, float2(0.5, 1)).r * 255.0 + 0.5);
  uint w, elements;
  tex1a.GetDimensions(w, elements);
  output[1] = w | (elements << 16);
}

[numthreads(1,1,1)]
void cs_texture_1d_mip(uint3 id : SV_DispatchThreadID) {
  output[0] = uint(tex1.SampleLevel(samp, 0.5, 1.0).r * 255.0 + 0.5);
  uint w, levels;
  tex1.GetDimensions(1, w, levels);
  output[1] = w | (levels << 16);
}

[numthreads(1,1,1)]
void cs_texture_1d_array_mip(uint3 id : SV_DispatchThreadID) {
  output[0] = uint(tex1a.SampleLevel(samp, float2(0.5, 1), 1.0).r * 255.0 + 0.5);
  uint w, elements, levels;
  tex1a.GetDimensions(1, w, elements, levels);
  output[1] = w | (elements << 8) | (levels << 16);
}

[numthreads(4,1,1)]
void cs_texture_1d_filter(uint3 id : SV_DispatchThreadID) {
  uint mag_value = uint(tex1.SampleGrad(samp, 0.5, 0.125, 0.0).r * 255.0 + 0.5);
  uint min_value = uint(tex1.SampleGrad(samp, 0.5, 0.5, 0.0).r * 255.0 + 0.5);
  uint mip_value = uint(tex1.SampleLevel(samp, 0.5, 0.25).r * 255.0 + 0.5);
  output[0] = mag_value | (min_value << 8) | (mip_value << 16);
  uint w, levels;
  tex1.GetDimensions(0, w, levels);
  output[1] = w | (levels << 16);
}

[numthreads(1,1,1)]
void cs_texture_1d_border(uint3 id : SV_DispatchThreadID) {
  float4 value = tex1.SampleLevel(samp, -0.125, 0.0);
  uint4 rgba = uint4(value * 255.0 + 0.5);
  output[0] = rgba.x | (rgba.y << 8) | (rgba.z << 16) | (rgba.w << 24);
  uint w;
  tex1.GetDimensions(w);
  output[1] = w;
}

[numthreads(1,1,1)]
void cs_texture_1d_address(uint3 id : SV_DispatchThreadID) {
  uint a = uint(tex1.SampleLevel(samp, -0.125, 0.0).r * 255.0 + 0.5);
  uint b = uint(tex1.SampleLevel(samp, 1.125, 0.0).r * 255.0 + 0.5);
  uint c = uint(tex1.SampleLevel(samp, 2.125, 0.0).r * 255.0 + 0.5);
  uint d = uint(tex1.SampleLevel(samp, -2.125, 0.0).r * 255.0 + 0.5);
  output[0] = a | (b << 8) | (c << 16) | (d << 24);
  uint w;
  tex1.GetDimensions(w);
  output[1] = w;
}

[numthreads(4,1,1)]
void cs_texture_1d_advanced(uint3 id : SV_DispatchThreadID) {
  uint level_value = uint(tex1.SampleLevel(samp, 0.25, 1.0).r * 255.0 + 0.5);
  uint bias_value = uint(tex1.SampleBias(samp, 0.25, 1.0).r * 255.0 + 0.5);
  uint grad_value = uint(tex1.SampleGrad(samp, 0.25, 0.5, 0.5).r * 255.0 + 0.5);
  uint offset_value = uint(tex1.SampleLevel(samp, 0.125, 0.0, 1).r * 255.0 + 0.5);
  output[0] = level_value | (bias_value << 8) | (grad_value << 16) |
              (offset_value << 24);
  uint w, levels;
  tex1.GetDimensions(1, w, levels);
  output[1] = w | (levels << 16);
}

[numthreads(4,1,1)]
void cs_texture_2d(uint3 id : SV_DispatchThreadID) {
  output[0] = uint(tex2.SampleLevel(samp, float2(0.5, 0.5), 0).r * 255.0 + 0.5);
  uint w, h;
  tex2.GetDimensions(w, h);
  output[1] = w | (h << 8);
}

[numthreads(4,1,1)]
void cs_texture_2d_array(uint3 id : SV_DispatchThreadID) {
  output[0] = uint(tex2a.SampleLevel(samp, float3(0.5, 0.5, 1), 0).r * 255.0 + 0.5);
  uint w, h, elements;
  tex2a.GetDimensions(w, h, elements);
  output[1] = w | (h << 8) | (elements << 16);
}

[numthreads(4,1,1)]
void cs_texture_3d(uint3 id : SV_DispatchThreadID) {
  output[0] = uint(tex3.SampleLevel(samp, float3(0.5, 0.5, 0.375), 0).r * 255.0 + 0.5);
  uint w, h, d;
  tex3.GetDimensions(w, h, d);
  output[1] = w | (h << 8) | (d << 16);
}

[numthreads(4,1,1)]
void cs_texture_cube(uint3 id : SV_DispatchThreadID) {
  output[0] = uint(texcube.SampleLevel(samp, float3(0, 0, 1), 0).r * 255.0 + 0.5);
  uint w, h;
  texcube.GetDimensions(w, h);
  output[1] = w | (h << 8);
}

[numthreads(4,1,1)]
void cs_texture_cube_array(uint3 id : SV_DispatchThreadID) {
  output[0] = uint(texcubea.SampleLevel(samp, float4(0, 0, 1, 1), 0).r * 255.0 + 0.5);
  uint w, h, cubes;
  texcubea.GetDimensions(w, h, cubes);
  output[1] = w | (h << 8) | (cubes << 16);
}

[numthreads(4,1,1)]
void cs_texture_2d_ms(uint3 id : SV_DispatchThreadID) {
  output[0] = uint(texms.Load(int2(0, 0), 0).r * 255.0 + 0.5);
  uint w, h, samples;
  texms.GetDimensions(w, h, samples);
  output[1] = w | (h << 8) | (samples << 24);
}

[numthreads(4,1,1)]
void cs_texture_2d_ms_array(uint3 id : SV_DispatchThreadID) {
  output[0] = uint(texmsa.Load(int3(0, 0, 1), 0).r * 255.0 + 0.5);
  uint w, h, elements, samples;
  texmsa.GetDimensions(w, h, elements, samples);
  output[1] = w | (h << 8) | (elements << 16) | (samples << 24);
}

#if defined(M12_TYPED_UINT)
[numthreads(1,1,1)]
void cs_texture_typed_uint(uint3 id : SV_DispatchThreadID) {
  output[0] = tex_uint.Load(int3(0, 0, 0));
  uint w, h;
  tex_uint.GetDimensions(w, h);
  output[1] = w | (h << 8);
}
#elif defined(M12_TYPED_SINT)
[numthreads(1,1,1)]
void cs_texture_typed_sint(uint3 id : SV_DispatchThreadID) {
  output[0] = (uint)tex_sint.Load(int3(0, 0, 0));
  uint w, h;
  tex_sint.GetDimensions(w, h);
  output[1] = w | (h << 8);
}
#elif defined(M12_TYPED_UINT2)
[numthreads(1,1,1)]
void cs_texture_typed_uint2(uint3 id : SV_DispatchThreadID) {
  uint2 value = tex_uint2.Load(int3(0, 0, 0));
  output[0] = value.x | (value.y << 16);
  uint w, h;
  tex_uint2.GetDimensions(w, h);
  output[1] = w | (h << 8);
}
#elif defined(M12_TYPED_UINT4)
[numthreads(1,1,1)]
void cs_texture_typed_uint4(uint3 id : SV_DispatchThreadID) {
  uint4 value = tex_uint4.Load(int3(0, 0, 0));
  output[0] = value.x | (value.y << 8) | (value.z << 16) | (value.w << 24);
  uint w, h;
  tex_uint4.GetDimensions(w, h);
  output[1] = w | (h << 8);
}
#elif defined(M12_TYPED_SINT4)
[numthreads(1,1,1)]
void cs_texture_typed_sint4(uint3 id : SV_DispatchThreadID) {
  int4 value = tex_sint4.Load(int3(0, 0, 0));
  output[0] = ((uint)value.x & 0xff) | (((uint)value.y & 0xff) << 8) |
              (((uint)value.z & 0xff) << 16) | (((uint)value.w & 0xff) << 24);
  uint w, h;
  tex_sint4.GetDimensions(w, h);
  output[1] = w | (h << 8);
}
#elif defined(M12_TYPED_FLOAT16)
[numthreads(1,1,1)]
void cs_texture_typed_float16(uint3 id : SV_DispatchThreadID) {
  output[0] = f32tof16(tex_float.Load(int3(0, 0, 0)));
  uint w, h;
  tex_float.GetDimensions(w, h);
  output[1] = w | (h << 8);
}
#elif defined(M12_TYPED_UINT64)
[numthreads(1,1,1)]
void cs_texture_typed_uint64(uint3 id : SV_DispatchThreadID) {
  uint64_t value = tex_uint64.Load(int3(0, 0, 0));
  output[0] = (uint)value;
  output[1] = (uint)(value >> 32);
  uint w, h;
  tex_uint64.GetDimensions(w, h);
  output[2] = w | (h << 8);
}
#elif defined(M12_TYPED_SINT64)
[numthreads(1,1,1)]
void cs_texture_typed_sint64(uint3 id : SV_DispatchThreadID) {
  uint64_t value = (uint64_t)tex_sint64.Load(int3(0, 0, 0));
  output[0] = (uint)value;
  output[1] = (uint)(value >> 32);
  uint w, h;
  tex_sint64.GetDimensions(w, h);
  output[2] = w | (h << 8);
}
#endif

RWTexture1D<float4> rw1 : register(u0);
RWTexture1DArray<float4> rw1a : register(u0);
RWTexture2D<float4> rw2 : register(u0);
RWTexture2DArray<float4> rw2a : register(u0);
RWTexture3D<float4> rw3 : register(u0);
#if defined(M12_STORE_UINT)
RWTexture2D<uint> rw_uint : register(u0);
#elif defined(M12_STORE_SINT)
RWTexture2D<int> rw_sint : register(u0);
#elif defined(M12_STORE_UINT4)
RWTexture2D<uint4> rw_uint4 : register(u0);
#elif defined(M12_STORE_SINT4)
RWTexture2D<int4> rw_sint4 : register(u0);
#elif defined(M12_STORE_UINT64)
RWTexture2D<uint64_t> rw_uint64 : register(u0);
#elif defined(M12_STORE_SINT64)
RWTexture2D<int64_t> rw_sint64 : register(u0);
#endif
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
#if defined(M12_STORE_UINT)
[numthreads(1,1,1)]
void cs_store_typed_uint(uint3 id : SV_DispatchThreadID) { rw_uint[uint2(0, 0)] = 0x12345678; }
#elif defined(M12_STORE_SINT)
[numthreads(1,1,1)]
void cs_store_typed_sint(uint3 id : SV_DispatchThreadID) { rw_sint[uint2(0, 0)] = -1234567; }
#elif defined(M12_STORE_UINT4)
[numthreads(1,1,1)]
void cs_store_typed_uint4(uint3 id : SV_DispatchThreadID) { rw_uint4[uint2(0, 0)] = uint4(10, 20, 30, 40); }
#elif defined(M12_STORE_SINT4)
[numthreads(1,1,1)]
void cs_store_typed_sint4(uint3 id : SV_DispatchThreadID) { rw_sint4[uint2(0, 0)] = int4(-1, -2, -3, -4); }
#elif defined(M12_STORE_UINT64)
[numthreads(1,1,1)]
void cs_store_typed_uint64(uint3 id : SV_DispatchThreadID) { rw_uint64[uint2(0, 0)] = 0x0123456789abcdefull; }
#elif defined(M12_STORE_SINT64)
[numthreads(1,1,1)]
void cs_store_typed_sint64(uint3 id : SV_DispatchThreadID) { rw_sint64[uint2(0, 0)] = (int64_t)0xffffffff12345678ull; }
#endif
[numthreads(4,1,1)]
void cs_store_2d_ms(uint3 id : SV_DispatchThreadID) { rwms[uint2(0, 0)] = float4(0.25, 0, 0, 1); }
[numthreads(4,1,1)]
void cs_store_2d_ms_array(uint3 id : SV_DispatchThreadID) { rwmsa[uint3(0, 0, 0)] = float4(0.25, 0, 0, 1); }
