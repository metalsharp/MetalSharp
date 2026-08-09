#!/usr/bin/env python3
"""Regression coverage for graphics-bundle lane replacement."""

import collections
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("update-graphics-bundle.py")


class UpdateGraphicsBundleTests(unittest.TestCase):
    def add_member(self, archive: tarfile.TarFile, root: Path, rel: str, payload: bytes) -> None:
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        archive.add(path, arcname=rel)

    def test_replaced_lanes_have_no_duplicate_members_and_preserve_unrelated_files(self) -> None:
        with tempfile.TemporaryDirectory(prefix="graphics-bundle-test-") as temp:
            root = Path(temp)
            source = root / "source"
            source.mkdir()
            raw = root / "input.tar"
            with tarfile.open(raw, "w") as archive:
                self.add_member(archive, source, "Graphics/dll/dxmt/x86_64-windows/d3d12.dll", b"dxmt-kept")
                self.add_member(archive, source, "Graphics/dll/vkd3d-proton/x86_64-windows/d3d12.dll", b"old-d3d12")
                self.add_member(archive, source, "Graphics/dll/vkd3d-proton/x86_64-windows/d3d12core.dll", b"old-d3d12core")
                self.add_member(archive, source, "Graphics/dll/vkd3d-proton/i386-windows/d3d12.dll", b"old-i386-d3d12")
                self.add_member(archive, source, "Graphics/dll/vkd3d-proton/i386-windows/d3d12core.dll", b"old-i386-d3d12core")
                self.add_member(archive, source, "Graphics/dll/moltenvk-vkmt/libMoltenVK.dylib", b"old-moltenvk")
                self.add_member(archive, source, "Graphics/dll/moltenvk-vkmt/MoltenVK_icd.json", b"old-icd")

            archive = root / "input.tar.zst"
            subprocess.run(["zstd", "-q", "-19", "-f", str(raw), "-o", str(archive)], check=True)

            vkd3d = root / "vkd3d"
            for arch in ("x86_64-windows", "i386-windows"):
                lane = vkd3d / arch
                lane.mkdir(parents=True)
                (lane / "d3d12.dll").write_bytes(f"new-{arch}-d3d12".encode())
                (lane / "d3d12core.dll").write_bytes(f"new-{arch}-d3d12core".encode())
            moltenvk = root / "moltenvk"
            moltenvk.mkdir()
            (moltenvk / "libMoltenVK.dylib").write_bytes(b"new-moltenvk")
            (moltenvk / "MoltenVK_icd.json").write_bytes(b"new-icd")

            output = root / "output.tar.zst"
            subprocess.run(
                [
                    "python3", str(SCRIPT), "--archive", str(archive), "--out", str(output),
                    "--vkd3d-root", str(vkd3d), "--moltenvk-root", str(moltenvk),
                ],
                check=True,
            )
            members = subprocess.check_output(["tar", "-tf", str(output)], text=True).splitlines()
            self.assertFalse(
                [member for member, count in collections.Counter(members).items() if count > 1],
                "replacing an existing lane must not append duplicate tar members",
            )

            extracted = root / "extracted"
            extracted.mkdir()
            subprocess.run(["tar", "-xf", str(output), "-C", str(extracted)], check=True)
            self.assertEqual(
                (extracted / "Graphics/dll/dxmt/x86_64-windows/d3d12.dll").read_bytes(), b"dxmt-kept"
            )
            self.assertEqual(
                (extracted / "Graphics/dll/vkd3d-proton/x86_64-windows/d3d12.dll").read_bytes(),
                b"new-x86_64-windows-d3d12",
            )
            self.assertEqual(
                (extracted / "Graphics/dll/moltenvk-vkmt/libMoltenVK.dylib").read_bytes(), b"new-moltenvk"
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
