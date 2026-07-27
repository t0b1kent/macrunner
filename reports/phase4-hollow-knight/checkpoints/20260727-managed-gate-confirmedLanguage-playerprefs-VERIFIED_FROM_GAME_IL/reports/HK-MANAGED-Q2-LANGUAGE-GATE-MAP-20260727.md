# Q2 — What actually gates the language flow

**Date:** 2026-07-27 · **Lane:** HK-MANAGED (offline)
**Method:** IL disassembly (`tools/hk_managed_il/hk_il.py`), caller census over all 18,239
MethodDefs of `Assembly-CSharp.dll` + all of `TeamCherry.Localization.dll`.

## The two registry values are independent and have different roles

### `GameLangSet` — the GATE (PlayerPrefs bool)

- **Written by exactly one method:** `StartManager.ConfirmLanguage` (token `0x06000e6c`):
  ```
  IL_000c: Platform.Current.LocalSharedData.SetInt("GameLangSet", 1)   // ISharedData::SetInt
  IL_0021: Platform.Current.LocalSharedData.Save()                     // ISharedData::Save
  ...
  IL_005e: ldc.i4.1 ; stfld confirmedLanguage                          // field 0x04000DE2
  ```
- **Read by exactly one method:** `StartManager.CheckIsLanguageSet` (token `0x06000e69`):
  ```
  IL_0001: Platform.Current.IsPlayerPrefsLoaded   // desktop: hardcoded true (see Q1)
  IL_000b: brfalse -> return false
  IL_000d: Platform.Current.LocalSharedData.GetBool("GameLangSet", false)  // -> PlayerPrefs.GetInt("GameLangSet",0) > 0
  ```
  (`PlayerPrefsSharedData.GetBool/GetInt`, tokens `0x0600108c/0x0600108e`, call
  `UnityEngine.PlayerPrefs.GetInt` directly — unencrypted path, `IsEncrypted=false` set in
  `DesktopPlatform::Awake` IL_0024.)
- **Deleted by** `GameManager.SetupStatusModifiers` (0x06000dd0) only when dev-config flag
  `gameConfig.clearPreferredLanguageSetting` is set — called from `GameManager.Start`, i.e.
  downstream of the wall; not a factor at boot.

### `M2H_lastLanguage` — the language CHOICE (PlayerPrefs string)

- Written by `LocalizationProjectSettings.OnSwitchedLanguage` (token `0x0600014d`):
  `LocalSharedData.SetString("M2H_lastLanguage", code.ToString()); Save();`
- Read by `LocalizationProjectSettings.TryGetSavedLanguageCode` (token `0x0600014b`):
  ```
  if (Platform.Current && LocalSharedData.HasKey("M2H_lastLanguage")) {
      out = LocalSharedData.GetString("M2H_lastLanguage", ""); return true;
  }
  out = "EN"; return false;
  ```

## The saved/system branch is NOT the switch — code-level proof

`TeamCherry.Localization.Language.RestoreLanguageSelection` (TCL token `0x06000009`):
```
if (TryGetSavedLanguageCode(out code)) {
    Log("Loaded saved language code '{0}'", code);
    if (available.Contains(code)) return code;
    LogError("... is not an available language");
}
if (Settings.useSystemLanguagePerDefault) {
    sys = GetSystemLanguage();
    Log("Loaded system language '{0}'", sys);          // <-- our runs print this
    code2 = LanguageNameToCode(sys).ToString();
    Log("Loaded system language code '{0}'", code2);
    if (available.Contains(code2)) return code2;
    LogError("System language code '{0}' is not an available language");
}
Log("Falling back to default language code '{0}'", defaultLangCode);
return defaultLangCode;
```
Call graph (verified by caller census): `RestoreLanguageSelection` ← only `Language.LoadLanguage`
← `Language..cctor` (static ctor) and 4 later AC call sites. It **selects which language asset
to load; it touches no gate.** Its return value is not consulted by `CheckIsLanguageSet` or the
Start coroutine. That is why run15 (system branch) stalls at the identical wall as
saved-language runs: the branch is cosmetic to boot flow. **Confirmed in code, not just by
run correlation.**

## `ConfirmLanguage` / `SetLanguage` / `allowSceneActivation` — precondition order

From `<Start>d__25.MoveNext` (0x060041d2) and the two sub-coroutines:

1. `LoadSceneAsync("Menu_Title")` → `allowSceneActivation = false` (IL_0077). **First.**
2. `if (!CheckIsLanguageSet() && Platform.Current.ShowLanguageSelect)` — desktop
   `ShowLanguageSelect` is hardcoded `true` (DesktopPlatform 0x06000f82: `ldc.i4.1; ret`) —
   → run `ShowLanguageSelect()` coroutine (fades menu in; calls
   `PreselectOption.HighlightDefault(false)`; completes) → then **gate:** poll
   `confirmedLanguage`.
3. `SetLanguage(string)` (0x06000e66) is the menu's option-select handler: switches display
   language, fades in the `languageConfirm` button object. **It does NOT set
   `confirmedLanguage` and does NOT write `GameLangSet`.** Optional for boot progress.
4. `ConfirmLanguage()` (0x06000e6c) is the menu's confirm-button handler: writes
   `GameLangSet=1`, `Save()`, sets `confirmedLanguage=true`. **This is the only release of
   the gate.**
5. `LanguageSettingDone()` coroutine (fade menu out, `ConfigManager.SaveConfig()`).
6. Poll `IsSharedDataMounted` (desktop: trivially true).
7. Saved-vs-current language reconciliation (`TryGetSavedLanguageCode` again; reload if
   different), then animator `SetTrigger("Start")`, poll for animator state `LoadingIcon`.
8. PlayerPrefs check (desktop: trivially true) → `SetSceneLoadState(1,true)` →
   **`allowSceneActivation = true` (IL_02d9)** → `yield return loadOperation` → Menu_Title
   activates → `GameManager.LevelActivated` logs `Performing automatic level start.`

## Caller census results (hard gates for "how can these fire")

Scanning every method body in `Assembly-CSharp.dll` for direct calls:

| method | direct IL callers |
|---|---|
| `StartManager.ConfirmLanguage` (0x06000e6c) | **none** |
| `StartManager.SetLanguage` (0x06000e66) | **none** |
| `StartManager.CancelLanguage` (0x06000e6d) | **none** |
| `StartManager.CheckIsLanguageSet` | `<Start>d__25.MoveNext` only |
| `StartManager.ShowLanguageSelect` | `<Start>d__25.MoveNext` only |

`ConfirmLanguage` is `public void ConfirmLanguage()` — public, instance, non-virtual
(MethodDef flags: mdPublic+mdHideBySig). Zero IL callers + public = **it is wired as a
UnityEvent/onClick handler on the language menu's confirm button** and is only reachable
through Unity's UI event system (a click/accept on the `languageConfirm` object) or via
`mono_runtime_invoke` (the actuator path, proven working 2026-07-22).
