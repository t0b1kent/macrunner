/*
 * MacRunner HyperBridge entrypoint dispatch.
 *
 * This is the first real x86_64-on-arm64 Wine loader boundary: PE-side
 * loader.c calls this Unix-side routine when it needs to run an AMD64 DLL
 * entrypoint from an ARM64 host process.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winbase.h"
#include "winuser.h"
#include "winternl.h"
#include "wine/asm.h"
#include "unix_private.h"
#include "unixlib.h"

#include "../../../../hyperbridge/include/hb_abi.h"
#include "../../../../hyperbridge/include/hb_context.h"
#include "../../../../hyperbridge/include/hb_decoder.h"
#include "../../../../hyperbridge/include/hb_ir.h"
#include "../../../../hyperbridge/include/hb_lifter.h"
#include "../../../../hyperbridge/include/hb_memory.h"
#include "../../../../hyperbridge/include/hb_runtime.h"

WINE_DEFAULT_DEBUG_CHANNEL(module);

struct macrunner_hb_special
{
    void *teb;
    void *peb;
};

#define MACRUNNER_HB_IMPORT_MAX 4096
#define MACRUNNER_HB_IMPORT_BASE 0x00006f0000000000ULL
#define MACRUNNER_HB_IMPORT_STRIDE 0x10ULL
#define MACRUNNER_HB_SEH_STACK_SLACK 0x10000ULL

struct macrunner_hb_import_thunk
{
    uint64_t guest_target;
    void *target;
    void *pe_call12;
    void *pe_callback12;
    uint64_t module_id;
    USHORT target_machine;
    char dll_name[96];
    char import_name[96];
};

static NTSTATUS macrunner_hb_run_x64( void *entry, hb_abi_x64_call_t *call, uint64_t *ret_value,
                                      uint64_t *blocks_out, uint64_t *steps_out,
                                      const char *label, void *image_base );
static BOOL macrunner_hb_pc_is_native_pe_builtin( uint64_t pc, void **module_base );

static pthread_mutex_t macrunner_hb_import_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct macrunner_hb_import_thunk macrunner_hb_imports[MACRUNNER_HB_IMPORT_MAX];
static unsigned int macrunner_hb_import_count;
static __thread void *macrunner_hb_bridge_stack_limit;
static __thread void *macrunner_hb_bridge_stack_base;
static __thread size_t macrunner_hb_bridge_stack_size;
static __thread uintptr_t macrunner_hb_native_call_guest_rsp;
static __thread void *macrunner_hb_original_stack_limit;
static __thread void *macrunner_hb_original_stack_base;

static IMAGE_NT_HEADERS *macrunner_hb_image_nt_header( void *module )
{
    IMAGE_DOS_HEADER *dos = module;
    IMAGE_NT_HEADERS *nt;

    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return NULL;
    nt = (IMAGE_NT_HEADERS *)((BYTE *)module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
    return nt;
}

static void *macrunner_hb_find_named_export( void *module, const char *name )
{
    IMAGE_DATA_DIRECTORY *dir;
    IMAGE_EXPORT_DIRECTORY *exports;
    const DWORD *names;
    const WORD *ordinals;
    const DWORD *functions;
    IMAGE_NT_HEADERS *nt;
    ULONG size;
    unsigned int i;

    if (!module || !name) return NULL;
    nt = macrunner_hb_image_nt_header( module );

    if (!nt) return NULL;
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir->VirtualAddress || !dir->Size) return NULL;
    exports = (IMAGE_EXPORT_DIRECTORY *)((BYTE *)module + dir->VirtualAddress);
    names = (const DWORD *)((BYTE *)module + exports->AddressOfNames);
    ordinals = (const WORD *)((BYTE *)module + exports->AddressOfNameOrdinals);
    functions = (const DWORD *)((BYTE *)module + exports->AddressOfFunctions);
    size = dir->Size;

    for (i = 0; i < exports->NumberOfNames; i++)
    {
        const char *export_name = (const char *)module + names[i];
        DWORD rva;

        if (strcmp( export_name, name )) continue;
        rva = functions[ordinals[i]];
        if (rva >= dir->VirtualAddress && rva < dir->VirtualAddress + size) return NULL;
        return (BYTE *)module + rva;
    }
    return NULL;
}

static BOOL macrunner_hb_read_local_memory( uintptr_t addr, void *buf, size_t size )
{
#ifdef __APPLE__
    mach_vm_size_t out_size = 0;
    kern_return_t kr;

    if (!addr || !buf || !size) return FALSE;
    kr = mach_vm_read_overwrite( mach_task_self(), (mach_vm_address_t)addr, (mach_vm_size_t)size,
                                 (mach_vm_address_t)buf, &out_size );
    return kr == KERN_SUCCESS && out_size == size;
#else
    if (!addr || !buf || !size) return FALSE;
    memcpy( buf, (const void *)addr, size );
    return TRUE;
#endif
}

static void *macrunner_hb_module_from_pc( void *pc )
{
    uintptr_t p = (uintptr_t)pc & ~(uintptr_t)0xfff;
    unsigned int i;

    for (i = 0; i < 0x100000; i++, p -= 0x1000)
    {
        IMAGE_DOS_HEADER dos;
        IMAGE_NT_HEADERS nt;
        uintptr_t nt_addr;

        if (!macrunner_hb_read_local_memory( p, &dos, sizeof(dos) )) continue;
        if (dos.e_magic != IMAGE_DOS_SIGNATURE) continue;
        if (dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000) continue;
        nt_addr = p + dos.e_lfanew;
        if (!macrunner_hb_read_local_memory( nt_addr, &nt, sizeof(nt) )) continue;
        if (nt.Signature != IMAGE_NT_SIGNATURE) continue;
        return (void *)p;
    }
    return NULL;
}

static int macrunner_hb_strieq( const char *a, const char *b )
{
    unsigned char ca, cb;

    if (!a || !b) return 0;
    while (*a && *b)
    {
        ca = (unsigned char)*a++;
        cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return 0;
    }
    return !*a && !*b;
}

static int macrunner_hb_stristarts( const char *str, const char *prefix )
{
    unsigned char ca, cb;

    if (!str || !prefix) return 0;
    while (*prefix)
    {
        if (!*str) return 0;
        ca = (unsigned char)*str++;
        cb = (unsigned char)*prefix++;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return 0;
    }
    return 1;
}

static void macrunner_hb_copy_cstr( char *dst, size_t dst_size, const char *src )
{
    size_t i;

    if (!dst || !dst_size) return;
    dst[0] = 0;
    if (!src) return;
    for (i = 0; i + 1 < dst_size && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static uint64_t macrunner_hb_get_block_limit( const char *label )
{
    const char *value = getenv( "MACRUNNER_HB_X64_BLOCK_LIMIT" );
    char *end = NULL;
    uint64_t limit;

    if (value && *value)
    {
        limit = strtoull( value, &end, 0 );
        if (end && *end == 0) return limit;
    }

    /* Thread entries may run GUI message loops for the life of the process.
     * Keep finite caps for DLL/callback probes, but don't kill real app
     * threads just because they stayed alive. */
    if (label && !strcmp( label, "thread" )) return 0;
    return 200000;
}

static uint64_t macrunner_hb_get_step_limit( const char *label, uint64_t block_limit )
{
    const char *value = getenv( "MACRUNNER_HB_X64_STEP_LIMIT" );
    char *end = NULL;
    uint64_t limit;

    if (value && *value)
    {
        limit = strtoull( value, &end, 0 );
        if (end && *end == 0) return limit;
    }

    /* Keep the interpreter's internal safety caps in sync with the outer app
     * policy.  A real x64 app thread may legitimately spend a long time inside
     * one lifted block (for example strlen/memchr-style loops), so disabling
     * the outer thread block limit must disable the per-context guard too. */
    if (label && !strcmp( label, "thread" ) && !block_limit) return 0;
    return 1000000;
}

static void macrunner_hb_get_proc_name_for_trace( uint64_t name_or_ordinal, char *name, size_t name_size )
{
    if (!name || !name_size) return;
    if (!name_or_ordinal)
    {
        macrunner_hb_copy_cstr( name, name_size, "GetProcAddress(NULL)" );
        return;
    }
    if (!(name_or_ordinal >> 16))
    {
        snprintf( name, name_size, "#%u", (unsigned int)name_or_ordinal );
        return;
    }
    macrunner_hb_copy_cstr( name, name_size, (const char *)(uintptr_t)name_or_ordinal );
}

static void macrunner_hb_get_export_module_name( void *module, char *name, size_t name_size )
{
    IMAGE_DATA_DIRECTORY *dir;
    IMAGE_EXPORT_DIRECTORY *exports;
    IMAGE_NT_HEADERS *nt;

    macrunner_hb_copy_cstr( name, name_size, "dynamic-builtin.dll" );
    if (!module || !name || !name_size) return;
    if (!(nt = macrunner_hb_image_nt_header( module ))) return;
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir->VirtualAddress || !dir->Size) return;
    exports = (IMAGE_EXPORT_DIRECTORY *)((BYTE *)module + dir->VirtualAddress);
    if (!exports->Name) return;
    macrunner_hb_copy_cstr( name, name_size, (const char *)module + exports->Name );
}

static int macrunner_hb_debug_enabled(void)
{
    const char *val = getenv( "MACRUNNER_HB_DEBUG" );
    return val && val[0] && val[0] != '0';
}

static int macrunner_hb_trace_abi_enabled(void)
{
    const char *val = getenv( "MACRUNNER_HB_TRACE_ABI" );
    return val && val[0] && val[0] != '0';
}

static int macrunner_hb_trace_abi_stack_enabled(void)
{
    const char *val = getenv( "MACRUNNER_HB_TRACE_ABI_STACK" );
    return val && val[0] && val[0] != '0';
}

static int macrunner_hb_trace_calc_object_enabled(void)
{
    const char *val = getenv( "MACRUNNER_HB_TRACE_CALC_OBJECT" );
    return val && val[0] && val[0] != '0';
}

static int macrunner_hb_trace_direct_native_enabled(void)
{
    const char *val = getenv( "MACRUNNER_HB_TRACE_DIRECT_NATIVE" );
    return val && val[0] && val[0] != '0';
}

static int macrunner_hb_trace_file_api_enabled(void)
{
    const char *val = getenv( "MACRUNNER_HB_TRACE_FILE_API" );
    return val && val[0] && val[0] != '0';
}

static int macrunner_hb_trace_npp_open_pack_enabled(void)
{
    const char *val = getenv( "MACRUNNER_HB_TRACE_NPP_OPEN_PACK" );
    return val && val[0] && val[0] != '0';
}

static int macrunner_hb_trace_image_api_enabled(void)
{
    const char *val = getenv( "MACRUNNER_HB_TRACE_IMAGE_API" );
    return val && val[0] && val[0] != '0';
}

static int macrunner_hb_trace_special_vm_enabled(void)
{
    const char *val = getenv( "MACRUNNER_HB_TRACE_SPECIAL_VM" );
    return val && val[0] && val[0] != '0';
}

static void macrunner_hb_trace_special_vm_fault( const char *op, hb_gva_t original,
                                                 mach_vm_address_t cur, size_t remaining,
                                                 kern_return_t kr, mach_vm_address_t region,
                                                 mach_vm_size_t region_size,
                                                 vm_prot_t protection )
{
    static int count;
    if (!macrunner_hb_trace_special_vm_enabled()) return;
    if (++count > 40) return;
    ERR( "macrunner-hb-special-vm: op=%s original=%p cur=%p remaining=0x%zx "
         "kr=%d region=%p end=%p size=0x%llx prot=0x%x\n",
         op, (void *)(uintptr_t)original, (void *)(uintptr_t)cur, remaining, kr,
         (void *)(uintptr_t)region, (void *)(uintptr_t)(region + region_size),
         (unsigned long long)region_size, protection );
}

static int macrunner_hb_trace_image_api_budget_allows(void)
{
    static int count;
    const char *val = getenv( "MACRUNNER_HB_TRACE_IMAGE_API_BUDGET" );
    int limit = val && val[0] ? atoi(val) : 160;

    if (limit <= 0) return 1;
    if (count < limit)
    {
        count++;
        return 1;
    }
    if (count == limit)
    {
        count++;
        fprintf( stderr, "macrunner-hb-image-api: budget exhausted at %d entries, silencing\n", limit );
    }
    return 0;
}

static int macrunner_hb_trace_geometry_enabled(void);

static int macrunner_hb_trace_geometry_budget_allows(void)
{
    static int count;
    const char *val = getenv( "MACRUNNER_HB_TRACE_GEOMETRY_BUDGET" );
    int limit = val && val[0] ? atoi(val) : 120;

    if (limit <= 0) return 1;
    if (count < limit)
    {
        count++;
        return 1;
    }
    if (count == limit)
    {
        count++;
        fprintf( stderr, "macrunner-hb-geometry-api: budget exhausted at %d entries, silencing\n", limit );
    }
    return 0;
}

