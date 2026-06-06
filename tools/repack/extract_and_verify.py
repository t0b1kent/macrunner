#!/usr/bin/env python3
import os
import sys
import subprocess
import hashlib

# Expected MD5 manifest for test_open.arc (a-dirs.arc clone)
EXPECTED_MANIFEST = {
    "4.cmd": "60a957659a2f1d6fccc5c6cf4a2a77be",
    "5.cmd": "5a3c3814b12673568aa1cb7388b5a626",
    "2/4.cmd": "60a957659a2f1d6fccc5c6cf4a2a77be",
    "2/5.cmd": "5a3c3814b12673568aa1cb7388b5a626",
    "2/22/4.cmd": "60a957659a2f1d6fccc5c6cf4a2a77be",
    "2/22/5.cmd": "5a3c3814b12673568aa1cb7388b5a626",
    "3/4.cmd": "60a957659a2f1d6fccc5c6cf4a2a77be"
}

def calculate_md5(filepath):
    hasher = hashlib.md5()
    with open(filepath, 'rb') as f:
        while chunk := f.read(8192):
            hasher.update(chunk)
    return hasher.hexdigest()

def main():
    if len(sys.argv) < 4:
        print("Usage: extract_and_verify.py <unarc_bin> <archive_path> <out_dir>")
        sys.exit(1)

    unarc_bin = sys.argv[1]
    archive_path = sys.argv[2]
    out_dir = sys.argv[3]

    print(f"Extracting {archive_path} to {out_dir} using {unarc_bin}...")
    
    # Ensure output directory exists
    os.makedirs(out_dir, exist_ok=True)

    # unarc command: unarc x -o+ -dp<out_dir> <archive_path>
    # FreeArc's unarc has a specific syntax where -dp must be immediately followed by the path.
    cmd = [unarc_bin, "x", "-o+", f"-dp{out_dir}", archive_path]
    
    try:
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False)
        print("--- extractor stdout ---")
        print(result.stdout)
        print("--- extractor stderr ---")
        print(result.stderr)
        
        if result.returncode != 0:
            print(f"Error: unarc exited with non-zero code {result.returncode}")
            print("REPACK RungA FAIL (extractor exited with error)")
            sys.exit(2)
    except Exception as e:
        print(f"Failed to execute extractor: {e}")
        print("REPACK RungA FAIL (execution failed)")
        sys.exit(3)

    # Verify files
    mismatches = []
    missing = []
    verified_count = 0

    for rel_path, expected_md5 in EXPECTED_MANIFEST.items():
        full_path = os.path.join(out_dir, rel_path)
        if not os.path.exists(full_path):
            missing.append(rel_path)
            continue

        actual_md5 = calculate_md5(full_path)
        if actual_md5 != expected_md5:
            mismatches.append(f"{rel_path} (expected: {expected_md5}, got: {actual_md5})")
        else:
            verified_count += 1

    total_files = len(EXPECTED_MANIFEST)
    if missing or mismatches:
        print(f"Verification failed: {len(missing)} missing, {len(mismatches)} mismatched.")
        if missing:
            print("Missing files:", missing)
        if mismatches:
            print("Mismatched files:", mismatches)
        print(f"REPACK RungA FAIL ({verified_count}/{total_files} files verified, mismatch: {len(mismatches)}, missing: {len(missing)})")
        sys.exit(4)

    print(f"All {total_files} files successfully verified against manifest!")
    print(f"REPACK RungA PASS ({verified_count}/{total_files} files, mismatch: 0)")
    sys.exit(0)

if __name__ == '__main__':
    main()
