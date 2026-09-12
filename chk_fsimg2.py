#!/usr/bin/env python3
# Build inode->path map for all directories in fs.img, then interpret [lk] parents.
import struct

img = open('fs.img', 'rb').read()
BS = 4096

def rd32(buf, off): return struct.unpack_from('<I', buf, off)[0]
def rd16(buf, off): return struct.unpack_from('<H', buf, off)[0]

def inode(ino):
    per_group = 8192
    g = (ino - 1) // per_group
    it = 3 if g == 0 else (g * 32768) + 2
    off = it * BS + ((ino - 1) % per_group) * 128
    return img[off:off+128]

def data_block(ino_obj, b):
    if b < 12:
        return rd32(ino_obj, 0x28 + b*4)
    blk = rd32(ino_obj, 0x28 + 12*4)
    tab = img[blk*BS:blk*BS+BS]
    return struct.unpack_from('<I', tab, (b-12)*4)[0]

def read_dir(ino):
    io = inode(ino)
    size = rd32(io, 4)
    nblocks = (size + BS - 1)//BS
    entries = []
    for b in range(nblocks):
        blk = data_block(io, b)
        if blk == 0: break
        p = blk*BS
        end = p + BS
        while p + 8 <= end:
            e_ino = rd32(img, p)
            rec = rd16(img, p+4)
            nl = img[p+6]
            ft = img[p+7]
            if rec == 0: break
            name = img[p+8:p+8+nl].decode('latin1')
            if e_ino: entries.append((name, e_ino, ft))
            p += rec
    return entries

# BFS from root
path_of = {2: '/'}
queue = [2]
dir_entries = {}
while queue:
    ino = queue.pop(0)
    try:
        ents = read_dir(ino)
    except Exception:
        continue
    dir_entries[ino] = ents
    for name, child, ft in ents:
        if ft == 2 and child not in path_of:
            path_of[child] = path_of[ino].rstrip('/') + '/' + name
            queue.append(child)

want = [1982, 3349, 2312, 2043, 225, 18, 3208, 12, 2, 335, 2044, 2313, 2121, 2125, 2139, 2171]
for w in want:
    print('inode %-6d = %s' % (w, path_of.get(w, '(not a dir / unknown)')))

print()
print('--- /usr/lib/python3.10 dirs ---')
for name, child, ft in dir_entries.get(2043, []):
    if ft == 2:
        print('   %-40s ino=%d' % (name, child))
print()
print('--- /usr/lib/python3 contents ---')
for name, child, ft in dir_entries.get(1982, []):
    print('   %-40s ino=%d ft=%d' % (name, child, ft))
