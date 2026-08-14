use serde_json::{json, Value};
use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

const SDK_PROFILE: &str = "metalsharp";
const SUMMARY_JSON_NAME: &str = "d3d12-runtime-doctor-summary.json";
const CONTRACT_SUMMARY_JSON_NAME: &str = "contract-summary-metalsharp.json";
const CONTRACT_SUMMARY_MD_NAME: &str = "contract-summary-metalsharp.md";
const LOG_FILE_NAME: &str = "d3d12-runtime-doctor.log";

pub fn latest_cached_report(appid: u32) -> Option<Value> {
    let home = dirs::home_dir()?;
    latest_cached_report_in(&probe_runs_root(&home, appid))
}

pub fn sdk_availability() -> Value {
    match find_sdk_root() {
        Some(root) => json!({
            "available": true,
            "sdkRoot": root.to_string_lossy().to_string(),
            "runProbesScript": root.join("scripts").join("run-probes.sh").to_string_lossy().to_string(),
            "compareContractScript": root.join("scripts").join("compare-contract.py").to_string_lossy().to_string(),
        }),
        None => json!({
            "available": false,
            "summary": "D3D12 Metal SDK scripts are not available from this backend context.",
        }),
    }
}

pub fn handle_steam_d3d12_runtime_doctor(body: &serde_json::Map<String, Value>) -> Value {
    let appid = match parse_appid(body) {
        Ok(appid) => appid,
        Err(error) => return json!({"ok": false, "error": error}),
    };

    let pipeline = body
        .get("pipeline")
        .and_then(|v| v.as_str())
        .and_then(crate::mtsp::engine::PipelineId::from_str_flexible)
        .map(|pipeline| crate::bottles::resolve_steam_pipeline_for_request(appid, Some(pipeline)))
        .unwrap_or_else(|| crate::bottles::resolve_steam_pipeline_for_request(appid, None));
    let refresh = body.get("refresh").and_then(|v| v.as_bool()).unwrap_or(true);
    let windowed_present = body.get("windowedPresent").and_then(|v| v.as_bool()).unwrap_or(false);

    if !refresh {
        if let Some(report) = latest_cached_report(appid) {
            return json!({"ok": true, "cached": true, "report": report});
        }
    }

    let dual = crate::scan::resolve_dual_game_dir(appid);
    let name = crate::steam::get_game_name_from_manifest(appid).unwrap_or_else(|| format!("Game {}", appid));
    let bottle = crate::bottles::ensure_steam_game_bottle(appid, &name, dual.wine_dir.as_deref(), pipeline).ok();

    let latest = latest_cached_report(appid);
    if pipeline != crate::mtsp::engine::PipelineId::Vkd3d {
        return json!({
            "ok": true,
            "report": {
                "schema": "metalsharp.d3d12-runtime-doctor.summary.v1",
                "appid": appid,
                "name": name,
                "bottleId": bottle.as_ref().map(|b| b.id.clone()),
                "pipeline": pipeline,
                "applicable": false,
                "ready": false,
                "status": "not_applicable",
                "summary": "D3D12 runtime doctor only applies to the Vkd3d DXMT D3D12 route.",
                "sdkAvailability": sdk_availability(),
                "latestCachedReport": latest,
            }
        });
    }

    let Some(sdk_root) = find_sdk_root() else {
        return json!({
            "ok": false,
            "error": "D3D12 Metal SDK scripts not found",
            "report": {
                "schema": "metalsharp.d3d12-runtime-doctor.summary.v1",
                "appid": appid,
                "name": name,
                "bottleId": bottle.as_ref().map(|b| b.id.clone()),
                "pipeline": pipeline,
                "applicable": true,
                "ready": false,
                "status": "sdk_unavailable",
                "summary": "D3D12 Metal SDK scripts are not available from this backend context.",
                "sdkAvailability": sdk_availability(),
                "latestCachedReport": latest,
            }
        });
    };

    let home = dirs::home_dir().unwrap_or_default();
    let app_root = probe_runs_root(&home, appid);
    let timestamp = timestamp_secs();
    let run_dir = app_root.join(&timestamp);
    let _ = fs::create_dir_all(&run_dir);

    let started_at = timestamp.clone();
    let run_probes_script = sdk_root.join("scripts").join("run-probes.sh");
    let compare_script = sdk_root.join("scripts").join("compare-contract.py");
    let validate_script = sdk_root.join("scripts").join("validate-contracts.py");

    let run_output = run_probe_suite(&run_probes_script, &run_dir, windowed_present);
    let validate_output = run_python_script(&validate_script, &[]);
    let compare_output = if required_results_present(&run_dir) {
        run_python_script(
            &compare_script,
            &[
                "--results-dir",
                run_dir.to_string_lossy().as_ref(),
                "--json-out",
                run_dir.join(CONTRACT_SUMMARY_JSON_NAME).to_string_lossy().as_ref(),
                "--markdown-out",
                run_dir.join(CONTRACT_SUMMARY_MD_NAME).to_string_lossy().as_ref(),
            ],
        )
    } else {
        CommandCapture::skipped("Required probe JSON outputs were not present after the probe run.".to_string())
    };

    let loader = load_json_if_exists(&run_dir.join(format!("probe-loader-{}.json", SDK_PROFILE)));
    let agility = load_json_if_exists(&run_dir.join(format!("probe-agility-ue5-{}.json", SDK_PROFILE)));
    let caps = load_json_if_exists(&run_dir.join(format!("probe-device-caps-{}.json", SDK_PROFILE)));
    let shaders = load_json_if_exists(&run_dir.join(format!("probe-shaders-{}.json", SDK_PROFILE)));
    let queues = load_json_if_exists(&run_dir.join(format!("probe-queues-{}.json", SDK_PROFILE)));
    let render = load_json_if_exists(&run_dir.join(format!("probe-render-headless-{}.json", SDK_PROFILE)));
    let present = load_json_if_exists(&run_dir.join(format!("probe-present-windowed-{}.json", SDK_PROFILE)));
    let compare = load_json_if_exists(&run_dir.join(CONTRACT_SUMMARY_JSON_NAME));

    let validation_ok = validate_output.success;
    let compare_pass = compare.as_ref().and_then(|v| v.get("pass")).and_then(Value::as_bool).unwrap_or(false);
    let ready = run_output.success && validation_ok && compare_pass;
    let waivers = collect_waiver_ids(compare.as_ref());
    let issues = collect_issue_titles(compare.as_ref());
    let finished_at = timestamp_secs();
    let status = status_string(&run_output, validation_ok, compare.as_ref(), ready);
    let summary = summary_string(ready, waivers.len(), issues.len());

    let report = json!({
        "schema": "metalsharp.d3d12-runtime-doctor.summary.v1",
        "appid": appid,
        "name": name,
        "bottleId": bottle.as_ref().map(|b| b.id.clone()),
        "pipeline": pipeline,
        "applicable": true,
        "ready": ready,
        "status": status,
        "summary": summary,
        "profile": SDK_PROFILE,
        "windowedPresentRequested": windowed_present,
        "startedAt": started_at,
        "finishedAt": finished_at,
        "sdkAvailability": {
            "available": true,
            "sdkRoot": sdk_root.to_string_lossy().to_string(),
        },
        "artifacts": {
            "runDir": run_dir.to_string_lossy().to_string(),
            "resultsDir": run_dir.to_string_lossy().to_string(),
            "doctorLog": run_dir.join(LOG_FILE_NAME).to_string_lossy().to_string(),
            "contractSummaryJson": run_dir.join(CONTRACT_SUMMARY_JSON_NAME).to_string_lossy().to_string(),
            "contractSummaryMarkdown": run_dir.join(CONTRACT_SUMMARY_MD_NAME).to_string_lossy().to_string(),
            "latestSummaryJson": app_root.join("latest-summary.json").to_string_lossy().to_string(),
        },
        "checks": {
            "loadedDllIdentity": summarize_loader(loader.as_ref()),
            "agilitySdk": summarize_agility(agility.as_ref()),
            "featureContract": summarize_feature_contract(caps.as_ref(), compare.as_ref(), validation_ok),
            "shaderPath": summarize_shader_path(shaders.as_ref()),
            "queueFence": summarize_queue_fence(queues.as_ref()),
            "renderReadback": summarize_render_readback(render.as_ref()),
            "windowedPresent": summarize_windowed_present(present.as_ref(), windowed_present),
        },
        "waivers": waivers,
        "issues": issues,
        "commandStatus": {
            "runProbes": command_status_json(&run_output),
            "validateContracts": command_status_json(&validate_output),
            "compareContracts": command_status_json(&compare_output),
        },
        "latestCachedReportBeforeRun": latest,
        "nextActions": build_next_actions(&run_output, validation_ok, compare.as_ref()),
    });

    let report_json = serde_json::to_string_pretty(&report).unwrap_or_else(|_| "{}".to_string());
    let _ = fs::write(run_dir.join(SUMMARY_JSON_NAME), format!("{}\n", report_json));
    let _ = fs::write(app_root.join("latest-summary.json"), format!("{}\n", report_json));
    let _ = fs::write(app_root.join("latest-run.txt"), format!("{}\n", run_dir.to_string_lossy()));

    let combined_log = build_log_file(
        appid,
        &name,
        pipeline,
        &sdk_root,
        &run_dir,
        &run_output,
        &validate_output,
        &compare_output,
        &report_json,
    );
    let _ = fs::write(run_dir.join(LOG_FILE_NAME), &combined_log);
    let _ = write_global_log(appid, &timestamp, &combined_log);

    json!({"ok": true, "report": report})
}

