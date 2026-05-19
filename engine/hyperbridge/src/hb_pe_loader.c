#include "hb_pe.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

hb_pe_image_t* hb_pe_load(const uint8_t* data, size_t size) {
    if (!data || size < 64) return NULL;
    hb_pe_image_t* pe = calloc(1, sizeof(hb_pe_image_t));
    if (!pe) return NULL;
    pe->raw_data = data;
    pe->raw_size = size;
    /* DOS header */
    pe->dos.e_magic = *(uint16_t*)data;
    if (pe->dos.e_magic != HB_PE_DOS_SIGNATURE) { free(pe); return NULL; }
    pe->dos.e_lfanew = *(uint32_t*)(data + 60);
    if (pe->dos.e_lfanew + 24 > size) { free(pe); return NULL; }
    /* NT signature */
    pe->nt_signature = *(uint32_t*)(data + pe->dos.e_lfanew);
    if (pe->nt_signature != HB_PE_NT_SIGNATURE) { free(pe); return NULL; }
    /* COFF header */
    uint8_t* coff = (uint8_t*)(data + pe->dos.e_lfanew + 4);
    pe->machine = *(uint16_t*)(coff);
    pe->number_of_sections = *(uint16_t*)(coff + 2);
    pe->time_date_stamp = *(uint32_t*)(coff + 4);
    pe->size_of_optional_header = *(uint16_t*)(coff + 16);
    pe->characteristics = *(uint16_t*)(coff + 18);
    /* Optional header */
    uint8_t* opt = coff + 20;
    pe->magic = *(uint16_t*)opt;
    if (pe->magic == 0x10b || pe->magic == 0x20b) {
        pe->entry_point = *(uint32_t*)(opt + 16);
        if (pe->magic == 0x10b) {
            pe->image_base_64 = *(uint32_t*)(opt + 28);
        } else {
            pe->image_base_64 = *(uint64_t*)(opt + 24);
        }
        pe->size_of_image = *(uint32_t*)(opt + 56);
        pe->size_of_headers = *(uint32_t*)(opt + 60);
        pe->number_of_rva_and_sizes = *(uint32_t*)(opt + 244);
        memcpy(pe->data_directory, opt + 248, sizeof(pe->data_directory));
    }
    pe->preferred_base = pe->image_base_64;
    /* Sections */
    pe->sections = calloc(pe->number_of_sections, sizeof(hb_pe_section_t));
    pe->section_count = pe->number_of_sections;
    uint8_t* sec = opt + pe->size_of_optional_header;
    for (uint16_t i = 0; i < pe->number_of_sections; i++) {
        memcpy(pe->sections[i].name, sec, 8);
        pe->sections[i].virtual_size = *(uint32_t*)(sec + 8);
        pe->sections[i].virtual_address = *(uint32_t*)(sec + 12);
        pe->sections[i].size_of_raw_data = *(uint32_t*)(sec + 16);
        pe->sections[i].pointer_to_raw_data = *(uint32_t*)(sec + 20);
        pe->sections[i].characteristics = *(uint32_t*)(sec + 36);
        sec += 40;
    }
    return pe;
}

void hb_pe_unload(hb_pe_image_t* pe) {
    if (!pe) return;
    free(pe->sections);
    for (uint32_t i = 0; i < pe->import_dll_count; i++) free(pe->import_dlls[i]);
    free(pe->import_dlls);
    if (pe->mapped_image) munmap(pe->mapped_image, pe->mapped_size);
    free(pe);
}

