cbuffer Addresses : register(b0) { uint addressLow; uint addressHigh; uint mode; };
RWByteAddressBuffer data : register(u0);
uint2 addressAt(uint offset) {
    uint low = addressLow + offset;
    return uint2(low, addressHigh + uint(low < addressLow));
}
[numthreads(1,1,1)]
void produce() {
    // D3D12_MULTI_NODE_GPU_INPUT: count, padding, node-input address/stride.
    data.Store(0, mode == 2u ? 257u : (mode == 5u ? 0u : 2u)); data.Store(4, 0u);
    data.Store2(8, addressAt(64)); data.Store2(16, uint2(mode == 3u ? 4u : 24u,0));
    // Two D3D12_NODE_GPU_INPUT descriptors.
    data.Store2(64, uint2(mode == 1u ? 9u : 0u, mode == 8u ? 3u : 4u));
    data.Store2(72, addressAt(mode == 4u ? 240u : 112u));
    data.Store2(80, uint2((mode == 7u || mode == 8u) ? 0u : 8u,0));
    data.Store2(88, uint2(mode == 6u ? 9u : 1u,4)); data.Store2(96, addressAt(144)); data.Store2(104, uint2(8,0));
    data.Store2(112, uint2(0,101)); data.Store2(120, uint2(1,202));
    data.Store2(128, uint2(2,303)); data.Store2(136, uint2(3,404));
    data.Store2(144, uint2(65536,505)); data.Store2(152, uint2(1,999));
    data.Store2(160, uint2(0,888)); data.Store2(168, uint2(0xffffffff,777));
}
