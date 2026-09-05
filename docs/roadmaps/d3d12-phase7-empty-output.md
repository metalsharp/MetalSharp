# Phase 7 empty-output publication

Status: bounded thread/group publication to coalescing EmptyNodeInput verified.
Phase 7 remains incomplete.

- EmptyNodeOutput metadata retains zero size/alignment and no payload type.
  Input flags are rejected in output declarations. Owned metadata tests cover
  both producer launch forms and the consumer's MaxRecords(4).
- Runtime IncrementOutputCount lowers scalar counts to GPU allocation/publication
  rather than a local counter. Group requests reserve once and share the handle;
  zero requests allocate nothing. Legacy standalone opcode scaffolding is separate.
- The allocator tracks logical slots/descriptors; no record payload is invented
  or dereferenced. Preflight requires explicit empty input at the target, not an
  argument-free node, and preserves nonempty layout mismatch rejection.
- Required target resolution uses immutable routing tables. Missing required
  consumers reject during creation.

Evidence: `/private/tmp/wg-empty-output/mismatch/` has 51 passing Work Graph/bridge
results, with Metal API Validation and no failed assertion. This is dirty
incremental development staging, not clean-release provenance.

The active probe sends six records from a thread producer and six from a
broadcasting producer with two threads. The result is `[12,4,20,3]` and a zero
tail: record total, coalescing invocation count, batch-size bitmap, producer
invocations. Final allocator counters prove the group request reserved once.
The zero-count probe returns `[0,0,0,3]` and verifies zero record/allocation counts.
Both mutate CPU input after recording. A payload-bearing consumer connected to
an empty output rejects during replay preflight, with no producer UAV writes and
zero record/allocation counts. This is not creation-time layout rejection. JSON includes exact UAV and allocator
checks; the native ICB recursion trace remains independently retained.

Remaining: empty-output arrays, recursive empty routing, other consumer launch
forms, cross-queue/GPU input breadth, declared limits and overflow/recycling,
strict ABI/clean staged checkpoint, independent reproducibility, and the rest
of the Phase 7 exit requirements.