static BOOL macrunner_hb_trace_image_api_interesting( const struct macrunner_hb_import_thunk *thunk )
{
    if (!thunk) return FALSE;
    if (macrunner_hb_strieq( thunk->dll_name, "gdiplus.dll" ) &&
        !strncasecmp( thunk->import_name, "Gdip", 4 )) return TRUE;
    if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
        (macrunner_hb_strieq( thunk->import_name, "LoadImageW" ) ||
         macrunner_hb_strieq( thunk->import_name, "LoadBitmapW" ) ||
         macrunner_hb_strieq( thunk->import_name, "LoadIconW" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetDC" ) ||
         macrunner_hb_strieq( thunk->import_name, "ReleaseDC" ))) return TRUE;
    if (macrunner_hb_strieq( thunk->dll_name, "gdi32.dll" ) &&
        (macrunner_hb_strieq( thunk->import_name, "CreateDIBSection" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateCompatibleDC" ) ||
         macrunner_hb_strieq( thunk->import_name, "DeleteDC" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetDC" ) ||
         macrunner_hb_strieq( thunk->import_name, "ReleaseDC" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetDeviceCaps" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetObjectW" ))) return TRUE;
    if (macrunner_hb_strieq( thunk->dll_name, "windowscodecs.dll" ) ||
        macrunner_hb_strieq( thunk->dll_name, "windowscodecs" )) return TRUE;
    return FALSE;
}

static void macrunner_hb_trace_image_api( hb_context_t *ctx, const char *phase,
                                          const struct macrunner_hb_import_thunk *thunk,
                                          uint64_t ret_addr, uint64_t rc, const uint64_t args[12] )
{
    if (!macrunner_hb_trace_image_api_enabled() ||
        !macrunner_hb_trace_image_api_interesting( thunk ) ||
        !macrunner_hb_trace_image_api_budget_allows()) return;

    fprintf( stderr, "macrunner-hb-image-api: phase=%s import=%s!%s ret=%p rc=%p "
             "args=%p,%p,%p,%p,%p,%p last_error=%lu\n",
             phase, thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ret_addr,
             (void *)(uintptr_t)rc, (void *)(uintptr_t)args[0],
             (void *)(uintptr_t)args[1], (void *)(uintptr_t)args[2],
             (void *)(uintptr_t)args[3], (void *)(uintptr_t)args[4],
             (void *)(uintptr_t)args[5], (unsigned long)RtlGetLastWin32Error() );

    if (ctx && ctx->memory && macrunner_hb_strieq( thunk->dll_name, "gdi32.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "CreateDIBSection" ))
    {
        uint32_t size = 0, width = 0, height = 0, compression = 0, image_size = 0;
        uint16_t planes = 0, bit_count = 0;
        uint64_t bits = 0;

        hb_memory_read_u32( ctx->memory, (hb_gva_t)args[1] + 0, &size );
        hb_memory_read_u32( ctx->memory, (hb_gva_t)args[1] + 4, &width );
        hb_memory_read_u32( ctx->memory, (hb_gva_t)args[1] + 8, &height );
        hb_memory_read_u16( ctx->memory, (hb_gva_t)args[1] + 12, &planes );
        hb_memory_read_u16( ctx->memory, (hb_gva_t)args[1] + 14, &bit_count );
        hb_memory_read_u32( ctx->memory, (hb_gva_t)args[1] + 16, &compression );
        hb_memory_read_u32( ctx->memory, (hb_gva_t)args[1] + 20, &image_size );
        hb_memory_read_u64( ctx->memory, (hb_gva_t)args[3], &bits );
        fprintf( stderr, "macrunner-hb-image-api: dib phase=%s bmi=%p size=%u width=%d height=%d "
                 "planes=%u bpp=%u compression=%u image_size=%u bits_out=%p\n",
                 phase, (void *)(uintptr_t)args[1], size, (int32_t)width, (int32_t)height,
                 planes, bit_count, compression, image_size, (void *)(uintptr_t)bits );
    }
}

static BOOL macrunner_hb_trace_geometry_interesting( const struct macrunner_hb_import_thunk *thunk )
{
    if (!thunk || !macrunner_hb_strieq( thunk->dll_name, "user32.dll" )) return FALSE;
    return macrunner_hb_strieq( thunk->import_name, "CreateWindowExW" ) ||
           macrunner_hb_strieq( thunk->import_name, "SetWindowPos" ) ||
           macrunner_hb_strieq( thunk->import_name, "MoveWindow" ) ||
           macrunner_hb_strieq( thunk->import_name, "ShowWindow" ) ||
           macrunner_hb_strieq( thunk->import_name, "GetWindowRect" ) ||
           macrunner_hb_strieq( thunk->import_name, "GetClientRect" ) ||
           macrunner_hb_strieq( thunk->import_name, "GetWindowPlacement" ) ||
           macrunner_hb_strieq( thunk->import_name, "SetWindowPlacement" ) ||
           macrunner_hb_strieq( thunk->import_name, "AdjustWindowRectEx" ) ||
           macrunner_hb_strieq( thunk->import_name, "SystemParametersInfoW" ) ||
           macrunner_hb_strieq( thunk->import_name, "GetSystemMetrics" );
}

static void macrunner_hb_trace_geometry_api( const char *phase,
                                             const struct macrunner_hb_import_thunk *thunk,
                                             uint64_t ret_addr, uint64_t rc, const uint64_t args[12] )
{
    if (!macrunner_hb_trace_geometry_enabled() ||
        !macrunner_hb_trace_geometry_interesting( thunk ) ||
        !macrunner_hb_trace_geometry_budget_allows())
        return;

    fprintf( stderr, "macrunner-hb-geometry-api: phase=%s import=%s!%s ret=%p rc=%p "
             "args=%p,%p,%p,%p,%p,%p,%p,%p\n",
             phase ? phase : "?", thunk->dll_name, thunk->import_name,
             (void *)(uintptr_t)ret_addr, (void *)(uintptr_t)rc,
             (void *)(uintptr_t)args[0], (void *)(uintptr_t)args[1],
             (void *)(uintptr_t)args[2], (void *)(uintptr_t)args[3],
             (void *)(uintptr_t)args[4], (void *)(uintptr_t)args[5],
             (void *)(uintptr_t)args[6], (void *)(uintptr_t)args[7] );
}

static int macrunner_hb_trace_direct_native_budget_allows(void)
{
    static int count;
    const char *val = getenv( "MACRUNNER_HB_TRACE_DIRECT_NATIVE_BUDGET" );
    int limit = val && val[0] ? atoi(val) : 120;

    if (limit <= 0) return 1;
    if (count < limit)
    {
        count++;
        return 1;
    }
    if (count == limit)
    {
        count++;
        fprintf( stderr, "macrunner-hb-direct-native: budget exhausted at %d entries, silencing\n", limit );
    }
    return 0;
}

static int macrunner_hb_trace_file_api_budget_allows(void)
{
    static int count;
    const char *val = getenv( "MACRUNNER_HB_TRACE_FILE_API_BUDGET" );
    int limit = val && val[0] ? atoi(val) : 120;

    if (limit <= 0) return 1;
    if (count < limit)
    {
        count++;
        return 1;
    }
    if (count == limit)
    {
        count++;
        fprintf( stderr, "macrunner-hb-file-api: budget exhausted at %d entries, silencing\n", limit );
    }
    return 0;
}

static int macrunner_hb_trace_abi_budget_allows(void)
{
    static int count;
    const char *val = getenv( "MACRUNNER_HB_TRACE_ABI_BUDGET" );
    int limit = val && val[0] ? atoi(val) : 200;

    if (limit <= 0) return 1;
    if (count < limit)
    {
        count++;
        return 1;
    }
    if (count == limit)
    {
        count++;
        ERR( "macrunner-hb-abi: trace budget exhausted at %d entries, silencing\n", limit );
    }
    return 0;
}

static int macrunner_hb_trace_callback_abi_enabled(void)
{
    const char *val = getenv( "MACRUNNER_HB_TRACE_CALLBACK_ABI" );
    return val && val[0] && val[0] != '0';
}

static int macrunner_hb_trace_geometry_enabled(void)
{
    const char *val = getenv( "MACRUNNER_HB_TRACE_GEOMETRY" );
    return val && val[0] && val[0] != '0';
}

static int macrunner_hb_env_enabled( const char *name )
{
    const char *val = getenv( name );

    return val && val[0] && val[0] != '0';
}

static BOOL macrunner_hb_use_callback12_for_thunk( const struct macrunner_hb_import_thunk *thunk )
{
    if (!thunk || !thunk->pe_callback12) return FALSE;
    if (macrunner_hb_env_enabled( "MACRUNNER_HB_USE_CALLBACK12" )) return TRUE;
    if (macrunner_hb_env_enabled( "MACRUNNER_HB_DEFWINDOWPROC_CALLBACK12" ) &&
        macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "DefWindowProcW" ))
        return TRUE;
    return FALSE;
}

static void macrunner_hb_trace_local_rect( const char *label, uint64_t addr )
{
    RECT rect;

    if (!macrunner_hb_trace_geometry_enabled() || addr < 0x10000) return;
    if (!macrunner_hb_read_local_memory( (uintptr_t)addr, &rect, sizeof(rect) )) return;

    ERR( "macrunner-hb-geometry-local-rect: %s addr=%p rect=(%ld,%ld)-(%ld,%ld) size=%ldx%ld\n",
         label ? label : "?", (void *)(uintptr_t)addr, rect.left, rect.top,
         rect.right, rect.bottom, rect.right - rect.left, rect.bottom - rect.top );
}

static void macrunner_hb_trace_x64_callback_abi( const char *phase, uint64_t target,
                                                 const uint64_t args[8], uint64_t ret )
{
    static int budget = 80;
    uint64_t qwords[12] = {0};
    RECT rects[3];
    CREATESTRUCTW cs;
    size_t i;

    if (!macrunner_hb_trace_callback_abi_enabled()) return;
    if (!args || !budget) return;
    if (args[1] != WM_NCCREATE && args[1] != WM_NCCALCSIZE && args[1] != WM_CREATE &&
        args[1] != WM_SIZE && args[1] != WM_WINDOWPOSCHANGING && args[1] != WM_WINDOWPOSCHANGED &&
        !(macrunner_hb_trace_geometry_enabled() && args[1] == 0 && args[2] >= 0x10000))
        return;
    budget--;

    ERR( "macrunner-hb-callback-abi: phase=%s target=%p hwnd=%p msg=0x%llx "
         "wparam=%p lparam=%p ret=%p x4=%p x5=%p x6=%p x7=%p\n",
         phase, (void *)(uintptr_t)target, (void *)(uintptr_t)args[0],
         (unsigned long long)args[1], (void *)(uintptr_t)args[2], (void *)(uintptr_t)args[3],
         (void *)(uintptr_t)ret, (void *)(uintptr_t)args[4], (void *)(uintptr_t)args[5],
         (void *)(uintptr_t)args[6], (void *)(uintptr_t)args[7] );

    if (args[3] >= 0x10000 && macrunner_hb_read_local_memory( (uintptr_t)args[3], qwords, sizeof(qwords) ))
    {
        for (i = 0; i < ARRAY_SIZE(qwords); i++)
            ERR( "macrunner-hb-callback-abi-qword: phase=%s msg=0x%llx lparam+0x%zx=%p\n",
                 phase, (unsigned long long)args[1], i * sizeof(qwords[0]), (void *)(uintptr_t)qwords[i] );
    }

    if (args[1] == WM_NCCALCSIZE && args[3] >= 0x10000)
    {
        if (args[2] && macrunner_hb_read_local_memory( (uintptr_t)args[3], rects, sizeof(rects) ))
        {
            for (i = 0; i < ARRAY_SIZE(rects); i++)
                ERR( "macrunner-hb-callback-abi-nccalc: phase=%s rect%zu=(%ld,%ld)-(%ld,%ld)\n",
                     phase, i, rects[i].left, rects[i].top, rects[i].right, rects[i].bottom );
        }
        else if (!args[2] && macrunner_hb_read_local_memory( (uintptr_t)args[3], &rects[0], sizeof(rects[0]) ))
        {
            ERR( "macrunner-hb-callback-abi-nccalc: phase=%s rect=(%ld,%ld)-(%ld,%ld)\n",
                 phase, rects[0].left, rects[0].top, rects[0].right, rects[0].bottom );
        }
    }
    else if ((args[1] == WM_NCCREATE || args[1] == WM_CREATE) && args[3] >= 0x10000 &&
             macrunner_hb_read_local_memory( (uintptr_t)args[3], &cs, sizeof(cs) ))
    {
        ERR( "macrunner-hb-callback-abi-create: phase=%s create lpCreateParams=%p hInstance=%p "
             "menu=%p parent=%p cy=%ld cx=%ld y=%ld x=%ld style=%08lx name=%p class=%p ex=%08lx\n",
             phase, cs.lpCreateParams, cs.hInstance, cs.hMenu, cs.hwndParent,
             cs.cy, cs.cx, cs.y, cs.x, cs.style, cs.lpszName, cs.lpszClass, cs.dwExStyle );
    }
    else if (macrunner_hb_trace_geometry_enabled() && args[1] == 0 && args[2] >= 0x10000)
    {
        macrunner_hb_trace_local_rect( "callback.arg2.rect", args[2] );
        if (args[3] >= 0x10000)
            macrunner_hb_trace_local_rect( "callback.arg3.rect", args[3] );
    }
}

static void macrunner_hb_finish_import( hb_context_t *ctx, uint64_t ret_addr, uint64_t value )
{
    ctx->regs.x64.rax = value;
    ctx->regs.x64.rsp += 8;
    ctx->pc = ret_addr;
}

static struct macrunner_hb_import_thunk *macrunner_hb_find_import_thunk( uint64_t guest_target )
{
    unsigned int i;

    for (i = 0; i < macrunner_hb_import_count; i++)
        if (macrunner_hb_imports[i].guest_target == guest_target) return &macrunner_hb_imports[i];
    return NULL;
}

static struct macrunner_hb_import_thunk *macrunner_hb_find_import_thunk_by_target( uint64_t target )
{
    unsigned int i;

    for (i = 0; i < macrunner_hb_import_count; i++)
        if ((uint64_t)(uintptr_t)macrunner_hb_imports[i].target == target) return &macrunner_hb_imports[i];
    return NULL;
}

static struct macrunner_hb_import_thunk *macrunner_hb_find_import_thunk_by_name( const char *dll_name,
                                                                                 const char *import_name )
{
    unsigned int i;

    for (i = 0; i < macrunner_hb_import_count; i++)
    {
        if (macrunner_hb_strieq( macrunner_hb_imports[i].dll_name, dll_name ) &&
            macrunner_hb_strieq( macrunner_hb_imports[i].import_name, import_name ))
            return &macrunner_hb_imports[i];
    }
    return NULL;
}

static uint64_t macrunner_hb_register_dynamic_import_thunk( const struct macrunner_hb_import_thunk *source,
                                                            void *target, void *module_base,
                                                            const char *import_name )
{
    struct macrunner_hb_import_thunk *slot;
    char dll_name[96];
    unsigned int i;

    if (!source || !target || !module_base || !import_name) return 0;
    macrunner_hb_get_export_module_name( module_base, dll_name, sizeof(dll_name) );

    pthread_mutex_lock( &macrunner_hb_import_mutex );
    for (i = 0; i < macrunner_hb_import_count; i++)
    {
        slot = &macrunner_hb_imports[i];
        if (slot->target == target && slot->module_id == (uint64_t)(uintptr_t)module_base &&
            !strcmp( slot->dll_name, dll_name ) && !strcmp( slot->import_name, import_name ))
        {
            uint64_t guest_target = slot->guest_target;
            pthread_mutex_unlock( &macrunner_hb_import_mutex );
            return guest_target;
        }
    }

    if (macrunner_hb_import_count >= MACRUNNER_HB_IMPORT_MAX)
    {
        pthread_mutex_unlock( &macrunner_hb_import_mutex );
        return 0;
    }

    slot = &macrunner_hb_imports[macrunner_hb_import_count++];
    memset( slot, 0, sizeof(*slot) );
    slot->guest_target = MACRUNNER_HB_IMPORT_BASE + (uint64_t)macrunner_hb_import_count * MACRUNNER_HB_IMPORT_STRIDE;
    slot->target = target;
    slot->pe_call12 = source->pe_call12;
    slot->pe_callback12 = source->pe_callback12;
    slot->module_id = (uint64_t)(uintptr_t)module_base;
    slot->target_machine = current_machine;
    macrunner_hb_copy_cstr( slot->dll_name, sizeof(slot->dll_name), dll_name );
    macrunner_hb_copy_cstr( slot->import_name, sizeof(slot->import_name), import_name );
    TRACE( "MacRunner HyperBridge registered dynamic proc thunk %s!%s native=%p guest=%p\n",
           slot->dll_name, slot->import_name, slot->target, (void *)(uintptr_t)slot->guest_target );
    pthread_mutex_unlock( &macrunner_hb_import_mutex );
    return slot->guest_target;
}

NTSTATUS macrunner_hb_register_import_thunk( void *args )
{
    struct macrunner_hb_register_import_thunk_params *params = args;
    struct macrunner_hb_import_thunk *slot;
    unsigned int i;

    if (!params || !params->target) return STATUS_INVALID_PARAMETER;

    pthread_mutex_lock( &macrunner_hb_import_mutex );
    for (i = 0; i < macrunner_hb_import_count; i++)
    {
        slot = &macrunner_hb_imports[i];
        if (slot->target == params->target && slot->module_id == params->module_id &&
            !strcmp( slot->dll_name, params->dll_name ) &&
            !strcmp( slot->import_name, params->import_name ))
        {
            params->guest_target = slot->guest_target;
            pthread_mutex_unlock( &macrunner_hb_import_mutex );
            return STATUS_SUCCESS;
        }
    }

    if (macrunner_hb_import_count >= MACRUNNER_HB_IMPORT_MAX)
    {
        pthread_mutex_unlock( &macrunner_hb_import_mutex );
        return STATUS_NO_MEMORY;
    }

    slot = &macrunner_hb_imports[macrunner_hb_import_count++];
    memset( slot, 0, sizeof(*slot) );
    slot->guest_target = MACRUNNER_HB_IMPORT_BASE + (uint64_t)macrunner_hb_import_count * MACRUNNER_HB_IMPORT_STRIDE;
    slot->target = params->target;
    slot->pe_call12 = params->pe_call12;
    slot->pe_callback12 = params->pe_callback12;
    slot->module_id = params->module_id;
    slot->target_machine = params->target_machine;
    memcpy( slot->dll_name, params->dll_name, sizeof(slot->dll_name) - 1 );
    memcpy( slot->import_name, params->import_name, sizeof(slot->import_name) - 1 );
    params->guest_target = slot->guest_target;
    TRACE( "MacRunner HyperBridge registered import thunk %s!%s native=%p pe_call12=%p pe_callback12=%p guest=%p machine=%04x\n",
           slot->dll_name, slot->import_name, slot->target, slot->pe_call12, slot->pe_callback12,
           (void *)(uintptr_t)slot->guest_target, slot->target_machine );
    pthread_mutex_unlock( &macrunner_hb_import_mutex );
    return STATUS_SUCCESS;
}

static hb_result_t macrunner_hb_special_read( void *user, hb_gva_t addr, void *out, size_t size )
{
    struct macrunner_hb_special *special = user;
    unsigned char *dst = out;
    size_t i;

    if (!special) return HB_ERR_MEMORY_FAULT;
    if (addr + size > 0x1000)
    {
#ifdef __APPLE__
        mach_vm_address_t cur = (mach_vm_address_t)addr;
        size_t remaining = size;

        if (size && addr + size < addr) return HB_ERR_MEMORY_FAULT;
        while (remaining)
        {
            mach_vm_address_t region = cur;
            mach_vm_size_t region_size = 0;
            vm_region_basic_info_data_64_t info;
            mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t object = MACH_PORT_NULL;
            mach_vm_size_t copied = 0;
            mach_vm_size_t chunk;
            kern_return_t kr;

            kr = mach_vm_region( mach_task_self(), &region, &region_size, VM_REGION_BASIC_INFO_64,
                                 (vm_region_info_t)&info, &count, &object );
            if (object != MACH_PORT_NULL) mach_port_deallocate( mach_task_self(), object );
            if (kr != KERN_SUCCESS || !(info.protection & VM_PROT_READ))
            {
                macrunner_hb_trace_special_vm_fault( "read-region", addr, cur, remaining, kr,
                                                     region, region_size, info.protection );
                return HB_ERR_MEMORY_FAULT;
            }
            if (cur < region || cur >= region + region_size)
            {
                macrunner_hb_trace_special_vm_fault( "read-gap", addr, cur, remaining, kr,
                                                     region, region_size, info.protection );
                return HB_ERR_MEMORY_FAULT;
            }

            chunk = region + region_size - cur;
            if (chunk > remaining) chunk = remaining;
            if (!chunk) return HB_ERR_MEMORY_FAULT;
            kr = mach_vm_read_overwrite( mach_task_self(), cur, chunk,
                                         (mach_vm_address_t)(uintptr_t)dst, &copied );
            if (kr != KERN_SUCCESS || copied != chunk)
            {
                macrunner_hb_trace_special_vm_fault( "read-copy", addr, cur, remaining, kr,
                                                     region, region_size, info.protection );
                return HB_ERR_MEMORY_FAULT;
            }
            cur += chunk;
            dst += chunk;
            remaining -= chunk;
        }
        return HB_OK;
#else
        return HB_ERR_MEMORY_FAULT;
#endif
    }
    memset( out, 0, size );

    for (i = 0; i < size; i++)
    {
        hb_gva_t off = addr + i;
        if (off >= 0x30 && off < 0x38)
            dst[i] = ((unsigned char *)&special->teb)[off - 0x30];
        else if (off >= 0x60 && off < 0x68)
            dst[i] = ((unsigned char *)&special->peb)[off - 0x60];
        else
            return HB_ERR_MEMORY_FAULT;
    }
    return HB_OK;
}

static hb_result_t macrunner_hb_special_write( void *user, hb_gva_t addr, const void *in, size_t size )
{
    if (!user || !in) return HB_ERR_MEMORY_FAULT;
    if (addr + size <= 0x1000) return HB_ERR_MEMORY_FAULT;
    if (macrunner_hb_env_enabled( "MACRUNNER_HB_TRACE_NATIVE_WRITES" ) &&
        addr >= 0x7ffd0000000ULL && addr < 0x7ffe0000000ULL)
    {
        uint64_t sample = 0;
        size_t copy = size < sizeof(sample) ? size : sizeof(sample);
        if (copy) memcpy( &sample, in, copy );
        fprintf( stderr,
                 "macrunner-hb-native-write: path=special_write addr=0x%llx size=%zu sample=0x%llx\n",
                 (unsigned long long)addr, size, (unsigned long long)sample );
    }
#ifdef __APPLE__
    {
        const unsigned char *src = in;
        mach_vm_address_t cur = (mach_vm_address_t)addr;
        size_t remaining = size;

        /*
         * The live VM map is a snapshot taken when the x64 bridge starts.
         * Native Wine calls such as HeapAlloc/LocalAlloc can create fresh
         * writable ranges after that point; validate those ranges against the
         * kernel before allowing x64 code to write through returned pointers.
         */
        if (size && addr + size < addr) return HB_ERR_MEMORY_FAULT;
        while (remaining)
        {
            mach_vm_address_t region = cur;
            mach_vm_size_t region_size = 0;
            vm_region_basic_info_data_64_t info;
            mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t object = MACH_PORT_NULL;
            mach_vm_size_t chunk;
            kern_return_t kr;

            kr = mach_vm_region( mach_task_self(), &region, &region_size, VM_REGION_BASIC_INFO_64,
                                 (vm_region_info_t)&info, &count, &object );
            if (object != MACH_PORT_NULL) mach_port_deallocate( mach_task_self(), object );
            if (kr != KERN_SUCCESS || !(info.protection & VM_PROT_WRITE))
            {
                macrunner_hb_trace_special_vm_fault( "write-region", addr, cur, remaining, kr,
                                                     region, region_size, info.protection );
                return HB_ERR_MEMORY_FAULT;
            }
            if (cur < region || cur >= region + region_size)
            {
                macrunner_hb_trace_special_vm_fault( "write-gap", addr, cur, remaining, kr,
                                                     region, region_size, info.protection );
                return HB_ERR_MEMORY_FAULT;
            }

            chunk = region + region_size - cur;
            if (chunk > remaining) chunk = remaining;
            if (!chunk || (mach_msg_type_number_t)chunk != chunk) return HB_ERR_MEMORY_FAULT;
            kr = mach_vm_write( mach_task_self(), cur, (vm_offset_t)(uintptr_t)src,
                                (mach_msg_type_number_t)chunk );
            if (kr != KERN_SUCCESS)
            {
                macrunner_hb_trace_special_vm_fault( "write-copy", addr, cur, remaining, kr,
                                                     region, region_size, info.protection );
                return HB_ERR_MEMORY_FAULT;
            }
            cur += chunk;
            src += chunk;
            remaining -= chunk;
        }
        return HB_OK;
    }
#else
    return HB_ERR_MEMORY_FAULT;
#endif
}

static hb_result_t macrunner_hb_map_live_address_space( hb_memory_t *mem )
{
#ifdef __APPLE__
    mach_vm_address_t addr = 0;
    const mach_vm_address_t max_addr = 0x00007fffffff0000ULL;
    mach_port_t task = mach_task_self();
    unsigned int mapped = 0;

    /* hb_memory_map(base != 0) records an existing live range without mmap().
     * Mapping the entire user range made holes look readable and let direct
     * memcpy fault on macOS high-address stack probes. Enumerate the real VM
     * map instead, so unmapped holes return HB_ERR_MEMORY_FAULT cleanly. */
    while (addr < max_addr)
    {
        mach_vm_address_t region = addr;
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        hb_perm_t perm = 0;
        kern_return_t kr;

        kr = mach_vm_region( task, &region, &size, VM_REGION_BASIC_INFO_64,
                             (vm_region_info_t)&info, &count, &object );
        if (object != MACH_PORT_NULL) mach_port_deallocate( task, object );
        if (kr != KERN_SUCCESS) break;

        if (region >= max_addr) break;
        if (region + size < region) break;
        if (region + size > max_addr) size = max_addr - region;

        if (info.protection & VM_PROT_READ) perm |= HB_PERM_READ;
        if (info.protection & VM_PROT_WRITE) perm |= HB_PERM_WRITE;
        if (info.protection & VM_PROT_EXECUTE) perm |= HB_PERM_EXEC;
        if (perm && hb_memory_map( mem, (hb_gva_t)region, (size_t)size, perm ) == HB_OK)
            mapped++;

        addr = region + size;
        if (addr <= region) break;
    }
    if (macrunner_hb_debug_enabled())
        ERR( "MacRunner HyperBridge mapped %u live VM regions\n", mapped );
    else
        TRACE( "MacRunner HyperBridge mapped %u live VM regions\n", mapped );
    return mapped ? HB_OK : HB_ERR_MEMORY_FAULT;
#else
    return hb_memory_map( mem, 0x10000, 0x7fffffff0000ULL - 0x10000,
                          HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC );
#endif
}

static hb_result_t macrunner_hb_mark_x64_image_exec_sections( hb_memory_t *mem, void *image_base )
{
    IMAGE_NT_HEADERS *nt = macrunner_hb_image_nt_header( image_base );
    IMAGE_SECTION_HEADER *sec;
    hb_result_t ret = HB_OK;
    unsigned int i;

    if (!mem || !image_base || !nt || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
        return HB_OK;

    sec = IMAGE_FIRST_SECTION( nt );
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
    {
        uint64_t base;
        size_t size;

        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        size = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        if (!size) continue;
        base = (uint64_t)(uintptr_t)image_base + sec->VirtualAddress;

        /*
         * Bug #7 design split: guest x64 execute permission belongs to
         * HyperBridge and comes from PE section metadata. Host ARM64 execute
         * permission is intentionally stripped in virtual.c so a direct BLR to
         * x64 bytes traps into the signal bridge instead of being decoded as
         * accidental ARM64 instructions.
         */
        ret = hb_memory_protect( mem, (hb_gva_t)base, size, HB_PERM_READ | HB_PERM_EXEC );
        if (ret != HB_OK) return ret;
    }
    return ret;
}

static hb_result_t macrunner_hb_lift_one_block( uint64_t pc, hb_ir_func_t **func )
{
    hb_decoder_t *dec;
    hb_result_t ret;

    dec = hb_decoder_create( HB_ARCH_X64, (const uint8_t *)(uintptr_t)pc, 256, pc );
    if (!dec) return HB_ERR_OUT_OF_MEMORY;
    ret = hb_lift_func_x64( dec, func );
    hb_decoder_destroy( dec );
    return ret;
}

static USHORT macrunner_hb_module_machine( void *module )
{
    const IMAGE_DOS_HEADER *dos = module;
    const IMAGE_NT_HEADERS *nt;

    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return IMAGE_FILE_MACHINE_UNKNOWN;
    nt = (const IMAGE_NT_HEADERS *)((const char *)module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return IMAGE_FILE_MACHINE_UNKNOWN;
    return nt->FileHeader.Machine;
}

static uint64_t macrunner_hb_module_size( void *module )
{
    const IMAGE_DOS_HEADER *dos = module;
    const IMAGE_NT_HEADERS *nt;

    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (const IMAGE_NT_HEADERS *)((const char *)module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return nt->OptionalHeader.SizeOfImage;
}

static size_t macrunner_hb_x64_stack_size( void *module )
{
    IMAGE_NT_HEADERS *nt = macrunner_hb_image_nt_header( module );
    const char *env = getenv( "MACRUNNER_HB_X64_STACK_SIZE" );
    size_t size = 4 * 1024 * 1024;

    if (nt && nt->OptionalHeader.SizeOfStackReserve > size)
        size = nt->OptionalHeader.SizeOfStackReserve;

    if (env && *env)
    {
        unsigned long long requested = strtoull( env, NULL, 0 );
        if (requested >= 0x10000 && requested <= 0x10000000ULL)
            size = requested;
    }

    if (size < 4 * 1024 * 1024) size = 4 * 1024 * 1024;
    if (size > 64 * 1024 * 1024) size = 64 * 1024 * 1024;
    return (size + 0xffff) & ~(size_t)0xffff;
}

static int macrunner_hb_pc_in_executable_section( void *module, uint64_t pc )
{
    IMAGE_NT_HEADERS *nt = macrunner_hb_image_nt_header( module );
    IMAGE_SECTION_HEADER *sec;
    uint64_t base = (uint64_t)(uintptr_t)module;
    DWORD rva;
    unsigned int i;

    if (!nt || pc < base) return FALSE;
    rva = (DWORD)(pc - base);
    sec = IMAGE_FIRST_SECTION( nt );
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
    {
        DWORD start = sec->VirtualAddress;
        DWORD size = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        if (rva >= start && rva < start + size) return TRUE;
    }
    return FALSE;
}

static void *macrunner_hb_export_by_ordinal( void *module, WORD ordinal )
{
    const IMAGE_DOS_HEADER *dos = module;
    const IMAGE_NT_HEADERS *nt;
    const IMAGE_DATA_DIRECTORY *dir;
    const IMAGE_EXPORT_DIRECTORY *exports;
    const DWORD *functions;
    DWORD index, rva;

    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    nt = (const IMAGE_NT_HEADERS *)((const char *)module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir->VirtualAddress || !dir->Size) return NULL;

    exports = (const IMAGE_EXPORT_DIRECTORY *)((const char *)module + dir->VirtualAddress);
    if (ordinal < exports->Base) return NULL;
    index = ordinal - exports->Base;
    if (index >= exports->NumberOfFunctions) return NULL;

    functions = (const DWORD *)((const char *)module + exports->AddressOfFunctions);
    rva = functions[index];
    if (!rva) return NULL;
    return (char *)module + rva;
}

static int macrunner_hb_pc_in_image( uint64_t pc, uint64_t image_base, uint64_t image_size )
{
    if (!image_base || !image_size) return 1;
    return pc >= image_base && pc < image_base + image_size;
}

int macrunner_hb_pc_is_x64_guest_code( void *pc )
{
    TEB *teb = NtCurrentTeb();
    void *image_base, *module;
    uint64_t image_size;

    /* A guest image view is not automatically executable code: PE headers and
     * .rdata live in the same view and can fault for ordinary data reasons.
     * Host VM protections are deliberately non-executable for x64 guest images,
     * so routing must use PE section metadata rather than mutable page execute
     * state.  The registered range path covers x64 DLLs and relocated high
     * images; the PEB fallback keeps the main image available during bootstrap. */
    if (macrunner_hb_is_current_x64_guest_exec_address( pc )) return TRUE;
    if (macrunner_hb_is_registered_x64_guest_address( pc ) &&
        (module = macrunner_hb_module_from_pc( pc )) &&
        macrunner_hb_module_machine( module ) == IMAGE_FILE_MACHINE_AMD64)
        return macrunner_hb_pc_in_executable_section( module, (uint64_t)(uintptr_t)pc );

    if (!pc || !teb || !teb->Peb) return FALSE;
    image_base = teb->Peb->ImageBaseAddress;
    if (macrunner_hb_module_machine( image_base ) != IMAGE_FILE_MACHINE_AMD64) return FALSE;
    image_size = macrunner_hb_module_size( image_base );
    return macrunner_hb_pc_in_image( (uint64_t)(uintptr_t)pc,
                                     (uint64_t)(uintptr_t)image_base, image_size ) &&
           macrunner_hb_pc_in_executable_section( image_base, (uint64_t)(uintptr_t)pc );
}

static int macrunner_hb_decode_spans_pc( uint64_t candidate, uint64_t pc,
                                         uint64_t image_base, uint64_t image_size,
                                         hb_decoded_t *decoded )
{
    uint64_t image_end = image_base + image_size;
    size_t len;

    if (!candidate || candidate >= pc) return FALSE;
    if (candidate < image_base || candidate >= image_end) return FALSE;
    len = (size_t)(image_end - candidate);
    if (len > 15) len = 15;
    if (!len) return FALSE;

    memset( decoded, 0, sizeof(*decoded) );
    if (hb_decode_x64( (const uint8_t *)(uintptr_t)candidate, len, candidate, decoded ) != HB_OK)
        return FALSE;
    return decoded->len && candidate + decoded->len > pc;
}

static int macrunner_hb_decode_ends_at_pc( uint64_t candidate, uint64_t pc,
                                           uint64_t image_base, uint64_t image_size,
                                           hb_decoded_t *decoded )
{
    uint64_t image_end = image_base + image_size;
    size_t len;

    if (!candidate || candidate >= pc) return FALSE;
    if (candidate < image_base || candidate >= image_end) return FALSE;
    len = (size_t)(image_end - candidate);
    if (len > 15) len = 15;
    if (!len) return FALSE;

    memset( decoded, 0, sizeof(*decoded) );
    if (hb_decode_x64( (const uint8_t *)(uintptr_t)candidate, len, candidate, decoded ) != HB_OK)
        return FALSE;
    return decoded->len && candidate + decoded->len == pc;
}

static int macrunner_hb_is_stack_boundary_prologue( const hb_decoded_t *decoded )
{
    if (!decoded) return FALSE;

    /* macOS can report a fault PC one ARM instruction (4 bytes) after the
     * original x64 callback target when the first x64 bytes were fetched as
     * an ARM64 instruction.  If those skipped x64 bytes are the function stack
     * prologue, starting HyperBridge at the reported PC creates an impossible
     * x64 stack: the epilogue later adds back space that was never subtracted
     * and RET consumes a stack argument as a return address. */
    if (decoded->opcode == HB_INS_SUB &&
        decoded->op1.present && decoded->op1.is_reg && decoded->op1.reg == HB_REG_RSP &&
        decoded->op2.present && decoded->op2.is_imm)
        return TRUE;

    if (decoded->opcode == HB_INS_PUSH)
        return TRUE;

    if (decoded->opcode == HB_INS_AND &&
        decoded->op1.present && decoded->op1.is_reg && decoded->op1.reg == HB_REG_RSP &&
        decoded->op2.present && decoded->op2.is_imm)
        return TRUE;

    return FALSE;
}

static int macrunner_hb_find_amd64_function_begin( uint64_t pc, void *image_base_ptr,
                                                   uint64_t image_size, uint64_t *begin,
                                                   uint64_t *end )
{
    IMAGE_DATA_DIRECTORY *dir;
    IMAGE_NT_HEADERS *nt;
    IMAGE_AMD64_RUNTIME_FUNCTION_ENTRY *funcs;
    uint64_t image_base = (uint64_t)(uintptr_t)image_base_ptr;
    DWORD rva;
    unsigned int count, i;

    if (begin) *begin = 0;
    if (end) *end = 0;
    if (!pc || !image_base_ptr || pc < image_base || pc >= image_base + image_size)
        return FALSE;

    nt = macrunner_hb_image_nt_header( image_base_ptr );
    if (!nt || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return FALSE;
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!dir->VirtualAddress || dir->Size < sizeof(*funcs)) return FALSE;
    if ((uint64_t)dir->VirtualAddress + dir->Size > image_size) return FALSE;

    rva = (DWORD)(pc - image_base);
    funcs = (IMAGE_AMD64_RUNTIME_FUNCTION_ENTRY *)((char *)image_base_ptr + dir->VirtualAddress);
    count = dir->Size / sizeof(*funcs);
    for (i = 0; i < count; i++)
    {
        if (!funcs[i].BeginAddress || funcs[i].EndAddress <= funcs[i].BeginAddress)
            continue;
        if (rva >= funcs[i].BeginAddress && rva < funcs[i].EndAddress)
        {
            if (begin) *begin = image_base + funcs[i].BeginAddress;
            if (end) *end = image_base + funcs[i].EndAddress;
            return TRUE;
        }
    }
    return FALSE;
}

ULONG64 macrunner_hb_normalize_x64_callback_pc( ULONG64 pc )
{
    TEB *teb = NtCurrentTeb();
    void *image_base_ptr;
    uint64_t image_base, image_size, func_begin = 0, func_end = 0;
    hb_decoded_t decoded, current, best, boundary_best;
    uint64_t candidate, best_pc = 0;
    uint64_t boundary_pc = 0;
    unsigned int current_len = 0;
    unsigned int back;
    size_t len;

    if (!pc || !teb || !teb->Peb) return pc;
    image_base_ptr = teb->Peb->ImageBaseAddress;
    if (macrunner_hb_module_machine( image_base_ptr ) != IMAGE_FILE_MACHINE_AMD64) return pc;
    image_base = (uint64_t)(uintptr_t)image_base_ptr;
    image_size = macrunner_hb_module_size( image_base_ptr );
    if (!macrunner_hb_pc_in_image( pc, image_base, image_size )) return pc;

    /* macOS reports the ARM64 fault PC after a failed 4-byte instruction
     * fetch.  When native ARM64 code accidentally branches to x64 bytes,
     * the reported PC can therefore land in the middle of the x64
     * instruction.  Route Phase F callbacks from the x64 instruction
     * boundary, not from the ARM64 fetch residue. */
    len = (size_t)(image_base + image_size - pc);
    if (len > 15) len = 15;
    memset( &current, 0, sizeof(current) );
    if (len && hb_decode_x64( (const uint8_t *)(uintptr_t)pc, len, pc, &current ) == HB_OK)
        current_len = current.len;

    if (macrunner_hb_find_amd64_function_begin( pc, image_base_ptr, image_size,
                                                &func_begin, &func_end ) &&
        func_begin && func_begin < pc && pc - func_begin <= 0x40)
    {
        TRACE( "MacRunner HyperBridge normalized x64 callback pc=%p -> %p "
               "pdata-begin end=%p current_len=%u\n",
               (void *)(uintptr_t)pc, (void *)(uintptr_t)func_begin,
               (void *)(uintptr_t)func_end, current_len );
        return func_begin;
    }

    memset( &boundary_best, 0, sizeof(boundary_best) );
    for (back = 1; back <= 14 && pc >= back; back++)
    {
        candidate = pc - back;
        if (!macrunner_hb_decode_ends_at_pc( candidate, pc, image_base, image_size, &decoded ) ||
            !macrunner_hb_is_stack_boundary_prologue( &decoded ))
            continue;
        if (!boundary_pc || decoded.len > boundary_best.len)
        {
            boundary_pc = candidate;
            boundary_best = decoded;
        }
    }
    if (boundary_pc)
    {
        TRACE( "MacRunner HyperBridge normalized x64 callback pc=%p -> %p "
               "stack-boundary len=%u opcode=%s current_len=%u\n",
               (void *)(uintptr_t)pc, (void *)(uintptr_t)boundary_pc,
               boundary_best.len, hb_opcode_name( boundary_best.opcode ), current_len );
        return boundary_pc;
    }

    memset( &best, 0, sizeof(best) );
    for (back = 1; back <= 14 && pc >= back; back++)
    {
        candidate = pc - back;
        if (!macrunner_hb_decode_spans_pc( candidate, pc, image_base, image_size, &decoded ))
            continue;
        if (!best_pc || decoded.len > best.len)
        {
            best_pc = candidate;
            best = decoded;
        }
    }

    if (best_pc && best.len > current_len)
    {
        TRACE( "MacRunner HyperBridge normalized x64 callback pc=%p -> %p len=%u opcode=%s current_len=%u\n",
               (void *)(uintptr_t)pc, (void *)(uintptr_t)best_pc, best.len,
               hb_opcode_name( best.opcode ), current_len );
        return best_pc;
    }
    return pc;
}

static hb_result_t macrunner_hb_setup_bridge_stack( hb_context_t *ctx, size_t stack_size,
                                                    void **stack_base )
{
    void *stack = NULL;
    SIZE_T size = stack_size + MACRUNNER_HB_SEH_STACK_SLACK;
    char *usable_stack;
    NTSTATUS status;
    hb_result_t ret;

    if (!ctx || !ctx->memory || !stack_base) return HB_ERR_INVALID_ARG;
    *stack_base = NULL;

    /* This stack is also passed to ARM64 PE exports through thunked x64
     * imports.  It must therefore be a Wine VAD, not just a host mmap, or
     * PE-side ntdll buffer validation rejects otherwise CPU-valid pointers. */
    status = NtAllocateVirtualMemory( NtCurrentProcess(), &stack, 0, &size,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    if (status) return HB_ERR_OUT_OF_MEMORY;

    ret = hb_memory_map( ctx->memory, (hb_gva_t)(uintptr_t)stack, size,
                         HB_PERM_READ | HB_PERM_WRITE );
    if (ret != HB_OK)
    {
        SIZE_T free_size = 0;
        NtFreeVirtualMemory( NtCurrentProcess(), &stack, &free_size, MEM_RELEASE );
        return ret;
    }

    usable_stack = (char *)stack + MACRUNNER_HB_SEH_STACK_SLACK;
    ctx->memory->stack_bottom = (hb_gva_t)(uintptr_t)usable_stack;
    ctx->memory->stack_top = ctx->memory->stack_bottom + stack_size;
    ctx->regs.x64.rsp = ctx->memory->stack_top;
    *stack_base = stack;
    return HB_OK;
}

static void macrunner_hb_prepare_arm64_pe_call(void)
{
#ifdef __aarch64__
    TEB *teb = NtCurrentTeb();

    /* PE ARM64 code uses x18 as the TEB.  This direct Unix-side import path
     * bypasses Wine's normal PE entry trampolines, so hydrate x18 before
     * branching into the native ARM64 implementation. */
    __asm__ volatile( "mov x18, %0" :: "r"(teb) : "memory" );
#endif
}

#ifdef __aarch64__
extern uint64_t macrunner_hb_arm64_pe_call12( void *target, const uint64_t *args,
                                              void *stack_top, TEB *teb );
extern NTSTATUS call_user_mode_callback( ULONG64 user_sp, void **ret_ptr, ULONG *ret_len,
                                         void *func, TEB *teb );

struct macrunner_hb_pe_callback12_frame
{
    void *target;
    const uint64_t *args;
    uint64_t ret;
};

__ASM_GLOBAL_FUNC( macrunner_hb_arm64_pe_call12,
                   "stp x29, x30, [sp, #-0xd0]!\n\t"
                   "mov x29, sp\n\t"
                   "stp x19, x20, [x29, #0x10]\n\t"
                   "stp x21, x22, [x29, #0x20]\n\t"
                   "stp x23, x24, [x29, #0x30]\n\t"
                   "stp x25, x26, [x29, #0x40]\n\t"
                   "stp x27, x28, [x29, #0x50]\n\t"
                   "stp d8,  d9,  [x29, #0x60]\n\t"
                   "stp d10, d11, [x29, #0x70]\n\t"
                   "stp d12, d13, [x29, #0x80]\n\t"
                   "stp d14, d15, [x29, #0x90]\n\t"
                   "mov x20, x0\n\t"         /* target */
                   "mov x21, x1\n\t"         /* args[12] */
                   "mov x22, x2\n\t"         /* PE-compatible scratch stack */
                   "mov x19, x3\n\t"         /* TEB */
                   "mov x18, x19\n\t"        /* Windows ARM64 TEB */
                   "ldr x7, [x19, #0x378]\n\t"   /* previous thread_data->syscall_frame */
                   "str x7, [x29, #0xa0]\n\t"
                   "mrs x8, fpcr\n\t"
                   "mrs x9, fpsr\n\t"
                   "bfi x8, x9, #0, #32\n\t"
                   "ldr x9, [x19]\n\t"            /* teb->Tib.ExceptionList */
                   "stp x8, x9, [x29, #0xb0]\n\t"
                   "sub x4, sp, #0x330\n\t"      /* nested syscall_frame on Unix stack */
                   "str x4, [x19, #0x378]\n\t"   /* thread_data->syscall_frame */
                   "str x19, [x4, #0x90]\n\t"    /* frame->x18 / TEB */
                   "str x30, [x4, #0xf0]\n\t"    /* frame->lr */
                   "str x22, [x4, #0xf8]\n\t"    /* frame->sp for nested KeUserModeCallback */
                   "str x20, [x4, #0x100]\n\t"   /* frame->pc / native target */
                   "str wzr, [x4, #0x10c]\n\t"   /* frame->restore_flags */
                   "add x8, x29, #0xd0\n\t"
                   "stp x7, x8, [x4, #0x110]\n\t" /* frame->prev_frame, syscall_cfa */
                   "mov sp, x22\n\t"
                   "ldr x12, [x21, #64]\n\t"
                   "str x12, [sp, #0]\n\t"
                   "ldr x12, [x21, #72]\n\t"
                   "str x12, [sp, #8]\n\t"
                   "ldr x12, [x21, #80]\n\t"
                   "str x12, [sp, #16]\n\t"
                   "ldr x12, [x21, #88]\n\t"
                   "str x12, [sp, #24]\n\t"
                   "ldr x0, [x21, #0]\n\t"
                   "ldr x1, [x21, #8]\n\t"
                   "ldr x2, [x21, #16]\n\t"
                   "ldr x3, [x21, #24]\n\t"
                   "ldr x4, [x21, #32]\n\t"
                   "ldr x5, [x21, #40]\n\t"
                   "ldr x6, [x21, #48]\n\t"
                   "ldr x7, [x21, #56]\n\t"
                   "blr x20\n\t"
                   "mov x23, x0\n\t"
                   "mov sp, x29\n\t"
                   "ldr x7, [x29, #0xa0]\n\t"
                   "str x7, [x19, #0x378]\n\t"   /* restore previous syscall_frame */
                   "ldp x8, x9, [x29, #0xb0]\n\t"
                   "str x9, [x19]\n\t"            /* restore teb->Tib.ExceptionList */
                   "msr fpcr, x8\n\t"
                   "lsr x8, x8, #32\n\t"
                   "msr fpsr, x8\n\t"
                   "mov x0, x23\n\t"
                   "ldp d14, d15, [x29, #0x90]\n\t"
                   "ldp d12, d13, [x29, #0x80]\n\t"
                   "ldp d10, d11, [x29, #0x70]\n\t"
                   "ldp d8,  d9,  [x29, #0x60]\n\t"
                   "ldp x27, x28, [x29, #0x50]\n\t"
                   "ldp x25, x26, [x29, #0x40]\n\t"
                   "ldp x23, x24, [x29, #0x30]\n\t"
                   "ldp x21, x22, [x29, #0x20]\n\t"
                   "ldp x19, x20, [x29, #0x10]\n\t"
                   "ldp x29, x30, [sp], #0xd0\n\t"
                   "ret" )
#endif

static uint64_t macrunner_hb_call_arm64_pe_import12( const struct macrunner_hb_import_thunk *thunk,
                                                     const uint64_t args[12] )
{
#ifdef __aarch64__
    TEB *teb = NtCurrentTeb();
    void *target = thunk ? thunk->target : NULL;
    void *old_base;
    void *old_limit;
    void *restore_base;
    void *restore_limit;
    void *old_kernel_stack;
    uintptr_t stack_top;
    uint64_t ret;
    void *ret_ptr;
    ULONG ret_len;
    NTSTATUS status;

    if (!target) return 0;
    if (!teb || !macrunner_hb_bridge_stack_base || !macrunner_hb_bridge_stack_limit ||
        macrunner_hb_bridge_stack_size < 0x2000)
    {
        typedef uint64_t (*macrunner_hb_native_fn12)( uint64_t, uint64_t, uint64_t, uint64_t,
                                                      uint64_t, uint64_t, uint64_t, uint64_t,
                                                      uint64_t, uint64_t, uint64_t, uint64_t );

        macrunner_hb_prepare_arm64_pe_call();
        return ((macrunner_hb_native_fn12)target)( args[0], args[1], args[2], args[3],
                                                   args[4], args[5], args[6], args[7],
                                                   args[8], args[9], args[10], args[11] );
    }

    restore_base = teb->Tib.StackBase;
    restore_limit = teb->Tib.StackLimit;
    old_base = macrunner_hb_original_stack_base ? macrunner_hb_original_stack_base : restore_base;
    old_limit = macrunner_hb_original_stack_limit ? macrunner_hb_original_stack_limit : restore_limit;
    old_kernel_stack = ntdll_get_thread_data()->kernel_stack;
    {
        uintptr_t bridge_low = (uintptr_t)macrunner_hb_bridge_stack_base;
        uintptr_t bridge_high = bridge_low + macrunner_hb_bridge_stack_size;
        uintptr_t guest_rsp = macrunner_hb_native_call_guest_rsp;

        /*
         * The translated x64 code and the temporary ARM64 PE call both live
         * on the same Wine VAD so native user32/kernelbase can validate guest
         * stack pointers.  Do not place the ARM64 call frame at the top of the
         * bridge stack: nested native calls from deep x64 frames would overwrite
         * saved x64 return addresses.  Instead use free stack space below the
         * current x64 RSP, leaving a guard gap for the guest frame above it.
         */
        if (guest_rsp > bridge_low + 0x8000 && guest_rsp < bridge_high)
            stack_top = guest_rsp - 0x4000;
        else
            stack_top = bridge_high - 0x200;
        stack_top &= ~(uintptr_t)0xf;
    }

    /*
     * Native ARM64 PE user32/ntdll can raise and dispatch callbacks while the
     * call is using our bridge stack.  Publish a merged TEB stack range so both
     * pre-existing SEH frames and temporary callback frames validate.
     *
     * Keep thread_data->kernel_stack on Wine's real syscall stack. Publishing
     * the bridge low-water mark there makes native CRT/user32 unwinding treat
     * this thunk frame like a syscall frame and can raise bogus native
     * exceptions (for example STATUS_INVALID_HANDLE from msvcrt!time).
     */
    if (old_limit && old_base && old_limit < old_base)
    {
        uintptr_t old_limit_addr = (uintptr_t)old_limit;
        uintptr_t old_base_addr = (uintptr_t)old_base;
        uintptr_t bridge_limit_addr = (uintptr_t)macrunner_hb_bridge_stack_limit;
        uintptr_t bridge_base_addr = (uintptr_t)macrunner_hb_bridge_stack_base +
                                     macrunner_hb_bridge_stack_size;

        uintptr_t merged_limit = old_limit_addr < bridge_limit_addr ?
                                 old_limit_addr : bridge_limit_addr;
        uintptr_t merged_base = old_base_addr > bridge_base_addr ?
                                old_base_addr : bridge_base_addr;
        uintptr_t seh_frame = (uintptr_t)teb->Tib.ExceptionList;

        if (seh_frame && seh_frame != ~(uintptr_t)0 && seh_frame < merged_limit &&
            merged_limit - seh_frame <= MACRUNNER_HB_SEH_STACK_SLACK)
            merged_limit = seh_frame & ~(uintptr_t)(MACRUNNER_HB_SEH_STACK_SLACK - 1);
        teb->Tib.StackLimit = (void *)merged_limit;
        teb->Tib.StackBase = (void *)merged_base;
    }
    else
    {
        teb->Tib.StackLimit = macrunner_hb_bridge_stack_limit;
        teb->Tib.StackBase = (char *)macrunner_hb_bridge_stack_base + macrunner_hb_bridge_stack_size;
    }
    TRACE( "MacRunner HyperBridge PE call stack bounds %s!%s old=%p-%p old_kernel=%p bridge=%p-%p applied=%p-%p kernel=%p guest_rsp=%p native_sp=%p seh=%p\n",
           thunk ? thunk->dll_name : "?", thunk ? thunk->import_name : "?",
           old_limit, old_base, old_kernel_stack, macrunner_hb_bridge_stack_limit,
           (char *)macrunner_hb_bridge_stack_base + macrunner_hb_bridge_stack_size,
           teb->Tib.StackLimit, teb->Tib.StackBase, ntdll_get_thread_data()->kernel_stack,
           (void *)macrunner_hb_native_call_guest_rsp, (void *)stack_top, teb->Tib.ExceptionList );
    if (macrunner_hb_use_callback12_for_thunk( thunk ))
    {
        struct macrunner_hb_pe_callback12_frame *frame;

        stack_top -= sizeof(*frame);
        stack_top &= ~(uintptr_t)0xf;
        frame = (struct macrunner_hb_pe_callback12_frame *)stack_top;
        frame->target = target;
        frame->args = args;
        frame->ret = 0;
        ret_ptr = NULL;
        ret_len = 0;
        macrunner_hb_prepare_arm64_pe_call();
        status = call_user_mode_callback( stack_top, &ret_ptr, &ret_len, thunk->pe_callback12, teb );
        ret = (!status && ret_ptr && ret_len >= sizeof(ret)) ? *(uint64_t *)ret_ptr : frame->ret;
    }
    else if (thunk && thunk->pe_call12)
    {
        macrunner_hb_prepare_arm64_pe_call();
        ret = macrunner_hb_arm64_pe_call12( target, args, (void *)stack_top, teb );
    }
    else
    {
        macrunner_hb_prepare_arm64_pe_call();
        ret = macrunner_hb_arm64_pe_call12( target, args, (void *)stack_top, teb );
    }
    macrunner_hb_prepare_arm64_pe_call();
    teb->Tib.StackBase = restore_base;
    teb->Tib.StackLimit = restore_limit;
    ntdll_get_thread_data()->kernel_stack = old_kernel_stack;
    return ret;
#else
    (void)target;
    (void)args;
    return 0;
#endif
}

static void macrunner_hb_trace_abi_stack( hb_context_t *ctx, const char *phase,
                                          const struct macrunner_hb_import_thunk *thunk,
                                          uint64_t ret_addr, const uint64_t args[12] );
static void macrunner_hb_trace_abi_return( hb_context_t *ctx,
                                           const struct macrunner_hb_import_thunk *thunk,
                                           uint64_t ret_addr, uint64_t value,
                                           const uint64_t args[12] );
static void macrunner_hb_normalize_import_args( const struct macrunner_hb_import_thunk *thunk,
                                                uint64_t args[12] );

static uint64_t macrunner_hb_call_arm64_pe_import12_for_ctx( hb_context_t *ctx,
                                                             const struct macrunner_hb_import_thunk *thunk,
                                                             const uint64_t args[12] )
{
    uintptr_t old_guest_rsp = macrunner_hb_native_call_guest_rsp;
    uint64_t ret;

    if (ctx) macrunner_hb_native_call_guest_rsp = ctx->regs.x64.rsp;
    ret = macrunner_hb_call_arm64_pe_import12( thunk, args );
    macrunner_hb_native_call_guest_rsp = old_guest_rsp;
    return ret;
}

struct macrunner_hb_timeb64
{
    int64_t time;
    uint16_t millitm;
    int16_t timezone;
    int16_t dstflag;
    int16_t pad;
};

static BOOL macrunner_hb_try_msvcrt_time_semantic( hb_context_t *ctx,
                                                   const struct macrunner_hb_import_thunk *thunk,
                                                   const uint64_t args[12],
                                                   uint64_t *ret )
{
    struct timeval tv;
    int64_t seconds;

    if (!ctx || !ctx->memory || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_strieq( thunk->dll_name, "msvcrt.dll" )) return FALSE;
    if (!macrunner_hb_strieq( thunk->import_name, "time" ) &&
        !macrunner_hb_strieq( thunk->import_name, "_time64" ) &&
        !macrunner_hb_strieq( thunk->import_name, "_ftime64" ) &&
        !macrunner_hb_strieq( thunk->import_name, "_ftime64_s" ))
        return FALSE;

    if (gettimeofday( &tv, NULL )) return FALSE;
    seconds = tv.tv_sec;

    if (macrunner_hb_strieq( thunk->import_name, "_ftime64" ) ||
        macrunner_hb_strieq( thunk->import_name, "_ftime64_s" ))
    {
        struct macrunner_hb_timeb64 out;

        if (!args[0])
        {
            *ret = macrunner_hb_strieq( thunk->import_name, "_ftime64_s" ) ? 22 /* EINVAL */ : 0;
            return TRUE;
        }

        out.time = seconds;
        out.millitm = tv.tv_usec / 1000;
        out.timezone = 0;
        out.dstflag = 0;
        out.pad = 0;
        if (hb_memory_write( ctx->memory, (hb_gva_t)args[0], &out, sizeof(out) ) != HB_OK)
            return FALSE;

        *ret = macrunner_hb_strieq( thunk->import_name, "_ftime64_s" ) ? 0 : 0;
        return TRUE;
    }

    if (args[0])
    {
        if (hb_memory_write( ctx->memory, (hb_gva_t)args[0], &seconds, sizeof(seconds) ) != HB_OK)
            return FALSE;
    }
    *ret = seconds;
    return TRUE;
}

static BOOL macrunner_hb_try_msvcrt_exit_semantic( hb_context_t *ctx,
                                                   const struct macrunner_hb_import_thunk *thunk,
                                                   const uint64_t args[12],
                                                   uint64_t *ret )
{
    if (!ctx || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_strieq( thunk->dll_name, "msvcrt.dll" )) return FALSE;

    if (macrunner_hb_strieq( thunk->import_name, "_onexit" ))
    {
        /*
         * Wine's native ARM64 msvcrt owns an ARM64 exit table.  Letting it store
         * raw x64 callbacks while it is already inside an x64 _initterm callback
         * can re-enter the native exit lock/callback path and hang startup.  For
         * now, report successful registration to the x64 CRT and keep the x64
         * callback out of the native table.  A production cross-arch onexit list
         * should replay these at process detach via Phase F.
         */
        *ret = args[0];
        TRACE( "MacRunner HyperBridge semantic msvcrt!_onexit func=%p ret=%p\n",
               (void *)(uintptr_t)args[0], (void *)(uintptr_t)*ret );
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "atexit" ) ||
        macrunner_hb_strieq( thunk->import_name, "_crt_atexit" ))
    {
        *ret = args[0] ? 0 : (uint64_t)-1;
        TRACE( "MacRunner HyperBridge semantic msvcrt!%s func=%p ret=%p\n",
               thunk->import_name, (void *)(uintptr_t)args[0],
               (void *)(uintptr_t)*ret );
        return TRUE;
    }

    return FALSE;
}

static hb_result_t macrunner_hb_call_import_thunk( hb_context_t *ctx,
                                                   const struct macrunner_hb_import_thunk *thunk )
{
    uint64_t ret_addr = 0;
    uint64_t args[12] = { 0 };
    uint64_t rc;
    unsigned int i;

    if (!ctx || !ctx->memory || !thunk || !thunk->target) return HB_ERR_INVALID_ARG;

    if (hb_memory_read_u64( ctx->memory, (hb_gva_t)ctx->regs.x64.rsp, &ret_addr ) != HB_OK || !ret_addr)
        return HB_ERR_EXEC_FAULT;

    args[0] = ctx->regs.x64.rcx;
    args[1] = ctx->regs.x64.rdx;
    args[2] = ctx->regs.x64.r8;
    args[3] = ctx->regs.x64.r9;
    for (i = 4; i < 12; i++)
        hb_memory_read_u64( ctx->memory, (hb_gva_t)ctx->regs.x64.rsp + 8 + 32 + (i - 4) * 8, &args[i] );
    macrunner_hb_normalize_import_args( thunk, args );
    if (macrunner_hb_trace_file_api_enabled() &&
        (macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "CreateFileW" ) ||
         macrunner_hb_strieq( thunk->import_name, "ReadFile" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetFileSize" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetFileSizeEx" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetFilePointer" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetFilePointerEx" )) &&
        macrunner_hb_trace_file_api_budget_allows())
    {
        fprintf( stderr, "macrunner-hb-file-api: before import=%s!%s pc=%p rsp=%p ret=%p "
                 "args=%p,%p,%p,%p,%p,%p,%p,%p\n",
                 thunk->dll_name, thunk->import_name,
                 (void *)(uintptr_t)ctx->pc, (void *)(uintptr_t)ctx->regs.x64.rsp,
                 (void *)(uintptr_t)ret_addr, (void *)(uintptr_t)args[0],
                 (void *)(uintptr_t)args[1], (void *)(uintptr_t)args[2],
                 (void *)(uintptr_t)args[3], (void *)(uintptr_t)args[4],
                 (void *)(uintptr_t)args[5], (void *)(uintptr_t)args[6],
                 (void *)(uintptr_t)args[7] );
    }

    TRACE( "MacRunner HyperBridge native import call %s!%s guest=%p target=%p ret=%p\n",
           thunk->dll_name, thunk->import_name, (void *)(uintptr_t)thunk->guest_target,
           thunk->target, (void *)(uintptr_t)ret_addr );

    macrunner_hb_trace_image_api( ctx, "before", thunk, ret_addr, 0, args );
    macrunner_hb_trace_geometry_api( "before", thunk, ret_addr, 0, args );
    macrunner_hb_trace_abi_stack( ctx, "before-native", thunk, ret_addr, args );
    if (macrunner_hb_try_msvcrt_time_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_msvcrt_exit_semantic( ctx, thunk, args, &rc ))
    {
        TRACE( "MacRunner HyperBridge semantic import call %s!%s ret=%p\n",
               thunk->dll_name, thunk->import_name, (void *)(uintptr_t)rc );
    }
    else
    {
        rc = macrunner_hb_call_arm64_pe_import12_for_ctx( ctx, thunk, args );
    }
    macrunner_hb_trace_abi_stack( ctx, "after-native", thunk, ret_addr, args );
    macrunner_hb_trace_abi_return( ctx, thunk, ret_addr, rc, args );
    if (macrunner_hb_trace_file_api_enabled() &&
        (macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "CreateFileW" ) ||
         macrunner_hb_strieq( thunk->import_name, "ReadFile" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetFileSize" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetFileSizeEx" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetFilePointer" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetFilePointerEx" )) &&
        macrunner_hb_trace_file_api_budget_allows())
    {
        fprintf( stderr, "macrunner-hb-file-api: after import=%s!%s pc=%p rsp=%p ret=%p "
                 "rc=%p last_error=%lu last_status=%08lx\n",
                 thunk->dll_name, thunk->import_name,
                 (void *)(uintptr_t)ctx->pc, (void *)(uintptr_t)ctx->regs.x64.rsp,
                 (void *)(uintptr_t)ret_addr, (void *)(uintptr_t)rc,
                 (unsigned long)RtlGetLastWin32Error(), (unsigned long)NtCurrentTeb()->LastStatusValue );
    }
    macrunner_hb_trace_image_api( ctx, "after", thunk, ret_addr, rc, args );
    macrunner_hb_trace_geometry_api( "after", thunk, ret_addr, rc, args );
    if (rc && (macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
               macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "GetProcAddress" ))
    {
        void *native_module = NULL;

        if (macrunner_hb_pc_is_native_pe_builtin( rc, &native_module ))
        {
            char proc_name[96];
            uint64_t guest_target;

            macrunner_hb_get_proc_name_for_trace( args[1], proc_name, sizeof(proc_name) );
            guest_target = macrunner_hb_register_dynamic_import_thunk( thunk, (void *)(uintptr_t)rc,
                                                                       native_module, proc_name );
            if (guest_target)
            {
                TRACE( "MacRunner HyperBridge wrapped dynamic proc %s!%s native=%p guest=%p\n",
                       thunk->dll_name, proc_name, (void *)(uintptr_t)rc,
                       (void *)(uintptr_t)guest_target );
                rc = guest_target;
            }
        }
    }
    ctx->regs.x64.rax = rc;
    ctx->regs.x64.rsp += 8;
    ctx->pc = ret_addr;
    return HB_OK;
}

static BOOL macrunner_hb_trace_import_interesting( const struct macrunner_hb_import_thunk *thunk )
{
    if (!thunk) return FALSE;
    if (macrunner_hb_strieq( thunk->dll_name, "native-direct" )) return TRUE;
    if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
        (macrunner_hb_strieq( thunk->import_name, "SendMessageW" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateWindowExW" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateDialogParamW" ) ||
         macrunner_hb_strieq( thunk->import_name, "DefWindowProcW" ) ||
         macrunner_hb_strieq( thunk->import_name, "MoveWindow" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetWindowPos" ) ||
         macrunner_hb_strieq( thunk->import_name, "DestroyWindow" ) ||
         macrunner_hb_strieq( thunk->import_name, "ShowWindow" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetWindowLongPtrW" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetWindowLongPtrW" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetWindowLongW" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetWindowLongW" ) ||
         macrunner_hb_strieq( thunk->import_name, "LoadStringW" ) ||
         macrunner_hb_strieq( thunk->import_name, "LoadImageW" ) ||
         macrunner_hb_strieq( thunk->import_name, "LoadBitmapW" ) ||
         macrunner_hb_strieq( thunk->import_name, "LoadIconW" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetClientRect" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetWindowRect" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetMonitorInfoW" ) ||
         macrunner_hb_strieq( thunk->import_name, "MonitorFromWindow" ) ||
         macrunner_hb_strieq( thunk->import_name, "MonitorFromRect" ) ||
         macrunner_hb_strieq( thunk->import_name, "MonitorFromPoint" ) ||
         macrunner_hb_strieq( thunk->import_name, "EnumDisplayMonitors" ) ||
         macrunner_hb_strieq( thunk->import_name, "SystemParametersInfoW" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetSystemMetrics" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetWindowPlacement" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetWindowPlacement" ) ||
         macrunner_hb_strieq( thunk->import_name, "AdjustWindowRectEx" ) ||
         macrunner_hb_strieq( thunk->import_name, "MapWindowPoints" ) ||
         macrunner_hb_strieq( thunk->import_name, "EnumChildWindows" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetPropW" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetDC" ) ||
         macrunner_hb_strieq( thunk->import_name, "ReleaseDC" ) ||
         macrunner_hb_strieq( thunk->import_name, "LoadAcceleratorsW" )))
        return TRUE;
    if (macrunner_hb_strieq( thunk->dll_name, "gdi32.dll" ) &&
        (macrunner_hb_strieq( thunk->import_name, "GetTextExtentPointW" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetTextExtentPoint32W" ) ||
         macrunner_hb_strieq( thunk->import_name, "SelectObject" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetBkMode" ) ||
         macrunner_hb_strieq( thunk->import_name, "DeleteObject" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetObjectW" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateCompatibleDC" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateCompatibleBitmap" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateDIBSection" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreatePatternBrush" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateSolidBrush" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateFontIndirectW" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateRectRgn" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetRectRgn" ) ||
         macrunner_hb_strieq( thunk->import_name, "CombineRgn" ) ||
         macrunner_hb_strieq( thunk->import_name, "EqualRgn" ) ||
         macrunner_hb_strieq( thunk->import_name, "DeleteDC" )))
        return TRUE;
    if (macrunner_hb_strieq( thunk->dll_name, "gdiplus.dll" ) &&
        (macrunner_hb_stristarts( thunk->import_name, "Gdip" ) ||
         macrunner_hb_strieq( thunk->import_name, "GdiplusStartup" ) ||
         macrunner_hb_strieq( thunk->import_name, "GdiplusShutdown" )))
        return TRUE;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "GetModuleHandleW" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetStartupInfoA" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetStartupInfoW" ) ||
         macrunner_hb_strieq( thunk->import_name, "FindResourceW" ) ||
         macrunner_hb_strieq( thunk->import_name, "FindResourceExW" ) ||
         macrunner_hb_strieq( thunk->import_name, "LoadResource" ) ||
         macrunner_hb_strieq( thunk->import_name, "LockResource" ) ||
         macrunner_hb_strieq( thunk->import_name, "SizeofResource" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateFileW" ) ||
         macrunner_hb_strieq( thunk->import_name, "ReadFile" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetFileSize" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetFileSizeEx" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetFilePointer" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetFilePointerEx" ) ||
         macrunner_hb_strieq( thunk->import_name, "HeapAlloc" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetSystemTimeAsFileTime" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetTimeZoneInformation" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetLastError" )))
        return TRUE;
    if (macrunner_hb_strieq( thunk->dll_name, "ucrtbase.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "__stdio_common_vswprintf" ))
        return TRUE;
    if (macrunner_hb_strieq( thunk->dll_name, "msvcrt.dll" ) &&
        (macrunner_hb_strieq( thunk->import_name, "_initterm" ) ||
         macrunner_hb_strieq( thunk->import_name, "_initterm_e" ) ||
         macrunner_hb_strieq( thunk->import_name, "_onexit" ) ||
         macrunner_hb_strieq( thunk->import_name, "atexit" ) ||
         macrunner_hb_strieq( thunk->import_name, "_crt_atexit" ) ||
         macrunner_hb_strieq( thunk->import_name, "time" ) ||
         macrunner_hb_strieq( thunk->import_name, "_time64" ) ||
         macrunner_hb_strieq( thunk->import_name, "_ftime64" ) ||
         macrunner_hb_strieq( thunk->import_name, "_lock" ) ||
         macrunner_hb_strieq( thunk->import_name, "_unlock" )))
        return TRUE;
    return FALSE;
}

static uint64_t macrunner_hb_u32_arg( uint64_t value )
{
    return (uint32_t)value;
}

static uint64_t macrunner_hb_i32_arg( uint64_t value )
{
    return (uint64_t)(int64_t)(int32_t)value;
}

static void macrunner_hb_normalize_import_args( const struct macrunner_hb_import_thunk *thunk,
                                                uint64_t args[12] )
{
    if (!thunk || !args) return;

    if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "CreateWindowExW" ))
    {
        /*
         * Win64 callers pass int/DWORD parameters in 64-bit slots; the high
         * bits are not part of the C value.  When forwarding to native ARM64
         * PE code, canonicalize the Win32 scalar arguments so x-register/stack
         * consumers cannot observe stale upper bits from the x64 frame.
         */
        args[0] = macrunner_hb_u32_arg( args[0] ); /* DWORD exStyle */
        args[3] = macrunner_hb_u32_arg( args[3] ); /* DWORD style */
        args[4] = macrunner_hb_i32_arg( args[4] ); /* INT x */
        args[5] = macrunner_hb_i32_arg( args[5] ); /* INT y */
        args[6] = macrunner_hb_i32_arg( args[6] ); /* INT width */
        args[7] = macrunner_hb_i32_arg( args[7] ); /* INT height */
    }
    else if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
             macrunner_hb_strieq( thunk->import_name, "DefWindowProcW" ))
    {
        args[1] = macrunner_hb_u32_arg( args[1] ); /* UINT msg */
    }
    else if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
             macrunner_hb_strieq( thunk->import_name, "MoveWindow" ))
    {
        args[1] = macrunner_hb_i32_arg( args[1] ); /* INT x */
        args[2] = macrunner_hb_i32_arg( args[2] ); /* INT y */
        args[3] = macrunner_hb_i32_arg( args[3] ); /* INT width */
        args[4] = macrunner_hb_i32_arg( args[4] ); /* INT height */
        args[5] = macrunner_hb_u32_arg( args[5] ); /* BOOL repaint */
    }
    else if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
             macrunner_hb_strieq( thunk->import_name, "SetWindowPos" ))
    {
        args[2] = macrunner_hb_i32_arg( args[2] ); /* INT x */
        args[3] = macrunner_hb_i32_arg( args[3] ); /* INT y */
        args[4] = macrunner_hb_i32_arg( args[4] ); /* INT width */
        args[5] = macrunner_hb_i32_arg( args[5] ); /* INT height */
        args[6] = macrunner_hb_u32_arg( args[6] ); /* UINT flags */
    }
    else if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
             (macrunner_hb_strieq( thunk->import_name, "GetWindowLongW" ) ||
              macrunner_hb_strieq( thunk->import_name, "GetWindowLongPtrW" ) ||
              macrunner_hb_strieq( thunk->import_name, "SetWindowLongW" ) ||
              macrunner_hb_strieq( thunk->import_name, "SetWindowLongPtrW" )))
    {
        args[1] = macrunner_hb_i32_arg( args[1] ); /* INT index */
        if (macrunner_hb_strieq( thunk->import_name, "SetWindowLongW" ))
            args[2] = macrunner_hb_i32_arg( args[2] ); /* LONG value */
    }
    else if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
             macrunner_hb_strieq( thunk->import_name, "ShowWindow" ))
    {
        args[1] = macrunner_hb_i32_arg( args[1] ); /* INT command */
    }
    else if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
             macrunner_hb_strieq( thunk->import_name, "SendMessageW" ))
    {
        args[1] = macrunner_hb_u32_arg( args[1] ); /* UINT msg */
    }
    else if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
             macrunner_hb_strieq( thunk->import_name, "MapWindowPoints" ))
    {
        args[3] = macrunner_hb_u32_arg( args[3] ); /* UINT point count */
    }
    else if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
             macrunner_hb_strieq( thunk->import_name, "LoadStringW" ))
    {
        args[1] = macrunner_hb_u32_arg( args[1] ); /* UINT resource id */
        args[3] = macrunner_hb_i32_arg( args[3] ); /* INT buffer chars */
    }
    else if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
             macrunner_hb_strieq( thunk->import_name, "LoadImageW" ))
    {
        /*
         * Win64 callers often write stack integer arguments as 32-bit slots.
         * Canonicalize LoadImageW's scalar tail so LR_* flags such as
         * LR_CREATEDIBSECTION do not inherit stale high bits from the x64 stack.
         */
        args[2] = macrunner_hb_u32_arg( args[2] ); /* UINT type */
        args[3] = macrunner_hb_i32_arg( args[3] ); /* INT cx */
        args[4] = macrunner_hb_i32_arg( args[4] ); /* INT cy */
        args[5] = macrunner_hb_u32_arg( args[5] ); /* UINT flags */
    }
    else if (macrunner_hb_strieq( thunk->dll_name, "gdi32.dll" ) &&
             macrunner_hb_strieq( thunk->import_name, "CreateCompatibleBitmap" ))
    {
        args[1] = macrunner_hb_i32_arg( args[1] ); /* INT width */
        args[2] = macrunner_hb_i32_arg( args[2] ); /* INT height */
    }
    else if (macrunner_hb_strieq( thunk->dll_name, "gdi32.dll" ) &&
             macrunner_hb_strieq( thunk->import_name, "CreateDIBSection" ))
    {
        args[2] = macrunner_hb_u32_arg( args[2] ); /* UINT usage */
        args[5] = macrunner_hb_u32_arg( args[5] ); /* DWORD offset */
    }
    else if (macrunner_hb_strieq( thunk->dll_name, "gdi32.dll" ) &&
             macrunner_hb_strieq( thunk->import_name, "GetObjectW" ))
    {
        args[1] = macrunner_hb_i32_arg( args[1] ); /* INT buffer bytes */
    }
    else if (macrunner_hb_strieq( thunk->dll_name, "gdi32.dll" ) &&
             macrunner_hb_strieq( thunk->import_name, "CreateSolidBrush" ))
    {
        args[0] = macrunner_hb_u32_arg( args[0] ); /* COLORREF */
    }
    else if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
              macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
             macrunner_hb_strieq( thunk->import_name, "CreateFileW" ))
    {
        args[1] = macrunner_hb_u32_arg( args[1] ); /* DWORD desired access */
        args[2] = macrunner_hb_u32_arg( args[2] ); /* DWORD share mode */
        args[4] = macrunner_hb_u32_arg( args[4] ); /* DWORD creation disposition */
        args[5] = macrunner_hb_u32_arg( args[5] ); /* DWORD flags/attributes */
    }
    else if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
              macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
             macrunner_hb_strieq( thunk->import_name, "ReadFile" ))
    {
        args[2] = macrunner_hb_u32_arg( args[2] ); /* DWORD bytes to read */
    }
    else if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
              macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
             macrunner_hb_strieq( thunk->import_name, "SetFilePointer" ))
    {
        args[1] = macrunner_hb_i32_arg( args[1] ); /* LONG/LARGE_INTEGER distance low */
        args[3] = macrunner_hb_u32_arg( args[3] ); /* DWORD move method */
    }
    else if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
              macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
             macrunner_hb_strieq( thunk->import_name, "SetFilePointerEx" ))
    {
        args[3] = macrunner_hb_u32_arg( args[3] ); /* DWORD move method */
    }
}

