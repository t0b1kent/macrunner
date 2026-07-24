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
    METHOD_SCENE_LOAD_ASYNC
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
    if (namespace_name && strings_equal(namespace_name, "UnityEngine.SceneManagement") &&
        strings_equal(class_name, "SceneManager"))
    {
        if (strings_equal(method_name, "LoadScene")) return METHOD_SCENE_LOAD;
        if (strings_equal(method_name, "LoadSceneAsync")) return METHOD_SCENE_LOAD_ASYNC;
    }
    if (strings_equal(method_name, "Awake"))
    {
        if (strings_equal(class_name, "GameManager")) return METHOD_GAME_MANAGER_AWAKE;
        if (strings_equal(class_name, "GameCameras")) return METHOD_GAME_CAMERAS_AWAKE;
        if (strings_equal(class_name, "UIManager")) return METHOD_UI_MANAGER_AWAKE;
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
        break;
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
    post_level_milestone_trace = env_enabled("MACRUNNER_HB_POST_LEVEL_MILESTONE_TRACE");
    if (!return_route_only && !language_flow_observer && !post_level_milestone_trace)
        return 0;
    oneshot_sequence =
        !return_route_only && language_flow_observer && !post_level_milestone_trace &&
        env_enabled("MACRUNNER_HB_LANGUAGE_ONESHOT_SEQUENCE");
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
        observer_log("init-failed", "status=call-context-introspection-rejected");
        return;
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
    (void)reason;
    (void)reserved;
    return TRUE;
}
