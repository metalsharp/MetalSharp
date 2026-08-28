# MetalSharp C Backend

The C backend is MetalSharp's only HTTP backend. It listens on `127.0.0.1:9274` by default and owns routing, setup, migration, Steam/game lifecycle, bottles, launcher providers, emulator providers, diagnostics, and update integration.

## Build

```bash
make -C app/src-c
```

Output:

```text
app/src-c/build/metalsharp-backend
```

Electron packages that binary as:

```text
MetalSharp.app/Contents/Resources/runtime/metalsharp-backend
```

## Test

```bash
make -C app/src-c test
```

The test target runs:

- Backend HTTP smoke coverage on an isolated temporary home and port
- JSON unit tests
- Migration unit tests
- HTTP disconnect/SIGPIPE regression coverage
- shadPS4 update transaction tests
- PCSX2 update transaction tests
- SharpEmu transaction and hardening tests

Sanitizer validation:

```bash
make -C app/src-c asan-test
```

## Layout

- `include/metalsharp_backend/`: public C interfaces
- `runtime/backend.c`: HTTP route dispatch
- `runtime/http_server.c`: loopback HTTP server
- `runtime/main.c`: process entry point
- `runtime/steam.c` and `steam_actions.c`: Steam lifecycle and launch routes
- `runtime/bottles.c`: bottle profiles, readiness, and component repair
- `runtime/setup.c`: runtime setup and dependency installation
- `runtime/migration.c`: update migration and state preservation
- `runtime/d3dmetal.c`: GPTK/D3DMetal integration
- `runtime/epic.c`, `gog.c`, `gamejolt.c`: launcher providers
- `runtime/pcsx2.c`, `rpcs3.c`, `shadps4.c`, `sharpemu.c`: emulator providers
- `tests/`: backend tests and fixtures

## Runtime Contract

- Normal port: `127.0.0.1:9274`
- Port override: `METALSHARP_PORT`
- Runtime home: `METALSHARP_HOME` or `~/.metalsharp`
- Version: supplied by `MS_BACKEND_VERSION` from the Makefile

Temporary ports and homes are for backend validation only. Normal Electron launches use the user's real MetalSharp home and the packaged C backend.
