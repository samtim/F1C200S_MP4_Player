# LiuLianPi F1C200S Video Player Firmware

A firmware built from scratch for the Allwinner F1C200S/F1C100S: **boots from 16 MB SPI NOR, drives a 1024×600 panel, autoplays an MP4 from the SD card root, ships the WiFi driver, and uses the VE for hardware H.264 decoding plus the display engine for hardware colour conversion**.

---

## 1. Hardware

| Item | Detail |
|---|---|
| SoC | Allwinner F1C200S (F1C100S family), ARM926EJ-S @ 408 MHz, no NEON, no FPU |
| DRAM | 64 MB DDR1 |
| Storage | 16 MB SPI NOR (GD25Q128) |
| Panel | 7" 1024×600 TTL RGB888 (BI50LA7002-27CD), DE mode, data latched on the **falling DCLK edge**, 18-bit input with R/G/B low 2 bits grounded |
| WiFi | RTL8188FU (USB) |
| Video engine | VE, H.264 hardware decode up to 720p30 |

Panel timing (verified on hardware):

```
pclk    51.2 MHz
htotal  1344    hsync 24   hbp 160   hfp 136
vtotal  635     vsync 2    vbp 21    vfp 12
polarity  HSYNC and VSYNC both positive
```

---

## 2. Current state

| Requirement | Status |
|---|---|
| Boot from 16 MB NOR flash | ✅ SPL + u-boot + kernel + jffs2 rootfs |
| Drive the 1024×600 panel | ✅ sun4i-drm with a custom panel-simple entry |
| Autoplay MP4 from SD root | ✅ `/etc/init.d/S99play` mounts the card and loops playback |
| WiFi driver from the SDK | ✅ rtl8188fu, loaded at boot |
| VE hardware decoding | ✅ cedar + libcedarc, MB32 tiled output |
| Audio | ✅ faad2 (fixed point) + ALSA, A/V within ±50 ms |
| Variable speed playback | ⚠️ audio is accurate; above 1.5× the panel framerate hits the hardware ceiling and frames are skipped |

Measured performance (1024×600 @ 24 fps source):

```
VE decode        16 – 20 ms/frame
MB32 → YUYV      13 – 22 ms/frame (depends on vertical sampling)
DE colour conv    0 ms (hardware)
AAC decode        2 – 3 ms/frame (fixed point)
```

The whole pipeline sustains roughly 24 – 30 fps, which is enough for 24 fps material.

---

## 3. Repository layout

```
F1C200S/
├── u-boot/                     u-boot v2025.07-rc2 (monke), SPL SPI NOR boot
├── linux-5.15.y/               kernel 5.15.202 (monke/linux-5.15.y)
├── buildroot-2023.02.3/        root filesystem (musl / arm926t / soft-float)
│   ├── configs/liulianpi_f1c100s_defconfig
│   └── package/faad2/0001-enable-fixed-point.patch   ← important
├── cedar/                      VE kernel driver, ported from 5.4 to 5.15
├── libcedarc/                  CedarX userspace decoder (aodzip), built for musl
├── buildroot-tiny200/          reference project (not used directly)
├── buildroot-epass/            reference project, source of the suniv DE patches
├── apps/
│   ├── llp-play.c              the player (single file, ~2200 lines)
│   ├── lcdtest.c               panel self test (colour bars / grid / checker)
│   ├── install-multimedia.sh   installs cedar, builds the player, writes S99play
│   └── mp4test.c               host-side demuxer verification tool
├── install-wifi.sh             WiFi driver and firmware installation
├── install-apps.sh             small helper programs
├── BuildMyImage.sh             assembles the 16 MB image
└── sample.mp4                  test clip
```

---

## 4. Building

### 4.0 Getting the upstream sources

This repository holds only the code written for this board plus the patches applied to the upstream projects. The trees themselves are not vendored; fetch them separately at the revisions below.

