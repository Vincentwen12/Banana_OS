#ifndef VM_H
#define VM_H

#include "axion.h"

/* Page table entry bits */
#define PTE_PRESENT  0x1
#define PTE_WRITE    0x2
#define PTE_USER     0x4
#define PTE_PWT      0x8
#define PTE_PCD      0x10
#define PTE_ACCESSED 0x20
#define PTE_DIRTY    0x40
#define PTE_HUGE     0x80
#define PTE_NX       (1ULL << 63)
#define PTE_ADDR_MSK 0x000FFFFFFFFFF000ULL

/* vm_map flag bits */
#define VM_READ   0x1
#define VM_WRITE  0x2
#define VM_EXEC   0x4
#define VM_USER   0x8

/* Per-task virtual address space context */
typedef struct vm_context {
    uint64_t pml4_phys;   /* physical address of PML4 (identity-mapped kernel) */
    uint64_t brk;         /* current program break */
} vm_context_t;

/* Create a fresh user address space that also identity-maps the kernel
 * (0..2GB, supervisor-usable) so the kernel keeps running after cr3 switch. */
vm_context_t* vm_create(void);
void   vm_destroy(vm_context_t* ctx);

/* Clone `parent`'s user address space (deep copy of every U/S=1 4KB page) into
 * a new context sharing the same kernel identity map. Returns NULL on OOM. */
vm_context_t* vm_fork(vm_context_t* parent);

/* Allocate anonymous pages and map [vaddr, vaddr+size) with the given flags. */
int    vm_map(vm_context_t* ctx, uint64_t vaddr, uint64_t size, uint64_t flags);

/* Map a single 4KB page: virtual vaddr -> physical phys. */
int    vm_map_page(vm_context_t* ctx, uint64_t vaddr, uint64_t phys, uint64_t flags);

/* Returns 1 if any page in [vaddr, vaddr + npages*4096) is currently mapped
 * (user or supervisor) in `ctx`, 0 if the whole range is free.  Used by the
 * mmap allocator to avoid silently double-mapping live user pages (a clobber
 * that shows up as glibc "corrupted size vs. prev_size"). */
int    vm_range_present(vm_context_t* ctx, uint64_t vaddr, uint64_t npages);

/* Unmap user pages in [vaddr, vaddr + len): pfree() each backing physical page
 * and clear the leaf PTE.  Supervisor / huge / absent entries are left intact
 * (the kernel identity map must survive).  vaddr and len must be 4KB aligned.
 * Returns 0. */
int    vm_unmap(vm_context_t* ctx, uint64_t vaddr, uint64_t len);

/* Switch the current core's address space. */
void   vm_switch(vm_context_t* ctx);

/* Returns the current CR3. */
uint64_t vm_current_cr3(void);

/* Anonymous page from the physical allocator (zeroed). */
void*  vm_alloc_page(void);

/* Copy `len` bytes from the kernel buffer `src` into `ctx`'s user address space
 * at virtual address `vaddr` (byte-granular, crosses page boundaries). Returns
 * 0 on success, -1 if any destination page is not mapped. */
int    vm_copy_to_user(vm_context_t* ctx, uint64_t vaddr, const void* src,
                       uint64_t len);

/* Copy `len` bytes from `ctx`'s user address space at virtual address `vaddr`
 * into the kernel buffer `dst`. Returns 0 on success, -1 if any source page is
 * not mapped. */
int    vm_copy_from_user(vm_context_t* ctx, void* dst, uint64_t vaddr,
                         uint64_t len);

#endif /* VM_H */