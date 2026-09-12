#ifndef ELF_INTERP_H
#define ELF_INTERP_H

#include "axion.h"

/* Forward declaration (mm/vm.h defines the full struct). */
struct vm_context;
typedef struct vm_context vm_context_t;

/* W6: dynamic linker (PT_INTERP) support. */
const char* elf_get_interp_path(void);

/* Linker load bias (AT_BASE). Meaningful only after elf_load_interp succeeds. */
uint64_t    elf_get_interp_base(void);

/* Non-zero if a usable dynamic linker exists on the root filesystem. */
int  elf_interp_available(void);

/* Load the dynamic linker at `path` into the main program's address space `vm`,
 * returning its entry point via *entry_out. Returns 0 on success, -1 on error
 * (missing file / not ELF / OOM). */
int  elf_load_interp(vm_context_t* vm, const char* path, uint64_t* entry_out);

#endif