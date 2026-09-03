struct VSOut {
  float4 position : SV_Position;
  float value : TEXCOORD0;
};

VSOut vs_main(uint id : SV_VertexID) {
  // Keep the fixture branch-based rather than using a constant array.  The
  // pinned DXIL lowering path has no resource backing for global constant
  // arrays, while these literal selects retain the intended dynamic
  // vertex-id behavior without inviting host-side vertex setup.
  VSOut output;
  output.position = id == 0 ? float4(-1.0, -1.0, 0.0, 1.0)
                    : (id == 1 ? float4(3.0, -1.0, 0.0, 2.0)
                               : float4(-1.0, 3.0, 0.0, 4.0));
  output.value = id == 0 ? 0.0 : (id == 1 ? 1.0 : 0.5);
  return output;
}

struct PSLinear {
  float4 position : SV_Position;
  float value : TEXCOORD0;
};

struct PSNoPerspective {
  float4 position : SV_Position;
  noperspective float value : TEXCOORD0;
};

struct PSCentroid {
  float4 position : SV_Position;
  centroid float value : TEXCOORD0;
};

struct PSSample {
  float4 position : SV_Position;
  sample float value : TEXCOORD0;
};

struct PSNoInterpolation {
  float4 position : SV_Position;
  nointerpolation float value : TEXCOORD0;
};

float4 ps_linear(PSLinear input) : SV_Target0 {
  return float4(input.value, 0.0, 0.0, 1.0);
}

float4 ps_noperspective(PSNoPerspective input) : SV_Target0 {
  return float4(input.value, 0.0, 0.0, 1.0);
}

float4 ps_centroid(PSCentroid input) : SV_Target0 {
  return float4(input.value, 0.0, 0.0, 1.0);
}

float4 ps_sample(PSSample input) : SV_Target0 {
  return float4(input.value, 0.0, 0.0, 1.0);
}

float4 ps_nointerpolation(PSNoInterpolation input) : SV_Target0 {
  return float4(input.value, 0.0, 0.0, 1.0);
}