| Component | Upstream | Revision used |
|---|---|---|
| u-boot | <https://gitee.com/monke/u-boot> | `d500829fa769e8e24729e83d3fd461216df72fb8` |
| kernel | <https://gitee.com/monke/linux-5.15.y> | `8f258122d805e71f6c69f538bcbd5c1d7cad3f85` |
| VE kernel driver | <https://gitee.com/monke/cedar> | `1c09a3cf63d4282eb237c60bf0a60b0350e8e673` (unmodified) |
| libcedarc | <https://github.com/aodzip/libcedarc> | `e4246be521203adb2d93d52482239044a7f9b6fe` |
| buildroot | <https://buildroot.org/download.html> | 2023.02.3 |
| faad2 | pulled by buildroot | 2.10.1 |

```sh
git clone https://gitee.com/monke/u-boot.git
git clone https://gitee.com/monke/linux-5.15.y.git
git clone https://gitee.com/monke/cedar.git
git clone https://github.com/aodzip/libcedarc.git
wget https://buildroot.org/downloads/buildroot-2023.02.3.tar.gz && tar xf buildroot-2023.02.3.tar.gz

# u-boot: 1024x600 panel mode
git -C u-boot apply ../patches/u-boot/0001-panel-1024x600.patch

# kernel: board support + the cedar VE driver ported from 5.4
git -C linux-5.15.y apply ../patches/linux/0001-liulianpi-board-support.patch
git -C linux-5.15.y apply ../patches/linux/0002-cedar-ve-driver-port.patch
cp configs/kernel.config linux-5.15.y/.config

# libcedarc: build it for musl, quiet its logging
git -C libcedarc apply ../patches/libcedarc/0001-build-for-musl.patch

# buildroot: board defconfig + the faad2 fixed-point patch
cp patches/buildroot/liulianpi_f1c100s_defconfig buildroot-2023.02.3/configs/
cp patches/buildroot/0001-enable-fixed-point.patch buildroot-2023.02.3/package/faad2/
```

`patches/linux/0002-cedar-ve-driver-port.patch` is large because it adds the whole `drivers/staging/media/sunxi/cedar/` directory. It is ported from monke's 5.4 tree and keeps its original licence headers.

### 4.1 Toolchains

```sh
# u-boot and kernel: Linaro 7.2.1
export PATH=/opt/gcc-linaro-7.2.1-2017.11-x86_64_arm-linux-gnueabi/bin:$PATH
# rootfs and applications: the musl toolchain buildroot generates
#   buildroot-2023.02.3/output/host/bin/arm-buildroot-linux-musleabi-
```

### 4.2 u-boot

```sh
cd u-boot
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- liulianpi_f1c100s_norflash_defconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- -j8
```

Panel parameters live in `.config`:

```
CONFIG_VIDEO_LCD_MODE="x:1024,y:600,depth:18,pclk_khz:51200,le:136,ri:160,up:21,lo:12,hs:24,vs:2,sync:3,vmode:0"
CONFIG_VIDEO_LCD_DCLK_PHASE=0
```

### 4.3 Kernel

```sh
cd linux-5.15.y
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- suniv_f1c100s_liulianpi_spinor_defconfig
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- -j6
```

Relevant options:

```
CONFIG_VIDEO_SUNXI_CEDAR_VE=y
CONFIG_VIDEO_SUNXI_CEDAR_ION=y
CONFIG_CMA=y / CONFIG_CMA_SIZE_MBYTES=24
CONFIG_FRAMEBUFFER_CONSOLE=y
CONFIG_EXFAT_FS / MSDOS_FS / NLS_UTF8    (SD card)
```

### 4.4 Root filesystem

```sh
cd buildroot-2023.02.3
make liulianpi_f1c100s_defconfig
make -j8          # about 40 minutes from scratch
```

### 4.5 Assembling the image

```sh
cd ~/F1C200S
sudo ./BuildMyImage.sh
# produces f1c100s_spiflash_16M.bin
```

The script runs `install-wifi.sh`, `install-apps.sh` and `install-multimedia.sh`, then lays the pieces out as follows:

