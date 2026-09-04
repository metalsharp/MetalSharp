// Negative fixture: u28 must not alias the internal input-record context.
RWByteAddressBuffer output : register(u28);
[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
[NodeIsProgramEntry]
void node_main() { output.Store(0, 0x12345678); }
