#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Cross-build the Allwinner CedarX stack (libcedarc) for the musl rootfs and
# install it together with the llp-play MP4 player.
#
#   libvdecoder / libcdc_base / libMemAdapter  -> built from source here
#   libawh264 / libVE / libvideoengine         -> prebuilt ARMv5TE soft-float
#                                                 musl blobs shipped by libcedarc
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOTFS=$HERE/rootfs
CEDAR=$HERE/libcedarc
BR=$HERE/buildroot-2023.02.3
export PATH=$BR/output/host/bin:$PATH

CROSS=$(ls "$BR/output/host/bin" 2>/dev/null | grep -m1 'linux-musl.*-gcc$' | sed 's/gcc$//')
[ -n "$CROSS" ] || { echo "musl toolchain not found - build buildroot first"; exit 1; }
echo "cross prefix: $CROSS"

mkdir -p "$ROOTFS/usr/lib" "$ROOTFS/usr/bin"

# ---------------------------------------------------------------- libcedarc
cd "$CEDAR"
if [ ! -f .cy_musl_built ]; then
	echo "== building libcedarc =="
	make distclean >/dev/null 2>&1 || true
	autoreconf -fi >/dev/null 2>&1
	./configure --host="${CROSS%-}" \
		CFLAGS="-O2 -g0" CXXFLAGS="-O2 -g0" \
		LDFLAGS="-L$CEDAR/library/toolchain-sunxi-arm9-musl" >/dev/null
	make -j4 >/dev/null
	touch .cy_musl_built
fi

echo "== installing libraries =="
for f in base/.libs/libcdc_base.so \
         memory/.libs/libMemAdapter.so \
         vdecoder/.libs/libvdecoder.so \
         library/toolchain-sunxi-arm9-musl/libawh264.so \
         library/toolchain-sunxi-arm9-musl/libVE.so \
         library/toolchain-sunxi-arm9-musl/libvideoengine.so; do
	[ -f "$f" ] || { echo "missing $f"; exit 1; }
	cp -f "$f" "$ROOTFS/usr/lib/"
	${CROSS}strip --strip-unneeded "$ROOTFS/usr/lib/$(basename "$f")" || true
done

# libgcc_s is needed by libawh264
if [ ! -f "$ROOTFS/lib/libgcc_s.so.1" ]; then
	sysroot=$("${CROSS}gcc" -print-sysroot)
	for cand in "$sysroot/lib/libgcc_s.so.1" "$sysroot/../lib/libgcc_s.so.1"; do
		[ -f "$cand" ] && cp -f "$cand" "$ROOTFS/lib/" && break
	done
fi

# ---------------------------------------------------------------- player
if [ -f "$HERE/apps/llp-play.c" ]; then
	echo "== building llp-play =="
	"${CROSS}gcc" -O2 -Wall -s -o "$ROOTFS/usr/bin/llp-play" "$HERE/apps/llp-play.c" \
		-I"$CEDAR/include" -I"$CEDAR/vdecoder/include" \
		-isystem "$("${CROSS}gcc" -print-sysroot)/usr/include/libdrm" \
		-L"$ROOTFS/usr/lib" -lMemAdapter -lvdecoder -lVE -lvideoengine -lcdc_base \
		-lfaad -lasound -ldrm -lpthread -lm
	chmod 0755 "$ROOTFS/usr/bin/llp-play"
fi

# ---------------------------------------------------------------- autoplay
cat > "$ROOTFS/etc/init.d/S99play" <<'INIT'
#!/bin/sh
# Mount the SD card and loop the first *.mp4 found in its root directory.
SD=/mnt/sd
LOG=/tmp/play.log

find_video() {
	for f in "$SD"/*.mp4 "$SD"/*.MP4 "$SD"/*.avi; do
		[ -f "$f" ] && { echo "$f"; return 0; }
	done
	return 1
}

mount_sd() {
	mkdir -p "$SD"
	for dev in /dev/mmcblk0p1 /dev/mmcblk0p2 /dev/mmcblk0; do
		[ -b "$dev" ] || continue
		mount -t auto "$dev" "$SD" 2>/dev/null && { echo "mounted $dev"; return 0; }
	done
	return 1
}

case "$1" in
	start)
		# Panel self test: colour bars, then a grid.  If the bars look wrong
		# the LCD timing is wrong; if they look right the fault is in the
		# decoder path.
		/usr/bin/lcdtest bars
		sleep 2
		/usr/bin/lcdtest grid
		sleep 2

		# stop fbcon from painting over the video
		for c in /sys/class/vtconsole/vtcon*; do
			[ -w "$c/bind" ] || continue
			grep -q "frame buffer" "$c/name" 2>/dev/null && echo 0 > "$c/bind"
		done

		printf "Autoplay: "
		if ! mount_sd; then echo "no SD card"; exit 0; fi
		v=$(find_video) || { echo "no mp4 on SD"; exit 0; }
		echo "playing $v"
		(
			while :; do
				/usr/bin/llp-play "$v" 2>&1 || sleep 10
				sleep 1
			done
		) &
		echo $! > /var/run/autoplay.pid
		;;
	stop)
		# Stop the restart loop first, otherwise it pulls the player back up
		# a second later and fights whatever the user starts by hand.
		if [ -f /var/run/autoplay.pid ]; then
			kill "$(cat /var/run/autoplay.pid)" 2>/dev/null
			rm -f /var/run/autoplay.pid
		fi
		killall llp-play 2>/dev/null
		for i in 1 2 3 4 5 6 7 8 9 10; do
			killall -0 llp-play 2>/dev/null || break
			sleep 1
		done
		killall -9 llp-play 2>/dev/null
		echo "autoplay stopped"
		;;
	restart)
		"$0" stop; "$0" start
		;;
	*)
		echo "Usage: $0 {start|stop|restart}"
		exit 1
		;;
esac
INIT
chmod 0755 "$ROOTFS/etc/init.d/S99play"

# make sure dlopen("libawh264.so") finds /usr/lib
echo "/lib:/usr/local/lib:/usr/lib" > "$ROOTFS/etc/ld-musl-arm.path"

echo "== multimedia installed =="
du -sh "$ROOTFS/usr/lib" "$ROOTFS/usr/bin" 2>/dev/null
