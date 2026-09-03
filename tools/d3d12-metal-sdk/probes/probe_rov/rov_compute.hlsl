RasterizerOrderedByteAddressBuffer g_rov : register(u0);

[numthreads(1, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
  uint prior = g_rov.Load(0);
  g_rov.Store(0, prior + 1);
}
