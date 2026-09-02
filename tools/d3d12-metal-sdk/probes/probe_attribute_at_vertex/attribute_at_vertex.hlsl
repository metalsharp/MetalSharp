struct VSOut {
  float4 position : SV_POSITION;
  nointerpolation float4 attr : ATTR0;
};

VSOut vs_main(uint vertex_id : SV_VertexID) {
  VSOut output;
  output.position = vertex_id == 0
      ? float4(-1.0, -1.0, 0.0, 1.0)
      : (vertex_id == 1
             ? float4(3.0, -1.0, 0.0, 1.0)
             : float4(-1.0, 3.0, 0.0, 1.0));
  output.attr = vertex_id == 0
      ? float4(0.125, 0.0, 0.0, 1.0)
      : (vertex_id == 1
             ? float4(0.500, 0.0, 0.0, 1.0)
             : float4(0.875, 0.0, 0.0, 1.0));
  return output;
}

struct PSIn {
  float4 position : SV_POSITION;
  nointerpolation float4 attr : ATTR0;
};

RWByteAddressBuffer output : register(u0);

float4 ps_main(PSIn input) : SV_TARGET {
  float vertex_0 = GetAttributeAtVertex(input.attr, 0)[0];
  float vertex_1 = GetAttributeAtVertex(input.attr, 1)[0];
  float vertex_2 = GetAttributeAtVertex(input.attr, 2)[0];
  output.Store(0, asuint(vertex_0));
  output.Store(4, asuint(vertex_1));
  output.Store(8, asuint(vertex_2));
  return float4(vertex_0, vertex_1, vertex_2, 1.0);
}
