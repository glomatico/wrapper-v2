#!/usr/bin/env python3
"""Deterministic *synthetic* native worker for end-to-end HTTP contract tests."""
import base64
import json
import os
import struct
import sys

from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"tools"))
from architecture import load_profile
profile = load_profile(os.environ.get("WRAPPER_ARCH", "x86_64"))
PROFILE = profile["profile"]
session = "logged_out"


def read_exact(n):
    result = bytearray()
    while len(result) < n:
        chunk = sys.stdin.buffer.read(n - len(result))
        if not chunk:
            raise EOFError
        result.extend(chunk)
    return bytes(result)


def recv():
    size = struct.unpack(">I", read_exact(4))[0]
    if not 0 < size <= 1024 * 1024:
        raise ValueError("invalid size")
    return json.loads(read_exact(size))


def send(value):
    data = json.dumps(value, separators=(",", ":")).encode()
    sys.stdout.buffer.write(struct.pack(">I", len(data)) + data)
    sys.stdout.buffer.flush()


def reply(id_, value=None, error=None, auth=None, restart_required=False, retryable=False):
    if error:
        packet={"v": 1, "id": id_, "ok": False, "error": {
            "code": error, "retryable": retryable or restart_required,
            "restart_required": restart_required,
        }}
        if auth is not None:
            packet["error"]["auth"]=auth
        send(packet)
    else:
        send({"v": 1, "id": id_, "ok": True, "result": value or {}})


def main():
    global session
    assert len(sys.argv) == 2
    while True:
        try:
            req = recv()
        except EOFError:
            return
        id_ = req["id"]
        op = req["op"]
        params = req["params"]
        if op == "initialize":
            reply(id_, {"profile": PROFILE, "architecture":profile["architecture"],
                        "context_supported":profile["context_supported"],
                        "protocol": 1, "session": {"state": session}})
        elif op == "session.status":
            reply(id_, {"state": session, "storefront": "143441"})
        elif op == "session.credentials":
            if session != "authenticated":
                reply(id_, error="authentication_required")
            else:
                reply(id_, {"dev_token": "mock-developer-token",
                            "music_user_token": "mock-media-user-token"})
        elif op == "session.restore":
            fault = Path(sys.argv[1]) / "restore-fault"
            if fault.exists():
                if fault.read_text() == "eof":
                    return
                reply(id_, error="session_validation_failed", retryable=True)
            elif os.environ.get("WRAPPER_MOCK_NO_CACHED_SESSION") == "1":
                reply(id_, error="no_cached_session")
            else:
                session = "authenticated"
                reply(id_, {"state": session, "storefront": "143441"})
        elif op == "session.login":
            if params["username"] == "authdiagnostic":
                reply(id_, error="authentication_failed", auth={
                    "stage":"native_response","callback_failure":"none",
                    "response_type":2,"apple_error_code":-42,
                    "credential_callbacks":1,"dialog_callbacks":1,
                    "unmatched_dialogs":0,"hsa2_requested":False,
                    "known_dialog_selected":True,
                    "unexpected_native_text":"SHOULD_NOT_LEAK",
                })
                continue
            if params["username"] == "twofactor":
                send({"v": 1, "id": id_, "event": "auth.challenge",
                      "challenge_id": "proof-1", "kind": "two_factor"})
                answer = recv()
                if (answer["id"] != id_ or answer["op"] != "auth.respond"
                        or answer["params"]["challenge_id"] != "proof-1"):
                    raise ValueError("invalid challenge")
                if answer["params"]["code"] != "123456":
                    reply(id_, error="authentication_failed")
                    continue
            session = "authenticated"
            reply(id_, {"state": session, "storefront": "143441"})
        elif op == "playback.get":
            marker_dir = os.environ.get("WRAPPER_MOCK_RECYCLE_ONCE_DIR")
            marker = Path(marker_dir) / "playback-recycled" if marker_dir else None
            if params["adam_id"] == "7777" and (marker is None or not marker.exists()):
                if marker is not None:
                    marker.touch()
                reply(id_, error="worker_recycle", restart_required=True)
                return
            if params["adam_id"] == "9999":
                os._exit(70)
            if params["adam_id"] == "8888":
                reply("wrong-request-id", {"songList": []})
                continue
            if session != "authenticated":
                reply(id_, error="authentication_required")
            else:
                reply(id_, {"songList": [{"id": params["adam_id"], "codec": "alac"}]})
        elif op == "context.get":
            marker_dir = os.environ.get("WRAPPER_MOCK_RECYCLE_ONCE_DIR")
            marker = Path(marker_dir) / "context-recycled" if marker_dir else None
            if params["adam_id"] == "7777" and (marker is None or not marker.exists()):
                if marker is not None:
                    marker.touch()
                reply(id_, error="worker_recycle", restart_required=True)
                return
            if not profile["context_supported"]:
                reply(id_,error="unsupported_capture_profile")
            elif session != "authenticated":
                reply(id_, error="authentication_required")
            else:
                invalid = params["adam_id"] == "6666"
                contract = profile["context_contract"]
                context = {
                    "profile": profile["context_profile"],
                    "ctx": base64.b64encode(bytes(1 if invalid else contract["ctx_bytes"])).decode(),
                    "state": base64.b64encode(bytes(contract["state_bytes"])).decode(),
                }
                context.update({field: 5 for field in contract["byte_fields"]})
                reply(id_, {"context": context})
        elif op == "session.close":
            reply(id_, {"state": "closed"})
            return
        else:
            reply(id_, error="unknown_operation")


if __name__ == "__main__":
    main()
