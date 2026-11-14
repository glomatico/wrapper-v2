#!/usr/bin/env python3
"""Deterministic *synthetic* native worker for end-to-end HTTP contract tests."""
import base64
import json
import os
import struct
import sys

PROFILE = "apple-music-3.6.0.1109-x86_64-research-v1"
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


def reply(id_, value=None, error=None):
    if error:
        send({"v": 1, "id": id_, "ok": False, "error": {
            "code": error, "retryable": False, "restart_required": False,
        }})
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
            reply(id_, {"profile": PROFILE, "protocol": 1, "session": {"state": session}})
        elif op == "session.status":
            reply(id_, {"state": session, "storefront": "143441"})
        elif op == "session.credentials":
            if session != "authenticated":
                reply(id_, error="authentication_required")
            else:
                reply(id_, {"dev_token": "mock-developer-token",
                            "music_user_token": "mock-media-user-token"})
        elif op == "session.restore":
            session = "authenticated"
            reply(id_, {"state": session, "storefront": "143441"})
        elif op == "session.login":
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
            if session != "authenticated":
                reply(id_, error="authentication_required")
            else:
                invalid = params["adam_id"] == "6666"
                reply(id_, {"context": {
                    "profile": PROFILE,
                    "ctx": base64.b64encode(bytes(1 if invalid else 0x8000)).decode(),
                    "state": base64.b64encode(bytes(0x800)).decode(),
                    "bl": 5,
                    "r9b": 17,
                }})
        elif op == "session.close":
            reply(id_, {"state": "closed"})
            return
        else:
            reply(id_, error="unknown_operation")


if __name__ == "__main__":
    main()