fn parse_appid(body: &serde_json::Map<String, Value>) -> Result<u32, &'static str> {
    let Some(value) = body.get("appid") else {
        return Err("appid required");
    };
    let Some(raw) = value.as_u64() else {
        return Err("appid must be a positive numeric Steam appid");
    };
    let appid = u32::try_from(raw).map_err(|_| "appid out of range")?;
    if appid == 0 {
        return Err("appid must be greater than zero");
    }
    Ok(appid)
}

fn find_sdk_root() -> Option<PathBuf> {
    sdk_root_candidates().into_iter().find(|path| {
        path.join("scripts").join("run-probes.sh").exists()
            && path.join("scripts").join("compare-contract.py").exists()
            && path.join("contracts").join("feature-support-contract.json").exists()
    })
}

fn sdk_root_candidates() -> Vec<PathBuf> {
    let mut candidates = Vec::new();

    if let Ok(cwd) = std::env::current_dir() {
        push_sdk_candidates(&mut candidates, &cwd);
    }

    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            push_sdk_candidates(&mut candidates, dir);
        }
    }

    if let Some(resources) = crate::platform::app_resources_dir() {
        push_sdk_candidates(&mut candidates, &resources);
        candidates.push(resources.join("d3d12-metal-sdk"));
    }

    if let Some(home) = dirs::home_dir() {
        candidates.push(home.join("Dev").join("metalsharp").join("tools").join("d3d12-metal-sdk"));
        candidates.push(home.join("repos").join("metalsharp").join("tools").join("d3d12-metal-sdk"));
        candidates.push(home.join("metalsharp").join("tools").join("d3d12-metal-sdk"));
    }

    dedupe_paths(candidates)
}

