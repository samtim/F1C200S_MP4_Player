#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# ---------------------------------------------------------------
# LiuLianPi F1C200S - build the 16MiB SPI NOR flash image
#
# Flash layout (must match the bootargs mtdparts and the kernel DTS):
#   0x000000 - 0x100000  1 MiB      u-boot (spl + u-boot)
#   0x100000 - 0x110000  64 KiB     dtb
#   0x110000 - 0x610000  5 MiB      zImage
#   0x610000 - 0x1000000 9.9375 MiB rootfs (jffs2, padded)
# ---------------------------------------------------------------
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
UBOOT=$HERE/u-boot/u-boot-sunxi-with-spl.bin
DTB=$HERE/linux-5.15.y/arch/arm/boot/dts/suniv-f1c100s-liulianpi-spinor.dtb
ZIMAGE=$HERE/linux-5.15.y/arch/arm/boot/zImage
ROOTFS=$HERE/rootfs
JFFS2=$HERE/rootfs.jffs2
IMG=$HERE/f1c100s_spiflash_16M.bin

ROOTFS_OFF=0x610000
ROOTFS_SIZE=0x9F0000

die() { echo "ERROR: $*" >&2; exit 1; }

for f in "$UBOOT" "$DTB" "$ZIMAGE"; do
	[ -f "$f" ] || die "missing $f"
done
[ -d "$ROOTFS" ] || die "missing rootfs directory"

# optional extras (WiFi driver + firmware) go into rootfs/ first
if [ -x "$HERE/install-wifi.sh" ]; then
	echo "==> installing wifi driver/firmware into rootfs"
	"$HERE/install-wifi.sh"
fi

if [ -x "$HERE/install-apps.sh" ]; then
	echo "==> building helper apps into rootfs"
	"$HERE/install-apps.sh"
fi

if [ -x "$HERE/install-multimedia.sh" ]; then
	echo "==> installing cedar + player into rootfs"
	"$HERE/install-multimedia.sh"
fi

usize=$(stat -c %s "$UBOOT")
[ "$usize" -le 1048576 ] || die "u-boot too big: $usize > 1MiB"
dsize=$(stat -c %s "$DTB")
[ "$dsize" -le 65536 ] || die "dtb too big: $dsize > 64KiB"
ksize=$(stat -c %s "$ZIMAGE")
[ "$ksize" -le 5242880 ] || die "zImage too big: $ksize > 5MiB"

echo "u-boot : $usize bytes"
echo "dtb    : $dsize bytes"
echo "zImage : $ksize bytes"
echo "rootfs : $(du -sh "$ROOTFS" | cut -f1)"

echo "==> creating 16MiB image"
dd if=/dev/zero of="$IMG" bs=1M count=16 status=none

echo "==> u-boot @ 0x0"
dd if="$UBOOT" of="$IMG" bs=1K conv=notrunc status=none

echo "==> dtb @ 0x100000"
dd if="$DTB" of="$IMG" bs=1K seek=1024 conv=notrunc status=none

echo "==> zImage @ 0x110000"
dd if="$ZIMAGE" of="$IMG" bs=1K seek=1088 conv=notrunc status=none

echo "==> rootfs (jffs2)"
mkfs.jffs2 -s 0x100 -e 0x10000 --pad=$ROOTFS_SIZE -d "$ROOTFS" -o "$JFFS2" 2>/dev/null
jsize=$(stat -c %s "$JFFS2")
[ "$jsize" -eq $((ROOTFS_SIZE)) ] || die "unexpected jffs2 size $jsize"

echo "==> rootfs @ 0x610000"
dd if="$JFFS2" of="$IMG" bs=1K seek=6208 conv=notrunc status=none

sync
echo "==> done: $IMG ($(stat -c %s "$IMG") bytes)"
