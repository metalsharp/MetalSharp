# Developer Diagnostics and Feedback
**Updated:** 2026-08-08

MetalSharp uses PostHog Cloud in the US region (`https://us.i.posthog.com`) for operational diagnostics and product feedback.

## User control

`developerTelemetry` is stored in `~/.metalsharp/configs/config.json` and defaults to `true` for new installs. The Settings toggle, **Allow developers to receive logs, errors, and crash reports**, disables future PostHog capture and session recording immediately when turned off. The preference survives runtime migration.

## Collected signals

When enabled, the renderer captures only these operational signals:

- `backend_ready` or `backend_unavailable` at app startup, including whether this is the first observed ready state on that installation.
- `backend_connection_lost` and `backend_recovered` from the existing backend health poll.
- Renderer error categories (`window_error`, `unhandled_rejection`, or `vue_error`) without exception messages, stacks, or payloads.
- Session recordings with every input and all text masked.
- `developer_feedback_submitted` only after the user explicitly presses **Send Feedback** in Settings. The submitted message is limited to 4,000 characters.

The application does not identify people or automatically send Steam API keys, passwords, local file paths, game-library names, raw log files, arbitrary backend responses, or other user-entered input values.

## Build and release configuration

The PostHog project write key is deliberately excluded from Git. For local builds, copy `app/.env.example` to ignored `app/.env` and set `VITE_POSTHOG_PROJECT_TOKEN`. Release CI receives the key from the repository secret `POSTHOG_PROJECT_TOKEN` and injects it only while building the renderer. The write key is intended for client delivery; account credentials and personal API keys must never be added to source or workflow files.