static void macrunner_hb_trace_guest_rect( hb_context_t *ctx, const char *label, uint64_t addr )
{
    RECT rect;
    hb_result_t r;

    if (!macrunner_hb_trace_geometry_enabled() || !ctx || !ctx->memory || addr < 0x10000) return;
    r = hb_memory_read( ctx->memory, (hb_gva_t)addr, &rect, sizeof(rect) );
    if (r != HB_OK)
    {
        ERR( "macrunner-hb-geometry-rect: %s addr=%p read=%s\n",
             label ? label : "?", (void *)(uintptr_t)addr, hb_result_string(r) );
        return;
    }
    ERR( "macrunner-hb-geometry-rect: %s addr=%p rect=(%ld,%ld)-(%ld,%ld) size=%ldx%ld\n",
         label ? label : "?", (void *)(uintptr_t)addr, rect.left, rect.top,
         rect.right, rect.bottom, rect.right - rect.left, rect.bottom - rect.top );
}

static void macrunner_hb_trace_guest_monitorinfo( hb_context_t *ctx, const char *label, uint64_t addr )
{
    MONITORINFO info;
    hb_result_t r;

    if (!macrunner_hb_trace_geometry_enabled() || !ctx || !ctx->memory || addr < 0x10000) return;
    r = hb_memory_read( ctx->memory, (hb_gva_t)addr, &info, sizeof(info) );
    if (r != HB_OK)
    {
        ERR( "macrunner-hb-geometry-monitor: %s addr=%p read=%s\n",
             label ? label : "?", (void *)(uintptr_t)addr, hb_result_string(r) );
        return;
    }
    ERR( "macrunner-hb-geometry-monitor: %s addr=%p cb=%lu monitor=(%ld,%ld)-(%ld,%ld) "
         "work=(%ld,%ld)-(%ld,%ld) flags=0x%lx monitor_size=%ldx%ld work_size=%ldx%ld\n",
         label ? label : "?", (void *)(uintptr_t)addr, info.cbSize,
         info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right, info.rcMonitor.bottom,
         info.rcWork.left, info.rcWork.top, info.rcWork.right, info.rcWork.bottom,
         info.dwFlags, info.rcMonitor.right - info.rcMonitor.left,
         info.rcMonitor.bottom - info.rcMonitor.top, info.rcWork.right - info.rcWork.left,
         info.rcWork.bottom - info.rcWork.top );
}

