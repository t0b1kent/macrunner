#include "hb_cache.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

#define HB_AOT_MAGIC   "HBTC"
#define HB_AOT_VERSION 2u
#define HB_AOT_ABI     1u

/* Persistent translation cache.  The cache is append-mode logically, but every
 * mutation rewrites through a temp file + fsync + rename so readers never see a
 * partially-written entry. */
typedef struct {
    char     magic[4];
    uint32_t version;
    uint32_t abi_version;
    uint32_t reserved;
} hb_aot_header_t;

typedef struct {
    uint32_t key_size;
    uint32_t native_size;
    uint32_t steps;
    uint8_t  valid;
    uint8_t  unsupported;
    uint16_t reserved;
} hb_aot_entry_meta_t;

typedef struct {
    hb_cache_key_t key;
    uint8_t* native_code;
    size_t native_size;
    uint32_t steps;
    uint8_t valid;
    uint8_t unsupported;
} hb_disk_entry_t;

struct hb_cache {
    char path[512];
    char root[512];
    uint32_t format_version;
    uint32_t abi_version;
    hb_cache_stats_t stats;
    hb_disk_entry_t* entries;
    size_t count;
    size_t cap;
    bool dirty;
};

static void free_entries(hb_disk_entry_t* entries, size_t count) {
    if (!entries) return;
    for (size_t i = 0; i < count; i++) free(entries[i].native_code);
    free(entries);
}

static int write_header(FILE* fp, uint32_t format_version, uint32_t abi_version) {
    hb_aot_header_t h;
    memcpy(h.magic, HB_AOT_MAGIC, 4);
    h.version = format_version ? format_version : HB_AOT_VERSION;
    h.abi_version = abi_version ? abi_version : HB_AOT_ABI;
    h.reserved = 0;
    return (fwrite(&h, sizeof(h), 1, fp) == 1) ? 0 : -1;
}

static bool force_aot_sync_failure(void) {
    const char* value = getenv("MACRUNNER_HB_TEST_FORCE_AOT_SYNC_FAIL");
    return value && value[0] && value[0] != '0';
}

static int flush_and_sync(FILE* fp) {
    if (!fp) return -1;
    if (fflush(fp) != 0) return -1;
    if (force_aot_sync_failure()) return -1;
    if (fsync(fileno(fp)) != 0) return -1;
    return 0;
}

static int read_header(FILE* fp, uint32_t format_version, uint32_t abi_version) {
    hb_aot_header_t h;
    if (fread(&h, sizeof(h), 1, fp) != 1) return -1;
    if (memcmp(h.magic, HB_AOT_MAGIC, 4) != 0) return -1;
    if (h.version != (format_version ? format_version : HB_AOT_VERSION)) return -1;
    if (h.abi_version != (abi_version ? abi_version : HB_AOT_ABI)) return -1;
    return 0;
}

static int write_entry(FILE* fp, const hb_disk_entry_t* entry) {
    hb_aot_entry_meta_t meta;

    if (!fp || !entry) return -1;
    memset(&meta, 0, sizeof(meta));
    meta.key_size = sizeof(hb_cache_key_t);
    meta.native_size = (uint32_t)entry->native_size;
    meta.steps = entry->steps;
    meta.valid = entry->valid;
    meta.unsupported = entry->unsupported;
    if (fwrite(&meta, sizeof(meta), 1, fp) != 1 ||
        fwrite(&entry->key, sizeof(entry->key), 1, fp) != 1)
        return -1;
    if (entry->native_size && entry->native_code &&
        fwrite(entry->native_code, 1, entry->native_size, fp) != entry->native_size)
        return -1;
    return 0;
}

static void make_parent_dir(const char* path) {
    char tmp[512];
    const char* slash;
    if (!path) return;
    slash = strrchr(path, '/');
    if (!slash || slash == path) return;
    size_t len = (size_t)(slash - path);
    if (len >= sizeof(tmp)) return;
    memcpy(tmp, path, len);
    tmp[len] = '\0';
    mkdir(tmp, 0755);
}

static int ensure_file(hb_cache_t* c) {
    FILE* fp;
    if (!c || !c->path[0]) return -1;
    make_parent_dir(c->path);
    fp = fopen(c->path, "rb");
    if (fp) {
        int ok = read_header(fp, c->format_version, c->abi_version);
        fclose(fp);
        if (ok == 0) return 0;
    }
    fp = fopen(c->path, "wb");
    if (!fp) return -1;
    if (write_header(fp, c->format_version, c->abi_version) != 0) { fclose(fp); return -1; }
    if (flush_and_sync(fp) != 0) { fclose(fp); return -1; }
    if (fclose(fp) != 0) return -1;
    return 0;
}

