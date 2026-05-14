# User Guide

## Adding an App
1. Open **Apps** tab
2. Click **Add App**
3. Enter path to Windows `.exe`
4. Set D3D backend if needed (None / Mock / Metal)
5. Save

## Running an App
1. Select app in **Apps** tab
2. Click **Run**
3. Watch live stdout/stderr and status badge (PASS / FAIL / CRASH / TIMEOUT)

## Viewing D3D Artifacts
1. Open **D3D** tab
2. Load latest launcher result or enter JSON path
3. Preview PPM image, trace, IR, and report

## Running Doctor
1. Open **Doctor** tab
2. Click **Run Doctor** or **Quick Verify**
3. Review summary cards for lanes, Metal, D3D bridge

## Exporting Debug Bundle
1. After a run, click **Export Debug Bundle**
2. Bundle is saved to `~/Desktop/MacRunner-Debug-Bundle-YYYYMMDD-HHMMSS/`
3. Contains launcher JSON, logs, doctor report, git diff stat
