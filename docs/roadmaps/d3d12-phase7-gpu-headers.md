# Phase 7 GPU-generated entry headers

Status: bounded D3D12 GPU-generated headers for thread, coalescing,
broadcasting, and the covered multi-node descriptor forms pass. Other launch
shapes and breadth remain open; GPU-generated headers are not generally
certified and Phase 7 remains open.

User-selected architecture: GPU-selected native ICB entry dispatch, not
GPU-gated passes for every possible entrypoint.

## Implemented

- Atomic program snapshots include the explicit entrypoint count and canonical
  shader slots. Prepared indirect pipelines preserve unavailable slot indices.
- Buffer-range snapshots retain native owners and all same-address aliases.
  Protected resources are excluded. Placed-buffer native offsets are retained.
- Sorted prefix-maximum address bounds preserve containing aliases without
  falsely joining adjacent allocations. Arithmetic rejects wraparound.
- Header validation supports zero-stride replication, exact final-record spans
  without trailing padding, empty input, zero work and unavailable entrypoints.
- GPU-selected pipeline arrays bind records, output, backing and context buffers.
  Thread streams use **one command**, with GPU record count determining dispatch
  size—not a command-capacity limit per record. Version-8/9 contexts select the
  current record by group while preserving routing/recursion state.
- GPU validation gates allocator reset. Invalid headers and zero work leave
  backing untouched. Status gates downstream scheduling so stale publications
  cannot be consumed.
- Programs with thread, coalescing, and broadcasting entrypoints use this path
  for single-node GPU input. Thread and coalescing streams use one indirect
  command; broadcasting uses one command per record. Coalescing
  uses GPU-selected two-record batches, and fixed-grid broadcasting preserves
  exact dense/sparse output. Mixed coalescing/thread and broadcasting/thread
  graphs pass exact output and group/bitmap counters. A
  D3D12_MULTI_NODE_GPU_INPUT header with two GPU descriptors is validated and
  compacted into two ICB commands without host descriptor reads. Other shapes
  retain the existing path rather than silently losing work.
- Two ordinary-compute defects exposed by the producer were fixed: the fallback
  sampler now has valid anisotropy, and root constants use retained buffer
  storage compatible with generic MSL buffer qualifiers.

## Evidence

`/private/tmp/wg-gpu-headers/full-current16/` contains the fresh full
phase7-workgraph result set, including the GPU-header variants, with exact
readback and no failed assertion. It uses the staged runtime at
`/Volumes/AverySSD/metalsharp-phase7-source.tjzA3h/gpu-header-current12/runtime`;
`/private/tmp/wg-gpu-headers/abi-current16/` is the matching paired Winemetal
ABI result. The stage records `source_dirty=true`, so this is not clean-source
reproducibility or release provenance.

The new D3D12 probe starts with poisoned header/payload storage. An ordinary
compute shader writes headers, entrypoint indices, counts, GPU payload
addresses and record data. Thread, coalescing, and broadcasting variants
produce exact dense/sparse output; the coalescing variant additionally reports
two groups and a batch-size bitmap (`values[5:7] == [2,4]`). There is no
intermediate readback. The single-node retained traces have
`host_header_read=0`; the multi-node trace has one event with the two
GPU-produced descriptors compacted into its command range.

Separate native evidence checks pipeline selection, invalid entrypoints,
command-capacity rejection, zero work, replication, empty input and gated reset.
Those native artifacts correctly retain `d3d12_integrated=false`; the D3D12
probe is separate. Host/GPU arithmetic tests cover aliasing and header validation.

Development-only `D3D12_METAL_SDK_PROBE_FILTER` selects result-name substrings in
run-probes.sh. Normal runs delete profile-scoped JSON, traces, and generated
node fixtures before starting; set `D3D12_METAL_SDK_KEEP_RESULTS=1` only for
forensic preservation. Use fresh result directories; filtered runs are not
full-profile or promotion evidence.

## Combined bounded checkpoint

The collected results at `/Volumes/AverySSD/phase7-combined-current/` pass all
61 declared Phase 7 evidence rows. The aggregate still fails on manifest status
`partial`; this is intentional, not permission to promote WorkGraphsTier.
Mesh evidence at `/Volumes/AverySSD/phase7-mesh-next/results/` passes all 11
checks (313/350 direct/indirect pixels, 256-byte payload, and statistics).
Geometry evidence at `/Volumes/AverySSD/phase7-geometry-next/results/` links
four of four fixtures; it does not establish general draw/readback coverage.
Both runs explicitly skipped the runner's strict prefix ABI gate and used the
previously paired-audited current12 runtime. Strict release ABI remains open.
The runner now audits the selected Wine installation rather than deriving its
ABI path from the externally staged DXMT route. Its PE-copy audit covers both
aliases and same-named application-directory DLLs; an isolated mutation check
confirms stale same-named `d3d12.dll` is rejected.