static hb_result_t load_entries(hb_cache_t* c, hb_disk_entry_t** out_entries, size_t* out_count) {
    FILE* fp;
    size_t cap = 16, count = 0;
    hb_disk_entry_t* entries;
    if (!c || !out_entries || !out_count) return HB_ERR_INVALID_ARG;
    *out_entries = NULL;
    *out_count = 0;
    fp = fopen(c->path, "rb");
    if (!fp) return HB_OK;
    if (read_header(fp, c->format_version, c->abi_version) != 0) {
        c->stats.corrupt_entries_ignored++;
        fclose(fp);
        return HB_OK;
    }
    entries = calloc(cap, sizeof(*entries));
    if (!entries) { fclose(fp); return HB_ERR_OUT_OF_MEMORY; }
    for (;;) {
        hb_aot_entry_meta_t meta;
        hb_cache_key_t key;
        if (fread(&meta, sizeof(meta), 1, fp) != 1) break;
        if (meta.key_size != sizeof(hb_cache_key_t) || meta.native_size > (128u * 1024u * 1024u)) {
            c->stats.corrupt_entries_ignored++;
            break;
        }
        if (fread(&key, sizeof(key), 1, fp) != 1) {
            c->stats.corrupt_entries_ignored++;
            break;
        }
        if (count >= cap) {
            cap *= 2;
            hb_disk_entry_t* n = realloc(entries, cap * sizeof(*entries));
            if (!n) { free_entries(entries, count); fclose(fp); return HB_ERR_OUT_OF_MEMORY; }
            memset(n + count, 0, (cap - count) * sizeof(*entries));
            entries = n;
        }
        entries[count].key = key;
        entries[count].native_size = meta.native_size;
        entries[count].steps = meta.steps;
        entries[count].valid = meta.valid;
        entries[count].unsupported = meta.unsupported;
        if (meta.native_size) {
            entries[count].native_code = malloc(meta.native_size);
            if (!entries[count].native_code) { free_entries(entries, count); fclose(fp); return HB_ERR_OUT_OF_MEMORY; }
            if (fread(entries[count].native_code, 1, meta.native_size, fp) != meta.native_size) {
                c->stats.corrupt_entries_ignored++;
                free(entries[count].native_code);
                entries[count].native_code = NULL;
                break;
            }
        }
        count++;
    }
    fclose(fp);
    *out_entries = entries;
    *out_count = count;
    return HB_OK;
}

static hb_result_t write_entries_atomic(hb_cache_t* c, const hb_disk_entry_t* entries, size_t count) {
    char tmp[560];
    FILE* fp;
    if (!c) return HB_ERR_INVALID_ARG;
    snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", c->path, (long)getpid());
    fp = fopen(tmp, "wb");
    if (!fp) return HB_ERR_NOT_FOUND;
    if (write_header(fp, c->format_version, c->abi_version) != 0) { fclose(fp); unlink(tmp); return HB_ERR_NOT_FOUND; }
    for (size_t i = 0; i < count; i++) {
        if (write_entry(fp, &entries[i]) != 0) { fclose(fp); unlink(tmp); return HB_ERR_NOT_FOUND; }
    }
    if (flush_and_sync(fp) != 0) { fclose(fp); unlink(tmp); return HB_ERR_NOT_FOUND; }
    if (fclose(fp) != 0) { unlink(tmp); return HB_ERR_NOT_FOUND; }
    if (rename(tmp, c->path) != 0) { unlink(tmp); return HB_ERR_NOT_FOUND; }
    c->dirty = false;
    return HB_OK;
}

static hb_result_t append_entry(hb_cache_t* c, const hb_disk_entry_t* entry) {
    FILE* fp;

    if (!c || !entry) return HB_ERR_INVALID_ARG;
    if (force_aot_sync_failure()) return HB_ERR_NOT_FOUND;
    fp = fopen(c->path, "ab");
    if (!fp) return HB_ERR_NOT_FOUND;
    if (write_entry(fp, entry) != 0) { fclose(fp); return HB_ERR_NOT_FOUND; }
    if (fflush(fp) != 0) { fclose(fp); return HB_ERR_NOT_FOUND; }
    if (fclose(fp) != 0) return HB_ERR_NOT_FOUND;
    c->dirty = true;
    return HB_OK;
}