static void macrunner_hb_trace_geometry_return( hb_context_t *ctx,
                                                const struct macrunner_hb_import_thunk *thunk,
                                                uint64_t value, const uint64_t args[12] )
{
    static int budget = 220;

    if (!macrunner_hb_trace_geometry_enabled() || !ctx || !thunk || !args || !budget) return;

    if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "GetClientRect" ))
    {
        budget--;
        ERR( "macrunner-hb-geometry-api: GetClientRect hwnd=%p ret=%p\n",
             (void *)(uintptr_t)args[0], (void *)(uintptr_t)value );
        macrunner_hb_trace_guest_rect( ctx, "GetClientRect.out", args[1] );
    }
    else if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
             macrunner_hb_strieq( thunk->import_name, "GetWindowRect" ))
    {
        budget--;
        ERR( "macrunner-hb-geometry-api: GetWindowRect hwnd=%p ret=%p\n",
             (void *)(uintptr_t)args[0], (void *)(uintptr_t)value );
        macrunner_hb_trace_guest_rect( ctx, "GetWindowRect.out", args[1] );
    }
    else if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
             macrunner_hb_strieq( thunk->import_name, "GetMonitorInfoW" ))
    {
        budget--;
        ERR( "macrunner-hb-geometry-api: GetMonitorInfoW monitor=%p info=%p ret=%p\n",
             (void *)(uintptr_t)args[0], (void *)(uintptr_t)args[1],
             (void *)(uintptr_t)value );
        macrunner_hb_trace_guest_monitorinfo( ctx, "GetMonitorInfoW.out", args[1] );
    }
    else if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
             macrunner_hb_strieq( thunk->import_name, "SystemParametersInfoW" ))
    {
        budget--;
        ERR( "macrunner-hb-geometry-api: SystemParametersInfoW action=0x%llx param=%p out=%p flags=%p ret=%p\n",
             (unsigned long long)args[0], (void *)(uintptr_t)args[1],
             (void *)(uintptr_t)args[2], (void *)(uintptr_t)args[3],
             (void *)(uintptr_t)value );
        if ((uint32_t)args[0] == 0x30) /* SPI_GETWORKAREA */
            macrunner_hb_trace_guest_rect( ctx, "SystemParametersInfoW.workarea", args[2] );
    }
    else if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
             macrunner_hb_strieq( thunk->import_name, "DefWindowProcW" ) &&
             (uint32_t)args[1] == WM_NCCALCSIZE)
    {
        budget--;
        ERR( "macrunner-hb-geometry-api: DefWindowProcW hwnd=%p msg=WM_NCCALCSIZE "
             "wparam=%p lparam=%p ret=%p\n",
             (void *)(uintptr_t)args[0], (void *)(uintptr_t)args[2],
             (void *)(uintptr_t)args[3], (void *)(uintptr_t)value );
        macrunner_hb_trace_guest_rect( ctx, "DefWindowProcW.nccalc.after", args[3] );
    }
}

