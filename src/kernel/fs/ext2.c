#include "ext2.h"
#include "dev/ata.h"
#include "mm.h"
#include "timer.h"
#include "vga.h"
#include "printk.h"
#include "sched.h"

/* ================= 挂载状态 ================= */
static int ext2_mounted = 0;
static ext2_superblock_t ext2_sb;
static uint32_t ext2_blk_size = 0;      /* 字节/块 */
static uint32_t ext2_ino_size = 128;
static uint32_t ext2_inodes_per_group = 0;
static uint32_t ext2_blocks_per_group = 0;
static uint8_t* ext2_blkbuf = NULL;     /* 块缓冲 (pmalloc) */
static uint8_t* ext2_indbuf = NULL;     /* 间接块缓冲 (pmalloc) */

/* ================= 小工具 ================= */
static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static int ext2_strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void ext2_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dst = (uint8_t*)d;
    const uint8_t* src = (const uint8_t*)s;
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
}

static void ext2_memset(void* d, int v, uint32_t n) {
    uint8_t* p = (uint8_t*)d;
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)v;
}

static int ext2_strlen(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

/* ================= 块读取 ================= */
/* 读取第 block 号块（从 slave 磁盘）到 dst，dst 至少 ext2_blk_size 字节 */
static int ext2_read_block(uint32_t block, uint8_t* dst) {
    uint32_t sectors_per_block = ext2_blk_size / 512;
    uint32_t lba = block * sectors_per_block;
    return ata_read_sectors_slave(lba, sectors_per_block, dst);
}

/* 写一个 1024 字节数据块（2 扇区）到 slave 磁盘 */
int ext2_write_block(uint32_t block_no, const uint8_t* data) {
    if (!ext2_mounted || !data) return -1;
    uint32_t sectors_per_block = ext2_blk_size / 512;
    uint32_t lba = block_no * sectors_per_block;
    return ata_write_sectors_slave(lba, sectors_per_block, data);
}

/* ================= 组描述符写回 ================= */
/* GDT 起始块：超级块固定位于字节 1024，GDT 紧随其后（字节 2048）。
 * 1024B 块 → 块 2（= s_first_data_block+1）；4096B 块 → 块 0（sb 在块 0 内）。 */
static uint32_t ext2_gdt_start_block(void) {
    return 2048u / ext2_blk_size;
}

static int ext2_write_gdesc(uint32_t group, const ext2_group_desc_t* gd) {
    uint32_t gdt_block = ext2_gdt_start_block();  /* GDT 起始块 */
    uint32_t entries_per_block = ext2_blk_size / 32;
    uint32_t target_block = gdt_block + (group / entries_per_block);
    uint32_t idx = group % entries_per_block;

    if (ext2_read_block(target_block, ext2_blkbuf) < 0) return -1;
    ext2_memcpy(ext2_blkbuf + idx * 32, gd, 32);
    return ext2_write_block(target_block, ext2_blkbuf);
}

/* 超级块写回：更新 free 计数（块 1 = 扇区 2/3） */
static int ext2_write_superblock(void) {
    uint8_t sb_raw[1024];
    if (ata_read_sectors_slave(2, 2, sb_raw) < 0) return -1;
    wr32(sb_raw + 0x0C, ext2_sb.s_free_blocks_count);
    wr32(sb_raw + 0x10, ext2_sb.s_free_inodes_count);
    return ata_write_sectors_slave(2, 2, sb_raw);
}

/* ================= 组描述符 ================= */
static int ext2_read_gdesc(uint32_t group, ext2_group_desc_t* gd) {
    uint32_t gdt_block = ext2_gdt_start_block();  /* GDT 起始块 */
    uint32_t entries_per_block = ext2_blk_size / 32;
    uint32_t target_block = gdt_block + (group / entries_per_block);
    uint32_t idx = group % entries_per_block;

    if (ext2_read_block(target_block, ext2_blkbuf) < 0) return -1;
    ext2_memcpy(gd, ext2_blkbuf + idx * 32, 32);
    return 0;
}

/* ================= inode ================= */
int ext2_read_inode(uint32_t ino, ext2_inode_t* inode) {
    if (!ext2_mounted || ino == 0) return -1;

    uint32_t group = (ino - 1) / ext2_inodes_per_group;
    uint32_t index = (ino - 1) % ext2_inodes_per_group;
    ext2_group_desc_t gd;
    if (ext2_read_gdesc(group, &gd) < 0) return -1;

    uint32_t table_block = gd.bg_inode_table;
    uint32_t off = table_block * ext2_blk_size + index * ext2_ino_size;
    uint32_t block = off / ext2_blk_size;
    uint32_t within = off % ext2_blk_size;

    if (ext2_read_block(block, ext2_blkbuf) < 0) return -1;
    /* inode 大小整除块大小，条目不会跨块 */
    ext2_memcpy(inode, ext2_blkbuf + within, ext2_ino_size);
    return 0;
}

/* 写回 inode 到 inode 表：定位组描述符 → inode 表块 + 组内偏移，读-改-写 */
int ext2_write_inode(uint32_t ino, const ext2_inode_t* inode) {
    if (!ext2_mounted || ino == 0 || !inode) return -1;

    uint32_t group = (ino - 1) / ext2_inodes_per_group;
    uint32_t index = (ino - 1) % ext2_inodes_per_group;
    ext2_group_desc_t gd;
    if (ext2_read_gdesc(group, &gd) < 0) return -1;

    uint32_t table_block = gd.bg_inode_table;
    uint32_t off = table_block * ext2_blk_size + index * ext2_ino_size;
    uint32_t block = off / ext2_blk_size;
    uint32_t within = off % ext2_blk_size;

    if (ext2_read_block(block, ext2_blkbuf) < 0) return -1;
    ext2_memcpy(ext2_blkbuf + within, inode, ext2_ino_size);
    return ext2_write_block(block, ext2_blkbuf);
}

/* ================= 数据块定位 ================= */
/* 返回 inode 第 idx 个数据块的块号，0 表示不存在/不支持。
 * 布局：i_block[0..11] 直接块，i_block[12] 单级间接，i_block[13] 两级间接。 */
static uint32_t ext2_data_block(const ext2_inode_t* inode, uint32_t idx) {
    uint32_t per_blk = ext2_blk_size / 4;

    if (idx < 12) {
        return inode->i_block[idx];
    }

    /* 单级间接块 */
    idx -= 12;
    if (idx < per_blk) {
        uint32_t indir = inode->i_block[12];
        if (indir == 0) return 0;
        if (ext2_read_block(indir, ext2_indbuf) < 0) return 0;
        return rd32(ext2_indbuf + idx * 4);
    }

    /* 两级间接块：i_block[13] -> 单级间接块号表 -> 数据块号表 */
    idx -= per_blk;
    uint32_t indir2 = inode->i_block[13];
    if (indir2 == 0) return 0;
    if (ext2_read_block(indir2, ext2_indbuf) < 0) return 0;
    uint32_t l1 = idx / per_blk;   /* 单级间接块在两级表中的索引 */
    uint32_t l2 = idx % per_blk;
    if (l1 >= per_blk) return 0;   /* 三级间接块暂不支持 */
    uint32_t l1blk = rd32(ext2_indbuf + l1 * 4);
    if (l1blk == 0) return 0;
    if (ext2_read_block(l1blk, ext2_indbuf) < 0) return 0;
    return rd32(ext2_indbuf + l2 * 4);
}

/* ================= 位图 / 组描述符写入 ================= */
/* 组位图操作：置/清第 bit 位（位图块本身是数据块，读→改→写回）。
 * bit 为组内局部索引（inode 位图为组内 inode 号-1，块位图为组内块偏移）。 */
static int ext2_bitmap_set(uint32_t bitmap_block, uint32_t bit, int set) {
    if (ext2_read_block(bitmap_block, ext2_blkbuf) < 0) return -1;
    uint8_t mask = (uint8_t)(1u << (bit & 7));
    if (set) ext2_blkbuf[bit >> 3] |= mask;
    else     ext2_blkbuf[bit >> 3] &= (uint8_t)~mask;
    return ext2_write_block(bitmap_block, ext2_blkbuf);
}

/* 组 inode 位图：在 [1, range) 内找第一个空闲位并置位，返回组内 inode 号
 * （组内 inode 0 恒保留不用）。range = min(inodes_per_group, 该组实际 inode 数)。 */
static int ext2_inode_bitmap_alloc(const ext2_group_desc_t* gd, uint32_t range) {
    if (ext2_read_block(gd->bg_inode_bitmap, ext2_blkbuf) < 0) return -1;
    for (uint32_t i = 1; i < range; i++) {
        if ((ext2_blkbuf[i >> 3] & (1u << (i & 7))) == 0) {
            ext2_blkbuf[i >> 3] |= (uint8_t)(1u << (i & 7));
            if (ext2_write_block(gd->bg_inode_bitmap, ext2_blkbuf) < 0) return -1;
            return (int)i;
        }
    }
    return -1;
}

/* 组块位图：在 [0, range) 内找第一个空闲位并置位，返回组内块号。
 * range = min(blocks_per_group, 该组实际块数)。 */
static uint32_t ext2_block_bitmap_alloc(const ext2_group_desc_t* gd, uint32_t range) {
    if (ext2_read_block(gd->bg_block_bitmap, ext2_blkbuf) < 0) return 0;
    for (uint32_t b = 0; b < range; b++) {
        if ((ext2_blkbuf[b >> 3] & (1u << (b & 7))) == 0) {
            ext2_blkbuf[b >> 3] |= (uint8_t)(1u << (b & 7));
            if (ext2_write_block(gd->bg_block_bitmap, ext2_blkbuf) < 0) return 0;
            return b;
        }
    }
    return 0;
}

/* W7 (Task 4.9): 多组分配。遍历全部组，优先用每组自身位图；
 * inode 号 = group*inodes_per_group + 组内号（组内 0 保留）。 */
int ext2_alloc_inode(void) {
    if (!ext2_mounted) return -1;
    if (ext2_sb.s_free_inodes_count == 0) return -1;

    uint32_t groups = (ext2_sb.s_inodes_count + ext2_inodes_per_group - 1)
                      / ext2_inodes_per_group;
    for (uint32_t g = 0; g < groups; g++) {
        ext2_group_desc_t gd;
        if (ext2_read_gdesc(g, &gd) < 0) continue;
        if (gd.bg_free_inodes_count == 0) continue;

        uint32_t remain = ext2_sb.s_inodes_count - g * ext2_inodes_per_group;
        uint32_t range = (remain > ext2_inodes_per_group)
                         ? ext2_inodes_per_group : remain;
        int local = ext2_inode_bitmap_alloc(&gd, range);
        if (local < 0) continue;

        gd.bg_free_inodes_count--;
        if (ext2_write_gdesc(g, &gd) < 0) return -1;

        ext2_sb.s_free_inodes_count--;
        ext2_write_superblock();
        return (int)(g * ext2_inodes_per_group + (uint32_t)local);
    }
    return -1;
}

/* 释放 inode：定位所属组，清组内位图位 + 组描述符/超级块 free 计数回写 */
int ext2_free_inode(uint32_t ino) {
    if (!ext2_mounted || ino == 0) return -1;

    uint32_t group = (ino - 1) / ext2_inodes_per_group;
    uint32_t local = (ino - 1) % ext2_inodes_per_group;
    ext2_group_desc_t gd;
    if (ext2_read_gdesc(group, &gd) < 0) return -1;

    if (ext2_bitmap_set(gd.bg_inode_bitmap, local, 0) < 0) return -1;

    gd.bg_free_inodes_count++;
    if (ext2_write_gdesc(group, &gd) < 0) return -1;

    ext2_sb.s_free_inodes_count++;
    ext2_write_superblock();
    return 0;
}

/* 分配一个空闲数据块：跨组遍历，块号 = group*blocks_per_group + 组内号 */
uint32_t ext2_alloc_block(void) {
    if (!ext2_mounted) return 0;
    if (ext2_sb.s_free_blocks_count == 0) return 0;

    uint32_t groups = (ext2_sb.s_blocks_count + ext2_blocks_per_group - 1)
                      / ext2_blocks_per_group;
    for (uint32_t g = 0; g < groups; g++) {
        ext2_group_desc_t gd;
        if (ext2_read_gdesc(g, &gd) < 0) continue;
        if (gd.bg_free_blocks_count == 0) continue;

        uint32_t remain = ext2_sb.s_blocks_count - g * ext2_blocks_per_group;
        uint32_t range = (remain > ext2_blocks_per_group)
                         ? ext2_blocks_per_group : remain;
        uint32_t local = ext2_block_bitmap_alloc(&gd, range);
        if (local == 0) continue;

        gd.bg_free_blocks_count--;
        if (ext2_write_gdesc(g, &gd) < 0) return 0;

        ext2_sb.s_free_blocks_count--;
        ext2_write_superblock();
        return g * ext2_blocks_per_group + local;
    }
    return 0;
}

/* 释放数据块：定位所属组，清组内位图位 + 组描述符/超级块 free 计数回写 */
int ext2_free_block(uint32_t blk) {
    if (!ext2_mounted || blk == 0) return -1;

    uint32_t group = blk / ext2_blocks_per_group;
    uint32_t local = blk % ext2_blocks_per_group;
    ext2_group_desc_t gd;
    if (ext2_read_gdesc(group, &gd) < 0) return -1;

    if (ext2_bitmap_set(gd.bg_block_bitmap, local, 0) < 0) return -1;

    gd.bg_free_blocks_count++;
    if (ext2_write_gdesc(group, &gd) < 0) return -1;

    ext2_sb.s_free_blocks_count++;
    ext2_write_superblock();
    return 0;
}

int ext2_free_counts(uint32_t* free_inodes, uint32_t* free_blocks) {
    if (!ext2_mounted) return -1;
    if (free_inodes) *free_inodes = ext2_sb.s_free_inodes_count;
    if (free_blocks) *free_blocks = ext2_sb.s_free_blocks_count;
    return 0;
}

/* ================= 文件读取 ================= */
int ext2_read_file(uint32_t ino, uint32_t offset, void* buf, uint32_t size) {
    if (!ext2_mounted) return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) < 0) return -1;

    uint32_t type = inode.i_mode & EXT2_S_IFMT;
    if (type != EXT2_S_IFREG && type != EXT2_S_IFLNK) return -1;

    if (offset >= inode.i_size) return 0;
    uint32_t remain = inode.i_size - offset;
    if (size > remain) size = remain;

    uint8_t* dst = (uint8_t*)buf;
    uint32_t copied = 0;
    while (copied < size) {
        uint32_t cur = offset + copied;
        uint32_t blk_idx = cur / ext2_blk_size;
        uint32_t within = cur % ext2_blk_size;
        uint32_t blk = ext2_data_block(&inode, blk_idx);
        if (blk == 0) break;
        if (ext2_read_block(blk, ext2_blkbuf) < 0) break;

        uint32_t chunk = ext2_blk_size - within;
        if (chunk > size - copied) chunk = size - copied;
        ext2_memcpy(dst + copied, ext2_blkbuf + within, chunk);
        copied += chunk;
    }
    return (int)copied;
}

