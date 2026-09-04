struct VSOut {
  float4 position : SV_Position;
};

VSOut vs_main(float3 position : POSITION) {
  VSOut output;
  output.position = float4(position, 1.0);
  return output;
}

float4 ps_main(float4 position : SV_Position,
               uint inner_coverage : SV_InnerCoverage) : SV_Target {
  return inner_coverage != 0 ? float4(1.0, 1.0, 1.0, 1.0)
                              : float4(0.0, 0.0, 0.0, 1.0);
}
