#!/usr/bin/env python3
"""W7 Phase 3.2: 从 Ubuntu jammy (x86-64) 下载 .deb 并解包到 tools/rootfs/，
递归解析 Depends 依赖，生成 tools/fs_manifest.txt。

用法（Windows 原生，Python3）:
  python fetch_deps.py                       # 用默认包列表 + 缓存
  python fetch_deps.py --packages "gcc make"
  python fetch_deps.py --offline             # 仅用 tools/debs/ 缓存解包

产物:
  tools/debs/*.deb       下载的包缓存
  tools/rootfs/...       解包后的文件树
  tools/fs_manifest.txt  每行 "rootfs/<path>:<path>"（供 ext2_mkfs.py 注入）
"""
import argparse
import gzip
import io
import lzma
import os
import re
import shutil
import struct
import sys
import tarfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
DEBS_DIR = os.path.join(HERE, 'debs')
ROOTFS_DIR = os.path.join(HERE, 'rootfs')
MANIFEST = os.path.join(HERE, 'fs_manifest.txt')

BASE_URLS = [
    'http://archive.ubuntu.com/ubuntu/dists/jammy/main/binary-amd64/Packages.xz',
    'http://archive.ubuntu.com/ubuntu/dists/jammy/universe/binary-amd64/Packages.xz',
]

TOP_PACKAGES = [
    'bash', 'coreutils', 'gcc', 'make', 'python3', 'vim', 'vim-common',
    'libc6', 'libc6-dev', 'binutils', 'libtinfo6', 'ncurses-base',
    'libgcc-s1', 'libstdc++6', 'linux-libc-dev',
]

UA = {'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)'}


def fetch(url, dest=None):
    req = urllib.request.Request(url, headers=UA)
    data = urllib.request.urlopen(req, timeout=60).read()
    if dest:
        with open(dest, 'wb') as f:
            f.write(data)
    return data


def load_packages():
    """解析 main+universe 的 Packages 索引，返回 {name: {Depends, Filename, Size}}。"""
    pkgs = {}
    for url in BASE_URLS:
        print(f"[pkgs] {url}")
        raw = fetch(url)
        if url.endswith('.xz'):
            text = lzma.decompress(raw).decode('utf-8', 'replace')
        else:
            text = gzip.decompress(raw).decode('utf-8', 'replace')
        cur = {}
        for line in text.splitlines():
            if not line:
                if 'Package' in cur:
                    name = cur['Package']
                    cur['Depends'] = cur.get('Depends', '')
                    pkgs[name] = cur
                cur = {}
                continue
            if line[0].isspace():
                continue
            k, _, v = line.partition(':')
            cur[k.strip()] = v.strip()
        if 'Package' in cur:
            pkgs[cur['Package']] = cur
    print(f"[pkgs] indexed {len(pkgs)} packages")
    return pkgs


def parse_depends(dep_str):
    """解析 Depends: 字段，返回包名集合（剥离版本/架构约束，忽略 OR 备选外）。
    每个候选 'a | b (=ver) [arch]' 取第一个候选即可。"""
    out = set()
    for clause in dep_str.split(','):
        clause = clause.strip()
        if not clause:
            continue
        first = clause.split('|')[0].strip()
        m = re.match(r'^([A-Za-z0-9+.\-]+)', first)
        if m:
            out.add(m.group(1))
    return out


def resolve(pkgs, roots):
    """BFS 展开依赖。返回有序包名列表（root 优先，按依赖深度排序）。"""
    ordered = []
    seen = set()
    queue = list(roots)
    while queue:
        name = queue.pop(0)
        if name in seen:
            continue
        seen.add(name)
        p = pkgs.get(name)
        if not p:
            print(f"[warn] package not found: {name}")
            continue
        ordered.append(name)
        for dep in parse_depends(p['Depends']):
            if dep not in seen:
                queue.append(dep)
    return ordered


def download(ordered, pkgs, use_cache=True):
    for name in ordered:
        p = pkgs[name]
        fn = os.path.basename(p.get('Filename', ''))
        if not fn:
            print(f"[warn] {name}: no Filename in index")
            continue
        dest = os.path.join(DEBS_DIR, fn)
        if os.path.exists(dest) and os.path.getsize(dest) > 0 and use_cache:
            print(f"[cache] {fn}")
            continue
        url = 'http://archive.ubuntu.com/ubuntu/' + p['Filename']
        print(f"[get]   {fn} ({p.get('Size', '?')}B)")
        try:
            data = fetch(url)
            with open(dest, 'wb') as f:
                f.write(data)
        except Exception as e:
            print(f"[fail]  {fn}: {e}")


