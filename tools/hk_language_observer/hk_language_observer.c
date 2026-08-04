#include <windows.h>
#include <stdint.h>

typedef struct _MonoArray MonoArray;
typedef struct _MonoClass MonoClass;
typedef struct _MonoClassField MonoClassField;
typedef struct _MonoDomain MonoDomain;
typedef struct _MonoImage MonoImage;
typedef struct _MonoMethod MonoMethod;
typedef struct _MonoMethodSignature MonoMethodSignature;
typedef struct _MonoObject MonoObject;
typedef struct _MonoProfiler MonoProfiler;
typedef struct _MonoProfilerCallContext MonoProfilerCallContext;
typedef struct _MonoReflectionType MonoReflectionType;
typedef struct _MonoString MonoString;
typedef struct _MonoType MonoType;
typedef struct _MonoVTable MonoVTable;
typedef void *MonoProfilerHandle;
typedef ULONG MonoGCHandle;

typedef int (__cdecl *mono_profiler_enable_call_context_introspection_fn)(void);
typedef int (__cdecl *mono_profiler_enable_allocations_fn)(void);
typedef MonoProfilerHandle (__cdecl *mono_profiler_create_fn)(MonoProfiler *profiler);
typedef const char *(__cdecl *mono_method_get_name_fn)(MonoMethod *method);
typedef MonoClass *(__cdecl *mono_method_get_class_fn)(MonoMethod *method);
typedef const char *(__cdecl *mono_class_get_name_fn)(MonoClass *klass);
typedef const char *(__cdecl *mono_class_get_namespace_fn)(MonoClass *klass);
typedef MonoClassField *(__cdecl *mono_class_get_field_from_name_fn)(MonoClass *klass,
                                                                     const char *name);
typedef MonoMethod *(__cdecl *mono_class_get_method_from_name_fn)(MonoClass *klass,
                                                                  const char *name,
                                                                  int parameter_count);
typedef void (__cdecl *mono_field_get_value_fn)(MonoObject *object, MonoClassField *field,
                                                 void *value);
typedef MonoGCHandle (__cdecl *mono_gchandle_new_fn)(MonoObject *object, int pinned);
typedef MonoImage *(__cdecl *mono_image_loaded_fn)(const char *name);
typedef MonoClass *(__cdecl *mono_class_from_name_fn)(MonoImage *image,
                                                       const char *name_space,
                                                       const char *name);
typedef MonoType *(__cdecl *mono_class_get_type_fn)(MonoClass *klass);
typedef MonoReflectionType *(__cdecl *mono_type_get_object_fn)(MonoDomain *domain,
                                                               MonoType *type);
typedef MonoDomain *(__cdecl *mono_get_root_domain_fn)(void);
typedef MonoString *(__cdecl *mono_string_new_fn)(MonoDomain *domain, const char *text);
typedef uintptr_t (__cdecl *mono_array_length_fn)(MonoArray *array);
typedef char *(__cdecl *mono_array_addr_with_size_fn)(MonoArray *array, int size,
                                                       uintptr_t index);
typedef MonoClass *(__cdecl *mono_object_get_class_fn)(MonoObject *object);
typedef void *(__cdecl *mono_object_unbox_fn)(MonoObject *object);
typedef MonoObject *(__cdecl *mono_runtime_invoke_fn)(MonoMethod *method, void *object,
                                                       void **parameters,
                                                       MonoObject **exception);
typedef MonoMethodSignature *(__cdecl *mono_method_signature_fn)(MonoMethod *method);
typedef unsigned int (__cdecl *mono_signature_get_param_count_fn)(MonoMethodSignature *signature);
typedef MonoType *(__cdecl *mono_signature_get_params_fn)(MonoMethodSignature *signature, void **iter);
typedef char *(__cdecl *mono_type_get_name_fn)(MonoType *type);
typedef void *(__cdecl *mono_profiler_call_context_get_this_fn)(MonoProfilerCallContext *context);
typedef void *(__cdecl *mono_profiler_call_context_get_argument_fn)(MonoProfilerCallContext *context,
                                                                     ULONG position);
typedef void (__cdecl *mono_profiler_call_context_free_buffer_fn)(void *buffer);
typedef char *(__cdecl *mono_string_to_utf8_fn)(MonoString *string);
typedef void (__cdecl *mono_free_fn)(void *memory);

/* newobj probe (MACRUNNER_HB_NEWOBJ_PROBE). All five are exported by the shipped
 * mono-2.0-bdwgc.dll -- verified by export-table scan before this was written:
 * mono_class_instance_size 0xba910, mono_class_get_nested_types 0xc1180,
 * mono_class_vtable 0x1c5de0, mono_object_new_specific 0x1d0310,
 * mono_object_new 0x1cfe50. */
typedef int32_t (__cdecl *mono_class_instance_size_fn)(MonoClass *klass);
typedef MonoClass *(__cdecl *mono_class_get_nested_types_fn)(MonoClass *klass, void **iter);
typedef MonoVTable *(__cdecl *mono_class_vtable_fn)(MonoDomain *domain, MonoClass *klass);
typedef MonoObject *(__cdecl *mono_object_new_specific_fn)(MonoVTable *vtable);
typedef MonoObject *(__cdecl *mono_object_new_fn)(MonoDomain *domain, MonoClass *klass);

typedef int (__cdecl *MonoProfilerCallInstrumentationFilterCallback)(MonoProfiler *profiler,
                                                                     MonoMethod *method);
typedef void (__cdecl *MonoProfilerMethodCallback)(MonoProfiler *profiler, MonoMethod *method,
                                                    MonoProfilerCallContext *context);
typedef void (__cdecl *MonoProfilerGCAllocationCallback)(MonoProfiler *profiler,
                                                          MonoObject *object);
typedef void (__cdecl *mono_profiler_set_call_instrumentation_filter_callback_fn)(
    MonoProfilerHandle handle, MonoProfilerCallInstrumentationFilterCallback callback);
typedef void (__cdecl *mono_profiler_set_method_callback_fn)(MonoProfilerHandle handle,
                                                              MonoProfilerMethodCallback callback);
typedef void (__cdecl *mono_profiler_set_gc_allocation_callback_fn)(
    MonoProfilerHandle handle, MonoProfilerGCAllocationCallback callback);

enum
{
    MONO_PROFILER_CALL_INSTRUMENTATION_NONE = 0,
    MONO_PROFILER_CALL_INSTRUMENTATION_ENTER = 1 << 1,
    MONO_PROFILER_CALL_INSTRUMENTATION_ENTER_CONTEXT = 1 << 2,
    MONO_PROFILER_CALL_INSTRUMENTATION_LEAVE = 1 << 3,
    MONO_PROFILER_CALL_INSTRUMENTATION_LEAVE_CONTEXT = 1 << 4
};

enum observed_method
{
    METHOD_OTHER,
    METHOD_SET_LANGUAGE,
    METHOD_CONFIRM_LANGUAGE,
    METHOD_HIGHLIGHT_DEFAULT,
    METHOD_ALLOW_SCENE_ACTIVATION,
    METHOD_GAME_MANAGER_AWAKE,
    METHOD_GAME_CAMERAS_AWAKE,
    METHOD_UI_MANAGER_AWAKE,
    METHOD_LEVEL_ACTIVATED,
    METHOD_MAKE_MENU_LEAN,
    METHOD_OPENING_SEQUENCE_ON_CHANGING_SEQUENCES,
    METHOD_SCENE_LOAD,
    METHOD_SCENE_LOAD_ASYNC,
    METHOD_CAMERA_TICK,
    METHOD_ASYNCOP_POLL,
    METHOD_MANAGED_TICK
};

struct mono_api
{
    mono_profiler_enable_call_context_introspection_fn enable_call_context_introspection;
    mono_profiler_enable_allocations_fn enable_allocations;
    mono_profiler_create_fn profiler_create;
    mono_profiler_set_call_instrumentation_filter_callback_fn set_filter;
    mono_profiler_set_method_callback_fn set_method_enter;
    mono_profiler_set_method_callback_fn set_method_leave;
    mono_profiler_set_gc_allocation_callback_fn set_gc_allocation;
    mono_method_get_name_fn method_get_name;
    mono_method_get_class_fn method_get_class;
    mono_class_get_name_fn class_get_name;
    mono_class_get_namespace_fn class_get_namespace;
    mono_class_get_field_from_name_fn class_get_field_from_name;
    mono_class_get_method_from_name_fn class_get_method_from_name;
    mono_image_loaded_fn image_loaded;
    mono_class_from_name_fn class_from_name;
    mono_class_get_type_fn class_get_type;
    mono_type_get_object_fn type_get_object;
    mono_get_root_domain_fn get_root_domain;
    mono_string_new_fn string_new;
    mono_array_length_fn array_length;
    mono_array_addr_with_size_fn array_addr_with_size;
    mono_field_get_value_fn field_get_value;
    mono_gchandle_new_fn gchandle_new;
    mono_object_get_class_fn object_get_class;
    mono_object_unbox_fn object_unbox;
    mono_runtime_invoke_fn runtime_invoke;
    mono_method_signature_fn method_signature;
    mono_signature_get_param_count_fn signature_get_param_count;
    mono_signature_get_params_fn signature_get_params;
    mono_type_get_name_fn type_get_name;
    mono_profiler_call_context_get_this_fn context_get_this;
    mono_profiler_call_context_get_argument_fn context_get_argument;
    mono_profiler_call_context_free_buffer_fn context_free_buffer;
    mono_string_to_utf8_fn string_to_utf8;
    mono_free_fn mono_free;
    mono_class_instance_size_fn class_instance_size;
    mono_class_get_nested_types_fn class_get_nested_types;
    mono_class_vtable_fn class_vtable;
    mono_object_new_specific_fn object_new_specific;
    mono_object_new_fn object_new;
};

#define MACRUNNER_HB_LANGUAGE_OBSERVER_BOOTSTRAP_VERSION 0x484b4c32u

struct macrunner_hb_language_observer_bootstrap
{
    ULONG version;
    ULONG introspection_enabled;
    HMODULE mono_module;
    const char *description;
};

static struct mono_api mono;
static LONG record_count;
static LONG record_limit = 128;
static LONG confirm_count;
static LONG allow_scene_activation = -1;
static LONG game_manager_awake;
static LONG game_cameras_awake;
static LONG ui_manager_awake;
static LONG return_route_only;
static LONG language_flow_observer;
static LONG oneshot_sequence;
static LONG oneshot_sequence_attempted;
static LONG post_level_milestone_trace;
static LONG post_level_sequence;
static LONG diagnostic_direct_confirm;
static LONG diagnostic_direct_confirm_capture_started;
static LONG diagnostic_direct_confirm_attempted;
static LONG start_manager_handle;
static MonoObject *start_manager_object;
static MonoMethod *confirm_language_method;
static MonoGCHandle retained_startmanager_handle;
static MonoObject *retained_startmanager_object;
static MonoProfilerHandle observer_handle;
static MonoClassField *confirmed_language_field;
static MonoClassField *load_operation_field;
static MonoMethod *allow_scene_activation_getter;
static char log_path[MAX_PATH];
/* MACRUNNER_HB_START_GAME_ACTUATOR — default OFF.
 *
 * WHY THIS EXISTS: the acceptance checklist (Making UI menu lean. / Opening_Sequence /
 * Levels are ready) is NOT reachable by a correct engine alone. hk_callers.py shows
 * UIManager::MakeMenuLean (0x06000f17) has exactly two callers, <RunStartNewGame>d__359
 * and <RunContinueGame>d__361 -- both PLAYER-initiated. run20 rendered the language
 * screen and its confirm dialog correctly, fired PreselectOption::HighlightDefault (the
 * menu highlighting its default button), and then waited for a keypress that no part of
 * our harness ever sends. So those markers are unreachable BY CONSTRUCTION.
 *
 * This presses the button, by the same proven route as the language actuator:
 * FindObjectsOfType -> exact_one gate -> mono_runtime_invoke.
 *
 * Target: UIManager::UIStartNewGame (token 0x06000ebd), verified ARITY 0 against the
 * shipped Assembly-CSharp.dll; its entire body is StartNewGame(this, false, false).
 *
 * NB it deliberately does NOT latch on the first attempt: UIManager lives in Menu_Title,
 * so the earliest HighlightDefault (language select) legitimately finds zero of them.
 * Latching there would burn the one shot before the target exists. It retries on each
 * trigger and latches only on a successful invoke. Counters are aggregate, never capped,
 * so a zero is a real zero rather than an exhausted budget. */
static LONG start_game_actuator;
static LONG start_game_latched;        /* set only after a successful invoke */
static LONG start_game_attempts;       /* aggregate, never capped */
static LONG start_game_not_found;      /* aggregate: UIManager not resolvable yet */
static LONG start_game_exceptions;     /* aggregate */
static LONG start_game_budget = 64;    /* retry cap so a broken menu cannot spin forever */

/* MACRUNNER_HB_NEWOBJ_PROBE — default OFF. Runs immediately BEFORE the
 * UIStartNewGame invoke, on the SAME thread, so it measures the state that the
 * failing allocation actually sees.
 *
 * WHY: runs 23 and 24 die identically (2/2, byte-identical last_bytes) at
 *   0x...c93: mov r14,rax / mov [rax+0x10],r15   with rax == 0
 * i.e. Mono's newobj for UIManager's `<>c__DisplayClass170_0` returned NULL.
 * Journal section 87 traced the only NULL route on the branch our object takes:
 *   mono_object_new_specific_checked +0x179 (rva 0x1d04c9):
 *     mov    rax,[rdi]              ; rax = vtable->klass
 *     mov    rcx,rdi                ; arg0 = vtable
 *     movsxd rdx,dword [rax+0x1c]   ; arg1 = klass->instance_size, SIGN-EXTENDED
 *     call   0x264720               ; mono_gc_alloc_obj
 * and I confirmed by disassembling 0x264720 that it performs NO size validation
 * -- its only NULL paths are the three Boehm allocators returning NULL.
 *
 * [H-D] says that guest read of [klass+0x1c] yields garbage, so a sign-extended
 * huge/negative size is rejected by Boehm with no OS request -- which reproduces
 * run24's counters exactly (virtualalloc_fail=15, ALL STATUS_CONFLICTING_ADDRESSES,
 * commit_fail_mprotect=0: our allocator never once refused MEMORY).
 *
 * Section 87 also recorded the cost not to repeat: run23 captured only the vtable,
 * never the klass, so [klass+0x1c] could not be read after the fact. This captures
 * the KLASS. */
static LONG newobj_probe;
static LONG newobj_probe_ran;        /* aggregate, never capped */
static LONG newobj_probe_control_ok; /* aggregate: control passed */
/* MACRUNNER_HB_CAMERA_PROBE — read Unity's OWN camera matrices as uint32 bit
 * patterns. Answers whether the view/projection HK feeds its draws is singular
 * or non-finite, independently of DXMT (whose constant-buffer probe classifies
 * arbitrary CB bytes and whose PE-side formatter cannot print floats at all). */
