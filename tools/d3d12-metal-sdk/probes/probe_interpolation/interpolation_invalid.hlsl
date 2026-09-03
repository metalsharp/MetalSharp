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

// This valid source-owned pixel entry is truncated by the C++ probe before
// PSO creation.  The negative lane therefore exercises malformed DXIL rather
// than depending on compiler-specific signature-linkage diagnostics.
struct InvalidPSIn {
  float4 position : SV_Position;
  float value : TEXCOORD0;
};

float4 ps_invalid(InvalidPSIn input) : SV_Target0 {
  return float4(input.value, 0.0, 0.0, 1.0);
}