| Offset | Content | Size |
|---|---|---|
| `0x000000` | u-boot (+SPL) | 1 MB |
| `0x100000` | device tree | 64 KB |
| `0x110000` | zImage | 5 MB |
| `0x610000` | root filesystem (jffs2) | remaining ~9.94 MB |

> The partitions must match the `mtdparts` on the kernel command line exactly. An early mismatch showed up as `No working init found` after boot.

---

## 5. Flashing

```sh
# with the board in FEL mode
sudo sunxi-fel -p spiflash-write 0 f1c100s_spiflash_16M.bin
```

or use u-boot's `sf` command over TFTP/USB.

---

## 6. Using the player

```sh
llp-play /mnt/sd/clip.mp4
```

### 6.1 Environment variables

| Variable | Effect |
|---|---|
| `LLP_RATE=1.5` | playback speed, 0.25 – 4.0. Audio is sped up by **changing the PCM sample rate**, never by resampling |
| `LLP_FULL=1` | full detail (convert every row; slowest) |
| `LLP_BENCH=1` | display self test: draws SMPTE colour bars and **never touches the decoder**, to tell display problems from decoder problems |
| `LLP_OUT=565` | use DRM + RGB565 with CPU colour conversion (fallback if the DE YUV path misbehaves) |
| `LLP_OUT=fb` | use the classic `/dev/fb0` path with software conversion |
| `LLP_BLOCKFLIP=1` | make page flips block (debugging) |
| `LLP_LDM=1` | load tile rows with LDM bursts (measured: no difference) |

### 6.2 Stopping playback

```sh
# the autoplay instance is started by init, so Ctrl-C never reaches it
touch /tmp/llp.stop
# or
/etc/init.d/S99play stop
```

The player is **single instance** (flock); a second one refuses to start and says so.

### 6.3 Status line

```
frame 96 fps=24.0 drop=2 draw=24.1 conv=22.1 flip=2.1 dec=16.0 feed=0.7 ret=0.0 rb=0 av=-33 ur=0
```

| Field | Meaning |
|---|---|
| `fps` | actual playback rate (should match the source) |
| `drop` | frames skipped to stay with the audio clock |
| `conv` | MB32 → YUYV conversion time |
| `dec` | VE decode time |
| `feed` | sample read + bitstream submit time |
| `flip` | page flip time |
| `av` | **A/V offset in milliseconds**, positive = picture ahead |
| `ur` | ALSA underrun count (should stay 0) |
| `rb` | timeline rebase count |

---

## 7. Data flow

```
SD card MP4 ──mmap──> demuxer (self contained, see llp-play.c)
                        │
                        ├─ H.264 ──> VE decode ──> MB32 tiled YUV420
                        │                              │
                        │                 CPU de-tile → YUYV (16bpp)
                        │                              │
                        │                   DRM/KMS double/triple buffer + flip
                        │                              │
                        │                     DE hardware CSC + scanout
                        │                              ▼
                        │                            panel
                        └─ AAC ──> faad2 (fixed point) ──> ALSA ──> codec
```

---

## 8. Problems solved along the way

This is the most valuable part of the project; every item was found on real hardware.

### 8.1 The YUV address register means something different on suniv

**Symptom**: a packed YUV layer through DRM produced snow, while RGB565 worked.

**Cause**: mainline writes a **byte** address to `SUN4I_BACKEND_IYUVADD_REG(0)` as it does on A10/A20, but the F1C100s register takes a **bit** address. The address was short by three bits, so the display engine scanned memory starting at 1/8 of the correct physical address.

**Fix** (`drivers/gpu/drm/sun4i/sun4i_backend.c`):

```c
-	regmap_write(backend->engine.regs, SUN4I_BACKEND_IYUVADD_REG(0), paddr);
+	regmap_write(backend->engine.regs, SUN4I_BACKEND_IYUVADD_REG(0),
+		     paddr << 3);
```

Source: `buildroot-epass/board/rhodesisland/epass/patch/linux/0003-f1c100s-defe-debe-fix.patch`

### 8.2 The VE outputs MB32 tiles, not planar frames

`PIXEL_FORMAT_YUV_MB32_420 = 7`: each 32×32 pixel block occupies 1024 contiguous bytes, rows inside a block are contiguous.

