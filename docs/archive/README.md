# Archived Roadmaps

This directory holds roadmaps and planning documents that have been **completed
or superseded**. Do not cite these documents as the current implementation contract
without verifying against the code and runtime manifests.

Active roadmaps live under [`../roadmaps/`](../roadmaps/).

## Contents

### `roadmaps/`

| File | Status | Reason archived |
|------|--------|-----------------|
| [`metalsharp-final-roadmap.md`](roadmaps/metalsharp-final-roadmap.md) | Completed | Phase 0-9B all delivered on `codex/beta7-dxmt-cohesion` (PR #129). Phase 9 findings led to the typed-lowering refactor. |
| [`dx12-pipeline-complete-roadmap.md`](roadmaps/dx12-pipeline-complete-roadmap.md) | Completed | D3D12→Metal pipeline work landed across Beta 7 phases. |
| [`beta7-dxmt-cohesion-roadmap.md`](roadmaps/beta7-dxmt-cohesion-roadmap.md) | Merged | Beta 7 cohesion work landed in PRs #118, #119, #127, #128. |

## Why archive instead of delete?

These documents contain valuable context about *why* design decisions were made
and *how* features evolved. They remain useful when:

- Diagnosing a regression in a system that went through these phases.
- Onboarding a new contributor who wants the historical picture.
- Re-litigating a design decision — the original reasoning is preserved.

## Last updated: 2026-07-08
