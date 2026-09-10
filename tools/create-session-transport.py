"""Create distinct AES-GCM transport invites in a new private output directory.

Keys are written to files only. Console output contains no secret material.
The server keeps server.json; give each client only its invite-N.json through
an authenticated private channel. Never commit these generated files.
"""
import argparse
import json
import os
from pathlib import Path
import secrets
import uuid


def create(output: Path, count: int) -> None:
    if not 1 <= count <= 16:
        raise ValueError("client count must be between 1 and 16")
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    keys = {}
    for number in range(1, count + 1):
        identity, key = uuid.uuid4().hex, secrets.token_hex(32)
        keys[identity] = key
        with (output / f"invite-{number}.json").open("x", encoding="utf-8") as stream:
            json.dump({"version": 1, "id": identity, "key": key}, stream)
            stream.flush()
            os.fsync(stream.fileno())
    with (output / "server.json").open("x", encoding="utf-8") as stream:
        json.dump({"version": 1, "keys": keys}, stream)
        stream.flush()
        os.fsync(stream.fileno())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--clients", type=int, default=2)
    args = parser.parse_args()
    create(args.output.resolve(), args.clients)
    print(f"Created {args.clients} distinct invites and the server key index.")
