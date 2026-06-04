/*
 * WoW64 virtual memory functions
 *
 * Copyright 2021 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#ifdef __APPLE__
# include <mach/mach_init.h>
# include <mach/mach_vm.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winnt.h"
#include "winternl.h"
#include "winioctl.h"
#include "wow64_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wow);

NTSTATUS write_guest32_output( void *dst, const void *src, SIZE_T size )
{
#ifdef __APPLE__
    kern_return_t kr;

    if (!dst || !src) return STATUS_INVALID_PARAMETER;
    if (!size) return STATUS_SUCCESS;

    /* MacRunner guest32 memory lives in a host mirror window.  Some internal
     * Wine output buffers, notably the MemoryWineUnixFuncs slot inside a PE32
     * builtin module, can be writable guest memory while the host VM page is
     * currently protected read/execute.  mach_vm_write preserves the guest
     * semantics without surfacing a native SIGBUS from the ARM64 writer. */
    kr = mach_vm_write( mach_task_self(), (mach_vm_address_t)(UINT_PTR)dst,
                        (vm_offset_t)(UINT_PTR)src, (mach_msg_type_number_t)size );
    if (kr == KERN_SUCCESS) return STATUS_SUCCESS;
    return STATUS_ACCESS_VIOLATION;
#else
    BYTE *out = dst;
    const BYTE *in = src;

    if (!dst || !src) return STATUS_INVALID_PARAMETER;
    while (size)
    {
        MEMORY_BASIC_INFORMATION mbi;
        ULONG_PTR region_end, cur = (ULONG_PTR)out;
        SIZE_T chunk;
        ULONG protect, old_protect = 0;
        NTSTATUS status;
        BOOL restore = FALSE;

        MESSAGE( "macrunner-wow64: guest32-write query out=%p remaining=%lu\n", out, size );
        status = NtQueryVirtualMemory( GetCurrentProcess(), out, MemoryBasicInformation,
                                       &mbi, sizeof(mbi), NULL );
        MESSAGE( "macrunner-wow64: guest32-write query status=%08lx base=%p size=%lu state=%08lx protect=%08lx\n",
                 status, mbi.BaseAddress, mbi.RegionSize, mbi.State, mbi.Protect );
        if (status) return status;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return STATUS_ACCESS_VIOLATION;

        region_end = (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
        if (region_end <= cur) return STATUS_ACCESS_VIOLATION;
        chunk = region_end - cur;
        if (chunk > size) chunk = size;

        protect = mbi.Protect & 0xff;
        if (protect != PAGE_READWRITE && protect != PAGE_EXECUTE_READWRITE)
        {
            void *base = (void *)(cur & ~(ULONG_PTR)0xfff);
            SIZE_T protect_size = ((cur + chunk + 0xfff) & ~(ULONG_PTR)0xfff) - (ULONG_PTR)base;

            MESSAGE( "macrunner-wow64: guest32-write protect base=%p size=%lu old=%08lx\n",
                     base, protect_size, mbi.Protect );
            status = NtProtectVirtualMemory( GetCurrentProcess(), &base, &protect_size,
                                             PAGE_READWRITE, &old_protect );
            MESSAGE( "macrunner-wow64: guest32-write protect status=%08lx old=%08lx\n",
                     status, old_protect );
            if (status) return status;
            restore = TRUE;
        }

        MESSAGE( "macrunner-wow64: guest32-write memcpy out=%p chunk=%lu restore=%u\n",
                 out, chunk, restore );
        memcpy( out, in, chunk );
        MESSAGE( "macrunner-wow64: guest32-write memcpy-done out=%p chunk=%lu\n", out, chunk );

        if (restore)
        {
            void *base = (void *)(cur & ~(ULONG_PTR)0xfff);
            SIZE_T protect_size = ((cur + chunk + 0xfff) & ~(ULONG_PTR)0xfff) - (ULONG_PTR)base;
            ULONG ignored;

            MESSAGE( "macrunner-wow64: guest32-write restore base=%p size=%lu old=%08lx\n",
                     base, protect_size, old_protect );
            NtProtectVirtualMemory( GetCurrentProcess(), &base, &protect_size,
                                    old_protect, &ignored );
            MESSAGE( "macrunner-wow64: guest32-write restore-done ignored=%08lx\n", ignored );
        }

        out += chunk;
        in += chunk;
        size -= chunk;
    }
    return STATUS_SUCCESS;
#endif
}

static BOOL WINAPIV send_cross_process_notification( HANDLE process, UINT id, const void *addr, SIZE_T size,
                                                     int nb_args, ... )
{
    CROSS_PROCESS_WORK_LIST *list;
    CROSS_PROCESS_WORK_ENTRY *entry;
    void *unused;
    HANDLE section;
    va_list args;
    int i;

    RtlOpenCrossProcessEmulatorWorkConnection( process, &section, (void **)&list );
    if (!list) return FALSE;
    if ((entry = RtlWow64PopCrossProcessWorkFromFreeList( &list->free_list )))
    {
        entry->id = id;
        entry->addr = (ULONG_PTR)addr;
        entry->size = size;
        if (nb_args)
        {
            va_start( args, nb_args );
            for (i = 0; i < nb_args; i++) entry->args[i] = va_arg( args, int );
            va_end( args );
        }
        RtlWow64PushCrossProcessWorkOntoWorkList( &list->work_list, entry, &unused );
    }
    NtUnmapViewOfSection( GetCurrentProcess(), list );
    NtClose( section );
    return TRUE;
}


