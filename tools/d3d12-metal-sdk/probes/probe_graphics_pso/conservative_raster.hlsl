struct VSIn { float3 position : POSITION; };
struct VSOut { float4 position : SV_Position; };

VSOut vs_main(VSIn input) {
  VSOut output;
  output.position = float4(input.position, 1.0);
  return output;
}

float4 ps_main() : SV_Target0 {
  return float4(1.0, 0.0, 0.0, 1.0);
}
