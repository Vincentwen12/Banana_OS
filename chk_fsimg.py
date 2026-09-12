#!/usr/bin/env python3
# Quick fs.img directory dump: walk /usr/lib/python3.10 and /usr/lib/python3.10/io
import struct

img = open('fs.img', 'rb').read()
BS = 4096

def rd32(buf, off): return struct.unpack_from('<I', buf, off)[0]
def rd16(buf, off): return struct.unpack_from('<H', buf, off)[0]

def inode(ino):
    per_group = 8192
    g = (ino - 1) // per_group
    if g == 0:
        it = 3
    else:
        base = g * 32768
        it = base + 2
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

def lookup(parent_ino, name):
    for n, ino, ft in read_dir(parent_ino):
        if n == name:
            return ino, ft
    return None, None

def lookup_path(path):
    cur = 2
    for seg in path.strip('/').split('/'):
        if not seg: continue
        cur, ft = lookup(cur, seg)
        if cur is None:
            return None, None
    return cur, ft

ino, ft = lookup_path('/usr/lib/python3.10')
print('usr/lib/python3.10 ino=%s ft=%s' % (ino, ft))
if ino:
    ents = read_dir(ino)
    print('entry count:', len(ents))
    for name, eino, eft in ents:
        print('   %-45s ino=%d ft=%d' % (name, eino, eft))

for p in ['/usr/lib/python3.10/io.py', '/usr/lib/python3.10/locale.py',
          '/usr/lib/python3.10/encodings/__init__.py', '/usr/lib/python3.10/os.py',
          '/usr/lib/python3.10/site.py', '/usr/bin/python3']:
    i, f = lookup_path(p)
    print('%-50s -> %s ft=%s' % (p, i, f))
