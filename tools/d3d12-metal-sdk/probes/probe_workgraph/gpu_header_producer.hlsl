cbuffer Addresses : register(b0) { uint addressLow; uint addressHigh; };
RWByteAddressBuffer data : register(u0);
uint2 addressAt(uint offset) {
    uint low = addressLow + offset;
    return uint2(low, addressHigh + uint(low < addressLow));
}
[numthreads(1,1,1)]
void produce() {
    data.Store2(0, uint2(0,4));
    data.Store2(8, addressAt(64));
    data.Store2(16, uint2(8,0));
    data.Store2(24, uint2(1,4));
    data.Store2(32, addressAt(96));
    data.Store2(40, uint2(8,0));
    data.Store2(64, uint2(0,101)); data.Store2(72, uint2(1,202));
    data.Store2(80, uint2(2,303)); data.Store2(88, uint2(3,404));
    data.Store2(96, uint2(65536,505)); data.Store2(104, uint2(1,999));
    data.Store2(112, uint2(0,888)); data.Store2(120, uint2(0xffffffff,777));
}
