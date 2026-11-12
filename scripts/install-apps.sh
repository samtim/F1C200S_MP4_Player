#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Cross-build the small helper apps and install them into rootfs/
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOTFS=$HERE/rootfs
BR=$HERE/buildroot-2023.02.3
export PATH=$BR/output/host/bin:$PATH

CROSS=$(ls "$BR/output/host/bin" 2>/dev/null | grep -m1 'linux-musl.*-gcc$' | sed 's/gcc$//')
[ -n "$CROSS" ] || { echo "musl toolchain not found"; exit 1; }

mkdir -p "$ROOTFS/usr/bin"

if [ -f "$HERE/apps/lcdtest.c" ]; then
	"${CROSS}gcc" -O2 -static -s -o "$ROOTFS/usr/bin/lcdtest" "$HERE/apps/lcdtest.c"
	chmod 0755 "$ROOTFS/usr/bin/lcdtest"
	echo "installed: $(ls -l "$ROOTFS/usr/bin/lcdtest" | awk '{print $5, $9}')"
fi
