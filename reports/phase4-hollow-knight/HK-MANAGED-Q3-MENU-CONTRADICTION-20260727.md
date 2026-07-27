# Q3 — The menu contradiction, resolved: PlayerPrefs reads see nothing

**Date:** 2026-07-27 · **Lane:** HK-MANAGED (offline)

## The contradiction

run10 captured the LANGUAGE SELECT MENU on screen
(`checkpoints/20260727-FIRST-VISIBLE-PIXEL-.../evidence/FIRST-VISIBLE-PIXEL-language-select.png`)
while running on a prefix that provably carries a saved language
(`user.reg`: `GameLangSet_h1172976845=dword:00000001`, `M2H_lastLanguage_h3859156181`="EN",
see `reports/phase4-hollow-knight/pref-state-watch-run12/`). A prefix with a saved language
should not ask which language.

## Game-side conditions for the menu (complete, from IL)

The menu branch in `StartManager.<Start>d__25.MoveNext` (IL_0089–00ac) is entered iff:

```
!CheckIsLanguageSet()  &&  Platform.Current.ShowLanguageSelect
```

- `DesktopPlatform::get_ShowLanguageSelect` (0x06000f82) = `ldc.i4.1; ret` — always true on
  desktop, not a variable.
- `CheckIsLanguageSet()` (0x06000e69) on desktop reduces to
  **`UnityEngine.PlayerPrefs.GetInt("GameLangSet", 0) > 0`**
  (via `PlayerPrefsSharedData.GetBool` → `PlayerPrefs.GetInt`, unencrypted).

`ShowLanguageSelect` (0x06000e6e) is the only code that displays that UI (its coroutine fades
in the `languageSelect` CanvasGroup and calls `PreselectOption.HighlightDefault`). There is no
other path to that screen in the assembly.

**Therefore run10's menu is hard evidence that, at `StartManager.Start` time in that run,
`PlayerPrefs.GetInt("GameLangSet", 0)` returned 0 — despite the value being present in the
prefix registry.**

## Independent second proof — the log lines say the same thing

`Language.RestoreLanguageSelection` prints `Loaded saved language code '{0}'` **only** when
`TryGetSavedLanguageCode` returns true, which requires
`PlayerPrefs.HasKey("M2H_lastLanguage") == true`.

| evidence | line printed | meaning |
|---|---|---|
| Windows oracle (`HK-ORACLE-Player.log:34,48`) | `Loaded saved language code 'EN'` ×2 | HasKey true — PlayerPrefs works |
| run6 / run8 / run10 / run12 `run.log` | `Loaded system language 'English'` + `Loaded system language code 'EN'`; `Loaded saved language` count = **0** | HasKey **false** — saved key invisible |

Same registry content, opposite read results, on the same game build. This is not
branch-selection noise; it is two independent managed reads (`HasKey` and `GetInt`) both
failing to see values that are byte-present in `user.reg`.

## Conclusion

The contradiction is resolved one layer below the game: **under MacRunner, Unity's native
PlayerPrefs implementation does not return the template prefix's registry values to the
game.** The menu in run10 is exactly what the game should show when its PlayerPrefs store
looks empty — the game's logic is correct; its data source is blind.

This also corrects the frontier report's `[HYPOTHESIS]` ("saved language in the template is
what blocks the boot"). The mechanism is the opposite direction: the saved language is present
but **unreadable**, so the menu appears on template runs just as it does on fresh-prefix runs.
The 4/4 prefix correlation holds for a different reason than supposed: prefixes without
`user.reg` ran with a working ConfirmLanguage actuator (the accepted 2026-07-22 one-shot),
which is what actually carried those runs past the menu. (Actuator provenance per
`HK-SCENE-ACTIVATION-STALL-ANALYSIS.md`: recent runs deployed an observer PE whose one-shot
selector was inert → menu shown, nobody confirms → park.)

### What this makes actionable (engine lane)

Instrument the native bottom of `UnityEngine.PlayerPrefs.HasKey` / `GetInt` on Windows
desktop Unity — the advapi32 registry query for value names `GameLangSet_h1172976845` /
`M2H_lastLanguage_h3859156181` under `HKCU\Software\Team Cherry\Hollow Knight`
(`RegOpenKeyExW`/`RegQueryValueExW` or the `NtQueryKey` path). `[HYPOTHESIS]` candidates, in
order: (a) key enumeration/open fails under our registry emulation; (b) the value-name hash
suffix Unity computes differs under our stack so lookups miss (the `_h1172976845` suffix is
Unity's own hash of the key name); (c) HKCU maps to a different user.reg than the one the
template seeds. Distinguishing these needs one registry-API trace, which is engine-lane
territory.

Note for run16 planning: deleting `GameLangSet`/`M2H_lastLanguage` from the template would
change nothing observable — the game already behaves as if they are absent.