/* ================= 文件写入 ================= */
/* 统计 inode 占用的 512 字节块数（含间接块本身），用于更新 i_blocks */
static uint32_t ext2_count_blocks(const ext2_inode_t* inode) {
    uint32_t per_blk = ext2_blk_size / 4;
    uint32_t count = 0;
    int i;
    for (i = 0; i < 12; i++) if (inode->i_block[i] != 0) count++;
    if (inode->i_block[12] != 0) {
        count++;  /* 单级间接块本身 */
        if (ext2_read_block(inode->i_block[12], ext2_indbuf) == 0) {
            for (uint32_t k = 0; k < per_blk; k++)
                if (rd32(ext2_indbuf + k * 4) != 0) count++;
        }
    }
    if (inode->i_block[13] != 0) {
        count++;  /* 两级间接块本身 */
        if (ext2_read_block(inode->i_block[13], ext2_indbuf) == 0) {
            for (uint32_t k = 0; k < per_blk; k++) {
                uint32_t l1 = rd32(ext2_indbuf + k * 4);
                if (l1 != 0) {
                    count++;  /* 单级间接块 */
                    if (ext2_read_block(l1, ext2_indbuf) == 0) {
                        for (uint32_t j = 0; j < per_blk; j++)
                            if (rd32(ext2_indbuf + j * 4) != 0) count++;
                    }
                }
            }
        }
    }
    return count * (ext2_blk_size / 512);  /* i_blocks 以 512 字节为单位 */
}

