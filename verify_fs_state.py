#!/usr/bin/env python3
"""Verify fs.img on-disk state after the QEMU write tests (read-only checks).
Target file: /etc/hostname (ext2-only, not shadowed by tmpfs).
Expected after phase1:
  - fsalloc: inode 25 set, block 4096 set; sb free inodes 230, free blocks 4093
  - wfile 0 BANANA-OS-WRITE!  -> direct block 37 starts with BANANA-OS-WRITE!
  - wfile 12288 HELLO-INDIRECT -> i_size 12303, i_block[12] indirect, data via table
"""
import struct

BLK = 1024
INO_SIZE = 128
S_IFMT = 0xF000
S_IFREG = 0x8000

d = open('fs.img', 'rb').read()


def rd32(off):
    return struct.unpack_from('<I', d, off)[0]


def inode(ino):
    off = 5 * BLK + (ino - 1) * INO_SIZE
    return d[off:off + INO_SIZE]


def bit_set(bitmap_block, bit):
    return bool(d[bitmap_block * BLK + (bit >> 3)] & (1 << (bit & 7)))


ok = True


def check(name, cond):
    global ok
    print(('PASS' if cond else 'FAIL') + '  ' + name)
    if not cond:
        ok = False


sb = d[BLK:2 * BLK]
gd = d[2 * BLK:2 * BLK + 32]
free_blocks = rd32(0x0C + BLK)
free_inodes = rd32(0x10 + BLK)
bg_free_blocks = struct.unpack_from('<H', gd, 12)[0]
bg_free_inodes = struct.unpack_from('<H', gd, 14)[0]
check('sb.free_blocks == 4093', free_blocks == 4093)
check('sb.free_inodes == 230', free_inodes == 230)
check('gd.free_blocks == sb.free_blocks', bg_free_blocks == free_blocks)
check('gd.free_inodes == sb.free_inodes', bg_free_inodes == free_inodes)

# fsalloc allocated inode 25 and block 4096
check('inode bitmap bit 25 set', bit_set(4, 25))
check('block bitmap bit 4096 set', bit_set(3, 4096))

# /etc/hostname is inode 13 (mkfs layout: /etc dir=12, hostname file=13), blk0=37
hi = inode(13)
mode = struct.unpack_from('<H', hi, 0)[0]
size = struct.unpack_from('<I', hi, 4)[0]
blocks = struct.unpack_from('<I', hi, 0x1C)[0]
direct = [struct.unpack_from('<I', hi, 0x28 + i * 4)[0] for i in range(15)]
check('hostname mode is regular', (mode & S_IFMT) == S_IFREG)
check('hostname i_size == 12302', size == 12302)
check('hostname i_blocks == (1 direct + 1 indir + 1 data)*2 == 6', blocks == 6)
check('hostname direct block == 37', direct[0] == 37)

# direct block content starts with overwritten string
blk0 = d[direct[0] * BLK:direct[0] * BLK + 16]
check('direct block starts with BANANA-OS-WRITE!', blk0.startswith(b'BANANA-OS-WRITE!'))

# single indirect block (i_block[12])
indir_blk = direct[12]
check('hostname i_block[12] (indirect) nonzero', indir_blk != 0)
check('indirect block bit set', bit_set(3, indir_blk))
indir_tab = d[indir_blk * BLK:indir_blk * BLK + 4]
data_blk = struct.unpack_from('<I', indir_tab, 0)[0]
check('indirect[0] data block nonzero', data_blk != 0)
check('data block bit set', bit_set(3, data_blk))
blk12 = d[data_blk * BLK:data_blk * BLK + 15]
check('indirect data block contains HELLO-INDIRECT', blk12.startswith(b'HELLO-INDIRECT'))

print('OVERALL:', 'ALL CHECKS PASSED' if ok else 'SOME CHECKS FAILED')