fn push_sdk_candidates(candidates: &mut Vec<PathBuf>, start: &Path) {
    let mut current = Some(start);
    for _ in 0..8 {
        let Some(dir) = current else {
            break;
        };
        candidates.push(dir.join("tools").join("d3d12-metal-sdk"));
        candidates.push(dir.to_path_buf());
        current = dir.parent();
    }
}

fn dedupe_paths(paths: Vec<PathBuf>) -> Vec<PathBuf> {
    let mut seen = std::collections::HashSet::new();
    let mut deduped = Vec::new();
    for path in paths {
        let key = path.to_string_lossy().to_string();
        if seen.insert(key) {
            deduped.push(path);
        }
    }
    deduped
}

fn probe_runs_root(home: &Path, appid: u32) -> PathBuf {
    crate::platform::metalsharp_home_dir_for(&home)
        .join("pipeline-cache")
        .join("vkd3d")
        .join("probes")
        .join(appid.to_string())
}

fn latest_cached_report_in(root: &Path) -> Option<Value> {
    let latest_summary = root.join("latest-summary.json");
    if latest_summary.exists() {
        return load_json_if_exists(&latest_summary);
    }

    let mut newest: Option<(u64, PathBuf)> = None;
    for entry in fs::read_dir(root).ok()?.flatten() {
        let path = entry.path();
        if !path.is_dir() {
            continue;
        }
        let Some(name) = path.file_name().and_then(OsStr::to_str) else {
            continue;
        };
        let Ok(ts) = name.parse::<u64>() else {
            continue;
        };
        let summary = path.join(SUMMARY_JSON_NAME);
        if !summary.exists() {
            continue;
        }
        match newest {
            Some((best_ts, _)) if best_ts >= ts => {},
            _ => newest = Some((ts, summary)),
        }
    }

    newest.and_then(|(_, path)| load_json_if_exists(&path))
}

