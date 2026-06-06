#!/usr/bin/env python3
"""Differential fuzzer for HyperBridge (x86-32 / x86-64).

Generates random instruction streams, validates them via Capstone,
runs them in parallel through both HyperBridge and Unicorn,
and isolates and shrinks semantic/decode divergences.
"""

from __future__ import annotations

import argparse
import json
import random
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Any, Set, Tuple

# Enable importing unicorn_adapter from tools/hb_oracle
ROOT_DIR = Path(__file__).resolve().parents[2]
sys.path.append(str(ROOT_DIR / "tools" / "hb_oracle"))

try:
    from capstone import CS_ARCH_X86, CS_MODE_32, CS_MODE_64, Cs
    import unicorn_adapter
except ImportError as e:
    print(f"Error importing dependencies: {e}", file=sys.stderr)
    sys.exit(1)


def generate_random_instruction(rng: random.Random, mode: str) -> bytes:
    """Generates a random sequence of bytes of random length (1 to 15)."""
    length = rng.randint(1, 15)
    
    # We can inject some biases to generate valid opcodes/prefixes more frequently
    prefixes = [b"", b"\x66", b"\x67", b"\xf0", b"\xf2", b"\xf3"]
    if mode == "x64":
        prefixes.extend([b"\x48", b"\x41", b"\x44"])
        
    code = bytearray(rng.choice(prefixes))
    while len(code) < length:
        code.append(rng.randint(0, 255))
        
    return bytes(code)


def shrink_reproducer(code: bytes, mode: str, runner_bin: Path, seed: int) -> bytes:
    """Recursively shrinks a failing fuzzed instruction to the smallest failing bytes."""
    md_val = CS_MODE_64 if mode == "x64" else CS_MODE_32
    md = Cs(CS_ARCH_X86, md_val)
    insns = list(md.disasm(code, 0x100000, count=1))
    if not insns:
        return code
        
    insn = insns[0]
    shrunk = code[:insn.size]
    
    def check_fails(bytes_to_check: bytes) -> bool:
        # Check if it still causes a mismatch or crash
        oracle = unicorn_adapter.run_case(seed, bytes_to_check.hex(), arch=mode)
        if not oracle.get("ok"):
            return False # Skip if oracle fails
            
        payload = f"{seed} {bytes_to_check.hex()}\n"
        env = {"HB_DIFF_ARCH": mode, "HB_DIFF_INTERP_ONLY": "1"}
        p = subprocess.run([str(runner_bin)], input=payload, text=True, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
        res = json.loads(p.stdout.strip())
        
        if not res.get("ok"):
            return True # HyperBridge crashed/failed
            
        mismatches = unicorn_adapter.diff_interpreter(res, oracle, arch=mode)
        return len(mismatches) > 0

    if check_fails(shrunk):
        while len(shrunk) > 1:
            first_byte = shrunk[0]
            if first_byte in [0x66, 0x67, 0xF0, 0xF2, 0xF3] or (mode == "x64" and 0x40 <= first_byte <= 0x4F):
                candidate = shrunk[1:]
                if check_fails(candidate):
                    shrunk = candidate
                    continue
            break
        return shrunk
    return code


def run_fuzz(mode: str, iterations: int, seed_val: int) -> int:
    runner_bin = ROOT_DIR / "engine" / "hyperbridge" / "tests" / "hb_diff_case_runner"
    if not runner_bin.exists():
        print(f"Error: trace runner binary {runner_bin} not found. Build HyperBridge first.", file=sys.stderr)
        return 1
        
    print(f"Starting differential fuzzer in {mode} mode (seed={seed_val}, iterations={iterations})...")
    rng = random.Random(seed_val)
    md_val = CS_MODE_64 if mode == "x64" else CS_MODE_32
    md = Cs(CS_ARCH_X86, md_val)
    
    unique_bugs: Dict[str, Tuple[bytes, str]] = {}
    checked_count = 0
    valid_count = 0
    mismatches_count = 0
    
    for i in range(iterations):
        code = generate_random_instruction(rng, mode)
        insns = list(md.disasm(code, 0x100000, count=1))
        if not insns:
            continue
            
        insn = insns[0]
        valid_count += 1
        code_hex = code[:insn.size].hex()
        
        # 1. Run Unicorn expected
        oracle = unicorn_adapter.run_case(seed_val, code_hex, arch=mode)
        if not oracle.get("ok"):
            continue
            
        checked_count += 1
        
        # 2. Run HyperBridge actual
        payload = f"{seed_val} {code_hex}\n"
        env = {"HB_DIFF_ARCH": mode, "HB_DIFF_INTERP_ONLY": "1"}
        p = subprocess.run([str(runner_bin)], input=payload, text=True, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
        res = json.loads(p.stdout.strip())
        
        # 3. Check for mismatches
        mismatch_desc = ""
        is_mismatch = False
        
        if not res.get("ok"):
            is_mismatch = True
            mismatch_desc = "hb_crash"
        else:
            mismatches = unicorn_adapter.diff_interpreter(res, oracle, arch=mode)
            if mismatches:
                is_mismatch = True
                mismatch_desc = mismatches[0] # Take first mismatch description
                
        if is_mismatch:
            mismatches_count += 1
            # Shrink to minimal reproducer
            shrunk_bytes = shrink_reproducer(code, mode, runner_bin, seed_val)
            bug_key = f"{insn.mnemonic}:{mismatch_desc.split(' ')[0]}"
            if bug_key not in unique_bugs:
                unique_bugs[bug_key] = (shrunk_bytes, mismatch_desc)
                print(f"Found unique bug [{bug_key}]: repro_bytes={shrunk_bytes.hex()} expected='{insn.mnemonic} {insn.op_str}' diff={mismatch_desc}")
                
        if checked_count % 500 == 0 and checked_count > 0:
            print(f"Fuzz progress: valid={valid_count} checked={checked_count} unique_bugs={len(unique_bugs)}")
            
    print("\n--- Fuzzing Campaign Finished ---")
    print(f"Total valid instructions generated: {valid_count}")
    print(f"Total semantically checked: {checked_count}")
    print(f"Total divergences found: {mismatches_count}")
    print(f"Unique bug profiles isolated: {len(unique_bugs)}")
    
    # Save fuzzed findings to reports/research/
    fuzz_report_path = ROOT_DIR / "reports" / "research" / f"HB-{mode.upper()}-FUZZ-findings.json"
    findings = {
        "mode": mode,
        "seed": seed_val,
        "iterations": iterations,
        "checked_count": checked_count,
        "unique_bugs_count": len(unique_bugs),
        "bugs": {k: {"repro": v[0].hex(), "diff": v[1]} for k, v in unique_bugs.items()}
    }
    fuzz_report_path.write_text(json.dumps(findings, indent=2), encoding="utf-8")
    print(f"Fuzz findings saved to: {fuzz_report_path}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Differential fuzzer for HyperBridge")
    ap.add_argument("--mode", default="x86", choices=["x86", "x64"], help="Instruction width mode")
    ap.add_argument("--iterations", type=int, default=2000, help="Number of fuzzed instructions to try")
    ap.add_argument("--seed", type=int, default=42, help="RNG seed")
    args = ap.parse_args()
    return run_fuzz(args.mode, args.iterations, args.seed)


if __name__ == "__main__":
    sys.exit(main())
