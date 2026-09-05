# Native Metal 4 Work Graph ICB feasibility probe

This validates the chosen GPU-generated indirect-command path on the host.
It does **not** implement or certify D3D12 downstream dynamic-grid dispatch.
Winemetal integration and D3D12 end-to-end evidence are separate requirements.
The source-owned `probe_workgraph_chain.cpp` now exercises bounded integration
in `dynamic-consumer`, `dynamic-consumer-u16`, `dynamic-consumer-empty`,
`dynamic-consumer-zero-grids`, and `dynamic-consumer-repeated` modes. These
cases are run by the SDK's Work Graph runner; this standalone probe still
reports `d3d12_integrated=false` because it does not invoke D3D12.

With `DEVELOPER_DIR` selecting the pinned Xcode beta:

```sh
tmp=$(mktemp -d)
scripts=tools/d3d12-metal-sdk/scripts
xcrun --sdk macosx clang++ -std=c++17 -fobjc-arc -Wall -Wextra -Werror \
  -mmacosx-version-min=26.0 "$scripts/probe-metal-workgraph-icb.mm" \
  -framework Metal -framework Foundation -o "$tmp/probe-icb"
xcrun --sdk macosx metal -std=metal4.0 \
  -c "$scripts/probe-metal-workgraph-icb.metal" -o "$tmp/probe.air"
MTL_DEBUG_LAYER=1 "$tmp/probe-icb" "$scripts/probe-metal-workgraph-icb.metal" \
  > "$tmp/active.json"
MTL_DEBUG_LAYER=1 "$tmp/probe-icb" "$scripts/probe-metal-workgraph-icb.metal" empty \
  > "$tmp/empty.json"
```

The host creates an empty ICB, pipelines with explicit indirect-command
support, argument bindings, and residency for the buffers, ICB and referenced
pipelines. A GPU kernel encodes per-record compute commands and their execution
range. No CPU command generation or intermediate readback chooses which
commands execute. Input records are populated only after command-buffer
encoding. Cross-encoder visibility uses an explicit Metal 4 device barrier.

Required results:

- Active range `[1,4]`: `[10,62,602,0,0,0,0,0]`. This includes non-unit X/Y/Z
  grids, a zero-grid command, and different per-command record pointers.
- Empty range `[1,0]`: eight zero words.
- A fifth, GPU-encoded poison command outside both execution ranges must never
  change output word 4.
- The shared-event wait and commit-feedback callback must complete without a
  GPU error. Both results must report `pass=true`, `gpu_completion_ok=true`,
  and `d3d12_integrated=false`.

## Windows PE / Unix bridge probe

`probes/probe_workgraph_icb_bridge.cpp` is built by `build-probes.sh` and run in
active/empty modes by `run-probes.sh --work-graph-only`. It resolves Winemetal
exports dynamically, so building the probe does not require a build-directory
import library. The runner pins the PE DLL and selects its matching Unix half
through the explicit temporary loader alias.

This probe exercises the current replay encoder API (`MTLCommandQueue` /
`MTLComputeCommandEncoder`) using native GPU ICBs. The Metal 4 command-buffer
probe above remains a separate proof; neither result alone closes D3D12
record-driven downstream broadcasting. Indirect resources have explicit usage
declarations, and ICBs, pipelines and buffers stay retained through completion.

The bridge adds append-only Unix slots 182–185, identical in normal and WoW64
tables, for ICB creation, ICB/pipeline GPU IDs and indirect-capable compute PSOs.
Existing pipeline-info layout is unchanged. The new ICB creation packet is 24
bytes; the indirect execution command is 40 bytes, both asserted at compile time.
`vendor/dxmt/tests/dxil/test_winemetal_icb_abi.cpp` checks their offsets as well:

```sh
for bits in 32 64; do
  x86_64-w64-mingw32-g++ -m$bits -std=c++17 -Ivendor/dxmt/src/winemetal \
    -c vendor/dxmt/tests/dxil/test_winemetal_icb_abi.cpp -o "$tmp/icb-abi-$bits.o"
done
```

Both compile checks pass; this is wire-layout evidence, not a WoW64 runtime
execution certification.

The PE probe checks null handles, zero command capacity, excessive kernel
buffer bindings, null pipeline descriptors, null encoders, misaligned range
offsets and short range buffers. Invalid execution packets must return failure
before reaching Metal. Completion is bounded and checked; active/empty results
match the native proof, including the excluded poison command. JSON reports
`d3d12_integrated=false` for these bridge-only tests. The separate D3D12
integration modes above verify replay's native-ICB path.

The first successful source-owned runs were on Apple M4 with Metal API
Validation enabled. Evidence is retained in `/private/tmp/wg-native-icb/`;
those temporary files are development evidence, not release artifacts.