fn run_probe_suite(script: &Path, results_dir: &Path, windowed_present: bool) -> CommandCapture {
    let mut args = vec![
        script.to_string_lossy().to_string(),
        "--profile".to_string(),
        SDK_PROFILE.to_string(),
        "--results-dir".to_string(),
        results_dir.to_string_lossy().to_string(),
    ];
    if windowed_present {
        args.push("--windowed-present".to_string());
    }
    run_command("/bin/bash", &args)
}

fn run_python_script(script: &Path, args: &[&str]) -> CommandCapture {
    let mut full_args = vec![script.to_string_lossy().to_string()];
    full_args.extend(args.iter().map(|s| s.to_string()));
    run_command("python3", &full_args)
}

fn run_command(program: &str, args: &[String]) -> CommandCapture {
    match Command::new(program).args(args).output() {
        Ok(output) => CommandCapture {
            program: program.to_string(),
            args: args.to_vec(),
            status_code: output.status.code(),
            success: output.status.success(),
            stdout: String::from_utf8_lossy(&output.stdout).to_string(),
            stderr: String::from_utf8_lossy(&output.stderr).to_string(),
            skipped: false,
        },
        Err(error) => CommandCapture {
            program: program.to_string(),
            args: args.to_vec(),
            status_code: None,
            success: false,
            stdout: String::new(),
            stderr: error.to_string(),
            skipped: false,
        },
    }
}

fn required_results_present(results_dir: &Path) -> bool {
    fs::read_dir(results_dir)
        .ok()
        .into_iter()
        .flatten()
        .filter_map(Result::ok)
        .any(|entry| entry.path().extension().and_then(|ext| ext.to_str()) == Some("json"))
}

fn load_json_if_exists(path: &Path) -> Option<Value> {
    let contents = fs::read_to_string(path).ok()?;
    serde_json::from_str(&contents).ok()
}

fn summarize_loader(loader: Option<&Value>) -> Value {
    let Some(loader) = loader else {
        return json!({"pass": false, "summary": "Loader probe result missing."});
    };
    json!({
        "pass": loader.get("pass").and_then(Value::as_bool).unwrap_or(false),
        "modules": loader.get("modules").cloned().unwrap_or_else(|| json!({})),
    })
}

fn summarize_agility(agility: Option<&Value>) -> Value {
    let Some(agility) = agility else {
        return json!({"pass": false, "summary": "Agility probe result missing."});
    };
    json!({
        "pass": agility.get("pass").and_then(Value::as_bool).unwrap_or(false),
        "sdkVersion": value_at_path(agility, &["sdk", "D3D12SDKVersion"]),
        "sdkPath": value_at_path(agility, &["sdk", "D3D12SDKPath"]),
        "deviceCreateSucceeded": value_at_path(agility, &["device_create", "succeeded"]),
        "routingMatchesDxmt": value_at_path(agility, &["routing", "d3d12_expected_path_match"]),
        "modules": agility.get("modules").cloned().unwrap_or_else(|| json!({})),
    })
}

fn summarize_feature_contract(caps: Option<&Value>, compare: Option<&Value>, validation_ok: bool) -> Value {
    let Some(caps) = caps else {
        return json!({"pass": false, "summary": "Capability probe result missing.", "contractValidationPass": validation_ok});
    };
    json!({
        "pass": compare.and_then(|v| v.get("pass")).and_then(Value::as_bool).unwrap_or(false) && validation_ok,
        "contractValidationPass": validation_ok,
        "comparePass": compare.and_then(|v| v.get("pass")).and_then(Value::as_bool).unwrap_or(false),
        "featureLevel": value_at_path(caps, &["feature_levels", "max_supported"]),
        "shaderModel": value_at_path(caps, &["shader_model", "highest"]),
        "bindingTier3": value_at_path(caps, &["requirements", "binding_tier_3"]),
        "waveOps": value_at_path(caps, &["requirements", "wave_ops"]),
        "atomic64Conservative": value_at_path(caps, &["requirements", "atomic64_conservative"]),
        "advancedFeaturesConservative": value_at_path(caps, &["requirements", "advanced_features_conservative"]),
        "issues": compare.and_then(|v| v.get("issues")).cloned().unwrap_or_else(|| json!([])),
    })
}

