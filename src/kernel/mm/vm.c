#include "vm.h"
#include "mm.h"

/*
 * vm.c — x86-64 per-task page table management.
 *
 * Design: every user task owns an independent 4-level page table (its own
 * CR3). The page table keeps the kernel identity mapping (0..2GB supervisor,
 * plus 3..4GB LAPIC) so the kernel keeps running with the user CR3 loaded.
 * User program segments are mapped at low canonical addresses (U/S=1) onto
 * fresh physical pages allocated from the Ω bitmap allocator (pmalloc).
 *
 * The kernel's identity map uses 1GB/2MB huge pages. vm_map_page() lazily
 * splits those huge pages down to 4KB where a user mapping is needed,
 * preserving identity supervisor mappings for every other page in the range.
 */

/* Address of the physical page stored in a page-table entry. */
#define P_ADDR(e)  ((uint64_t)(e) & PTE_ADDR_MSK)

/* 4-level index extraction (48-bit canonical address). */
#define IDX4(v)    ((int)(((v) >> 39) & 0x1FF))
#define IDX3(v)    ((int)(((v) >> 30) & 0x1FF))
#define IDX2(v)    ((int)(((v) >> 21) & 0x1FF))
#define IDX1(v)    ((int)(((v) >> 12) & 0x1FF))

/* Build a 4KB page-table entry from VM_* flags. */
static inline uint64_t pte_flags(uint64_t flags)
{
    uint64_t p = PTE_PRESENT;
    if (flags & VM_WRITE) p |= PTE_WRITE;
    if (flags & VM_USER)  p |= PTE_USER;
    if (!(flags & VM_EXEC)) p |= PTE_NX;
    return p;
}

/* Zero a memory range (kernel uses identity mapping: physical == virtual). */
static void vm_zero(void* p, uint64_t n)
{
    uint8_t* b = (uint8_t*)p;
    for (uint64_t i = 0; i < n; i++) b[i] = 0;
}

/* Allocate and zero a fresh page-table page (512 entries). */
static uint64_t* alloc_table(void)
{
    void* p = pmalloc();
    if (!p) return (uint64_t*)0;
    vm_zero(p, PAGE_SIZE);
    return (uint64_t*)p;
}

vm_context_t* vm_create(void)
{
    uint64_t* pml4 = alloc_table();
    if (!pml4) return (vm_context_t*)0;

    uint64_t* pdpt0 = alloc_table();
    if (!pdpt0) return (vm_context_t*)0;

    /* Kernel identity map (0..2GB as 1GB huge pages) + LAPIC (3..4GB).
     *
     * The low-half identity map is supervisor-only (U/S=0), so user code
     * cannot read or write the kernel image.  User pages (ELF segments,
     * user stack, mmap regions) are mapped later via vm_map_page(), which
     * lazily splits the identity huge pages down to 4KB and sets U/S=1 only
     * on the target leaf; intermediate levels carry U/S=1 so those user
     * leaves stay reachable, while all sibling pages remain U/S=0
     * (supervisor).  The LAPIC region (3..4GB) stays supervisor. */
    pml4[0] = (uint64_t)pdpt0 | PTE_PRESENT | PTE_WRITE;
    pdpt0[0] = 0x00000000ULL | PTE_PRESENT | PTE_WRITE | PTE_HUGE;
    pdpt0[1] = 0x40000000ULL | PTE_PRESENT | PTE_WRITE | PTE_HUGE;
    pdpt0[3] = 0xC0000000ULL | PTE_PRESENT | PTE_WRITE | PTE_HUGE | PTE_PCD;

    /* The context struct itself lives in a physical page (identity-mapped). */
    vm_context_t* ctx = (vm_context_t*)pmalloc();
    if (!ctx) return (vm_context_t*)0;

    ctx->pml4_phys = (uint64_t)pml4;
    ctx->brk = 0;

    /* W7 (Task 2.5): 映射内核信号 restorer 桩到固定用户地址
     * 0x600000000000（signal.c 的 sig_stub_page：mov $15,%eax; syscall）。
     * 注意不能落在 512GB 用户栈区（0x7FFC000000-0x8000000000）内，
     * 否则被 vm_map 的栈映射覆盖。 */
    {
        extern uint8_t* sig_stub_page;   /* defined in syscall/signal.c */
        if (sig_stub_page) {
            vm_map_page(ctx, 0x600000000000ULL, (uint64_t)sig_stub_page,
                        VM_USER | VM_READ | VM_WRITE | VM_EXEC);
        }
    }
    return ctx;
}

