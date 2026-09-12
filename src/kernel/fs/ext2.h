#ifndef EXT2_H
#define EXT2_H

#include "axion.h"
#include "vfs.h"

#define EXT2_MAGIC      0xEF53
#define EXT2_ROOT_INO   2

/* inode 类型掩码 */
#define EXT2_S_IFMT     0xF000
#define EXT2_S_IFREG    0x8000
#define EXT2_S_IFDIR    0x4000
#define EXT2_S_IFLNK    0xA000

/* 目录项 file_type */
#define EXT2_FT_UNKNOWN 0
#define EXT2_FT_REG     1
#define EXT2_FT_DIR     2
#define EXT2_FT_LNK     7

/* 超级块（只读所需字段，按磁盘偏移打包） */
typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
} __attribute__((packed)) ext2_superblock_t;

/* 组描述符 */
typedef struct {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
} __attribute__((packed)) ext2_group_desc_t;

/* inode（128 字节，与磁盘布局一致；ext2_read_inode 按 s_inode_size=128 拷贝，
 * 结构必须正好 128 字节，否则栈上结构会溢出 28 字节破坏调用者栈帧） */
typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed)) ext2_inode_t;

/* 目录项（解析后的视图） */
typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[256];
} ext2_dir_entry_t;

int  ext2_mount(void);
int  ext2_read_inode(uint32_t ino, ext2_inode_t* inode);
int  ext2_read_file(uint32_t ino, uint32_t offset, void* buf, uint32_t size);
int ext2_read_dir(uint32_t ino, int (*cb)(ext2_dir_entry_t*, void*), void* arg);

/* W7: getdents64 — 按字节偏移迭代目录项，填充 linux_dirent64
 * (d_ino/d_off/d_reclen/d_type/d_name) 到 buf，最多 size 字节。
 * off 为起始偏移（首调 0）；返回写入字节数，0 = 目录结束，负值 = 出错。
 * *next_off 保存下一次调用的起始偏移（目录流中已返回的最后条目之后）。 */
int ext2_getdents(uint32_t ino, uint64_t off, void* buf, uint64_t size,
                  uint64_t* next_off);

int ext2_lookup(uint32_t parent, const char* name, uint32_t* child_ino);
int  ext2_lookup_path(const char* path, uint32_t* out_ino);
int  ext2_inode_size(uint32_t ino, uint32_t* size);
int  ext2_inode_type(uint32_t ino, uint32_t* type);
uint32_t ext2_block_size(void);

/* W7: 超级块计数（statfs/sysinfo 用）。 */
void ext2_superblock_counts(uint32_t* blocks, uint32_t* free_blocks,
                            uint32_t* inodes, uint32_t* free_inodes);

/* 写路径：位图 / 组描述符 */
int      ext2_alloc_inode(void);              /* 分配一个空闲 inode，返回 ino（失败 -1） */
int      ext2_free_inode(uint32_t ino);       /* 释放 inode（清 inode 位图 + 计数回写） */
uint32_t ext2_alloc_block(void);              /* 分配一个空闲数据块，返回块号（失败 0） */
int      ext2_free_block(uint32_t blk);       /* 释放数据块（清块位图 + 计数回写） */
int      ext2_free_counts(uint32_t* free_inodes, uint32_t* free_blocks);  /* 内存中 free 计数 */

/* 写路径：inode / 数据块 / 文件 */
int ext2_write_inode(uint32_t ino, const ext2_inode_t* inode);  /* 写回 inode 表 */
int ext2_write_block(uint32_t block_no, const uint8_t* data);   /* 写一个 1024B 数据块 */
int ext2_write_file(uint32_t ino, uint32_t offset, const void* data, uint32_t size);

/* 写路径：创建 / 删除（目录项操作，Task 1.10） */
int ext2_create_file(uint32_t parent_ino, const char* name, uint32_t mode);  /* 新建普通文件，返回 ino（失败 -1） */
int ext2_mkdir(uint32_t parent_ino, const char* name);                       /* 新建目录（含 . 和 ..），返回 ino（失败 -1） */
int ext2_unlink(uint32_t parent_ino, const char* name);                      /* 删除普通文件（目录项 + 释放 inode/数据块） */
int ext2_truncate(uint32_t ino);                                             /* 截断文件到 size 0（释放全部数据块） */

/* W7: 更多目录操作 */
int ext2_rmdir(uint32_t parent_ino, const char* name);                       /* 删除空目录（非空返回 -2 = ENOTEMPTY） */
int ext2_rename(uint32_t old_parent, const char* old_name,
                uint32_t new_parent, const char* new_name);                  /* 改名/搬移目录项 */
int ext2_chmod(uint32_t ino, uint32_t mode);                                 /* 改 inode 权限位 */
int ext2_symlink_target(uint32_t ino, char* buf, int max);                   /* 读符号链接目标 */

/* VFS 适配层：打开文件 / 列出目录 */
file_t* ext2_open(const char* path, uint64_t flags);
int     ext2_list(const char* path, char* buf, int max);

#endif /* EXT2_H */