static void macrunner_hb_trace_abi_return( hb_context_t *ctx,
                                           const struct macrunner_hb_import_thunk *thunk,
                                           uint64_t ret_addr, uint64_t value,
                                           const uint64_t args[12] )
{
    TEB *teb = NtCurrentTeb();

    if (!macrunner_hb_trace_abi_enabled() || !ctx || !thunk ||
        !macrunner_hb_trace_import_interesting( thunk ))
        return;
    if (!macrunner_hb_trace_abi_budget_allows()) return;

    ERR( "macrunner-hb-abi-ret: import=%s!%s pc=%p rsp=%p ret=%p value=%p "
         "last_error=%lu last_status=%08lx "
         "rax=%p rbx=%p rcx=%p rdx=%p rsi=%p rdi=%p rbp=%p r8=%p r9=%p "
         "r12=%p r13=%p r14=%p r15=%p\n",
         thunk->dll_name, thunk->import_name,
         (void *)(uintptr_t)ctx->pc, (void *)(uintptr_t)ctx->regs.x64.rsp,
         (void *)(uintptr_t)ret_addr, (void *)(uintptr_t)value,
         teb ? teb->LastErrorValue : 0, teb ? teb->LastStatusValue : 0,
         (void *)(uintptr_t)ctx->regs.x64.rax, (void *)(uintptr_t)ctx->regs.x64.rbx,
         (void *)(uintptr_t)ctx->regs.x64.rcx, (void *)(uintptr_t)ctx->regs.x64.rdx,
         (void *)(uintptr_t)ctx->regs.x64.rsi, (void *)(uintptr_t)ctx->regs.x64.rdi,
         (void *)(uintptr_t)ctx->regs.x64.rbp, (void *)(uintptr_t)ctx->regs.x64.r8,
         (void *)(uintptr_t)ctx->regs.x64.r9, (void *)(uintptr_t)ctx->regs.x64.r12,
         (void *)(uintptr_t)ctx->regs.x64.r13, (void *)(uintptr_t)ctx->regs.x64.r14,
         (void *)(uintptr_t)ctx->regs.x64.r15 );
    macrunner_hb_trace_geometry_return( ctx, thunk, value, args );
}