void vm_destroy(vm_context_t* ctx)
{
    /* W6: page tracking is not yet implemented; the task's pages are not
     * reclaimed individually. Freeing the context is intentionally deferred
     * until fork/exit provide proper address-space teardown. */
    (void)ctx;
}

/* Byte copy (identity-mapped kernel addresses: physical == virtual).
 * W7 (Task 4.8): ≥64B 走 `rep movsb`（QEMU TCG 对 rep movsb 有块复制
 * 优化，比逐字节 C 循环快一个量级）；小块保留普通循环避免 rep 开销。 */
static void vm_copy(void* dst, const void* src, uint64_t n)
{
    if (n >= 64) {
        __asm__ __volatile__("rep movsb"
                             : "+D"(dst), "+S"(src), "+c"(n)
                             :
                             : "memory");
    } else {
        uint8_t* d = (uint8_t*)dst;
        const uint8_t* s = (const uint8_t*)src;
        for (uint64_t i = 0; i < n; i++) d[i] = s[i];
    }
}

/* Translate a virtual address in `ctx`'s address space to its physical address
 * (identity-mapped, so usable directly by the kernel). Returns 0 if the page
 * is not present. */
static uint64_t vm_translate(vm_context_t* ctx, uint64_t vaddr)
{
    uint64_t* pml4 = (uint64_t*)ctx->pml4_phys;
    int i4 = IDX4(vaddr), i3 = IDX3(vaddr), i2 = IDX2(vaddr), i1 = IDX1(vaddr);

    if (!(pml4[i4] & PTE_PRESENT)) return 0;
    uint64_t* pdpt = (uint64_t*)P_ADDR(pml4[i4]);
    if (!(pdpt[i3] & PTE_PRESENT)) return 0;
    if (pdpt[i3] & PTE_HUGE)
        return P_ADDR(pdpt[i3]) + (vaddr & 0x3FFFFFFFULL);

    uint64_t* pd = (uint64_t*)P_ADDR(pdpt[i3]);
    if (!(pd[i2] & PTE_PRESENT)) return 0;
    if (pd[i2] & PTE_HUGE)
        return P_ADDR(pd[i2]) + (vaddr & 0x1FFFFFULL);

    uint64_t* pt = (uint64_t*)P_ADDR(pd[i2]);
    if (!(pt[i1] & PTE_PRESENT)) return 0;
    return P_ADDR(pt[i1]) + (vaddr & 0xFFFULL);
}

int vm_copy_to_user(vm_context_t* ctx, uint64_t vaddr, const void* src,
                    uint64_t len)
{
    if (!ctx) return -1;

    const uint8_t* s = (const uint8_t*)src;
    for (uint64_t off = 0; off < len; ) {
        uint64_t va = vaddr + off;
        uint64_t phys = vm_translate(ctx, va);
        if (!phys) return -1;

        uint64_t chunk = PAGE_SIZE - (va & 0xFFFULL);
        if (chunk > len - off) chunk = len - off;

        vm_copy((void*)phys, s + off, chunk);
        off += chunk;
    }
    return 0;
}

int vm_copy_from_user(vm_context_t* ctx, void* dst, uint64_t vaddr,
                      uint64_t len)
{
    if (!ctx) return -1;

    uint8_t* d = (uint8_t*)dst;
    for (uint64_t off = 0; off < len; ) {
        uint64_t va = vaddr + off;
        uint64_t phys = vm_translate(ctx, va);
        if (!phys) return -1;

        uint64_t chunk = PAGE_SIZE - (va & 0xFFFULL);
        if (chunk > len - off) chunk = len - off;

        vm_copy(d + off, (const void*)phys, chunk);
        off += chunk;
    }
    return 0;
}

/* Convert a leaf PTE flag word into VM_* flags for vm_map_page. */
static uint64_t vm_flags_from_pte(uint64_t pte)
{
    uint64_t f = VM_USER | VM_READ;
    if (pte & PTE_WRITE) f |= VM_WRITE;
    if (!(pte & PTE_NX)) f |= VM_EXEC;
    return f;
}

