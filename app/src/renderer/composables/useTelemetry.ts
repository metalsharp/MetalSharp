import posthog from "posthog-js";

interface PostHogBuildConfig {
  projectToken?: string;
  host?: string;
}

let initialized = false;
let enabled = false;
let handlersInstalled = false;
let buildConfig: PostHogBuildConfig = {};

export function setTelemetryBuildConfig(config: PostHogBuildConfig) {
  buildConfig = config;
}

function installExceptionHandlers() {
  if (handlersInstalled) return;
  handlersInstalled = true;
  window.addEventListener("error", () => {
    captureTelemetry("renderer_error", { kind: "window_error" });
  });
  window.addEventListener("unhandledrejection", () => {
    captureTelemetry("renderer_error", { kind: "unhandled_rejection" });
  });
}

/** Enable or disable developer diagnostics. The app defaults this to enabled. */
export async function configureTelemetry(allowDeveloperTelemetry: boolean): Promise<boolean> {
  enabled = allowDeveloperTelemetry;
  const { projectToken, host } = buildConfig;
  if (!projectToken || !host) return false;

  if (!initialized) {
    posthog.init(projectToken, {
      api_host: host,
      autocapture: false,
      capture_pageview: false,
      capture_pageleave: false,
      disable_session_recording: !allowDeveloperTelemetry,
      session_recording: { maskAllInputs: true, maskTextSelector: "*" },
      opt_out_capturing_by_default: !allowDeveloperTelemetry,
      person_profiles: "never",
    });
    initialized = true;
    installExceptionHandlers();
  }

  if (allowDeveloperTelemetry) {
    posthog.opt_in_capturing();
    posthog.startSessionRecording();
  } else {
    posthog.stopSessionRecording();
    posthog.opt_out_capturing();
  }
  return true;
}

export function captureTelemetry(event: string, properties?: Record<string, string | number | boolean>) {
  if (initialized && enabled) posthog.capture(event, properties);
}

export function captureTelemetryException(_error: unknown) {
  captureTelemetry("renderer_error", { kind: "vue_error" });
}

/** Free-text is captured only when a user explicitly submits this form. */
export function submitDeveloperFeedback(message: string): boolean {
  const feedback = message.trim().slice(0, 4000);
  if (!initialized || !enabled || !feedback) return false;
  posthog.capture("developer_feedback_submitted", { feedback });
  return true;
}
