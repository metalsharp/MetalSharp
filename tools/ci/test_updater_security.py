#!/usr/bin/env python3
"""Regression tests for updater privileged-operation path handling."""

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SHELL_UPDATER = ROOT / "app" / "updater" / "update.sh"
PYTHON_UPDATER = ROOT / "app" / "updater" / "update.py"


def load_python_updater():
    spec = importlib.util.spec_from_file_location("metalsharp_updater", PYTHON_UPDATER)
    if spec is None or spec.loader is None:
        raise RuntimeError("unable to load updater module")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


updater = load_python_updater()


class UpdaterSecurityTests(unittest.TestCase):
    def test_shell_updater_passes_paths_as_osascript_arguments(self):
        source = SHELL_UPDATER.read_text()

        self.assertIn("on run argv", source)
        self.assertIn("quoted form of (commandArg as text)", source)
        self.assertIn('run_privileged ditto "$APP_SOURCE" "$TMP_APP_PATH"', source)
        self.assertIn('run_privileged hdiutil-attach "$MOUNT_POINT" "$DMG_PATH"', source)
        self.assertNotIn("${command//\\\"/", source)
        self.assertNotIn("run_privileged \"ditto '$APP_SOURCE'", source)
        self.assertNotIn("escaped_dmg=", source)
        self.assertNotIn("escaped_mount=", source)

    def test_shell_privileged_fallback_keeps_untrusted_path_out_of_script(self):
        function_source = subprocess.run(
            [
                "sed",
                "-n",
                "/^run_privileged() {/,/^}/p",
                str(SHELL_UPDATER),
            ],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        malicious_path = "/tmp/MetalSharp'$(touch /tmp/pwned)'.app"

        with tempfile.TemporaryDirectory(prefix="updater-shell-security-") as temp:
            temp_path = Path(temp)
            capture = temp_path / "osascript-argv.json"
            fake_osascript = temp_path / "osascript"
            fake_osascript.write_text(
                "#!/usr/bin/env python3\n"
                "import json\n"
                "import os\n"
                "import sys\n"
                "with open(os.environ['CAPTURE'], 'w') as handle:\n"
                "    json.dump(sys.argv[1:], handle)\n"
            )
            fake_osascript.chmod(0o755)
            environment = {
                **os.environ,
                "CAPTURE": str(capture),
                "PATH": f"{temp}:{os.environ['PATH']}",
            }

            subprocess.run(
                [
                    "bash",
                    "-c",
                    'eval "$1"; run_privileged ditto "$2" "$3"',
                    "updater-test",
                    function_source,
                    malicious_path,
                    "/Applications/.MetalSharp.app.update.123",
                ],
                check=True,
                env=environment,
            )

            osascript_argv = json.loads(capture.read_text())
            script_end = osascript_argv.index("--")
            self.assertNotIn(malicious_path, osascript_argv[:script_end])
            self.assertEqual(osascript_argv[script_end + 1], "/usr/bin/ditto")
            self.assertEqual(osascript_argv[script_end + 2], malicious_path)

    def test_shell_updater_rejects_unsafe_bundle_names(self):
        source = SHELL_UPDATER.read_text()

        self.assertIn("*[!A-Za-z0-9._-]*) return 1", source)

    def test_python_privileged_fallback_keeps_untrusted_path_out_of_script(self):
        malicious_path = "/tmp/MetalSharp'$(touch /tmp/pwned)'.app"
        completed = [
            subprocess.CompletedProcess(["cp"], 1, "", "permission denied"),
            subprocess.CompletedProcess(["osascript"], 0, "", ""),
        ]

        with patch.object(updater, "run", side_effect=completed) as run:
            self.assertTrue(updater.admin_cp_r(malicious_path, "/Applications/MetalSharp.app"))

        privileged_argv = run.call_args_list[1].args[0]
        script = privileged_argv[privileged_argv.index("-e") + 1]
        self.assertIn(malicious_path, privileged_argv)
        self.assertNotIn(malicious_path, script)
        self.assertIn("quoted form of", script)
        self.assertEqual(privileged_argv[privileged_argv.index("--") + 1], "/bin/cp")

    @unittest.skipUnless(
        sys.platform == "darwin" and shutil.which("osascript"),
        "requires macOS osascript",
    )
    def test_apple_script_quoting_does_not_execute_path_payload(self):
        with tempfile.TemporaryDirectory(prefix="updater-security-") as temp:
            marker = Path(temp) / "executed"
            malicious_path = f"{temp}/MetalSharp'$(touch {marker})'.app"
            script = updater.PRIVILEGED_APPLESCRIPT.replace(
                "do shell script shellCommand with administrator privileges",
                "return shellCommand",
            )

            result = subprocess.run(
                ["osascript", "-e", script, "--", "/bin/echo", malicious_path],
                check=True,
                capture_output=True,
                text=True,
            )

            self.assertFalse(marker.exists())
            self.assertIn("quoted form", script)
            self.assertIn("MetalSharp", result.stdout)

    def test_python_updater_rejects_unsafe_bundle_name_before_file_checks(self):
        malicious_path = "/tmp/MetalSharp'$(touch /tmp/pwned)'.app"

        with patch.object(updater.subprocess, "run") as run:
            self.assertFalse(updater.verify_app_bundle(malicious_path))

        run.assert_not_called()


if __name__ == "__main__":
    unittest.main(verbosity=2)
