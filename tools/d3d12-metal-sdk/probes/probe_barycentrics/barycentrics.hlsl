struct VSOut {
  float4 position : SV_Position;
};

VSOut vs_main(uint vertex_id : SV_VertexID) {
  VSOut output;
  output.position = vertex_id == 0
      ? float4(-1.0, -1.0, 0.0, 1.0)
      : (vertex_id == 1 ? float4(3.0, -1.0, 0.0, 1.0)
                        : float4(-1.0, 3.0, 0.0, 1.0));
  return output;
}

float4 ps_main(float3 barycentrics : SV_Barycentrics) : SV_Target {
  return float4(barycentrics, 1.0);
}
