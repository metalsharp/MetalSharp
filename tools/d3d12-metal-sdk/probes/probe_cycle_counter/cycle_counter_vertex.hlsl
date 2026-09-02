struct VSOut {
  float4 position : SV_POSITION;
};

VSOut vs_main(uint vertex_id : SV_VertexID) {
  VSOut output;
  output.position = vertex_id == 0
      ? float4(-1.0, -1.0, 0.0, 1.0)
      : (vertex_id == 1
             ? float4(3.0, -1.0, 0.0, 1.0)
             : float4(-1.0, 3.0, 0.0, 1.0));
  return output;
}
