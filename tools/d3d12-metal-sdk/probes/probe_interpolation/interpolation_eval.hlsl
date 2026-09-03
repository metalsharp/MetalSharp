struct VSOut {
  float4 position : SV_Position;
  float value : TEXCOORD0;
};

VSOut vs_main(uint id : SV_VertexID) {
  VSOut output;
  output.position = id == 0 ? float4(-1.0, -1.0, 0.0, 1.0)
                    : (id == 1 ? float4(3.0, -1.0, 0.0, 2.0)
                               : float4(-1.0, 3.0, 0.0, 4.0));
  output.value = id == 0 ? 0.0 : (id == 1 ? 1.0 : 0.5);
  return output;
}

struct PSIn {
  float4 position : SV_Position;
  float value : TEXCOORD0;
};

float4 ps_eval(PSIn input) : SV_Target0 {
  float centroid_value = EvaluateAttributeCentroid(input.value);
  float sample_value = EvaluateAttributeAtSample(input.value, 0);
  float snapped_value = EvaluateAttributeSnapped(input.value, int2(0, 0));
  return float4(centroid_value, sample_value, snapped_value, 1.0);
}