static LONG camera_probe;
static LONG asyncop_poll;        /* MACRUNNER_HB_ASYNCOP_POLL: hook AsyncOperation.get_isDone/get_progress */
static LONG asyncop_poll_count;
static LONG managed_tick;        /* MACRUNNER_HB_MANAGED_TICK: is the managed frame loop alive at all? */
static LONG managed_tick_count;
static LONG camera_probe_budget;
static LONG camera_probe_stride = 1;
static LONG camera_probe_dumps;      /* aggregate, never capped */
static LONG camera_probe_suppressed; /* aggregate, never capped */
static LONG camera_probe_ticks;      /* aggregate, never capped */
static LONG camera_probe_active;     /* reentrancy guard */
static LONG camera_probe_control_done;
/* MACRUNNER_HB_CAMERA_M33_REPAIR — DIAGNOSTIC ONLY, default OFF. run19 measured
 * worldToCameraMatrix[15] == 0x0000087e (invariant over 14 samples / 4 cameras /
 * 4 phases) where 0x3f800000 is required, which pins ndc.z at 1.000600 at EVERY
 * depth and far-plane-clips every triangle. This knob repairs that ONE float at
 * the managed boundary and re-sets the matrix, to settle the root-cause report's
 * §6 limit: the probe reads through the managed getter, so it proves the value
 * Mono returns is corrupt, NOT that the same bytes reach UnityPlayer's C++
 * renderer. If repairing only m33 turns the ordinal-200 readback non-black, the
 * causal chain is proven end-to-end rather than argued. This is NOT the fix —
 * the fix belongs in HyperBridge; a managed-boundary patch would light the screen
 * while leaving a translator defect that silently truncates struct tails. */
static LONG camera_m33_repair;
static LONG m33_attempted;      /* aggregate, never capped */
static LONG m33_patched;
static LONG m33_already_ok;
static LONG m33_unreadable;
static LONG m33_setter_missing;
static LONG m33_exception;
static LONG m33_verified;       /* read back == 0x3f800000 through the same path */
static LONG m33_verify_failed;
static LONG m33_logged;

struct text_builder
{
    char *buffer;
    SIZE_T capacity;
    SIZE_T length;
};

static void builder_init(struct text_builder *builder, char *buffer, SIZE_T capacity)
{
    builder->buffer = buffer;
    builder->capacity = capacity;
    builder->length = 0;
    if (capacity) buffer[0] = 0;
}

static void builder_append(struct text_builder *builder, const char *text)
{
    while (*text && builder->length + 1 < builder->capacity)
        builder->buffer[builder->length++] = *text++;
    if (builder->capacity) builder->buffer[builder->length] = 0;
}

static void builder_append_unsigned(struct text_builder *builder, ULONGLONG value)
{
    char digits[24];
    ULONG count = 0;

    do
    {
        digits[count++] = '0' + value % 10;
        value /= 10;
    } while (value && count < sizeof(digits));
    while (count)
    {
        char digit[2] = { digits[--count], 0 };
        builder_append(builder, digit);
    }
}

static void builder_append_signed(struct text_builder *builder, LONGLONG value)
{
    if (value < 0)
    {
        builder_append(builder, "-");
        builder_append_unsigned(builder, (ULONGLONG)(-value));
    }
    else builder_append_unsigned(builder, value);
}

static void copy_bytes(void *destination, const void *source, SIZE_T size)
{
    BYTE *dst = destination;
    const BYTE *src = source;

    while (size--) *dst++ = *src++;
}

static int strings_equal(const char *left, const char *right)
{
    while (*left && *left == *right)
    {
        left++;
        right++;
    }
    return *left == *right;
}

static int env_enabled(const char *name)
{
    char value[16];
    DWORD size = GetEnvironmentVariableA(name, value, sizeof(value));

    return size && size < sizeof(value) && value[0] && value[0] != '0';
}

static LONG env_record_limit(const char *name)
{
    char value[32];
    unsigned long parsed;
    DWORD i;
    DWORD size = GetEnvironmentVariableA(name, value, sizeof(value));

    if (!size || size >= sizeof(value)) return 128;
    parsed = 0;
    for (i = 0; i < size; i++)
    {
        if (value[i] < '0' || value[i] > '9') return 128;
        parsed = parsed * 10 + value[i] - '0';
        if (parsed > 1024) return 128;
    }
    if (!parsed) return 128;
    return (LONG)parsed;
}

static void write_bytes(const char *bytes, DWORD size)
{
    HANDLE handle;
    DWORD written;

    if (log_path[0])
    {
        handle = CreateFileA(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (handle != INVALID_HANDLE_VALUE)
        {
            WriteFile(handle, bytes, size, &written, NULL);
            CloseHandle(handle);
            return;
        }
    }
    handle = GetStdHandle(STD_ERROR_HANDLE);
    if (handle && handle != INVALID_HANDLE_VALUE) WriteFile(handle, bytes, size, &written, NULL);
}

static void observer_log(const char *event, const char *detail)
{
    char line[768];
    struct text_builder builder;
    LONG ordinal = InterlockedIncrement(&record_count);

    if (ordinal > record_limit)
    {
        if (ordinal == record_limit + 1)
        {
            builder_init(&builder, line, sizeof(line));
            builder_append(&builder,
                           "macrunner-hb-language-flow: event=budget-exhausted limit=");
            builder_append_signed(&builder, record_limit);
            builder_append(&builder, "\r\n");
            write_bytes(line, (DWORD)builder.length);
        }
        return;
    }
    builder_init(&builder, line, sizeof(line));
    builder_append(&builder, return_route_only ? "macrunner-hb-return-route-managed: seq="
                                               : "macrunner-hb-language-flow: seq=");
    builder_append_signed(&builder, ordinal);
    builder_append(&builder, " event=");
    builder_append(&builder, event);
    builder_append(&builder, " ");
    if (detail) builder_append(&builder, detail);
    builder_append(&builder, "\r\n");
    write_bytes(line, (DWORD)builder.length);
}

static FARPROC resolve(HMODULE module, const char *name)
{
    FARPROC proc = GetProcAddress(module, name);
    char detail[256];
    struct text_builder builder;

    if (!proc)
    {
        builder_init(&builder, detail, sizeof(detail));
        builder_append(&builder, "status=missing-api api=");
        builder_append(&builder, name);
        observer_log("init-failed", detail);
    }
    return proc;
}

#define RESOLVE_API(module, field, name) \
    do { *(FARPROC *)&mono.field = resolve((module), (name)); if (!mono.field) return; } while (0)

#define RESOLVE_OPTIONAL_API(module, field, name) \
    do { *(FARPROC *)&mono.field = GetProcAddress((module), (name)); } while (0)

static enum observed_method classify_method(MonoMethod *method)
{
    MonoClass *klass;
    const char *class_name, *namespace_name, *method_name;

    if (!method || !(method_name = mono.method_get_name(method)) ||
        !(klass = mono.method_get_class(method)) || !(class_name = mono.class_get_name(klass)))
        return METHOD_OTHER;
    namespace_name = mono.class_get_namespace(klass);

    if (strings_equal(class_name, "StartManager"))
    {
        if (strings_equal(method_name, "SetLanguage")) return METHOD_SET_LANGUAGE;
        if (strings_equal(method_name, "ConfirmLanguage")) return METHOD_CONFIRM_LANGUAGE;
    }
    if (strings_equal(class_name, "PreselectOption") &&
        strings_equal(method_name, "HighlightDefault"))
        return METHOD_HIGHLIGHT_DEFAULT;
    if (namespace_name && strings_equal(namespace_name, "UnityEngine") &&
        strings_equal(class_name, "AsyncOperation") &&
        strings_equal(method_name, "set_allowSceneActivation"))
        return METHOD_ALLOW_SCENE_ACTIVATION;
    /* MacRunner 2026-08-04 — is anybody still POLLING the load?
     *
     * Comparison with the 28.07 run that reached the menu puts the divergence at exactly one
     * step: ours stops after `set_allowSceneActivation` returns (seq=7) while the healthy run
     * goes on to PreselectOption.HighlightDefault.  Two very different defects produce that,
     * and only a measurement separates them: either the coroutine that waits on the operation
     * is no longer running (no polls at all), or it runs and the operation never completes
     * (polls forever with progress stuck).  Hooking the two getters the wait loop uses answers
     * it directly.  Gated: these fire every frame in a healthy game. */
    if (asyncop_poll && namespace_name && strings_equal(namespace_name, "UnityEngine") &&
        strings_equal(class_name, "AsyncOperation") &&
        (strings_equal(method_name, "get_isDone") || strings_equal(method_name, "get_progress")))
        return METHOD_ASYNCOP_POLL;
    if (namespace_name && strings_equal(namespace_name, "UnityEngine.SceneManagement") &&
        strings_equal(class_name, "SceneManager"))
    {
        if (strings_equal(method_name, "LoadScene")) return METHOD_SCENE_LOAD;
        if (strings_equal(method_name, "LoadSceneAsync")) return METHOD_SCENE_LOAD_ASYNC;
    }
    /* MacRunner 2026-08-04 — is the MANAGED frame loop alive after seq=7?
     *
     * The stall sits exactly one step past set_allowSceneActivation, and two very different
     * defects fit: the Unity scripting loop stopped ticking entirely, or it ticks and only the
     * scene activation never lands.  Nothing measured so far separates them — the AsyncOperation
     * getters are not called even in a healthy Unity (yield on an operation resumes natively).
     * A per-frame Update of a manager that certainly exists at this stage does separate them:
     * no ticks after seq=7 means the loop is dead and the load is a symptom. */
    if (managed_tick && strings_equal(method_name, "Update") &&
        (strings_equal(class_name, "GameManager") || strings_equal(class_name, "UIManager") ||
         strings_equal(class_name, "StartManager") || strings_equal(class_name, "InputHandler") ||
         strings_equal(class_name, "CameraController")))
        return METHOD_MANAGED_TICK;
    if (strings_equal(method_name, "Awake"))
    {
        if (strings_equal(class_name, "GameManager")) return METHOD_GAME_MANAGER_AWAKE;
        if (strings_equal(class_name, "GameCameras")) return METHOD_GAME_CAMERAS_AWAKE;
        if (strings_equal(class_name, "UIManager")) return METHOD_UI_MANAGER_AWAKE;
    }
    /* The three managed callers of the Unity unprojection APIs that can emit
     * "Screen position out of view frustum" (from Assembly-CSharp IL). Which of
     * them fires is itself the answer to where the 17927 warnings come from. */
    if (camera_probe)
    {
        if (strings_equal(class_name, "TouchManager") &&
            strings_equal(method_name, "ConvertScreenToWorldPoint"))
            return METHOD_CAMERA_TICK;
        if (strings_equal(class_name, "ScreenToWorldPoint") &&
            strings_equal(method_name, "DoScreenToWorldPoint"))
            return METHOD_CAMERA_TICK;
        if (strings_equal(class_name, "CameraController") &&
            strings_equal(method_name, "LateUpdate"))
            return METHOD_CAMERA_TICK;
    }
    return METHOD_OTHER;
}

static int method_parameter_count(MonoMethod *method, unsigned int *count)
{
    MonoMethodSignature *signature;

    if (!mono.method_signature || !mono.signature_get_param_count) return 0;
    signature = mono.method_signature(method);
    if (!signature) return 0;
    *count = mono.signature_get_param_count(signature);
    return 1;
}

static enum observed_method classify_post_level_method(MonoMethod *method)
{
    MonoClass *klass;
    const char *class_name, *method_name;
    unsigned int arity;

    if (!method || !(method_name = mono.method_get_name(method)) ||
        !(klass = mono.method_get_class(method)) || !(class_name = mono.class_get_name(klass)) ||
        !method_parameter_count(method, &arity))
        return METHOD_OTHER;

    if (strings_equal(class_name, "GameManager") &&
        strings_equal(method_name, "LevelActivated") && arity == 2)
        return METHOD_LEVEL_ACTIVATED;
    if (strings_equal(class_name, "UIManager") &&
        strings_equal(method_name, "MakeMenuLean") && arity == 0)
        return METHOD_MAKE_MENU_LEAN;
    if (strings_equal(class_name, "OpeningSequence") &&
        strings_equal(method_name, "OnChangingSequences") && arity == 0)
        return METHOD_OPENING_SEQUENCE_ON_CHANGING_SEQUENCES;
    return METHOD_OTHER;
}

static int post_level_method_details(enum observed_method observed, const char **class_name,
                                     const char **method_name, unsigned int *arity)
{
    switch (observed)
    {
    case METHOD_LEVEL_ACTIVATED:
        *class_name = "GameManager";
        *method_name = "LevelActivated";
        *arity = 2;
        return 1;
    case METHOD_MAKE_MENU_LEAN:
        *class_name = "UIManager";
        *method_name = "MakeMenuLean";
        *arity = 0;
        return 1;
    case METHOD_OPENING_SEQUENCE_ON_CHANGING_SEQUENCES:
        *class_name = "OpeningSequence";
        *method_name = "OnChangingSequences";
        *arity = 0;
        return 1;
    default:
        return 0;
    }
}

static int language_method_instrumentation(enum observed_method observed)
{
    if (observed == METHOD_OTHER) return MONO_PROFILER_CALL_INSTRUMENTATION_NONE;
    if (observed == METHOD_CAMERA_TICK)
        return camera_probe ? MONO_PROFILER_CALL_INSTRUMENTATION_ENTER
                            : MONO_PROFILER_CALL_INSTRUMENTATION_NONE;
    if (observed == METHOD_ASYNCOP_POLL)
        return asyncop_poll ? MONO_PROFILER_CALL_INSTRUMENTATION_ENTER
                            : MONO_PROFILER_CALL_INSTRUMENTATION_NONE;
    if (observed == METHOD_MANAGED_TICK)
        return managed_tick ? MONO_PROFILER_CALL_INSTRUMENTATION_ENTER
                            : MONO_PROFILER_CALL_INSTRUMENTATION_NONE;
    if (return_route_only && observed != METHOD_SET_LANGUAGE &&
        observed != METHOD_HIGHLIGHT_DEFAULT)
        return MONO_PROFILER_CALL_INSTRUMENTATION_NONE;
    if (!return_route_only && !language_flow_observer)
        return MONO_PROFILER_CALL_INSTRUMENTATION_NONE;
    if (observed == METHOD_CONFIRM_LANGUAGE || observed == METHOD_ALLOW_SCENE_ACTIVATION)
        return MONO_PROFILER_CALL_INSTRUMENTATION_ENTER |
               MONO_PROFILER_CALL_INSTRUMENTATION_ENTER_CONTEXT |
               MONO_PROFILER_CALL_INSTRUMENTATION_LEAVE |
               MONO_PROFILER_CALL_INSTRUMENTATION_LEAVE_CONTEXT;
    if (observed == METHOD_SCENE_LOAD || observed == METHOD_SCENE_LOAD_ASYNC)
        return MONO_PROFILER_CALL_INSTRUMENTATION_ENTER |
               MONO_PROFILER_CALL_INSTRUMENTATION_ENTER_CONTEXT |
               MONO_PROFILER_CALL_INSTRUMENTATION_LEAVE;
    return MONO_PROFILER_CALL_INSTRUMENTATION_ENTER;
}

static int method_filter(MonoProfiler *profiler, MonoMethod *method)
{
    enum observed_method observed = classify_method(method);
    enum observed_method post_level_observed;
    int instrumentation;
    (void)profiler;

    instrumentation = language_method_instrumentation(observed);
    if (post_level_milestone_trace)
    {
        post_level_observed = classify_post_level_method(method);
        if (post_level_observed != METHOD_OTHER)
            instrumentation |= MONO_PROFILER_CALL_INSTRUMENTATION_ENTER |
                               MONO_PROFILER_CALL_INSTRUMENTATION_LEAVE;
    }
    return instrumentation;
}

static MonoObject *context_this(MonoProfilerCallContext *context)
{
    MonoObject *object = NULL;
    void *buffer = context ? mono.context_get_this(context) : NULL;

    if (buffer)
    {
        copy_bytes(&object, buffer, sizeof(object));
        mono.context_free_buffer(buffer);
    }
    return object;
}

static int read_confirmed_language(MonoMethod *method, MonoProfilerCallContext *context,
                                   int *value, const char **unavailable_reason)
{
    MonoClass *klass;
    MonoObject *object = context_this(context);
    BYTE managed_value = 0;

    *unavailable_reason = NULL;
    if (!object)
    {
        *unavailable_reason = "no-this";
        return 0;
    }
    if (!(klass = mono.method_get_class(method)))
    {
        *unavailable_reason = "missing-class";
        return 0;
    }
    if (!confirmed_language_field)
        confirmed_language_field = mono.class_get_field_from_name(klass, "confirmedLanguage");
    if (!confirmed_language_field)
    {
        *unavailable_reason = "missing-field";
        return 0;
    }
    mono.field_get_value(object, confirmed_language_field, &managed_value);
    *value = !!managed_value;
    return 1;
}

static void builder_append_bool_availability(struct text_builder *builder, int available,
                                             int value, const char *unavailable_reason)
{
    if (available) builder_append(builder, value ? "true" : "false");
    else
    {
        builder_append(builder, "unavailable reason=");
        builder_append(builder, unavailable_reason ? unavailable_reason : "unknown");
    }
}

static int read_bool_argument(MonoProfilerCallContext *context, ULONG position, int *value)
{
    void *buffer = context ? mono.context_get_argument(context, position) : NULL;

    if (!buffer) return 0;
    *value = !!*(const BYTE *)buffer;
    mono.context_free_buffer(buffer);
    return 1;
}

static int read_allow_scene_activation_property(int *value, const char **unavailable_reason)
{
    MonoObject *manager, *operation, *boxed, *exception = NULL;
    MonoClass *manager_class, *operation_class;
    void *unboxed;

    *unavailable_reason = NULL;
    manager = InterlockedCompareExchangePointer(
        (void *volatile *)&retained_startmanager_object, NULL, NULL);
    if (!manager)
        manager = InterlockedCompareExchangePointer(
            (void *volatile *)&start_manager_object, NULL, NULL);
    if (!manager)
    {
        *unavailable_reason = "no-retained-start-manager";
        return 0;
    }
    manager_class = mono.object_get_class(manager);
    if (!manager_class)
    {
        *unavailable_reason = "missing-start-manager-class";
        return 0;
    }
    if (!load_operation_field)
        load_operation_field = mono.class_get_field_from_name(manager_class, "loadop");
    if (!load_operation_field)
    {
        *unavailable_reason = "missing-loadop-field";
        return 0;
    }
    operation = NULL;
    mono.field_get_value(manager, load_operation_field, &operation);
    if (!operation)
    {
        *unavailable_reason = "null-loadop";
        return 0;
    }
    operation_class = mono.object_get_class(operation);
    if (!operation_class)
    {
        *unavailable_reason = "missing-async-operation-class";
        return 0;
    }
    if (!allow_scene_activation_getter)
        allow_scene_activation_getter =
            mono.class_get_method_from_name(operation_class, "get_allowSceneActivation", 0);
    if (!allow_scene_activation_getter)
    {
        *unavailable_reason = "missing-allow-scene-activation-getter";
        return 0;
    }
    boxed = mono.runtime_invoke(allow_scene_activation_getter, operation, NULL, &exception);
    if (exception)
    {
        *unavailable_reason = "getter-managed-exception";
        return 0;
    }
    if (!boxed || !(unboxed = mono.object_unbox(boxed)))
    {
        *unavailable_reason = "getter-result-unavailable";
        return 0;
    }
    *value = !!*(const BYTE *)unboxed;
    return 1;
}

static void log_allow_scene_activation_snapshot(const char *phase)
{
    char detail[256];
    struct text_builder builder;
    const char *unavailable_reason;
    int value = -1;
    int available = read_allow_scene_activation_property(&value, &unavailable_reason);

    if (available) InterlockedExchange(&allow_scene_activation, value);
    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "class=UnityEngine.AsyncOperation method=get_allowSceneActivation phase=");
    builder_append(&builder, phase);
    builder_append(&builder, " value=");
    if (available) builder_append_signed(&builder, value);
    else
    {
        builder_append(&builder, "unavailable reason=");
        builder_append(&builder, unavailable_reason ? unavailable_reason : "unknown");
    }
    builder_append(&builder, " source=StartManager.loadop");
    observer_log("allow-scene-activation-snapshot", detail);
}

