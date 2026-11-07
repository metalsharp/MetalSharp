#!/usr/bin/env python3
"""Makes the x86_64 runtime's Mach-O files relocatable inside MetalSharp-Wine-x86_64.app.

Some libraries of the bundles' Wine name their dependencies by the paths of the machines they were built on
(/tmp/metalsharp-wine-deps/lib/libgnutls.30.dylib, or a bare libz.dylib) and carry those machines' rpaths. They load
only because MetalSharp's wrapper puts the runtime's directories on DYLD_FALLBACK_LIBRARY_PATH. Here every such
dependency names the bundled file relative to its user (@loader_path/...), @rpath dependencies get an rpath to the
bundled file, and rpaths outside the app are removed.

    relocate-macho.py <App>/Contents
"""
import os
import subprocess
import sys

SYSTEM = ("/usr/lib/", "/System/")
# Wine modules allowed to miss libraries (reported, not fatal); none since the app carries GStreamer and FFmpeg
KNOWN_MISSING = set()
BUILD_MACHINE = ("/tmp/", "/private/", "/Volumes/", "/Users/", "/opt/", "/usr/local/")


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def is_macho(path):
    with open(path, "rb") as f:
        magic = f.read(4)
    return magic in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca")


def apple_signed(path):
    """Apple's code (D3DMetal, Metal Shader Converter) is shipped as Apple signed it, so it is never edited."""
    info = subprocess.run(["codesign", "-dvv", path], capture_output=True, text=True).stderr
    return "Authority=Software Signing" in info.splitlines()


def load_commands(path):
    """(install name or None, dependencies, rpaths) of one Mach-O (the first architecture's)."""
    ident, deps, rpaths = None, [], []
    arch = run("lipo", "-archs", path).split()[0]
    lines = run("otool", "-arch", arch, "-l", path).splitlines()
    for i, line in enumerate(lines):
        line = line.strip()
        if line.startswith("cmd "):
            cmd = line.split()[1]
            for follow in lines[i + 1:i + 4]:
                follow = follow.strip()
                if follow.startswith(("name ", "path ")):
                    value = follow.split(None, 1)[1].rsplit(" (offset", 1)[0]
                    if cmd == "LC_ID_DYLIB":
                        ident = value
                    elif cmd in ("LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB", "LC_LAZY_LOAD_DYLIB"):
                        deps.append(value)
                    elif cmd == "LC_RPATH":
                        rpaths.append(value)
                    break
    return ident, list(dict.fromkeys(deps)), list(dict.fromkeys(rpaths))


def main():
    contents = os.path.abspath(sys.argv[1])
    files = {}
    for root, _, names in os.walk(contents):
        if ".framework" in root:
            continue
        for name in names:
            path = os.path.join(root, name)
            if os.path.isfile(path) and not os.path.islink(path) and is_macho(path) and not apple_signed(path):
                files[path] = None
    lib = os.path.join(contents, "lib")
    wine_roots = (os.path.join(contents, "MacOS") + os.sep, lib + os.sep)

    def libraries(root):
        found = {}
        for directory, _, names in os.walk(root):
            if ".framework" in directory:
                continue
            for name in names:
                path = os.path.join(directory, name)
                if os.path.isfile(path) and (name.endswith((".dylib", ".so")) or ".so." in name):
                    found.setdefault(name, path)
        return found

    # The Wine tree's libraries: lib/wine/x86_64-unix first, then the rest of lib
    wine_libs = libraries(os.path.join(lib, "wine/x86_64-unix"))
    for name, path in libraries(lib).items():
        wine_libs.setdefault(name, path)

    unresolved, warnings = [], []
    for path in files:
        here = os.path.dirname(path)
        in_wine = path.startswith(wine_roots)
        if in_wine:
            candidates = dict(libraries(here))
            for name, target in wine_libs.items():
                candidates.setdefault(name, target)
        else:
            # Mono, FNA, the shims: their own directory tree (runtime/<name>), which MetalSharp points them at
            rel = os.path.relpath(path, os.path.join(contents, "share/metalsharp")).split(os.sep)
            component = os.path.join(contents, "share/metalsharp", *rel[:2]) if len(rel) > 2 else here
            candidates = libraries(here)
            for name, target in libraries(component).items():
                candidates.setdefault(name, target)
        ident, deps, rpaths = load_commands(path)
        changes, add_rpaths = [], []
        if in_wine and ident and not ident.startswith(("@", "/usr/", "/System/")):
            changes += ["-id", "@rpath/" + os.path.basename(ident)]
        for dep in deps:
            if dep.startswith(SYSTEM) or dep == ident:
                continue
            leaf = os.path.basename(dep)
            if dep.startswith("@rpath/"):
                target = candidates.get(leaf) if in_wine else None
                resolved = any(os.path.isfile(os.path.normpath(os.path.join(here, r[len("@loader_path/"):], leaf)))
                               for r in rpaths if r.startswith("@loader_path"))
                if in_wine and not target and not resolved and os.path.basename(path) in KNOWN_MISSING:
                    warnings.append(f"{os.path.relpath(path, contents)}: {dep} (not bundled)")
                if target and not resolved:
                    rel = os.path.relpath(os.path.dirname(target), here)
                    rpath = "@loader_path/" if rel == "." else "@loader_path/" + rel + "/"
                    if rpath not in rpaths and rpath not in add_rpaths:
                        add_rpaths.append(rpath)
                continue
            if dep.startswith("@"):
                continue
            # a build machine's path, or a bare name the dynamic loader finds only on a search path
            if not in_wine and dep.startswith("/") and not dep.startswith(BUILD_MACHINE):
                continue
            target = candidates.get(leaf)
            if not target:
                fatal = in_wine and os.path.basename(path) not in KNOWN_MISSING
                (unresolved if fatal else warnings).append(f"{os.path.relpath(path, contents)}: {dep}")
                continue
            changes += ["-change", dep, "@loader_path/" + os.path.relpath(target, here)]
        for rpath in rpaths:
            if rpath.startswith(BUILD_MACHINE):
                changes += ["-delete_rpath", rpath]
        for rpath in add_rpaths:
            changes += ["-add_rpath", rpath]
        if changes:
            done = subprocess.run(["install_name_tool", *changes, path], capture_output=True, text=True)
            if done.returncode:
                (unresolved if in_wine else warnings).append(
                    f"{os.path.relpath(path, contents)}: install_name_tool failed: {done.stderr.strip()}")
            else:
                print(f"relocated {os.path.relpath(path, contents)}: {' '.join(changes)}")
    if warnings:
        print("left as they are:\n  " + "\n  ".join(warnings), file=sys.stderr)
    if unresolved:
        print("unresolved dependencies:\n  " + "\n  ".join(unresolved), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
