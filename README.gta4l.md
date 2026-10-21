# U-Boot for the Galaxy Tab A7 10.4 2020 (SM-T500, gta4l)

U-Boot that Samsung's bootloader (ABL) starts from the `boot` partition. It
shows a boot menu, keeps the battery charging (with the percentage when a
charger is plugged into the powered-off tablet), and boots Linux from a FIT
image (kernel, initramfs, device tree) stored in the same boot image.

Used by [pmaports-gta4l](https://github.com/limonabisi/pmaports-gta4l) (postmarketOS)
and [archlinux-on-gta4l](https://github.com/limonabisi/archlinux-on-gta4l).

Based on [LinuxForGta4l/u-boot](https://github.com/LinuxForGta4l/u-boot) `gta4l`,
plus the commits on top of `0824f9a3`:

- keep charging in U-Boot and show it; pass ABL's command line to Linux
- drop the SD-card bootargs from the DTS
- the charger state as a battery of four bars
- the charge percentage; started by a charger, stay in the menu and charge
- the first menu entry is "Boot Linux"

## Build

```sh
sudo apt install gcc-aarch64-linux-gnu make bison flex python3 libssl-dev libgnutls28-dev uuid-dev bc
git clone https://github.com/limonabisi/u-boot-gta4l
cd u-boot-gta4l
./gta4l/build.sh
```

The result is `out/u-boot-nodtb.bin.gz`.

## Make a boot image

Download `boot.img` from the [Releases](https://github.com/limonabisi/u-boot-gta4l/releases)
page (a working gta4l boot image: U-Boot plus the postmarketOS kernel 7.2), then:

```sh
./gta4l/build.sh boot.img
```

This builds U-Boot, puts it into `boot.img` in place of the old one, and writes
`gta4l-boot.tar` (Odin, **AP**) and `gta4l-boot.img`. The Linux kernel,
initramfs and device tree in the image stay as they are.

To build a boot image from your own kernel instead, take a postmarketOS
`boot.img` (Image.gz, initramfs and DTB inside) and run:

```sh
python3 gta4l/uboot-bootimg.py pmos-boot.img out/u-boot-nodtb.bin.gz \
    out/dts/upstream/src/arm64/qcom/sm6115-samsung-gta4l.dtb gta4l-boot.img
```

## Menu

Volume Up/Down move, Power selects. Without a key press it boots Linux after
5 seconds; hold Volume Down while it starts to stay in the menu.

1. Boot Linux
2. Enter fastboot mode (USB, `fastboot oem run:<command>` works)
3. Enter recovery mode
4. Reboot
5. Power off
