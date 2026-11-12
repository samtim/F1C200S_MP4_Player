#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Install the RTL8188FU (RTL8188FTV) USB WiFi driver + firmware into rootfs/
# Only cfg80211 + rtl8188fu are copied: installing all 162 modules would add
# 5.5 MiB to a rootfs that has to fit in a ~9.9 MiB jffs2 partition.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
KERNEL=$HERE/linux-5.15.y
ROOTFS=$HERE/rootfs
CROSS=arm-linux-gnueabi-
export PATH=$PATH:/opt/gcc-linaro-7.2.1-2017.11-x86_64_arm-linux-gnueabi/bin

KREL=$(make -s -C "$KERNEL" ARCH=arm CROSS_COMPILE=$CROSS kernelrelease)
echo "kernel release: $KREL"

MODDIR=$ROOTFS/lib/modules/$KREL
mkdir -p "$MODDIR/kernel/net/wireless"
mkdir -p "$MODDIR/kernel/drivers/net/wireless/realtek"

CFG=$KERNEL/net/wireless/cfg80211.ko
RTL=$KERNEL/drivers/net/wireless/realtek/rtl8188fu/rtl8188fu.ko
for f in "$CFG" "$RTL"; do
	[ -f "$f" ] || { echo "missing $f (build the kernel first)"; exit 1; }
done

install -m 0644 "$CFG" "$MODDIR/kernel/net/wireless/cfg80211.ko"
install -m 0644 "$RTL" "$MODDIR/kernel/drivers/net/wireless/realtek/rtl8188fu.ko"

# strip debug info to save flash space
for k in "$MODDIR/kernel/net/wireless/cfg80211.ko" \
         "$MODDIR/kernel/drivers/net/wireless/realtek/rtl8188fu.ko"; do
	${CROSS}strip --strip-debug "$k" || true
done

echo "module sizes:"
ls -l "$MODDIR/kernel/net/wireless/cfg80211.ko" \
      "$MODDIR/kernel/drivers/net/wireless/realtek/rtl8188fu.ko"

# firmware
mkdir -p "$ROOTFS/lib/firmware/rtlwifi"
if [ -f "$HERE/firmware/rtl8188fufw.bin" ]; then
	install -m 0644 "$HERE/firmware/rtl8188fufw.bin" \
		"$ROOTFS/lib/firmware/rtlwifi/rtl8188fufw.bin"
else
	echo "WARNING: firmware/rtl8188fufw.bin missing"
fi

# module dependency map
depmod -b "$ROOTFS" "$KREL"
echo "--- modules.dep ---"
cat "$MODDIR/modules.dep"

# load the driver early so wlan0/wlan1 show up in 'ifconfig -a'
cat > "$ROOTFS/etc/init.d/S45wifi" <<'INIT'
#!/bin/sh
# Load the on-board RTL8188FTV USB WiFi driver.
case "$1" in
	start)
		printf "Loading rtl8188fu: "
		if modprobe rtl8188fu 2>/dev/null; then echo "OK"; else echo "FAILED"; fi
		;;
	stop)  ;;
	restart) ;;
	*) echo "Usage: $0 {start|stop|restart}"; exit 1 ;;
esac
INIT
chmod 0755 "$ROOTFS/etc/init.d/S45wifi"

echo "== wifi installed =="
du -sh "$ROOTFS/lib/modules" "$ROOTFS/lib/firmware"