fn summarize_shader_path(shaders: Option<&Value>) -> Value {
    let Some(shaders) = shaders else {
        return json!({"pass": false, "summary": "Shader probe result missing."});
    };
    json!({
        "pass": shaders.get("pass").and_then(Value::as_bool).unwrap_or(false),
        "dxbcVertex": value_at_path(shaders, &["dxmt_shader_paths", "dxbc_vertex_sm50"]),
        "dxbcPixel": value_at_path(shaders, &["dxmt_shader_paths", "dxbc_pixel_sm50"]),
        "dxbcCompute": value_at_path(shaders, &["dxmt_shader_paths", "dxbc_compute_sm50"]),
        "dxilToMsl": value_at_path(shaders, &["dxc", "dxil_to_msl"]),
        "dxcExitCode": value_at_path(shaders, &["compile", "dxc_exit_code"]),
        "waveProbeHr": value_at_path(shaders, &["compile", "ps_6_0_wave_probe"]),
        "bindlessStatus": value_at_path(shaders, &["dxmt_shader_paths", "bindless_descriptor_indexing"]),
    })
}

fn summarize_queue_fence(queues: Option<&Value>) -> Value {
    let Some(queues) = queues else {
        return json!({"pass": false, "summary": "Queue probe result missing."});
    };
    let copy = value_at_path(queues, &["synchronization", "copy_completed"]);
    let render = value_at_path(queues, &["synchronization", "render_completed"]);
    let compute = value_at_path(queues, &["synchronization", "compute_completed"]);
    let present = value_at_path(queues, &["synchronization", "present_completed"]);
    json!({
        "pass": queues.get("pass").and_then(Value::as_bool).unwrap_or(false),
        "signalWaitOk": hr_is_ok(value_at_path(queues, &["synchronization", "copy_signal"]))
            && hr_is_ok(value_at_path(queues, &["synchronization", "render_wait"]))
            && hr_is_ok(value_at_path(queues, &["synchronization", "compute_wait"]))
            && hr_is_ok(value_at_path(queues, &["synchronization", "present_wait"])),
        "cpuWaitOk": hr_is_ok(value_at_path(queues, &["synchronization", "cpu_wait"])),
        "readbackVerified": value_at_path(queues, &["readback", "verified"]),
        "fenceValues": {
            "copy": copy,
            "render": render,
            "compute": compute,
            "present": present,
        }
    })
}

fn summarize_render_readback(render: Option<&Value>) -> Value {
    let Some(render) = render else {
        return json!({"pass": false, "summary": "Headless render probe result missing."});
    };
    json!({
        "pass": render.get("pass").and_then(Value::as_bool).unwrap_or(false),
        "renderChangedFromClear": value_at_path(render, &["draw_checks", "render_changed_from_clear"]),
        "indexedGeometryVerified": value_at_path(render, &["draw_checks", "indexed_geometry_verified"]),
        "sampledTextureVerified": value_at_path(render, &["draw_checks", "indexed_texture_verified"]),
        "depthVerified": value_at_path(render, &["draw_checks", "depth_verified"]),
        "computeVerified": value_at_path(render, &["uav_checks", "compute_verified"]),
        "graphicsUavSupported": value_at_path(render, &["uav_checks", "graphics_supported"]),
        "checksum": value_at_path(render, &["readback", "checksum"]),
        "clearChecksum": value_at_path(render, &["readback", "clear_checksum"]),
    })
}

fn summarize_windowed_present(present: Option<&Value>, requested: bool) -> Value {
    if !requested {
        return json!({"requested": false});
    }
    let Some(present) = present else {
        return json!({"requested": true, "pass": false, "summary": "Windowed present probe was requested but no result file was produced."});
    };
    json!({
        "requested": true,
        "pass": present.get("pass").and_then(Value::as_bool).unwrap_or(false),
        "presentCount": value_at_path(present, &["present", "last_present_count"]),
        "resizeVerified": value_at_path(present, &["resize", "post_resize_verified"]),
    })
}

fn command_status_json(capture: &CommandCapture) -> Value {
    json!({
        "program": capture.program,
        "args": capture.args,
        "success": capture.success,
        "statusCode": capture.status_code,
        "skipped": capture.skipped,
    })
}

