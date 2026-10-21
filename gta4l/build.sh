#!/bin/sh
# Build U-Boot for the Galaxy Tab A7 10.4 2020 (SM-T500, gta4l).
#
#   ./gta4l/build.sh                 build out/u-boot-nodtb.bin.gz
#   ./gta4l/build.sh boot.img        also put it into boot.img: gta4l-boot.img
#                                    and gta4l-boot.tar (Odin, AP)
#
# boot.img is a working gta4l U-Boot boot image (from the Releases page of this
# repository): its kernel slot is replaced with the new U-Boot, the Linux
# kernel, initramfs and device tree in it stay as they are.
#
# Needs: aarch64-linux-gnu-gcc, make, bison, flex, python3, libssl-dev,
# libgnutls28-dev, uuid-dev, gzip, tar
# (Debian/Ubuntu: sudo apt install gcc-aarch64-linux-gnu make bison flex
#  python3 libssl-dev libgnutls28-dev uuid-dev bc)
set -e
cd "$(dirname "$0")/.."
CROSS=${CROSS_COMPILE:-aarch64-linux-gnu-}
command -v ${CROSS}gcc >/dev/null || { echo "missing ${CROSS}gcc (sudo apt install gcc-aarch64-linux-gnu)"; exit 1; }

make O=out ARCH=arm CROSS_COMPILE=$CROSS qcom_defconfig
./scripts/config --file out/.config -d TOOLS_MKEFICAPSULE
make O=out ARCH=arm CROSS_COMPILE=$CROSS olddefconfig >/dev/null
make O=out ARCH=arm CROSS_COMPILE=$CROSS -j"$(nproc)"
gzip -9 -n -c out/u-boot-nodtb.bin > out/u-boot-nodtb.bin.gz
echo "built: out/u-boot-nodtb.bin.gz"

if [ -n "$1" ]; then
	python3 gta4l/swap-uboot.py "$1" out/u-boot-nodtb.bin.gz gta4l-boot.img
	rm -rf out/odin && mkdir -p out/odin
	cp gta4l-boot.img out/odin/boot.img
	(cd out/odin && tar -H ustar -cf ../../gta4l-boot.tar boot.img)
	echo "done: gta4l-boot.tar (flash with Odin as AP) and gta4l-boot.img (fastboot flash boot)"
fi
