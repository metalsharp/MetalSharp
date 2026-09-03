struct VSOut {
  float4 position : SV_Position;
};

VSOut vs_main(uint id : SV_VertexID) {
  VSOut output;
  output.position = id == 0 ? float4(-1.0, -1.0, 0.0, 1.0)
                    : (id == 1 ? float4(3.0, -1.0, 0.0, 1.0)
                               : float4(-1.0, 3.0, 0.0, 1.0));
  return output;
}

RasterizerOrderedTexture1D<uint> target_1d : register(u0);
RasterizerOrderedTexture1DArray<uint> target_1d_array : register(u0);
RasterizerOrderedTexture3D<uint> target_3d : register(u0);

float4 ps_1d(VSOut input) : SV_Target0 {
  uint value = target_1d[0];
  target_1d[0] = value + 1;
  return float4(1.0, 0.0, 0.0, 1.0);
}

float4 ps_1d_array(VSOut input) : SV_Target0 {
  uint value = target_1d_array[uint2(0, 1)];
  target_1d_array[uint2(0, 1)] = value + 1;
  return float4(1.0, 0.0, 0.0, 1.0);
}

float4 ps_3d(VSOut input) : SV_Target0 {
  uint value = target_3d[uint3(0, 0, 0)];
  target_3d[uint3(0, 0, 0)] = value + 1;
  return float4(1.0, 0.0, 0.0, 1.0);
}
