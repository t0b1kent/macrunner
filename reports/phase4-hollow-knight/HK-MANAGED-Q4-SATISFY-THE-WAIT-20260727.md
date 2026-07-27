# Q4 — What would make Hollow Knight proceed: exact signatures

**Date:** 2026-07-27 · **Lane:** HK-MANAGED (offline)

## The wait and its release

The park is `while (!confirmedLanguage) yield return null;` in
`StartManager.<Start>d__25.MoveNext` (token `0x060041d2`, poll at IL_00da, field token
`0x04000DE2`). The game sets that field in exactly one place — the language menu's confirm
button handler:

```
public void StartManager::ConfirmLanguage()     // token 0x06000e6c, RVA 0x50d70
                                                 // public, instance, non-virtual, void (void)
```

Effects (IL-verified): `LocalSharedData.SetInt("GameLangSet", 1)` → `Save()` → refresh confirm
text → `CancelFade()` → `StartCoroutine(FadeOut(languageConfirm, 0.25))` →
`confirmedLanguage = true`.

## Option A — managed call (immediately actionable)

With the existing Mono profiler actuator (`tools/hk_language_observer/`), invoke:

```
mono_runtime_invoke(method = StartManager::ConfirmLanguage (token 0x06000e6c),
                    obj    = <live StartManager instance in the boot scene>,
                    params = NULL, exc = NULL)
```

- **Correct fire point:** on or after `PreselectOption.HighlightDefault` (the menu is up and
  the coroutine is inside the poll) — exactly the trigger the accepted 2026-07-22 one-shot
  used. `SetLanguage("EN")` before it is **optional**: `ConfirmLanguage` alone releases the
  gate (the display language was already loaded by `RestoreLanguageSelection`; `SetLanguage`
  (0x06000e66) only re-switches display language and reveals the confirm button).
- **Do not** substitute a direct field write of `confirmedLanguage` except as diagnostics:
  it skips the `GameLangSet=1` + `Save()` side effects, so the next boot would show the menu
  again (and, unlike the invoke, it proves nothing about the real path).

## Option B — input event (what a user would do)

The confirm is wired through Unity's event system (zero IL callers of `ConfirmLanguage` in
`Assembly-CSharp.dll` — census over 18,239 methods): an **accept/click on the
`languageConfirm` UI object** of the Start scene's language-select menu (after
`HighlightDefault`). Given the standing finding that synthetic input (CGEvent / session tap /
win32 PostMessage/SendInput) does not reach Unity under our stack, Option A is the reliable
path; Option B requires real user input or in-process NSEvent injection.

## What happens after the release (so the next wall is recognizable)

Once `confirmedLanguage=true`, remaining gates self-clear on desktop:
1. `LanguageSettingDone` coroutine (menu fade-out + `ConfigManager.SaveConfig()`);
2. `IsSharedDataMounted` poll — hardcoded `true` on desktop (Platform/DesktopOnlineSubsystem);
3. animator poll — `startManagerAnimator` must reach state `LoadingIcon` after
   `SetTrigger("Start")`; bottom native call `Animator::GetCurrentAnimatorStateInfo`.
   `[HYPOTHESIS]` clears on its own (normal main-thread animator work), but if a run with a
   confirmed language still stalls, **this is the next suspect** — marker: neither
   `Didn't need to wait for PlayerPrefs load.` nor `Waiting for PlayerPrefs load...` prints;
4. PlayerPrefs gate — hardcoded `true` on desktop → prints
   `Didn't need to wait for PlayerPrefs load.` (oracle idx40);
5. game itself sets `loadOperation.allowSceneActivation = true` (IL_02d9) →
   `Performing automatic level start.` → `Game controller set to None.`

Success markers in log order: `Didn't need to wait for PlayerPrefs load.` →
`Performing automatic level start.` → `Game controller set to None.`

## Caveat for run design

Per Q3: the menu appears because PlayerPrefs reads miss, so auto-confirm via Option A is the
**only** way any run currently gets past the menu — with or without the saved-language
template. A run that fixes the PlayerPrefs read (registry trace first) would instead take the
no-menu path: `CheckIsLanguageSet()==true` → skip straight to gate 2 with no confirm needed.
