# C1 causal-target readback control — 2026-07-26

Question: does the new, default-off `MACRUNNER_HB_CAUSAL_TARGET_READBACK=1`
path read the actual Metal color target for the exact causal draw, rather than a
stale or unrelated buffer?

## Instrument contract

`_MTLCommandEncoder_endEncoding` schedules the existing GPU readback primitive
only when the exact causal draw has passed signature matching. The schedule
happens after `endEncoding`, uses `state->render_target`, and prints
`surface_claim=causal-target-not-final-backbuffer`. It is therefore explicitly
not a final-presented-surface claim.

The source contract passed, and the built and deployed ARM64 host artifact both
had SHA-256 `86e17fdc0f1f8a90efd27a8632fadb1667895d5e326c63e2eb2470e5631c8df2`.
The x86_64 runtime prefix was synchronized before Wine; its `system32/d3d11.dll`
and `system32/winemetal.dll` SHA-256 values were respectively
`172ddbdba215a66d4974b07ccd9d6403400bf330d3cc68d112d1c33099113f41` and
`80a6dc36d65468ed047bac45bd27b775f896f868866793a1f484c3521b0b1440`.

The sealed actual child contained `C1`, `grid=1`, `grid_control=C1`, and
`target_readback=1`. Both launch logs existed and grew at the two-minute gate.

## Control result: PASS

At the exact logical C1 draw, the runtime logged:

```text
phase=c1-inject result=ok ... output=rt0 value=1,0,1,1
phase=encoder-end control=C1 result=ok ... rtv=0xc53b86580
phase=schedule control=C1 result=ok target=0xc53b86580 size=1024x768 target_draw=2 surface_claim=causal-target-not-final-backbuffer
phase=readback-complete checkpoint=causal-C1 tag=0x2 status=4 ... size=1024x768 ... pixels=786432 black=131072 nonblack=655360 colorful=655360 min=0,0,0,0 max=255,0,255,255 mean=212.500,0.000,212.500,212.500
```

The matching run-local `rtv` and `target` pointer establishes that the readback
sampled the target retained by the causal encoder. `655360 = 1024 × 640` is
the exact C1 viewport/scissor coverage; the `131072` black pixels are the known
top/bottom 64-row bands outside that draw. The independently injected C1
fragment and sidechannel both reported magenta for those 655360 pixels.

`UNSUPPORTED=0`, `HUP=0`, `fault=0`, and `reject=0` at the control result.

Verdict: the causal target-readback transport is honest for the written
viewport. It is now admissible to run C0 with the same path. This evidence does
**not** prove that this target is the surface ultimately presented by DXMT; that
identity remains unproven.
