#!/usr/bin/env python3
"""Holds a packaged tree to the oldest macOS the release supports (MACOS_FLOOR, 15.0).

Some of the payloads MetalSharp's bundles carry (Unity Mono, SDL2/SDL3, FMOD, libgdiplus, the host runtime) were
linked on macOS 26 or 27 without a deployment target, so their LC_BUILD_VERSION names that macOS and dyld would refuse
them on macOS 15. For each Mach-O in the tree (Apple-signed code is left as Apple ships it), this checks against the
macOS 15 SDK's text stubs that it links no system library and imports no system symbol macOS 15 lacks (weak imports
are fine: they are checked at run time). Only a file that passes is relabelled.

  macos-floor.py --sdk <MacOSX15.x.sdk> [--fix] <tree>...

--fix: relabel each file above the floor to it (vtool, keeping its SDK version) and give a dylib whose install name is
an absolute build path @rpath/<its name>. The caller signs afterwards. Without --fix it only reports. Exit 1 if any file
is above the floor (after --fix: any file that could not be held to it) or needs something macOS 15 lacks."""
import argparse, os, re, subprocess, sys

FLOOR = os.environ.get("MACOS_FLOOR", "15.0")
MAGICS = {b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe"}
LIST_RE = re.compile(r"(symbols|objc-classes|objc-eh-types|objc-ivars|weak-symbols|thread-local-symbols):\s*\[([^\]]*)\]", re.S)
INSTALL_RE = re.compile(r"install-name:\s*'?([^'\n]+)'?")
SYSTEM = ("/usr/lib/", "/System/Library/")


def vt(v):
    return tuple(int(x) for x in (v.split(".") + ["0", "0"])[:3])


def run(*args):
    return subprocess.run(args, capture_output=True, text=True).stdout


def load_sdk(sdk):
    symbols, names = set(), set()
    for d, _, files in os.walk(sdk):
        for n in files:
            if not n.endswith(".tbd"):
                continue
            try:
                text = open(os.path.join(d, n), errors="replace").read()
            except OSError:
                continue
            for doc in text.split("--- !tapi"):
                m = INSTALL_RE.search(doc)
                if m:
                    names.add(m.group(1).strip())
                for kind, body in LIST_RE.findall(doc):
                    for s in re.split(r"[,\s]+", body):
                        s = s.strip().strip("'\"")
                        if not s:
                            continue
                        if kind == "objc-classes":
                            symbols.update({"_OBJC_CLASS_$_" + s, "_OBJC_METACLASS_$_" + s})
                        elif kind == "objc-eh-types":
                            symbols.add("_OBJC_EHTYPE_$_" + s)
                        elif kind == "objc-ivars":
                            symbols.add("_OBJC_IVAR_$_" + s)
                        else:
                            symbols.add(s)
    if not names:
        sys.exit(f"macos-floor: no .tbd stubs in {sdk}")
    return symbols, names


def machos(roots):
    for root in roots:
        for d, dirs, files in os.walk(root):
            for n in files:
                p = os.path.join(d, n)
                if os.path.islink(p):
                    continue
                try:
                    with open(p, "rb") as f:
                        if f.read(4) in MAGICS:
                            yield p
                except OSError:
                    pass


def apple_signed(p):
    info = subprocess.run(["codesign", "-dvv", p], capture_output=True, text=True).stderr
    return "\nAuthority=Software Signing\n" in "\n" + info + "\n"


def minimums(p):
    return [l.split()[1] for l in run("vtool", "-show-build", p).splitlines() if l.strip().startswith("minos")]


def needs(p, symbols, names):
    """What p needs from the system that macOS 15 lacks."""
    missing = []
    for arch in [a for a in run("lipo", "-archs", p).split() if a in ("arm64", "arm64e", "x86_64")]:
        own_id = run("otool", "-arch", arch, "-D", p).splitlines()[1:2]
        weak = set(re.findall(r"cmd LC_LOAD_WEAK_DYLIB\n.*\n\s+name (\S+)", run("otool", "-arch", arch, "-l", p)))
        libs = {}
        for line in run("otool", "-arch", arch, "-L", p).splitlines()[1:]:
            lib = line.strip().split(" (")[0]
            if lib and lib not in own_id:
                libs[os.path.basename(lib).split(".")[0]] = lib
        for lib in libs.values():
            if lib.startswith(SYSTEM) and lib not in names and lib not in weak:
                missing.append(f"[{arch}] library {lib}")
        for line in run("nm", "-m", "-u", "-arch", arch, p).splitlines():
            m = re.match(r"\s*\(undefined\)\s+external\s+(\S+)\s+\(from (\S+)\)", line)
            if not m:
                continue
            sym, frm = m.groups()
            lib = libs.get(frm) or next((v for k, v in libs.items() if os.path.basename(v).startswith(frm)), "")
            if lib.startswith(SYSTEM) and lib not in weak and sym not in symbols:
                missing.append(f"[{arch}] {sym} ({lib})")
    return missing


def hold(p):
    """Relabels p to the floor, keeping each slice's SDK version, and makes an absolute build-path id @rpath."""
    show = run("vtool", "-show-build", p)
    sdk = next((l.split()[1] for l in show.splitlines() if l.strip().startswith("sdk")), FLOOR)
    tmp = p + ".floor"
    r = subprocess.run(["vtool", "-set-build-version", "macos", FLOOR, sdk, "-replace", "-output", tmp, p],
                       capture_output=True, text=True)
    if r.returncode:
        return r.stderr.strip()
    os.chmod(tmp, os.stat(p).st_mode)
    os.replace(tmp, p)
    ids = run("otool", "-D", p).splitlines()[1:2]
    if ids and ids[0].startswith("/") and not ids[0].startswith(SYSTEM):
        subprocess.run(["install_name_tool", "-id", "@rpath/" + os.path.basename(ids[0]), p], capture_output=True)
    return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", required=True)
    ap.add_argument("--fix", action="store_true")
    ap.add_argument("roots", nargs="+")
    a = ap.parse_args()
    symbols, names = load_sdk(a.sdk)
    bad = held = 0
    for p in machos(a.roots):
        if apple_signed(p):
            continue
        above = [m for m in minimums(p) if vt(m) > vt(FLOOR)]
        missing = needs(p, symbols, names)
        if missing:
            bad += 1
            print(f"needs newer macOS: {p}: " + ", ".join(missing[:8]))
            continue
        if above and a.fix:
            err = hold(p)
            if err:
                bad += 1
                print(f"could not relabel {p}: {err}")
                continue
            held += 1
        elif above:
            bad += 1
            print(f"above macOS {FLOOR} ({','.join(sorted(set(above)))}): {p}")
    print(f"macos-floor: {held} relabelled to macOS {FLOOR}, {bad} problems")
    return 1 if bad else 0


sys.exit(main())
