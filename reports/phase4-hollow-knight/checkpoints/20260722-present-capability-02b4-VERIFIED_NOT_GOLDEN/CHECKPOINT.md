# Hollow Knight Present Capability Checkpoint

Classification: `VERIFIED_CAPABILITY_NOT_GOLDEN`

This checkpoint preserves the exact runtime capability that reached a real
Metal-backed DXGI swapchain and submitted frames with the current native pair
and the `02b4` winemac build.

## Verified Capability

- Run contract: `READY`, blockers `0`.
- Effective child environment: `113` entries.
- `CreateSwapChainForHwnd`: success, `rc=0`.
- `GetBuffer`: `1`.
- `Present`: `448`.
- `Present1`: `448`.
- `UNSUPPORTED`, `MEMORY_FAULT`, reject, input, focus and activation counters:
  all `0`.
- Bounded runner termination: `rc=124`.
- First-Present and late captures: `BLACK`.

The successful submission counters prove that `winemac.so` `02b4` does not
categorically prevent render submission. This is a capability checkpoint, not
a strict one-file A/B exoneration: the effective `WINEDLLPATH` contained one
additional duplicate of the same DXMT root, and the outer wall-clock interval
was discontinuous. Both facts remain disclosed in the preserved report.

## Runtime Identity

- `aarch64-unix/ntdll.so`:
  `9a3f20b426adeeb86d70c06d0b7cfc9e687838e6eace5e43c282a19eb0363fc7`
- `aarch64-windows/ntdll.dll`:
  `3863660ac0b5f57c106d610d6a88d897a89a6c0ab626f890e66b4d572f02de4d`
- `x86_64-windows/ntdll.dll`:
  `b08516ba28c2d0d884b22673301bfc6eb3b5b48ee7458cd1248303289e431b78`
- `aarch64-unix/winemac.so`:
  `02b4d94ecb5827ea331decccb846f1bf6ff97df632ea28f6b40951794f8c9604`
- DXMT overlay inventory:
  `d7e36206e8bd82c21cbb9b1fe8f1c4ed6952d719c9d67336ea3f7590a890c88c`
- Translation-cache preimage inventory:
  `26aced9e8d430ee129839cae182bac3cbb0279aac70953025b08558de590869e`

## Scope

The payload contains an APFS copy-on-write clone of the complete staged dist,
the exact C0 DXMT overlay, the clean translation-cache preimage, captures,
runner scripts, run-contract evidence, reports and a sanitized child-environment
identity. The raw `final-child.json` is intentionally excluded because it
contains environment values.

This is not `GOLDEN`: no non-black product pixel was observed. The run also had
no `GameManager`, usable `UIManager`, `GameCameras` or scene-load milestone, so
the next product boundary remains Unity scene/bootstrap state.

Do not launch a game directly from this checkpoint. Clone its payload into a
new disposable arm and preserve this directory read-only.