static MEMORY_RANGE_ENTRY *memory_range_entry_array_32to64( const MEMORY_RANGE_ENTRY32 *addresses32,
                                                            ULONG count )
{
    MEMORY_RANGE_ENTRY *addresses = Wow64AllocateTemp( sizeof(MEMORY_RANGE_ENTRY) * count );
    ULONG i;

    for (i = 0; i < count; i++)
    {
        addresses[i].VirtualAddress = ULongToPtr( addresses32[i].VirtualAddress );
        addresses[i].NumberOfBytes = addresses32[i].NumberOfBytes;
    }

    return addresses;
}

static NTSTATUS mem_extended_parameters_32to64( MEM_EXTENDED_PARAMETER **ret_params,
                                                const MEM_EXTENDED_PARAMETER32 *params32, ULONG *count,
                                                BOOL set_limit )
{
    ULONG i;
    MEM_EXTENDED_PARAMETER *params;
    MEM_ADDRESS_REQUIREMENTS *req;
    MEM_ADDRESS_REQUIREMENTS32 *req32 = NULL;

    if (*count && !params32) return STATUS_INVALID_PARAMETER;

    params = Wow64AllocateTemp( (*count + 1) * sizeof(*params) + sizeof(*req) );
    req = (MEM_ADDRESS_REQUIREMENTS *)(params + *count + 1);

    for (i = 0; i < *count; i++)
    {
        params[i].Type = params32[i].Type;
        params[i].Reserved = 0;
        switch (params[i].Type)
        {
        case MemExtendedParameterAddressRequirements:
            req32 = ULongToPtr( params32[i].Pointer );
            params[i].Pointer = req;
            break;
        case MemExtendedParameterAttributeFlags:
        case MemExtendedParameterNumaNode:
        case MemExtendedParameterImageMachine:
            params[i].ULong = params32[i].ULong;
            break;
        case MemExtendedParameterPartitionHandle:
        case MemExtendedParameterUserPhysicalHandle:
            params[i].Handle = ULongToHandle( params32[i].Handle );
            break;
        }
    }

    if (req32)
    {
        if (req32->HighestEndingAddress > highest_user_address) return STATUS_INVALID_PARAMETER;
        req->LowestStartingAddress = ULongToPtr( req32->LowestStartingAddress );
        req->HighestEndingAddress  = ULongToPtr( req32->HighestEndingAddress );
        req->Alignment             = req32->Alignment;
    }
    else if (set_limit)
    {
        req->LowestStartingAddress = NULL;
        req->HighestEndingAddress  = (void *)highest_user_address;
        req->Alignment             = 0;

        params[i].Type = MemExtendedParameterAddressRequirements;
        params[i].Reserved = 0;
        params[i].Pointer = req;
        *count = i + 1;
    }
    *ret_params = params;
    return STATUS_SUCCESS;
}

/**********************************************************************
 *           wow64_NtAllocateVirtualMemory
 */
NTSTATUS WINAPI wow64_NtAllocateVirtualMemory( UINT *args )
{
    HANDLE process;
    ULONG *addr32;
    ULONG_PTR zero_bits;
    ULONG *size32;
    ULONG type;
    ULONG protect;
    BOOL is_current;
    void *addr;
    SIZE_T size;
    NTSTATUS status;
    static int trace_count;

    process = get_handle( &args );
    addr32 = get_ptr( &args );
    zero_bits = get_ulong( &args );
    size32 = get_ptr( &args );
    type = get_ulong( &args );
    protect = get_ulong( &args );
    is_current = RtlIsCurrentProcess( process );
    if (!addr32 || !size32) return STATUS_ACCESS_VIOLATION;
    addr = is_current ? guest32_host_ptr( *addr32 ) : ULongToPtr( *addr32 );
    size = *size32;

    if (!addr && (type & MEM_COMMIT)) type |= MEM_RESERVE;

    if (trace_count < 64)
    {
        MESSAGE( "macrunner-wow64: NtAllocateVirtualMemory pre process=%p addr32=%p val=%08lx zero=%08Ix size32=%p size=%Ix type=%08lx protect=%08lx current=%u\n",
                 process, addr32, addr32 ? *addr32 : 0, zero_bits,
                 size32, size, type, protect, is_current );
    }

    if (!is_current) send_cross_process_notification( process, CrossProcessPreVirtualAlloc,
                                                      addr, size, 3, type, protect, 0 );
    else if (pBTCpuNotifyMemoryAlloc) pBTCpuNotifyMemoryAlloc( addr, size, type, protect, FALSE, 0 );

    status = NtAllocateVirtualMemory( process, &addr, get_zero_bits( zero_bits ), &size, type, protect );

    if (trace_count < 64)
    {
        MESSAGE( "macrunner-wow64: NtAllocateVirtualMemory post status=%08lx addr=%p guest=%08lx size=%Ix\n",
                 status, addr, PtrToUlong( addr ), size );
        trace_count++;
    }

    if (!is_current) send_cross_process_notification( process, CrossProcessPostVirtualAlloc,
                                                      addr, size, 3, type, protect, status );
    else if (pBTCpuNotifyMemoryAlloc) pBTCpuNotifyMemoryAlloc( addr, size, type, protect, TRUE, status );

    if (!status)
    {
        put_addr( addr32, addr );
        put_size( size32, size );
    }
    return status;
}


/**********************************************************************
 *           wow64_NtAllocateVirtualMemoryEx
 */
