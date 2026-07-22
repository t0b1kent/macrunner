#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
import pathlib
import signal
import subprocess
import time


SIGNAL_POLICY_COMMANDS = [
    "process handle SIGSEGV --stop false --notify false --pass true",
    "process handle SIGBUS  --stop false --notify false --pass true",
    "process handle SIGILL  --stop false --notify false --pass true",
    "process handle SIGUSR1 --stop false --notify false --pass true",
    "process handle SIGUSR2 --stop false --notify false --pass true",
    "process handle SIGINT  --stop true  --notify true  --pass false",
    "process handle -t",
]

REQUIRED_SIGNAL_POLICY = {
    "SIGSEGV": ("true", "false", "false"),
    "SIGBUS": ("true", "false", "false"),
    "SIGILL": ("true", "false", "false"),
    "SIGUSR1": ("true", "false", "false"),
    "SIGUSR2": ("true", "false", "false"),
}


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


def parse_signal_policy(stdout):
    rows = {}
    for raw_line in stdout.splitlines():
        parts = raw_line.split()
        if len(parts) >= 4 and parts[0] in REQUIRED_SIGNAL_POLICY:
            rows[parts[0]] = tuple(value.lower() for value in parts[1:4])
    ok = all(rows.get(name) == expected
             for name, expected in REQUIRED_SIGNAL_POLICY.items())
    return ok, rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--tid", type=int, required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--capture-script", required=True)
    parser.add_argument("--timeout", type=int, default=30)
    parser.add_argument("--expect", choices=("hit", "timeout"), required=True)
    args = parser.parse_args()

    output = pathlib.Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    capture_script = pathlib.Path(args.capture_script).resolve()
    commands = [
        "/usr/bin/lldb",
        "--batch",
        "--attach-pid", str(args.pid),
        "-o", "command script import %s" % capture_script,
        "-o", "thread list",
    ]
    for command in SIGNAL_POLICY_COMMANDS:
        commands.extend(["-o", command])
    commands.extend([
        "-o", ("breakpoint set --name hb_jit_helper_exec_two_block_loop "
               "--one-shot true --thread-id 0x%x" % args.tid),
        "-o", "continue",
        "-o", "thread info",
        "-o", "thread list",
        "-o", "hk-capture %s" % output,
        "-o", "process detach",
        "-o", "quit",
    ])
    started = int(time.time())
    proc = subprocess.Popen(commands, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True,
                            start_new_session=True)
    timed_out = False
    try:
        stdout, stderr = proc.communicate(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        os.killpg(proc.pid, signal.SIGINT)
        try:
            stdout, stderr = proc.communicate(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate()

    (output / "lldb.stdout").write_text(stdout)
    (output / "lldb.stderr").write_text(stderr)
    capture = output / "capture.json"
    capture_status = None
    capture_thread_id = None
    capture_function = None
    capture_stop_reason = None
    if capture.exists():
        capture_json = json.loads(capture.read_text())
        capture_status = capture_json.get("status")
        capture_thread_id = capture_json.get("thread_id")
        capture_function = capture_json.get("function")
        capture_stop_reason = capture_json.get("stop_reason")
    policy_ok, policy_rows = parse_signal_policy(stdout)
    capture_tid_match = capture_thread_id == args.tid
    target_alive = alive(args.pid)
    timeout_detach_probe = None
    if args.expect == "timeout" and timed_out and target_alive:
        probe_commands = [
            "/usr/bin/lldb", "--batch", "--attach-pid", str(args.pid),
            "-o", "process detach", "-o", "quit",
        ]
        probe = subprocess.run(probe_commands, capture_output=True, text=True,
                               timeout=10, start_new_session=True)
        (output / "detach-probe.stdout").write_text(probe.stdout)
        (output / "detach-probe.stderr").write_text(probe.stderr)
        timeout_detach_probe = {
            "argv": probe_commands,
            "returncode": probe.returncode,
            "target_alive_after": alive(args.pid),
        }
    passed = ((args.expect == "hit" and not timed_out and proc.returncode == 0
               and policy_ok and capture_status == "PASS" and capture_tid_match
               and capture_function == "hb_jit_helper_exec_two_block_loop"
               and target_alive) or
              (args.expect == "timeout" and timed_out and policy_ok
               and not capture.exists() and target_alive and timeout_detach_probe
               and timeout_detach_probe["returncode"] == 0
               and timeout_detach_probe["target_alive_after"]))
    record = {
        "schema": "macrunner.hk.lldb-capture-supervisor.v1",
        "argv": commands,
        "capture_script_sha256": sha256(capture_script),
        "capture_function": capture_function,
        "capture_status": capture_status,
        "capture_stop_reason": capture_stop_reason,
        "capture_thread_id": capture_thread_id,
        "capture_tid_match": capture_tid_match,
        "expect": args.expect,
        "finished_epoch": int(time.time()),
        "lldb_returncode": proc.returncode,
        "passed": passed,
        "pid": args.pid,
        "signal_policy_ok": policy_ok,
        "signal_policy_rows": policy_rows,
        "started_epoch": started,
        "target_alive_after": target_alive,
        "tid": args.tid,
        "timed_out": timed_out,
        "timeout_detach_probe": timeout_detach_probe,
        "timeout_seconds": args.timeout,
    }
    with open(output / "supervisor.json", "w", encoding="utf-8") as stream:
        json.dump(record, stream, indent=2, sort_keys=True)
        stream.write("\n")
    print("PASS" if passed else "FAIL")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
