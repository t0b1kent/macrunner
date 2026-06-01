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
#include <dlfcn.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <wchar.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
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
#include "winioctl.h"
#include "winnls.h"
#include "wine/asm.h"
#include "wine/list.h"
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
    hb_memory_t *mem;
    void *teb;
    void *peb;
};

#define MACRUNNER_HB_IMPORT_MAX 4096
#define MACRUNNER_HB_IMPORT_BASE 0x00006f0000000000ULL
#define MACRUNNER_HB_IMPORT_STRIDE 0x10ULL
#define MACRUNNER_HB_IMPORT_CODE_SIZE (MACRUNNER_HB_IMPORT_MAX * MACRUNNER_HB_IMPORT_STRIDE)
#define MACRUNNER_HB_IMPORT_ARG_MAX 20
#define MACRUNNER_HB_APISET_MODULE_MAX 512
#define MACRUNNER_HB_TLS_SLOT_MAX 128
#define MACRUNNER_HB_TLS_THREAD_MAX 512
#define MACRUNNER_HB_REGISTERED_MESSAGE_MAX 128
#define MACRUNNER_HB_REGISTERED_MESSAGE_NAME_MAX 128
#define MACRUNNER_HB_LOCAL_HEAP_MAX 8192
#define MACRUNNER_HB_LOCAL_HEAP_ARENA_MAX 256
#define MACRUNNER_HB_LOCAL_HEAP_ARENA_SIZE 0x01000000ULL
#define MACRUNNER_HB_LOCAL_HEAP_ALIGN 16
#define MACRUNNER_HB_LOCAL_FILE_MAX 256
#define MACRUNNER_HB_LOCAL_FILE_BASE 0x00006f4000000000ULL
#define MACRUNNER_HB_VIRTUAL_REGION_MAX 8192
#define MACRUNNER_HB_PSEUDO_HWINSTA 0x00006f5000000010ULL
#define MACRUNNER_HB_PSEUDO_HDESK 0x00006f5000000020ULL
#define MACRUNNER_HB_PSEUDO_HCURSOR_BASE 0x00006f5000010000ULL
#define MACRUNNER_HB_PSEUDO_HICON_BASE 0x00006f5000020000ULL
#define MACRUNNER_HB_PSEUDO_HWND_BASE 0x00006f5000030000ULL
#define MACRUNNER_HB_SEH_STACK_SLACK 0x10000ULL
#define MACRUNNER_HB_IR_CACHE_SIZE 8192
#define MACRUNNER_HB_IMPORT_TARGET_MAP_SIZE 8192
#define MACRUNNER_HB_D3D11_MODULE ((uint64_t)0x00006f2000001100ULL)
#define MACRUNNER_HB_D3D12_MODULE ((uint64_t)0x00006f2000001200ULL)
#define MACRUNNER_HB_DXGI_MODULE  ((uint64_t)0x00006f2000001300ULL)
#define MACRUNNER_HB_LIST_MODULES_32BIT   0x01
#define MACRUNNER_HB_LIST_MODULES_64BIT   0x02
#define MACRUNNER_HB_LIST_MODULES_ALL     0x03
#define MACRUNNER_HB_LIST_MODULES_DEFAULT 0x00

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

struct macrunner_hb_apiset_module
{
    char dll_name[96];
    uint64_t module_id;
    USHORT machine;
};

struct macrunner_hb_local_file
{
    uint64_t handle;
    int fd;
};

struct macrunner_hb_registered_message
{
    char name[MACRUNNER_HB_REGISTERED_MESSAGE_NAME_MAX];
    UINT message;
};

struct macrunner_hb_local_heap
{
    void *base;
    SIZE_T requested_size;
    SIZE_T allocation_size;
    void *allocation_base;
    BOOL arena_backed;
};

struct macrunner_hb_local_heap_arena
{
    BYTE *base;
    SIZE_T size;
    SIZE_T used;
};

struct macrunner_hb_moduleinfo64
{
    uint64_t lpBaseOfDll;
    DWORD SizeOfImage;
    DWORD pad;
    uint64_t EntryPoint;
};

struct macrunner_hb_virtual_region
{
    uint64_t base;
    SIZE_T size;
    ULONG protect;
};

struct macrunner_hb_ir_cache_entry
{
    uint64_t pc;
    hb_ir_func_t *func;
};

struct macrunner_hb_ir_cache
{
    struct macrunner_hb_ir_cache_entry entries[MACRUNNER_HB_IR_CACHE_SIZE];
};

struct macrunner_hb_tls_thread_values
{
    DWORD tid;
    uint64_t tls_values[MACRUNNER_HB_TLS_SLOT_MAX];
    uint64_t fls_values[MACRUNNER_HB_TLS_SLOT_MAX];
};

static NTSTATUS macrunner_hb_run_x64( void *entry, hb_abi_x64_call_t *call, ULONG64 *ret_value,
                                      ULONG64 *blocks_out, ULONG64 *steps_out,
                                      const char *label, void *image_base );
static BOOL macrunner_hb_pc_is_native_pe_builtin( uint64_t pc, void **module_base );
static void macrunner_hb_trace_guest_wstr( hb_context_t *ctx, const char *name, uint64_t addr );
static void macrunner_hb_remember_apiset_module_locked( const char *dll_name, uint64_t module_id,
                                                        USHORT machine );

static pthread_mutex_t macrunner_hb_import_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct macrunner_hb_import_thunk macrunner_hb_imports[MACRUNNER_HB_IMPORT_MAX];
static struct macrunner_hb_import_thunk *macrunner_hb_import_target_map[MACRUNNER_HB_IMPORT_TARGET_MAP_SIZE];
static struct macrunner_hb_apiset_module macrunner_hb_apiset_modules[MACRUNNER_HB_APISET_MODULE_MAX];
static unsigned int macrunner_hb_import_count;
static BOOL macrunner_hb_import_code_mapped;
static unsigned int macrunner_hb_apiset_module_count;
static int macrunner_hb_import_target_map_overflow;
static pthread_mutex_t macrunner_hb_tls_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned char macrunner_hb_tls_slots[MACRUNNER_HB_TLS_SLOT_MAX];
static unsigned char macrunner_hb_fls_slots[MACRUNNER_HB_TLS_SLOT_MAX];
static uint64_t macrunner_hb_fls_callbacks[MACRUNNER_HB_TLS_SLOT_MAX];
static struct macrunner_hb_tls_thread_values macrunner_hb_tls_thread_values[MACRUNNER_HB_TLS_THREAD_MAX];
static pthread_mutex_t macrunner_hb_error_mode_mutex = PTHREAD_MUTEX_INITIALIZER;
static DWORD macrunner_hb_error_mode;
static __thread DWORD macrunner_hb_thread_error_mode;
static __thread DWORD macrunner_hb_com_model;
static __thread unsigned int macrunner_hb_com_init_count;
static pthread_mutex_t macrunner_hb_initial_env_mutex = PTHREAD_MUTEX_INITIALIZER;
static char **macrunner_hb_initial_narrow_env;
static WCHAR **macrunner_hb_initial_wide_env;
static WCHAR *macrunner_hb_empty_wide_env[] = { NULL };
static char *macrunner_hb_empty_narrow_env[] = { NULL };
static pthread_mutex_t macrunner_hb_registered_message_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct macrunner_hb_registered_message macrunner_hb_registered_messages[MACRUNNER_HB_REGISTERED_MESSAGE_MAX];
static UINT macrunner_hb_registered_message_next = 0xc000;
static ATOM macrunner_hb_synthetic_class_next = 0xc000;
static uint64_t macrunner_hb_synthetic_hwnd_next = MACRUNNER_HB_PSEUDO_HWND_BASE;
static pthread_mutex_t macrunner_hb_slist_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t macrunner_hb_local_heap_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct macrunner_hb_local_heap macrunner_hb_local_heaps[MACRUNNER_HB_LOCAL_HEAP_MAX];
static struct macrunner_hb_local_heap_arena macrunner_hb_local_heap_arenas[MACRUNNER_HB_LOCAL_HEAP_ARENA_MAX];
static pthread_mutex_t macrunner_hb_local_file_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct macrunner_hb_local_file macrunner_hb_local_files[MACRUNNER_HB_LOCAL_FILE_MAX];
static uint64_t macrunner_hb_local_file_next = MACRUNNER_HB_LOCAL_FILE_BASE;
static pthread_mutex_t macrunner_hb_virtual_region_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct macrunner_hb_virtual_region macrunner_hb_virtual_regions[MACRUNNER_HB_VIRTUAL_REGION_MAX];
static unsigned int macrunner_hb_virtual_region_count;
static __thread void *macrunner_hb_bridge_stack_limit;
static __thread void *macrunner_hb_bridge_stack_base;
static __thread size_t macrunner_hb_bridge_stack_size;
static __thread uintptr_t macrunner_hb_native_call_guest_rsp;
static __thread void *macrunner_hb_original_stack_limit;
static __thread void *macrunner_hb_original_stack_base;
static pthread_mutex_t macrunner_hb_wow64_guest32_mutex = PTHREAD_MUTEX_INITIALIZER;
static hb_memory_t *macrunner_hb_wow64_guest32_mem;
static uint32_t macrunner_hb_wow64_guest32_next = 0x70000000u;

static uint64_t macrunner_hb_now_us(void)
{
    struct timeval tv;

    if (gettimeofday( &tv, NULL )) return 0;
    return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

__attribute__((visibility("default"))) void *macrunner_hb_wow64_guest32_memory(void)
{
    hb_memory_t *ret;

    pthread_mutex_lock( &macrunner_hb_wow64_guest32_mutex );
    if (!macrunner_hb_wow64_guest32_mem)
    {
        macrunner_hb_wow64_guest32_mem = hb_memory_create( 0 );
        if (macrunner_hb_wow64_guest32_mem &&
            hb_memory_guest32_reserve( macrunner_hb_wow64_guest32_mem ) != HB_OK)
        {
            hb_memory_destroy( macrunner_hb_wow64_guest32_mem );
            macrunner_hb_wow64_guest32_mem = NULL;
        }
        if (macrunner_hb_wow64_guest32_mem)
        {
            setenv( "MACRUNNER_HB_WOW64_GUEST32", "1", 1 );
        }
    }
    ret = macrunner_hb_wow64_guest32_mem;
    pthread_mutex_unlock( &macrunner_hb_wow64_guest32_mutex );
    return ret;
}

static hb_perm_t macrunner_hb_protect_to_perm( ULONG protect )
{
    hb_perm_t perm = HB_PERM_NONE;
    ULONG p = protect & 0xff;

    if (p == PAGE_NOACCESS) return HB_PERM_NONE;
    if (p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
        p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE ||
        p == PAGE_EXECUTE_WRITECOPY) perm = (hb_perm_t)(perm | HB_PERM_READ);
    if (p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
        p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY)
        perm = (hb_perm_t)(perm | HB_PERM_WRITE);
    if (p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
        p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY)
        perm = (hb_perm_t)(perm | HB_PERM_EXEC);
    return perm;
}

__attribute__((visibility("default"))) NTSTATUS macrunner_hb_wow64_guest32_alloc( SIZE_T size,
                                                                                  ULONG protect,
                                                                                  void **host_ptr )
{
    return macrunner_hb_wow64_guest32_alloc_range( size, protect, 0x10000000u, 0x70000000u, TRUE, host_ptr );
}

__attribute__((visibility("default"))) NTSTATUS macrunner_hb_wow64_guest32_map_fixed( ULONG_PTR guest_base,
                                                                                      SIZE_T size,
                                                                                      ULONG protect,
                                                                                      void **host_ptr )
{
    hb_memory_t *mem;
    SIZE_T map_size;
    hb_result_t result;
    uint32_t base = (uint32_t)guest_base;

    if (!host_ptr || !size) return STATUS_INVALID_PARAMETER;
    *host_ptr = NULL;
    if (guest_base > 0xffffffffu) return STATUS_INVALID_PARAMETER;

    if (!(mem = macrunner_hb_wow64_guest32_memory())) return STATUS_NO_MEMORY;
    map_size = (size + 0xfff) & ~(SIZE_T)0xfff;
    if ((uint64_t)base + map_size > HB_GUEST32_SIZE) return STATUS_INVALID_PARAMETER;

    pthread_mutex_lock( &macrunner_hb_wow64_guest32_mutex );
    result = hb_memory_guest32_map( mem, base, map_size, macrunner_hb_protect_to_perm( protect ) );
    pthread_mutex_unlock( &macrunner_hb_wow64_guest32_mutex );

    if (result != HB_OK)
    {
        if (result == HB_ERR_OUT_OF_MEMORY) return STATUS_NO_MEMORY;
        if (result == HB_ERR_INVALID_ARG) return STATUS_CONFLICTING_ADDRESSES;
        return STATUS_UNSUCCESSFUL;
    }
    *host_ptr = (BYTE *)hb_memory_guest32_base( mem ) + base;
    return STATUS_SUCCESS;
}

__attribute__((visibility("default"))) NTSTATUS macrunner_hb_wow64_guest32_protect( ULONG_PTR guest_base,
                                                                                    SIZE_T size,
                                                                                    ULONG protect )
{
    hb_memory_t *mem;
    hb_result_t result;
    ULONG_PTR normalized = guest_base;
    uint32_t base;

    if (!size) return STATUS_INVALID_PARAMETER;
    if (!(mem = macrunner_hb_wow64_guest32_memory())) return STATUS_NO_MEMORY;
    if (normalized > 0xffffffffu)
    {
        ULONG_PTR host_base = (ULONG_PTR)hb_memory_guest32_base( mem );

        if (!host_base || normalized < host_base || normalized - host_base >= HB_GUEST32_SIZE)
            return STATUS_INVALID_PARAMETER;
        normalized -= host_base;
    }
    base = (uint32_t)normalized;
    if ((uint64_t)base + size > HB_GUEST32_SIZE) return STATUS_INVALID_PARAMETER;

    pthread_mutex_lock( &macrunner_hb_wow64_guest32_mutex );
    result = hb_memory_guest32_protect( mem, base, size, macrunner_hb_protect_to_perm( protect ) );
    pthread_mutex_unlock( &macrunner_hb_wow64_guest32_mutex );

    if (result == HB_OK) return STATUS_SUCCESS;
    if (result == HB_ERR_OUT_OF_MEMORY) return STATUS_NO_MEMORY;
    if (result == HB_ERR_INVALID_ARG || result == HB_ERR_NOT_FOUND) return STATUS_CONFLICTING_ADDRESSES;
    return STATUS_UNSUCCESSFUL;
}

__attribute__((visibility("default"))) NTSTATUS macrunner_hb_wow64_guest32_alloc_range( SIZE_T size,
                                                                                        ULONG protect,
                                                                                        ULONG_PTR limit_low,
                                                                                        ULONG_PTR limit_high,
                                                                                        BOOL top_down,
                                                                                        void **host_ptr )
{
    hb_memory_t *mem;
    SIZE_T map_size;
    ULONG_PTR low, high;
    uint32_t base;
    hb_result_t result = HB_ERR_OUT_OF_MEMORY;
    unsigned int attempts = 0;

    if (!host_ptr || !size) return STATUS_INVALID_PARAMETER;
    *host_ptr = NULL;
    if (!(mem = macrunner_hb_wow64_guest32_memory())) return STATUS_NO_MEMORY;

    map_size = (size + 0xffff) & ~(SIZE_T)0xffff;
    if (!map_size || map_size > HB_GUEST32_SIZE) return STATUS_NO_MEMORY;
    low = limit_low ? limit_low : 0x10000u;
    high = limit_high ? limit_high : HB_GUEST32_SIZE;
    if (high > HB_GUEST32_SIZE) high = HB_GUEST32_SIZE;
    if (low < 0x10000u) low = 0x10000u;
    if (low >= high || map_size > high - low) return STATUS_NO_MEMORY;

    pthread_mutex_lock( &macrunner_hb_wow64_guest32_mutex );
    if (top_down)
    {
        base = (uint32_t)((high - map_size) & ~0xffffu);
        if (macrunner_hb_wow64_guest32_next < high && macrunner_hb_wow64_guest32_next >= low + map_size)
            base = (macrunner_hb_wow64_guest32_next - (uint32_t)map_size) & ~0xffffu;
        while (base >= low && attempts++ < 65536)
        {
            result = hb_memory_guest32_map( mem, base, map_size, macrunner_hb_protect_to_perm( protect ) );
            if (result == HB_OK) break;
            if (base < 0x10000u + map_size) break;
            base -= 0x10000u;
        }
        if (result == HB_OK) macrunner_hb_wow64_guest32_next = base;
    }
    else
    {
        base = (uint32_t)((low + 0xffffu) & ~0xffffu);
        while ((uint64_t)base + map_size <= high && attempts++ < 65536)
        {
            result = hb_memory_guest32_map( mem, base, map_size, macrunner_hb_protect_to_perm( protect ) );
            if (result == HB_OK) break;
            base += 0x10000u;
        }
    }
    pthread_mutex_unlock( &macrunner_hb_wow64_guest32_mutex );

    if (result != HB_OK)
    {
        if (result == HB_ERR_OUT_OF_MEMORY) return STATUS_NO_MEMORY;
        if (result == HB_ERR_INVALID_ARG) return STATUS_CONFLICTING_ADDRESSES;
        return STATUS_UNSUCCESSFUL;
    }
    *host_ptr = (BYTE *)hb_memory_guest32_base( mem ) + base;
    return STATUS_SUCCESS;
}

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

#define MACRUNNER_HB_MODULE_FROM_PC_CACHE_SIZE 64

struct macrunner_hb_module_from_pc_cache_entry
{
    uintptr_t start;
    uintptr_t end;
    void *module;
};

static __thread struct macrunner_hb_module_from_pc_cache_entry macrunner_hb_module_from_pc_cache[MACRUNNER_HB_MODULE_FROM_PC_CACHE_SIZE];
static __thread unsigned int macrunner_hb_module_from_pc_cache_next;

static void macrunner_hb_module_from_pc_cache_put( uintptr_t start, size_t size, void *module )
{
    struct macrunner_hb_module_from_pc_cache_entry *entry;

    if (!start || !size || !module) return;
    if (size > UINTPTR_MAX - start) return;
    entry = &macrunner_hb_module_from_pc_cache[
        macrunner_hb_module_from_pc_cache_next++ % MACRUNNER_HB_MODULE_FROM_PC_CACHE_SIZE];
    entry->start = start;
    entry->end = start + size;
    entry->module = module;
}

static void *macrunner_hb_module_from_pc( void *pc )
{
    uintptr_t addr = (uintptr_t)pc;
    uintptr_t p = (uintptr_t)pc & ~(uintptr_t)0xfff;
    unsigned int i;

    for (i = 0; i < MACRUNNER_HB_MODULE_FROM_PC_CACHE_SIZE; i++)
    {
        const struct macrunner_hb_module_from_pc_cache_entry *entry = &macrunner_hb_module_from_pc_cache[i];

        if (entry->module && addr >= entry->start && addr < entry->end)
            return entry->module;
    }

    for (i = 0; i < 0x100000 && p >= 0x1000; i++, p -= 0x1000)
    {
        IMAGE_DOS_HEADER dos;
        IMAGE_NT_HEADERS nt;
        uintptr_t nt_addr;
        size_t image_size;

        if (!macrunner_hb_read_local_memory( p, &dos, sizeof(dos) )) continue;
        if (dos.e_magic != IMAGE_DOS_SIGNATURE) continue;
        if (dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000) continue;
        nt_addr = p + dos.e_lfanew;
        if (!macrunner_hb_read_local_memory( nt_addr, &nt, sizeof(nt) )) continue;
        if (nt.Signature != IMAGE_NT_SIGNATURE) continue;
        image_size = nt.OptionalHeader.SizeOfImage;
        if (image_size && addr >= p && addr < p + image_size)
            macrunner_hb_module_from_pc_cache_put( p, image_size, (void *)p );
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

static void macrunner_hb_copy_unicode_ascii( char *dst, size_t dst_size, const UNICODE_STRING *src )
{
    unsigned int i, len;

    if (!dst || !dst_size) return;
    dst[0] = 0;
    if (!src || !src->Buffer || !src->Length) return;
    len = src->Length / sizeof(WCHAR);
    if (len >= dst_size) len = dst_size - 1;
    for (i = 0; i < len; i++)
    {
        WCHAR ch = src->Buffer[i];
        dst[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
    }
    dst[len] = 0;
}

static LDR_DATA_TABLE_ENTRY *macrunner_hb_ldr_entry_from_pc( void *pc )
{
    PEB *peb = NtCurrentTeb()->Peb;
    LIST_ENTRY *head, *entry;
    uintptr_t addr = (uintptr_t)pc;

    if (!peb || !peb->LdrData) return NULL;
    head = &peb->LdrData->InMemoryOrderModuleList;
    for (entry = head->Flink; entry && entry != head; entry = entry->Flink)
    {
        LDR_DATA_TABLE_ENTRY *ldr = CONTAINING_RECORD( entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks );
        uintptr_t base = (uintptr_t)ldr->DllBase;

        if (base && addr >= base && addr < base + ldr->SizeOfImage) return ldr;
    }
    return NULL;
}

static LDR_DATA_TABLE_ENTRY *macrunner_hb_ldr_entry_from_module( void *module )
{
    PEB *peb = NtCurrentTeb()->Peb;
    LIST_ENTRY *head, *entry;

    if (!peb || !peb->LdrData || !module) return NULL;
    head = &peb->LdrData->InMemoryOrderModuleList;
    for (entry = head->Flink; entry && entry != head; entry = entry->Flink)
    {
        LDR_DATA_TABLE_ENTRY *ldr = CONTAINING_RECORD( entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks );

        if (ldr->DllBase == module) return ldr;
    }
    return NULL;
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
     * DLL initializers can legitimately execute many tiny basic blocks before
     * returning; keep production unbounded unless diagnostics set an env cap. */
    if (label && !strcmp( label, "thread" )) return 0;
    if (label && !strcmp( label, "dll" )) return 0;
    if (label && !strcmp( label, "x64-signal-callback" )) return 0;
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
    if (label && !strcmp( label, "dll" ) && !block_limit) return 0;
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

static int macrunner_hb_env_flag( const char *name )
{
    const char *val = getenv( name );
    return val && val[0] && val[0] != '0';
}

static int macrunner_hb_cached_env_flag( int *cache, const char *name )
{
    int value = __atomic_load_n( cache, __ATOMIC_RELAXED );

    if (value < 0)
    {
        value = macrunner_hb_env_flag( name );
        __atomic_store_n( cache, value, __ATOMIC_RELAXED );
    }
    return value;
}

static int macrunner_hb_trace_abi_enabled(void)
{
    static int cache = -1;
    return macrunner_hb_cached_env_flag( &cache, "MACRUNNER_HB_TRACE_ABI" );
}

static int macrunner_hb_trace_abi_stack_enabled(void)
{
    static int cache = -1;
    return macrunner_hb_cached_env_flag( &cache, "MACRUNNER_HB_TRACE_ABI_STACK" );
}

static int macrunner_hb_trace_pe_stack_enabled(void)
{
    static int cache = -1;
    return macrunner_hb_cached_env_flag( &cache, "MACRUNNER_HB_TRACE_PE_STACK" );
}

static int macrunner_hb_trace_calc_object_enabled(void)
{
    static int cache = -1;
    return macrunner_hb_cached_env_flag( &cache, "MACRUNNER_HB_TRACE_CALC_OBJECT" );
}

static int macrunner_hb_trace_direct_native_enabled(void)
{
    static int cache = -1;
    return macrunner_hb_cached_env_flag( &cache, "MACRUNNER_HB_TRACE_DIRECT_NATIVE" );
}

static int macrunner_hb_trace_cfg_dispatch_enabled(void)
{
    static int cache = -1;
    return macrunner_hb_cached_env_flag( &cache, "MACRUNNER_HB_TRACE_CFG_DISPATCH" );
}

static int macrunner_hb_trace_file_api_enabled(void)
{
    static int cache = -1;
    return macrunner_hb_cached_env_flag( &cache, "MACRUNNER_HB_TRACE_FILE_API" );
}

static int macrunner_hb_trace_npp_open_pack_enabled(void)
{
    static int cache = -1;
    return macrunner_hb_cached_env_flag( &cache, "MACRUNNER_HB_TRACE_NPP_OPEN_PACK" );
}

static int macrunner_hb_trace_image_api_enabled(void)
{
    static int cache = -1;
    return macrunner_hb_cached_env_flag( &cache, "MACRUNNER_HB_TRACE_IMAGE_API" );
}

static int macrunner_hb_trace_special_vm_enabled(void)
{
    static int cache = -1;
    return macrunner_hb_cached_env_flag( &cache, "MACRUNNER_HB_TRACE_SPECIAL_VM" );
}

static int macrunner_hb_trace_native_writes_enabled(void)
{
    static int cache = -1;
    return macrunner_hb_cached_env_flag( &cache, "MACRUNNER_HB_TRACE_NATIVE_WRITES" );
}

static int macrunner_hb_trace_thread_lifecycle_enabled(void)
{
    static int cache = -1;
    int value = __atomic_load_n( &cache, __ATOMIC_RELAXED );

    if (value < 0)
    {
        value = macrunner_hb_env_flag( "MACRUNNER_HB_TRACE_BOOTSTRAP" ) ||
                macrunner_hb_env_flag( "MACRUNNER_TRACE_UI_INPUT" ) ||
                macrunner_hb_env_flag( "MACRUNNER_TRACE_UI_EVENT_PATH" ) ||
                macrunner_hb_env_flag( "MACRUNNER_TRACE_UI_WAIT" );
        __atomic_store_n( &cache, value, __ATOMIC_RELAXED );
    }
    return value;
}

static int macrunner_hb_trace_wait_semantic_enabled(void)
{
    static int cache = -1;
    int value = __atomic_load_n( &cache, __ATOMIC_RELAXED );

    if (value < 0)
    {
        value = macrunner_hb_env_flag( "MACRUNNER_HB_TRACE_WAIT_SEMANTIC" ) ||
                macrunner_hb_env_flag( "MACRUNNER_TRACE_UI_WAIT" );
        __atomic_store_n( &cache, value, __ATOMIC_RELAXED );
    }
    return value;
}

static int macrunner_hb_trace_wait_semantic_budget_allows(void)
{
    static int count;
    const char *val = getenv( "MACRUNNER_HB_TRACE_WAIT_SEMANTIC_BUDGET" );
    int limit = val && val[0] ? atoi( val ) : 400;

    if (!macrunner_hb_trace_wait_semantic_enabled()) return 0;
    if (limit <= 0) return 1;
    if (count < limit)
    {
        count++;
        return 1;
    }
    if (count == limit)
    {
        count++;
        fprintf( stderr, "macrunner-hb-wait-semantic: trace budget exhausted at %d entries, silencing\n",
                 limit );
        fflush( stderr );
    }
    return 0;
}

static uint64_t macrunner_hb_trace_return_address( hb_context_t *ctx )
{
    uint64_t ret = 0;
    if (ctx && ctx->memory)
        hb_memory_read_u64( ctx->memory, (hb_gva_t)ctx->regs.x64.rsp, &ret );
    return ret;
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
         macrunner_hb_strieq( thunk->import_name, "LoadIconW" ))) return TRUE;
    if (macrunner_hb_strieq( thunk->dll_name, "gdi32.dll" ) &&
        (macrunner_hb_strieq( thunk->import_name, "CreateDIBSection" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetDIBits" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetDIBits" ) ||
         macrunner_hb_strieq( thunk->import_name, "StretchDIBits" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetObjectW" ))) return TRUE;
    if (macrunner_hb_strieq( thunk->dll_name, "windowscodecs.dll" ) ||
        macrunner_hb_strieq( thunk->dll_name, "windowscodecs" )) return TRUE;
    return FALSE;
}

static void macrunner_hb_trace_hex_bytes( const unsigned char *bytes, size_t count )
{
    size_t i;

    for (i = 0; i < count; i++) fprintf( stderr, "%02x", bytes[i] );
}

static BOOL macrunner_hb_read_bmi_header( hb_context_t *ctx, uint64_t bmi,
                                          int32_t *width, int32_t *height,
                                          uint16_t *bit_count, uint32_t *image_size )
{
    uint32_t w = 0, h = 0;

    if (!ctx || !ctx->memory || !bmi) return FALSE;
    if (hb_memory_read_u32( ctx->memory, (hb_gva_t)bmi + 4, &w ) != HB_OK) return FALSE;
    if (hb_memory_read_u32( ctx->memory, (hb_gva_t)bmi + 8, &h ) != HB_OK) return FALSE;
    if (hb_memory_read_u16( ctx->memory, (hb_gva_t)bmi + 14, bit_count ) != HB_OK) return FALSE;
    if (hb_memory_read_u32( ctx->memory, (hb_gva_t)bmi + 20, image_size ) != HB_OK) *image_size = 0;
    *width = (int32_t)w;
    *height = (int32_t)h;
    return TRUE;
}

static size_t macrunner_hb_bmi_image_size( int32_t width, int32_t height, uint16_t bit_count,
                                           uint32_t image_size, uint64_t rows_hint )
{
    uint64_t abs_width, abs_height, stride, rows, total;

    if (image_size) return image_size;
    if (!width || !height || !bit_count) return 0;
    abs_width = width < 0 ? -(uint64_t)width : (uint64_t)width;
    abs_height = height < 0 ? -(uint64_t)height : (uint64_t)height;
    rows = rows_hint ? rows_hint : abs_height;
    stride = ((abs_width * bit_count + 31) / 32) * 4;
    total = stride * rows;
    return total > (1024 * 1024) ? 0 : (size_t)total;
}

static void macrunner_hb_arm_mem_watch( uint64_t addr, size_t size, const char *why )
{
    char start[32], end[32];
    static unsigned int arm_count;

    if (!addr || !size || size > 1024 * 1024) return;
    if (!macrunner_hb_env_flag( "MACRUNNER_HB_TRACE_MEM_WATCH_AUTO" ) &&
        !macrunner_hb_env_flag( "MACRUNNER_HB_TRACE_MEM_WATCH" )) return;
    if (++arm_count > 32) return;
    snprintf( start, sizeof(start), "0x%llx", (unsigned long long)addr );
    snprintf( end, sizeof(end), "0x%llx", (unsigned long long)(addr + size - 1) );
    setenv( "MACRUNNER_HB_TRACE_MEM_WATCH", "1", 1 );
    setenv( "MACRUNNER_HB_TRACE_MEM_WATCH_START", start, 1 );
    setenv( "MACRUNNER_HB_TRACE_MEM_WATCH_END", end, 1 );
    fprintf( stderr, "macrunner-hb-image-watch: why=%s start=%s end=%s size=%zu\n",
             why, start, end, size );
}

static void macrunner_hb_trace_image_sample( hb_context_t *ctx, const char *phase,
                                             const char *label, uint64_t addr, size_t size )
{
    unsigned char bytes[32];
    size_t count, i, nonzero = 0;
    hb_result_t r;

    if (!ctx || !ctx->memory || !addr || !size) return;
    count = size < sizeof(bytes) ? size : sizeof(bytes);
    memset( bytes, 0, sizeof(bytes) );
    r = hb_memory_read( ctx->memory, (hb_gva_t)addr, bytes, count );
    if (r == HB_OK)
    {
        for (i = 0; i < count; i++) if (bytes[i]) nonzero++;
        fprintf( stderr, "macrunner-hb-image-bits: phase=%s label=%s addr=%p size=%zu "
                 "sample=%zu nonzero=%zu bytes=",
                 phase, label, (void *)(uintptr_t)addr, size, count, nonzero );
        macrunner_hb_trace_hex_bytes( bytes, count );
        fprintf( stderr, "\n" );
    }
    else
    {
        fprintf( stderr, "macrunner-hb-image-bits: phase=%s label=%s addr=%p size=%zu "
                 "read=%s\n", phase, label, (void *)(uintptr_t)addr, size, hb_result_string( r ) );
    }
}

static void macrunner_hb_trace_image_api( hb_context_t *ctx, const char *phase,
                                          const struct macrunner_hb_import_thunk *thunk,
                                          uint64_t ret_addr, uint64_t rc, const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX] )
{
    int32_t width = 0, height = 0;
    uint16_t bit_count = 0;
    uint32_t image_size = 0;
    size_t sample_size;

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
        if (!strcmp( phase, "after" ) && bits)
        {
            sample_size = macrunner_hb_bmi_image_size( (int32_t)width, (int32_t)height,
                                                       bit_count, image_size, 0 );
            macrunner_hb_trace_image_sample( ctx, phase, "CreateDIBSection.bits_out",
                                             bits, sample_size );
        }
    }

    if (ctx && ctx->memory && macrunner_hb_strieq( thunk->dll_name, "gdi32.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "GetDIBits" ) &&
        !strcmp( phase, "after" ) && args[4] && args[5] &&
        macrunner_hb_read_bmi_header( ctx, args[5], &width, &height, &bit_count, &image_size ))
    {
        sample_size = macrunner_hb_bmi_image_size( width, height, bit_count, image_size, args[3] );
        fprintf( stderr, "macrunner-hb-image-api: getdibits bits=%p bmi=%p width=%d height=%d "
                 "bpp=%u image_size=%zu rows=%llu rc=%p\n",
                 (void *)(uintptr_t)args[4], (void *)(uintptr_t)args[5], width, height,
                 bit_count, sample_size, (unsigned long long)args[3], (void *)(uintptr_t)rc );
        macrunner_hb_arm_mem_watch( args[4], sample_size, "GetDIBits.after" );
        macrunner_hb_trace_image_sample( ctx, phase, "GetDIBits.bits", args[4], sample_size );
    }

    if (ctx && ctx->memory && macrunner_hb_strieq( thunk->dll_name, "gdi32.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "SetDIBits" ) && args[4] && args[5] &&
        macrunner_hb_read_bmi_header( ctx, args[5], &width, &height, &bit_count, &image_size ))
    {
        sample_size = macrunner_hb_bmi_image_size( width, height, bit_count, image_size, args[3] );
        fprintf( stderr, "macrunner-hb-image-api: setdibits bits=%p bmi=%p width=%d height=%d "
                 "bpp=%u image_size=%zu rows=%llu phase=%s\n",
                 (void *)(uintptr_t)args[4], (void *)(uintptr_t)args[5], width, height,
                 bit_count, sample_size, (unsigned long long)args[3], phase );
        macrunner_hb_arm_mem_watch( args[4], sample_size, "SetDIBits" );
        macrunner_hb_trace_image_sample( ctx, phase, "SetDIBits.bits", args[4], sample_size );
    }

    if (ctx && ctx->memory && macrunner_hb_strieq( thunk->dll_name, "gdi32.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "StretchDIBits" ) && args[9] && args[10] &&
        macrunner_hb_read_bmi_header( ctx, args[10], &width, &height, &bit_count, &image_size ))
    {
        sample_size = macrunner_hb_bmi_image_size( width, height, bit_count, image_size, args[8] );
        fprintf( stderr, "macrunner-hb-image-api: stretchdibits bits=%p bmi=%p width=%d height=%d "
                 "bpp=%u image_size=%zu src_rect=%llux%llu phase=%s\n",
                 (void *)(uintptr_t)args[9], (void *)(uintptr_t)args[10], width, height,
                 bit_count, sample_size, (unsigned long long)args[7],
                 (unsigned long long)args[8], phase );
        macrunner_hb_arm_mem_watch( args[9], sample_size, "StretchDIBits" );
        macrunner_hb_trace_image_sample( ctx, phase, "StretchDIBits.bits", args[9], sample_size );
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
                                             uint64_t ret_addr, uint64_t rc, const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX] )
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

static int macrunner_hb_trace_cfg_dispatch_budget_allows(void)
{
    static int count;
    const char *val = getenv( "MACRUNNER_HB_TRACE_CFG_DISPATCH_BUDGET" );
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
        fprintf( stderr, "macrunner-hb-cfg-dispatch: budget exhausted at %d entries, silencing\n", limit );
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
    static int cache = -1;
    return macrunner_hb_cached_env_flag( &cache, "MACRUNNER_HB_TRACE_CALLBACK_ABI" );
}

static int macrunner_hb_trace_geometry_enabled(void)
{
    static int cache = -1;
    return macrunner_hb_cached_env_flag( &cache, "MACRUNNER_HB_TRACE_GEOMETRY" );
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
    if ((macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "GetModuleHandleW" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetModuleHandleA" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetModuleHandleExW" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetModuleHandleExA" ) ||
         macrunner_hb_strieq( thunk->import_name, "LoadLibraryA" ) ||
         macrunner_hb_strieq( thunk->import_name, "LoadLibraryW" ) ||
         macrunner_hb_strieq( thunk->import_name, "LoadLibraryExA" ) ||
         macrunner_hb_strieq( thunk->import_name, "LoadLibraryExW" ) ||
         macrunner_hb_strieq( thunk->import_name, "FreeLibrary" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetProcAddress" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetEnvironmentStringsA" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetEnvironmentStringsW" ) ||
         macrunner_hb_strieq( thunk->import_name, "FreeEnvironmentStringsA" ) ||
         macrunner_hb_strieq( thunk->import_name, "FreeEnvironmentStringsW" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetEnvironmentVariableA" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetEnvironmentVariableW" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetEnvironmentVariableA" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetEnvironmentVariableW" )))
        return TRUE;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "EnumResourceNamesW" ) ||
         macrunner_hb_strieq( thunk->import_name, "EnumResourceNamesA" ) ||
         macrunner_hb_strieq( thunk->import_name, "EnumResourceNamesExW" ) ||
         macrunner_hb_strieq( thunk->import_name, "EnumResourceNamesExA" ) ||
         macrunner_hb_strieq( thunk->import_name, "EnumResourceTypesW" ) ||
         macrunner_hb_strieq( thunk->import_name, "EnumResourceTypesA" ) ||
         macrunner_hb_strieq( thunk->import_name, "EnumResourceTypesExW" ) ||
         macrunner_hb_strieq( thunk->import_name, "EnumResourceTypesExA" ) ||
         macrunner_hb_strieq( thunk->import_name, "EnumResourceLanguagesW" ) ||
         macrunner_hb_strieq( thunk->import_name, "EnumResourceLanguagesA" ) ||
         macrunner_hb_strieq( thunk->import_name, "EnumResourceLanguagesExW" ) ||
         macrunner_hb_strieq( thunk->import_name, "EnumResourceLanguagesExA" )))
        return TRUE;
    return FALSE;
}

static BOOL macrunner_hb_trace_callback12_enabled(void)
{
    return macrunner_hb_env_enabled( "MACRUNNER_HB_TRACE_CALLBACK12" );
}

static BOOL macrunner_hb_trace_module_handle_enabled(void)
{
    static int cache = -1;
    return macrunner_hb_cached_env_flag( &cache, "MACRUNNER_HB_TRACE_MODULE_HANDLE" );
}

static void macrunner_hb_trace_module_handle_result( hb_context_t *ctx,
                                                     const struct macrunner_hb_import_thunk *thunk,
                                                     const char *path, const char *name, DWORD flags,
                                                     const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                     void *module, uint64_t ret, DWORD error )
{
    static int count;
    const int limit = 160;
    IMAGE_NT_HEADERS *nt;

    if (!macrunner_hb_trace_module_handle_enabled()) return;
    if (count++ >= limit)
    {
        if (count == limit + 1)
            fprintf( stderr, "macrunner-hb-module-handle: budget exhausted at %d entries\n", limit );
        return;
    }
    nt = macrunner_hb_image_nt_header( module );
    fprintf( stderr, "macrunner-hb-module-handle: import=%s!%s path=%s name=%s flags=0x%lx "
             "args=%p,%p,%p module=%p machine=0x%x ret=%p error=%lu pc=%p rsp=%p\n",
             thunk ? thunk->dll_name : "(none)", thunk ? thunk->import_name : "(none)",
             path ? path : "(none)", (name && name[0]) ? name : "(null)", (unsigned long)flags,
             (void *)(uintptr_t)args[0], (void *)(uintptr_t)args[1], (void *)(uintptr_t)args[2],
             module, nt ? nt->FileHeader.Machine : IMAGE_FILE_MACHINE_UNKNOWN,
             (void *)(uintptr_t)ret, (unsigned long)error,
             ctx ? (void *)(uintptr_t)ctx->regs.x64.rip : NULL,
             ctx ? (void *)(uintptr_t)ctx->regs.x64.rsp : NULL );
}

static void macrunner_hb_trace_local_rect( const char *label, uint64_t addr )
{
    RECT rect;

    if (!macrunner_hb_trace_geometry_enabled() || addr < 0x10000) return;
    if (!macrunner_hb_read_local_memory( (uintptr_t)addr, &rect, sizeof(rect) )) return;

    ERR( "macrunner-hb-geometry-local-rect: %s addr=%p rect=(%ld,%ld)-(%ld,%ld) size=%ldx%ld\n",
         label ? label : "?", (void *)(uintptr_t)addr, (long)rect.left, (long)rect.top,
         (long)rect.right, (long)rect.bottom, (long)(rect.right - rect.left),
         (long)(rect.bottom - rect.top) );
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
                     phase, i, (long)rects[i].left, (long)rects[i].top,
                     (long)rects[i].right, (long)rects[i].bottom );
        }
        else if (!args[2] && macrunner_hb_read_local_memory( (uintptr_t)args[3], &rects[0], sizeof(rects[0]) ))
        {
            ERR( "macrunner-hb-callback-abi-nccalc: phase=%s rect=(%ld,%ld)-(%ld,%ld)\n",
                 phase, (long)rects[0].left, (long)rects[0].top,
                 (long)rects[0].right, (long)rects[0].bottom );
        }
    }
    else if ((args[1] == WM_NCCREATE || args[1] == WM_CREATE) && args[3] >= 0x10000 &&
             macrunner_hb_read_local_memory( (uintptr_t)args[3], &cs, sizeof(cs) ))
    {
        ERR( "macrunner-hb-callback-abi-create: phase=%s create lpCreateParams=%p hInstance=%p "
             "menu=%p parent=%p cy=%ld cx=%ld y=%ld x=%ld style=%08lx name=%p class=%p ex=%08lx\n",
             phase, cs.lpCreateParams, cs.hInstance, cs.hMenu, cs.hwndParent,
             (long)cs.cy, (long)cs.cx, (long)cs.y, (long)cs.x, (unsigned long)cs.style,
             cs.lpszName, cs.lpszClass, (unsigned long)cs.dwExStyle );
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

static size_t macrunner_hb_import_target_hash( uint64_t target )
{
    return (size_t)(((target >> 4) ^ (target >> 17) ^ (target >> 32)) &
                    (MACRUNNER_HB_IMPORT_TARGET_MAP_SIZE - 1));
}

static void macrunner_hb_import_target_map_put( struct macrunner_hb_import_thunk *slot )
{
    size_t idx, probe;
    unsigned int i;

    if (!slot || !slot->target) return;
    idx = macrunner_hb_import_target_hash( (uint64_t)(uintptr_t)slot->target );
    for (i = 0; i < MACRUNNER_HB_IMPORT_TARGET_MAP_SIZE; i++)
    {
        probe = (idx + i) & (MACRUNNER_HB_IMPORT_TARGET_MAP_SIZE - 1);
        if (!macrunner_hb_import_target_map[probe] ||
            macrunner_hb_import_target_map[probe]->target == slot->target)
        {
            __atomic_store_n( &macrunner_hb_import_target_map[probe], slot, __ATOMIC_RELEASE );
            return;
        }
    }
    __atomic_store_n( &macrunner_hb_import_target_map_overflow, 1, __ATOMIC_RELEASE );
}

static BOOL macrunner_hb_ensure_import_code_page(void)
{
    void *base = (void *)(uintptr_t)MACRUNNER_HB_IMPORT_BASE;

    if (macrunner_hb_import_code_mapped) return TRUE;
    if (mmap( base, MACRUNNER_HB_IMPORT_CODE_SIZE, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0 ) != base)
    {
        WARN( "MacRunner HyperBridge failed to map ARM64 import thunk page %p-%p: %s\n",
              base, (char *)base + MACRUNNER_HB_IMPORT_CODE_SIZE, strerror(errno) );
        return FALSE;
    }
    macrunner_hb_import_code_mapped = TRUE;
    return TRUE;
}

static void macrunner_hb_emit_native_import_code_thunk( struct macrunner_hb_import_thunk *slot )
{
    uint32_t *code;
    uint64_t target;
    void *base = (void *)(uintptr_t)MACRUNNER_HB_IMPORT_BASE;

    if (!slot || !slot->target) return;
    if (current_machine != IMAGE_FILE_MACHINE_ARM64) return;
    if (slot->target_machine != current_machine) return;
    if (!macrunner_hb_ensure_import_code_page()) return;

    if (mprotect( base, MACRUNNER_HB_IMPORT_CODE_SIZE, PROT_READ | PROT_WRITE ))
    {
        WARN( "MacRunner HyperBridge failed to make ARM64 import thunk page writable: %s\n", strerror(errno) );
        return;
    }

    code = (uint32_t *)(uintptr_t)slot->guest_target;
    target = (uint64_t)(uintptr_t)slot->target;
    code[0] = 0x58000050; /* ldr x16, #8 */
    code[1] = 0xd61f0200; /* br x16 */
    memcpy( &code[2], &target, sizeof(target) );
    __builtin___clear_cache( (char *)code, (char *)code + MACRUNNER_HB_IMPORT_STRIDE );

    if (mprotect( base, MACRUNNER_HB_IMPORT_CODE_SIZE, PROT_READ | PROT_EXEC ))
        WARN( "MacRunner HyperBridge failed to make ARM64 import thunk page executable: %s\n", strerror(errno) );
}

static struct macrunner_hb_import_thunk *macrunner_hb_find_import_thunk_by_target( uint64_t target )
{
    size_t idx, probe;
    unsigned int i;

    idx = macrunner_hb_import_target_hash( target );
    for (i = 0; i < MACRUNNER_HB_IMPORT_TARGET_MAP_SIZE; i++)
    {
        struct macrunner_hb_import_thunk *slot;

        probe = (idx + i) & (MACRUNNER_HB_IMPORT_TARGET_MAP_SIZE - 1);
        slot = __atomic_load_n( &macrunner_hb_import_target_map[probe], __ATOMIC_ACQUIRE );
        if (!slot) break;
        if ((uint64_t)(uintptr_t)slot->target == target) return slot;
    }

    if (__atomic_load_n( &macrunner_hb_import_target_map_overflow, __ATOMIC_ACQUIRE ))
    {
        for (i = 0; i < macrunner_hb_import_count; i++)
            if ((uint64_t)(uintptr_t)macrunner_hb_imports[i].target == target) return &macrunner_hb_imports[i];
    }
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
    macrunner_hb_import_target_map_put( slot );
    macrunner_hb_emit_native_import_code_thunk( slot );
    TRACE( "MacRunner HyperBridge registered dynamic proc thunk %s!%s native=%p guest=%p\n",
           slot->dll_name, slot->import_name, slot->target, (void *)(uintptr_t)slot->guest_target );
    pthread_mutex_unlock( &macrunner_hb_import_mutex );
    return slot->guest_target;
}

static BOOL macrunner_hb_kernel_export_has_local_semantic( const char *dll_name, const char *import_name )
{
    if (!dll_name || !import_name) return FALSE;
    if (macrunner_hb_strieq( dll_name, "api-ms-win-crt-string-l1-1-0.dll" ) ||
        macrunner_hb_strieq( dll_name, "api-ms-win-crt-private-l1-1-0.dll" ) ||
        macrunner_hb_strieq( dll_name, "ucrtbase.dll" ) ||
        macrunner_hb_strieq( dll_name, "msvcrt.dll" ))
        return macrunner_hb_strieq( import_name, "memset" ) ||
               macrunner_hb_strieq( import_name, "memcpy" ) ||
               macrunner_hb_strieq( import_name, "memmove" ) ||
               macrunner_hb_strieq( import_name, "memchr" ) ||
               macrunner_hb_strieq( import_name, "memcmp" ) ||
               macrunner_hb_strieq( import_name, "strcmp" ) ||
               macrunner_hb_strieq( import_name, "strncmp" ) ||
               macrunner_hb_strieq( import_name, "strlen" ) ||
               macrunner_hb_strieq( import_name, "strnlen" ) ||
               macrunner_hb_strieq( import_name, "wcslen" ) ||
               macrunner_hb_strieq( import_name, "wcsnlen" ) ||
               macrunner_hb_strieq( import_name, "isdigit" ) ||
               macrunner_hb_strieq( import_name, "isspace" ) ||
               macrunner_hb_strieq( import_name, "isxdigit" ) ||
               macrunner_hb_strieq( import_name, "tolower" ) ||
               macrunner_hb_strieq( import_name, "toupper" ) ||
               macrunner_hb_strieq( import_name, "_tolower_l" ) ||
               macrunner_hb_strieq( import_name, "_toupper_l" ) ||
               macrunner_hb_strieq( import_name, "_iswalpha_l" ) ||
               macrunner_hb_strieq( import_name, "_iswcntrl_l" ) ||
               macrunner_hb_strieq( import_name, "_iswdigit_l" ) ||
               macrunner_hb_strieq( import_name, "_iswlower_l" ) ||
               macrunner_hb_strieq( import_name, "_iswprint_l" ) ||
               macrunner_hb_strieq( import_name, "_iswpunct_l" ) ||
               macrunner_hb_strieq( import_name, "_iswspace_l" ) ||
               macrunner_hb_strieq( import_name, "_iswupper_l" ) ||
               macrunner_hb_strieq( import_name, "_iswxdigit_l" ) ||
               macrunner_hb_strieq( import_name, "_towlower_l" ) ||
               macrunner_hb_strieq( import_name, "_towupper_l" ) ||
               macrunner_hb_strieq( import_name, "_strcoll_l" ) ||
               macrunner_hb_strieq( import_name, "_strxfrm_l" ) ||
               macrunner_hb_strieq( import_name, "_wcscoll_l" ) ||
               macrunner_hb_strieq( import_name, "_wcsxfrm_l" ) ||
               macrunner_hb_strieq( import_name, "_strdup" ) ||
               macrunner_hb_strieq( import_name, "mbrlen" );
    if (macrunner_hb_strieq( dll_name, "api-ms-win-core-winrt-l1-1-0.dll" ) ||
        macrunner_hb_strieq( dll_name, "combase.dll" ))
        return macrunner_hb_strieq( import_name, "RoInitialize" ) ||
               macrunner_hb_strieq( import_name, "RoUninitialize" ) ||
               macrunner_hb_strieq( import_name, "RoGetActivationFactory" ) ||
               macrunner_hb_strieq( import_name, "RoActivateInstance" ) ||
               macrunner_hb_strieq( import_name, "CoInitializeEx" ) ||
               macrunner_hb_strieq( import_name, "CoUninitialize" ) ||
               macrunner_hb_strieq( import_name, "CoTaskMemAlloc" ) ||
               macrunner_hb_strieq( import_name, "CoTaskMemRealloc" ) ||
               macrunner_hb_strieq( import_name, "CoTaskMemFree" );
    if (macrunner_hb_strieq( dll_name, "ole32.dll" ))
        return macrunner_hb_strieq( import_name, "CoInitialize" ) ||
               macrunner_hb_strieq( import_name, "CoInitializeEx" ) ||
               macrunner_hb_strieq( import_name, "CoUninitialize" ) ||
               macrunner_hb_strieq( import_name, "CoTaskMemAlloc" ) ||
               macrunner_hb_strieq( import_name, "CoTaskMemRealloc" ) ||
               macrunner_hb_strieq( import_name, "CoTaskMemFree" );
    if (macrunner_hb_strieq( dll_name, "api-ms-win-crt-locale-l1-1-0.dll" ) ||
        macrunner_hb_strieq( dll_name, "ucrtbase.dll" ) ||
        macrunner_hb_strieq( dll_name, "msvcrt.dll" ))
        return macrunner_hb_strieq( import_name, "_create_locale" ) ||
               macrunner_hb_strieq( import_name, "_free_locale" ) ||
               macrunner_hb_strieq( import_name, "_configthreadlocale" ) ||
               macrunner_hb_strieq( import_name, "setlocale" ) ||
               macrunner_hb_strieq( import_name, "__pctype_func" ) ||
               macrunner_hb_strieq( import_name, "___mb_cur_max_func" ) ||
               macrunner_hb_strieq( import_name, "___lc_codepage_func" ) ||
               macrunner_hb_strieq( import_name, "localeconv" );
    if (macrunner_hb_strieq( dll_name, "api-ms-win-crt-math-l1-1-0.dll" ) ||
        macrunner_hb_strieq( dll_name, "ucrtbase.dll" ) ||
        macrunner_hb_strieq( dll_name, "msvcrt.dll" ))
        return macrunner_hb_strieq( import_name, "ceilf" );
    if (macrunner_hb_strieq( dll_name, "api-ms-win-crt-stdio-l1-1-0.dll" ) ||
        macrunner_hb_strieq( dll_name, "ucrtbase.dll" ) ||
        macrunner_hb_strieq( dll_name, "msvcrt.dll" ))
        return macrunner_hb_strieq( import_name, "__acrt_iob_func" ) ||
               macrunner_hb_strieq( import_name, "__stdio_common_vfprintf" ) ||
               macrunner_hb_strieq( import_name, "__stdio_common_vfwprintf" ) ||
               macrunner_hb_strieq( import_name, "_fileno" ) ||
               macrunner_hb_strieq( import_name, "_fseeki64" ) ||
               macrunner_hb_strieq( import_name, "_ftelli64" ) ||
               macrunner_hb_strieq( import_name, "_setmode" ) ||
               macrunner_hb_strieq( import_name, "_wfopen" ) ||
               macrunner_hb_strieq( import_name, "fclose" ) ||
               macrunner_hb_strieq( import_name, "fflush" ) ||
               macrunner_hb_strieq( import_name, "fgetwc" ) ||
               macrunner_hb_strieq( import_name, "fopen" ) ||
               macrunner_hb_strieq( import_name, "fputc" ) ||
               macrunner_hb_strieq( import_name, "fputwc" ) ||
               macrunner_hb_strieq( import_name, "fread" ) ||
               macrunner_hb_strieq( import_name, "fseek" ) ||
               macrunner_hb_strieq( import_name, "fwrite" ) ||
               macrunner_hb_strieq( import_name, "getc" ) ||
               macrunner_hb_strieq( import_name, "setbuf" ) ||
               macrunner_hb_strieq( import_name, "ungetc" ) ||
               macrunner_hb_strieq( import_name, "ungetwc" );
    if (macrunner_hb_strieq( dll_name, "api-ms-win-crt-multibyte-l1-1-0.dll" ) ||
        macrunner_hb_strieq( dll_name, "ucrtbase.dll" ) ||
        macrunner_hb_strieq( dll_name, "msvcrt.dll" ))
        return macrunner_hb_strieq( import_name, "_mbtowc_l" );
    if (macrunner_hb_strieq( dll_name, "api-ms-win-crt-heap-l1-1-0.dll" ) ||
        macrunner_hb_strieq( dll_name, "ucrtbase.dll" ) ||
        macrunner_hb_strieq( dll_name, "msvcrt.dll" ))
        return macrunner_hb_strieq( import_name, "malloc" ) ||
               macrunner_hb_strieq( import_name, "free" ) ||
               macrunner_hb_strieq( import_name, "calloc" ) ||
               macrunner_hb_strieq( import_name, "realloc" ) ||
               macrunner_hb_strieq( import_name, "_aligned_malloc" ) ||
               macrunner_hb_strieq( import_name, "_aligned_free" );
    if (macrunner_hb_strieq( dll_name, "user32.dll" ))
        return macrunner_hb_strieq( import_name, "RegisterWindowMessageA" ) ||
               macrunner_hb_strieq( import_name, "RegisterWindowMessageW" ) ||
               macrunner_hb_strieq( import_name, "LoadCursorA" ) ||
               macrunner_hb_strieq( import_name, "LoadCursorW" ) ||
               macrunner_hb_strieq( import_name, "LoadIconA" ) ||
               macrunner_hb_strieq( import_name, "LoadIconW" ) ||
               macrunner_hb_strieq( import_name, "LoadImageA" ) ||
               macrunner_hb_strieq( import_name, "LoadImageW" ) ||
               macrunner_hb_strieq( import_name, "GetProcessWindowStation" ) ||
               macrunner_hb_strieq( import_name, "GetThreadDesktop" ) ||
               macrunner_hb_strieq( import_name, "GetUserObjectInformationA" ) ||
               macrunner_hb_strieq( import_name, "GetUserObjectInformationW" ) ||
               macrunner_hb_strieq( import_name, "OpenInputDesktop" ) ||
               macrunner_hb_strieq( import_name, "SetThreadDesktop" ) ||
               macrunner_hb_strieq( import_name, "CloseDesktop" );
    if (macrunner_hb_strieq( dll_name, "shell32.dll" ) ||
        macrunner_hb_strieq( dll_name, "shcore.dll" ))
        return macrunner_hb_strieq( import_name, "CommandLineToArgvW" ) ||
               macrunner_hb_strieq( import_name, "SHGetKnownFolderPath" );
    if (macrunner_hb_strieq( import_name, "WaitOnAddress" ) ||
        macrunner_hb_strieq( import_name, "WakeByAddressAll" ) ||
        macrunner_hb_strieq( import_name, "WakeByAddressSingle" ) ||
        macrunner_hb_strieq( import_name, "RtlWaitOnAddress" ) ||
        macrunner_hb_strieq( import_name, "RtlWakeAddressAll" ) ||
        macrunner_hb_strieq( import_name, "RtlWakeAddressSingle" ))
        return TRUE;
    if (macrunner_hb_strieq( dll_name, "advapi32.dll" ))
        return macrunner_hb_strieq( import_name, "OpenProcessToken" ) ||
               macrunner_hb_strieq( import_name, "OpenThreadToken" ) ||
               macrunner_hb_strieq( import_name, "GetTokenInformation" ) ||
               macrunner_hb_strieq( import_name, "RegOpenKeyW" ) ||
               macrunner_hb_strieq( import_name, "RegOpenKeyExW" ) ||
               macrunner_hb_strieq( import_name, "RegCreateKeyExW" ) ||
               macrunner_hb_strieq( import_name, "RegQueryValueExW" ) ||
               macrunner_hb_strieq( import_name, "RegSetValueExW" ) ||
               macrunner_hb_strieq( import_name, "RegDeleteValueW" ) ||
               macrunner_hb_strieq( import_name, "RegCloseKey" ) ||
               macrunner_hb_stristarts( import_name, "Event" ) ||
               macrunner_hb_strieq( import_name, "IsValidSid" ) ||
               macrunner_hb_strieq( import_name, "EqualSid" ) ||
               macrunner_hb_strieq( import_name, "GetLengthSid" ) ||
               macrunner_hb_strieq( import_name, "GetSidIdentifierAuthority" ) ||
               macrunner_hb_strieq( import_name, "GetSidSubAuthority" ) ||
               macrunner_hb_strieq( import_name, "GetSidSubAuthorityCount" );
    if (macrunner_hb_strieq( dll_name, "ntdll.dll" ) ||
        macrunner_hb_strieq( dll_name, "ntdll" ))
        return macrunner_hb_strieq( import_name, "NtQueryVirtualMemory" ) ||
               macrunner_hb_strieq( import_name, "LdrGetDllHandle" ) ||
               macrunner_hb_strieq( import_name, "LdrGetDllHandleEx" ) ||
               macrunner_hb_strieq( import_name, "RtlFindExportedRoutineByName" ) ||
               macrunner_hb_strieq( import_name, "RtlInitializeSRWLock" ) ||
               macrunner_hb_strieq( import_name, "RtlAcquireSRWLockExclusive" ) ||
               macrunner_hb_strieq( import_name, "RtlAcquireSRWLockShared" ) ||
               macrunner_hb_strieq( import_name, "RtlReleaseSRWLockExclusive" ) ||
               macrunner_hb_strieq( import_name, "RtlReleaseSRWLockShared" ) ||
               macrunner_hb_strieq( import_name, "RtlTryAcquireSRWLockExclusive" ) ||
               macrunner_hb_strieq( import_name, "RtlTryAcquireSRWLockShared" ) ||
               macrunner_hb_strieq( import_name, "RtlInitializeConditionVariable" ) ||
               macrunner_hb_strieq( import_name, "RtlSleepConditionVariableCS" ) ||
               macrunner_hb_strieq( import_name, "RtlSleepConditionVariableSRW" ) ||
               macrunner_hb_strieq( import_name, "RtlWakeAllConditionVariable" ) ||
               macrunner_hb_strieq( import_name, "RtlWakeConditionVariable" ) ||
               macrunner_hb_stristarts( import_name, "Etw" );
    if (macrunner_hb_strieq( dll_name, "psapi.dll" ))
        return macrunner_hb_strieq( import_name, "EnumProcessModules" ) ||
               macrunner_hb_strieq( import_name, "EnumProcessModulesEx" ) ||
               macrunner_hb_strieq( import_name, "GetModuleBaseNameA" ) ||
               macrunner_hb_strieq( import_name, "GetModuleBaseNameW" ) ||
               macrunner_hb_strieq( import_name, "GetModuleFileNameExA" ) ||
               macrunner_hb_strieq( import_name, "GetModuleFileNameExW" ) ||
               macrunner_hb_strieq( import_name, "GetModuleInformation" ) ||
               macrunner_hb_strieq( import_name, "GetProcessImageFileNameA" ) ||
               macrunner_hb_strieq( import_name, "GetProcessImageFileNameW" );
    if (!macrunner_hb_strieq( dll_name, "kernel32.dll" ) &&
        !macrunner_hb_strieq( dll_name, "kernelbase.dll" ))
        return FALSE;

    return macrunner_hb_strieq( import_name, "GetModuleHandleW" ) ||
           macrunner_hb_strieq( import_name, "GetModuleHandleA" ) ||
           macrunner_hb_strieq( import_name, "DisableThreadLibraryCalls" ) ||
           macrunner_hb_strieq( import_name, "GetModuleHandleExW" ) ||
           macrunner_hb_strieq( import_name, "GetModuleHandleExA" ) ||
           macrunner_hb_strieq( import_name, "LoadLibraryA" ) ||
           macrunner_hb_strieq( import_name, "LoadLibraryW" ) ||
           macrunner_hb_strieq( import_name, "LoadLibraryExA" ) ||
           macrunner_hb_strieq( import_name, "LoadLibraryExW" ) ||
           macrunner_hb_strieq( import_name, "FreeLibrary" ) ||
           macrunner_hb_strieq( import_name, "GetProcAddress" ) ||
           macrunner_hb_strieq( import_name, "EnumProcessModules" ) ||
           macrunner_hb_strieq( import_name, "EnumProcessModulesEx" ) ||
           macrunner_hb_strieq( import_name, "K32EnumProcessModules" ) ||
           macrunner_hb_strieq( import_name, "K32EnumProcessModulesEx" ) ||
           macrunner_hb_strieq( import_name, "GetModuleBaseNameA" ) ||
           macrunner_hb_strieq( import_name, "GetModuleBaseNameW" ) ||
           macrunner_hb_strieq( import_name, "K32GetModuleBaseNameA" ) ||
           macrunner_hb_strieq( import_name, "K32GetModuleBaseNameW" ) ||
           macrunner_hb_strieq( import_name, "GetModuleFileNameExA" ) ||
           macrunner_hb_strieq( import_name, "GetModuleFileNameExW" ) ||
           macrunner_hb_strieq( import_name, "K32GetModuleFileNameExA" ) ||
           macrunner_hb_strieq( import_name, "K32GetModuleFileNameExW" ) ||
           macrunner_hb_strieq( import_name, "GetModuleInformation" ) ||
           macrunner_hb_strieq( import_name, "K32GetModuleInformation" ) ||
           macrunner_hb_strieq( import_name, "GetProcessImageFileNameA" ) ||
           macrunner_hb_strieq( import_name, "GetProcessImageFileNameW" ) ||
           macrunner_hb_strieq( import_name, "K32GetProcessImageFileNameA" ) ||
           macrunner_hb_strieq( import_name, "K32GetProcessImageFileNameW" ) ||
           macrunner_hb_strieq( import_name, "GetEnvironmentStringsA" ) ||
           macrunner_hb_strieq( import_name, "GetEnvironmentStringsW" ) ||
           macrunner_hb_strieq( import_name, "FreeEnvironmentStringsA" ) ||
           macrunner_hb_strieq( import_name, "FreeEnvironmentStringsW" ) ||
           macrunner_hb_strieq( import_name, "GetCommandLineA" ) ||
           macrunner_hb_strieq( import_name, "GetCommandLineW" ) ||
           macrunner_hb_strieq( import_name, "GetModuleFileNameA" ) ||
           macrunner_hb_strieq( import_name, "GetModuleFileNameW" ) ||
           macrunner_hb_strieq( import_name, "FormatMessageA" ) ||
           macrunner_hb_strieq( import_name, "FormatMessageW" ) ||
           macrunner_hb_strieq( import_name, "QueryPerformanceFrequency" ) ||
           macrunner_hb_strieq( import_name, "QueryPerformanceCounter" ) ||
           macrunner_hb_strieq( import_name, "GetSystemTimePreciseAsFileTime" ) ||
           macrunner_hb_strieq( import_name, "GetSystemTimeAsFileTime" ) ||
           macrunner_hb_strieq( import_name, "GetSystemTime" ) ||
           macrunner_hb_strieq( import_name, "GetLocalTime" ) ||
           macrunner_hb_strieq( import_name, "SystemTimeToFileTime" ) ||
           macrunner_hb_strieq( import_name, "FileTimeToSystemTime" ) ||
           macrunner_hb_strieq( import_name, "SystemTimeToTzSpecificLocalTime" ) ||
           macrunner_hb_strieq( import_name, "TzSpecificLocalTimeToSystemTime" ) ||
           macrunner_hb_strieq( import_name, "GetTempPathA" ) ||
           macrunner_hb_strieq( import_name, "GetTempPathW" ) ||
           macrunner_hb_strieq( import_name, "GetTempPath2A" ) ||
           macrunner_hb_strieq( import_name, "GetTempPath2W" ) ||
           macrunner_hb_strieq( import_name, "GetACP" ) ||
           macrunner_hb_strieq( import_name, "GetOEMCP" ) ||
           macrunner_hb_strieq( import_name, "AreFileApisANSI" ) ||
           macrunner_hb_strieq( import_name, "IsValidCodePage" ) ||
           macrunner_hb_strieq( import_name, "GetCPInfo" ) ||
           macrunner_hb_strieq( import_name, "MultiByteToWideChar" ) ||
           macrunner_hb_strieq( import_name, "WideCharToMultiByte" ) ||
           macrunner_hb_strieq( import_name, "GetStringTypeW" ) ||
           macrunner_hb_strieq( import_name, "LCMapStringEx" ) ||
           macrunner_hb_strieq( import_name, "GetStartupInfoA" ) ||
           macrunner_hb_strieq( import_name, "GetStartupInfoW" ) ||
           macrunner_hb_strieq( import_name, "GetStdHandle" ) ||
           macrunner_hb_strieq( import_name, "GetFileType" ) ||
           macrunner_hb_strieq( import_name, "GetFileInformationByHandle" ) ||
           macrunner_hb_strieq( import_name, "WriteFile" ) ||
           macrunner_hb_strieq( import_name, "GetLastError" ) ||
           macrunner_hb_strieq( import_name, "SetLastError" ) ||
           macrunner_hb_strieq( import_name, "GetProcessHeap" ) ||
           macrunner_hb_strieq( import_name, "GetCurrentProcess" ) ||
           macrunner_hb_strieq( import_name, "GetCurrentThread" ) ||
           macrunner_hb_strieq( import_name, "GetCurrentProcessId" ) ||
           macrunner_hb_strieq( import_name, "GetCurrentThreadId" ) ||
           macrunner_hb_strieq( import_name, "GetTickCount" ) ||
           macrunner_hb_strieq( import_name, "GetTickCount64" ) ||
           macrunner_hb_strieq( import_name, "InitializeCriticalSection" ) ||
           macrunner_hb_strieq( import_name, "InitializeCriticalSectionAndSpinCount" ) ||
           macrunner_hb_strieq( import_name, "InitializeCriticalSectionEx" ) ||
           macrunner_hb_strieq( import_name, "DeleteCriticalSection" ) ||
           macrunner_hb_strieq( import_name, "EnterCriticalSection" ) ||
           macrunner_hb_strieq( import_name, "LeaveCriticalSection" ) ||
           macrunner_hb_strieq( import_name, "TryEnterCriticalSection" ) ||
           macrunner_hb_strieq( import_name, "InitializeSRWLock" ) ||
           macrunner_hb_strieq( import_name, "AcquireSRWLockExclusive" ) ||
           macrunner_hb_strieq( import_name, "AcquireSRWLockShared" ) ||
           macrunner_hb_strieq( import_name, "ReleaseSRWLockExclusive" ) ||
           macrunner_hb_strieq( import_name, "ReleaseSRWLockShared" ) ||
           macrunner_hb_strieq( import_name, "TryAcquireSRWLockExclusive" ) ||
           macrunner_hb_strieq( import_name, "TryAcquireSRWLockShared" ) ||
           macrunner_hb_strieq( import_name, "InitializeConditionVariable" ) ||
           macrunner_hb_strieq( import_name, "SleepConditionVariableCS" ) ||
           macrunner_hb_strieq( import_name, "SleepConditionVariableSRW" ) ||
           macrunner_hb_strieq( import_name, "WakeAllConditionVariable" ) ||
           macrunner_hb_strieq( import_name, "WakeConditionVariable" ) ||
           macrunner_hb_strieq( import_name, "InitializeSListHead" ) ||
           macrunner_hb_strieq( import_name, "InterlockedFlushSList" ) ||
           macrunner_hb_strieq( import_name, "InterlockedPopEntrySList" ) ||
           macrunner_hb_strieq( import_name, "InterlockedPushEntrySList" ) ||
           macrunner_hb_strieq( import_name, "InterlockedPushListSList" ) ||
           macrunner_hb_strieq( import_name, "InterlockedPushListSListEx" ) ||
           macrunner_hb_strieq( import_name, "QueryDepthSList" ) ||
            macrunner_hb_strieq( import_name, "VirtualAlloc" ) ||
           macrunner_hb_strieq( import_name, "VirtualAllocEx" ) ||
           macrunner_hb_strieq( import_name, "VirtualFree" ) ||
           macrunner_hb_strieq( import_name, "VirtualFreeEx" ) ||
           macrunner_hb_strieq( import_name, "VirtualProtect" ) ||
           macrunner_hb_strieq( import_name, "VirtualProtectEx" ) ||
           macrunner_hb_strieq( import_name, "VirtualQuery" ) ||
           macrunner_hb_strieq( import_name, "VirtualQueryEx" ) ||
           macrunner_hb_strieq( import_name, "GetLogicalProcessorInformation" ) ||
           macrunner_hb_strieq( import_name, "GetLogicalProcessorInformationEx" ) ||
           macrunner_hb_strieq( import_name, "TlsAlloc" ) ||
           macrunner_hb_strieq( import_name, "TlsSetValue" ) ||
           macrunner_hb_strieq( import_name, "TlsGetValue" ) ||
           macrunner_hb_strieq( import_name, "TlsFree" ) ||
           macrunner_hb_strieq( import_name, "FlsAlloc" ) ||
           macrunner_hb_strieq( import_name, "FlsSetValue" ) ||
           macrunner_hb_strieq( import_name, "FlsGetValue" ) ||
           macrunner_hb_strieq( import_name, "FlsFree" ) ||
           macrunner_hb_strieq( import_name, "CreateEventA" ) ||
           macrunner_hb_strieq( import_name, "CreateEventW" ) ||
           macrunner_hb_strieq( import_name, "CreateEventExA" ) ||
           macrunner_hb_strieq( import_name, "CreateEventExW" ) ||
           macrunner_hb_strieq( import_name, "OpenEventA" ) ||
           macrunner_hb_strieq( import_name, "OpenEventW" ) ||
           macrunner_hb_strieq( import_name, "SetEvent" ) ||
           macrunner_hb_strieq( import_name, "ResetEvent" ) ||
           macrunner_hb_strieq( import_name, "PulseEvent" ) ||
           macrunner_hb_strieq( import_name, "CreateSemaphoreA" ) ||
           macrunner_hb_strieq( import_name, "CreateSemaphoreW" ) ||
           macrunner_hb_strieq( import_name, "CreateSemaphoreExA" ) ||
            macrunner_hb_strieq( import_name, "CreateSemaphoreExW" ) ||
            macrunner_hb_strieq( import_name, "OpenSemaphoreA" ) ||
            macrunner_hb_strieq( import_name, "OpenSemaphoreW" ) ||
            macrunner_hb_strieq( import_name, "ReleaseSemaphore" ) ||
            macrunner_hb_strieq( import_name, "CreateMutexA" ) ||
            macrunner_hb_strieq( import_name, "CreateMutexW" ) ||
            macrunner_hb_strieq( import_name, "CreateMutexExA" ) ||
            macrunner_hb_strieq( import_name, "CreateMutexExW" ) ||
            macrunner_hb_strieq( import_name, "OpenMutexA" ) ||
            macrunner_hb_strieq( import_name, "OpenMutexW" ) ||
            macrunner_hb_strieq( import_name, "ReleaseMutex" ) ||
            macrunner_hb_strieq( import_name, "CreatePipe" ) ||
            macrunner_hb_strieq( import_name, "SetErrorMode" ) ||
            macrunner_hb_strieq( import_name, "GetErrorMode" ) ||
            macrunner_hb_strieq( import_name, "SetThreadErrorMode" ) ||
            macrunner_hb_strieq( import_name, "OutputDebugStringA" ) ||
           macrunner_hb_strieq( import_name, "OutputDebugStringW" ) ||
           macrunner_hb_strieq( import_name, "WaitForSingleObjectEx" ) ||
           macrunner_hb_strieq( import_name, "WaitForMultipleObjectsEx" ) ||
           macrunner_hb_strieq( import_name, "SetThreadDescription" ) ||
           macrunner_hb_strieq( import_name, "ResumeThread" ) ||
           macrunner_hb_strieq( import_name, "SuspendThread" ) ||
           macrunner_hb_strieq( import_name, "CreateDirectoryA" ) ||
           macrunner_hb_strieq( import_name, "CreateDirectoryW" ) ||
           macrunner_hb_strieq( import_name, "CreateDirectoryExA" ) ||
           macrunner_hb_strieq( import_name, "CreateDirectoryExW" ) ||
           macrunner_hb_strieq( import_name, "SetFileAttributesA" ) ||
           macrunner_hb_strieq( import_name, "SetFileAttributesW" ) ||
           macrunner_hb_strieq( import_name, "MoveFileA" ) ||
           macrunner_hb_strieq( import_name, "MoveFileW" ) ||
           macrunner_hb_strieq( import_name, "MoveFileExA" ) ||
           macrunner_hb_strieq( import_name, "MoveFileExW" ) ||
           macrunner_hb_strieq( import_name, "BCryptGenRandom" );
}

static void macrunner_hb_synthetic_d3d11_createdevice_target(void) {}
static void macrunner_hb_synthetic_d3d12_createdevice_target(void) {}
static void macrunner_hb_synthetic_dxgi_createfactory_target(void) {}
static void macrunner_hb_synthetic_dxgi_createfactory1_target(void) {}

static BOOL macrunner_hb_synthetic_d3d_enabled(void)
{
    const char *backend = getenv( "MACRUNNER_D3D_BACKEND" );

    return macrunner_hb_env_enabled( "MACRUNNER_D3D_TRACE" ) ||
           (backend && macrunner_hb_strieq( backend, "mock" ));
}

static const char *macrunner_hb_basename_a( const char *name )
{
    const char *base = name;

    if (!name) return "";
    for (; *name; name++)
        if (*name == '\\' || *name == '/') base = name + 1;
    return base;
}

static uint64_t macrunner_hb_synthetic_d3d_module_handle( const char *name )
{
    const char *base;

    if (!macrunner_hb_synthetic_d3d_enabled()) return 0;
    base = macrunner_hb_basename_a( name );
    if (macrunner_hb_strieq( base, "d3d11.dll" )) return MACRUNNER_HB_D3D11_MODULE;
    if (macrunner_hb_strieq( base, "d3d12.dll" )) return MACRUNNER_HB_D3D12_MODULE;
    if (macrunner_hb_strieq( base, "dxgi.dll" )) return MACRUNNER_HB_DXGI_MODULE;
    return 0;
}

static const char *macrunner_hb_synthetic_d3d_module_name( uint64_t handle )
{
    switch (handle)
    {
    case MACRUNNER_HB_D3D11_MODULE: return "d3d11.dll";
    case MACRUNNER_HB_D3D12_MODULE: return "d3d12.dll";
    case MACRUNNER_HB_DXGI_MODULE: return "dxgi.dll";
    default: return NULL;
    }
}

static const char *macrunner_hb_synthetic_d3d_api_for_proc( const char *dll_name,
                                                            const char *proc_name )
{
    if (macrunner_hb_strieq( dll_name, "d3d11.dll" ) &&
        macrunner_hb_strieq( proc_name, "D3D11CreateDevice" ))
        return "d3d11";
    if (macrunner_hb_strieq( dll_name, "d3d12.dll" ) &&
        macrunner_hb_strieq( proc_name, "D3D12CreateDevice" ))
        return "d3d12";
    if (macrunner_hb_strieq( dll_name, "dxgi.dll" ) &&
        (macrunner_hb_strieq( proc_name, "CreateDXGIFactory" ) ||
         macrunner_hb_strieq( proc_name, "CreateDXGIFactory1" )))
        return "dxgi";
    return NULL;
}

static void *macrunner_hb_synthetic_d3d_proc_target( const char *dll_name,
                                                     const char *proc_name )
{
    if (macrunner_hb_strieq( dll_name, "d3d11.dll" ) &&
        macrunner_hb_strieq( proc_name, "D3D11CreateDevice" ))
        return macrunner_hb_synthetic_d3d11_createdevice_target;
    if (macrunner_hb_strieq( dll_name, "d3d12.dll" ) &&
        macrunner_hb_strieq( proc_name, "D3D12CreateDevice" ))
        return macrunner_hb_synthetic_d3d12_createdevice_target;
    if (macrunner_hb_strieq( dll_name, "dxgi.dll" ) &&
        macrunner_hb_strieq( proc_name, "CreateDXGIFactory" ))
        return macrunner_hb_synthetic_dxgi_createfactory_target;
    if (macrunner_hb_strieq( dll_name, "dxgi.dll" ) &&
        macrunner_hb_strieq( proc_name, "CreateDXGIFactory1" ))
        return macrunner_hb_synthetic_dxgi_createfactory1_target;
    return NULL;
}

static uint64_t macrunner_hb_register_synthetic_import_thunk( const struct macrunner_hb_import_thunk *source,
                                                              uint64_t module_id,
                                                              const char *dll_name,
                                                              const char *import_name )
{
    struct macrunner_hb_import_thunk *slot;
    void *target;
    unsigned int i;

    if (!source || !dll_name || !import_name) return 0;
    if (!(target = macrunner_hb_synthetic_d3d_proc_target( dll_name, import_name ))) return 0;

    pthread_mutex_lock( &macrunner_hb_import_mutex );
    for (i = 0; i < macrunner_hb_import_count; i++)
    {
        slot = &macrunner_hb_imports[i];
        if (slot->module_id == module_id &&
            macrunner_hb_strieq( slot->dll_name, dll_name ) &&
            macrunner_hb_strieq( slot->import_name, import_name ))
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
    slot->module_id = module_id;
    slot->target_machine = current_machine;
    macrunner_hb_copy_cstr( slot->dll_name, sizeof(slot->dll_name), dll_name );
    macrunner_hb_copy_cstr( slot->import_name, sizeof(slot->import_name), import_name );
    macrunner_hb_import_target_map_put( slot );
    macrunner_hb_emit_native_import_code_thunk( slot );
    TRACE( "MacRunner HyperBridge registered synthetic proc thunk %s!%s guest=%p\n",
           slot->dll_name, slot->import_name, (void *)(uintptr_t)slot->guest_target );
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
    macrunner_hb_remember_apiset_module_locked( params->dll_name, params->target_module_id,
                                                params->target_module_machine );
    macrunner_hb_import_target_map_put( slot );
    macrunner_hb_emit_native_import_code_thunk( slot );
    params->guest_target = slot->guest_target;
    TRACE( "MacRunner HyperBridge registered import thunk %s!%s native=%p pe_call12=%p pe_callback12=%p guest=%p machine=%04x\n",
           slot->dll_name, slot->import_name, slot->target, slot->pe_call12, slot->pe_callback12,
           (void *)(uintptr_t)slot->guest_target, slot->target_machine );
    pthread_mutex_unlock( &macrunner_hb_import_mutex );
    return STATUS_SUCCESS;
}

static void macrunner_hb_cache_live_region( struct macrunner_hb_special *special,
                                            mach_vm_address_t region,
                                            mach_vm_size_t region_size,
                                            vm_prot_t protection )
{
    hb_perm_t perm = 0;

    if (!special || !special->mem || !region || !region_size) return;
    if (region + region_size < region) return;
    if (hb_memory_find_region( special->mem, (hb_gva_t)region )) return;

    if (protection & VM_PROT_READ) perm |= HB_PERM_READ;
    if (protection & VM_PROT_WRITE) perm |= HB_PERM_WRITE;
    if (protection & VM_PROT_EXECUTE) perm |= HB_PERM_EXEC;
    if (!perm) return;

    (void)hb_memory_map( special->mem, (hb_gva_t)region, (size_t)region_size, perm );
}

static hb_result_t macrunner_hb_special_read( void *user, hb_gva_t addr, void *out, size_t size )
{
    struct macrunner_hb_special *special = user;
    unsigned char *dst = out;
    size_t i;

    if (!special) return HB_ERR_MEMORY_FAULT;
#ifdef __APPLE__
    if (size && addr >= 0x7ffe1000ULL && addr + size >= addr &&
        addr + size <= 0x7ffe1000ULL + sizeof(void *))
    {
        void *dispatcher = __wine_syscall_dispatcher;

        memcpy( out, (const BYTE *)&dispatcher + (addr - 0x7ffe1000ULL), size );
        return HB_OK;
    }

    if (size && addr >= 0x7ffe0000ULL && addr + size >= addr &&
        addr + size <= 0x7ffe1000ULL)
    {
        /*
         * x64 syscall stubs in win32u/ntdll still use the canonical Windows
         * KUSER_SHARED_DATA address (0x7ffe0000).  Wine maps the live page at
         * WINE_USER_SHARED_DATA_ADDRESS on Apple ARM64, so bridge reads must
         * translate the low Windows address instead of querying Mach VM there.
         */
        memcpy( out, (const void *)(WINE_USER_SHARED_DATA_ADDRESS + (addr - 0x7ffe0000ULL)), size );
        return HB_OK;
    }
#endif
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
            macrunner_hb_cache_live_region( special, region, region_size, info.protection );

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
    if (macrunner_hb_trace_native_writes_enabled() &&
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
            macrunner_hb_cache_live_region( user, region, region_size, info.protection );

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

static size_t macrunner_hb_ir_cache_hash( uint64_t pc )
{
    return (size_t)((pc >> 4) ^ (pc >> 17) ^ (pc >> 32)) & (MACRUNNER_HB_IR_CACHE_SIZE - 1);
}

static hb_ir_func_t *macrunner_hb_ir_cache_find( struct macrunner_hb_ir_cache *cache, uint64_t pc )
{
    size_t idx, i;

    if (!cache) return NULL;
    idx = macrunner_hb_ir_cache_hash( pc );
    for (i = 0; i < MACRUNNER_HB_IR_CACHE_SIZE; i++)
    {
        struct macrunner_hb_ir_cache_entry *entry = &cache->entries[(idx + i) & (MACRUNNER_HB_IR_CACHE_SIZE - 1)];
        if (!entry->func) return NULL;
        if (entry->pc == pc) return entry->func;
    }
    return NULL;
}

static BOOL macrunner_hb_ir_cache_put( struct macrunner_hb_ir_cache *cache, uint64_t pc, hb_ir_func_t *func )
{
    size_t idx, i;

    if (!cache || !func) return FALSE;
    idx = macrunner_hb_ir_cache_hash( pc );
    for (i = 0; i < MACRUNNER_HB_IR_CACHE_SIZE; i++)
    {
        struct macrunner_hb_ir_cache_entry *entry = &cache->entries[(idx + i) & (MACRUNNER_HB_IR_CACHE_SIZE - 1)];
        if (!entry->func)
        {
            entry->pc = pc;
            entry->func = func;
            return TRUE;
        }
        if (entry->pc == pc) return TRUE;
    }
    return FALSE;
}

static BOOL macrunner_hb_x64_resolves_to_jmp_rax( hb_context_t *ctx, uint64_t pc, unsigned int depth )
{
    uint8_t op0, op1;
    uint32_t disp32;
    uint64_t slot, target;

    if (!ctx || !ctx->memory || !pc || depth > 4) return FALSE;
    if (hb_memory_read_u8( ctx->memory, (hb_gva_t)pc, &op0 ) != HB_OK ||
        hb_memory_read_u8( ctx->memory, (hb_gva_t)pc + 1, &op1 ) != HB_OK)
        return FALSE;

    /* ff e0 = jmp rax. MSVC CFG/XFG guard dispatch ultimately resolves here. */
    if (op0 == 0xff && op1 == 0xe0) return TRUE;

    /* ff 25 disp32 = jmp qword ptr [rip+disp32]. Follow import/guard slots. */
    if (op0 == 0xff && op1 == 0x25)
    {
        if (hb_memory_read_u32( ctx->memory, (hb_gva_t)pc + 2, &disp32 ) != HB_OK)
            return FALSE;
        slot = pc + 6 + (int64_t)(int32_t)disp32;
        if (hb_memory_read_u64( ctx->memory, (hb_gva_t)slot, &target ) != HB_OK)
            return FALSE;
        return macrunner_hb_x64_resolves_to_jmp_rax( ctx, target, depth + 1 );
    }

    return FALSE;
}

static hb_result_t macrunner_hb_try_x64_cfg_dispatch_fast_path( hb_context_t *ctx, const char *label,
                                                                ULONG64 *steps )
{
    uint64_t pc, target, slot, ret_addr, new_rsp;
    uint8_t op0, op1;
    uint32_t disp32;
    BOOL is_call;
    hb_result_t ret;

    if (!ctx || !ctx->memory) return HB_ERR_NOT_FOUND;
    pc = ctx->pc;

    if (hb_memory_read_u8( ctx->memory, (hb_gva_t)pc, &op0 ) != HB_OK ||
        hb_memory_read_u8( ctx->memory, (hb_gva_t)pc + 1, &op1 ) != HB_OK)
        return HB_ERR_NOT_FOUND;

    if (op0 == 0xff && op1 == 0xe0)
    {
        target = ctx->regs.x64.rax;
        is_call = FALSE;
        ret_addr = 0;
    }
    else if (op0 == 0xff && (op1 == 0x15 || op1 == 0x25))
    {
        if (hb_memory_read_u32( ctx->memory, (hb_gva_t)pc + 2, &disp32 ) != HB_OK)
            return HB_ERR_NOT_FOUND;
        slot = pc + 6 + (int64_t)(int32_t)disp32;
        if (hb_memory_read_u64( ctx->memory, (hb_gva_t)slot, &target ) != HB_OK ||
            !macrunner_hb_x64_resolves_to_jmp_rax( ctx, target, 0 ))
            return HB_ERR_NOT_FOUND;
        is_call = (op1 == 0x15);
        ret_addr = pc + 6;
        target = ctx->regs.x64.rax;
    }
    else return HB_ERR_NOT_FOUND;

    if (!target)
    {
        ctx->last_result = HB_ERR_EXEC_FAULT;
        return HB_ERR_EXEC_FAULT;
    }

    if (is_call)
    {
        new_rsp = ctx->regs.x64.rsp - 8;
        ret = hb_memory_write_u64( ctx->memory, (hb_gva_t)new_rsp, ret_addr );
        if (ret != HB_OK)
        {
            ctx->last_result = ret;
            return ret;
        }
        ctx->regs.x64.rsp = new_rsp;
    }

    ctx->pc = target;
    ctx->regs.x64.rip = target;
    ctx->last_result = HB_OK;
    if (steps) (*steps)++;

    if (macrunner_hb_trace_cfg_dispatch_enabled() && macrunner_hb_trace_cfg_dispatch_budget_allows())
        fprintf( stderr, "macrunner-hb-cfg-dispatch: label=%s pc=%p %s target=%p rsp=%p\n",
                 label ? label : "x64", (void *)(uintptr_t)pc, is_call ? "call" : "jmp",
                 (void *)(uintptr_t)target, (void *)(uintptr_t)ctx->regs.x64.rsp );

    return HB_OK;
}

static void macrunner_hb_ir_cache_destroy( struct macrunner_hb_ir_cache *cache )
{
    size_t i;

    if (!cache) return;
    for (i = 0; i < MACRUNNER_HB_IR_CACHE_SIZE; i++)
        if (cache->entries[i].func) hb_ir_func_destroy( cache->entries[i].func );
    free( cache );
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

static USHORT macrunner_hb_import_lookup_machine( const struct macrunner_hb_import_thunk *thunk )
{
    USHORT machine = IMAGE_FILE_MACHINE_UNKNOWN;
    TEB *teb = NtCurrentTeb();

    if (thunk && thunk->module_id)
        machine = macrunner_hb_module_machine( (void *)(uintptr_t)thunk->module_id );
    if (machine == IMAGE_FILE_MACHINE_AMD64 || machine == IMAGE_FILE_MACHINE_I386)
        return machine;

    if (teb && teb->Peb)
    {
        machine = macrunner_hb_module_machine( teb->Peb->ImageBaseAddress );
        if (machine == IMAGE_FILE_MACHINE_AMD64 || machine == IMAGE_FILE_MACHINE_I386)
            return machine;
    }

    return current_machine;
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

static BOOL macrunner_hb_read_guest_astr( hb_context_t *ctx, uint64_t addr, char *out, size_t out_size )
{
    size_t i;

    if (!out || !out_size) return FALSE;
    out[0] = 0;
    if (!ctx || !ctx->memory || !addr) return FALSE;

    for (i = 0; i + 1 < out_size; i++)
    {
        uint8_t ch;

        if (hb_memory_read_u8( ctx->memory, (hb_gva_t)addr + i, &ch ) != HB_OK)
            return FALSE;
        if (!ch) break;
        out[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
    }
    out[i] = 0;
    return TRUE;
}

static BOOL macrunner_hb_read_guest_wstr( hb_context_t *ctx, uint64_t addr, char *out, size_t out_size )
{
    size_t i;

    if (!out || !out_size) return FALSE;
    out[0] = 0;
    if (!ctx || !ctx->memory || !addr) return FALSE;

    for (i = 0; i + 1 < out_size; i++)
    {
        uint16_t ch;

        if (hb_memory_read_u16( ctx->memory, (hb_gva_t)addr + i * sizeof(uint16_t), &ch ) != HB_OK)
            return FALSE;
        if (!ch) break;
        out[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
    }
    out[i] = 0;
    return TRUE;
}

static BOOL macrunner_hb_read_guest_wstr_buf( hb_context_t *ctx, uint64_t addr, WCHAR *out, size_t out_count )
{
    size_t i;

    if (!out || !out_count) return FALSE;
    out[0] = 0;
    if (!ctx || !ctx->memory || !addr) return FALSE;

    for (i = 0; i + 1 < out_count; i++)
    {
        uint16_t ch;

        if (hb_memory_read_u16( ctx->memory, (hb_gva_t)addr + i * sizeof(uint16_t), &ch ) != HB_OK)
            return FALSE;
        out[i] = ch;
        if (!ch) return TRUE;
    }
    out[i] = 0;
    return TRUE;
}

static void macrunner_hb_canon_module_name( const char *src, char *dst, size_t dst_size )
{
    const char *base;
    size_t i;

    if (!dst || !dst_size) return;
    dst[0] = 0;
    if (!src) return;

    base = src;
    for (i = 0; src[i]; i++)
    {
        if (src[i] == '\\' || src[i] == '/' || src[i] == ':')
            base = src + i + 1;
    }

    for (i = 0; i + 1 < dst_size && base[i]; i++)
    {
        char ch = base[i];

        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        dst[i] = ch;
    }
    dst[i] = 0;
}

static BOOL macrunner_hb_module_name_matches( const char *requested,
                                              const UNICODE_STRING *module_name )
{
    char req[128], req_dll[132], raw_have[128], have[128];
    BOOL req_has_dot = FALSE;
    size_t len, i;

    macrunner_hb_canon_module_name( requested, req, sizeof(req) );
    macrunner_hb_copy_unicode_ascii( raw_have, sizeof(raw_have), module_name );
    macrunner_hb_canon_module_name( raw_have, have, sizeof(have) );
    if (!req[0] || !have[0]) return FALSE;
    if (macrunner_hb_strieq( req, have )) return TRUE;

    for (i = 0; req[i]; i++) if (req[i] == '.') req_has_dot = TRUE;
    len = strlen( req );
    if (!req_has_dot && len + 4 < sizeof(req_dll))
    {
        memcpy( req_dll, req, len );
        memcpy( req_dll + len, ".dll", 5 );
        if (macrunner_hb_strieq( req_dll, have )) return TRUE;
    }
    return FALSE;
}

static BOOL macrunner_hb_ascii_module_names_match( const char *requested, const char *candidate )
{
    char req[128], req_dll[132], cand[128];
    BOOL req_has_dot = FALSE;
    size_t len, i;

    macrunner_hb_canon_module_name( requested, req, sizeof(req) );
    macrunner_hb_canon_module_name( candidate, cand, sizeof(cand) );
    if (!req[0] || !cand[0]) return FALSE;
    if (macrunner_hb_strieq( req, cand )) return TRUE;

    for (i = 0; req[i]; i++) if (req[i] == '.') req_has_dot = TRUE;
    len = strlen( req );
    if (!req_has_dot && len + 4 < sizeof(req_dll))
    {
        memcpy( req_dll, req, len );
        memcpy( req_dll + len, ".dll", 5 );
        if (macrunner_hb_strieq( req_dll, cand )) return TRUE;
    }
    return FALSE;
}

static void macrunner_hb_remember_apiset_module_locked( const char *dll_name, uint64_t module_id,
                                                        USHORT machine )
{
    char canon[128];
    unsigned int i;

    if (!dll_name || !module_id || !machine) return;
    macrunner_hb_canon_module_name( dll_name, canon, sizeof(canon) );
    if (!macrunner_hb_stristarts( canon, "api-" ) && !macrunner_hb_stristarts( canon, "ext-" ))
        return;

    for (i = 0; i < macrunner_hb_apiset_module_count; i++)
    {
        struct macrunner_hb_apiset_module *entry = &macrunner_hb_apiset_modules[i];

        if (entry->machine == machine && macrunner_hb_ascii_module_names_match( canon, entry->dll_name ))
        {
            entry->module_id = module_id;
            return;
        }
    }

    if (macrunner_hb_apiset_module_count >= MACRUNNER_HB_APISET_MODULE_MAX) return;
    macrunner_hb_copy_cstr( macrunner_hb_apiset_modules[macrunner_hb_apiset_module_count].dll_name,
                            sizeof(macrunner_hb_apiset_modules[macrunner_hb_apiset_module_count].dll_name),
                            canon );
    macrunner_hb_apiset_modules[macrunner_hb_apiset_module_count].module_id = module_id;
    macrunner_hb_apiset_modules[macrunner_hb_apiset_module_count].machine = machine;
    macrunner_hb_apiset_module_count++;
}

static void *macrunner_hb_find_remembered_apiset_module( const char *dll_name, USHORT preferred_machine,
                                                         char *resolved_name, size_t resolved_name_size )
{
    char canon[128];
    uint64_t first = 0, exact = 0;
    USHORT first_machine = 0, exact_machine = 0;
    unsigned int i;
    void *module;

    macrunner_hb_canon_module_name( dll_name, canon, sizeof(canon) );
    if (!macrunner_hb_stristarts( canon, "api-" ) && !macrunner_hb_stristarts( canon, "ext-" ))
        return NULL;

    pthread_mutex_lock( &macrunner_hb_import_mutex );
    for (i = 0; i < macrunner_hb_apiset_module_count; i++)
    {
        const struct macrunner_hb_apiset_module *entry = &macrunner_hb_apiset_modules[i];

        if (!macrunner_hb_ascii_module_names_match( canon, entry->dll_name )) continue;
        if (!first)
        {
            first = entry->module_id;
            first_machine = entry->machine;
        }
        if (!preferred_machine || entry->machine == preferred_machine)
        {
            exact = entry->module_id;
            exact_machine = entry->machine;
            break;
        }
    }
    pthread_mutex_unlock( &macrunner_hb_import_mutex );

    module = (void *)(uintptr_t)(exact ? exact : first);
    if (!module) return NULL;
    if (resolved_name && resolved_name_size)
    {
        macrunner_hb_get_export_module_name( module, resolved_name, resolved_name_size );
        if (!resolved_name[0])
            snprintf( resolved_name, resolved_name_size, "api-set-target-%04x",
                      exact ? exact_machine : first_machine );
    }
    return module;
}

static void *macrunner_hb_find_loaded_module_by_name( const char *name, USHORT preferred_machine )
{
    PEB *peb = NtCurrentTeb()->Peb;
    LIST_ENTRY *head, *entry;
    void *first = NULL;
    unsigned int guard = 0;
    static int miss_dump_count;

    if (!peb || !peb->LdrData || !name || !name[0]) return NULL;
    head = &peb->LdrData->InMemoryOrderModuleList;
    for (entry = head->Flink; entry && entry != head && guard++ < 4096; entry = entry->Flink)
    {
        LDR_DATA_TABLE_ENTRY *ldr = CONTAINING_RECORD( entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks );
        void *base = ldr->DllBase;

        if (!base || !macrunner_hb_module_name_matches( name, &ldr->BaseDllName )) continue;
        if (!first) first = base;
        if (!preferred_machine || macrunner_hb_module_machine( base ) == preferred_machine)
            return base;
    }
    if (!first && macrunner_hb_trace_module_handle_enabled() && miss_dump_count++ < 3)
    {
        unsigned int dump_guard = 0;

        fprintf( stderr, "macrunner-hb-module-list-miss: requested=%s preferred=0x%x\n",
                 name, preferred_machine );
        for (entry = head->Flink; entry && entry != head && dump_guard < 96; entry = entry->Flink, dump_guard++)
        {
            LDR_DATA_TABLE_ENTRY *ldr = CONTAINING_RECORD( entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks );
            char base_name[128], full_name[256];

            macrunner_hb_copy_unicode_ascii( base_name, sizeof(base_name), &ldr->BaseDllName );
            macrunner_hb_copy_unicode_ascii( full_name, sizeof(full_name), &ldr->FullDllName );
            fprintf( stderr, "macrunner-hb-module-list-entry: index=%u base=%p machine=0x%x base_name=%s full_name=%s\n",
                     dump_guard, ldr->DllBase, (unsigned int)macrunner_hb_module_machine( ldr->DllBase ),
                     base_name[0] ? base_name : "(empty)", full_name[0] ? full_name : "(empty)" );
        }
    }
    return first;
}

static size_t macrunner_hb_wstrlen_local( const WCHAR *str )
{
    size_t len = 0;

    if (!str) return 0;
    while (str[len]) len++;
    return len;
}

static WCHAR macrunner_hb_tolower_wchar( WCHAR ch )
{
    if (ch >= 'A' && ch <= 'Z') return ch + 'a' - 'A';
    return ch;
}

static WCHAR macrunner_hb_toupper_wchar( WCHAR ch )
{
    if (ch >= 'a' && ch <= 'z') return ch + 'A' - 'a';
    return ch;
}

static SIZE_T macrunner_hb_local_heap_allocation_size( SIZE_T requested_size )
{
    SIZE_T size = requested_size ? requested_size : 1;
    const SIZE_T align_mask = MACRUNNER_HB_LOCAL_HEAP_ALIGN - 1;

    if (size > ~(SIZE_T)0 - align_mask) return 0;
    return (size + align_mask) & ~align_mask;
}

static BOOL macrunner_hb_local_heap_remember( void *ptr, SIZE_T requested_size,
                                              SIZE_T allocation_size,
                                              void *allocation_base, BOOL arena_backed )
{
    unsigned int i;

    if (!ptr) return TRUE;
    pthread_mutex_lock( &macrunner_hb_local_heap_mutex );
    for (i = 0; i < MACRUNNER_HB_LOCAL_HEAP_MAX; i++)
    {
        if (macrunner_hb_local_heaps[i].base == ptr)
        {
            pthread_mutex_unlock( &macrunner_hb_local_heap_mutex );
            return TRUE;
        }
        if (!macrunner_hb_local_heaps[i].base)
        {
            macrunner_hb_local_heaps[i].base = ptr;
            macrunner_hb_local_heaps[i].requested_size = requested_size;
            macrunner_hb_local_heaps[i].allocation_size = allocation_size;
            macrunner_hb_local_heaps[i].allocation_base = allocation_base ? allocation_base : ptr;
            macrunner_hb_local_heaps[i].arena_backed = arena_backed;
            pthread_mutex_unlock( &macrunner_hb_local_heap_mutex );
            return TRUE;
        }
    }
    pthread_mutex_unlock( &macrunner_hb_local_heap_mutex );
    return FALSE;
}

static BOOL macrunner_hb_local_heap_forget( void *ptr, struct macrunner_hb_local_heap *entry )
{
    unsigned int i;

    if (!ptr) return TRUE;
    pthread_mutex_lock( &macrunner_hb_local_heap_mutex );
    for (i = 0; i < MACRUNNER_HB_LOCAL_HEAP_MAX; i++)
    {
        if (macrunner_hb_local_heaps[i].base == ptr)
        {
            if (entry) *entry = macrunner_hb_local_heaps[i];
            memset( &macrunner_hb_local_heaps[i], 0, sizeof(macrunner_hb_local_heaps[i]) );
            pthread_mutex_unlock( &macrunner_hb_local_heap_mutex );
            return TRUE;
        }
    }
    pthread_mutex_unlock( &macrunner_hb_local_heap_mutex );
    return FALSE;
}

static BOOL macrunner_hb_local_heap_lookup( void *ptr, SIZE_T *requested_size,
                                            SIZE_T *allocation_size )
{
    unsigned int i;

    if (!ptr) return FALSE;
    pthread_mutex_lock( &macrunner_hb_local_heap_mutex );
    for (i = 0; i < MACRUNNER_HB_LOCAL_HEAP_MAX; i++)
    {
        if (macrunner_hb_local_heaps[i].base == ptr)
        {
            if (requested_size) *requested_size = macrunner_hb_local_heaps[i].requested_size;
            if (allocation_size) *allocation_size = macrunner_hb_local_heaps[i].allocation_size;
            pthread_mutex_unlock( &macrunner_hb_local_heap_mutex );
            return TRUE;
        }
    }
    pthread_mutex_unlock( &macrunner_hb_local_heap_mutex );
    return FALSE;
}

static BOOL macrunner_hb_local_heap_contains( void *ptr )
{
    return macrunner_hb_local_heap_lookup( ptr, NULL, NULL );
}

static int macrunner_hb_heap_bucket_tail_enabled(void)
{
    static int cache = -1;
    return macrunner_hb_cached_env_flag( &cache, "MACRUNNER_HB_ENABLE_HEAP_BUCKET_TAIL_COMMIT" );
}

static int macrunner_hb_trace_heap_bucket_tail_enabled(void)
{
    static int cache = -1;
    return macrunner_hb_cached_env_flag( &cache, "MACRUNNER_HB_TRACE_HEAP_BUCKET_TAIL" );
}

static BOOL macrunner_hb_memory_protect_writable( ULONG protect )
{
    protect &= 0xff;
    return protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
           protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
}

static BOOL macrunner_hb_trace_virtual_region_enabled(void)
{
    return macrunner_hb_env_enabled( "MACRUNNER_HB_TRACE_VIRTUAL_REGION" );
}

static void macrunner_hb_sync_virtual_region( hb_context_t *ctx, void *base, SIZE_T size,
                                              ULONG protect )
{
    hb_region_t *existing, *after;
    hb_result_t result = HB_OK;
    hb_perm_t perm;

    if (!ctx || !ctx->memory || !base || !size) return;
    perm = macrunner_hb_protect_to_perm( protect );
    existing = hb_memory_find_region( ctx->memory, (hb_gva_t)(uintptr_t)base );
    if (existing)
        result = hb_memory_protect( ctx->memory, (hb_gva_t)(uintptr_t)base, size, perm );
    else if (perm)
        result = hb_memory_map( ctx->memory, (hb_gva_t)(uintptr_t)base, size, perm );
    after = hb_memory_find_region( ctx->memory, (hb_gva_t)(uintptr_t)base );
    if (macrunner_hb_trace_virtual_region_enabled())
        fprintf( stderr, "macrunner-hb-virtual-sync: base=%p size=%#zx protect=%#lx perm=%#x "
                 "existing=%p result=%s after=%p after_base=%p after_size=%#zx after_perm=%#x\n",
                 base, (size_t)size, (unsigned long)protect, perm, existing, hb_result_string( result ),
                 after, after ? (void *)(uintptr_t)after->base : NULL,
                 after ? after->size : 0, after ? after->perm : 0 );
}

static void macrunner_hb_forget_virtual_region( hb_context_t *ctx, void *base )
{
    if (!ctx || !ctx->memory || !base) return;
    (void)hb_memory_unmap( ctx->memory, (hb_gva_t)(uintptr_t)base );
}

static void macrunner_hb_remember_virtual_region( void *base, SIZE_T size, ULONG protect )
{
    uint64_t addr = (uint64_t)(uintptr_t)base;
    unsigned int i;

    if (!base || !size) return;
    if (!macrunner_hb_protect_to_perm( protect ))
    {
        if (macrunner_hb_trace_virtual_region_enabled())
            fprintf( stderr, "macrunner-hb-virtual-record-skip: base=%p size=%#zx protect=%#lx\n",
                     base, (size_t)size, (unsigned long)protect );
        return;
    }

    pthread_mutex_lock( &macrunner_hb_virtual_region_mutex );
    for (i = 0; i < macrunner_hb_virtual_region_count; i++)
    {
        if (macrunner_hb_virtual_regions[i].base == addr)
        {
            macrunner_hb_virtual_regions[i].size = size;
            macrunner_hb_virtual_regions[i].protect = protect;
            if (macrunner_hb_trace_virtual_region_enabled())
                fprintf( stderr, "macrunner-hb-virtual-record-update: base=%p size=%#zx "
                         "protect=%#lx count=%u\n", base, (size_t)size, (unsigned long)protect,
                         macrunner_hb_virtual_region_count );
            pthread_mutex_unlock( &macrunner_hb_virtual_region_mutex );
            return;
        }
    }
    if (macrunner_hb_virtual_region_count < MACRUNNER_HB_VIRTUAL_REGION_MAX)
    {
        struct macrunner_hb_virtual_region *region =
            &macrunner_hb_virtual_regions[macrunner_hb_virtual_region_count++];
        region->base = addr;
        region->size = size;
        region->protect = protect;
        if (macrunner_hb_trace_virtual_region_enabled())
            fprintf( stderr, "macrunner-hb-virtual-record-add: base=%p size=%#zx protect=%#lx count=%u\n",
                     base, (size_t)size, (unsigned long)protect, macrunner_hb_virtual_region_count );
    }
    pthread_mutex_unlock( &macrunner_hb_virtual_region_mutex );
}

static void macrunner_hb_forget_virtual_region_record( void *base )
{
    uint64_t addr = (uint64_t)(uintptr_t)base;
    unsigned int i;

    if (!base) return;

    pthread_mutex_lock( &macrunner_hb_virtual_region_mutex );
    for (i = 0; i < macrunner_hb_virtual_region_count; )
    {
        if (macrunner_hb_virtual_regions[i].base == addr)
        {
            if (macrunner_hb_trace_virtual_region_enabled())
                fprintf( stderr, "macrunner-hb-virtual-record-remove: base=%p count=%u\n",
                         base, macrunner_hb_virtual_region_count );
            macrunner_hb_virtual_regions[i] =
                macrunner_hb_virtual_regions[--macrunner_hb_virtual_region_count];
            continue;
        }
        i++;
    }
    pthread_mutex_unlock( &macrunner_hb_virtual_region_mutex );
}

static void macrunner_hb_replay_virtual_regions( hb_context_t *ctx )
{
    unsigned int i;

    if (!ctx || !ctx->memory) return;

    pthread_mutex_lock( &macrunner_hb_virtual_region_mutex );
    if (macrunner_hb_trace_virtual_region_enabled())
        fprintf( stderr, "macrunner-hb-virtual-replay: count=%u\n",
                 macrunner_hb_virtual_region_count );
    for (i = 0; i < macrunner_hb_virtual_region_count; i++)
    {
        struct macrunner_hb_virtual_region region = macrunner_hb_virtual_regions[i];
        macrunner_hb_sync_virtual_region( ctx, (void *)(uintptr_t)region.base,
                                          region.size, region.protect );
    }
    pthread_mutex_unlock( &macrunner_hb_virtual_region_mutex );
}

static void macrunner_hb_ensure_heap_bucket_tail( hb_context_t *ctx, uint64_t ptr,
                                                  const struct macrunner_hb_import_thunk *thunk )
{
    const uintptr_t bucket_size = 0x01000000ULL;
    const uintptr_t page_size = 0x1000;
    uintptr_t bucket, tail_page;
    MEMORY_BASIC_INFORMATION info;
    SIZE_T result = 0;
    NTSTATUS status;
    BOOL writable = FALSE;

    if (!macrunner_hb_heap_bucket_tail_enabled() || !ptr) return;

    bucket = (uintptr_t)ptr & ~(bucket_size - 1);
    tail_page = (bucket + bucket_size - 0x10) & ~(page_size - 1);
    if (tail_page < 0x10000 || tail_page < bucket) return;

    memset( &info, 0, sizeof(info) );
    status = NtQueryVirtualMemory( NtCurrentProcess(), (void *)tail_page, MemoryBasicInformation,
                                   &info, sizeof(info), &result );
    if (!status && info.State == MEM_COMMIT && macrunner_hb_memory_protect_writable( info.Protect ))
        writable = TRUE;

    if (!writable)
    {
        void *base = (void *)tail_page;
        SIZE_T size = page_size;

        status = NtAllocateVirtualMemory( NtCurrentProcess(), &base, 0, &size,
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
        if (status == STATUS_CONFLICTING_ADDRESSES)
        {
            base = (void *)tail_page;
            size = page_size;
            status = NtAllocateVirtualMemory( NtCurrentProcess(), &base, 0, &size,
                                              MEM_COMMIT, PAGE_READWRITE );
        }
        if (!status && base == (void *)tail_page) writable = TRUE;
    }

    if (writable && ctx && ctx->memory)
        (void)hb_memory_map( ctx->memory, (hb_gva_t)tail_page, page_size, HB_PERM_READ | HB_PERM_WRITE );

    if (macrunner_hb_trace_heap_bucket_tail_enabled())
        fprintf( stderr, "macrunner-hb-heap-bucket-tail: import=%s!%s ptr=%p "
                 "bucket=%p tail=%p writable=%d query_status=%08lx state=0x%lx protect=0x%lx\n",
                 thunk ? thunk->dll_name : "?", thunk ? thunk->import_name : "?",
                 (void *)(uintptr_t)ptr, (void *)bucket, (void *)tail_page, writable,
                 (unsigned long)status, (unsigned long)info.State, (unsigned long)info.Protect );
}

static void *macrunner_hb_local_heap_alloc( SIZE_T requested_size, BOOL zero )
{
    SIZE_T block_size = macrunner_hb_local_heap_allocation_size( requested_size );
    unsigned int i, arena_slot = MACRUNNER_HB_LOCAL_HEAP_ARENA_MAX;
    void *ptr = NULL;
    void *arena_base = NULL;
    SIZE_T arena_size = MACRUNNER_HB_LOCAL_HEAP_ARENA_SIZE;
    NTSTATUS status;

    if (!block_size || block_size > MACRUNNER_HB_LOCAL_HEAP_ARENA_SIZE / 2)
    {
        SIZE_T reserve_size = block_size;
        void *base = NULL;

        if (!reserve_size) return NULL;
        if (reserve_size < MACRUNNER_HB_LOCAL_HEAP_ARENA_SIZE)
            reserve_size = MACRUNNER_HB_LOCAL_HEAP_ARENA_SIZE;
        status = NtAllocateVirtualMemory( NtCurrentProcess(), &base, 0, &reserve_size,
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
        if (status) return NULL;
        if (!macrunner_hb_local_heap_remember( base, requested_size, reserve_size, base, FALSE ))
        {
            SIZE_T free_size = 0;

            NtFreeVirtualMemory( NtCurrentProcess(), &base, &free_size, MEM_RELEASE );
            return NULL;
        }
        if (zero) memset( base, 0, requested_size ? requested_size : 1 );
        return base;
    }

    pthread_mutex_lock( &macrunner_hb_local_heap_mutex );
    for (i = 0; i < MACRUNNER_HB_LOCAL_HEAP_ARENA_MAX; i++)
    {
        struct macrunner_hb_local_heap_arena *arena = &macrunner_hb_local_heap_arenas[i];

        if (arena->base && arena->used + block_size <= arena->size)
        {
            ptr = arena->base + arena->used;
            arena->used += block_size;
            arena_base = arena->base;
            arena_size = arena->size;
            break;
        }
        if (!arena->base && arena_slot == MACRUNNER_HB_LOCAL_HEAP_ARENA_MAX)
            arena_slot = i;
    }
    pthread_mutex_unlock( &macrunner_hb_local_heap_mutex );

    if (!ptr)
    {
        if (arena_slot == MACRUNNER_HB_LOCAL_HEAP_ARENA_MAX) return NULL;
        status = NtAllocateVirtualMemory( NtCurrentProcess(), &arena_base, 0, &arena_size,
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
        if (status) return NULL;

        pthread_mutex_lock( &macrunner_hb_local_heap_mutex );
        if (!macrunner_hb_local_heap_arenas[arena_slot].base)
        {
            macrunner_hb_local_heap_arenas[arena_slot].base = arena_base;
            macrunner_hb_local_heap_arenas[arena_slot].size = arena_size;
            macrunner_hb_local_heap_arenas[arena_slot].used = block_size;
            ptr = arena_base;
        }
        pthread_mutex_unlock( &macrunner_hb_local_heap_mutex );
        if (!ptr)
        {
            SIZE_T free_size = 0;

            NtFreeVirtualMemory( NtCurrentProcess(), &arena_base, &free_size, MEM_RELEASE );
            return macrunner_hb_local_heap_alloc( requested_size, zero );
        }
    }

    if (!macrunner_hb_local_heap_remember( ptr, requested_size, block_size, arena_base, TRUE ))
    {
        if (!zero) memset( ptr, 0, block_size );
        return NULL;
    }
    if (zero) memset( ptr, 0, requested_size ? requested_size : 1 );
    return ptr;
}

static BOOL macrunner_hb_local_heap_release( void *ptr )
{
    struct macrunner_hb_local_heap entry;
    SIZE_T free_size = 0;
    void *allocation_base;

    memset( &entry, 0, sizeof(entry) );
    if (!macrunner_hb_local_heap_forget( ptr, &entry )) return FALSE;
    if (!ptr || entry.arena_backed) return TRUE;
    allocation_base = entry.allocation_base ? entry.allocation_base : ptr;
    return !NtFreeVirtualMemory( NtCurrentProcess(), &allocation_base, &free_size, MEM_RELEASE );
}

static void *macrunner_hb_local_heap_realloc( void *old_ptr, SIZE_T requested_size,
                                              BOOL zero_tail, BOOL in_place_only )
{
    SIZE_T old_requested_size = 0;
    void *new_ptr;
    SIZE_T copy_size;

    if (!old_ptr) return macrunner_hb_local_heap_alloc( requested_size, zero_tail );
    if (!macrunner_hb_local_heap_lookup( old_ptr, &old_requested_size, NULL )) return NULL;
    if (in_place_only) return NULL;
    new_ptr = macrunner_hb_local_heap_alloc( requested_size, FALSE );
    if (!new_ptr) return NULL;
    copy_size = old_requested_size < requested_size ? old_requested_size : requested_size;
    if (copy_size) memcpy( new_ptr, old_ptr, copy_size );
    if (zero_tail && requested_size > old_requested_size)
        memset( (BYTE *)new_ptr + old_requested_size, 0, requested_size - old_requested_size );
    macrunner_hb_local_heap_release( old_ptr );
    return new_ptr;
}

static uint64_t macrunner_hb_local_file_remember( int fd )
{
    unsigned int i;
    uint64_t handle = 0;

    if (fd < 0) return 0;
    pthread_mutex_lock( &macrunner_hb_local_file_mutex );
    for (i = 0; i < MACRUNNER_HB_LOCAL_FILE_MAX; i++)
    {
        if (!macrunner_hb_local_files[i].handle)
        {
            handle = macrunner_hb_local_file_next++;
            macrunner_hb_local_files[i].handle = handle;
            macrunner_hb_local_files[i].fd = fd;
            break;
        }
    }
    pthread_mutex_unlock( &macrunner_hb_local_file_mutex );
    return handle;
}

static int macrunner_hb_local_file_fd( uint64_t handle )
{
    unsigned int i;
    int fd = -1;

    pthread_mutex_lock( &macrunner_hb_local_file_mutex );
    for (i = 0; i < MACRUNNER_HB_LOCAL_FILE_MAX; i++)
    {
        if (macrunner_hb_local_files[i].handle == handle)
        {
            fd = macrunner_hb_local_files[i].fd;
            break;
        }
    }
    pthread_mutex_unlock( &macrunner_hb_local_file_mutex );
    return fd;
}

static BOOL macrunner_hb_local_file_close( uint64_t handle )
{
    unsigned int i;
    int fd = -1;

    pthread_mutex_lock( &macrunner_hb_local_file_mutex );
    for (i = 0; i < MACRUNNER_HB_LOCAL_FILE_MAX; i++)
    {
        if (macrunner_hb_local_files[i].handle == handle)
        {
            fd = macrunner_hb_local_files[i].fd;
            macrunner_hb_local_files[i].handle = 0;
            macrunner_hb_local_files[i].fd = -1;
            break;
        }
    }
    pthread_mutex_unlock( &macrunner_hb_local_file_mutex );
    if (fd < 0) return FALSE;
    close( fd );
    return TRUE;
}

static BOOL macrunner_hb_local_file_duplicate( uint64_t handle, uint64_t *dup_handle )
{
    int fd;
    int dup_fd;

    if (!dup_handle) return FALSE;
    *dup_handle = 0;
    fd = macrunner_hb_local_file_fd( handle );
    if (fd < 0) return FALSE;
    dup_fd = dup( fd );
    if (dup_fd < 0) return FALSE;
    *dup_handle = macrunner_hb_local_file_remember( dup_fd );
    if (!*dup_handle)
    {
        close( dup_fd );
        return FALSE;
    }
    return TRUE;
}

static int macrunner_hb_wcsnicmp_local( const WCHAR *a, const WCHAR *b, ULONG len )
{
    ULONG i;

    for (i = 0; i < len; i++)
    {
        WCHAR ca = macrunner_hb_tolower_wchar( a[i] );
        WCHAR cb = macrunner_hb_tolower_wchar( b[i] );

        if (ca != cb) return ca < cb ? -1 : 1;
        if (!ca) return 0;
    }
    return 0;
}

static NTSTATUS macrunner_hb_get_apiset_entry( const API_SET_NAMESPACE *map, const WCHAR *name,
                                               ULONG len, const API_SET_NAMESPACE_ENTRY **entry )
{
    static const WCHAR apiW[] = {'a','p','i','-',0};
    static const WCHAR extW[] = {'e','x','t','-',0};
    const API_SET_HASH_ENTRY *hash_entry;
    ULONG hash, i, hash_len;
    int min, max;

    if (entry) *entry = NULL;
    if (!entry || len <= 4) return STATUS_INVALID_PARAMETER;
    if (macrunner_hb_wcsnicmp_local( name, apiW, 4 ) &&
        macrunner_hb_wcsnicmp_local( name, extW, 4 ))
        return STATUS_INVALID_PARAMETER;
    if (!map) return STATUS_APISET_NOT_PRESENT;

    for (i = hash_len = 0; i < len; i++)
    {
        if (name[i] == '.') break;
        if (name[i] == '-') hash_len = i;
    }
    for (i = hash = 0; i < hash_len; i++)
        hash = hash * map->HashFactor + macrunner_hb_tolower_wchar( name[i] );

    hash_entry = (API_SET_HASH_ENTRY *)((char *)map + map->HashOffset);
    min = 0;
    max = map->Count - 1;
    while (min <= max)
    {
        int pos = (min + max) / 2;

        if (hash_entry[pos].Hash < hash) min = pos + 1;
        else if (hash_entry[pos].Hash > hash) max = pos - 1;
        else
        {
            *entry = (API_SET_NAMESPACE_ENTRY *)((char *)map + map->EntryOffset) + hash_entry[pos].Index;
            if ((*entry)->HashedLength != hash_len * sizeof(WCHAR)) break;
            if (macrunner_hb_wcsnicmp_local( (WCHAR *)((char *)map + (*entry)->NameOffset),
                                             name, hash_len ))
                break;
            return STATUS_SUCCESS;
        }
    }
    return STATUS_APISET_NOT_PRESENT;
}

static NTSTATUS macrunner_hb_get_apiset_target( const API_SET_NAMESPACE *map,
                                                const API_SET_NAMESPACE_ENTRY *entry,
                                                UNICODE_STRING *ret )
{
    const API_SET_VALUE_ENTRY *value;

    if (!map || !entry || !ret || !entry->ValueCount) return STATUS_DLL_NOT_FOUND;
    value = (API_SET_VALUE_ENTRY *)((char *)map + entry->ValueOffset);
    if (!value->ValueOffset) return STATUS_DLL_NOT_FOUND;
    ret->Buffer = (WCHAR *)((char *)map + value->ValueOffset);
    ret->Length = value->ValueLength;
    ret->MaximumLength = value->ValueLength;
    return STATUS_SUCCESS;
}

static BOOL macrunner_hb_resolve_known_apiset_target_name( const char *canon, char *target,
                                                           size_t target_size )
{
    const char *fallback = NULL;

    if (!canon || !target || !target_size) return FALSE;
    if (macrunner_hb_stristarts( canon, "api-ms-win-core-fibers-" ))
        fallback = "kernelbase.dll";
    else if (macrunner_hb_stristarts( canon, "api-ms-win-core-localization-" ))
        fallback = "kernelbase.dll";
    else if (macrunner_hb_stristarts( canon, "api-ms-win-core-winrt-l1-" ) ||
             macrunner_hb_stristarts( canon, "api-ms-win-core-winrt-string-l1-" ))
        fallback = "combase.dll";

    if (!fallback) return FALSE;
    macrunner_hb_copy_cstr( target, target_size, fallback );
    return TRUE;
}

static BOOL macrunner_hb_resolve_apiset_target_name( const char *name, char *target, size_t target_size )
{
    const API_SET_NAMESPACE *map;
    const API_SET_NAMESPACE_ENTRY *entry;
    UNICODE_STRING resolved;
    WCHAR wname[256];
    char canon[256];
    BOOL has_dot = FALSE;
    size_t i, len, out_len;

    if (!target || !target_size) return FALSE;
    target[0] = 0;
    if (!name || !name[0]) return FALSE;

    macrunner_hb_canon_module_name( name, canon, sizeof(canon) );
    if (macrunner_hb_stristarts( canon, "api-" ) == FALSE &&
        macrunner_hb_stristarts( canon, "ext-" ) == FALSE)
        return FALSE;

    for (i = 0; canon[i] && i + 1 < ARRAY_SIZE(wname); i++)
    {
        if (canon[i] == '.') has_dot = TRUE;
        wname[i] = (unsigned char)canon[i];
    }
    if (!has_dot && i + 4 < ARRAY_SIZE(wname))
    {
        wname[i++] = '.';
        wname[i++] = 'd';
        wname[i++] = 'l';
        wname[i++] = 'l';
    }
    wname[i] = 0;

    map = NtCurrentTeb()->Peb ? NtCurrentTeb()->Peb->ApiSetMap : NULL;
    len = macrunner_hb_wstrlen_local( wname );
    if (macrunner_hb_get_apiset_entry( map, wname, len, &entry ) ||
        macrunner_hb_get_apiset_target( map, entry, &resolved ))
        return macrunner_hb_resolve_known_apiset_target_name( canon, target, target_size );

    out_len = resolved.Length / sizeof(WCHAR);
    if (out_len >= target_size) out_len = target_size - 1;
    for (i = 0; i < out_len; i++)
    {
        WCHAR ch = resolved.Buffer[i];
        target[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
    }
    target[out_len] = 0;
    return target[0] != 0;
}

static void *macrunner_hb_find_loaded_module_or_apiset_target( const char *name,
                                                               USHORT preferred_machine,
                                                               char *resolved_name,
                                                               size_t resolved_name_size )
{
    void *module;
    char target[256];

    if (resolved_name && resolved_name_size) resolved_name[0] = 0;
    module = macrunner_hb_find_loaded_module_by_name( name, preferred_machine );
    if (module) return module;
    module = macrunner_hb_find_remembered_apiset_module( name, preferred_machine,
                                                         resolved_name, resolved_name_size );
    if (module) return module;
    if (!macrunner_hb_resolve_apiset_target_name( name, target, sizeof(target) ))
        return NULL;
    module = macrunner_hb_find_loaded_module_by_name( target, preferred_machine );
    if (module && resolved_name && resolved_name_size)
        macrunner_hb_copy_cstr( resolved_name, resolved_name_size, target );
    return module;
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

static BOOL macrunner_hb_address_in_section( void *module, const char *section, uint64_t target )
{
    IMAGE_NT_HEADERS *nt = macrunner_hb_image_nt_header( module );
    IMAGE_SECTION_HEADER *sec;
    uint64_t base = (uint64_t)(uintptr_t)module;
    unsigned int i;

    if (!nt || !section || !target) return FALSE;
    sec = IMAGE_FIRST_SECTION( nt );
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
    {
        uint64_t start, end, size;

        if (strncmp( (const char *)sec->Name, section, IMAGE_SIZEOF_SHORT_NAME )) continue;
        size = max( sec->Misc.VirtualSize, sec->SizeOfRawData );
        start = base + sec->VirtualAddress;
        end = start + size;
        return target >= start && target < end;
    }
    return FALSE;
}

static IMAGE_ARM64EC_METADATA *macrunner_hb_get_arm64x_metadata( void *module )
{
    IMAGE_DATA_DIRECTORY *dir;
    IMAGE_LOAD_CONFIG_DIRECTORY *cfg;
    IMAGE_NT_HEADERS *nt;
    ULONG size;

    if (!(nt = macrunner_hb_image_nt_header( module ))) return NULL;
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    if (!dir->VirtualAddress || !dir->Size) return NULL;
    cfg = (IMAGE_LOAD_CONFIG_DIRECTORY *)((BYTE *)module + dir->VirtualAddress);
    size = dir->Size;
    size = min( size, cfg->Size );
    if (size <= offsetof( IMAGE_LOAD_CONFIG_DIRECTORY, CHPEMetadataPointer )) return NULL;
    if (cfg->CHPEMetadataPointer <= (ULONG_PTR)module) return NULL;
    if (cfg->CHPEMetadataPointer >= (ULONG_PTR)module + nt->OptionalHeader.SizeOfImage)
        return NULL;
    return (IMAGE_ARM64EC_METADATA *)cfg->CHPEMetadataPointer;
}

static void *macrunner_hb_redirect_arm64x_thunk_to_native( void *module, void *ptr )
{
    IMAGE_ARM64EC_METADATA *metadata = macrunner_hb_get_arm64x_metadata( module );
    const IMAGE_ARM64EC_REDIRECTION_ENTRY *map;
    ULONG_PTR rva = (ULONG_PTR)ptr - (ULONG_PTR)module;
    int min, max;

    if (!metadata || !ptr) return ptr;
    map = (const IMAGE_ARM64EC_REDIRECTION_ENTRY *)((BYTE *)module + metadata->RedirectionMetadata);
    min = 0;
    max = metadata->RedirectionMetadataCount - 1;
    while (min <= max)
    {
        int pos = (min + max) / 2;

        if (map[pos].Source == rva) return (BYTE *)module + map[pos].Destination;
        if (map[pos].Source < rva) min = pos + 1;
        else max = pos - 1;
    }
    return ptr;
}

typedef void (*macrunner_hb_xtajit64_notify_alloc_t)(void *, SIZE_T, ULONG, ULONG, NTSTATUS);
typedef void (*macrunner_hb_xtajit64_notify_free_t)(void *, SIZE_T, ULONG, NTSTATUS);
typedef void (*macrunner_hb_xtajit64_notify_protect_t)(void *, SIZE_T, ULONG, NTSTATUS);

static void *macrunner_hb_xtajit64_unix_export( const char *name )
{
    void *proc = dlsym( RTLD_DEFAULT, name );
    void *handle;

    if (proc) return proc;
    handle = dlopen( "@rpath/xtajit64.so", RTLD_LAZY | RTLD_GLOBAL );
    return handle ? dlsym( handle, name ) : NULL;
}

static void macrunner_hb_notify_xtajit64_memory_alloc( void *base, SIZE_T size, ULONG type,
                                                       ULONG protect, NTSTATUS status )
{
    static macrunner_hb_xtajit64_notify_alloc_t notify;

    if (!notify) notify = (macrunner_hb_xtajit64_notify_alloc_t)
        macrunner_hb_xtajit64_unix_export( "macrunner_xtajit64_notify_memory_alloc_unix" );
    if (notify) notify( base, size, type, protect, status );
}

static void macrunner_hb_notify_xtajit64_memory_free( void *base, SIZE_T size, ULONG type,
                                                      NTSTATUS status )
{
    static macrunner_hb_xtajit64_notify_free_t notify;

    if (!notify) notify = (macrunner_hb_xtajit64_notify_free_t)
        macrunner_hb_xtajit64_unix_export( "macrunner_xtajit64_notify_memory_free_unix" );
    if (notify) notify( base, size, type, status );
}

static void macrunner_hb_notify_xtajit64_memory_protect( void *base, SIZE_T size, ULONG protect,
                                                         NTSTATUS status )
{
    static macrunner_hb_xtajit64_notify_protect_t notify;

    if (!notify) notify = (macrunner_hb_xtajit64_notify_protect_t)
        macrunner_hb_xtajit64_unix_export( "macrunner_xtajit64_notify_memory_protect_unix" );
    if (notify) notify( base, size, protect, status );
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
    if (rva >= dir->VirtualAddress && rva < dir->VirtualAddress + dir->Size) return NULL;
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
    if (macrunner_hb_is_registered_x64_guest_address( (void *)(uintptr_t)pc ) &&
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

int macrunner_hb_pc_is_x64_guest_code_no_lock( void *pc )
{
    void *module;

    if (!pc) return FALSE;
    if (!macrunner_hb_is_registered_x64_guest_address( pc )) return FALSE;
    module = macrunner_hb_module_from_pc( pc );
    if (!module || macrunner_hb_module_machine( module ) != IMAGE_FILE_MACHINE_AMD64) return FALSE;
    return macrunner_hb_pc_in_executable_section( module, (uint64_t)(uintptr_t)pc );
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

static uint64_t macrunner_hb_normalize_tls_callback_pc( uint64_t pc, void *image_base_ptr,
                                                        uint64_t image_size )
{
    IMAGE_DATA_DIRECTORY *data_dir;
    const IMAGE_TLS_DIRECTORY *tls;
    const PIMAGE_TLS_CALLBACK *callback;
    IMAGE_NT_HEADERS *nt;
    uint64_t image_base = (uint64_t)(uintptr_t)image_base_ptr;
    uint64_t image_end = image_base + image_size;
    uint64_t best = 0, best_delta = UINT64_MAX;
    unsigned int i;

    if (!pc || !image_base_ptr || !image_size || pc < image_base || pc >= image_end)
        return pc;
    nt = macrunner_hb_image_nt_header( image_base_ptr );
    if (!nt || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return pc;

    data_dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!data_dir->VirtualAddress || data_dir->Size < sizeof(*tls)) return pc;
    if ((uint64_t)data_dir->VirtualAddress + sizeof(*tls) > image_size) return pc;

    tls = (const IMAGE_TLS_DIRECTORY *)((const char *)image_base_ptr + data_dir->VirtualAddress);
    callback = (const PIMAGE_TLS_CALLBACK *)(uintptr_t)tls->AddressOfCallBacks;
    if (!callback || (uint64_t)(uintptr_t)callback < image_base ||
        (uint64_t)(uintptr_t)callback >= image_end)
        return pc;

    for (i = 0; i < 256; i++)
    {
        uint64_t slot = (uint64_t)(uintptr_t)&callback[i];
        uint64_t target;
        uint64_t delta;

        if (slot < image_base || slot + sizeof(*callback) > image_end) break;
        target = (uint64_t)(uintptr_t)callback[i];
        if (!target) break;
        if (target < image_base || target >= image_end) continue;
        if (!macrunner_hb_pc_in_executable_section( image_base_ptr, target )) continue;
        if (pc == target) return pc;

        /*
         * ARM64 execute faults from an x64 callback can report the PC a few
         * bytes before or after the real x64 target.  Do not guess from local
         * padding bytes; TLS gives us the authoritative callback entry list.
         */
        delta = pc > target ? pc - target : target - pc;
        if (delta <= 4 && delta < best_delta)
        {
            best = target;
            best_delta = delta;
        }
    }

    if (best)
    {
        TRACE( "MacRunner HyperBridge normalized x64 TLS callback pc=%p -> %p delta=%s\n",
               (void *)(uintptr_t)pc, (void *)(uintptr_t)best, wine_dbgstr_longlong(best_delta) );
        return best;
    }
    return pc;
}

static BOOL macrunner_hb_read_amd64_image_size( uint64_t image_base, uint64_t *image_size )
{
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS nt;
    uint64_t nt_addr;

    if (image_size) *image_size = 0;
    if (!image_base || !image_size) return FALSE;
    if (!macrunner_hb_read_local_memory( (uintptr_t)image_base, &dos, sizeof(dos) )) return FALSE;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
    if (dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000) return FALSE;
    nt_addr = image_base + (uint64_t)dos.e_lfanew;
    if (nt_addr < image_base) return FALSE;
    if (!macrunner_hb_read_local_memory( (uintptr_t)nt_addr, &nt, sizeof(nt) )) return FALSE;
    if (nt.Signature != IMAGE_NT_SIGNATURE) return FALSE;
    if (nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return FALSE;
    if (!nt.OptionalHeader.SizeOfImage) return FALSE;
    *image_size = nt.OptionalHeader.SizeOfImage;
    return TRUE;
}

ULONG64 macrunner_hb_normalize_x64_tls_callback_pc( ULONG64 pc, ULONG64 image_base,
                                                    ULONG64 reason )
{
    void *image_base_ptr = (void *)(uintptr_t)image_base;
    uint64_t image_size;

    if (!pc || !image_base || reason > DLL_THREAD_DETACH) return pc;

    if (!macrunner_hb_read_amd64_image_size( image_base, &image_size )) return pc;
    if (!image_size || !macrunner_hb_pc_in_image( pc, image_base, image_size )) return pc;
    return macrunner_hb_normalize_tls_callback_pc( pc, image_base_ptr, image_size );
}

ULONG64 macrunner_hb_normalize_x64_callback_pc( ULONG64 pc )
{
    TEB *teb = NtCurrentTeb();
    void *image_base_ptr = NULL;
    void *module;
    uint64_t image_base, image_size, func_begin = 0, func_end = 0;
    hb_decoded_t decoded, current, best, boundary_best;
    uint64_t candidate, best_pc = 0, tls_pc;
    uint64_t boundary_pc = 0;
    unsigned int current_len = 0;
    unsigned int back;
    size_t len;

    if (!pc || !teb || !teb->Peb) return pc;
    if ((module = macrunner_hb_module_from_pc( (void *)(uintptr_t)pc )) &&
        macrunner_hb_module_machine( module ) == IMAGE_FILE_MACHINE_AMD64)
        image_base_ptr = module;
    else if (macrunner_hb_module_machine( teb->Peb->ImageBaseAddress ) == IMAGE_FILE_MACHINE_AMD64)
        image_base_ptr = teb->Peb->ImageBaseAddress;
    if (!image_base_ptr) return pc;

    image_base = (uint64_t)(uintptr_t)image_base_ptr;
    image_size = macrunner_hb_module_size( image_base_ptr );
    if (!macrunner_hb_pc_in_image( pc, image_base, image_size )) return pc;

    tls_pc = macrunner_hb_normalize_tls_callback_pc( pc, image_base_ptr, image_size );
    if (tls_pc != pc) return tls_pc;
    pc = tls_pc;
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
                   "mov x21, x1\n\t"         /* args[MACRUNNER_HB_IMPORT_ARG_MAX] */
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
                   "ldp x12, x13, [x21, #64]\n\t"
                   "stp x12, x13, [sp, #0]\n\t"
                   "ldp x12, x13, [x21, #80]\n\t"
                   "stp x12, x13, [sp, #16]\n\t"
                   "ldp x12, x13, [x21, #96]\n\t"
                   "stp x12, x13, [sp, #32]\n\t"
                   "ldp x12, x13, [x21, #112]\n\t"
                   "stp x12, x13, [sp, #48]\n\t"
                   "ldp x12, x13, [x21, #128]\n\t"
                   "stp x12, x13, [sp, #64]\n\t"
                   "ldp x12, x13, [x21, #144]\n\t"
                   "stp x12, x13, [sp, #80]\n\t"
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
                                                     const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX] )
{
#ifdef __aarch64__
    TEB *teb = NtCurrentTeb();
    void *target = thunk ? thunk->target : NULL;
    void *old_base;
    void *old_limit;
    void *old_deallocation_stack;
    void *restore_base;
    void *restore_limit;
    void *restore_deallocation_stack;
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
        typedef uint64_t (*macrunner_hb_native_fn20)( uint64_t, uint64_t, uint64_t, uint64_t,
                                                      uint64_t, uint64_t, uint64_t, uint64_t,
                                                      uint64_t, uint64_t, uint64_t, uint64_t,
                                                      uint64_t, uint64_t, uint64_t, uint64_t,
                                                      uint64_t, uint64_t, uint64_t, uint64_t );

        macrunner_hb_prepare_arm64_pe_call();
        return ((macrunner_hb_native_fn20)target)( args[0], args[1], args[2], args[3],
                                                   args[4], args[5], args[6], args[7],
                                                   args[8], args[9], args[10], args[11],
                                                   args[12], args[13], args[14], args[15],
                                                   args[16], args[17], args[18], args[19] );
    }

    restore_base = teb->Tib.StackBase;
    restore_limit = teb->Tib.StackLimit;
    restore_deallocation_stack = teb->DeallocationStack;
    old_base = macrunner_hb_original_stack_base ? macrunner_hb_original_stack_base : restore_base;
    old_limit = macrunner_hb_original_stack_limit ? macrunner_hb_original_stack_limit : restore_limit;
    old_deallocation_stack = teb->DeallocationStack;
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
        if (macrunner_hb_trace_pe_stack_enabled())
            ERR( "macrunner-hb-pe-stack: import=%s!%s guest_rsp=%p stack_top=%p "
                 "bridge=%p-%p raw_stack=%p slack=%#llx teb=%p-%p dealloc=%p "
                 "kernel=%p original=%p-%p\n",
                 thunk ? thunk->dll_name : "?", thunk ? thunk->import_name : "?",
                 (void *)guest_rsp, (void *)stack_top, macrunner_hb_bridge_stack_base,
                 (char *)macrunner_hb_bridge_stack_base + macrunner_hb_bridge_stack_size,
                 macrunner_hb_bridge_stack_limit, (unsigned long long)MACRUNNER_HB_SEH_STACK_SLACK,
                 restore_limit, restore_base, teb->DeallocationStack,
                 ntdll_get_thread_data()->kernel_stack, old_limit, old_base );
    }

    /*
     * Native ARM64 PE code is about to execute with SP on the HyperBridge
     * stack. Publish that stack as the current TEB stack. Do not merge it with
     * Wine's native stack: the two ranges are discontiguous, and ARM64 PE stack
     * probing treats TEB.StackLimit..StackBase as a single committed stack.
     */
    teb->DeallocationStack = macrunner_hb_bridge_stack_limit;
    teb->Tib.StackLimit = macrunner_hb_bridge_stack_limit;
    teb->Tib.StackBase = (char *)macrunner_hb_bridge_stack_base + macrunner_hb_bridge_stack_size;
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
        if (macrunner_hb_trace_callback12_enabled())
            fprintf( stderr, "macrunner-hb-callback12-call: import=%s!%s target=%p "
                     "args=%p,%p,%p,%p,%p,%p,%p,%p\n",
                     thunk ? thunk->dll_name : "(none)", thunk ? thunk->import_name : "(none)",
                     (void *)(uintptr_t)target,
                     (void *)(uintptr_t)args[0], (void *)(uintptr_t)args[1],
                     (void *)(uintptr_t)args[2], (void *)(uintptr_t)args[3],
                     (void *)(uintptr_t)args[4], (void *)(uintptr_t)args[5],
                     (void *)(uintptr_t)args[6], (void *)(uintptr_t)args[7] );
        macrunner_hb_prepare_arm64_pe_call();
        status = call_user_mode_callback( stack_top, &ret_ptr, &ret_len, thunk->pe_callback12, teb );
        ret = (!status && ret_ptr && ret_len >= sizeof(ret)) ? *(uint64_t *)ret_ptr : frame->ret;
        if (macrunner_hb_trace_callback12_enabled())
            fprintf( stderr, "macrunner-hb-callback12-return: import=%s!%s status=%08lx "
                     "ret=%p ret_ptr=%p ret_len=%lu frame_ret=%p\n",
                     thunk ? thunk->dll_name : "(none)", thunk ? thunk->import_name : "(none)",
                     (unsigned long)status, (void *)(uintptr_t)ret, ret_ptr,
                     (unsigned long)ret_len, (void *)(uintptr_t)frame->ret );
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
    teb->DeallocationStack = restore_deallocation_stack ? restore_deallocation_stack : old_deallocation_stack;
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
                                          uint64_t ret_addr, const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX] );
static void macrunner_hb_trace_abi_return( hb_context_t *ctx,
                                           const struct macrunner_hb_import_thunk *thunk,
                                           uint64_t ret_addr, uint64_t value,
                                           const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX] );
static void macrunner_hb_normalize_import_args( const struct macrunner_hb_import_thunk *thunk,
                                                uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX] );

static uint64_t macrunner_hb_call_arm64_pe_import12_for_ctx( hb_context_t *ctx,
                                                             const struct macrunner_hb_import_thunk *thunk,
                                                             const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX] )
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

static BOOL macrunner_hb_try_get_module_handle_semantic( hb_context_t *ctx,
                                                         const struct macrunner_hb_import_thunk *thunk,
                                                         const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                         uint64_t *ret )
{
    const DWORD valid_flags = GET_MODULE_HANDLE_EX_FLAG_PIN |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT |
                              GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;
    const BOOL is_kernel =
        thunk && (macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
                  macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" ));
    void *module = NULL;
    const char *path = "unknown";
    char name[128] = "";
    char resolved_name[256];
    DWORD flags = 0;
    BOOL ex_call = FALSE;
    USHORT lookup_machine;

    if (!ctx || !ctx->memory || !thunk || !args || !ret || !is_kernel) return FALSE;
    if (macrunner_hb_strieq( thunk->import_name, "GetModuleHandleExW" ) ||
        macrunner_hb_strieq( thunk->import_name, "GetModuleHandleExA" ))
    {
        flags = (DWORD)args[0];
        ex_call = TRUE;
    }
    else if (!macrunner_hb_strieq( thunk->import_name, "GetModuleHandleW" ) &&
             !macrunner_hb_strieq( thunk->import_name, "GetModuleHandleA" ))
        return FALSE;

    if (ex_call)
    {
        if (!args[2])
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            *ret = FALSE;
            macrunner_hb_trace_module_handle_result( ctx, thunk, "bad-output", name, flags, args,
                                                     NULL, *ret, ERROR_INVALID_PARAMETER );
            return TRUE;
        }
        if ((flags & ~valid_flags) ||
            ((flags & (GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT)) ==
             (GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT)))
        {
            hb_memory_write_u64( ctx->memory, (hb_gva_t)args[2], 0 );
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            *ret = FALSE;
            macrunner_hb_trace_module_handle_result( ctx, thunk, "bad-flags", name, flags, args,
                                                     NULL, *ret, ERROR_INVALID_PARAMETER );
            return TRUE;
        }
    }

    if (ex_call && (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS))
    {
        LDR_DATA_TABLE_ENTRY *ldr = macrunner_hb_ldr_entry_from_pc( (void *)(uintptr_t)args[1] );
        path = "from-address";
        module = ldr ? ldr->DllBase : NULL;
        if (ldr) macrunner_hb_copy_unicode_ascii( name, sizeof(name), &ldr->BaseDllName );
    }
    else if (!(ex_call ? args[1] : args[0]))
    {
        PEB *peb = NtCurrentTeb()->Peb;
        path = "image-base";
        module = peb ? peb->ImageBaseAddress : NULL;
    }
    else
    {
        uint64_t name_addr = ex_call ? args[1] : args[0];
        BOOL ok;

        if (macrunner_hb_strieq( thunk->import_name, "GetModuleHandleA" ) ||
            macrunner_hb_strieq( thunk->import_name, "GetModuleHandleExA" ))
            ok = macrunner_hb_read_guest_astr( ctx, name_addr, name, sizeof(name) );
        else
            ok = macrunner_hb_read_guest_wstr( ctx, name_addr, name, sizeof(name) );
        if (!ok)
        {
            if (ex_call) hb_memory_write_u64( ctx->memory, (hb_gva_t)args[2], 0 );
            RtlSetLastWin32Error( ERROR_MOD_NOT_FOUND );
            *ret = ex_call ? FALSE : 0;
            macrunner_hb_trace_module_handle_result( ctx, thunk, "name-read-failed", name,
                                                     flags, args, NULL, *ret, ERROR_MOD_NOT_FOUND );
            return TRUE;
        }
        path = "by-name";
        lookup_machine = macrunner_hb_import_lookup_machine( thunk );
        module = macrunner_hb_find_loaded_module_or_apiset_target( name, lookup_machine,
                                                                   resolved_name, sizeof(resolved_name) );
        if (module && resolved_name[0]) path = "api-set";
    }

    if (ex_call) hb_memory_write_u64( ctx->memory, (hb_gva_t)args[2], (uint64_t)(uintptr_t)module );
    if (!module)
    {
        RtlSetLastWin32Error( ERROR_MOD_NOT_FOUND );
        *ret = ex_call ? FALSE : 0;
        macrunner_hb_trace_module_handle_result( ctx, thunk, path, name, flags, args, NULL,
                                                 *ret, ERROR_MOD_NOT_FOUND );
        return TRUE;
    }

    RtlSetLastWin32Error( ERROR_SUCCESS );
    *ret = ex_call ? TRUE : (uint64_t)(uintptr_t)module;
    macrunner_hb_trace_module_handle_result( ctx, thunk, path, name, flags, args, module,
                                             *ret, ERROR_SUCCESS );
    return TRUE;
}

static BOOL macrunner_hb_read_guest_unicode_string_ascii( hb_context_t *ctx, uint64_t addr,
                                                          char *out, size_t out_size )
{
    uint16_t length;
    uint64_t buffer;
    size_t i, count;

    if (!out || !out_size) return FALSE;
    out[0] = 0;
    if (!ctx || !ctx->memory || !addr) return FALSE;
    if (hb_memory_read_u16( ctx->memory, (hb_gva_t)addr, &length ) != HB_OK ||
        hb_memory_read_u64( ctx->memory, (hb_gva_t)addr + 8, &buffer ) != HB_OK ||
        !buffer)
        return FALSE;

    count = length / sizeof(uint16_t);
    if (count >= out_size) count = out_size - 1;
    for (i = 0; i < count; i++)
    {
        uint16_t ch;

        if (hb_memory_read_u16( ctx->memory, (hb_gva_t)buffer + i * sizeof(uint16_t), &ch ) != HB_OK)
            return FALSE;
        out[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
    }
    out[i] = 0;
    return TRUE;
}

static BOOL macrunner_hb_try_ldr_module_semantic( hb_context_t *ctx,
                                                  const struct macrunner_hb_import_thunk *thunk,
                                                  const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                  uint64_t *ret )
{
    char name[256], resolved_name[256];
    uint64_t name_arg, base_arg;
    void *module = NULL;
    USHORT lookup_machine;
    NTSTATUS status;

    if (!ctx || !ctx->memory || !thunk || !args || !ret ||
        !macrunner_hb_strieq( thunk->dll_name, "ntdll.dll" ))
        return FALSE;

    resolved_name[0] = 0;

    if (macrunner_hb_strieq( thunk->import_name, "LdrGetDllHandle" ))
    {
        name_arg = args[2];
        base_arg = args[3];
    }
    else if (macrunner_hb_strieq( thunk->import_name, "LdrGetDllHandleEx" ))
    {
        name_arg = args[3];
        base_arg = args[4];
    }
    else return FALSE;

    if (!base_arg)
    {
        *ret = STATUS_INVALID_PARAMETER;
        NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
        return TRUE;
    }

    lookup_machine = macrunner_hb_import_lookup_machine( thunk );
    if (name_arg && macrunner_hb_read_guest_unicode_string_ascii( ctx, name_arg, name, sizeof(name) ))
        module = macrunner_hb_find_loaded_module_or_apiset_target( name, lookup_machine,
                                                                   resolved_name, sizeof(resolved_name) );
    else
    {
        PEB *peb = NtCurrentTeb()->Peb;

        name[0] = 0;
        module = peb ? peb->ImageBaseAddress : NULL;
    }
    if (!module && lookup_machine != current_machine && name[0])
        module = macrunner_hb_find_loaded_module_or_apiset_target( name, current_machine,
                                                                   resolved_name, sizeof(resolved_name) );

    status = module ? STATUS_SUCCESS : STATUS_DLL_NOT_FOUND;
    if (hb_memory_write_u64( ctx->memory, (hb_gva_t)base_arg, (uint64_t)(uintptr_t)module ) != HB_OK)
        status = STATUS_ACCESS_VIOLATION;
    NtCurrentTeb()->LastStatusValue = status;
    if (NT_ERROR( status )) RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
    else RtlSetLastWin32Error( ERROR_SUCCESS );
    *ret = status;
    TRACE( "MacRunner HyperBridge semantic %s!%s name=%s resolved=%s module=%p status=%08lx\n",
           thunk->dll_name, thunk->import_name, name,
           resolved_name[0] ? resolved_name : name, module, (unsigned long)status );
    return TRUE;
}

static BOOL macrunner_hb_try_ldr_export_semantic( hb_context_t *ctx,
                                                  const struct macrunner_hb_import_thunk *thunk,
                                                  const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                  uint64_t *ret )
{
    char proc_name[128];
    char export_module_name[96];
    void *module, *proc;
    USHORT machine;
    uint64_t guest_target = 0;

    if (!ctx || !ctx->memory || !thunk || !args || !ret ||
        !macrunner_hb_strieq( thunk->dll_name, "ntdll.dll" ) ||
        !macrunner_hb_strieq( thunk->import_name, "RtlFindExportedRoutineByName" ))
        return FALSE;

    module = (void *)(uintptr_t)args[0];
    if (!module || !args[1] || !macrunner_hb_read_guest_astr( ctx, args[1], proc_name, sizeof(proc_name) ))
    {
        *ret = 0;
        return TRUE;
    }
    if (!strcmp( proc_name, "__wine_unix_call_dispatcher" ))
    {
        static uint64_t *dispatcher_cell;

        if (!dispatcher_cell) dispatcher_cell = macrunner_hb_local_heap_alloc( sizeof(*dispatcher_cell), TRUE );
        if (!dispatcher_cell)
        {
            *ret = 0;
            return TRUE;
        }
        *dispatcher_cell = (uint64_t)(uintptr_t)__wine_unix_call_dispatcher;
        macrunner_hb_sync_virtual_region( ctx, dispatcher_cell, sizeof(*dispatcher_cell), PAGE_READWRITE );
        *ret = (uint64_t)(uintptr_t)dispatcher_cell;
        TRACE( "MacRunner HyperBridge semantic ntdll!RtlFindExportedRoutineByName "
               "name=%s ret=%p native-dispatcher-cell=%p\n",
               proc_name, (void *)(uintptr_t)*ret, (void *)(uintptr_t)*dispatcher_cell );
        return TRUE;
    }
    if (!strcmp( proc_name, "__wine_syscall_dispatcher" ))
    {
        static uint64_t *dispatcher_cell;

        if (!dispatcher_cell) dispatcher_cell = macrunner_hb_local_heap_alloc( sizeof(*dispatcher_cell), TRUE );
        if (!dispatcher_cell)
        {
            *ret = 0;
            return TRUE;
        }
        *dispatcher_cell = (uint64_t)(uintptr_t)__wine_syscall_dispatcher;
        macrunner_hb_sync_virtual_region( ctx, dispatcher_cell, sizeof(*dispatcher_cell), PAGE_READWRITE );
        *ret = (uint64_t)(uintptr_t)dispatcher_cell;
        TRACE( "MacRunner HyperBridge semantic ntdll!RtlFindExportedRoutineByName "
               "name=%s ret=%p native-dispatcher-cell=%p\n",
               proc_name, (void *)(uintptr_t)*ret, (void *)(uintptr_t)*dispatcher_cell );
        return TRUE;
    }
    proc = macrunner_hb_find_named_export( module, proc_name );
    if (!proc)
    {
        *ret = 0;
        return TRUE;
    }

    machine = macrunner_hb_module_machine( module );
    macrunner_hb_get_export_module_name( module, export_module_name, sizeof(export_module_name) );
    if (macrunner_hb_address_in_section( module, ".hexpthk", (uint64_t)(uintptr_t)proc ))
    {
        void *native_proc = macrunner_hb_redirect_arm64x_thunk_to_native( module, proc );

        if (native_proc && native_proc != proc)
            guest_target = macrunner_hb_register_dynamic_import_thunk( thunk, native_proc,
                                                                       module, proc_name );
        *ret = guest_target ? guest_target : (uint64_t)(uintptr_t)proc;
    }
    else if (machine == IMAGE_FILE_MACHINE_AMD64 || machine == IMAGE_FILE_MACHINE_I386)
    {
        if (macrunner_hb_kernel_export_has_local_semantic( export_module_name, proc_name ))
            guest_target = macrunner_hb_register_dynamic_import_thunk( thunk, proc, module, proc_name );
        *ret = guest_target ? guest_target : (uint64_t)(uintptr_t)proc;
    }
    else if (macrunner_hb_kernel_export_has_local_semantic( export_module_name, proc_name ))
    {
        guest_target = macrunner_hb_register_dynamic_import_thunk( thunk, proc, module, proc_name );
        *ret = guest_target;
    }
    else *ret = 0;

    TRACE( "MacRunner HyperBridge semantic ntdll!RtlFindExportedRoutineByName module=%p "
           "machine=%04x name=%s proc=%p ret=%p\n",
           module, machine, proc_name, proc, (void *)(uintptr_t)*ret );
    return TRUE;
}

static void macrunner_hb_d3d_trace_append( FILE *file, const char *api,
                                           const char *command, const char *payload )
{
    fprintf( file, "{\"api\":\"%s\",\"command\":\"%s\",\"payload\":%s}\n",
             api, command, payload );
}

static BOOL macrunner_hb_d3d_scenario_has( const char *scenario, const char *needle )
{
    return scenario && needle && strstr( scenario, needle ) != NULL;
}

static void macrunner_hb_write_synthetic_d3d_trace( const char *api )
{
    const char *path = getenv( "MACRUNNER_D3D_TRACE_PATH" );
    const char *scenario = getenv( "MACRUNNER_D3D_SCENARIO" );
    FILE *file;

    if (!path || !path[0]) return;
    if (!scenario || !scenario[0]) scenario = "triangle";
    if (!(file = fopen( path, "ab" ))) return;

    macrunner_hb_d3d_trace_append( file, api, "object_create",
        "{\"object_id\":1,\"object_type\":\"device\",\"refcount\":1}" );
    macrunner_hb_d3d_trace_append( file, api, "method_call",
        "{\"object_id\":1,\"method\":\"CreateDevice\",\"hresult\":0}" );
    macrunner_hb_d3d_trace_append( file, api, "create_render_target",
        "{\"width\":64,\"height\":64}" );
    macrunner_hb_d3d_trace_append( file, api, "clear",
        "{\"color\":[16,24,32,255]}" );

    if (macrunner_hb_d3d_scenario_has( scenario, "create_device" ) ||
        macrunner_hb_d3d_scenario_has( scenario, "clear" ))
    {
        macrunner_hb_d3d_trace_append( file, api, "present", "{}" );
        fclose( file );
        return;
    }

    if (macrunner_hb_d3d_scenario_has( scenario, "viewport_scissor" ))
    {
        macrunner_hb_d3d_trace_append( file, api, "set_viewport",
            "{\"x\":8,\"y\":8,\"width\":40,\"height\":40}" );
        macrunner_hb_d3d_trace_append( file, api, "set_scissor",
            "{\"x\":12,\"y\":12,\"width\":32,\"height\":32}" );
    }
    else
    {
        macrunner_hb_d3d_trace_append( file, api, "set_viewport",
            "{\"x\":0,\"y\":0,\"width\":64,\"height\":64}" );
        macrunner_hb_d3d_trace_append( file, api, "set_scissor",
            "{\"x\":0,\"y\":0,\"width\":64,\"height\":64}" );
    }
    macrunner_hb_d3d_trace_append( file, api, "set_topology",
        "{\"topology\":\"trianglelist\"}" );

    if (macrunner_hb_d3d_scenario_has( scenario, "constant_buffer" ))
        macrunner_hb_d3d_trace_append( file, api, "set_constant_buffer",
            "{\"slot\":0,\"bytes\":64,\"color\":[64,160,255,255]}" );
    else if (macrunner_hb_d3d_scenario_has( scenario, "descriptor_heap" ))
        macrunner_hb_d3d_trace_append( file, api, "create_descriptor_heap",
            "{\"type\":\"CBV_SRV_UAV\",\"count\":4}" );
    else if (macrunner_hb_d3d_scenario_has( scenario, "root_signature" ))
        macrunner_hb_d3d_trace_append( file, api, "create_root_signature",
            "{\"parameters\":1}" );
    else if (macrunner_hb_d3d_scenario_has( scenario, "fence_wait" ))
    {
        macrunner_hb_d3d_trace_append( file, api, "create_fence", "{\"initial_value\":0}" );
        macrunner_hb_d3d_trace_append( file, api, "signal", "{\"value\":1}" );
        macrunner_hb_d3d_trace_append( file, api, "wait_fence", "{\"value\":1}" );
    }
    else if (macrunner_hb_d3d_scenario_has( scenario, "barrier_transitions" ))
        macrunner_hb_d3d_trace_append( file, api, "resource_barrier",
            "{\"resource\":\"rt0\",\"before\":\"copy_dest\",\"after\":\"render_target\"}" );
    else if (macrunner_hb_d3d_scenario_has( scenario, "invalid_barrier_negative_test" ))
        macrunner_hb_d3d_trace_append( file, api, "resource_barrier",
            "{\"resource\":\"rt0\",\"before\":\"present\",\"after\":\"present\"}" );
    else if (macrunner_hb_d3d_scenario_has( scenario, "depth_clear" ))
    {
        macrunner_hb_d3d_trace_append( file, api, "create_depth_target", "{\"format\":\"d24s8\"}" );
        macrunner_hb_d3d_trace_append( file, api, "clear_depth", "{\"depth\":1.0,\"stencil\":0}" );
    }
    else if (macrunner_hb_d3d_scenario_has( scenario, "blend_state" ))
        macrunner_hb_d3d_trace_append( file, api, "set_blend_state", "{\"enabled\":true}" );
    else if (macrunner_hb_d3d_scenario_has( scenario, "rasterizer_state" ))
        macrunner_hb_d3d_trace_append( file, api, "set_rasterizer_state", "{\"cull\":\"none\"}" );
    else if (macrunner_hb_d3d_scenario_has( scenario, "sampler_state" ))
        macrunner_hb_d3d_trace_append( file, api, "set_sampler", "{\"slot\":0,\"filter\":\"linear\"}" );
    else if (macrunner_hb_d3d_scenario_has( scenario, "texture" ))
    {
        macrunner_hb_d3d_trace_append( file, api, "set_shader", "{\"shader\":\"texture-sample\"}" );
        macrunner_hb_d3d_trace_append( file, api, "set_texture",
            "{\"width\":2,\"height\":2,\"pixels\":[[255,255,0,255],[0,255,255,255],[255,0,255,255],[255,255,255,255]]}" );
    }
    else
        macrunner_hb_d3d_trace_append( file, api, "set_shader", "{\"shader\":\"vertex-color\"}" );

    macrunner_hb_d3d_trace_append( file, api, "set_vertex_buffer",
        "{\"vertices\":[{\"position\":[0.0,0.75,0,1],\"color\":[1,0,0,1],\"uv\":[0.5,0.0]},{\"position\":[-0.75,-0.75,0,1],\"color\":[0,1,0,1],\"uv\":[0.0,1.0]},{\"position\":[0.75,-0.75,0,1],\"color\":[0,0,1,1],\"uv\":[1.0,1.0]}]}" );
    if (macrunner_hb_d3d_scenario_has( scenario, "copy_resource" ))
        macrunner_hb_d3d_trace_append( file, api, "copy_resource",
            "{\"src\":\"texture0\",\"dst\":\"texture1\"}" );
    if (macrunner_hb_d3d_scenario_has( scenario, "indexed" ))
    {
        macrunner_hb_d3d_trace_append( file, api, "set_index_buffer", "{\"indices\":[0,1,2]}" );
        macrunner_hb_d3d_trace_append( file, api, "draw_indexed", "{\"index_count\":3}" );
    }
    else
        macrunner_hb_d3d_trace_append( file, api, "draw", "{\"vertex_count\":3}" );
    if (macrunner_hb_d3d_scenario_has( scenario, "resize_swapchain" ))
        macrunner_hb_d3d_trace_append( file, api, "resize_swapchain",
            "{\"width\":96,\"height\":96,\"buffer_count\":2}" );
    macrunner_hb_d3d_trace_append( file, api, "present", "{}" );
    if (macrunner_hb_d3d_scenario_has( scenario, "present_loop_3_frames" ))
    {
        macrunner_hb_d3d_trace_append( file, api, "present", "{}" );
        macrunner_hb_d3d_trace_append( file, api, "present", "{}" );
    }
    fclose( file );
}

static BOOL macrunner_hb_try_synthetic_d3d_semantic( hb_context_t *ctx,
                                                     const struct macrunner_hb_import_thunk *thunk,
                                                     const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                     uint64_t *ret )
{
    const char *api;

    (void)ctx;
    (void)args;
    if (!thunk || !ret) return FALSE;
    if (!(api = macrunner_hb_synthetic_d3d_api_for_proc( thunk->dll_name, thunk->import_name )))
        return FALSE;

    macrunner_hb_write_synthetic_d3d_trace( api );
    RtlSetLastWin32Error( ERROR_SUCCESS );
    NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
    *ret = 0;
    TRACE( "MacRunner HyperBridge synthetic D3D import %s!%s ret=%p\n",
           thunk->dll_name, thunk->import_name, (void *)(uintptr_t)*ret );
    return TRUE;
}

static BOOL macrunner_hb_try_library_loader_semantic( hb_context_t *ctx,
                                                      const struct macrunner_hb_import_thunk *thunk,
                                                      const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                      uint64_t *ret )
{
    const BOOL is_kernel =
        thunk && (macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
                  macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" ));
    uint64_t native_args[MACRUNNER_HB_IMPORT_ARG_MAX];
    WCHAR module_name_w[260];
    char module_name_a[260];
    char resolved_name[256];
    char proc_name_buffer[128];
    void *module_base = NULL;
    const char *synthetic_dll_name;
    BOOL is_wide = FALSE;
    USHORT lookup_machine;
    USHORT module_machine;

    if (!ctx || !ctx->memory || !thunk || !args || !ret || !is_kernel) return FALSE;
    memcpy( native_args, args, sizeof(native_args) );

    if (macrunner_hb_strieq( thunk->import_name, "LoadLibraryA" ) ||
        macrunner_hb_strieq( thunk->import_name, "LoadLibraryW" ) ||
        macrunner_hb_strieq( thunk->import_name, "LoadLibraryExA" ) ||
        macrunner_hb_strieq( thunk->import_name, "LoadLibraryExW" ))
    {
        const char *module_name_for_lookup;
        uint64_t synthetic_module;

        is_wide = macrunner_hb_strieq( thunk->import_name, "LoadLibraryW" ) ||
                  macrunner_hb_strieq( thunk->import_name, "LoadLibraryExW" );
        if (macrunner_hb_strieq( thunk->import_name, "LoadLibraryExA" ) ||
            macrunner_hb_strieq( thunk->import_name, "LoadLibraryExW" ))
        {
            if (args[1])
            {
                RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
                NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
                *ret = 0;
                return TRUE;
            }
        }
        if (is_wide)
        {
            unsigned int i;

            if (!macrunner_hb_read_guest_wstr_buf( ctx, args[0], module_name_w,
                                                   ARRAY_SIZE(module_name_w) ))
            {
                RtlSetLastWin32Error( ERROR_MOD_NOT_FOUND );
                NtCurrentTeb()->LastStatusValue = STATUS_DLL_NOT_FOUND;
                *ret = 0;
                return TRUE;
            }
            native_args[0] = (uint64_t)(uintptr_t)module_name_w;
            module_name_a[0] = 0;
            for (i = 0; i + 1 < ARRAY_SIZE(module_name_a) && module_name_w[i]; i++)
            {
                WCHAR ch = module_name_w[i];
                module_name_a[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
                module_name_a[i + 1] = 0;
            }
        }
        else
        {
            if (!macrunner_hb_read_guest_astr( ctx, args[0], module_name_a, sizeof(module_name_a) ))
            {
                RtlSetLastWin32Error( ERROR_MOD_NOT_FOUND );
                NtCurrentTeb()->LastStatusValue = STATUS_DLL_NOT_FOUND;
                *ret = 0;
                return TRUE;
            }
            native_args[0] = (uint64_t)(uintptr_t)module_name_a;
        }
        module_name_for_lookup = module_name_a;
        if ((synthetic_module = macrunner_hb_synthetic_d3d_module_handle( module_name_for_lookup )))
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
            *ret = synthetic_module;
            TRACE( "MacRunner HyperBridge synthetic LoadLibrary %s ret=%p\n",
                   module_name_for_lookup, (void *)(uintptr_t)*ret );
            return TRUE;
        }
        lookup_machine = macrunner_hb_import_lookup_machine( thunk );
        module_base = macrunner_hb_find_loaded_module_or_apiset_target( module_name_for_lookup, lookup_machine,
                                                                        resolved_name, sizeof(resolved_name) );
        if (!module_base && lookup_machine != current_machine)
            module_base = macrunner_hb_find_loaded_module_or_apiset_target( module_name_for_lookup, current_machine,
                                                                            resolved_name, sizeof(resolved_name) );
        if (!module_base && current_machine != IMAGE_FILE_MACHINE_ARM64 &&
            lookup_machine != IMAGE_FILE_MACHINE_ARM64)
            module_base = macrunner_hb_find_loaded_module_or_apiset_target( module_name_for_lookup,
                                                                            IMAGE_FILE_MACHINE_ARM64,
                                                                            resolved_name, sizeof(resolved_name) );
        if (!module_base)
        {
            if (macrunner_hb_strieq( module_name_for_lookup, "winmm.dll" ) ||
                macrunner_hb_strieq( module_name_for_lookup, "xinput1_4.dll" ) ||
                macrunner_hb_strieq( module_name_for_lookup, "xinput9_1_0.dll" ))
            {
                RtlSetLastWin32Error( ERROR_MOD_NOT_FOUND );
                NtCurrentTeb()->LastStatusValue = STATUS_DLL_NOT_FOUND;
                *ret = 0;
            }
            else
            {
                UNICODE_STRING load_name;
                HMODULE loaded = NULL;
                DWORD load_flags = (macrunner_hb_strieq( thunk->import_name, "LoadLibraryExA" ) ||
                                    macrunner_hb_strieq( thunk->import_name, "LoadLibraryExW" )) ?
                                   (DWORD)args[2] : 0;
                void *ntdll_module, *ldr_target = NULL;
                struct macrunner_hb_import_thunk ldr_thunk = *thunk;
                uint64_t ldr_args[MACRUNNER_HB_IMPORT_ARG_MAX] = {0};
                NTSTATUS status = STATUS_DLL_NOT_FOUND;

                if (!is_wide)
                {
                    unsigned int i;

                    for (i = 0; i + 1 < ARRAY_SIZE(module_name_w) && module_name_a[i]; i++)
                        module_name_w[i] = (unsigned char)module_name_a[i];
                    module_name_w[i] = 0;
                }
                RtlInitUnicodeString( &load_name, module_name_w );
                TRACE( "MacRunner HyperBridge semantic %s!%s ldr-load module=%s flags=0x%lx "
                       "lookup_machine=%04x current_machine=%04x\n",
                       thunk->dll_name, thunk->import_name, module_name_for_lookup,
                       (unsigned long)load_flags, lookup_machine, current_machine );
                ntdll_module = macrunner_hb_find_loaded_module_or_apiset_target( "ntdll.dll",
                                                                                 IMAGE_FILE_MACHINE_ARM64,
                                                                                 NULL, 0 );
                if (ntdll_module) ldr_target = macrunner_hb_find_named_export( ntdll_module, "LdrLoadDll" );
                if (ldr_target)
                {
                    ldr_thunk.target = ldr_target;
                    ldr_thunk.target_machine = IMAGE_FILE_MACHINE_ARM64;
                    lstrcpynA( ldr_thunk.dll_name, "ntdll.dll", ARRAY_SIZE(ldr_thunk.dll_name) );
                    lstrcpynA( ldr_thunk.import_name, "LdrLoadDll", ARRAY_SIZE(ldr_thunk.import_name) );
                    ldr_args[0] = 0;
                    ldr_args[1] = load_flags;
                    ldr_args[2] = (uint64_t)(uintptr_t)&load_name;
                    ldr_args[3] = (uint64_t)(uintptr_t)&loaded;
                    status = (NTSTATUS)macrunner_hb_call_arm64_pe_import12_for_ctx( ctx, &ldr_thunk, ldr_args );
                }
                NtCurrentTeb()->LastStatusValue = status;
                if (NT_ERROR( status ))
                {
                    RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
                    *ret = 0;
                }
                else
                {
                    RtlSetLastWin32Error( ERROR_SUCCESS );
                    *ret = (uint64_t)(uintptr_t)loaded;
                }
            }
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
            *ret = (uint64_t)(uintptr_t)module_base;
        }
        TRACE( "MacRunner HyperBridge semantic %s!%s loaded-only module=%s ret=%p\n",
               thunk->dll_name, thunk->import_name, resolved_name[0] ? resolved_name : module_name_for_lookup,
               (void *)(uintptr_t)*ret );
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "FreeLibrary" ))
    {
        if (!args[0])
        {
            RtlSetLastWin32Error( ERROR_INVALID_HANDLE );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_HANDLE;
            *ret = 0;
            return TRUE;
        }
        /*
         * LoadLibraryA/W above returns an existing native counterpart HMODULE
         * without incrementing Wine loader refcounts. Do not re-enter the loader
         * from the x64 import callback just to undo a refcount we did not take.
         */
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = TRUE;
        TRACE( "MacRunner HyperBridge semantic %s!FreeLibrary loaded-only module=%p ret=%p\n",
               thunk->dll_name, (void *)(uintptr_t)args[0], (void *)(uintptr_t)*ret );
        return TRUE;
    }

    if (!macrunner_hb_strieq( thunk->import_name, "GetProcAddress" )) return FALSE;
    if (!args[0])
    {
        RtlSetLastWin32Error( ERROR_INVALID_HANDLE );
        NtCurrentTeb()->LastStatusValue = STATUS_INVALID_HANDLE;
        *ret = 0;
        return TRUE;
    }
    if (!(args[1] >> 16))
    {
        snprintf( proc_name_buffer, sizeof(proc_name_buffer), "#%lu",
                  (unsigned long)LOWORD(args[1]) );
    }
    else
    {
        if (!macrunner_hb_read_guest_astr( ctx, args[1], proc_name_buffer, sizeof(proc_name_buffer) ))
        {
            RtlSetLastWin32Error( ERROR_PROC_NOT_FOUND );
            NtCurrentTeb()->LastStatusValue = STATUS_ENTRYPOINT_NOT_FOUND;
            *ret = 0;
            return TRUE;
        }
        native_args[1] = (uint64_t)(uintptr_t)proc_name_buffer;
    }
    if ((synthetic_dll_name = macrunner_hb_synthetic_d3d_module_name( args[0] )))
    {
        if (macrunner_hb_synthetic_d3d_api_for_proc( synthetic_dll_name, proc_name_buffer ))
        {
            *ret = macrunner_hb_register_synthetic_import_thunk( thunk, args[0],
                                                                 synthetic_dll_name, proc_name_buffer );
            if (!*ret)
            {
                RtlSetLastWin32Error( ERROR_NOT_ENOUGH_MEMORY );
                NtCurrentTeb()->LastStatusValue = STATUS_NO_MEMORY;
                return TRUE;
            }
            RtlSetLastWin32Error( ERROR_SUCCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_PROC_NOT_FOUND );
            NtCurrentTeb()->LastStatusValue = STATUS_ENTRYPOINT_NOT_FOUND;
            *ret = 0;
        }
        TRACE( "MacRunner HyperBridge synthetic GetProcAddress module=%s name=%s ret=%p\n",
               synthetic_dll_name, proc_name_buffer, (void *)(uintptr_t)*ret );
        return TRUE;
    }
    module_base = (void *)(uintptr_t)args[0];
    module_machine = macrunner_hb_module_machine( module_base );
    if (module_machine == IMAGE_FILE_MACHINE_AMD64 ||
        module_machine == IMAGE_FILE_MACHINE_I386 ||
        module_machine == IMAGE_FILE_MACHINE_ARM64X ||
        module_machine == IMAGE_FILE_MACHINE_ARM64EC)
    {
        void *proc;

        if (!(args[1] >> 16))
            proc = macrunner_hb_export_by_ordinal( module_base, LOWORD(args[1]) );
        else
            proc = macrunner_hb_find_named_export( module_base, proc_name_buffer );
        if (!proc)
        {
            RtlSetLastWin32Error( ERROR_PROC_NOT_FOUND );
            NtCurrentTeb()->LastStatusValue = STATUS_ENTRYPOINT_NOT_FOUND;
            *ret = 0;
        }
        else if (macrunner_hb_address_in_section( module_base, ".hexpthk", (uint64_t)(uintptr_t)proc ))
        {
            void *native_proc = macrunner_hb_redirect_arm64x_thunk_to_native( module_base, proc );
            uint64_t guest_target = 0;

            if (native_proc && native_proc != proc)
                guest_target = macrunner_hb_register_dynamic_import_thunk( thunk, native_proc,
                                                                           module_base, proc_name_buffer );
            if (!guest_target)
            {
                RtlSetLastWin32Error( ERROR_NOT_ENOUGH_MEMORY );
                NtCurrentTeb()->LastStatusValue = STATUS_NO_MEMORY;
                *ret = 0;
                TRACE( "MacRunner HyperBridge semantic guest GetProcAddress rejected .hexpthk "
                       "module=%p name=%s proc=%p native=%p\n",
                       module_base, proc_name_buffer, proc, native_proc );
            }
            else
            {
                RtlSetLastWin32Error( ERROR_SUCCESS );
                NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
                *ret = guest_target;
            }
        }
        else if (module_machine == IMAGE_FILE_MACHINE_AMD64 ||
                 module_machine == IMAGE_FILE_MACHINE_I386)
        {
            char export_module_name[96];
            uint64_t guest_target = 0;

            macrunner_hb_get_export_module_name( module_base, export_module_name,
                                                 sizeof(export_module_name) );
            if (macrunner_hb_kernel_export_has_local_semantic( export_module_name, proc_name_buffer ))
                guest_target = macrunner_hb_register_dynamic_import_thunk( thunk, proc,
                                                                           module_base,
                                                                           proc_name_buffer );
            if (guest_target)
            {
                RtlSetLastWin32Error( ERROR_SUCCESS );
                NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
                *ret = guest_target;
                TRACE( "MacRunner HyperBridge semantic guest GetProcAddress wrapped local "
                       "semantic %s!%s guest=%p raw=%p\n",
                       export_module_name, proc_name_buffer, (void *)(uintptr_t)guest_target, proc );
            }
            else
            {
                RtlSetLastWin32Error( ERROR_SUCCESS );
                NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
                *ret = (uint64_t)(uintptr_t)proc;
            }
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
            *ret = (uint64_t)(uintptr_t)proc;
        }
        TRACE( "MacRunner HyperBridge semantic guest GetProcAddress module=%p machine=%04x name=%s ret=%p\n",
               module_base, module_machine, proc_name_buffer, (void *)(uintptr_t)*ret );
        return TRUE;
    }
    if (!(args[1] >> 16))
        *ret = (uint64_t)(uintptr_t)macrunner_hb_export_by_ordinal( module_base, LOWORD(args[1]) );
    else
        *ret = (uint64_t)(uintptr_t)macrunner_hb_find_named_export( module_base, proc_name_buffer );
    if (!*ret)
    {
        RtlSetLastWin32Error( ERROR_PROC_NOT_FOUND );
        NtCurrentTeb()->LastStatusValue = STATUS_ENTRYPOINT_NOT_FOUND;
        TRACE( "MacRunner HyperBridge semantic %s!GetProcAddress module=%p name=%s ret=NULL\n",
               thunk->dll_name, (void *)(uintptr_t)args[0], proc_name_buffer );
        return TRUE;
    }
    if (macrunner_hb_pc_is_native_pe_builtin( *ret, &module_base ))
    {
        uint64_t guest_target = macrunner_hb_register_dynamic_import_thunk( thunk, (void *)(uintptr_t)*ret,
                                                                            module_base,
                                                                            proc_name_buffer );
        if (!guest_target)
        {
            RtlSetLastWin32Error( ERROR_NOT_ENOUGH_MEMORY );
            NtCurrentTeb()->LastStatusValue = STATUS_NO_MEMORY;
            *ret = 0;
            return TRUE;
        }
        *ret = guest_target;
    }

    TRACE( "MacRunner HyperBridge semantic %s!GetProcAddress module=%p name=%s ret=%p\n",
           thunk->dll_name, (void *)(uintptr_t)args[0], proc_name_buffer,
           (void *)(uintptr_t)*ret );
    return TRUE;
}

static BOOL macrunner_hb_systemtime_to_large_time( const SYSTEMTIME *st, LARGE_INTEGER *ft )
{
    struct tm tm_buf;
    struct tm check;
    time_t seconds;
    int64_t ticks;

    if (st->wYear < 1601 || st->wMonth < 1 || st->wMonth > 12 ||
        st->wDay < 1 || st->wDay > 31 || st->wHour > 23 ||
        st->wMinute > 59 || st->wSecond > 59 || st->wMilliseconds > 999)
        return FALSE;

    memset( &tm_buf, 0, sizeof(tm_buf) );
    tm_buf.tm_year = st->wYear - 1900;
    tm_buf.tm_mon = st->wMonth - 1;
    tm_buf.tm_mday = st->wDay;
    tm_buf.tm_hour = st->wHour;
    tm_buf.tm_min = st->wMinute;
    tm_buf.tm_sec = st->wSecond;
    tm_buf.tm_isdst = -1;
    seconds = timegm( &tm_buf );
    if (!gmtime_r( &seconds, &check ) ||
        check.tm_year != tm_buf.tm_year || check.tm_mon != tm_buf.tm_mon ||
        check.tm_mday != tm_buf.tm_mday || check.tm_hour != tm_buf.tm_hour ||
        check.tm_min != tm_buf.tm_min || check.tm_sec != tm_buf.tm_sec)
        return FALSE;

    ticks = ((int64_t)seconds + 11644473600LL) * 10000000LL +
            (int64_t)st->wMilliseconds * 10000LL;
    if (ticks < 0) return FALSE;
    ft->QuadPart = ticks;
    return TRUE;
}

static BOOL macrunner_hb_large_time_to_systemtime( LARGE_INTEGER ft, SYSTEMTIME *st )
{
    uint64_t ticks = (uint64_t)ft.QuadPart;
    time_t seconds;
    struct tm tm_buf;
    struct tm *tm;

    if (ft.QuadPart < 0) return FALSE;
    seconds = (time_t)(ticks / 10000000ULL - 11644473600ULL);
    tm = gmtime_r( &seconds, &tm_buf );
    if (!tm) return FALSE;

    st->wYear = tm->tm_year + 1900;
    st->wMonth = tm->tm_mon + 1;
    st->wDayOfWeek = tm->tm_wday;
    st->wDay = tm->tm_mday;
    st->wHour = tm->tm_hour;
    st->wMinute = tm->tm_min;
    st->wSecond = tm->tm_sec;
    st->wMilliseconds = (ticks / 10000ULL) % 1000ULL;
    return TRUE;
}

static int macrunner_hb_compare_tzdate( const SYSTEMTIME *st, const SYSTEMTIME *compare )
{
    static const int month_lengths[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int first, last, limit, dayinsecs;

    if (st->wMonth < compare->wMonth) return -1;
    if (st->wMonth > compare->wMonth) return 1;

    if (!compare->wYear)
    {
        first = (6 + compare->wDayOfWeek - st->wDayOfWeek + st->wDay) % 7 + 1;
        last = month_lengths[st->wMonth - 1] +
               (st->wMonth == 2 && (!(st->wYear % 4) && (st->wYear % 100 || !(st->wYear % 400))));
        limit = first + 7 * (compare->wDay - 1);
        if (limit > last) limit -= 7;
    }
    else limit = compare->wDay;

    limit = ((limit * 24 + compare->wHour) * 60 + compare->wMinute) * 60;
    dayinsecs = ((st->wDay * 24 + st->wHour) * 60 + st->wMinute) * 60 + st->wSecond;
    return dayinsecs - limit;
}

static DWORD macrunner_hb_get_timezone_id( const TIME_ZONE_INFORMATION *info, LARGE_INTEGER time,
                                           BOOL is_local )
{
    int year;
    BOOL before_standard_date, after_daylight_date;
    LARGE_INTEGER t2;
    SYSTEMTIME st;

    if (!info->DaylightDate.wMonth) return TIME_ZONE_ID_UNKNOWN;
    if (info->StandardDate.wMonth == 0 ||
        (info->StandardDate.wYear == 0 &&
         (info->StandardDate.wDay < 1 || info->StandardDate.wDay > 5 ||
          info->DaylightDate.wDay < 1 || info->DaylightDate.wDay > 5)))
        return TIME_ZONE_ID_INVALID;

    if (!is_local) time.QuadPart -= info->Bias * (LONGLONG)600000000;
    if (!macrunner_hb_large_time_to_systemtime( time, &st )) return TIME_ZONE_ID_INVALID;
    year = st.wYear;
    if (!is_local)
    {
        t2.QuadPart = time.QuadPart - info->DaylightBias * (LONGLONG)600000000;
        if (!macrunner_hb_large_time_to_systemtime( t2, &st )) return TIME_ZONE_ID_INVALID;
    }
    if (st.wYear == year) before_standard_date = macrunner_hb_compare_tzdate( &st, &info->StandardDate ) < 0;
    else before_standard_date = st.wYear < year;

    if (!is_local)
    {
        t2.QuadPart = time.QuadPart - info->StandardBias * (LONGLONG)600000000;
        if (!macrunner_hb_large_time_to_systemtime( t2, &st )) return TIME_ZONE_ID_INVALID;
    }
    if (st.wYear == year) after_daylight_date = macrunner_hb_compare_tzdate( &st, &info->DaylightDate ) >= 0;
    else after_daylight_date = st.wYear > year;

    if (info->DaylightDate.wMonth < info->StandardDate.wMonth)
    {
        if (before_standard_date && after_daylight_date) return TIME_ZONE_ID_DAYLIGHT;
    }
    else
    {
        if (before_standard_date || after_daylight_date) return TIME_ZONE_ID_DAYLIGHT;
    }
    return TIME_ZONE_ID_STANDARD;
}

static void macrunner_hb_get_current_timezone_info( TIME_ZONE_INFORMATION *info )
{
    time_t now = time( NULL );
    struct tm local_tm, utc_tm;
    time_t local_as_utc, utc_as_utc;

    memset( info, 0, sizeof(*info) );
    if (!localtime_r( &now, &local_tm ) || !gmtime_r( &now, &utc_tm ))
        return;

    local_tm.tm_isdst = -1;
    utc_tm.tm_isdst = 0;
    local_as_utc = timegm( &local_tm );
    utc_as_utc = timegm( &utc_tm );
    info->Bias = (LONG)((utc_as_utc - local_as_utc) / 60);
}

static BOOL macrunner_hb_try_environment_semantic( hb_context_t *ctx,
                                                   const struct macrunner_hb_import_thunk *thunk,
                                                   const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                   uint64_t *ret )
{
    char name[128];
    char value[1024];
    const char *env;
    DWORD size = (DWORD)args[2];
    DWORD len;
    BOOL wide;
    BOOL set_call;

    if (!ctx || !ctx->memory || !thunk || !args || !ret) return FALSE;
    if (macrunner_hb_strieq( thunk->dll_name, "ntdll.dll" ))
    {
        if (macrunner_hb_strieq( thunk->import_name, "RtlGetLastWin32Error" ))
        {
            *ret = NtCurrentTeb()->LastErrorValue;
            return TRUE;
        }

        if (macrunner_hb_strieq( thunk->import_name, "RtlSetLastWin32Error" ))
        {
            NtCurrentTeb()->LastErrorValue = (DWORD)args[0];
            *ret = 0;
            return TRUE;
        }

        return FALSE;
    }

    if (!macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) &&
        !macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" ))
        return FALSE;

    if (macrunner_hb_strieq( thunk->import_name, "GetEnvironmentStringsW" ))
    {
        RTL_USER_PROCESS_PARAMETERS *params = NtCurrentTeb()->Peb->ProcessParameters;
        static WCHAR empty_environmentW[2];

        *ret = (uint64_t)(uintptr_t)((params && params->Environment) ?
                                     params->Environment : empty_environmentW);
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetEnvironmentStringsA" ))
    {
        static char empty_environmentA[2];

        *ret = (uint64_t)(uintptr_t)empty_environmentA;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "FreeEnvironmentStringsW" ) ||
        macrunner_hb_strieq( thunk->import_name, "FreeEnvironmentStringsA" ))
    {
        *ret = TRUE;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetCommandLineW" ))
    {
        RTL_USER_PROCESS_PARAMETERS *params = NtCurrentTeb()->Peb->ProcessParameters;
        static WCHAR empty_command_lineW;

        *ret = (uint64_t)(uintptr_t)((params && params->CommandLine.Buffer) ?
                                     params->CommandLine.Buffer : &empty_command_lineW);
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetCommandLineA" ))
    {
        RTL_USER_PROCESS_PARAMETERS *params = NtCurrentTeb()->Peb->ProcessParameters;
        static char command_lineA[32768];
        unsigned int i, len = 0;

        command_lineA[0] = 0;
        if (params && params->CommandLine.Buffer)
        {
            len = params->CommandLine.Length / sizeof(WCHAR);
            if (len >= sizeof(command_lineA)) len = sizeof(command_lineA) - 1;
            for (i = 0; i < len; i++) command_lineA[i] = (char)(params->CommandLine.Buffer[i] & 0x7f);
            command_lineA[len] = 0;
        }
        *ret = (uint64_t)(uintptr_t)command_lineA;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetModuleFileNameW" ) ||
        macrunner_hb_strieq( thunk->import_name, "GetModuleFileNameA" ))
    {
        RTL_USER_PROCESS_PARAMETERS *params = NtCurrentTeb()->Peb->ProcessParameters;
        const UNICODE_STRING *module_name = NULL;
        LDR_DATA_TABLE_ENTRY *ldr = NULL;
        DWORD cap = (DWORD)args[2];
        DWORD i, len, to_copy;
        BOOL is_wide = macrunner_hb_strieq( thunk->import_name, "GetModuleFileNameW" );

        if (!args[1] || !cap)
        {
            RtlSetLastWin32Error( ERROR_INSUFFICIENT_BUFFER );
            NtCurrentTeb()->LastStatusValue = STATUS_BUFFER_TOO_SMALL;
            *ret = 0;
            return TRUE;
        }

        if (args[0])
        {
            ldr = macrunner_hb_ldr_entry_from_module( (void *)(uintptr_t)args[0] );
            if (!ldr)
            {
                RtlSetLastWin32Error( ERROR_MOD_NOT_FOUND );
                NtCurrentTeb()->LastStatusValue = STATUS_DLL_NOT_FOUND;
                *ret = 0;
                return TRUE;
            }
            module_name = &ldr->FullDllName;
        }
        else if (params) module_name = &params->ImagePathName;

        if (!module_name || !module_name->Buffer)
        {
            RtlSetLastWin32Error( ERROR_FILE_NOT_FOUND );
            NtCurrentTeb()->LastStatusValue = STATUS_DLL_NOT_FOUND;
            *ret = 0;
            return TRUE;
        }

        len = module_name->Length / sizeof(WCHAR);
        to_copy = len < cap - 1 ? len : cap - 1;
        if (is_wide)
        {
            for (i = 0; i < to_copy; i++)
            {
                if (hb_memory_write_u16( ctx->memory, (hb_gva_t)args[1] + i * sizeof(WCHAR),
                                         module_name->Buffer[i] ) != HB_OK)
                {
                    RtlSetLastWin32Error( ERROR_NOACCESS );
                    NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
                    *ret = 0;
                    return TRUE;
                }
            }
            hb_memory_write_u16( ctx->memory, (hb_gva_t)args[1] + to_copy * sizeof(WCHAR), 0 );
        }
        else
        {
            for (i = 0; i < to_copy; i++)
            {
                WCHAR wch = module_name->Buffer[i];
                uint8_t ch = (wch >= 0x20 && wch < 0x7f) ? (uint8_t)wch : '?';

                if (hb_memory_write_u8( ctx->memory, (hb_gva_t)args[1] + i, ch ) != HB_OK)
                {
                    RtlSetLastWin32Error( ERROR_NOACCESS );
                    NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
                    *ret = 0;
                    return TRUE;
                }
            }
            hb_memory_write_u8( ctx->memory, (hb_gva_t)args[1] + to_copy, 0 );
        }
        if (to_copy < len)
        {
            RtlSetLastWin32Error( ERROR_INSUFFICIENT_BUFFER );
            NtCurrentTeb()->LastStatusValue = STATUS_BUFFER_TOO_SMALL;
            *ret = cap;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
            *ret = len;
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "QueryPerformanceFrequency" ))
    {
        if (!args[0] || hb_memory_write_u64( ctx->memory, (hb_gva_t)args[0], 1000000000ULL ) != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = FALSE;
            return TRUE;
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = TRUE;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "QueryPerformanceCounter" ))
    {
        struct timeval tv;
        uint64_t ticks;

        if (!args[0])
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = FALSE;
            return TRUE;
        }
        gettimeofday( &tv, NULL );
        ticks = (uint64_t)tv.tv_sec * 1000000000ULL + (uint64_t)tv.tv_usec * 1000ULL;
        if (hb_memory_write_u64( ctx->memory, (hb_gva_t)args[0], ticks ) != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = FALSE;
            return TRUE;
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = TRUE;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetSystemTimePreciseAsFileTime" ) ||
        macrunner_hb_strieq( thunk->import_name, "GetSystemTimeAsFileTime" ))
    {
        struct timeval tv;
        uint64_t filetime;

        if (!args[0])
        {
            *ret = 0;
            return TRUE;
        }
        gettimeofday( &tv, NULL );
        filetime = ((uint64_t)tv.tv_sec + 11644473600ULL) * 10000000ULL +
                   (uint64_t)tv.tv_usec * 10ULL;
        hb_memory_write_u32( ctx->memory, (hb_gva_t)args[0], (DWORD)filetime );
        hb_memory_write_u32( ctx->memory, (hb_gva_t)args[0] + sizeof(DWORD), (DWORD)(filetime >> 32) );
        *ret = 0;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetSystemTime" ) ||
        macrunner_hb_strieq( thunk->import_name, "GetLocalTime" ))
    {
        struct timeval tv;
        time_t seconds;
        struct tm tm_buf;
        struct tm *tm;
        SYSTEMTIME st;
        BOOL local = macrunner_hb_strieq( thunk->import_name, "GetLocalTime" );

        if (!args[0])
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = 0;
            return TRUE;
        }
        gettimeofday( &tv, NULL );
        seconds = (time_t)tv.tv_sec;
        tm = local ? localtime_r( &seconds, &tm_buf ) : gmtime_r( &seconds, &tm_buf );
        if (!tm)
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = 0;
            return TRUE;
        }
        st.wYear = tm->tm_year + 1900;
        st.wMonth = tm->tm_mon + 1;
        st.wDayOfWeek = tm->tm_wday;
        st.wDay = tm->tm_mday;
        st.wHour = tm->tm_hour;
        st.wMinute = tm->tm_min;
        st.wSecond = tm->tm_sec;
        st.wMilliseconds = tv.tv_usec / 1000;
        if (hb_memory_write( ctx->memory, (hb_gva_t)args[0], &st, sizeof(st) ) != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = 0;
            return TRUE;
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = 0;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "SystemTimeToFileTime" ))
    {
        SYSTEMTIME st;
        LARGE_INTEGER filetime;

        if (!args[0] || !args[1] ||
            hb_memory_read( ctx->memory, (hb_gva_t)args[0], &st, sizeof(st) ) != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = FALSE;
            return TRUE;
        }
        if (!macrunner_hb_systemtime_to_large_time( &st, &filetime ))
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = FALSE;
            return TRUE;
        }
        if (hb_memory_write_u32( ctx->memory, (hb_gva_t)args[1], (DWORD)filetime.QuadPart ) != HB_OK ||
            hb_memory_write_u32( ctx->memory, (hb_gva_t)args[1] + sizeof(DWORD),
                                 (DWORD)((uint64_t)filetime.QuadPart >> 32) ) != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = FALSE;
            return TRUE;
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = TRUE;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "FileTimeToSystemTime" ))
    {
        DWORD low, high;
        LARGE_INTEGER filetime;
        SYSTEMTIME st;

        if (!args[0] || !args[1] ||
            hb_memory_read_u32( ctx->memory, (hb_gva_t)args[0], &low ) != HB_OK ||
            hb_memory_read_u32( ctx->memory, (hb_gva_t)args[0] + sizeof(DWORD), &high ) != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = FALSE;
            return TRUE;
        }
        filetime.QuadPart = ((uint64_t)high << 32) | low;
        if (!macrunner_hb_large_time_to_systemtime( filetime, &st ))
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = FALSE;
            return TRUE;
        }
        if (hb_memory_write( ctx->memory, (hb_gva_t)args[1], &st, sizeof(st) ) != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = FALSE;
            return TRUE;
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = TRUE;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "SystemTimeToTzSpecificLocalTime" ) ||
        macrunner_hb_strieq( thunk->import_name, "TzSpecificLocalTimeToSystemTime" ))
    {
        TIME_ZONE_INFORMATION tzinfo;
        SYSTEMTIME input, output;
        LARGE_INTEGER filetime;
        DWORD tzid;
        LONGLONG bias;
        BOOL system_to_local = macrunner_hb_strieq( thunk->import_name, "SystemTimeToTzSpecificLocalTime" );

        if (!args[1] || !args[2] ||
            hb_memory_read( ctx->memory, (hb_gva_t)args[1], &input, sizeof(input) ) != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = FALSE;
            return TRUE;
        }
        if (args[0])
        {
            if (hb_memory_read( ctx->memory, (hb_gva_t)args[0], &tzinfo, sizeof(tzinfo) ) != HB_OK)
            {
                RtlSetLastWin32Error( ERROR_NOACCESS );
                NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
                *ret = FALSE;
                return TRUE;
            }
        }
        else
        {
            macrunner_hb_get_current_timezone_info( &tzinfo );
        }
        if (!macrunner_hb_systemtime_to_large_time( &input, &filetime ))
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = FALSE;
            return TRUE;
        }

        tzid = macrunner_hb_get_timezone_id( &tzinfo, filetime, !system_to_local );
        switch (tzid)
        {
        case TIME_ZONE_ID_UNKNOWN:
            bias = tzinfo.Bias;
            break;
        case TIME_ZONE_ID_STANDARD:
            bias = tzinfo.Bias + tzinfo.StandardBias;
            break;
        case TIME_ZONE_ID_DAYLIGHT:
            bias = tzinfo.Bias + tzinfo.DaylightBias;
            break;
        default:
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = FALSE;
            return TRUE;
        }

        if (system_to_local) filetime.QuadPart -= bias * (LONGLONG)600000000;
        else filetime.QuadPart += bias * (LONGLONG)600000000;

        if (!macrunner_hb_large_time_to_systemtime( filetime, &output ))
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = FALSE;
            return TRUE;
        }
        if (hb_memory_write( ctx->memory, (hb_gva_t)args[2], &output, sizeof(output) ) != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = FALSE;
            return TRUE;
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = TRUE;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetTempPathW" ) ||
        macrunner_hb_strieq( thunk->import_name, "GetTempPath2W" ) ||
        macrunner_hb_strieq( thunk->import_name, "GetTempPathA" ) ||
        macrunner_hb_strieq( thunk->import_name, "GetTempPath2A" ))
    {
        static const char temp_path[] = "C:\\windows\\temp\\";
        DWORD cap = (DWORD)args[0];
        DWORD path_len = sizeof(temp_path) - 1;
        BOOL is_wide = macrunner_hb_strieq( thunk->import_name, "GetTempPathW" ) ||
                       macrunner_hb_strieq( thunk->import_name, "GetTempPath2W" );
        DWORD i;

        if (!args[1] || cap <= path_len)
        {
            *ret = path_len + 1;
            return TRUE;
        }
        if (is_wide)
        {
            for (i = 0; i <= path_len; i++)
                hb_memory_write_u16( ctx->memory, (hb_gva_t)args[1] + i * sizeof(WCHAR),
                                     (WCHAR)temp_path[i] );
        }
        else
        {
            for (i = 0; i <= path_len; i++)
                hb_memory_write_u8( ctx->memory, (hb_gva_t)args[1] + i, (uint8_t)temp_path[i] );
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = path_len;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetACP" ))
    {
        *ret = 1252;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetOEMCP" ))
    {
        *ret = 437;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "AreFileApisANSI" ))
    {
        *ret = TRUE;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "IsValidCodePage" ))
    {
        UINT codepage = (UINT)args[0];

        *ret = (codepage == 0 /* CP_ACP */ || codepage == 1 /* CP_OEMCP */ ||
                codepage == 1252 || codepage == 437 || codepage == 65001 /* CP_UTF8 */);
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetCPInfo" ))
    {
        UINT codepage = (UINT)args[0];
        CPINFO info;
        hb_result_t write;

        if (codepage == 0) codepage = 1252;
        else if (codepage == 1) codepage = 437;

        if (!args[1] || (codepage != 1252 && codepage != 437 && codepage != 65001))
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = FALSE;
            return TRUE;
        }

        memset( &info, 0, sizeof(info) );
        info.MaxCharSize = codepage == 65001 ? 4 : 1;
        info.DefaultChar[0] = '?';
        write = hb_memory_write( ctx->memory, (hb_gva_t)args[1], &info, sizeof(info) );
        if (write != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = FALSE;
            return TRUE;
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = TRUE;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "MultiByteToWideChar" ))
    {
        int cb = (int)(int32_t)args[3];
        int out_cap = (int)(int32_t)args[5];
        uint64_t src = args[2], dst = args[4];
        size_t count = 0, i;
        BOOL nul_terminated = cb == -1;

        if (!src || cb == 0 || cb < -1)
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = 0;
            return TRUE;
        }

        if (nul_terminated)
        {
            for (count = 0; count < 32768; count++)
            {
                uint8_t ch = 0;
                if (hb_memory_read_u8( ctx->memory, (hb_gva_t)src + count, &ch ) != HB_OK)
                {
                    RtlSetLastWin32Error( ERROR_NOACCESS );
                    NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
                    *ret = 0;
                    return TRUE;
                }
                if (!ch) { count++; break; }
            }
        }
        else count = (size_t)cb;

        if (!dst || !out_cap)
        {
            *ret = count;
            return TRUE;
        }
        if (out_cap < (int)count)
        {
            RtlSetLastWin32Error( ERROR_INSUFFICIENT_BUFFER );
            NtCurrentTeb()->LastStatusValue = STATUS_BUFFER_TOO_SMALL;
            *ret = 0;
            return TRUE;
        }
        for (i = 0; i < count; i++)
        {
            uint8_t ch = 0;
            WCHAR wch;

            if (hb_memory_read_u8( ctx->memory, (hb_gva_t)src + i, &ch ) != HB_OK)
            {
                RtlSetLastWin32Error( ERROR_NOACCESS );
                NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
                *ret = 0;
                return TRUE;
            }
            wch = (ch < 0x80) ? ch : '?';
            if (hb_memory_write_u16( ctx->memory, (hb_gva_t)dst + i * sizeof(WCHAR), wch ) != HB_OK)
            {
                RtlSetLastWin32Error( ERROR_NOACCESS );
                NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
                *ret = 0;
                return TRUE;
            }
            if (nul_terminated && !ch) break;
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = count;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "WideCharToMultiByte" ))
    {
        int cch = (int)(int32_t)args[3];
        int out_cap = (int)(int32_t)args[5];
        uint64_t src = args[2], dst = args[4];
        size_t count = 0, i;
        BOOL nul_terminated = cch == -1;
        BOOL used_default = FALSE;

        if (!src || cch == 0 || cch < -1)
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = 0;
            return TRUE;
        }

        if (nul_terminated)
        {
            for (count = 0; count < 32768; count++)
            {
                uint16_t ch = 0;
                if (hb_memory_read_u16( ctx->memory, (hb_gva_t)src + count * sizeof(WCHAR), &ch ) != HB_OK)
                {
                    RtlSetLastWin32Error( ERROR_NOACCESS );
                    NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
                    *ret = 0;
                    return TRUE;
                }
                if (!ch) { count++; break; }
            }
        }
        else count = (size_t)cch;

        if (!dst || !out_cap)
        {
            *ret = count;
            return TRUE;
        }
        if (out_cap < (int)count)
        {
            RtlSetLastWin32Error( ERROR_INSUFFICIENT_BUFFER );
            NtCurrentTeb()->LastStatusValue = STATUS_BUFFER_TOO_SMALL;
            *ret = 0;
            return TRUE;
        }
        for (i = 0; i < count; i++)
        {
            uint16_t wch = 0;
            uint8_t ch;

            if (hb_memory_read_u16( ctx->memory, (hb_gva_t)src + i * sizeof(WCHAR), &wch ) != HB_OK)
            {
                RtlSetLastWin32Error( ERROR_NOACCESS );
                NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
                *ret = 0;
                return TRUE;
            }
            if (wch < 0x80) ch = (uint8_t)wch;
            else
            {
                ch = '?';
                used_default = TRUE;
            }
            if (hb_memory_write_u8( ctx->memory, (hb_gva_t)dst + i, ch ) != HB_OK)
            {
                RtlSetLastWin32Error( ERROR_NOACCESS );
                NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
                *ret = 0;
                return TRUE;
            }
            if (nul_terminated && !wch) break;
        }
        if (args[7]) hb_memory_write_u32( ctx->memory, (hb_gva_t)args[7], used_default );
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = count;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetStringTypeW" ))
    {
        DWORD type = (DWORD)args[0];
        uint64_t src = args[1], dst = args[3];
        int cch = (int)(int32_t)args[2];
        size_t count = 0, i;

        if (!src || !dst || cch == 0 || cch < -1)
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = FALSE;
            return TRUE;
        }

        if (cch == -1)
        {
            for (count = 0; count < 32768; count++)
            {
                uint16_t wch = 0;
                if (hb_memory_read_u16( ctx->memory, (hb_gva_t)src + count * sizeof(WCHAR), &wch ) != HB_OK)
                {
                    RtlSetLastWin32Error( ERROR_NOACCESS );
                    NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
                    *ret = FALSE;
                    return TRUE;
                }
                if (!wch) { count++; break; }
            }
        }
        else count = (size_t)cch;

        for (i = 0; i < count; i++)
        {
            uint16_t wch = 0;
            WORD flags = 0;

            if (hb_memory_read_u16( ctx->memory, (hb_gva_t)src + i * sizeof(WCHAR), &wch ) != HB_OK)
            {
                RtlSetLastWin32Error( ERROR_NOACCESS );
                NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
                *ret = FALSE;
                return TRUE;
            }
            if (type == CT_CTYPE1)
            {
                if (wch >= 'A' && wch <= 'Z') flags |= C1_UPPER | C1_ALPHA;
                else if (wch >= 'a' && wch <= 'z') flags |= C1_LOWER | C1_ALPHA;
                if (wch >= '0' && wch <= '9') flags |= C1_DIGIT;
                if ((wch >= '0' && wch <= '9') || (wch >= 'A' && wch <= 'F') || (wch >= 'a' && wch <= 'f'))
                    flags |= C1_XDIGIT;
                if (wch == ' ' || wch == '\t') flags |= C1_BLANK;
                if (wch == ' ' || wch == '\t' || wch == '\r' || wch == '\n' || wch == '\f' || wch == '\v')
                    flags |= C1_SPACE;
                if (wch < 0x20 || wch == 0x7f) flags |= C1_CNTRL;
                if (wch < 0x80 && !flags && wch) flags |= C1_PUNCT;
            }
            if (hb_memory_write_u16( ctx->memory, (hb_gva_t)dst + i * sizeof(WORD), flags ) != HB_OK)
            {
                RtlSetLastWin32Error( ERROR_NOACCESS );
                NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
                *ret = FALSE;
                return TRUE;
            }
            if (cch == -1 && !wch) break;
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = TRUE;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "LCMapStringEx" ))
    {
        DWORD flags = (DWORD)args[1];
        uint64_t src = args[2], dst = args[4];
        int cch_src = (int)(int32_t)args[3];
        int cch_dst = (int)(int32_t)args[5];
        BOOL sort_key = !!(flags & LCMAP_SORTKEY);
        size_t count = 0, i;

        if (!src || cch_src == 0 || cch_src < -1 || cch_dst < 0)
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = 0;
            return TRUE;
        }

        if (cch_src == -1)
        {
            for (count = 0; count < 32768; count++)
            {
                uint16_t wch = 0;
                if (hb_memory_read_u16( ctx->memory, (hb_gva_t)src + count * sizeof(WCHAR), &wch ) != HB_OK)
                {
                    RtlSetLastWin32Error( ERROR_NOACCESS );
                    NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
                    *ret = 0;
                    return TRUE;
                }
                if (!wch) { count++; break; }
            }
        }
        else count = (size_t)cch_src;

        if (!dst || !cch_dst)
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
            *ret = count;
            return TRUE;
        }
        if ((size_t)cch_dst < count)
        {
            RtlSetLastWin32Error( ERROR_INSUFFICIENT_BUFFER );
            NtCurrentTeb()->LastStatusValue = STATUS_BUFFER_TOO_SMALL;
            *ret = 0;
            return TRUE;
        }

        for (i = 0; i < count; i++)
        {
            uint16_t wch = 0;
            WCHAR mapped;

            if (hb_memory_read_u16( ctx->memory, (hb_gva_t)src + i * sizeof(WCHAR), &wch ) != HB_OK)
            {
                RtlSetLastWin32Error( ERROR_NOACCESS );
                NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
                *ret = 0;
                return TRUE;
            }
            mapped = wch;
            if (flags & LCMAP_LOWERCASE) mapped = macrunner_hb_tolower_wchar( mapped );
            if (flags & LCMAP_UPPERCASE) mapped = macrunner_hb_toupper_wchar( mapped );

            if (sort_key)
            {
                uint8_t ch = mapped < 0x80 ? (uint8_t)mapped : '?';
                if (hb_memory_write_u8( ctx->memory, (hb_gva_t)dst + i, ch ) != HB_OK)
                {
                    RtlSetLastWin32Error( ERROR_NOACCESS );
                    NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
                    *ret = 0;
                    return TRUE;
                }
            }
            else if (hb_memory_write_u16( ctx->memory, (hb_gva_t)dst + i * sizeof(WCHAR), mapped ) != HB_OK)
            {
                RtlSetLastWin32Error( ERROR_NOACCESS );
                NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
                *ret = 0;
                return TRUE;
            }
            if (cch_src == -1 && !wch) break;
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = count;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetStartupInfoW" ))
    {
        RTL_USER_PROCESS_PARAMETERS *params;
        STARTUPINFOW info;
        hb_result_t write;

        memset( &info, 0, sizeof(info) );
        params = NtCurrentTeb()->Peb->ProcessParameters;
        if (params)
        {
            info.cb              = sizeof(info);
            info.lpDesktop       = params->Desktop.Buffer;
            info.lpTitle         = params->WindowTitle.Buffer;
            info.dwX             = params->dwX;
            info.dwY             = params->dwY;
            info.dwXSize         = params->dwXSize;
            info.dwYSize         = params->dwYSize;
            info.dwXCountChars   = params->dwXCountChars;
            info.dwYCountChars   = params->dwYCountChars;
            info.dwFillAttribute = params->dwFillAttribute;
            info.dwFlags         = params->dwFlags;
            info.wShowWindow     = params->wShowWindow;
            info.cbReserved2     = params->RuntimeInfo.MaximumLength;
            info.lpReserved2     = params->RuntimeInfo.MaximumLength ? (void *)params->RuntimeInfo.Buffer : NULL;
            if (params->dwFlags & STARTF_USESTDHANDLES)
            {
                info.hStdInput   = params->hStdInput;
                info.hStdOutput  = params->hStdOutput;
                info.hStdError   = params->hStdError;
            }
        }

        write = hb_memory_write( ctx->memory, (hb_gva_t)args[0], &info, sizeof(info) );
        if (write != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
        }
        *ret = 0;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetStartupInfoA" ))
    {
        RTL_USER_PROCESS_PARAMETERS *params;
        STARTUPINFOA info;
        hb_result_t write;

        memset( &info, 0, sizeof(info) );
        params = NtCurrentTeb()->Peb->ProcessParameters;
        if (params)
        {
            info.cb              = sizeof(info);
            info.dwX             = params->dwX;
            info.dwY             = params->dwY;
            info.dwXSize         = params->dwXSize;
            info.dwYSize         = params->dwYSize;
            info.dwXCountChars   = params->dwXCountChars;
            info.dwYCountChars   = params->dwYCountChars;
            info.dwFillAttribute = params->dwFillAttribute;
            info.dwFlags         = params->dwFlags;
            info.wShowWindow     = params->wShowWindow;
            info.cbReserved2     = params->RuntimeInfo.MaximumLength;
            info.lpReserved2     = params->RuntimeInfo.MaximumLength ? (void *)params->RuntimeInfo.Buffer : NULL;
            if (params->dwFlags & STARTF_USESTDHANDLES)
            {
                info.hStdInput   = params->hStdInput;
                info.hStdOutput  = params->hStdOutput;
                info.hStdError   = params->hStdError;
            }
            else
            {
                info.hStdInput   = INVALID_HANDLE_VALUE;
                info.hStdOutput  = INVALID_HANDLE_VALUE;
                info.hStdError   = INVALID_HANDLE_VALUE;
            }
        }

        write = hb_memory_write( ctx->memory, (hb_gva_t)args[0], &info, sizeof(info) );
        if (write != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
        }
        *ret = 0;
        return TRUE;
    }

    if (!macrunner_hb_strieq( thunk->import_name, "GetEnvironmentVariableA" ) &&
        !macrunner_hb_strieq( thunk->import_name, "GetEnvironmentVariableW" ) &&
        !macrunner_hb_strieq( thunk->import_name, "SetEnvironmentVariableA" ) &&
        !macrunner_hb_strieq( thunk->import_name, "SetEnvironmentVariableW" ))
        return FALSE;

    set_call = macrunner_hb_strieq( thunk->import_name, "SetEnvironmentVariableA" ) ||
               macrunner_hb_strieq( thunk->import_name, "SetEnvironmentVariableW" );
    wide = macrunner_hb_strieq( thunk->import_name, "GetEnvironmentVariableW" ) ||
           macrunner_hb_strieq( thunk->import_name, "SetEnvironmentVariableW" );
    if (!(wide ? macrunner_hb_read_guest_wstr( ctx, args[0], name, sizeof(name) ) :
                 macrunner_hb_read_guest_astr( ctx, args[0], name, sizeof(name) )))
    {
        RtlSetLastWin32Error( ERROR_ENVVAR_NOT_FOUND );
        NtCurrentTeb()->LastStatusValue = STATUS_VARIABLE_NOT_FOUND;
        *ret = 0;
        return TRUE;
    }

    if (set_call)
    {
        if (!args[1])
        {
            unsetenv( name );
        }
        else
        {
            if (!(wide ? macrunner_hb_read_guest_wstr( ctx, args[1], value, sizeof(value) ) :
                         macrunner_hb_read_guest_astr( ctx, args[1], value, sizeof(value) )))
            {
                RtlSetLastWin32Error( ERROR_NOACCESS );
                NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
                *ret = FALSE;
                return TRUE;
            }
            setenv( name, value, 1 );
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = TRUE;
        return TRUE;
    }

    env = getenv( name );
    if (!env)
    {
        RtlSetLastWin32Error( ERROR_ENVVAR_NOT_FOUND );
        NtCurrentTeb()->LastStatusValue = STATUS_VARIABLE_NOT_FOUND;
        *ret = 0;
        return TRUE;
    }

    macrunner_hb_copy_cstr( value, sizeof(value), env );
    len = strlen(value);
    if (!args[1] || size <= len)
    {
        *ret = len + 1;
        return TRUE;
    }

    if (wide)
    {
        WCHAR wvalue[1024];
        DWORD i;

        for (i = 0; i <= len && i < ARRAY_SIZE(wvalue); i++) wvalue[i] = (unsigned char)value[i];
        if (hb_memory_write( ctx->memory, (hb_gva_t)args[1], wvalue, (len + 1) * sizeof(WCHAR) ) != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = 0;
            return TRUE;
        }
    }
    else if (hb_memory_write( ctx->memory, (hb_gva_t)args[1], value, len + 1 ) != HB_OK)
    {
        RtlSetLastWin32Error( ERROR_NOACCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
        *ret = 0;
        return TRUE;
    }

    RtlSetLastWin32Error( ERROR_SUCCESS );
    NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
    *ret = len;
    return TRUE;
}

static size_t macrunner_hb_wcslen_local( const WCHAR *str )
{
    const WCHAR *p = str;

    if (!p) return 0;
    while (*p) p++;
    return p - str;
}

static BOOL macrunner_hb_write_int_ptr( const uint64_t ptr, int value )
{
    SIZE_T written = 0;

    return NtWriteVirtualMemory( GetCurrentProcess(), (void *)(uintptr_t)ptr,
                                 &value, sizeof(value), &written ) == STATUS_SUCCESS &&
           written == sizeof(value);
}

static WCHAR **macrunner_hb_command_line_to_argvw( const WCHAR *cmdline, int *argc_out )
{
    int qcount, bcount;
    const WCHAR *s;
    WCHAR **argv;
    DWORD argc;
    WCHAR *d;

    if (!cmdline || !argc_out)
    {
        NtCurrentTeb()->LastErrorValue = ERROR_INVALID_PARAMETER;
        return NULL;
    }

    if (!*cmdline)
    {
        RTL_USER_PROCESS_PARAMETERS *params = NtCurrentTeb()->Peb->ProcessParameters;
        static const WCHAR empty_imageW;
        const WCHAR *image = (params && params->ImagePathName.Buffer) ?
                             params->ImagePathName.Buffer : &empty_imageW;
        size_t len = (params && params->ImagePathName.Buffer) ?
                     params->ImagePathName.Length / sizeof(WCHAR) : 0;
        size_t size = sizeof(WCHAR *) * 2 + (len + 1) * sizeof(WCHAR);
        WCHAR *dst;
        size_t i;

        if (!(argv = macrunner_hb_local_heap_alloc( size, FALSE ))) return NULL;
        dst = (WCHAR *)(argv + 2);
        argv[0] = dst;
        argv[1] = NULL;
        for (i = 0; i < len; i++) dst[i] = image[i];
        dst[len] = 0;
        *argc_out = 1;
        return argv;
    }

    argc = 1;
    s = cmdline;
    if (*s == '"')
    {
        s++;
        while (*s)
            if (*s++ == '"')
                break;
    }
    else
    {
        while (*s && *s != ' ' && *s != '\t')
            s++;
    }
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s)
        argc++;

    qcount = bcount = 0;
    while (*s)
    {
        if ((*s == ' ' || *s == '\t') && qcount == 0)
        {
            while (*s == ' ' || *s == '\t')
                s++;
            if (*s)
                argc++;
            bcount = 0;
        }
        else if (*s == '\\')
        {
            bcount++;
            s++;
        }
        else if (*s == '"')
        {
            if ((bcount & 1) == 0)
                qcount++;
            s++;
            bcount = 0;
            while (*s == '"')
            {
                qcount++;
                s++;
            }
            qcount = qcount % 3;
            if (qcount == 2)
                qcount = 0;
        }
        else
        {
            bcount = 0;
            s++;
        }
    }

    argv = macrunner_hb_local_heap_alloc( (argc + 1) * sizeof(WCHAR *) +
                   (macrunner_hb_wcslen_local( cmdline ) + 1) * sizeof(WCHAR), FALSE );
    if (!argv) return NULL;

    argv[0] = d = (WCHAR *)(argv + argc + 1);
    s = cmdline;
    while ((*d++ = *s++)) {}
    d = argv[0];
    argc = 1;
    if (*d == '"')
    {
        s = d + 1;
        while (*s)
        {
            if (*s == '"')
            {
                s++;
                break;
            }
            *d++ = *s++;
        }
    }
    else
    {
        while (*d && *d != ' ' && *d != '\t')
            d++;
        s = d;
        if (*s)
            s++;
    }
    *d++ = 0;
    while (*s == ' ' || *s == '\t')
        s++;
    if (!*s)
    {
        argv[argc] = NULL;
        *argc_out = argc;
        return argv;
    }

    argv[argc++] = d;
    qcount = bcount = 0;
    while (*s)
    {
        if ((*s == ' ' || *s == '\t') && qcount == 0)
        {
            *d++ = 0;
            bcount = 0;
            do {
                s++;
            } while (*s == ' ' || *s == '\t');
            if (*s)
                argv[argc++] = d;
        }
        else if (*s == '\\')
        {
            *d++ = *s++;
            bcount++;
        }
        else if (*s == '"')
        {
            if ((bcount & 1) == 0)
            {
                d -= bcount / 2;
                qcount++;
            }
            else
            {
                d = d - bcount / 2 - 1;
                *d++ = '"';
            }
            s++;
            bcount = 0;
            while (*s == '"')
            {
                if (++qcount == 3)
                {
                    *d++ = '"';
                    qcount = 0;
                }
                s++;
            }
            if (qcount == 2)
                qcount = 0;
        }
        else
        {
            *d++ = *s++;
            bcount = 0;
        }
    }
    *d = 0;
    argv[argc] = NULL;
    *argc_out = argc;
    return argv;
}

static BOOL macrunner_hb_try_command_line_semantic( hb_context_t *ctx,
                                                    const struct macrunner_hb_import_thunk *thunk,
                                                    const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                    uint64_t *ret )
{
    WCHAR **argv;
    int argc = 0;

    if (!ctx || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_strieq( thunk->import_name, "CommandLineToArgvW" )) return FALSE;
    if (!macrunner_hb_strieq( thunk->dll_name, "shell32.dll" ) &&
        !macrunner_hb_strieq( thunk->dll_name, "shcore.dll" ))
        return FALSE;

    argv = macrunner_hb_command_line_to_argvw( (const WCHAR *)(uintptr_t)args[0], &argc );
    if (argv && args[1] && !macrunner_hb_write_int_ptr( args[1], argc ))
    {
        macrunner_hb_local_heap_release( argv );
        argv = NULL;
        NtCurrentTeb()->LastErrorValue = ERROR_INVALID_PARAMETER;
    }

    *ret = (uint64_t)(uintptr_t)argv;
    TRACE( "MacRunner HyperBridge semantic %s!CommandLineToArgvW cmd=%p argc=%d ret=%p\n",
           thunk->dll_name, (void *)(uintptr_t)args[0], argc, argv );
    return TRUE;
}

static BOOL macrunner_hb_psapi_name_is( const char *name, const char *base )
{
    char k32_name[96];

    if (macrunner_hb_strieq( name, base )) return TRUE;
    snprintf( k32_name, sizeof(k32_name), "K32%s", base );
    return macrunner_hb_strieq( name, k32_name );
}

static BOOL macrunner_hb_is_current_process_handle( uint64_t handle )
{
    return (HANDLE)(uintptr_t)handle == GetCurrentProcess();
}

static LDR_DATA_TABLE_ENTRY *macrunner_hb_first_ldr_entry(void)
{
    PEB *peb = NtCurrentTeb()->Peb;
    LIST_ENTRY *head;

    if (!peb || !peb->LdrData) return NULL;
    head = &peb->LdrData->InLoadOrderModuleList;
    if (!head->Flink || head->Flink == head) return NULL;
    return CONTAINING_RECORD( head->Flink, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks );
}

static const UNICODE_STRING *macrunner_hb_process_image_path(void)
{
    RTL_USER_PROCESS_PARAMETERS *params = NtCurrentTeb()->Peb->ProcessParameters;

    return params ? &params->ImagePathName : NULL;
}

static BOOL macrunner_hb_write_guest_win32_string( hb_context_t *ctx, uint64_t dst, DWORD cap,
                                                   const WCHAR *src, DWORD len, BOOL wide,
                                                   DWORD *written )
{
    DWORD i, to_copy;

    if (!ctx || !dst || !cap || !src) return FALSE;
    to_copy = len < cap - 1 ? len : cap - 1;
    if (wide)
    {
        for (i = 0; i < to_copy; i++)
            if (hb_memory_write_u16( ctx->memory, (hb_gva_t)dst + i * sizeof(WCHAR), src[i] ) != HB_OK)
                return FALSE;
        if (hb_memory_write_u16( ctx->memory, (hb_gva_t)dst + to_copy * sizeof(WCHAR), 0 ) != HB_OK)
            return FALSE;
    }
    else
    {
        for (i = 0; i < to_copy; i++)
        {
            WCHAR wch = src[i];
            uint8_t ch = (wch >= 0x20 && wch < 0x7f) ? (uint8_t)wch : '?';

            if (hb_memory_write_u8( ctx->memory, (hb_gva_t)dst + i, ch ) != HB_OK)
                return FALSE;
        }
        if (hb_memory_write_u8( ctx->memory, (hb_gva_t)dst + to_copy, 0 ) != HB_OK)
            return FALSE;
    }
    if (written) *written = (len < cap) ? len : cap;
    return TRUE;
}

static BOOL macrunner_hb_try_psapi_module_semantic( hb_context_t *ctx,
                                                    const struct macrunner_hb_import_thunk *thunk,
                                                    const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                    uint64_t *ret )
{
    const char *name;

    if (!ctx || !thunk || !args || !ret) return FALSE;
    name = thunk->import_name;

    if (macrunner_hb_psapi_name_is( name, "EnumProcessModules" ) ||
        macrunner_hb_psapi_name_is( name, "EnumProcessModulesEx" ))
    {
        DWORD filter = macrunner_hb_psapi_name_is( name, "EnumProcessModulesEx" ) ?
                       (DWORD)args[4] : MACRUNNER_HB_LIST_MODULES_DEFAULT;
        DWORD count = (DWORD)args[2], needed = 0, written = 0;
        PEB *peb = NtCurrentTeb()->Peb;
        LIST_ENTRY *head, *entry;

        if (!macrunner_hb_is_current_process_handle( args[0] ))
        {
            RtlSetLastWin32Error( ERROR_INVALID_HANDLE );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_HANDLE;
            *ret = FALSE;
            return TRUE;
        }
        if (filter & ~MACRUNNER_HB_LIST_MODULES_ALL)
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = FALSE;
            return TRUE;
        }
        if (count && !args[1])
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = FALSE;
            return TRUE;
        }
        if (!args[3])
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = FALSE;
            return TRUE;
        }

        if ((filter & MACRUNNER_HB_LIST_MODULES_ALL) != MACRUNNER_HB_LIST_MODULES_32BIT &&
            peb && peb->LdrData)
        {
            head = &peb->LdrData->InLoadOrderModuleList;
            for (entry = head->Flink; entry && entry != head; entry = entry->Flink)
            {
                LDR_DATA_TABLE_ENTRY *ldr = CONTAINING_RECORD( entry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks );
                uint64_t base = (uint64_t)(uintptr_t)ldr->DllBase;

                needed += sizeof(uint64_t);
                if (args[1] && written + sizeof(uint64_t) <= count)
                {
                    if (hb_memory_write_u64( ctx->memory, (hb_gva_t)args[1] + written, base ) != HB_OK)
                    {
                        RtlSetLastWin32Error( ERROR_NOACCESS );
                        NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
                        *ret = FALSE;
                        return TRUE;
                    }
                    written += sizeof(uint64_t);
                }
            }
        }

        if (hb_memory_write_u32( ctx->memory, (hb_gva_t)args[3], needed ) != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = FALSE;
            return TRUE;
        }
        TRACE( "MacRunner HyperBridge semantic %s!%s modules_bytes=%u written=%u\n",
               thunk->dll_name, thunk->import_name, needed, written );
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = TRUE;
        return TRUE;
    }

    if (macrunner_hb_psapi_name_is( name, "GetModuleInformation" ))
    {
        LDR_DATA_TABLE_ENTRY *ldr;
        struct macrunner_hb_moduleinfo64 info;

        if (!macrunner_hb_is_current_process_handle( args[0] ))
        {
            RtlSetLastWin32Error( ERROR_INVALID_HANDLE );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_HANDLE;
            *ret = FALSE;
            return TRUE;
        }
        if (!args[2] || (DWORD)args[3] < sizeof(info))
        {
            RtlSetLastWin32Error( ERROR_INSUFFICIENT_BUFFER );
            NtCurrentTeb()->LastStatusValue = STATUS_BUFFER_TOO_SMALL;
            *ret = FALSE;
            return TRUE;
        }

        ldr = args[1] ? macrunner_hb_ldr_entry_from_module( (void *)(uintptr_t)args[1] )
                      : macrunner_hb_first_ldr_entry();
        if (!ldr)
        {
            RtlSetLastWin32Error( ERROR_MOD_NOT_FOUND );
            NtCurrentTeb()->LastStatusValue = STATUS_DLL_NOT_FOUND;
            *ret = FALSE;
            return TRUE;
        }
        memset( &info, 0, sizeof(info) );
        info.lpBaseOfDll = (uint64_t)(uintptr_t)ldr->DllBase;
        info.SizeOfImage = ldr->SizeOfImage;
        info.EntryPoint = (uint64_t)(uintptr_t)ldr->EntryPoint;
        if (hb_memory_write( ctx->memory, (hb_gva_t)args[2], &info, sizeof(info) ) != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = FALSE;
            return TRUE;
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = TRUE;
        return TRUE;
    }

    if (macrunner_hb_psapi_name_is( name, "GetModuleFileNameExA" ) ||
        macrunner_hb_psapi_name_is( name, "GetModuleFileNameExW" ) ||
        macrunner_hb_psapi_name_is( name, "GetModuleBaseNameA" ) ||
        macrunner_hb_psapi_name_is( name, "GetModuleBaseNameW" ) ||
        macrunner_hb_psapi_name_is( name, "GetProcessImageFileNameA" ) ||
        macrunner_hb_psapi_name_is( name, "GetProcessImageFileNameW" ))
    {
        BOOL wide = macrunner_hb_psapi_name_is( name, "GetModuleFileNameExW" ) ||
                    macrunner_hb_psapi_name_is( name, "GetModuleBaseNameW" ) ||
                    macrunner_hb_psapi_name_is( name, "GetProcessImageFileNameW" );
        BOOL base_only = macrunner_hb_psapi_name_is( name, "GetModuleBaseNameA" ) ||
                         macrunner_hb_psapi_name_is( name, "GetModuleBaseNameW" );
        BOOL process_image = macrunner_hb_psapi_name_is( name, "GetProcessImageFileNameA" ) ||
                             macrunner_hb_psapi_name_is( name, "GetProcessImageFileNameW" );
        const UNICODE_STRING *src = NULL;
        LDR_DATA_TABLE_ENTRY *ldr;
        const WCHAR *str;
        DWORD len, written = 0;
        uint64_t dst = process_image ? args[1] : args[2];
        DWORD cap = (DWORD)(process_image ? args[2] : args[3]);

        if (!macrunner_hb_is_current_process_handle( args[0] ))
        {
            RtlSetLastWin32Error( ERROR_INVALID_HANDLE );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_HANDLE;
            *ret = 0;
            return TRUE;
        }

        if (process_image)
            src = macrunner_hb_process_image_path();
        else
        {
            ldr = args[1] ? macrunner_hb_ldr_entry_from_module( (void *)(uintptr_t)args[1] )
                          : macrunner_hb_first_ldr_entry();
            if (!ldr)
            {
                RtlSetLastWin32Error( ERROR_MOD_NOT_FOUND );
                NtCurrentTeb()->LastStatusValue = STATUS_DLL_NOT_FOUND;
                *ret = 0;
                return TRUE;
            }
            src = base_only ? &ldr->BaseDllName : &ldr->FullDllName;
        }
        str = src ? src->Buffer : NULL;
        len = (src && src->Buffer) ? src->Length / sizeof(WCHAR) : 0;
        if (!str || !dst || !cap)
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = 0;
            return TRUE;
        }
        if (!macrunner_hb_write_guest_win32_string( ctx, dst, cap, str, len, wide, &written ))
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = 0;
            return TRUE;
        }
        RtlSetLastWin32Error( written < len ? ERROR_INSUFFICIENT_BUFFER : ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = written < len ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
        *ret = written;
        return TRUE;
    }

    return FALSE;
}

static const char *macrunner_hb_win32_error_text( DWORD id )
{
    switch (id)
    {
    case ERROR_SUCCESS: return "The operation completed successfully.\r\n";
    case ERROR_FILE_NOT_FOUND: return "The system cannot find the file specified.\r\n";
    case ERROR_PATH_NOT_FOUND: return "The system cannot find the path specified.\r\n";
    case ERROR_ACCESS_DENIED: return "Access is denied.\r\n";
    case ERROR_INVALID_HANDLE: return "The handle is invalid.\r\n";
    case ERROR_NOT_ENOUGH_MEMORY: return "Not enough memory resources are available to process this command.\r\n";
    case ERROR_INVALID_DATA: return "The data is invalid.\r\n";
    case ERROR_INVALID_PARAMETER: return "The parameter is incorrect.\r\n";
    case ERROR_INSUFFICIENT_BUFFER: return "The data area passed to a system call is too small.\r\n";
    case ERROR_MOD_NOT_FOUND: return "The specified module could not be found.\r\n";
    case ERROR_PROC_NOT_FOUND: return "The specified procedure could not be found.\r\n";
    case ERROR_ENVVAR_NOT_FOUND: return "The system could not find the environment option that was entered.\r\n";
    default: return NULL;
    }
}

static BOOL macrunner_hb_try_format_message_semantic( hb_context_t *ctx,
                                                      const struct macrunner_hb_import_thunk *thunk,
                                                      const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                      uint64_t *ret )
{
    char textA[160];
    WCHAR textW[160];
    const char *known;
    DWORD flags, message_id, size, len, i;
    BOOL wide, allocate;
    uint64_t dst;

    if (!ctx || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_strieq( thunk->import_name, "FormatMessageA" ) &&
        !macrunner_hb_strieq( thunk->import_name, "FormatMessageW" ))
        return FALSE;
    if (!macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) &&
        !macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" ))
        return FALSE;

    flags = (DWORD)args[0];
    message_id = (DWORD)args[2];
    dst = args[4];
    size = (DWORD)args[5];
    wide = macrunner_hb_strieq( thunk->import_name, "FormatMessageW" );
    allocate = !!(flags & FORMAT_MESSAGE_ALLOCATE_BUFFER);

    if ((flags & FORMAT_MESSAGE_FROM_STRING) && args[1])
    {
        RtlSetLastWin32Error( ERROR_CALL_NOT_IMPLEMENTED );
        NtCurrentTeb()->LastStatusValue = STATUS_NOT_IMPLEMENTED;
        *ret = 0;
        return TRUE;
    }

    known = macrunner_hb_win32_error_text( message_id );
    if (known) macrunner_hb_copy_cstr( textA, sizeof(textA), known );
    else snprintf( textA, sizeof(textA), "Unknown error %lu.\r\n", (unsigned long)message_id );

    len = strlen( textA );
    for (i = 0; i <= len && i < ARRAY_SIZE(textW); i++) textW[i] = (unsigned char)textA[i];
    if (i == ARRAY_SIZE(textW)) textW[ARRAY_SIZE(textW) - 1] = 0;

    if (allocate)
    {
        void *buffer;
        SIZE_T bytes = wide ? (len + 1) * sizeof(WCHAR) : len + 1;

        if (!dst)
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = 0;
            return TRUE;
        }
        buffer = macrunner_hb_local_heap_alloc( bytes, FALSE );
        if (!buffer)
        {
            RtlSetLastWin32Error( ERROR_NOT_ENOUGH_MEMORY );
            NtCurrentTeb()->LastStatusValue = STATUS_NO_MEMORY;
            *ret = 0;
            return TRUE;
        }
        if (wide) memcpy( buffer, textW, bytes );
        else memcpy( buffer, textA, bytes );
        if (hb_memory_write_u64( ctx->memory, (hb_gva_t)dst, (uint64_t)(uintptr_t)buffer ) != HB_OK)
        {
            macrunner_hb_local_heap_release( buffer );
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = 0;
            return TRUE;
        }
    }
    else
    {
        if (!dst || !size)
        {
            RtlSetLastWin32Error( ERROR_INSUFFICIENT_BUFFER );
            NtCurrentTeb()->LastStatusValue = STATUS_BUFFER_TOO_SMALL;
            *ret = 0;
            return TRUE;
        }
        if (!macrunner_hb_write_guest_win32_string( ctx, dst, size, textW, len, wide, NULL ))
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = 0;
            return TRUE;
        }
        if (len >= size)
        {
            RtlSetLastWin32Error( ERROR_INSUFFICIENT_BUFFER );
            NtCurrentTeb()->LastStatusValue = STATUS_BUFFER_TOO_SMALL;
            *ret = 0;
            return TRUE;
        }
    }

    RtlSetLastWin32Error( ERROR_SUCCESS );
    NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
    *ret = len;
    TRACE( "MacRunner HyperBridge semantic %s!%s id=%lu flags=%08lx len=%lu allocate=%u\n",
           thunk->dll_name, thunk->import_name, (unsigned long)message_id,
           (unsigned long)flags, (unsigned long)len, allocate );
    return TRUE;
}

static BOOL macrunner_hb_try_etw_semantic( hb_context_t *ctx,
                                           const struct macrunner_hb_import_thunk *thunk,
                                           const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                           uint64_t *ret )
{
    if (!ctx || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_stristarts( thunk->import_name, "Etw" ) &&
        !macrunner_hb_stristarts( thunk->import_name, "Event" ))
        return FALSE;

    if (macrunner_hb_strieq( thunk->import_name, "EtwEventEnabled" ) ||
        macrunner_hb_strieq( thunk->import_name, "EtwEventProviderEnabled" ) ||
        macrunner_hb_strieq( thunk->import_name, "EtwGetTraceEnableFlags" ) ||
        macrunner_hb_strieq( thunk->import_name, "EtwGetTraceEnableLevel" ) ||
        macrunner_hb_strieq( thunk->import_name, "EtwGetTraceLoggerHandle" ) ||
        macrunner_hb_strieq( thunk->import_name, "EventEnabled" ) ||
        macrunner_hb_strieq( thunk->import_name, "EventProviderEnabled" ))
    {
        *ret = 0;
        return TRUE;
    }

    if ((macrunner_hb_strieq( thunk->import_name, "EtwEventRegister" ) ||
         macrunner_hb_strieq( thunk->import_name, "EventRegister" )) && args[3])
        hb_memory_write_u64( ctx->memory, (hb_gva_t)args[3], 1 );
    else if ((macrunner_hb_strieq( thunk->import_name, "EtwRegisterTraceGuidsA" ) ||
              macrunner_hb_strieq( thunk->import_name, "EtwRegisterTraceGuidsW" )) && args[7])
        hb_memory_write_u64( ctx->memory, (hb_gva_t)args[7], 1 );

    NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
    RtlSetLastWin32Error( ERROR_SUCCESS );
    *ret = 0;
    TRACE( "MacRunner HyperBridge semantic ntdll!%s noop\n", thunk->import_name );
    return TRUE;
}

static BOOL macrunner_hb_try_cotaskmem_semantic( hb_context_t *ctx,
                                                const struct macrunner_hb_import_thunk *thunk,
                                                const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                uint64_t *ret )
{
    void *ptr;

    if (!ctx || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_strieq( thunk->dll_name, "ole32.dll" ) &&
        !macrunner_hb_strieq( thunk->dll_name, "combase.dll" ))
        return FALSE;

    if (macrunner_hb_strieq( thunk->import_name, "CoTaskMemAlloc" ))
    {
        ptr = macrunner_hb_local_heap_alloc( (SIZE_T)args[0], FALSE );
        *ret = (uint64_t)(uintptr_t)ptr;
        return TRUE;
    }
    if (macrunner_hb_strieq( thunk->import_name, "CoTaskMemRealloc" ))
    {
        ptr = (void *)(uintptr_t)args[0];

        if (!ptr || macrunner_hb_local_heap_contains( ptr ))
        {
            ptr = macrunner_hb_local_heap_realloc( ptr, (SIZE_T)args[1], FALSE, FALSE );
            *ret = (uint64_t)(uintptr_t)ptr;
            return TRUE;
        }
    }
    if (macrunner_hb_strieq( thunk->import_name, "CoTaskMemFree" ))
    {
        ptr = (void *)(uintptr_t)args[0];

        if (!ptr || macrunner_hb_local_heap_release( ptr ))
        {
            *ret = 0;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL macrunner_hb_guid_equal( const GUID *a, const GUID *b )
{
    return a && b && !memcmp( a, b, sizeof(*a) );
}

static BOOL macrunner_hb_get_process_env_w( const WCHAR *name, WCHAR *value, size_t value_count )
{
    RTL_USER_PROCESS_PARAMETERS *params = NtCurrentTeb()->Peb->ProcessParameters;
    const WCHAR *env = params ? params->Environment : NULL;
    size_t name_len = macrunner_hb_wcslen_local( name );

    if (!name || !value || !value_count) return FALSE;
    value[0] = 0;
    while (env && *env)
    {
        const WCHAR *entry = env;
        const WCHAR *eq = entry;
        size_t len = macrunner_hb_wcslen_local( entry );
        size_t copy_len;

        while (*eq && *eq != '=') eq++;
        if (*eq == '=' && (size_t)(eq - entry) == name_len &&
            !macrunner_hb_wcsnicmp_local( entry, name, (ULONG)name_len ))
        {
            const WCHAR *src = eq + 1;

            copy_len = macrunner_hb_wcslen_local( src );
            if (copy_len >= value_count) copy_len = value_count - 1;
            memcpy( value, src, copy_len * sizeof(WCHAR) );
            value[copy_len] = 0;
            return TRUE;
        }
        env += len + 1;
    }
    return FALSE;
}

static void macrunner_hb_append_wstr( WCHAR *dst, size_t dst_count, const WCHAR *suffix )
{
    size_t len = macrunner_hb_wcslen_local( dst );
    size_t i = 0;

    if (!dst || !dst_count || !suffix || len >= dst_count) return;
    while (len + i + 1 < dst_count && suffix[i])
    {
        dst[len + i] = suffix[i];
        i++;
    }
    dst[len + i] = 0;
}

static void macrunner_hb_default_user_profile_w( WCHAR *path, size_t path_count )
{
    static const WCHAR userprofileW[] = {'U','S','E','R','P','R','O','F','I','L','E',0};
    static const WCHAR usernameW[] = {'U','S','E','R','N','A','M','E',0};
    static const WCHAR prefixW[] = {'C',':','\\','u','s','e','r','s','\\',0};
    static const WCHAR userW[] = {'u','s','e','r',0};
    WCHAR name[128];

    if (!path || !path_count) return;
    if (macrunner_hb_get_process_env_w( userprofileW, path, path_count )) return;

    memcpy( path, prefixW, sizeof(prefixW) );
    if (!macrunner_hb_get_process_env_w( usernameW, name, ARRAY_SIZE(name) ))
        memcpy( name, userW, sizeof(userW) );
    macrunner_hb_append_wstr( path, path_count, name );
}

static void macrunner_hb_known_folder_path_w( const GUID *id, WCHAR *path, size_t path_count )
{
    static const GUID folderid_desktop =
        {0xb4bfcc3a,0xdb2c,0x424c,{0xb0,0x29,0x7f,0xe9,0x9a,0x87,0xc6,0x41}};
    static const GUID folderid_documents =
        {0xfdd39ad0,0x238f,0x46af,{0xad,0xb4,0x6c,0x85,0x48,0x03,0x69,0xc7}};
    static const GUID folderid_local_appdata =
        {0xf1b32785,0x6fba,0x4fcf,{0x9d,0x55,0x7b,0x8e,0x7f,0x15,0x70,0x91}};
    static const GUID folderid_local_appdata_low =
        {0xa520a1a4,0x1780,0x4ff6,{0xbd,0x18,0x16,0x73,0x43,0xc5,0xaf,0x16}};
    static const GUID folderid_profile =
        {0x5e6c858f,0x0e22,0x4760,{0x9a,0xfe,0xea,0x33,0x17,0xb6,0x71,0x73}};
    static const GUID folderid_program_data =
        {0x62ab5d82,0xfdc1,0x4dc3,{0xa9,0xdd,0x07,0x0d,0x1d,0x49,0x5d,0x97}};
    static const GUID folderid_roaming_appdata =
        {0x3eb685db,0x65f9,0x4cf6,{0xa0,0x3a,0xe3,0xef,0x65,0x72,0x9f,0x3d}};
    static const GUID folderid_saved_games =
        {0x4c5c32ff,0xbb9d,0x43b0,{0xb5,0xb4,0x2d,0x72,0xe5,0x4e,0xaa,0xa4}};
    static const GUID folderid_system =
        {0x1ac14e77,0x02e7,0x4e5d,{0xb7,0x44,0x2e,0xb1,0xae,0x51,0x98,0xb7}};
    static const GUID folderid_windows =
        {0xf38bf404,0x1d43,0x42f2,{0x93,0x05,0x67,0xde,0x0b,0x28,0xfc,0x23}};
    static const WCHAR appdata_localW[] = {'\\','A','p','p','D','a','t','a','\\','L','o','c','a','l',0};
    static const WCHAR appdata_lowW[] = {'\\','A','p','p','D','a','t','a','\\','L','o','c','a','l','L','o','w',0};
    static const WCHAR appdata_roamingW[] = {'\\','A','p','p','D','a','t','a','\\','R','o','a','m','i','n','g',0};
    static const WCHAR desktopW[] = {'\\','D','e','s','k','t','o','p',0};
    static const WCHAR documentsW[] = {'\\','D','o','c','u','m','e','n','t','s',0};
    static const WCHAR saved_gamesW[] = {'\\','S','a','v','e','d',' ','G','a','m','e','s',0};
    static const WCHAR program_dataW[] = {'C',':','\\','P','r','o','g','r','a','m','D','a','t','a',0};
    static const WCHAR systemW[] = {'C',':','\\','w','i','n','d','o','w','s','\\','s','y','s','t','e','m','3','2',0};
    static const WCHAR windowsW[] = {'C',':','\\','w','i','n','d','o','w','s',0};

    if (!path || !path_count) return;
    path[0] = 0;
    if (macrunner_hb_guid_equal( id, &folderid_program_data ))
    {
        memcpy( path, program_dataW, sizeof(program_dataW) );
        return;
    }
    if (macrunner_hb_guid_equal( id, &folderid_system ))
    {
        memcpy( path, systemW, sizeof(systemW) );
        return;
    }
    if (macrunner_hb_guid_equal( id, &folderid_windows ))
    {
        memcpy( path, windowsW, sizeof(windowsW) );
        return;
    }

    macrunner_hb_default_user_profile_w( path, path_count );
    if (macrunner_hb_guid_equal( id, &folderid_profile )) return;
    if (macrunner_hb_guid_equal( id, &folderid_local_appdata ))
        macrunner_hb_append_wstr( path, path_count, appdata_localW );
    else if (macrunner_hb_guid_equal( id, &folderid_local_appdata_low ))
        macrunner_hb_append_wstr( path, path_count, appdata_lowW );
    else if (macrunner_hb_guid_equal( id, &folderid_roaming_appdata ))
        macrunner_hb_append_wstr( path, path_count, appdata_roamingW );
    else if (macrunner_hb_guid_equal( id, &folderid_desktop ))
        macrunner_hb_append_wstr( path, path_count, desktopW );
    else if (macrunner_hb_guid_equal( id, &folderid_documents ))
        macrunner_hb_append_wstr( path, path_count, documentsW );
    else if (macrunner_hb_guid_equal( id, &folderid_saved_games ))
        macrunner_hb_append_wstr( path, path_count, saved_gamesW );
}

static BOOL macrunner_hb_try_known_folder_semantic( hb_context_t *ctx,
                                                   const struct macrunner_hb_import_thunk *thunk,
                                                   const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                   uint64_t *ret )
{
    GUID id;
    WCHAR path[MAX_PATH];
    WCHAR *allocated;
    SIZE_T size;

    if (!ctx || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_strieq( thunk->dll_name, "shell32.dll" ) ||
        !macrunner_hb_strieq( thunk->import_name, "SHGetKnownFolderPath" ))
        return FALSE;

    if (!args[0] || !args[3])
    {
        *ret = E_INVALIDARG;
        return TRUE;
    }
    if (hb_memory_write_u64( ctx->memory, (hb_gva_t)args[3], 0 ) != HB_OK ||
        hb_memory_read( ctx->memory, (hb_gva_t)args[0], &id, sizeof(id) ) != HB_OK)
    {
        *ret = E_INVALIDARG;
        return TRUE;
    }

    macrunner_hb_known_folder_path_w( &id, path, ARRAY_SIZE(path) );
    size = (macrunner_hb_wcslen_local( path ) + 1) * sizeof(WCHAR);
    if (!(allocated = macrunner_hb_local_heap_alloc( size, FALSE )))
    {
        *ret = E_OUTOFMEMORY;
        return TRUE;
    }
    memcpy( allocated, path, size );
    if (hb_memory_write( ctx->memory, (hb_gva_t)args[3], &allocated, sizeof(allocated) ) != HB_OK)
    {
        macrunner_hb_local_heap_release( allocated );
        *ret = E_INVALIDARG;
        return TRUE;
    }

    TRACE( "MacRunner HyperBridge semantic %s!SHGetKnownFolderPath rfid=%08x-%04x-%04x path=%p\n",
           thunk->dll_name, id.Data1, id.Data2, id.Data3, allocated );
    *ret = S_OK;
    return TRUE;
}

static BOOL macrunner_hb_read_guest_path( hb_context_t *ctx, uint64_t addr, BOOL ansi,
                                          char *out, size_t out_size )
{
    return ansi ? macrunner_hb_read_guest_astr( ctx, addr, out, out_size ) :
                  macrunner_hb_read_guest_wstr( ctx, addr, out, out_size );
}

static void macrunner_hb_normalize_unix_path( const char *input, char *out, size_t out_size )
{
    char tmp[4096];
    const char *p;
    size_t i, len;

    if (!out || !out_size) return;
    out[0] = 0;
    if (!input) return;

    for (i = 0; i + 1 < sizeof(tmp) && input[i]; i++)
        tmp[i] = (input[i] == '\\') ? '/' : input[i];
    tmp[i] = 0;

    p = tmp;
    if (!strncmp( p, "//??/", 5 )) p += 5;
    if ((p[0] == 'Z' || p[0] == 'z') && p[1] == ':') p += 2;

    if (p[0] == '/')
    {
        macrunner_hb_copy_cstr( out, out_size, p );
        return;
    }

    if (p[0] && p[1] == ':') p += 2;
    while (*p == '/') p++;
    if (!getcwd( out, out_size ))
    {
        out[0] = 0;
        return;
    }
    len = strlen(out);
    if (len + 1 < out_size && (!len || out[len - 1] != '/')) out[len++] = '/';
    out[len] = 0;
    if (len < out_size) macrunner_hb_copy_cstr( out + len, out_size - len, p );
}

static LARGE_INTEGER macrunner_hb_unix_time_to_filetime( time_t value )
{
    LARGE_INTEGER ret;
    uint64_t ticks = ((uint64_t)value + 11644473600ULL) * 10000000ULL;

    ret.QuadPart = ticks;
    return ret;
}

static void macrunner_hb_filetime_from_large( FILETIME *out, LARGE_INTEGER value )
{
    out->dwLowDateTime = (DWORD)value.QuadPart;
    out->dwHighDateTime = (DWORD)((uint64_t)value.QuadPart >> 32);
}

static DWORD macrunner_hb_attributes_from_stat( const struct stat *st )
{
    DWORD attrs = FILE_ATTRIBUTE_ARCHIVE;

    if (S_ISDIR( st->st_mode )) attrs = FILE_ATTRIBUTE_DIRECTORY;
    if (!(st->st_mode & S_IWUSR)) attrs |= FILE_ATTRIBUTE_READONLY;
    return attrs;
}

static BOOL macrunner_hb_query_file_attributes_path( const char *path,
                                                     FILE_BASIC_INFORMATION *basic,
                                                     FILE_NETWORK_OPEN_INFORMATION *full )
{
    struct stat st;
    char unix_path[4096];
    DWORD attrs;

    macrunner_hb_normalize_unix_path( path, unix_path, sizeof(unix_path) );
    if (!unix_path[0] || stat( unix_path, &st ))
    {
        DWORD err = (errno == ENOENT || errno == ENOTDIR) ? ERROR_PATH_NOT_FOUND : ERROR_ACCESS_DENIED;
        NTSTATUS status = (errno == ENOENT || errno == ENOTDIR) ? STATUS_OBJECT_PATH_NOT_FOUND :
                                                              STATUS_ACCESS_DENIED;

        RtlSetLastWin32Error( err );
        NtCurrentTeb()->LastStatusValue = status;
        return FALSE;
    }

    attrs = macrunner_hb_attributes_from_stat( &st );
    if (basic)
    {
        memset( basic, 0, sizeof(*basic) );
        basic->CreationTime = macrunner_hb_unix_time_to_filetime( st.st_ctime );
        basic->LastAccessTime = macrunner_hb_unix_time_to_filetime( st.st_atime );
        basic->LastWriteTime = macrunner_hb_unix_time_to_filetime( st.st_mtime );
        basic->ChangeTime = macrunner_hb_unix_time_to_filetime( st.st_ctime );
        basic->FileAttributes = attrs;
    }
    if (full)
    {
        memset( full, 0, sizeof(*full) );
        full->CreationTime = macrunner_hb_unix_time_to_filetime( st.st_ctime );
        full->LastAccessTime = macrunner_hb_unix_time_to_filetime( st.st_atime );
        full->LastWriteTime = macrunner_hb_unix_time_to_filetime( st.st_mtime );
        full->ChangeTime = macrunner_hb_unix_time_to_filetime( st.st_ctime );
        full->AllocationSize.QuadPart = (LONGLONG)st.st_blocks * 512;
        full->EndOfFile.QuadPart = st.st_size;
        full->FileAttributes = attrs;
    }
    RtlSetLastWin32Error( ERROR_SUCCESS );
    NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
    return TRUE;
}

static void macrunner_hb_set_errno_error( void );

static BOOL macrunner_hb_try_file_attribute_semantic( hb_context_t *ctx,
                                                      const struct macrunner_hb_import_thunk *thunk,
                                                      const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                      uint64_t *ret )
{
    char name[4096];
    BOOL ansi, ex, set_attrs;

    if (!ctx || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) &&
        !macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" ))
        return FALSE;

    ansi = macrunner_hb_strieq( thunk->import_name, "GetFileAttributesA" ) ||
           macrunner_hb_strieq( thunk->import_name, "GetFileAttributesExA" ) ||
           macrunner_hb_strieq( thunk->import_name, "SetFileAttributesA" );
    ex = macrunner_hb_strieq( thunk->import_name, "GetFileAttributesExA" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetFileAttributesExW" );
    set_attrs = macrunner_hb_strieq( thunk->import_name, "SetFileAttributesA" ) ||
                macrunner_hb_strieq( thunk->import_name, "SetFileAttributesW" );

    if (!ansi && !ex && !set_attrs && !macrunner_hb_strieq( thunk->import_name, "GetFileAttributesW" ))
        return FALSE;
    if (!macrunner_hb_read_guest_path( ctx, args[0], ansi, name, sizeof(name) ))
    {
        RtlSetLastWin32Error( ERROR_PATH_NOT_FOUND );
        NtCurrentTeb()->LastStatusValue = STATUS_OBJECT_PATH_NOT_FOUND;
        *ret = (ex || set_attrs) ? FALSE : INVALID_FILE_ATTRIBUTES;
        return TRUE;
    }

    if (set_attrs)
    {
        char unix_path[4096];
        struct stat st;
        mode_t mode;

        macrunner_hb_normalize_unix_path( name, unix_path, sizeof(unix_path) );
        if (!unix_path[0] || stat( unix_path, &st ))
        {
            macrunner_hb_set_errno_error();
            *ret = FALSE;
            return TRUE;
        }

        mode = st.st_mode;
        if ((DWORD)args[1] & FILE_ATTRIBUTE_READONLY)
            mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
        else
            mode |= S_IWUSR;
        if (chmod( unix_path, mode ) < 0)
        {
            macrunner_hb_set_errno_error();
            *ret = FALSE;
            return TRUE;
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = TRUE;
        TRACE( "MacRunner HyperBridge semantic %s!%s path=%s unix=%s attrs=0x%lx\n",
               thunk->dll_name, thunk->import_name, name, unix_path, (unsigned long)(DWORD)args[1] );
        return TRUE;
    }

    if (ex)
    {
        FILE_NETWORK_OPEN_INFORMATION info;
        WIN32_FILE_ATTRIBUTE_DATA data;

        if ((GET_FILEEX_INFO_LEVELS)args[1] != GetFileExInfoStandard || !args[2])
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = FALSE;
            return TRUE;
        }
        if (!macrunner_hb_query_file_attributes_path( name, NULL, &info ))
        {
            *ret = FALSE;
            return TRUE;
        }
        memset( &data, 0, sizeof(data) );
        data.dwFileAttributes = info.FileAttributes;
        data.ftCreationTime.dwLowDateTime = info.CreationTime.u.LowPart;
        data.ftCreationTime.dwHighDateTime = info.CreationTime.u.HighPart;
        data.ftLastAccessTime.dwLowDateTime = info.LastAccessTime.u.LowPart;
        data.ftLastAccessTime.dwHighDateTime = info.LastAccessTime.u.HighPart;
        data.ftLastWriteTime.dwLowDateTime = info.LastWriteTime.u.LowPart;
        data.ftLastWriteTime.dwHighDateTime = info.LastWriteTime.u.HighPart;
        data.nFileSizeLow = info.EndOfFile.u.LowPart;
        data.nFileSizeHigh = info.EndOfFile.u.HighPart;
        if (hb_memory_write( ctx->memory, (hb_gva_t)args[2], &data, sizeof(data) ) != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = FALSE;
            return TRUE;
        }
        *ret = TRUE;
        return TRUE;
    }
    else
    {
        FILE_BASIC_INFORMATION info;

        if (!macrunner_hb_query_file_attributes_path( name, &info, NULL ))
        {
            *ret = INVALID_FILE_ATTRIBUTES;
            return TRUE;
        }
        *ret = info.FileAttributes;
        return TRUE;
    }
}

static void macrunner_hb_set_errno_error( void )
{
    DWORD err;
    NTSTATUS status;

    switch (errno)
    {
    case ENOENT:
    case ENOTDIR:
        err = ERROR_FILE_NOT_FOUND;
        status = STATUS_OBJECT_NAME_NOT_FOUND;
        break;
    case EACCES:
    case EPERM:
        err = ERROR_ACCESS_DENIED;
        status = STATUS_ACCESS_DENIED;
        break;
    default:
        err = ERROR_INVALID_PARAMETER;
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    RtlSetLastWin32Error( err );
    NtCurrentTeb()->LastStatusValue = status;
}

static int macrunner_hb_createfile_open_flags( DWORD access, DWORD disposition )
{
    int flags = 0;

    if ((access & GENERIC_WRITE) && (access & GENERIC_READ)) flags |= O_RDWR;
    else if (access & GENERIC_WRITE) flags |= O_WRONLY;
    else flags |= O_RDONLY;

    switch (disposition)
    {
    case CREATE_NEW:
        flags |= O_CREAT | O_EXCL;
        break;
    case CREATE_ALWAYS:
        flags |= O_CREAT | O_TRUNC;
        break;
    case OPEN_ALWAYS:
        flags |= O_CREAT;
        break;
    case TRUNCATE_EXISTING:
        flags |= O_TRUNC;
        break;
    case OPEN_EXISTING:
    default:
        break;
    }
    return flags;
}

static BOOL macrunner_hb_try_local_file_semantic( hb_context_t *ctx,
                                                  const struct macrunner_hb_import_thunk *thunk,
                                                  const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                  uint64_t *ret )
{
    BOOL ansi, create_directory, create_directory_ex;

    if (!ctx || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) &&
        !macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" ))
        return FALSE;

    create_directory = macrunner_hb_strieq( thunk->import_name, "CreateDirectoryA" ) ||
                       macrunner_hb_strieq( thunk->import_name, "CreateDirectoryW" );
    create_directory_ex = macrunner_hb_strieq( thunk->import_name, "CreateDirectoryExA" ) ||
                          macrunner_hb_strieq( thunk->import_name, "CreateDirectoryExW" );
    if (create_directory || create_directory_ex)
    {
        char path[4096], unix_path[4096];
        struct stat st;
        uint64_t path_arg = create_directory_ex ? args[1] : args[0];
        int err;

        ansi = macrunner_hb_strieq( thunk->import_name, "CreateDirectoryA" ) ||
               macrunner_hb_strieq( thunk->import_name, "CreateDirectoryExA" );
        if (!macrunner_hb_read_guest_path( ctx, path_arg, ansi, path, sizeof(path) ))
        {
            RtlSetLastWin32Error( ERROR_PATH_NOT_FOUND );
            NtCurrentTeb()->LastStatusValue = STATUS_OBJECT_PATH_NOT_FOUND;
            *ret = FALSE;
            return TRUE;
        }
        macrunner_hb_normalize_unix_path( path, unix_path, sizeof(unix_path) );
        if (!unix_path[0])
        {
            RtlSetLastWin32Error( ERROR_PATH_NOT_FOUND );
            NtCurrentTeb()->LastStatusValue = STATUS_OBJECT_PATH_NOT_FOUND;
            *ret = FALSE;
            return TRUE;
        }
        if (!mkdir( unix_path, 0777 ))
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
            *ret = TRUE;
            TRACE( "MacRunner HyperBridge semantic %s!%s path=%s unix=%s created\n",
                   thunk->dll_name, thunk->import_name, path, unix_path );
            return TRUE;
        }

        err = errno;
        if (err == EEXIST)
        {
            RtlSetLastWin32Error( ERROR_ALREADY_EXISTS );
            NtCurrentTeb()->LastStatusValue = STATUS_OBJECT_NAME_COLLISION;
        }
        else if (err == ENOENT || err == ENOTDIR)
        {
            RtlSetLastWin32Error( ERROR_PATH_NOT_FOUND );
            NtCurrentTeb()->LastStatusValue = STATUS_OBJECT_PATH_NOT_FOUND;
        }
        else if (err == EACCES || err == EPERM)
        {
            RtlSetLastWin32Error( ERROR_ACCESS_DENIED );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_DENIED;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
        }
        if (err == EEXIST && (!stat( unix_path, &st ) && !S_ISDIR( st.st_mode )))
        {
            RtlSetLastWin32Error( ERROR_ALREADY_EXISTS );
            NtCurrentTeb()->LastStatusValue = STATUS_OBJECT_NAME_COLLISION;
        }
        *ret = FALSE;
        TRACE( "MacRunner HyperBridge semantic %s!%s path=%s unix=%s failed errno=%d last_error=%lu\n",
               thunk->dll_name, thunk->import_name, path, unix_path, err,
               (unsigned long)RtlGetLastWin32Error() );
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "MoveFileA" ) ||
        macrunner_hb_strieq( thunk->import_name, "MoveFileW" ) ||
        macrunner_hb_strieq( thunk->import_name, "MoveFileExA" ) ||
        macrunner_hb_strieq( thunk->import_name, "MoveFileExW" ))
    {
        char old_path[4096], new_path[4096], old_unix[4096], new_unix[4096];
        DWORD flags = (macrunner_hb_strieq( thunk->import_name, "MoveFileExA" ) ||
                       macrunner_hb_strieq( thunk->import_name, "MoveFileExW" )) ? (DWORD)args[2] : 0;
        BOOL move_ex = macrunner_hb_strieq( thunk->import_name, "MoveFileExA" ) ||
                       macrunner_hb_strieq( thunk->import_name, "MoveFileExW" );
        struct stat st;

        ansi = macrunner_hb_strieq( thunk->import_name, "MoveFileA" ) ||
               macrunner_hb_strieq( thunk->import_name, "MoveFileExA" );
        if (!macrunner_hb_read_guest_path( ctx, args[0], ansi, old_path, sizeof(old_path) ) ||
            (!args[1] && !move_ex) ||
            (args[1] && !macrunner_hb_read_guest_path( ctx, args[1], ansi, new_path, sizeof(new_path) )))
        {
            RtlSetLastWin32Error( ERROR_PATH_NOT_FOUND );
            NtCurrentTeb()->LastStatusValue = STATUS_OBJECT_PATH_NOT_FOUND;
            *ret = FALSE;
            return TRUE;
        }
        if (!args[1])
        {
            RtlSetLastWin32Error( ERROR_CALL_NOT_IMPLEMENTED );
            NtCurrentTeb()->LastStatusValue = STATUS_NOT_IMPLEMENTED;
            *ret = FALSE;
            return TRUE;
        }
        macrunner_hb_normalize_unix_path( old_path, old_unix, sizeof(old_unix) );
        macrunner_hb_normalize_unix_path( new_path, new_unix, sizeof(new_unix) );
        if (!old_unix[0] || !new_unix[0])
        {
            RtlSetLastWin32Error( ERROR_PATH_NOT_FOUND );
            NtCurrentTeb()->LastStatusValue = STATUS_OBJECT_PATH_NOT_FOUND;
            *ret = FALSE;
            return TRUE;
        }
        if (!(flags & MOVEFILE_REPLACE_EXISTING) && !stat( new_unix, &st ))
        {
            RtlSetLastWin32Error( ERROR_ALREADY_EXISTS );
            NtCurrentTeb()->LastStatusValue = STATUS_OBJECT_NAME_COLLISION;
            *ret = FALSE;
            return TRUE;
        }
        if (rename( old_unix, new_unix ) < 0)
        {
            macrunner_hb_set_errno_error();
            *ret = FALSE;
            return TRUE;
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = TRUE;
        TRACE( "MacRunner HyperBridge semantic %s!%s old=%s new=%s flags=0x%lx\n",
               thunk->dll_name, thunk->import_name, old_unix, new_unix, (unsigned long)flags );
        return TRUE;
    }

    ansi = macrunner_hb_strieq( thunk->import_name, "CreateFileA" );
    if (ansi || macrunner_hb_strieq( thunk->import_name, "CreateFileW" ))
    {
        char path[4096], unix_path[4096];
        int fd, flags;
        uint64_t handle;

        if (!macrunner_hb_read_guest_path( ctx, args[0], ansi, path, sizeof(path) ))
        {
            RtlSetLastWin32Error( ERROR_PATH_NOT_FOUND );
            NtCurrentTeb()->LastStatusValue = STATUS_OBJECT_PATH_NOT_FOUND;
            *ret = (uint64_t)(uintptr_t)INVALID_HANDLE_VALUE;
            return TRUE;
        }
        macrunner_hb_normalize_unix_path( path, unix_path, sizeof(unix_path) );
        flags = macrunner_hb_createfile_open_flags( (DWORD)args[1], (DWORD)args[4] );
        fd = open( unix_path, flags, 0666 );
        if (fd < 0)
        {
            macrunner_hb_set_errno_error();
            *ret = (uint64_t)(uintptr_t)INVALID_HANDLE_VALUE;
            return TRUE;
        }
        if (!(handle = macrunner_hb_local_file_remember( fd )))
        {
            close( fd );
            RtlSetLastWin32Error( ERROR_TOO_MANY_OPEN_FILES );
            NtCurrentTeb()->LastStatusValue = STATUS_TOO_MANY_OPENED_FILES;
            *ret = (uint64_t)(uintptr_t)INVALID_HANDLE_VALUE;
            return TRUE;
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = handle;
        TRACE( "MacRunner HyperBridge semantic %s!%s path=%s unix=%s handle=%p\n",
               thunk->dll_name, thunk->import_name, path, unix_path, (void *)(uintptr_t)handle );
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "ReadFile" ))
    {
        int fd = macrunner_hb_local_file_fd( args[0] );
        DWORD request = (DWORD)args[2], done32 = 0;
        char *buffer;
        ssize_t done;

        if (fd < 0) return FALSE;
        if (!(buffer = malloc( request ? request : 1 )))
        {
            RtlSetLastWin32Error( ERROR_NOT_ENOUGH_MEMORY );
            NtCurrentTeb()->LastStatusValue = STATUS_NO_MEMORY;
            *ret = FALSE;
            return TRUE;
        }
        done = read( fd, buffer, request );
        if (done < 0)
        {
            free( buffer );
            macrunner_hb_set_errno_error();
            if (args[3]) hb_memory_write_u32( ctx->memory, (hb_gva_t)args[3], 0 );
            *ret = FALSE;
            return TRUE;
        }
        if (done && hb_memory_write( ctx->memory, (hb_gva_t)args[1], buffer, (size_t)done ) != HB_OK)
        {
            free( buffer );
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            if (args[3]) hb_memory_write_u32( ctx->memory, (hb_gva_t)args[3], 0 );
            *ret = FALSE;
            return TRUE;
        }
        free( buffer );
        done32 = (DWORD)done;
        if (args[3]) hb_memory_write_u32( ctx->memory, (hb_gva_t)args[3], done32 );
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = TRUE;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetFileSize" ) ||
        macrunner_hb_strieq( thunk->import_name, "GetFileSizeEx" ))
    {
        int fd = macrunner_hb_local_file_fd( args[0] );
        struct stat st;

        if (fd < 0) return FALSE;
        if (fstat( fd, &st ))
        {
            macrunner_hb_set_errno_error();
            *ret = macrunner_hb_strieq( thunk->import_name, "GetFileSizeEx" ) ? FALSE : INVALID_FILE_SIZE;
            return TRUE;
        }
        if (macrunner_hb_strieq( thunk->import_name, "GetFileSizeEx" ))
        {
            LARGE_INTEGER size;

            size.QuadPart = st.st_size;
            if (!args[1] || hb_memory_write( ctx->memory, (hb_gva_t)args[1], &size, sizeof(size) ) != HB_OK)
            {
                RtlSetLastWin32Error( ERROR_NOACCESS );
                NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
                *ret = FALSE;
                return TRUE;
            }
            *ret = TRUE;
        }
        else
        {
            if (args[1]) hb_memory_write_u32( ctx->memory, (hb_gva_t)args[1], (DWORD)((uint64_t)st.st_size >> 32) );
            *ret = (DWORD)st.st_size;
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetFileInformationByHandle" ))
    {
        int fd = macrunner_hb_local_file_fd( args[0] );
        BY_HANDLE_FILE_INFORMATION info;
        LARGE_INTEGER ft;
        struct stat st;

        if (fd < 0) return FALSE;
        if (!args[1])
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = FALSE;
            return TRUE;
        }
        if (fstat( fd, &st ))
        {
            macrunner_hb_set_errno_error();
            *ret = FALSE;
            return TRUE;
        }

        memset( &info, 0, sizeof(info) );
        info.dwFileAttributes = macrunner_hb_attributes_from_stat( &st );
        ft = macrunner_hb_unix_time_to_filetime( st.st_ctime );
        macrunner_hb_filetime_from_large( &info.ftCreationTime, ft );
        ft = macrunner_hb_unix_time_to_filetime( st.st_atime );
        macrunner_hb_filetime_from_large( &info.ftLastAccessTime, ft );
        ft = macrunner_hb_unix_time_to_filetime( st.st_mtime );
        macrunner_hb_filetime_from_large( &info.ftLastWriteTime, ft );
        info.nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
        info.nFileSizeLow = (DWORD)st.st_size;
        info.nNumberOfLinks = (DWORD)st.st_nlink;
        info.nFileIndexHigh = (DWORD)((uint64_t)st.st_ino >> 32);
        info.nFileIndexLow = (DWORD)st.st_ino;

        if (hb_memory_write( ctx->memory, (hb_gva_t)args[1], &info, sizeof(info) ) != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = FALSE;
            return TRUE;
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = TRUE;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "SetFilePointer" ) ||
        macrunner_hb_strieq( thunk->import_name, "SetFilePointerEx" ))
    {
        int fd = macrunner_hb_local_file_fd( args[0] );
        LARGE_INTEGER out;
        off_t pos;
        int whence;
        int64_t distance;

        if (fd < 0) return FALSE;
        whence = ((DWORD)(macrunner_hb_strieq( thunk->import_name, "SetFilePointerEx" ) ? args[3] : args[3]) == FILE_END) ?
                 SEEK_END : (((DWORD)args[3] == FILE_CURRENT) ? SEEK_CUR : SEEK_SET);
        if (macrunner_hb_strieq( thunk->import_name, "SetFilePointerEx" ))
            distance = (int64_t)args[1];
        else
        {
            LONG high = 0;
            if (args[2]) hb_memory_read( ctx->memory, (hb_gva_t)args[2], &high, sizeof(high) );
            distance = ((int64_t)high << 32) | (DWORD)args[1];
        }
        pos = lseek( fd, (off_t)distance, whence );
        if (pos == (off_t)-1)
        {
            macrunner_hb_set_errno_error();
            *ret = macrunner_hb_strieq( thunk->import_name, "SetFilePointerEx" ) ? FALSE : INVALID_SET_FILE_POINTER;
            return TRUE;
        }
        if (macrunner_hb_strieq( thunk->import_name, "SetFilePointerEx" ))
        {
            out.QuadPart = pos;
            if (args[2]) hb_memory_write( ctx->memory, (hb_gva_t)args[2], &out, sizeof(out) );
            *ret = TRUE;
        }
        else
        {
            if (args[2]) hb_memory_write_u32( ctx->memory, (hb_gva_t)args[2], (DWORD)((uint64_t)pos >> 32) );
            *ret = (DWORD)pos;
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        return TRUE;
    }

    return FALSE;
}

static BOOL macrunner_hb_try_system_info_semantic( hb_context_t *ctx,
                                                   const struct macrunner_hb_import_thunk *thunk,
                                                   const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                   uint64_t *ret )
{
    SYSTEM_INFO info;
    BOOL is_logical_ex;
    BOOL is_logical_legacy;

    if (!ctx || !ctx->memory || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) &&
        !macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" ))
        return FALSE;
    is_logical_ex = macrunner_hb_strieq( thunk->import_name, "GetLogicalProcessorInformationEx" );
    is_logical_legacy = macrunner_hb_strieq( thunk->import_name, "GetLogicalProcessorInformation" );
    if (is_logical_ex || is_logical_legacy)
    {
        hb_gva_t buffer_gva = (hb_gva_t)(is_logical_ex ? args[1] : args[0]);
        hb_gva_t len_gva = (hb_gva_t)(is_logical_ex ? args[2] : args[1]);
        DWORD guest_len = 0;
        ULONG ret_len = 0;
        void *buffer = NULL;
        NTSTATUS status;

        if (!len_gva || hb_memory_read( ctx->memory, len_gva, &guest_len, sizeof(guest_len) ) != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = FALSE;
            return TRUE;
        }

        if (guest_len)
        {
            buffer = malloc( guest_len );
            if (!buffer)
            {
                RtlSetLastWin32Error( ERROR_OUTOFMEMORY );
                NtCurrentTeb()->LastStatusValue = STATUS_NO_MEMORY;
                *ret = FALSE;
                return TRUE;
            }
        }

        if (is_logical_ex)
        {
            LOGICAL_PROCESSOR_RELATIONSHIP relationship = (LOGICAL_PROCESSOR_RELATIONSHIP)args[0];
            status = NtQuerySystemInformationEx( SystemLogicalProcessorInformationEx, &relationship,
                                                 sizeof(relationship), buffer, guest_len, &ret_len );
        }
        else
            status = NtQuerySystemInformation( SystemLogicalProcessorInformation, buffer, guest_len, &ret_len );
        if (status == STATUS_INFO_LENGTH_MISMATCH) status = STATUS_BUFFER_TOO_SMALL;

        if ((!status || status == STATUS_BUFFER_TOO_SMALL) &&
            hb_memory_write( ctx->memory, len_gva, &ret_len, sizeof(ret_len) ) != HB_OK)
            status = STATUS_INVALID_PARAMETER;
        if (!status && guest_len && buffer_gva &&
            hb_memory_write( ctx->memory, buffer_gva, buffer, ret_len <= guest_len ? ret_len : guest_len ) != HB_OK)
            status = STATUS_INVALID_PARAMETER;

        free( buffer );
        NtCurrentTeb()->LastStatusValue = status;
        if (NT_ERROR( status ))
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = FALSE;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            *ret = TRUE;
        }
        return TRUE;
    }
    if (!macrunner_hb_strieq( thunk->import_name, "GetSystemInfo" ) &&
        !macrunner_hb_strieq( thunk->import_name, "GetNativeSystemInfo" ))
        return FALSE;

    memset( &info, 0, sizeof(info) );
    info.wProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
    info.dwPageSize = 0x1000;
    info.lpMinimumApplicationAddress = (void *)0x10000;
    info.lpMaximumApplicationAddress = (void *)0x00007ffffffeffffULL;
    info.dwActiveProcessorMask = 1;
    info.dwNumberOfProcessors = 1;
    info.dwProcessorType = PROCESSOR_AMD_X8664;
    info.dwAllocationGranularity = 0x10000;
    info.wProcessorLevel = 0x8664;
    info.wProcessorRevision = 0;
    if (!args[0] || hb_memory_write( ctx->memory, (hb_gva_t)args[0], &info, sizeof(info) ) != HB_OK)
    {
        RtlSetLastWin32Error( ERROR_NOACCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
    }
    else
    {
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
    }
    *ret = 0;
    return TRUE;
}

static void macrunner_hb_fill_memory_status( MEMORYSTATUSEX *status )
{
    uint64_t total = 8ULL * 1024 * 1024 * 1024;
    uint64_t avail = 4ULL * 1024 * 1024 * 1024;
    uint64_t total_virtual = 0x00007ffffffeffffULL - 0x10000ULL;

#ifdef __APPLE__
    mach_port_t host = mach_host_self();
    host_basic_info_data_t basic_info;
    vm_statistics64_data_t vm_stat;
    vm_size_t page_size = 0;
    mach_msg_type_number_t count;

    count = HOST_BASIC_INFO_COUNT;
    if (host_info( host, HOST_BASIC_INFO, (host_info_t)&basic_info, &count ) == KERN_SUCCESS &&
        basic_info.max_mem)
        total = basic_info.max_mem;

    count = HOST_VM_INFO64_COUNT;
    if (host_page_size( host, &page_size ) == KERN_SUCCESS &&
        host_statistics64( host, HOST_VM_INFO64, (host_info64_t)&vm_stat, &count ) == KERN_SUCCESS &&
        page_size)
        avail = ((uint64_t)vm_stat.free_count + vm_stat.inactive_count + vm_stat.speculative_count) *
                (uint64_t)page_size;

    mach_port_deallocate( mach_task_self(), host );
#endif

    if (!total) total = 8ULL * 1024 * 1024 * 1024;
    if (avail > total) avail = total;

    memset( status, 0, sizeof(*status) );
    status->dwLength = sizeof(*status);
    status->dwMemoryLoad = (DWORD)(((total - avail) * 100) / total);
    status->ullTotalPhys = total;
    status->ullAvailPhys = avail;
    status->ullTotalPageFile = total * 2;
    status->ullAvailPageFile = avail + total;
    status->ullTotalVirtual = total_virtual;
    status->ullAvailVirtual = total_virtual / 2;
    status->ullAvailExtendedVirtual = 0;
}

static BOOL macrunner_hb_try_memory_status_semantic( hb_context_t *ctx,
                                                     const struct macrunner_hb_import_thunk *thunk,
                                                     const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                     uint64_t *ret )
{
    MEMORYSTATUSEX status_ex;
    DWORD length;

    if (!ctx || !ctx->memory || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) &&
        !macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" ))
        return FALSE;
    if (!macrunner_hb_strieq( thunk->import_name, "GlobalMemoryStatus" ) &&
        !macrunner_hb_strieq( thunk->import_name, "GlobalMemoryStatusEx" ))
        return FALSE;

    if (!args[0])
    {
        RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
        NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
        *ret = FALSE;
        return TRUE;
    }

    macrunner_hb_fill_memory_status( &status_ex );

    if (macrunner_hb_strieq( thunk->import_name, "GlobalMemoryStatusEx" ))
    {
        if (hb_memory_read( ctx->memory, (hb_gva_t)args[0], &length, sizeof(length) ) != HB_OK ||
            length != sizeof(status_ex) ||
            hb_memory_write( ctx->memory, (hb_gva_t)args[0], &status_ex, sizeof(status_ex) ) != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = FALSE;
            return TRUE;
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = TRUE;
        return TRUE;
    }
    else
    {
        MEMORYSTATUS status;

        memset( &status, 0, sizeof(status) );
        status.dwLength = sizeof(status);
        status.dwMemoryLoad = status_ex.dwMemoryLoad;
        status.dwTotalPhys = status_ex.ullTotalPhys;
        status.dwAvailPhys = status_ex.ullAvailPhys;
        status.dwTotalPageFile = status_ex.ullTotalPageFile;
        status.dwAvailPageFile = status_ex.ullAvailPageFile;
        status.dwTotalVirtual = status_ex.ullTotalVirtual;
        status.dwAvailVirtual = status_ex.ullAvailVirtual;
        if (hb_memory_write( ctx->memory, (hb_gva_t)args[0], &status, sizeof(status) ) != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        }
        *ret = 0;
        return TRUE;
    }
}

static BOOL macrunner_hb_try_msvcrt_time_semantic( hb_context_t *ctx,
                                                   const struct macrunner_hb_import_thunk *thunk,
                                                   const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
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
                                                   const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                   uint64_t *ret )
{
    BOOL is_crt_runtime;

    if (!ctx || !thunk || !args || !ret) return FALSE;
    is_crt_runtime = macrunner_hb_strieq( thunk->dll_name, "msvcrt.dll" ) ||
                     macrunner_hb_strieq( thunk->dll_name, "ucrtbase.dll" ) ||
                     macrunner_hb_strieq( thunk->dll_name, "api-ms-win-crt-runtime-l1-1-0.dll" );
    if (!is_crt_runtime) return FALSE;

    if (macrunner_hb_strieq( thunk->import_name, "_initterm" ) ||
        macrunner_hb_strieq( thunk->import_name, "_initterm_e" ))
    {
        BOOL stop_on_error = macrunner_hb_strieq( thunk->import_name, "_initterm_e" );
        uint64_t begin = args[0], end = args[1], slot;
        unsigned int count = 0;

        if (!begin || !end || begin > end || end - begin > 0x10000)
        {
            *ret = 0;
            return TRUE;
        }
        for (slot = begin; slot < end; slot += sizeof(uint64_t))
        {
            hb_abi_x64_call_t call;
            ULONG64 callback_ret = 0, blocks = 0, steps = 0;
            uint64_t callback = 0;
            void *module;
            NTSTATUS status;

            if (hb_memory_read_u64( ctx->memory, (hb_gva_t)slot, &callback ) != HB_OK)
                return FALSE;
            if (!callback) continue;
            module = macrunner_hb_module_from_pc( (void *)(uintptr_t)callback );
            if (!module || macrunner_hb_module_machine( module ) != IMAGE_FILE_MACHINE_AMD64)
                continue;

            memset( &call, 0, sizeof(call) );
            status = macrunner_hb_run_x64( (void *)(uintptr_t)callback, &call, &callback_ret,
                                           &blocks, &steps, thunk->import_name, module );
            TRACE( "MacRunner HyperBridge semantic crt!%s callback=%p status=%lx "
                   "ret=%p blocks=%s steps=%s\n",
                   thunk->import_name, (void *)(uintptr_t)callback, (unsigned long)status,
                   (void *)(uintptr_t)callback_ret, wine_dbgstr_longlong(blocks),
                   wine_dbgstr_longlong(steps) );
            if (status) return FALSE;
            count++;
            if (stop_on_error && callback_ret)
            {
                *ret = callback_ret;
                return TRUE;
            }
        }
        *ret = 0;
        TRACE( "MacRunner HyperBridge semantic crt!%s range=%p-%p callbacks=%u ret=%p\n",
               thunk->import_name, (void *)(uintptr_t)begin, (void *)(uintptr_t)end,
               count, (void *)(uintptr_t)*ret );
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "_initialize_onexit_table" ) ||
        macrunner_hb_strieq( thunk->import_name, "_o__initialize_onexit_table" ))
    {
        uint64_t table[3];

        if (!args[0])
        {
            *ret = (uint64_t)-1;
            return TRUE;
        }
        if (hb_memory_read( ctx->memory, (hb_gva_t)args[0], table, sizeof(table) ) != HB_OK)
            return FALSE;
        if (table[0] == table[2])
        {
            memset( table, 0, sizeof(table) );
            if (hb_memory_write( ctx->memory, (hb_gva_t)args[0], table, sizeof(table) ) != HB_OK)
                return FALSE;
        }
        *ret = 0;
        TRACE( "MacRunner HyperBridge semantic crt!%s table=%p ret=%p\n",
               thunk->import_name, (void *)(uintptr_t)args[0], (void *)(uintptr_t)*ret );
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "_register_onexit_function" ) ||
        macrunner_hb_strieq( thunk->import_name, "_o__register_onexit_function" ))
    {
        *ret = args[0] ? 0 : (uint64_t)-1;
        TRACE( "MacRunner HyperBridge semantic crt!%s table=%p func=%p ret=%p\n",
               thunk->import_name, (void *)(uintptr_t)args[0],
               (void *)(uintptr_t)args[1], (void *)(uintptr_t)*ret );
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "_execute_onexit_table" ) ||
        macrunner_hb_strieq( thunk->import_name, "_o__execute_onexit_table" ))
    {
        if (!args[0])
        {
            *ret = (uint64_t)-1;
            return TRUE;
        }
        *ret = 0;
        TRACE( "MacRunner HyperBridge semantic crt!%s table=%p ret=%p\n",
               thunk->import_name, (void *)(uintptr_t)args[0], (void *)(uintptr_t)*ret );
        return TRUE;
    }

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

    if (macrunner_hb_strieq( thunk->import_name, "_cexit" ) ||
        macrunner_hb_strieq( thunk->import_name, "_c_exit" ))
    {
        *ret = 0;
        TRACE( "MacRunner HyperBridge semantic msvcrt!%s ret=%p\n",
               thunk->import_name, (void *)(uintptr_t)*ret );
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "exit" ) ||
        macrunner_hb_strieq( thunk->import_name, "_exit" ) ||
        macrunner_hb_strieq( thunk->import_name, "_Exit" ) ||
        macrunner_hb_strieq( thunk->import_name, "quick_exit" ))
    {
        TRACE( "MacRunner HyperBridge semantic msvcrt!%s code=%lu\n",
               thunk->import_name, (unsigned long)(ULONG)args[0] );
        NtTerminateProcess( GetCurrentProcess(), (LONG)(ULONG)args[0] );
        exit( (int)(ULONG)args[0] );
    }

    return FALSE;
}

static BOOL macrunner_hb_try_crt_environment_init_semantic( hb_context_t *ctx,
                                                            const struct macrunner_hb_import_thunk *thunk,
                                                            const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                            uint64_t *ret )
{
    BOOL is_crt_runtime;
    PEB *peb;
    RTL_USER_PROCESS_PARAMETERS *params;
    const WCHAR *env, *cur;
    size_t count, i;

    if (!ctx || !thunk || !args || !ret) return FALSE;
    is_crt_runtime = macrunner_hb_strieq( thunk->dll_name, "msvcrt.dll" ) ||
                     macrunner_hb_strieq( thunk->dll_name, "ucrtbase.dll" ) ||
                     macrunner_hb_strieq( thunk->dll_name, "api-ms-win-crt-runtime-l1-1-0.dll" );
    if (!is_crt_runtime) return FALSE;

    if (macrunner_hb_strieq( thunk->import_name, "_configure_narrow_argv" ) ||
        macrunner_hb_strieq( thunk->import_name, "_configure_wide_argv" ) ||
        macrunner_hb_strieq( thunk->import_name, "_initialize_narrow_environment" ) ||
        macrunner_hb_strieq( thunk->import_name, "_initialize_wide_environment" ))
    {
        *ret = 0;
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        RtlSetLastWin32Error( ERROR_SUCCESS );
        TRACE( "MacRunner HyperBridge semantic crt environment init %s!%s ret=%p\n",
               thunk->dll_name, thunk->import_name, (void *)(uintptr_t)*ret );
        return TRUE;
    }

    if (!macrunner_hb_strieq( thunk->import_name, "_get_initial_narrow_environment" ) &&
        !macrunner_hb_strieq( thunk->import_name, "_get_initial_wide_environment" ) &&
        !macrunner_hb_strieq( thunk->import_name, "__p__environ" ) &&
        !macrunner_hb_strieq( thunk->import_name, "__p__wenviron" ))
        return FALSE;

    pthread_mutex_lock( &macrunner_hb_initial_env_mutex );
    if (!macrunner_hb_initial_wide_env || !macrunner_hb_initial_narrow_env)
    {
        peb = NtCurrentTeb()->Peb;
        params = peb ? peb->ProcessParameters : NULL;
        env = params ? params->Environment : NULL;
        count = 0;
        for (cur = env; cur && *cur; cur += lstrlenW( cur ) + 1) count++;

        if (count)
        {
            macrunner_hb_initial_wide_env = calloc( count + 1, sizeof(*macrunner_hb_initial_wide_env) );
            macrunner_hb_initial_narrow_env = calloc( count + 1, sizeof(*macrunner_hb_initial_narrow_env) );
        }
        if (!count || !macrunner_hb_initial_wide_env || !macrunner_hb_initial_narrow_env)
        {
            free( macrunner_hb_initial_wide_env );
            free( macrunner_hb_initial_narrow_env );
            macrunner_hb_initial_wide_env = macrunner_hb_empty_wide_env;
            macrunner_hb_initial_narrow_env = macrunner_hb_empty_narrow_env;
        }
        else
        {
            for (i = 0, cur = env; i < count && cur && *cur; i++, cur += lstrlenW( cur ) + 1)
            {
                size_t len = lstrlenW( cur );
                size_t j;
                char *narrow = malloc( len + 1 );

                macrunner_hb_initial_wide_env[i] = (WCHAR *)cur;
                if (!narrow) continue;
                for (j = 0; j < len; j++)
                    narrow[j] = cur[j] && cur[j] < 0x80 ? (char)cur[j] : '?';
                narrow[len] = 0;
                macrunner_hb_initial_narrow_env[i] = narrow;
            }
        }
    }

    if (macrunner_hb_strieq( thunk->import_name, "_get_initial_wide_environment" ))
        *ret = (uint64_t)(uintptr_t)macrunner_hb_initial_wide_env;
    else if (macrunner_hb_strieq( thunk->import_name, "_get_initial_narrow_environment" ))
        *ret = (uint64_t)(uintptr_t)macrunner_hb_initial_narrow_env;
    else if (macrunner_hb_strieq( thunk->import_name, "__p__wenviron" ))
        *ret = (uint64_t)(uintptr_t)&macrunner_hb_initial_wide_env;
    else
        *ret = (uint64_t)(uintptr_t)&macrunner_hb_initial_narrow_env;
    pthread_mutex_unlock( &macrunner_hb_initial_env_mutex );

    NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
    RtlSetLastWin32Error( ERROR_SUCCESS );
    TRACE( "MacRunner HyperBridge semantic crt environment %s!%s ret=%p\n",
           thunk->dll_name, thunk->import_name, (void *)(uintptr_t)*ret );
    return TRUE;
}

static BOOL macrunner_hb_try_crt_locale_semantic( hb_context_t *ctx,
                                                  const struct macrunner_hb_import_thunk *thunk,
                                                  const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                  uint64_t *ret )
{
    BOOL is_crt_locale;

    if (!ctx || !thunk || !args || !ret) return FALSE;
    is_crt_locale = macrunner_hb_strieq( thunk->dll_name, "api-ms-win-crt-locale-l1-1-0.dll" ) ||
                    macrunner_hb_strieq( thunk->dll_name, "ucrtbase.dll" ) ||
                    macrunner_hb_strieq( thunk->dll_name, "msvcrt.dll" );
    if (!is_crt_locale) return FALSE;

    if (macrunner_hb_strieq( thunk->import_name, "_create_locale" ))
    {
        *ret = 0;
        return TRUE;
    }
    if (macrunner_hb_strieq( thunk->import_name, "_free_locale" ))
    {
        *ret = 0;
        return TRUE;
    }
    if (macrunner_hb_strieq( thunk->import_name, "_configthreadlocale" ))
    {
        *ret = 0;
        return TRUE;
    }
    if (macrunner_hb_strieq( thunk->import_name, "setlocale" ))
    {
        static const char c_locale[] = "C";
        *ret = args[1] ? args[1] : (uint64_t)(uintptr_t)c_locale;
        return TRUE;
    }
    if (macrunner_hb_strieq( thunk->import_name, "___mb_cur_max_func" ))
    {
        *ret = 1;
        return TRUE;
    }
    if (macrunner_hb_strieq( thunk->import_name, "___lc_codepage_func" ))
    {
        *ret = 1252;
        return TRUE;
    }
    if (macrunner_hb_strieq( thunk->import_name, "__pctype_func" ))
    {
        static uint16_t *pctype;
        unsigned int ch;

        if (!pctype)
        {
            uint16_t *raw = macrunner_hb_local_heap_alloc( 258 * sizeof(uint16_t), TRUE );

            if (!raw)
            {
                *ret = 0;
                return TRUE;
            }
            pctype = raw + 1;
            for (ch = 0; ch < 256; ch++)
            {
                uint16_t flags = 0;

                if (ch >= 'A' && ch <= 'Z') flags |= 0x0001 | 0x0100;
                if (ch >= 'a' && ch <= 'z') flags |= 0x0002 | 0x0100;
                if (ch >= '0' && ch <= '9') flags |= 0x0004;
                if (ch == ' ' || (ch >= '\t' && ch <= '\r')) flags |= 0x0008;
                if (ch < 0x20 || ch == 0x7f) flags |= 0x0020;
                if (ch == ' ' || ch == '\t') flags |= 0x0040;
                if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') ||
                    (ch >= 'a' && ch <= 'f')) flags |= 0x0080;
                if (ch >= 0x21 && ch <= 0x7e && !(flags & (0x0100 | 0x0004)) && ch != ' ')
                    flags |= 0x0010;
                pctype[ch] = flags;
            }
            macrunner_hb_sync_virtual_region( ctx, raw, 258 * sizeof(uint16_t), PAGE_READWRITE );
        }
        *ret = (uint64_t)(uintptr_t)pctype;
        return TRUE;
    }
    if (macrunner_hb_strieq( thunk->import_name, "localeconv" ))
    {
        static void *lconv;

        if (!lconv)
        {
            BYTE *block = macrunner_hb_local_heap_alloc( 512, TRUE );
            uint64_t dot, empty;

            if (!block)
            {
                *ret = 0;
                return TRUE;
            }
            dot = (uint64_t)(uintptr_t)(block + 256);
            empty = (uint64_t)(uintptr_t)(block + 258);
            *(uint64_t *)(block + 0) = dot;
            *(uint64_t *)(block + 8) = empty;
            *(uint64_t *)(block + 16) = empty;
            block[256] = '.';
            block[257] = 0;
            block[258] = 0;
            lconv = block;
            macrunner_hb_sync_virtual_region( ctx, block, 512, PAGE_READWRITE );
        }
        *ret = (uint64_t)(uintptr_t)lconv;
        return TRUE;
    }
    return FALSE;
}

#define MACRUNNER_HB_CRT_SCAN_LIMIT (16 * 1024 * 1024)

static uint64_t macrunner_hb_ret_i32( int value )
{
    return (uint64_t)(uint32_t)(int32_t)value;
}

static BOOL macrunner_hb_is_crt_string_dll( const char *dll_name )
{
    return macrunner_hb_strieq( dll_name, "api-ms-win-crt-string-l1-1-0.dll" ) ||
           macrunner_hb_strieq( dll_name, "api-ms-win-crt-private-l1-1-0.dll" ) ||
           macrunner_hb_strieq( dll_name, "ucrtbase.dll" ) ||
           macrunner_hb_strieq( dll_name, "msvcrt.dll" );
}

static BOOL macrunner_hb_is_crt_multibyte_dll( const char *dll_name )
{
    return macrunner_hb_strieq( dll_name, "api-ms-win-crt-multibyte-l1-1-0.dll" ) ||
           macrunner_hb_strieq( dll_name, "ucrtbase.dll" ) ||
           macrunner_hb_strieq( dll_name, "msvcrt.dll" );
}

static BOOL macrunner_hb_is_crt_math_dll( const char *dll_name )
{
    return macrunner_hb_strieq( dll_name, "api-ms-win-crt-math-l1-1-0.dll" ) ||
           macrunner_hb_strieq( dll_name, "ucrtbase.dll" ) ||
           macrunner_hb_strieq( dll_name, "msvcrt.dll" );
}

static BOOL macrunner_hb_try_crt_math_semantic( hb_context_t *ctx,
                                                const struct macrunner_hb_import_thunk *thunk,
                                                const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                uint64_t *ret )
{
    uint32_t in_bits, out_bits;
    float in, out;

    (void)args;

    if (!ctx || !thunk || !ret) return FALSE;
    if (!macrunner_hb_is_crt_math_dll( thunk->dll_name ) ||
        !macrunner_hb_strieq( thunk->import_name, "ceilf" ))
        return FALSE;

    in_bits = (uint32_t)ctx->regs.x64.xmm[0][0];
    memcpy( &in, &in_bits, sizeof(in) );
    out = ceilf( in );
    memcpy( &out_bits, &out, sizeof(out_bits) );
    ctx->regs.x64.xmm[0][0] = (ctx->regs.x64.xmm[0][0] & 0xffffffff00000000ULL) | out_bits;
    *ret = 0;
    return TRUE;
}

static BOOL macrunner_hb_try_crt_multibyte_semantic( hb_context_t *ctx,
                                                     const struct macrunner_hb_import_thunk *thunk,
                                                     const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                     uint64_t *ret )
{
    uint8_t ch;

    if (!ctx || !ctx->memory || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_is_crt_multibyte_dll( thunk->dll_name ) ||
        !macrunner_hb_strieq( thunk->import_name, "_mbtowc_l" ))
        return FALSE;

    if (!args[1])
    {
        *ret = 0;
        return TRUE;
    }
    if (!args[2])
    {
        *ret = macrunner_hb_ret_i32( -1 );
        return TRUE;
    }
    if (hb_memory_read_u8( ctx->memory, (hb_gva_t)args[1], &ch ) != HB_OK)
        return FALSE;
    if (!ch)
    {
        if (args[0] && hb_memory_write_u16( ctx->memory, (hb_gva_t)args[0], 0 ) != HB_OK)
            return FALSE;
        *ret = 0;
        return TRUE;
    }
    if (ch >= 0x80)
    {
        *ret = macrunner_hb_ret_i32( -1 );
        return TRUE;
    }
    if (args[0] && hb_memory_write_u16( ctx->memory, (hb_gva_t)args[0], ch ) != HB_OK)
        return FALSE;
    *ret = 1;
    return TRUE;
}

static BOOL macrunner_hb_is_crt_heap_dll( const char *dll_name )
{
    return macrunner_hb_strieq( dll_name, "api-ms-win-crt-heap-l1-1-0.dll" ) ||
           macrunner_hb_strieq( dll_name, "ucrtbase.dll" ) ||
           macrunner_hb_strieq( dll_name, "msvcrt.dll" );
}

static BOOL macrunner_hb_try_crt_heap_semantic( hb_context_t *ctx,
                                                const struct macrunner_hb_import_thunk *thunk,
                                                const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                uint64_t *ret )
{
    const char *name;
    void *ptr;
    SIZE_T size;

    if (!ctx || !ctx->memory || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_is_crt_heap_dll( thunk->dll_name )) return FALSE;
    name = thunk->import_name;

    if (macrunner_hb_strieq( name, "malloc" ))
    {
        size = (SIZE_T)args[0];
        ptr = macrunner_hb_local_heap_alloc( size, FALSE );
        if (ptr) macrunner_hb_sync_virtual_region( ctx, ptr, size ? size : 1, PAGE_READWRITE );
        *ret = (uint64_t)(uintptr_t)ptr;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "calloc" ))
    {
        SIZE_T count = (SIZE_T)args[0], elem = (SIZE_T)args[1];

        if (elem && count > ~(SIZE_T)0 / elem)
        {
            *ret = 0;
            return TRUE;
        }
        size = count * elem;
        ptr = macrunner_hb_local_heap_alloc( size, TRUE );
        if (ptr) macrunner_hb_sync_virtual_region( ctx, ptr, size ? size : 1, PAGE_READWRITE );
        *ret = (uint64_t)(uintptr_t)ptr;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "realloc" ))
    {
        ptr = (void *)(uintptr_t)args[0];
        size = (SIZE_T)args[1];
        if (!ptr || macrunner_hb_local_heap_contains( ptr ))
        {
            ptr = macrunner_hb_local_heap_realloc( ptr, size, FALSE, FALSE );
            if (ptr) macrunner_hb_sync_virtual_region( ctx, ptr, size ? size : 1, PAGE_READWRITE );
            *ret = (uint64_t)(uintptr_t)ptr;
            return TRUE;
        }
        return FALSE;
    }
    if (macrunner_hb_strieq( name, "free" ))
    {
        ptr = (void *)(uintptr_t)args[0];
        if (ptr && macrunner_hb_local_heap_contains( ptr )) macrunner_hb_local_heap_release( ptr );
        *ret = 0;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "_aligned_malloc" ))
    {
        SIZE_T alignment = (SIZE_T)args[1];
        uintptr_t raw_addr, aligned_addr;
        void *raw;

        size = (SIZE_T)args[0];
        if (alignment < sizeof(void *)) alignment = sizeof(void *);
        if (alignment & (alignment - 1)) alignment = 1ULL << (8 * sizeof(SIZE_T) - __builtin_clzl(alignment));
        if (size > ~(SIZE_T)0 - alignment - sizeof(void *))
        {
            *ret = 0;
            return TRUE;
        }
        raw = macrunner_hb_local_heap_alloc( size + alignment + sizeof(void *), FALSE );
        if (!raw)
        {
            *ret = 0;
            return TRUE;
        }
        raw_addr = (uintptr_t)raw + sizeof(void *);
        aligned_addr = (raw_addr + alignment - 1) & ~(uintptr_t)(alignment - 1);
        ((void **)aligned_addr)[-1] = raw;
        macrunner_hb_sync_virtual_region( ctx, raw, size + alignment + sizeof(void *), PAGE_READWRITE );
        *ret = (uint64_t)aligned_addr;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "_aligned_free" ))
    {
        ptr = (void *)(uintptr_t)args[0];
        if (ptr)
        {
            void *raw = ((void **)ptr)[-1];

            if (raw && macrunner_hb_local_heap_contains( raw )) macrunner_hb_local_heap_release( raw );
        }
        *ret = 0;
        return TRUE;
    }

    return FALSE;
}

static BOOL macrunner_hb_guest_strlen_bound( hb_context_t *ctx, uint64_t addr,
                                             size_t max_count, size_t *len )
{
    size_t i;

    if (!ctx || !ctx->memory || !addr || !len) return FALSE;
    for (i = 0; i < max_count; i++)
    {
        uint8_t ch;

        if (hb_memory_read_u8( ctx->memory, (hb_gva_t)addr + i, &ch ) != HB_OK)
            return FALSE;
        if (!ch)
        {
            *len = i;
            return TRUE;
        }
    }
    *len = max_count;
    return TRUE;
}

static BOOL macrunner_hb_guest_wcslen_bound( hb_context_t *ctx, uint64_t addr,
                                             size_t max_count, size_t *len )
{
    size_t i;

    if (!ctx || !ctx->memory || !addr || !len) return FALSE;
    for (i = 0; i < max_count; i++)
    {
        uint16_t ch;

        if (hb_memory_read_u16( ctx->memory, (hb_gva_t)addr + i * sizeof(uint16_t), &ch ) != HB_OK)
            return FALSE;
        if (!ch)
        {
            *len = i;
            return TRUE;
        }
    }
    *len = max_count;
    return TRUE;
}

static BOOL macrunner_hb_guest_strcmp_bound( hb_context_t *ctx, uint64_t left,
                                             uint64_t right, size_t max_count,
                                             BOOL bounded, int *result )
{
    size_t i;

    if (!ctx || !ctx->memory || !left || !right || !result) return FALSE;
    for (i = 0; i < max_count; i++)
    {
        uint8_t a, b;

        if (hb_memory_read_u8( ctx->memory, (hb_gva_t)left + i, &a ) != HB_OK ||
            hb_memory_read_u8( ctx->memory, (hb_gva_t)right + i, &b ) != HB_OK)
            return FALSE;
        if (a != b || !a)
        {
            *result = (int)a - (int)b;
            return TRUE;
        }
    }
    *result = 0;
    return bounded;
}

static BOOL macrunner_hb_guest_wcscmp_bound( hb_context_t *ctx, uint64_t left,
                                             uint64_t right, size_t max_count,
                                             BOOL bounded, int *result )
{
    size_t i;

    if (!ctx || !ctx->memory || !left || !right || !result) return FALSE;
    for (i = 0; i < max_count; i++)
    {
        uint16_t a, b;

        if (hb_memory_read_u16( ctx->memory, (hb_gva_t)left + i * sizeof(uint16_t), &a ) != HB_OK ||
            hb_memory_read_u16( ctx->memory, (hb_gva_t)right + i * sizeof(uint16_t), &b ) != HB_OK)
            return FALSE;
        if (a != b || !a)
        {
            *result = (int)a - (int)b;
            return TRUE;
        }
    }
    *result = 0;
    return bounded;
}

static BOOL macrunner_hb_guest_memmove_bytes( hb_context_t *ctx, uint64_t dst,
                                              uint64_t src, size_t size )
{
    uint8_t buffer[256];

    if (!ctx || !ctx->memory) return FALSE;
    if (!size) return TRUE;
    if (!dst || !src) return FALSE;

    if (dst > src && dst - src < size)
    {
        size_t remaining = size;

        while (remaining)
        {
            size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
            remaining -= chunk;
            if (hb_memory_read( ctx->memory, (hb_gva_t)src + remaining, buffer, chunk ) != HB_OK ||
                hb_memory_write( ctx->memory, (hb_gva_t)dst + remaining, buffer, chunk ) != HB_OK)
                return FALSE;
        }
        return TRUE;
    }

    {
        size_t offset = 0;

        while (offset < size)
        {
            size_t chunk = size - offset;

            if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
            if (hb_memory_read( ctx->memory, (hb_gva_t)src + offset, buffer, chunk ) != HB_OK ||
                hb_memory_write( ctx->memory, (hb_gva_t)dst + offset, buffer, chunk ) != HB_OK)
                return FALSE;
            offset += chunk;
        }
    }
    return TRUE;
}

static BOOL macrunner_hb_guest_memset_bytes( hb_context_t *ctx, uint64_t dst,
                                             uint8_t value, size_t size )
{
    uint8_t buffer[256];
    size_t offset = 0;

    if (!ctx || !ctx->memory) return FALSE;
    if (!size) return TRUE;
    if (!dst) return FALSE;
    memset( buffer, value, sizeof(buffer) );
    while (offset < size)
    {
        size_t chunk = size - offset;

        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        if (hb_memory_write( ctx->memory, (hb_gva_t)dst + offset, buffer, chunk ) != HB_OK)
            return FALSE;
        offset += chunk;
    }
    return TRUE;
}

static BOOL macrunner_hb_guest_memcmp_bytes( hb_context_t *ctx, uint64_t left,
                                             uint64_t right, size_t size, int *result )
{
    uint8_t a[256], b[256];
    size_t offset = 0;

    if (!ctx || !ctx->memory || !result) return FALSE;
    if (!size)
    {
        *result = 0;
        return TRUE;
    }
    if (!left || !right) return FALSE;
    while (offset < size)
    {
        size_t chunk = size - offset;
        size_t i;

        if (chunk > sizeof(a)) chunk = sizeof(a);
        if (hb_memory_read( ctx->memory, (hb_gva_t)left + offset, a, chunk ) != HB_OK ||
            hb_memory_read( ctx->memory, (hb_gva_t)right + offset, b, chunk ) != HB_OK)
            return FALSE;
        for (i = 0; i < chunk; i++)
        {
            if (a[i] != b[i])
            {
                *result = (int)a[i] - (int)b[i];
                return TRUE;
            }
        }
        offset += chunk;
    }
    *result = 0;
    return TRUE;
}

static int macrunner_hb_ascii_tolower_i( int ch )
{
    return (ch >= 'A' && ch <= 'Z') ? ch + ('a' - 'A') : ch;
}

static int macrunner_hb_ascii_toupper_i( int ch )
{
    return (ch >= 'a' && ch <= 'z') ? ch + ('A' - 'a') : ch;
}

static BOOL macrunner_hb_try_crt_string_semantic( hb_context_t *ctx,
                                                  const struct macrunner_hb_import_thunk *thunk,
                                                  const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                  uint64_t *ret )
{
    const char *name;
    size_t len;
    int cmp;

    if (!ctx || !thunk || !args || !ret || !macrunner_hb_is_crt_string_dll( thunk->dll_name ))
        return FALSE;
    name = thunk->import_name;

    if (macrunner_hb_strieq( name, "memset" ))
    {
        if (!macrunner_hb_guest_memset_bytes( ctx, args[0], (uint8_t)args[1], (size_t)args[2] ))
            return FALSE;
        *ret = args[0];
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "memcpy" ) || macrunner_hb_strieq( name, "memmove" ))
    {
        if (!macrunner_hb_guest_memmove_bytes( ctx, args[0], args[1], (size_t)args[2] ))
            return FALSE;
        *ret = args[0];
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "memcmp" ))
    {
        if (!macrunner_hb_guest_memcmp_bytes( ctx, args[0], args[1], (size_t)args[2], &cmp ))
            return FALSE;
        *ret = macrunner_hb_ret_i32( cmp );
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "memchr" ))
    {
        uint8_t needle = (uint8_t)args[1];
        uint8_t buffer[256];
        size_t offset = 0;
        size_t size = (size_t)args[2];

        if (!args[0] && size) return FALSE;
        while (offset < size)
        {
            size_t chunk = size - offset;
            size_t i;

            if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
            if (hb_memory_read( ctx->memory, (hb_gva_t)args[0] + offset, buffer, chunk ) != HB_OK)
                return FALSE;
            for (i = 0; i < chunk; i++)
            {
                if (buffer[i] == needle)
                {
                    *ret = args[0] + offset + i;
                    return TRUE;
                }
            }
            offset += chunk;
        }
        *ret = 0;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "strcmp" ) || macrunner_hb_strieq( name, "_strcoll_l" ))
    {
        if (!macrunner_hb_guest_strcmp_bound( ctx, args[0], args[1],
                                              MACRUNNER_HB_CRT_SCAN_LIMIT, FALSE, &cmp ))
            return FALSE;
        *ret = macrunner_hb_ret_i32( cmp );
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "strncmp" ))
    {
        if (!macrunner_hb_guest_strcmp_bound( ctx, args[0], args[1], (size_t)args[2], TRUE, &cmp ))
            return FALSE;
        *ret = macrunner_hb_ret_i32( cmp );
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "strlen" ))
    {
        if (!macrunner_hb_guest_strlen_bound( ctx, args[0], MACRUNNER_HB_CRT_SCAN_LIMIT, &len ))
            return FALSE;
        *ret = len;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "strnlen" ))
    {
        if (!macrunner_hb_guest_strlen_bound( ctx, args[0], (size_t)args[1], &len ))
            return FALSE;
        *ret = len;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "wcslen" ))
    {
        if (!macrunner_hb_guest_wcslen_bound( ctx, args[0], MACRUNNER_HB_CRT_SCAN_LIMIT, &len ))
            return FALSE;
        *ret = len;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "wcsnlen" ))
    {
        if (!macrunner_hb_guest_wcslen_bound( ctx, args[0], (size_t)args[1], &len ))
            return FALSE;
        *ret = len;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "_wcscoll_l" ))
    {
        if (!macrunner_hb_guest_wcscmp_bound( ctx, args[0], args[1],
                                              MACRUNNER_HB_CRT_SCAN_LIMIT, FALSE, &cmp ))
            return FALSE;
        *ret = macrunner_hb_ret_i32( cmp );
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "_strxfrm_l" ))
    {
        if (!macrunner_hb_guest_strlen_bound( ctx, args[1], MACRUNNER_HB_CRT_SCAN_LIMIT, &len ))
            return FALSE;
        if (args[0] && args[2])
        {
            size_t copy = len < (size_t)args[2] - 1 ? len : (size_t)args[2] - 1;

            if (!macrunner_hb_guest_memmove_bytes( ctx, args[0], args[1], copy ) ||
                hb_memory_write_u8( ctx->memory, (hb_gva_t)args[0] + copy, 0 ) != HB_OK)
                return FALSE;
        }
        *ret = len;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "_wcsxfrm_l" ))
    {
        if (!macrunner_hb_guest_wcslen_bound( ctx, args[1], MACRUNNER_HB_CRT_SCAN_LIMIT, &len ))
            return FALSE;
        if (args[0] && args[2])
        {
            size_t copy = len < (size_t)args[2] - 1 ? len : (size_t)args[2] - 1;

            if (!macrunner_hb_guest_memmove_bytes( ctx, args[0], args[1], copy * sizeof(uint16_t) ) ||
                hb_memory_write_u16( ctx->memory, (hb_gva_t)args[0] + copy * sizeof(uint16_t), 0 ) != HB_OK)
                return FALSE;
        }
        *ret = len;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "_strdup" ))
    {
        void *copy;

        if (!macrunner_hb_guest_strlen_bound( ctx, args[0], MACRUNNER_HB_CRT_SCAN_LIMIT, &len ))
            return FALSE;
        if (!(copy = macrunner_hb_local_heap_alloc( len + 1, FALSE )))
        {
            *ret = 0;
            return TRUE;
        }
        macrunner_hb_sync_virtual_region( ctx, copy, len + 1, PAGE_READWRITE );
        if (hb_memory_read( ctx->memory, (hb_gva_t)args[0], copy, len + 1 ) != HB_OK)
        {
            macrunner_hb_local_heap_release( copy );
            return FALSE;
        }
        *ret = (uint64_t)(uintptr_t)copy;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "mbrlen" ))
    {
        uint8_t ch = 0;

        if (!args[0])
        {
            *ret = 0;
            return TRUE;
        }
        if (!args[1])
        {
            *ret = ~(uint64_t)1;
            return TRUE;
        }
        if (hb_memory_read_u8( ctx->memory, (hb_gva_t)args[0], &ch ) != HB_OK)
            return FALSE;
        *ret = ch ? (ch < 0x80 ? 1 : ~(uint64_t)0) : 0;
        return TRUE;
    }

    if (macrunner_hb_strieq( name, "isdigit" ) ||
        macrunner_hb_strieq( name, "isspace" ) ||
        macrunner_hb_strieq( name, "isxdigit" ) ||
        macrunner_hb_strieq( name, "tolower" ) ||
        macrunner_hb_strieq( name, "toupper" ) ||
        macrunner_hb_strieq( name, "_tolower_l" ) ||
        macrunner_hb_strieq( name, "_toupper_l" ))
    {
        int ch = (int)(int32_t)(uint32_t)args[0];
        int valid = ch >= 0 && ch <= 255;
        int value = 0;

        if (macrunner_hb_strieq( name, "tolower" ) || macrunner_hb_strieq( name, "_tolower_l" ))
            value = valid ? macrunner_hb_ascii_tolower_i( ch ) : ch;
        else if (macrunner_hb_strieq( name, "toupper" ) || macrunner_hb_strieq( name, "_toupper_l" ))
            value = valid ? macrunner_hb_ascii_toupper_i( ch ) : ch;
        else if (valid && macrunner_hb_strieq( name, "isdigit" ))
            value = ch >= '0' && ch <= '9';
        else if (valid && macrunner_hb_strieq( name, "isspace" ))
            value = ch == ' ' || (ch >= '\t' && ch <= '\r');
        else if (valid && macrunner_hb_strieq( name, "isxdigit" ))
            value = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
        *ret = macrunner_hb_ret_i32( value );
        return TRUE;
    }

    if (macrunner_hb_stristarts( name, "_isw" ) ||
        macrunner_hb_strieq( name, "_towlower_l" ) ||
        macrunner_hb_strieq( name, "_towupper_l" ))
    {
        uint32_t ch = (uint32_t)args[0];
        int value = 0;

        if (macrunner_hb_strieq( name, "_towlower_l" ))
            value = (int)((ch >= 'A' && ch <= 'Z') ? ch + ('a' - 'A') : ch);
        else if (macrunner_hb_strieq( name, "_towupper_l" ))
            value = (int)((ch >= 'a' && ch <= 'z') ? ch + ('A' - 'a') : ch);
        else if (macrunner_hb_strieq( name, "_iswalpha_l" ))
            value = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        else if (macrunner_hb_strieq( name, "_iswcntrl_l" ))
            value = ch < 0x20 || ch == 0x7f;
        else if (macrunner_hb_strieq( name, "_iswdigit_l" ))
            value = ch >= '0' && ch <= '9';
        else if (macrunner_hb_strieq( name, "_iswlower_l" ))
            value = ch >= 'a' && ch <= 'z';
        else if (macrunner_hb_strieq( name, "_iswprint_l" ))
            value = ch >= 0x20 && ch != 0x7f;
        else if (macrunner_hb_strieq( name, "_iswpunct_l" ))
            value = ch < 0x80 && ch >= 0x21 && ch <= 0x7e &&
                    !((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == ' ');
        else if (macrunner_hb_strieq( name, "_iswspace_l" ))
            value = ch == ' ' || (ch >= '\t' && ch <= '\r');
        else if (macrunner_hb_strieq( name, "_iswupper_l" ))
            value = ch >= 'A' && ch <= 'Z';
        else if (macrunner_hb_strieq( name, "_iswxdigit_l" ))
            value = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
        else
            return FALSE;
        *ret = macrunner_hb_ret_i32( value );
        return TRUE;
    }

    return FALSE;
}

static uint64_t macrunner_hb_unhandled_exception_filter;

static BOOL macrunner_hb_try_unhandled_exception_filter_semantic( hb_context_t *ctx,
                                                                  const struct macrunner_hb_import_thunk *thunk,
                                                                  const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                                  uint64_t *ret )
{
    uint64_t old_filter;

    if (!ctx || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_strieq( thunk->import_name, "SetUnhandledExceptionFilter" ))
        return FALSE;
    if (!macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) &&
        !macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" ))
        return FALSE;

    /*
     * The x64 app registers an x64 callback.  Native ARM64 kernelbase cannot
     * call it directly, so keep the guest pointer on the x64 side and return
     * the previous guest filter just like the Win32 API contract requires.
     */
    old_filter = macrunner_hb_unhandled_exception_filter;
    macrunner_hb_unhandled_exception_filter = args[0];
    *ret = old_filter;
    TRACE( "MacRunner HyperBridge semantic %s!SetUnhandledExceptionFilter filter=%p old=%p\n",
           thunk->dll_name, (void *)(uintptr_t)args[0], (void *)(uintptr_t)*ret );
    return TRUE;
}

static BOOL macrunner_hb_try_kernel32_stdio_semantic( hb_context_t *ctx,
                                                     const struct macrunner_hb_import_thunk *thunk,
                                                     const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                     uint64_t *ret )
{
    RTL_USER_PROCESS_PARAMETERS *params;
    IO_STATUS_BLOCK iosb;
    SIZE_T written_size = 0;
    NTSTATUS status;
    DWORD written = 0;

    if (!ctx || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) &&
        !macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" ))
        return FALSE;

    if (macrunner_hb_strieq( thunk->import_name, "GetStdHandle" ))
    {
        params = NtCurrentTeb()->Peb->ProcessParameters;
        switch ((DWORD)args[0])
        {
        case STD_INPUT_HANDLE:
            *ret = (uint64_t)(uintptr_t)(params ? params->hStdInput : NULL);
            break;
        case STD_OUTPUT_HANDLE:
            *ret = (uint64_t)(uintptr_t)(params ? params->hStdOutput : NULL);
            break;
        case STD_ERROR_HANDLE:
            *ret = (uint64_t)(uintptr_t)(params ? params->hStdError : NULL);
            break;
        default:
            RtlSetLastWin32Error( ERROR_INVALID_HANDLE );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_HANDLE;
            *ret = 0;
            break;
        }
        TRACE( "MacRunner HyperBridge semantic %s!GetStdHandle id=%#x ret=%p\n",
               thunk->dll_name, (unsigned int)(DWORD)args[0], (void *)(uintptr_t)*ret );
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetFileType" ))
    {
        FILE_FS_DEVICE_INFORMATION info;
        HANDLE handle = (HANDLE)(uintptr_t)args[0];

        if (macrunner_hb_local_file_fd( args[0] ) >= 0)
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
            *ret = FILE_TYPE_DISK;
            return TRUE;
        }

        params = NtCurrentTeb()->Peb->ProcessParameters;
        if ((DWORD)args[0] == STD_INPUT_HANDLE)
            handle = params ? params->hStdInput : NULL;
        else if ((DWORD)args[0] == STD_OUTPUT_HANDLE)
            handle = params ? params->hStdOutput : NULL;
        else if ((DWORD)args[0] == STD_ERROR_HANDLE)
            handle = params ? params->hStdError : NULL;

        status = NtQueryVolumeInformationFile( handle, &iosb, &info, sizeof(info),
                                               FileFsDeviceInformation );
        NtCurrentTeb()->LastStatusValue = status;
        if (status)
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = FILE_TYPE_UNKNOWN;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            switch (info.DeviceType)
            {
            case FILE_DEVICE_NULL:
            case FILE_DEVICE_CONSOLE:
            case FILE_DEVICE_SERIAL_PORT:
            case FILE_DEVICE_PARALLEL_PORT:
            case FILE_DEVICE_TAPE:
            case FILE_DEVICE_UNKNOWN:
                *ret = FILE_TYPE_CHAR;
                break;
            case FILE_DEVICE_NAMED_PIPE:
                *ret = FILE_TYPE_PIPE;
                break;
            default:
                *ret = FILE_TYPE_DISK;
                break;
            }
        }
        TRACE( "MacRunner HyperBridge semantic %s!GetFileType handle=%p ret=%p status=%08lx\n",
               thunk->dll_name, handle, (void *)(uintptr_t)*ret, (unsigned long)status );
        return TRUE;
    }

    if (!macrunner_hb_strieq( thunk->import_name, "WriteFile" )) return FALSE;
    if (macrunner_hb_local_file_fd( args[0] ) >= 0)
    {
        int fd = macrunner_hb_local_file_fd( args[0] );
        DWORD request = (DWORD)args[2], done32 = 0;
        char *buffer;
        ssize_t done;

        if (!(buffer = malloc( request ? request : 1 )))
        {
            RtlSetLastWin32Error( ERROR_NOT_ENOUGH_MEMORY );
            NtCurrentTeb()->LastStatusValue = STATUS_NO_MEMORY;
            *ret = FALSE;
            return TRUE;
        }
        if (request && hb_memory_read( ctx->memory, (hb_gva_t)args[1], buffer, request ) != HB_OK)
        {
            free( buffer );
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = FALSE;
            return TRUE;
        }
        done = write( fd, buffer, request );
        free( buffer );
        if (done < 0)
        {
            macrunner_hb_set_errno_error();
            if (args[3]) hb_memory_write_u32( ctx->memory, (hb_gva_t)args[3], 0 );
            *ret = FALSE;
            return TRUE;
        }
        done32 = (DWORD)done;
        if (args[3]) hb_memory_write_u32( ctx->memory, (hb_gva_t)args[3], done32 );
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = TRUE;
        return TRUE;
    }
    if (!args[0] || args[0] == (uint64_t)(uintptr_t)INVALID_HANDLE_VALUE)
    {
        status = STATUS_INVALID_HANDLE;
        if (args[3])
            NtWriteVirtualMemory( GetCurrentProcess(), (void *)(uintptr_t)args[3],
                                  &written, sizeof(written), &written_size );
        RtlSetLastWin32Error( ERROR_INVALID_HANDLE );
        NtCurrentTeb()->LastStatusValue = status;
        *ret = FALSE;
        TRACE( "MacRunner HyperBridge semantic %s!WriteFile invalid handle=%p bytes=%lu ret=%p written=%lu status=%08lx\n",
               thunk->dll_name, (void *)(uintptr_t)args[0], (unsigned long)(ULONG)args[2],
               (void *)(uintptr_t)*ret, (unsigned long)written, (unsigned long)status );
        return TRUE;
    }
    if (args[4])
    {
        RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
        NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
        *ret = FALSE;
        return TRUE;
    }

    status = NtWriteFile( (HANDLE)(uintptr_t)args[0], NULL, NULL, NULL, &iosb,
                          (void *)(uintptr_t)args[1], (ULONG)args[2], NULL, NULL );
    if (!status)
        written = (DWORD)iosb.Information;
    else
        RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
    NtCurrentTeb()->LastStatusValue = status;
    if (args[3] && (status = NtWriteVirtualMemory( GetCurrentProcess(), (void *)(uintptr_t)args[3],
                                                   &written, sizeof(written), &written_size )))
    {
        RtlSetLastWin32Error( ERROR_NOACCESS );
        NtCurrentTeb()->LastStatusValue = status;
        *ret = FALSE;
        return TRUE;
    }
    *ret = !status;
    TRACE( "MacRunner HyperBridge semantic %s!WriteFile handle=%p bytes=%lu ret=%p written=%lu status=%08lx\n",
           thunk->dll_name, (void *)(uintptr_t)args[0], (unsigned long)(ULONG)args[2],
           (void *)(uintptr_t)*ret, (unsigned long)written, (unsigned long)status );
    return TRUE;
}

static BOOL macrunner_hb_try_ntdll_memory_semantic( hb_context_t *ctx,
                                                    const struct macrunner_hb_import_thunk *thunk,
                                                    const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                    uint64_t *ret )
{
    NTSTATUS status;
    MEMORY_INFORMATION_CLASS info_class;
    PVOID buffer;
    SIZE_T length;

    if (!ctx || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_strieq( thunk->dll_name, "ntdll.dll" ) &&
        !macrunner_hb_strieq( thunk->dll_name, "ntdll" ))
        return FALSE;
    if (!macrunner_hb_strieq( thunk->import_name, "NtQueryVirtualMemory" ))
        return FALSE;

    info_class = (MEMORY_INFORMATION_CLASS)args[2];
    buffer = (PVOID)(uintptr_t)args[3];
    length = (SIZE_T)args[4];
    status = NtQueryVirtualMemory( (HANDLE)(uintptr_t)args[0],
                                   (LPCVOID)(uintptr_t)args[1],
                                   info_class, buffer, length,
                                   (SIZE_T *)(uintptr_t)args[5] );
    if (status && info_class == MemoryWineUnixFuncs)
    {
        static void *winemetal_unix_handle;
        static const unixlib_entry_t *winemetal_unix_funcs;
        char module_name[96];
        const char *dxmt_root;
        const char *project_root;
        char path[4096];

        macrunner_hb_get_export_module_name( (void *)(uintptr_t)args[1], module_name, sizeof(module_name) );
        if (macrunner_hb_strieq( module_name, "winemetal.dll" ))
        {
            if (length != sizeof(unixlib_handle_t))
            {
                status = STATUS_INFO_LENGTH_MISMATCH;
            }
            else
            {
                if (!winemetal_unix_funcs)
                {
                    dxmt_root = getenv( "MACRUNNER_DXMT_ROOT" );
                    project_root = getenv( "MACRUNNER_ROOT" );
                    if (dxmt_root && *dxmt_root)
                        snprintf( path, sizeof(path), "%s/aarch64-unix/winemetal.so", dxmt_root );
                    else if (project_root && *project_root)
                        snprintf( path, sizeof(path), "%s/engine/graphics/dist/dxmt/aarch64-unix/winemetal.so",
                                  project_root );
                    else
                        path[0] = 0;
                    if (path[0])
                        winemetal_unix_handle = dlopen( path, RTLD_NOW );
                    if (winemetal_unix_handle)
                        winemetal_unix_funcs = dlsym( winemetal_unix_handle, "__wine_unix_call_funcs" );
                }
                if (winemetal_unix_funcs)
                {
                    *(unixlib_handle_t *)buffer = (UINT_PTR)winemetal_unix_funcs;
                    status = STATUS_SUCCESS;
                    if (macrunner_hb_trace_thread_lifecycle_enabled())
                        fprintf( stderr, "macrunner-hb-dxmt-unixlib-bridge: module=%s funcs=%p\n",
                                 module_name, winemetal_unix_funcs );
                }
            }
        }
    }
    NtCurrentTeb()->LastStatusValue = status;
    *ret = status;
    return TRUE;
}

static BOOL macrunner_hb_is_crt_stdio_dll( const char *dll_name )
{
    return macrunner_hb_strieq( dll_name, "ucrtbase.dll" ) ||
           macrunner_hb_strieq( dll_name, "api-ms-win-crt-stdio-l1-1-0.dll" ) ||
           macrunner_hb_strieq( dll_name, "msvcrt.dll" );
}

static int macrunner_hb_crt_stdio_open_flags( const char *mode )
{
    BOOL plus = mode && strchr( mode, '+' );

    if (!mode || !mode[0]) return O_RDONLY;
    if (mode[0] == 'w') return (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
    if (mode[0] == 'a') return (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
    return plus ? O_RDWR : O_RDONLY;
}

static BOOL macrunner_hb_try_crt_stdio_semantic( hb_context_t *ctx,
                                                 const struct macrunner_hb_import_thunk *thunk,
                                                 const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                 uint64_t *ret )
{
    static void *iob_table;
    const size_t iob_size = 0x100;
    const char *name;
    unsigned int index;

    if (!ctx || !ctx->memory || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_is_crt_stdio_dll( thunk->dll_name )) return FALSE;
    name = thunk->import_name;

    if (macrunner_hb_strieq( name, "__acrt_iob_func" ))
    {
        index = (unsigned int)args[0];
        if (index > 2) index = 2;
        if (!iob_table)
        {
            iob_table = macrunner_hb_local_heap_alloc( iob_size * 3, TRUE );
            if (iob_table) macrunner_hb_sync_virtual_region( ctx, iob_table, iob_size * 3, PAGE_READWRITE );
        }
        *ret = iob_table ? (uint64_t)(uintptr_t)((BYTE *)iob_table + index * iob_size) : 0;
        return TRUE;
    }

    if (macrunner_hb_strieq( name, "fopen" ) || macrunner_hb_strieq( name, "_wfopen" ))
    {
        char path[4096], mode[32], unix_path[4096];
        BOOL wide = macrunner_hb_strieq( name, "_wfopen" );
        int fd, flags;
        uint64_t handle;

        if (!macrunner_hb_read_guest_path( ctx, args[0], !wide, path, sizeof(path) ) ||
            !(wide ? macrunner_hb_read_guest_wstr( ctx, args[1], mode, sizeof(mode) ) :
                     macrunner_hb_read_guest_astr( ctx, args[1], mode, sizeof(mode) )))
        {
            *ret = 0;
            return TRUE;
        }
        macrunner_hb_normalize_unix_path( path, unix_path, sizeof(unix_path) );
        flags = macrunner_hb_crt_stdio_open_flags( mode );
        fd = open( unix_path, flags, 0666 );
        if (fd < 0)
        {
            *ret = 0;
            return TRUE;
        }
        if (!(handle = macrunner_hb_local_file_remember( fd )))
        {
            close( fd );
            *ret = 0;
            return TRUE;
        }
        *ret = handle;
        return TRUE;
    }

    if (macrunner_hb_strieq( name, "fclose" ))
    {
        *ret = macrunner_hb_local_file_close( args[0] ) ? 0 : EOF;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "fflush" ))
    {
        int fd = macrunner_hb_local_file_fd( args[0] );

        if (fd >= 0) fsync( fd );
        *ret = 0;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "_fileno" ))
    {
        int fd = macrunner_hb_local_file_fd( args[0] );

        if (fd < 0 && iob_table && args[0] >= (uint64_t)(uintptr_t)iob_table &&
            args[0] < (uint64_t)(uintptr_t)((BYTE *)iob_table + iob_size * 3))
            fd = (int)((args[0] - (uint64_t)(uintptr_t)iob_table) / iob_size);
        *ret = macrunner_hb_ret_i32( fd >= 0 ? fd : -1 );
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "_setmode" ))
    {
        *ret = macrunner_hb_ret_i32( 0 );
        return TRUE;
    }

    if (macrunner_hb_strieq( name, "fwrite" ))
    {
        int fd = macrunner_hb_local_file_fd( args[3] );
        size_t size = (size_t)args[1], count = (size_t)args[2], total, done = 0;
        uint8_t buffer[4096];

        if (fd < 0 || !size || !count || count > ~(size_t)0 / size)
        {
            *ret = 0;
            return TRUE;
        }
        total = size * count;
        while (done < total)
        {
            size_t chunk = total - done;
            ssize_t wrote;

            if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
            if (hb_memory_read( ctx->memory, (hb_gva_t)args[0] + done, buffer, chunk ) != HB_OK)
                break;
            wrote = write( fd, buffer, chunk );
            if (wrote <= 0) break;
            done += (size_t)wrote;
            if ((size_t)wrote < chunk) break;
        }
        *ret = done / size;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "fread" ))
    {
        int fd = macrunner_hb_local_file_fd( args[3] );
        size_t size = (size_t)args[1], count = (size_t)args[2], total, done = 0;
        uint8_t buffer[4096];

        if (fd < 0 || !size || !count || count > ~(size_t)0 / size)
        {
            *ret = 0;
            return TRUE;
        }
        total = size * count;
        while (done < total)
        {
            size_t chunk = total - done;
            ssize_t got;

            if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
            got = read( fd, buffer, chunk );
            if (got <= 0) break;
            if (hb_memory_write( ctx->memory, (hb_gva_t)args[0] + done, buffer, (size_t)got ) != HB_OK)
                break;
            done += (size_t)got;
            if ((size_t)got < chunk) break;
        }
        *ret = done / size;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "fseek" ) || macrunner_hb_strieq( name, "_fseeki64" ))
    {
        int fd = macrunner_hb_local_file_fd( args[0] );
        off_t off = (off_t)args[1];

        *ret = (fd >= 0 && lseek( fd, off, (int)args[2] ) >= 0) ? 0 : EOF;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "_ftelli64" ))
    {
        int fd = macrunner_hb_local_file_fd( args[0] );
        off_t pos = fd >= 0 ? lseek( fd, 0, SEEK_CUR ) : (off_t)-1;

        *ret = (uint64_t)pos;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "fputc" ) || macrunner_hb_strieq( name, "fputwc" ))
    {
        int fd = macrunner_hb_local_file_fd( args[1] );
        uint8_t ch = (uint8_t)args[0];

        *ret = (fd >= 0 && write( fd, &ch, 1 ) == 1) ? (uint32_t)ch : (uint32_t)EOF;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "getc" ) || macrunner_hb_strieq( name, "fgetwc" ))
    {
        int fd = macrunner_hb_local_file_fd( args[0] );
        uint8_t ch = 0;

        *ret = (fd >= 0 && read( fd, &ch, 1 ) == 1) ? ch : (uint32_t)EOF;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "ungetc" ) || macrunner_hb_strieq( name, "ungetwc" ))
    {
        int fd = macrunner_hb_local_file_fd( args[1] );

        if (fd >= 0) lseek( fd, -1, SEEK_CUR );
        *ret = (uint32_t)args[0];
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "setbuf" ))
    {
        *ret = 0;
        return TRUE;
    }

    return FALSE;
}

static void macrunner_hb_render_append_char( char *out, size_t out_size,
                                             size_t *pos, char ch )
{
    if (!out || !out_size || !pos) return;
    if (*pos + 1 < out_size) out[*pos] = ch;
    (*pos)++;
}

static void macrunner_hb_render_append_str( char *out, size_t out_size,
                                            size_t *pos, const char *str )
{
    if (!str) str = "";
    while (*str) macrunner_hb_render_append_char( out, out_size, pos, *str++ );
}

static BOOL macrunner_hb_read_x64_va_arg( hb_context_t *ctx, uint64_t va_list,
                                          unsigned int *index, uint64_t *value )
{
    if (!ctx || !ctx->memory || !index || !value || !va_list) return FALSE;
    if (hb_memory_read_u64( ctx->memory, (hb_gva_t)va_list + (uint64_t)(*index) * 8, value ) != HB_OK)
        return FALSE;
    (*index)++;
    return TRUE;
}

static BOOL macrunner_hb_try_crt_vfprintf_semantic( hb_context_t *ctx,
                                                    const struct macrunner_hb_import_thunk *thunk,
                                                    const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                    uint64_t *ret )
{
    char format[512];
    char rendered[2048];
    size_t pos = 0;
    unsigned int arg_index = 0;
    const char *p;

    if (!ctx || !ctx->memory || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_is_crt_stdio_dll( thunk->dll_name ) ||
        (!macrunner_hb_strieq( thunk->import_name, "__stdio_common_vfprintf" ) &&
         !macrunner_hb_strieq( thunk->import_name, "__stdio_common_vfwprintf" )))
        return FALSE;
    if (macrunner_hb_strieq( thunk->import_name, "__stdio_common_vfwprintf" ))
    {
        *ret = 0;
        return TRUE;
    }
    if (!macrunner_hb_read_guest_astr( ctx, args[2], format, sizeof(format) ))
    {
        *ret = -1;
        return TRUE;
    }

    for (p = format; *p; p++)
    {
        uint64_t raw = 0;
        char number[64];
        BOOL long_arg = FALSE;

        if (*p != '%')
        {
            macrunner_hb_render_append_char( rendered, sizeof(rendered), &pos, *p );
            continue;
        }
        p++;
        if (*p == '%')
        {
            macrunner_hb_render_append_char( rendered, sizeof(rendered), &pos, '%' );
            continue;
        }
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') p++;
        if (*p == '*')
        {
            macrunner_hb_read_x64_va_arg( ctx, args[4], &arg_index, &raw );
            p++;
        }
        else while (*p >= '0' && *p <= '9') p++;
        if (*p == '.')
        {
            p++;
            if (*p == '*')
            {
                macrunner_hb_read_x64_va_arg( ctx, args[4], &arg_index, &raw );
                p++;
            }
            else while (*p >= '0' && *p <= '9') p++;
        }
        if (*p == 'l')
        {
            long_arg = TRUE;
            p++;
            if (*p == 'l') p++;
        }
        else if (*p == 'I' && p[1] == '6' && p[2] == '4')
        {
            long_arg = TRUE;
            p += 3;
        }
        else if (*p == 'z' || *p == 't')
        {
            long_arg = TRUE;
            p++;
        }

        switch (*p)
        {
        case 's':
        {
            char text[512];
            if (!macrunner_hb_read_x64_va_arg( ctx, args[4], &arg_index, &raw ) || !raw ||
                !macrunner_hb_read_guest_astr( ctx, raw, text, sizeof(text) ))
                macrunner_hb_render_append_str( rendered, sizeof(rendered), &pos, "(null)" );
            else
                macrunner_hb_render_append_str( rendered, sizeof(rendered), &pos, text );
            break;
        }
        case 'd':
        case 'i':
            if (!macrunner_hb_read_x64_va_arg( ctx, args[4], &arg_index, &raw )) raw = 0;
            snprintf( number, sizeof(number), "%lld",
                      long_arg ? (long long)raw : (long long)(int32_t)raw );
            macrunner_hb_render_append_str( rendered, sizeof(rendered), &pos, number );
            break;
        case 'u':
            if (!macrunner_hb_read_x64_va_arg( ctx, args[4], &arg_index, &raw )) raw = 0;
            snprintf( number, sizeof(number), "%llu",
                      long_arg ? (unsigned long long)raw : (unsigned long long)(uint32_t)raw );
            macrunner_hb_render_append_str( rendered, sizeof(rendered), &pos, number );
            break;
        case 'x':
        case 'X':
            if (!macrunner_hb_read_x64_va_arg( ctx, args[4], &arg_index, &raw )) raw = 0;
            snprintf( number, sizeof(number), *p == 'X' ? "%llX" : "%llx",
                      long_arg ? (unsigned long long)raw : (unsigned long long)(uint32_t)raw );
            macrunner_hb_render_append_str( rendered, sizeof(rendered), &pos, number );
            break;
        case 'p':
            if (!macrunner_hb_read_x64_va_arg( ctx, args[4], &arg_index, &raw )) raw = 0;
            snprintf( number, sizeof(number), "%p", (void *)(uintptr_t)raw );
            macrunner_hb_render_append_str( rendered, sizeof(rendered), &pos, number );
            break;
        case 'c':
            if (!macrunner_hb_read_x64_va_arg( ctx, args[4], &arg_index, &raw )) raw = 0;
            macrunner_hb_render_append_char( rendered, sizeof(rendered), &pos, (char)raw );
            break;
        default:
            macrunner_hb_render_append_char( rendered, sizeof(rendered), &pos, '%' );
            if (*p) macrunner_hb_render_append_char( rendered, sizeof(rendered), &pos, *p );
            break;
        }
    }

    if (sizeof(rendered)) rendered[pos < sizeof(rendered) ? pos : sizeof(rendered) - 1] = 0;
    fwrite( rendered, 1, pos < sizeof(rendered) ? pos : sizeof(rendered) - 1, stdout );
    fflush( stdout );
    RtlSetLastWin32Error( ERROR_SUCCESS );
    NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
    *ret = pos;
    return TRUE;
}

static BOOL macrunner_hb_try_thread_creation_semantic( hb_context_t *ctx,
                                                       const struct macrunner_hb_import_thunk *thunk,
                                                       const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                       uint64_t *ret )
{
    ULONG_PTR buffer[offsetof( PS_ATTRIBUTE_LIST, Attributes[2] ) / sizeof(ULONG_PTR)];
    PS_ATTRIBUTE_LIST *attr_list = (PS_ATTRIBUTE_LIST *)buffer;
    SECURITY_ATTRIBUTES sa_copy, *sa = NULL;
    OBJECT_ATTRIBUTES attr;
    CLIENT_ID client_id;
    HANDLE process = GetCurrentProcess();
    HANDLE handle = NULL;
    TEB *teb = NULL;
    uint64_t sa_guest = 0, start = 0, param = 0, id_guest = 0;
    SIZE_T stack = 0, stack_commit = 0, stack_reserve = 0, written_size = 0;
    DWORD flags = 0, tid = 0;
    NTSTATUS status;

    if (!ctx || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) &&
        !macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" ))
        return FALSE;

    if (macrunner_hb_strieq( thunk->import_name, "SetThreadDescription" ))
    {
        THREAD_NAME_INFORMATION info;
        const WCHAR *description = (const WCHAR *)(uintptr_t)args[1];
        USHORT length = 0;
        NTSTATUS name_status = STATUS_SUCCESS;

        if (description)
        {
            const WCHAR *p = description;
            while (*p)
            {
                if (length > USHRT_MAX - sizeof(WCHAR))
                {
                    NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
                    *ret = HRESULT_FROM_NT( STATUS_INVALID_PARAMETER );
                    return TRUE;
                }
                length += sizeof(WCHAR);
                p++;
            }
        }

        info.ThreadName.Length = length;
        info.ThreadName.MaximumLength = length;
        info.ThreadName.Buffer = (WCHAR *)description;
        name_status = NtSetInformationThread( (HANDLE)(uintptr_t)args[0], ThreadNameInformation,
                                              &info, sizeof(info) );
        NtCurrentTeb()->LastStatusValue = name_status;
        *ret = name_status ? HRESULT_FROM_NT( name_status ) : S_OK;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "ResumeThread" ) ||
        macrunner_hb_strieq( thunk->import_name, "SuspendThread" ))
    {
        ULONG previous = 0;

        if (macrunner_hb_strieq( thunk->import_name, "ResumeThread" ))
            status = NtResumeThread( (HANDLE)(uintptr_t)args[0], &previous );
        else
            status = NtSuspendThread( (HANDLE)(uintptr_t)args[0], &previous );

        NtCurrentTeb()->LastStatusValue = status;
        if (status)
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = ~0u;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            *ret = previous;
        }
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            fprintf( stderr, "macrunner-hb-wait-semantic: thread-state import=%s!%s pc=%p rsp=%p "
                     "handle=%p status=%08lx previous=%lu ret=%p last_error=%lu\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)ctx->regs.x64.rsp, (void *)(uintptr_t)args[0],
                     (unsigned long)status, (unsigned long)previous, (void *)(uintptr_t)*ret,
                     (unsigned long)NtCurrentTeb()->LastErrorValue );
            fflush( stderr );
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "CreateThread" ))
    {
        sa_guest = args[0];
        stack = (SIZE_T)args[1];
        start = args[2];
        param = args[3];
        flags = (DWORD)args[4];
        id_guest = args[5];
    }
    else if (macrunner_hb_strieq( thunk->import_name, "CreateRemoteThread" ))
    {
        process = (HANDLE)(uintptr_t)args[0];
        sa_guest = args[1];
        stack = (SIZE_T)args[2];
        start = args[3];
        param = args[4];
        flags = (DWORD)args[5];
        id_guest = args[6];
    }
    else if (macrunner_hb_strieq( thunk->import_name, "CreateRemoteThreadEx" ))
    {
        if (args[6])
        {
            RtlSetLastWin32Error( ERROR_CALL_NOT_IMPLEMENTED );
            NtCurrentTeb()->LastStatusValue = STATUS_NOT_IMPLEMENTED;
            *ret = 0;
            return TRUE;
        }
        process = (HANDLE)(uintptr_t)args[0];
        sa_guest = args[1];
        stack = (SIZE_T)args[2];
        start = args[3];
        param = args[4];
        flags = (DWORD)args[5];
        id_guest = args[7];
    }
    else return FALSE;

    if (!start)
    {
        RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
        NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
        *ret = 0;
        return TRUE;
    }

    if (process != GetCurrentProcess())
    {
        RtlSetLastWin32Error( ERROR_CALL_NOT_IMPLEMENTED );
        NtCurrentTeb()->LastStatusValue = STATUS_NOT_IMPLEMENTED;
        *ret = 0;
        return TRUE;
    }

    if (sa_guest)
    {
        if (hb_memory_read( ctx->memory, (hb_gva_t)sa_guest, &sa_copy, sizeof(sa_copy) ) != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = 0;
            return TRUE;
        }
        sa = &sa_copy;
    }

    if (flags & STACK_SIZE_PARAM_IS_A_RESERVATION) stack_reserve = stack;
    else stack_commit = stack;

    attr_list->Attributes[0].Attribute    = PS_ATTRIBUTE_CLIENT_ID;
    attr_list->Attributes[0].Size         = sizeof(client_id);
    attr_list->Attributes[0].ValuePtr     = &client_id;
    attr_list->Attributes[0].ReturnLength = NULL;
    attr_list->Attributes[1].Attribute    = PS_ATTRIBUTE_TEB_ADDRESS;
    attr_list->Attributes[1].Size         = sizeof(teb);
    attr_list->Attributes[1].ValuePtr     = &teb;
    attr_list->Attributes[1].ReturnLength = NULL;
    attr_list->TotalLength = offsetof( PS_ATTRIBUTE_LIST, Attributes[2] );

    InitializeObjectAttributes( &attr, NULL, 0, NULL, sa ? sa->lpSecurityDescriptor : NULL );
    if (sa && sa->bInheritHandle) attr.Attributes |= OBJ_INHERIT;

    status = NtCreateThreadEx( &handle, THREAD_ALL_ACCESS, &attr, process,
                               (PRTL_THREAD_START_ROUTINE)(uintptr_t)start,
                               (void *)(uintptr_t)param, THREAD_CREATE_FLAGS_CREATE_SUSPENDED,
                               0, stack_commit, stack_reserve, attr_list );
    NtCurrentTeb()->LastStatusValue = status;
    if (status)
    {
        RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
        *ret = 0;
        return TRUE;
    }

    if (id_guest)
    {
        tid = (DWORD)(ULONG_PTR)client_id.UniqueThread;
        status = NtWriteVirtualMemory( GetCurrentProcess(), (void *)(uintptr_t)id_guest,
                                       &tid, sizeof(tid), &written_size );
        NtCurrentTeb()->LastStatusValue = status;
        if (status)
        {
            NtClose( handle );
            RtlSetLastWin32Error( ERROR_NOACCESS );
            *ret = 0;
            return TRUE;
        }
    }

    if (!(flags & CREATE_SUSPENDED))
    {
        status = NtResumeThread( handle, NULL );
        NtCurrentTeb()->LastStatusValue = status;
        if (status)
        {
            NtClose( handle );
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = 0;
            return TRUE;
        }
    }

    *ret = (uint64_t)(uintptr_t)handle;
    if (macrunner_hb_trace_wait_semantic_budget_allows())
    {
        fprintf( stderr, "macrunner-hb-wait-semantic: thread import=%s!%s pc=%p rsp=%p "
                 "handle=%p tid=%lu start=%p param=%p flags=%#lx suspended=%u\n",
                 thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                 (void *)(uintptr_t)ctx->regs.x64.rsp, (void *)(uintptr_t)*ret,
                 (unsigned long)tid, (void *)(uintptr_t)start, (void *)(uintptr_t)param,
                 (unsigned long)flags, (unsigned int)((flags & CREATE_SUSPENDED) != 0) );
        fflush( stderr );
    }
    TRACE( "MacRunner HyperBridge semantic %s!%s start=%p param=%p flags=%#lx ret=%p tid=%lu\n",
           thunk->dll_name, thunk->import_name, (void *)(uintptr_t)start,
           (void *)(uintptr_t)param, (unsigned long)flags, (void *)(uintptr_t)*ret,
           (unsigned long)tid );
    return TRUE;
}

static LARGE_INTEGER *macrunner_hb_get_nt_timeout( LARGE_INTEGER *time, DWORD timeout )
{
    if (timeout == INFINITE) return NULL;
    time->QuadPart = (LONGLONG)timeout * -10000;
    return time;
}

struct macrunner_hb_wait_addr_entry
{
    struct list entry;
    const void *addr;
    DWORD tid;
};

struct macrunner_hb_wait_addr_queue
{
    struct list queue;
    LONG lock;
};

static struct macrunner_hb_wait_addr_queue macrunner_hb_wait_addr_queues[256];

static struct macrunner_hb_wait_addr_queue *macrunner_hb_get_wait_addr_queue( const void *addr )
{
    ULONG_PTR val = (ULONG_PTR)addr;

    return &macrunner_hb_wait_addr_queues[(val >> 4) % ARRAY_SIZE(macrunner_hb_wait_addr_queues)];
}

static void macrunner_hb_wait_addr_spin_lock( LONG *lock )
{
    while (InterlockedCompareExchange( lock, -1, 0 ))
        YieldProcessor();
}

static void macrunner_hb_wait_addr_spin_unlock( LONG *lock )
{
    InterlockedExchange( lock, 0 );
}

static BOOL macrunner_hb_compare_wait_addr( const void *addr, const void *cmp, SIZE_T size )
{
    if (!addr || !cmp) return FALSE;

    switch (size)
    {
    case 1:
        return *(const UCHAR *)addr == *(const UCHAR *)cmp;
    case 2:
        return *(const USHORT *)addr == *(const USHORT *)cmp;
    case 4:
        return *(const ULONG *)addr == *(const ULONG *)cmp;
    case 8:
        return *(const ULONG64 *)addr == *(const ULONG64 *)cmp;
    default:
        return FALSE;
    }
}

static NTSTATUS macrunner_hb_rtl_wait_on_address( const void *addr, const void *cmp, SIZE_T size,
                                                  const LARGE_INTEGER *timeout )
{
    struct macrunner_hb_wait_addr_queue *queue;
    struct macrunner_hb_wait_addr_entry entry;
    NTSTATUS status;

    if (size != 1 && size != 2 && size != 4 && size != 8)
        return STATUS_INVALID_PARAMETER;

    queue = macrunner_hb_get_wait_addr_queue( addr );
    entry.addr = addr;
    entry.tid = GetCurrentThreadId();

    macrunner_hb_wait_addr_spin_lock( &queue->lock );
    if (!macrunner_hb_compare_wait_addr( addr, cmp, size ))
    {
        macrunner_hb_wait_addr_spin_unlock( &queue->lock );
        return STATUS_SUCCESS;
    }

    if (!queue->queue.next)
        list_init( &queue->queue );
    list_add_tail( &queue->queue, &entry.entry );
    macrunner_hb_wait_addr_spin_unlock( &queue->lock );

    status = NtWaitForAlertByThreadId( NULL, timeout );

    if (entry.addr)
    {
        macrunner_hb_wait_addr_spin_lock( &queue->lock );
        if (entry.addr)
            list_remove( &entry.entry );
        macrunner_hb_wait_addr_spin_unlock( &queue->lock );
    }

    return status == STATUS_ALERTED ? STATUS_SUCCESS : status;
}

static void macrunner_hb_rtl_wake_address_all( const void *addr )
{
    struct macrunner_hb_wait_addr_queue *queue;
    struct macrunner_hb_wait_addr_entry *entry, *next;
    unsigned int count = 0;
    HANDLE tids[256];

    if (!addr) return;

    queue = macrunner_hb_get_wait_addr_queue( addr );
    macrunner_hb_wait_addr_spin_lock( &queue->lock );
    if (!queue->queue.next)
        list_init( &queue->queue );

    LIST_FOR_EACH_ENTRY_SAFE( entry, next, &queue->queue, struct macrunner_hb_wait_addr_entry, entry )
    {
        if (entry->addr == addr)
        {
            entry->addr = NULL;
            list_remove( &entry->entry );
            if (count == ARRAY_SIZE(tids))
            {
                NtAlertMultipleThreadByThreadId( tids, count, NULL, NULL );
                count = 0;
            }
            tids[count++] = ULongToHandle( entry->tid );
        }
    }

    macrunner_hb_wait_addr_spin_unlock( &queue->lock );
    if (count)
        NtAlertMultipleThreadByThreadId( tids, count, NULL, NULL );
}

static void macrunner_hb_rtl_wake_address_single( const void *addr )
{
    struct macrunner_hb_wait_addr_queue *queue;
    struct macrunner_hb_wait_addr_entry *entry;
    DWORD tid = 0;

    if (!addr) return;

    queue = macrunner_hb_get_wait_addr_queue( addr );
    macrunner_hb_wait_addr_spin_lock( &queue->lock );
    if (!queue->queue.next)
        list_init( &queue->queue );

    LIST_FOR_EACH_ENTRY( entry, &queue->queue, struct macrunner_hb_wait_addr_entry, entry )
    {
        if (entry->addr == addr)
        {
            tid = entry->tid;
            entry->addr = NULL;
            list_remove( &entry->entry );
            break;
        }
    }

    macrunner_hb_wait_addr_spin_unlock( &queue->lock );
    if (tid)
        NtAlertThreadByThreadId( ULongToHandle( tid ) );
}

static BOOL macrunner_hb_try_wait_address_semantic( hb_context_t *ctx,
                                                    const struct macrunner_hb_import_thunk *thunk,
                                                    const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                    uint64_t *ret )
{
    LARGE_INTEGER timeout;
    LARGE_INTEGER *timeout_ptr = NULL;
    NTSTATUS status;

    if (!ctx || !thunk || !args || !ret) return FALSE;

    if (macrunner_hb_strieq( thunk->import_name, "WakeByAddressAll" ) ||
        macrunner_hb_strieq( thunk->import_name, "RtlWakeAddressAll" ))
    {
        macrunner_hb_rtl_wake_address_all( (const void *)(uintptr_t)args[0] );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = 0;
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: address-wake import=%s!%s pc=%p "
                     "caller=%p rsp=%p addr=%p mode=all\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)caller, (void *)(uintptr_t)ctx->regs.x64.rsp,
                     (void *)(uintptr_t)args[0] );
            fflush( stderr );
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "WakeByAddressSingle" ) ||
        macrunner_hb_strieq( thunk->import_name, "RtlWakeAddressSingle" ))
    {
        macrunner_hb_rtl_wake_address_single( (const void *)(uintptr_t)args[0] );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = 0;
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: address-wake import=%s!%s pc=%p "
                     "caller=%p rsp=%p addr=%p mode=single\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)caller, (void *)(uintptr_t)ctx->regs.x64.rsp,
                     (void *)(uintptr_t)args[0] );
            fflush( stderr );
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "WaitOnAddress" ))
    {
        timeout_ptr = macrunner_hb_get_nt_timeout( &timeout, (DWORD)args[3] );
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: address-wait-before import=%s!%s pc=%p "
                     "caller=%p rsp=%p addr=%p cmp=%p size=%zu timeout_ms=%lu\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)caller, (void *)(uintptr_t)ctx->regs.x64.rsp,
                     (void *)(uintptr_t)args[0], (void *)(uintptr_t)args[1],
                     (size_t)args[2], (unsigned long)(DWORD)args[3] );
            fflush( stderr );
        }
        status = macrunner_hb_rtl_wait_on_address( (const void *)(uintptr_t)args[0],
                                                   (const void *)(uintptr_t)args[1],
                                                   (SIZE_T)args[2], timeout_ptr );
        NtCurrentTeb()->LastStatusValue = status;
        if (status == STATUS_SUCCESS)
        {
            NtCurrentTeb()->LastErrorValue = 0;
            *ret = 1;
        }
        else
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = 0;
        }
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: address-wait import=%s!%s pc=%p "
                     "caller=%p rsp=%p addr=%p cmp=%p size=%zu timeout_ms=%lu "
                     "status=%08lx ret=%p last_error=%lu\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)caller, (void *)(uintptr_t)ctx->regs.x64.rsp,
                     (void *)(uintptr_t)args[0], (void *)(uintptr_t)args[1],
                     (size_t)args[2], (unsigned long)(DWORD)args[3], (unsigned long)status,
                     (void *)(uintptr_t)*ret, (unsigned long)NtCurrentTeb()->LastErrorValue );
            fflush( stderr );
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "RtlWaitOnAddress" ))
    {
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: address-wait-before import=%s!%s pc=%p "
                     "caller=%p rsp=%p addr=%p cmp=%p size=%zu timeout=%p\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)caller, (void *)(uintptr_t)ctx->regs.x64.rsp,
                     (void *)(uintptr_t)args[0], (void *)(uintptr_t)args[1],
                     (size_t)args[2], (void *)(uintptr_t)args[3] );
            fflush( stderr );
        }
        status = macrunner_hb_rtl_wait_on_address( (const void *)(uintptr_t)args[0],
                                                   (const void *)(uintptr_t)args[1],
                                                   (SIZE_T)args[2],
                                                   (const LARGE_INTEGER *)(uintptr_t)args[3] );
        NtCurrentTeb()->LastStatusValue = status;
        *ret = status;
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: address-wait import=%s!%s pc=%p "
                     "caller=%p rsp=%p addr=%p cmp=%p size=%zu timeout=%p status=%08lx\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)caller, (void *)(uintptr_t)ctx->regs.x64.rsp,
                     (void *)(uintptr_t)args[0], (void *)(uintptr_t)args[1],
                     (size_t)args[2], (void *)(uintptr_t)args[3], (unsigned long)status );
            fflush( stderr );
        }
        return TRUE;
    }

    return FALSE;
}

static BOOL macrunner_hb_guest_slist_uses_x64_header( const hb_context_t *ctx )
{
    return !ctx || ctx->arch != HB_ARCH_X86;
}

static uint64_t macrunner_hb_guest_slist_sequence( const hb_context_t *ctx, void *list )
{
    uint64_t alignment;

    if (!list) return 0;
    alignment = *(uint64_t *)list;
    if (macrunner_hb_guest_slist_uses_x64_header( ctx ))
        return (alignment >> 16) & 0x0000ffffffffffffULL;
    return (alignment >> 48) & 0xffff;
}

static WORD macrunner_hb_guest_slist_depth( const hb_context_t *ctx, void *list )
{
    uint64_t alignment;

    if (!list) return 0;
    alignment = *(uint64_t *)list;
    if (macrunner_hb_guest_slist_uses_x64_header( ctx ))
        return (WORD)(alignment & 0xffff);
    return (WORD)((alignment >> 32) & 0xffff);
}

static uint64_t macrunner_hb_guest_slist_first( const hb_context_t *ctx, void *list )
{
    if (!list) return 0;
    if (macrunner_hb_guest_slist_uses_x64_header( ctx ))
        return *(uint64_t *)((char *)list + 8) & 0xfffffffffffffff0ULL;
    return *(uint32_t *)list;
}

static void macrunner_hb_guest_slist_set_depth_sequence( const hb_context_t *ctx, void *list,
                                                         WORD depth, uint64_t sequence )
{
    if (macrunner_hb_guest_slist_uses_x64_header( ctx ))
        *(uint64_t *)list = ((sequence & 0x0000ffffffffffffULL) << 16) | depth;
    else
    {
        uint64_t alignment = *(uint64_t *)list;
        alignment &= 0x00000000ffffffffULL;
        alignment |= (uint64_t)depth << 32;
        alignment |= (sequence & 0xffff) << 48;
        *(uint64_t *)list = alignment;
    }
}

static void macrunner_hb_guest_slist_set_first( const hb_context_t *ctx, void *list, uint64_t first )
{
    if (macrunner_hb_guest_slist_uses_x64_header( ctx ))
    {
        uint64_t *region = (uint64_t *)((char *)list + 8);
        uint64_t flags = *region & 0xf;
        if (!(flags & 1)) flags |= 1;
        *region = (first & 0xfffffffffffffff0ULL) | flags;
    }
    else
    {
        uint64_t alignment = *(uint64_t *)list;
        alignment &= 0xffffffff00000000ULL;
        alignment |= (uint32_t)first;
        *(uint64_t *)list = alignment;
    }
}

static uint64_t macrunner_hb_guest_slist_entry_next( const hb_context_t *ctx, void *entry )
{
    if (!entry) return 0;
    if (macrunner_hb_guest_slist_uses_x64_header( ctx ))
        return *(uint64_t *)entry;
    return *(uint32_t *)entry;
}

static void macrunner_hb_guest_slist_set_entry_next( const hb_context_t *ctx, void *entry,
                                                     uint64_t next )
{
    if (macrunner_hb_guest_slist_uses_x64_header( ctx ))
        *(uint64_t *)entry = next;
    else
        *(uint32_t *)entry = (uint32_t)next;
}

static void macrunner_hb_guest_slist_initialize( const hb_context_t *ctx, void *list )
{
    if (!list) return;
    pthread_mutex_lock( &macrunner_hb_slist_mutex );
    *(uint64_t *)list = 0;
    if (macrunner_hb_guest_slist_uses_x64_header( ctx ))
        *(uint64_t *)((char *)list + 8) = 1;
    pthread_mutex_unlock( &macrunner_hb_slist_mutex );
}

static uint64_t macrunner_hb_guest_slist_flush( const hb_context_t *ctx, void *list )
{
    uint64_t old, sequence;

    if (!list) return 0;
    pthread_mutex_lock( &macrunner_hb_slist_mutex );
    old = macrunner_hb_guest_slist_first( ctx, list );
    if (old)
    {
        sequence = macrunner_hb_guest_slist_sequence( ctx, list ) + 1;
        macrunner_hb_guest_slist_set_first( ctx, list, 0 );
        macrunner_hb_guest_slist_set_depth_sequence( ctx, list, 0, sequence );
    }
    pthread_mutex_unlock( &macrunner_hb_slist_mutex );
    return old;
}

static uint64_t macrunner_hb_guest_slist_pop( const hb_context_t *ctx, void *list )
{
    uint64_t entry, next, sequence;
    WORD depth;

    if (!list) return 0;
    pthread_mutex_lock( &macrunner_hb_slist_mutex );
    entry = macrunner_hb_guest_slist_first( ctx, list );
    if (entry)
    {
        next = macrunner_hb_guest_slist_entry_next( ctx, (void *)(uintptr_t)entry );
        depth = macrunner_hb_guest_slist_depth( ctx, list );
        sequence = macrunner_hb_guest_slist_sequence( ctx, list ) + 1;
        macrunner_hb_guest_slist_set_first( ctx, list, next );
        macrunner_hb_guest_slist_set_depth_sequence( ctx, list, depth ? depth - 1 : 0, sequence );
    }
    pthread_mutex_unlock( &macrunner_hb_slist_mutex );
    return entry;
}

static uint64_t macrunner_hb_guest_slist_push( const hb_context_t *ctx, void *list, void *entry )
{
    uint64_t old, sequence;
    WORD depth;

    if (!list || !entry) return 0;
    pthread_mutex_lock( &macrunner_hb_slist_mutex );
    old = macrunner_hb_guest_slist_first( ctx, list );
    depth = macrunner_hb_guest_slist_depth( ctx, list );
    sequence = macrunner_hb_guest_slist_sequence( ctx, list ) + 1;
    macrunner_hb_guest_slist_set_entry_next( ctx, entry, old );
    macrunner_hb_guest_slist_set_first( ctx, list, (uint64_t)(uintptr_t)entry );
    macrunner_hb_guest_slist_set_depth_sequence( ctx, list, depth + 1, sequence );
    pthread_mutex_unlock( &macrunner_hb_slist_mutex );
    return old;
}

static uint64_t macrunner_hb_guest_slist_push_list( const hb_context_t *ctx, void *list,
                                                    void *first, void *last, ULONG count )
{
    uint64_t old, sequence;
    WORD depth;

    if (!list || !first || !last) return 0;
    pthread_mutex_lock( &macrunner_hb_slist_mutex );
    old = macrunner_hb_guest_slist_first( ctx, list );
    depth = macrunner_hb_guest_slist_depth( ctx, list );
    sequence = macrunner_hb_guest_slist_sequence( ctx, list ) + 1;
    macrunner_hb_guest_slist_set_entry_next( ctx, last, old );
    macrunner_hb_guest_slist_set_first( ctx, list, (uint64_t)(uintptr_t)first );
    macrunner_hb_guest_slist_set_depth_sequence( ctx, list, depth + count, sequence );
    pthread_mutex_unlock( &macrunner_hb_slist_mutex );
    return old;
}

static BOOL macrunner_hb_ascii_to_wchar_buffer( const char *src, WCHAR *dst, size_t dst_count )
{
    size_t i;

    if (!src || !dst || !dst_count) return FALSE;
    for (i = 0; i + 1 < dst_count && src[i]; i++) dst[i] = (unsigned char)src[i];
    if (src[i]) return FALSE;
    dst[i] = 0;
    return TRUE;
}

#define MACRUNNER_HB_DIRECTORY_TRAVERSE 0x0002
#define MACRUNNER_HB_DIRECTORY_CREATE_OBJECT 0x0004
#define MACRUNNER_HB_COINIT_APARTMENTTHREADED 0x2
#define MACRUNNER_HB_BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002U
#define MACRUNNER_HB_BCRYPT_RNG_ALG_HANDLE 0x81ULL

static NTSTATUS macrunner_hb_get_named_object_directory( HANDLE *out )
{
    static HANDLE handle;
    NTSTATUS status = STATUS_SUCCESS;

    if (!handle)
    {
        WCHAR buffer[64];
        UNICODE_STRING name;
        OBJECT_ATTRIBUTES attr;
        HANDLE dir = 0;

        swprintf( buffer, ARRAY_SIZE(buffer), L"\\Sessions\\%u\\BaseNamedObjects",
                  NtCurrentTeb()->Peb->SessionId );
        RtlInitUnicodeString( &name, buffer );
        InitializeObjectAttributes( &attr, &name, 0, 0, NULL );
        status = NtOpenDirectoryObject( &dir,
                                        MACRUNNER_HB_DIRECTORY_CREATE_OBJECT |
                                        MACRUNNER_HB_DIRECTORY_TRAVERSE,
                                        &attr );
        if (!status && InterlockedCompareExchangePointer( (void *volatile *)&handle, dir, 0 ))
            NtClose( dir );
    }

    *out = handle;
    return handle ? STATUS_SUCCESS : status;
}

static void macrunner_hb_init_named_object_attributes( OBJECT_ATTRIBUTES *attr, UNICODE_STRING *nameW,
                                                       SECURITY_ATTRIBUTES *sa, const WCHAR *name,
                                                       BOOL create, BOOL inherit )
{
    HANDLE root = 0;
    ULONG attributes = (inherit || (sa && sa->bInheritHandle)) ? OBJ_INHERIT : 0;

    if (create) attributes |= OBJ_OPENIF;
    if (name)
    {
        RtlInitUnicodeString( nameW, name );
        macrunner_hb_get_named_object_directory( &root );
    }
    InitializeObjectAttributes( attr, name ? nameW : NULL, attributes, root,
                                sa ? sa->lpSecurityDescriptor : NULL );
}

static BOOL macrunner_hb_try_winrt_semantic( hb_context_t *ctx,
                                             const struct macrunner_hb_import_thunk *thunk,
                                             const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                             uint64_t *ret )
{
    if (!ctx || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_strieq( thunk->dll_name, "api-ms-win-core-winrt-l1-1-0.dll" ) &&
        !macrunner_hb_strieq( thunk->dll_name, "combase.dll" ))
        return FALSE;

    if (macrunner_hb_strieq( thunk->import_name, "RoInitialize" ))
    {
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = S_OK;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "RoUninitialize" ))
    {
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = 0;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "RoGetActivationFactory" ) ||
        macrunner_hb_strieq( thunk->import_name, "RoActivateInstance" ))
    {
        if (macrunner_hb_strieq( thunk->import_name, "RoGetActivationFactory" ) && args[2])
            *(void **)(uintptr_t)args[2] = NULL;
        else if (macrunner_hb_strieq( thunk->import_name, "RoActivateInstance" ) && args[1])
            *(void **)(uintptr_t)args[1] = NULL;
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = 0x80040154u; /* REGDB_E_CLASSNOTREG */
        return TRUE;
    }

    return FALSE;
}

static BOOL macrunner_hb_try_com_apartment_semantic( hb_context_t *ctx,
                                                     const struct macrunner_hb_import_thunk *thunk,
                                                     const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                     uint64_t *ret )
{
    DWORD model;
    DWORD threading_model;
    DWORD current_threading_model;
    HRESULT hr;

    if (!ctx || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_strieq( thunk->dll_name, "ole32.dll" ) &&
        !macrunner_hb_strieq( thunk->dll_name, "combase.dll" ))
        return FALSE;

    if (macrunner_hb_strieq( thunk->import_name, "CoUninitialize" ))
    {
        if (macrunner_hb_com_init_count) macrunner_hb_com_init_count--;
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = 0;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "CoInitialize" ))
        model = MACRUNNER_HB_COINIT_APARTMENTTHREADED;
    else if (macrunner_hb_strieq( thunk->import_name, "CoInitializeEx" ))
        model = (DWORD)args[1];
    else
        return FALSE;

    threading_model = model & MACRUNNER_HB_COINIT_APARTMENTTHREADED;
    current_threading_model = macrunner_hb_com_model & MACRUNNER_HB_COINIT_APARTMENTTHREADED;
    if (macrunner_hb_com_init_count && threading_model != current_threading_model)
    {
        *ret = RPC_E_CHANGED_MODE;
        return TRUE;
    }

    hr = macrunner_hb_com_init_count ? S_FALSE : S_OK;
    macrunner_hb_com_model = model;
    macrunner_hb_com_init_count++;
    NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
    *ret = hr;
    return TRUE;
}

static BOOL macrunner_hb_copy_registered_message_name( const struct macrunner_hb_import_thunk *thunk,
                                                       uint64_t ptr, char *buffer, size_t size )
{
    size_t i;

    if (!ptr || !buffer || !size) return FALSE;
    if (macrunner_hb_strieq( thunk->import_name, "RegisterWindowMessageA" ))
    {
        const char *name = (const char *)(uintptr_t)ptr;

        for (i = 0; i + 1 < size && name[i]; i++) buffer[i] = name[i];
        if (name[i]) return FALSE;
        buffer[i] = 0;
        return i > 0;
    }
    else
    {
        const WCHAR *name = (const WCHAR *)(uintptr_t)ptr;

        for (i = 0; i + 1 < size && name[i]; i++)
            buffer[i] = name[i] < 0x80 ? (char)name[i] : '?';
        if (name[i]) return FALSE;
        buffer[i] = 0;
        return i > 0;
    }
}

static int macrunner_hb_write_user_object_string( hb_context_t *ctx, BOOL wide,
                                                  hb_gva_t info_gva, DWORD len,
                                                  hb_gva_t needed_gva, const char *value )
{
    DWORD needed;
    size_t i, chars;
    hb_result_t result;

    if (!value) return -1;
    chars = strlen( value ) + 1;
    needed = wide ? (DWORD)(chars * sizeof(WCHAR)) : (DWORD)chars;
    if (needed_gva &&
        hb_memory_write( ctx->memory, needed_gva, &needed, sizeof(needed) ) != HB_OK)
        return -1;
    if (!info_gva || len < needed) return 1;
    if (wide)
    {
        WCHAR buffer[MACRUNNER_HB_REGISTERED_MESSAGE_NAME_MAX];

        if (chars > ARRAY_SIZE(buffer)) return 1;
        for (i = 0; i < chars; i++) buffer[i] = (unsigned char)value[i];
        result = hb_memory_write( ctx->memory, info_gva, buffer, needed );
    }
    else
        result = hb_memory_write( ctx->memory, info_gva, value, needed );
    return result == HB_OK ? 0 : -1;
}

static BOOL macrunner_hb_current_process_is_rundll32(void)
{
    const UNICODE_STRING *image = macrunner_hb_process_image_path();
    static const char rundll32[] = "rundll32.exe";
    size_t suffix_len = ARRAY_SIZE(rundll32) - 1;
    size_t chars, off, i;

    if (!image || !image->Buffer || image->Length < suffix_len * sizeof(WCHAR))
        return FALSE;

    chars = image->Length / sizeof(WCHAR);
    off = chars - suffix_len;
    for (i = 0; i < suffix_len; i++)
    {
        WCHAR ch = image->Buffer[off + i];
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (ch != (unsigned char)rundll32[i]) return FALSE;
    }
    return TRUE;
}

static BOOL macrunner_hb_is_pseudo_hwnd( uint64_t hwnd )
{
    return (hwnd & ~0xffffULL) == MACRUNNER_HB_PSEUDO_HWND_BASE;
}

static BOOL macrunner_hb_try_user32_semantic( hb_context_t *ctx,
                                              const struct macrunner_hb_import_thunk *thunk,
                                              const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                              uint64_t *ret )
{
    char name[MACRUNNER_HB_REGISTERED_MESSAGE_NAME_MAX];
    UINT message = 0;
    unsigned int i;

    if (!ctx || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_strieq( thunk->dll_name, "user32.dll" ))
        return FALSE;

    if (macrunner_hb_strieq( thunk->import_name, "RegisterClassA" ) ||
        macrunner_hb_strieq( thunk->import_name, "RegisterClassW" ) ||
        macrunner_hb_strieq( thunk->import_name, "RegisterClassExA" ) ||
        macrunner_hb_strieq( thunk->import_name, "RegisterClassExW" ))
    {
        ATOM atom;

        if (!macrunner_hb_current_process_is_rundll32()) return FALSE;

        pthread_mutex_lock( &macrunner_hb_registered_message_mutex );
        atom = macrunner_hb_synthetic_class_next++;
        if (macrunner_hb_synthetic_class_next < 0xc000) macrunner_hb_synthetic_class_next = 0xc000;
        pthread_mutex_unlock( &macrunner_hb_registered_message_mutex );

        *ret = atom;
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "CreateWindowExA" ) ||
        macrunner_hb_strieq( thunk->import_name, "CreateWindowExW" ))
    {
        uint64_t hwnd;

        if (!macrunner_hb_current_process_is_rundll32()) return FALSE;
        pthread_mutex_lock( &macrunner_hb_registered_message_mutex );
        hwnd = macrunner_hb_synthetic_hwnd_next++;
        if ((macrunner_hb_synthetic_hwnd_next & ~0xffffULL) != MACRUNNER_HB_PSEUDO_HWND_BASE)
            macrunner_hb_synthetic_hwnd_next = MACRUNNER_HB_PSEUDO_HWND_BASE;
        pthread_mutex_unlock( &macrunner_hb_registered_message_mutex );

        *ret = hwnd;
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        return TRUE;
    }

    if ((macrunner_hb_strieq( thunk->import_name, "DestroyWindow" ) ||
         macrunner_hb_strieq( thunk->import_name, "ShowWindow" ) ||
         macrunner_hb_strieq( thunk->import_name, "UpdateWindow" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetFocus" )) &&
        macrunner_hb_is_pseudo_hwnd( args[0] ))
    {
        *ret = TRUE;
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "LoadCursorA" ) ||
        macrunner_hb_strieq( thunk->import_name, "LoadCursorW" ) ||
        macrunner_hb_strieq( thunk->import_name, "LoadIconA" ) ||
        macrunner_hb_strieq( thunk->import_name, "LoadIconW" ))
    {
        uint64_t id = args[1] <= 0xffff ? args[1] : 0;
        BOOL cursor = macrunner_hb_strieq( thunk->import_name, "LoadCursorA" ) ||
                      macrunner_hb_strieq( thunk->import_name, "LoadCursorW" );

        if (!args[0] && id)
        {
            *ret = (cursor ? MACRUNNER_HB_PSEUDO_HCURSOR_BASE : MACRUNNER_HB_PSEUDO_HICON_BASE) | id;
            RtlSetLastWin32Error( ERROR_SUCCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
            return TRUE;
        }
        return FALSE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "LoadImageA" ) ||
        macrunner_hb_strieq( thunk->import_name, "LoadImageW" ))
    {
        uint64_t id = args[1] <= 0xffff ? args[1] : 0;
        UINT type = (UINT)args[2];

        if (!args[0] && id && (type == IMAGE_ICON || type == IMAGE_CURSOR))
        {
            *ret = (type == IMAGE_CURSOR ? MACRUNNER_HB_PSEUDO_HCURSOR_BASE :
                                           MACRUNNER_HB_PSEUDO_HICON_BASE) | id;
            RtlSetLastWin32Error( ERROR_SUCCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
            return TRUE;
        }
        return FALSE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetProcessWindowStation" ))
    {
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = MACRUNNER_HB_PSEUDO_HWINSTA;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetThreadDesktop" ) ||
        macrunner_hb_strieq( thunk->import_name, "OpenInputDesktop" ))
    {
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = MACRUNNER_HB_PSEUDO_HDESK;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "SetThreadDesktop" ) ||
        macrunner_hb_strieq( thunk->import_name, "CloseDesktop" ))
    {
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = TRUE;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetUserObjectInformationA" ) ||
        macrunner_hb_strieq( thunk->import_name, "GetUserObjectInformationW" ))
    {
        USEROBJECTFLAGS flags;
        hb_gva_t info_gva = (hb_gva_t)args[2];
        DWORD len = (DWORD)args[3];
        hb_gva_t needed_gva = (hb_gva_t)args[4];
        DWORD needed;
        BOOL wide = macrunner_hb_strieq( thunk->import_name, "GetUserObjectInformationW" );
        BOOL is_desktop = args[0] == MACRUNNER_HB_PSEUDO_HDESK;
        BOOL is_winsta = args[0] == MACRUNNER_HB_PSEUDO_HWINSTA;
        int write_result;

        if (!is_desktop && !is_winsta)
        {
            RtlSetLastWin32Error( ERROR_INVALID_HANDLE );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_HANDLE;
            *ret = FALSE;
            return TRUE;
        }

        if ((INT)args[1] == UOI_NAME || (INT)args[1] == UOI_TYPE)
        {
            const char *value = (INT)args[1] == UOI_NAME ?
                                (is_desktop ? "Default" : "WinSta0") :
                                (is_desktop ? "Desktop" : "WindowStation");

            write_result = macrunner_hb_write_user_object_string( ctx, wide, info_gva, len,
                                                                  needed_gva, value );
            if (write_result == 1)
            {
                RtlSetLastWin32Error( ERROR_INSUFFICIENT_BUFFER );
                NtCurrentTeb()->LastStatusValue = STATUS_BUFFER_TOO_SMALL;
                *ret = FALSE;
            }
            else if (write_result)
            {
                RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
                NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
                *ret = FALSE;
            }
            else
            {
                RtlSetLastWin32Error( ERROR_SUCCESS );
                NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
                *ret = TRUE;
            }
            return TRUE;
        }

        if ((INT)args[1] == UOI_FLAGS)
        {
            needed = sizeof(flags);
            if (needed_gva &&
                hb_memory_write( ctx->memory, needed_gva, &needed, sizeof(needed) ) != HB_OK)
            {
                RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
                NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
                *ret = FALSE;
                return TRUE;
            }
            if (!info_gva || len < sizeof(flags))
            {
                RtlSetLastWin32Error( ERROR_BUFFER_OVERFLOW );
                NtCurrentTeb()->LastStatusValue = STATUS_BUFFER_OVERFLOW;
                *ret = FALSE;
                return TRUE;
            }
            memset( &flags, 0, sizeof(flags) );
            if (is_winsta) flags.dwFlags = WSF_VISIBLE;
            if (hb_memory_write( ctx->memory, info_gva, &flags, sizeof(flags) ) != HB_OK)
            {
                RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
                NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
                *ret = FALSE;
                return TRUE;
            }
            RtlSetLastWin32Error( ERROR_SUCCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
            *ret = TRUE;
            return TRUE;
        }

        RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
        NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
        *ret = FALSE;
        return TRUE;
    }

    if (!macrunner_hb_strieq( thunk->import_name, "RegisterWindowMessageA" ) &&
        !macrunner_hb_strieq( thunk->import_name, "RegisterWindowMessageW" ))
        return FALSE;

    if (!macrunner_hb_copy_registered_message_name( thunk, args[0], name, sizeof(name) ))
    {
        RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
        NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
        *ret = 0;
        return TRUE;
    }

    pthread_mutex_lock( &macrunner_hb_registered_message_mutex );
    for (i = 0; i < MACRUNNER_HB_REGISTERED_MESSAGE_MAX; i++)
    {
        if (macrunner_hb_registered_messages[i].message &&
            !strcmp( macrunner_hb_registered_messages[i].name, name ))
        {
            message = macrunner_hb_registered_messages[i].message;
            break;
        }
    }
    if (!message)
    {
        for (i = 0; i < MACRUNNER_HB_REGISTERED_MESSAGE_MAX; i++)
        {
            if (!macrunner_hb_registered_messages[i].message)
            {
                message = macrunner_hb_registered_message_next++;
                if (macrunner_hb_registered_message_next > 0xffff)
                    macrunner_hb_registered_message_next = 0xc000;
                macrunner_hb_copy_cstr( macrunner_hb_registered_messages[i].name,
                                        sizeof(macrunner_hb_registered_messages[i].name), name );
                macrunner_hb_registered_messages[i].message = message;
                break;
            }
        }
    }
    pthread_mutex_unlock( &macrunner_hb_registered_message_mutex );

    if (!message)
    {
        RtlSetLastWin32Error( ERROR_NOT_ENOUGH_MEMORY );
        NtCurrentTeb()->LastStatusValue = STATUS_NO_MEMORY;
        *ret = 0;
        return TRUE;
    }

    RtlSetLastWin32Error( ERROR_SUCCESS );
    NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
    *ret = message;
    return TRUE;
}

static NTSTATUS macrunner_hb_critical_section_init( RTL_CRITICAL_SECTION *cs, ULONG spin )
{
    if (!cs) return STATUS_INVALID_PARAMETER;

    cs->DebugInfo = (void *)(ULONG_PTR)-1;
    cs->LockCount = -1;
    cs->RecursionCount = 0;
    cs->OwningThread = 0;
    cs->LockSemaphore = 0;
    cs->SpinCount = spin & ~0x80000000u;
    return STATUS_SUCCESS;
}

static HANDLE macrunner_hb_critical_section_semaphore( RTL_CRITICAL_SECTION *cs )
{
    HANDLE sem = cs->LockSemaphore;
    HANDLE new_sem = 0;
    NTSTATUS status;

    if ((ULONG_PTR)sem > 1) return sem;

    status = NtCreateSemaphore( &new_sem, SEMAPHORE_ALL_ACCESS, NULL, 0, 0x7fffffff );
    if (status) return 0;

    sem = InterlockedCompareExchangePointer( (void *volatile *)&cs->LockSemaphore, new_sem, 0 );
    if (sem)
    {
        NtClose( new_sem );
        return (ULONG_PTR)sem > 1 ? sem : 0;
    }
    return new_sem;
}

static NTSTATUS macrunner_hb_critical_section_delete( RTL_CRITICAL_SECTION *cs )
{
    HANDLE sem;

    if (!cs) return STATUS_INVALID_PARAMETER;

    sem = (ULONG_PTR)cs->LockSemaphore > 1 ? cs->LockSemaphore : 0;
    cs->DebugInfo = NULL;
    cs->LockCount = -1;
    cs->RecursionCount = 0;
    cs->OwningThread = 0;
    cs->LockSemaphore = 0;
    cs->SpinCount = 0;
    if (sem) NtClose( sem );
    return STATUS_SUCCESS;
}

static NTSTATUS macrunner_hb_critical_section_enter( RTL_CRITICAL_SECTION *cs )
{
    HANDLE tid = ULongToHandle( GetCurrentThreadId() );
    ULONG count;

    if (!cs) return STATUS_INVALID_PARAMETER;

    if (cs->SpinCount)
    {
        if (InterlockedCompareExchange( &cs->LockCount, 0, -1 ) == -1)
            goto acquired;
        for (count = cs->SpinCount; count > 0; count--)
        {
            if (cs->LockCount > 0) break;
            if (cs->LockCount == -1 &&
                InterlockedCompareExchange( &cs->LockCount, 0, -1 ) == -1)
                goto acquired;
            YieldProcessor();
        }
    }

    if (InterlockedIncrement( &cs->LockCount ))
    {
        HANDLE sem;
        NTSTATUS status;

        if (cs->OwningThread == tid)
        {
            cs->RecursionCount++;
            return STATUS_SUCCESS;
        }

        sem = macrunner_hb_critical_section_semaphore( cs );
        if (!sem)
        {
            InterlockedDecrement( &cs->LockCount );
            return STATUS_NO_MEMORY;
        }
        status = NtWaitForSingleObject( sem, FALSE, NULL );
        if (status)
        {
            InterlockedDecrement( &cs->LockCount );
            return status;
        }
    }

acquired:
    cs->OwningThread = tid;
    cs->RecursionCount = 1;
    return STATUS_SUCCESS;
}

static BOOL macrunner_hb_critical_section_try_enter( RTL_CRITICAL_SECTION *cs )
{
    HANDLE tid = ULongToHandle( GetCurrentThreadId() );

    if (!cs) return FALSE;
    if (InterlockedCompareExchange( &cs->LockCount, 0, -1 ) == -1)
    {
        cs->OwningThread = tid;
        cs->RecursionCount = 1;
        return TRUE;
    }
    if (cs->OwningThread == tid)
    {
        InterlockedIncrement( &cs->LockCount );
        cs->RecursionCount++;
        return TRUE;
    }
    return FALSE;
}

static NTSTATUS macrunner_hb_critical_section_leave( RTL_CRITICAL_SECTION *cs )
{
    if (!cs) return STATUS_INVALID_PARAMETER;

    if (--cs->RecursionCount)
    {
        if (cs->RecursionCount > 0) InterlockedDecrement( &cs->LockCount );
        return STATUS_SUCCESS;
    }

    cs->OwningThread = 0;
    if (InterlockedDecrement( &cs->LockCount ) >= 0)
    {
        HANDLE sem = macrunner_hb_critical_section_semaphore( cs );
        if (!sem) return STATUS_NO_MEMORY;
        return NtReleaseSemaphore( sem, 1, NULL );
    }
    return STATUS_SUCCESS;
}

static struct macrunner_hb_tls_thread_values *macrunner_hb_tls_values_for_current_thread( BOOL create )
{
    DWORD tid = (DWORD)(ULONG_PTR)NtCurrentTeb()->ClientId.UniqueThread;
    struct macrunner_hb_tls_thread_values *free_entry = NULL;
    unsigned int i;

    if (!tid) return NULL;
    for (i = 0; i < MACRUNNER_HB_TLS_THREAD_MAX; i++)
    {
        struct macrunner_hb_tls_thread_values *entry = &macrunner_hb_tls_thread_values[i];

        if (entry->tid == tid) return entry;
        if (!entry->tid && !free_entry) free_entry = entry;
    }
    if (!create || !free_entry) return NULL;
    memset( free_entry, 0, sizeof(*free_entry) );
    free_entry->tid = tid;
    return free_entry;
}

static BOOL macrunner_hb_teb_tls_set_value( DWORD index, uint64_t value )
{
    TEB *teb = NtCurrentTeb();
    DWORD expansion_index;
    size_t expansion_count;

    if (index < TLS_MINIMUM_AVAILABLE)
    {
        teb->TlsSlots[index] = (void *)(uintptr_t)value;
        return TRUE;
    }

    expansion_index = index - TLS_MINIMUM_AVAILABLE;
    expansion_count = MACRUNNER_HB_TLS_SLOT_MAX - TLS_MINIMUM_AVAILABLE;
    if (expansion_index >= expansion_count) return FALSE;
    if (!teb->TlsExpansionSlots &&
        !(teb->TlsExpansionSlots = calloc( expansion_count, sizeof(void *) )))
        return FALSE;
    teb->TlsExpansionSlots[expansion_index] = (void *)(uintptr_t)value;
    return TRUE;
}

static BOOL macrunner_hb_teb_tls_get_value( DWORD index, uint64_t *value )
{
    TEB *teb = NtCurrentTeb();
    DWORD expansion_index;

    if (!value) return FALSE;
    if (index < TLS_MINIMUM_AVAILABLE)
    {
        *value = (uint64_t)(uintptr_t)teb->TlsSlots[index];
        return TRUE;
    }

    expansion_index = index - TLS_MINIMUM_AVAILABLE;
    if (expansion_index >= MACRUNNER_HB_TLS_SLOT_MAX - TLS_MINIMUM_AVAILABLE) return FALSE;
    *value = teb->TlsExpansionSlots ? (uint64_t)(uintptr_t)teb->TlsExpansionSlots[expansion_index] : 0;
    return TRUE;
}

static BOOL macrunner_hb_try_kernel32_handle_semantic( hb_context_t *ctx,
                                                       const struct macrunner_hb_import_thunk *thunk,
                                                       const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                       uint64_t *ret )
{
    LARGE_INTEGER timeout;
    NTSTATUS status;

    if (!ctx || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) &&
        !macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" ))
        return FALSE;

    if (macrunner_hb_strieq( thunk->import_name, "GetLastError" ))
    {
        *ret = NtCurrentTeb()->LastErrorValue;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "SetLastError" ))
    {
        NtCurrentTeb()->LastErrorValue = (DWORD)args[0];
        *ret = 0;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "DisableThreadLibraryCalls" ))
    {
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        NtCurrentTeb()->LastErrorValue = ERROR_SUCCESS;
        *ret = TRUE;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "SetErrorMode" ))
    {
        DWORD old_mode;

        pthread_mutex_lock( &macrunner_hb_error_mode_mutex );
        old_mode = macrunner_hb_error_mode;
        macrunner_hb_error_mode = (DWORD)args[0];
        pthread_mutex_unlock( &macrunner_hb_error_mode_mutex );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        RtlSetLastWin32Error( ERROR_SUCCESS );
        *ret = old_mode;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetErrorMode" ))
    {
        pthread_mutex_lock( &macrunner_hb_error_mode_mutex );
        *ret = macrunner_hb_error_mode;
        pthread_mutex_unlock( &macrunner_hb_error_mode_mutex );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        RtlSetLastWin32Error( ERROR_SUCCESS );
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "SetThreadErrorMode" ))
    {
        DWORD old_mode = macrunner_hb_thread_error_mode;

        macrunner_hb_thread_error_mode = (DWORD)args[0];
        if (args[1]) *(DWORD *)(uintptr_t)args[1] = old_mode;
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        RtlSetLastWin32Error( ERROR_SUCCESS );
        *ret = TRUE;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "OutputDebugStringA" ) ||
        macrunner_hb_strieq( thunk->import_name, "OutputDebugStringW" ))
    {
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        RtlSetLastWin32Error( ERROR_SUCCESS );
        *ret = 0;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "CreateEventA" ) ||
        macrunner_hb_strieq( thunk->import_name, "CreateEventW" ) ||
        macrunner_hb_strieq( thunk->import_name, "CreateEventExA" ) ||
        macrunner_hb_strieq( thunk->import_name, "CreateEventExW" ))
    {
        SECURITY_ATTRIBUTES *sa = (SECURITY_ATTRIBUTES *)(uintptr_t)args[0];
        OBJECT_ATTRIBUTES attr;
        OBJECT_ATTRIBUTES *attr_ptr = NULL;
        UNICODE_STRING nameW;
        WCHAR name_buffer[MAX_PATH];
        const WCHAR *name = NULL;
        HANDLE handle = 0;
        DWORD flags, access;

        if (macrunner_hb_strieq( thunk->import_name, "CreateEventExA" ) ||
            macrunner_hb_strieq( thunk->import_name, "CreateEventExW" ))
        {
            flags = (DWORD)args[2];
            access = (DWORD)args[3];
            if (args[1])
            {
                if (macrunner_hb_strieq( thunk->import_name, "CreateEventExA" ))
                {
                    if (!macrunner_hb_ascii_to_wchar_buffer( (const char *)(uintptr_t)args[1],
                                                             name_buffer, ARRAY_SIZE(name_buffer) ))
                    {
                        RtlSetLastWin32Error( ERROR_FILENAME_EXCED_RANGE );
                        NtCurrentTeb()->LastStatusValue = STATUS_NAME_TOO_LONG;
                        *ret = 0;
                        return TRUE;
                    }
                    name = name_buffer;
                }
                else name = (const WCHAR *)(uintptr_t)args[1];
            }
        }
        else
        {
            flags = 0;
            if (args[1]) flags |= CREATE_EVENT_MANUAL_RESET;
            if (args[2]) flags |= CREATE_EVENT_INITIAL_SET;
            access = EVENT_ALL_ACCESS;
            if (args[3])
            {
                if (macrunner_hb_strieq( thunk->import_name, "CreateEventA" ))
                {
                    if (!macrunner_hb_ascii_to_wchar_buffer( (const char *)(uintptr_t)args[3],
                                                             name_buffer, ARRAY_SIZE(name_buffer) ))
                    {
                        RtlSetLastWin32Error( ERROR_FILENAME_EXCED_RANGE );
                        NtCurrentTeb()->LastStatusValue = STATUS_NAME_TOO_LONG;
                        *ret = 0;
                        return TRUE;
                    }
                    name = name_buffer;
                }
                else name = (const WCHAR *)(uintptr_t)args[3];
            }
        }

        if (sa || name)
        {
            macrunner_hb_init_named_object_attributes( &attr, &nameW, sa, name, TRUE, FALSE );
            attr_ptr = &attr;
        }
        status = NtCreateEvent( &handle, access, attr_ptr,
                                (flags & CREATE_EVENT_MANUAL_RESET) ? NotificationEvent : SynchronizationEvent,
                                (flags & CREATE_EVENT_INITIAL_SET) != 0 );
        NtCurrentTeb()->LastStatusValue = status;
        if (status == STATUS_OBJECT_NAME_EXISTS)
        {
            RtlSetLastWin32Error( ERROR_ALREADY_EXISTS );
            *ret = (uint64_t)(uintptr_t)handle;
        }
        else if (status)
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = 0;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            *ret = (uint64_t)(uintptr_t)handle;
        }
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            fprintf( stderr, "macrunner-hb-wait-semantic: event-create import=%s!%s pc=%p rsp=%p "
                     "handle=%p manual=%u initial=%u access=%#lx status=%08lx ret=%p last_error=%lu\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)ctx->regs.x64.rsp, (void *)(uintptr_t)handle,
                     (unsigned int)((flags & CREATE_EVENT_MANUAL_RESET) != 0),
                     (unsigned int)((flags & CREATE_EVENT_INITIAL_SET) != 0),
                     (unsigned long)access, (unsigned long)status, (void *)(uintptr_t)*ret,
                     (unsigned long)NtCurrentTeb()->LastErrorValue );
            fflush( stderr );
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "OpenEventA" ) ||
        macrunner_hb_strieq( thunk->import_name, "OpenEventW" ))
    {
        OBJECT_ATTRIBUTES attr;
        UNICODE_STRING nameW;
        WCHAR name_buffer[MAX_PATH];
        const WCHAR *name = NULL;
        HANDLE handle = 0;

        if (args[2])
        {
            if (macrunner_hb_strieq( thunk->import_name, "OpenEventA" ))
            {
                if (!macrunner_hb_ascii_to_wchar_buffer( (const char *)(uintptr_t)args[2],
                                                         name_buffer, ARRAY_SIZE(name_buffer) ))
                {
                    RtlSetLastWin32Error( ERROR_FILENAME_EXCED_RANGE );
                    NtCurrentTeb()->LastStatusValue = STATUS_NAME_TOO_LONG;
                    *ret = 0;
                    return TRUE;
                }
                name = name_buffer;
            }
            else name = (const WCHAR *)(uintptr_t)args[2];
        }
        else
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = 0;
            return TRUE;
        }

        macrunner_hb_init_named_object_attributes( &attr, &nameW, NULL, name, FALSE, args[1] != 0 );
        status = NtOpenEvent( &handle, (ACCESS_MASK)args[0], &attr );
        NtCurrentTeb()->LastStatusValue = status;
        if (status)
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = 0;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            *ret = (uint64_t)(uintptr_t)handle;
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "CreateSemaphoreA" ) ||
        macrunner_hb_strieq( thunk->import_name, "CreateSemaphoreW" ) ||
        macrunner_hb_strieq( thunk->import_name, "CreateSemaphoreExA" ) ||
        macrunner_hb_strieq( thunk->import_name, "CreateSemaphoreExW" ))
    {
        SECURITY_ATTRIBUTES *sa = (SECURITY_ATTRIBUTES *)(uintptr_t)args[0];
        OBJECT_ATTRIBUTES attr;
        OBJECT_ATTRIBUTES *attr_ptr = NULL;
        UNICODE_STRING nameW;
        WCHAR name_buffer[MAX_PATH];
        const WCHAR *name = NULL;
        HANDLE handle = 0;
        LONG initial = (LONG)args[1];
        LONG max = (LONG)args[2];
        DWORD access = SEMAPHORE_ALL_ACCESS;

        if (macrunner_hb_strieq( thunk->import_name, "CreateSemaphoreExA" ) ||
            macrunner_hb_strieq( thunk->import_name, "CreateSemaphoreExW" ))
        {
            access = (DWORD)args[5];
            if (args[3])
            {
                if (macrunner_hb_strieq( thunk->import_name, "CreateSemaphoreExA" ))
                {
                    if (!macrunner_hb_ascii_to_wchar_buffer( (const char *)(uintptr_t)args[3],
                                                             name_buffer, ARRAY_SIZE(name_buffer) ))
                    {
                        RtlSetLastWin32Error( ERROR_FILENAME_EXCED_RANGE );
                        NtCurrentTeb()->LastStatusValue = STATUS_NAME_TOO_LONG;
                        *ret = 0;
                        return TRUE;
                    }
                    name = name_buffer;
                }
                else name = (const WCHAR *)(uintptr_t)args[3];
            }
        }
        else if (args[3])
        {
            if (macrunner_hb_strieq( thunk->import_name, "CreateSemaphoreA" ))
            {
                if (!macrunner_hb_ascii_to_wchar_buffer( (const char *)(uintptr_t)args[3],
                                                         name_buffer, ARRAY_SIZE(name_buffer) ))
                {
                    RtlSetLastWin32Error( ERROR_FILENAME_EXCED_RANGE );
                    NtCurrentTeb()->LastStatusValue = STATUS_NAME_TOO_LONG;
                    *ret = 0;
                    return TRUE;
                }
                name = name_buffer;
            }
            else name = (const WCHAR *)(uintptr_t)args[3];
        }

        if (sa || name)
        {
            macrunner_hb_init_named_object_attributes( &attr, &nameW, sa, name, TRUE, FALSE );
            attr_ptr = &attr;
        }
        status = NtCreateSemaphore( &handle, access, attr_ptr, initial, max );
        NtCurrentTeb()->LastStatusValue = status;
        if (status == STATUS_OBJECT_NAME_EXISTS)
        {
            RtlSetLastWin32Error( ERROR_ALREADY_EXISTS );
            *ret = (uint64_t)(uintptr_t)handle;
        }
        else if (status)
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = 0;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            *ret = (uint64_t)(uintptr_t)handle;
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "OpenSemaphoreA" ) ||
        macrunner_hb_strieq( thunk->import_name, "OpenSemaphoreW" ))
    {
        OBJECT_ATTRIBUTES attr;
        UNICODE_STRING nameW;
        WCHAR name_buffer[MAX_PATH];
        const WCHAR *name = NULL;
        HANDLE handle = 0;

        if (args[2])
        {
            if (macrunner_hb_strieq( thunk->import_name, "OpenSemaphoreA" ))
            {
                if (!macrunner_hb_ascii_to_wchar_buffer( (const char *)(uintptr_t)args[2],
                                                         name_buffer, ARRAY_SIZE(name_buffer) ))
                {
                    RtlSetLastWin32Error( ERROR_FILENAME_EXCED_RANGE );
                    NtCurrentTeb()->LastStatusValue = STATUS_NAME_TOO_LONG;
                    *ret = 0;
                    return TRUE;
                }
                name = name_buffer;
            }
            else name = (const WCHAR *)(uintptr_t)args[2];
        }
        else
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = 0;
            return TRUE;
        }

        macrunner_hb_init_named_object_attributes( &attr, &nameW, NULL, name, FALSE, args[1] != 0 );
        status = NtOpenSemaphore( &handle, (ACCESS_MASK)args[0], &attr );
        NtCurrentTeb()->LastStatusValue = status;
        if (status)
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = 0;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            *ret = (uint64_t)(uintptr_t)handle;
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "ReleaseSemaphore" ))
    {
        ULONG previous = 0;

        status = NtReleaseSemaphore( (HANDLE)(uintptr_t)args[0], (ULONG)args[1],
                                     args[2] ? &previous : NULL );
        if (!status && args[2] &&
            hb_memory_write( ctx->memory, (hb_gva_t)args[2], &previous, sizeof(previous) ) != HB_OK)
            status = STATUS_INVALID_PARAMETER;
        NtCurrentTeb()->LastStatusValue = status;
        if (status)
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = FALSE;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            *ret = TRUE;
        }
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: event-signal import=%s!%s pc=%p rsp=%p "
                     "caller=%p handle=%p status=%08lx ret=%p last_error=%lu\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)ctx->regs.x64.rsp, (void *)(uintptr_t)caller,
                     (void *)(uintptr_t)args[0], (unsigned long)status, (void *)(uintptr_t)*ret,
                     (unsigned long)NtCurrentTeb()->LastErrorValue );
            fflush( stderr );
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "CreateMutexA" ) ||
        macrunner_hb_strieq( thunk->import_name, "CreateMutexW" ) ||
        macrunner_hb_strieq( thunk->import_name, "CreateMutexExA" ) ||
        macrunner_hb_strieq( thunk->import_name, "CreateMutexExW" ))
    {
        SECURITY_ATTRIBUTES *sa = (SECURITY_ATTRIBUTES *)(uintptr_t)args[0];
        OBJECT_ATTRIBUTES attr;
        OBJECT_ATTRIBUTES *attr_ptr = NULL;
        UNICODE_STRING nameW;
        WCHAR name_buffer[MAX_PATH];
        const WCHAR *name = NULL;
        HANDLE handle = 0;
        DWORD flags = 0;
        ACCESS_MASK access = MUTEX_ALL_ACCESS;

        if (macrunner_hb_strieq( thunk->import_name, "CreateMutexExA" ) ||
            macrunner_hb_strieq( thunk->import_name, "CreateMutexExW" ))
        {
            flags = (DWORD)args[2];
            access = (ACCESS_MASK)args[3];
            if (args[1])
            {
                if (macrunner_hb_strieq( thunk->import_name, "CreateMutexExA" ))
                {
                    if (!macrunner_hb_ascii_to_wchar_buffer( (const char *)(uintptr_t)args[1],
                                                             name_buffer, ARRAY_SIZE(name_buffer) ))
                    {
                        RtlSetLastWin32Error( ERROR_FILENAME_EXCED_RANGE );
                        NtCurrentTeb()->LastStatusValue = STATUS_NAME_TOO_LONG;
                        *ret = 0;
                        return TRUE;
                    }
                    name = name_buffer;
                }
                else name = (const WCHAR *)(uintptr_t)args[1];
            }
        }
        else
        {
            if (args[1]) flags |= CREATE_MUTEX_INITIAL_OWNER;
            if (args[2])
            {
                if (macrunner_hb_strieq( thunk->import_name, "CreateMutexA" ))
                {
                    if (!macrunner_hb_ascii_to_wchar_buffer( (const char *)(uintptr_t)args[2],
                                                             name_buffer, ARRAY_SIZE(name_buffer) ))
                    {
                        RtlSetLastWin32Error( ERROR_FILENAME_EXCED_RANGE );
                        NtCurrentTeb()->LastStatusValue = STATUS_NAME_TOO_LONG;
                        *ret = 0;
                        return TRUE;
                    }
                    name = name_buffer;
                }
                else name = (const WCHAR *)(uintptr_t)args[2];
            }
        }

        if (sa || name)
        {
            macrunner_hb_init_named_object_attributes( &attr, &nameW, sa, name, TRUE, FALSE );
            attr_ptr = &attr;
        }
        status = NtCreateMutant( &handle, access, attr_ptr,
                                 (flags & CREATE_MUTEX_INITIAL_OWNER) != 0 );
        NtCurrentTeb()->LastStatusValue = status;
        if (status == STATUS_OBJECT_NAME_EXISTS)
        {
            RtlSetLastWin32Error( ERROR_ALREADY_EXISTS );
            *ret = (uint64_t)(uintptr_t)handle;
        }
        else if (status)
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = 0;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            *ret = (uint64_t)(uintptr_t)handle;
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "OpenMutexA" ) ||
        macrunner_hb_strieq( thunk->import_name, "OpenMutexW" ))
    {
        OBJECT_ATTRIBUTES attr;
        UNICODE_STRING nameW;
        WCHAR name_buffer[MAX_PATH];
        const WCHAR *name = NULL;
        HANDLE handle = 0;

        if (args[2])
        {
            if (macrunner_hb_strieq( thunk->import_name, "OpenMutexA" ))
            {
                if (!macrunner_hb_ascii_to_wchar_buffer( (const char *)(uintptr_t)args[2],
                                                         name_buffer, ARRAY_SIZE(name_buffer) ))
                {
                    RtlSetLastWin32Error( ERROR_FILENAME_EXCED_RANGE );
                    NtCurrentTeb()->LastStatusValue = STATUS_NAME_TOO_LONG;
                    *ret = 0;
                    return TRUE;
                }
                name = name_buffer;
            }
            else name = (const WCHAR *)(uintptr_t)args[2];
        }
        else
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = 0;
            return TRUE;
        }

        macrunner_hb_init_named_object_attributes( &attr, &nameW, NULL, name, FALSE, args[1] != 0 );
        status = NtOpenMutant( &handle, (ACCESS_MASK)args[0], &attr );
        NtCurrentTeb()->LastStatusValue = status;
        if (status)
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = 0;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            *ret = (uint64_t)(uintptr_t)handle;
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "ReleaseMutex" ))
    {
        status = NtReleaseMutant( (HANDLE)(uintptr_t)args[0], NULL );
        NtCurrentTeb()->LastStatusValue = status;
        if (status)
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = FALSE;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            *ret = TRUE;
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "CreatePipe" ))
    {
        static unsigned int pipe_index;
        SECURITY_ATTRIBUTES sa_local;
        SECURITY_ATTRIBUTES *sa = NULL;
        OBJECT_ATTRIBUTES attr;
        UNICODE_STRING nt_name;
        IO_STATUS_BLOCK iosb;
        LARGE_INTEGER timeout;
        WCHAR name[64];
        HANDLE read_pipe = INVALID_HANDLE_VALUE;
        HANDLE write_pipe = INVALID_HANDLE_VALUE;
        DWORD size = (DWORD)args[3];

        if (!args[0] || !args[1] ||
            hb_memory_write( ctx->memory, (hb_gva_t)args[0], &read_pipe, sizeof(read_pipe) ) != HB_OK ||
            hb_memory_write( ctx->memory, (hb_gva_t)args[1], &write_pipe, sizeof(write_pipe) ) != HB_OK)
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = FALSE;
            return TRUE;
        }

        if (args[2])
        {
            if (hb_memory_read( ctx->memory, (hb_gva_t)args[2], &sa_local, sizeof(sa_local) ) != HB_OK)
            {
                RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
                NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
                *ret = FALSE;
                return TRUE;
            }
            sa = &sa_local;
        }

        if (!size) size = 4096;
        swprintf( name, ARRAY_SIZE(name), L"\\??\\pipe\\Win32.Pipes.%08lu.%08u",
                  GetCurrentProcessId(), ++pipe_index );
        RtlInitUnicodeString( &nt_name, name );
        InitializeObjectAttributes( &attr, &nt_name,
                                    OBJ_CASE_INSENSITIVE | ((sa && sa->bInheritHandle) ? OBJ_INHERIT : 0),
                                    0, sa ? sa->lpSecurityDescriptor : NULL );
        timeout.QuadPart = (ULONGLONG)NMPWAIT_USE_DEFAULT_WAIT * -10000;

        status = WINE_NT_CREATE_NAMED_PIPE_FILE( &read_pipe,
                                                 GENERIC_READ | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
                                                 &attr, &iosb, FILE_SHARE_WRITE, FILE_OPEN_IF,
                                                 FILE_SYNCHRONOUS_IO_NONALERT,
                                                 FALSE, FALSE, FALSE, 1, size, size, &timeout );
        if (!status)
            status = NtOpenFile( &write_pipe, GENERIC_WRITE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &attr,
                                 &iosb, 0, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE );
        if (!status &&
            (hb_memory_write( ctx->memory, (hb_gva_t)args[0], &read_pipe, sizeof(read_pipe) ) != HB_OK ||
             hb_memory_write( ctx->memory, (hb_gva_t)args[1], &write_pipe, sizeof(write_pipe) ) != HB_OK))
        {
            NtClose( read_pipe );
            NtClose( write_pipe );
            read_pipe = INVALID_HANDLE_VALUE;
            write_pipe = INVALID_HANDLE_VALUE;
            hb_memory_write( ctx->memory, (hb_gva_t)args[0], &read_pipe, sizeof(read_pipe) );
            hb_memory_write( ctx->memory, (hb_gva_t)args[1], &write_pipe, sizeof(write_pipe) );
            status = STATUS_INVALID_PARAMETER;
        }
        if (status)
        {
            if (read_pipe != INVALID_HANDLE_VALUE) NtClose( read_pipe );
            if (write_pipe != INVALID_HANDLE_VALUE) NtClose( write_pipe );
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            NtCurrentTeb()->LastStatusValue = status;
            *ret = FALSE;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
            *ret = TRUE;
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "SetEvent" ) ||
        macrunner_hb_strieq( thunk->import_name, "ResetEvent" ) ||
        macrunner_hb_strieq( thunk->import_name, "PulseEvent" ))
    {
        if (macrunner_hb_strieq( thunk->import_name, "SetEvent" ))
            status = NtSetEvent( (HANDLE)(uintptr_t)args[0], NULL );
        else if (macrunner_hb_strieq( thunk->import_name, "ResetEvent" ))
            status = NtResetEvent( (HANDLE)(uintptr_t)args[0], NULL );
        else
            status = NtPulseEvent( (HANDLE)(uintptr_t)args[0], NULL );

        NtCurrentTeb()->LastStatusValue = status;
        if (status)
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = FALSE;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            *ret = TRUE;
        }
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: event-signal import=%s!%s pc=%p "
                     "caller=%p rsp=%p handle=%p status=%08lx ret=%p last_error=%lu\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)caller, (void *)(uintptr_t)ctx->regs.x64.rsp,
                     (void *)(uintptr_t)args[0], (unsigned long)status, (void *)(uintptr_t)*ret,
                     (unsigned long)NtCurrentTeb()->LastErrorValue );
            fflush( stderr );
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "VirtualAlloc" ) ||
        macrunner_hb_strieq( thunk->import_name, "VirtualAllocEx" ))
    {
        HANDLE process = NtCurrentProcess();
        void *base;
        SIZE_T size;
        ULONG type, protect;

        if (macrunner_hb_strieq( thunk->import_name, "VirtualAllocEx" ))
        {
            process = (HANDLE)(uintptr_t)args[0];
            base = (void *)(uintptr_t)args[1];
            size = (SIZE_T)args[2];
            type = (ULONG)args[3];
            protect = (ULONG)args[4];
        }
        else
        {
            base = (void *)(uintptr_t)args[0];
            size = (SIZE_T)args[1];
            type = (ULONG)args[2];
            protect = (ULONG)args[3];
        }

        status = NtAllocateVirtualMemory( process, &base, 0, &size, type, protect );
        NtCurrentTeb()->LastStatusValue = status;
        if (status)
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = 0;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            *ret = (uint64_t)(uintptr_t)base;
            if (process == NtCurrentProcess() && (type & MEM_COMMIT))
            {
                macrunner_hb_remember_virtual_region( base, size, protect );
                macrunner_hb_sync_virtual_region( ctx, base, size, protect );
            }
            if (process == NtCurrentProcess())
                macrunner_hb_notify_xtajit64_memory_alloc( base, size, type, protect, status );
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "VirtualFree" ) ||
        macrunner_hb_strieq( thunk->import_name, "VirtualFreeEx" ))
    {
        HANDLE process = NtCurrentProcess();
        void *base, *original_base;
        SIZE_T size, original_size;
        ULONG type;

        if (macrunner_hb_strieq( thunk->import_name, "VirtualFreeEx" ))
        {
            process = (HANDLE)(uintptr_t)args[0];
            base = (void *)(uintptr_t)args[1];
            size = (SIZE_T)args[2];
            type = (ULONG)args[3];
        }
        else
        {
            base = (void *)(uintptr_t)args[0];
            size = (SIZE_T)args[1];
            type = (ULONG)args[2];
        }
        original_base = base;
        original_size = size;

        status = NtFreeVirtualMemory( process, &base, &size, type );
        NtCurrentTeb()->LastStatusValue = status;
        if (status)
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = FALSE;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            *ret = TRUE;
            if (process == NtCurrentProcess())
            {
                if (type & MEM_RELEASE)
                {
                    macrunner_hb_forget_virtual_region_record( original_base );
                    macrunner_hb_forget_virtual_region( ctx, original_base );
                    macrunner_hb_notify_xtajit64_memory_free( original_base, original_size, type, status );
                }
                else if ((type & MEM_DECOMMIT) && original_size)
                {
                    macrunner_hb_forget_virtual_region_record( original_base );
                    macrunner_hb_sync_virtual_region( ctx, original_base, original_size, PAGE_NOACCESS );
                    macrunner_hb_notify_xtajit64_memory_free( original_base, original_size, type, status );
                }
            }
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "VirtualProtect" ) ||
        macrunner_hb_strieq( thunk->import_name, "VirtualProtectEx" ))
    {
        HANDLE process = NtCurrentProcess();
        void *base;
        SIZE_T size;
        ULONG protect, old_protect = 0;
        uint64_t old_protect_guest;

        if (macrunner_hb_strieq( thunk->import_name, "VirtualProtectEx" ))
        {
            process = (HANDLE)(uintptr_t)args[0];
            base = (void *)(uintptr_t)args[1];
            size = (SIZE_T)args[2];
            protect = (ULONG)args[3];
            old_protect_guest = args[4];
        }
        else
        {
            base = (void *)(uintptr_t)args[0];
            size = (SIZE_T)args[1];
            protect = (ULONG)args[2];
            old_protect_guest = args[3];
        }

        status = old_protect_guest ? NtProtectVirtualMemory( process, &base, &size, protect, &old_protect )
                                   : STATUS_INVALID_PARAMETER;
        NtCurrentTeb()->LastStatusValue = status;
        if (status)
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = FALSE;
        }
        else
        {
            *(DWORD *)(uintptr_t)old_protect_guest = old_protect;
            RtlSetLastWin32Error( ERROR_SUCCESS );
            *ret = TRUE;
            if (process == NtCurrentProcess())
            {
                macrunner_hb_remember_virtual_region( base, size, protect );
                macrunner_hb_sync_virtual_region( ctx, base, size, protect );
                macrunner_hb_notify_xtajit64_memory_protect( base, size, protect, status );
            }
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "VirtualQuery" ) ||
        macrunner_hb_strieq( thunk->import_name, "VirtualQueryEx" ))
    {
        HANDLE process = NtCurrentProcess();
        const void *addr;
        MEMORY_BASIC_INFORMATION *info;
        SIZE_T length, result = 0;

        if (macrunner_hb_strieq( thunk->import_name, "VirtualQueryEx" ))
        {
            process = (HANDLE)(uintptr_t)args[0];
            addr = (const void *)(uintptr_t)args[1];
            info = (MEMORY_BASIC_INFORMATION *)(uintptr_t)args[2];
            length = (SIZE_T)args[3];
        }
        else
        {
            addr = (const void *)(uintptr_t)args[0];
            info = (MEMORY_BASIC_INFORMATION *)(uintptr_t)args[1];
            length = (SIZE_T)args[2];
        }

        status = NtQueryVirtualMemory( process, addr, MemoryBasicInformation, info, length, &result );
        NtCurrentTeb()->LastStatusValue = status;
        if (status)
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = 0;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            *ret = result;
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetProcessHeap" ))
    {
        PEB *peb = NtCurrentTeb()->Peb;

        *ret = (uint64_t)(uintptr_t)(peb ? peb->ProcessHeap : NULL);
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "HeapAlloc" ))
    {
        SIZE_T size = (SIZE_T)args[2];
        void *ptr = macrunner_hb_local_heap_alloc( size, !!((DWORD)args[1] & HEAP_ZERO_MEMORY) );

        if (ptr) macrunner_hb_sync_virtual_region( ctx, ptr, size ? size : 1, PAGE_READWRITE );
        *ret = (uint64_t)(uintptr_t)ptr;
        RtlSetLastWin32Error( ptr ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY );
        NtCurrentTeb()->LastStatusValue = ptr ? STATUS_SUCCESS : STATUS_NO_MEMORY;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "HeapReAlloc" ))
    {
        void *old_ptr = (void *)(uintptr_t)args[2];
        SIZE_T size = (SIZE_T)args[3];
        void *ptr;

        if (!old_ptr || !macrunner_hb_local_heap_contains( old_ptr ))
        {
            *ret = 0;
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            return TRUE;
        }
        ptr = macrunner_hb_local_heap_realloc( old_ptr, size, !!((DWORD)args[1] & HEAP_ZERO_MEMORY),
                                               !!((DWORD)args[1] & HEAP_REALLOC_IN_PLACE_ONLY) );
        if (ptr) macrunner_hb_sync_virtual_region( ctx, ptr, size ? size : 1, PAGE_READWRITE );
        *ret = (uint64_t)(uintptr_t)ptr;
        RtlSetLastWin32Error( ptr ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY );
        NtCurrentTeb()->LastStatusValue = ptr ? STATUS_SUCCESS : STATUS_NO_MEMORY;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "HeapFree" ))
    {
        void *ptr = (void *)(uintptr_t)args[2];

        if (ptr && !macrunner_hb_local_heap_contains( ptr ))
        {
            *ret = FALSE;
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            return TRUE;
        }
        if (ptr) macrunner_hb_local_heap_release( ptr );
        *ret = TRUE;
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "HeapSize" ))
    {
        SIZE_T size = 0;

        if (!args[2] || !macrunner_hb_local_heap_lookup( (void *)(uintptr_t)args[2], &size, NULL ))
        {
            *ret = ~(uint64_t)0;
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            return TRUE;
        }
        *ret = size;
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetCurrentProcess" ))
    {
        *ret = (uint64_t)(uintptr_t)(HANDLE)(intptr_t)-1;
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetCurrentThread" ))
    {
        *ret = (uint64_t)(uintptr_t)(HANDLE)(intptr_t)-2;
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetCurrentProcessId" ))
    {
        *ret = (DWORD)(ULONG_PTR)NtCurrentTeb()->ClientId.UniqueProcess;
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetCurrentThreadId" ))
    {
        *ret = (DWORD)(ULONG_PTR)NtCurrentTeb()->ClientId.UniqueThread;
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetTickCount" ))
    {
        *ret = NtGetTickCount();
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetTickCount64" ))
    {
        *ret = (ULONGLONG)NtGetTickCount();
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "InitializeCriticalSection" ) ||
        macrunner_hb_strieq( thunk->import_name, "InitializeCriticalSectionAndSpinCount" ) ||
        macrunner_hb_strieq( thunk->import_name, "InitializeCriticalSectionEx" ))
    {
        RTL_CRITICAL_SECTION *cs = (RTL_CRITICAL_SECTION *)(uintptr_t)args[0];
        DWORD spin = 0, flags = 0;

        if (macrunner_hb_strieq( thunk->import_name, "InitializeCriticalSectionAndSpinCount" ))
            spin = (DWORD)args[1];
        else if (macrunner_hb_strieq( thunk->import_name, "InitializeCriticalSectionEx" ))
        {
            spin = (DWORD)args[1];
            flags = (DWORD)args[2];
        }
        (void)flags;
        status = macrunner_hb_critical_section_init( cs, spin );

        NtCurrentTeb()->LastStatusValue = status;
        RtlSetLastWin32Error( status ? RtlNtStatusToDosError( status ) : ERROR_SUCCESS );
        *ret = macrunner_hb_strieq( thunk->import_name, "InitializeCriticalSection" ) ? 0 : !status;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "DeleteCriticalSection" ))
    {
        RTL_CRITICAL_SECTION *cs = (RTL_CRITICAL_SECTION *)(uintptr_t)args[0];

        status = macrunner_hb_critical_section_delete( cs );
        NtCurrentTeb()->LastStatusValue = status;
        RtlSetLastWin32Error( status ? RtlNtStatusToDosError( status ) : ERROR_SUCCESS );
        *ret = 0;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "EnterCriticalSection" ))
    {
        RTL_CRITICAL_SECTION *cs = (RTL_CRITICAL_SECTION *)(uintptr_t)args[0];

        status = macrunner_hb_critical_section_enter( cs );
        NtCurrentTeb()->LastStatusValue = status;
        RtlSetLastWin32Error( status ? RtlNtStatusToDosError( status ) : ERROR_SUCCESS );
        *ret = 0;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "LeaveCriticalSection" ))
    {
        RTL_CRITICAL_SECTION *cs = (RTL_CRITICAL_SECTION *)(uintptr_t)args[0];

        status = macrunner_hb_critical_section_leave( cs );
        NtCurrentTeb()->LastStatusValue = status;
        RtlSetLastWin32Error( status ? RtlNtStatusToDosError( status ) : ERROR_SUCCESS );
        *ret = 0;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "TryEnterCriticalSection" ))
    {
        RTL_CRITICAL_SECTION *cs = (RTL_CRITICAL_SECTION *)(uintptr_t)args[0];

        *ret = macrunner_hb_critical_section_try_enter( cs );
        RtlSetLastWin32Error( cs ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER );
        NtCurrentTeb()->LastStatusValue = cs ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "SetCriticalSectionSpinCount" ))
    {
        RTL_CRITICAL_SECTION *cs = (RTL_CRITICAL_SECTION *)(uintptr_t)args[0];

        if (cs)
        {
            ULONG old_spin = cs->SpinCount;
            cs->SpinCount = (DWORD)args[1];
            *ret = old_spin;
        }
        else *ret = 0;
        RtlSetLastWin32Error( cs ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER );
        NtCurrentTeb()->LastStatusValue = cs ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "InitializeSListHead" ))
    {
        macrunner_hb_guest_slist_initialize( ctx, (void *)(uintptr_t)args[0] );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = 0;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "InterlockedFlushSList" ))
    {
        *ret = macrunner_hb_guest_slist_flush( ctx, (void *)(uintptr_t)args[0] );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "InterlockedPopEntrySList" ))
    {
        *ret = macrunner_hb_guest_slist_pop( ctx, (void *)(uintptr_t)args[0] );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "InterlockedPushEntrySList" ))
    {
        *ret = macrunner_hb_guest_slist_push( ctx, (void *)(uintptr_t)args[0],
                                              (void *)(uintptr_t)args[1] );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "InterlockedPushListSList" ) ||
        macrunner_hb_strieq( thunk->import_name, "InterlockedPushListSListEx" ))
    {
        *ret = macrunner_hb_guest_slist_push_list( ctx, (void *)(uintptr_t)args[0],
                                                   (void *)(uintptr_t)args[1],
                                                   (void *)(uintptr_t)args[2],
                                                   (ULONG)args[3] );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "QueryDepthSList" ))
    {
        pthread_mutex_lock( &macrunner_hb_slist_mutex );
        *ret = macrunner_hb_guest_slist_depth( ctx, (void *)(uintptr_t)args[0] );
        pthread_mutex_unlock( &macrunner_hb_slist_mutex );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "TlsAlloc" ) ||
        macrunner_hb_strieq( thunk->import_name, "FlsAlloc" ))
    {
        unsigned char *slots = macrunner_hb_strieq( thunk->import_name, "TlsAlloc" ) ?
                               macrunner_hb_tls_slots : macrunner_hb_fls_slots;
        uint64_t *callbacks = macrunner_hb_strieq( thunk->import_name, "FlsAlloc" ) ?
                              macrunner_hb_fls_callbacks : NULL;
        DWORD index = TLS_OUT_OF_INDEXES;
        DWORD i;

        pthread_mutex_lock( &macrunner_hb_tls_mutex );
        for (i = 0; i < MACRUNNER_HB_TLS_SLOT_MAX; i++)
        {
            if (slots[i]) continue;
            slots[i] = 1;
            if (callbacks) callbacks[i] = args[0];
            index = i;
            break;
        }
        if (index != TLS_OUT_OF_INDEXES &&
            macrunner_hb_strieq( thunk->import_name, "TlsAlloc" ) &&
            !macrunner_hb_teb_tls_set_value( index, 0 ))
        {
            slots[index] = 0;
            index = TLS_OUT_OF_INDEXES;
        }
        pthread_mutex_unlock( &macrunner_hb_tls_mutex );
        *ret = index;
        RtlSetLastWin32Error( index == TLS_OUT_OF_INDEXES ? ERROR_NOT_ENOUGH_MEMORY : ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = index == TLS_OUT_OF_INDEXES ? STATUS_NO_MEMORY : STATUS_SUCCESS;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "TlsSetValue" ) ||
        macrunner_hb_strieq( thunk->import_name, "FlsSetValue" ))
    {
        DWORD index = (DWORD)args[0];
        unsigned char *slots = macrunner_hb_strieq( thunk->import_name, "TlsSetValue" ) ?
                               macrunner_hb_tls_slots : macrunner_hb_fls_slots;
        struct macrunner_hb_tls_thread_values *thread_values;
        uint64_t *values;

        pthread_mutex_lock( &macrunner_hb_tls_mutex );
        if (index >= MACRUNNER_HB_TLS_SLOT_MAX || !slots[index])
        {
            pthread_mutex_unlock( &macrunner_hb_tls_mutex );
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = FALSE;
            return TRUE;
        }
        if (!(thread_values = macrunner_hb_tls_values_for_current_thread( TRUE )))
        {
            pthread_mutex_unlock( &macrunner_hb_tls_mutex );
            RtlSetLastWin32Error( ERROR_NOT_ENOUGH_MEMORY );
            NtCurrentTeb()->LastStatusValue = STATUS_NO_MEMORY;
            *ret = FALSE;
            return TRUE;
        }
        values = macrunner_hb_strieq( thunk->import_name, "TlsSetValue" ) ?
                 thread_values->tls_values : thread_values->fls_values;
        values[index] = args[1];
        if (macrunner_hb_strieq( thunk->import_name, "TlsSetValue" ) &&
            !macrunner_hb_teb_tls_set_value( index, args[1] ))
        {
            pthread_mutex_unlock( &macrunner_hb_tls_mutex );
            RtlSetLastWin32Error( ERROR_NOT_ENOUGH_MEMORY );
            NtCurrentTeb()->LastStatusValue = STATUS_NO_MEMORY;
            *ret = FALSE;
            return TRUE;
        }
        pthread_mutex_unlock( &macrunner_hb_tls_mutex );
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = TRUE;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "TlsGetValue" ) ||
        macrunner_hb_strieq( thunk->import_name, "FlsGetValue" ))
    {
        DWORD index = (DWORD)args[0];
        unsigned char *slots = macrunner_hb_strieq( thunk->import_name, "TlsGetValue" ) ?
                               macrunner_hb_tls_slots : macrunner_hb_fls_slots;
        struct macrunner_hb_tls_thread_values *thread_values;
        uint64_t value = 0;

        pthread_mutex_lock( &macrunner_hb_tls_mutex );
        if (index >= MACRUNNER_HB_TLS_SLOT_MAX || !slots[index])
        {
            pthread_mutex_unlock( &macrunner_hb_tls_mutex );
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = 0;
            return TRUE;
        }
        thread_values = macrunner_hb_tls_values_for_current_thread( FALSE );
        if (thread_values)
        {
            uint64_t *values = macrunner_hb_strieq( thunk->import_name, "TlsGetValue" ) ?
                               thread_values->tls_values : thread_values->fls_values;
            value = values[index];
        }
        if (macrunner_hb_strieq( thunk->import_name, "TlsGetValue" ))
            macrunner_hb_teb_tls_get_value( index, &value );
        pthread_mutex_unlock( &macrunner_hb_tls_mutex );
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = value;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "TlsFree" ) ||
        macrunner_hb_strieq( thunk->import_name, "FlsFree" ))
    {
        DWORD index = (DWORD)args[0];
        unsigned char *slots = macrunner_hb_strieq( thunk->import_name, "TlsFree" ) ?
                               macrunner_hb_tls_slots : macrunner_hb_fls_slots;
        BOOL tls = macrunner_hb_strieq( thunk->import_name, "TlsFree" );
        unsigned int i;

        pthread_mutex_lock( &macrunner_hb_tls_mutex );
        if (index >= MACRUNNER_HB_TLS_SLOT_MAX || !slots[index])
        {
            pthread_mutex_unlock( &macrunner_hb_tls_mutex );
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = FALSE;
            return TRUE;
        }
        slots[index] = 0;
        macrunner_hb_fls_callbacks[index] = 0;
        if (tls) virtual_clear_tls_index( index );
        for (i = 0; i < MACRUNNER_HB_TLS_THREAD_MAX; i++)
        {
            if (!macrunner_hb_tls_thread_values[i].tid) continue;
            if (tls) macrunner_hb_tls_thread_values[i].tls_values[index] = 0;
            else macrunner_hb_tls_thread_values[i].fls_values[index] = 0;
        }
        pthread_mutex_unlock( &macrunner_hb_tls_mutex );
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = TRUE;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "LocalAlloc" ))
    {
        UINT flags = (UINT)args[0];
        SIZE_T size = (SIZE_T)args[1];
        void *ptr = macrunner_hb_local_heap_alloc( size, !!(flags & LMEM_ZEROINIT) );

        *ret = (uint64_t)(uintptr_t)ptr;
        RtlSetLastWin32Error( ptr ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY );
        NtCurrentTeb()->LastStatusValue = ptr ? STATUS_SUCCESS : STATUS_NO_MEMORY;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "LocalReAlloc" ))
    {
        void *old_ptr = (void *)(uintptr_t)args[0];
        SIZE_T size = (SIZE_T)args[1];
        UINT flags = (UINT)args[2];
        void *ptr;

        if (!old_ptr || !macrunner_hb_local_heap_contains( old_ptr )) return FALSE;
        ptr = macrunner_hb_local_heap_realloc( old_ptr, size, !!(flags & LMEM_ZEROINIT),
                                               FALSE );
        *ret = (uint64_t)(uintptr_t)ptr;
        RtlSetLastWin32Error( ptr ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY );
        NtCurrentTeb()->LastStatusValue = ptr ? STATUS_SUCCESS : STATUS_NO_MEMORY;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "LocalFree" ))
    {
        void *ptr = (void *)(uintptr_t)args[0];

        if (!ptr)
        {
            *ret = 0;
            RtlSetLastWin32Error( ERROR_SUCCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
            return TRUE;
        }
        if (!macrunner_hb_local_heap_contains( ptr )) return FALSE;
        macrunner_hb_local_heap_release( ptr );
        *ret = 0;
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "LocalSize" ))
    {
        SIZE_T size;

        if (!args[0] || !macrunner_hb_local_heap_lookup( (void *)(uintptr_t)args[0], &size, NULL ))
            return FALSE;
        *ret = size;
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "WaitForSingleObject" ) ||
        macrunner_hb_strieq( thunk->import_name, "WaitForSingleObjectEx" ))
    {
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: before import=%s!%s pc=%p caller=%p rsp=%p "
                     "handle=%p timeout_ms=%lu alertable=%u\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)caller,
                     (void *)(uintptr_t)ctx->regs.x64.rsp, (void *)(uintptr_t)args[0],
                     (unsigned long)(DWORD)args[1],
                     (unsigned int)(macrunner_hb_strieq( thunk->import_name,
                                                         "WaitForSingleObjectEx" ) && args[2]) );
            fflush( stderr );
        }
        status = NtWaitForSingleObject( (HANDLE)(uintptr_t)args[0],
                                        macrunner_hb_strieq( thunk->import_name, "WaitForSingleObjectEx" ) &&
                                        args[2],
                                        macrunner_hb_get_nt_timeout( &timeout, (DWORD)args[1] ) );
        NtCurrentTeb()->LastStatusValue = status;
        if (NT_ERROR( status ))
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = WAIT_FAILED;
        }
        else *ret = status;
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: after import=%s!%s pc=%p caller=%p rsp=%p "
                     "handle=%p timeout_ms=%lu status=%08lx ret=%p last_error=%lu\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)caller,
                     (void *)(uintptr_t)ctx->regs.x64.rsp, (void *)(uintptr_t)args[0],
                     (unsigned long)(DWORD)args[1], (unsigned long)status,
                     (void *)(uintptr_t)*ret, (unsigned long)NtCurrentTeb()->LastErrorValue );
            fflush( stderr );
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "WaitForMultipleObjects" ) ||
        macrunner_hb_strieq( thunk->import_name, "WaitForMultipleObjectsEx" ))
    {
        HANDLE handles[MAXIMUM_WAIT_OBJECTS];
        DWORD count = (DWORD)args[0];

        if (!count || count > MAXIMUM_WAIT_OBJECTS ||
            hb_memory_read( ctx->memory, (hb_gva_t)args[1], handles, count * sizeof(handles[0]) ) != HB_OK)
        {
            if (macrunner_hb_trace_wait_semantic_budget_allows())
            {
                fprintf( stderr, "macrunner-hb-wait-semantic: invalid import=%s!%s pc=%p rsp=%p "
                         "count=%lu handles_gva=%p wait_all=%u timeout_ms=%lu\n",
                         thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                         (void *)(uintptr_t)ctx->regs.x64.rsp, (unsigned long)count,
                         (void *)(uintptr_t)args[1], (unsigned int)args[2],
                         (unsigned long)(DWORD)args[3] );
                fflush( stderr );
            }
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = WAIT_FAILED;
            return TRUE;
        }
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: before import=%s!%s pc=%p caller=%p rsp=%p "
                     "count=%lu handles_gva=%p handles=%p,%p,%p,%p wait_all=%u timeout_ms=%lu alertable=%u\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)caller,
                     (void *)(uintptr_t)ctx->regs.x64.rsp, (unsigned long)count,
                     (void *)(uintptr_t)args[1], count > 0 ? handles[0] : NULL,
                     count > 1 ? handles[1] : NULL, count > 2 ? handles[2] : NULL,
                     count > 3 ? handles[3] : NULL, (unsigned int)args[2],
                     (unsigned long)(DWORD)args[3],
                     (unsigned int)(macrunner_hb_strieq( thunk->import_name,
                                                         "WaitForMultipleObjectsEx" ) && args[4]) );
            fflush( stderr );
        }
        status = NtWaitForMultipleObjects( count, handles, args[2] ? WaitAll : WaitAny,
                                           macrunner_hb_strieq( thunk->import_name, "WaitForMultipleObjectsEx" ) &&
                                           args[4],
                                           macrunner_hb_get_nt_timeout( &timeout, (DWORD)args[3] ) );
        NtCurrentTeb()->LastStatusValue = status;
        if (NT_ERROR( status ))
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = WAIT_FAILED;
        }
        else *ret = status;
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: after import=%s!%s pc=%p caller=%p rsp=%p "
                     "count=%lu wait_all=%u timeout_ms=%lu status=%08lx ret=%p last_error=%lu\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)caller,
                     (void *)(uintptr_t)ctx->regs.x64.rsp, (unsigned long)count,
                     (unsigned int)args[2], (unsigned long)(DWORD)args[3], (unsigned long)status,
                     (void *)(uintptr_t)*ret, (unsigned long)NtCurrentTeb()->LastErrorValue );
            fflush( stderr );
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "DuplicateHandle" ))
    {
        HANDLE source_process = (HANDLE)(uintptr_t)args[0];
        HANDLE source_handle = (HANDLE)(uintptr_t)args[1];
        HANDLE target_process = (HANDLE)(uintptr_t)args[2];
        hb_gva_t target_handle_gva = (hb_gva_t)args[3];
        ACCESS_MASK desired_access = (ACCESS_MASK)(DWORD)args[4];
        ULONG attributes = args[5] ? OBJ_INHERIT : 0;
        ULONG options = (ULONG)(DWORD)args[6];
        HANDLE duplicate = NULL;
        uint64_t local_duplicate = 0;

        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            fprintf( stderr, "macrunner-hb-wait-semantic: dup-handle before import=%s!%s pc=%p rsp=%p "
                     "source_process=%p source=%p target_process=%p target_gva=%p access=%08lx inherit=%u options=%08lx\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)ctx->regs.x64.rsp, source_process, source_handle,
                     target_process, (void *)(uintptr_t)target_handle_gva,
                     (unsigned long)desired_access, (unsigned int)args[5], (unsigned long)options );
            fflush( stderr );
        }

        if (!target_handle_gva && !(options & DUPLICATE_CLOSE_SOURCE))
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = FALSE;
            return TRUE;
        }

        if ((source_process == NtCurrentProcess() || source_process == (HANDLE)(intptr_t)-1) &&
            (target_process == NtCurrentProcess() || target_process == (HANDLE)(intptr_t)-1) &&
            macrunner_hb_local_file_duplicate( (uint64_t)(uintptr_t)source_handle, &local_duplicate ))
        {
            if (target_handle_gva &&
                hb_memory_write_u64( ctx->memory, target_handle_gva, local_duplicate ) != HB_OK)
            {
                macrunner_hb_local_file_close( local_duplicate );
                RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
                NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
                *ret = FALSE;
                return TRUE;
            }
            if (options & DUPLICATE_CLOSE_SOURCE)
                macrunner_hb_local_file_close( (uint64_t)(uintptr_t)source_handle );
            RtlSetLastWin32Error( ERROR_SUCCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
            *ret = TRUE;
            return TRUE;
        }

        status = NtDuplicateObject( source_process, source_handle, target_process,
                                    target_handle_gva ? &duplicate : NULL, desired_access,
                                    attributes, options );
        NtCurrentTeb()->LastStatusValue = status;
        if (!status && target_handle_gva &&
            hb_memory_write_u64( ctx->memory, target_handle_gva,
                                 (uint64_t)(uintptr_t)duplicate ) != HB_OK)
        {
            NtClose( duplicate );
            status = STATUS_ACCESS_VIOLATION;
            NtCurrentTeb()->LastStatusValue = status;
        }
        if (NT_ERROR( status ))
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = FALSE;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            *ret = TRUE;
        }
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            fprintf( stderr, "macrunner-hb-wait-semantic: dup-handle after import=%s!%s pc=%p rsp=%p "
                     "status=%08lx duplicate=%p ret=%p last_error=%lu\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)ctx->regs.x64.rsp, (unsigned long)status,
                     duplicate, (void *)(uintptr_t)*ret,
                     (unsigned long)NtCurrentTeb()->LastErrorValue );
            fflush( stderr );
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "CloseHandle" ))
    {
        if (macrunner_hb_local_file_close( args[0] ))
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
            *ret = TRUE;
            return TRUE;
        }
        status = NtClose( (HANDLE)(uintptr_t)args[0] );
        NtCurrentTeb()->LastStatusValue = status;
        if (NT_ERROR( status ))
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = FALSE;
        }
        else *ret = TRUE;
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            fprintf( stderr, "macrunner-hb-wait-semantic: close import=%s!%s pc=%p rsp=%p "
                     "handle=%p status=%08lx ret=%p last_error=%lu\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)ctx->regs.x64.rsp, (void *)(uintptr_t)args[0],
                     (unsigned long)status, (void *)(uintptr_t)*ret,
                     (unsigned long)NtCurrentTeb()->LastErrorValue );
            fflush( stderr );
        }
        return TRUE;
    }

    return FALSE;
}

static BOOL macrunner_hb_try_registry_semantic( hb_context_t *ctx,
                                                const struct macrunner_hb_import_thunk *thunk,
                                                const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                uint64_t *ret )
{
    static uint64_t next_handle = 0x00006f00f0000000ULL;
    const char *name;

    if (!ctx || !ctx->memory || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_strieq( thunk->dll_name, "advapi32.dll" )) return FALSE;
    name = thunk->import_name;

    if (macrunner_hb_strieq( name, "RegCloseKey" ) ||
        macrunner_hb_strieq( name, "RegSetValueExW" ) ||
        macrunner_hb_strieq( name, "RegDeleteValueW" ))
    {
        *ret = ERROR_SUCCESS;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "RegOpenKeyW" ))
    {
        if (args[2]) hb_memory_write_u64( ctx->memory, (hb_gva_t)args[2], next_handle++ );
        *ret = ERROR_SUCCESS;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "RegOpenKeyExW" ))
    {
        if (args[4]) hb_memory_write_u64( ctx->memory, (hb_gva_t)args[4], next_handle++ );
        *ret = ERROR_SUCCESS;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "RegCreateKeyExW" ))
    {
        if (args[7]) hb_memory_write_u64( ctx->memory, (hb_gva_t)args[7], next_handle++ );
        if (args[8]) hb_memory_write_u32( ctx->memory, (hb_gva_t)args[8], REG_CREATED_NEW_KEY );
        *ret = ERROR_SUCCESS;
        return TRUE;
    }
    if (macrunner_hb_strieq( name, "RegQueryValueExW" ))
    {
        if (args[4]) hb_memory_write_u32( ctx->memory, (hb_gva_t)args[4], REG_SZ );
        if (args[5]) hb_memory_write_u32( ctx->memory, (hb_gva_t)args[5], 0 );
        *ret = ERROR_FILE_NOT_FOUND;
        return TRUE;
    }

    return FALSE;
}

static BOOL macrunner_hb_try_security_token_semantic( hb_context_t *ctx,
                                                      const struct macrunner_hb_import_thunk *thunk,
                                                      const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                      uint64_t *ret )
{
    NTSTATUS status;

    if (!ctx || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_strieq( thunk->dll_name, "advapi32.dll" )) return FALSE;

    if (macrunner_hb_strieq( thunk->import_name, "OpenProcessToken" ) ||
        macrunner_hb_strieq( thunk->import_name, "OpenThreadToken" ))
    {
        HANDLE handle = NULL;
        uint64_t value;
        hb_gva_t out = (hb_gva_t)(macrunner_hb_strieq( thunk->import_name, "OpenProcessToken" ) ?
                                  args[2] : args[3]);

        if (!out)
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = FALSE;
            return TRUE;
        }
        if (macrunner_hb_strieq( thunk->import_name, "OpenProcessToken" ))
            status = NtOpenProcessToken( (HANDLE)(uintptr_t)args[0], (DWORD)args[1], &handle );
        else
            status = NtOpenThreadToken( (HANDLE)(uintptr_t)args[0], (DWORD)args[1], !!args[2], &handle );
        NtCurrentTeb()->LastStatusValue = status;
        if (NT_ERROR( status ))
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = FALSE;
            return TRUE;
        }
        value = (uint64_t)(uintptr_t)handle;
        if (hb_memory_write_u64( ctx->memory, out, value ) != HB_OK)
        {
            NtClose( handle );
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = FALSE;
            return TRUE;
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = TRUE;
        TRACE( "MacRunner HyperBridge semantic %s!%s access=0x%lx handle=%p\n",
               thunk->dll_name, thunk->import_name, (unsigned long)(DWORD)args[1], handle );
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "GetTokenInformation" ))
    {
        TOKEN_INFORMATION_CLASS class = (TOKEN_INFORMATION_CLASS)args[1];
        ULONG length = (ULONG)args[3], needed = 0;
        void *buffer = NULL;

        if (length > 1024 * 1024)
        {
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = FALSE;
            return TRUE;
        }
        if (length && !args[2])
        {
            RtlSetLastWin32Error( ERROR_NOACCESS );
            NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
            *ret = FALSE;
            return TRUE;
        }
        if (length && !(buffer = malloc( length )))
        {
            RtlSetLastWin32Error( ERROR_NOT_ENOUGH_MEMORY );
            NtCurrentTeb()->LastStatusValue = STATUS_NO_MEMORY;
            *ret = FALSE;
            return TRUE;
        }

        status = NtQueryInformationToken( (HANDLE)(uintptr_t)args[0], class, buffer, length, &needed );
        if (!NT_ERROR( status ) && buffer && length >= sizeof(uint64_t) &&
            (class == TokenUser || class == TokenOwner || class == TokenPrimaryGroup ||
             class == TokenIntegrityLevel || class == TokenAppContainerSid))
        {
            uintptr_t sid;
            uintptr_t base = (uintptr_t)buffer;

            memcpy( &sid, buffer, sizeof(sid) );
            if (sid >= base && sid < base + length)
            {
                sid = (uintptr_t)args[2] + sid - base;
                memcpy( buffer, &sid, sizeof(sid) );
            }
        }
        else if (!NT_ERROR( status ) && buffer &&
                 (class == TokenGroups || class == TokenRestrictedSids) && length >= sizeof(DWORD))
        {
            DWORD group_count = 0, i;

            memcpy( &group_count, buffer, sizeof(group_count) );
            for (i = 0; i < group_count; i++)
            {
                size_t ptr_offset = sizeof(DWORD) + i * sizeof(SID_AND_ATTRIBUTES);
                uintptr_t sid;
                uintptr_t base = (uintptr_t)buffer;

                if (ptr_offset + sizeof(sid) > length) break;
                memcpy( &sid, (BYTE *)buffer + ptr_offset, sizeof(sid) );
                if (sid >= base && sid < base + length)
                {
                    sid = (uintptr_t)args[2] + sid - base;
                    memcpy( (BYTE *)buffer + ptr_offset, &sid, sizeof(sid) );
                }
            }
        }
        if (!NT_ERROR( status ) && buffer &&
            hb_memory_write( ctx->memory, (hb_gva_t)args[2], buffer, length ) != HB_OK)
            status = STATUS_ACCESS_VIOLATION;
        free( buffer );
        if (args[4] && hb_memory_write_u32( ctx->memory, (hb_gva_t)args[4], needed ) != HB_OK)
            status = STATUS_ACCESS_VIOLATION;

        NtCurrentTeb()->LastStatusValue = status;
        if (NT_ERROR( status ))
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = FALSE;
        }
        else
        {
            RtlSetLastWin32Error( ERROR_SUCCESS );
            *ret = TRUE;
        }
        TRACE( "MacRunner HyperBridge semantic %s!%s class=%u length=%lu needed=%lu status=%08lx\n",
               thunk->dll_name, thunk->import_name, (unsigned int)class,
               (unsigned long)length, (unsigned long)needed, (unsigned long)status );
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "IsValidSid" ) ||
        macrunner_hb_strieq( thunk->import_name, "EqualSid" ) ||
        macrunner_hb_strieq( thunk->import_name, "GetLengthSid" ) ||
        macrunner_hb_strieq( thunk->import_name, "GetSidIdentifierAuthority" ) ||
        macrunner_hb_strieq( thunk->import_name, "GetSidSubAuthority" ) ||
        macrunner_hb_strieq( thunk->import_name, "GetSidSubAuthorityCount" ))
    {
        uint8_t rev = 0, count = 0, count2 = 0;
        hb_gva_t sid = (hb_gva_t)args[0];
        ULONG length;

        if (!sid ||
            hb_memory_read_u8( ctx->memory, sid, &rev ) != HB_OK ||
            hb_memory_read_u8( ctx->memory, sid + 1, &count ) != HB_OK ||
            rev != SID_REVISION || count > SID_MAX_SUB_AUTHORITIES)
        {
            *ret = macrunner_hb_strieq( thunk->import_name, "IsValidSid" ) ? FALSE : 0;
            return TRUE;
        }
        length = 8 + (ULONG)count * sizeof(DWORD);
        if (count && hb_memory_read_u8( ctx->memory, sid + length - 1, &rev ) != HB_OK)
        {
            *ret = macrunner_hb_strieq( thunk->import_name, "IsValidSid" ) ? FALSE : 0;
            return TRUE;
        }

        if (macrunner_hb_strieq( thunk->import_name, "IsValidSid" ))
            *ret = TRUE;
        else if (macrunner_hb_strieq( thunk->import_name, "GetLengthSid" ))
            *ret = length;
        else if (macrunner_hb_strieq( thunk->import_name, "GetSidIdentifierAuthority" ))
            *ret = sid + 2;
        else if (macrunner_hb_strieq( thunk->import_name, "GetSidSubAuthorityCount" ))
            *ret = sid + 1;
        else if (macrunner_hb_strieq( thunk->import_name, "GetSidSubAuthority" ))
        {
            DWORD index = (DWORD)args[1];
            *ret = index < count ? sid + 8 + (uint64_t)index * sizeof(DWORD) : 0;
        }
        else
        {
            hb_gva_t other = (hb_gva_t)args[1];
            BYTE left[8 + SID_MAX_SUB_AUTHORITIES * sizeof(DWORD)];
            BYTE right[8 + SID_MAX_SUB_AUTHORITIES * sizeof(DWORD)];

            if (!other ||
                hb_memory_read_u8( ctx->memory, other, &rev ) != HB_OK ||
                hb_memory_read_u8( ctx->memory, other + 1, &count2 ) != HB_OK ||
                rev != SID_REVISION || count2 != count ||
                hb_memory_read( ctx->memory, sid, left, length ) != HB_OK ||
                hb_memory_read( ctx->memory, other, right, length ) != HB_OK)
                *ret = FALSE;
            else
                *ret = !memcmp( left, right, length );
        }
        RtlSetLastWin32Error( ERROR_SUCCESS );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        TRACE( "MacRunner HyperBridge semantic %s!%s sid=%p count=%u ret=%p\n",
               thunk->dll_name, thunk->import_name, (void *)(uintptr_t)sid, count,
               (void *)(uintptr_t)*ret );
        return TRUE;
    }

    return FALSE;
}

static BOOL macrunner_hb_try_bcrypt_semantic( hb_context_t *ctx,
                                              const struct macrunner_hb_import_thunk *thunk,
                                              const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                              uint64_t *ret )
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG count;
    ULONG flags;
    int fd;

    if (!ctx || !thunk || !args || !ret) return FALSE;
    if (!macrunner_hb_strieq( thunk->dll_name, "bcrypt.dll" ) ||
        !macrunner_hb_strieq( thunk->import_name, "BCryptGenRandom" ))
        return FALSE;

    count = (ULONG)args[2];
    flags = (ULONG)args[3];
    if (!args[0] && !(flags & MACRUNNER_HB_BCRYPT_USE_SYSTEM_PREFERRED_RNG))
        status = STATUS_INVALID_HANDLE;
    else if (args[0] && args[0] != MACRUNNER_HB_BCRYPT_RNG_ALG_HANDLE &&
             !(flags & MACRUNNER_HB_BCRYPT_USE_SYSTEM_PREFERRED_RNG))
        status = STATUS_NOT_IMPLEMENTED;
    else if (!args[1])
        status = STATUS_INVALID_PARAMETER;
    else if (count)
    {
        fd = open( "/dev/urandom", O_RDONLY );
        if (fd == -1) status = STATUS_UNSUCCESSFUL;
        else
        {
            UCHAR buffer[256];
            ULONG offset = 0;

            while (offset < count)
            {
                ULONG chunk = count - offset;
                ssize_t got;

                if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
                do
                {
                    got = read( fd, buffer, chunk );
                }
                while (got == -1 && errno == EINTR);
                if (got <= 0)
                {
                    status = STATUS_UNSUCCESSFUL;
                    break;
                }
                if (hb_memory_write( ctx->memory, (hb_gva_t)(args[1] + offset),
                                     buffer, (size_t)got ) != HB_OK)
                {
                    status = STATUS_INVALID_PARAMETER;
                    break;
                }
                offset += (ULONG)got;
            }
            close( fd );
        }
    }

    NtCurrentTeb()->LastStatusValue = status;
    *ret = (uint64_t)(uint32_t)status;
    return TRUE;
}

struct macrunner_hb_vectored_handler
{
    uint64_t handle;
    uint64_t func;
};

#define MACRUNNER_HB_VECTORED_MAX 32

static pthread_mutex_t macrunner_hb_vectored_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct macrunner_hb_vectored_handler macrunner_hb_vectored_handlers[MACRUNNER_HB_VECTORED_MAX];
static uint64_t macrunner_hb_vectored_next_handle = 0x6f10000000000000ULL;

static BOOL macrunner_hb_try_vectored_exception_semantic( hb_context_t *ctx,
                                                          const struct macrunner_hb_import_thunk *thunk,
                                                          const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                          uint64_t *ret )
{
    BOOL is_kernel_exception_api, is_ntdll_exception_api;
    unsigned int i;

    if (!ctx || !thunk || !args || !ret) return FALSE;
    is_kernel_exception_api = (macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
                               macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" ));
    is_ntdll_exception_api = macrunner_hb_strieq( thunk->dll_name, "ntdll.dll" );
    if (!is_kernel_exception_api && !is_ntdll_exception_api) return FALSE;

    if ((is_kernel_exception_api && macrunner_hb_strieq( thunk->import_name, "AddVectoredExceptionHandler" )) ||
        (is_ntdll_exception_api && macrunner_hb_strieq( thunk->import_name, "RtlAddVectoredExceptionHandler" )))
    {
        uint64_t func = args[1];
        int chosen = -1;

        if (!func)
        {
            *ret = 0;
            return TRUE;
        }

        pthread_mutex_lock( &macrunner_hb_vectored_mutex );
        for (i = 0; i < MACRUNNER_HB_VECTORED_MAX; i++)
        {
            if (!macrunner_hb_vectored_handlers[i].func)
            {
                chosen = i;
                break;
            }
        }
        if (chosen >= 0)
        {
            if (args[0])
            {
                memmove( &macrunner_hb_vectored_handlers[1], &macrunner_hb_vectored_handlers[0],
                         chosen * sizeof(macrunner_hb_vectored_handlers[0]) );
                chosen = 0;
            }
            macrunner_hb_vectored_handlers[chosen].handle = macrunner_hb_vectored_next_handle++;
            macrunner_hb_vectored_handlers[chosen].func = func;
            *ret = macrunner_hb_vectored_handlers[chosen].handle;
        }
        else *ret = 0;
        pthread_mutex_unlock( &macrunner_hb_vectored_mutex );
        return TRUE;
    }

    if ((is_kernel_exception_api && macrunner_hb_strieq( thunk->import_name, "RemoveVectoredExceptionHandler" )) ||
        (is_ntdll_exception_api && macrunner_hb_strieq( thunk->import_name, "RtlRemoveVectoredExceptionHandler" )))
    {
        BOOL removed = FALSE;

        pthread_mutex_lock( &macrunner_hb_vectored_mutex );
        for (i = 0; i < MACRUNNER_HB_VECTORED_MAX; i++)
        {
            if (macrunner_hb_vectored_handlers[i].handle == args[0])
            {
                memset( &macrunner_hb_vectored_handlers[i], 0, sizeof(macrunner_hb_vectored_handlers[i]) );
                removed = TRUE;
                break;
            }
        }
        pthread_mutex_unlock( &macrunner_hb_vectored_mutex );
        *ret = removed;
        return TRUE;
    }

    if (is_kernel_exception_api && macrunner_hb_strieq( thunk->import_name, "RaiseException" ))
    {
        EXCEPTION_RECORD record;
        CONTEXT context;
        EXCEPTION_POINTERS pointers;
        uint64_t exception_args[EXCEPTION_MAXIMUM_PARAMETERS];
        uint64_t handlers[MACRUNNER_HB_VECTORED_MAX];
        ULONG64 handler_ret = EXCEPTION_CONTINUE_SEARCH;
        unsigned int handler_count = 0;

        memset( &record, 0, sizeof(record) );
        memset( &context, 0, sizeof(context) );
        memset( exception_args, 0, sizeof(exception_args) );

        record.ExceptionCode = (DWORD)args[0];
        record.ExceptionFlags = (DWORD)args[1];
        record.ExceptionAddress = (void *)(uintptr_t)ctx->pc;
        if (args[2] > EXCEPTION_MAXIMUM_PARAMETERS)
        {
            record.ExceptionFlags |= EXCEPTION_NONCONTINUABLE;
            record.NumberParameters = EXCEPTION_MAXIMUM_PARAMETERS;
        }
        else record.NumberParameters = (DWORD)args[2];
        if (record.NumberParameters && args[3])
        {
            if (hb_memory_read( ctx->memory, (hb_gva_t)args[3], exception_args,
                                record.NumberParameters * sizeof(exception_args[0]) ) != HB_OK)
            {
                RtlSetLastWin32Error( ERROR_NOACCESS );
                NtCurrentTeb()->LastStatusValue = STATUS_ACCESS_VIOLATION;
                *ret = 0;
                return TRUE;
            }
            for (i = 0; i < record.NumberParameters; i++)
                record.ExceptionInformation[i] = exception_args[i];
        }

        pointers.ExceptionRecord = &record;
        pointers.ContextRecord = &context;

        pthread_mutex_lock( &macrunner_hb_vectored_mutex );
        for (i = 0; i < MACRUNNER_HB_VECTORED_MAX; i++)
        {
            if (macrunner_hb_vectored_handlers[i].func)
                handlers[handler_count++] = macrunner_hb_vectored_handlers[i].func;
        }
        pthread_mutex_unlock( &macrunner_hb_vectored_mutex );

        for (i = 0; i < handler_count; i++)
        {
            hb_abi_x64_call_t call;
            ULONG64 blocks = 0, steps = 0;
            NTSTATUS status;

            memset( &call, 0, sizeof(call) );
            call.rcx = (uint64_t)(uintptr_t)&pointers;
            status = macrunner_hb_run_x64( (void *)(uintptr_t)handlers[i], &call, &handler_ret,
                                           &blocks, &steps, "x64-vectored-exception",
                                           NtCurrentTeb()->Peb->ImageBaseAddress );
            if (status)
            {
                NtCurrentTeb()->LastStatusValue = status;
                *ret = 0;
                return TRUE;
            }
            if ((LONG)handler_ret == EXCEPTION_CONTINUE_EXECUTION)
            {
                *ret = 0;
                return TRUE;
            }
        }

        NtCurrentTeb()->LastStatusValue = STATUS_UNHANDLED_EXCEPTION;
        *ret = 0;
        return TRUE;
    }

    return FALSE;
}

static BOOL macrunner_hb_try_ntdll_version_semantic( hb_context_t *ctx, void *image_base )
{
    LDR_DATA_TABLE_ENTRY *ldr;
    void *module, *target;
    uint64_t ret_addr = 0;
    PEB *peb;
    DWORD major, minor, build, platform;
    WORD product_type = VER_NT_WORKSTATION;

    if (!ctx || !ctx->memory || !image_base) return FALSE;
    if (macrunner_hb_module_machine( image_base ) != IMAGE_FILE_MACHINE_AMD64) return FALSE;
    ldr = macrunner_hb_ldr_entry_from_pc( (void *)(uintptr_t)ctx->pc );
    if (!ldr || ldr->DllBase != image_base ||
        !macrunner_hb_module_name_matches( "ntdll.dll", &ldr->BaseDllName ))
        return FALSE;

    if (hb_memory_read_u64( ctx->memory, (hb_gva_t)ctx->regs.x64.rsp, &ret_addr ) != HB_OK || !ret_addr)
        return FALSE;

    peb = NtCurrentTeb()->Peb;
    major = peb ? peb->OSMajorVersion : 10;
    minor = peb ? peb->OSMinorVersion : 0;
    build = peb ? peb->OSBuildNumber : 19045;
    platform = peb ? peb->OSPlatformId : VER_PLATFORM_WIN32_NT;

    target = macrunner_hb_find_named_export( image_base, "RtlGetNtVersionNumbers" );
    if (target && ctx->pc == (uint64_t)(uintptr_t)target)
    {
        if (ctx->regs.x64.rcx && hb_memory_write_u32( ctx->memory, (hb_gva_t)ctx->regs.x64.rcx, major ) != HB_OK)
            return FALSE;
        if (ctx->regs.x64.rdx && hb_memory_write_u32( ctx->memory, (hb_gva_t)ctx->regs.x64.rdx, minor ) != HB_OK)
            return FALSE;
        if (ctx->regs.x64.r8 &&
            hb_memory_write_u32( ctx->memory, (hb_gva_t)ctx->regs.x64.r8, 0xf0000000u | build ) != HB_OK)
            return FALSE;
        macrunner_hb_finish_import( ctx, ret_addr, 0 );
        if (macrunner_hb_env_enabled( "MACRUNNER_HB_TRACE_VERSION_SEMANTIC" ))
            fprintf( stderr, "macrunner-hb-version-semantic: RtlGetNtVersionNumbers "
                     "major=%lu minor=%lu build=%lu ret=%p\n",
                     (unsigned long)major, (unsigned long)minor, (unsigned long)build,
                     (void *)(uintptr_t)ret_addr );
        return TRUE;
    }

    target = macrunner_hb_find_named_export( image_base, "RtlGetVersion" );
    if (target && ctx->pc == (uint64_t)(uintptr_t)target)
    {
        RTL_OSVERSIONINFOEXW info;
        DWORD size = 0;
        size_t write_size;

        if (!ctx->regs.x64.rcx) return FALSE;
        hb_memory_read_u32( ctx->memory, (hb_gva_t)ctx->regs.x64.rcx, &size );
        memset( &info, 0, sizeof(info) );
        info.dwOSVersionInfoSize = size;
        info.dwMajorVersion = major;
        info.dwMinorVersion = minor;
        info.dwBuildNumber = build;
        info.dwPlatformId = platform;
        info.wProductType = product_type;
        write_size = size == sizeof(RTL_OSVERSIONINFOEXW) ? sizeof(info) : sizeof(RTL_OSVERSIONINFOW);
        if (hb_memory_write( ctx->memory, (hb_gva_t)ctx->regs.x64.rcx, &info, write_size ) != HB_OK)
            return FALSE;
        macrunner_hb_finish_import( ctx, ret_addr, STATUS_SUCCESS );
        if (macrunner_hb_env_enabled( "MACRUNNER_HB_TRACE_VERSION_SEMANTIC" ))
            fprintf( stderr, "macrunner-hb-version-semantic: RtlGetVersion size=%lu "
                     "major=%lu minor=%lu build=%lu ret=%p\n",
                     (unsigned long)size, (unsigned long)major, (unsigned long)minor,
                     (unsigned long)build, (void *)(uintptr_t)ret_addr );
        return TRUE;
    }

    target = macrunner_hb_find_named_export( image_base, "RtlGetNtProductType" );
    if (target && ctx->pc == (uint64_t)(uintptr_t)target)
    {
        if (ctx->regs.x64.rcx &&
            hb_memory_write_u32( ctx->memory, (hb_gva_t)ctx->regs.x64.rcx, product_type ) != HB_OK)
            return FALSE;
        macrunner_hb_finish_import( ctx, ret_addr, TRUE );
        return TRUE;
    }

    target = macrunner_hb_find_named_export( image_base, "RtlGetProductInfo" );
    if (target && ctx->pc == (uint64_t)(uintptr_t)target)
    {
        uint64_t product_ptr = 0;
        DWORD product = (product_type == VER_NT_WORKSTATION) ? PRODUCT_ULTIMATE_N : PRODUCT_STANDARD_SERVER;

        if (hb_memory_read_u64( ctx->memory, (hb_gva_t)ctx->regs.x64.rsp + 8 + 32,
                                &product_ptr ) != HB_OK || !product_ptr)
        {
            macrunner_hb_finish_import( ctx, ret_addr, FALSE );
            return TRUE;
        }
        if ((DWORD)ctx->regs.x64.rcx < 6)
        {
            hb_memory_write_u32( ctx->memory, (hb_gva_t)product_ptr, PRODUCT_UNDEFINED );
            macrunner_hb_finish_import( ctx, ret_addr, FALSE );
            return TRUE;
        }
        if (hb_memory_write_u32( ctx->memory, (hb_gva_t)product_ptr, product ) != HB_OK)
            return FALSE;
        macrunner_hb_finish_import( ctx, ret_addr, TRUE );
        return TRUE;
    }

    target = macrunner_hb_find_named_export( image_base, "__wine_dbg_output" );
    if (target && ctx->pc == (uint64_t)(uintptr_t)target)
    {
        macrunner_hb_finish_import( ctx, ret_addr, 0 );
        return TRUE;
    }

    return FALSE;
}

static unsigned int macrunner_hb_import_arg_count( const struct macrunner_hb_import_thunk *thunk )
{
    if (!thunk) return 12;

    if ((macrunner_hb_strieq( thunk->dll_name, "api-ms-win-core-winrt-l1-1-0.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "combase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "RoUninitialize" ))
        return 0;
    if ((macrunner_hb_strieq( thunk->dll_name, "api-ms-win-core-winrt-l1-1-0.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "combase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "RoInitialize" ))
        return 1;
    if ((macrunner_hb_strieq( thunk->dll_name, "api-ms-win-core-winrt-l1-1-0.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "combase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "RoActivateInstance" ))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "api-ms-win-core-winrt-l1-1-0.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "combase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "RoGetActivationFactory" ))
        return 3;
    if (macrunner_hb_strieq( thunk->dll_name, "ole32.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "CoInitialize" ))
        return 1;
    if ((macrunner_hb_strieq( thunk->dll_name, "ole32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "combase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "CoInitializeEx" ))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "ole32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "combase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "CoUninitialize" ))
        return 0;
    if ((macrunner_hb_strieq( thunk->dll_name, "ole32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "combase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "CoTaskMemAlloc" ) ||
         macrunner_hb_strieq( thunk->import_name, "CoTaskMemFree" )))
        return 1;
    if ((macrunner_hb_strieq( thunk->dll_name, "ole32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "combase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "CoTaskMemRealloc" ))
        return 2;
    if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
        (macrunner_hb_strieq( thunk->import_name, "RegisterWindowMessageA" ) ||
         macrunner_hb_strieq( thunk->import_name, "RegisterWindowMessageW" )))
        return 1;
    if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
        (macrunner_hb_strieq( thunk->import_name, "LoadCursorA" ) ||
         macrunner_hb_strieq( thunk->import_name, "LoadCursorW" ) ||
         macrunner_hb_strieq( thunk->import_name, "LoadIconA" ) ||
         macrunner_hb_strieq( thunk->import_name, "LoadIconW" )))
        return 2;
    if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
        (macrunner_hb_strieq( thunk->import_name, "LoadImageA" ) ||
         macrunner_hb_strieq( thunk->import_name, "LoadImageW" )))
        return 6;
    if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "GetProcessWindowStation" ))
        return 0;
    if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
        (macrunner_hb_strieq( thunk->import_name, "GetThreadDesktop" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetThreadDesktop" ) ||
         macrunner_hb_strieq( thunk->import_name, "CloseDesktop" )))
        return 1;
    if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
        (macrunner_hb_strieq( thunk->import_name, "GetUserObjectInformationA" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetUserObjectInformationW" )))
        return 5;
    if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "OpenInputDesktop" ))
        return 3;
    if ((macrunner_hb_strieq( thunk->dll_name, "shell32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "shcore.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "CommandLineToArgvW" ))
        return 2;
    if (macrunner_hb_strieq( thunk->dll_name, "shell32.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "SHGetKnownFolderPath" ))
        return 4;
    if (macrunner_hb_strieq( thunk->dll_name, "advapi32.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "OpenProcessToken" ))
        return 3;
    if (macrunner_hb_strieq( thunk->dll_name, "advapi32.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "OpenThreadToken" ))
        return 4;
    if (macrunner_hb_strieq( thunk->dll_name, "advapi32.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "GetTokenInformation" ))
        return 5;
    if (macrunner_hb_strieq( thunk->dll_name, "advapi32.dll" ) &&
        (macrunner_hb_strieq( thunk->import_name, "RegCloseKey" ) ||
         macrunner_hb_strieq( thunk->import_name, "RegDeleteValueW" )))
        return 1;
    if (macrunner_hb_strieq( thunk->dll_name, "advapi32.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "RegOpenKeyW" ))
        return 3;
    if (macrunner_hb_strieq( thunk->dll_name, "advapi32.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "RegOpenKeyExW" ))
        return 5;
    if (macrunner_hb_strieq( thunk->dll_name, "advapi32.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "RegSetValueExW" ))
        return 6;
    if (macrunner_hb_strieq( thunk->dll_name, "advapi32.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "RegQueryValueExW" ))
        return 6;
    if (macrunner_hb_strieq( thunk->dll_name, "advapi32.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "RegCreateKeyExW" ))
        return 9;
    if (macrunner_hb_strieq( thunk->dll_name, "advapi32.dll" ) &&
        (macrunner_hb_strieq( thunk->import_name, "IsValidSid" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetLengthSid" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetSidIdentifierAuthority" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetSidSubAuthorityCount" )))
        return 1;
    if (macrunner_hb_strieq( thunk->dll_name, "advapi32.dll" ) &&
        (macrunner_hb_strieq( thunk->import_name, "EqualSid" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetSidSubAuthority" )))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "psapi.dll" )) &&
        (macrunner_hb_psapi_name_is( thunk->import_name, "EnumProcessModules" ) ||
         macrunner_hb_psapi_name_is( thunk->import_name, "GetModuleFileNameExA" ) ||
         macrunner_hb_psapi_name_is( thunk->import_name, "GetModuleFileNameExW" ) ||
         macrunner_hb_psapi_name_is( thunk->import_name, "GetModuleBaseNameA" ) ||
         macrunner_hb_psapi_name_is( thunk->import_name, "GetModuleBaseNameW" ) ||
         macrunner_hb_psapi_name_is( thunk->import_name, "GetModuleInformation" )))
        return 4;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "psapi.dll" )) &&
        macrunner_hb_psapi_name_is( thunk->import_name, "EnumProcessModulesEx" ))
        return 5;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "psapi.dll" )) &&
        (macrunner_hb_psapi_name_is( thunk->import_name, "GetProcessImageFileNameA" ) ||
         macrunner_hb_psapi_name_is( thunk->import_name, "GetProcessImageFileNameW" )))
        return 3;

    if (macrunner_hb_strieq( thunk->import_name, "WakeByAddressAll" ) ||
        macrunner_hb_strieq( thunk->import_name, "WakeByAddressSingle" ) ||
        macrunner_hb_strieq( thunk->import_name, "RtlWakeAddressAll" ) ||
        macrunner_hb_strieq( thunk->import_name, "RtlWakeAddressSingle" ))
        return 1;
    if (macrunner_hb_strieq( thunk->import_name, "WaitOnAddress" ) ||
        macrunner_hb_strieq( thunk->import_name, "RtlWaitOnAddress" ))
        return 4;

    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "CreateThread" ))
        return 6;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "CreateRemoteThread" ))
        return 7;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "CreateRemoteThreadEx" ))
        return 8;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "ResumeThread" ) ||
         macrunner_hb_strieq( thunk->import_name, "SuspendThread" )))
        return 1;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "WaitForMultipleObjects" ))
        return 4;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "WaitForMultipleObjectsEx" ))
        return 5;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "WaitForSingleObject" ))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "WaitForSingleObjectEx" ))
        return 3;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "DuplicateHandle" ))
        return 7;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "CloseHandle" ))
        return 1;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "SetEvent" ) ||
         macrunner_hb_strieq( thunk->import_name, "ResetEvent" ) ||
         macrunner_hb_strieq( thunk->import_name, "PulseEvent" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetErrorMode" ) ||
         macrunner_hb_strieq( thunk->import_name, "OutputDebugStringA" ) ||
         macrunner_hb_strieq( thunk->import_name, "OutputDebugStringW" )))
        return 1;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "GetErrorMode" ))
        return 0;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "SetThreadErrorMode" ))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "SetThreadDescription" ))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "OpenEventA" ) ||
         macrunner_hb_strieq( thunk->import_name, "OpenEventW" )))
        return 3;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "CreateEventA" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateEventW" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateEventExA" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateEventExW" )))
        return 4;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "CreateSemaphoreA" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateSemaphoreW" )))
        return 4;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "CreateSemaphoreExA" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateSemaphoreExW" )))
        return 6;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "OpenSemaphoreA" ) ||
         macrunner_hb_strieq( thunk->import_name, "OpenSemaphoreW" ) ||
         macrunner_hb_strieq( thunk->import_name, "ReleaseSemaphore" )))
        return 3;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "CreateMutexA" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateMutexW" ) ||
         macrunner_hb_strieq( thunk->import_name, "OpenMutexA" ) ||
         macrunner_hb_strieq( thunk->import_name, "OpenMutexW" )))
        return 3;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "CreateMutexExA" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateMutexExW" )))
        return 4;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "ReleaseMutex" ))
        return 1;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "CreatePipe" ))
        return 4;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "GetLogicalProcessorInformation" ))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "GetLogicalProcessorInformationEx" ))
        return 3;
    if (macrunner_hb_strieq( thunk->dll_name, "bcrypt.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "BCryptGenRandom" ))
        return 4;
    if ((macrunner_hb_strieq( thunk->dll_name, "ntdll.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "ntdll" )) &&
        macrunner_hb_strieq( thunk->import_name, "NtQueryVirtualMemory" ))
        return 6;
    if ((macrunner_hb_strieq( thunk->dll_name, "ntdll.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "ntdll" )) &&
        macrunner_hb_strieq( thunk->import_name, "LdrGetDllHandle" ))
        return 4;
    if ((macrunner_hb_strieq( thunk->dll_name, "ntdll.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "ntdll" )) &&
        macrunner_hb_strieq( thunk->import_name, "LdrGetDllHandleEx" ))
        return 5;
    if ((macrunner_hb_strieq( thunk->dll_name, "ntdll.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "ntdll" )) &&
        macrunner_hb_strieq( thunk->import_name, "RtlFindExportedRoutineByName" ))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "api-ms-win-crt-locale-l1-1-0.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "ucrtbase.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "msvcrt.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "__pctype_func" ) ||
         macrunner_hb_strieq( thunk->import_name, "___mb_cur_max_func" ) ||
         macrunner_hb_strieq( thunk->import_name, "___lc_codepage_func" ) ||
         macrunner_hb_strieq( thunk->import_name, "localeconv" )))
        return 0;
    if ((macrunner_hb_strieq( thunk->dll_name, "api-ms-win-crt-locale-l1-1-0.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "ucrtbase.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "msvcrt.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "_configthreadlocale" ) ||
         macrunner_hb_strieq( thunk->import_name, "_free_locale" )))
        return 1;
    if ((macrunner_hb_strieq( thunk->dll_name, "api-ms-win-crt-locale-l1-1-0.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "ucrtbase.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "msvcrt.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "_create_locale" ))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "api-ms-win-crt-locale-l1-1-0.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "ucrtbase.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "msvcrt.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "setlocale" ))
        return 2;
    if (macrunner_hb_is_crt_math_dll( thunk->dll_name ) &&
        macrunner_hb_strieq( thunk->import_name, "ceilf" ))
        return 0;
    if (macrunner_hb_is_crt_string_dll( thunk->dll_name ) &&
        (macrunner_hb_strieq( thunk->import_name, "strlen" ) ||
         macrunner_hb_strieq( thunk->import_name, "wcslen" ) ||
         macrunner_hb_strieq( thunk->import_name, "isdigit" ) ||
         macrunner_hb_strieq( thunk->import_name, "isspace" ) ||
         macrunner_hb_strieq( thunk->import_name, "isxdigit" ) ||
         macrunner_hb_strieq( thunk->import_name, "tolower" ) ||
         macrunner_hb_strieq( thunk->import_name, "toupper" ) ||
         macrunner_hb_strieq( thunk->import_name, "_strdup" )))
        return 1;
    if (macrunner_hb_is_crt_string_dll( thunk->dll_name ) &&
        (macrunner_hb_strieq( thunk->import_name, "strcmp" ) ||
         macrunner_hb_strieq( thunk->import_name, "strnlen" ) ||
         macrunner_hb_strieq( thunk->import_name, "wcsnlen" ) ||
         macrunner_hb_strieq( thunk->import_name, "_tolower_l" ) ||
         macrunner_hb_strieq( thunk->import_name, "_toupper_l" ) ||
         macrunner_hb_strieq( thunk->import_name, "_iswalpha_l" ) ||
         macrunner_hb_strieq( thunk->import_name, "_iswcntrl_l" ) ||
         macrunner_hb_strieq( thunk->import_name, "_iswdigit_l" ) ||
         macrunner_hb_strieq( thunk->import_name, "_iswlower_l" ) ||
         macrunner_hb_strieq( thunk->import_name, "_iswprint_l" ) ||
         macrunner_hb_strieq( thunk->import_name, "_iswpunct_l" ) ||
         macrunner_hb_strieq( thunk->import_name, "_iswspace_l" ) ||
         macrunner_hb_strieq( thunk->import_name, "_iswupper_l" ) ||
         macrunner_hb_strieq( thunk->import_name, "_iswxdigit_l" ) ||
         macrunner_hb_strieq( thunk->import_name, "_towlower_l" ) ||
         macrunner_hb_strieq( thunk->import_name, "_towupper_l" )))
        return 2;
    if (macrunner_hb_is_crt_string_dll( thunk->dll_name ) &&
        (macrunner_hb_strieq( thunk->import_name, "strncmp" ) ||
         macrunner_hb_strieq( thunk->import_name, "memset" ) ||
         macrunner_hb_strieq( thunk->import_name, "memcpy" ) ||
         macrunner_hb_strieq( thunk->import_name, "memmove" ) ||
         macrunner_hb_strieq( thunk->import_name, "memchr" ) ||
         macrunner_hb_strieq( thunk->import_name, "memcmp" ) ||
         macrunner_hb_strieq( thunk->import_name, "_strcoll_l" ) ||
         macrunner_hb_strieq( thunk->import_name, "_wcscoll_l" ) ||
         macrunner_hb_strieq( thunk->import_name, "mbrlen" )))
        return 3;
    if (macrunner_hb_is_crt_string_dll( thunk->dll_name ) &&
        (macrunner_hb_strieq( thunk->import_name, "_strxfrm_l" ) ||
         macrunner_hb_strieq( thunk->import_name, "_wcsxfrm_l" )))
        return 4;
    if (macrunner_hb_is_crt_multibyte_dll( thunk->dll_name ) &&
        macrunner_hb_strieq( thunk->import_name, "_mbtowc_l" ))
        return 4;
    if (macrunner_hb_is_crt_heap_dll( thunk->dll_name ) &&
        (macrunner_hb_strieq( thunk->import_name, "malloc" ) ||
         macrunner_hb_strieq( thunk->import_name, "free" ) ||
         macrunner_hb_strieq( thunk->import_name, "_aligned_free" )))
        return 1;
    if (macrunner_hb_is_crt_heap_dll( thunk->dll_name ) &&
        (macrunner_hb_strieq( thunk->import_name, "calloc" ) ||
         macrunner_hb_strieq( thunk->import_name, "realloc" ) ||
         macrunner_hb_strieq( thunk->import_name, "_aligned_malloc" )))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "VirtualFree" ) ||
         macrunner_hb_strieq( thunk->import_name, "VirtualQuery" )))
        return 3;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "VirtualAlloc" ) ||
         macrunner_hb_strieq( thunk->import_name, "VirtualProtect" ) ||
         macrunner_hb_strieq( thunk->import_name, "VirtualFreeEx" ) ||
         macrunner_hb_strieq( thunk->import_name, "VirtualQueryEx" )))
        return 4;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "VirtualAllocEx" ) ||
         macrunner_hb_strieq( thunk->import_name, "VirtualProtectEx" )))
        return 5;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "GetLastError" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetProcessHeap" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetCurrentProcess" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetCurrentThread" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetCurrentProcessId" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetCurrentThreadId" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetTickCount" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetTickCount64" ) ||
         macrunner_hb_strieq( thunk->import_name, "TlsAlloc" )))
        return 0;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "GetSystemInfo" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetNativeSystemInfo" ) ||
         macrunner_hb_strieq( thunk->import_name, "GlobalMemoryStatus" ) ||
         macrunner_hb_strieq( thunk->import_name, "GlobalMemoryStatusEx" )))
        return 1;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "GetFileAttributesA" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetFileAttributesW" )))
        return 1;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "SetFileAttributesA" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetFileAttributesW" )))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "GetFileAttributesExA" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetFileAttributesExW" )))
        return 3;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "CreateFileA" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateFileW" )))
        return 7;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "MoveFileA" ) ||
         macrunner_hb_strieq( thunk->import_name, "MoveFileW" )))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "MoveFileExA" ) ||
         macrunner_hb_strieq( thunk->import_name, "MoveFileExW" )))
        return 3;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "CreateDirectoryA" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateDirectoryW" )))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "CreateDirectoryExA" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateDirectoryExW" )))
        return 3;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "ReadFile" ) ||
         macrunner_hb_strieq( thunk->import_name, "WriteFile" )))
        return 5;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "GetFileSize" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetFileSizeEx" )))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "GetFileInformationByHandle" ))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "SetFilePointer" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetFilePointerEx" )))
        return 4;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "LocalAlloc" ))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "LocalReAlloc" ))
        return 3;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "SetLastError" ) ||
         macrunner_hb_strieq( thunk->import_name, "LocalFree" ) ||
         macrunner_hb_strieq( thunk->import_name, "LocalSize" ) ||
         macrunner_hb_strieq( thunk->import_name, "TlsGetValue" ) ||
         macrunner_hb_strieq( thunk->import_name, "TlsFree" ) ||
         macrunner_hb_strieq( thunk->import_name, "FlsAlloc" ) ||
         macrunner_hb_strieq( thunk->import_name, "FlsGetValue" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetStdHandle" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetFileType" ) ||
         macrunner_hb_strieq( thunk->import_name, "FlsFree" )))
        return 1;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "TlsSetValue" ) ||
         macrunner_hb_strieq( thunk->import_name, "FlsSetValue" )))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "HeapAlloc" ) ||
         macrunner_hb_strieq( thunk->import_name, "HeapFree" ) ||
         macrunner_hb_strieq( thunk->import_name, "HeapSize" )))
        return 3;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "HeapReAlloc" ))
        return 4;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "AddVectoredExceptionHandler" ))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "RemoveVectoredExceptionHandler" ))
        return 1;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "RaiseException" ))
        return 4;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "GetEnvironmentStringsW" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetEnvironmentStringsA" )))
        return 0;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "FreeEnvironmentStringsW" ) ||
         macrunner_hb_strieq( thunk->import_name, "FreeEnvironmentStringsA" )))
        return 1;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "GetModuleFileNameW" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetModuleFileNameA" )))
        return 3;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "FormatMessageA" ) ||
         macrunner_hb_strieq( thunk->import_name, "FormatMessageW" )))
        return 7;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "QueryPerformanceFrequency" ) ||
         macrunner_hb_strieq( thunk->import_name, "QueryPerformanceCounter" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetSystemTimePreciseAsFileTime" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetSystemTimeAsFileTime" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetSystemTime" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetLocalTime" )))
        return 1;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "SystemTimeToFileTime" ) ||
         macrunner_hb_strieq( thunk->import_name, "FileTimeToSystemTime" )))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "SystemTimeToTzSpecificLocalTime" ) ||
         macrunner_hb_strieq( thunk->import_name, "TzSpecificLocalTimeToSystemTime" )))
        return 3;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "InitializeCriticalSection" ) ||
         macrunner_hb_strieq( thunk->import_name, "DeleteCriticalSection" ) ||
         macrunner_hb_strieq( thunk->import_name, "EnterCriticalSection" ) ||
         macrunner_hb_strieq( thunk->import_name, "LeaveCriticalSection" ) ||
         macrunner_hb_strieq( thunk->import_name, "TryEnterCriticalSection" ) ||
         macrunner_hb_strieq( thunk->import_name, "InitializeSRWLock" ) ||
         macrunner_hb_strieq( thunk->import_name, "AcquireSRWLockExclusive" ) ||
         macrunner_hb_strieq( thunk->import_name, "AcquireSRWLockShared" ) ||
         macrunner_hb_strieq( thunk->import_name, "ReleaseSRWLockExclusive" ) ||
         macrunner_hb_strieq( thunk->import_name, "ReleaseSRWLockShared" ) ||
         macrunner_hb_strieq( thunk->import_name, "TryAcquireSRWLockExclusive" ) ||
         macrunner_hb_strieq( thunk->import_name, "TryAcquireSRWLockShared" ) ||
         macrunner_hb_strieq( thunk->import_name, "InitializeConditionVariable" ) ||
         macrunner_hb_strieq( thunk->import_name, "WakeAllConditionVariable" ) ||
         macrunner_hb_strieq( thunk->import_name, "WakeConditionVariable" )))
        return 1;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "SleepConditionVariableCS" ))
        return 3;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "SleepConditionVariableSRW" ))
        return 4;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "InitializeCriticalSectionAndSpinCount" ))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "InitializeCriticalSectionEx" ))
        return 3;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "InitializeSListHead" ) ||
         macrunner_hb_strieq( thunk->import_name, "InterlockedFlushSList" ) ||
         macrunner_hb_strieq( thunk->import_name, "InterlockedPopEntrySList" ) ||
         macrunner_hb_strieq( thunk->import_name, "QueryDepthSList" )))
        return 1;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "InterlockedPushEntrySList" ))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "InterlockedPushListSList" ) ||
         macrunner_hb_strieq( thunk->import_name, "InterlockedPushListSListEx" )))
        return 4;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "GetTempPathW" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetTempPath2W" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetTempPathA" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetTempPath2A" )))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "GetACP" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetOEMCP" ) ||
         macrunner_hb_strieq( thunk->import_name, "AreFileApisANSI" )))
        return 0;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "IsValidCodePage" ))
        return 1;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "GetCPInfo" ))
        return 2;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "GetStringTypeW" ))
        return 4;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "MultiByteToWideChar" ))
        return 6;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "WideCharToMultiByte" ))
        return 8;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        macrunner_hb_strieq( thunk->import_name, "LCMapStringEx" ))
        return 9;
    if (macrunner_hb_strieq( thunk->import_name, "EventActivityIdControl" ) ||
        macrunner_hb_strieq( thunk->import_name, "EventEnabled" ))
        return 2;
    if (macrunner_hb_strieq( thunk->import_name, "EventProviderEnabled" ))
        return 3;
    if (macrunner_hb_strieq( thunk->import_name, "EventRegister" ) ||
        macrunner_hb_strieq( thunk->import_name, "EventSetInformation" ) ||
        macrunner_hb_strieq( thunk->import_name, "EventWrite" ) ||
        macrunner_hb_strieq( thunk->import_name, "EventWriteString" ))
        return 4;
    if (macrunner_hb_strieq( thunk->import_name, "EventUnregister" ))
        return 1;
    if (macrunner_hb_strieq( thunk->import_name, "EventWriteTransfer" ))
        return 6;
    if (macrunner_hb_strieq( thunk->import_name, "EventWriteEx" ))
        return 8;
    if (macrunner_hb_strieq( thunk->dll_name, "ntdll.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "RtlAddVectoredExceptionHandler" ))
        return 2;
    if (macrunner_hb_strieq( thunk->dll_name, "ntdll.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "RtlRemoveVectoredExceptionHandler" ))
        return 1;
    if (macrunner_hb_strieq( thunk->import_name, "EtwEventRegister" ))
        return 4;
    if (macrunner_hb_strieq( thunk->import_name, "EtwEventActivityIdControl" ) ||
        macrunner_hb_strieq( thunk->import_name, "EtwEventEnabled" ) ||
        macrunner_hb_strieq( thunk->import_name, "EtwEventProviderEnabled" ) ||
        macrunner_hb_strieq( thunk->import_name, "EtwLogTraceEvent" ))
        return 2;
    if (macrunner_hb_strieq( thunk->import_name, "EtwEventUnregister" ) ||
        macrunner_hb_strieq( thunk->import_name, "EtwGetTraceEnableFlags" ) ||
        macrunner_hb_strieq( thunk->import_name, "EtwGetTraceEnableLevel" ) ||
        macrunner_hb_strieq( thunk->import_name, "EtwGetTraceLoggerHandle" ) ||
        macrunner_hb_strieq( thunk->import_name, "EtwUnregisterTraceGuids" ))
        return 1;
    if (macrunner_hb_strieq( thunk->import_name, "EtwEventSetInformation" ) ||
        macrunner_hb_strieq( thunk->import_name, "EtwEventWriteString" ) ||
        macrunner_hb_strieq( thunk->import_name, "EtwEventWriteTransfer" ))
        return 5;
    if (macrunner_hb_strieq( thunk->import_name, "EtwEventWrite" ))
        return 4;
    if (macrunner_hb_strieq( thunk->import_name, "EtwEventWriteEx" ))
        return 9;
    if (macrunner_hb_strieq( thunk->import_name, "EtwRegisterTraceGuidsA" ) ||
        macrunner_hb_strieq( thunk->import_name, "EtwRegisterTraceGuidsW" ))
        return 8;
    if (macrunner_hb_strieq( thunk->dll_name, "ntdll.dll" ) &&
        (macrunner_hb_strieq( thunk->import_name, "RtlInitializeSRWLock" ) ||
         macrunner_hb_strieq( thunk->import_name, "RtlAcquireSRWLockExclusive" ) ||
         macrunner_hb_strieq( thunk->import_name, "RtlAcquireSRWLockShared" ) ||
         macrunner_hb_strieq( thunk->import_name, "RtlReleaseSRWLockExclusive" ) ||
         macrunner_hb_strieq( thunk->import_name, "RtlReleaseSRWLockShared" ) ||
         macrunner_hb_strieq( thunk->import_name, "RtlTryAcquireSRWLockExclusive" ) ||
         macrunner_hb_strieq( thunk->import_name, "RtlTryAcquireSRWLockShared" ) ||
         macrunner_hb_strieq( thunk->import_name, "RtlInitializeConditionVariable" ) ||
         macrunner_hb_strieq( thunk->import_name, "RtlWakeAllConditionVariable" ) ||
         macrunner_hb_strieq( thunk->import_name, "RtlWakeConditionVariable" )))
        return 1;
    if (macrunner_hb_strieq( thunk->dll_name, "ntdll.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "RtlSleepConditionVariableCS" ))
        return 3;
    if (macrunner_hb_strieq( thunk->dll_name, "ntdll.dll" ) &&
        macrunner_hb_strieq( thunk->import_name, "RtlSleepConditionVariableSRW" ))
        return 4;
    if (macrunner_hb_is_crt_stdio_dll( thunk->dll_name ) &&
        macrunner_hb_strieq( thunk->import_name, "__acrt_iob_func" ))
        return 1;
    if (macrunner_hb_is_crt_stdio_dll( thunk->dll_name ) &&
        (macrunner_hb_strieq( thunk->import_name, "__stdio_common_vfprintf" ) ||
         macrunner_hb_strieq( thunk->import_name, "__stdio_common_vfwprintf" )))
        return 5;
    if (macrunner_hb_is_crt_stdio_dll( thunk->dll_name ) &&
        (macrunner_hb_strieq( thunk->import_name, "_fileno" ) ||
         macrunner_hb_strieq( thunk->import_name, "_ftelli64" ) ||
         macrunner_hb_strieq( thunk->import_name, "fclose" ) ||
         macrunner_hb_strieq( thunk->import_name, "fflush" ) ||
         macrunner_hb_strieq( thunk->import_name, "fgetwc" ) ||
         macrunner_hb_strieq( thunk->import_name, "getc" )))
        return 1;
    if (macrunner_hb_is_crt_stdio_dll( thunk->dll_name ) &&
        (macrunner_hb_strieq( thunk->import_name, "_setmode" ) ||
         macrunner_hb_strieq( thunk->import_name, "_wfopen" ) ||
         macrunner_hb_strieq( thunk->import_name, "fopen" ) ||
         macrunner_hb_strieq( thunk->import_name, "fputc" ) ||
         macrunner_hb_strieq( thunk->import_name, "fputwc" ) ||
         macrunner_hb_strieq( thunk->import_name, "setbuf" ) ||
         macrunner_hb_strieq( thunk->import_name, "ungetc" ) ||
         macrunner_hb_strieq( thunk->import_name, "ungetwc" )))
        return 2;
    if (macrunner_hb_is_crt_stdio_dll( thunk->dll_name ) &&
        (macrunner_hb_strieq( thunk->import_name, "_fseeki64" ) ||
         macrunner_hb_strieq( thunk->import_name, "fseek" )))
        return 3;
    if (macrunner_hb_is_crt_stdio_dll( thunk->dll_name ) &&
        (macrunner_hb_strieq( thunk->import_name, "fread" ) ||
         macrunner_hb_strieq( thunk->import_name, "fwrite" )))
        return 4;

    if (macrunner_hb_strieq( thunk->dll_name, "win32u.dll" ))
    {
        if (macrunner_hb_strieq( thunk->import_name, "NtUserCreateWindowEx" )) return 17;
        if (macrunner_hb_strieq( thunk->import_name, "NtGdiSetDIBitsToDeviceInternal" )) return 16;
        if (macrunner_hb_strieq( thunk->import_name, "NtGdiStretchDIBitsInternal" )) return 16;
        if (macrunner_hb_strieq( thunk->import_name, "NtGdiMaskBlt" )) return 13;
    }

    return 12;
}

static hb_result_t macrunner_hb_call_import_thunk( hb_context_t *ctx,
                                                   const struct macrunner_hb_import_thunk *thunk )
{
    uint64_t ret_addr = 0;
    uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX] = { 0 };
    uint64_t rc;
    unsigned int arg_count;
    unsigned int i;
    BOOL trace_createwindow;
    BOOL trace_registerclass;

    if (!ctx || !ctx->memory || !thunk || !thunk->target) return HB_ERR_INVALID_ARG;

    if (hb_memory_read_u64( ctx->memory, (hb_gva_t)ctx->regs.x64.rsp, &ret_addr ) != HB_OK || !ret_addr)
        return HB_ERR_EXEC_FAULT;

    args[0] = ctx->regs.x64.rcx;
    args[1] = ctx->regs.x64.rdx;
    args[2] = ctx->regs.x64.r8;
    args[3] = ctx->regs.x64.r9;
    arg_count = macrunner_hb_import_arg_count( thunk );
    if (arg_count > MACRUNNER_HB_IMPORT_ARG_MAX) return HB_ERR_UNSUPPORTED_FEATURE;
    for (i = 4; i < arg_count; i++)
    {
        hb_result_t read = hb_memory_read_u64( ctx->memory,
                                               (hb_gva_t)ctx->regs.x64.rsp + 8 + 32 + (i - 4) * 8,
                                               &args[i] );
        if (read != HB_OK && arg_count > 12) return HB_ERR_MEMORY_FAULT;
    }
    if (macrunner_hb_trace_direct_native_enabled() &&
        macrunner_hb_strieq( thunk->dll_name, "win32u.dll" ) &&
        (macrunner_hb_strieq( thunk->import_name, "NtGdiStretchDIBitsInternal" ) ||
         macrunner_hb_strieq( thunk->import_name, "NtGdiSetDIBitsToDeviceInternal" ) ||
         macrunner_hb_strieq( thunk->import_name, "NtUserCreateWindowEx" )))
        fprintf( stderr, "macrunner-hb-high-arity: import=%s!%s argc=%u "
                 "a8=%p a9=%p a10=%p a11=%p a12=%p a13=%p a14=%p a15=%p a16=%p\n",
                 thunk->dll_name, thunk->import_name, arg_count,
                 (void *)(uintptr_t)args[8], (void *)(uintptr_t)args[9],
                 (void *)(uintptr_t)args[10], (void *)(uintptr_t)args[11],
                 (void *)(uintptr_t)args[12], (void *)(uintptr_t)args[13],
                 (void *)(uintptr_t)args[14], (void *)(uintptr_t)args[15],
                 (void *)(uintptr_t)args[16] );
    macrunner_hb_normalize_import_args( thunk, args );
    trace_createwindow = getenv( "MACRUNNER_HB_TRACE_CREATEWINDOW" ) &&
                         (macrunner_hb_strieq( thunk->import_name, "CreateWindowExW" ) ||
                          macrunner_hb_strieq( thunk->import_name, "CreateWindowExA" ) ||
                          macrunner_hb_strieq( thunk->import_name, "NtUserCreateWindowEx" ));
    trace_registerclass = getenv( "MACRUNNER_HB_TRACE_CREATEWINDOW" ) &&
                          (macrunner_hb_strieq( thunk->import_name, "RegisterClassExW" ) ||
                           macrunner_hb_strieq( thunk->import_name, "RegisterClassW" ));
    if (trace_createwindow)
    {
        fprintf( stderr, "macrunner-hb-createwindow-call: before import=%s!%s pc=%p rsp=%p ret=%p "
                 "args=%p,%p,%p,%p,%p,%p,%p,%p,%p,%p,%p,%p,%p,%p,%p,%p,%p\n",
                 thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                 (void *)(uintptr_t)ctx->regs.x64.rsp, (void *)(uintptr_t)ret_addr,
                 (void *)(uintptr_t)args[0], (void *)(uintptr_t)args[1],
                 (void *)(uintptr_t)args[2], (void *)(uintptr_t)args[3],
                 (void *)(uintptr_t)args[4], (void *)(uintptr_t)args[5],
                 (void *)(uintptr_t)args[6], (void *)(uintptr_t)args[7],
                 (void *)(uintptr_t)args[8], (void *)(uintptr_t)args[9],
                 (void *)(uintptr_t)args[10], (void *)(uintptr_t)args[11],
                 (void *)(uintptr_t)args[12], (void *)(uintptr_t)args[13],
                 (void *)(uintptr_t)args[14], (void *)(uintptr_t)args[15],
                 (void *)(uintptr_t)args[16] );
        if (macrunner_hb_strieq( thunk->import_name, "CreateWindowExW" ))
        {
            macrunner_hb_trace_guest_wstr( ctx, "CreateWindowExW.class", args[1] );
            macrunner_hb_trace_guest_wstr( ctx, "CreateWindowExW.title", args[2] );
        }
    }
    if (trace_registerclass)
    {
        uint64_t wndproc = 0, class_name = 0;
        uint32_t cb_size = 0, style = 0;

        hb_memory_read_u32( ctx->memory, (hb_gva_t)args[0], &cb_size );
        hb_memory_read_u32( ctx->memory, (hb_gva_t)args[0] + 4, &style );
        hb_memory_read_u64( ctx->memory, (hb_gva_t)args[0] + 8, &wndproc );
        hb_memory_read_u64( ctx->memory, (hb_gva_t)args[0] + 64, &class_name );
        fprintf( stderr, "macrunner-hb-registerclass-call: before import=%s!%s pc=%p ret=%p "
                 "class_struct=%p cb=%u style=0x%x wndproc=%p class=%p\n",
                 thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                 (void *)(uintptr_t)ret_addr, (void *)(uintptr_t)args[0],
                 cb_size, style, (void *)(uintptr_t)wndproc, (void *)(uintptr_t)class_name );
        if (macrunner_hb_strieq( thunk->import_name, "RegisterClassExW" ) ||
            macrunner_hb_strieq( thunk->import_name, "RegisterClassW" ))
            macrunner_hb_trace_guest_wstr( ctx, "RegisterClass.class", class_name );
    }
    if (macrunner_hb_trace_file_api_enabled() &&
        (macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "CreateFileW" ) ||
         macrunner_hb_strieq( thunk->import_name, "ReadFile" ) ||
         macrunner_hb_strieq( thunk->import_name, "WriteFile" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetFileSize" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetFileSizeEx" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetFileInformationByHandle" ) ||
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
    if (macrunner_hb_try_synthetic_d3d_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_get_module_handle_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_ldr_module_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_ldr_export_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_library_loader_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_psapi_module_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_format_message_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_etw_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_environment_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_command_line_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_known_folder_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_local_file_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_file_attribute_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_system_info_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_memory_status_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_msvcrt_time_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_msvcrt_exit_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_crt_environment_init_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_crt_locale_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_crt_math_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_crt_multibyte_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_crt_heap_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_crt_string_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_unhandled_exception_filter_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_kernel32_stdio_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_ntdll_memory_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_wait_address_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_crt_stdio_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_crt_vfprintf_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_thread_creation_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_cotaskmem_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_com_apartment_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_user32_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_winrt_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_kernel32_handle_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_registry_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_security_token_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_bcrypt_semantic( ctx, thunk, args, &rc ) ||
        macrunner_hb_try_vectored_exception_semantic( ctx, thunk, args, &rc ))
    {
        TRACE( "MacRunner HyperBridge semantic import call %s!%s ret=%p\n",
               thunk->dll_name, thunk->import_name, (void *)(uintptr_t)rc );
    }
    else
    {
        rc = macrunner_hb_call_arm64_pe_import12_for_ctx( ctx, thunk, args );
    }
    if (rc && (macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
               macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "HeapAlloc" ) ||
         macrunner_hb_strieq( thunk->import_name, "HeapReAlloc" )))
        macrunner_hb_ensure_heap_bucket_tail( ctx, rc, thunk );
    if ((macrunner_hb_env_enabled( "MACRUNNER_HB_TRACE_HEAP_IMPORT" ) ||
         macrunner_hb_env_enabled( "MACRUNNER_HB_TRACE_MEMORY_IMPORT" )) &&
        (macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "HeapAlloc" ) ||
         macrunner_hb_strieq( thunk->import_name, "HeapReAlloc" ) ||
         macrunner_hb_strieq( thunk->import_name, "HeapFree" ) ||
         macrunner_hb_strieq( thunk->import_name, "HeapSize" ) ||
         macrunner_hb_strieq( thunk->import_name, "LocalAlloc" ) ||
         macrunner_hb_strieq( thunk->import_name, "LocalReAlloc" ) ||
         macrunner_hb_strieq( thunk->import_name, "LocalFree" ) ||
         macrunner_hb_strieq( thunk->import_name, "LocalSize" ) ||
         macrunner_hb_strieq( thunk->import_name, "VirtualAlloc" ) ||
         macrunner_hb_strieq( thunk->import_name, "VirtualAllocEx" ) ||
         macrunner_hb_strieq( thunk->import_name, "VirtualFree" ) ||
         macrunner_hb_strieq( thunk->import_name, "VirtualFreeEx" ) ||
         macrunner_hb_strieq( thunk->import_name, "VirtualProtect" ) ||
         macrunner_hb_strieq( thunk->import_name, "VirtualProtectEx" ) ||
         macrunner_hb_strieq( thunk->import_name, "VirtualQuery" ) ||
         macrunner_hb_strieq( thunk->import_name, "VirtualQueryEx" )))
    {
        int trace_heap_stack = macrunner_hb_env_enabled( "MACRUNNER_HB_TRACE_HEAP_IMPORT_STACK" );
        uint64_t stack0 = 0, stack1 = 0, stack2 = 0, stack3 = 0, stack4 = 0, stack5 = 0;

        if (trace_heap_stack && ctx->mode == HB_MODE_64BIT)
        {
            hb_memory_read_u64( ctx->memory, (hb_gva_t)ctx->regs.x64.rsp, &stack0 );
            hb_memory_read_u64( ctx->memory, (hb_gva_t)ctx->regs.x64.rsp + 8, &stack1 );
            hb_memory_read_u64( ctx->memory, (hb_gva_t)ctx->regs.x64.rsp + 16, &stack2 );
            hb_memory_read_u64( ctx->memory, (hb_gva_t)ctx->regs.x64.rsp + 24, &stack3 );
            hb_memory_read_u64( ctx->memory, (hb_gva_t)ctx->regs.x64.rsp + 32, &stack4 );
            hb_memory_read_u64( ctx->memory, (hb_gva_t)ctx->regs.x64.rsp + 40, &stack5 );
        }

        fprintf( stderr, "macrunner-hb-heap-import: import=%s!%s "
                 "a0=%p a1=%p a2=%p a3=%p ret=%p pc=%p ret_addr=%p",
                 thunk->dll_name, thunk->import_name,
                 (void *)(uintptr_t)args[0], (void *)(uintptr_t)args[1],
                 (void *)(uintptr_t)args[2], (void *)(uintptr_t)args[3],
                 (void *)(uintptr_t)rc, (void *)(uintptr_t)ctx->pc,
                 (void *)(uintptr_t)ret_addr );
        if (trace_heap_stack)
            fprintf( stderr, " rsp=%p stack0=%p stack1=%p stack2=%p stack3=%p stack4=%p stack5=%p",
                     (void *)(uintptr_t)ctx->regs.x64.rsp,
                     (void *)(uintptr_t)stack0, (void *)(uintptr_t)stack1,
                     (void *)(uintptr_t)stack2, (void *)(uintptr_t)stack3,
                     (void *)(uintptr_t)stack4, (void *)(uintptr_t)stack5 );
        fprintf( stderr, "\n" );
    }
    if (trace_createwindow)
    {
        TEB *teb = NtCurrentTeb();

        fprintf( stderr, "macrunner-hb-createwindow-call: after import=%s!%s pc=%p ret_addr=%p "
                 "rc=%p last_error=%lu last_status=%08lx\n",
                 thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                 (void *)(uintptr_t)ret_addr, (void *)(uintptr_t)rc,
                 teb ? (unsigned long)teb->LastErrorValue : 0,
                 teb ? (unsigned long)teb->LastStatusValue : 0 );
        fflush( stderr );
    }
    if (trace_registerclass)
    {
        TEB *teb = NtCurrentTeb();

        fprintf( stderr, "macrunner-hb-registerclass-call: after import=%s!%s pc=%p ret_addr=%p "
                 "rc=%p last_error=%lu last_status=%08lx\n",
                 thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                 (void *)(uintptr_t)ret_addr, (void *)(uintptr_t)rc,
                 teb ? (unsigned long)teb->LastErrorValue : 0,
                 teb ? (unsigned long)teb->LastStatusValue : 0 );
        fflush( stderr );
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
         macrunner_hb_strieq( thunk->import_name, "GetFileInformationByHandle" ) ||
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
            else
            {
                ERR( "MacRunner HyperBridge failed to register dynamic proc %s!%s native=%p; "
                     "not returning raw native pointer to x64 guest\n",
                     thunk->dll_name, proc_name, (void *)(uintptr_t)rc );
                RtlSetLastWin32Error( ERROR_NOT_ENOUGH_MEMORY );
                NtCurrentTeb()->LastStatusValue = STATUS_NO_MEMORY;
                rc = 0;
            }
        }
    }
    ctx->regs.x64.rax = rc;
    ctx->regs.x64.rsp += 8;
    ctx->pc = ret_addr;
    return HB_OK;
}

NTSTATUS macrunner_hb_x64_import_context( void *args )
{
    struct macrunner_hb_x64_import_context_params *params = args;
    struct macrunner_hb_import_thunk *thunk;
    struct macrunner_hb_special special;
    hb_context_t *ctx = NULL;
    TEB *teb;
    CHPE_V2_CPU_AREA_INFO *cpu_area;
    BOOL trace_import;
    void *old_bridge_stack_limit = macrunner_hb_bridge_stack_limit;
    void *old_bridge_stack_base = macrunner_hb_bridge_stack_base;
    size_t old_bridge_stack_size = macrunner_hb_bridge_stack_size;
    void *old_original_stack_limit = macrunner_hb_original_stack_limit;
    void *old_original_stack_base = macrunner_hb_original_stack_base;
    hb_result_t ret;

    if (!params) return STATUS_INVALID_PARAMETER;
    params->handled = 0;
    params->status = STATUS_NOT_FOUND;

#ifndef __aarch64__
    return STATUS_NOT_IMPLEMENTED;
#else
    if (!(ctx = hb_context_create( HB_ARCH_X64, HB_BACKEND_INTERP ))) return STATUS_NO_MEMORY;
    ctx->memory = hb_memory_create( 0 );
    if (!ctx->memory)
    {
        hb_context_destroy( ctx );
        return STATUS_NO_MEMORY;
    }

    memset( &special, 0, sizeof(special) );
    teb = NtCurrentTeb();
    cpu_area = teb ? teb->ChpeV2CpuAreaInfo : NULL;
    if (cpu_area && cpu_area->EmulatorStackBase > cpu_area->EmulatorStackLimit)
    {
        macrunner_hb_bridge_stack_limit = (void *)(ULONG_PTR)cpu_area->EmulatorStackLimit;
        macrunner_hb_bridge_stack_base = (void *)(ULONG_PTR)cpu_area->EmulatorStackLimit;
        macrunner_hb_bridge_stack_size = cpu_area->EmulatorStackBase - cpu_area->EmulatorStackLimit;
        macrunner_hb_original_stack_limit = teb->Tib.StackLimit;
        macrunner_hb_original_stack_base = teb->Tib.StackBase;
    }
    special.mem = ctx->memory;
    special.teb = teb;
    special.peb = teb ? teb->Peb : NULL;
    hb_memory_set_special_handlers( ctx->memory, macrunner_hb_special_read,
                                    macrunner_hb_special_write, &special );

    ret = macrunner_hb_map_live_address_space( ctx->memory );
    if (ret != HB_OK)
    {
        params->status = STATUS_NO_MEMORY;
        goto done;
    }
    macrunner_hb_replay_virtual_regions( ctx );

    ctx->regs.x64.rax = params->rax;
    ctx->regs.x64.rbx = params->rbx;
    ctx->regs.x64.rcx = params->rcx;
    ctx->regs.x64.rdx = params->rdx;
    ctx->regs.x64.rsi = params->rsi;
    ctx->regs.x64.rdi = params->rdi;
    ctx->regs.x64.rsp = params->rsp;
    ctx->regs.x64.rbp = params->rbp;
    ctx->regs.x64.r8  = params->r8;
    ctx->regs.x64.r9  = params->r9;
    ctx->regs.x64.r10 = params->r10;
    ctx->regs.x64.r11 = params->r11;
    ctx->regs.x64.r12 = params->r12;
    ctx->regs.x64.r13 = params->r13;
    ctx->regs.x64.r14 = params->r14;
    ctx->regs.x64.r15 = params->r15;
    ctx->regs.x64.rip = params->rip;
    ctx->regs.x64.rflags = params->rflags;
    memcpy( ctx->regs.x64.xmm, params->xmm, sizeof(ctx->regs.x64.xmm) );
    ctx->pc = params->rip;
    ctx->fs_base = params->fs_base;
    ctx->gs_base = params->gs_base;
    ctx->seg_cs = params->seg_cs;
    ctx->seg_ds = params->seg_ds;
    ctx->seg_es = params->seg_es;
    ctx->seg_fs = params->seg_fs;
    ctx->seg_gs = params->seg_gs;
    ctx->seg_ss = params->seg_ss;

    if (!(thunk = macrunner_hb_find_import_thunk( ctx->pc )))
        goto done;

    trace_import = macrunner_hb_env_enabled( "MACRUNNER_HB_TRACE_XTAJIT64_IMPORT" );
    if (trace_import)
    {
        fprintf( stderr, "macrunner-xtajit64-import: before import=%s!%s pc=%p rsp=%p target=%p\n",
                 thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                 (void *)(uintptr_t)ctx->regs.x64.rsp, thunk->target );
        fflush( stderr );
    }
    ret = macrunner_hb_call_import_thunk( ctx, thunk );
    if (trace_import)
    {
        fprintf( stderr, "macrunner-xtajit64-import: after import=%s!%s result=%s pc=%p rsp=%p rax=%p\n",
                 thunk->dll_name, thunk->import_name, hb_result_string(ret),
                 (void *)(uintptr_t)ctx->pc, (void *)(uintptr_t)ctx->regs.x64.rsp,
                 (void *)(uintptr_t)ctx->regs.x64.rax );
        fflush( stderr );
    }
    if (ret != HB_OK)
    {
        ERR( "MacRunner xtajit64 import handoff failed pc=%p %s!%s result=%s\n",
             (void *)(uintptr_t)ctx->pc, thunk->dll_name, thunk->import_name, hb_result_string(ret) );
        params->status = STATUS_INVALID_IMAGE_FORMAT;
        goto done;
    }

    params->rax = ctx->regs.x64.rax;
    params->rbx = ctx->regs.x64.rbx;
    params->rcx = ctx->regs.x64.rcx;
    params->rdx = ctx->regs.x64.rdx;
    params->rsi = ctx->regs.x64.rsi;
    params->rdi = ctx->regs.x64.rdi;
    params->rsp = ctx->regs.x64.rsp;
    params->rbp = ctx->regs.x64.rbp;
    params->r8  = ctx->regs.x64.r8;
    params->r9  = ctx->regs.x64.r9;
    params->r10 = ctx->regs.x64.r10;
    params->r11 = ctx->regs.x64.r11;
    params->r12 = ctx->regs.x64.r12;
    params->r13 = ctx->regs.x64.r13;
    params->r14 = ctx->regs.x64.r14;
    params->r15 = ctx->regs.x64.r15;
    params->rip = ctx->pc;
    params->rflags = ctx->regs.x64.rflags;
    memcpy( params->xmm, ctx->regs.x64.xmm, sizeof(params->xmm) );
    params->fs_base = ctx->fs_base;
    params->gs_base = ctx->gs_base;
    params->seg_cs = ctx->seg_cs;
    params->seg_ds = ctx->seg_ds;
    params->seg_es = ctx->seg_es;
    params->seg_fs = ctx->seg_fs;
    params->seg_gs = ctx->seg_gs;
    params->seg_ss = ctx->seg_ss;
    params->handled = 1;
    params->status = STATUS_SUCCESS;

    TRACE( "MacRunner xtajit64 import handoff %s!%s next=%p rax=%p rsp=%p\n",
           thunk->dll_name, thunk->import_name, (void *)(uintptr_t)params->rip,
           (void *)(uintptr_t)params->rax, (void *)(uintptr_t)params->rsp );

done:
    macrunner_hb_bridge_stack_limit = old_bridge_stack_limit;
    macrunner_hb_bridge_stack_base = old_bridge_stack_base;
    macrunner_hb_bridge_stack_size = old_bridge_stack_size;
    macrunner_hb_original_stack_limit = old_original_stack_limit;
    macrunner_hb_original_stack_base = old_original_stack_base;
    if (ctx)
    {
        if (ctx->memory) hb_memory_destroy( ctx->memory );
        ctx->memory = NULL;
        hb_context_destroy( ctx );
    }
    return STATUS_SUCCESS;
#endif
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
    if ((macrunner_hb_strieq( thunk->dll_name, "ole32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "combase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "CoInitialize" ) ||
         macrunner_hb_strieq( thunk->import_name, "CoInitializeEx" ) ||
         macrunner_hb_strieq( thunk->import_name, "CoUninitialize" )))
        return TRUE;
    if ((macrunner_hb_strieq( thunk->dll_name, "kernel32.dll" ) ||
         macrunner_hb_strieq( thunk->dll_name, "kernelbase.dll" )) &&
        (macrunner_hb_strieq( thunk->import_name, "GetModuleHandleW" ) ||
         macrunner_hb_strieq( thunk->import_name, "LoadLibraryA" ) ||
         macrunner_hb_strieq( thunk->import_name, "LoadLibraryW" ) ||
         macrunner_hb_strieq( thunk->import_name, "LoadLibraryExA" ) ||
         macrunner_hb_strieq( thunk->import_name, "LoadLibraryExW" ) ||
         macrunner_hb_strieq( thunk->import_name, "FreeLibrary" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetProcAddress" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetEnvironmentVariableA" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetEnvironmentVariableW" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetEnvironmentVariableA" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetEnvironmentVariableW" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetCommandLineA" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetCommandLineW" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetModuleFileNameA" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetModuleFileNameW" ) ||
         macrunner_hb_strieq( thunk->import_name, "QueryPerformanceFrequency" ) ||
         macrunner_hb_strieq( thunk->import_name, "QueryPerformanceCounter" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetSystemTimePreciseAsFileTime" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetSystemTime" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetLocalTime" ) ||
         macrunner_hb_strieq( thunk->import_name, "SystemTimeToFileTime" ) ||
         macrunner_hb_strieq( thunk->import_name, "FileTimeToSystemTime" ) ||
         macrunner_hb_strieq( thunk->import_name, "SystemTimeToTzSpecificLocalTime" ) ||
         macrunner_hb_strieq( thunk->import_name, "TzSpecificLocalTimeToSystemTime" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetTempPathA" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetTempPathW" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetTempPath2A" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetTempPath2W" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetACP" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetOEMCP" ) ||
         macrunner_hb_strieq( thunk->import_name, "AreFileApisANSI" ) ||
         macrunner_hb_strieq( thunk->import_name, "IsValidCodePage" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetCPInfo" ) ||
         macrunner_hb_strieq( thunk->import_name, "MultiByteToWideChar" ) ||
         macrunner_hb_strieq( thunk->import_name, "WideCharToMultiByte" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetStringTypeW" ) ||
         macrunner_hb_strieq( thunk->import_name, "LCMapStringEx" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetStartupInfoA" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetStartupInfoW" ) ||
         macrunner_hb_strieq( thunk->import_name, "FindResourceW" ) ||
         macrunner_hb_strieq( thunk->import_name, "FindResourceExW" ) ||
         macrunner_hb_strieq( thunk->import_name, "LoadResource" ) ||
         macrunner_hb_strieq( thunk->import_name, "LockResource" ) ||
         macrunner_hb_strieq( thunk->import_name, "SizeofResource" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateFileW" ) ||
         macrunner_hb_strieq( thunk->import_name, "ReadFile" ) ||
         macrunner_hb_strieq( thunk->import_name, "WriteFile" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetFileType" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetFileSize" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetFileSizeEx" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetFileInformationByHandle" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetFilePointer" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetFilePointerEx" ) ||
         macrunner_hb_strieq( thunk->import_name, "HeapAlloc" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetSystemTimeAsFileTime" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetTimeZoneInformation" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateThread" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateRemoteThread" ) ||
         macrunner_hb_strieq( thunk->import_name, "CreateRemoteThreadEx" ) ||
         macrunner_hb_strieq( thunk->import_name, "ResumeThread" ) ||
         macrunner_hb_strieq( thunk->import_name, "SuspendThread" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetThreadDescription" ) ||
         macrunner_hb_strieq( thunk->import_name, "WaitForMultipleObjects" ) ||
         macrunner_hb_strieq( thunk->import_name, "WaitForMultipleObjectsEx" ) ||
         macrunner_hb_strieq( thunk->import_name, "WaitForSingleObject" ) ||
         macrunner_hb_strieq( thunk->import_name, "WaitForSingleObjectEx" ) ||
         macrunner_hb_strieq( thunk->import_name, "CloseHandle" ) ||
         macrunner_hb_strieq( thunk->import_name, "AddVectoredExceptionHandler" ) ||
         macrunner_hb_strieq( thunk->import_name, "RemoveVectoredExceptionHandler" ) ||
         macrunner_hb_strieq( thunk->import_name, "RaiseException" ) ||
         macrunner_hb_strieq( thunk->import_name, "GetLastError" ) ||
         macrunner_hb_strieq( thunk->import_name, "SetLastError" )))
        return TRUE;
    if (macrunner_hb_strieq( thunk->dll_name, "ntdll.dll" ) &&
        (macrunner_hb_strieq( thunk->import_name, "RtlAddVectoredExceptionHandler" ) ||
         macrunner_hb_strieq( thunk->import_name, "RtlRemoveVectoredExceptionHandler" ) ||
         macrunner_hb_strieq( thunk->import_name, "RtlGetLastWin32Error" ) ||
         macrunner_hb_strieq( thunk->import_name, "RtlSetLastWin32Error" )))
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

static BOOL macrunner_hb_is_pseudo_user32_resource( uint64_t handle )
{
    return (handle & ~0xffffULL) == MACRUNNER_HB_PSEUDO_HCURSOR_BASE ||
           (handle & ~0xffffULL) == MACRUNNER_HB_PSEUDO_HICON_BASE;
}

static void macrunner_hb_normalize_window_class_resources( const struct macrunner_hb_import_thunk *thunk,
                                                           uint64_t class_ptr )
{
    if (!thunk || !class_ptr) return;

    if (macrunner_hb_strieq( thunk->import_name, "RegisterClassExA" ) ||
        macrunner_hb_strieq( thunk->import_name, "RegisterClassExW" ))
    {
        WNDCLASSEXW *cls = (WNDCLASSEXW *)(uintptr_t)class_ptr;

        if (cls->cbSize >= sizeof(*cls))
        {
            if (macrunner_hb_is_pseudo_user32_resource( (uint64_t)(uintptr_t)cls->hIcon ))
                cls->hIcon = NULL;
            if (macrunner_hb_is_pseudo_user32_resource( (uint64_t)(uintptr_t)cls->hCursor ))
                cls->hCursor = NULL;
            if (macrunner_hb_is_pseudo_user32_resource( (uint64_t)(uintptr_t)cls->hIconSm ))
                cls->hIconSm = NULL;
        }
    }
    else if (macrunner_hb_strieq( thunk->import_name, "RegisterClassA" ) ||
             macrunner_hb_strieq( thunk->import_name, "RegisterClassW" ))
    {
        WNDCLASSW *cls = (WNDCLASSW *)(uintptr_t)class_ptr;

        if (macrunner_hb_is_pseudo_user32_resource( (uint64_t)(uintptr_t)cls->hIcon ))
            cls->hIcon = NULL;
        if (macrunner_hb_is_pseudo_user32_resource( (uint64_t)(uintptr_t)cls->hCursor ))
            cls->hCursor = NULL;
    }
}

static void macrunner_hb_normalize_import_args( const struct macrunner_hb_import_thunk *thunk,
                                                uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX] )
{
    if (!thunk || !args) return;

    if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
        (macrunner_hb_strieq( thunk->import_name, "RegisterClassA" ) ||
         macrunner_hb_strieq( thunk->import_name, "RegisterClassW" ) ||
         macrunner_hb_strieq( thunk->import_name, "RegisterClassExA" ) ||
         macrunner_hb_strieq( thunk->import_name, "RegisterClassExW" )))
    {
        macrunner_hb_normalize_window_class_resources( thunk, args[0] );
    }
    else if (macrunner_hb_strieq( thunk->dll_name, "user32.dll" ) &&
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
         label ? label : "?", (void *)(uintptr_t)addr, (long)rect.left, (long)rect.top,
         (long)rect.right, (long)rect.bottom, (long)(rect.right - rect.left),
         (long)(rect.bottom - rect.top) );
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
         label ? label : "?", (void *)(uintptr_t)addr, (unsigned long)info.cbSize,
         (long)info.rcMonitor.left, (long)info.rcMonitor.top, (long)info.rcMonitor.right,
         (long)info.rcMonitor.bottom, (long)info.rcWork.left, (long)info.rcWork.top,
         (long)info.rcWork.right, (long)info.rcWork.bottom, (unsigned long)info.dwFlags,
         (long)(info.rcMonitor.right - info.rcMonitor.left),
         (long)(info.rcMonitor.bottom - info.rcMonitor.top),
         (long)(info.rcWork.right - info.rcWork.left),
         (long)(info.rcWork.bottom - info.rcWork.top) );
}

static void macrunner_hb_trace_geometry_return( hb_context_t *ctx,
                                                const struct macrunner_hb_import_thunk *thunk,
                                                uint64_t value, const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX] )
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
                                           const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX] )
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
         (unsigned long)(teb ? teb->LastErrorValue : 0),
         (unsigned long)(teb ? teb->LastStatusValue : 0),
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
                                          uint64_t ret_addr, const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX] )
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
    if (!ctx || !ctx->memory) return;
    fprintf( stderr, "macrunner-hb-nonexec-pc: label=%s pc=%llx rva=%llx "
             "blocks=%llu steps=%llu rsp=%llx rax=%llx rcx=%llx rdx=%llx\n",
             label ? label : "entry",
             (unsigned long long)ctx->pc,
             (unsigned long long)(ctx->pc - image_start),
             (unsigned long long)blocks, (unsigned long long)steps,
             (unsigned long long)ctx->regs.x64.rsp,
             (unsigned long long)ctx->regs.x64.rax,
             (unsigned long long)ctx->regs.x64.rcx,
             (unsigned long long)ctx->regs.x64.rdx );
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
    if (nt && macrunner_hb_get_arm64x_metadata( base ))
    {
        if (module_base) *module_base = base;
        return TRUE;
    }
    if (!nt || (nt->FileHeader.Machine != current_machine &&
                nt->FileHeader.Machine != IMAGE_FILE_MACHINE_ARM64X &&
                nt->FileHeader.Machine != IMAGE_FILE_MACHINE_ARM64EC))
        return FALSE;
    if (module_base) *module_base = base;
    return TRUE;
}

static BOOL macrunner_hb_pc_is_unix_call_dispatcher( uint64_t pc )
{
    Dl_info dli = {0};

    if (!pc || !dladdr( (void *)(uintptr_t)pc, &dli )) return FALSE;
    if (!dli.dli_sname ||
        (strcmp( dli.dli_sname, "__wine_unix_call_dispatcher" ) &&
         strcmp( dli.dli_sname, "__wine_syscall_dispatcher" ))) return FALSE;
    if (!dli.dli_fname || !strstr( dli.dli_fname, "/ntdll.so" )) return FALSE;
    return TRUE;
}

static BOOL macrunner_hb_label_allows_direct_native( const char *label )
{
    return label && (!strcmp( label, "x64-wndproc" ) ||
                     !strcmp( label, "x64-subclassproc" ) ||
                     !strcmp( label, "x64-signal-callback" ) ||
                     !strcmp( label, "dll" ) ||
                     !strcmp( label, "thread" ));
}

static BOOL macrunner_hb_pc_is_syscall_dispatcher( uint64_t pc )
{
    return pc && (pc == (uint64_t)(uintptr_t)__wine_syscall_dispatcher ||
                  macrunner_hb_pc_is_unix_call_dispatcher( pc ));
}

static void macrunner_hb_ensure_win32u_syscall_table(void)
{
    static int attempted;
    void *handle;
    const unixlib_entry_t *funcs;

    if (KeServiceDescriptorTable[1].ServiceLimit || attempted) return;
    attempted = 1;

    handle = dlopen( "@rpath/win32u.so", RTLD_NOW | RTLD_GLOBAL );
    if (!handle) handle = dlopen( "win32u.so", RTLD_NOW | RTLD_GLOBAL );
    if (!handle) return;

    funcs = dlsym( handle, "__wine_unix_call_funcs" );
    if (funcs && funcs[0]) funcs[0]( NULL );
}

static hb_result_t macrunner_hb_dispatch_x64_syscall( hb_context_t *ctx, uint64_t target,
                                                      uint64_t ret_addr )
{
    struct macrunner_hb_import_thunk thunk;
    const UINT service = (UINT)ctx->regs.x64.rax;
    const UINT id = service & 0xfff;
    const UINT table_idx = (service >> 12) & 3;
    SYSTEM_SERVICE_TABLE *table = &KeServiceDescriptorTable[table_idx];
    uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX] = { 0 };
    uint64_t rc;
    unsigned int i;

    if (table_idx == 1 && !table->ServiceLimit)
    {
        macrunner_hb_ensure_win32u_syscall_table();
        table = &KeServiceDescriptorTable[table_idx];
    }

    if (id >= table->ServiceLimit)
        rc = STATUS_INVALID_SYSTEM_SERVICE;
    else
    {
        args[0] = ctx->regs.x64.rcx;
        args[1] = ctx->regs.x64.rdx;
        args[2] = ctx->regs.x64.r8;
        args[3] = ctx->regs.x64.r9;
        for (i = 4; i < MACRUNNER_HB_IMPORT_ARG_MAX; i++)
            hb_memory_read_u64( ctx->memory, (hb_gva_t)ctx->regs.x64.rsp + 8 + 32 + (i - 4) * 8,
                                &args[i] );

        memset( &thunk, 0, sizeof(thunk) );
        thunk.target = (void *)table->ServiceTable[id];
        thunk.target_machine = current_machine;
        strcpy( thunk.dll_name, "native-syscall" );
        snprintf( thunk.import_name, sizeof(thunk.import_name), "%04x", service );

        rc = macrunner_hb_call_arm64_pe_import12_for_ctx( ctx, &thunk, args );
    }

    if (macrunner_hb_trace_direct_native_enabled() &&
        macrunner_hb_trace_direct_native_budget_allows())
        fprintf( stderr, "macrunner-hb-x64-syscall: target=%p service=%04x table=%u id=%u "
                 "limit=%lu ret=%p rc=%p\n",
                 (void *)(uintptr_t)target, service, table_idx, id,
                 (unsigned long)table->ServiceLimit, (void *)(uintptr_t)ret_addr,
                 (void *)(uintptr_t)rc );

    macrunner_hb_finish_import( ctx, ret_addr, rc );
    return HB_OK;
}

static hb_result_t macrunner_hb_call_direct_native_target( hb_context_t *ctx, uint64_t target )
{
    struct macrunner_hb_import_thunk *registered;
    struct macrunner_hb_import_thunk thunk;
    void *native_module = NULL;
    char native_module_name[96];
    uint64_t ret_addr = 0;
    uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX] = { 0 };
    uint64_t rc;
    unsigned int i;

    if (!ctx || !ctx->memory || !target) return HB_ERR_INVALID_ARG;
    if ((registered = macrunner_hb_find_import_thunk_by_target( target )))
    {
        TRACE( "MacRunner HyperBridge routing direct native target through import dispatcher %s!%s target=%p\n",
               registered->dll_name, registered->import_name, (void *)(uintptr_t)target );
        return macrunner_hb_call_import_thunk( ctx, registered );
    }

    if (hb_memory_read_u64( ctx->memory, (hb_gva_t)ctx->regs.x64.rsp, &ret_addr ) != HB_OK || !ret_addr)
        return HB_ERR_EXEC_FAULT;

    if (macrunner_hb_pc_is_syscall_dispatcher( target ))
        return macrunner_hb_dispatch_x64_syscall( ctx, target, ret_addr );

    args[0] = ctx->regs.x64.rcx;
    args[1] = ctx->regs.x64.rdx;
    args[2] = ctx->regs.x64.r8;
    args[3] = ctx->regs.x64.r9;
    for (i = 4; i < MACRUNNER_HB_IMPORT_ARG_MAX; i++)
        hb_memory_read_u64( ctx->memory, (hb_gva_t)ctx->regs.x64.rsp + 8 + 32 + (i - 4) * 8, &args[i] );

    native_module_name[0] = 0;
    native_module = macrunner_hb_module_from_pc( (void *)(uintptr_t)target );
    if (native_module &&
        macrunner_hb_address_in_section( native_module, ".hexpthk", target ))
    {
        void *native_target = macrunner_hb_redirect_arm64x_thunk_to_native( native_module,
                                                                            (void *)(uintptr_t)target );

        if (native_target && native_target != (void *)(uintptr_t)target)
            target = (uint64_t)(uintptr_t)native_target;
    }
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
    uint64_t original_target;
    ULONG64 ret = 0, blocks = 0, steps = 0;
    void *target_module;
    NTSTATUS status;

    if (!target || !args) return 0;
    original_target = target;
    target = macrunner_hb_normalize_x64_tls_callback_pc( target, args[0], args[1] );
    target_module = macrunner_hb_module_from_pc( (void *)(uintptr_t)target );

    if (macrunner_hb_env_enabled( "MACRUNNER_HB_TRACE_CALLBACK_ROUTE" ))
    {
        LDR_DATA_TABLE_ENTRY *ldr;
        char module_name[96], ldr_base[128], ldr_full[256];
        uintptr_t rva = target_module ? (uintptr_t)target - (uintptr_t)target_module : 0;

        macrunner_hb_get_export_module_name( target_module, module_name, sizeof(module_name) );
        ldr_base[0] = ldr_full[0] = 0;
        if ((ldr = macrunner_hb_ldr_entry_from_pc( (void *)(uintptr_t)target )))
        {
            macrunner_hb_copy_unicode_ascii( ldr_base, sizeof(ldr_base), &ldr->BaseDllName );
            macrunner_hb_copy_unicode_ascii( ldr_full, sizeof(ldr_full), &ldr->FullDllName );
        }
        fprintf( stderr, "macrunner-hb-callback-target-ldr: target=%p module=%s base=%p rva=0x%zx "
                 "ldr_base=%s ldr_full=%s\n",
                 (void *)(uintptr_t)target, module_name, target_module, rva,
                 ldr_base[0] ? ldr_base : "(none)", ldr_full[0] ? ldr_full : "(none)" );
        fprintf( stderr, "macrunner-hb-callback-dispatch: target=%p "
                 "original=%p x0=%p x1=%p x2=%p x3=%p x4=%p x5=%p x6=%p x7=%p\n",
                 (void *)(uintptr_t)target, (void *)(uintptr_t)original_target,
                 (void *)(uintptr_t)args[0], (void *)(uintptr_t)args[1],
                 (void *)(uintptr_t)args[2], (void *)(uintptr_t)args[3],
                 (void *)(uintptr_t)args[4], (void *)(uintptr_t)args[5],
                 (void *)(uintptr_t)args[6], (void *)(uintptr_t)args[7] );
    }

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
                                   target_module ? target_module : NtCurrentTeb()->Peb->ImageBaseAddress );
    if (status)
    {
        ERR( "MacRunner Phase F x64 callback failed target=%p status=%lx blocks=%s steps=%s\n",
             (void *)(uintptr_t)target, (unsigned long)status, wine_dbgstr_longlong(blocks),
             wine_dbgstr_longlong(steps) );
        return 0;
    }
    if (macrunner_hb_env_enabled( "MACRUNNER_HB_TRACE_CALLBACK_ROUTE" ))
        fprintf( stderr, "macrunner-hb-callback-return: target=%p ret=%p blocks=%s steps=%s\n",
                 (void *)(uintptr_t)target, (void *)(uintptr_t)ret,
                 wine_dbgstr_longlong(blocks), wine_dbgstr_longlong(steps) );
    TRACE( "MacRunner Phase F x64 callback returned target=%p ret=%p blocks=%s steps=%s\n",
           (void *)(uintptr_t)target, (void *)(uintptr_t)ret,
           wine_dbgstr_longlong(blocks), wine_dbgstr_longlong(steps) );
    macrunner_hb_trace_x64_callback_abi( "after", target, args, ret );
    return ret;
}

static NTSTATUS macrunner_hb_run_x64( void *entry, hb_abi_x64_call_t *call, ULONG64 *ret_value,
                                      ULONG64 *blocks_out, ULONG64 *steps_out,
                                      const char *label, void *image_base )
{
    struct macrunner_hb_special special;
    struct macrunner_hb_owned_ir_func
    {
        hb_ir_func_t *func;
        struct macrunner_hb_owned_ir_func *next;
    };
    TEB *teb;
    hb_context_t *ctx = NULL;
    hb_jit_runtime_t *jit_rt = NULL;
    struct macrunner_hb_ir_cache *ir_cache = NULL;
    struct macrunner_hb_owned_ir_func *jit_owned_ir = NULL;
    hb_exec_result_t out;
    ULONG64 blocks = 0, steps = 0, jit_fallbacks = 0;
    uint64_t last_block_pc = 0;
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
    void *old_teb_deallocation_stack = NULL;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    const char *status_reason = "uninitialised";
    const char *progress_env = getenv( "MACRUNNER_HB_TRACE_PROGRESS" );
    const char *heartbeat_env = getenv( "MACRUNNER_HB_TRACE_HEARTBEAT" );
    const char *backend_env = getenv( "MACRUNNER_HB_BACKEND" );
    char *progress_end = NULL;
    uint64_t progress_interval = 0;
    BOOL heartbeat_enabled = heartbeat_env && *heartbeat_env && *heartbeat_env != '0';
    hb_backend_t backend = (backend_env && (!strcmp( backend_env, "jit" ) ||
                                            !strcmp( backend_env, "JIT" ))) ?
                           HB_BACKEND_JIT : HB_BACKEND_INTERP;
    uint64_t heartbeat_last_us = 0;
    uint64_t heartbeat_next_block = 1000;
    hb_result_t ret;
    int debug_enabled = macrunner_hb_debug_enabled();
    int trace_calc_object = macrunner_hb_trace_calc_object_enabled();
    int trace_npp_open_pack = macrunner_hb_trace_npp_open_pack_enabled();
    int trace_low_stack = macrunner_hb_env_enabled( "MACRUNNER_HB_TRACE_LOW_STACK" );
    int trace_thread_run = label && !strcmp( label, "thread" ) &&
                           macrunner_hb_trace_thread_lifecycle_enabled();

    if (progress_env && *progress_env && *progress_env != '0')
    {
        progress_interval = strtoull( progress_env, &progress_end, 0 );
        if (!progress_interval || progress_interval < 1000) progress_interval = 100000;
    }
    if (heartbeat_enabled) heartbeat_last_us = macrunner_hb_now_us();

    if (!entry || !call) return STATUS_INVALID_PARAMETER;
    if (ret_value) *ret_value = 0;
    if (blocks_out) *blocks_out = 0;
    if (steps_out) *steps_out = 0;

#ifndef __aarch64__
    return STATUS_NOT_IMPLEMENTED;
#else
    if (debug_enabled)
        ERR( "MacRunner HyperBridge run begin %s entry=%p image=%p\n",
             label ? label : "x64", entry, image_base );
    if (trace_thread_run)
    {
        fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_enter label=%s entry=%p image=%p "
                 "image_size=%p backend=%s block_limit=%s step_limit=%s stack_size=%zu "
                 "old_teb_stack=%p-%p\n",
                 label, entry, image_base, (void *)(uintptr_t)image_size,
                 backend == HB_BACKEND_JIT ? "jit" : "interp",
                 wine_dbgstr_longlong(block_limit), wine_dbgstr_longlong(step_limit),
                 stack_size, old_teb_stack_limit, old_teb_stack_base );
        fflush( stderr );
    }
    ctx = hb_context_create( HB_ARCH_X64, backend );
    if (!ctx) return STATUS_NO_MEMORY;
    hb_context_set_block_limit( ctx, block_limit );
    hb_context_set_step_limit( ctx, step_limit );
    ctx->memory = hb_memory_create( 0 );
    if (!ctx->memory)
    {
        hb_context_destroy( ctx );
        return STATUS_NO_MEMORY;
    }
    if (trace_thread_run)
    {
        fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_context_ready label=%s ctx=%p memory=%p\n",
                 label, ctx, ctx->memory );
        fflush( stderr );
    }

    teb = NtCurrentTeb();
    special.mem = ctx->memory;
    special.teb = teb;
    special.peb = teb->Peb;
    ctx->gs_base = (uint64_t)(uintptr_t)special.teb;
    ctx->fs_base = (uint64_t)(uintptr_t)special.teb;
    old_teb_stack_limit = teb->Tib.StackLimit;
    old_teb_stack_base = teb->Tib.StackBase;
    old_teb_deallocation_stack = teb->DeallocationStack;
    hb_memory_set_special_handlers( ctx->memory, macrunner_hb_special_read,
                                    macrunner_hb_special_write, &special );

    ret = macrunner_hb_map_live_address_space( ctx->memory );
    if (debug_enabled)
        ERR( "MacRunner HyperBridge after live-map %s entry=%p result=%s\n",
             label ? label : "x64", entry, hb_result_string(ret) );
    if (trace_thread_run)
    {
        fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_live_map label=%s result=%s\n",
                 label, hb_result_string(ret) );
        fflush( stderr );
    }
    if (ret != HB_OK)
    {
        ERR( "MacRunner HyperBridge live map failed %s entry=%p result=%s\n",
             label ? label : "x64", entry, hb_result_string(ret) );
        status = STATUS_NO_MEMORY;
        status_reason = "live-map";
        goto done;
    }
    macrunner_hb_replay_virtual_regions( ctx );
    if (trace_thread_run)
    {
        fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_virtual_replay_done label=%s\n", label );
        fflush( stderr );
    }
    ret = macrunner_hb_mark_x64_image_exec_sections( ctx->memory, image_base );
    if (trace_thread_run)
    {
        fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_exec_overlay label=%s image=%p result=%s\n",
                 label, image_base, hb_result_string(ret) );
        fflush( stderr );
    }
    if (ret != HB_OK)
    {
        ERR( "MacRunner HyperBridge x64 exec overlay failed %s image=%p result=%s\n",
             label ? label : "x64", image_base, hb_result_string(ret) );
        status = STATUS_INVALID_IMAGE_FORMAT;
        status_reason = "exec-overlay";
        goto done;
    }

    if (trace_thread_run)
    {
        fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_before_stack label=%s stack_size=%zu\n",
                 label, stack_size );
        fflush( stderr );
    }
    ret = macrunner_hb_setup_bridge_stack( ctx, stack_size, &stack_base );
    if (debug_enabled)
        ERR( "MacRunner HyperBridge after stack %s entry=%p result=%s stack=%p\n",
             label ? label : "x64", entry, hb_result_string(ret), stack_base );
    if (trace_thread_run)
    {
        fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_after_stack label=%s result=%s "
                 "stack=%p stack_top=%p memory_stack=%p-%p\n",
                 label, hb_result_string(ret), stack_base, stack_base ? (char *)stack_base + stack_size : NULL,
                 (void *)(uintptr_t)ctx->memory->stack_bottom, (void *)(uintptr_t)ctx->memory->stack_top );
        fflush( stderr );
    }
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
    teb->DeallocationStack = macrunner_hb_bridge_stack_limit;
    teb->Tib.StackLimit = macrunner_hb_bridge_stack_base;
    teb->Tib.StackBase = (char *)macrunner_hb_bridge_stack_base + macrunner_hb_bridge_stack_size;
    if (trace_thread_run)
    {
        fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_teb_stack_swapped label=%s "
                 "teb_stack=%p-%p bridge=%p-%p original=%p-%p\n",
                 label, teb->Tib.StackLimit, teb->Tib.StackBase,
                 macrunner_hb_bridge_stack_limit,
                 (char *)macrunner_hb_bridge_stack_base + macrunner_hb_bridge_stack_size,
                 macrunner_hb_original_stack_limit, macrunner_hb_original_stack_base );
        fflush( stderr );
    }

    if (trace_thread_run)
    {
        fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_before_abi label=%s entry=%p "
                 "rcx=%p rdx=%p r8=%p r9=%p stack_args=%zu\n",
                 label, entry, (void *)(uintptr_t)call->rcx, (void *)(uintptr_t)call->rdx,
                 (void *)(uintptr_t)call->r8, (void *)(uintptr_t)call->r9, call->stack_arg_count );
        fflush( stderr );
    }
    ret = hb_abi_x64_call( ctx, (uint64_t)(uintptr_t)entry, call, NULL );
    if (debug_enabled)
        ERR( "MacRunner HyperBridge after abi %s entry=%p result=%s pc=%p rsp=%p\n",
             label ? label : "x64", entry, hb_result_string(ret),
             (void *)(uintptr_t)ctx->pc, (void *)(uintptr_t)ctx->regs.x64.rsp );
    if (trace_thread_run)
    {
        fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_after_abi label=%s result=%s "
                 "pc=%p rip=%p rsp=%p shadow_ret=%p\n",
                 label, hb_result_string(ret), (void *)(uintptr_t)ctx->pc,
                 (void *)(uintptr_t)ctx->regs.x64.rip, (void *)(uintptr_t)ctx->regs.x64.rsp,
                 (void *)(uintptr_t)0xffff0000 );
        fflush( stderr );
    }
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

    ir_cache = calloc( 1, sizeof(*ir_cache) );
    if (!ir_cache)
    {
        status = STATUS_NO_MEMORY;
        status_reason = "ir-cache";
        goto done;
    }
    if (trace_thread_run)
    {
        fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_ir_cache_ready label=%s cache=%p\n",
                 label, ir_cache );
        fflush( stderr );
    }
    if (backend == HB_BACKEND_JIT)
    {
        jit_rt = hb_jit_runtime_create( ctx );
        if (trace_thread_run)
        {
            fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_jit_runtime label=%s jit=%p\n",
                     label, jit_rt );
            fflush( stderr );
        }
        if (!jit_rt)
        {
            status = STATUS_NO_MEMORY;
            status_reason = "jit-runtime";
            goto done;
        }
        if (debug_enabled)
            ERR( "MacRunner HyperBridge JIT backend enabled %s entry=%p\n",
                 label ? label : "x64", entry );
    }

    for (;;)
    {
        hb_ir_func_t *func = NULL;
        BOOL transient_func = FALSE;
        struct macrunner_hb_import_thunk *import_thunk;
        uint64_t block_pc;

        if (trace_thread_run && !blocks && !steps)
        {
            fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_first_loop label=%s pc=%p rip=%p "
                     "rsp=%p image=%p-%p\n",
                     label, (void *)(uintptr_t)ctx->pc, (void *)(uintptr_t)ctx->regs.x64.rip,
                     (void *)(uintptr_t)ctx->regs.x64.rsp, (void *)(uintptr_t)image_start,
                     (void *)(uintptr_t)(image_start + image_size) );
            fflush( stderr );
        }
        if (ctx->pc == 0xffff0000)
        {
            if (ret_value) *ret_value = ctx->regs.x64.rax;
            status = STATUS_SUCCESS;
            status_reason = "guest-return";
            if (macrunner_hb_trace_thread_lifecycle_enabled())
            {
                fprintf( stderr, "macrunner-ui-input: stage=hb_run_guest_return label=%s entry=%p last_block=%p "
                         "pc=%p ret=%p blocks=%s steps=%s rsp=%p\n",
                         label ? label : "entry", entry, (void *)(uintptr_t)last_block_pc,
                         (void *)(uintptr_t)ctx->pc, (void *)(uintptr_t)ctx->regs.x64.rax,
                         wine_dbgstr_longlong(blocks), wine_dbgstr_longlong(steps),
                         (void *)(uintptr_t)ctx->regs.x64.rsp );
                fflush( stderr );
            }
            break;
        }
        if ((import_thunk = macrunner_hb_find_import_thunk( ctx->pc )))
        {
            if (trace_thread_run)
            {
                fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_import_thunk "
                         "label=%s pc=%p import=%s!%s count=%u\n",
                         label, (void *)(uintptr_t)ctx->pc, import_thunk->dll_name,
                         import_thunk->import_name, macrunner_hb_import_count );
                fflush( stderr );
            }
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
        if (trace_thread_run)
        {
            fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_after_import_lookup "
                     "label=%s pc=%p import_count=%u\n",
                     label, (void *)(uintptr_t)ctx->pc, macrunner_hb_import_count );
            fflush( stderr );
        }
        if (!macrunner_hb_pc_in_image( ctx->pc, image_start, image_size ))
        {
            void *guest_module = macrunner_hb_module_from_pc( (void *)(uintptr_t)ctx->pc );
            void *native_module = NULL;

            if (guest_module &&
                macrunner_hb_module_machine( guest_module ) == IMAGE_FILE_MACHINE_AMD64 &&
                macrunner_hb_pc_in_executable_section( guest_module, ctx->pc ))
            {
                ret = macrunner_hb_mark_x64_image_exec_sections( ctx->memory, guest_module );
                if (ret == HB_ERR_NOT_FOUND || ret == HB_ERR_MEMORY_FAULT)
                {
                    hb_result_t remap = macrunner_hb_map_live_address_space( ctx->memory );
                    if (trace_thread_run)
                    {
                        fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_guest_module_remap "
                                 "label=%s pc=%p module=%p overlay=%s remap=%s\n",
                                 label, (void *)(uintptr_t)ctx->pc, guest_module,
                                 hb_result_string(ret), hb_result_string(remap) );
                        fflush( stderr );
                    }
                    if (remap == HB_OK)
                    {
                        macrunner_hb_replay_virtual_regions( ctx );
                        ret = macrunner_hb_mark_x64_image_exec_sections( ctx->memory, guest_module );
                    }
                }
                if (ret != HB_OK)
                {
                    ERR( "MacRunner HyperBridge x64 guest module exec overlay failed %s pc=%p module=%p result=%s\n",
                         label ? label : "entry", (void *)(uintptr_t)ctx->pc, guest_module,
                         hb_result_string(ret) );
                    status = STATUS_INVALID_IMAGE_FORMAT;
                    status_reason = "guest-module-exec-overlay";
                    break;
                }
                TRACE( "MacRunner HyperBridge switching x64 guest module %s pc=%p old=%p-%p new=%p-%p\n",
                       label ? label : "entry", (void *)(uintptr_t)ctx->pc,
                       (void *)(uintptr_t)image_start, (void *)(uintptr_t)(image_start + image_size),
                       guest_module, (char *)guest_module + macrunner_hb_module_size( guest_module ) );
                image_base = guest_module;
                image_start = (uint64_t)(uintptr_t)guest_module;
                image_size = macrunner_hb_module_size( guest_module );
            }
            if (macrunner_hb_pc_in_image( ctx->pc, image_start, image_size ))
                goto in_guest_image;

            if (macrunner_hb_label_allows_direct_native( label ) &&
                (macrunner_hb_pc_is_native_pe_builtin( ctx->pc, &native_module ) ||
                 macrunner_hb_pc_is_unix_call_dispatcher( ctx->pc )))
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
            {
                Dl_info dli = {0};
                const char *native_image = "?";
                const char *native_symbol = "?";
                void *native_base = NULL;

                if (dladdr( (void *)(uintptr_t)ctx->pc, &dli ))
                {
                    native_image = dli.dli_fname ? dli.dli_fname : "?";
                    native_symbol = dli.dli_sname ? dli.dli_sname : "?";
                    native_base = dli.dli_fbase;
                }
                ERR( "MacRunner HyperBridge refused non-application target %s pc=%p image=%p-%p "
                     "native_image=%s native_base=%p native_symbol=%s "
                     "rax=%p rcx=%p rdx=%p rsi=%p rdi=%p rsp=%p blocks=%s steps=%s\n",
                     label ? label : "entry", (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)image_start, (void *)(uintptr_t)(image_start + image_size),
                     native_image, native_base, native_symbol,
                     (void *)(uintptr_t)ctx->regs.x64.rax,
                     (void *)(uintptr_t)ctx->regs.x64.rcx,
                     (void *)(uintptr_t)ctx->regs.x64.rdx,
                     (void *)(uintptr_t)ctx->regs.x64.rsi,
                     (void *)(uintptr_t)ctx->regs.x64.rdi,
                     (void *)(uintptr_t)ctx->regs.x64.rsp,
                     wine_dbgstr_longlong(blocks), wine_dbgstr_longlong(steps) );
            }
            status = STATUS_INVALID_IMAGE_FORMAT;
            status_reason = "non-application-target";
            break;
        }
	in_guest_image:
        if (trace_thread_run)
        {
            fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_in_guest_image "
                     "label=%s pc=%p image=%p-%p\n",
                     label, (void *)(uintptr_t)ctx->pc, (void *)(uintptr_t)image_start,
                     (void *)(uintptr_t)(image_start + image_size) );
            fflush( stderr );
        }
        if (macrunner_hb_module_machine( (void *)(uintptr_t)image_base ) != IMAGE_FILE_MACHINE_AMD64 ||
            macrunner_hb_get_arm64x_metadata( (void *)(uintptr_t)image_base ))
        {
            void *native_module = NULL;

            if (macrunner_hb_label_allows_direct_native( label ) &&
                macrunner_hb_pc_is_native_pe_builtin( ctx->pc, &native_module ))
            {
                TRACE( "MacRunner HyperBridge dispatching native PE target inside native/CHPE image "
                       "%s pc=%p module=%p\n",
                       label ? label : "entry", (void *)(uintptr_t)ctx->pc, native_module );
                ret = macrunner_hb_call_direct_native_target( ctx, ctx->pc );
                if (ret != HB_OK)
                {
                    ERR( "MacRunner HyperBridge native PE target failed %s pc=%p result=%s\n",
                         label ? label : "entry", (void *)(uintptr_t)ctx->pc, hb_result_string(ret) );
                    status = STATUS_INVALID_IMAGE_FORMAT;
                    status_reason = "direct-native-image";
                    break;
                }
                continue;
            }
            ERR( "MacRunner HyperBridge refused native/CHPE image execution %s pc=%p image=%p-%p machine=%04x\n",
                 label ? label : "entry", (void *)(uintptr_t)ctx->pc,
                 (void *)(uintptr_t)image_start, (void *)(uintptr_t)(image_start + image_size),
                 macrunner_hb_module_machine( (void *)(uintptr_t)image_base ) );
            status = STATUS_INVALID_IMAGE_FORMAT;
            status_reason = "native-chpe-image";
            break;
        }
        if (macrunner_hb_try_ntdll_version_semantic( ctx, image_base ))
        {
            if (trace_thread_run)
            {
                fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_version_semantic "
                         "label=%s pc=%p\n", label, (void *)(uintptr_t)ctx->pc );
                fflush( stderr );
            }
            continue;
        }
        if (trace_thread_run)
        {
            fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_after_version_check "
                     "label=%s pc=%p\n", label, (void *)(uintptr_t)ctx->pc );
            fflush( stderr );
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
        if (trace_thread_run)
        {
            fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_before_cfg_fast_path "
                     "label=%s block=%s pc=%p\n",
                     label, wine_dbgstr_longlong(blocks), (void *)(uintptr_t)ctx->pc );
            fflush( stderr );
        }
        ret = macrunner_hb_try_x64_cfg_dispatch_fast_path( ctx, label, &steps );
        if (ret == HB_OK)
        {
            if (trace_thread_run)
            {
                fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_cfg_fast_path "
                         "label=%s next_pc=%p steps=%s\n",
                         label, (void *)(uintptr_t)ctx->pc, wine_dbgstr_longlong(steps) );
                fflush( stderr );
            }
            continue;
        }
        if (ret != HB_ERR_NOT_FOUND)
        {
            fprintf( stderr, "macrunner-hb-runtime-fail: label=%s block_pc=%p next_pc=%p "
                     "ret=%s out=%s reason=cfg-dispatch-fast-path blocks=%s steps=%s "
                     "rax=%p rsp=%p r10=%p\n",
                     label ? label : "entry", (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)ctx->pc, hb_result_string(ret),
                     hb_result_string(ctx->last_result), wine_dbgstr_longlong(blocks),
                     wine_dbgstr_longlong(steps), (void *)(uintptr_t)ctx->regs.x64.rax,
                     (void *)(uintptr_t)ctx->regs.x64.rsp, (void *)(uintptr_t)ctx->regs.x64.r10 );
            status = STATUS_INVALID_IMAGE_FORMAT;
            status_reason = "cfg-dispatch-fast-path";
            break;
        }
        if (trace_calc_object) macrunner_hb_trace_calc_probe( ctx, image_start, "before-block" );
        if (trace_npp_open_pack) macrunner_hb_trace_npp_open_pack( ctx, image_start, "before-block" );
        block_pc = ctx->pc;
        last_block_pc = block_pc;
        if (trace_thread_run)
        {
            fprintf( stderr, "macrunner-ui-input: stage=hb_run_x64_before_block "
                     "label=%s block=%s pc=%p rsp=%p\n",
                     label, wine_dbgstr_longlong(blocks), (void *)(uintptr_t)block_pc,
                     (void *)(uintptr_t)ctx->regs.x64.rsp );
            fflush( stderr );
        }
        if (heartbeat_enabled)
        {
            uint64_t now_us = macrunner_hb_now_us();

            if (blocks >= heartbeat_next_block ||
                (now_us && heartbeat_last_us && now_us - heartbeat_last_us >= 1000000ULL))
            {
                fprintf( stderr, "macrunner-hb-heartbeat: label=%s blocks=%s steps=%s "
                         "block_pc=%p rva=%p rsp=%p rax=%p rcx=%p rdx=%p rsi=%p rdi=%p\n",
                         label ? label : "entry", wine_dbgstr_longlong(blocks),
                         wine_dbgstr_longlong(steps), (void *)(uintptr_t)block_pc,
                         (void *)(uintptr_t)(block_pc - image_start),
                         (void *)(uintptr_t)ctx->regs.x64.rsp,
                         (void *)(uintptr_t)ctx->regs.x64.rax,
                         (void *)(uintptr_t)ctx->regs.x64.rcx,
                         (void *)(uintptr_t)ctx->regs.x64.rdx,
                         (void *)(uintptr_t)ctx->regs.x64.rsi,
                         (void *)(uintptr_t)ctx->regs.x64.rdi );
                fflush( stderr );
                heartbeat_last_us = now_us ? now_us : heartbeat_last_us;
                heartbeat_next_block = blocks + 1000;
            }
        }
        if (blocks <= 80)
            TRACE( "MacRunner HyperBridge block %s pc=%p rsp=%p rax=%p\n",
                   wine_dbgstr_longlong(blocks), (void *)(uintptr_t)ctx->pc,
                   (void *)(uintptr_t)ctx->regs.x64.rsp, (void *)(uintptr_t)ctx->regs.x64.rax );

        if (debug_enabled && blocks <= 200)
            ERR( "MacRunner HyperBridge before lift %s block=%s pc=%p rsp=%p\n",
                 label ? label : "x64", wine_dbgstr_longlong(blocks),
                 (void *)(uintptr_t)ctx->pc, (void *)(uintptr_t)ctx->regs.x64.rsp );
        func = macrunner_hb_ir_cache_find( ir_cache, block_pc );
        if (func)
            ret = HB_OK;
        else
        {
            ret = macrunner_hb_lift_one_block( ctx->pc, &func );
            if (ret == HB_OK && func && !macrunner_hb_ir_cache_put( ir_cache, block_pc, func ))
                transient_func = TRUE;
        }
        if (debug_enabled && blocks <= 200)
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
        if (debug_enabled && blocks <= 200)
            ERR( "MacRunner HyperBridge before run %s block=%s pc=%p instrs=%zu\n",
                 label ? label : "x64", wine_dbgstr_longlong(blocks),
                 (void *)(uintptr_t)ctx->pc,
                 func && func->cfg && func->cfg->entry ? func->cfg->entry->instr_count : 0 );
        if (jit_rt)
        {
            ret = hb_jit_runtime_run( jit_rt, func, &out );
            if (ret == HB_OK && (out.result == HB_ERR_UNSUPPORTED_OPCODE ||
                                 out.result == HB_ERR_INTERNAL))
            {
                if (++jit_fallbacks <= 20)
                    fprintf( stderr, "macrunner-hb-jit-fallback: label=%s pc=%p out=%s reason=%s\n",
                             label ? label : "entry", (void *)(uintptr_t)block_pc,
                             hb_result_string(out.result),
                             out.fault_reason ? out.fault_reason : "none" );
                ctx->last_result = HB_OK;
                memset( &out, 0, sizeof(out) );
                ret = hb_runtime_run( ctx, func, HB_BACKEND_INTERP, &out );
            }
        }
        else
            ret = hb_runtime_run( ctx, func, HB_BACKEND_INTERP, &out );
        if (debug_enabled && blocks <= 200)
            ERR( "MacRunner HyperBridge after run %s block=%s ret=%s out=%s steps=%s pc=%p\n",
                 label ? label : "x64", wine_dbgstr_longlong(blocks),
                 hb_result_string(ret), hb_result_string(out.result),
                 wine_dbgstr_longlong(out.steps_executed), (void *)(uintptr_t)ctx->pc );
        if (transient_func)
        {
            if (jit_rt)
            {
                struct macrunner_hb_owned_ir_func *owned = malloc( sizeof(*owned) );
                if (owned)
                {
                    owned->func = func;
                    owned->next = jit_owned_ir;
                    jit_owned_ir = owned;
                }
            }
            else hb_ir_func_destroy( func );
        }
        steps += out.steps_executed;
        if (progress_interval && blocks && !(blocks % progress_interval))
            fprintf( stderr, "macrunner-hb-progress: label=%s block=%s block_pc=%p "
                     "next_pc=%p rva=%p ret=%s out=%s out_steps=%s total_steps=%s "
                     "rax=%p rcx=%p rdx=%p rsi=%p rdi=%p rsp=%p\n",
                     label ? label : "entry", wine_dbgstr_longlong(blocks),
                     (void *)(uintptr_t)block_pc, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)(block_pc - image_start),
                     hb_result_string(ret), hb_result_string(out.result),
                     wine_dbgstr_longlong(out.steps_executed),
                     wine_dbgstr_longlong(steps),
                     (void *)(uintptr_t)ctx->regs.x64.rax,
                     (void *)(uintptr_t)ctx->regs.x64.rcx,
                     (void *)(uintptr_t)ctx->regs.x64.rdx,
                     (void *)(uintptr_t)ctx->regs.x64.rsi,
                     (void *)(uintptr_t)ctx->regs.x64.rdi,
                     (void *)(uintptr_t)ctx->regs.x64.rsp );
        if (trace_low_stack && ctx->memory &&
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
            LDR_DATA_TABLE_ENTRY *fault_ldr = macrunner_hb_ldr_entry_from_pc( (void *)(uintptr_t)block_pc );
            char fault_base[128], fault_full[256], fault_bytes[64];
            uintptr_t fault_module = fault_ldr ? (uintptr_t)fault_ldr->DllBase : image_start;
            size_t fault_rva = fault_module ? (size_t)(block_pc - fault_module) : 0;

            fault_base[0] = fault_full[0] = fault_bytes[0] = 0;
            if (fault_ldr)
            {
                unsigned int i;
                char *ptr = fault_bytes;

                macrunner_hb_copy_unicode_ascii( fault_base, sizeof(fault_base), &fault_ldr->BaseDllName );
                macrunner_hb_copy_unicode_ascii( fault_full, sizeof(fault_full), &fault_ldr->FullDllName );
                if (block_pc >= fault_module && block_pc + 16 <= fault_module + fault_ldr->SizeOfImage)
                {
                    const BYTE *bytes = (const BYTE *)(uintptr_t)block_pc;

                    for (i = 0; i < 16; i++)
                        ptr += snprintf( ptr, sizeof(fault_bytes) - (ptr - fault_bytes),
                                         "%s%02x", i ? " " : "", bytes[i] );
                }
            }
            fprintf( stderr, "macrunner-hb-runtime-fail: label=%s block_pc=%p next_pc=%p "
                     "module=%s rva=%p bytes=%s ret=%s out=%s reason=%s blocks=%s steps=%s out_steps=%s "
                     "rax=%p rcx=%p rdx=%p rsi=%p rdi=%p rsp=%p "
                     "r8=%p r9=%p r10=%p r11=%p full=%s\n",
                     label ? label : "entry", (void *)(uintptr_t)block_pc,
                     (void *)(uintptr_t)ctx->pc,
                     fault_base[0] ? fault_base : "(unknown)", (void *)fault_rva,
                     fault_bytes[0] ? fault_bytes : "(none)", hb_result_string(ret),
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
                      (void *)(uintptr_t)ctx->regs.x64.r11,
                      fault_full[0] ? fault_full : "(unknown)" );
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
            if (trace_calc_object) macrunner_hb_trace_calc_probe( ctx, image_start, "run-fault" );
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
    if (jit_rt) hb_jit_runtime_destroy( jit_rt );
    while (jit_owned_ir)
    {
        struct macrunner_hb_owned_ir_func *next = jit_owned_ir->next;
        hb_ir_func_destroy( jit_owned_ir->func );
        free( jit_owned_ir );
        jit_owned_ir = next;
    }
    macrunner_hb_ir_cache_destroy( ir_cache );
    macrunner_hb_bridge_stack_limit = old_bridge_stack_limit;
    macrunner_hb_bridge_stack_base = old_bridge_stack_base;
    macrunner_hb_bridge_stack_size = old_bridge_stack_size;
    macrunner_hb_original_stack_limit = old_original_stack_limit;
    macrunner_hb_original_stack_base = old_original_stack_base;
    if (teb)
    {
        teb->Tib.StackLimit = old_teb_stack_limit;
        teb->Tib.StackBase = old_teb_stack_base;
        teb->DeallocationStack = old_teb_deallocation_stack;
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
    ULONG64 ret_value = 0;
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
    if (macrunner_hb_trace_thread_lifecycle_enabled())
    {
        fprintf( stderr, "macrunner-ui-input: stage=hb_x64_thread_entry_begin entry=%p arg=%p image=%p\n",
                 params->entry, params->arg, NtCurrentTeb()->Peb->ImageBaseAddress );
        fflush( stderr );
    }

    status = macrunner_hb_run_x64( params->entry, &call, &params->ret,
                                   &params->blocks, &params->steps, "thread",
                                   NtCurrentTeb()->Peb->ImageBaseAddress );
    if (macrunner_hb_trace_thread_lifecycle_enabled())
    {
        fprintf( stderr, "macrunner-ui-input: stage=hb_x64_thread_entry_return entry=%p arg=%p status=%lx ret=%s blocks=%s steps=%s\n",
                 params->entry, params->arg, (unsigned long)status, wine_dbgstr_longlong(params->ret),
                 wine_dbgstr_longlong(params->blocks), wine_dbgstr_longlong(params->steps) );
        fflush( stderr );
    }
    TRACE( "MacRunner HyperBridge x64 thread returned status=%lx ret=%s blocks=%s steps=%s\n",
           (unsigned long)status, wine_dbgstr_longlong(params->ret), wine_dbgstr_longlong(params->blocks),
           wine_dbgstr_longlong(params->steps) );
    return status;
}