NTSTATUS WINAPI wow64_NtAllocateVirtualMemoryEx( UINT *args )
{
    HANDLE process = get_handle( &args );
    ULONG *addr32 = get_ptr( &args );
    ULONG *size32 = get_ptr( &args );
    ULONG type = get_ulong( &args );
    ULONG protect = get_ulong( &args );
    MEM_EXTENDED_PARAMETER32 *params32 = get_ptr( &args );
    ULONG count = get_ulong( &args );

    NTSTATUS status;
    MEM_EXTENDED_PARAMETER *params64;
    BOOL is_current = RtlIsCurrentProcess( process );
    void *addr;
    SIZE_T size;
    BOOL set_limit;

    if (!addr32 || !size32) return STATUS_ACCESS_VIOLATION;
    addr = is_current ? guest32_host_ptr( *addr32 ) : ULongToPtr( *addr32 );
    size = *size32;
    set_limit = (!*addr32 && is_current);

    if (!addr) type |= MEM_RESERVE;

    if ((status = mem_extended_parameters_32to64( &params64, params32, &count, set_limit ))) return status;

    if (!is_current) send_cross_process_notification( process, CrossProcessPreVirtualAlloc,
                                                      addr, size, 3, type, protect, 0 );
    else if (pBTCpuNotifyMemoryAlloc) pBTCpuNotifyMemoryAlloc( addr, size, type, protect, FALSE, 0 );

    status = NtAllocateVirtualMemoryEx( process, &addr, &size, type, protect, params64, count );

    if (!is_current) send_cross_process_notification( process, CrossProcessPostVirtualAlloc,
                                                      addr, size, 3, type, protect, status );
    else if (pBTCpuNotifyMemoryAlloc) pBTCpuNotifyMemoryAlloc( addr, size, type, protect, TRUE, status );

    if (!status)
    {
        put_addr( addr32, addr );
        put_size( size32, size );
    }
    return status;
}


/**********************************************************************
 *           wow64_NtAreMappedFilesTheSame
 */
NTSTATUS WINAPI wow64_NtAreMappedFilesTheSame( UINT *args )
{
    void *ptr1 = get_ptr( &args );
    void *ptr2 = get_ptr( &args );

    return NtAreMappedFilesTheSame( ptr1, ptr2 );
}


/**********************************************************************
 *           wow64_NtCreateSectionEx
 */
NTSTATUS WINAPI wow64_NtCreateSectionEx( UINT *args )
{
    ULONG *handle_ptr = get_ptr( &args );
    ACCESS_MASK access = get_ulong( &args );
    OBJECT_ATTRIBUTES32 *attr32 = get_ptr( &args );
    const LARGE_INTEGER *size = get_ptr( &args );
    ULONG protect = get_ulong( &args );
    ULONG flags = get_ulong( &args );
    HANDLE file = get_handle( &args );
    MEM_EXTENDED_PARAMETER32 *params32 = get_ptr( &args );
    ULONG count = get_ulong( &args );

    MEM_EXTENDED_PARAMETER *params64;
    struct object_attr64 attr;
    HANDLE handle = 0;
    NTSTATUS status;

    if ((status = mem_extended_parameters_32to64( &params64, params32, &count, FALSE ))) return status;

    *handle_ptr = 0;
    status = NtCreateSectionEx( &handle, access, objattr_32to64( &attr, attr32 ),
                                size, protect, flags, file, params64, count );
    put_handle( handle_ptr, handle );
    return status;
}


/**********************************************************************
 *           wow64_NtFlushInstructionCache
 */
NTSTATUS WINAPI wow64_NtFlushInstructionCache( UINT *args )
{
    HANDLE process = get_handle( &args );
    const void *addr = get_ptr( &args );
    SIZE_T size = get_ulong( &args );

    if (RtlIsCurrentProcess( process ))
    {
        if (pBTCpuFlushInstructionCache2) pBTCpuFlushInstructionCache2( addr, size );
    }
    else send_cross_process_notification( process, CrossProcessFlushCache, addr, size, 0 );

    return NtFlushInstructionCache( process, addr, size );
}


/**********************************************************************
 *           wow64_NtFlushVirtualMemory
 */
NTSTATUS WINAPI wow64_NtFlushVirtualMemory( UINT *args )
{
    HANDLE process = get_handle( &args );
    ULONG *addr32 = get_ptr( &args );
    ULONG *size32 = get_ptr( &args );
    ULONG unknown = get_ulong( &args );

    void *addr;
    SIZE_T size;
    NTSTATUS status;

    if (!addr32 || !size32) return STATUS_ACCESS_VIOLATION;

    status = NtFlushVirtualMemory( process, (const void **)addr_32to64( &addr, addr32 ),
                                   size_32to64( &size, size32 ), unknown );
    if (!status)
    {
        put_addr( addr32, addr );
        put_size( size32, size );
    }
    return status;
}


/**********************************************************************
 *           wow64_NtFreeVirtualMemory
 */
NTSTATUS WINAPI wow64_NtFreeVirtualMemory( UINT *args )
{
    HANDLE process = get_handle( &args );
    ULONG *addr32 = get_ptr( &args );
    ULONG *size32 = get_ptr( &args );
    ULONG type = get_ulong( &args );

    BOOL is_current = RtlIsCurrentProcess( process );
    void *addr;
    SIZE_T size;
    NTSTATUS status;

    if (!addr32 || !size32) return STATUS_ACCESS_VIOLATION;
    addr = is_current ? guest32_host_ptr( *addr32 ) : ULongToPtr( *addr32 );
    size = *size32;

    if (!is_current) send_cross_process_notification( process, CrossProcessPreVirtualFree,
                                                      addr, size, 2, type, 0 );
    else if (pBTCpuNotifyMemoryFree) pBTCpuNotifyMemoryFree( addr, size, type, FALSE, 0 );

    status = NtFreeVirtualMemory( process, &addr, &size, type );

    if (!is_current) send_cross_process_notification( process, CrossProcessPostVirtualFree,
                                                      addr, size, 2, type, status );
    else if (pBTCpuNotifyMemoryFree) pBTCpuNotifyMemoryFree( addr, size, type, TRUE, status );

    if (!status)
    {
        put_addr( addr32, addr );
        put_size( size32, size );
    }
    return status;
}


