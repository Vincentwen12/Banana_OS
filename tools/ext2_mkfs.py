#!/usr/bin/env python3
"""EXT2 文件系统镜像生成器（多块组、可配置块尺寸）。

不依赖 mkfs.ext2，在任意平台生成 ext2 镜像。默认保持旧行为（8MB/1024B
单组）；支持 --size/--block-size/--inodes-per-group 参数化生成 512MB/
4096B/多组镜像（W7 Phase 3.2）。

布局（4096B 块，s_first_data_block=0）：
  - 块 0：boot(0..1023) + 超级块(1024..2047) + GDT(2048..)
  - 组 0：块位图=1、inode 位图=2、inode 表=3..3+itb-1、数据从 3+itb
  - 组 g>0：起始块 g*bpg；块位图=+0、inode 位图=+1、inode 表=+2..+2+itb-1

用法：python ext2_mkfs.py [--size 512M] [--block-size 4096]
      [--inodes-per-group 8192] [output.img]
"""
import argparse
import os
import struct
import sys

S_IFREG = 0x8000
S_IFDIR = 0x4000
S_IFLNK = 0xA000
FT_UNKNOWN = 0
FT_REG = 1
FT_DIR = 2
FT_SYM = 7


def parse_size(s):
    s = s.strip().upper()
    mult = 1
    if s.endswith('K'):
        mult, s = 1024, s[:-1]
    elif s.endswith('M'):
        mult, s = 1024 * 1024, s[:-1]
    elif s.endswith('G'):
        mult, s = 1024 * 1024 * 1024, s[:-1]
    return int(float(s) * mult)


