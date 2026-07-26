# HK View-Frustum Anomaly Analysis

Date: 2026-07-26

Status: **offline log/source analysis only.** No build, run, code change, or
commit was performed.

## Executive verdict

`Screen position out of view frustum` is a meaningful Unity-side camera/UI
state signal, but it is **not yet proof that all submitted D3D geometry is
outside the GPU clip volume**. The current message contains a fixed screen
coordinate at a viewport boundary, not a world position or a matrix value. It
is fully compatible with a repeated screen-to-camera/raycast request using a
clamped/default pointer coordinate.

A degenerate view/projection matrix remains a valid Wall B hypothesis. It must
be confirmed from the current run's bound VS constant-buffer bytes and/or
post-VS clip coordinates, not from this warning alone.

## 1. Meaning and limits of the Unity warning

### What the message establishes

**[SUPPORTED, API-family level]** Unity emitted the diagnostic while converting
or validating a *screen-space position* against a camera view/frustum. Such a
diagnostic is expected from a camera operation that consumes a screen point,
such as a screen-to-world/ray conversion or an event/UI raycast path, when the
point is outside the camera's effective pixel rectangle/frustum.

**[NOT PROVEN]** The exact managed API is not identified by the present log.
The local evidence has no UnityPlayer string cross-reference or managed stack
at the emission site. It is therefore not justified to name
`Camera.ScreenPointToRay`, `ScreenToWorldPoint`, or a specific UI event as fact.

### What the message does not establish

- It does **not** include a world-space object position, clip-space coordinate,
  or a matrix value.
- It is not direct evidence that `WorldToScreenPoint` generated the value.
  That API takes a world position and returns screen coordinates (including a
  depth component); a diagnostic printing an input `screen pos` and `Camera
  rect` more naturally belongs to the opposite screen-to-camera direction.
- An object merely being behind the near/far plane is not established by this
  message. A caller may separately use a world-to-screen result, but that
  causal chain is absent here.
- A zero, identity, NaN, or otherwise bad view/projection matrix is one
  possible cause of an invalid camera conversion, not a conclusion from the
  text. NaN would normally be visible in the formatted numeric output; the
  observed values are finite integers.

## 2. Current live evidence and the suspicious coordinate

Case-sensitive search of
`laneA-frame-dump-20260726-032428/launch.stdout` found **four** occurrences.
The first context is:

```text
Couldn't find a UIManager, make sure one exists in the scene.
Screen position out of view frustum (screen pos 0.000000, 767.000000)
  (Camera rect 0 0 1024 768)
dxmt-hk-swaptrace: ... OMSetRenderTargets ...
dxmt-hk-drawtrace: ... DrawIndexed ...
```

This strengthens the Unity lifecycle/UI correlation already seen in earlier
black-frame work. It does not show which Draw, camera, or managed caller owns
the warning.

For Unity's usual bottom-left screen convention, `(0,767)` is the top-left
pixel of a `1024x768` surface; under a top-left convention it is the
bottom-left pixel. In both conventions it is a **corner**, not an arbitrary
offscreen coordinate. With the customary half-open rect
`[0,1024) x [0,768)`, it is inside. A strict internal comparison that excludes
the minimum x boundary, a one-pixel coordinate conversion, or a clamp from an
out-of-range input can nevertheless reject it.

The exact repetition is important:

- **[SUPPORTED]** `(0,767)` is finite and deterministic, not a printed NaN or
  a varying projected world coordinate.
- **[HYPOTHESIS, stronger than bad-matrix inference]** it is a fixed cursor/UI
  edge coordinate, possibly used repeatedly by a missing/incorrect UI camera.
- **[HYPOTHESIS]** an invalid camera transform could also collapse or clamp
  multiple requests to the same corner, but the log alone cannot distinguish
  this from ordinary edge handling.

## 3. Historical occurrence

The requested case-sensitive `rg -F` search over each `launch.stdout` gives:

| Run | Exact warning count | Black-frame relation |
|---|---:|---|
| `laneA-ring-ledger-v4-20260725-205648` | 16,114 | Same investigated black-render family; count alone has no per-frame pixel attribution. |
| `laneA-ring-ledger-20260725-120428` | 5,768 | Same family; terminal Gfx storm is an independent confounder. |
| `laneA-ring-ledger-v3-20260725-150601` | 6,293 | Same family; transient-recovery behaviour prevents a count-to-black causal claim. |
| `laneA-ring-ledger-v2-20260725-133318` | 21,056 | Same family; old bounded graphics telemetry cannot establish visible coverage. |
| `laneA-frame-dump-20260726-032428` (live snapshot) | 4 | New full-render/frame-dump run; warnings occur immediately after missing-UIManager lines. |

