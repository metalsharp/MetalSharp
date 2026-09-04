struct VSOut { float4 position : SV_Position; };

VSOut vs_main(uint id : SV_VertexID) {
  VSOut output;
  output.position = id == 0 ? float4(-3.0, -3.0, 0.0, 1.0)
                    : (id == 1 ? float4(9.0, -3.0, 0.0, 1.0)
                               : float4(-3.0, 9.0, 0.0, 1.0));
  return output;
}

float4 ps_main(VSOut input) : SV_Target0 {
  return float4(1.0, 0.0, 0.0, 1.0);
}
