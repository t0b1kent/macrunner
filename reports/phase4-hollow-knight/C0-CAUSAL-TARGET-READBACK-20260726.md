# C0 causal-target readback — 2026-07-26

Question: after the target-readback path passed its C1 magenta control, is the
actual Metal color target for the exact C0 draw black or non-black?

## Admitted run

The C0 run used the same host artifact and x86_64 Windows DLL SHA values as the
passed C1 control. Its sealed child had C0/grid/C0/target-readback all enabled.
The C1 and C0 preflight environments have the same 122 names and timeout; their
only semantic changes are C1→C0 in the causal and grid-control tokens. Run
directory, disposable prefix, and start epoch necessarily differ.

Both logs existed and grew at the two-minute gate. The run reached `Present=4`,
`DrawIndexed=3`, and `OMSetRenderTargets=5` with `UNSUPPORTED=0`, `HUP=0`,
`fault=0`, and `reject=0`.

## Result: exact causal target is black

```text
phase=draw-select control=C0 result=ok ... viewport=0,64,1024,640 scissor=0,64,1024,640
phase=schedule control=C0 result=ok target=0xbacb72580 size=1024x768 target_draw=2 surface_claim=causal-target-not-final-backbuffer
phase=sidechannel-complete control=C0 result=ok class=BLACK written=655360 black=655360 magenta=0 other=0
phase=readback-complete checkpoint=causal-C0 tag=0x2 status=4 texture=0xbacb72580 format=70 size=1024x768 ... pixels=786432 black=786432 nonblack=0 colorful=0 min=0,0,0,0 max=0,0,0,5 mean=0.000,0.000,0.000,4.167
```

The target pointer is the same in the C0 schedule and completion record. The
same instrument read the C1 target as 655360 magenta pixels, so a stale/read-wrong
buffer explanation is excluded for this path.

Verdict: the exact C0 draw’s active 1024×768 Metal attachment is black at
encoder completion. This is a proven intermediate product boundary, not a
final-backbuffer claim: the target may still be offscreen or may later be
composited/overwritten. The next question is the presented-surface target graph.
