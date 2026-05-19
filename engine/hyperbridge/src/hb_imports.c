#include "hb_pe.h"
#include "hb_thunk.h"
#include <stdlib.h>
#include <string.h>

hb_result_t hb_imports_resolve(hb_pe_image_t* pe, hb_thunk_table_t* thunks) {
    if (!pe || !pe->mapped_image || !thunks) return HB_ERR_INVALID_ARG;

    uint32_t import_rva = pe->data_directory[HB_PE_DD_IMPORT][0];
    uint32_t import_size = pe->data_directory[HB_PE_DD_IMPORT][1];
    if (import_rva == 0 || import_size == 0) return HB_OK; /* no imports */

    uint8_t* base = pe->mapped_image;
    hb_pe_import_desc_t* desc = (hb_pe_import_desc_t*)(base + import_rva);

    while (desc->name_rva != 0) {
        const char* dll_name = hb_pe_import_dll_name(pe, desc->name_rva);
        if (!dll_name) break;

        uint64_t* orig_thunk = (uint64_t*)(base + desc->original_first_thunk);
        uint64_t* iat = (uint64_t*)(base + desc->first_thunk);

        size_t idx = 0;
        while (1) {
            uint64_t entry = orig_thunk[idx];
            if (entry == 0) break;

            if (entry & 0x8000000000000000ULL) {
                /* Ordinal import — not supported in MVP */
                iat[idx] = 0xDEADBEEF;
            } else {
                const char* func_name = hb_pe_import_func_name(pe, (uint32_t)entry);
                if (func_name) {
                    hb_thunk_def_t* thunk = hb_thunk_find_by_name(thunks, dll_name, func_name);
                    if (thunk) {
                        iat[idx] = thunk->id;
                    } else {
                        iat[idx] = 0xDEADBEEF; /* unresolved sentinel */
                    }
                } else {
                    iat[idx] = 0xDEADBEEF;
                }
            }
            idx++;
        }

        desc++;
    }

    return HB_OK;
}