vm_context_t* vm_fork(vm_context_t* parent)
{
    if (!parent) return (vm_context_t*)0;

    vm_context_t* child = vm_create();
    if (!child) return (vm_context_t*)0;

    uint64_t* pp4 = (uint64_t*)parent->pml4_phys;
    for (int i4 = 0; i4 < 512; i4++) {
        uint64_t e4 = pp4[i4];
        if (!(e4 & PTE_PRESENT) || (e4 & PTE_HUGE)) continue;
        uint64_t* ppdpt = (uint64_t*)P_ADDR(e4);

        for (int i3 = 0; i3 < 512; i3++) {
            uint64_t e3 = ppdpt[i3];
            if (!(e3 & PTE_PRESENT) || (e3 & PTE_HUGE)) continue;
            uint64_t* ppd = (uint64_t*)P_ADDR(e3);

            for (int i2 = 0; i2 < 512; i2++) {
                uint64_t e2 = ppd[i2];
                if (!(e2 & PTE_PRESENT) || (e2 & PTE_HUGE)) continue;
                uint64_t* ppt = (uint64_t*)P_ADDR(e2);

                for (int i1 = 0; i1 < 512; i1++) {
                    uint64_t e1 = ppt[i1];
                    if (!(e1 & PTE_PRESENT)) continue;
                    if (!(e1 & PTE_USER)) continue;  /* shared kernel mapping */

                    uint64_t vaddr = ((uint64_t)i4 << 39) | ((uint64_t)i3 << 30) |
                                     ((uint64_t)i2 << 21) | ((uint64_t)i1 << 12);
                    void* np = vm_alloc_page();
                    if (!np) return (vm_context_t*)0;
                    vm_copy(np, (void*)P_ADDR(e1), PAGE_SIZE);
                    if (vm_map_page(child, vaddr, (uint64_t)np,
                                    vm_flags_from_pte(e1)) < 0)
                        return (vm_context_t*)0;
                }
            }
        }
    }

    child->brk = parent->brk;
    return child;
}

int vm_map_page(vm_context_t* ctx, uint64_t vaddr, uint64_t phys, uint64_t flags)
{
    if (!ctx) return -1;
    if (vaddr & 0xFFFULL) return -1;   /* must be 4KB aligned */

    uint64_t* pml4 = (uint64_t*)ctx->pml4_phys;
    int i4 = IDX4(vaddr);
    int i3 = IDX3(vaddr);
    int i2 = IDX2(vaddr);
    int i1 = IDX1(vaddr);

    /* Level 4: PML4 -> PDPT.  Intermediate entries get U/S=1 so that user
     * leaf pages are reachable (user access requires U/S=1 at every level). */
    if (!(pml4[i4] & PTE_PRESENT)) {
        uint64_t* t = alloc_table();
        if (!t) return -1;
        pml4[i4] = (uint64_t)t | PTE_PRESENT | PTE_WRITE | PTE_USER;
    } else {
        pml4[i4] |= PTE_USER;
    }
    uint64_t* pdpt = (uint64_t*)P_ADDR(pml4[i4]);

    /* Level 3: PDPT -> PD (split a 1GB huge page if present, or allocate a
     * fresh PD if the slot is empty — user mappings can land in any slot). */
    if (pdpt[i3] & PTE_HUGE) {
        uint64_t* pd = alloc_table();
        if (!pd) return -1;
        uint64_t base = P_ADDR(pdpt[i3]);
        for (int j = 0; j < 512; j++) {
            /* Sibling 2MB pages stay supervisor (U/S=0); only the target
             * 4KB leaf gets U/S=1 below, via pte_flags(flags). */
            pd[j] = (base + ((uint64_t)j << 21))
                   | PTE_PRESENT | PTE_WRITE | PTE_HUGE;
        }
        pdpt[i3] = (uint64_t)pd | PTE_PRESENT | PTE_WRITE | PTE_USER;
    } else if (!(pdpt[i3] & PTE_PRESENT)) {
        uint64_t* pd = alloc_table();
        if (!pd) return -1;
        pdpt[i3] = (uint64_t)pd | PTE_PRESENT | PTE_WRITE | PTE_USER;
    } else {
        pdpt[i3] |= PTE_USER;
    }
    uint64_t* pd = (uint64_t*)P_ADDR(pdpt[i3]);

    /* Level 2: PD -> PT (split a 2MB huge page if present, or allocate a
     * fresh PT if the slot is empty). */
    if (pd[i2] & PTE_HUGE) {
        uint64_t* pt = alloc_table();
        if (!pt) return -1;
        uint64_t base = P_ADDR(pd[i2]);
        for (int j = 0; j < 512; j++) {
            /* Sibling 4KB pages stay supervisor (U/S=0); only the target
             * leaf is set U/S=1 below, via pte_flags(flags). */
            pt[j] = (base + ((uint64_t)j << 12))
                    | PTE_PRESENT | PTE_WRITE;
        }
        pd[i2] = (uint64_t)pt | PTE_PRESENT | PTE_WRITE | PTE_USER;
    } else if (!(pd[i2] & PTE_PRESENT)) {
        uint64_t* pt = alloc_table();
        if (!pt) return -1;
        pd[i2] = (uint64_t)pt | PTE_PRESENT | PTE_WRITE | PTE_USER;
    } else {
        pd[i2] |= PTE_USER;
    }
    uint64_t* pt = (uint64_t*)P_ADDR(pd[i2]);

    /* Level 1: PT -> 4KB physical page. */
    pt[i1] = (phys & PTE_ADDR_MSK) | pte_flags(flags);
    return 0;
}