/* 对已有文件 inode 写入数据：offset 起 size 字节。
 * - 数据在直接块内则覆盖写入；
 * - 超出 12 个直接块则分配单级间接块（i_block[12]），必要时分配新数据块；
 * - 两级及以上暂不支持（返回 -1）。
 * 成功后更新 i_size / i_mtime / i_blocks / i_dtime 并写回 inode。 */
int ext2_write_file(uint32_t ino, uint32_t offset, const void* data, uint32_t size) {
    if (!ext2_mounted || !data || size == 0) return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) < 0) return -1;
    uint32_t type = inode.i_mode & EXT2_S_IFMT;
    if (type != EXT2_S_IFREG && type != EXT2_S_IFLNK) return -1;

    const uint8_t* src = (const uint8_t*)data;
    uint32_t end = offset + size;
    uint32_t first = offset / ext2_blk_size;
    uint32_t last = (end - 1) / ext2_blk_size;
    uint32_t per_blk = ext2_blk_size / 4;
    uint32_t indir = inode.i_block[12];

    for (uint32_t b = first; b <= last; b++) {
        uint32_t blk = 0;

        if (b < 12) {
            /* 直接块 */
            blk = inode.i_block[b];
            if (blk == 0) {
                blk = ext2_alloc_block();
                if (blk == 0) return -1;
                inode.i_block[b] = blk;
            }
        } else if (b < 12 + per_blk) {
            /* 单级间接块：i_block[12] -> 块号表 */
            if (indir == 0) {
                indir = ext2_alloc_block();
                if (indir == 0) return -1;
                inode.i_block[12] = indir;
                /* 清空新间接块内容后写回（用 ext2_indbuf，避免固定 1024B
                 * 栈数组在块尺寸 >1024 时越界写栈；写回后内存即全零，
                 * 无需再从磁盘读回） */
                ext2_memset(ext2_indbuf, 0, ext2_blk_size);
                if (ext2_write_block(indir, ext2_indbuf) < 0) return -1;
            } else {
                if (ext2_read_block(indir, ext2_indbuf) < 0) return -1;
            }
            uint32_t idx = b - 12;
            blk = rd32(ext2_indbuf + idx * 4);
            if (blk == 0) {
                blk = ext2_alloc_block();
                if (blk == 0) return -1;
                wr32(ext2_indbuf + idx * 4, blk);
                if (ext2_write_block(indir, ext2_indbuf) < 0) return -1;
            }
        } else {
            /* 两级间接块：i_block[13] -> 单级间接块号表 -> 数据块号表 */
            uint32_t idx = b - 12 - per_blk;
            uint32_t l1 = idx / per_blk;   /* 单级间接块在两级表中的索引 */
            uint32_t l2 = idx % per_blk;
            if (l1 >= per_blk) return -1;  /* 三级间接块暂不支持 */

            uint32_t indir2 = inode.i_block[13];
            if (indir2 == 0) {
                /* 分配两级表块并清零写回 */
                indir2 = ext2_alloc_block();
                if (indir2 == 0) return -1;
                inode.i_block[13] = indir2;
                ext2_memset(ext2_indbuf, 0, ext2_blk_size);
                if (ext2_write_block(indir2, ext2_indbuf) < 0) return -1;
            } else {
                if (ext2_read_block(indir2, ext2_indbuf) < 0) return -1;
            }
            uint32_t l1blk = rd32(ext2_indbuf + l1 * 4);
            if (l1blk == 0) {
                /* 分配单级间接块：先登记进两级表并写回，再清零写回新表 */
                l1blk = ext2_alloc_block();
                if (l1blk == 0) return -1;
                wr32(ext2_indbuf + l1 * 4, l1blk);
                if (ext2_write_block(indir2, ext2_indbuf) < 0) return -1;
                ext2_memset(ext2_indbuf, 0, ext2_blk_size);
                if (ext2_write_block(l1blk, ext2_indbuf) < 0) return -1;
            } else {
                if (ext2_read_block(l1blk, ext2_indbuf) < 0) return -1;
            }
            blk = rd32(ext2_indbuf + l2 * 4);
            if (blk == 0) {
                blk = ext2_alloc_block();
                if (blk == 0) return -1;
                wr32(ext2_indbuf + l2 * 4, blk);
                if (ext2_write_block(l1blk, ext2_indbuf) < 0) return -1;
            }
        }

        /* 读数据块 → 覆盖写入本块内范围 → 写回 */
        if (ext2_read_block(blk, ext2_blkbuf) < 0) { vga_puts("ext2: write: read fail\n"); return -1; }
        uint32_t chunk_start = (b == first) ? (offset % ext2_blk_size) : 0;
        uint32_t chunk_end   = (b == last)  ? ((end - 1) % ext2_blk_size) + 1 : ext2_blk_size;
        ext2_memcpy(ext2_blkbuf + chunk_start, src + (b * ext2_blk_size + chunk_start - offset), chunk_end - chunk_start);
        if (ext2_write_block(blk, ext2_blkbuf) < 0) { vga_puts("ext2: write: write fail\n"); return -1; }
    }

    /* 更新元数据并写回 inode */
    if (end > inode.i_size) inode.i_size = end;
    uint32_t now = (uint32_t)(timer_ms() / 1000);
    inode.i_mtime = now;
    inode.i_ctime = now;   /* 写操作同时更新 ctime（inode 内容变更） */
    inode.i_dtime = 0;
    inode.i_blocks = ext2_count_blocks(&inode);
    if (ext2_write_inode(ino, &inode) < 0) { vga_puts("ext2: write: inode fail\n"); return -1; }

    return (int)size;
}

/* ================= 截断 ================= */
/* 释放 inode 的全部数据块（直接块 + 单级/两级间接块），size 置 0 并写回。
 * O_TRUNC 打开时使用；unlink 释放数据块时也复用。 */
int ext2_truncate(uint32_t ino) {
    if (!ext2_mounted || ino == 0) return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) < 0) return -1;

    uint32_t per_blk = ext2_blk_size / 4;
    int i;

    /* 直接块 */
    for (i = 0; i < 12; i++) {
        if (inode.i_block[i] != 0) {
            ext2_free_block(inode.i_block[i]);
            inode.i_block[i] = 0;
        }
    }
    /* 单级间接块：先释放其指向的数据块，再释放间接块本身 */
    if (inode.i_block[12] != 0) {
        if (ext2_read_block(inode.i_block[12], ext2_indbuf) == 0) {
            for (uint32_t k = 0; k < per_blk; k++) {
                uint32_t d = rd32(ext2_indbuf + k * 4);
                if (d != 0) ext2_free_block(d);
            }
        }
        ext2_free_block(inode.i_block[12]);
        inode.i_block[12] = 0;
    }
    /* 两级间接块：释放所有单级间接块及其数据块，再释放两级表块 */
    if (inode.i_block[13] != 0) {
        if (ext2_read_block(inode.i_block[13], ext2_indbuf) == 0) {
            for (uint32_t k = 0; k < per_blk; k++) {
                uint32_t l1 = rd32(ext2_indbuf + k * 4);
                if (l1 != 0) {
                    if (ext2_read_block(l1, ext2_indbuf) == 0) {
                        for (uint32_t j = 0; j < per_blk; j++) {
                            uint32_t d = rd32(ext2_indbuf + j * 4);
                            if (d != 0) ext2_free_block(d);
                        }
                    }
                    ext2_free_block(l1);
                }
            }
        }
        ext2_free_block(inode.i_block[13]);
        inode.i_block[13] = 0;
    }

    inode.i_size = 0;
    inode.i_blocks = 0;
    inode.i_mtime = (uint32_t)(timer_ms() / 1000);
    return ext2_write_inode(ino, &inode);
}

