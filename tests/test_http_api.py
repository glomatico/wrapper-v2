#!/usr/bin/env python3
"""Real HTTP + Rust supervisor + synthetic worker contract, no Apple runtime."""
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
from urllib.parse import urlencode
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]


class ServerContract(unittest.TestCase):
    architecture = "x86_64"
    @classmethod
    def setUpClass(cls):
        binary = ROOT / "target/debug/wrapperd"
        if not binary.is_file():
            raise unittest.SkipTest("build Rust server before running HTTP tests")
        cls.temp = tempfile.TemporaryDirectory(prefix="wrapper-http-test-")
        directory = Path(cls.temp.name)
        cls.launcher = directory / "worker-launch"
        cls.launcher.write_text(
            "#!/bin/sh\nexec " + shlex.quote(sys.executable) + " " +
            shlex.quote(str(ROOT / "tests/mock_worker.py")) + ' "$@"\n'
        )
        cls.launcher.chmod(0o700)
        elf = bytearray(64)
        elf[:6] = b"\x7fELF\x02\x01"
        cls.profile = json.loads((ROOT/f"profiles/apple-music-1109-{cls.architecture}.json").read_text())
        elf[18:20] = cls.profile["elf_machine"].to_bytes(2,"little")
        worker = directory/"system/bin/apple-music-worker"
        worker.parent.mkdir(parents=True)
        worker.write_bytes(elf)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            cls.port = sock.getsockname()[1]
        env = dict(os.environ,
                   WRAPPER_HOST="127.0.0.1", WRAPPER_PORT=str(cls.port), WRAPPER_ARCH=cls.architecture,
                   WRAPPER_LAUNCHER=str(cls.launcher),
                   WRAPPER_ROOTFS=str(directory),
                   WRAPPER_DEVICE_INFO="a/b/c/d/e/f/g/h/i", RUST_LOG="info")
        cls.log_path = directory / "wrapper.jsonl"
        cls.log_file = cls.log_path.open("wb")
        cls.server = subprocess.Popen(
            [str(binary)], env=env, stdout=subprocess.DEVNULL,
            stderr=cls.log_file,
        )
        for _ in range(100):
            try:
                status, _ = cls.request("GET", "/health", auth=False)
                if status == 200:
                    break
            except Exception:
                time.sleep(0.05)
        else:
            raise RuntimeError("HTTP server did not start")

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "server"):
            cls.server.terminate()
            try:
                cls.server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                cls.server.kill()
                cls.server.wait()
        if hasattr(cls, "log_file"):
            cls.log_file.close()
        if hasattr(cls, "temp"):
            cls.temp.cleanup()

    @classmethod
    def request(cls, method, path, payload=None, auth=True):
        headers = {"Content-Type": "application/json"}
        data = json.dumps(payload).encode() if payload is not None else None
        request = Request(
            f"http://127.0.0.1:{cls.port}{path}", data=data,
            method=method, headers=headers,
        )
        try:
            response = urlopen(request, timeout=8)
        except HTTPError as err:
            response = err
        content = json.loads(response.read())
        assert response.headers["Cache-Control"] == "no-store"
        return response.status, content

    def test_01_unversioned_api_without_bearer(self):
        health_status, health = self.request("GET", "/health", auth=False)
        self.assertEqual(health_status, 200)
        self.assertEqual(health["version"], "1.0.0")
        self.assertEqual(self.request("GET", "/info", auth=False)[0], 200)
        self.assertEqual(self.request("GET", "/session/credentials")[0], 403)
        status, info = self.request("GET", "/info")
        self.assertEqual(status, 200)
        self.assertEqual(info["decryption"], "client_side")
        self.assertEqual(info["version"], "1.0.0")
        self.assertEqual(info["protocol_version"], 1)
        self.assertEqual(info["architecture"], self.architecture)
        self.assertEqual(info["profile"], self.profile["profile"])
        self.assertEqual(info["context_supported"], self.profile["context_supported"])
        self.assertEqual("context" in info["features"], self.profile["context_supported"])
        self.assertIn("credentials", info["features"])
        self.assertEqual(self.request("GET", "/v1/info")[0], 404)
        self.assertEqual(self.request("GET", "/v1/session")[0], 404)

    def test_02_login_challenge_and_busy(self):
        status, info = self.request("POST", "/session/login",
                                    {"username": "twofactor", "password": "dummy"})
        self.assertEqual(status, 202)
        self.assertEqual(info["kind"], "two_factor")
        self.assertEqual(self.request("GET", "/session")[0], 503)
        self.assertEqual(self.request("POST", "/session/challenge",
                                      {"challenge_id": "incorrect", "code": "123456"})[0], 400)
        status, info = self.request("POST", "/session/challenge",
                                    {"challenge_id": info["challenge_id"], "code": "123456"})
        self.assertEqual(status, 200)
        self.assertEqual(info["state"], "authenticated")

    def test_03_playback_context_and_validation(self):
        status, playback = self.request("GET", "/playback?adam_id=12345")
        self.assertEqual(status, 200)
        self.assertEqual(playback["songList"][0]["id"], "12345")
        query = urlencode({"adam_id": "12345", "key_uri": "skd://example/test?x=1"})
        status, context = self.request("GET", "/context?" + query)
        if not self.profile["context_supported"]:
            self.assertEqual(status,502)
            self.assertEqual(context["error"]["code"],"unsupported_capture_profile")
            status,alias=self.request("GET","/key?"+urlencode({"adamId":"12345","uri":"skd://example/test"}))
            self.assertEqual(alias["error"]["code"],"unsupported_capture_profile")
            self.assertEqual(self.request("POST", "/decrypt", {"samples":[]})[0], 404)
            return
        self.assertEqual(status, 200)
        self.assertIn("ctx", context)
        self.assertTrue(context["profile"].startswith("apple-music-"))
        status, alias = self.request("GET", "/key?" + urlencode(
            {"adamId": "12345", "uri": "skd://example/test?x=1"}))
        self.assertEqual(status, 200)
        self.assertEqual(alias, context)
        self.assertEqual(self.request("GET", "/playback?adam_id=1&adam_id=2")[0], 400)
        self.assertEqual(self.request("GET", "/context?adam_id=1&key_uri=bad")[0], 400)
        status, invalid = self.request("GET", "/context?" + urlencode(
            {"adam_id": "6666", "key_uri": "skd://test"}))
        self.assertEqual(status, 503)
        self.assertEqual(invalid["error"]["code"], "invalid_context")
        self.assertEqual(self.request("POST", "/decrypt", {"samples":[]})[0], 404)

    def test_04_worker_crashes_and_restarts(self):
        self.assertEqual(self.request("GET", "/playback?adam_id=8888")[0], 503)
        self.assertEqual(self.request("GET", "/session")[1]["state"], "logged_out")
        self.assertEqual(self.request("POST", "/session/restore")[0], 200)
        self.assertEqual(self.request("GET", "/playback?adam_id=9999")[0], 503)
        self.assertEqual(self.request("GET", "/session")[1]["state"], "logged_out")
        self.assertEqual(self.request("POST", "/session/close")[0], 200)
        self.assertEqual(self.request("GET", "/session")[1]["state"], "logged_out")

    def test_05_two_factor_cancellation(self):
        status, challenge = self.request("POST", "/session/login",
                                         {"username": "twofactor", "password": "dummy"})
        self.assertEqual(status, 202)
        status, error = self.request("POST", "/session/challenge",
                                     {"challenge_id": challenge["challenge_id"], "code": ""})
        self.assertEqual(status, 502)
        self.assertEqual(error["error"]["code"], "authentication_failed")
        self.assertEqual(self.request("GET", "/session")[1]["state"], "logged_out")

    def test_06_direct_login_without_challenge(self):
        status, session = self.request("POST", "/session/login",
                                       {"username": "mock-account", "password": "mock-password"})
        self.assertEqual(status, 200)
        self.assertEqual(session["state"], "authenticated")
        self.assertEqual(self.request("GET", "/session")[1]["state"], "authenticated")

    def test_07_credentials_export(self):
        status, credentials = self.request("GET", "/session/credentials", auth=False)
        self.assertEqual(status, 200)
        self.assertEqual(credentials["dev_token"], "mock-developer-token")
        self.assertEqual(credentials["music_user_token"], "mock-media-user-token")

    def test_08_logs_are_jsonl_and_do_not_contain_request_secrets(self):
        secret = "private-nonlogging-marker-0123456789"
        self.assertEqual(self.request("GET", "/context?" + urlencode({
            "adam_id": "12345", "key_uri": "skd://" + secret,
        }))[0], 200)
        self.assertEqual(self.request("POST", "/v1/" + secret,
                         {"password": secret})[0], 404)
        lines = self.log_path.read_text().splitlines()
        self.assertGreater(len(lines), 0)
        self.assertNotIn(secret, "\n".join(lines))
        self.assertNotIn("skd://", "\n".join(lines))
        self.assertNotIn("mock-media-user-token", "\n".join(lines))
        events = [json.loads(line) for line in lines]
        self.assertTrue(all("timestamp" in item and "level" in item
                            and "event" in item for item in events))
        self.assertTrue(any(item["event"] == "server.started" for item in events))
        requests = [item for item in events if item["event"] == "http.request"]
        self.assertTrue(any(item["path"] == "/context" and item["status"] == 200
                            for item in requests))
        self.assertTrue(any(item["path"] == "<unknown>" and item["status"] == 404
                            for item in requests))
        self.assertTrue(all(isinstance(item["request_id"], int)
                            and isinstance(item["duration_ms"], int)
                            and isinstance(item["status"], int) for item in requests))
        self.assertFalse(any(item.get("path") == "/health" for item in requests))


class ArmServerContract(ServerContract):
    architecture = "arm64-v8a"

if __name__ == "__main__":
    unittest.main()
