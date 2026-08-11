"""Radiance .hdr -> 16-bit PNG, so Unreal imports it as a FLAT 2D texture.

WHY THIS EXISTS. UE's TextureFactory imports .hdr as a LongLat CUBE MAP
("LogEditorFactories: HDR Image imported as LongLat cube map"), but the sky
dome material samples a 2D equirect texture -- so an .hdr option binds to
nothing and the night sky renders empty. Owner hit exactly this. PNG imports
flat, which is also what the shipped procedural starfield is.

No numpy-free deps: parses the RGBE scanline format directly (the only format
Radiance writes for these files) and tone-maps to 16-bit with a fixed exposure
so the result stays comparable between options.
"""
import sys, numpy as np
from PIL import Image

def read_hdr(path):
    with open(path,'rb') as f:
        # header
        line = f.readline()
        if not line.startswith(b'#?'): raise ValueError('not a radiance file')
        while True:
            line = f.readline()
            if line.strip() == b'': break
        dims = f.readline().split()
        if dims[0] != b'-Y' or dims[2] != b'+X': raise ValueError(f'unsupported orientation {dims}')
        h, w = int(dims[1]), int(dims[3])
        data = np.zeros((h, w, 4), np.uint8)
        for y in range(h):
            hdr4 = f.read(4)
            if len(hdr4) < 4: raise ValueError('truncated')
            if hdr4[0]==2 and hdr4[1]==2 and ((hdr4[2]<<8)|hdr4[3])==w and w>=8 and w<32768:
                for c in range(4):          # new RLE: per-channel
                    x = 0
                    while x < w:
                        cnt = f.read(1)[0]
                        if cnt > 128:
                            val = f.read(1)[0]; n = cnt-128
                            data[y, x:x+n, c] = val; x += n
                        else:
                            n = cnt
                            data[y, x:x+n, c] = np.frombuffer(f.read(n), np.uint8); x += n
            else:                            # flat scanline
                rest = f.read(4*w-4)
                data[y] = np.frombuffer(hdr4+rest, np.uint8).reshape(w,4)
        e = data[:,:,3].astype(np.int32)
        scale = np.where(e>0, np.ldexp(1.0, e-136), 0.0)     # 2^(e-128-8)
        rgb = data[:,:,:3].astype(np.float32) * scale[...,None]
        return rgb

src, dst = sys.argv[1], sys.argv[2]
rgb = read_hdr(src)
print(f"read {rgb.shape[1]}x{rgb.shape[0]}  max {rgb.max():.3f}  mean {rgb.mean():.5f}")
# Resize to POT 8192x4096 so mips generate; Lanczos in float via PIL per channel.
out_w, out_h = 8192, 4096
# PIL cannot hold 3-channel float; resize each channel as an 'F' image.
chans = []
for c in range(3):
    ch = Image.fromarray(np.ascontiguousarray(rgb[:,:,c], np.float32), mode='F')
    chans.append(np.asarray(ch.resize((out_w,out_h), Image.LANCZOS), np.float32))
a = np.stack(chans, axis=2)
# Store linear, scaled so the brightest 0.01% maps near full range -- keeps the
# nebula's dynamic range without clipping the few hot stars.
hi = np.percentile(a, 99.99)
a = np.clip(a / max(hi,1e-6), 0.0, 1.0)
# LINEAR 16-bit, because import_sky_textures.py imports the starmap slot with
# srgb=False and TC_HDR_COMPRESSED (BC6H) and the sky materials sample it with
# SAMPLERTYPE_LINEAR_COLOR. sRGB-encoding the bytes here would be decoded by
# nothing and the nebula would render washed out. 16-bit because the image is
# very dark (mean ~0.002) and 8-bit linear would band the nebula to death.
# PIL cannot write 16-bit RGB at all, so the PNG chunks are assembled directly.
import zlib, struct
u16 = (np.clip(a, 0.0, 1.0) * 65535.0 + 0.5).astype(np.uint16)
h_, w_ = u16.shape[:2]
be = u16.astype(">u2")
raw = bytearray()
for y in range(h_):
    raw.append(0)          # filter type 0 (None) per scanline
    raw += be[y].tobytes()
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
png = bytes([137, 80, 78, 71, 13, 10, 26, 10])
png += chunk(b"IHDR", struct.pack(">IIBBBBB", w_, h_, 16, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
open(dst, "wb").write(png)
print(f"wrote {dst} {out_w}x{out_h} 16-bit (norm {hi:.4f})")
