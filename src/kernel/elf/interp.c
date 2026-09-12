#include "interp.h"
#include "loader.h"
#include "mm.h"
#include "fs/vfs.h"

/* Default dynamic linker search paths. The PT_INTERP string of a glibc binary
 * is usually an absolute path (e.g. /lib64/ld-linux-x86-64.so.2); these entries
 * are used only when we need a fallback default. */
static const char* interp_candidates[] = {
    "/lib/ld-linux-x86-64.so.2",
    "/lib64/ld-linux-x86-64.so.2",
};
#define INTERP_CANDIDATES (sizeof(interp_candidates) / sizeof(interp_candidates[0]))

static const char* interp_path = (const char*)0;
static uint64_t interp_base = 0;

const char* elf_get_interp_path(void)
{
    return interp_path;
}

uint64_t elf_get_interp_base(void)
{
    return interp_base;
}

/* Read an entire regular file into a freshly allocated buffer. Uses
 * pmalloc_contig so the buffer is physically contiguous: vfs_read writes
 * fsize bytes into one linear buffer, and stray page allocations (hot-cache
 * reuse, bitmap fragmentation) would otherwise make pmalloc() pages
 * non-contiguous, letting the write overflow into adjacent pages (e.g. the
 * current task's kernel stack). Returns NULL on any error. */
static void* read_whole_file(const char* path, uint64_t* size_out)
{
    file_t* f = vfs_open(path, O_RDONLY);
    if (!f) return NULL;

    uint64_t fsize = f->size;
    if (fsize == 0 || fsize > (16 * 1024 * 1024)) {
        vfs_close(f);
        return NULL;
    }

    uint64_t num_pages = (fsize + 0xFFF) / 0x1000;
    void* buf = pmalloc_contig((int)num_pages);
    if (!buf) {
        vfs_close(f);
        return NULL;
    }

    if (vfs_read(f, buf, fsize) != fsize) {
        vfs_close(f);
        return NULL;
    }
    vfs_close(f);

    if (size_out) *size_out = fsize;
    return buf;
}

int elf_interp_available(void)
{
    for (uint64_t i = 0; i < INTERP_CANDIDATES; i++) {
        file_t* f = vfs_open(interp_candidates[i], O_RDONLY);
        if (f) {
            vfs_close(f);
            return 1;
        }
    }
    return 0;
}

int elf_load_interp(vm_context_t* vm, const char* path, uint64_t* entry_out)
{
    if (!vm || !path || !entry_out) return -1;

    uint64_t size = 0;
    void* data = read_whole_file(path, &size);
    if (!data) return -1;

    /* glibc's ld-linux is ET_DYN (PIE); map it at the standard linker load
     * bias. elf_map_program uses its own ET_DYN bias resolution. */
    uint64_t base = 0x7F0000000000ULL;  /* typical mmap base for ld.so */
    uint64_t entry = 0;
    int r = elf_map_program(data, size, vm, base, &entry);
    if (r == 0) {
        interp_path = path;
        interp_base = base;
        *entry_out = entry;
    }
    return r;
}