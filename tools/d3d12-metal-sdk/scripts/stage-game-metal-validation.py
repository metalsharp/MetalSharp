#!/usr/bin/env python3
"""Stage captured VKD3D game shader corpora into an offline Metal validation lab.

This is intentionally file-based and safe to run without launching a game. It:

1. Discovers captured DXMT shader-cache directories.
2. Optionally runs DXMT's native DXIL converter to regenerate MSL.
3. Compiles MSL/Metal sources into AIR and metallib with Apple's Metal tools.
4. Builds and runs a tiny Objective-C++ Metal framework harness that loads each
   metallib and creates compute pipeline states for kernel functions.

Outputs are staged under /Volumes/AverySSD by default so internal storage does
not absorb large corpora.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any


SCRIPT_DIR = Path(__file__).resolve().parent
SDK_ROOT = SCRIPT_DIR.parent
PROJECT_ROOT = SDK_ROOT.parent.parent
DEFAULT_STAGE_ROOT = Path("/Volumes/AverySSD/MetalSharp-VKD3D-CorpusLab")
DEFAULT_CONVERTER = PROJECT_ROOT / "vendor" / "dxmt" / "build-metalsharp-x64" / "tests" / "test_dxil_converter"


def run(command: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def safe_label(path: Path) -> str:
    parts = [part for part in path.parts[-5:] if part not in {"/", "shader-cache", "vkd3d"}]
    label = "__".join(parts) or path.name or "corpus"
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", label)[:160]


def resolve_xcrun_tool(name: str) -> str:
    completed = run(["xcrun", "--find", name])
    if completed.returncode != 0 or not completed.stdout.strip():
        raise RuntimeError(f"unable to resolve {name} via xcrun: {completed.stderr.strip()}")
    return completed.stdout.strip()


def default_corpora() -> list[Path]:
    roots: list[Path] = []
    home = Path.home()

    global_vkd3d = home / ".metalsharp" / "shader-cache" / "vkd3d"
    if global_vkd3d.is_dir():
        roots.extend(path for path in sorted(global_vkd3d.iterdir()) if path.is_dir())

    tmp_root = home / ".metalsharp" / "tmp"
    if tmp_root.is_dir():
        roots.extend(sorted(path for path in tmp_root.glob("*vkd3d*/**/shader-cache") if path.is_dir()))

    steam_root = Path("/Volumes/AverySSD/SteamLibrary/steamapps/common")
    if steam_root.is_dir():
        roots.extend(sorted(path for path in steam_root.glob("*/.metalsharp-cache/shader-cache/vkd3d/*") if path.is_dir()))

    deduped: list[Path] = []
    seen: set[str] = set()
    for root in roots:
        key = str(root.resolve()) if root.exists() else str(root)
        if key in seen:
            continue
        seen.add(key)
        if any(root.glob("*.dxbc")) or any(root.glob("*.dxil")) or any(root.glob("*.msl")) or any(root.glob("*.metal")):
            deduped.append(root)
    return deduped


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def run_converter(converter: Path, corpus: Path, out_dir: Path) -> dict[str, Any]:
    out_dir.mkdir(parents=True, exist_ok=True)
    command = [str(converter), str(corpus), "--dump-msl", str(out_dir)]
    completed = run(command)
    write_text(
        out_dir / "dxmt-converter.log",
        "command=" + " ".join(command) + "\n"
        f"returncode={completed.returncode}\n"
        "stdout:\n"
        + completed.stdout
        + "\nstderr:\n"
        + completed.stderr,
    )
    generated = sorted(out_dir.glob("*.metal")) + sorted(out_dir.glob("*.msl"))
    return {
        "command": command,
        "returncode": completed.returncode,
        "ok": completed.returncode == 0,
        "generated_msl_count": len(generated),
        "log": str(out_dir / "dxmt-converter.log"),
    }


FUNCTION_RE = re.compile(r"\b(vertex|fragment|kernel)\s+[^;\n{]*?\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(")


def parse_functions(source: Path) -> list[dict[str, str]]:
    try:
        text = source.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    functions: list[dict[str, str]] = []
    seen: set[tuple[str, str]] = set()
    for match in FUNCTION_RE.finditer(text):
        key = (match.group(1), match.group(2))
        if key in seen:
            continue
        seen.add(key)
        functions.append({"stage": match.group(1), "name": match.group(2)})
    return functions


def metal_compile_flags(source: Path) -> list[str]:
    return [
        "-x",
        "metal",
        "-std=metal3.0",
        "-Wno-unused-variable",
        "-Wno-unused-function",
        "-Wno-implicit-function-declaration",
        "-Wno-incompatible-pointer-types",
        "-Wno-int-conversion",
        "-c",
        str(source),
    ]


def compile_metal_source(source: Path, label: str, out_root: Path, metal_tool: str, metallib_tool: str) -> dict[str, Any]:
    stem = source.stem
    air = out_root / "air" / label / f"{stem}.air"
    metallib = out_root / "metallib" / label / f"{stem}.metallib"
    log = out_root / "logs" / label / f"{stem}.log"
    air.parent.mkdir(parents=True, exist_ok=True)
    metallib.parent.mkdir(parents=True, exist_ok=True)
    log.parent.mkdir(parents=True, exist_ok=True)

    metal_cmd = [metal_tool, *metal_compile_flags(source), "-o", str(air)]
    metal = run(metal_cmd)
    metallib_result: subprocess.CompletedProcess[str] | None = None
    ok = metal.returncode == 0 and air.is_file() and air.stat().st_size > 0
    metallib_cmd = [metallib_tool, str(air), "-o", str(metallib)]
    if ok:
        metallib_result = run(metallib_cmd)
        ok = metallib_result.returncode == 0 and metallib.is_file() and metallib.stat().st_size > 0

    write_text(
        log,
        f"source={source}\n"
        f"air={air}\n"
        f"metallib={metallib}\n"
        f"metal_command={' '.join(metal_cmd)}\n"
        f"metal_returncode={metal.returncode}\n"
        "metal_stdout:\n"
        f"{metal.stdout}\n"
        "metal_stderr:\n"
        f"{metal.stderr}\n"
        f"metallib_command={' '.join(metallib_cmd)}\n"
        f"metallib_returncode={metallib_result.returncode if metallib_result else ''}\n"
        "metallib_stdout:\n"
        f"{metallib_result.stdout if metallib_result else ''}\n"
        "metallib_stderr:\n"
        f"{metallib_result.stderr if metallib_result else ''}\n",
    )

    return {
        "corpus": label,
        "source": str(source),
        "source_sha256": sha256(source) if source.is_file() else None,
        "functions": parse_functions(source),
        "air": str(air),
        "air_sha256": sha256(air) if air.is_file() else None,
        "metallib": str(metallib),
        "metallib_sha256": sha256(metallib) if metallib.is_file() else None,
        "log": str(log),
        "status": "ok" if ok else "failed",
        "metal_returncode": metal.returncode,
        "metallib_returncode": metallib_result.returncode if metallib_result else None,
        "metal_stderr_tail": metal.stderr[-2000:],
        "metallib_stderr_tail": metallib_result.stderr[-2000:] if metallib_result else "",
    }


HARNESS_SOURCE = r'''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

static void print_json_string(NSString *value) {
  NSData *data = [NSJSONSerialization dataWithJSONObject:@[value ?: @""]
                                                 options:0
                                                   error:nil];
  NSString *json = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
  NSRange start = [json rangeOfString:@"["];
  NSRange end = [json rangeOfString:@"]" options:NSBackwardsSearch];
  if (start.location != NSNotFound && end.location != NSNotFound && end.location > start.location) {
    NSString *inner = [json substringWithRange:NSMakeRange(start.location + 1, end.location - start.location - 1)];
    printf("%s", [inner UTF8String]);
  } else {
    printf("\"\"");
  }
}

int main(int argc, char **argv) {
  @autoreleasepool {
    if (argc < 2) {
      fprintf(stderr, "usage: %s manifest.json\n", argv[0]);
      return 2;
    }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      fprintf(stderr, "no Metal device\n");
      return 2;
    }

    NSString *manifestPath = [NSString stringWithUTF8String:argv[1]];
    NSData *data = [NSData dataWithContentsOfFile:manifestPath];
    if (!data) {
      fprintf(stderr, "manifest not readable: %s\n", argv[1]);
      return 2;
    }
    NSError *jsonError = nil;
    NSDictionary *manifest = [NSJSONSerialization JSONObjectWithData:data options:0 error:&jsonError];
    if (![manifest isKindOfClass:[NSDictionary class]]) {
      fprintf(stderr, "manifest json invalid: %s\n", [[jsonError description] UTF8String]);
      return 2;
    }

    NSArray *shaders = manifest[@"shaders"];
    NSUInteger ok = 0;
    NSUInteger failed = 0;
    printf("{\"schema\":\"metalsharp.vkd3d.native-metal-probe.v1\",\"device\":");
    print_json_string([device name]);
    printf(",\"results\":[");
    BOOL firstResult = YES;

    for (NSDictionary *shader in shaders) {
      if (![shader isKindOfClass:[NSDictionary class]]) continue;
      if (![[shader objectForKey:@"status"] isEqual:@"ok"]) continue;
      NSString *path = shader[@"metallib"];
      if (![path isKindOfClass:[NSString class]] || [path length] == 0) continue;

      NSError *libraryError = nil;
      id<MTLLibrary> library = [device newLibraryWithFile:path error:&libraryError];
      BOOL recordOk = library != nil;
      NSMutableArray *errors = [NSMutableArray array];
      if (!library) {
        [errors addObject:[libraryError localizedDescription] ?: @"library load failed"];
      }

      NSArray *functions = shader[@"functions"];
      if (library && [functions isKindOfClass:[NSArray class]] && [functions count] > 0) {
        for (NSDictionary *fnInfo in functions) {
          if (![fnInfo isKindOfClass:[NSDictionary class]]) continue;
          NSString *name = fnInfo[@"name"];
          NSString *stage = fnInfo[@"stage"];
          if (![name isKindOfClass:[NSString class]] || [name length] == 0) continue;
          id<MTLFunction> function = [library newFunctionWithName:name];
          if (!function) {
            recordOk = NO;
            [errors addObject:[NSString stringWithFormat:@"missing function %@", name]];
            continue;
          }
          if ([stage isEqual:@"kernel"]) {
            NSError *pipelineError = nil;
            id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&pipelineError];
            if (!pipeline) {
              recordOk = NO;
              [errors addObject:[NSString stringWithFormat:@"compute pipeline %@: %@", name, [pipelineError localizedDescription] ?: @"failed"]];
            }
          }
        }
      } else if (library && [[library functionNames] count] == 0) {
        recordOk = NO;
        [errors addObject:@"library has no functions"];
      }

      if (!firstResult) printf(",");
      firstResult = NO;
      printf("{\"metallib\":");
      print_json_string(path);
      printf(",\"ok\":%s,\"error_count\":%lu", recordOk ? "true" : "false", (unsigned long)[errors count]);
      if ([errors count] > 0) {
        NSData *errorData = [NSJSONSerialization dataWithJSONObject:errors options:0 error:nil];
        NSString *errorJson = [[NSString alloc] initWithData:errorData encoding:NSUTF8StringEncoding];
        printf(",\"errors\":%s", [errorJson UTF8String]);
      }
      printf("}");

      if (recordOk) ok++; else failed++;
    }

    printf("],\"ok_count\":%lu,\"failure_count\":%lu}\n", (unsigned long)ok, (unsigned long)failed);
    return failed == 0 ? 0 : 1;
  }
}
'''


def build_native_harness(out_root: Path) -> dict[str, Any]:
    native_dir = out_root / "native"
    source = native_dir / "metal_library_probe.mm"
    binary = native_dir / "metal_library_probe"
    source.parent.mkdir(parents=True, exist_ok=True)
    write_text(source, HARNESS_SOURCE)
    command = [
        "clang++",
        "-std=c++17",
        "-fobjc-arc",
        "-framework",
        "Foundation",
        "-framework",
        "Metal",
        str(source),
        "-o",
        str(binary),
    ]
    completed = run(command)
    write_text(
        native_dir / "metal_library_probe.build.log",
        "command=" + " ".join(command) + "\n"
        f"returncode={completed.returncode}\n"
        "stdout:\n"
        + completed.stdout
        + "\nstderr:\n"
        + completed.stderr,
    )
    return {
        "source": str(source),
        "binary": str(binary),
        "command": command,
        "returncode": completed.returncode,
        "ok": completed.returncode == 0 and binary.is_file(),
        "log": str(native_dir / "metal_library_probe.build.log"),
    }


def run_native_harness(harness: Path, manifest: Path, out_root: Path) -> dict[str, Any]:
    completed = run([str(harness), str(manifest)])
    log = out_root / "native" / "metal_library_probe.run.log"
    write_text(log, f"returncode={completed.returncode}\nstdout:\n{completed.stdout}\nstderr:\n{completed.stderr}\n")
    parsed: dict[str, Any] | None = None
    try:
        parsed = json.loads(completed.stdout)
    except json.JSONDecodeError:
        parsed = None
    return {
        "returncode": completed.returncode,
        "ok": completed.returncode == 0,
        "log": str(log),
        "stdout_json": parsed,
        "stderr_tail": completed.stderr[-2000:],
    }


def collect_sources(corpus: Path, generated_dir: Path) -> list[Path]:
    sources = sorted(corpus.glob("*.msl")) + sorted(corpus.glob("*.metal"))
    sources.extend(sorted(generated_dir.glob("*.msl")))
    sources.extend(sorted(generated_dir.glob("*.metal")))
    deduped: list[Path] = []
    seen: set[str] = set()
    for source in sources:
        key = str(source.resolve())
        if key in seen:
            continue
        seen.add(key)
        deduped.append(source)
    return deduped


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", action="append", type=Path, default=[], help="Captured shader-cache directory.")
    parser.add_argument("--stage-root", type=Path, default=DEFAULT_STAGE_ROOT)
    parser.add_argument("--name", default="", help="Optional run name. Defaults to timestamped run.")
    parser.add_argument("--converter", type=Path, default=DEFAULT_CONVERTER)
    parser.add_argument("--skip-converter", action="store_true", help="Only compile existing .msl/.metal files.")
    parser.add_argument("--limit", type=int, default=0, help="Optional max Metal source files per corpus.")
    parser.add_argument("--clean", action="store_true", help="Remove the selected run directory before staging.")
    parser.add_argument("--allow-empty", action="store_true", help="Succeed if no corpora or shaders are found.")
    args = parser.parse_args()

    corpora = [path for path in args.corpus if path.is_dir()] if args.corpus else default_corpora()
    if not corpora:
        if args.allow_empty:
            print("no shader corpora found")
            return 0
        print("no shader corpora found; pass --corpus /path/to/shader-cache", file=sys.stderr)
        return 1

    timestamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    run_name = args.name or f"vkd3d-metal-validation-{timestamp}"
    out_root = args.stage_root / run_name
    if args.clean and out_root.exists():
        shutil.rmtree(out_root)
    out_root.mkdir(parents=True, exist_ok=True)

    try:
        metal_tool = resolve_xcrun_tool("metal")
        metallib_tool = resolve_xcrun_tool("metallib")
    except RuntimeError as error:
        print(str(error), file=sys.stderr)
        return 1

    converter_available = args.converter.is_file() and os.access(args.converter, os.X_OK) and not args.skip_converter
    corpus_records: list[dict[str, Any]] = []
    shader_results: list[dict[str, Any]] = []

    for corpus in corpora:
        label = safe_label(corpus)
        generated_dir = out_root / "generated-msl" / label
        converter_result: dict[str, Any] | None = None
        if converter_available and (any(corpus.glob("*.dxbc")) or any(corpus.glob("*.dxil"))):
            converter_result = run_converter(args.converter, corpus, generated_dir)

        sources = collect_sources(corpus, generated_dir)
        if args.limit > 0:
            sources = sources[: args.limit]
        corpus_records.append(
            {
                "label": label,
                "path": str(corpus),
                "source_count": len(sources),
                "converter": converter_result,
            }
        )
        for source in sources:
            result = compile_metal_source(source, label, out_root, metal_tool, metallib_tool)
            shader_results.append(result)
            if result["status"] != "ok":
                print(f"Metal compile failed: {source}", file=sys.stderr)

    manifest = {
        "schema": "metalsharp.vkd3d.game-metal-validation.v1",
        "stage_root": str(out_root),
        "metal_tool": metal_tool,
        "metallib_tool": metallib_tool,
        "converter": str(args.converter),
        "converter_available": converter_available,
        "corpus_count": len(corpus_records),
        "shader_count": len(shader_results),
        "metal_ok_count": sum(1 for result in shader_results if result["status"] == "ok"),
        "metal_failure_count": sum(1 for result in shader_results if result["status"] != "ok"),
        "corpora": corpus_records,
        "shaders": shader_results,
    }
    manifest_path = out_root / "manifest.json"
    write_text(manifest_path, json.dumps(manifest, indent=2) + "\n")

    harness_result = build_native_harness(out_root)
    native_result: dict[str, Any] | None = None
    if harness_result["ok"]:
        native_result = run_native_harness(Path(harness_result["binary"]), manifest_path, out_root)
    manifest["native_harness"] = harness_result
    manifest["native_probe"] = native_result
    write_text(manifest_path, json.dumps(manifest, indent=2) + "\n")

    summary = {
        "stage_root": str(out_root),
        "corpus_count": manifest["corpus_count"],
        "shader_count": manifest["shader_count"],
        "metal_ok_count": manifest["metal_ok_count"],
        "metal_failure_count": manifest["metal_failure_count"],
        "native_probe_ok": bool(native_result and native_result.get("ok")),
        "native_probe_failure_count": (native_result or {}).get("stdout_json", {}).get("failure_count")
        if native_result and isinstance(native_result.get("stdout_json"), dict)
        else None,
    }
    write_text(out_root / "summary.json", json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))

    if manifest["shader_count"] == 0 and not args.allow_empty:
        return 1
    if manifest["metal_failure_count"]:
        return 1
    if native_result and not native_result.get("ok"):
        return 1
    if not harness_result["ok"]:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