There is stronger qualitative co-occurrence in independent retained evidence:

- `DRAW-EXECUTED-BLACK-CAUSE.md` records 1,391 such warnings alongside 1,390
  Present frames and black pixel-watcher results.
- `BACKBUFFER-READBACK-RESULT.md` records 1,758 messages, all with `(0,767)`
  and a `1024x768` rect, after missing GameCameras/UIManager.
- `PSO-PERSIST-FIX-VERIFY.md` records 1,563 messages during a live Unity frame
  phase.

**[CONCLUSION]** The warning reliably co-occurs with the known incomplete
Unity/UI scene state and black-frame research runs. Its *rate* does not track
blackness or render severity: the current live snapshot is black-frame
investigation with only four early messages, while historical counts vary from
5,768 to 21,056. It is a useful state marker, not a causal metric.

## 4. Degenerate-matrix hypothesis and Wall B

The following chain is internally consistent but unproven:

```text
bad view/projection or camera state
  -> primitives transform outside clip space or have invalid W
  -> DrawIndexed/encoding/Present still succeed
  -> final target retains its black clear
  -> visible window is black
```

This is compatible with high Draw/Clear/Present totals, zero faults, and a
black frame. It is also compatible with the camera/UI lifecycle warnings.

It is **not uniquely implied** by them. The same observations also fit:

- black vertex/material/texture input;
- depth/stencil, blend, viewport, or scissor rejecting all visible fragments;
- draws into an offscreen target with no working final composition;
- a screen-point UI raycast that is unrelated to the render camera.

The old C2/C3 magenta result remains a narrow historical control: a selected
old pipeline could reach a pixel when forced. It neither proves nor disproves
the current camera matrices of all current draws.

## 5. Minimal next-run confirmation: VS constant-buffer provenance

### Capture points

Instrument the D3D11 context only as bounded diagnostics:

| Point | Candidate | Record |
|---|---|---|
| VS binding | `engine/dxmt/src/d3d11/d3d11_context_impl.cpp:2361` `VSSetConstantBuffers`, plus its `VSSetConstantBuffers1` implementation | stage, start slot, count, stable buffer ID, first constant, and constant count. |
| PS binding control | `d3d11_context_impl.cpp:2433` `PSSetConstantBuffers`, plus `PSSetConstantBuffers1` | Same fields; distinguishes a fragment-only parameter issue from VS transforms. |
| CB writes | `d3d11_context_impl.cpp:1434` `UpdateSubresource`/`UpdateSubresource1`, and all Map/Unmap write paths | buffer ID, offset/length, content hash, and a bounded raw snapshot for buffers later bound to VS. |
| Draw snapshot | `d3d11_context_impl.cpp:1596` `DrawIndexed`, after active state has been resolved and before draw encoding | VS identity, active VS slots/ranges, last-update sequence/hash, active RTV, viewport, scissor, and depth/blend state. |

The binding record alone is insufficient: a dynamic buffer can be rewritten
after it is bound. The draw snapshot must identify the latest contents and the
specified `FirstConstant`/`NumConstants` range.

### Matrix analysis rule

For the first bounded Draws that use the relevant Unity VS:

1. Preserve the exact bytes of all active VS constant-buffer ranges, rather
   than assuming projection is in slot `b0`.
2. Decode candidate 16-float blocks in both row-major and column-major form.
   Report all-zero rows/columns, non-finite values, near-zero W-producing rows,
   and singular/near-singular transforms. An identity matrix is a flag for
   investigation, **not** automatically invalid: UI and orthographic paths can
   validly use simple transforms.
3. Link each candidate to shader reflection/known constant-buffer layout before
   calling it view or projection.
4. Strongest discriminator: for the same Draw, log a bounded clip-space
   position/W range after the VS. If all vertices have invalid W or lie outside
   clip bounds, the matrix/transform hypothesis becomes direct evidence. If
   clip positions cover the viewport, reject it and move to material/target
   analysis.

### Frame-dump decision integration

- A coloured pre-Present dump rejects universal frustum rejection for that
  frame and moves the boundary to presenter/capture.
- A uniform black dump plus out-of-bounds clip positions supports this
  hypothesis directly.
- A uniform black dump plus valid clip coverage requires the target graph,
  texture/material, and depth/blend branches from
  `HK-WALL-B-WITH-WORKING-RENDER.md`.

No matrix or camera fix is justified before this draw-to-buffer-to-clip
evidence exists.