### Full backing-preservation follow-up

`/Volumes/AverySSD/phase7-full-backing/results/` passes the positive multi-input
control plus all six negative/empty variants, including a valid first child
followed by an invalid second child. Rejection and empty-work probes compare
all 2 MiB of backing memory against an offset-varying sentinel, covering
allocator metadata, payload slots, and trailing bytes—not only the old 80-byte
prefix. `/Volumes/AverySSD/phase7-full-backing-overdepth/results/` also passes
the recursion overdepth regression with the same full-allocation check.

### GPU record replication

`/Volumes/AverySSD/phase7-replication/results/` passes all eight multi-input
variants. A compute-produced descriptor with count four and record stride zero
replicates value 101, yielding an atomic downstream sum of 404 while the
independent sparse child yields 505. The replication-specific shader uses
`InterlockedAdd`, avoiding overlapping non-atomic stores and proving record
multiplicity rather than merely observing one final value. All six negative/
empty variants continue to preserve the full backing allocation. This proves
zero **record** stride, not zero descriptor-table stride or exhaustive capacity.

### Coalescing replication

`/Volumes/AverySSD/phase7-coalescing-replication/results/` passes all twelve
GPU-header variants. The coalescing zero-record-stride case yields
`[404,0,0,0,505,2,4,0,0,0,0,0,0,0,0,0]`: exact atomic payload sum,
independent sparse routing, two batches, and a two-record batch-size bitmap.
Multi-input coalescing is explicitly kept in dispatch mode 3 rather than being
accidentally redirected to the single-header mode. Partial batches and larger
replicated streams remain unproven.

### Partial coalescing batch

`/Volumes/AverySSD/phase7-partial-coalescing/results/` verifies three
GPU-produced zero-stride records in batches of two and one. Exact output is
`[303,0,0,0,505,2,6,0,0,0,0,0,0,0,0,0]`; the bitmap six witnesses both
batch sizes. Full-batch replication, ordinary GPU-header coalescing, and
coalescing recursion pass in the same focused run. This is bounded partial-
batch evidence, not larger-capacity or cross-queue replication certification.

### Multi-group broadcasting correction

The expanded two-group fixture failed against current12 with only one group's
contribution (`[101,202,303,404,505,...]`). Broadcasting ICBs bind a single record
per command, so they must retain the raw single-record context instead of the
version-8/9 stream context that indexes records by group. After that correction,
`/Volumes/AverySSD/phase7-broadcast-after/results/` passes all thirteen GPU-header
variants, including exact broadcasting sums `[202,404,606,808,505,...]`.
The failed control is retained at `/Volumes/AverySSD/phase7-broadcast-before/`.
The rebuilt runtime is `/Volumes/AverySSD/phase7-broadcast-fixed-stage/runtime`;
paired ABI evidence is `/Volumes/AverySSD/phase7-broadcast-fixed-abi/` (optional
prefix, not strict release ABI). The 13-artifact manifest was regenerated for
this stage. Clean-source release reproducibility remains unproven.

### XYZ broadcasting follow-up

`/Volumes/AverySSD/phase7-broadcast-xyz/results/` extends the fixed-grid
GPU-header fixture to 2x2x2 groups. Exact atomic contributions are
`[808,1616,2424,3232,505,0,0,0,0,0,0,0,0,0,0,0]`; the independent sparse
entry remains unchanged. This verifies eight contributions per dense input
record across all three grid axes, not general system-value or thread shapes.

### GPU table-address alignment

`/Volumes/AverySSD/phase7-alignment/results/` passes all fourteen GPU-header
variants against `/Volumes/AverySSD/phase7-alignment-stage/runtime`. Multi-input
validation now checks the table address's 8-byte alignment before dereferencing
child descriptors, in addition to its existing stride checks. A GPU-produced
address offset by one byte rejects with all 2 MiB of backing and user output
unchanged. This stage remains development evidence, not clean reproducibility.

## Remaining

- Multi-node broadcasting, duplicate/zero-stride descriptors, larger descriptor
  tables and mixed invalid entries; the current two-descriptor and
  zero-descriptor results are bounded evidence only.
- Exact D3D12 GPU-header rejection, zero-work and replication breadth; existing
  host-read/CPU-input zero-stride restrictions also need closure.
- Alias lifetime, sparse resources, protected-resource exclusion, and resources
  registered after submission but before a queued dependency releases. Encoding-
  time snapshots alone do not prove these supported.
- Full input/output capacity and backing-memory guarantees; existing bounded
  allocator limits are not exhaustive support.
- Broader compute regression for the constant/sampler fixes, strict ABI audit,
  clean staging and independent reproducibility. All other Phase 7 gates remain.
