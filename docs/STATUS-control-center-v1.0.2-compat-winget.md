# Control Center v1.0.2 Compat Winget Status

Date: 2026-05-14
Scope: compat runner winget manifests, downloader SHA-256 verification, runner result artifacts, public compat report.
Out of scope: engine/, wine-fork/, .hyperbridge-work, GitHub API/PR integration.

## Patch stack

- patches/control-center-v1.0.2-compat-1-winget-setup.patch
- patches/control-center-v1.0.2-compat-2-plan-manifest-reader.patch
- patches/control-center-v1.0.2-compat-3-downloader-sha256.patch
- patches/control-center-v1.0.2-compat-4-runner-result-json.patch
- patches/control-center-v1.0.2-compat-5-report-version-vault.patch

## Gate 1 - winget setup and index

```terminal
$ ./tools/compat-runner/winget_setup.sh && python3 tools/compat-runner/winget_index.py && sqlite3 "$HOME/.macrunner-compat/index.sqlite" "SELECT COUNT(*) FROM packages;"
updated, last manifest 2026-05-14T03:19:26Z, installer manifests 136410
indexed: 12835 packages in 28.1s -> /Users/timurtoby/.macrunner-compat/index.sqlite
12835
```

Result: PASS, index >= 5000 entries.

## Gate 2 - downloader SHA-256 PASS

```terminal
$ python3 tools/compat-runner/downloader.py --winget-id 7zip.7zip
cached to /Users/timurtoby/.macrunner-compat/cache/d64a0468.exe (sha256 verified d64a0468f5b5b0b0fc5b2188450bcd655b70809d97b1c4535f2884635094377d)
```

Result: PASS, real 7-Zip installer previously downloaded from winget manifest URL.

## Gate 3 - downloader tamper-fail

```terminal
$ python3 tools/compat-runner/downloader.py --url "file://$HOME/.macrunner-compat/cache/d64a0468.exe" --expect-sha256 0000000000000000000000000000000000000000000000000000000000000000 2>&1; rc=$?; echo "tamper_rc=$rc"; test "$rc" -ne 0
sha256 mismatch, expected 0000000000000000000000000000000000000000000000000000000000000000, got d64a0468f5b5b0b0fc5b2188450bcd655b70809d97b1c4535f2884635094377d, refusing
tamper_rc=1
```

Result: PASS, verifier refuses mismatched content.

## Gate 4 - runner result.json artifacts

```terminal
$ python3 tools/compat-runner/runner.py --plan tools/compat-runner/plan.yaml --limit 5
/Volumes/MacOS/MacRunner/reports/compat-runs/20260514-132420
engine_state: pass=0 fail=0 hang=1
result.json files: 5
results: ENGINE_REGRESSION
```

```terminal
$ python3 tools/compat-runner/runner.py --plan tools/compat-runner/plan.yaml --limit 5
/Volumes/MacOS/MacRunner/reports/compat-runs/20260514-132712
engine_state: pass=1 fail=0 hang=0
result.json files: 5
results: FAIL, PARTIAL
```

```terminal
$ python3 - <<PY # result.json validation
latest_run reports/compat-runs/20260514-132712
latest_result_json_count 5
latest_results ['FAIL', 'PARTIAL']
engine_regression_artifacts 5
PY
```

Result: PASS, runner emits valid result.json files and real ENGINE_REGRESSION artifacts when loop-wineboot hangs.

## Gate 5 - public report MacRunner only row

```terminal
$ python3 tools/compat-runner/report.py && grep -c "MacRunner only" reports/compat-public/index.md
/Volumes/MacOS/MacRunner/reports/compat-public
rows=50
1
```

Result: PASS, report has >= 1 MacRunner only row.

## Gate 6 - no GitHub API integration

```terminal
$ rg -n "gh api|api.github|pull request|GitHub Issues" tools/compat-runner || true
```

Result: PASS, winget-pkgs is used as cloned offline data only.

## Current limitations

- Direct upstream installer downloads can fail independently of MacRunner; downloader stops on network/SHA errors and writes no silent fallback.
- `MACRUNNER_COMPAT_USE_SYSTEM_PROXY=1` is opt-in; default downloader path bypasses broken macOS system proxy settings.
- Runner records `ENGINE_REGRESSION` when loop-wineboot is unhealthy; engine ownership remains with the parallel engine agent.
- Latest healthy engine probe reached installer execution but produced only FAIL/PARTIAL results for the first five programs.
- Report comparison is local/offline competitor metadata, not public API ingestion.