static void macrunner_hb_trace_guest_wstr( hb_context_t *ctx, const char *name, uint64_t addr )
{
    char text[96];
    unsigned int i, out = 0;

    if (!ctx || !ctx->memory || !addr || addr < 0x10000) return;

    for (i = 0; i < 40 && out + 5 < sizeof(text); i++)
    {
        uint16_t ch = 0;
        hb_result_t r = hb_memory_read_u16( ctx->memory, (hb_gva_t)addr + i * 2, &ch );

        if (r != HB_OK)
        {
            ERR( "macrunner-hb-guest-wstr: %s=%p read=%s index=%u\n",
                 name ? name : "?", (void *)(uintptr_t)addr, hb_result_string(r), i );
            return;
        }
        if (!ch) break;
        if (ch >= 0x20 && ch < 0x7f)
            text[out++] = (char)ch;
        else
        {
            text[out++] = '?';
        }
    }
    text[out] = 0;
    ERR( "macrunner-hb-guest-wstr: %s=%p text=\"%s\"\n",
         name ? name : "?", (void *)(uintptr_t)addr, text );
}

static void macrunner_hb_trace_startupinfo( hb_context_t *ctx,
                                            const struct macrunner_hb_import_thunk *thunk,
                                            const char *phase, uint64_t addr )
{
    uint32_t cb = 0, flags = 0;
    uint16_t show = 0, cb_reserved2 = 0;

    if (!macrunner_hb_trace_abi_enabled() || !ctx || !ctx->memory || !thunk || addr < 0x10000)
        return;
    if (!((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
           macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
          (macrunner_hb_strieq( thunk->import_name, "GetStartupInfoA" ) ||
           macrunner_hb_strieq( thunk->import_name, "GetStartupInfoW" ))))
        return;

    hb_memory_read_u32( ctx->memory, (hb_gva_t)addr + 0, &cb );
    hb_memory_read_u32( ctx->memory, (hb_gva_t)addr + 60, &flags );
    hb_memory_read_u16( ctx->memory, (hb_gva_t)addr + 64, &show );
    hb_memory_read_u16( ctx->memory, (hb_gva_t)addr + 66, &cb_reserved2 );
    ERR( "macrunner-hb-startupinfo: phase=%s import=%s!%s addr=%p cb=%u flags=0x%08x "
         "wShowWindow=%u cbReserved2=%u\n",
         phase ? phase : "?", thunk->dll_name, thunk->import_name,
         (void *)(uintptr_t)addr, cb, flags, show, cb_reserved2 );
}

static void macrunner_hb_trace_windowplacement( hb_context_t *ctx,
                                                const struct macrunner_hb_import_thunk *thunk,
                                                const char *phase, uint64_t addr )
{
    uint32_t length = 0, flags = 0, show = 0;
    int32_t min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    int32_t left = 0, top = 0, right = 0, bottom = 0;

    if (!macrunner_hb_trace_abi_enabled() || !ctx || !ctx->memory || !thunk || addr < 0x10000)
        return;
    if (!(macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
          (macrunner_hb_strieq( thunk->import_name, "SetWindowPlacement" ) ||
           macrunner_hb_strieq( thunk->import_name, "GetWindowPlacement" ))))
        return;

    hb_memory_read_u32( ctx->memory, (hb_gva_t)addr + 0, &length );
    hb_memory_read_u32( ctx->memory, (hb_gva_t)addr + 4, &flags );
    hb_memory_read_u32( ctx->memory, (hb_gva_t)addr + 8, &show );
    hb_memory_read_u32( ctx->memory, (hb_gva_t)addr + 12, (uint32_t *)&min_x );
    hb_memory_read_u32( ctx->memory, (hb_gva_t)addr + 16, (uint32_t *)&min_y );
    hb_memory_read_u32( ctx->memory, (hb_gva_t)addr + 20, (uint32_t *)&max_x );
    hb_memory_read_u32( ctx->memory, (hb_gva_t)addr + 24, (uint32_t *)&max_y );
    hb_memory_read_u32( ctx->memory, (hb_gva_t)addr + 28, (uint32_t *)&left );
    hb_memory_read_u32( ctx->memory, (hb_gva_t)addr + 32, (uint32_t *)&top );
    hb_memory_read_u32( ctx->memory, (hb_gva_t)addr + 36, (uint32_t *)&right );
    hb_memory_read_u32( ctx->memory, (hb_gva_t)addr + 40, (uint32_t *)&bottom );

    ERR( "macrunner-hb-windowplacement: phase=%s import=%s!%s addr=%p "
         "length=%u flags=0x%08x showCmd=%u min=(%d,%d) max=(%d,%d) "
         "normal=(%d,%d)-(%d,%d)\n",
         phase ? phase : "?", thunk->dll_name, thunk->import_name,
         (void *)(uintptr_t)addr, length, flags, show, min_x, min_y, max_x, max_y,
         left, top, right, bottom );
}

static void macrunner_hb_trace_abi_stack( hb_context_t *ctx, const char *phase,
                                          const struct macrunner_hb_import_thunk *thunk,
                                          uint64_t ret_addr, const uint64_t args[12] )
{
    static const int offsets[] =
    {
        0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38,
        0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70, 0x78,
        0x80, 0x220, 0x228, 0x230, 0x238, 0x240, 0x248,
        0x250, 0x258, 0x260, 0x268, 0x270, 0x278, 0x280
    };
    unsigned int i;

    if (!macrunner_hb_trace_abi_enabled() || !ctx || !thunk ||
        !macrunner_hb_trace_import_interesting( thunk ))
        return;
    if (!macrunner_hb_trace_abi_budget_allows()) return;

    ERR( "macrunner-hb-abi: phase=%s import=%s!%s pc=%p rsp=%p ret=%p "
         "rcx=%p rdx=%p r8=%p r9=%p a4=%p a5=%p a6=%p a7=%p a8=%p a9=%p a10=%p a11=%p\n",
         phase ? phase : "?", thunk->dll_name, thunk->import_name,
         (void *)(uintptr_t)ctx->pc, (void *)(uintptr_t)ctx->regs.x64.rsp,
         (void *)(uintptr_t)ret_addr, (void *)(uintptr_t)args[0],
         (void *)(uintptr_t)args[1], (void *)(uintptr_t)args[2],
         (void *)(uintptr_t)args[3], (void *)(uintptr_t)args[4],
         (void *)(uintptr_t)args[5], (void *)(uintptr_t)args[6],
         (void *)(uintptr_t)args[7], (void *)(uintptr_t)args[8],
         (void *)(uintptr_t)args[9], (void *)(uintptr_t)args[10],
         (void *)(uintptr_t)args[11] );

    if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "CreateWindowExW" ))
    {
        ERR( "macrunner-hb-createwindowexw: phase=%s exstyle=%p class=%p title=%p style=%p "
             "x=%p y=%p width=%p height=%p parent=%p menu=%p instance=%p param=%p\n",
             phase ? phase : "?", (void *)(uintptr_t)args[0],
             (void *)(uintptr_t)args[1], (void *)(uintptr_t)args[2],
             (void *)(uintptr_t)args[3], (void *)(uintptr_t)args[4],
             (void *)(uintptr_t)args[5], (void *)(uintptr_t)args[6],
             (void *)(uintptr_t)args[7], (void *)(uintptr_t)args[8],
             (void *)(uintptr_t)args[9], (void *)(uintptr_t)args[10],
             (void *)(uintptr_t)args[11] );
        if (!phase || !strcmp( phase, "before-native" ))
        {
            macrunner_hb_trace_guest_wstr( ctx, "CreateWindowExW.class", args[1] );
            macrunner_hb_trace_guest_wstr( ctx, "CreateWindowExW.title", args[2] );
        }
    }
    else if (macrunner_hb_trace_geometry_enabled() &&
             macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
             macrunner_hb_strieq( thunk->import_name, "DefWindowProcW" ) &&
             (uint32_t)args[1] == WM_NCCALCSIZE)
    {
        ERR( "macrunner-hb-geometry-api: DefWindowProcW.%s hwnd=%p "
             "msg=WM_NCCALCSIZE wparam=%p lparam=%p\n",
             phase ? phase : "?", (void *)(uintptr_t)args[0],
             (void *)(uintptr_t)args[2], (void *)(uintptr_t)args[3] );
        macrunner_hb_trace_guest_rect( ctx,
            (!phase || !strcmp( phase, "before-native" )) ?
            "DefWindowProcW.nccalc.before" : "DefWindowProcW.nccalc.stack-after",
            args[3] );
    }
    macrunner_hb_trace_startupinfo( ctx, thunk, phase, args[0] );
    macrunner_hb_trace_windowplacement( ctx, thunk, phase, args[1] );

    if (!macrunner_hb_trace_abi_stack_enabled()) return;

    for (i = 0; i < ARRAY_SIZE(offsets); i++)
    {
        uint64_t val = 0;
        hb_result_t r = hb_memory_read_u64( ctx->memory, (hb_gva_t)ctx->regs.x64.rsp + offsets[i], &val );
        ERR( "macrunner-hb-abi-stack: phase=%s import=%s!%s rsp+0x%x %s %p\n",
             phase ? phase : "?", thunk->dll_name, thunk->import_name, offsets[i],
             hb_result_string(r), (void *)(uintptr_t)val );
    }
}

static void macrunner_hb_trace_qwords_at( hb_context_t *ctx, const char *name, uint64_t base )
{
    static const int offsets[] =
    {
        0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x40,
        0x100, 0x108, 0x110, 0x118, 0x120, 0x128, 0x130,
        0x170, 0x180, 0x188, 0x190, 0x5e0, 0x5f8, 0x600
    };
    unsigned int i;

    if (!ctx || !ctx->memory || !base) return;
    for (i = 0; i < ARRAY_SIZE(offsets); i++)
    {
        uint64_t val = 0;
        hb_result_t r = hb_memory_read_u64( ctx->memory, (hb_gva_t)base + offsets[i], &val );
        if (r == HB_OK || offsets[i] == 0x188 || offsets[i] == 0x180 || offsets[i] == 0x190)
            ERR( "macrunner-hb-calc-object: base=%s addr=%p +0x%x %s %p\n",
                 name ? name : "?", (void *)(uintptr_t)base, offsets[i],
                 hb_result_string(r), (void *)(uintptr_t)val );
    }
}

static void macrunner_hb_trace_dword_at( hb_context_t *ctx, const char *name, uint64_t addr )
{
    uint32_t val = 0;
    hb_result_t r;

    if (!ctx || !ctx->memory || !addr) return;
    r = hb_memory_read_u32( ctx->memory, (hb_gva_t)addr, &val );
    ERR( "macrunner-hb-calc-state: %s addr=%p %s value=0x%x\n",
         name ? name : "?", (void *)(uintptr_t)addr, hb_result_string(r), val );
}

static void macrunner_hb_trace_calc_probe( hb_context_t *ctx, uint64_t image_start, const char *phase )
{
    uint64_t rva;

    if (!macrunner_hb_trace_calc_object_enabled() || !ctx || !image_start || ctx->pc < image_start)
        return;

    rva = ctx->pc - image_start;
    if (rva != 0x805c && rva != 0x8071 && rva != 0x8074 &&
        rva != 0x80ad && rva != 0x80d9 &&
        rva != 0x1b9b8 && rva != 0x1ba31 && rva != 0x1ba63 &&
        rva != 0x1ba7c && rva != 0x1ba85 && rva != 0x1ba95 &&
        rva != 0x61ae8 && rva != 0x6010c &&
        rva != 0x1a34c && rva != 0x1a39b && rva != 0x3eb28 &&
        rva != 0x10720 && rva != 0x10726 && rva != 0x21aa0 &&
        rva != 0x21920 && rva != 0x22770 && rva != 0x22f1c)
        return;

    ERR( "macrunner-hb-calc-probe: phase=%s rva=%p pc=%p "
         "rax=%p rbx=%p rcx=%p rdx=%p rsi=%p rdi=%p rbp=%p r8=%p r9=%p r12=%p rsp=%p\n",
         phase ? phase : "?", (void *)(uintptr_t)rva, (void *)(uintptr_t)ctx->pc,
         (void *)(uintptr_t)ctx->regs.x64.rax, (void *)(uintptr_t)ctx->regs.x64.rbx,
         (void *)(uintptr_t)ctx->regs.x64.rcx, (void *)(uintptr_t)ctx->regs.x64.rdx,
         (void *)(uintptr_t)ctx->regs.x64.rsi, (void *)(uintptr_t)ctx->regs.x64.rdi,
         (void *)(uintptr_t)ctx->regs.x64.rbp, (void *)(uintptr_t)ctx->regs.x64.r8,
         (void *)(uintptr_t)ctx->regs.x64.r9, (void *)(uintptr_t)ctx->regs.x64.r12,
         (void *)(uintptr_t)ctx->regs.x64.rsp );
    macrunner_hb_trace_qwords_at( ctx, "rcx", ctx->regs.x64.rcx );
    macrunner_hb_trace_qwords_at( ctx, "rdx", ctx->regs.x64.rdx );
    macrunner_hb_trace_qwords_at( ctx, "rsi", ctx->regs.x64.rsi );
    macrunner_hb_trace_qwords_at( ctx, "rbp", ctx->regs.x64.rbp );
    macrunner_hb_trace_qwords_at( ctx, "r12", ctx->regs.x64.r12 );
    macrunner_hb_trace_qwords_at( ctx, "calc-global", image_start + 0x73460 );
    macrunner_hb_trace_qwords_at( ctx, "calc-x188", image_start + 0x73460 + 0x188 );
    macrunner_hb_trace_dword_at( ctx, "crt-state", image_start + 0x730ac );
    macrunner_hb_trace_qwords_at( ctx, "crt-lock", image_start + 0x73048 );
    if (rva == 0x805c || rva == 0x8071 || rva == 0x8074 || rva == 0x80ad || rva == 0x80d9)
    {
        uint8_t guid_last = 0;
        uint32_t table_entry = 0;

        hb_memory_read_u8( ctx->memory, (hb_gva_t)ctx->regs.x64.rsp + 0x67, &guid_last );
        hb_memory_read_u32( ctx->memory, (hb_gva_t)image_start + 0x62d80 + guid_last * 4, &table_entry );
        ERR( "macrunner-hb-calc-pixfmt: rva=%p guid_last=0x%02x table_entry=%u "
             "rax=%p rcx=%p rdx=%p r8=%p r9=%p rsp=%p\n",
             (void *)(uintptr_t)rva, guid_last, table_entry,
             (void *)(uintptr_t)ctx->regs.x64.rax, (void *)(uintptr_t)ctx->regs.x64.rcx,
             (void *)(uintptr_t)ctx->regs.x64.rdx, (void *)(uintptr_t)ctx->regs.x64.r8,
             (void *)(uintptr_t)ctx->regs.x64.r9, (void *)(uintptr_t)ctx->regs.x64.rsp );
    }
}