/**********************************************************************
 *           wow64_NtGetNlsSectionPtr
 */
NTSTATUS WINAPI wow64_NtGetNlsSectionPtr( UINT *args )
{
    ULONG type = get_ulong( &args );
    ULONG id = get_ulong( &args );
    void *unknown = get_ptr( &args );
    ULONG *addr32 = get_ptr( &args );
    ULONG *size32 = get_ptr( &args );

    void *addr;
    SIZE_T size;
    NTSTATUS status;

    status = NtGetNlsSectionPtr( type, id, unknown, addr_32to64( &addr, addr32 ),
                                 size_32to64( &size, size32 ));
    if (!status)
    {
        put_addr( addr32, addr );
        put_size( size32, size );
    }
    return status;
}


/**********************************************************************
 *           wow64_NtGetWriteWatch
 */
NTSTATUS WINAPI wow64_NtGetWriteWatch( UINT *args )
{
    HANDLE handle = get_handle( &args );
    ULONG flags = get_ulong( &args );
    void *base = get_ptr( &args );
    SIZE_T size = get_ulong( &args );
    ULONG *addr_ptr = get_ptr( &args );
    ULONG *count_ptr = get_ptr( &args );
    ULONG *granularity = get_ptr( &args );

    ULONG_PTR i, count;
    void **addresses;
    NTSTATUS status;

    if (!count_ptr || !granularity) return STATUS_ACCESS_VIOLATION;
    count = *count_ptr;
    if (!count || !size) return STATUS_INVALID_PARAMETER;
    if (flags & ~WRITE_WATCH_FLAG_RESET) return STATUS_INVALID_PARAMETER;
    if (!addr_ptr) return STATUS_ACCESS_VIOLATION;

    addresses = Wow64AllocateTemp( count * sizeof(*addresses) );
    if (!(status = NtGetWriteWatch( handle, flags, base, size, addresses, &count, granularity )))
    {
        for (i = 0; i < count; i++) addr_ptr[i] = PtrToUlong( addresses[i] );
        *count_ptr = count;
    }
    return status;
}


/**********************************************************************
 *           wow64_NtInitializeNlsFiles
 */
NTSTATUS WINAPI wow64_NtInitializeNlsFiles( UINT *args )
{
    ULONG *addr32 = get_ptr( &args );
    LCID *lcid = get_ptr( &args );
    LARGE_INTEGER *size = get_ptr( &args );

    void *addr;
    NTSTATUS status;

    status = NtInitializeNlsFiles( addr_32to64( &addr, addr32 ), lcid, size );
    if (!status) put_addr( addr32, addr );
    return status;
}


/**********************************************************************
 *           wow64_NtLockVirtualMemory
 */
NTSTATUS WINAPI wow64_NtLockVirtualMemory( UINT *args )
{
    HANDLE process = get_handle( &args );
    ULONG *addr32 = get_ptr( &args );
    ULONG *size32 = get_ptr( &args );
    ULONG unknown = get_ulong( &args );

    void *addr;
    SIZE_T size;
    NTSTATUS status;

    if (!addr32 || !size32) return STATUS_ACCESS_VIOLATION;

    status = NtLockVirtualMemory( process, addr_32to64( &addr, addr32 ),
                                  size_32to64( &size, size32 ), unknown );
    if (!status)
    {
        put_addr( addr32, addr );
        put_size( size32, size );
    }
    return status;
}


static void notify_map_view_of_section( HANDLE handle, void *addr, SIZE_T size, ULONG alloc,
                                        ULONG protect, NTSTATUS *ret_status )
{
    SECTION_IMAGE_INFORMATION info;
    NTSTATUS status;

    if (!NtCurrentTeb()->Tib.ArbitraryUserPointer) return;
    if (NtQuerySection( handle, SectionImageInformation, &info, sizeof(info), NULL )) return;
    if (info.Machine != current_machine) return;
    init_image_mapping( addr );
    if (!pBTCpuNotifyMapViewOfSection) return;
    status = pBTCpuNotifyMapViewOfSection( NULL, addr, NULL, size, alloc, protect );
    if (NT_SUCCESS(status)) return;
    NtUnmapViewOfSection( GetCurrentProcess(), addr );
    *ret_status = status;
}

static BOOL is_pe32_support_image_map_warning( HANDLE handle, HANDLE process, NTSTATUS status )
{
    SECTION_IMAGE_INFORMATION info;

    if (status != STATUS_IMAGE_MACHINE_TYPE_MISMATCH && status != STATUS_IMAGE_NOT_AT_BASE) return FALSE;
    if (!RtlIsCurrentProcess( process )) return FALSE;
    if (current_machine != IMAGE_FILE_MACHINE_I386) return FALSE;
    if (NtQuerySection( handle, SectionImageInformation, &info, sizeof(info), NULL )) return FALSE;

    return info.Machine == IMAGE_FILE_MACHINE_I386 || info.Machine == IMAGE_FILE_MACHINE_AMD64;
}

/**********************************************************************
 *           wow64_NtMapViewOfSection
 */