hb_cache_t* hb_cache_open(const char* root, const hb_cache_options_t* options) {
    hb_cache_t* c = calloc(1, sizeof(hb_cache_t));
    if (!c) return NULL;
    c->format_version = options && options->format_version ? options->format_version : HB_AOT_VERSION;
    c->abi_version = options && options->abi_version ? options->abi_version : HB_AOT_ABI;
    if (root && root[0]) strncpy(c->root, root, sizeof(c->root) - 1);
    else strncpy(c->root, "build/hyperbridge-cache", sizeof(c->root) - 1);
    mkdir(c->root, 0755);
    snprintf(c->path, sizeof(c->path), "%s/translation-cache.bin", c->root);
    if (ensure_file(c) != 0) { free(c); return NULL; }
    if (load_entries(c, &c->entries, &c->count) != HB_OK) { free(c); return NULL; }
    c->cap = c->count ? c->count : 16;
    return c;
}

hb_cache_t* hb_cache_create(const char* path) {
    hb_cache_t* c = calloc(1, sizeof(hb_cache_t));
    if (!c) return NULL;
    c->format_version = HB_AOT_VERSION;
    c->abi_version = HB_AOT_ABI;
    if (path && path[0]) strncpy(c->path, path, sizeof(c->path) - 1);
    else strncpy(c->path, "build/hyperbridge-cache/translation-cache.bin", sizeof(c->path) - 1);
    if (ensure_file(c) != 0) { free(c); return NULL; }
    if (load_entries(c, &c->entries, &c->count) != HB_OK) { free(c); return NULL; }
    c->cap = c->count ? c->count : 16;
    return c;
}

void hb_cache_destroy(hb_cache_t* cache) {
    if (!cache) return;
    free_entries(cache->entries, cache->count);
    free(cache);
}
void hb_cache_close(hb_cache_t* cache) {
    if (!cache) return;
    if (cache->dirty) (void)write_entries_atomic(cache, cache->entries, cache->count);
    hb_cache_destroy(cache);
}

hb_result_t hb_cache_get(hb_cache_t* cache, const hb_cache_key_t* key, hb_cache_entry_t** out) {
    if (!cache || !key || !out) return HB_ERR_INVALID_ARG;
    *out = NULL;
    cache->stats.lookups++;
    for (size_t i = cache->count; i > 0; i--) {
        size_t pos = i - 1;
        if (memcmp(&cache->entries[pos].key, key, sizeof(*key)) == 0) {
            hb_cache_entry_t* e = calloc(1, sizeof(*e));
            if (!e) return HB_ERR_OUT_OF_MEMORY;
            e->key = cache->entries[pos].key;
            e->valid = cache->entries[pos].valid != 0;
            e->unsupported = cache->entries[pos].unsupported != 0;
            e->steps = cache->entries[pos].steps;
            e->native_size = cache->entries[pos].native_size;
            if (e->native_size) {
                e->native_code = malloc(e->native_size);
                if (!e->native_code) { free(e); return HB_ERR_OUT_OF_MEMORY; }
                memcpy(e->native_code, cache->entries[pos].native_code, e->native_size);
            }
            *out = e;
            cache->stats.hits++;
            return HB_OK;
        }
    }
    cache->stats.misses++;
    return HB_ERR_NOT_FOUND;
}

hb_result_t hb_cache_lookup(hb_cache_t* cache, const hb_cache_key_t* key, hb_cache_entry_t** out) {
    return hb_cache_get(cache, key, out);
}

