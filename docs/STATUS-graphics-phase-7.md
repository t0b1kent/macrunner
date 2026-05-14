# STATUS graphics phase 7 - HUD wired

Date: 2026-05-14T05:48:49Z
Scope: DXVK/HUD text stream parser to MacRunner JSONL events.

## Gate

Command:

```text
python3 engine/graphics/dist/hud/graphics-hud-proxy.py --synthetic 24 --output engine/graphics/dist/hud/out/hud-events.jsonl
python3 - <<'PY'
import json
from pathlib import Path
p=Path('engine/graphics/dist/hud/out/hud-events.jsonl')
rows=[json.loads(x) for x in p.read_text().splitlines()]
print(f'events={len(rows)}')
print(f'first={rows[0]["backend"]} frame={rows[0]["frame"]} fps={rows[0]["fps"]}')
print(f'last={rows[-1]["backend"]} frame={rows[-1]["frame"]} fps={rows[-1]["fps"]}')
PY
```

Output:

```text
hud events: 24
events=24
first=dxvk-hud frame=0 fps=59.5
last=dxvk-hud frame=23 fps=59.9
```

## Result

PASS. Parser emits valid JSONL and validates >=20 HUD events. Live DXVK HUD capture is deferred until Wine fixture execution is unblocked by Phase G engine runtime work.