static int read_int32_argument(MonoProfilerCallContext *context, ULONG position, int *value)
{
    LONG raw = 0;
    void *buffer = context ? mono.context_get_argument(context, position) : NULL;

    if (!buffer) return 0;
    copy_bytes(&raw, buffer, sizeof(raw));
    mono.context_free_buffer(buffer);
    *value = (int)raw;
    return 1;
}

static void copy_scene_text(char *out, SIZE_T out_cap, const char *text)
{
    SIZE_T i = 0;

    if (!out_cap) return;
    if (!text)
    {
        out[0] = 0;
        return;
    }
    while (*text && i + 1 < out_cap)
    {
        unsigned char ch = (unsigned char)*text++;
        out[i++] = (ch <= ' ' || ch == '=') ? '_' : (char)ch;
    }
    out[i] = 0;
}

static int read_mono_string_argument(MonoProfilerCallContext *context, ULONG position, char *out,
                                     SIZE_T out_cap, int *ok, int *is_null,
                                     const char **reason)
{
    MonoString *string = NULL;
    char *utf8;
    void *buffer = context ? mono.context_get_argument(context, position) : NULL;

    *ok = 0;
    *is_null = 0;
    *reason = NULL;
    if (out_cap) out[0] = 0;
    if (!buffer)
    {
        *reason = "missing-argument";
        return 0;
    }
    copy_bytes(&string, buffer, sizeof(string));
    mono.context_free_buffer(buffer);
    if (!string)
    {
        *is_null = 1;
        *reason = "null-string";
        return 0;
    }
    if (!mono.string_to_utf8 || !mono.mono_free)
    {
        *reason = "missing-string-api";
        return 0;
    }
    utf8 = mono.string_to_utf8(string);
    if (!utf8)
    {
        *reason = "string-to-utf8-null";
        return 0;
    }
    copy_scene_text(out, out_cap, utf8);
    mono.mono_free(utf8);
    *ok = 1;
    return 1;
}

enum scene_argument_kind
{
    SCENE_ARGUMENT_UNKNOWN,
    SCENE_ARGUMENT_STRING,
    SCENE_ARGUMENT_INT32
};

static enum scene_argument_kind scene_argument_kind(MonoMethod *method)
{
    MonoMethodSignature *signature;
    MonoType *type;
    char *type_name;
    void *iter = NULL;
    enum scene_argument_kind kind = SCENE_ARGUMENT_UNKNOWN;

    if (!mono.method_signature || !mono.signature_get_param_count || !mono.signature_get_params ||
        !mono.type_get_name || !mono.mono_free)
        return SCENE_ARGUMENT_UNKNOWN;
    signature = mono.method_signature(method);
    if (!signature || mono.signature_get_param_count(signature) < 1) return SCENE_ARGUMENT_UNKNOWN;
    type = mono.signature_get_params(signature, &iter);
    if (!type) return SCENE_ARGUMENT_UNKNOWN;
    type_name = mono.type_get_name(type);
    if (!type_name) return SCENE_ARGUMENT_UNKNOWN;
    if (strings_equal(type_name, "string") || strings_equal(type_name, "System.String") ||
        strings_equal(type_name, "String"))
        kind = SCENE_ARGUMENT_STRING;
    else if (strings_equal(type_name, "int") || strings_equal(type_name, "int32") ||
             strings_equal(type_name, "System.Int32") || strings_equal(type_name, "Int32"))
        kind = SCENE_ARGUMENT_INT32;
    mono.mono_free(type_name);
    return kind;
}

static void append_scene_argument(struct text_builder *builder, MonoMethod *method,
                                  MonoProfilerCallContext *context)
{
    enum scene_argument_kind kind = scene_argument_kind(method);
    char scene[160];
    int ok, is_null, index;
    const char *reason;

    if (kind == SCENE_ARGUMENT_STRING)
    {
        if (read_mono_string_argument(context, 0, scene, sizeof(scene), &ok, &is_null, &reason) &&
            ok && scene[0])
        {
            builder_append(builder, scene);
            return;
        }
        builder_append(builder, "unavailable");
        return;
    }
    if (kind == SCENE_ARGUMENT_INT32)
    {
        if (read_int32_argument(context, 0, &index))
        {
            builder_append(builder, "index:");
            builder_append_signed(builder, index);
            return;
        }
        builder_append(builder, "unavailable");
        return;
    }
    builder_append(builder, "unavailable");
}

static void log_scene_load(enum observed_method observed, MonoMethod *method,
                           MonoProfilerCallContext *context, const char *result)
{
    char detail[512];
    struct text_builder builder;

    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "class=SceneManager method=");
    builder_append(&builder, observed == METHOD_SCENE_LOAD ? "LoadScene" : "LoadSceneAsync");
    builder_append(&builder, " mode=");
    builder_append(&builder, observed == METHOD_SCENE_LOAD ? "sync" : "async");
    builder_append(&builder, " scene=");
    append_scene_argument(&builder, method, context);
    builder_append(&builder, " result=");
    builder_append(&builder, result);
    builder_append(&builder, " allowSceneActivation=");
    builder_append_signed(&builder, InterlockedCompareExchange(&allow_scene_activation, 0, 0));
    builder_append(&builder, " uiManagerSeen=");
    builder_append_signed(&builder, InterlockedCompareExchange(&ui_manager_awake, 0, 0) ? 1 : 0);
    observer_log("scene-load", detail);
}

static void log_manager_awake(enum observed_method observed)
{
    const char *class_name;
    LONG *seen;
    char detail[256];
    struct text_builder builder;

    if (observed == METHOD_GAME_MANAGER_AWAKE)
    {
        class_name = "GameManager";
        seen = &game_manager_awake;
    }
    else if (observed == METHOD_GAME_CAMERAS_AWAKE)
    {
        class_name = "GameCameras";
        seen = &game_cameras_awake;
    }
    else
    {
        class_name = "UIManager";
        seen = &ui_manager_awake;
    }
    if (InterlockedCompareExchange(seen, 1, 0)) return;
    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "class=");
    builder_append(&builder, class_name);
    builder_append(&builder, " method=Awake allowSceneActivation=");
    builder_append_signed(&builder, InterlockedCompareExchange(&allow_scene_activation, 0, 0));
    builder_append(&builder, " level1_membership=sealed");
    observer_log("manager-created", detail);
    if (InterlockedCompareExchange(&allow_scene_activation, 0, 0) == 1)
    {
        builder_init(&builder, detail, sizeof(detail));
        builder_append(&builder, "scene=Menu_Title evidence=");
        builder_append(&builder, class_name);
        builder_append(&builder, ".Awake+sealed-level1-membership");
        observer_log("menu-title-activated", detail);
    }
}