static void macrunner_hb_trace_npp_open_pack( hb_context_t *ctx, uint64_t image_start,
                                              const char *phase )
{
    unsigned char buf[32];
    uint64_t rva, base = 0;
    uint32_t d0 = 0, d1 = 0, d2 = 0, d3 = 0, d4 = 0, d5 = 0;
    BOOL have_buf = FALSE;

    if (!macrunner_hb_trace_npp_open_pack_enabled() || !ctx || !image_start || ctx->pc < image_start)
        return;

    rva = ctx->pc - image_start;
    if (rva != 0x414c7c && rva != 0x414cfa && rva != 0x414cfc &&
        rva != 0x414e68 && rva != 0x4150eb && rva != 0x415154 &&
        rva != 0x41516e && rva != 0x41518d && rva != 0x415191 &&
        rva != 0x4151b4 && rva != 0x4151ba)
        return;

    if (rva >= 0x414c7c && rva <= 0x414e78) base = ctx->regs.x64.rbx;
    else if (rva >= 0x4150eb && rva <= 0x4151ba) base = ctx->regs.x64.rax;

    if (base >= 0x10000)
    {
        have_buf = macrunner_hb_read_local_memory( (uintptr_t)base, buf, sizeof(buf) );
        if (have_buf)
        {
            memcpy( &d0, buf + 0, sizeof(d0) );
            memcpy( &d1, buf + 4, sizeof(d1) );
            memcpy( &d2, buf + 8, sizeof(d2) );
            memcpy( &d3, buf + 12, sizeof(d3) );
            memcpy( &d4, buf + 16, sizeof(d4) );
            memcpy( &d5, buf + 20, sizeof(d5) );
        }
    }

    ERR( "macrunner-hb-npp-open-pack: phase=%s rva=%p pc=%p "
         "rax=%p rbx=%p rcx=%p rdx=%p r8=%p r9=%p rbp=%p rsp=%p "
         "xmm0=%016llx:%016llx pack=%p read=%u "
         "dwords=%08x,%08x,%08x,%08x,%08x,%08x\n",
         phase ? phase : "?", (void *)(uintptr_t)rva, (void *)(uintptr_t)ctx->pc,
         (void *)(uintptr_t)ctx->regs.x64.rax, (void *)(uintptr_t)ctx->regs.x64.rbx,
         (void *)(uintptr_t)ctx->regs.x64.rcx, (void *)(uintptr_t)ctx->regs.x64.rdx,
         (void *)(uintptr_t)ctx->regs.x64.r8, (void *)(uintptr_t)ctx->regs.x64.r9,
         (void *)(uintptr_t)ctx->regs.x64.rbp, (void *)(uintptr_t)ctx->regs.x64.rsp,
         (unsigned long long)ctx->regs.x64.xmm[0][1],
         (unsigned long long)ctx->regs.x64.xmm[0][0], (void *)(uintptr_t)base,
         have_buf ? 1 : 0, d0, d1, d2, d3, d4, d5 );
}

static void macrunner_hb_trace_nonexec_pc( hb_context_t *ctx, uint64_t image_start,
                                           const char *label, uint64_t blocks,
                                           uint64_t steps )
{
    uint64_t pc_qwords[8] = { 0 };
    uint64_t sp_qwords[8] = { 0 };
    unsigned int i;

    if (!ctx || !ctx->memory) return;
    for (i = 0; i < ARRAY_SIZE(pc_qwords); i++)
        hb_memory_read_u64( ctx->memory, (hb_gva_t)ctx->pc + i * 8, &pc_qwords[i] );
    for (i = 0; i < ARRAY_SIZE(sp_qwords); i++)
        hb_memory_read_u64( ctx->memory, (hb_gva_t)ctx->regs.x64.rsp + i * 8, &sp_qwords[i] );

    ERR( "macrunner-hb-nonexec-pc: label=%s pc=%p rva=%p blocks=%s steps=%s "
         "rax=%p rcx=%p rdx=%p rbx=%p rsp=%p rsi=%p rdi=%p rbp=%p "
         "r8=%p r9=%p r10=%p r11=%p r12=%p r13=%p r14=%p r15=%p\n",
         label ? label : "entry", (void *)(uintptr_t)ctx->pc,
         (void *)(uintptr_t)(ctx->pc - image_start), wine_dbgstr_longlong(blocks),
         wine_dbgstr_longlong(steps),
         (void *)(uintptr_t)ctx->regs.x64.rax, (void *)(uintptr_t)ctx->regs.x64.rcx,
         (void *)(uintptr_t)ctx->regs.x64.rdx, (void *)(uintptr_t)ctx->regs.x64.rbx,
         (void *)(uintptr_t)ctx->regs.x64.rsp, (void *)(uintptr_t)ctx->regs.x64.rsi,
         (void *)(uintptr_t)ctx->regs.x64.rdi, (void *)(uintptr_t)ctx->regs.x64.rbp,
         (void *)(uintptr_t)ctx->regs.x64.r8, (void *)(uintptr_t)ctx->regs.x64.r9,
         (void *)(uintptr_t)ctx->regs.x64.r10, (void *)(uintptr_t)ctx->regs.x64.r11,
         (void *)(uintptr_t)ctx->regs.x64.r12, (void *)(uintptr_t)ctx->regs.x64.r13,
         (void *)(uintptr_t)ctx->regs.x64.r14, (void *)(uintptr_t)ctx->regs.x64.r15 );
    ERR( "macrunner-hb-nonexec-pc-qwords: pc=%p [%p %p %p %p %p %p %p %p]\n",
         (void *)(uintptr_t)ctx->pc,
         (void *)(uintptr_t)pc_qwords[0], (void *)(uintptr_t)pc_qwords[1],
         (void *)(uintptr_t)pc_qwords[2], (void *)(uintptr_t)pc_qwords[3],
         (void *)(uintptr_t)pc_qwords[4], (void *)(uintptr_t)pc_qwords[5],
         (void *)(uintptr_t)pc_qwords[6], (void *)(uintptr_t)pc_qwords[7] );
    ERR( "macrunner-hb-nonexec-stack-qwords: rsp=%p [%p %p %p %p %p %p %p %p]\n",
         (void *)(uintptr_t)ctx->regs.x64.rsp,
         (void *)(uintptr_t)sp_qwords[0], (void *)(uintptr_t)sp_qwords[1],
         (void *)(uintptr_t)sp_qwords[2], (void *)(uintptr_t)sp_qwords[3],
         (void *)(uintptr_t)sp_qwords[4], (void *)(uintptr_t)sp_qwords[5],
         (void *)(uintptr_t)sp_qwords[6], (void *)(uintptr_t)sp_qwords[7] );
}

static BOOL macrunner_hb_pc_is_native_pe_builtin( uint64_t pc, void **module_base )
{
    void *base = NULL;
    IMAGE_NT_HEADERS *nt;

    if (module_base) *module_base = NULL;
    if (!pc) return FALSE;
    base = macrunner_hb_module_from_pc( (void *)(uintptr_t)pc );
    if (!base) return FALSE;
    nt = macrunner_hb_image_nt_header( base );
    if (!nt || nt->FileHeader.Machine != current_machine) return FALSE;
    if (module_base) *module_base = base;
    return TRUE;
}

static BOOL macrunner_hb_label_allows_direct_native( const char *label )
{
    return label && (!strcmp( label, "x64-wndproc" ) ||
                     !strcmp( label, "x64-subclassproc" ) ||
                     !strcmp( label, "x64-signal-callback" ) ||
                     !strcmp( label, "thread" ));
}

static hb_result_t macrunner_hb_call_direct_native_target( hb_context_t *ctx, uint64_t target )
{
    struct macrunner_hb_import_thunk *registered;
    struct macrunner_hb_import_thunk thunk;
    void *native_module = NULL;
    char native_module_name[96];
    uint64_t ret_addr = 0;
    uint64_t args[12] = { 0 };
    uint64_t rc;
    unsigned int i;

    if (!ctx || !ctx->memory || !target) return HB_ERR_INVALID_ARG;
    if (hb_memory_read_u64( ctx->memory, (hb_gva_t)ctx->regs.x64.rsp, &ret_addr ) != HB_OK || !ret_addr)
        return HB_ERR_EXEC_FAULT;

    args[0] = ctx->regs.x64.rcx;
    args[1] = ctx->regs.x64.rdx;
    args[2] = ctx->regs.x64.r8;
    args[3] = ctx->regs.x64.r9;
    for (i = 4; i < 12; i++)
        hb_memory_read_u64( ctx->memory, (hb_gva_t)ctx->regs.x64.rsp + 8 + 32 + (i - 4) * 8, &args[i] );
    if ((registered = macrunner_hb_find_import_thunk_by_target( target )))
        macrunner_hb_normalize_import_args( registered, args );

    if (registered)
    {
        TRACE( "MacRunner HyperBridge routing direct native target through import dispatcher %s!%s target=%p\n",
               registered->dll_name, registered->import_name, (void *)(uintptr_t)target );
        return macrunner_hb_call_import_thunk( ctx, registered );
    }

    native_module_name[0] = 0;
    native_module = macrunner_hb_module_from_pc( (void *)(uintptr_t)target );
    if (native_module)
        macrunner_hb_get_export_module_name( native_module, native_module_name, sizeof(native_module_name) );
    else
        macrunner_hb_copy_cstr( native_module_name, sizeof(native_module_name), "unknown" );

    memset( &thunk, 0, sizeof(thunk) );
    thunk.target = (void *)(uintptr_t)target;
    thunk.target_machine = current_machine;
    strcpy( thunk.dll_name, "native-direct" );
    strcpy( thunk.import_name, "callback" );

    TRACE( "MacRunner HyperBridge direct native PE call target=%p ret=%p rcx=%p rdx=%p r8=%p r9=%p\n",
           (void *)(uintptr_t)target, (void *)(uintptr_t)ret_addr,
           (void *)(uintptr_t)args[0], (void *)(uintptr_t)args[1],
           (void *)(uintptr_t)args[2], (void *)(uintptr_t)args[3] );
    if (macrunner_hb_trace_direct_native_enabled() &&
        macrunner_hb_trace_direct_native_budget_allows())
    {
        uintptr_t rva = native_module ? (uintptr_t)((BYTE *)(uintptr_t)target - (BYTE *)native_module) : 0;

        fprintf( stderr, "macrunner-hb-direct-native: before module=%s base=%p rva=0x%zx target=%p ret=%p "
                 "args=%p,%p,%p,%p,%p,%p rsp=%p pc=%p\n",
                 native_module_name, native_module, rva, (void *)(uintptr_t)target,
                 (void *)(uintptr_t)ret_addr, (void *)(uintptr_t)args[0],
                 (void *)(uintptr_t)args[1], (void *)(uintptr_t)args[2],
                 (void *)(uintptr_t)args[3], (void *)(uintptr_t)args[4],
                 (void *)(uintptr_t)args[5], (void *)(uintptr_t)ctx->regs.x64.rsp,
                 (void *)(uintptr_t)ctx->pc );
    }
    macrunner_hb_trace_abi_stack( ctx, "before-native", &thunk, ret_addr, args );
    rc = macrunner_hb_call_arm64_pe_import12_for_ctx( ctx, &thunk, args );
    macrunner_hb_trace_abi_stack( ctx, "after-native", &thunk, ret_addr, args );
    macrunner_hb_trace_abi_return( ctx, &thunk, ret_addr, rc, args );
    if (macrunner_hb_trace_direct_native_enabled() &&
        macrunner_hb_trace_direct_native_budget_allows())
    {
        uintptr_t rva = native_module ? (uintptr_t)((BYTE *)(uintptr_t)target - (BYTE *)native_module) : 0;

        fprintf( stderr, "macrunner-hb-direct-native: after module=%s base=%p rva=0x%zx target=%p ret=%p "
                 "rc=%p last_error=%lu\n",
                 native_module_name, native_module, rva, (void *)(uintptr_t)target,
                 (void *)(uintptr_t)ret_addr, (void *)(uintptr_t)rc,
                 (unsigned long)RtlGetLastWin32Error() );
        if (ctx->memory && !strcasecmp( native_module_name, "windowscodecs.dll" ) && args[1])
        {
            unsigned char bytes[16] = {0};

            if (hb_memory_read( ctx->memory, (hb_gva_t)args[1], bytes, sizeof(bytes) ) == HB_OK)
                fprintf( stderr, "macrunner-hb-direct-native: out16 arg1=%p "
                         "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
                         (void *)(uintptr_t)args[1], bytes[3], bytes[2], bytes[1], bytes[0],
                         bytes[5], bytes[4], bytes[7], bytes[6], bytes[8], bytes[9],
                         bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15] );
        }
    }
    macrunner_hb_finish_import( ctx, ret_addr, rc );
    return HB_OK;
}

uint64_t macrunner_hb_dispatch_x64_callback( uint64_t target, const uint64_t args[8] )
{
    hb_abi_x64_call_t call = {0};
    uint64_t stack_args[4] = {0};
    uint64_t ret = 0, blocks = 0, steps = 0;
    NTSTATUS status;

    if (!target || !args) return 0;

    if (macrunner_hb_env_enabled( "MACRUNNER_HB_TRACE_CALLBACK_ROUTE" ))
        fprintf( stderr, "macrunner-hb-callback-dispatch: target=%p "
                 "x0=%p x1=%p x2=%p x3=%p x4=%p x5=%p x6=%p x7=%p\n",
                 (void *)(uintptr_t)target,
                 (void *)(uintptr_t)args[0], (void *)(uintptr_t)args[1],
                 (void *)(uintptr_t)args[2], (void *)(uintptr_t)args[3],
                 (void *)(uintptr_t)args[4], (void *)(uintptr_t)args[5],
                 (void *)(uintptr_t)args[6], (void *)(uintptr_t)args[7] );

    if (!macrunner_hb_pc_is_x64_guest_code( (void *)(uintptr_t)target ))
    {
        ERR( "MacRunner Phase F rejected non-x64 callback target=%p\n",
             (void *)(uintptr_t)target );
        return 0;
    }

    call.rcx = args[0];
    call.rdx = args[1];
    call.r8  = args[2];
    call.r9  = args[3];
    stack_args[0] = args[4];
    stack_args[1] = args[5];
    stack_args[2] = args[6];
    stack_args[3] = args[7];
    call.stack_args = stack_args;
    call.stack_arg_count = ARRAY_SIZE(stack_args);

    TRACE( "MacRunner Phase F dispatch x64 callback target=%p args=%p,%p,%p,%p,%p,%p,%p,%p\n",
           (void *)(uintptr_t)target, (void *)(uintptr_t)args[0],
           (void *)(uintptr_t)args[1], (void *)(uintptr_t)args[2],
           (void *)(uintptr_t)args[3], (void *)(uintptr_t)args[4],
           (void *)(uintptr_t)args[5], (void *)(uintptr_t)args[6],
           (void *)(uintptr_t)args[7] );
    macrunner_hb_trace_x64_callback_abi( "before", target, args, 0 );
    status = macrunner_hb_run_x64( (void *)(uintptr_t)target, &call, &ret, &blocks, &steps,
                                   "x64-signal-callback",
                                   NtCurrentTeb()->Peb->ImageBaseAddress );
    if (status)
    {
        ERR( "MacRunner Phase F x64 callback failed target=%p status=%lx blocks=%s steps=%s\n",
             (void *)(uintptr_t)target, status, wine_dbgstr_longlong(blocks),
             wine_dbgstr_longlong(steps) );
        return 0;
    }
    TRACE( "MacRunner Phase F x64 callback returned target=%p ret=%p blocks=%s steps=%s\n",
           (void *)(uintptr_t)target, (void *)(uintptr_t)ret,
           wine_dbgstr_longlong(blocks), wine_dbgstr_longlong(steps) );
    macrunner_hb_trace_x64_callback_abi( "after", target, args, ret );
    return ret;
}

