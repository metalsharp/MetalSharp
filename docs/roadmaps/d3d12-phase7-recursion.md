# Phase 7 self-recursion

Status: bounded thread, coalescing and native-ICB broadcasting self-recursion pass; exhaustive Phase 7 remains open.

User selected depth-expanded GPU indirect passes rather than a persistent GPU
queue redesign. The host encodes the declared finite depth; GPU publication and
record counts determine active work. No intermediate readback or CPU record
scheduler was added. Recursive native-ICB broadcasting now has exact execution
and queue-trace evidence.

## Implementation

- DXIL tag 19 is retained in owned node metadata and registry snapshots.
- Actual self-edge presence is separate from a declaration. Self-edges require
  a nonzero declared maximum. Other cycles remain invalid.
- Node-budget accounting includes array spans plus every recursion declaration.
- Version-6 raw and version-7 descriptor contexts preserve the routing prefix
  and add remaining levels in a 64-byte layout. Scheduler and ICB generation
  preserve the extension. Routing-aware queries read it; ordinary nonrecursive
  contexts report zero, even when a maximum was declared.
- Leaf self-output IsValid is false. Other destinations are unaffected.
- Preflight expands self-edges against the existing depth-32/visit-1024 bounds
  before upstream writes; replay follows that finite expansion.
- Publication words retain recursion level in upper bits. Completion ORs in the
  publish bit, and each scheduled pass filters its source level. This prevents
  multiple self-output ports from mixing records across recursion levels.

## Evidence

`/private/tmp/wg-recursion/batches-final/`: all 48 official Work Graph/bridge results
pass, with Metal API Validation and no failed assertion. The strict Winemetal ABI
check passes under `/private/tmp/wg-recursion/abi/`; context layout assertions
compile for both 32-bit and 64-bit Windows targets. This is dirty development
staging, not a clean-source/release checkpoint.

- One self-output port produces `[4,3,2,1,0,99,14]` with a zero tail.
- Two ports produce `[32,12,4,1,0,99,14]` with a zero tail, distinguishing publication
  generations rather than collapsing records into the wrong level.
- The value 14 is the validity bitmap for remaining levels 3,2,1, with leaf 0
  invalid. Both ports must agree. A declaration-only nonrecursive node reports
  zero remaining levels and writes its input value 99 independently.
- Early termination produces `[0,0,2,1,0,99,12]`: remaining encoded levels do no
  work even though the stopping node's self-output is still valid.
- Record-driven broadcasting through native ICBs preserves the single-port
  `[4,3,2,1,0,99,14]` result. The retained `workgraph-recursion-icb-phase7-workgraph.trace.log` contains
  exactly three `WorkGraph native ICB` entries. The official runner now retains
  this trace beside the result on future runs.
- Coalescing produces `[22,18,14,10,0,99,14,0,8,4]`: four records travel through
  four levels, with eight two-record batches independently verified.
- The legal depth boundary executes 32 invocations with value sum 528,
  remaining-level sum 496, full level bitmap and leaf-excluding validity bitmap.
- One level beyond rejects before recursive writes or backing-table reset, while
  an independent nonrecursive dispatch still writes 99. The probe does not
  request separate SetProgram initialization when checking backing preservation.
- Post-recording CPU input mutation does not change execution.
- The old cycle-rejection fixture was a legal self-edge. It now exercises an
  illegal two-node cycle and still verifies no upstream writes.
- Separate native helper tests cover versions 6/7 and leaf validity; host tests
  cover metadata ownership, missing declarations and recursion budget charges.

## Remaining

- Partial coalescing batches and broader broadcasting/ICB/resource combinations,
  GPU input and cross-queue cases.
- Mixed-node depth boundaries and broader termination/no-write rejection cases.
- Complete creation-time graph-depth/override validation and general topology
  scalability beyond the current visit/allocator bounds.
- General resources/local roots, overflow/recycling and output-limit semantics.
- Clean staged checkpoint and independent reproducibility for these changes;
  all other Phase 7 exit requirements remain mandatory.

Reference: https://microsoft.github.io/DirectX-Specs/d3d/WorkGraphs.html
(recursion, node limits, shader function attributes and remaining-level query).
