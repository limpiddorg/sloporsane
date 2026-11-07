"""Exercise the public installer offline, with no writes outside the build directory."""
import base64
import hashlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest

TEMPLATE = Path(__file__).with_name("install.sh.in")


class InstallerTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=os.environ["SOS_INSTALLER_TEST_ROOT"])
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.home = self.root / "user home"
        self.server = self.root / "release"
        self.server.mkdir()
        self.tools = self.root / "tools"
        self.tools.mkdir()
        self.env = dict(os.environ, HOME=str(self.home), XDG_DATA_HOME="",
                        PATH=str(self.tools) + os.pathsep + os.environ["PATH"],
                        TEST_RELEASE=str(self.server), TEST_OS="Linux", TEST_ARCH="x86_64")
        self.stub("uname", '#!/bin/sh\ncase "$1" in -s) echo "$TEST_OS";; -m) echo "$TEST_ARCH";; esac\n')
        self.stub("curl", '''#!/bin/sh
while [ "$#" -gt 0 ]; do
  case "$1" in
    https://*) url=$1 ;;
    --output) shift; output=$1 ;;
  esac
  shift
done
cp "$TEST_RELEASE/${url##*/}" "$output"
''')
        self.stub("base64", f'#!{sys.executable}\nimport base64,sys\nsys.stdout.buffer.write(base64.b64decode(sys.stdin.buffer.read(), validate=True))\n')
        self.script = self.root / "install.sh"
        self.render()
        self.archive()

    def stub(self, name, content):
        path = self.tools / name
        path.write_text(content)
        path.chmod(0o755)

    def render(self, version="v0.1.0", url="https://downloads.example.test/v0.1.0"):
        text = TEMPLATE.read_text()
        for key, value in (("VERSION", version), ("RELEASE_URL", url)):
            text = text.replace(f"@@{key}_BASE64@@", base64.b64encode(value.encode()).decode())
        self.script.write_text(text)

    def archive(self, platform="linux-x86_64", *, launcher=True, content=b"#!/bin/sh\nprintf 'SOS test\\n'\n", unsafe=False):
        asset = self.server / f"sos-{platform}.tar.gz"
        with tarfile.open(asset, "w:gz") as package:
            entry = tarfile.TarInfo("../escape" if unsafe else "sos/sos" if launcher else "sos/other")
            entry.size = len(content)
            entry.mode = 0o755
            package.addfile(entry, io.BytesIO(content))
        digest = hashlib.sha256(asset.read_bytes()).hexdigest()
        (self.server / "SHA256SUMS").write_text(f"{digest}  {asset.name}\n")
        return digest

    def run_installer(self, success=True):
        result = subprocess.run(["/bin/bash", str(self.script)], env=self.env, cwd=self.root,
                                text=True, capture_output=True)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    @property
    def link(self):
        return self.home / ".local/bin/sos"

    def test_install_repeat_update_and_spaces(self):
        self.run_installer()
        previous = self.link.resolve()
        self.assertEqual(subprocess.check_output([str(self.link)], text=True), "SOS test\n")
        self.run_installer()
        self.assertEqual(self.link.resolve(), previous)
        self.archive(content=b"#!/bin/sh\nexit 0\n")
        self.render(version="v0.2.0")
        self.run_installer()
        self.assertNotEqual(self.link.resolve(), previous)
        self.assertTrue(previous.exists())

    def test_mac_arm64(self):
        self.env.update(TEST_OS="Darwin", TEST_ARCH="arm64")
        self.archive("macos-arm64")
        self.run_installer()
        self.assertTrue(self.link.is_file())

    def test_unsupported_platform(self):
        self.env["TEST_ARCH"] = "aarch64"
        self.assertIn("Supported platforms", self.run_installer(False).stderr)
        self.assertFalse(self.home.exists())

    def test_checksum_failure_preserves_installation(self):
        self.run_installer()
        previous = self.link.resolve()
        (self.server / "SHA256SUMS").write_text("0" * 64 + "  sos-linux-x86_64.tar.gz\n")
        self.assertIn("checksum mismatch", self.run_installer(False).stderr)
        self.assertEqual(self.link.resolve(), previous)

    def test_download_failure_preserves_installation(self):
        self.run_installer()
        previous = self.link.resolve()
        (self.server / "sos-linux-x86_64.tar.gz").unlink()
        self.run_installer(False)
        self.assertEqual(self.link.resolve(), previous)

    def test_bad_archives_do_not_install(self):
        for kwargs in ({"launcher": False}, {"unsafe": True}):
            with self.subTest(kwargs=kwargs):
                self.archive(**kwargs)
                self.run_installer(False)
                self.assertFalse(self.link.exists())

    def test_ambiguous_checksum_is_rejected(self):
        manifest = self.server / "SHA256SUMS"
        manifest.write_text(manifest.read_text() * 2)
        self.assertIn("ambiguous", self.run_installer(False).stderr)

    def test_xdg_and_relative_fallback(self):
        self.env["XDG_DATA_HOME"] = str(self.root / "custom data")
        self.run_installer()
        self.assertTrue(self.link.resolve().is_relative_to(self.root / "custom data"))
        self.env["XDG_DATA_HOME"] = "relative"
        self.run_installer()
        self.assertTrue(self.link.resolve().is_relative_to(self.home / ".local/share"))

    def test_unrendered_and_invalid_configuration(self):
        self.script.write_text(TEMPLATE.read_text())
        self.run_installer(False)
        self.render(url="http://insecure.example.test")
        self.run_installer(False)
        self.render(version="$(touch should-not-exist)")
        self.run_installer(False)
        self.assertFalse((self.root / "should-not-exist").exists())

    def test_install_lock_and_existing_directory_are_preserved(self):
        lock = self.home / ".local/share/sos/app/.install.lock"
        lock.mkdir(parents=True)
        self.assertIn("Another installation", self.run_installer(False).stderr)
        self.assertTrue(lock.is_dir())
        lock.rmdir()
        self.link.mkdir(parents=True)
        self.assertIn("Cannot replace a directory", self.run_installer(False).stderr)
        self.assertTrue(self.link.is_dir())


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: test_installer.py BUILD_DIRECTORY")
    root = Path(sys.argv.pop()).resolve()
    root.mkdir(parents=True, exist_ok=True)
    os.environ["SOS_INSTALLER_TEST_ROOT"] = str(root)
    unittest.main()
