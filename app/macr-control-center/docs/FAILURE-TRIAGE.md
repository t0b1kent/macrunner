# Failure Triage

The Failure Triage assistant analyzes launcher results and classifies failures automatically.

## Classification

| Class | Triggers |
|-------|----------|
| `timeout` | `timedOut == true` or `status == "TIMEOUT"` |
| `crash` | `crashed == true` or `rc == -11` or `stderrTail` contains `c0000005` |
| `missingDll` | `status == "MISSING_DLL"` or `stderrTail` contains `module not found` |
| `invalidExe` | `status == "INVALID_EXE"` or `error` is non-nil |
| `d3dValidationError` | `d3dEnabled == true` and `d3dStatus == "FAIL"` |
| `cleanupFailed` | `cleanupOk == false` |
| `unknown` | No other rule matches |

## Diagnosis Output

Each classification produces:
- **Cause**: one-line human-readable summary
- **Evidence**: specific fields that triggered the match
- **Recommended command**: CLI command to reproduce or investigate
- **Files to include**: artifact paths to attach to bug reports

## UI

The **Run** tab shows a triage badge after each run. Click it to open the full diagnosis sheet.
