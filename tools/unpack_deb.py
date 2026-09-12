#!/usr/bin/env python3
"""Unpack .deb (ar + zstd) files downloaded by download_bash.ps1 and extract
the ELF binaries needed by the Axion-Ban kernel:
  bash.deb       -> ./bin/bash                    -> tools/bash
  libc6.deb      -> ./lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 -> tools/ld-linux-x86-64.so.2
  libtinfo6.deb  -> ./lib/x86_64-linux-gnu/libtinfo.so.6        -> tools/libtinfo.so.6

Python's tarfile cannot read zstd; we hand the data.tar.* member to the
Windows bsdtar.exe (supports ar+zstd) instead.
"""
import io
import os
import subprocess
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def ar_members(path):
    """Yield (name, data) for members of an ar (System V) archive."""
    with open(path, 'rb') as f:
        magic = f.read(8)
        if magic != b'!<arch>\n':
            raise RuntimeError(f"{path}: not an ar archive")
        while True:
            hdr = f.read(60)
            if len(hdr) < 60:
                break
            name = hdr[0:16].decode().strip()
            try:
                size = int(hdr[48:58].decode().strip())
            except ValueError:
                break
            data = f.read(size)
            yield name, data
            if size % 2 == 1:
                f.read(1)


def decompress_tar(tar_bytes):
    """data.tar.zst -> raw tar bytes (python tarfile cannot read zstd)."""
    import zstandard as zstd
    dctx = zstd.ZstdDecompressor()
    with dctx.stream_reader(io.BytesIO(tar_bytes)) as r:
        return r.read()


def extract_members(tar_bytes, wanted):
    """Return wanted paths (no leading ./) -> bytes from a raw tar blob."""
    data = decompress_tar(tar_bytes)
    out = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:') as tf:
        for m in tf.getmembers():
            if not m.isfile():
                continue
            p = m.name
            if p.startswith('./'):
                p = p[2:]
            if p in wanted:
                out[p] = tf.extractfile(m).read()
    return out


def main():
    jobs = [
        ('bash.deb',     {'usr/bin/bash': 'bash'},),
        ('libc6.deb',    {'usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2': 'ld-linux-x86-64.so.2',
                          'usr/lib/x86_64-linux-gnu/libc.so.6': 'libc.so.6'},),
        ('libtinfo6.deb',{'usr/lib/x86_64-linux-gnu/libtinfo.so.6.4': 'libtinfo.so.6'},),
    ]
    for deb, mapping in jobs:
        p = os.path.join(HERE, deb)
        if not os.path.exists(p):
            print(f"skip {deb}: missing")
            continue
        tar = None
        for name, data in ar_members(p):
            if name.startswith('data.tar.'):
                tar = data
                break
        if tar is None:
            print(f"WARN: {deb}: no data.tar member")
            continue
        found = extract_members(tar, set(mapping.keys()))
        for src, dst in mapping.items():
            if src not in found:
                print(f"WARN: {deb} has no {src}")
                continue
            out = os.path.join(HERE, dst)
            with open(out, 'wb') as f:
                f.write(found[src])
            print(f"{dst} <- {deb}:/{src} ({len(found[src])} bytes)")
    print("unpack done")


if __name__ == '__main__':
    main()
