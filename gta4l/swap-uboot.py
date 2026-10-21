#!/usr/bin/env python3
"""Replace U-Boot in a gta4l boot image, keep everything else.

  swap-uboot.py <boot.img> <u-boot-nodtb.bin.gz> <new-boot.img>

The boot image is an Android boot image v2 whose kernel slot holds U-Boot and
whose ramdisk slot holds a FIT with the Linux kernel, initramfs and DTB
(see uboot-bootimg.py). Only the kernel slot and the header checksum change.
"""
import hashlib
import struct
import sys

src, kernel_path, dst = sys.argv[1:4]
d = open(src, 'rb').read()
if d[:8] != b'ANDROID!':
    sys.exit('not an Android boot image')
ks, ka, rs, ra, ss, sa, ta, ps, hv, osv = struct.unpack_from('<10I', d, 8)
if hv != 2:
    sys.exit('boot image header v%d, expected v2' % hv)
rdo_size, = struct.unpack_from('<I', d, 1632)
dtb_size, = struct.unpack_from('<I', d, 1648)


def pages(n):
    return (n + ps - 1) // ps


off = ps
parts = []
for size in (ks, rs, ss, rdo_size, dtb_size):
    parts.append(d[off:off + size])
    off += pages(size) * ps
tail = d[off:]

parts[0] = open(kernel_path, 'rb').read()
hdr = bytearray(d[:ps])
struct.pack_into('<I', hdr, 8, len(parts[0]))
h = hashlib.sha1()
for p in parts:
    h.update(p)
    h.update(struct.pack('<I', len(p)))
hdr[576:608] = h.digest().ljust(32, b'\0')

out = bytes(hdr)
for p in parts:
    out += p + b'\0' * (pages(len(p)) * ps - len(p))
open(dst, 'wb').write(out + tail)
print('U-Boot', ks, '->', len(parts[0]), 'bytes; ramdisk (FIT)', rs, 'bytes kept')