```
luma:    block (tx, ty) = base + (ty * mbw + tx) * 1024 + row * 32 + col
chroma:  32 bytes hold 16 interleaved (U, V) pairs, U in the even byte
```

libcedarc's own `ConvertMb32420ToNv21Y/C` is NEON assembly behind `CEDARX_DECODER_ARM32`, unusable on ARMv5, so the converter here is hand written.

### 8.3 ION buffers are uncached, so memory bandwidth is about 20 MB/s

`ion_cma_heap` maps with `dma_mmap_coherent()`, i.e. a coherent, non-cacheable mapping. On ARM926 a single uncached 32-bit read costs roughly 87 cycles.

**Consequence**: moving the 2.1 MB a frame needs (933 KB read + 1.2 MB write) costs 50 – 70 ms of CPU. That is the performance ceiling of the whole board, and the reason the converter touches as few pixels as possible: it only reshuffles bytes and leaves colour conversion to the display engine.

Measured split: writing 1.2 MB takes only 8.8 ms (write combining works), reading 933 KB takes ~50 ms. **Reads are the bottleneck**, which is why vertical decimation is the most effective optimisation.

### 8.4 `memcpy` degenerates into byte copies through a byte pointer

```c
memcpy(yw, yl + j * 1024, 32);   /* yl is uint8_t*, so gcc cannot assume alignment → 32 single byte reads */
```

Using an explicit `const uint32_t *` cuts the number of bus transactions by four. Note also that `LDM`/`STM` do **not** merge into bursts on ARM926 non-cacheable memory — measured to be equivalent to (sometimes slower than) separate accesses, so they are not used.

### 8.5 faad2 defaults to floating point, which is tens of times slower on a soft-float ARM9

`libfaad/common.h` ships with `//#define FIXED_POINT` commented out, so AAC decoding ran in floating point and every multiply-add became a call into the `__aeabi_fmul` software emulation. The audio thread was costing about 12 ms per video frame.

With `package/faad2/0001-enable-fixed-point.patch` applied, the number of soft-float calls in `libfaad.so` drops to **zero** and audio CPU falls to 2 – 3 ms.

**This is a general problem**: any faad2 build for a soft-float ARM9 should enable fixed point.

### 8.6 Variable speed: change the PCM rate, do not resample

`sun4i-codec` accepts **continuous** rates from 8000 to 192000 Hz (`SNDRV_PCM_RATE_CONTINUOUS`). So changing speed is just `sample_rate × speed`, with the PCM data passed through untouched: no resampling, no CPU cost, no artefacts, only the expected pitch shift.

When the card cannot provide the requested rate (for example 48 kHz × 1.5 = 72 kHz, where it offers 96 kHz), the resampling ratio must be

```
media samples per device sample = (media rate × speed) / actual rate
```

Using the speed itself as the ratio is wrong and makes the audio run at three times speed.

### 8.7 Audio hiss: the input mixers were being enabled

The F1C100S codec is an "input mixer → output mixer" arrangement. An early version switched every playback control on to make sure something was audible, which also enabled **capture** stages such as `Left Mixer Mic In switch`, mixing the microphone preamp noise straight into the output.

Those controls are named `Left Mixer …` / `Right Mixer …`, so matching only a `Mic` prefix misses them; the name has to be searched in full.

### 8.8 A/V sync: audio must not start before the video clock exists

The first frame takes about 500 ms to decode. If the audio thread starts writing immediately it runs half a second ahead forever. The audio thread now waits until the video time base is established.

And **never "rebase" the timeline to forgive lateness** — that accepts the lag as permanent and the video drifts ever further behind. The correct response is to **skip rendering a frame** (keep decoding) so the timeline actually catches up.

### 8.9 Pitfall in the frame dropping logic

Pacing on `pts` breaks on streams with B frames: the decoder hands pictures over out of presentation order, `pts` steps backwards, the target lands in the past and those frames are judged "late" and thrown away.