static void invoke_oneshot_sequence(void)
{
    MonoImage *image, *unity_image;
    MonoClass *klass, *object_class, *unity_object_class;
    MonoType *type;
    MonoReflectionType *reflection_type;
    MonoDomain *domain;
    MonoMethod *find_method, *set_method, *confirm_method;
    MonoArray *found;
    MonoObject *instance, *exception;
    MonoString *language;
    MonoObject **element_storage;
    MonoGCHandle handle;
    HWND fg;
    DWORD fg_pid = 0, fg_tid = 0, tid;
    char fg_class[64] = {0};
    void *invoke_args[1];
    void *set_args[1];
    int exact, count_ok;
    struct text_builder builder;
    char detail[384];

    /* Commit the one-shot state before nested managed calls can reenter the profiler. */
    if (!oneshot_sequence ||
        InterlockedCompareExchange(&oneshot_sequence_attempted, 1, 0))
        return;

    tid = GetCurrentThreadId();
    fg = GetForegroundWindow();
    if (fg)
    {
        fg_tid = GetWindowThreadProcessId(fg, &fg_pid);
        GetClassNameA(fg, fg_class, (int)sizeof(fg_class));
    }
    exact = fg && fg_tid == tid && fg_pid == (DWORD)GetCurrentProcessId() &&
            strings_equal(fg_class, "UnityWndClass");
    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "wine_tid=");
    builder_append_unsigned(&builder, tid);
    builder_append(&builder, " window_tid=");
    builder_append_unsigned(&builder, fg_tid);
    builder_append(&builder, " window_pid=");
    builder_append_unsigned(&builder, fg_pid);
    builder_append(&builder, " foreground=");
    builder_append_unsigned(&builder, (ULONG_PTR)fg);
    builder_append(&builder, " class=");
    builder_append(&builder, fg_class);
    builder_append(&builder, " native_tid_source=join-win32u-by-wine_tid exact=");
    builder_append(&builder, exact ? "1" : "0");
    observer_log("oneshot-thread-identity", detail);
    if (!exact) return;

    /* Match the accepted 145129 actuator: late lookup, no allocation profiler. */
    image = mono.image_loaded("Assembly-CSharp");
    unity_image = mono.image_loaded("UnityEngine");
    klass = image ? mono.class_from_name(image, "", "StartManager") : NULL;
    domain = mono.get_root_domain();
    type = klass ? mono.class_get_type(klass) : NULL;
    reflection_type = type && domain ? mono.type_get_object(domain, type) : NULL;
    unity_object_class = unity_image
        ? mono.class_from_name(unity_image, "UnityEngine", "Object") : NULL;
    find_method = unity_object_class
        ? mono.class_get_method_from_name(unity_object_class, "FindObjectsOfType", 1)
        : NULL;
    if (!reflection_type || !find_method)
    {
        builder_init(&builder, detail, sizeof(detail));
        builder_append(&builder, "image=");
        builder_append_unsigned(&builder, (ULONG_PTR)image);
        builder_append(&builder, " unity_image=");
        builder_append_unsigned(&builder, (ULONG_PTR)unity_image);
        builder_append(&builder, " class=");
        builder_append_unsigned(&builder, (ULONG_PTR)klass);
        builder_append(&builder, " type=");
        builder_append_unsigned(&builder, (ULONG_PTR)type);
        builder_append(&builder, " reflection_type=");
        builder_append_unsigned(&builder, (ULONG_PTR)reflection_type);
        builder_append(&builder, " find_method=");
        builder_append_unsigned(&builder, (ULONG_PTR)find_method);
        builder_append(&builder, " status=lookup-resolution-failed");
        observer_log("oneshot-lookup", detail);
        return;
    }

    exception = NULL;
    invoke_args[0] = reflection_type;
    found = (MonoArray *)mono.runtime_invoke(find_method, NULL, invoke_args, &exception);
    instance = NULL;
    count_ok = 0;
    if (!exception && found && mono.array_length(found) == 1)
    {
        element_storage = (MonoObject **)mono.array_addr_with_size(
            found, (int)sizeof(MonoObject *), (uintptr_t)0);
        instance = element_storage ? *element_storage : NULL;
        if (instance && (object_class = mono.object_get_class(instance)) &&
            strings_equal(mono.class_get_name(object_class), "StartManager"))
            count_ok = 1;
        else
            instance = NULL;
    }
    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "exception=");
    builder_append_unsigned(&builder, (ULONG_PTR)exception);
    builder_append(&builder, " array=");
    builder_append_unsigned(&builder, (ULONG_PTR)found);
    builder_append(&builder, " count=");
    builder_append_signed(&builder,
                          (!exception && found) ? (LONG)mono.array_length(found) : -1);
    builder_append(&builder, " exact_one=");
    builder_append(&builder, count_ok ? "1" : "0");
    observer_log("oneshot-lookup", detail);
    if (!count_ok) return;

    set_method = mono.class_get_method_from_name(object_class, "SetLanguage", 1);
    confirm_method = mono.class_get_method_from_name(object_class, "ConfirmLanguage", 0);
    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "classification=NOT_GOLDEN phase=resolve set_method=");
    builder_append_unsigned(&builder, (ULONG_PTR)set_method);
    builder_append(&builder, " set_param_count=1 confirm_method=");
    builder_append_unsigned(&builder, (ULONG_PTR)confirm_method);
    builder_append(&builder, " confirm_param_count=0 language_code=EN status=");
    builder_append(&builder,
                   !set_method && !confirm_method ? "missing-both-methods" :
                   !set_method ? "missing-set-method" :
                   !confirm_method ? "missing-confirm-method" : "methods-ok");
    observer_log("oneshot-sequence", detail);
    if (!set_method || !confirm_method) return;

    handle = mono.gchandle_new(instance, 1);
    if (!handle)
    {
        observer_log("oneshot-sequence",
                     "classification=NOT_GOLDEN phase=invoke status=gchandle-failed");
        return;
    }
    retained_startmanager_handle = handle;
    InterlockedExchangePointer((void *volatile *)&retained_startmanager_object, instance);
    log_allow_scene_activation_snapshot("before-language-sequence");

    language = mono.string_new(domain, "EN");
    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "classification=NOT_GOLDEN phase=argument method=SetLanguage");
    builder_append(&builder, " language_code=EN string=");
    builder_append_unsigned(&builder, (ULONG_PTR)language);
    builder_append(&builder, language ? " status=ok" : " status=string-null");
    observer_log("oneshot-sequence", detail);
    if (!language) return;

    set_args[0] = language;
    exception = NULL;
    observer_log("oneshot-sequence",
                 "classification=NOT_GOLDEN phase=invoke status=begin method=SetLanguage language_code=EN argument_count=1");
    mono.runtime_invoke(set_method, instance, set_args, &exception);
    observer_log("oneshot-sequence",
                 exception
                     ? "classification=NOT_GOLDEN phase=return status=managed-exception method=SetLanguage language_code=EN"
                     : "classification=NOT_GOLDEN phase=return status=ok method=SetLanguage language_code=EN");
    if (!exception)
    {
        exception = NULL;
        observer_log("oneshot-sequence",
                     "classification=NOT_GOLDEN phase=invoke status=begin method=ConfirmLanguage argument_count=0");
        mono.runtime_invoke(confirm_method, instance, NULL, &exception);
        observer_log("oneshot-sequence",
                     exception
                         ? "classification=NOT_GOLDEN phase=return status=managed-exception method=ConfirmLanguage"
                         : "classification=NOT_GOLDEN phase=return status=ok method=ConfirmLanguage");
        builder_init(&builder, detail, sizeof(detail));
        builder_append(&builder, "classification=NOT_GOLDEN lifetime=process retained_handle=");
        builder_append_unsigned(&builder, (ULONG_PTR)retained_startmanager_handle);
        builder_append(&builder, " cleanup=deferred-to-process-exit");
        observer_log("oneshot-sequence", detail);
    }
}

static void gc_allocation(MonoProfiler *profiler, MonoObject *object)
{
    MonoClass *klass;
    MonoMethod *method;
    MonoGCHandle handle;
    const char *class_name;
    (void)profiler;

    if (!diagnostic_direct_confirm ||
        InterlockedCompareExchange(&diagnostic_direct_confirm_capture_started, 0, 0) ||
        !object || !(klass = mono.object_get_class(object)) ||
        !(class_name = mono.class_get_name(klass)) ||
        !strings_equal(class_name, "StartManager") ||
        InterlockedCompareExchange(&diagnostic_direct_confirm_capture_started, 1, 0)) return;
    method = mono.class_get_method_from_name(klass, "ConfirmLanguage", 0);
    if (!method)
    {
        observer_log("diagnostic-direct-confirm",
                     "classification=NOT_GOLDEN phase=capture status=missing-method");
        if (observer_handle) mono.set_gc_allocation(observer_handle, NULL);
        return;
    }
    handle = mono.gchandle_new(object, 1);
    if (!handle)
    {
        observer_log("diagnostic-direct-confirm",
                     "classification=NOT_GOLDEN phase=capture status=gchandle-failed");
        if (observer_handle) mono.set_gc_allocation(observer_handle, NULL);
        return;
    }
    InterlockedExchangePointer((void *volatile *)&start_manager_object, object);
    InterlockedExchangePointer((void *volatile *)&confirm_language_method, method);
    InterlockedExchange(&start_manager_handle, (LONG)handle);
    observer_log("diagnostic-direct-confirm",
                 "classification=NOT_GOLDEN phase=capture status=ready class=StartManager");
    if (observer_handle) mono.set_gc_allocation(observer_handle, NULL);
}

static void invoke_diagnostic_direct_confirm(void)
{
    MonoMethod *method;
    MonoObject *object;
    MonoObject *exception = NULL;

    if (!diagnostic_direct_confirm ||
        InterlockedCompareExchange(&diagnostic_direct_confirm_attempted, 1, 0))
        return;
    object = InterlockedCompareExchangePointer((void *volatile *)&start_manager_object,
                                               NULL, NULL);
    method = InterlockedCompareExchangePointer((void *volatile *)&confirm_language_method,
                                               NULL, NULL);
    if (!object || !method)
    {
        observer_log("diagnostic-direct-confirm",
                     "classification=NOT_GOLDEN phase=invoke status=missing-start-manager");
        return;
    }
    observer_log("diagnostic-direct-confirm",
                 "classification=NOT_GOLDEN phase=invoke status=begin method=ConfirmLanguage");
    mono.runtime_invoke(method, object, NULL, &exception);
    observer_log("diagnostic-direct-confirm",
                 exception
                     ? "classification=NOT_GOLDEN phase=return status=managed-exception method=ConfirmLanguage"
	                     : "classification=NOT_GOLDEN phase=return status=ok method=ConfirmLanguage");
}

/* Press "Start Game" the same way the language actuator picks a language.
 *
 * Mirrors invoke_oneshot_sequence() deliberately -- same thread-identity gate, same
 * FindObjectsOfType resolution, same exact_one requirement, same runtime_invoke -- so
 * that the one PROVEN actuator route in this tool is reused rather than reinvented.
 * StartNewGame touches InputHandler::StopUIInput, so running it off the Unity UI thread
 * would be meaningless; the identity gate is load-bearing, not decoration. */
static void builder_append_hex32(struct text_builder *builder, ULONG value);

static void builder_append_hex64(struct text_builder *builder, ULONGLONG value)
{
    builder_append_hex32(builder, (ULONG)(value >> 32));
    builder_append_hex32(builder, (ULONG)value);
}

/* Two readers of the SAME dword, deliberately at different access widths.
 *
 * The failing instruction is a 4-byte `movsxd rdx, dword [rax+0x1c]`. If a wide
 * read and a byte-wise reassembly of the same address disagree, that is itself a
 * translator memory-coherence defect of the first order -- exactly the class this
 * tree already proved once (the m33 narrow-store -> overlapping-wide-load hop).
 * Costs nothing to check, so check it. */
static ULONG dword_at_wide(const void *base, unsigned off)
{
    if (!base) return 0;
    return *(const volatile ULONG *)(const void *)((const unsigned char *)base + off);
}

static ULONG dword_at_bytes(const void *base, unsigned off)
{
    const volatile unsigned char *p;

    if (!base) return 0;
    p = (const volatile unsigned char *)base + off;
    return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

static ULONGLONG qword_at(const void *base, unsigned off)
{
    if (!base) return 0;
    return *(const volatile ULONGLONG *)(const void *)((const unsigned char *)base + off);
}

/* MonoVTable layout is not guessed -- it is read off mono_gc_alloc_obj (rva
 * 0x264720) in the shipped DLL, which does `mov rax,[rcx]` for vtable->klass and
 * `cmp qword [rcx+8],0` for vtable->gc_descr. MonoClass+0x1c is likewise read off
 * the caller's `movsxd rdx,dword [rax+0x1c]`, and +0x28 off its
 * `test dword [rax+0x28],0x800` (has_references). */
#define MONO_VTABLE_KLASS_OFF     0x00
#define MONO_VTABLE_GC_DESCR_OFF  0x08
#define MONO_CLASS_INSTANCE_SIZE_OFF 0x1c
#define MONO_CLASS_FLAGS_OFF      0x28

/* Runs immediately before the UIStartNewGame invoke, on the same thread.
 * Returns nothing: every result is logged, including the refusals. */
static void run_newobj_probe(MonoClass *uimanager_class)
{
    MonoImage *corlib;
    MonoClass *object_class, *nested, *target;
    MonoDomain *domain;
    MonoVTable *vt, *control_vt;
    MonoObject *control_obj, *obj_specific, *obj_new;
    void *iter;
    const char *name;
    int32_t control_size, api_size;
    ULONG raw_before_wide, raw_before_bytes, raw_after_wide, raw_after_bytes, flags;
    ULONGLONG vt_klass, vt_gc_descr;
    int nested_seen, control_ok, i;
    struct text_builder builder;
    char detail[768];

    if (!newobj_probe) return;
    InterlockedIncrement(&newobj_probe_ran);

    /* Refuse loudly rather than crash the actuator. `not logged != did not happen`
     * cuts both ways: an absent probe line must be distinguishable from a probe
     * that ran and found nothing. */
    if (!mono.image_loaded || !mono.class_from_name || !mono.get_root_domain ||
        !mono.class_instance_size || !mono.class_get_nested_types ||
        !mono.class_vtable || !mono.object_new_specific || !mono.object_new ||
        !uimanager_class)
    {
        builder_init(&builder, detail, sizeof(detail));
        builder_append(&builder, "classification=NOT_GOLDEN phase=preflight status=api-unavailable"
                                 " instance_size=");
        builder_append_signed(&builder, mono.class_instance_size ? 1 : 0);
        builder_append(&builder, " nested_types=");
        builder_append_signed(&builder, mono.class_get_nested_types ? 1 : 0);
        builder_append(&builder, " vtable=");
        builder_append_signed(&builder, mono.class_vtable ? 1 : 0);
        builder_append(&builder, " new_specific=");
        builder_append_signed(&builder, mono.object_new_specific ? 1 : 0);
        builder_append(&builder, " object_new=");
        builder_append_signed(&builder, mono.object_new ? 1 : 0);
        builder_append(&builder, " uimanager_class=");
        builder_append_signed(&builder, uimanager_class ? 1 : 0);
        observer_log("newobj-probe", detail);
        return;
    }

    domain = mono.get_root_domain();

    /* ---- CONTROL, with an answer known IN ADVANCE ------------------------
     * System.Object is the empty reference type: on 64-bit its instance_size is
     * exactly the MonoObject header, vtable + synchronisation = 16 bytes. And
     * allocating one must succeed -- this boot has already allocated 47240
     * objects. If either of those is wrong, the probe's own call path is broken
     * and NOTHING below it may be believed. A control that cannot fail is not a
     * control; this one can fail in both directions. */
    corlib = mono.image_loaded("mscorlib");
    object_class = corlib ? mono.class_from_name(corlib, "System", "Object") : NULL;
    control_size = object_class ? mono.class_instance_size(object_class) : -1;
    control_vt = (object_class && domain) ? mono.class_vtable(domain, object_class) : NULL;
    control_obj = control_vt ? mono.object_new_specific(control_vt) : NULL;
    control_ok = (control_size == 16) && (control_obj != NULL);
    if (control_ok) InterlockedIncrement(&newobj_probe_control_ok);

    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "classification=NOT_GOLDEN phase=control target=System.Object"
                             " instance_size=");
    builder_append_signed(&builder, control_size);
    builder_append(&builder, " expected=16 vtable=0x");
    builder_append_hex64(&builder, (ULONGLONG)(ULONG_PTR)control_vt);
    builder_append(&builder, " alloc=0x");
    builder_append_hex64(&builder, (ULONGLONG)(ULONG_PTR)control_obj);
    builder_append(&builder, control_ok ? " result=PASS" :
                   " result=FAILED -> DO-NOT-TRUST-ANY-NEWOBJ-NUMBER-BELOW");
    observer_log("newobj-probe", detail);

    /* ---- locate the exact class that fails -------------------------------
     * `<>c__DisplayClass170_0` is nested in UIManager -- verified offline
     * against the shipped Assembly-CSharp.dll (TypeDef row 3036, NestedClass ->
     * row 480 UIManager) with exactly 3 fields: <>4__this, permaDeath, bossRush.
     * That field set is what makes the size PREDICTABLE: a 16-byte header plus a
     * pointer plus two bools = 0x1a, rounded to 0x18..0x20. */
    target = NULL;
    nested_seen = 0;
    iter = NULL;
    while ((nested = mono.class_get_nested_types(uimanager_class, &iter)) != NULL)
    {
        nested_seen++;
        name = mono.class_get_name(nested);
        if (name && strings_equal(name, "<>c__DisplayClass170_0"))
        {
            target = nested;
            break;
        }
    }
    if (!target)
    {
        builder_init(&builder, detail, sizeof(detail));
        builder_append(&builder, "classification=NOT_GOLDEN phase=resolve"
                                 " status=nested-class-not-found nested_seen=");
        builder_append_signed(&builder, nested_seen);
        observer_log("newobj-probe", detail);
        return;
    }

    /* Raw reads BEFORE any API call that could lazily initialise the class --
     * mono_class_instance_size() calls mono_class_init() when size_inited is 0,
     * so reading after it would hide an uninitialised-metadata story. */
    raw_before_wide = dword_at_wide(target, MONO_CLASS_INSTANCE_SIZE_OFF);
    raw_before_bytes = dword_at_bytes(target, MONO_CLASS_INSTANCE_SIZE_OFF);

    api_size = mono.class_instance_size(target);

    raw_after_wide = dword_at_wide(target, MONO_CLASS_INSTANCE_SIZE_OFF);
    raw_after_bytes = dword_at_bytes(target, MONO_CLASS_INSTANCE_SIZE_OFF);
    flags = dword_at_wide(target, MONO_CLASS_FLAGS_OFF);

    vt = domain ? mono.class_vtable(domain, target) : NULL;
    vt_klass = qword_at(vt, MONO_VTABLE_KLASS_OFF);
    vt_gc_descr = qword_at(vt, MONO_VTABLE_GC_DESCR_OFF);

    /* THE MINIMAL REPRODUCER: allocate the exact class that fails, through the
     * exact entry point Mono's newobj wrapper uses (mono_object_new_specific ->
     * mono_object_new_specific_checked -> mono_gc_alloc_obj). If this returns
     * NULL, the defect is reproducible without UIStartNewGame at all. */
    obj_specific = vt ? mono.object_new_specific(vt) : NULL;
    obj_new = domain ? mono.object_new(domain, target) : NULL;

    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "classification=NOT_GOLDEN phase=subject"
                             " target=UIManager/<>c__DisplayClass170_0 predicted_size=0x18..0x20"
                             " klass=0x");
    builder_append_hex64(&builder, (ULONGLONG)(ULONG_PTR)target);
    builder_append(&builder, " raw_before_wide=0x");
    builder_append_hex32(&builder, raw_before_wide);
    builder_append(&builder, " raw_before_bytes=0x");
    builder_append_hex32(&builder, raw_before_bytes);
    builder_append(&builder, " api_instance_size=");
    builder_append_signed(&builder, api_size);
    builder_append(&builder, " raw_after_wide=0x");
    builder_append_hex32(&builder, raw_after_wide);
    builder_append(&builder, " raw_after_bytes=0x");
    builder_append_hex32(&builder, raw_after_bytes);
    builder_append(&builder, " width_agree=");
    builder_append(&builder, (raw_after_wide == raw_after_bytes) ? "yes" : "NO");
    builder_append(&builder, " flags=0x");
    builder_append_hex32(&builder, flags);
    builder_append(&builder, " has_references=");
    builder_append(&builder, (flags & 0x800u) ? "1" : "0");
    observer_log("newobj-probe", detail);

    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "classification=NOT_GOLDEN phase=alloc vtable=0x");
    builder_append_hex64(&builder, (ULONGLONG)(ULONG_PTR)vt);
    builder_append(&builder, " vt_klass=0x");
    builder_append_hex64(&builder, vt_klass);
    builder_append(&builder, " vt_klass_matches=");
    builder_append(&builder, (vt_klass == (ULONGLONG)(ULONG_PTR)target) ? "yes" : "NO");
    builder_append(&builder, " vt_gc_descr=0x");
    builder_append_hex64(&builder, vt_gc_descr);
    builder_append(&builder, " object_new_specific=0x");
    builder_append_hex64(&builder, (ULONGLONG)(ULONG_PTR)obj_specific);
    builder_append(&builder, " object_new=0x");
    builder_append_hex64(&builder, (ULONGLONG)(ULONG_PTR)obj_new);
    builder_append(&builder, " reproduced=");
    builder_append(&builder, (!obj_specific || !obj_new) ? "YES" : "no");
    builder_append(&builder, " control=");
    builder_append(&builder, control_ok ? "PASS" : "FAILED");
    observer_log("newobj-probe", detail);

    /* klass+0x00..0x3c as 16 dwords. Section 87 recorded the cost of not doing
     * this: run23 captured only the vtable, so [klass+0x1c] could never be read
     * after the fact and both processes were gone. */
    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "classification=NOT_GOLDEN phase=klass-dump base=0x");
    builder_append_hex64(&builder, (ULONGLONG)(ULONG_PTR)target);
    builder_append(&builder, " dwords=");
    for (i = 0; i < 16; i++)
    {
        if (i) builder_append(&builder, ",");
        builder_append_hex32(&builder, dword_at_wide(target, (unsigned)(i * 4)));
    }
    observer_log("newobj-probe", detail);
}