/* ================= 目录项插入 ================= */
/* 在 dir_ino 目录中插入 (name -> child_ino) 目录项。
 * 遍历各数据块找空闲 slot（inode==0 且 rec_len 足够）复用/拆分；
 * 全满则分配新块追加到目录 inode（支持直接块 + 单级间接块）。 */

/* ---- W6.5 优化 #6：目录项缓存（dentry cache）----
 * 缓存 (parent_ino, name) → child_ino，避免 ls /bin 等对同一目录的
 * 重复磁盘读取。64 槽循环替换（简化 LRU）；写操作（create/mkdir/unlink）
 * 会清空全表以保证正确性（文件系统写操作低频，全清可接受）。
 * 缓存保存 name 副本（≤31 字符）以便命中时完整比对，避免 hash 碰撞
 * 误命中；超长名跳过缓存。 */
#define DCACHE_SLOTS 64
#define DCACHE_NAME_MAX 31
typedef struct {
    uint32_t parent;
    uint32_t hash;     /* name 的 FNV-1a 简化哈希（快速筛选） */
    uint32_t child;
    char     name[DCACHE_NAME_MAX + 1];
    uint8_t  valid;
} dentry_t;

static dentry_t dcache[DCACHE_SLOTS];
static uint32_t dcache_next = 0;   /* 循环插入游标 */

/* FNV-1a 32-bit 简化版（不依赖 libc） */
static uint32_t dcache_name_hash(const char* s)
{
    uint32_t h = 0x811C9DC5u;
    while (s && *s) {
        h ^= (uint8_t)*s++;
        h *= 0x01000193u;
    }
    return h;
}

static void dcache_flush(void)
{
    for (int i = 0; i < DCACHE_SLOTS; i++) dcache[i].valid = 0;
}

/* 插入一个目录项（循环替换最旧槽）；name 过长则跳过缓存 */
static void dcache_insert(uint32_t parent, const char* name, uint32_t child)
{
    int len = 0;
    while (name[len] && len < DCACHE_NAME_MAX) len++;
    if (name[len] != '\0') return;   /* 超长名不缓存 */

    uint32_t h = dcache_name_hash(name);
    uint32_t idx = dcache_next;
    dcache_next = (dcache_next + 1) % DCACHE_SLOTS;
    dcache[idx].parent = parent;
    dcache[idx].hash   = h;
    dcache[idx].child  = child;
    for (int i = 0; i <= len; i++) dcache[idx].name[i] = name[i];
    dcache[idx].valid  = 1;
}

/* 命中返回 0 并填 child；未命中返回 -1 */
static int dcache_lookup(uint32_t parent, const char* name, uint32_t* child)
{
    uint32_t h = dcache_name_hash(name);
    for (int i = 0; i < DCACHE_SLOTS; i++) {
        if (dcache[i].valid && dcache[i].parent == parent && dcache[i].hash == h) {
            /* 完整比对 name，杜绝 hash 碰撞误命中 */
            const char* a = dcache[i].name;
            const char* b = name;
            int ok = 1;
            while (*a && *b) { if (*a != *b) { ok = 0; break; } a++; b++; }
            if (ok && *a == *b) {
                if (child) *child = dcache[i].child;
                return 0;
            }
        }
    }
    return -1;
}

static int ext2_dir_insert(uint32_t dir_ino, const char* name, uint32_t child_ino, uint8_t ftype) {
    int name_len = ext2_strlen(name);
    if (name_len <= 0 || name_len > 255 || child_ino == 0) return -1;
    uint32_t needed = (uint32_t)((8 + name_len + 3) & ~3);

    ext2_inode_t inode;
    if (ext2_read_inode(dir_ino, &inode) < 0) return -1;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    uint32_t nblocks = (inode.i_size + ext2_blk_size - 1) / ext2_blk_size;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk = ext2_data_block(&inode, b);
        if (blk == 0) continue;
        if (ext2_read_block(blk, ext2_blkbuf) < 0) return -1;

        uint8_t* p = ext2_blkbuf;
        uint8_t* end = ext2_blkbuf + ext2_blk_size;
        while (p + 8 <= end) {
            uint32_t e_ino = rd32(p);
            uint32_t rec_len = rd16(p + 4);
            if (rec_len == 0 || p + rec_len > end) break;

            if (e_ino == 0 && rec_len >= needed) {
                /* 复用空闲 slot：空间足够则拆分出新的空闲 slot */
                uint32_t remain = rec_len - needed;
                wr32(p, child_ino);
                wr16(p + 4, (remain >= 8) ? needed : rec_len);
                p[6] = (uint8_t)name_len;
                p[7] = ftype;
                for (int i = 0; i < name_len; i++) p[8 + i] = (uint8_t)name[i];
                if (remain >= 8) {
                    uint8_t* q = p + needed;
                    wr32(q, 0);
                    wr16(q + 4, remain);
                    q[6] = 0;
                    q[7] = 0;
                }
                if (ext2_write_block(blk, ext2_blkbuf) < 0) return -1;
                return 0;
            }
            p += rec_len;
        }
    }

    /* 所有块均无空闲空间：分配新块追加到目录末尾 */
    uint32_t nblk = ext2_alloc_block();
    if (nblk == 0) return -1;

    ext2_memset(ext2_blkbuf, 0, ext2_blk_size);
    wr32(ext2_blkbuf, child_ino);
    wr16(ext2_blkbuf + 4, needed);
    ext2_blkbuf[6] = (uint8_t)name_len;
    ext2_blkbuf[7] = ftype;
    for (int i = 0; i < name_len; i++) ext2_blkbuf[8 + i] = (uint8_t)name[i];
    uint8_t* q = ext2_blkbuf + needed;
    wr32(q, 0);
    wr16(q + 4, ext2_blk_size - needed);
    q[6] = 0;
    q[7] = 0;
    if (ext2_write_block(nblk, ext2_blkbuf) < 0) {
        ext2_free_block(nblk);
        return -1;
    }

    /* 把新块挂到目录 inode（直接块 / 单级间接块） */
    uint32_t per_blk = ext2_blk_size / 4;
    if (nblocks < 12) {
        inode.i_block[nblocks] = nblk;
    } else if (nblocks < 12 + per_blk) {
        uint32_t indir = inode.i_block[12];
        if (indir == 0) {
            indir = ext2_alloc_block();
            if (indir == 0) { ext2_free_block(nblk); return -1; }
            ext2_memset(ext2_indbuf, 0, ext2_blk_size);
            if (ext2_write_block(indir, ext2_indbuf) < 0) {
                ext2_free_block(nblk);
                ext2_free_block(indir);
                return -1;
            }
            inode.i_block[12] = indir;
        }
        if (ext2_read_block(indir, ext2_indbuf) < 0) {
            ext2_free_block(nblk);
            return -1;
        }
        wr32(ext2_indbuf + (nblocks - 12) * 4, nblk);
        if (ext2_write_block(indir, ext2_indbuf) < 0) {
            ext2_free_block(nblk);
            return -1;
        }
    } else {
        ext2_free_block(nblk);
        return -1;  /* 两级间接：目录写路径暂不支持 */
    }

    inode.i_size += ext2_blk_size;
    inode.i_blocks = ext2_count_blocks(&inode);
    inode.i_mtime = (uint32_t)(timer_ms() / 1000);
    return ext2_write_inode(dir_ino, &inode);
}

