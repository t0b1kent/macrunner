# Q1 — What happens between oracle idx38 and idx40 (the PlayerPrefs "wait")

**Date:** 2026-07-27 · **Lane:** HK-MANAGED (offline, no run slot)
**Method:** IL disassembly of `Assembly-CSharp.dll` (game-hollow.knight-(89718), 1.5.12620)
with `tools/hk_managed_il/hk_il.py` (dnfile + dncil). Full dumps preserved in
`reports/phase4-hollow-knight/managed-decompile/`.

## Answer

Between `Unloading 5 Unused Serialized files` (idx38, a Unity **engine-internal** log — the
string does not exist in any managed assembly) and `Didn't need to wait for PlayerPrefs load.`
(idx40), the game executes the tail of **one coroutine**: `StartManager.Start()`, state machine
`StartManager.<Start>d__25.MoveNext` — **token `0x060041d2`, RVA `0x16c374`** — which must pass
four sequential gates. Every gate is a **per-frame poll** (`yield return null`), none is a
blocking wait. There is **no native wait call at the bottom of any of them**; the coroutine is
re-entered once per frame by Unity's coroutine scheduler on the main thread.

### The four gates, in code order (IL offsets from the dump)

**Gate 1 — language confirm poll** (only on the menu path). `MoveNext` IL_00da:
```
IL_00da: ldfld        field confirmedLanguage        // StartManager.confirmedLanguage, field token 0x04000DE2
IL_00e0: brfalse.s    195   // -> IL_00c3: yield return null (state 2), re-poll next frame
```
Reconstructed: `while (!confirmedLanguage) yield return null;`
`confirmedLanguage` is written in exactly one place in the entire assembly:
`StartManager.ConfirmLanguage` (token `0x06000e6c`), `stfld` at its IL_0060.

**Gate 2 — shared-data mount poll.** IL_0128:
```
IL_0128: callvirt  Platform::get_IsSharedDataMounted   // 0x06001006 / DesktopPlatform 0x06000f6d
IL_0132: brfalse.s 273  // -> yield return null (state 4)
```
On desktop this is **hardcoded true**: `Platform::get_IsSharedDataMounted` (RVA 0x54c8e) is
`ldc.i4.1; ret`; `DesktopOnlineSubsystem::get_IsSharedDataMounted` (0x06000fab) is also
`ldc.i4.1; ret`; `GOGGalaxyOnlineSubsystem` does **not** override it (method census, no
`IsSharedDataMounted` in its method list). Dead GOG subsystem cannot block this gate.

**Gate 3 — animator state poll.** IL_020e–0229:
```
IL_01fe: ldstr "LoadingIcon" -> Animator::StringToHash -> <loadingIconNameHash>
IL_020e: startManagerAnimator.GetCurrentAnimatorStateInfo(0)
IL_021e: AnimatorStateInfo::get_shortNameHash
IL_0229: beq -> proceed ; else yield return null (state 5)
```
Reconstructed: `while (animator.GetCurrentAnimatorStateInfo(0).shortNameHash != hash("LoadingIcon")) yield return null;`
Entered after `startManagerAnimator.SetTrigger("Start")` with `WillShowControllerNotice=false`,
`WillShowQuote=true` (IL_01cc–01f9). Bottom native call: `UnityEngine.Animator::GetCurrentAnimatorStateInfo`.

**Gate 4 — the PlayerPrefs "wait" itself.** IL_0293:
```
IL_0293: callvirt  Platform::get_IsPlayerPrefsLoaded    // 0x06001071
IL_029d: brfalse.s 606   // -> log "Waiting for PlayerPrefs load..." once, yield null (state 6)
IL_02a7: ldstr "Didn't need to wait for PlayerPrefs load."   // oracle idx40
```
**On desktop this gate is a formality.** `Platform::get_IsPlayerPrefsLoaded` (token
`0x06001071`, RVA `0x553ba`) is the complete body:
```
IL_0001: ldc.i4.1
IL_0002: ret
```
`DesktopPlatform` does **not** override it (only `XBoxConsolePlatform` does, token 0x06001142).
So on any desktop build the first evaluation succeeds and idx40 prints immediately.

After gate 4: `Platform.SetSceneLoadState(1, true)` (IL_02c7), then
`loadOperation.allowSceneActivation = true` (IL_02d9), then `yield return loadOperation`
(state 7) → `Menu_Title` activates → `GameManager.LevelActivated` (0x06000d53) logs
`Performing automatic level start.` (oracle idx44).

## Where our runs actually stop

Our runs never print `Waiting for PlayerPrefs load...` **nor** `Didn't need to wait...`
(verified by grep = 0/0 in run6/run8/run10/run12 `run.log`). Since gate 4 cannot block on
desktop, the runs **never reach gate 4**. run10's on-screen LANGUAGE SELECT menu
(`checkpoints/20260727-FIRST-VISIBLE-PIXEL-.../evidence/FIRST-VISIBLE-PIXEL-language-select.png`)
is only reachable through the menu branch of state 0, which yields into gate 1.

**Conclusion: the park is Gate 1 — `while (!StartManager.confirmedLanguage) yield return null;`
in `StartManager.<Start>d__25.MoveNext` (IL_00da loop, coroutine states 2/3).** It is a poll
predicate that never becomes true, not a blocking wait. The single producer of the predicate
is `StartManager.ConfirmLanguage` (token `0x06000e6c`) — that is the method the engine lane
should instrument (method enter/leave), plus a field-write watch on `confirmedLanguage`
(field token `0x04000DE2`).

## What the engine lane should NOT look for

- No `WaitHandle`/`Monitor`/native blocking call is involved at the wall. A thread-state or
  sync-wait instrument will show nothing; the main thread is simply re-running a coroutine
  whose `ldfld confirmedLanguage` always reads 0.
- The PlayerPrefs load itself is not async on desktop and is not the wall, despite the log
  line's name. (Why PlayerPrefs *reads* still matter: see Q3 report — the menu only appears
  because `PlayerPrefs.GetInt("GameLangSet", 0)` returned 0.)