NTSTATUS WINAPI wow64_NtMapViewOfSection( UINT *args )
{
    UINT *raw_args = args;
    HANDLE handle = get_handle( &args );
    HANDLE process = get_handle( &args );
    ULONG *addr32 = get_ptr( &args );
    ULONG_PTR zero_bits = get_ulong( &args );
    SIZE_T commit = get_ulong( &args );
    const LARGE_INTEGER *offset = get_ptr( &args );
    ULONG *size32 = get_ptr( &args );
    SECTION_INHERIT inherit = get_ulong( &args );
    ULONG alloc = get_ulong( &args );
    ULONG protect = get_ulong( &args );

    void *addr;
    SIZE_T size;
    NTSTATUS status;
    void *prev = NtCurrentTeb()->Tib.ArbitraryUserPointer;
    ULONG addr_before;
    ULONG size_before;
    LONGLONG offset_value = offset ? offset->QuadPart : 0;

    if (!addr32 || !size32) return STATUS_ACCESS_VIOLATION;
    addr_before = *addr32;
    size_before = *size32;

    MESSAGE( "macrunner-wow64: NtMapViewOfSection raw_args=%p raw=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
             raw_args, raw_args[0], raw_args[1], raw_args[2], raw_args[3], raw_args[4],
             raw_args[5], raw_args[6], raw_args[7], raw_args[8], raw_args[9] );
    MESSAGE( "macrunner-wow64: NtMapViewOfSection enter handle=%p process=%p addr32=%p addr=%08lx zero=%Ix commit=%Ix offset_ptr=%p offset=%016llx size32=%p size=%08lx inherit=%u alloc=%08lx protect=%08lx\n",
             handle, process, addr32, addr_before, zero_bits, commit, offset,
             (unsigned long long)offset_value, size32, size_before, inherit, alloc, protect );

    NtCurrentTeb()->Tib.ArbitraryUserPointer = ULongToPtr( NtCurrentTeb32()->Tib.ArbitraryUserPointer );
    status = WINE_NT_MAP_VIEW( handle, process, addr_32to64( &addr, addr32 ), get_zero_bits( zero_bits ),
                               commit, offset, size_32to64( &size, size32 ), inherit, alloc, protect );
    MESSAGE( "macrunner-wow64: NtMapViewOfSection native status=%08lx addr=%p size=%Ix\n",
             status, addr, size );
    if (NT_SUCCESS(status))
    {
        put_addr( addr32, addr );
        put_size( size32, size );
        if (RtlIsCurrentProcess( process ))
            notify_map_view_of_section( handle, addr, size, alloc, protect, &status );
    }
    if (is_pe32_support_image_map_warning( handle, process, status )) status = STATUS_SUCCESS;
    NtCurrentTeb()->Tib.ArbitraryUserPointer = prev;
    MESSAGE( "macrunner-wow64: NtMapViewOfSection leave status=%08lx out_addr=%08lx out_size=%08lx\n",
             status, addr32 ? *addr32 : 0, size32 ? *size32 : 0 );
    return status;
}

/**********************************************************************
 *           wow64_NtMapViewOfSectionEx
 */
NTSTATUS WINAPI wow64_NtMapViewOfSectionEx( UINT *args )
{
    HANDLE handle = get_handle( &args );
    HANDLE process = get_handle( &args );
    ULONG *addr32 = get_ptr( &args );
    const LARGE_INTEGER *offset = get_ptr( &args );
    ULONG *size32 = get_ptr( &args );
    ULONG alloc = get_ulong( &args );
    ULONG protect = get_ulong( &args );
    MEM_EXTENDED_PARAMETER32 *params32 = get_ptr( &args );
    ULONG count = get_ulong( &args );

    void *addr;
    SIZE_T size;
    NTSTATUS status;
    MEM_EXTENDED_PARAMETER *params64;
    BOOL is_current = RtlIsCurrentProcess( process );
    BOOL set_limit;
    void *prev = NtCurrentTeb()->Tib.ArbitraryUserPointer;

    if (!addr32 || !size32) return STATUS_ACCESS_VIOLATION;
    set_limit = (!*addr32 && is_current);

    if ((status = mem_extended_parameters_32to64( &params64, params32, &count, set_limit ))) return status;

    NtCurrentTeb()->Tib.ArbitraryUserPointer = ULongToPtr( NtCurrentTeb32()->Tib.ArbitraryUserPointer );
    status = NtMapViewOfSectionEx( handle, process, addr_32to64( &addr, addr32 ), offset,
                                   size_32to64( &size, size32 ), alloc, protect, params64, count );
    if (NT_SUCCESS(status))
    {
        put_addr( addr32, addr );
        put_size( size32, size );
        if (is_current) notify_map_view_of_section( handle, addr, size, alloc, protect, &status );
    }
    if (is_pe32_support_image_map_warning( handle, process, status )) status = STATUS_SUCCESS;
    NtCurrentTeb()->Tib.ArbitraryUserPointer = prev;
    return status;
}

/**********************************************************************
 *           wow64_NtProtectVirtualMemory
 */
