struct VSOut {
  float4 position : SV_POSITION;
};

VSOut vs_main(uint vertex_id : SV_VertexID) {
  VSOut output;
  uint id = vertex_id % 3;
  output.position = id == 0
      ? float4(-1.0, -1.0, 0.0, 1.0)
      : (id == 1 ? float4(3.0, -1.0, 0.0, 1.0)
                : float4(-1.0, 3.0, 0.0, 1.0));
  return output;
}

RasterizerOrderedTexture2DArray<uint> g_rov : register(u0);

float4 ps_main(VSOut input) : SV_TARGET {
  uint prior = g_rov[uint3(0, 0, 1)];
  g_rov[uint3(0, 0, 1)] = prior + 1;
  return float4(1.0, 0.0, 0.0, 1.0);
}
