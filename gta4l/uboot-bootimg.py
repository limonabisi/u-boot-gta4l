#!/usr/bin/env python3
"""
Build a U-Boot boot image for the Galaxy Tab A7 (gta4l) out of a working
pmOS boot.img, using only the Python standard library.

  ABL loads:  kernel = U-Boot (gzip), dtb = U-Boot's DTB,
              ramdisk = a FIT image with the Linux kernel, DTB and initramfs
  U-Boot:     shows its menu (5 s, then boots pmOS), keeps the charger going,
              and boots the FIT with ABL's command line passed on.

Usage:  python3 UBOOT-BOOTIMG.py <pmos boot.img> <u-boot-nodtb.bin.gz> <u-boot dtb> <out boot.img>
"""
import zlib
import struct
import tarfile
import time
import sys

PAGE = 4096


def pad(b, n=PAGE):
    return b + b"\0" * (-len(b) % n)


def read_bootimg(path):
    d = open(path, "rb").read()
    if d[:8] != b"ANDROID!":
        sys.exit(f"{path}: not an Android boot image")
    (ksz, kaddr, rsz, raddr, ssz, saddr, tags, page, hv, osv) = struct.unpack_from("<10I", d, 8)
    name = d[48:64]
    cmd = d[64:576]
    extra = d[608:1632]
    dtsz = struct.unpack_from("<I", d, 1648)[0] if hv >= 2 else 0
    hdr_pages = 1
    off = page * hdr_pages

    def take(size):
        nonlocal off
        blob = d[off:off + size]
        off += (size + page - 1) // page * page
        return blob

    kernel = take(ksz)
    ramdisk = take(rsz)
    take(ssz)
    if hv >= 1:
        rdt = struct.unpack_from("<I", d, 1632)[0]
        take(rdt)
    dtb = take(dtsz) if dtsz else b""
    return dict(kernel=kernel, ramdisk=ramdisk, dtb=dtb, kaddr=kaddr, raddr=raddr,
                tags=tags, page=page, osv=osv, name=name,
                cmdline=(cmd + extra).split(b"\0")[0],
                dtb_addr=struct.unpack_from("<Q", d, 1652)[0] if hv >= 2 else 0)


# --- a minimal flattened device tree writer, enough for a FIT image ---------

class FDT:
    def __init__(self):
        self.struct = bytearray()
        self.strings = bytearray()
        self.stroff = {}

    def _s(self, name):
        if name not in self.stroff:
            self.stroff[name] = len(self.strings)
            self.strings += name.encode() + b"\0"
        return self.stroff[name]

    def begin(self, name):
        self.struct += struct.pack(">I", 1) + pad(name.encode() + b"\0", 4)

    def end(self):
        self.struct += struct.pack(">I", 2)

    def prop(self, name, value):
        if isinstance(value, str):
            value = value.encode() + b"\0"
        elif isinstance(value, int):
            value = struct.pack(">I", value)
        self.struct += struct.pack(">III", 3, len(value), self._s(name)) + pad(value, 4)

    def blob(self):
        self.struct += struct.pack(">I", 9)
        rsv = b"\0" * 16
        off_rsv = 40
        off_struct = off_rsv + len(rsv)
        off_strings = off_struct + len(self.struct)
        total = off_strings + len(self.strings)
        hdr = struct.pack(">10I", 0xd00dfeed, total, off_struct, off_strings, off_rsv,
                          17, 16, 0, len(self.strings), len(self.struct))
        return hdr + rsv + bytes(self.struct) + bytes(self.strings)


def make_fit(kernel, comp, initramfs, dtb):
    f = FDT()
    f.begin("")
    f.prop("description", "gta4l postmarketOS")
    f.prop("timestamp", int(time.time()))
    f.prop("#address-cells", 1)
    f.begin("images")

    f.begin("kernel-1")
    f.prop("description", "Linux")
    f.prop("data", kernel)
    f.prop("type", "kernel")
    f.prop("arch", "arm64")
    f.prop("os", "linux")
    f.prop("compression", comp)
    f.prop("load", 0x80000000)
    f.prop("entry", 0x80000000)
    f.end()

    f.begin("ramdisk-1")
    f.prop("description", "postmarketOS initramfs")
    f.prop("data", initramfs)
    f.prop("type", "ramdisk")
    f.prop("arch", "arm64")
    f.prop("os", "linux")
    f.prop("compression", "none")
    f.end()

    f.begin("fdt-1")
    f.prop("description", "sm6115-samsung-gta4l")
    f.prop("data", dtb)
    f.prop("type", "flat_dt")
    f.prop("arch", "arm64")
    f.prop("compression", "none")
    f.end()

    f.end()  # images
    f.begin("configurations")
    f.prop("default", "conf-1")
    f.begin("conf-1")
    f.prop("description", "gta4l")
    f.prop("kernel", "kernel-1")
    f.prop("ramdisk", "ramdisk-1")
    f.prop("fdt", "fdt-1")
    f.end()
    f.end()
    f.end()  # root
    return f.blob()


def write_bootimg(path, src, kernel, ramdisk, dtb):
    page = PAGE
    hdr = bytearray(page)
    hdr[0:8] = b"ANDROID!"
    struct.pack_into("<10I", hdr, 8, len(kernel), src["kaddr"], len(ramdisk), src["raddr"],
                     0, 0, src["tags"], page, 2, src["osv"])
    hdr[48:64] = src["name"]
    cmd = src["cmdline"]
    if len(cmd) > 512 + 1024 - 1:
        sys.exit("command line too long")
    hdr[64:64 + min(len(cmd), 512)] = cmd[:512]
    hdr[608:608 + len(cmd[512:])] = cmd[512:]
    struct.pack_into("<IQI", hdr, 1632, 0, 0, 1660)      # no recovery dtbo, header size
    struct.pack_into("<IQ", hdr, 1648, len(dtb), src["dtb_addr"])
    with open(path, "wb") as o:
        o.write(bytes(hdr))
        o.write(pad(kernel, page))
        o.write(pad(ramdisk, page))
        o.write(pad(dtb, page))


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    src = read_bootimg(sys.argv[1])
    uboot = open(sys.argv[2], "rb").read()
    uboot_dtb = open(sys.argv[3], "rb").read()

    k = src["kernel"]
    if k[:2] == b"\x1f\x8b":
        # Image.gz, maybe with a DTB appended: keep just the gzip stream
        z = zlib.decompressobj(31)
        image = z.decompress(k)
        k = k[:len(k) - len(z.unused_data)]
        comp = "gzip"
    else:
        image = k
        comp = "none"
    if image[56:60] != b"ARM\x64":
        sys.exit("the kernel in the boot image is not an arm64 Image")
    if not src["dtb"]:
        sys.exit("the boot image has no DTB")

    fit = make_fit(k, comp, src["ramdisk"], src["dtb"])
    write_bootimg(sys.argv[4], src, uboot, fit, uboot_dtb)
    print(f"Linux {len(image)} B ({comp} {len(k)} B), initramfs {len(src['ramdisk'])} B, DTB {len(src['dtb'])} B")
    print(f"U-Boot {len(uboot)} B, FIT {len(fit)} B -> {sys.argv[4]}")
    print("cmdline:", src["cmdline"].decode(errors="replace"))

    # Odin wants a tar whose member is called boot.img
    tar = sys.argv[4].rsplit(".", 1)[0] + ".tar"
    with tarfile.open(tar, "w", format=tarfile.USTAR_FORMAT) as t:
        t.add(sys.argv[4], arcname="boot.img")
    print("Odin AP:", tar)


if __name__ == "__main__":
    main()