/* ================= 创建文件 / 目录 ================= */
int ext2_create_file(uint32_t parent_ino, const char* name, uint32_t mode) {
    if (!ext2_mounted || !name || !name[0]) return -1;

    dcache_flush();   /* W6.5: 目录结构变更，清 dentry 缓存 */

    int ino = ext2_alloc_inode();
    if (ino < 0) return -1;

    ext2_inode_t inode;
    ext2_memset(&inode, 0, sizeof(inode));
    inode.i_mode = (uint16_t)((mode & 0x0FFF) | EXT2_S_IFREG);
    /* W7: 文件属主 = 创建者（banana 创建的文件归 banana 所有）。 */
    inode.i_uid = current_task ? current_task->uid : 0;
    inode.i_gid = current_task ? current_task->gid : 0;
    inode.i_links_count = 1;
    inode.i_size = 0;
    inode.i_blocks = 0;
    uint32_t now = (uint32_t)(timer_ms() / 1000);
    inode.i_atime = now;
    inode.i_ctime = now;
    inode.i_mtime = now;
    inode.i_dtime = 0;
    if (ext2_write_inode((uint32_t)ino, &inode) < 0) {
        ext2_free_inode((uint32_t)ino);
        return -1;
    }

    if (ext2_dir_insert(parent_ino, name, (uint32_t)ino, EXT2_FT_REG) < 0) {
        ext2_free_inode((uint32_t)ino);
        return -1;
    }
    return ino;
}

int ext2_mkdir(uint32_t parent_ino, const char* name) {
    if (!ext2_mounted || !name || !name[0]) return -1;

    dcache_flush();   /* W6.5: 目录结构变更，清 dentry 缓存 */

    int ino = ext2_alloc_inode();
    if (ino < 0) return -1;

    uint32_t blk = ext2_alloc_block();
    if (blk == 0) {
        ext2_free_inode((uint32_t)ino);
        return -1;
    }

    /* 空目录块：. 和 .. 两项（. rec_len=12，.. rec_len=12，尾部空闲 slot） */
    ext2_memset(ext2_blkbuf, 0, ext2_blk_size);
    wr32(ext2_blkbuf, (uint32_t)ino);
    wr16(ext2_blkbuf + 4, 12);
    ext2_blkbuf[6] = 1;
    ext2_blkbuf[7] = EXT2_FT_DIR;
    ext2_blkbuf[8] = '.';
    wr32(ext2_blkbuf + 12, parent_ino);
    wr16(ext2_blkbuf + 16, 12);
    ext2_blkbuf[18] = 2;
    ext2_blkbuf[19] = EXT2_FT_DIR;
    ext2_blkbuf[20] = '.';
    ext2_blkbuf[21] = '.';
    wr32(ext2_blkbuf + 24, 0);
    wr16(ext2_blkbuf + 28, ext2_blk_size - 24);
    ext2_blkbuf[30] = 0;
    ext2_blkbuf[31] = 0;
    if (ext2_write_block(blk, ext2_blkbuf) < 0) {
        ext2_free_block(blk);
        ext2_free_inode((uint32_t)ino);
        return -1;
    }

    ext2_inode_t inode;
    ext2_memset(&inode, 0, sizeof(inode));
    inode.i_mode = EXT2_S_IFDIR | 0x01FF;
    /* W7: 目录属主 = 创建者。 */
    inode.i_uid = current_task ? current_task->uid : 0;
    inode.i_gid = current_task ? current_task->gid : 0;
    inode.i_links_count = 2;
    inode.i_size = ext2_blk_size;
    inode.i_blocks = 2;   /* 1 块 × 512B 单位 */
    uint32_t now = (uint32_t)(timer_ms() / 1000);
    inode.i_atime = now;
    inode.i_ctime = now;
    inode.i_mtime = now;
    inode.i_block[0] = blk;
    if (ext2_write_inode((uint32_t)ino, &inode) < 0) {
        ext2_free_block(blk);
        ext2_free_inode((uint32_t)ino);
        return -1;
    }

    if (ext2_dir_insert(parent_ino, name, (uint32_t)ino, EXT2_FT_DIR) < 0) {
        ext2_free_block(blk);
        ext2_free_inode((uint32_t)ino);
        return -1;
    }
    return ino;
}

