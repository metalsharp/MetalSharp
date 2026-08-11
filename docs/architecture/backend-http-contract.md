# Backend HTTP Request Contract
**Updated:** 2026-08-11

MetalSharp's Rust backend listens on the loopback HTTP interface and receives
JSON requests from the Electron bridge and local diagnostic tools. Every route
that consumes a JSON body uses the same bounded body reader in
`app/src-rust/src/main.rs`.

## Request-body rules

- The request body must be a JSON object. Empty bodies, malformed JSON, and
  valid JSON values that are not objects are invalid requests.
- The body limit is **16 MiB** (`16 * 1024 * 1024` bytes). A known
  `Content-Length` above the limit is rejected before reading from the socket.
  Chunked or otherwise unknown-length bodies are read through a limit of one
  extra byte so the backend can detect overflow without buffering the rest.
- A body over the limit returns HTTP **413 Payload Too Large**.
- Body read failures and JSON parse/type failures return HTTP **400 Bad
  Request**. The route handler is not called after a body error.
- POST routes that do not consume a JSON body retain their existing empty-body
  behavior; the contract applies when a route calls the shared body reader.

Responses use the existing JSON envelope:

```json
{"ok":false,"error":"..."}
```

This boundary prevents a local caller or a browser-driven request from making
the backend buffer an unbounded payload. It also keeps malformed input from
being silently converted into `{}` and then reported as a misleading
route-specific validation error.

Regression coverage for valid objects, malformed/non-object JSON, bounded
streams, known oversized content lengths, and status codes lives in the Rust
backend tests in `app/src-rust/src/main.rs`.