fn build_next_actions(run_output: &CommandCapture, validation_ok: bool, compare: Option<&Value>) -> Vec<String> {
    if !run_output.success {
        return vec![
            "Open the D3D12 runtime doctor log under ~/.metalsharp/logs and the per-app run directory under ~/.metalsharp/pipeline-cache/vkd3d/probes/ to inspect probe runner stderr.".to_string(),
            "Verify the local MetalSharp DXMT runtime under ~/.metalsharp/runtime/wine/lib/dxmt still contains the expected x86_64-windows and x86_64-unix assets.".to_string(),
        ];
    }

    if !validation_ok {
        return vec![
            "Fix the D3D12 Metal SDK contract files before trusting probe output, then re-run the doctor.".to_string()
        ];
    }

    let Some(compare) = compare else {
        return vec![
            "The contract comparator did not produce a summary; inspect the run directory and rerun the doctor."
                .to_string(),
        ];
    };

    if compare.get("pass").and_then(Value::as_bool).unwrap_or(false) {
        let waivers = collect_waiver_ids(Some(compare));
        if waivers.is_empty() {
            return vec!["Runtime doctor is green; move to a game-specific launch only after confirming the title still fails above the SDK layer.".to_string()];
        }
        return waivers
            .into_iter()
            .map(|waiver| format!("Retire waiver `{}` by adding the missing probe coverage before broadening D3D12 capability claims.", waiver))
            .collect();
    }

    compare
        .get("issues")
        .and_then(Value::as_array)
        .map(|issues| {
            issues
                .iter()
                .take(5)
                .filter_map(|issue| issue.get("title").and_then(Value::as_str).map(str::to_string))
                .collect::<Vec<_>>()
        })
        .filter(|issues| !issues.is_empty())
        .unwrap_or_else(|| {
            vec!["The comparator reported a failure; inspect the issue list in the contract summary JSON.".to_string()]
        })
}

fn collect_waiver_ids(compare: Option<&Value>) -> Vec<String> {
    compare
        .and_then(|v| v.get("risky_stub_checks"))
        .and_then(Value::as_array)
        .map(|items| {
            items
                .iter()
                .filter_map(|item| {
                    item.get("waiver").and_then(|w| w.get("id")).and_then(Value::as_str).map(str::to_string)
                })
                .collect()
        })
        .unwrap_or_default()
}

fn collect_issue_titles(compare: Option<&Value>) -> Vec<String> {
    compare
        .and_then(|v| v.get("issues"))
        .and_then(Value::as_array)
        .map(|items| {
            items
                .iter()
                .filter_map(|item| {
                    let title = item.get("title").and_then(Value::as_str)?;
                    let detail = item.get("detail").and_then(Value::as_str).unwrap_or("");
                    Some(if detail.is_empty() { title.to_string() } else { format!("{}: {}", title, detail) })
                })
                .collect()
        })
        .unwrap_or_default()
}

fn status_string(
    run_output: &CommandCapture,
    validation_ok: bool,
    compare: Option<&Value>,
    ready: bool,
) -> &'static str {
    if ready {
        "passed"
    } else if !run_output.success {
        "probe_run_failed"
    } else if !validation_ok {
        "contract_validation_failed"
    } else if compare.is_none() {
        "comparison_missing"
    } else {
        "comparison_failed"
    }
}

fn summary_string(ready: bool, waiver_count: usize, issue_count: usize) -> String {
    if ready {
        if waiver_count == 0 {
            return "D3D12 runtime doctor passed all required SDK checks with no active waivers.".to_string();
        }
        return format!(
            "D3D12 runtime doctor passed the required SDK checks with {} active waiver{} still recorded.",
            waiver_count,
            if waiver_count == 1 { "" } else { "s" }
        );
    }

    if issue_count > 0 {
        return format!(
            "D3D12 runtime doctor found {} contract or probe issue{}.",
            issue_count,
            if issue_count == 1 { "" } else { "s" }
        );
    }

    "D3D12 runtime doctor did not complete successfully.".to_string()
}

fn value_at_path(root: &Value, path: &[&str]) -> Value {
    let mut current = root;
    for key in path {
        let Some(next) = current.get(*key) else {
            return Value::Null;
        };
        current = next;
    }
    current.clone()
}

fn hr_is_ok(value: Value) -> bool {
    value.as_str() == Some("0x00000000")
}

