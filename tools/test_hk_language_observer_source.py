#!/usr/bin/env python3
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "tools/hk_language_observer/hk_language_observer.c").read_text()
BUILD_SOURCE = (ROOT / "tools/hk_language_observer/build.sh").read_text()
ZERO_ENTRY_SOURCE = (ROOT / "tools/hk_language_observer/zero_pe_entry.py").read_text()
LOADER_SOURCE = (ROOT / "engine/wine/dlls/ntdll/loader.c").read_text()
UNIXLIB_SOURCE = (ROOT / "engine/wine/dlls/ntdll/unixlib.h").read_text()
HB_SOURCE = (ROOT / "engine/wine/dlls/ntdll/unix/macrunner_hb.c").read_text()
CONTRACT_PATH = (
    ROOT
    / "reports/phase4-hollow-knight/first-run-language-static-20260718/STATIC-UI-CONTRACT.json"
)
CONTRACT = json.loads(CONTRACT_PATH.read_text())


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require(
    'env_enabled("MACRUNNER_HB_LANGUAGE_FLOW_OBSERVER")' in SOURCE,
    "observer must remain default-off",
)
require(
    "MACRUNNER_HB_LANGUAGE_FLOW_OBSERVER_MAX" in SOURCE
    and "parsed > 1024" in SOURCE
    and "event=budget-exhausted" in SOURCE,
    "observer must have a hard record budget",
)
for symbol in (
    "StartManager",
    "SetLanguage",
    "ConfirmLanguage",
    "confirmedLanguage",
    "PreselectOption",
    "HighlightDefault",
    "AsyncOperation",
    "set_allowSceneActivation",
    "GameManager",
    "GameCameras",
    "UIManager",
):
    require(symbol in SOURCE, f"missing observer target: {symbol}")
require("phase=enter" in SOURCE and "phase=return" in SOURCE, "entry/return not paired")
require(
    "phase=return before=" in SOURCE and 'builder_append(&builder, " after=")' in SOURCE,
    "allowSceneActivation transition not recorded",
)
require(
    "phase=enter call=" in SOURCE and 'builder_append(&builder, " confirmedLanguage=")' in SOURCE,
    "ConfirmLanguage state/count missing",
)
require("mono_profiler_set_call_instrumentation_filter_callback" in SOURCE, "no JIT filter")
require("MONO_PROFILER_CALL_INSTRUMENTATION_NONE" in SOURCE, "non-target methods not excluded")
require(
    'env_enabled("MACRUNNER_HB_POST_LEVEL_MILESTONE_TRACE")' in SOURCE
    and "post_level_milestone_trace" in SOURCE
    and "!return_route_only && !language_flow_observer && !post_level_milestone_trace" in SOURCE,
    "post-level milestone trace must be default-off and parsed once at observer init",
)
require(
    'strings_equal(class_name, "GameManager")' in SOURCE
    and 'strings_equal(method_name, "LevelActivated") && arity == 2' in SOURCE
    and 'strings_equal(class_name, "UIManager")' in SOURCE
    and 'strings_equal(method_name, "MakeMenuLean") && arity == 0' in SOURCE
    and 'strings_equal(class_name, "OpeningSequence")' in SOURCE
    and 'strings_equal(method_name, "OnChangingSequences") && arity == 0' in SOURCE,
    "post-level classifier must accept only the exact class/method/arity identities",
)
require(
    "MONO_PROFILER_CALL_INSTRUMENTATION_ENTER |\n"
    "                               MONO_PROFILER_CALL_INSTRUMENTATION_LEAVE" in SOURCE
    and "post_level_observed != METHOD_OTHER" in SOURCE,
    "post-level targets must request enter plus leave callbacks only",
)
require(
    "!post_level_milestone_trace &&\n        env_enabled(\"MACRUNNER_HB_LANGUAGE_DIAGNOSTIC_DIRECT_CONFIRM\")"
    in SOURCE
    and "if (post_level_milestone_trace)\n    {\n        RESOLVE_API(module, method_signature" in SOURCE
    and "RESOLVE_API(module, enable_allocations" in SOURCE[
        SOURCE.index("if (diagnostic_direct_confirm)") :
    ],
    "post-level trace path must not enable allocations, direct confirm, or runtime invoke",
)
require(
    'observer_log("post-level-milestone", detail)' in SOURCE
    and 'builder_append(&builder, " managed_tid=")' in SOURCE
    and 'builder_append(&builder, " sequence=")' in SOURCE
    and "post-level-milestone-trace=armed" in SOURCE
    and "GameManager.LevelActivated/2@0x06000D53:0x4C870" in SOURCE
    and "UIManager.MakeMenuLean/0@0x06000F17:0x52560" in SOURCE
    and "OpeningSequence.OnChangingSequences/0@0x06000355:0x194A4" in SOURCE,
    "post-level trace must emit bounded records and startup proof with static token/RVA",
)
require(
    'builder_append(builder, value ? "true" : "false")' in SOURCE
    and 'builder_append(builder, "unavailable reason=")' in SOURCE
    and "confirmedLanguage=\");\n        builder_append_signed(&builder, value)" not in SOURCE,
    "confirmedLanguage unavailable reads must not be rendered as -1",
)
for forbidden in ("SendInput", "mouse_event", "keybd_event"):
    require(forbidden not in SOURCE, f"observer must not cause managed/input action: {forbidden}")