NTSTATUS WINAPI wow64_NtProtectVirtualMemory( UINT *args )
{
    HANDLE process = get_handle( &args );
    ULONG *addr32 = get_ptr( &args );
    ULONG *size32 = get_ptr( &args );
    ULONG new_prot = get_ulong( &args );
    ULONG *old_prot = get_ptr( &args );

    BOOL is_current = RtlIsCurrentProcess( process );
    void *addr;
    SIZE_T size;
    NTSTATUS status;

    if (!addr32 || !size32 || !old_prot) return STATUS_ACCESS_VIOLATION;
    addr = is_current ? guest32_host_ptr( *addr32 ) : ULongToPtr( *addr32 );
    size = *size32;

    if (!is_current) send_cross_process_notification( process, CrossProcessPreVirtualProtect,
                                                      addr, size, 2, new_prot, 0 );
    else if (pBTCpuNotifyMemoryProtect) pBTCpuNotifyMemoryProtect( addr, size, new_prot, FALSE, 0 );

    status = NtProtectVirtualMemory( process, &addr, &size, new_prot, old_prot );

    if (!is_current) send_cross_process_notification( process, CrossProcessPostVirtualProtect,
                                                      addr, size, 2, new_prot, status );
    else if (pBTCpuNotifyMemoryProtect) pBTCpuNotifyMemoryProtect( addr, size, new_prot, TRUE, status );

    if (!status)
    {
        put_addr( addr32, addr );
        put_size( size32, size );
    }
    return status;
}


/**********************************************************************
 *           wow64_NtQueryVirtualMemory
 */
NTSTATUS WINAPI wow64_NtQueryVirtualMemory( UINT *args )
{
    HANDLE handle = get_handle( &args );
    ULONG addr32 = get_ulong( &args );
    MEMORY_INFORMATION_CLASS class = get_ulong( &args );
    void *ptr = get_ptr( &args );
    ULONG len = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    BOOL is_current = RtlIsCurrentProcess( handle );
    void *query_addr = is_current ? guest32_host_ptr( addr32 ) : ULongToPtr( addr32 );
    SIZE_T res_len = 0;
    NTSTATUS status;

    switch (class)
    {
    case MemoryBasicInformation:  /* MEMORY_BASIC_INFORMATION */
        if (len < sizeof(MEMORY_BASIC_INFORMATION32))
            status = STATUS_INFO_LENGTH_MISMATCH;
        else if (!ptr)
            status = STATUS_ACCESS_VIOLATION;
        else if (addr32 > highest_user_address)
            status = STATUS_INVALID_PARAMETER;
        else
        {
            MEMORY_BASIC_INFORMATION info;
            MEMORY_BASIC_INFORMATION32 *info32 = ptr;

            if (!(status = NtQueryVirtualMemory( handle, query_addr, class, &info, sizeof(info), &res_len )))
            {
                ULONG base32 = PtrToUlong( info.BaseAddress );

                info32->BaseAddress = base32;
                info32->AllocationBase = PtrToUlong( info.AllocationBase );
                info32->AllocationProtect = info.AllocationProtect;
                info32->RegionSize = info.RegionSize;
                info32->State = info.State;
                info32->Protect = info.Protect;
                info32->Type = info.Type;
                if (base32 <= highest_user_address && info.RegionSize > highest_user_address - base32 + 1)
                    info32->RegionSize = highest_user_address - base32 + 1;
            }
        }
        res_len = sizeof(MEMORY_BASIC_INFORMATION32);
        break;

    case MemoryMappedFilenameInformation:  /* MEMORY_SECTION_NAME */
    {
        MEMORY_SECTION_NAME *info;
        MEMORY_SECTION_NAME32 *info32 = ptr;
        SIZE_T size = len + sizeof(*info) - sizeof(*info32);

        if (!ptr) status = STATUS_ACCESS_VIOLATION;
        else
        {
            info = Wow64AllocateTemp( size );
            if (!(status = NtQueryVirtualMemory( handle, query_addr, class, info, size, &res_len )))
            {
                info32->SectionFileName.Length = info->SectionFileName.Length;
                info32->SectionFileName.MaximumLength = info->SectionFileName.MaximumLength;
                info32->SectionFileName.Buffer = PtrToUlong( info32 + 1 );
                memcpy( info32 + 1, info->SectionFileName.Buffer, info->SectionFileName.MaximumLength );
            }
            res_len += sizeof(*info32) - sizeof(*info);
        }
        break;
    }

    case MemoryRegionInformation: /* MEMORY_REGION_INFORMATION */
    {
        if (len < sizeof(MEMORY_REGION_INFORMATION32))
            status = STATUS_INFO_LENGTH_MISMATCH;
        else if (!ptr)
            status = STATUS_ACCESS_VIOLATION;
        else if (addr32 > highest_user_address)
            status = STATUS_INVALID_PARAMETER;
        else
        {
            MEMORY_REGION_INFORMATION info;
            MEMORY_REGION_INFORMATION32 *info32 = ptr;

            if (!(status = NtQueryVirtualMemory( handle, query_addr, class, &info, sizeof(info), &res_len )))
            {
                ULONG alloc_base32 = PtrToUlong( info.AllocationBase );

                info32->AllocationBase = alloc_base32;
                info32->AllocationProtect = info.AllocationProtect;
                info32->RegionType = info.RegionType;
                info32->RegionSize = info.RegionSize;
                info32->CommitSize = info.CommitSize;
                info32->PartitionId = info.PartitionId;
                info32->NodePreference = info.NodePreference;
                if (alloc_base32 <= highest_user_address && info.RegionSize > highest_user_address - alloc_base32 + 1)
                    info32->RegionSize = highest_user_address - alloc_base32 + 1;
            }
        }
        res_len = sizeof(MEMORY_REGION_INFORMATION32);
        break;
    }

    case MemoryWorkingSetExInformation:  /* MEMORY_WORKING_SET_EX_INFORMATION */
    {
        MEMORY_WORKING_SET_EX_INFORMATION32 *info32 = ptr;
        MEMORY_WORKING_SET_EX_INFORMATION *info;
        ULONG i, count = len / sizeof(*info32);

        if (len < sizeof(*info32)) return STATUS_INFO_LENGTH_MISMATCH;
        if (!ptr) return STATUS_ACCESS_VIOLATION;

        info = Wow64AllocateTemp( count * sizeof(*info) );
        for (i = 0; i < count; i++) info[i].VirtualAddress = ULongToPtr( info32[i].VirtualAddress );
        if (!(status = NtQueryVirtualMemory( handle, query_addr, class, info, count * sizeof(*info), &res_len )))
        {
            count = res_len / sizeof(*info);
            for (i = 0; i < count; i++) info32[i].VirtualAttributes.Flags = info[i].VirtualAttributes.Flags;
            res_len = count * sizeof(*info32);
        }
        break;
    }

    case MemoryImageInformation: /* MEMORY_IMAGE_INFORMATION */
    {
        if (len < sizeof(MEMORY_IMAGE_INFORMATION32)) return STATUS_INFO_LENGTH_MISMATCH;

        if (!ptr) status = STATUS_ACCESS_VIOLATION;
        else if (addr32 > highest_user_address) status = STATUS_INVALID_PARAMETER;
        else
        {
            MEMORY_IMAGE_INFORMATION info;
            MEMORY_IMAGE_INFORMATION32 *info32 = ptr;

            if (!(status = NtQueryVirtualMemory( handle, query_addr, class, &info, sizeof(info), &res_len )))
            {
                info32->ImageBase   = PtrToUlong( info.ImageBase );
                info32->SizeOfImage = info.SizeOfImage;
                info32->ImageFlags  = info.ImageFlags;
            }
        }
        res_len = sizeof(MEMORY_IMAGE_INFORMATION32);
        break;
    }

    case MemoryWineUnixWow64Funcs:
        return STATUS_INVALID_INFO_CLASS;

    case MemoryWineUnixFuncs:
        if (len != sizeof(UINT64)) status = STATUS_INFO_LENGTH_MISMATCH;
        else if (!ptr) status = STATUS_ACCESS_VIOLATION;
        else
        {
            UINT64 funcs = 0;

            MESSAGE( "macrunner-wow64: MemoryWineUnixFuncs enter handle=%p module32=%08lx query=%p out=%p len=%lu\n",
                     handle, addr32, query_addr, ptr, len );
            status = NtQueryVirtualMemory( handle, query_addr, MemoryWineUnixWow64Funcs,
                                            &funcs, sizeof(funcs), &res_len );
            MESSAGE( "macrunner-wow64: MemoryWineUnixFuncs native status=%08lx funcs=%016llx res_len=%lu\n",
                     status, (unsigned long long)funcs, res_len );
            if (!status) status = write_guest32_output( ptr, &funcs, sizeof(funcs) );
            MESSAGE( "macrunner-wow64: MemoryWineUnixFuncs leave status=%08lx out=%p\n", status, ptr );
        }
        break;

    default:
        FIXME( "unsupported class %u\n", class );
        return STATUS_INVALID_INFO_CLASS;
    }
    if (!status || status == STATUS_INFO_LENGTH_MISMATCH) put_size( retlen, res_len );
    if (class == MemoryWineUnixFuncs)
        MESSAGE( "macrunner-wow64: MemoryWineUnixFuncs return status=%08lx retlen=%p res_len=%lu\n",
                 status, retlen, res_len );
    return status;
}


