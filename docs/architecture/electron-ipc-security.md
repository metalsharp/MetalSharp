# Electron IPC Security Contract
**Updated:** 2026-08-11

MetalSharp's renderer is an untrusted caller. A renderer compromise must not
turn the preload bridge into a generic command runner, arbitrary local HTTP
client, or privileged installer launcher.

## Trust-boundary rules

- `backend:request` accepts only the exact `GET` and `POST` method/path pairs
  used by the renderer. Query-bearing endpoints validate their query keys and
  values (`appid`, `prefix`, and log offsets) instead of accepting arbitrary
  URLs. Migration cleanup and update-cache cleanup are main-process-only
  routes.
- `app:install-deps` accepts a discriminated action union. Homebrew actions
  select one of the checked-in package names; script actions select one of the
  checked-in script names. The main process resolves scripts from the one
  packaged/development `scripts` directory and never parses a renderer command
  string.
- `updater:spawn-install` takes no renderer-controlled path, PID, or version.
  The main process asks the backend for the downloaded artifact, then requires
  a regular, non-symlink DMG under the configured MetalSharp update cache whose
  filename version exactly matches the backend-reported version. The backend
  PID and application PID are obtained by trusted main-process code.

The allowlist is intentionally explicit. Adding a renderer backend call
requires adding its method/path and, when applicable, a query validator to
`app/src/main/ipc-security.ts`, plus a renderer call-site test or review of the
corresponding backend contract.

## Compatibility

The renderer's normal backend routes and update workflow remain unchanged from
the user's perspective. The updater preload method is now parameterless, and
the unused free-form dependency command interface is replaced by the typed
action union. Backend route implementations remain the source of truth for
their request bodies and existing process/PID containment checks.