static NTSTATUS macrunner_hb_run_x64( void *entry, hb_abi_x64_call_t *call, uint64_t *ret_value,
                                      uint64_t *blocks_out, uint64_t *steps_out,
                                      const char *label, void *image_base )
{
    struct macrunner_hb_special special;
    TEB *teb;
    hb_context_t *ctx = NULL;
    hb_exec_result_t out;
    uint64_t blocks = 0, steps = 0;
    uint64_t block_limit = macrunner_hb_get_block_limit( label );
    uint64_t step_limit = macrunner_hb_get_step_limit( label, block_limit );
    uint64_t image_start = (uint64_t)(uintptr_t)image_base;
    uint64_t image_size = macrunner_hb_module_size( image_base );
    size_t stack_size = macrunner_hb_x64_stack_size( image_base );
    void *stack_base = NULL;
    void *old_bridge_stack_limit = macrunner_hb_bridge_stack_limit;
    void *old_bridge_stack_base = macrunner_hb_bridge_stack_base;
    size_t old_bridge_stack_size = macrunner_hb_bridge_stack_size;
    void *old_original_stack_limit = macrunner_hb_original_stack_limit;
    void *old_original_stack_base = macrunner_hb_original_stack_base;
    void *old_teb_stack_limit = NULL;
    void *old_teb_stack_base = NULL;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    const char *status_reason = "uninitialised";
    hb_result_t ret;

    if (!entry || !call) return STATUS_INVALID_PARAMETER;
    if (ret_value) *ret_value = 0;
    if (blocks_out) *blocks_out = 0;
    if (steps_out) *steps_out = 0;

#ifndef __aarch64__
    return STATUS_NOT_IMPLEMENTED;
#else
    if (macrunner_hb_debug_enabled())
        ERR( "MacRunner HyperBridge run begin %s entry=%p image=%p\n",
             label ? label : "x64", entry, image_base );
    ctx = hb_context_create( HB_ARCH_X64, HB_BACKEND_INTERP );
    if (!ctx) return STATUS_NO_MEMORY;
    hb_context_set_block_limit( ctx, block_limit );
    hb_context_set_step_limit( ctx, step_limit );
    ctx->memory = hb_memory_create( 0 );
    if (!ctx->memory)
    {
        hb_context_destroy( ctx );
        return STATUS_NO_MEMORY;
    }

    teb = NtCurrentTeb();
    special.teb = teb;
    special.peb = teb->Peb;
    ctx->gs_base = (uint64_t)(uintptr_t)special.teb;
    ctx->fs_base = (uint64_t)(uintptr_t)special.teb;
    old_teb_stack_limit = teb->Tib.StackLimit;
    old_teb_stack_base = teb->Tib.StackBase;
    hb_memory_set_special_handlers( ctx->memory, macrunner_hb_special_read,
                                    macrunner_hb_special_write, &special );

    ret = macrunner_hb_map_live_address_space( ctx->memory );
    if (macrunner_hb_debug_enabled())
        ERR( "MacRunner HyperBridge after live-map %s entry=%p result=%s\n",
             label ? label : "x64", entry, hb_result_string(ret) );
    if (ret != HB_OK)
    {
        ERR( "MacRunner HyperBridge live map failed %s entry=%p result=%s\n",
             label ? label : "x64", entry, hb_result_string(ret) );
        status = STATUS_NO_MEMORY;
        status_reason = "live-map";
        goto done;
    }
    ret = macrunner_hb_mark_x64_image_exec_sections( ctx->memory, image_base );
    if (ret != HB_OK)
    {
        ERR( "MacRunner HyperBridge x64 exec overlay failed %s image=%p result=%s\n",
             label ? label : "x64", image_base, hb_result_string(ret) );
        status = STATUS_INVALID_IMAGE_FORMAT;
        status_reason = "exec-overlay";
        goto done;
    }

    ret = macrunner_hb_setup_bridge_stack( ctx, stack_size, &stack_base );
    if (macrunner_hb_debug_enabled())
        ERR( "MacRunner HyperBridge after stack %s entry=%p result=%s stack=%p\n",
             label ? label : "x64", entry, hb_result_string(ret), stack_base );
    if (ret != HB_OK)
    {
        ERR( "MacRunner HyperBridge stack setup failed %s entry=%p result=%s\n",
             label ? label : "x64", entry, hb_result_string(ret) );
        status = STATUS_NO_MEMORY;
        status_reason = "stack-setup";
        goto done;
    }
    macrunner_hb_bridge_stack_limit = stack_base;
    macrunner_hb_bridge_stack_base = (char *)stack_base + MACRUNNER_HB_SEH_STACK_SLACK;
    macrunner_hb_bridge_stack_size = stack_size;
    macrunner_hb_original_stack_limit = old_teb_stack_limit;
    macrunner_hb_original_stack_base = old_teb_stack_base;
    teb->Tib.StackLimit = macrunner_hb_bridge_stack_base;
    teb->Tib.StackBase = (char *)macrunner_hb_bridge_stack_base + macrunner_hb_bridge_stack_size;

    ret = hb_abi_x64_call( ctx, (uint64_t)(uintptr_t)entry, call, NULL );
    if (macrunner_hb_debug_enabled())
        ERR( "MacRunner HyperBridge after abi %s entry=%p result=%s pc=%p rsp=%p\n",
             label ? label : "x64", entry, hb_result_string(ret),
             (void *)(uintptr_t)ctx->pc, (void *)(uintptr_t)ctx->regs.x64.rsp );
    if (ret != HB_OK)
    {
        ERR( "MacRunner HyperBridge ABI setup failed %s entry=%p result=%s rsp=%p\n",
             label ? label : "x64", entry, hb_result_string(ret), (void *)(uintptr_t)ctx->regs.x64.rsp );
        status = STATUS_INVALID_PARAMETER;
        status_reason = "abi-setup";
        goto done;
    }

    TRACE( "MacRunner HyperBridge x64 %s start entry=%p image=%p-%p rsp=%p stack=%p-%p\n",
           label ? label : "entry", entry,
           (void *)(uintptr_t)image_start, (void *)(uintptr_t)(image_start + image_size),
           (void *)(uintptr_t)ctx->regs.x64.rsp,
           stack_base, (char *)stack_base + stack_size );

    for (;;)
    {
        hb_ir_func_t *func = NULL;
        struct macrunner_hb_import_thunk *import_thunk;

        if (ctx->pc == 0xffff0000)
        {
            if (ret_value) *ret_value = ctx->regs.x64.rax;
            status = STATUS_SUCCESS;
            status_reason = "guest-return";
            break;
        }
        if ((import_thunk = macrunner_hb_find_import_thunk( ctx->pc )))
        {
            ret = macrunner_hb_call_import_thunk( ctx, import_thunk );
            if (ret != HB_OK)
            {
                ERR( "MacRunner HyperBridge import thunk failed pc=%p %s!%s result=%s\n",
                     (void *)(uintptr_t)ctx->pc, import_thunk->dll_name, import_thunk->import_name,
                     hb_result_string(ret) );
                status = STATUS_INVALID_IMAGE_FORMAT;
                status_reason = "import-thunk";
                break;
            }
            continue;
        }
        if (!macrunner_hb_pc_in_image( ctx->pc, image_start, image_size ))
        {
            void *native_module = NULL;

            if (macrunner_hb_label_allows_direct_native( label ) &&
                macrunner_hb_pc_is_native_pe_builtin( ctx->pc, &native_module ))
            {
                TRACE( "MacRunner HyperBridge dispatching direct native target %s pc=%p module=%p\n",
                       label ? label : "entry", (void *)(uintptr_t)ctx->pc, native_module );
                ret = macrunner_hb_call_direct_native_target( ctx, ctx->pc );
                if (ret != HB_OK)
                {
                    ERR( "MacRunner HyperBridge direct native target failed %s pc=%p result=%s\n",
                         label ? label : "entry", (void *)(uintptr_t)ctx->pc, hb_result_string(ret) );
                    status = STATUS_INVALID_IMAGE_FORMAT;
                    status_reason = "direct-native";
                    break;
                }
                continue;
            }
            ERR( "MacRunner HyperBridge refused non-application target %s pc=%p image=%p-%p "
                 "rax=%p rcx=%p rdx=%p rsi=%p rdi=%p rsp=%p blocks=%s steps=%s\n",
                 label ? label : "entry", (void *)(uintptr_t)ctx->pc,
                 (void *)(uintptr_t)image_start, (void *)(uintptr_t)(image_start + image_size),
                 (void *)(uintptr_t)ctx->regs.x64.rax,
                 (void *)(uintptr_t)ctx->regs.x64.rcx,
                 (void *)(uintptr_t)ctx->regs.x64.rdx,
                 (void *)(uintptr_t)ctx->regs.x64.rsi,
                 (void *)(uintptr_t)ctx->regs.x64.rdi,
                 (void *)(uintptr_t)ctx->regs.x64.rsp,
                 wine_dbgstr_longlong(blocks), wine_dbgstr_longlong(steps) );
            status = STATUS_INVALID_IMAGE_FORMAT;
            status_reason = "non-application-target";
            break;
        }
        if (++blocks > block_limit && block_limit)
        {
            ERR( "MacRunner HyperBridge x64 %s block limit at pc=%p\n",
                 label ? label : "entry", (void *)(uintptr_t)ctx->pc );
            status = STATUS_TIMEOUT;
            status_reason = "block-limit";
            break;
        }
        if (!macrunner_hb_pc_in_executable_section( image_base, ctx->pc ))
        {
            macrunner_hb_trace_nonexec_pc( ctx, image_start, label, blocks, steps );
            status = STATUS_INVALID_IMAGE_FORMAT;
            status_reason = "nonexec-section";
            break;
        }
        macrunner_hb_trace_calc_probe( ctx, image_start, "before-block" );
        macrunner_hb_trace_npp_open_pack( ctx, image_start, "before-block" );
        uint64_t block_pc = ctx->pc;
        if (blocks <= 80)
            TRACE( "MacRunner HyperBridge block %s pc=%p rsp=%p rax=%p\n",
                   wine_dbgstr_longlong(blocks), (void *)(uintptr_t)ctx->pc,
                   (void *)(uintptr_t)ctx->regs.x64.rsp, (void *)(uintptr_t)ctx->regs.x64.rax );

        if (macrunner_hb_debug_enabled() && blocks <= 200)
            ERR( "MacRunner HyperBridge before lift %s block=%s pc=%p rsp=%p\n",
                 label ? label : "x64", wine_dbgstr_longlong(blocks),
                 (void *)(uintptr_t)ctx->pc, (void *)(uintptr_t)ctx->regs.x64.rsp );
        ret = macrunner_hb_lift_one_block( ctx->pc, &func );
        if (macrunner_hb_debug_enabled() && blocks <= 200)
            ERR( "MacRunner HyperBridge after lift %s block=%s pc=%p result=%s func=%p\n",
                 label ? label : "x64", wine_dbgstr_longlong(blocks),
                 (void *)(uintptr_t)ctx->pc, hb_result_string(ret), func );
        if (ret != HB_OK || !func)
        {
            ERR( "MacRunner HyperBridge lift failed pc=%p result=%s\n",
                 (void *)(uintptr_t)ctx->pc, hb_result_string(ret) );
            status = STATUS_INVALID_IMAGE_FORMAT;
            status_reason = "lift";
            break;
        }

        memset( &out, 0, sizeof(out) );
        if (macrunner_hb_debug_enabled() && blocks <= 200)
            ERR( "MacRunner HyperBridge before run %s block=%s pc=%p instrs=%zu\n",
                 label ? label : "x64", wine_dbgstr_longlong(blocks),
                 (void *)(uintptr_t)ctx->pc,
                 func && func->cfg && func->cfg->entry ? func->cfg->entry->instr_count : 0 );
        ret = hb_runtime_run( ctx, func, HB_BACKEND_INTERP, &out );
        if (macrunner_hb_debug_enabled() && blocks <= 200)
            ERR( "MacRunner HyperBridge after run %s block=%s ret=%s out=%s steps=%s pc=%p\n",
                 label ? label : "x64", wine_dbgstr_longlong(blocks),
                 hb_result_string(ret), hb_result_string(out.result),
                 wine_dbgstr_longlong(out.steps_executed), (void *)(uintptr_t)ctx->pc );
        hb_ir_func_destroy( func );
        steps += out.steps_executed;
        if (macrunner_hb_env_enabled( "MACRUNNER_HB_TRACE_LOW_STACK" ) && ctx->memory &&
            (ctx->regs.x64.rsp < ctx->memory->stack_bottom + 0x20000 ||
             ctx->regs.x64.rsp > ctx->memory->stack_top))
        {
            static int low_stack_budget = 120;
            if (low_stack_budget-- > 0)
                fprintf( stderr, "macrunner-hb-low-stack: block=%s from_pc=%p next_pc=%p "
                         "rsp=%p stack=[%p..%p] ret=%s out=%s steps=%s\n",
                         wine_dbgstr_longlong(blocks), (void *)(uintptr_t)block_pc,
                         (void *)(uintptr_t)ctx->pc, (void *)(uintptr_t)ctx->regs.x64.rsp,
                         (void *)(uintptr_t)ctx->memory->stack_bottom,
                         (void *)(uintptr_t)ctx->memory->stack_top,
                         hb_result_string(ret), hb_result_string(out.result),
                         wine_dbgstr_longlong(out.steps_executed) );
        }

        if (ret != HB_OK || (out.result != HB_OK && out.result != HB_ERR_NOT_FOUND))
        {
            fprintf( stderr, "macrunner-hb-runtime-fail: label=%s block_pc=%p next_pc=%p "
                     "ret=%s out=%s reason=%s blocks=%s steps=%s out_steps=%s "
                     "rax=%p rcx=%p rdx=%p rsi=%p rdi=%p rsp=%p "
                     "r8=%p r9=%p r10=%p r11=%p\n",
                     label ? label : "entry", (void *)(uintptr_t)block_pc,
                     (void *)(uintptr_t)ctx->pc, hb_result_string(ret),
                     hb_result_string(out.result), out.fault_reason ? out.fault_reason : "",
                     wine_dbgstr_longlong(blocks), wine_dbgstr_longlong(steps),
                     wine_dbgstr_longlong(out.steps_executed),
                     (void *)(uintptr_t)ctx->regs.x64.rax,
                     (void *)(uintptr_t)ctx->regs.x64.rcx,
                     (void *)(uintptr_t)ctx->regs.x64.rdx,
                     (void *)(uintptr_t)ctx->regs.x64.rsi,
                     (void *)(uintptr_t)ctx->regs.x64.rdi,
                     (void *)(uintptr_t)ctx->regs.x64.rsp,
                     (void *)(uintptr_t)ctx->regs.x64.r8,
                     (void *)(uintptr_t)ctx->regs.x64.r9,
                     (void *)(uintptr_t)ctx->regs.x64.r10,
                     (void *)(uintptr_t)ctx->regs.x64.r11 );
            ERR( "MacRunner HyperBridge run failed %s pc=%p ret=%s out=%s reason=%s "
                 "rax=%p rcx=%p rdx=%p rsi=%p rdi=%p rsp=%p\n",
                 label ? label : "entry", (void *)(uintptr_t)ctx->pc,
                 hb_result_string(ret), hb_result_string(out.result),
                 out.fault_reason ? out.fault_reason : "",
                 (void *)(uintptr_t)ctx->regs.x64.rax,
                 (void *)(uintptr_t)ctx->regs.x64.rcx,
                (void *)(uintptr_t)ctx->regs.x64.rdx,
                (void *)(uintptr_t)ctx->regs.x64.rsi,
                (void *)(uintptr_t)ctx->regs.x64.rdi,
                (void *)(uintptr_t)ctx->regs.x64.rsp );
            macrunner_hb_trace_calc_probe( ctx, image_start, "run-fault" );
            status = STATUS_INVALID_IMAGE_FORMAT;
            status_reason = "runtime";
            break;
        }
    }

done:
    if (status)
        fprintf( stderr, "macrunner-hb-run-exit: label=%s status=%08x reason=%s pc=%p "
                 "blocks=%s steps=%s rax=%p rcx=%p rdx=%p rsi=%p rdi=%p rsp=%p ret=%s\n",
                 label ? label : "entry", (unsigned int)status, status_reason,
                 ctx ? (void *)(uintptr_t)ctx->pc : NULL,
                 wine_dbgstr_longlong(blocks), wine_dbgstr_longlong(steps),
                 ctx ? (void *)(uintptr_t)ctx->regs.x64.rax : NULL,
                 ctx ? (void *)(uintptr_t)ctx->regs.x64.rcx : NULL,
                 ctx ? (void *)(uintptr_t)ctx->regs.x64.rdx : NULL,
                 ctx ? (void *)(uintptr_t)ctx->regs.x64.rsi : NULL,
                 ctx ? (void *)(uintptr_t)ctx->regs.x64.rdi : NULL,
                 ctx ? (void *)(uintptr_t)ctx->regs.x64.rsp : NULL,
                 hb_result_string(ret) );
    if (blocks_out) *blocks_out = blocks;
    if (steps_out) *steps_out = steps;
    macrunner_hb_bridge_stack_limit = old_bridge_stack_limit;
    macrunner_hb_bridge_stack_base = old_bridge_stack_base;
    macrunner_hb_bridge_stack_size = old_bridge_stack_size;
    macrunner_hb_original_stack_limit = old_original_stack_limit;
    macrunner_hb_original_stack_base = old_original_stack_base;
    if (teb)
    {
        teb->Tib.StackLimit = old_teb_stack_limit;
        teb->Tib.StackBase = old_teb_stack_base;
    }
    if (ctx) hb_context_destroy( ctx );
    if (stack_base)
    {
        SIZE_T free_size = 0;
        NtFreeVirtualMemory( NtCurrentProcess(), &stack_base, &free_size, MEM_RELEASE );
    }
    return status;
#endif
}

NTSTATUS macrunner_hb_x64_dll_entry( void *args )
{
    struct macrunner_hb_x64_dll_entry_params *params = args;
    hb_abi_x64_call_t call;
    uint64_t stack_args[1] = { 0 };
    USHORT module_machine;
    uint64_t ret_value = 0;
    NTSTATUS status;

    if (!params || !params->entry) return STATUS_INVALID_PARAMETER;

#ifndef __aarch64__
    return STATUS_NOT_IMPLEMENTED;
#else
    module_machine = macrunner_hb_module_machine( params->module );
    if (module_machine != IMAGE_FILE_MACHINE_AMD64)
    {
        ERR( "MacRunner HyperBridge refusing x64 entry entry=%p module=%p machine=%04x expected=%04x\n",
             params->entry, params->module, module_machine, IMAGE_FILE_MACHINE_AMD64 );
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    memset( &call, 0, sizeof(call) );
    call.rcx = (uint64_t)(uintptr_t)params->module;
    call.rdx = params->reason;
    call.r8  = (uint64_t)(uintptr_t)params->reserved;
    call.stack_args = stack_args;
    call.stack_arg_count = 0;

    status = macrunner_hb_run_x64( params->entry, &call, &ret_value,
                                   &params->blocks, &params->steps, "dll", params->module );
    params->ret = ret_value ? 1 : 0;
    return status;
#endif
}

NTSTATUS macrunner_hb_x64_thread_entry( void *args )
{
    struct macrunner_hb_x64_thread_entry_params *params = args;
    hb_abi_x64_call_t call;
    NTSTATUS status;

    if (!params || !params->entry) return STATUS_INVALID_PARAMETER;

    memset( &call, 0, sizeof(call) );
    call.rcx = (uint64_t)(uintptr_t)params->arg;

    if (macrunner_hb_debug_enabled())
        ERR( "MacRunner HyperBridge unix thread entry begin entry=%p arg=%p image=%p\n",
             params->entry, params->arg, NtCurrentTeb()->Peb->ImageBaseAddress );

    status = macrunner_hb_run_x64( params->entry, &call, &params->ret,
                                   &params->blocks, &params->steps, "thread",
                                   NtCurrentTeb()->Peb->ImageBaseAddress );
    TRACE( "MacRunner HyperBridge x64 thread returned status=%lx ret=%s blocks=%s steps=%s\n",
           status, wine_dbgstr_longlong(params->ret), wine_dbgstr_longlong(params->blocks),
           wine_dbgstr_longlong(params->steps) );
    return status;
}