static void invoke_start_game_actuator(void)
{
    MonoImage *image, *unity_image;
    MonoClass *klass, *unity_object_class, *object_class;
    MonoDomain *domain;
    MonoType *type;
    MonoReflectionType *reflection_type;
    MonoMethod *find_method, *start_method;
    MonoArray *found;
    MonoObject *instance, *exception;
    MonoObject **element_storage;
    HWND fg;
    DWORD fg_pid = 0, fg_tid = 0, tid;
    char fg_class[64] = {0};
    void *invoke_args[1];
    int exact, count, count_ok, attempt;
    struct text_builder builder;
    char detail[384];

    if (!start_game_actuator ||
        InterlockedCompareExchange(&start_game_latched, 0, 0)) return;

    attempt = (int)InterlockedIncrement(&start_game_attempts);
    if (attempt > start_game_budget)
    {
        if (attempt == start_game_budget + 1)
            observer_log("start-game", "classification=NOT_GOLDEN phase=gate status=budget-exhausted");
        return;
    }

    /* Must be the Unity UI thread with the foreground window, exactly as the language
     * actuator requires -- the accepted 145129 actuator shape. */
    tid = GetCurrentThreadId();
    fg = GetForegroundWindow();
    if (fg)
    {
        fg_tid = GetWindowThreadProcessId(fg, &fg_pid);
        GetClassNameA(fg, fg_class, (int)sizeof(fg_class));
    }
    exact = fg && fg_tid == tid && fg_pid == (DWORD)GetCurrentProcessId() &&
            strings_equal(fg_class, "UnityWndClass");
    if (!exact)
    {
        builder_init(&builder, detail, sizeof(detail));
        builder_append(&builder, "classification=NOT_GOLDEN phase=gate status=not-ui-thread attempt=");
        builder_append_signed(&builder, attempt);
        builder_append(&builder, " wine_tid=");
        builder_append_unsigned(&builder, tid);
        builder_append(&builder, " window_tid=");
        builder_append_unsigned(&builder, fg_tid);
        observer_log("start-game", detail);
        return;
    }

    image = mono.image_loaded("Assembly-CSharp");
    unity_image = mono.image_loaded("UnityEngine");
    klass = image ? mono.class_from_name(image, "", "UIManager") : NULL;
    domain = mono.get_root_domain();
    type = klass ? mono.class_get_type(klass) : NULL;
    reflection_type = type && domain ? mono.type_get_object(domain, type) : NULL;
    unity_object_class = unity_image
        ? mono.class_from_name(unity_image, "UnityEngine", "Object") : NULL;
    find_method = unity_object_class
        ? mono.class_get_method_from_name(unity_object_class, "FindObjectsOfType", 1)
        : NULL;
    if (!reflection_type || !find_method)
    {
        builder_init(&builder, detail, sizeof(detail));
        builder_append(&builder, "classification=NOT_GOLDEN phase=resolve status=type-unresolved attempt=");
        builder_append_signed(&builder, attempt);
        builder_append(&builder, " class=");
        builder_append_unsigned(&builder, (ULONG_PTR)klass);
        builder_append(&builder, " find_method=");
        builder_append_unsigned(&builder, (ULONG_PTR)find_method);
        observer_log("start-game", detail);
        return;
    }

    exception = NULL;
    invoke_args[0] = reflection_type;
    found = (MonoArray *)mono.runtime_invoke(find_method, NULL, invoke_args, &exception);
    instance = NULL;
    count = (!exception && found) ? (int)mono.array_length(found) : -1;
    count_ok = 0;
    if (count == 1)
    {
        element_storage = (MonoObject **)mono.array_addr_with_size(
            found, (int)sizeof(MonoObject *), (uintptr_t)0);
        instance = element_storage ? *element_storage : NULL;
        if (instance && (object_class = mono.object_get_class(instance)) &&
            strings_equal(mono.class_get_name(object_class), "UIManager"))
            count_ok = 1;
        else
            instance = NULL;
    }
    if (!count_ok)
    {
        /* Expected before Menu_Title: UIManager lives there, and run20 shows the first two
         * HighlightDefault triggers (seq=8, seq=37) both precede UIManager::Awake (seq=67).
         * Not an error, NOT a reason to latch -- and it must NOT consume the invoke budget,
         * or a few early triggers would exhaust it before the target ever exists. Refund it.
         * The log is rate-limited but the counter is aggregate, so a zero stays a real zero. */
        LONG misses = InterlockedIncrement(&start_game_not_found);
        InterlockedDecrement(&start_game_attempts);
        if (misses <= 8)
        {
            builder_init(&builder, detail, sizeof(detail));
            builder_append(&builder, "classification=NOT_GOLDEN phase=lookup status=no-uimanager-yet miss=");
            builder_append_signed(&builder, misses);
            builder_append(&builder, " count=");
            builder_append_signed(&builder, count);
            observer_log("start-game", detail);
        }
        return;
    }

    start_method = mono.class_get_method_from_name(object_class, "UIStartNewGame", 0);
    if (!start_method)
    {
        observer_log("start-game",
                     "classification=NOT_GOLDEN phase=resolve status=missing-UIStartNewGame arity=0");
        return;
    }

    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "classification=NOT_GOLDEN phase=invoke status=begin method=UIStartNewGame arity=0 attempt=");
    builder_append_signed(&builder, attempt);
    builder_append(&builder, " instance=");
    builder_append_unsigned(&builder, (ULONG_PTR)instance);
    observer_log("start-game", detail);

    /* Measured HERE deliberately: same thread, same moment in the boot, after the
     * identity gate has already passed -- i.e. the state the failing allocation
     * actually sees. Placing it earlier would measure a different process. */
    run_newobj_probe(object_class);

    exception = NULL;
    mono.runtime_invoke(start_method, instance, NULL, &exception);
    if (exception)
    {
        InterlockedIncrement(&start_game_exceptions);
        builder_init(&builder, detail, sizeof(detail));
        builder_append(&builder, "classification=NOT_GOLDEN phase=return status=managed-exception method=UIStartNewGame attempt=");
        builder_append_signed(&builder, attempt);
        observer_log("start-game", detail);
        return; /* retry: the menu may not have finished wiring ih/gs yet */
    }

    InterlockedExchange(&start_game_latched, 1);
    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "classification=NOT_GOLDEN phase=return status=ok method=UIStartNewGame attempt=");
    builder_append_signed(&builder, attempt);
    builder_append(&builder, " attempts_total=");
    builder_append_signed(&builder, (LONG)InterlockedCompareExchange(&start_game_attempts, 0, 0));
    builder_append(&builder, " not_found=");
    builder_append_signed(&builder, (LONG)InterlockedCompareExchange(&start_game_not_found, 0, 0));
    builder_append(&builder, " exceptions=");
    builder_append_signed(&builder, (LONG)InterlockedCompareExchange(&start_game_exceptions, 0, 0));
    observer_log("start-game", detail);
}

static void log_post_level_milestone(enum observed_method observed, const char *phase)
{
    const char *class_name, *method_name;
    unsigned int arity;
    char detail[256];
    struct text_builder builder;

    if (!post_level_method_details(observed, &class_name, &method_name, &arity)) return;
    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "phase=");
    builder_append(&builder, phase);
    builder_append(&builder, " class=");
    builder_append(&builder, class_name);
    builder_append(&builder, " method=");
    builder_append(&builder, method_name);
    builder_append(&builder, " arity=");
    builder_append_unsigned(&builder, arity);
    builder_append(&builder, " managed_tid=");
    builder_append_unsigned(&builder, GetCurrentThreadId());
    builder_append(&builder, " sequence=");
    builder_append_signed(&builder, InterlockedIncrement(&post_level_sequence));
    observer_log("post-level-milestone", detail);
}

/* ------------------------------------------------------------------ *
 * MACRUNNER_HB_CAMERA_PROBE
 *
 * Every float is printed as its uint32 bit pattern, never as a decimal:
 * the payload is the discriminator we actually need (0xffc00000 = x86 QNaN
 * indefinite, i.e. a genuine invalid operation, vs 0x7fc00000 = host libm
 * default NaN, i.e. our translator's fingerprint).
 *
 * A control block runs FIRST and has a known-in-advance answer:
 * UnityEngine.Matrix4x4::get_identity must come back through the exact same
 * invoke -> unbox -> hex path as 3f800000 on the diagonal and 00000000
 * everywhere else. If the control does not print that, no camera number
 * below may be believed.
 * ------------------------------------------------------------------ */

static void builder_append_hex32(struct text_builder *builder, ULONG value)
{
    static const char digits[] = "0123456789abcdef";
    char out[8];
    int i;

    for (i = 7; i >= 0; i--)
    {
        out[i] = digits[value & 0xfu];
        value >>= 4;
    }
    for (i = 0; i < 8; i++)
        if (builder->length + 1 < builder->capacity) builder->buffer[builder->length++] = out[i];
    if (builder->capacity) builder->buffer[builder->length] = 0;
}

static ULONG float_bits_at(const void *base, unsigned index)
{
    ULONG value = 0;

    copy_bytes(&value, (const char *)base + index * 4u, sizeof(value));
    return value;
}

static void builder_append_float_vector(struct text_builder *builder, const void *base,
                                        unsigned count)
{
    unsigned i;

    for (i = 0; i < count; i++)
    {
        if (i) builder_append(builder, ",");
        builder_append_hex32(builder, float_bits_at(base, i));
    }
}

/* Invoke a 0-arg getter and unbox the value-type result. NULL on any failure. */
static void *camera_probe_invoke_value(MonoMethod *method, void *instance)
{
    MonoObject *exception = NULL, *result;

    if (!method) return NULL;
    result = mono.runtime_invoke(method, instance, NULL, &exception);
    if (exception || !result) return NULL;
    return mono.object_unbox(result);
}

static MonoObject *camera_probe_invoke_object(MonoMethod *method, void *instance)
{
    MonoObject *exception = NULL, *result;

    if (!method) return NULL;
    result = mono.runtime_invoke(method, instance, NULL, &exception);
    if (exception) return NULL;
    return result;
}

static MonoClass *camera_probe_unity_class(const char *name_space, const char *name)
{
    static const char *const images[] = {"UnityEngine.CoreModule", "UnityEngine"};
    MonoImage *image;
    MonoClass *klass;
    unsigned i;

    for (i = 0; i < sizeof(images) / sizeof(images[0]); i++)
    {
        image = mono.image_loaded(images[i]);
        if (!image) continue;
        klass = mono.class_from_name(image, name_space, name);
        if (klass) return klass;
    }
    return NULL;
}

/* Control with a known-in-advance result — runs before any camera is read. */
static void camera_probe_run_control(void)
{
    MonoClass *matrix_class;
    MonoMethod *identity_getter;
    void *raw;
    char detail[512];
    struct text_builder builder;

    if (InterlockedCompareExchange(&camera_probe_control_done, 1, 0)) return;

    matrix_class = camera_probe_unity_class("UnityEngine", "Matrix4x4");
    identity_getter =
        matrix_class ? mono.class_get_method_from_name(matrix_class, "get_identity", 0) : NULL;
    raw = camera_probe_invoke_value(identity_getter, NULL);

    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "control=Matrix4x4.identity class=");
    builder_append_unsigned(&builder, (ULONG_PTR)matrix_class);
    builder_append(&builder, " getter=");
    builder_append_unsigned(&builder, (ULONG_PTR)identity_getter);
    builder_append(&builder, " expect=3f800000-on-diagonal-00000000-elsewhere status=");
    builder_append(&builder, raw ? "read" : "UNREADABLE-DO-NOT-TRUST-CAMERA-DATA");
    if (raw)
    {
        builder_append(&builder, " m=");
        builder_append_float_vector(&builder, raw, 16);
    }
    observer_log("camera-probe-control", detail);
}