require(
    'env_enabled("MACRUNNER_HB_LANGUAGE_DIAGNOSTIC_DIRECT_CONFIRM")' in SOURCE
    and "if (!diagnostic_direct_confirm ||" in SOURCE
    and "diagnostic_direct_confirm_attempted" in SOURCE
    and "classification=NOT_GOLDEN" in SOURCE,
    "direct ConfirmLanguage fallback must remain default-off, one-shot, and NOT_GOLDEN",
)
require(
    "mono_profiler_enable_allocations" in SOURCE
    and "mono_profiler_set_gc_allocation_callback" in SOURCE
    and "mono_object_get_class" in SOURCE
    and 'strings_equal(class_name, "StartManager")' in SOURCE
    and "mono_gchandle_new" in SOURCE
    and "mono_class_get_method_from_name" in SOURCE
    and "mono_runtime_invoke" in SOURCE,
    "diagnostic fallback must capture the allocated StartManager object and invoke its exact method",
)
require(
    "mono.gchandle_new(object, 1)" in SOURCE
    and "mono.set_gc_allocation(observer_handle, NULL)" in SOURCE
    and "mono_gchandle_get_target" not in SOURCE,
    "fallback must pin once, disable allocation callbacks, and avoid late target resolution",
)
invoke = SOURCE[SOURCE.index("static void invoke_diagnostic_direct_confirm") :]
require(
    invoke.index("phase=invoke status=begin") < invoke.index("mono.runtime_invoke"),
    "runtime invoke must be preceded by an exact boundary record",
)
require(
    SOURCE.index("invoke_diagnostic_direct_confirm();")
    > SOURCE.index('case METHOD_HIGHLIGHT_DEFAULT:'),
    "diagnostic fallback must wait for the exact language UI readiness callback",
)
require(
    'env_enabled("MACRUNNER_HB_LANGUAGE_ONESHOT_SEQUENCE")' in SOURCE
    and "!return_route_only && language_flow_observer && !post_level_milestone_trace" in SOURCE
    and "oneshot_sequence_attempted" in SOURCE,
    "accepted 145129 one-shot sequence must remain default-off, bounded, and excluded from post-level mode",
)
oneshot = SOURCE[
    SOURCE.index("static void invoke_oneshot_sequence")
    : SOURCE.index("static void gc_allocation")
]
require(
    'mono.image_loaded("Assembly-CSharp")' in oneshot
    and 'mono.class_from_name(image, "", "StartManager")' in oneshot
    and '"FindObjectsOfType", 1' in oneshot
    and "mono.gchandle_new(instance, 1)" in oneshot,
    "one-shot must preserve the accepted no-allocation-profiler StartManager lookup and lifetime",
)
require(
    oneshot.index('mono.class_get_method_from_name(object_class, "SetLanguage", 1)')
    < oneshot.index('mono.class_get_method_from_name(object_class, "ConfirmLanguage", 0)')
    < oneshot.index("mono.runtime_invoke(confirm_method, instance, NULL, &exception)"),
    "one-shot must preserve the accepted SetLanguage(EN) then ConfirmLanguage sequence",
)
require(
    "set_allowSceneActivation" not in oneshot,
    "one-shot must not control scene activation",
)
scene_log = SOURCE[
    SOURCE.index("static void log_scene_load")
    : SOURCE.index("static void log_manager_awake")
]
require(
    "mono.runtime_invoke" not in scene_log and "set_allowSceneActivation" not in scene_log,
    "LoadScene observation must remain logging-only",
)
allow_read = SOURCE[
    SOURCE.index("static int read_allow_scene_activation_property")
    : SOURCE.index("static void log_allow_scene_activation_snapshot")
]
require(
    '"loadop"' in allow_read
    and '"get_allowSceneActivation", 0' in allow_read
    and "mono.object_unbox(boxed)" in allow_read
    and '"set_allowSceneActivation"' not in allow_read,
    "bool fallback must read StartManager.loadop through the getter without scene control",
)
for forbidden in ("snprintf", "strtoul", "strcmp", "memcpy"):
    require(forbidden not in SOURCE, f"observer must remain independent of CRT startup: {forbidden}")
