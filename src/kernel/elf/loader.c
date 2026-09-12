#include "loader.h"
#include "interp.h"
#include "vga.h"
#include "mm.h"
#include "mm/vm.h"
#include "port.h"
#include "fs/vfs.h"
#include "fs/devfs.h"
#include "fs/tmpfs.h"
#include "sched.h"

/* Print string */
static void print_str(const char* s)
{
    while (*s) vga_putc(*s++);
}

/* Copy bytes (kernel string.c exports memcpy but without a shared header). */
static void copy_bytes(void* dst, const void* src, uint64_t n)
{
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

/* Map a single PT_LOAD segment into `vm` at `p_vaddr + load_bias`, copying the
 * file bytes that fall within each page (handles a non-page-aligned file
 * offset). Returns 0 on success, -1 on OOM. */
static int map_load_segment(const uint8_t* data, const elf64_phdr_t* phdr,
                            vm_context_t* vm, uint64_t load_bias)
{
    uint64_t vaddr  = phdr->p_vaddr + load_bias;
    uint64_t offset = phdr->p_offset;
    uint64_t filesz = phdr->p_filesz;
    uint64_t memsz  = phdr->p_memsz;

    if (memsz == 0) return 0;

    uint64_t flags = VM_USER;
    if (phdr->p_flags & PF_X) flags |= VM_EXEC;
    if (phdr->p_flags & PF_W) flags |= VM_WRITE;
    if (phdr->p_flags & PF_R) flags |= VM_READ;

    uint64_t seg_start = vaddr & ~0xFFFULL;
    uint64_t seg_end   = (vaddr + memsz + 0xFFFULL) & ~0xFFFULL;

    for (uint64_t a = seg_start; a < seg_end; a += PAGE_SIZE) {
        uint8_t* page = (uint8_t*)vm_alloc_page();
        if (!page) return -1;

        uint64_t lo = (a > vaddr) ? a : vaddr;
        uint64_t hi = (a + PAGE_SIZE < vaddr + filesz)
                            ? (a + PAGE_SIZE) : (vaddr + filesz);
        if (lo < hi) {
            copy_bytes(page + (lo - a),
                       data + offset + (lo - vaddr),
                       hi - lo);
        }

        if (vm_map_page(vm, a, (uint64_t)page, flags) < 0)
            return -1;
    }
    return 0;
}

/*
 * elf_load_from_mem — load an ELF image (already in a kernel buffer) into a
 * fresh per-task address space.
 *
 * W6: PT_LOAD segments are mapped via vm_map_page onto fresh physical pages at
 * their user virtual addresses (U/S=1), handling non-page-aligned file offsets.
 * PT_INTERP records the dynamic linker path, PT_GNU_STACK is accepted
 * (non-exec stack), a 4MB user stack is allocated, and entry/stack_top are set.
 */
int elf_load_from_mem(void* data, uint64_t size, elf_context_t* ctx)
{
    (void)size;
    if (!data || !ctx) return -1;

    elf64_ehdr_t* ehdr = (elf64_ehdr_t*)data;

    /* 1. Validate ELF magic */
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        print_str("Error: Not a valid ELF file\n");
        return -1;
    }

    /* 2. Validate ELF class (64-bit) */
    if (ehdr->e_ident[4] != ELFCLASS64) {
        print_str("Error: Not a 64-bit ELF\n");
        return -1;
    }

    /* 3. Validate endianness (little-endian) */
    if (ehdr->e_ident[5] != ELFDATA2LSB) {
        print_str("Error: Not little-endian\n");
        return -1;
    }

    /* 4. Validate type (executable or PIE) */
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        print_str("Error: Not an executable\n");
        return -1;
    }

    /* 5. Create the task's own address space. */
    vm_context_t* vm = vm_create();
    if (!vm) {
        print_str("Error: Out of memory for address space\n");
        return -1;
    }

    ctx->vmctx = vm;
    /* 用户程序装载基址必须高于内核全部物理区，否则用户映射会遮蔽内核的
     * 「线性=物理」恒等访问：内核写恒等线性地址时实际落到用户页，破坏内核
     * 内存（run9 fork 覆写 bash 文本页、内核栈被用户堆遮蔽即此）。
     * 内核物理区：热区 [0x800000,0x40800000)、温区 [0x40800000,0x60800000)、
     * 回旋镖 [0x60800000,0x70800000)，另有 LAPIC [0xC0000000,0x100000000)。
     * 取 256GB：远高于全部内核物理区并避开 LAPIC，远低于 libc mmap 提示区
     * (~0x7f0000100000) 与 512GB 用户栈顶，给 brk 增长留足空间。 */
    ctx->base  = 0x4000000000ULL;
    ctx->brk_start = 0;
    ctx->brk_end   = 0;
    ctx->phdr_count = ehdr->e_phnum;
    ctx->phdr_entsize = ehdr->e_phentsize;
    ctx->phdr_addr  = 0;
    ctx->main_entry = 0;
    ctx->interp_path[0] = '\0';
    ctx->interp_base = 0;

    /* ET_DYN (PIE) programs are relocated by a load bias; ET_EXEC uses absolute
     * addresses. */
    uint64_t load_bias = (ehdr->e_type == ET_DYN) ? ctx->base : 0;
    ctx->main_entry = ehdr->e_entry + load_bias;

    uint64_t max_vaddr = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        elf64_phdr_t* phdr = (elf64_phdr_t*)((uint8_t*)data +
                              ehdr->e_phoff + i * ehdr->e_phentsize);

        switch (phdr->p_type) {
        case PT_LOAD: {
            if (map_load_segment((uint8_t*)data, phdr, vm, load_bias) < 0) {
                print_str("Error: Out of memory\n");
                return -1;
            }
            uint64_t end = phdr->p_vaddr + load_bias + phdr->p_memsz;
            if (end > max_vaddr) max_vaddr = end;
            break;
        }

        case PT_INTERP: {
            const char* ip = (const char*)data + phdr->p_offset;
            int n = 0;
            while (n < 63 && ip[n]) { ctx->interp_path[n] = ip[n]; n++; }
            ctx->interp_path[n] = '\0';
            break;
        }

        case PT_PHDR:
            ctx->phdr_addr = phdr->p_vaddr + load_bias;
            break;

        case PT_GNU_STACK:
            /* Default to a non-executable stack. Executable stacks are rare and
             * not required by dynamically-linked bash, so accept the header
             * without changing the stack mapping flags. */
            break;

        default:
            break;
        }
    }

    /* 7. Entry point. If the program needs a dynamic linker (PT_INTERP), load
     * it into this address space and dispatch through the linker entry. */
    ctx->entry = ehdr->e_entry + load_bias;
    if (ctx->interp_path[0] != '\0') {
        uint64_t interp_entry = 0;
        if (elf_load_interp(vm, ctx->interp_path, &interp_entry) != 0) {
            print_str("Error: Failed to load dynamic linker ");
            print_str(ctx->interp_path);
            print_str("\n");
            return -1;
        }
        ctx->entry = interp_entry;
        ctx->interp_base = elf_get_interp_base();
    }

    /* 8. Allocate a 4MB user stack just below USER_STACK_TOP. */
    if (vm_map(vm, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE,
               VM_READ | VM_WRITE | VM_USER) < 0) {
        print_str("Error: Out of memory for stack\n");
        return -1;
    }
    ctx->stack_top = USER_STACK_TOP;

    /* 9. Program break starts just past the last loaded segment. */
    uint64_t brk = (max_vaddr + 0xFFFULL) & ~0xFFFULL;
    if (brk == 0) brk = 0x600000;
    ctx->brk_start = brk;
    ctx->brk_end   = brk;
    vm->brk = brk;

    return 0;
}

