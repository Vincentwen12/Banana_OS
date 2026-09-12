/* shm.c — W6 共享内存最小实现（Task 3.2）。
 * 由子代理 B 填充：shmget / shmat / shmctl / shmdt 及 8 槽位表。
 *
 * 设计要点：
 *  - 8 槽位全局表 {key,size,phys,ref,used} + 自旋锁保护。
 *  - 段物理页直接由 Ω pmalloc 分配（identity-mapped，内核/用户均可用），
 *    size 向上取整 4KB，多页按连续物理页使用（与 mmap_user 同一假设）。
 *  - shmat 用 vm_map_page 把段物理页映射进 current_task->mm_context；
 *    fork() 深拷贝页表后，子进程再次 shmat 会以共享物理页覆盖其拷贝映射，
 *    从而实现父子真正共享同一物理页。
 *  - vm 层没有 unmap 接口（sys_munmap 也是空实现），shmdt 在本文件内自行
 *    走 4 级页表清 PTE（不清物理页——页归属槽位，仅 IPC_RMID 释放）。
 */

#include "axion.h"
#include "mm.h"
#include "mm/vm.h"
#include "sched.h"
#include "syscall.h"
#include "vga.h"

/* ---- 8 槽位共享内存表 ---- */
#define SHM_MAX_SEGS     8
#define SHM_ATTACH_BASE  0x6000000000ULL  /* 默认映射基址：id * 0x1000 */
#define SHM_RDONLY       0x1000           /* 010000 octal (Linux SHM_RDONLY) */
#define SHM_PTE_ADDR_MSK 0x000FFFFFFFFFF000ULL

typedef struct {
    uint64_t key;    /* IPC key（key=0 表示 IPC_PRIVATE；占用以 used 标记） */
    uint64_t size;   /* 段大小（已向上取整为 PAGE_SIZE 的整数倍） */
    uint64_t phys;   /* 段基址物理地址（identity-mapped） */
    uint64_t ref;    /* attach 次数 */
    int      used;   /* 槽位占用标记 */
} shm_seg_t;

static shm_seg_t shm_segs[SHM_MAX_SEGS];
static volatile int shm_table_lock = 0;

static inline void shm_lock(void)
{
    while (__sync_lock_test_and_set(&shm_table_lock, 1))
        __asm__ volatile("pause");
}

static inline void shm_unlock(void)
{
    __sync_lock_release(&shm_table_lock);
}

static void shm_zero(void* p, uint64_t n)
{
    uint8_t* b = (uint8_t*)p;
    for (uint64_t i = 0; i < n; i++) b[i] = 0;
}

/* 4 级页表叶子翻译：返回 vaddr 所在 4KB 页的物理地址；路径上出现 huge
 * 页或缺失返回 0（shm 映射都由 vm_map_page 建 4KB 叶子，不会被 huge 页覆盖）。 */
static uint64_t shm_translate(vm_context_t* mm, uint64_t vaddr)
{
    uint64_t* pml4 = (uint64_t*)mm->pml4_phys;
    int i4 = (int)((vaddr >> 39) & 0x1FF);
    int i3 = (int)((vaddr >> 30) & 0x1FF);
    int i2 = (int)((vaddr >> 21) & 0x1FF);
    int i1 = (int)((vaddr >> 12) & 0x1FF);

    if (!(pml4[i4] & 1)) return 0;
    uint64_t* pdpt = (uint64_t*)(pml4[i4] & SHM_PTE_ADDR_MSK);
    if (!(pdpt[i3] & 1) || (pdpt[i3] & 0x80)) return 0;
    uint64_t* pd = (uint64_t*)(pdpt[i3] & SHM_PTE_ADDR_MSK);
    if (!(pd[i2] & 1) || (pd[i2] & 0x80)) return 0;
    uint64_t* pt = (uint64_t*)(pd[i2] & SHM_PTE_ADDR_MSK);
    if (!(pt[i1] & 1)) return 0;
    return pt[i1] & SHM_PTE_ADDR_MSK;
}

/* 清除一个 4KB 叶子 PTE（shmdt 用；vm 层无 unmap 接口，sys_munmap 亦为空）。
 * 只解映射、不释放物理页——页归属 shm 槽位，仅 shmctl(IPC_RMID) 释放。 */
static int shm_unmap_page(vm_context_t* mm, uint64_t vaddr)
{
    uint64_t* pml4 = (uint64_t*)mm->pml4_phys;
    int i4 = (int)((vaddr >> 39) & 0x1FF);
    int i3 = (int)((vaddr >> 30) & 0x1FF);
    int i2 = (int)((vaddr >> 21) & 0x1FF);
    int i1 = (int)((vaddr >> 12) & 0x1FF);

    if (!(pml4[i4] & 1)) return -1;
    uint64_t* pdpt = (uint64_t*)(pml4[i4] & SHM_PTE_ADDR_MSK);
    if (!(pdpt[i3] & 1) || (pdpt[i3] & 0x80)) return -1;
    uint64_t* pd = (uint64_t*)(pdpt[i3] & SHM_PTE_ADDR_MSK);
    if (!(pd[i2] & 1) || (pd[i2] & 0x80)) return -1;
    uint64_t* pt = (uint64_t*)(pd[i2] & SHM_PTE_ADDR_MSK);
    if (!(pt[i1] & 1)) return -1;

    pt[i1] = 0;
    __asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
    return 0;
}

