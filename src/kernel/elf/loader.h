#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include "axion.h"

/* ELF header constants */
#define EI_NIDENT     16
#define ELFCLASS64    2
#define ELFDATA2LSB   1
#define ET_EXEC       2
#define ET_DYN        3

#define PT_NULL       0
#define PT_LOAD       1
#define PT_DYNAMIC    2
#define PT_INTERP     3
#define PT_NOTE       4
#define PT_PHDR       6
#define PT_GNU_STACK  0x6474E551

/* p_flags bits */
#define PF_X          0x1
#define PF_W          0x2
#define PF_R          0x4

/* 64-bit ELF header */
typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

/* 64-bit Program Header */
typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

/* Forward declaration (mm/vm.h defines the full struct). */
struct vm_context;
typedef struct vm_context vm_context_t;

/* ELF load context */
typedef struct {
    uint64_t entry;
    uint64_t main_entry;      /* W6: main program entry (pre-interp; AT_ENTRY) */
    uint64_t stack_top;
    uint64_t base;
    uint64_t brk_start;
    uint64_t brk_end;
    uint64_t phdr_count;
    uint64_t phdr_entsize;    /* W6: e_phentsize (AT_PHENT) */
    uint64_t phdr_addr;
    uint64_t interp_base;         /* W6: linker load bias (AT_BASE; 0 = static) */
    vm_context_t* vmctx;          /* W6: per-task address space */
    char    interp_path[64];      /* W6: PT_INTERP path (empty = static) */
} elf_context_t;

#define USER_STACK_SIZE  (4 * 1024 * 1024)  /* 4MB */
#define USER_STACK_TOP   0x8000000000ULL     /* 512GB (typical Linux) */

int  elf_load(const char* path, elf_context_t* ctx);
int  elf_load_from_mem(void* data, uint64_t size, elf_context_t* ctx);
void elf_unload(elf_context_t* ctx);

/* Map an ELF image (in a kernel buffer) into an existing vm_context at `base`;
 * returns the entry via *entry_out. Used by the dynamic linker loader. */
int  elf_map_program(void* data, uint64_t size, vm_context_t* vm,
                     uint64_t base, uint64_t* entry_out);

/* Prepare the initial user stack (argc/argv/envp + auxv) in `ctx->vmctx` and
 * return the resulting user RSP via *rsp_out. `argv`/`envp` are arrays of
 * user-space string pointers (read from the caller's address space). */
int  elf_build_user_stack(elf_context_t* ctx, int argc, const char** argv,
                          const char** envp, uint64_t* rsp_out);

#endif