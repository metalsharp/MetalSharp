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

struct ROVValue {
  uint value;
};

RasterizerOrderedStructuredBuffer<ROVValue> g_rov : register(u0);

float4 ps_main(VSOut input) : SV_TARGET {
  uint prior = g_rov[0].value;
  g_rov[0].value = prior + 1;
  return float4(1.0, 0.0, 0.0, 1.0);
}
