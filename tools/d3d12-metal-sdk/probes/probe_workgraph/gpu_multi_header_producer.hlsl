cbuffer Addresses : register(b0) { uint addressLow; uint addressHigh; uint mode; };
RWByteAddressBuffer data : register(u0);
uint2 addressAt(uint offset) {
    uint low = addressLow + offset;
    return uint2(low, addressHigh + uint(low < addressLow));
}
[numthreads(1,1,1)]
void produce() {
    // D3D12_MULTI_NODE_GPU_INPUT: count, padding, node-input address/stride.
    data.Store(0, mode == 2u ? 257u : (mode == 5u ? 0u : (mode == 14u ? 8u : 2u))); data.Store(4, 0u);
    data.Store2(8, addressAt(mode == 9u ? 65u : 64u)); data.Store2(16, uint2(mode == 13u ? 0u : (mode == 3u ? 4u : 24u),0));
    if (mode == 14u) {
        // Four duplicate broadcasting descriptors followed by four duplicate
        // sparse thread descriptors exercise a table larger than the minimal
        // two-entry fixture while keeping the payloads out of the table.
        for (uint i = 0u; i < 8u; ++i) {
            uint offset = 64u + i * 24u;
            uint broadcast = i < 4u;
            data.Store2(offset, uint2(broadcast ? 0u : 1u, 4u));
            data.Store2(offset + 8u, addressAt(broadcast ? 256u : 288u));
            data.Store2(offset + 16u, uint2(8u, 0u));
        }
        data.Store2(256, uint2(0u, 101u)); data.Store2(264, uint2(1u, 202u));
        data.Store2(272, uint2(2u, 303u)); data.Store2(280, uint2(3u, 404u));
        data.Store2(288, uint2(65536u, 505u)); data.Store2(296, uint2(1u, 999u));
        data.Store2(304, uint2(0u, 888u)); data.Store2(312, uint2(0xffffffffu, 777u));
    } else {
        // Two D3D12_NODE_GPU_INPUT descriptors.
        data.Store2(64, uint2(mode == 1u ? 9u : 0u, mode == 11u ? 256u : (mode == 10u ? 0u : (mode == 8u ? 3u : 4u))));
        data.Store2(72, mode == 10u ? uint2(0xffffffffu,0xffffffffu) : addressAt(mode == 4u ? 504u : 112u));
        data.Store2(80, uint2((mode == 7u || mode == 8u || mode == 11u) ? 0u : 8u,0));
        data.Store2(88, uint2(mode == 6u ? 9u : (mode == 12u ? 0u : 1u),4)); data.Store2(96, addressAt(mode == 12u ? 112u : 144u)); data.Store2(104, uint2(8,0));
        data.Store2(112, uint2(0,101)); data.Store2(120, uint2(1,202));
        data.Store2(128, uint2(2,303)); data.Store2(136, uint2(3,404));
        data.Store2(144, uint2(65536,505)); data.Store2(152, uint2(1,999));
        data.Store2(160, uint2(0,888)); data.Store2(168, uint2(0xffffffff,777));
    }
}
