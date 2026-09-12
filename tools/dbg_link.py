import os, sys, io, tarfile, zstandard

def open_deb_tar(path):
    with open(path, 'rb') as f:
        f.read(8)
        while True:
            h = f.read(60)
            if len(h) < 60:
                break
            name = h[0:16].decode().strip().rstrip('/')
            size = int(h[48:58].decode().strip())
            data = f.read(size)
            if size % 2:
                f.read(1)
            if name.startswith('data.tar'):
                raw = data
                break
    dctx = zstandard.ZstdDecompressor()
    data = dctx.stream_reader(io.BytesIO(raw)).read()
    return tarfile.open(fileobj=io.BytesIO(data), mode='r:')

fn = 'tools/debs/vim_8.2.3995-1ubuntu2_amd64.deb'
tf = open_deb_tar(fn)
for m in tf.getmembers():
    if 'usr/bin' in m.name:
        sys.stdout.write('%r sym=%r link=%r\n' % (m.name, m.issym(), m.linkname))
tf.close()
sys.stdout.flush()