require(
    "-nostdlib" in BUILD_SOURCE
    and "--entry,DllMain" in BUILD_SOURCE
    and "zero_pe_entry.py" in BUILD_SOURCE
    and "-fno-stack-protector" in BUILD_SOURCE,
    "observer build must use the no-CRT/no-entry explicit-init contract",
)
require("-luser32" in BUILD_SOURCE, "passive managed HWND/focus evidence import missing")
require(
    'struct.pack_into("<I", image, optional_offset + 16, 0)' in ZERO_ENTRY_SOURCE
    and 'image[pe_offset : pe_offset + 4] != b"PE\\0\\0"' in ZERO_ENTRY_SOURCE,
    "no-entry post-link step must validate PE structure before clearing AddressOfEntryPoint",
)

require(
    'get_env( L"MACRUNNER_HB_LANGUAGE_FLOW_OBSERVER"' in LOADER_SOURCE,
    "Mono profiler bootstrap must remain default-off",
)
require(
    "macrunner_hb_is_exact_mono_name( libname )" in LOADER_SOURCE
    and "macrunner_hb_is_exact_mono_name( &wm->ldr.BaseDllName )" in LOADER_SOURCE
    and 'RTL_CONSTANT_STRING( L"mono-2.0-bdwgc.dll" )' in LOADER_SOURCE,
    "profiler bootstrap must be scoped to the exact Mono runtime module",
)
require(
    'RTL_CONSTANT_STRING( L"mono-profiler-hk_language.dll" )' in LOADER_SOURCE
    and 'static const char export_name[] = "macrunner_hb_profiler_init_hk_language"'
    in LOADER_SOURCE
    and "macrunner_hb_prepare_language_observer" in LOADER_SOURCE
    and "macrunner_hb_initialize_language_observer" in LOADER_SOURCE,
    "embedded Mono bootstrap must preload and initialize only the read-only observer",
)
require(
    "MACRUNNER_HB_LANGUAGE_OBSERVER_BOOTSTRAP_VERSION 0x484b4c32u" in SOURCE
    and "MACRUNNER_HB_LANGUAGE_OBSERVER_BOOTSTRAP_VERSION 0x484b4c32u" in LOADER_SOURCE
    and "macrunner_hb_profiler_init_hk_language" in SOURCE
    and "bootstrap->introspection_enabled != 1" in SOURCE
    and "bootstrap.introspection_enabled = 1" in LOADER_SOURCE
    and "invalid-bootstrap-contract" in SOURCE,
    "observer bootstrap ABI must be versioned and validated on both sides",
)
require(
    "macrunner_hb_call_x64_cdecl_one_arg" in LOADER_SOURCE
    and "unix_macrunner_hb_x64_dll_entry" in LOADER_SOURCE
    and "void   *arg0;" in UNIXLIB_SOURCE
    and "params->arg0 ? params->arg0 : params->module" in HB_SOURCE,
    "ARM64EC bootstrap must cross the x64 callback corridor",
)
require(
    "macrunner_hb_register_x64_original_exec_sections( params->module, nt )" in HB_SOURCE,
    "x64 DLL corridor must register executable sections before observer callbacks",
)
require(
    "macrunner_hb_x64_original_exec_module_from_pc" in HB_SOURCE
    and "if (size >= best_size) continue;" in HB_SOURCE
    and "cached broad winner cover a nested, narrower PE section" in HB_SOURCE
    and "macrunner_hb_x64_original_exec_cache_count == count" in HB_SOURCE,
    "overlapping builtin PE ranges need exact, bounded original-section resolution",
)
module_from_pc = HB_SOURCE[HB_SOURCE.index("static void *macrunner_hb_module_from_pc") :]
require(
    module_from_pc.index("macrunner_hb_x64_original_exec_module_from_pc( addr )")
    < module_from_pc.index("MACRUNNER_HB_MODULE_FROM_PC_CACHE_SIZE"),
    "exact original PE identity must precede the broader LDR/module cache",
)
guest_no_lock = HB_SOURCE[HB_SOURCE.index("int macrunner_hb_pc_is_x64_guest_code_no_lock") :]
require(
    guest_no_lock.index("macrunner_hb_x64_original_exec_module_from_pc")
    < guest_no_lock.index("macrunner_hb_is_registered_x64_guest_address"),
    "exact registered AMD64 executable sections must precede broad guest-range admission",
)
pe_module_no_lock = HB_SOURCE[HB_SOURCE.index("void *macrunner_hb_pe_module_from_pc_no_lock") :]
require(
    pe_module_no_lock.index("macrunner_hb_x64_original_exec_module_from_pc")
    < pe_module_no_lock.index("macrunner_hb_ldr_entry_from_pc"),
    "callback module identity must prefer the exact PE over an overlapping dynamic builtin",
)
ldr_load_dll = LOADER_SOURCE[LOADER_SOURCE.index("NTSTATUS WINAPI DECLSPEC_HOTPATCH LdrLoadDll") :]
require(
    ldr_load_dll.index("macrunner_hb_start_language_observer(")
    < ldr_load_dll.index("RtlEnterCriticalSection( &loader_section )")
    and ldr_load_dll.index("macrunner_hb_publish_language_observer_mono(")
    < ldr_load_dll.index("nts = process_attach("),
    "observer worker must start outside loader lock and poll before Mono process_attach",
)
require(
    'static const char export_name[] = "mono_profiler_enable_call_context_introspection"'
    in LOADER_SOURCE
    and "RtlFindExportedRoutineByName( mono_module, export_name )" in LOADER_SOURCE
    and "RtlCreateUserThread" in LOADER_SOURCE
    and "i++ == 120000" in LOADER_SOURCE
    and "i < 5000" in LOADER_SOURCE
    and "observer-worker-start-timeout" in LOADER_SOURCE
    and "observer-worker-arm-timeout" in LOADER_SOURCE
    and "mono-module-publish-timeout" in LOADER_SOURCE
    and "NtQueryPerformanceCounter" in LOADER_SOURCE
    and "YieldProcessor" in LOADER_SOURCE
    and "code[0] != 0x48" in LOADER_SOURCE
    and "code[20] != 0xff" in LOADER_SOURCE
    and "code[31] != 0xc7" in LOADER_SOURCE
    and "code[45] != 0xc3" in LOADER_SOURCE
    and "call_contexts = macrunner_hb_mono_rel32_target" in LOADER_SOURCE
    and "mono-introspection-export-shape" in LOADER_SOURCE
    and "mono-introspection-hook-call" in LOADER_SOURCE
    and "mono-introspection-startup-done" in LOADER_SOURCE
    and "mono-introspection-ready-timeout" in LOADER_SOURCE,
    "observer bootstrap must wait on the bounded Mono hook-ready/lock-open gate",
)
for forbidden in ("ConfirmLanguage", "mono_runtime_invoke", "SendInput", "keybd_event"):
    require(
        forbidden not in LOADER_SOURCE,
        f"loader bootstrap must not perform managed/input action: {forbidden}",
    )

require(CONTRACT["event_system"]["first_selected_game_object"] is None, "firstSelected drift")
require(CONTRACT["language_select"]["preselected"] == "EnglishButton", "language preselect drift")
require(CONTRACT["language_confirm"]["preselected"] == "CancelButton", "confirm preselect drift")
require(
    CONTRACT["language_confirm"]["confirm_navigation"] == ["left", "right"],
    "confirm navigation drift",
)
require(
    CONTRACT["minimum_initial_keyboard_actions"]
    == ["Submit:EnglishButton", "Horizontal:CancelButton->ConfirmButton", "Submit:ConfirmButton"],
    "minimum input sequence drift",
)
require(CONTRACT["one_event_initial_ab_reaches_confirm_language"] is False, "one-event claim unsafe")

print("PASS: HK first-run language observer and sealed static input contract")