hb_result_t hb_cache_put(hb_cache_t* cache, const hb_cache_key_t* key, hb_cache_entry_t* entry) {
    size_t pos;
    bool replacing;
    if (!cache || !key || !entry) return HB_ERR_INVALID_ARG;
    pos = cache->count;
    for (size_t i = cache->count; i > 0; i--) {
        size_t candidate = i - 1;
        if (memcmp(&cache->entries[candidate].key, key, sizeof(*key)) == 0) {
            pos = candidate;
            break;
        }
    }
    replacing = pos != cache->count;
    if (pos == cache->count) {
        if (cache->count >= cache->cap) {
            size_t new_cap = cache->cap ? cache->cap * 2 : 16;
            hb_disk_entry_t* n = realloc(cache->entries, new_cap * sizeof(*cache->entries));
            if (!n) return HB_ERR_OUT_OF_MEMORY;
            memset(n + cache->cap, 0, (new_cap - cache->cap) * sizeof(*n));
            cache->entries = n;
            cache->cap = new_cap;
        }
        cache->count++;
    } else {
        free(cache->entries[pos].native_code);
    }
    memset(&cache->entries[pos], 0, sizeof(cache->entries[pos]));
    cache->entries[pos].key = *key;
    cache->entries[pos].native_size = entry->native_size;
    cache->entries[pos].steps = entry->steps;
    cache->entries[pos].valid = entry->valid ? 1 : 0;
    cache->entries[pos].unsupported = entry->unsupported ? 1 : 0;
    if (entry->native_code && entry->native_size) {
        cache->entries[pos].native_code = malloc(entry->native_size);
        if (!cache->entries[pos].native_code) return HB_ERR_OUT_OF_MEMORY;
        memcpy(cache->entries[pos].native_code, entry->native_code, entry->native_size);
        cache->stats.bytes_stored += entry->native_size;
    }
    cache->stats.entries_stored++;
    (void)replacing;
    return append_entry(cache, &cache->entries[pos]);
}

hb_result_t hb_cache_store(hb_cache_t* cache, const hb_cache_key_t* key, const uint8_t* native_blob, size_t native_size, const hb_cache_entry_t* metadata) {
    hb_cache_entry_t entry;
    if (!cache || !key) return HB_ERR_INVALID_ARG;
    memset(&entry, 0, sizeof(entry));
    if (metadata) entry = *metadata;
    entry.key = *key;
    entry.native_code = (uint8_t*)native_blob;
    entry.native_size = native_size;
    entry.valid = true;
    return hb_cache_put(cache, key, &entry);
}

hb_result_t hb_cache_invalidate(hb_cache_t* cache, uint32_t version) {
    size_t count, keep = 0;
    hb_result_t r;
    if (!cache) return HB_ERR_INVALID_ARG;
    count = cache->count;
    for (size_t i = 0; i < count; i++) {
        if (cache->entries[i].key.version == version) {
            if (keep != i) cache->entries[keep] = cache->entries[i];
            keep++;
        } else {
            free(cache->entries[i].native_code);
            cache->stats.invalidations++;
        }
    }
    cache->count = keep;
    r = write_entries_atomic(cache, cache->entries, cache->count);
    return r;
}

hb_result_t hb_cache_invalidate_module(hb_cache_t* cache, uint64_t module_id) {
    size_t count, keep = 0;
    hb_result_t r;
    if (!cache) return HB_ERR_INVALID_ARG;
    count = cache->count;
    for (size_t i = 0; i < count; i++) {
        if (cache->entries[i].key.module_id != module_id) {
            if (keep != i) cache->entries[keep] = cache->entries[i];
            keep++;
        } else {
            free(cache->entries[i].native_code);
            cache->stats.invalidations++;
        }
    }
    cache->count = keep;
    r = write_entries_atomic(cache, cache->entries, cache->count);
    return r;
}

hb_result_t hb_cache_prune(hb_cache_t* cache, uint64_t max_bytes) {
    struct stat st;
    if (!cache) return HB_ERR_INVALID_ARG;
    if (stat(cache->path, &st) != 0) return HB_OK;
    if ((uint64_t)st.st_size <= max_bytes) return HB_OK;
    return hb_cache_clear(cache);
}

hb_result_t hb_cache_clear(hb_cache_t* cache) {
    if (!cache) return HB_ERR_INVALID_ARG;
    free_entries(cache->entries, cache->count);
    cache->entries = NULL;
    cache->count = 0;
    cache->cap = 0;
    cache->stats.invalidations++;
    return write_entries_atomic(cache, NULL, 0);
}

hb_result_t hb_cache_stats(hb_cache_t* cache, hb_cache_stats_t* out) {
    if (!cache || !out) return HB_ERR_INVALID_ARG;
    *out = cache->stats;
    return HB_OK;
}

void hb_cache_entry_free(hb_cache_entry_t* entry) {
    if (!entry) return;
    free(entry->native_code);
    free(entry);
}

hb_result_t hb_cache_key_compute(const uint8_t* code, size_t len, hb_arch_t arch, uint32_t version, hb_cache_key_t* out) {
    if (!code || !out) return HB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(hb_cache_key_t));
    out->version = version;
    out->abi_version = HB_AOT_ABI;
    out->arch = (uint8_t)arch;
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) { h ^= code[i]; h *= 1099511628211ULL; }
    out->code_hash = h;
    out->code_len = len;
    return HB_OK;
}