/* ================= 删除 ================= */
int ext2_unlink(uint32_t parent_ino, const char* name) {
    if (!ext2_mounted || !name || !name[0]) return -1;

    dcache_flush();   /* W6.5: 目录结构变更，清 dentry 缓存 */

    ext2_inode_t dir;
    if (ext2_read_inode(parent_ino, &dir) < 0) return -1;
    if ((dir.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    int name_len = ext2_strlen(name);
    if (name_len <= 0 || name_len > 255) return -1;

    /* 遍历目录块查找目标条目，记录其块号与块内偏移（以及前一条目） */
    uint32_t nblocks = (dir.i_size + ext2_blk_size - 1) / ext2_blk_size;
    int found = 0;
    uint32_t child_ino = 0;
    uint32_t child_blk = 0;
    uint8_t* cur_p = 0;
    uint8_t* prev_p = 0;

    for (uint32_t b = 0; b < nblocks && !found; b++) {
        uint32_t blk = ext2_data_block(&dir, b);
        if (blk == 0) continue;
        if (ext2_read_block(blk, ext2_blkbuf) < 0) return -1;

        uint8_t* p = ext2_blkbuf;
        uint8_t* end = ext2_blkbuf + ext2_blk_size;
        uint8_t* prev = 0;
        while (p + 8 <= end) {
            uint32_t e_ino = rd32(p);
            uint32_t rec_len = rd16(p + 4);
            uint8_t nlen = p[6];
            if (rec_len == 0 || p + rec_len > end) break;

            if (e_ino != 0 && nlen == (uint8_t)name_len) {
                int match = 1;
                for (int i = 0; i < name_len; i++) {
                    if (p[8 + i] != (uint8_t)name[i]) { match = 0; break; }
                }
                if (match) {
                    found = 1;
                    child_ino = e_ino;
                    child_blk = blk;
                    cur_p = p;
                    prev_p = prev;
                    break;
                }
            }
            prev = p;
            p += rec_len;
        }
    }
    if (!found) return -1;

    /* 仅普通文件 / 符号链接可 unlink（目录需 rmdir，不在本任务范围） */
    ext2_inode_t cinode;
    if (ext2_read_inode(child_ino, &cinode) < 0) return -1;
    uint32_t ctype = cinode.i_mode & EXT2_S_IFMT;
    if (ctype != EXT2_S_IFREG && ctype != EXT2_S_IFLNK) return -1;

    /* 从目录块移除：有前一条目则将其 rec_len 并入（条目被吸收）；
     * 否则把本条目置为空闲 slot（inode=0） */
    if (ext2_read_block(child_blk, ext2_blkbuf) < 0) return -1;
    uint32_t cur_rec = rd16(cur_p + 4);
    if (prev_p != 0) {
        uint32_t prev_rec = rd16(prev_p + 4);
        wr16(prev_p + 4, prev_rec + cur_rec);
    } else {
        wr32(cur_p, 0);
        cur_p[6] = 0;
        cur_p[7] = 0;
    }
    if (ext2_write_block(child_blk, ext2_blkbuf) < 0) return -1;

    /* inode links--；归零则释放数据块与 inode */
    cinode.i_links_count--;
    if (cinode.i_links_count == 0) {
        ext2_truncate(child_ino);
        ext2_free_inode(child_ino);
    } else {
        cinode.i_ctime = (uint32_t)(timer_ms() / 1000);
        ext2_write_inode(child_ino, &cinode);
    }
    return 0;
}

/* ================= W7: 更多目录操作 ================= */

/* 在目录 parent_ino 中查找 name 的目录项，记录 inode/所在块/块内偏移。 */
typedef struct {
    uint32_t child_ino;
    uint32_t child_blk;
    uint32_t cur_off;    /* 目标条目在块内偏移 */
    uint32_t prev_off;   /* 前一条目偏移（0xFFFFFFFF = 无） */
} ext2_dir_find_t;

static int ext2_dir_find(uint32_t parent_ino, const char* name, ext2_dir_find_t* out)
{
    ext2_inode_t dir;
    if (ext2_read_inode(parent_ino, &dir) < 0) return -1;
    if ((dir.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    int name_len = ext2_strlen(name);
    if (name_len <= 0 || name_len > 255) return -1;

    uint32_t nblocks = (dir.i_size + ext2_blk_size - 1) / ext2_blk_size;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk = ext2_data_block(&dir, b);
        if (blk == 0) continue;
        if (ext2_read_block(blk, ext2_blkbuf) < 0) return -1;

        uint8_t* p = ext2_blkbuf;
        uint8_t* end = ext2_blkbuf + ext2_blk_size;
        uint8_t* prev = 0;
        while (p + 8 <= end) {
            uint32_t e_ino = rd32(p);
            uint32_t rec_len = rd16(p + 4);
            uint8_t nlen = p[6];
            if (rec_len == 0 || p + rec_len > end) break;

            if (e_ino != 0 && nlen == (uint8_t)name_len) {
                int match = 1;
                for (int i = 0; i < name_len; i++) {
                    if (p[8 + i] != (uint8_t)name[i]) { match = 0; break; }
                }
                if (match) {
                    out->child_ino = e_ino;
                    out->child_blk = blk;
                    out->cur_off = (uint32_t)(p - ext2_blkbuf);
                    out->prev_off = prev ? (uint32_t)(prev - ext2_blkbuf)
                                        : 0xFFFFFFFFu;
                    return 0;
                }
            }
            prev = p;
            p += rec_len;
        }
    }
    return -1;
}

/* 从父目录块中移除位于 child_blk/cur_off 的目录项（合并到前一条目或置空）。 */
static int ext2_dir_remove(uint32_t child_blk, uint32_t cur_off, uint32_t prev_off)
{
    if (ext2_read_block(child_blk, ext2_blkbuf) < 0) return -1;
    uint8_t* cur_p = ext2_blkbuf + cur_off;
    uint32_t cur_rec = rd16(cur_p + 4);
    if (prev_off != 0xFFFFFFFFu) {
        uint8_t* prev_p = ext2_blkbuf + prev_off;
        uint32_t prev_rec = rd16(prev_p + 4);
        wr16(prev_p + 4, prev_rec + cur_rec);
    } else {
        wr32(cur_p, 0);
        cur_p[6] = 0;
        cur_p[7] = 0;
    }
    return ext2_write_block(child_blk, ext2_blkbuf);
}

/* ext2_rmdir — 删除空目录（仅含 . 和 ..）。非空返回 -2（ENOTEMPTY）。 */
int ext2_rmdir(uint32_t parent_ino, const char* name)
{
    if (!ext2_mounted || !name || !name[0]) return -1;
    dcache_flush();

    ext2_dir_find_t f;
    if (ext2_dir_find(parent_ino, name, &f) < 0) return -1;

    ext2_inode_t cinode;
    if (ext2_read_inode(f.child_ino, &cinode) < 0) return -1;
    if ((cinode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    /* 空目录校验：除 . 和 .. 外无其它有效条目 */
    uint32_t dnblocks = (cinode.i_size + ext2_blk_size - 1) / ext2_blk_size;
    for (uint32_t b = 0; b < dnblocks; b++) {
        uint32_t blk = ext2_data_block(&cinode, b);
        if (blk == 0) continue;
        if (ext2_read_block(blk, ext2_blkbuf) < 0) return -1;
        uint8_t* p = ext2_blkbuf;
        uint8_t* end = ext2_blkbuf + ext2_blk_size;
        while (p + 8 <= end) {
            uint32_t e_ino = rd32(p);
            uint32_t rec_len = rd16(p + 4);
            uint8_t nlen = p[6];
            if (rec_len == 0 || p + rec_len > end) break;
            if (e_ino != 0) {
                int is_dot = (nlen == 1 && p[8] == '.') ||
                             (nlen == 2 && p[8] == '.' && p[9] == '.');
                if (!is_dot) return -2;   /* ENOTEMPTY */
            }
            p += rec_len;
        }
    }

    if (ext2_dir_remove(f.child_blk, f.cur_off, f.prev_off) < 0) return -1;

    /* 释放目录数据块与 inode；父目录 links-- */
    ext2_truncate(f.child_ino);
    ext2_free_inode(f.child_ino);

    ext2_inode_t dir;
    if (ext2_read_inode(parent_ino, &dir) == 0 && dir.i_links_count > 0) {
        dir.i_links_count--;
        dir.i_mtime = (uint32_t)(timer_ms() / 1000);
        ext2_write_inode(parent_ino, &dir);
    }
    return 0;
}

/* ext2_rename — 目录项改名/搬移（同父目录或跨目录）。新名已存在则先删除。 */
int ext2_rename(uint32_t old_parent, const char* old_name,
                uint32_t new_parent, const char* new_name)
{
    if (!ext2_mounted || !old_name || !new_name || !old_name[0] || !new_name[0])
        return -1;
    dcache_flush();

    ext2_dir_find_t f;
    if (ext2_dir_find(old_parent, old_name, &f) < 0) return -1;

    /* 目标名已存在（且非同一 inode）：先删除（Linux rename 覆盖语义） */
    uint32_t existing = 0;
    if (ext2_lookup(new_parent, new_name, &existing) == 0 && existing != f.child_ino) {
        ext2_inode_t ex;
        if (ext2_read_inode(existing, &ex) == 0) {
            if ((ex.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
                ext2_rmdir(new_parent, new_name);
            else
                ext2_unlink(new_parent, new_name);
        }
    }

    /* 取子 inode 类型（目录项 file_type） */
    ext2_inode_t cinode;
    if (ext2_read_inode(f.child_ino, &cinode) < 0) return -1;
    uint8_t ftype = ((cinode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
                        ? EXT2_FT_DIR : EXT2_FT_REG;

    if (ext2_dir_insert(new_parent, new_name, f.child_ino, ftype) < 0) return -1;
    if (ext2_dir_remove(f.child_blk, f.cur_off, f.prev_off) < 0) return -1;

    /* 跨目录 rename：旧父目录 links--，新父目录 links++（目录含子目录引用） */
    if (ftype == EXT2_FT_DIR && old_parent != new_parent) {
        ext2_inode_t dir;
        if (ext2_read_inode(old_parent, &dir) == 0 && dir.i_links_count > 0) {
            dir.i_links_count--;
            ext2_write_inode(old_parent, &dir);
        }
        if (ext2_read_inode(new_parent, &dir) == 0) {
            dir.i_links_count++;
            ext2_write_inode(new_parent, &dir);
        }
    }
    return 0;
}

/* ext2_chmod — 修改 inode 权限位（保留文件类型）。 */
int ext2_chmod(uint32_t ino, uint32_t mode)
{
    if (!ext2_mounted || ino == 0) return -1;
    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) < 0) return -1;
    inode.i_mode = (inode.i_mode & EXT2_S_IFMT) | (uint16_t)(mode & 0x0FFF);
    inode.i_ctime = (uint32_t)(timer_ms() / 1000);
    return ext2_write_inode(ino, &inode);
}

/* ext2_symlink_target — 读符号链接目标（快链接存 i_block，慢链接走数据块）。 */
int ext2_symlink_target(uint32_t ino, char* buf, int max)
{
    if (!ext2_mounted || !buf || max <= 0) return -1;
    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) < 0) return -1;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFLNK) return -1;

    uint32_t size = inode.i_size;
    if (size >= (uint32_t)max) size = (uint32_t)max - 1;

    if (inode.i_size < 60) {
        /* fast symlink：目标字符串直接存于 i_block 前 60 字节 */
        for (uint32_t i = 0; i < size; i++) {
            buf[i] = (char)((inode.i_block[i / 4] >> ((i % 4) * 8)) & 0xFF);
        }
    } else {
        if (ext2_read_file(ino, 0, buf, size) < 0) return -1;
    }
    buf[size] = '\0';
    return (int)size;
}

/* ================= 目录读取 ================= */
int ext2_read_dir(uint32_t ino, int (*cb)(ext2_dir_entry_t*, void*), void* arg) {
    if (!ext2_mounted) return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) < 0) return -1;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    uint32_t nblocks = (inode.i_size + ext2_blk_size - 1) / ext2_blk_size;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk = ext2_data_block(&inode, b);
        if (blk == 0) break;
        if (ext2_read_block(blk, ext2_blkbuf) < 0) return -1;

        uint8_t* p = ext2_blkbuf;
        uint8_t* end = ext2_blkbuf + ext2_blk_size;
        while (p + 8 <= end) {
            uint32_t e_ino = rd32(p);
            uint16_t rec_len = rd16(p + 4);
            uint8_t name_len = p[6];
            uint8_t file_type = p[7];
            if (rec_len == 0) break;

            if (e_ino != 0) {
                ext2_dir_entry_t de;
                de.inode = e_ino;
                de.rec_len = rec_len;
                de.name_len = name_len;
                de.file_type = file_type;
                int i;
                for (i = 0; i < name_len && i < 255; i++) de.name[i] = (char)p[8 + i];
                de.name[i] = '\0';
                if (cb && cb(&de, arg)) return 0;
            }
            p += rec_len;
        }
    }
    return 0;
}

/* W7: getdents64 — 按字节偏移迭代目录项（linux_dirent64 布局：
 * d_ino(8) d_off(8) d_reclen(2) d_type(1) d_name[..]\0，条目按 8 字节对齐）。
 * 直接把结构写入用户 buf（当前任务页表已切换，用户页可直接寻址）。
 * off 单调递增：d_off 记下一条目偏移，调用者存入 fd->offset 供下次续读。 */
int ext2_getdents(uint32_t ino, uint64_t off, void* buf, uint64_t size,
                  uint64_t* next_off)
{
    if (!ext2_mounted || !buf || !next_off || size == 0) return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) < 0) return -1;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    uint64_t written = 0;
    uint64_t entry_off = 0;   /* 目录字节流中的当前偏移（跨块连续） */
    uint32_t nblocks = (inode.i_size + ext2_blk_size - 1) / ext2_blk_size;

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk = ext2_data_block(&inode, b);
        if (blk == 0) break;
        if (ext2_read_block(blk, ext2_blkbuf) < 0) return -1;

        uint8_t* p = ext2_blkbuf;
        uint8_t* end = ext2_blkbuf + ext2_blk_size;
        while (p + 8 <= end) {
            uint32_t e_ino = rd32(p);
            uint16_t rec_len = rd16(p + 4);
            uint8_t name_len = p[6];
            uint8_t file_type = p[7];
            if (rec_len == 0) break;

            uint64_t this_off = entry_off;
            entry_off += rec_len;

            if (e_ino == 0 || this_off < off) { p += rec_len; continue; }
            if (name_len == 0 || name_len > 255) { p += rec_len; continue; }

            uint64_t reclen = 20 + name_len;       /* 含 d_name 终止 NUL */
            reclen = (reclen + 7) & ~7ULL;         /* 8 字节对齐：NUL 必须落在条目 padding 内，
                                                      否则下一条目会覆盖 NUL（nlen=5/13/... 时） */

            if (written + reclen > size) {
                *next_off = this_off;              /* 放不下：下次从此重试 */
                return (int)written;
            }

            uint8_t* q = (uint8_t*)buf + written;
            *(uint64_t*)q            = e_ino;      /* d_ino */
            *(uint64_t*)(q + 8)      = entry_off;  /* d_off = 下一条目偏移 */
            *(uint16_t*)(q + 16)     = (uint16_t)reclen;  /* d_reclen */
            q[18] = file_type;                     /* d_type */
            for (uint32_t i = 0; i < name_len; i++) q[19 + i] = (uint8_t)p[8 + i];
            q[19 + name_len] = 0;
            written += reclen;
            p += rec_len;
        }
    }
    *next_off = entry_off;   /* 目录末尾 */
    return (int)written;
}