def ar_extract(path, outdir):
    """解包 .deb（ar 归档）中的 data.tar.*。返回解压出的文件名。"""
    with open(path, 'rb') as f:
        magic = f.read(8)
        if magic != b'!<arch>\n':
            raise ValueError(f"{path}: not an ar archive")
        target = None
        raw = None
        while True:
            hdr = f.read(60)
            if len(hdr) < 60:
                break
            name = hdr[0:16].decode('ascii', 'replace').strip().rstrip('/')
            try:
                size = int(hdr[48:58].decode().strip() or '0')
            except ValueError:
                size = 0
            data = f.read(size)
            if size % 2:
                f.read(1)
            if name.startswith('data.tar'):
                target, raw = name, data
    if not target or raw is None:
        raise ValueError(f"{path}: no data.tar member")
    return target, raw


def xz_decompress_all(raw):
    """流式解压 xz（jammy 的 data.tar.xz 无 uncompressed-size 字段，
    lzma.decompress() 会报 'could not determine content size'）。"""
    d = lzma.LZMADecompressor()
    out = bytearray()
    buf = memoryview(raw)
    while True:
        chunk = d.decompress(buf)
        out += chunk
        if d.eof:
            break
        if d.needs_input:
            break
        buf = memoryview(b'')
    return bytes(out)


def zstd_decompress_all(raw):
    import zstandard
    dctx = zstandard.ZstdDecompressor()
    return dctx.stream_reader(io.BytesIO(raw)).read()


def open_deb_tar(deb_path):
    """解出 deb 的 data.tar 并打开为 tarfile（调用方负责 close）。"""
    name, raw = ar_extract(deb_path, '')
    if name.endswith('.xz'):
        data = xz_decompress_all(raw)
    elif name.endswith('.zst'):
        data = zstd_decompress_all(raw)
    elif name.endswith('.gz'):
        data = gzip.decompress(raw)
    else:
        data = raw
    return tarfile.open(fileobj=io.BytesIO(data), mode='r:')


def skip_member(rel):
    """过滤：只保留关键目录，跳过文档/源码/man。"""
    if not rel or rel in ('.', '..'):
        return True
    if not any(rel.startswith(p + '/') or rel == p
               for p in ('bin', 'sbin', 'lib', 'lib64', 'usr', 'etc')):
        return True
    if ('/doc/' in rel or rel.startswith('usr/share/doc')
            or rel.startswith('usr/share/man')
            or '/include/' in rel and not rel.startswith('usr/include')):
        return True
    return False