/**********************************************************************
 *           wow64_NtReadVirtualMemory
 */
NTSTATUS WINAPI wow64_NtReadVirtualMemory( UINT *args )
{
    HANDLE process = get_handle( &args );
    const void *addr = get_ptr( &args );
    void *buffer = get_ptr( &args );
    SIZE_T size = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    SIZE_T ret_size;
    NTSTATUS status;

    status = NtReadVirtualMemory( process, addr, buffer, size, &ret_size );
    put_size( retlen, ret_size );
    return status;
}


/**********************************************************************
 *           wow64_NtResetWriteWatch
 */
NTSTATUS WINAPI wow64_NtResetWriteWatch( UINT *args )
{
    HANDLE process = get_handle( &args );
    void *base = get_ptr( &args );
    SIZE_T size = get_ulong( &args );

    return NtResetWriteWatch( process, base, size );
}


/**********************************************************************
 *           wow64_NtSetInformationVirtualMemory
 */
NTSTATUS WINAPI wow64_NtSetInformationVirtualMemory( UINT *args )
{
    HANDLE process = get_handle( &args );
    VIRTUAL_MEMORY_INFORMATION_CLASS info_class = get_ulong( &args );
    ULONG count = get_ulong( &args );
    MEMORY_RANGE_ENTRY32 *addresses32 = get_ptr( &args );
    PVOID ptr = get_ptr( &args );
    ULONG len = get_ulong( &args );

    MEMORY_RANGE_ENTRY *addresses;

    switch (info_class)
    {
    case VmPrefetchInformation:
    case VmPageDirtyStateInformation:
        break;
    default:
        FIXME( "(%p,info_class=%u,%lu,%p,%p,%lu): not implemented\n",
               process, info_class, count, addresses32, ptr, len );
        return STATUS_INVALID_PARAMETER_2;
    }

    if (!ptr) return STATUS_INVALID_PARAMETER_5;
    if (len != sizeof(ULONG)) return STATUS_INVALID_PARAMETER_6;
    if (!count) return STATUS_INVALID_PARAMETER_3;
    if (!addresses32) return STATUS_ACCESS_VIOLATION;

    addresses = memory_range_entry_array_32to64( addresses32, count );
    return NtSetInformationVirtualMemory( process, info_class, count, addresses, ptr, len );
}


/**********************************************************************
 *           wow64_NtSetLdtEntries
 */