/* ================= 查找 ================= */
typedef struct {
    const char* name;
    uint32_t out;
    int found;
} ext2_lookup_ctx_t;

static int ext2_lookup_cb(ext2_dir_entry_t* e, void* arg) {
    ext2_lookup_ctx_t* c = (ext2_lookup_ctx_t*)arg;
    if (ext2_strcmp(e->name, c->name) == 0) {
        c->out = e->inode;
        c->found = 1;
        return 1;  /* 停止遍历 */
    }
    return 0;
}

int ext2_lookup(uint32_t parent, const char* name, uint32_t* child_ino) {
    if (!ext2_mounted || !name || !name[0]) return -1;

    /* 先查缓存 */
    uint32_t cached;
    if (dcache_lookup(parent, name, &cached) == 0) {
        if (child_ino) *child_ino = cached;
        return 0;
    }

    ext2_lookup_ctx_t c;
    c.name = name;
    c.out = 0;
    c.found = 0;
    int r = ext2_read_dir(parent, ext2_lookup_cb, &c);
    if (r < 0) return -1;
    if (!c.found) return -1;
    if (child_ino) *child_ino = c.out;
    dcache_insert(parent, name, c.out);
    return 0;
}

/* 解析绝对路径（从根 inode 逐级 lookup），返回目标 inode */
int ext2_lookup_path(const char* path, uint32_t* out_ino) {
    if (!ext2_mounted || !path) return -1;

    uint32_t cur = EXT2_ROOT_INO;

    if (path[0] != '/') return -1;
    if (path[1] == '\0') {
        if (out_ino) *out_ino = cur;
        return 0;
    }

    /* 每次处理一级目录名 */
    const char* p = path + 1;
    while (*p) {
        char seg[256];
        int i = 0;
        while (*p && *p != '/' && i < 255) seg[i++] = *p++;
        seg[i] = '\0';
        if (*p == '/') p++;
        if (i == 0) continue;

        uint32_t next;
        if (ext2_lookup(cur, seg, &next) < 0) return -1;
        cur = next;
    }

    if (out_ino) *out_ino = cur;
    return 0;
}

int ext2_inode_size(uint32_t ino, uint32_t* size) {
    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) < 0) return -1;
    if (size) *size = inode.i_size;
    return 0;
}

int ext2_inode_type(uint32_t ino, uint32_t* type) {
    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) < 0) return -1;
    if (type) *type = inode.i_mode & EXT2_S_IFMT;
    return 0;
}

uint32_t ext2_block_size(void) {
    return ext2_blk_size;
}

/* W7: 超级块计数（statfs/sysinfo 用）。 */
void ext2_superblock_counts(uint32_t* blocks, uint32_t* free_blocks,
                            uint32_t* inodes, uint32_t* free_inodes)
{
    if (blocks)      *blocks = ext2_sb.s_blocks_count;
    if (free_blocks) *free_blocks = ext2_sb.s_free_blocks_count;
    if (inodes)      *inodes = ext2_sb.s_inodes_count;
    if (free_inodes) *free_inodes = ext2_sb.s_free_inodes_count;
}

/* ================= VFS 适配层 ================= */
/* 文件池（64 槽）：close 时 ref_count 归零即回收槽位供复用 */
file_t ext2_file_pool[64];
uint8_t ext2_file_pool_used[64];

static uint64_t ext2_read_op(void* f, uint64_t off, void* buf, uint64_t size) {
    file_t* file = (file_t*)f;
    if (file->f_type == EXT2_S_IFDIR) return (uint64_t)(-21);  /* EISDIR */
    uint32_t ino = (uint32_t)(uintptr_t)file->private_data;
    uint64_t max = 0xFFFFFFFFull;
    if (size > max) size = max;
    int32_t r = ext2_read_file(ino, (uint32_t)off, buf, (uint32_t)size);
    if (r >= 0) file->offset = off + (uint64_t)r;   /* 推进文件偏移 */
    return r < 0 ? (uint64_t)(-1) : (uint64_t)r;
}

static uint64_t ext2_write_op(void* f, uint64_t off, const void* buf, uint64_t size) {
    file_t* file = (file_t*)f;
    if (size == 0) return 0;
    uint32_t ino = (uint32_t)(uintptr_t)file->private_data;
    uint64_t max = 0xFFFFFFFFull;
    if (size > max) size = max;
    int32_t r = ext2_write_file(ino, (uint32_t)off, buf, (uint32_t)size);
    if (r >= 0) file->offset = off + (uint64_t)r;   /* 推进文件偏移 */
    return r < 0 ? (uint64_t)(-1) : (uint64_t)r;
}

static uint64_t ext2_close_op(void* f) {
    file_t* file = (file_t*)f;
    if (file->ref_count > 0) {
        file->ref_count--;
        if (file->ref_count == 0) {
            /* 回收文件池槽位，供后续 open 复用（池仅 16 个，必须回收） */
            int idx = (int)(file - ext2_file_pool);
            if (idx >= 0 && idx < 64) ext2_file_pool_used[idx] = 0;
        }
    }
    return 0;
}