static void camera_probe_dump_one(MonoObject *camera, MonoClass *camera_class,
                                  MonoClass *object_class, MonoClass *behaviour_class,
                                  MonoClass *component_class, MonoClass *transform_class,
                                  const char *phase, int index)
{
    MonoMethod *getter;
    MonoObject *name_object, *transform;
    void *raw;
    char *utf8;
    char detail[640];
    struct text_builder builder;

    /* identity + configuration */
    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "phase=");
    builder_append(&builder, phase);
    builder_append(&builder, " camera=");
    builder_append_signed(&builder, index);
    builder_append(&builder, " object=");
    builder_append_unsigned(&builder, (ULONG_PTR)camera);
    builder_append(&builder, " name=");
    getter = object_class ? mono.class_get_method_from_name(object_class, "get_name", 0) : NULL;
    name_object = camera_probe_invoke_object(getter, camera);
    utf8 = (name_object && mono.string_to_utf8) ? mono.string_to_utf8((MonoString *)name_object)
                                                : NULL;
    builder_append(&builder, utf8 ? utf8 : "?");
    if (utf8 && mono.mono_free) mono.mono_free(utf8);
    getter = behaviour_class ? mono.class_get_method_from_name(behaviour_class, "get_enabled", 0)
                             : NULL;
    raw = camera_probe_invoke_value(getter, camera);
    builder_append(&builder, " enabled=");
    builder_append_signed(&builder, raw ? *(const unsigned char *)raw : -1);
    getter = mono.class_get_method_from_name(camera_class, "get_orthographic", 0);
    raw = camera_probe_invoke_value(getter, camera);
    builder_append(&builder, " orthographic=");
    builder_append_signed(&builder, raw ? *(const unsigned char *)raw : -1);
    observer_log("camera-probe", detail);

    /* scalar configuration, as bit patterns */
    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "camera=");
    builder_append_signed(&builder, index);
    builder_append(&builder, " orthographicSize=");
    raw = camera_probe_invoke_value(
        mono.class_get_method_from_name(camera_class, "get_orthographicSize", 0), camera);
    if (raw) builder_append_hex32(&builder, float_bits_at(raw, 0));
    else builder_append(&builder, "unreadable");
    builder_append(&builder, " fieldOfView=");
    raw = camera_probe_invoke_value(
        mono.class_get_method_from_name(camera_class, "get_fieldOfView", 0), camera);
    if (raw) builder_append_hex32(&builder, float_bits_at(raw, 0));
    else builder_append(&builder, "unreadable");
    builder_append(&builder, " near=");
    raw = camera_probe_invoke_value(
        mono.class_get_method_from_name(camera_class, "get_nearClipPlane", 0), camera);
    if (raw) builder_append_hex32(&builder, float_bits_at(raw, 0));
    else builder_append(&builder, "unreadable");
    builder_append(&builder, " far=");
    raw = camera_probe_invoke_value(
        mono.class_get_method_from_name(camera_class, "get_farClipPlane", 0), camera);
    if (raw) builder_append_hex32(&builder, float_bits_at(raw, 0));
    else builder_append(&builder, "unreadable");
    builder_append(&builder, " aspect=");
    raw = camera_probe_invoke_value(
        mono.class_get_method_from_name(camera_class, "get_aspect", 0), camera);
    if (raw) builder_append_hex32(&builder, float_bits_at(raw, 0));
    else builder_append(&builder, "unreadable");
    builder_append(&builder, " pixelRect=");
    raw = camera_probe_invoke_value(
        mono.class_get_method_from_name(camera_class, "get_pixelRect", 0), camera);
    if (raw) builder_append_float_vector(&builder, raw, 4);
    else builder_append(&builder, "unreadable");
    observer_log("camera-probe", detail);

    /* render configuration. A non-NULL targetTexture would explain the exact
     * symptom (correct clear on the backbuffer, no geometry) without any
     * matrix being wrong, so it has to be ruled in or out explicitly.
     * backgroundColor is directly comparable to the dumped frame's two
     * colours: (0,0,0,5/255) over the camera rect and (0,0,0,0) outside. */
    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "camera=");
    builder_append_signed(&builder, index);
    builder_append(&builder, " targetTexture=");
    getter = mono.class_get_method_from_name(camera_class, "get_targetTexture", 0);
    builder_append_unsigned(&builder, (ULONG_PTR)camera_probe_invoke_object(getter, camera));
    builder_append(&builder, " clearFlags=");
    raw = camera_probe_invoke_value(
        mono.class_get_method_from_name(camera_class, "get_clearFlags", 0), camera);
    if (raw) builder_append_hex32(&builder, float_bits_at(raw, 0));
    else builder_append(&builder, "unreadable");
    builder_append(&builder, " cullingMask=");
    raw = camera_probe_invoke_value(
        mono.class_get_method_from_name(camera_class, "get_cullingMask", 0), camera);
    if (raw) builder_append_hex32(&builder, float_bits_at(raw, 0));
    else builder_append(&builder, "unreadable");
    builder_append(&builder, " depth=");
    raw = camera_probe_invoke_value(
        mono.class_get_method_from_name(camera_class, "get_depth", 0), camera);
    if (raw) builder_append_hex32(&builder, float_bits_at(raw, 0));
    else builder_append(&builder, "unreadable");
    builder_append(&builder, " backgroundColor=");
    raw = camera_probe_invoke_value(
        mono.class_get_method_from_name(camera_class, "get_backgroundColor", 0), camera);
    if (raw) builder_append_float_vector(&builder, raw, 4);
    else builder_append(&builder, "unreadable");
    builder_append(&builder, " rect=");
    raw = camera_probe_invoke_value(
        mono.class_get_method_from_name(camera_class, "get_rect", 0), camera);
    if (raw) builder_append_float_vector(&builder, raw, 4);
    else builder_append(&builder, "unreadable");
    observer_log("camera-probe", detail);

    /* the two matrices that decide whether geometry lands on screen */
    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "camera=");
    builder_append_signed(&builder, index);
    builder_append(&builder, " projectionMatrix=");
    raw = camera_probe_invoke_value(
        mono.class_get_method_from_name(camera_class, "get_projectionMatrix", 0), camera);
    if (raw) builder_append_float_vector(&builder, raw, 16);
    else builder_append(&builder, "unreadable");
    observer_log("camera-probe", detail);

    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "camera=");
    builder_append_signed(&builder, index);
    builder_append(&builder, " worldToCameraMatrix=");
    raw = camera_probe_invoke_value(
        mono.class_get_method_from_name(camera_class, "get_worldToCameraMatrix", 0), camera);
    if (raw) builder_append_float_vector(&builder, raw, 16);
    else builder_append(&builder, "unreadable");
    observer_log("camera-probe", detail);

    /* cameraToWorldMatrix — the SAME conceptual data through a DIFFERENT native
     * producer (get_cameraToWorldMatrix_Injected). worldToCameraMatrix and
     * projectionMatrix have byte-identical managed getters, so the defect is on
     * the native producer side; this splits "a copy path shared by every 64-byte
     * matrix icall" (expect m[15] wrong here too) from "this one producer"
     * (expect m[15] == 3f800000 here). Both answers are informative. */
    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "camera=");
    builder_append_signed(&builder, index);
    builder_append(&builder, " cameraToWorldMatrix=");
    raw = camera_probe_invoke_value(
        mono.class_get_method_from_name(camera_class, "get_cameraToWorldMatrix", 0), camera);
    if (raw) builder_append_float_vector(&builder, raw, 16);
    else builder_append(&builder, "unreadable");
    observer_log("camera-probe", detail);

    /* transform — a zero lossyScale is the cheapest way to make a view singular */
    getter = component_class ? mono.class_get_method_from_name(component_class, "get_transform", 0)
                             : NULL;
    transform = camera_probe_invoke_object(getter, camera);
    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "camera=");
    builder_append_signed(&builder, index);
    builder_append(&builder, " transform=");
    builder_append_unsigned(&builder, (ULONG_PTR)transform);
    if (transform && transform_class)
    {
        builder_append(&builder, " position=");
        raw = camera_probe_invoke_value(
            mono.class_get_method_from_name(transform_class, "get_position", 0), transform);
        if (raw) builder_append_float_vector(&builder, raw, 3);
        else builder_append(&builder, "unreadable");
        builder_append(&builder, " lossyScale=");
        raw = camera_probe_invoke_value(
            mono.class_get_method_from_name(transform_class, "get_lossyScale", 0), transform);
        if (raw) builder_append_float_vector(&builder, raw, 3);
        else builder_append(&builder, "unreadable");
    }
    observer_log("camera-probe", detail);
}

static void camera_probe_emit_totals(const char *reason);

/* DIAGNOSTIC repair of worldToCameraMatrix[15] — see the knob comment at the top.
 * Repairs the single float and writes the matrix back through Unity's own setter,
 * so the question "does this one float cause the black frame" becomes a pixel
 * measurement instead of an argument. */
static void camera_probe_repair_m33(void)
{
    MonoClass *camera_class;
    MonoMethod *all_getter, *getter, *setter, *reset;
    MonoArray *cameras;
    MonoObject **slot, *exception;
    void *raw, *args[1];
    ULONG matrix[16];            /* ULONG[] not char[]: guarantees 4-byte alignment */
    uintptr_t count, i;
    ULONG before, after;
    char detail[256];
    struct text_builder builder;

    camera_class = camera_probe_unity_class("UnityEngine", "Camera");
    if (!camera_class) return;
    getter = mono.class_get_method_from_name(camera_class, "get_worldToCameraMatrix", 0);
    setter = mono.class_get_method_from_name(camera_class, "set_worldToCameraMatrix", 1);
    reset = mono.class_get_method_from_name(camera_class, "ResetWorldToCameraMatrix", 0);
    if (!getter || !setter)
    {
        InterlockedIncrement(&m33_setter_missing);
        return;
    }

    all_getter = mono.class_get_method_from_name(camera_class, "get_allCameras", 0);
    cameras = (MonoArray *)camera_probe_invoke_object(all_getter, NULL);
    count = cameras ? mono.array_length(cameras) : 0;

    for (i = 0; i < count && i < 16u; i++)
    {
        slot = (MonoObject **)mono.array_addr_with_size(cameras, (int)sizeof(MonoObject *), i);
        if (!slot || !*slot) continue;
        InterlockedIncrement(&m33_attempted);

        /* Reset FIRST. Assigning worldToCameraMatrix makes Unity stop deriving it
         * from the transform, so without this the camera would freeze at the first
         * repaired value and stop following the player — the screen would light up
         * but the scene would not track, which is a worse experiment. */
        if (reset)
        {
            exception = NULL;
            mono.runtime_invoke(reset, *slot, NULL, &exception);
        }

        raw = camera_probe_invoke_value(getter, *slot);
        if (!raw)
        {
            InterlockedIncrement(&m33_unreadable);
            continue;
        }
        copy_bytes(matrix, raw, sizeof(matrix));
        before = matrix[15];
        if (before == 0x3f800000u)
        {
            /* Already correct — e.g. if an engine-side fix landed. Then this knob
             * is a no-op and cannot manufacture a false pixel result. */
            InterlockedIncrement(&m33_already_ok);
            continue;
        }
        matrix[15] = 0x3f800000u;

        args[0] = matrix;        /* value-type argument: pointer to the raw data */
        exception = NULL;
        mono.runtime_invoke(setter, *slot, args, &exception);
        if (exception)
        {
            InterlockedIncrement(&m33_exception);
            continue;
        }
        InterlockedIncrement(&m33_patched);

        /* Read back through the SAME getter. "The setter returned without an
         * exception" is not evidence that the value landed. */
        raw = camera_probe_invoke_value(getter, *slot);
        after = raw ? float_bits_at(raw, 15) : 0u;
        if (after == 0x3f800000u) InterlockedIncrement(&m33_verified);
        else InterlockedIncrement(&m33_verify_failed);

        if (InterlockedIncrement(&m33_logged) <= 24)
        {
            builder_init(&builder, detail, sizeof(detail));
            builder_append(&builder, "camera=");
            builder_append_signed(&builder, (LONG)i);
            builder_append(&builder, " m33_before=");
            builder_append_hex32(&builder, before);
            builder_append(&builder, " m33_after_readback=");
            builder_append_hex32(&builder, after);
            builder_append(&builder, " status=");
            builder_append(&builder, after == 0x3f800000u ? "REPAIRED" : "SET-DID-NOT-STICK");
            observer_log("camera-m33-repair", detail);
        }
    }
}

/* UnityEngine.Time. The dumped frame's only colour, (0,0,0,5/255), is the same
 * value Unity writes as the vertex colour — which is what a scene that has not
 * faded in looks like. Every HK fade is driven by Time; if deltaTime or
 * timeScale is zero, or time does not advance between two dumps, the menu is
 * rendered-but-frozen and no matrix is at fault. Two samples, not one:
 * a single reading cannot distinguish "stopped" from "slow". */
static void camera_probe_dump_time(const char *phase)
{
    MonoClass *time_class;
    void *raw;
    char detail[448];
    struct text_builder builder;
    static const char *const scalars[] = {
        "get_timeScale", "get_time", "get_unscaledTime", "get_deltaTime",
        "get_unscaledDeltaTime", "get_realtimeSinceStartup", "get_fixedDeltaTime"};
    static const char *const labels[] = {
        "timeScale", "time", "unscaledTime", "deltaTime",
        "unscaledDeltaTime", "realtimeSinceStartup", "fixedDeltaTime"};
    unsigned i;

    time_class = camera_probe_unity_class("UnityEngine", "Time");
    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "phase=");
    builder_append(&builder, phase);
    builder_append(&builder, " time_class=");
    builder_append_unsigned(&builder, (ULONG_PTR)time_class);
    if (time_class)
    {
        for (i = 0; i < sizeof(scalars) / sizeof(scalars[0]); i++)
        {
            builder_append(&builder, " ");
            builder_append(&builder, labels[i]);
            builder_append(&builder, "=");
            raw = camera_probe_invoke_value(
                mono.class_get_method_from_name(time_class, scalars[i], 0), NULL);
            if (raw) builder_append_hex32(&builder, float_bits_at(raw, 0));
            else builder_append(&builder, "unreadable");
        }
        builder_append(&builder, " frameCount=");
        raw = camera_probe_invoke_value(
            mono.class_get_method_from_name(time_class, "get_frameCount", 0), NULL);
        if (raw) builder_append_hex32(&builder, float_bits_at(raw, 0));
        else builder_append(&builder, "unreadable");
    }
    builder_append(&builder, " tick_ms=");
    builder_append_unsigned(&builder, GetTickCount64());
    observer_log("camera-probe-time", detail);
}

