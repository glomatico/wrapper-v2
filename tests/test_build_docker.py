"""Test the Docker CLI contract without building an image or shipping Apple files."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "tools/build-docker.sh"


class DockerBuildScriptTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="wrapper-build-cli-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        tools = self.root / "bin"
        tools.mkdir()
        stub = tools / "docker"
        stub.write_text(
            "#!/usr/bin/env python3\n"
            "import hashlib,json,os,pathlib,sys\n"
            "args=sys.argv[1:]\n"
            "context=pathlib.Path(next(x.split('=',1)[1] for x in args if x.startswith('apple_apk=')))\n"
            "archive=context/'AppleMusic.package'\n"
            "pathlib.Path(os.environ['TEST_DOCKER_RESULT']).write_text(json.dumps({"
            "'args':args,'file':archive.name,'digest':hashlib.sha256(archive.read_bytes()).hexdigest(),"
            "'context':str(context)}))\n"
        )
        stub.chmod(0o755)
        self.env = dict(os.environ)
        for name in ("APPLE_MUSIC_APK", "WRAPPER_ARCH", "WRAPPER_IMAGE"):
            self.env.pop(name, None)
        self.env["PATH"] = str(tools) + os.pathsep + self.env["PATH"]
        self.env["TEST_DOCKER_RESULT"] = str(self.root / "result.json")
        self.path = self.root / "Apple Music test.apk"
        self.path.write_bytes(b"synthetic apk contents")

    def invoke(self, *args):
        return subprocess.run(
            [str(SCRIPT), *map(str, args)],
            cwd=self.root,
            env=self.env,
            capture_output=True,
            text=True,
        )

    def result(self):
        value = json.loads((self.root / "result.json").read_text())
        self.assertEqual(value["file"], "AppleMusic.package")
        self.assertEqual(value["digest"], hashlib.sha256(self.path.read_bytes()).hexdigest())
        self.assertFalse(Path(value["context"]).exists(), "temporary APK input was not cleaned up")
        return value["args"]

    def test_positional_apk_and_arch_after_path(self):
        response = self.invoke(self.path.name, "--arch", "arm64-v8a")
        self.assertEqual(response.returncode, 0, response.stderr)
        args = self.result()
        self.assertIn("linux/arm64", args)
        self.assertIn("wrapper-v2:local", args)

    def test_apkm_and_arch_before_path(self):
        archive = self.root / "Apple Music test.apkm"
        archive.write_bytes(self.path.read_bytes())
        self.path = archive
        response = self.invoke("--arch=x86_64", archive.name)
        self.assertEqual(response.returncode, 0, response.stderr)
        self.assertIn("linux/amd64", self.result())

    def test_environment_fallback(self):
        self.env["APPLE_MUSIC_APK"] = str(self.path)
        response = self.invoke("--arch", "x86_64")
        self.assertEqual(response.returncode, 0, response.stderr)
        self.result()

    def test_positional_overrides_environment(self):
        self.env["APPLE_MUSIC_APK"] = str(self.root / "missing.apkm")
        response = self.invoke(self.path)
        self.assertEqual(response.returncode, 0, response.stderr)
        self.result()

    def test_rejects_invalid_arguments(self):
        self.assertEqual(self.invoke().returncode, 2)
        self.assertEqual(self.invoke(self.path, self.path).returncode, 2)
        self.assertEqual(self.invoke("--arch").returncode, 2)
        self.assertEqual(self.invoke("--unknown", self.path).returncode, 2)
        self.assertFalse((self.root / "result.json").exists())


if __name__ == "__main__":
    unittest.main()
