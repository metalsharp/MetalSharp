# Vendor Trust Kit
**Updated:** 2026-09-22

The anti-cheat path is cooperative vendor trust: a vendor or game developer can inspect MetalSharp's runtime identity, launch model, logs, and non-evasion policy without reverse-engineering the repository.

## Kit Contents

- runtime identity and version
- host ABI manifest
- signed/notarized artifact evidence when available
- Steam identity model
- compatdata sample
- launch log sample
- process tree and environment handoff explanation
- anti-cheat classification output
- no-bypass/no-spoof policy
- known unsupported boundaries
- contact and reproduction instructions

## Generator

`tools/package/create-vendor-trust-kit.sh` creates a local bundle under `dist/vendor-trust-kit/`, copies the core policy/runtime docs, and writes a `manifest.json` recording the current git commit, version files, and included documents. The kit prepares evidence for a vendor conversation.

## Required Before External Use

- Replace placeholder signing/notarization state with real notarized artifact evidence.
- Attach real launch logs from a target game.
- Attach real anti-cheat classification JSON from Launch Doctor.
- Add explicit vendor/game contact context.