static void camera_probe_dump(const char *phase)
{
    MonoClass *camera_class, *object_class, *behaviour_class, *component_class, *transform_class;
    MonoMethod *all_cameras_getter, *main_getter;
    MonoObject *main_camera;
    MonoArray *all_cameras;
    MonoObject **element_storage;
    uintptr_t count, i;
    char detail[384];
    struct text_builder builder;

    if (!camera_probe) return;
    /* Managed getters below must never re-enter this probe through the filter. */
    if (InterlockedCompareExchange(&camera_probe_active, 1, 0)) return;

    /* The m33 repair runs on EVERY tick, deliberately BEFORE the stride and budget
     * gates: those gates exist to bound LOGGING, and a repair throttled by the log
     * budget would leave almost every frame unrepaired, which would make the
     * resulting pixel measurement meaningless. */
    if (camera_m33_repair) camera_probe_repair_m33();

    /* Per-frame callers would spend the whole budget in the first few frames;
     * stride spreads the samples across the boot instead. The tick counter is
     * itself a measurement: it is the managed call rate of the unprojection
     * callers, directly comparable to the frustum-warning count. */
    if (strings_equal(phase, "camera-tick"))
    {
        LONG tick = InterlockedIncrement(&camera_probe_ticks);

        if (!(tick % 4096)) camera_probe_emit_totals("periodic");
        if (camera_probe_stride > 1 && tick % camera_probe_stride)
        {
            InterlockedExchange(&camera_probe_active, 0);
            return;
        }
    }

    if (InterlockedIncrement(&camera_probe_dumps) > camera_probe_budget)
    {
        LONG suppressed = InterlockedIncrement(&camera_probe_suppressed);

        if (suppressed == 1 || !(suppressed % 4096)) camera_probe_emit_totals("budget");
        InterlockedExchange(&camera_probe_active, 0);
        return;
    }

    camera_probe_run_control();
    camera_probe_dump_time(phase);

    camera_class = camera_probe_unity_class("UnityEngine", "Camera");
    object_class = camera_probe_unity_class("UnityEngine", "Object");
    behaviour_class = camera_probe_unity_class("UnityEngine", "Behaviour");
    component_class = camera_probe_unity_class("UnityEngine", "Component");
    transform_class = camera_probe_unity_class("UnityEngine", "Transform");
    main_getter = camera_class ? mono.class_get_method_from_name(camera_class, "get_main", 0) : NULL;
    all_cameras_getter =
        camera_class ? mono.class_get_method_from_name(camera_class, "get_allCameras", 0) : NULL;
    main_camera = camera_probe_invoke_object(main_getter, NULL);
    all_cameras = (MonoArray *)camera_probe_invoke_object(all_cameras_getter, NULL);
    count = all_cameras ? mono.array_length(all_cameras) : 0;

    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "phase=");
    builder_append(&builder, phase);
    builder_append(&builder, " dump=");
    builder_append_signed(&builder, camera_probe_dumps);
    builder_append(&builder, " camera_class=");
    builder_append_unsigned(&builder, (ULONG_PTR)camera_class);
    builder_append(&builder, " main=");
    builder_append_unsigned(&builder, (ULONG_PTR)main_camera);
    builder_append(&builder, " allCameras=");
    builder_append_unsigned(&builder, (ULONGLONG)count);
    builder_append(&builder, " managed_tid=");
    builder_append_unsigned(&builder, GetCurrentThreadId());
    observer_log("camera-probe-begin", detail);

    if (camera_class)
        for (i = 0; i < count && i < 16u; i++)
        {
            element_storage = (MonoObject **)mono.array_addr_with_size(
                all_cameras, (int)sizeof(MonoObject *), i);
            if (!element_storage || !*element_storage) continue;
            camera_probe_dump_one(*element_storage, camera_class, object_class, behaviour_class,
                                  component_class, transform_class, phase, (int)i);
        }

    InterlockedExchange(&camera_probe_active, 0);
}

/* Aggregate totals — a capped instrument that does not say what it dropped
 * reads as full coverage when it is not. */
static void camera_probe_emit_totals(const char *reason)
{
    char detail[256];
    struct text_builder builder;

    if (!camera_probe) return;
    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "reason=");
    builder_append(&builder, reason);
    builder_append(&builder, " attempted=");
    builder_append_signed(&builder, camera_probe_dumps);
    builder_append(&builder, " emitted=");
    builder_append_signed(&builder,
                          camera_probe_dumps - camera_probe_suppressed > camera_probe_budget
                              ? camera_probe_budget
                              : camera_probe_dumps - camera_probe_suppressed);
    builder_append(&builder, " suppressed_by_budget=");
    builder_append_signed(&builder, camera_probe_suppressed);
    builder_append(&builder, " budget=");
    builder_append_signed(&builder, camera_probe_budget);
    builder_append(&builder, " unprojection_caller_ticks=");
    builder_append_signed(&builder, camera_probe_ticks);
    builder_append(&builder, " stride=");
    builder_append_signed(&builder, camera_probe_stride);
    observer_log("camera-probe-totals", detail);

    if (!camera_m33_repair) return;
    /* Repair totals are aggregate and never capped — a repair instrument that
     * reported only its first N successes would read as full coverage. */
    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "reason=");
    builder_append(&builder, reason);
    builder_append(&builder, " attempted=");
    builder_append_signed(&builder, m33_attempted);
    builder_append(&builder, " patched=");
    builder_append_signed(&builder, m33_patched);
    builder_append(&builder, " verified_readback=");
    builder_append_signed(&builder, m33_verified);
    builder_append(&builder, " verify_failed=");
    builder_append_signed(&builder, m33_verify_failed);
    builder_append(&builder, " already_ok=");
    builder_append_signed(&builder, m33_already_ok);
    builder_append(&builder, " unreadable=");
    builder_append_signed(&builder, m33_unreadable);
    builder_append(&builder, " setter_missing=");
    builder_append_signed(&builder, m33_setter_missing);
    builder_append(&builder, " managed_exception=");
    builder_append_signed(&builder, m33_exception);
    observer_log("camera-m33-repair-totals", detail);
}

static void method_enter(MonoProfiler *profiler, MonoMethod *method,
                         MonoProfilerCallContext *context)
{
    enum observed_method observed = classify_method(method);
    enum observed_method post_level_observed;
    char detail[256];
    struct text_builder builder;
    int value, available, requested_available, state_value, state_available;
    const char *unavailable_reason, *state_unavailable_reason;
    LONG before;
    (void)profiler;

    if (post_level_milestone_trace)
    {
        post_level_observed = classify_post_level_method(method);
        if (post_level_observed != METHOD_OTHER)
            log_post_level_milestone(post_level_observed, "enter");
    }

    switch (observed)
    {
    case METHOD_SET_LANGUAGE:
    {
        LARGE_INTEGER counter;

        QueryPerformanceCounter(&counter);
        builder_init(&builder, detail, sizeof(detail));
        builder_append(&builder, "class=StartManager method=SetLanguage phase=enter pid=");
        builder_append_unsigned(&builder, GetCurrentProcessId());
        builder_append(&builder, " wine_tid=");
        builder_append_unsigned(&builder, GetCurrentThreadId());
        builder_append(&builder, " native_tid_source=join-win32u-by-wine_tid hwnd=");
        builder_append_unsigned(&builder, (ULONG_PTR)GetForegroundWindow());
        builder_append(&builder, " focus=");
        builder_append_unsigned(&builder, (ULONG_PTR)GetFocus());
        builder_append(&builder, " active=");
        builder_append_unsigned(&builder, (ULONG_PTR)GetActiveWindow());
        builder_append(&builder, " keycode=0x0d direction=down acceptance=managed timestamp_ms=");
        builder_append_unsigned(&builder, GetTickCount64());
        builder_append(&builder, " qpc=");
        builder_append_signed(&builder, counter.QuadPart);
        observer_log("unity-ui-callback", detail);
        break;
    }
    case METHOD_HIGHLIGHT_DEFAULT:
        builder_init(&builder, detail, sizeof(detail));
        builder_append(&builder, "class=PreselectOption method=HighlightDefault phase=enter pid=");
        builder_append_unsigned(&builder, GetCurrentProcessId());
        builder_append(&builder, " wine_tid=");
        builder_append_unsigned(&builder, GetCurrentThreadId());
        builder_append(&builder, " native_tid_source=join-macdrv-by-wine_tid hwnd=");
        builder_append_unsigned(&builder, (ULONG_PTR)GetForegroundWindow());
        builder_append(&builder, " focus=");
        builder_append_unsigned(&builder, (ULONG_PTR)GetFocus());
        builder_append(&builder, " active=");
        builder_append_unsigned(&builder, (ULONG_PTR)GetActiveWindow());
        builder_append(&builder, " keycode=0x0d direction=proof");
        observer_log("unity-selection", detail);
        if (!return_route_only)
        {
            invoke_oneshot_sequence();
            invoke_diagnostic_direct_confirm();
            /* Same trigger, later target: HighlightDefault fires both on the language
             * screen and again once the Menu_Title menu is interactive (run20 seq=37 and
             * seq=121). The actuator itself decides whether UIManager exists yet. */
            invoke_start_game_actuator();
        }
        break;
    case METHOD_CONFIRM_LANGUAGE:
        value = 0;
        available = read_confirmed_language(method, context, &value, &unavailable_reason);
        builder_init(&builder, detail, sizeof(detail));
        builder_append(&builder, "class=StartManager method=ConfirmLanguage phase=enter call=");
        builder_append_signed(&builder, InterlockedIncrement(&confirm_count));
        builder_append(&builder, " confirmedLanguage=");
        builder_append_bool_availability(&builder, available, value, unavailable_reason);
        observer_log("confirm-language", detail);
        camera_probe_dump("confirm-language");
        break;
    case METHOD_CAMERA_TICK:
        camera_probe_dump("camera-tick");
        break;
    case METHOD_MANAGED_TICK:
    {
        LONG n = InterlockedIncrement(&managed_tick_count);
        if (n <= 5 || (n % 512) == 0)
        {
            builder_init(&builder, detail, sizeof(detail));
            builder_append(&builder, "class=");
            builder_append(&builder, mono.class_get_name(mono.method_get_class(method)));
            builder_append(&builder, " method=Update ticks=");
            builder_append_signed(&builder, n);
            observer_log("managed-tick", detail);
        }
        break;
    }
    case METHOD_ASYNCOP_POLL:
    {
        /* Rate-limited: the first few prove the loop is alive, then a sparse sample keeps the
         * log honest without drowning it (a healthy game polls this every frame). */
        LONG n = InterlockedIncrement(&asyncop_poll_count);
        if (n <= 8 || (n % 512) == 0)
        {
            int a_value = -1;
            const char *a_reason = NULL;
            int a_ok = read_allow_scene_activation_property(&a_value, &a_reason);

            builder_init(&builder, detail, sizeof(detail));
            builder_append(&builder, "class=UnityEngine.AsyncOperation method=");
            builder_append(&builder, mono.method_get_name(method));
            builder_append(&builder, " polls=");
            builder_append_signed(&builder, n);
            builder_append(&builder, " allowSceneActivation=");
            if (a_ok) builder_append_signed(&builder, a_value);
            else
            {
                builder_append(&builder, "unavailable reason=");
                builder_append(&builder, a_reason ? a_reason : "unknown");
            }
            observer_log("asyncop-poll", detail);
        }
        break;
    }
    case METHOD_ALLOW_SCENE_ACTIVATION:
        state_value = -1;
        state_available =
            read_allow_scene_activation_property(&state_value, &state_unavailable_reason);
        if (state_available)
            before = InterlockedExchange(&allow_scene_activation, state_value);
        else
            before = InterlockedCompareExchange(&allow_scene_activation, 0, 0);
        value = -1;
        requested_available = read_bool_argument(context, 0, &value);
        builder_init(&builder, detail, sizeof(detail));
        builder_append(&builder,
                       "class=UnityEngine.AsyncOperation method=set_allowSceneActivation phase=enter before=");
        builder_append_signed(&builder, before);
        builder_append(&builder, " requested=");
        if (requested_available) builder_append_signed(&builder, value);
        else builder_append(&builder, "unavailable");
        builder_append(&builder, " beforeSource=");
        builder_append(&builder, state_available ? "StartManager.loadop-getter" : "cached");
        builder_append(&builder, " requestedSource=");
        builder_append(&builder, requested_available ? "call-context" : "unavailable");
        if (!state_available)
        {
            builder_append(&builder, " stateReason=");
            builder_append(&builder,
                           state_unavailable_reason ? state_unavailable_reason : "unknown");
        }
        observer_log("allow-scene-activation", detail);
        break;
    case METHOD_SCENE_LOAD:
    case METHOD_SCENE_LOAD_ASYNC:
        log_scene_load(observed, method, context, "attempt");
        break;
    case METHOD_GAME_MANAGER_AWAKE:
    case METHOD_GAME_CAMERAS_AWAKE:
    case METHOD_UI_MANAGER_AWAKE:
        log_manager_awake(observed);
        /* Guaranteed sample points that do not depend on the unprojection
         * callers ever firing. GameCameras::Awake is where HK builds its
         * cameras; UIManager::Awake lands with the menu UI, which is the late
         * sample the strided ticks might otherwise never reach. */
        camera_probe_dump(observed == METHOD_GAME_CAMERAS_AWAKE   ? "gamecameras-awake"
                          : observed == METHOD_UI_MANAGER_AWAKE   ? "uimanager-awake"
                                                                  : "gamemanager-awake");
        /* Second trigger for the start-game actuator, and the one that matters.
         * run20's ordering, measured: HighlightDefault seq=8 (L1853) and seq=37
         * (L1888) both land BEFORE UIManager::Awake (seq=67, L1932), so at those
         * two the UIManager genuinely does not exist yet. GameCameras::Awake
         * (seq=95, L1960) is the first hook AFTER it. Relying on HighlightDefault
         * seq=121 alone would leave exactly one usable trigger in the whole run. */
        if (!return_route_only && observed == METHOD_GAME_CAMERAS_AWAKE)
            invoke_start_game_actuator();
        break;
    default:
        break;
    }
}

static void method_leave(MonoProfiler *profiler, MonoMethod *method,
                         MonoProfilerCallContext *context)
{
    enum observed_method observed = classify_method(method);
    enum observed_method post_level_observed;
    char detail[256];
    struct text_builder builder;
    int value, available, state_available;
    const char *unavailable_reason, *state_unavailable_reason, *state_source;
    LONG before;
    (void)profiler;

    if (post_level_milestone_trace)
    {
        post_level_observed = classify_post_level_method(method);
        if (post_level_observed != METHOD_OTHER)
            log_post_level_milestone(post_level_observed, "leave");
    }

    if (observed == METHOD_CONFIRM_LANGUAGE)
    {
        value = 0;
        available = read_confirmed_language(method, context, &value, &unavailable_reason);
        builder_init(&builder, detail, sizeof(detail));
        builder_append(&builder, "class=StartManager method=ConfirmLanguage phase=return call=");
        builder_append_signed(&builder, InterlockedCompareExchange(&confirm_count, 0, 0));
        builder_append(&builder, " confirmedLanguage=");
        builder_append_bool_availability(&builder, available, value, unavailable_reason);
        observer_log("confirm-language", detail);
    }
    else if (observed == METHOD_ALLOW_SCENE_ACTIVATION)
    {
        value = -1;
        state_available =
            read_allow_scene_activation_property(&value, &state_unavailable_reason);
        state_source = state_available ? "StartManager.loadop-getter" : "unavailable";
        if (!state_available)
        {
            state_available = read_bool_argument(context, 0, &value);
            state_source = state_available ? "call-context" : "unavailable";
            state_unavailable_reason = state_available ? NULL : "call-context-and-loadop-unavailable";
        }
        if (state_available)
            before = InterlockedExchange(&allow_scene_activation, value);
        else
            before = InterlockedCompareExchange(&allow_scene_activation, 0, 0);
        builder_init(&builder, detail, sizeof(detail));
        builder_append(&builder,
                       "class=UnityEngine.AsyncOperation method=set_allowSceneActivation phase=return before=");
        builder_append_signed(&builder, before);
        builder_append(&builder, " after=");
        if (state_available) builder_append_signed(&builder, value);
        else builder_append(&builder, "unavailable");
        builder_append(&builder, " afterSource=");
        builder_append(&builder, state_source);
        if (!state_available)
        {
            builder_append(&builder, " stateReason=");
            builder_append(&builder,
                           state_unavailable_reason ? state_unavailable_reason : "unknown");
        }
        observer_log("allow-scene-activation", detail);
    }
    else if (observed == METHOD_SCENE_LOAD || observed == METHOD_SCENE_LOAD_ASYNC)
    {
        log_scene_load(observed, method, context, "return");
    }
}

