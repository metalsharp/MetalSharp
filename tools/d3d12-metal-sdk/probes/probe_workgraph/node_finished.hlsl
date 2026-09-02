RWByteAddressBuffer output : register(u0);

struct [NodeTrackRWInputSharing] INPUT_RECORD {
  uint value;
};

[Shader("node")]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(1, 1, 1)]
[NodeIsProgramEntry]
void node_main(RWDispatchNodeInputRecord<INPUT_RECORD> input) {
  output.Store(0, input.FinishedCrossGroupSharing() ? 1u : 0u);
}