hb_result_t hb_pe_map_image(hb_pe_image_t* pe, uint64_t base) {
    if (!pe || !pe->raw_data) return HB_ERR_INVALID_ARG;
    if (pe->mapped_image) { munmap(pe->mapped_image, pe->mapped_size); pe->mapped_image = NULL; }

    size_t map_size = pe->size_of_image;
    if (map_size == 0) return HB_ERR_INVALID_ARG;
    map_size = (map_size + 4095) & ~4095;

    void* p = mmap((void*)base, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        p = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return HB_ERR_OUT_OF_MEMORY;
    }
    pe->mapped_image = (uint8_t*)p;
    pe->mapped_size = map_size;
    pe->mapped_base = (uint64_t)(uintptr_t)p;

    /* Copy headers */
    size_t hdr_size = pe->size_of_headers;
    if (hdr_size > pe->raw_size) hdr_size = pe->raw_size;
    memcpy(pe->mapped_image, pe->raw_data, hdr_size);

    /* Copy sections */
    for (uint16_t i = 0; i < pe->number_of_sections; i++) {
        hb_pe_section_t* s = &pe->sections[i];
        if (s->virtual_address == 0) continue;
        uint8_t* dest = pe->mapped_image + s->virtual_address;
        uint32_t raw_size = s->size_of_raw_data;
        uint32_t virt_size = s->virtual_size;
        if (raw_size > 0 && s->pointer_to_raw_data < pe->raw_size) {
            if (raw_size > pe->raw_size - s->pointer_to_raw_data)
                raw_size = (uint32_t)(pe->raw_size - s->pointer_to_raw_data);
            memcpy(dest, pe->raw_data + s->pointer_to_raw_data, raw_size);
        }
        if (virt_size > raw_size) {
            memset(dest + raw_size, 0, virt_size - raw_size);
        }
        /* Set permissions based on characteristics */
        int prot = PROT_READ;
        if (s->characteristics & 0x80000000) prot |= PROT_WRITE; /* IMAGE_SCN_MEM_WRITE */
        if (s->characteristics & 0x20000000) prot |= PROT_EXEC; /* IMAGE_SCN_MEM_EXECUTE */
        mprotect(dest, (virt_size + 4095) & ~4095, prot);
    }

    return HB_OK;
}

hb_result_t hb_pe_apply_relocations(hb_pe_image_t* pe, uint64_t new_base) {
    if (!pe || !pe->mapped_image) return HB_ERR_INVALID_ARG;
    if (new_base == pe->preferred_base) return HB_OK;

    uint32_t reloc_rva = pe->data_directory[HB_PE_DD_BASERELOC][0];
    uint32_t reloc_size = pe->data_directory[HB_PE_DD_BASERELOC][1];
    if (reloc_rva == 0 || reloc_size == 0) return HB_OK;

    int64_t delta = (int64_t)new_base - (int64_t)pe->preferred_base;
    uint8_t* base = pe->mapped_image;

    uint32_t offset = 0;
    while (offset < reloc_size) {
        uint32_t page_rva = *(uint32_t*)(base + reloc_rva + offset);
        uint32_t block_size = *(uint32_t*)(base + reloc_rva + offset + 4);
        if (block_size == 0 || block_size > reloc_size - offset) break;

        uint32_t num_entries = (block_size - 8) / 2;
        uint16_t* entries = (uint16_t*)(base + reloc_rva + offset + 8);

        for (uint32_t i = 0; i < num_entries; i++) {
            uint16_t entry = entries[i];
            uint16_t type = (entry >> 12) & 0xF;
            uint16_t off = entry & 0xFFF;

            if (type == 0) continue;
            uint8_t* addr = base + page_rva + off;
            if (type == 3) {
                uint32_t* p = (uint32_t*)addr;
                *p = (uint32_t)((int64_t)*p + delta);
            } else if (type == 10) {
                uint64_t* p = (uint64_t*)addr;
                *p = (uint64_t)((int64_t)*p + delta);
            }
        }
        offset += block_size;
    }

    return HB_OK;
}

hb_pe_section_t* hb_pe_find_section(hb_pe_image_t* pe, const char* name) {
    if (!pe || !name) return NULL;
    for (uint16_t i = 0; i < pe->section_count; i++) {
        if (strncmp(pe->sections[i].name, name, 8) == 0) return &pe->sections[i];
    }
    return NULL;
}

hb_gva_t hb_pe_rva_to_gva(hb_pe_image_t* pe, uint32_t rva) {
    if (!pe) return 0;
    if (pe->mapped_base) return pe->mapped_base + rva;
    return pe->preferred_base + rva;
}

const char* hb_pe_import_dll_name(hb_pe_image_t* pe, uint32_t rva) {
    if (!pe || !pe->mapped_image) return NULL;
    return (const char*)(pe->mapped_image + rva);
}

const char* hb_pe_import_func_name(hb_pe_image_t* pe, uint32_t rva) {
    if (!pe || !pe->mapped_image) return NULL;
    return (const char*)(pe->mapped_image + rva + 2); /* skip hint */
}
