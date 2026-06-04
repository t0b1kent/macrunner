#!/usr/bin/env python3
import os
import re
import signal
import subprocess
import time
from pathlib import Path

repo = Path("/Users/timurtoby/Documents/MacRunner/Main/MacRunner")
game = Path("/Users/timurtoby/Documents/MacRunner/Main/game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620")
run_id = os.environ.get("MR_HK_RUN_ID", "run-20260602-livelock-decisive-1800b")
duration = int(os.environ.get("MR_HK_DURATION", "1800"))
run_dir = repo / "reports/phase4-hollow-knight" / run_id
log_path = run_dir / "run.log"
d3d_path = run_dir / "d3d-trace.log"
samples_path = run_dir / "samples.tsv"
summary_path = run_dir / "final-summary.txt"
state_path = run_dir / "controller.state"

run_dir.mkdir(parents=True, exist_ok=True)
(repo / "artifacts/tmp").mkdir(parents=True, exist_ok=True)

env = os.environ.copy()
env.update(
    {
        "HOME": "/Users/timurtoby",
        "TMPDIR": str(repo / "artifacts/tmp"),
        "MACRUNNER_GRAPHICS_BACKEND": "dxmt",
        "MACRUNNER_HB_TRACE_HEARTBEAT": "1",
        "MACRUNNER_HB_TRACE_WAIT_SEMANTIC": "1",
        "MACRUNNER_HB_TRACE_WAIT_SEMANTIC_BUDGET": "5000",
        "MACRUNNER_HB_TRACE_IMAGE_API": "1",
        "MACRUNNER_HB_TRACE_IMAGE_API_BUDGET": "5000",
        "MACRUNNER_D3D_TRACE": "1",
        "MACRUNNER_D3D_TRACE_PATH": str(d3d_path),
    }
)

cmd = [
    str(repo / "scripts/mr-run.sh"),
    str(repo / "engine/wine/dist-arm64ec-spike"),
    "Hollow Knight.exe",
    str(duration),
    "--",
    "-logFile",
    "-",
]

heartbeat_re = re.compile(
    r"macrunner-hb-heartbeat: .* blocks=([0-9a-fA-F]+).* steps=([0-9a-fA-F]+).* block_pc=([^ ]+).* rva=([^ ]+)"
)


def count_text(path: Path, needle: str) -> int:
    if not path.exists():
        return 0
    return path.read_text(errors="replace").count(needle)


def grep_count(path: Path, pattern: str) -> int:
    if not path.exists():
        return 0
    rx = re.compile(pattern)
    return sum(1 for line in path.read_text(errors="replace").splitlines() if rx.search(line))


def child_stats(pid: int):
    try:
        out = subprocess.check_output(
            ["ps", "-axo", "pid=,ppid=,command="], text=True, errors="replace"
        )
        children = {}
        commands = {}
        for line in out.splitlines():
            parts = line.strip().split(None, 2)
            if len(parts) < 2:
                continue
            child_pid, parent_pid = parts[0], parts[1]
            command = parts[2] if len(parts) > 2 else ""
            children.setdefault(parent_pid, []).append(child_pid)
            commands[child_pid] = command

        queue = list(children.get(str(pid), []))
        descendants = []
        while queue:
            child_pid = queue.pop(0)
            descendants.append(child_pid)
            queue.extend(children.get(child_pid, []))

        child = ""
        for candidate in descendants:
            if "Hollow Knight.exe" in commands.get(candidate, ""):
                child = candidate
                break
        if not child and descendants:
            child = descendants[0]
        if not child:
            return "", "", ""
        out = subprocess.check_output(["ps", "-p", child, "-o", "%cpu=", "-o", "etime="], text=True).strip()
        parts = out.split(None, 1)
        return child, parts[0] if parts else "", parts[1] if len(parts) > 1 else ""
    except Exception:
        return "", "", ""


def sample(label: str, proc: subprocess.Popen):
    text = log_path.read_text(errors="replace") if log_path.exists() else ""
    hb_lines = [line for line in text.splitlines() if "macrunner-hb-heartbeat" in line]
    blocks_hex = steps_hex = block_pc = rva = ""
    blocks_dec = steps_dec = ""
    if hb_lines:
        best = None
        for line in hb_lines:
            m = heartbeat_re.search(line)
            if not m:
                continue
            blocks = int(m.group(1), 16)
            if best is None or blocks > best[0]:
                best = (blocks, m)
        if best:
            m = best[1]
            blocks_hex, steps_hex, block_pc, rva = m.groups()
            blocks_dec = str(int(blocks_hex, 16))
            steps_dec = str(int(steps_hex, 16))
    pid, pcpu, etime = child_stats(proc.pid)
    row = [
        label,
        str(int(time.time())),
        str(len(hb_lines)),
        blocks_hex,
        blocks_dec,
        steps_hex,
        steps_dec,
        rva,
        block_pc,
        str(grep_count(log_path, r"nt-get-context guest-x64|get-context import=")),
        str(count_text(log_path, "D3D11CreateDevice") + count_text(d3d_path, "D3D11CreateDevice")),
        str(count_text(log_path, "GfxDevice") + count_text(d3d_path, "GfxDevice")),
        str(count_text(log_path, "CreateSwapChain") + count_text(d3d_path, "CreateSwapChain")),
        pid,
        pcpu,
        etime,
    ]
    with samples_path.open("a") as f:
        f.write("\t".join(row) + "\n")


with samples_path.open("w") as f:
    f.write(
        "sample_s\tepoch\thb_count\tblocks_hex\tblocks_dec\tsteps_hex\tsteps_dec\trva\tblock_pc\tgetctx\td3d11\tgfx\tcreateswapchain\tpid\tpcpu\tetime\n"
    )

with log_path.open("wb") as log:
    proc = subprocess.Popen(cmd, cwd=game, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)

state_path.write_text(f"run_id={run_id}\nmr_pid={proc.pid}\nstarted={time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}\n")

last = 0
sample_points = [t for t in (60, 300, 600, 900, 1200, 1500, 1800) if t <= duration]
if duration not in sample_points:
    sample_points.append(duration)
for t in sample_points:
    delay = t - last
    if delay > 0:
        time.sleep(delay)
    sample(str(t), proc)
    last = t
    if proc.poll() is not None:
        break

run_rc = proc.wait()
sample("final", proc)

clean = subprocess.run([str(repo / "scripts/mr-clean.sh"), "--prune"], cwd=repo, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
(run_dir / "clean.log").write_text(clean.stdout)

tail = "\n".join((log_path.read_text(errors="replace").splitlines()[-80:] if log_path.exists() else []))
summary_path.write_text(
    f"run_rc={run_rc} clean_rc={clean.returncode}\n"
    + samples_path.read_text(errors="replace")
    + "counts:\n"
    + f"getctx={grep_count(log_path, r'nt-get-context guest-x64|get-context import=')}\n"
    + f"d3d11={count_text(log_path, 'D3D11CreateDevice') + count_text(d3d_path, 'D3D11CreateDevice')}\n"
    + f"gfx={count_text(log_path, 'GfxDevice') + count_text(d3d_path, 'GfxDevice')}\n"
    + f"createswapchain={count_text(log_path, 'CreateSwapChain') + count_text(d3d_path, 'CreateSwapChain')}\n"
    + f"runtime_fail={count_text(log_path, 'macrunner-hb-runtime-fail')}\n"
    + f"jit_fallback={count_text(log_path, 'macrunner-hb-jit-fallback')}\n"
    + "tail:\n"
    + tail
    + "\n"
)
