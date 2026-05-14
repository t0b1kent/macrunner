# Compatibility Database

The Control Center maintains a local compatibility database to track app health over time.

## Storage

- Entries: `~/Library/Application Support/MacRunnerControlCenter/compatibility_entries.json`
- History: `~/Library/Application Support/MacRunnerControlCenter/compatibility_history.json`

## Entry Schema

| Field | Description |
|-------|-------------|
| `id` | App UUID |
| `name` | Display name |
| `exePathHash` | SHA-256 of exe path (for dedup) |
| `arch` | Detected architecture |
| `lastStatus` | Latest run status |
| `bestD3DBackend` | Best-performing D3D backend |
| `lastSuccessfulVersion` | MacRunner version of last pass |
| `failuresCount` | Consecutive failures |
| `notes` | Free-form notes |
| `tags` | User-defined tags |
| `category` | Game, tool, benchmark, etc. |

## History Schema

Each run produces a history record:

| Field | Description |
|-------|-------------|
| `appId` | App UUID |
| `status` | PASS / FAIL / CRASH / TIMEOUT |
| `durationMs` | Run duration |
| `d3dBackend` | Backend used |
| `timestamp` | Run date |
| `launcherJsonPath` | Path to launcher result JSON |

## UI

Open the **Compat** tab to browse entries, filter by status, and view per-app history.
