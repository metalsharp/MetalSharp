struct VSOut { float4 position : SV_Position; };

VSOut vs_main(uint id : SV_VertexID) {
  VSOut output;
  // Use two triangles for a quad whose outer edges are outside all sample
  // locations. The coverage result then tests SampleMask rather than a
  // diagonal edge tie at the one-pixel viewport corner.
  float2 position = id == 0 ? float2(-3.0, -3.0) :
                    (id == 1 ? float2(3.0, -3.0) :
                     (id == 2 ? float2(3.0, 3.0) :
                      (id == 3 ? float2(-3.0, -3.0) :
                       (id == 4 ? float2(3.0, 3.0) : float2(-3.0, 3.0)))));
  output.position = float4(position, 0.0, 1.0);
  return output;
}

float4 ps_main(VSOut input, uint sample_index : SV_SampleIndex) : SV_Target0 {
  return float4((float)(sample_index + 1), 0.0,
                (float)sample_index, 1.0);
}