static int configure_observer(void)
{
    return_route_only = env_enabled("MACRUNNER_HB_RETURN_ROUTE_OBSERVER");
    language_flow_observer = env_enabled("MACRUNNER_HB_LANGUAGE_FLOW_OBSERVER");
    asyncop_poll = env_enabled("MACRUNNER_HB_ASYNCOP_POLL");
    managed_tick = env_enabled("MACRUNNER_HB_MANAGED_TICK");
    post_level_milestone_trace = env_enabled("MACRUNNER_HB_POST_LEVEL_MILESTONE_TRACE");
    if (!return_route_only && !language_flow_observer && !post_level_milestone_trace)
        return 0;
    camera_probe = env_enabled("MACRUNNER_HB_CAMERA_PROBE");
    /* Default OFF and gated on the probe itself: run20 is an A/B that requires the
     * identical instrument, so this must be behaviourally invisible unless asked for. */
    camera_m33_repair = camera_probe && env_enabled("MACRUNNER_HB_CAMERA_M33_REPAIR");
    camera_probe_budget = camera_probe ? env_record_limit("MACRUNNER_HB_CAMERA_PROBE_MAX") : 0;
    camera_probe_stride = camera_probe ? env_record_limit("MACRUNNER_HB_CAMERA_PROBE_STRIDE") : 1;
    oneshot_sequence =
        !return_route_only && language_flow_observer && !post_level_milestone_trace &&
        env_enabled("MACRUNNER_HB_LANGUAGE_ONESHOT_SEQUENCE");
    /* Default OFF, and gated on the same trigger path the language actuator uses, so a
     * run that does not ask for it is byte-for-byte the previous behaviour. */
    start_game_actuator =
        !return_route_only && language_flow_observer &&
        env_enabled("MACRUNNER_HB_START_GAME_ACTUATOR");
    if (start_game_actuator)
    {
        LONG budget = env_record_limit("MACRUNNER_HB_START_GAME_ACTUATOR_MAX");
        if (budget > 0) start_game_budget = budget;
    }
    /* Gated on the actuator, because it runs inside it and measures the state at
     * that exact call site. Default OFF: unset => byte-identical behaviour. */
    newobj_probe = start_game_actuator && env_enabled("MACRUNNER_HB_NEWOBJ_PROBE");
    diagnostic_direct_confirm =
        !oneshot_sequence && !return_route_only && language_flow_observer &&
        !post_level_milestone_trace &&
        env_enabled("MACRUNNER_HB_LANGUAGE_DIAGNOSTIC_DIRECT_CONFIRM");
    if (return_route_only) record_limit = env_record_limit("MACRUNNER_HB_RETURN_ROUTE_OBSERVER_MAX");
    else if (language_flow_observer)
        record_limit = env_record_limit("MACRUNNER_HB_LANGUAGE_FLOW_OBSERVER_MAX");
    else record_limit = env_record_limit("MACRUNNER_HB_POST_LEVEL_MILESTONE_TRACE_MAX");
    GetEnvironmentVariableA(return_route_only
                                ? "MACRUNNER_HB_RETURN_ROUTE_OBSERVER_LOG"
                                : (language_flow_observer
                                       ? "MACRUNNER_HB_LANGUAGE_FLOW_OBSERVER_LOG"
                                       : "MACRUNNER_HB_POST_LEVEL_MILESTONE_TRACE_LOG"),
                            log_path, sizeof(log_path));
    return 1;
}

static void initialize_observer(HMODULE module, const char *description,
                                int introspection_enabled)
{
    MonoProfilerHandle handle;
    static ULONG_PTR profiler_cookie;
    char detail[512];
    struct text_builder builder;

    RESOLVE_API(module, enable_call_context_introspection,
                "mono_profiler_enable_call_context_introspection");
    RESOLVE_API(module, profiler_create, "mono_profiler_create");
    RESOLVE_API(module, set_filter, "mono_profiler_set_call_instrumentation_filter_callback");
    RESOLVE_API(module, set_method_enter, "mono_profiler_set_method_enter_callback");
    RESOLVE_API(module, set_method_leave, "mono_profiler_set_method_leave_callback");
    RESOLVE_API(module, method_get_name, "mono_method_get_name");
    RESOLVE_API(module, method_get_class, "mono_method_get_class");
    RESOLVE_API(module, class_get_name, "mono_class_get_name");
    RESOLVE_API(module, class_get_namespace, "mono_class_get_namespace");
    RESOLVE_API(module, class_get_field_from_name, "mono_class_get_field_from_name");
    RESOLVE_API(module, field_get_value, "mono_field_get_value");
    RESOLVE_OPTIONAL_API(module, method_signature, "mono_method_signature");
    RESOLVE_OPTIONAL_API(module, signature_get_param_count, "mono_signature_get_param_count");
    RESOLVE_OPTIONAL_API(module, signature_get_params, "mono_signature_get_params");
    RESOLVE_OPTIONAL_API(module, type_get_name, "mono_type_get_name");
    RESOLVE_OPTIONAL_API(module, string_to_utf8, "mono_string_to_utf8");
    RESOLVE_OPTIONAL_API(module, mono_free, "mono_free");
    if (post_level_milestone_trace)
    {
        RESOLVE_API(module, method_signature, "mono_method_signature");
        RESOLVE_API(module, signature_get_param_count, "mono_signature_get_param_count");
    }
    if (language_flow_observer)
    {
        RESOLVE_API(module, class_get_method_from_name, "mono_class_get_method_from_name");
        RESOLVE_API(module, object_get_class, "mono_object_get_class");
        RESOLVE_API(module, object_unbox, "mono_object_unbox");
        RESOLVE_API(module, runtime_invoke, "mono_runtime_invoke");
    }
    if (oneshot_sequence)
    {
        RESOLVE_API(module, gchandle_new, "mono_gchandle_new");
        RESOLVE_API(module, image_loaded, "mono_image_loaded");
        RESOLVE_API(module, class_from_name, "mono_class_from_name");
        RESOLVE_API(module, class_get_type, "mono_class_get_type");
        RESOLVE_API(module, type_get_object, "mono_type_get_object");
        RESOLVE_API(module, get_root_domain, "mono_get_root_domain");
        RESOLVE_API(module, string_new, "mono_string_new");
        RESOLVE_API(module, array_length, "mono_array_length");
        RESOLVE_API(module, array_addr_with_size, "mono_array_addr_with_size");
    }
    if (newobj_probe)
    {
        /* OPTIONAL on purpose. RESOLVE_API aborts observer init on a miss, which
         * would take the PROVEN 27/27 language actuator down with it. A diagnostic
         * must never be able to break the load-bearing path, so these resolve
         * softly and run_newobj_probe refuses (loudly) if any is NULL.
         * All 5 were confirmed present in the shipped DLL's export table before
         * this was written, so a miss here is a real anomaly worth logging. */
        RESOLVE_OPTIONAL_API(module, image_loaded, "mono_image_loaded");
        RESOLVE_OPTIONAL_API(module, class_from_name, "mono_class_from_name");
        RESOLVE_OPTIONAL_API(module, get_root_domain, "mono_get_root_domain");
        RESOLVE_OPTIONAL_API(module, class_instance_size, "mono_class_instance_size");
        RESOLVE_OPTIONAL_API(module, class_get_nested_types, "mono_class_get_nested_types");
        RESOLVE_OPTIONAL_API(module, class_vtable, "mono_class_vtable");
        RESOLVE_OPTIONAL_API(module, object_new_specific, "mono_object_new_specific");
        RESOLVE_OPTIONAL_API(module, object_new, "mono_object_new");
    }
    if (diagnostic_direct_confirm)
    {
        RESOLVE_API(module, enable_allocations, "mono_profiler_enable_allocations");
        RESOLVE_API(module, set_gc_allocation,
                    "mono_profiler_set_gc_allocation_callback");
        RESOLVE_API(module, gchandle_new, "mono_gchandle_new");
    }
    RESOLVE_API(module, context_get_this, "mono_profiler_call_context_get_this");
    RESOLVE_API(module, context_get_argument, "mono_profiler_call_context_get_argument");
    RESOLVE_API(module, context_free_buffer, "mono_profiler_call_context_free_buffer");

    if (!introspection_enabled && !mono.enable_call_context_introspection())
    {
        /* MacRunner 2026-08-04 — НЕ смертельно, и вот почему.
         *
         * Интроспекция контекста вызова нужна для чтения АРГУМЕНТОВ методов. Сама
         * последовательность событий (вход/выход) от неё не зависит. Mono выдаёт это
         * разрешение только когда профилировщик поднимается ВМЕСТЕ со средой исполнения;
         * при подключении к уже запущенному Mono — отказывает. Именно так вышло на настоящей
         * Windows 04.08: наблюдатель доставлен подставной version.dll, дошёл до этой строки и
         * умирал здесь, хотя всё остальное работало.
         *
         * Раньше отказ означал полный отказ, и эталон было не снять в принципе. Теперь
         * записываем предупреждение и продолжаем: поток событий важнее аргументов. В НАШИХ
         * прогонах путь другой (движок передаёт introspection_enabled=1 и сюда не заходит),
         * поэтому их поведение не меняется. */
        observer_log("introspection-unavailable",
                     "status=rejected impact=arguments-unavailable events=still-recorded");
    }
    if (diagnostic_direct_confirm && !mono.enable_allocations())
    {
        observer_log("init-failed", "status=allocation-profiling-rejected");
        return;
    }
    handle = mono.profiler_create((MonoProfiler *)&profiler_cookie);
    if (!handle)
    {
        observer_log("init-failed", "status=profiler-create-null");
        return;
    }
    mono.set_filter(handle, method_filter);
    mono.set_method_enter(handle, method_enter);
    mono.set_method_leave(handle, method_leave);
    observer_handle = handle;
    if (diagnostic_direct_confirm) mono.set_gc_allocation(observer_handle, gc_allocation);
    builder_init(&builder, detail, sizeof(detail));
    builder_append(&builder, "status=armed limit=");
    builder_append_signed(&builder, record_limit);
    builder_append(&builder, " description=");
    builder_append(&builder, description ? description : "");
    builder_append(&builder, " mode=");
    builder_append(&builder, return_route_only ? "return-route" : "language-flow");
    if (oneshot_sequence)
        builder_append(&builder,
                       " oneshot-sequence=armed source=accepted-145129 activation-control=excluded");
    /* Positive control for the poll hook: a zero count means "nobody polls" only if the hook was
     * armed at all.  Print the gate here, in the banner that always appears. */
    builder_append(&builder, " asyncop-poll=");
    builder_append_signed(&builder, asyncop_poll ? 1 : 0);
    builder_append(&builder, " managed-tick=");
    builder_append_signed(&builder, managed_tick ? 1 : 0);
    if (post_level_milestone_trace)
    {
        builder_append(&builder,
                       " post-level-milestone-trace=armed allocations=excluded direct-confirm=excluded");
        builder_append(&builder,
                       " target0=GameManager.LevelActivated/2@0x06000D53:0x4C870");
        builder_append(&builder,
                       " target1=UIManager.MakeMenuLean/0@0x06000F17:0x52560");
        builder_append(&builder,
                       " target2=OpeningSequence.OnChangingSequences/0@0x06000355:0x194A4");
    }
    observer_log("observer-init", detail);
}

__declspec(dllexport) void __cdecl mono_profiler_init_hk_language(const char *description)
{
    HMODULE module;

    if (!configure_observer()) return;
    module = GetModuleHandleW(L"mono-2.0-bdwgc.dll");
    if (!module)
    {
        observer_log("init-failed", "status=mono-module-not-loaded");
        return;
    }
    initialize_observer(module, description, 0);
}

__declspec(dllexport) void __cdecl macrunner_hb_profiler_init_hk_language(
    const struct macrunner_hb_language_observer_bootstrap *bootstrap)
{
    if (!configure_observer()) return;
    if (!bootstrap || bootstrap->version != MACRUNNER_HB_LANGUAGE_OBSERVER_BOOTSTRAP_VERSION ||
        bootstrap->introspection_enabled != 1 || !bootstrap->mono_module)
    {
        observer_log("init-failed", "status=invalid-bootstrap-contract");
        return;
    }
    initialize_observer(bootstrap->mono_module, bootstrap->description,
                        bootstrap->introspection_enabled);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    (void)instance;
    (void)reserved;

    /* MacRunner 2026-08-04 — самозацепление для ЭТАЛОННОГО захвата на Windows.
     *
     * Обычный путь (движок зовёт macrunner_hb_profiler_init_hk_language) на настоящей Windows
     * недоступен: движка там нет. Инжектор пробовал позвать штатный экспорт удалённым потоком и
     * умирал молча — под Prism и игра, и инжектор эмулируемые x64, у них РАЗНАЯ раскладка
     * модулей, и адрес LoadLibraryW/экспорта, взятый в своём процессе, в чужом означает не то.
     * Замер 04.08: журнал инжектора обрывается сразу после "Mono на месте", ни одна из четырёх
     * последующих веток отказа не печатается — то есть он не возвращается, а падает.
     *
     * Здесь этой проблемы нет по построению: DllMain исполняется УЖЕ В ЦЕЛЕВОМ процессе, все
     * адреса свои. Достаточно обычной LoadLibraryW, и два самых хрупких шага внедрения отпадают.
     *
     * Отдельный гейт, а не «всегда»: в НАШИХ прогонах наблюдателя инициализирует движок через
     * бутстрап, и самозацепление дало бы двойную инициализацию. Переменная ставится только в
     * эталонном захвате, поэтому поведение наших прогонов не меняется ни на бит. */
    if (reason == DLL_PROCESS_ATTACH)
    {
        /* env_enabled — свой читатель переменных: библиотека собирается без стандартной
         * библиотеки C, поэтому getenv здесь недоступен (ld.lld: undefined symbol). */
        if (env_enabled("MACRUNNER_HB_OBSERVER_SELF_ATTACH"))
            mono_profiler_init_hk_language("self-attach");
    }
    return TRUE;
}