class Ext2Fs:
    def __init__(self, total_blocks, block_size, inodes_per_group,
                 blocks_per_group):
        self.BLOCK_SIZE = block_size
        self.INODE_SIZE = 128
        self.TOTAL_BLOCKS = total_blocks
        self.INODES_PER_GROUP = inodes_per_group
        self.BLOCKS_PER_GROUP = blocks_per_group

        # 块号 0 恒保留（boot/sb/GDT）；组数按 inode 上限计算（组内 inode
        # 数可能超出最后一块组容量，但 inode 表尺寸按 inodes_per_group 计）。
        self.IT_BLOCKS = (inodes_per_group * self.INODE_SIZE) // block_size
        group_count = (total_blocks + blocks_per_group - 1) // blocks_per_group
        self.GROUP_COUNT = group_count
        self.TOTAL_INODES = group_count * inodes_per_group

        self.img = bytearray(total_blocks * block_size)
        self.block_bitmap = [0] * total_blocks
        self.inode_bitmap = [0] * self.TOTAL_INODES   # 全局 ino 索引
        self.inodes = [bytearray(self.INODE_SIZE)
                       for _ in range(self.TOTAL_INODES)]
        self.next_block = 0
        self.next_ino = 12
        self.dirs = {2: []}        # ino -> [(name, child_ino, ft)]
        self.dir_modes = {}        # ino -> mode

        # ---- 组元数据布局 ----
        # gdt 起始字节 2048（块 0 内，4096B 块）；1024B 块在块 2。
        gdt_start = 2048 // block_size
        self.gdts = []             # per-group (bb, ib, it)
        for g in range(group_count):
            base = g * blocks_per_group
            if g == 0:
                bb, ib, it = gdt_start + 1, gdt_start + 2, gdt_start + 3
            else:
                bb, ib, it = base, base + 1, base + 2
            self.gdts.append((bb, ib, it))
            # 标记元数据块为已用
            for b in range(bb, it + self.IT_BLOCKS):
                self.block_bitmap[b] = 1

        # 标记超级块/GDT 块为已用（1024B：块 1(sb)+2(gdt)；4096B：块 0 内含）
        sb_block = 1024 // block_size
        gdt_blocks = max(1, (group_count * 32 + block_size - 1) // block_size)
        for b in range(sb_block, gdt_start + gdt_blocks):
            self.block_bitmap[b] = 1

        # 预留块 0（boot/sb/GDT）
        self.block_bitmap[0] = 1
        # inode 0 保留
        self.inode_bitmap[0] = 1
        for i in range(1, 12):      # 保留 inode 1..11
            self.inode_bitmap[i] = 1

    def _find_free_block(self):
        n = self.next_block
        while n < self.TOTAL_BLOCKS and self.block_bitmap[n]:
            n += 1
        if n >= self.TOTAL_BLOCKS:
            # 从头再找（可能有碎片）
            n = 0
            while n < self.TOTAL_BLOCKS and self.block_bitmap[n]:
                n += 1
            if n >= self.TOTAL_BLOCKS:
                raise RuntimeError("out of blocks")
        return n

    def alloc_block(self):
        b = self._find_free_block()
        self.block_bitmap[b] = 1
        self.next_block = b + 1
        return b

    def alloc_inode(self):
        n = self.next_ino
        while n < self.TOTAL_INODES and self.inode_bitmap[n]:
            n += 1
        if n >= self.TOTAL_INODES:
            n = 12
            while n < self.TOTAL_INODES and self.inode_bitmap[n]:
                n += 1
            if n >= self.TOTAL_INODES:
                raise RuntimeError("out of inodes")
        self.inode_bitmap[n] = 1
        self.next_ino = n + 1
        return n

    def _group_of(self, ino):
        return (ino - 1) // self.INODES_PER_GROUP

    def _write_block_at(self, blk, data):
        self.img[blk * self.BLOCK_SIZE:(blk + 1) * self.BLOCK_SIZE] = data

    def _alloc_indirect(self, entries):
        buf = bytearray(self.BLOCK_SIZE)
        for k, b in enumerate(entries):
            struct.pack_into('<I', buf, k * 4, b)
        blk = self.alloc_block()
        self._write_block_at(blk, buf)
        return blk

    def write_inode(self, ino, mode, size, blocks):
        inode = self.inodes[ino]
        inode[:] = b'\x00' * self.INODE_SIZE
        struct.pack_into('<H', inode, 0x00, mode)
        struct.pack_into('<H', inode, 0x02, 0)                # i_uid
        struct.pack_into('<I', inode, 0x04, size)             # i_size
        struct.pack_into('<I', inode, 0x08, 0)                # i_atime
        struct.pack_into('<I', inode, 0x0C, 0)                # i_ctime
        struct.pack_into('<I', inode, 0x10, 0)                # i_mtime
        struct.pack_into('<I', inode, 0x14, 0)                # i_dtime
        struct.pack_into('<H', inode, 0x18, 0)                # i_gid
        links = 2 if (mode & S_IFDIR) else 1
        struct.pack_into('<H', inode, 0x1A, links)            # i_links_count

        PER = self.BLOCK_SIZE // 4
        n = len(blocks)
        indirect_extra = 0
        for j in range(min(12, n)):
            struct.pack_into('<I', inode, 0x28 + j * 4, blocks[j])
        if n > 12:
            i1 = self._alloc_indirect(blocks[12:12 + PER])
            struct.pack_into('<I', inode, 0x28 + 12 * 4, i1)
            indirect_extra += 1
            if n > 12 + PER:
                rest = blocks[12 + PER:]
                groups = [rest[i:i + PER] for i in range(0, len(rest), PER)]
                l2_entries = []
                for g in groups:
                    l2_entries.append(self._alloc_indirect(g))
                    indirect_extra += 1
                i2 = self._alloc_indirect(l2_entries)
                struct.pack_into('<I', inode, 0x28 + 13 * 4, i2)
                indirect_extra += 1
        struct.pack_into('<I', inode, 0x1C,
                         (n + indirect_extra) * (self.BLOCK_SIZE // 512))

    def make_file(self, content):
        n = (len(content) + self.BLOCK_SIZE - 1) // self.BLOCK_SIZE
        blocks = []
        for i in range(n):
            b = self.alloc_block()
            blocks.append(b)
            chunk = content[i * self.BLOCK_SIZE:(i + 1) * self.BLOCK_SIZE]
            self.img[b * self.BLOCK_SIZE:b * self.BLOCK_SIZE + len(chunk)] = chunk
        return blocks

    def ensure_dir(self, parent, name):
        if isinstance(name, str):
            name = name.encode()
        for (n, child, ft) in self.dirs[parent]:
            if n == name and ft == FT_DIR:
                return child
        ino = self.alloc_inode()
        self.dirs[ino] = []
        self.dirs[parent].append((name, ino, FT_DIR))
        return ino

    def walk_dir(self, parts):
        cur = 2
        for p in parts:
            cur = self.ensure_dir(cur, p)
        return cur

    def create_file(self, img_path, content):
        parts = [p for p in img_path.split('/') if p]
        if not parts:
            raise RuntimeError("empty path")
        parent = self.walk_dir(parts[:-1])
        name = parts[-1].encode()
        blocks = self.make_file(content)
        ino = self.alloc_inode()
        self.write_inode(ino, S_IFREG | 0o755, len(content), blocks)
        self.dirs[parent].append((name, ino, FT_REG))
        return ino

    def create_symlink(self, img_path, target):
        parts = [p for p in img_path.split('/') if p]
        if not parts:
            raise RuntimeError("empty path")
        parent = self.walk_dir(parts[:-1])
        name = parts[-1].encode()
        t = target.encode()
        blocks = self.make_file(t)
        ino = self.alloc_inode()
        inode = self.inodes[ino]
        inode[:] = b'\x00' * self.INODE_SIZE
        struct.pack_into('<H', inode, 0x00, S_IFLNK | 0o777)
        struct.pack_into('<I', inode, 0x04, len(t))
        struct.pack_into('<H', inode, 0x1A, 1)
        struct.pack_into('<I', inode, 0x1C, len(blocks) * (self.BLOCK_SIZE // 512))
        for j, b in enumerate(blocks[:12]):
            struct.pack_into('<I', inode, 0x28 + j * 4, b)
        self.dirs[parent].append((name, ino, FT_SYM))
        return ino

    def materialize_dirs(self):
        for ino in self.dirs:
            entries = self.dirs[ino]
            parent = 2
            for pino, ents in self.dirs.items():
                for (n, child, ft) in ents:
                    if child == ino:
                        parent = pino
            items = [(b'.', ino, FT_DIR), (b'..', parent, FT_DIR)] + entries
            blocks = []
            block = self.alloc_block()
            blocks.append(block)
            data = bytearray(self.BLOCK_SIZE)
            pos = 0

            def term():
                if pos < self.BLOCK_SIZE:
                    struct.pack_into('<I', data, pos, 0)
                    struct.pack_into('<H', data, pos + 4, self.BLOCK_SIZE - pos)
                    struct.pack_into('<B', data, pos + 6, 0)
                    struct.pack_into('<B', data, pos + 7, 0)

            for name, child, ft in items:
                nlen = len(name)
                rec = (8 + nlen + 3) & ~3
                if pos + rec > self.BLOCK_SIZE - 8:
                    term()
                    self._write_block_at(block, data)
                    block = self.alloc_block()
                    blocks.append(block)
                    data = bytearray(self.BLOCK_SIZE)
                    pos = 0
                struct.pack_into('<I', data, pos, child)
                struct.pack_into('<H', data, pos + 4, rec)
                struct.pack_into('<B', data, pos + 6, nlen)
                struct.pack_into('<B', data, pos + 7, ft)
                data[pos + 8:pos + 8 + nlen] = name
                pos += rec
            term()
            self._write_block_at(block, data)
            mode = self.dir_modes.get(ino, 0o755)
            self.write_inode(ino, S_IFDIR | mode,
                             len(blocks) * self.BLOCK_SIZE, blocks)

    def flush(self, out):
        B = self.BLOCK_SIZE
        used_blocks = sum(self.block_bitmap)
        used_inodes = sum(self.inode_bitmap)

        # ---- 超级块 (字节偏移 1024) ----
        sb = bytearray(1024)
        log_block = {1024: 0, 2048: 1, 4096: 2}[B]
        first_data = 0 if B >= 2048 else 1
        struct.pack_into('<I', sb, 0x00, self.TOTAL_INODES)      # s_inodes_count
        struct.pack_into('<I', sb, 0x04, self.TOTAL_BLOCKS)      # s_blocks_count
        struct.pack_into('<I', sb, 0x08, 0)                       # s_r_blocks_count
        struct.pack_into('<I', sb, 0x0C, self.TOTAL_BLOCKS - used_blocks)
        struct.pack_into('<I', sb, 0x10, self.TOTAL_INODES - used_inodes)
        struct.pack_into('<I', sb, 0x14, first_data)             # s_first_data_block
        struct.pack_into('<I', sb, 0x18, log_block)              # s_log_block_size
        struct.pack_into('<I', sb, 0x1C, log_block)              # s_log_frag_size
        struct.pack_into('<I', sb, 0x20, self.BLOCKS_PER_GROUP)
        struct.pack_into('<I', sb, 0x24, self.BLOCKS_PER_GROUP)
        struct.pack_into('<I', sb, 0x28, self.INODES_PER_GROUP)
        struct.pack_into('<H', sb, 0x38, 0xEF53)                 # s_magic
        struct.pack_into('<H', sb, 0x3A, 1)                      # s_state
        struct.pack_into('<I', sb, 0x4C, 1)                      # EXT2_DYNAMIC_REV
        struct.pack_into('<I', sb, 0x54, 11)                     # s_first_ino
        struct.pack_into('<H', sb, 0x58, self.INODE_SIZE)        # s_inode_size
        self.img[1024:2048] = sb

        # ---- GDT (字节偏移 2048) ----
        gdt_bytes = bytearray(max(1024, self.GROUP_COUNT * 32))
        for g, (bb, ib, it) in enumerate(self.gdts):
            g_free_b = used_in_group_blocks = 0
            base = g * self.BLOCKS_PER_GROUP
            end = min(base + self.BLOCKS_PER_GROUP, self.TOTAL_BLOCKS)
            g_blocks = end - base
            # 组内元数据块数（该组位图/inode 表在组内占用的块数）
            meta = 0
            for b in range(base, end):
                if self.block_bitmap[b]:
                    if b == bb or b == ib or (it <= b < it + self.IT_BLOCKS):
                        meta += 1
            g_free_b = g_blocks - meta - data_used_in_group(g, self, base, end)
            g_free_i = self.INODES_PER_GROUP - inode_used_in_group(g, self)
            struct.pack_into('<I', gdt_bytes, g * 32 + 0x00, bb)
            struct.pack_into('<I', gdt_bytes, g * 32 + 0x04, ib)
            struct.pack_into('<I', gdt_bytes, g * 32 + 0x08, it)
            struct.pack_into('<H', gdt_bytes, g * 32 + 0x0C, g_free_b)
            struct.pack_into('<H', gdt_bytes, g * 32 + 0x0E, g_free_i)
            struct.pack_into('<H', gdt_bytes, g * 32 + 0x10,
                             dirs_in_group(g, self))
        gdt_start = 2048 // B
        self.img[gdt_start * B:gdt_start * B + len(gdt_bytes)] = gdt_bytes

        # ---- 各组件位图 / inode 位图 ----
        for g, (bb, ib, it) in enumerate(self.gdts):
            base = g * self.BLOCKS_PER_GROUP
            end = min(base + self.BLOCKS_PER_GROUP, self.TOTAL_BLOCKS)
            bbuf = bytearray(B)
            for b in range(base, end):
                if self.block_bitmap[b]:
                    local = b - base
                    bbuf[local >> 3] |= (1 << (local & 7))
            self._write_block_at(bb, bbuf)

            ibuf = bytearray(B)
            for ino in range(g * self.INODES_PER_GROUP,
                             min((g + 1) * self.INODES_PER_GROUP,
                                 self.TOTAL_INODES)):
                if self.inode_bitmap[ino]:
                    local = (ino - 1) % self.INODES_PER_GROUP
                    ibuf[local >> 3] |= (1 << (local & 7))
            self._write_block_at(ib, ibuf)

            # ---- inode 表 ----
            for ino in range(g * self.INODES_PER_GROUP,
                             min((g + 1) * self.INODES_PER_GROUP,
                                 self.TOTAL_INODES)):
                if ino == 0:
                    continue
                local = (ino - 1) % self.INODES_PER_GROUP
                off = it * B + local * self.INODE_SIZE
                self.img[off:off + self.INODE_SIZE] = self.inodes[ino]

        with open(out, 'wb') as f:
            f.write(self.img)
        print(f"wrote {out}: {len(self.img)} bytes, "
              f"{used_inodes} inodes, {used_blocks} blocks used, "
              f"{self.GROUP_COUNT} groups")


def data_used_in_group(g, fs, base, end):
    """组 g 内数据块占用数（排除元数据块）。"""
    n = 0
    bb, ib, it = fs.gdts[g]
    for b in range(base, end):
        if fs.block_bitmap[b] and b != bb and b != ib and not (it <= b < it + fs.IT_BLOCKS):
            n += 1
    return n


def inode_used_in_group(g, fs):
    start = g * fs.INODES_PER_GROUP
    end = min(start + fs.INODES_PER_GROUP, fs.TOTAL_INODES)
    return sum(fs.inode_bitmap[start:end])


def dirs_in_group(g, fs):
    n = 0
    start = g * fs.INODES_PER_GROUP
    end = min(start + fs.INODES_PER_GROUP, fs.TOTAL_INODES)
    for ino in range(start, end):
        if ino in fs.dirs:
            n += 1
    return n


def build(args):
    total_bytes = parse_size(args.size)
    block_size = args.block_size
    total_blocks = total_bytes // block_size
    blocks_per_group = args.blocks_per_group
    inodes_per_group = args.inodes_per_group

    if total_blocks % blocks_per_group != 0 and blocks_per_group > 0:
        pass  # 最后组可能不满，mkfs 允许

    fs = Ext2Fs(total_blocks, block_size, inodes_per_group, blocks_per_group)

    # 基础测试文件
    fs.create_file('/etc/hostname', b'BananaOS\n')
    fs.create_file('/bin/hello', b'Hello from EXT2!\n')

    # 基础目录
    for d in ['tmp', 'root', 'usr', 'var', 'home']:
        fs.walk_dir([d])
    fs.dir_modes[fs.walk_dir(['tmp'])] = 0o1777

    # manifest 注入 (每行 "host_file:img_path")
    here = os.path.dirname(os.path.abspath(__file__))
    manifest = args.manifest
    if not os.path.isabs(manifest):
        manifest = os.path.join(here, manifest)
    if os.path.exists(manifest):
        for line in open(manifest, encoding='utf-8'):
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split(':', 1)
            if len(parts) != 2:
                continue
            host, img = parts[0].strip(), parts[1].strip()
            # host 解析：兼容两种 manifest 格式——
            #   1) 相对 tools/ 的路径（Phase 3.1 fs_manifest_multig.txt: bash:/bin/bash）
            #   2) 相对 tools/rootfs/ 的路径（fetch_deps.py 生成: bin/bash:/bin/bash）
            host_path = None
            for cand in (host,
                         os.path.join(here, host),
                         os.path.join(here, 'rootfs', host)):
                if os.path.exists(cand):
                    host_path = cand
                    break
            if host_path is None:
                print(f"WARN: {host} missing, skipping {img}")
                continue
            with open(host_path, 'rb') as f:
                fs.create_file(img, f.read())
            print(f"inject {host} -> {img}")

    fs.materialize_dirs()
    fs.flush(args.output)


def main():
    ap = argparse.ArgumentParser(description='EXT2 multi-group image builder')
    ap.add_argument('--size', default='8M', help='image size (e.g. 512M)')
    ap.add_argument('--block-size', type=int, default=1024,
                    help='block size in bytes (1024/2048/4096)')
    ap.add_argument('--blocks-per-group', type=int, default=8192,
                    help='blocks per group')
    ap.add_argument('--inodes-per-group', type=int, default=256,
                    help='inodes per group')
    ap.add_argument('--manifest', default='fs_manifest.txt',
                    help='injection manifest (default fs_manifest.txt)')
    ap.add_argument('output', nargs='?', default='fs.img',
                    help='output image path')
    args = ap.parse_args()
    build(args)


if __name__ == '__main__':
    main()
