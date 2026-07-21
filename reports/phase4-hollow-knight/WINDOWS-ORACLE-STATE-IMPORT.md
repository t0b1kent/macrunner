# Hollow Knight Windows Oracle State Import

Classification: `VERIFIED_WINDOWS_ORACLE_NOT_GOLDEN`

No Hollow Knight or MacRunner game runtime was executed while preparing this
state.

## Source

- Windows export: `hk-state-20260721-225722`
- Host: Windows 11 ARM64 (`10.0.26200.0`)
- Export manifest SHA-256:
  `1cf7c39f26f9ab294a0ab4385cf7565f625d665fbd153a888481040f6bf94607`
- Payload manifest SHA-256:
  `c7575e5d1fe32156a0ad289ccab01825a4679da8c39745f7d33e1965226f1b9a`
- Payload: 6 files, 16,607 bytes; independent verification passed 6/6.
- A second export (`hk-state-20260721-230038`) had a different timestamped
  export manifest but a byte-identical six-file payload.

Raw state is retained locally at:

`reports/phase4-hollow-knight/windows-oracle/hk-state-20260721-225722-VERIFIED_WINDOWS_ORACLE`

## Windows Ground Truth

The native Windows `Player.log` proves:

- Direct3D 11 initialized on the Parallels display adapter.
- Saved language `EN` was loaded and restored.
- Automatic level start was reached.
- `Opening_Sequence` loaded and progressed.
- The run grew from 4,274 to 47,392 and then 67,498 loaded Unity objects.

The exported PlayerPrefs key contains 40 values, including:

- `GameLangSet_h1172976845 = 1`
- `M2H_lastLanguage_h3859156181 = EN`
- `lastProfileIndex_h2074025210 = 1`

There is no `user1.dat`; this oracle captures language/bootstrap state before a
bench save, not a durable gameplay slot.

## Prepared Mac Prefix

Prepared prefix template:

`artifacts/hk-windows-oracle-prefix-template-20260721-225722-v3`

- Base: the previously verified GameLangSet prefix template.
- Registry import: offline, 40/40 exact value lines present.
- LocalLow import: 5/5 files byte-identical to the Windows export.
- Wine `user.reg` SHA-256:
  `2bc52f0afbd41ed48d2c42551fd2e463f024152872e5a164a5b4d36b4540c9a3`
- Import classification:
  `WINDOWS_ORACLE_PREFIX_TEMPLATE_NOT_GOLDEN`

The failed `regedit`-based preparation roots were removed. Only the verified
1.9 MiB offline-import template remains.

## Next Runtime

Run exactly one probe-off Hollow Knight process using the verified
Present-capability runtime snapshot and this prefix template. The product gates
are scene/bootstrap progression, `GetBuffer`, `Present`, and a non-black window
capture. No profiler, synthetic input, focus manipulation, source edit, rebuild,
or retry is part of that run.
