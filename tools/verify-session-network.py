"""Verify encrypted, distinct player records across a real server restart.

Starts only owned hidden processes and uses an isolated synthetic seed, invites
and local profile names. Generated credentials stay in ignored runtime folders.
"""
import argparse
import json
import os
from pathlib import Path
import runpy
import socket
import subprocess
import time
import uuid


def verify(editor: Path, players: int, port: int, timeout: int, render: str = "dx12", server_mode: str = "dedicated") -> Path:
    if not editor.is_file() or not 1 <= port <= 65535:
        raise ValueError("An existing editor executable and valid UDP port are required")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        if os.name == "nt":
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        probe.bind(("127.0.0.1", port))
    workspace = Path(__file__).resolve().parent.parent
    project = workspace / "ue-project" / "VoxelEarth.uproject"
    run_id = "session-net-" + uuid.uuid4().hex
    directory = workspace / "Saved" / "Tests" / run_id
    directory.mkdir(parents=True)
    create = runpy.run_path(str(Path(__file__).with_name("create-session-transport.py")))["create"]
    create(directory / "private", players)
    seed = time.time_ns() % (2**63)
    save = project.parent / "Saved" / "VoxelWorlds" / f"{seed}.vxlog"
    if save.exists() or Path(str(save) + ".checkpoints").exists():
        raise RuntimeError("Refusing an existing world save")
    invites = [json.loads((directory / "private" / f"invite-{i}.json").read_text()) for i in range(1, players + 1)]

    def phase(role: str):
        phase_dir = directory / role
        phase_dir.mkdir()
        owned = []
        deadline = time.monotonic() + timeout
        common = [str(project), "-unattended", "-nosplash", "-nosound", "-Multiprocess", "-NoSteam",
                  "-VoxelNoMenu", "-VoxelSyntheticTerrain", f"-VoxelSeed={seed}", f"-VoxelSessionVerifyDir={phase_dir}",
                  f"-VoxelSessionVerifyPlayers={players}"]

        def start(label, url, extra):
            log = phase_dir / f"{label}.log"
            command = [str(editor), common[0], url, *common[1:], f"-abslog={log}", *extra]
            process = subprocess.Popen(command, cwd=workspace, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                       creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            owned.append(process)
            return log

        def wait_for(predicate, label):
            while time.monotonic() < deadline:
                if predicate():
                    return
                if any(process.poll() is not None for process in owned):
                    raise RuntimeError(f"An owned process exited before {label}; inspect {phase_dir}")
                time.sleep(0.25)
            raise TimeoutError(f"Timed out waiting for {label}; inspect {phase_dir}")

        try:
            server_flags = ["-server", "-nullrhi"] if server_mode == "dedicated" else ["-game", f"-{render}", "-sm6", "-windowed", "-ResX=640", "-ResY=360"]
            server_url = "/Engine/Maps/Entry" + ("?listen" if server_mode == "listen" else "")
            server_log = start("server", server_url, [*server_flags, f"-port={port}", f"-VoxelSessionVerify={role}",
                              f"-VoxelTransportKeys={directory / 'private' / 'server.json'}"])
            wait_for(lambda: server_log.exists() and f"listening on port {port}" in server_log.read_text(errors="replace"), "server listen")
            for number, invite in enumerate(invites, 1):
                start(f"client-{number}", f"127.0.0.1:{port}?EncryptionToken={invite['id']}",
                      ["-game", f"-{render}", "-sm6", "-windowed", "-ResX=640", "-ResY=360", "-VoxelSessionVerify=client", f"-VoxelPlayerProfile={run_id}-client-{number}",
                       f"-VoxelTransportInvite={directory / 'private' / f'invite-{number}.json'}"])
            report = phase_dir / "ready.json"
            wait_for(lambda: report.exists(), "server state verification")
            result = json.loads(report.read_text(encoding="utf-8-sig"))
            if not result["passed"] or len(result["players"]) != players:
                raise RuntimeError(f"Server state verification failed; inspect {report}")
            if len({row["id"] for row in result["players"]}) != players or not all(row["encrypted"] for row in result["players"]):
                raise RuntimeError("Distinct encrypted admissions were not established")
            (phase_dir / "stop").touch()
            end = time.monotonic() + 45
            for process in owned:
                process.wait(timeout=max(1, end - time.monotonic()))
                if process.returncode != 0:
                    raise RuntimeError(f"Owned process exited unsuccessfully; inspect {phase_dir}")
            return result
        finally:
            (phase_dir / "stop").touch(exist_ok=True)
            for process in owned:
                if process.poll() is None:
                    process.terminate()
            for process in owned:
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()

    initial = phase("write")
    restored = phase("read")
    if initial["worldId"] != restored["worldId"] or initial["players"] != restored["players"]:
        raise RuntimeError("World or player identity/state changed across server restart")
    result = {"passed": True, "players": players, "server_mode": server_mode, "seed": seed, "port": port, "write": initial, "read": restored}
    (directory / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    return directory


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--editor", type=Path, default=Path("D:/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe"))
    parser.add_argument("--players", type=int, choices=range(1, 17), default=2)
    parser.add_argument("--port", type=int, default=17991)
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument("--render", choices=("dx12", "nullrhi"), default="dx12")
    parser.add_argument("--server-mode", choices=("dedicated", "listen"), default="dedicated")
    args = parser.parse_args()
    print("PASS: encrypted player restart verification:", verify(args.editor, args.players, args.port, args.timeout, args.render, args.server_mode))