NTSTATUS WINAPI wow64_NtSetLdtEntries( UINT *args )
{
    ULONG sel1 = get_ulong( &args );
    ULONG64 entry1 = get_ulong64( &args );
    ULONG sel2 = get_ulong( &args );
    ULONG64 entry2 = get_ulong64( &args );

    /* CW Hack 26470 & 26456: Rosetta hangs if you try to set a 16-bit LDT */
    {
        char buffer[64];
        NTSTATUS status = NtQuerySystemInformation( SystemProcessorBrandString, buffer, sizeof(buffer), NULL );

        if (!status && strstr( buffer, "VirtualApple" ))
        {
            ERR("HACK: not calling NtSetLdtEntries()\n");
            return STATUS_NOT_IMPLEMENTED;
        }
    }

    return NtSetLdtEntries( sel1, *(LDT_ENTRY *)&entry1, sel2, *(LDT_ENTRY *)&entry2 );
}


/**********************************************************************
 *           wow64_NtUnlockVirtualMemory
 */
NTSTATUS WINAPI wow64_NtUnlockVirtualMemory( UINT *args )
{
    HANDLE process = get_handle( &args );
    ULONG *addr32 = get_ptr( &args );
    ULONG *size32 = get_ptr( &args );
    ULONG unknown = get_ulong( &args );

    void *addr;
    SIZE_T size;
    NTSTATUS status;

    if (!addr32 || !size32) return STATUS_ACCESS_VIOLATION;

    status = NtUnlockVirtualMemory( process, addr_32to64( &addr, addr32 ),
                                    size_32to64( &size, size32 ), unknown );
    if (!status)
    {
        put_addr( addr32, addr );
        put_size( size32, size );
    }
    return status;
}


/**********************************************************************
 *           wow64_NtUnmapViewOfSection
 */
NTSTATUS WINAPI wow64_NtUnmapViewOfSection( UINT *args )
{
    HANDLE process = get_handle( &args );
    void *addr = get_ptr( &args );

    BOOL is_current = RtlIsCurrentProcess( process );
    NTSTATUS status;

    if (is_current && pBTCpuNotifyUnmapViewOfSection) pBTCpuNotifyUnmapViewOfSection( addr, FALSE, 0 );
    status = NtUnmapViewOfSection( process, addr );
    if (is_current && pBTCpuNotifyUnmapViewOfSection) pBTCpuNotifyUnmapViewOfSection( addr, TRUE, status );
    return status;
}


/**********************************************************************
 *           wow64_NtUnmapViewOfSectionEx
 */
NTSTATUS WINAPI wow64_NtUnmapViewOfSectionEx( UINT *args )
{
    HANDLE process = get_handle( &args );
    void *addr = get_ptr( &args );
    ULONG flags = get_ulong( &args );

    BOOL is_current = RtlIsCurrentProcess( process );
    NTSTATUS status;

    if (is_current && pBTCpuNotifyUnmapViewOfSection) pBTCpuNotifyUnmapViewOfSection( addr, FALSE, 0 );
    status = NtUnmapViewOfSectionEx( process, addr, flags );
    if (is_current && pBTCpuNotifyUnmapViewOfSection) pBTCpuNotifyUnmapViewOfSection( addr, TRUE, status );
    return status;
}


/**********************************************************************
 *           wow64_NtWow64AllocateVirtualMemory64
 */
NTSTATUS WINAPI wow64_NtWow64AllocateVirtualMemory64( UINT *args )
{
    HANDLE process = get_handle( &args );
    void **addr = get_ptr( &args );
    ULONG_PTR zero_bits = get_ulong64( &args );
    SIZE_T *size = get_ptr( &args );
    ULONG type = get_ulong( &args );
    ULONG protect = get_ulong( &args );

    if (!addr || !size) return STATUS_ACCESS_VIOLATION;
    return NtAllocateVirtualMemory( process, addr, zero_bits, size, type, protect );
}


/**********************************************************************
 *           wow64_NtWow64ReadVirtualMemory64
 */
NTSTATUS WINAPI wow64_NtWow64ReadVirtualMemory64( UINT *args )
{
    HANDLE process = get_handle( &args );
    void *addr = (void *)(ULONG_PTR)get_ulong64( &args );
    void *buffer = get_ptr( &args );
    SIZE_T size = get_ulong64( &args );
    SIZE_T *ret_size = get_ptr( &args );

    return NtReadVirtualMemory( process, addr, buffer, size, ret_size );
}


/**********************************************************************
 *           wow64_NtWow64WriteVirtualMemory64
 */
NTSTATUS WINAPI wow64_NtWow64WriteVirtualMemory64( UINT *args )
{
    HANDLE process = get_handle( &args );
    void *addr = (void *)(ULONG_PTR)get_ulong64( &args );
    const void *buffer = get_ptr( &args );
    SIZE_T size = get_ulong64( &args );
    SIZE_T *ret_size = get_ptr( &args );

    return NtWriteVirtualMemory( process, addr, buffer, size, ret_size );
}


/**********************************************************************
 *           wow64_NtWriteVirtualMemory
 */
NTSTATUS WINAPI wow64_NtWriteVirtualMemory( UINT *args )
{
    HANDLE process = get_handle( &args );
    void *addr = get_ptr( &args );
    const void *buffer = get_ptr( &args );
    SIZE_T size = get_ulong( &args );
    ULONG *retlen = get_ptr( &args );

    SIZE_T ret_size;
    NTSTATUS status;

    status = NtWriteVirtualMemory( process, addr, buffer, size, &ret_size );
    put_size( retlen, ret_size );
    return status;
}