/* Map an ELF image (already in a kernel buffer) into an EXISTING vm_context at
 * `base`. Used to load the dynamic linker / interpreter (ET_DYN) into the same
 * address space as the main program. Returns the interpreter entry via
 * *entry_out. */
int elf_map_program(void* data, uint64_t size, vm_context_t* vm,
                    uint64_t base, uint64_t* entry_out)
{
    (void)size;
    if (!data || !vm || !entry_out) return -1;

    elf64_ehdr_t* ehdr = (elf64_ehdr_t*)data;

    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F')
        return -1;
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
        return -1;

    uint64_t load_bias = (ehdr->e_type == ET_DYN) ? base : 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        elf64_phdr_t* phdr = (elf64_phdr_t*)((uint8_t*)data +
                              ehdr->e_phoff + i * ehdr->e_phentsize);
        if (phdr->p_type == PT_LOAD) {
            if (map_load_segment((uint8_t*)data, phdr, vm, load_bias) < 0)
                return -1;
        }
    }

    *entry_out = ehdr->e_entry + load_bias;
    return 0;
}

/* ---- User stack (W6: argc/argv/envp + auxv) ---- */

/* String length helper (kernel has no shared strlen). */
static uint64_t strlen_s(const char* s)
{
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

/* Push a little-endian uint64 down onto the user stack at *sp. */
static int push_u64(vm_context_t* vm, uint64_t* sp, uint64_t val)
{
    *sp -= 8;
    return vm_copy_to_user(vm, *sp, &val, 8);
}

/* Push a NUL-terminated string (plus its terminator) down onto the user stack,
 * returning its resulting user address via *addr_out. */
static int push_str(vm_context_t* vm, uint64_t* sp, const char* s,
                    uint64_t* addr_out)
{
    uint64_t len = strlen_s(s) + 1;
    *sp -= len;
    if (vm_copy_to_user(vm, *sp, s, len) < 0) return -1;
    *addr_out = *sp;
    return 0;
}

/* Auxiliary vector (auxv) tags used to bootstrap the dynamic linker. */
#define AT_NULL         0
#define AT_PHDR         3
#define AT_PHENT        4
#define AT_PHNUM        5
#define AT_PAGESZ       6
#define AT_BASE         7
#define AT_FLAGS        8
#define AT_ENTRY        9
#define AT_UID          11
#define AT_EUID         12
#define AT_GID          13
#define AT_EGID         14
#define AT_PLATFORM     15
#define AT_CLKTCK       17
#define AT_SECURE       23
#define AT_RANDOM       25
#define AT_EXECFN       31
#define AT_SYSINFO_EHDR 33

int elf_build_user_stack(elf_context_t* ctx, int argc, const char** argv,
                         const char** envp, uint64_t* rsp_out)
{
    if (!ctx || !ctx->vmctx || !rsp_out) return -1;
    if (argc < 0) argc = 0;

    vm_context_t* vm = ctx->vmctx;

    /* Fixed-size staging for user string addresses (kernel has no malloc). */
    #define MAX_UV_STR 64
    uint64_t argv_uv[MAX_UV_STR];
    uint64_t envp_uv[MAX_UV_STR];

    int envc = 0;
    while (envp && envp[envc]) envc++;
    if (argc > MAX_UV_STR) argc = MAX_UV_STR;
    if (envc > MAX_UV_STR) envc = MAX_UV_STR;

    uint64_t sp = USER_STACK_TOP;

    /* 1. Argument + environment strings at the top of the stack. */
    for (int i = 0; i < argc; i++) {
        if (push_str(vm, &sp, argv[i], &argv_uv[i]) < 0) return -1;
    }
    for (int i = 0; i < envc; i++) {
        if (push_str(vm, &sp, envp[i], &envp_uv[i]) < 0) return -1;
    }

    /* AT_PLATFORM string. */
    uint64_t platform_uv = 0;
    if (push_str(vm, &sp, "x86_64", &platform_uv) < 0) return -1;

    /* AT_RANDOM: 16 pseudo-random bytes (glibc stack guard / ASLR seed). */
    uint64_t rand_uv = 0;
    {
        uint8_t rnd[16];
        uint64_t t = rdtsc();
        for (int i = 0; i < 16; i++)
            rnd[i] = (uint8_t)((t >> ((i & 7) * 8)) ^ (0xA5 * i + 0x39));
        sp = (sp - 16) & ~0xFULL;   /* 16-byte align */
        if (vm_copy_to_user(vm, sp, rnd, 16) < 0) return -1;
        rand_uv = sp;
    }

    /* 3. Auxiliary vector. */
    enum { AUX_MAX = 24 };
    struct { uint64_t type, val; } auxv[AUX_MAX];
    int naux = 0;
    #define AUX(t, v) do { auxv[naux].type = (t); auxv[naux].val = (v); naux++; } while (0)

    AUX(AT_SYSINFO_EHDR, 0);
    AUX(AT_PHDR,  ctx->phdr_addr);
    AUX(AT_PHENT, ctx->phdr_entsize);
    AUX(AT_PHNUM, ctx->phdr_count);
    AUX(AT_PAGESZ, PAGE_SIZE);
    if (ctx->interp_base)
        AUX(AT_BASE, ctx->interp_base);
    AUX(AT_FLAGS, 0);
    AUX(AT_ENTRY, ctx->main_entry);
    /* W7: 用户凭据写入 auxv（execve 时用当前任务；内核 Shell run 时用
     * 默认用户 banana）。 */
    uint32_t aux_uid = (current_task && current_task->mm_context)
                           ? current_task->uid : g_default_uid;
    uint32_t aux_gid = (current_task && current_task->mm_context)
                           ? current_task->gid : g_default_gid;
    AUX(AT_UID, aux_uid);
    AUX(AT_EUID, aux_uid);
    AUX(AT_GID, aux_gid);
    AUX(AT_EGID, aux_gid);
    AUX(AT_PLATFORM, platform_uv);
    AUX(AT_CLKTCK, 100);
    AUX(AT_SECURE, 0);
    AUX(AT_RANDOM, rand_uv);
    AUX(AT_EXECFN, (argc > 0 && argv[0]) ? argv_uv[0] : 0);
    #undef AUX

    /* 16-byte align, then adjust so that pushing auxv(2*naux+2) +
     * envp(envc+1) + argv(argc+1) + argc(1) u64s leaves RSP 16-byte
     * aligned at the entry point (x86-64 ABI; ld.so uses SSE movaps on
     * the stack). The 8-byte gap (if any) sits between the strings and
     * the auxv, invisible to ld.so's argc/argv/envp/auxv walk. */
    sp &= ~0xFULL;
    uint64_t total_u64 = (uint64_t)(naux * 2 + 2) + (uint64_t)(envc + 1)
                       + (uint64_t)(argc + 1) + 1;
    if (total_u64 % 2 == 1)
        sp -= 8;

    /* Terminator first (highest address of the auxv block). */
    if (push_u64(vm, &sp, 0) < 0) return -1;   /* AT_NULL val */
    if (push_u64(vm, &sp, 0) < 0) return -1;   /* AT_NULL type */
    for (int i = naux - 1; i >= 0; i--) {
        if (push_u64(vm, &sp, auxv[i].val) < 0) return -1;
        if (push_u64(vm, &sp, auxv[i].type) < 0) return -1;
    }

    /* 4. envp pointer array (NULL terminator at the high end). */
    if (push_u64(vm, &sp, 0) < 0) return -1;
    for (int i = envc - 1; i >= 0; i--) {
        if (push_u64(vm, &sp, envp_uv[i]) < 0) return -1;
    }

    /* 5. argv pointer array (NULL terminator at the high end). */
    if (push_u64(vm, &sp, 0) < 0) return -1;
    for (int i = argc - 1; i >= 0; i--) {
        if (push_u64(vm, &sp, argv_uv[i]) < 0) return -1;
    }

    /* 6. argc at the bottom — this is the initial RSP.
     * 对齐已在 auxv 区起始处调整（见上文），此处直接 push。 */
    if (push_u64(vm, &sp, (uint64_t)argc) < 0) return -1;

    *rsp_out = sp;
    return 0;
}

int elf_load(const char* path, elf_context_t* ctx)
{
    if (!path || !ctx) return -1;

    /* Open via the real VFS (devfs, then EXT2 disk filesystem). */
    file_t* f = vfs_open(path, O_RDONLY);
    if (!f) {
        print_str("Error: File not found: ");
        print_str(path);
        print_str("\n");
        return -1;
    }

    /* Read entire file into memory */
    uint64_t fsize = f->size;
    if (fsize == 0 || fsize > (16 * 1024 * 1024)) {
        print_str("Error: File too large or empty\n");
        return -1;
    }

    /* Allocate buffer for file data — pages must be physically contiguous,
     * because vfs_read writes fsize bytes into one linear buffer.
     * W7: 512MB 多组镜像下大文件（如 coreutils ls）用 pmalloc 逐页分配可能
     * 不连续，改用 pmalloc_contig 一次性分配连续页。 */
    uint64_t num_pages = (fsize + 0xFFF) / 0x1000;
    void* buf = pmalloc_contig((int)num_pages);
    if (!buf) {
        print_str("Error: Out of memory (contig)\n");
        return -1;
    }

    /* Read file content */
    uint64_t bytes_read = vfs_read(f, buf, fsize);
    if (bytes_read != fsize) {
        print_str("Error: Failed to read file\n");
        return -1;
    }

    int result = elf_load_from_mem(buf, fsize, ctx);
    return result;
}

void elf_unload(elf_context_t* ctx)
{
    (void)ctx;
    /* W6: proper address-space teardown arrives with fork/exit. */
}