int vm_map(vm_context_t* ctx, uint64_t vaddr, uint64_t size, uint64_t flags)
{
    if (!ctx) return -1;
    if (size == 0) return 0;

    uint64_t start = vaddr & ~0xFFFULL;
    uint64_t end   = (vaddr + size + 0xFFF) & ~0xFFFULL;

    for (uint64_t a = start; a < end; a += PAGE_SIZE) {
        void* page = vm_alloc_page();
        if (!page) return -1;
        if (vm_map_page(ctx, a, (uint64_t)page, flags) < 0) return -1;
    }
    return 0;
}

/* Returns 1 if any page in the range is mapped (user or supervisor).  Each
 * page is probed via the full 4-level walk; huge-page identity leaves count as
 * "present", which is exactly what the mmap allocator needs to avoid placing a
 * new mapping on top of live pages. */
int vm_range_present(vm_context_t* ctx, uint64_t vaddr, uint64_t npages)
{
    if (!ctx) return 1;
    for (uint64_t i = 0; i < npages; i++) {
        if (vm_translate(ctx, vaddr + i * PAGE_SIZE) != 0) return 1;
    }
    return 0;
}

/* Unmap user pages in a 4KB-aligned range: clear the leaf PTE and return the
 * physical page to the Ω allocator.  Only entries with U/S=1 are touched, so
 * the kernel identity map (supervisor, possibly huge) always survives.  A
 * stale TLB entry on this core is flushed with invlpg after each clear. */
int vm_unmap(vm_context_t* ctx, uint64_t vaddr, uint64_t len)
{
    if (!ctx) return -1;
    if (vaddr & 0xFFFULL) return -1;

    uint64_t end = vaddr + len;
    for (uint64_t a = vaddr; a < end; a += PAGE_SIZE) {
        int i4 = IDX4(a), i3 = IDX3(a), i2 = IDX2(a), i1 = IDX1(a);
        uint64_t* pml4 = (uint64_t*)ctx->pml4_phys;
        if (!(pml4[i4] & PTE_PRESENT)) continue;
        uint64_t* pdpt = (uint64_t*)P_ADDR(pml4[i4]);
        if (!(pdpt[i3] & PTE_PRESENT) || (pdpt[i3] & PTE_HUGE)) continue;
        uint64_t* pd = (uint64_t*)P_ADDR(pdpt[i3]);
        if (!(pd[i2] & PTE_PRESENT) || (pd[i2] & PTE_HUGE)) continue;
        uint64_t* pt = (uint64_t*)P_ADDR(pd[i2]);
        if (!(pt[i1] & PTE_PRESENT)) continue;
        if (!(pt[i1] & PTE_USER) || (pt[i1] & PTE_HUGE)) continue;
        pfree((void*)P_ADDR(pt[i1]));
        pt[i1] = 0;
        __asm__ __volatile__("invlpg (%0)" :: "r"(a) : "memory");
    }
    return 0;
}

void vm_switch(vm_context_t* ctx)
{
    if (!ctx) return;
    __asm__ volatile("movq %0, %%cr3" :: "r"(ctx->pml4_phys) : "memory");
}

uint64_t vm_current_cr3(void)
{
    uint64_t cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
    return cr3;
}

void* vm_alloc_page(void)
{
    void* p = pmalloc();
    if (p) vm_zero(p, PAGE_SIZE);
    return p;
}