/* 29: shmget(key, size, flags) — 同 key 段已存在则返回其 id，否则新建。
 * 最小实现：不强制 IPC_CREAT（测试以 flags=0 首次调用也需创建成功）。 */
uint64_t sys_shmget(uint64_t key, uint64_t size, uint64_t flags,
                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)flags; (void)a4; (void)a5; (void)a6;
    if (size == 0) return (uint64_t)-1;

    shm_lock();

    /* key != 0：查找已存在段（key=0 = IPC_PRIVATE，总是新建） */
    if (key != 0) {
        for (int i = 0; i < SHM_MAX_SEGS; i++) {
            if (shm_segs[i].used && shm_segs[i].key == key) {
                shm_unlock();
                return (uint64_t)i;
            }
        }
    }

    /* 找空槽 */
    int slot = -1;
    for (int i = 0; i < SHM_MAX_SEGS; i++) {
        if (!shm_segs[i].used) { slot = i; break; }
    }
    if (slot < 0) { shm_unlock(); return (uint64_t)-1; }

    /* 分配连续物理页（pmalloc 首适配，与 mmap_user 同假设；OOM 时回滚） */
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    void* first = (void*)0;
    for (uint64_t i = 0; i < pages; i++) {
        void* p = pmalloc();
        if (!p) {
            for (uint64_t j = 0; j < i; j++)
                pfree((void*)((uint64_t)first + j * PAGE_SIZE));
            shm_unlock();
            return (uint64_t)-1;   /* ENOMEM */
        }
        if (i == 0) first = p;
    }
    for (uint64_t i = 0; i < pages; i++)
        shm_zero((void*)((uint64_t)first + i * PAGE_SIZE), PAGE_SIZE);

    shm_segs[slot].key  = key;
    shm_segs[slot].size = pages * PAGE_SIZE;
    shm_segs[slot].phys = (uint64_t)first;
    shm_segs[slot].ref  = 0;
    shm_segs[slot].used = 1;
    shm_unlock();

    return (uint64_t)slot;
}

/* 30: shmat(id, addr, flags) — 把段映射进当前进程地址空间，返回用户 vaddr。
 * addr==0 时使用默认基址 0x6000000000 + id*0x1000。 */
uint64_t sys_shmat(uint64_t id, uint64_t addr, uint64_t flags,
                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (id >= SHM_MAX_SEGS) return (uint64_t)-1;

    vm_context_t* mm = current_task ? current_task->mm_context : 0;
    if (!mm) return (uint64_t)-1;

    shm_lock();
    shm_seg_t* s = &shm_segs[id];
    if (!s->used) { shm_unlock(); return (uint64_t)-1; }

    uint64_t vaddr = (addr == 0) ? (SHM_ATTACH_BASE + id * PAGE_SIZE)
                                 : (addr & ~0xFFFULL);
    uint64_t f = VM_USER | VM_READ | VM_WRITE;
    if (flags & SHM_RDONLY) f = VM_USER | VM_READ;

    uint64_t pages = s->size / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        if (vm_map_page(mm, vaddr + i * PAGE_SIZE,
                        s->phys + i * PAGE_SIZE, f) < 0) {
            shm_unlock();
            return (uint64_t)-1;
        }
    }
    s->ref++;
    shm_unlock();

    return vaddr;
}

/* 67: shmdt(addr) — 解除映射。通过翻译 addr 找到所属段，再清除默认基址
 * 上的整段 PTE。仅支持默认基址（addr==0 那次 shmat）的解除。 */
uint64_t sys_shmdt(uint64_t addr, uint64_t a2, uint64_t a3,
                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    vm_context_t* mm = current_task ? current_task->mm_context : 0;
    if (!mm) return (uint64_t)-1;

    addr &= ~0xFFFULL;
    uint64_t phys = shm_translate(mm, addr);
    if (!phys) return (uint64_t)-1;   /* 该地址没有映射 */

    shm_lock();
    for (int i = 0; i < SHM_MAX_SEGS; i++) {
        shm_seg_t* s = &shm_segs[i];
        if (!s->used) continue;
        if (phys >= s->phys && phys < s->phys + s->size) {
            uint64_t base = SHM_ATTACH_BASE + (uint64_t)i * PAGE_SIZE;
            uint64_t pages = s->size / PAGE_SIZE;
            for (uint64_t p = 0; p < pages; p++)
                shm_unmap_page(mm, base + p * PAGE_SIZE);
            if (s->ref > 0) s->ref--;
            shm_unlock();
            return 0;
        }
    }
    shm_unlock();
    return (uint64_t)-1;
}

/* 31: shmctl(id, cmd, buf) — 仅支持 IPC_RMID(0)：释放物理页并清空槽位。 */
uint64_t sys_shmctl(uint64_t id, uint64_t cmd, uint64_t buf,
                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)buf; (void)a4; (void)a5; (void)a6;
    if (id >= SHM_MAX_SEGS) return (uint64_t)-1;

    shm_lock();
    shm_seg_t* s = &shm_segs[id];
    if (!s->used) { shm_unlock(); return (uint64_t)-1; }

    if (cmd == 0) {  /* IPC_RMID */
        uint64_t pages = s->size / PAGE_SIZE;
        for (uint64_t i = 0; i < pages; i++)
            pfree((void*)(s->phys + i * PAGE_SIZE));
        s->key = 0; s->size = 0; s->phys = 0; s->ref = 0; s->used = 0;
        shm_unlock();
        return 0;
    }
    shm_unlock();
    return (uint64_t)-1;   /* 其它命令不支持 */
}
