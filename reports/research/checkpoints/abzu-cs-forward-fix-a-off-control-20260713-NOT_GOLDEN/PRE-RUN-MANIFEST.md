# ABZU Fix A OFF control — immutable pre-run manifest

- Classification: `NOT_GOLDEN`, one causal-control diagnostic; timeout `300s`, never `900s`.
- Attempt budget: exactly one; retry prohibited after `ATTEMPT-CONSUMED`.
- Treatment checkpoint: `abzu-cs-forward-fix-a-verify-20260713-NOT_GOLDEN`; run.log SHA-256 `1f140a79f41c59eaca3f71fb3300aac3cd83da3d2e34aef5c6e5a11bc5b90094`.
- Named independent variable: `MACRUNNER_HB_CS_FORWARD_NTDLL=1` (treatment) to `0` (this control). All requested feature flags and observers are otherwise identical; child environment is captured byte-exact for post-run diff.
- No source edit or rebuild between treatment and control. Main HEAD `0b09f80511f51e0eb8f5d58da642ae0e6a866745`; KEEP reference `e7cca2e3`, no reset/revert/stash/commit.
- Main source `unix/macrunner_hb.c` SHA-256 `4e16f99429b4d8548cd808a9bce93357fb8346f0281e6f12d0628926fab9a108`.
- Exact rebuilt Unix ntdll.so under both arms: SHA-256 `3d7d65c1e2e14ef8413bbe021361ea65bfdf0f4c94a3f4159d6918b888db2b57`.
- Execution root `/Users/timurtoby/Documents/MacRunner/Main/MacRunner-abzu`; HEAD `f58219bb31dd808c4da6784f8cb4cd1775a88edf`.
- Runtime overlay `artifacts/_abzu-guest-pc-movement-overlay-runtime-20260713/dist`; pre-control Unix ntdll.so SHA-256 `14d3563d105defde6efae10563e58b3ac7e88278b2533b96e882c8d6efacf2b5`, backed up and restored on exit.
- Runner/game SHA-256: `d2d23492abd564693cab90cb0c49204b6f357f1af3f4244bd6cda6200eec8ee2` / `b9bdbc743742f45845b152df7663b042e2b5da5d318894584d70f6d63d6839e7`.
- Source DXMT d3d11/dxgi/winemetal SHA-256: `842fa9e7228184ba46d1b81e2c75c0952fa813516b812f072c3d8cb9ff818ce8` / `30eb89bf1b37e2d650006105087c4c2e3e13cd1c9b067d47e793dcc6b93f8035` / `e49765a9e1a2f0f0522d24c07b0db48f769afa4e84908eca9f8b289289a55929`.
- Early overlay winemetal SHA-256 `7aa914d654101470b9fa5440dd3a82331087208f5094a5d7c9cb0cb875d9fc24`; this is the byte layout matching treatment memory (`+0x46b0` and `last_bytes`) before the later source-dist sync.
- Serialization fails closed before deployment/attempt consumption for Wine/wineserver/wineboot, ABZU/Hollow Knight, mr-run, make/ninja/cmake-build.
- Prefix is fresh/disposable; wineboot skipped; services/actxprxy enabled; translation cache preserved; no global kill.
- Passive observer policy is unchanged: optional four-second host samples at game ages 120s and 270s only.
- Decision rule requested by coordinator: same 55s `winemetal+0x46b0 -> 0x7ffd020044c -> c000007b` with zero CS-forward hits means Fix A is not the trigger; absence of that crash under OFF makes Fix A the trigger. Artifact/hash or serialization drift invalidates the comparison fail-closed.