#[allow(clippy::too_many_arguments)]
fn build_log_file(
    appid: u32,
    name: &str,
    pipeline: crate::mtsp::engine::PipelineId,
    sdk_root: &Path,
    run_dir: &Path,
    run_output: &CommandCapture,
    validate_output: &CommandCapture,
    compare_output: &CommandCapture,
    report_json: &str,
) -> String {
    format!(
        "timestamp: {timestamp}\nappid: {appid}\nname: {name}\npipeline: {pipeline:?}\nsdk_root: {sdk_root}\nrun_dir: {run_dir}\n\n== run-probes ==\ncommand: {run_program} {run_args}\nsuccess: {run_success}\nstatus: {run_status:?}\nstdout:\n{run_stdout}\nstderr:\n{run_stderr}\n\n== validate-contracts ==\ncommand: {validate_program} {validate_args}\nsuccess: {validate_success}\nstatus: {validate_status:?}\nstdout:\n{validate_stdout}\nstderr:\n{validate_stderr}\n\n== compare-contract ==\ncommand: {compare_program} {compare_args}\nsuccess: {compare_success}\nstatus: {compare_status:?}\nstdout:\n{compare_stdout}\nstderr:\n{compare_stderr}\n\n== summary ==\n{report_json}\n",
        timestamp = timestamp_secs(),
        appid = appid,
        name = name,
        pipeline = pipeline,
        sdk_root = sdk_root.to_string_lossy(),
        run_dir = run_dir.to_string_lossy(),
        run_program = run_output.program,
        run_args = run_output.args.join(" "),
        run_success = run_output.success,
        run_status = run_output.status_code,
        run_stdout = run_output.stdout,
        run_stderr = run_output.stderr,
        validate_program = validate_output.program,
        validate_args = validate_output.args.join(" "),
        validate_success = validate_output.success,
        validate_status = validate_output.status_code,
        validate_stdout = validate_output.stdout,
        validate_stderr = validate_output.stderr,
        compare_program = compare_output.program,
        compare_args = compare_output.args.join(" "),
        compare_success = compare_output.success,
        compare_status = compare_output.status_code,
        compare_stdout = compare_output.stdout,
        compare_stderr = compare_output.stderr,
        report_json = report_json,
    )
}

fn write_global_log(appid: u32, timestamp: &str, contents: &str) -> std::io::Result<()> {
    let log_dir = crate::platform::metalsharp_home_dir().join("logs");
    fs::create_dir_all(&log_dir)?;
    fs::write(log_dir.join(format!("d3d12-runtime-doctor-{}-{}.log", appid, timestamp)), contents)
}

fn timestamp_secs() -> String {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_secs().to_string()
}

#[derive(Debug, Clone)]
struct CommandCapture {
    program: String,
    args: Vec<String>,
    status_code: Option<i32>,
    success: bool,
    stdout: String,
    stderr: String,
    skipped: bool,
}

impl CommandCapture {
    fn skipped(detail: String) -> Self {
        Self {
            program: "skipped".to_string(),
            args: Vec::new(),
            status_code: None,
            success: false,
            stdout: String::new(),
            stderr: detail,
            skipped: true,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_root(name: &str) -> PathBuf {
        let root =
            std::env::temp_dir().join(format!("metalsharp-d3d12-runtime-doctor-{}-{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).unwrap();
        root
    }

    #[test]
    fn latest_cached_report_prefers_latest_summary_file() {
        let root = temp_root("latest-summary");
        let latest = root.join("latest-summary.json");
        fs::write(&latest, "{\"ready\":true}\n").unwrap();

        let report = latest_cached_report_in(&root).unwrap();
        assert_eq!(report.get("ready").and_then(Value::as_bool), Some(true));

        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn latest_cached_report_scans_timestamp_directories() {
        let root = temp_root("scan-dirs");
        let old_dir = root.join("100");
        let new_dir = root.join("200");
        fs::create_dir_all(&old_dir).unwrap();
        fs::create_dir_all(&new_dir).unwrap();
        fs::write(old_dir.join(SUMMARY_JSON_NAME), "{\"run\":1}\n").unwrap();
        fs::write(new_dir.join(SUMMARY_JSON_NAME), "{\"run\":2}\n").unwrap();

        let report = latest_cached_report_in(&root).unwrap();
        assert_eq!(report.get("run").and_then(Value::as_u64), Some(2));

        let _ = fs::remove_dir_all(&root);
    }
}
