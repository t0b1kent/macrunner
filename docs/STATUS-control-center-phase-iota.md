# STATUS control-center phase iota

Status: PASS
Area: Release packaging plan

## Implemented

- Added ControlCenterPackagingPlanner with app-local package script invocation.
- Explicitly excludes engine, wine-fork, and protected root scripts.
- Added config/control-center.json documenting process-only integration and external disk roots.

## Remaining Limitations

- Signing, notarization, and DMG release automation remain outside this patch.
