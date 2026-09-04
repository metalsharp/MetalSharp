struct VSOut {
  float4 position : SV_Position;
  float4 color : COLOR0;
};

VSOut vs_main(uint vertex_id : SV_VertexID, uint view_id : SV_ViewID) {
  float2 position = vertex_id == 0 ? float2(-1.0, -1.0) :
                    vertex_id == 1 ? float2(3.0, -1.0) :
                                      float2(-1.0, 3.0);
  VSOut output;
  output.position = float4(position, 0.0, 1.0);
  output.color = view_id == 0 ? float4(1.0, 0.0, 0.0, 1.0)
                              : float4(0.0, 1.0, 0.0, 1.0);
  return output;
}

float4 ps_main(VSOut input) : SV_Target {
  return input.color;
}