def unpack_files(tf, outdir):
    """第一遍：解包全部真实文件与目录。"""
    n = 0
    for member in tf.getmembers():
        if not member.isfile() and not member.isdir():
            continue
        rel = member.name.lstrip('./')
        if skip_member(rel):
            continue
        dst = os.path.join(outdir, rel)
        if member.isdir():
            os.makedirs(dst, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with tf.extractfile(member) as src, open(dst, 'wb') as out:
            shutil.copyfileobj(src, out)
        n += 1
    return n


def resolve_links(tf, outdir):
    """第二遍：处理符号/硬链接（Windows 无链接权限，复制内容）。
    优先同一 tar 内目标（成员名带 ./ 前缀），其次磁盘（跨 deb 已完整）。"""
    n = 0
    for member in tf.getmembers():
        if not member.issym() and not member.islnk():
            continue
        rel = member.name.lstrip('./')
        if skip_member(rel):
            continue
        dst = os.path.join(outdir, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        link = member.linkname.replace('\\', '/')
        if link.startswith('/'):
            norm = link.lstrip('/')
        else:
            norm = os.path.normpath(
                os.path.dirname(rel) + '/' + link).replace('\\', '/')
        copied = False
        for cand in (norm, './' + norm):
            try:
                tm = tf.getmember(cand)
                if tm.isfile():
                    with tf.extractfile(tm) as src, open(dst, 'wb') as out:
                        shutil.copyfileobj(src, out)
                    copied = True
                    break
            except KeyError:
                continue
        if not copied:
            disk = os.path.join(outdir, norm)
            if os.path.exists(disk):
                shutil.copyfile(disk, dst)
                copied = True
        if not copied:
            print(f"[sym] skip {rel} -> {member.linkname}")
        else:
            n += 1
    return n


def unpack_all(outdir):
    """两遍解包所有缓存 deb。返回 (deb数, 文件数, 链接数)。"""
    debs = sorted(f for f in os.listdir(DEBS_DIR) if f.endswith('.deb'))
    total_f = 0
    for fn in debs:
        try:
            tf = open_deb_tar(os.path.join(DEBS_DIR, fn))
        except Exception as e:
            print(f"[skip] {fn}: {e}")
            continue
        with tf:
            total_f += unpack_files(tf, outdir)
    total_l = 0
    for fn in debs:
        try:
            tf = open_deb_tar(os.path.join(DEBS_DIR, fn))
        except Exception:
            continue
        with tf:
            total_l += resolve_links(tf, outdir)
    return len(debs), total_f, total_l


def gen_ld_so_cache(rootfs):
    """生成 /etc/ld.so.cache（glibc 2.35 新格式，magic + version + 扩展头）。

    Ubuntu 的 ld.so 编译时默认搜索路径不含 multiarch 目录，完全依赖 cache
    定位共享库；无 cache 时所有 DT_NEEDED 库都报 "cannot stat shared object"。

    格式（elf/dl-cache.h, glibc 2.35）：
      cache_file_new: magic[17]="glibc-ld.so.cache" + version[3]="1.1"
        + nlibs + len_strings + flags(=2, little-endian) + pad[3]
        + extension_offset(=0) + unused[12]      → 共 48 字节
      file_entry_new: flags(=3, FLAG_ELF_LIBC6) + key + value
        + osversion(0) + hwcap(0)                → 共 24 字节
      key/value 为相对【文件起始】的偏移；条目按 _dl_cache_libcmp 升序
      （数字段按数值比较），ld.so 用二分查找。
    """
    libs = []
    seen = set()
    for d in ('lib/x86_64-linux-gnu', 'usr/lib/x86_64-linux-gnu',
              'lib64', 'usr/lib64'):
        base = os.path.join(rootfs, d)
        if not os.path.isdir(base):
            continue
        for fn in sorted(os.listdir(base)):
            if 'so' not in fn:
                continue
            p = os.path.join(base, fn)
            if os.path.isfile(p) and os.path.getsize(p) > 0:
                if fn not in seen:
                    seen.add(fn)
                    libs.append((fn, '/' + d + '/' + fn))
    if not libs:
        print("[ldcache] no libs found, skip")
        return

    # glibc _dl_cache_libcmp：数字段按数值比较（如 .so.10 > .so.9）
    def libcmp(p1, p2):
        i = j = 0
        n1, n2 = len(p1), len(p2)
        while i < n1:
            if p1[i].isdigit():
                if j < n2 and p2[j].isdigit():
                    v1 = 0
                    while i < n1 and p1[i].isdigit():
                        v1 = v1 * 10 + int(p1[i]); i += 1
                    v2 = 0
                    while j < n2 and p2[j].isdigit():
                        v2 = v2 * 10 + int(p2[j]); j += 1
                    if v1 != v2:
                        return v1 - v2
                else:
                    return 1
            elif j >= n2:
                return 1
            elif p2[j].isdigit():
                return -1
            elif p1[i] != p2[j]:
                return ord(p1[i]) - ord(p2[j])
            else:
                i += 1; j += 1
        return 0 if j >= n2 else -1

    import functools
    libs.sort(key=functools.cmp_to_key(lambda a, b: libcmp(a[0], b[0])))

    strs = b''
    offsets = {}

    def add(s):
        nonlocal strs
        if s in offsets:
            return offsets[s]
        off = len(strs)
        offsets[s] = off
        strs += s.encode() + b'\x00'
        return off

    nlibs = len(libs)
    entries_base = 48 + 24 * nlibs   # key/value 相对文件起始
    entries = []
    for key, val in libs:
        ko, vo = add(key), add(val)
        # file_entry_new: flags=3(FLAG_ELF_LIBC6), key, value, osversion, hwcap
        entries.append(struct.pack('<iIIIQ', 3, entries_base + ko,
                                   entries_base + vo, 0, 0))

    # cache_file_new: magic[17] + version[3] + nlibs + len_strings
    #   + flags(2=little) + pad[3] + extension_offset(0) + unused[12]
    header = (b'glibc-ld.so.cache' + b'1.1'
              + struct.pack('<II', nlibs, len(strs))
              + b'\x02' + b'\x00\x00\x00' + b'\x00\x00\x00\x00'
              + b'\x00' * 12)
    assert len(header) == 48, len(header)
    cache = header + b''.join(entries) + strs

    dst = os.path.join(rootfs, 'etc', 'ld.so.cache')
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, 'wb') as f:
        f.write(cache)
    print(f"[ldcache] wrote {nlibs} libs -> etc/ld.so.cache "
          f"({len(cache)} bytes)")


def write_configs(rootfs):
    """写入基础配置文件（覆盖 glibc/bash 运行所需）。"""
    def w(rel, content, mode=0o644):
        p = os.path.join(rootfs, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as f:
            f.write(content)
        os.chmod(p, mode)

    w('etc/passwd',
      b'root:x:0:0:root:/root:/bin/bash\n'
      b'banana:x:1000:1000:banana:/home/banana:/bin/bash\n')
    w('etc/group',
      b'root:x:0:\n'
      b'banana:x:1000:\n')
    w('etc/nsswitch.conf',
      b'passwd: files\n'
      b'group: files\n'
      b'shadow: files\n'
      b'hosts: files dns\n')
    w('etc/hostname', b'axion\n')
    w('etc/profile',
      b'export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\n'
      b'export HOME=/home/banana\n')
    w('etc/bash.bashrc',
      b'if [ "$PS1" ]; then\n'
      b'  PS1="\\u@\\h:\\w\\$ "\n'
      b'fi\n')
    w('etc/ld.so.conf',
      b'/lib/x86_64-linux-gnu\n'
      b'/usr/lib/x86_64-linux-gnu\n'
      b'/usr/lib64\n')
    w('home/banana/.bashrc',
      b'export PS1="\\u@\\h:\\w\\$ "\n'
      b'export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\n')
    w('home/banana/.profile',
      b'export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\n')

    # alternatives 机制的命令链接（Ubuntu 由 update-alternatives 生成，deb 内无静态链接）
    for src, dst in (('usr/bin/python3.10', 'usr/bin/python3'),
                     ('usr/bin/vim.basic', 'usr/bin/vim')):
        sp = os.path.join(rootfs, src)
        dp = os.path.join(rootfs, dst)
        if os.path.exists(sp) and not os.path.exists(dp):
            os.makedirs(os.path.dirname(dp), exist_ok=True)
            shutil.copyfile(sp, dp)
            print(f"[link] {dst} <- {src}")

    gen_ld_so_cache(rootfs)


def write_manifest():
    lines = []
    for root, dirs, files in os.walk(ROOTFS_DIR):
        dirs.sort()
        for fn in sorted(files):
            rel = os.path.relpath(os.path.join(root, fn), ROOTFS_DIR)
            rel = rel.replace('\\', '/')   # manifest 一律用 POSIX 分隔符
            lines.append(f'{rel}:{rel}')
    with open(MANIFEST, 'w', encoding='utf-8') as f:
        f.write('# AUTO-GENERATED by fetch_deps.py -- do not edit\n')
        f.write('\n'.join(lines))
        f.write('\n')
    print(f"[manifest] wrote {len(lines)} entries to {MANIFEST}")


def main():
    ap = argparse.ArgumentParser(description='fetch & unpack Ubuntu jammy debs')
    ap.add_argument('--packages', default=' '.join(TOP_PACKAGES),
                    help='top-level package names (space separated)')
    ap.add_argument('--offline', action='store_true',
                    help='skip downloads, unpack from debs/ cache only')
    ap.add_argument('--no-unpack', action='store_true',
                    help='download only (no unpack)')
    args = ap.parse_args()

    os.makedirs(DEBS_DIR, exist_ok=True)
    os.makedirs(ROOTFS_DIR, exist_ok=True)

    roots = args.packages.split()
    pkgs = {} if args.offline else load_packages()
    ordered = roots if args.offline else resolve(pkgs, roots)
    print(f"[resolve] {len(ordered)} packages")

    if not args.offline:
        download(ordered, pkgs)

    # 解包所有缓存 .deb（两遍：先文件，后链接）
    if not args.no_unpack:
        ndebs, nfiles, nlinks = unpack_all(ROOTFS_DIR)
        print(f"[unpack] {ndebs} debs, {nfiles} files, {nlinks} links")
        write_configs(ROOTFS_DIR)
        print("[config] config files written")

    write_manifest()
    print("[done]")


if __name__ == '__main__':
    main()