Pacing is therefore done by **presentation slot**: `target = t0 + (first_pts + slot × interval) / speed`, with the interval taken from `stts`/`dts`.

### 8.10 `S99play stop` did not kill the restart loop

```sh
( while :; do llp-play; sleep 1; done ) &     # killall llp-play does not touch this subshell
```

The loop PID has to be recorded:

```sh
) &
echo $! > /var/run/autoplay.pid
```

Without that, manual runs fight an invisible instance over the VE and the CMA pool, presenting as `done, 0 frames` plus `the first slice of the frame is not 0`.

---

## 9. Known limitations

1. **Frames are skipped above 1.5× speed**: `dec 16 ms + conv 22 ms` already fills the 24 fps budget, while 1.5× needs 36 fps of throughput. Audio stays accurate; the picture judders.
2. **A 24 fps source is close to the limit**: a handful of dropped frames is normal; 30 fps material is worse.
3. **Tight rootfs**: 7.4 MB of content in a 9.94 MB jffs2 partition.
4. **`ip: can't find device 'lo'`**: buildroot's stock S40network configuration, harmless.
5. **cedarc `debug :` log spam**: emitted by the prebuilt `libVE.so` / `libvideoengine.so`, cannot be turned off.
6. `panel-simple: Specify missing connector_type`: harmless warning.

### If more headroom is needed

The real fix is **letting the display engine scale**: convert only 512×300 on the CPU and let DEFE (the display frontend) scale it up to 1024×600, cutting both reads and writes by four. That requires porting the scaler coefficient handshake fix from `0003-f1c100s-defe-debe-fix.patch` — mainline writes the coefficient RAM without waiting for `COEF_ACCESS`, which produces snow on the F1C100s.

The second option is **mapping the ION buffers as cached**, worth 5 – 8× on reads, but it also requires fixing ion's flush ioctl (it currently calls `dmac_flush_range()` on a *user* virtual address on ARMv5, which is wrong). Higher risk.

---

## 10. Quick troubleshooting

| Symptom | Check |
|---|---|
| Black screen | `LLP_BENCH=1` for colour bars. Bars fine → decoder/conversion; bars black too → display path |
| Snow / garbage | make sure the kernel carries the `paddr << 3` fix |
| `done, 0 frames` | another instance is running. `ps | grep llp`, `reboot` if the VE is wedged |
| Choppy picture | look at `fps`, `drop`, `conv`, `dec` in the status line |
| Hiss or crackle | check the `mixer: on` list for capture stages, and whether `ur` grows |
| A/V out of sync | look at `av`; make sure `audio: synced to video clock` appears |
| Distorted audio at non-1× speed | look at `audio: card gave ... (step ...)`; `step` must equal `media × speed / actual` |

---

## 11. References

- Build environment: <http://doc.liulianpi.cn/web/#/5/23>
- u-boot / kernel / cedar: <https://gitee.com/monke>
- libcedarc: <https://github.com/aodzip/libcedarc>
- suniv display engine patches: <https://github.com/inapp123/buildroot-epass>

---

## 12. License

The code written for this project - the player, the helper programs, the build
scripts and the patches - is released under the **GNU General Public License,
version 2** (`GPL-2.0`). The full text is in [LICENSE](LICENSE).

GPL-2.0 is not an arbitrary choice here; it is the only version compatible with
everything this firmware links against:

| Component | Licence |
|---|---|
| this project (player, scripts, patches) | GPL-2.0 |
| Linux kernel | GPL-2.0 only (with the syscall note) |
| u-boot | GPL-2.0 or later |
| buildroot | GPL-2.0 or later |
| **faad2** (AAC decoder, linked into the player) | **GPL-2.0** - this alone forces the player to be GPL |
| alsa-lib | LGPL-2.1 or later |
| libdrm | MIT |
| libcedarc / cedar | see their own repositories |

Because the player is linked against faad2, any binary built from this source
must be distributed under GPL-2.0 as well. If you ship the firmware image, ship
the corresponding source too.

Prebuilt firmware images and vendor documentation are deliberately **not** part
of this repository; see `.gitignore`.
