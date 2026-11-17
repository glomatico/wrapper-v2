"""Automatic authentication using a private environment and synthetic worker."""
import json
import os
from pathlib import Path
import shlex
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]


class AutoAuthTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="wrapper-auto-auth-")
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        launch = root / "worker-launch"
        launch.write_text("#!/bin/sh\nexec " + shlex.quote(sys.executable) + " "
                          + shlex.quote(str(ROOT / "tests/mock_worker.py")) + ' "$@"\n')
        launch.chmod(0o700)
        elf = bytearray(64)
        elf[:6] = b"\x7fELF\x02\x01"
        elf[18:20] = (62).to_bytes(2, "little")
        worker = root / "system/bin/apple-music-worker"
        worker.parent.mkdir(parents=True)
        worker.write_bytes(elf)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            self.port = sock.getsockname()[1]
        self.env = dict(os.environ)
        self.env.pop("WRAPPER_USERNAME", None)
        self.env.pop("WRAPPER_PASSWORD", None)
        self.env.update(WRAPPER_HOST="127.0.0.1", WRAPPER_PORT=str(self.port),
                        WRAPPER_ARCH="x86_64", WRAPPER_ROOTFS=str(root),
                        WRAPPER_LAUNCHER=str(launch),
                        WRAPPER_DEVICE_INFO="a/b/c/d/e/f/g/h/i", RUST_LOG="info")
        self.log = root / "logs.jsonl"

    def launch(self):
        binary = ROOT / "target/debug/wrapperd"
        if not binary.exists():
            self.skipTest("cargo build first")
        stream = self.log.open("wb")
        self.addCleanup(stream.close)
        proc = subprocess.Popen([str(binary)], env=self.env, stdout=subprocess.DEVNULL,
                                stderr=stream)
        self.addCleanup(lambda: self.stop(proc))
        return proc

    @staticmethod
    def stop(proc):
        if proc.poll() is None:
            proc.terminate()
        try:
            proc.wait(timeout=4)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=4)

    def request(self, method, path, payload=None):
        body = json.dumps(payload).encode() if payload is not None else None
        request = Request(f"http://127.0.0.1:{self.port}{path}", method=method,
                          data=body, headers={"Content-Type":"application/json"})
        try:
            response = urlopen(request, timeout=5)
        except HTTPError as err:
            response = err
        return response.status, json.loads(response.read())

    def wait_for(self, predicate, timeout=6):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                if predicate():
                    return
            except (OSError, ValueError):
                pass
            time.sleep(0.04)
        self.fail("startup action did not finish")

    def events(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_login_from_env_and_no_secret_leakage(self):
        self.env.update(WRAPPER_USERNAME="unique-auto-user-secret",
                        WRAPPER_PASSWORD="unique-auto-password-secret",
                        WRAPPER_MOCK_NO_CACHED_SESSION="1")
        self.launch()
        self.wait_for(lambda: any(e.get("event") == "session.auto_auth_complete"
                                  for e in self.events()))
        self.assertEqual(self.request("GET", "/session")[1]["state"], "authenticated")
        logs = self.log.read_text()
        self.assertNotIn("unique-auto-user-secret", logs)
        self.assertNotIn("unique-auto-password-secret", logs)

    def test_restore_precedes_login(self):
        self.env.update(WRAPPER_USERNAME="twofactor", WRAPPER_PASSWORD="some-password")
        self.launch()
        self.wait_for(lambda: any(e.get("event") == "session.auto_auth_restored"
                                  for e in self.events()))
        self.assertEqual(self.request("GET", "/session")[1]["state"], "authenticated")
        self.assertFalse(any(e.get("event") == "session.auto_auth_2fa_required"
                             for e in self.events()))

    def test_2fa_is_cancelled_and_manual_login_still_works(self):
        self.env.update(WRAPPER_USERNAME="twofactor", WRAPPER_PASSWORD="some-password",
                        WRAPPER_MOCK_NO_CACHED_SESSION="1")
        self.launch()
        self.wait_for(lambda: any(e.get("event") == "session.auto_auth_2fa_required"
                                  for e in self.events()))
        status, challenge = self.request("POST", "/session/login",
                                         {"username":"twofactor","password":"dummy"})
        self.assertEqual(status, 202)
        self.assertEqual(self.request("POST", "/session/challenge",
                                      {"challenge_id":challenge["challenge_id"],
                                       "code":"123456"})[0], 200)
        self.assertEqual(self.request("GET", "/session")[1]["state"], "authenticated")

    def test_incomplete_credentials_fail_closed(self):
        self.env.update(WRAPPER_USERNAME="unique-config-secret")
        proc = self.launch()
        self.assertEqual(proc.wait(timeout=4), 1)
        events = self.events()
        self.assertEqual(events[-1]["code"], "WRAPPER_CREDENTIALS")
        self.assertNotIn("unique-config-secret", self.log.read_text())


if __name__ == "__main__":
    unittest.main()