static file_ops_t ext2_ops;
static int ext2_ops_ready = 0;

/* 把绝对路径拆分为父目录 inode + 文件名（最后一个 '/' 之前为父路径）。 */
static int ext2_split_path(const char* path, uint32_t* parent_ino, char* name, int name_max) {
    if (!path || path[0] != '/' || !parent_ino || !name) return -1;

    const char* slash = 0;
    for (const char* p = path; *p; p++) {
        if (*p == '/') slash = p;
    }
    if (!slash) return -1;

    char parent[256];
    int plen = (int)(slash - path);
    if (plen == 0) {
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        if (plen >= (int)sizeof(parent)) return -1;
        for (int i = 0; i < plen; i++) parent[i] = path[i];
        parent[plen] = '\0';
    }

    if (ext2_lookup_path(parent, parent_ino) < 0) return -1;
    uint32_t ptype;
    if (ext2_inode_type(*parent_ino, &ptype) < 0) return -1;
    if (ptype != EXT2_S_IFDIR) return -1;

    int nlen = 0;
    for (const char* p = slash + 1; *p && nlen < name_max - 1; p++) name[nlen++] = *p;
    name[nlen] = '\0';
    if (nlen == 0) return -1;   /* 尾部斜杠（如 "/tmp/"）不算合法文件名 */
    return 0;
}

file_t* ext2_open(const char* path, uint64_t flags) {
    if (!ext2_mounted || !path) return (file_t*)0;

    uint32_t ino;
    if (ext2_lookup_path(path, &ino) < 0) {
        /* 不存在：仅 O_CREAT 时创建普通文件 */
        if (!(flags & O_CREAT)) return (file_t*)0;
        char name[256];
        uint32_t parent_ino;
        if (ext2_split_path(path, &parent_ino, name, sizeof(name)) < 0) return (file_t*)0;
        int nino = ext2_create_file(parent_ino, name, 0x01FF);
        if (nino < 0) return (file_t*)0;
        ino = (uint32_t)nino;
    }

    uint32_t type;
    if (ext2_inode_type(ino, &type) < 0) return (file_t*)0;
    /* 普通文件 / 符号链接 / 目录都可打开；目录用 getdents64 读取 */
    if (type != EXT2_S_IFREG && type != EXT2_S_IFLNK && type != EXT2_S_IFDIR)
        return (file_t*)0;

    /* O_TRUNC：截断到 size 0（清空数据块） */
    if (flags & O_TRUNC) {
        if (ext2_truncate(ino) < 0) return (file_t*)0;
    }

    if (!ext2_ops_ready) {
        ext2_ops.read = ext2_read_op;
        ext2_ops.write = ext2_write_op;
        ext2_ops.open = 0;
        ext2_ops.close = ext2_close_op;
        ext2_ops.ioctl = 0;
        ext2_ops.lseek = 0;
        ext2_ops_ready = 1;
    }

    static file_t* pool = ext2_file_pool;
    int slot = -1;
    for (int i = 0; i < 64; i++) {
        if (!ext2_file_pool_used[i]) { slot = i; break; }
    }
    if (slot < 0) {
        printk(KERN_WARNING, "[ext2] POOL EXHAUSTED open %s\n", path);
        return (file_t*)0;
    }
    ext2_file_pool_used[slot] = 1;

    file_t* f = &pool[slot];
    f->inode = ino;
    uint32_t sz = 0;
    ext2_inode_size(ino, &sz);
    f->size = sz;
    f->offset = (flags & O_APPEND) ? sz : 0;   /* O_APPEND：从文件末尾写 */
    f->ops = &ext2_ops;
    f->private_data = (void*)(uintptr_t)ino;
    f->ref_count = 1;
    f->flags = flags;
    f->f_type = type;
    for (int i = 0; i < 64; i++) f->name[i] = 0;
    for (int i = 0; path[i] && i < 63; i++) f->name[i] = path[i];
    return f;
}

typedef struct {
    char* buf;
    int   max;
    int   pos;
} ext2_list_ctx_t;

static int ext2_list_cb(ext2_dir_entry_t* e, void* arg) {
    ext2_list_ctx_t* c = (ext2_list_ctx_t*)arg;
    int i;
    for (i = 0; e->name[i] && i < e->name_len && c->pos < c->max - 2; i++) {
        c->buf[c->pos++] = e->name[i];
    }
    if (c->pos < c->max - 1)
        c->buf[c->pos++] = (e->file_type == EXT2_FT_DIR) ? '/' : ' ';
    if (c->pos < c->max - 1)
        c->buf[c->pos++] = ' ';
    return 0;
}

int ext2_list(const char* path, char* buf, int max) {
    if (!ext2_mounted || !buf || max <= 0) return -1;

    uint32_t ino;
    if (ext2_lookup_path(path, &ino) < 0) return -1;

    uint32_t type;
    if (ext2_inode_type(ino, &type) < 0) return -1;
    if (type != EXT2_S_IFDIR) return -1;

    ext2_list_ctx_t c;
    c.buf = buf;
    c.max = max;
    c.pos = 0;
    if (ext2_read_dir(ino, ext2_list_cb, &c) < 0) return -1;
    if (c.pos < max) buf[c.pos] = '\0';
    return c.pos;
}

/* ================= 挂载 ================= */
int ext2_mount(void) {
    uint8_t sb_raw[1024];

    /* 超级块固定位于字节偏移 1024（扇区 2、3） */
    if (ata_read_sectors_slave(2, 2, sb_raw) < 0) {
        vga_puts("[ext2] read superblock failed\n");
        return -1;
    }

    /* 校验魔数 0xEF53（偏移 0x38） */
    if (rd16(sb_raw + 0x38) != EXT2_MAGIC) {
        vga_puts("[ext2] bad magic (no EXT2 on slave disk)\n");
        return -1;
    }

    ext2_memcpy(&ext2_sb, sb_raw, sizeof(ext2_superblock_t));
    ext2_blk_size = 1024u << ext2_sb.s_log_block_size;
    ext2_ino_size = ext2_sb.s_inode_size ? ext2_sb.s_inode_size : 128;
    ext2_inodes_per_group = ext2_sb.s_inodes_per_group;
    ext2_blocks_per_group = ext2_sb.s_blocks_per_group;

    if (ext2_blk_size > 4096) {
        vga_puts("[ext2] block size > 4096 unsupported\n");
        return -1;
    }

    /* 分配块缓冲 */
    ext2_blkbuf = (uint8_t*)pmalloc();
    ext2_indbuf = (uint8_t*)pmalloc();
    if (!ext2_blkbuf || !ext2_indbuf) {
        vga_puts("[ext2] out of memory for buffers\n");
        return -1;
    }

    ext2_mounted = 1;

    vga_puts("[ext2] mounted: block_size=");
    {
        /* 打印块大小 */
        char tmp[16]; int i = 0; uint32_t n = ext2_blk_size;
        while (n) { tmp[i++] = '0' + (n % 10); n /= 10; }
        if (i == 0) tmp[i++] = '0';
        while (i) vga_putc(tmp[--i]);
    }
    vga_puts(", inodes=");
    {
        char tmp[16]; int i = 0; uint32_t n = ext2_sb.s_inodes_count;
        while (n) { tmp[i++] = '0' + (n % 10); n /= 10; }
        if (i == 0) tmp[i++] = '0';
        while (i) vga_putc(tmp[--i]);
    }
    vga_puts("\n");

    /* 兜底：确保 W6 验收目录（/tmp /root /usr /var）存在，镜像缺则创建 */
    {
        static const char* base_dirs[] = { "tmp", "root", "usr", "var" };
        for (int i = 0; i < 4; i++) {
            uint32_t ino;
            if (ext2_lookup(EXT2_ROOT_INO, base_dirs[i], &ino) < 0) {
                if (ext2_mkdir(EXT2_ROOT_INO, base_dirs[i]) >= 0) {
                    vga_puts("[ext2] created /");
                    vga_puts(base_dirs[i]);
                    vga_puts("\n");
                }
            }
        }
    }

    return 0;
}