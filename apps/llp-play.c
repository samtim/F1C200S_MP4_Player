/* SPDX-License-Identifier: GPL-2.0 */
/*
 * llp-play - minimal MP4 (H.264) player for LiuLianPi / F1C200S.
 *
 *   llp-play /mnt/sd/sample.mp4 [-v]
 *
 * Decoding is done by the Allwinner VE through libcedarc (libvdecoder +
 * libawh264).  Frames come back as YUV420 planar and are converted to the
 * framebuffer pixel format in software, then blitted to /dev/fb0.
 *
 * The MP4 demuxer is self contained: only what an "avc1" video track needs
 * (stsd/avcC, stts, ctts, stsc, stsz, stco/co64, mdhd) is parsed.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/resource.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "vdecoder.h"
#include "memoryAdapter.h"
#include "sc_interface.h"

/* The mp4 lives on a FAT SD card.  Reading one sample per frame with
 * pread() costs more than the decode itself on this board, so the whole
 * file is mapped once and samples are copied out of the page cache. */
static const uint8_t *g_map;
static size_t g_map_len;

static int map_file(const char *path)
{
	struct stat st;
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return -1;
	if (fstat(fd, &st) == 0 && st.st_size > 0) {
		g_map_len = (size_t)st.st_size;
		g_map = mmap(NULL, g_map_len, PROT_READ, MAP_PRIVATE, fd, 0);
		if (g_map == MAP_FAILED) {
			g_map = NULL;
			g_map_len = 0;
		}
	}
	close(fd);
	if (!g_map)
		return -1;
	printf("file: mapped %u bytes\n", (unsigned)g_map_len);
	return 0;
}

static ssize_t file_pread(int fd, void *dst, size_t n, off_t off)
{
	if (g_map && off >= 0 && (size_t)off + n <= g_map_len) {
		memcpy(dst, g_map + (size_t)off, n);
		return (ssize_t)n;
	}
	return pread(fd, dst, n, off);
}


/* ------------------------------------------------------------------ */
/* MP4 demuxer                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
	uint32_t *offs;    /* per-sample file offset */
	uint32_t *sizes;   /* per-sample size */
	int64_t  *dts;     /* decode time, us */
	int64_t  *cts;     /* composition time, us */
	int       n;

	int       width, height;
	uint8_t  *avcc;    /* avcC payload */
	int       avcc_len;
	int       nal_size; /* 1, 2 or 4 */
	uint8_t  *asc;     /* AudioSpecificConfig (audio track) */
	int       asc_len;
} track_t;

static uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static const uint8_t *box_find(const uint8_t *p, const uint8_t *end, const char *type,
			       uint32_t *plen)
{
	while (p + 8 <= end) {
		uint32_t size = rd32(p);
		const uint8_t *payload = p + 8;
		int hdr = 8;

		if (size == 1) {
			if (p + 16 > end)
				return NULL;
			size = rd32(p + 8);
			hdr = 16;
			payload = p + 16;
		} else if (size == 0) {
			size = (uint32_t)(end - p);
		}
		if (size < (uint32_t)hdr || p + size > end)
			return NULL;
		if (!memcmp(p + 4, type, 4)) {
			if (plen)
				*plen = size - hdr;
			return payload;
		}
		p += size;
	}
	return NULL;
}

static const uint8_t *box_path(const uint8_t *p, uint32_t len, const char *path,
			       uint32_t *plen)
{
	char name[5];
	const char *s = path;

	while (*s) {
		int i = 0;

		while (*s && *s != '/' && i < 4)
			name[i++] = *s++;
		name[i] = 0;
		if (*s == '/')
			s++;
		p = box_find(p, p + len, name, &len);
		if (!p)
			return NULL;
	}
	if (plen)
		*plen = len;
	return p;
}

static int parse_stbl(const uint8_t *stbl, uint32_t len, track_t *t, uint32_t timescale)
{
	const uint8_t *p;
	uint32_t l;
	int i, j;
	uint32_t *chunk_off = NULL;
	uint32_t n_chunks = 0;
	uint32_t *dts_delta = NULL, *dts_count = NULL;
	uint32_t n_dts = 0;
	int rc = -1;

	/* --- sample sizes --- */
	p = box_find(stbl, stbl + len, "stsz", &l);
	if (!p || l < 12)
		return -1;
	{
		uint32_t fixed = rd32(p + 4);
		uint32_t count = rd32(p + 8);

		if (count == 0 || count > 2000000)
			return -1;
		t->n = (int)count;
		t->sizes = calloc(count, sizeof(uint32_t));
		if (!t->sizes)
			return -1;
		for (i = 0; i < (int)count; i++) {
			if (fixed) {
				t->sizes[i] = fixed;
			} else if ((uint32_t)(12 + 4 * i + 4) <= l) {
				t->sizes[i] = rd32(p + 12 + 4 * i);
			} else {
				return -1;
			}
		}
	}

	/* --- chunk offsets --- */
	p = box_find(stbl, stbl + len, "stco", &l);
	if (p && l >= 8) {
		n_chunks = rd32(p + 4);
		chunk_off = calloc(n_chunks ? n_chunks : 1, sizeof(uint32_t));
		if (!chunk_off)
			return -1;
		for (i = 0; i < (int)n_chunks; i++) {
			if ((uint32_t)(8 + 4 * i + 4) > l)
				goto out;
			chunk_off[i] = rd32(p + 8 + 4 * i);
		}
	} else {
		p = box_find(stbl, stbl + len, "co64", &l);
		if (!p || l < 8)
			goto out;
		n_chunks = rd32(p + 4);
		chunk_off = calloc(n_chunks ? n_chunks : 1, sizeof(uint32_t));
		if (!chunk_off)
			return -1;
		for (i = 0; i < (int)n_chunks; i++) {
			if ((uint32_t)(8 + 8 * i + 8) > l)
				goto out;
			chunk_off[i] = rd32(p + 8 + 8 * i + 4);
		}
	}

	/* --- sample to chunk --- */
	{
		const uint8_t *stsc = box_find(stbl, stbl + len, "stsc", &l);
		uint32_t n_stsc;

		if (!stsc || l < 8)
			goto out;
		n_stsc = rd32(stsc + 4);

		t->offs = calloc(t->n ? t->n : 1, sizeof(uint32_t));
		if (!t->offs)
			goto out;

		uint32_t sample = 0;

		for (i = 0; i < (int)n_stsc && sample < (uint32_t)t->n; i++) {
			uint32_t first = rd32(stsc + 8 + 12 * i);
			uint32_t per = rd32(stsc + 12 + 12 * i);
			uint32_t next = (i + 1 < (int)n_stsc) ? rd32(stsc + 8 + 12 * (i + 1))
							      : n_chunks + 1;

			if (first < 1)
				first = 1;
			for (uint32_t ch = first; ch < next && ch <= n_chunks &&
					     sample < (uint32_t)t->n; ch++) {
				uint32_t off = chunk_off[ch - 1];

				for (j = 0; j < (int)per && sample < (uint32_t)t->n; j++) {
					t->offs[sample] = off;
					off += t->sizes[sample];
					sample++;
				}
			}
		}
		if (sample != (uint32_t)t->n)
			fprintf(stderr, "warning: only mapped %u of %d samples\n", sample, t->n);
	}

	/* --- decode times --- */
	{
		const uint8_t *stts = box_find(stbl, stbl + len, "stts", &l);
		int64_t dts = 0;
		uint32_t s = 0;

		if (!stts || l < 8)
			goto out;
		n_dts = rd32(stts + 4);
		dts_count = calloc(n_dts ? n_dts : 1, sizeof(uint32_t));
		dts_delta = calloc(n_dts ? n_dts : 1, sizeof(uint32_t));
		t->dts = calloc(t->n ? t->n : 1, sizeof(int64_t));
		t->cts = calloc(t->n ? t->n : 1, sizeof(int64_t));
		if (!dts_count || !dts_delta || !t->dts || !t->cts)
			goto out;
		for (i = 0; i < (int)n_dts; i++) {
			if ((uint32_t)(8 + 8 * i + 8) > l)
				goto out;
			dts_count[i] = rd32(stts + 8 + 8 * i);
			dts_delta[i] = rd32(stts + 12 + 8 * i);
		}
		for (i = 0; i < (int)n_dts && s < (uint32_t)t->n; i++) {
			for (j = 0; j < (int)dts_count[i] && s < (uint32_t)t->n; j++) {
				t->dts[s] = dts * 1000000LL / (int64_t)timescale;
				dts += dts_delta[i];
				s++;
			}
		}
	}

	/* --- composition offsets --- */
	{
		const uint8_t *ctts = box_find(stbl, stbl + len, "ctts", &l);
		uint32_t c = 0;

		if (ctts && l >= 8) {
			uint32_t n = rd32(ctts + 4);

			for (i = 0; i < (int)n && c < (uint32_t)t->n; i++) {
				uint32_t cnt = rd32(ctts + 8 + 8 * i);
				int32_t off = (int32_t)rd32(ctts + 12 + 8 * i);

				for (j = 0; j < (int)cnt && c < (uint32_t)t->n; j++, c++)
					t->cts[c] = t->dts[c] + (int64_t)off * 1000000LL /
							       (int64_t)timescale;
			}
		}
		for (i = 0; i < t->n; i++)
			if (t->cts[i] == 0 && t->dts[i] != 0)
				t->cts[i] = t->dts[i];
	}

	rc = 0;
out:
	free(chunk_off);
	free(dts_count);
	free(dts_delta);
	return rc;
}

/* Walk the esds descriptor tree and copy out the AudioSpecificConfig
 * (DecoderSpecificInfo, tag 0x05). */
static int desc_len(const uint8_t **pp, const uint8_t *end)
{
	int len = 0, b;

	do {
		if (*pp >= end)
			return -1;
		b = *(*pp)++;
		len = (len << 7) | (b & 0x7f);
	} while (b & 0x80);
	return len;
}

static int find_desc(const uint8_t *p, const uint8_t *end, int want,
		     uint8_t **out, int *outlen, int depth)
{
	if (depth > 5)
		return -1;
	while (p + 2 <= end) {
		int tag = *p++;
		int len = desc_len(&p, end);

		if (len < 0 || p + len > end)
			return -1;
		if (tag == want) {
			*out = malloc((size_t)len);
			if (!*out)
				return -1;
			memcpy(*out, p, (size_t)len);
			*outlen = len;
			return 0;
		}
		if (tag == 0x03 && len > 3) {
			/* ES_Descriptor: ES_ID(2) + flags(1) then children */
			if (find_desc(p + 3, p + len, want, out, outlen, depth + 1) == 0)
				return 0;
		} else if (tag == 0x04 && len > 13) {
			/* DecoderConfigDescriptor: 13 byte header then children */
			if (find_desc(p + 13, p + len, want, out, outlen, depth + 1) == 0)
				return 0;
		}
		p += len;
	}
	return -1;
}

static int esds_get_asc(const uint8_t *esds, uint32_t len, uint8_t **out, int *outlen)
{
	const uint8_t *p = esds, *end = esds + len;

	if (len < 4)
		return -1;
	p += 4;                          /* version + flags */
	return find_desc(p, end, 0x05, out, outlen, 0);
}

static int mp4_parse(const char *path, track_t *t, track_t *a)
{
	int fd = open(path, O_RDONLY);
	uint64_t fsize;
	uint8_t *moov = NULL;
	uint32_t moov_len = 0;
	int have_moov = 0;
	int got_track = 0;
	int got_audio = 0;

	memset(t, 0, sizeof(*t));
	t->nal_size = 4;
	if (fd < 0) {
		perror(path);
		return -1;
	}

	fsize = (uint64_t)lseek(fd, 0, SEEK_END);

	uint64_t pos = 0;

	while (pos + 8 <= fsize) {
		uint8_t hdr[16];
		uint32_t size;
		int hlen = 8;

		if (file_pread(fd, hdr, 8, (off_t)pos) != 8)
			break;
		size = rd32(hdr);
		if (size == 1) {
			if (file_pread(fd, hdr + 8, 8, (off_t)(pos + 8)) != 8)
				break;
			size = rd32(hdr + 8);
			hlen = 16;
		} else if (size == 0) {
			size = (uint32_t)(fsize - pos);
		}
		if (size < 8)
			break;
		if (!memcmp(hdr + 4, "moov", 4)) {
			moov_len = size - hlen;
			moov = malloc(moov_len);
			if (!moov)
				goto out;
			if (file_pread(fd, moov, moov_len, (off_t)(pos + hlen)) != (ssize_t)moov_len)
				goto out;
			have_moov = 1;
			break;
		}
		pos += size;
	}
	if (!have_moov)
		goto out;

	{
		const uint8_t *p = moov;
		const uint8_t *end = moov + moov_len;

		while (p + 8 <= end) {
			uint32_t size = rd32(p);
			const uint8_t *trak, *hdlr, *mdhd, *stbl, *stsd, *entry;
			uint32_t tl, hl, l, spl, esize, timescale = 1000;
			int is_video, is_audio;

			if (size < 8 || p + size > end)
				break;
			if (memcmp(p + 4, "trak", 4))
				goto next_trak;

			trak = p + 8;
			tl = size - 8;

			hdlr = box_path(trak, tl, "mdia/hdlr", &hl);
			if (!hdlr || hl < 12)
				goto next_trak;
			is_video = !memcmp(hdlr + 8, "vide", 4);
			is_audio = !memcmp(hdlr + 8, "soun", 4);
			if (is_video && got_track)
				goto next_trak;
			if (is_audio && (got_audio || !a))
				goto next_trak;
			if (!is_video && !is_audio)
				goto next_trak;

			mdhd = box_path(trak, tl, "mdia/mdhd", &l);
			if (mdhd && l >= 20)
				timescale = (mdhd[0] == 1) ? rd32(mdhd + 20) : rd32(mdhd + 12);
			if (!timescale)
				timescale = 1000;

			stbl = box_path(trak, tl, "mdia/minf/stbl", &spl);
			if (!stbl)
				goto next_trak;

			stsd = box_find(stbl, stbl + spl, "stsd", &l);
			if (!stsd || l < 16)
				goto next_trak;
			entry = stsd + 8;
			esize = rd32(entry);
			if (esize < 8 || entry + esize > stbl + spl)
				goto next_trak;

			if (is_video) {
				t->width = rd16(entry + 8 + 24);
				t->height = rd16(entry + 8 + 26);
				if (esize >= 86) {
					/* video sample entry: 86 byte header, then child boxes */
					const uint8_t *avcc = box_find(entry + 86, entry + esize,
								       "avcC", &l);

					if (avcc && l > 6) {
						t->avcc = malloc(l);
						if (!t->avcc)
							goto next_trak;
						memcpy(t->avcc, avcc, l);
						t->avcc_len = (int)l;
						t->nal_size = (avcc[4] & 3) + 1;
					}
				}
				if (parse_stbl(stbl, spl, t, timescale) < 0)
					goto next_trak;
				got_track = 1;
			} else {
				/* audio sample entry ('mp4a'): 36 byte header, then 'esds' */
				const uint8_t *esds = esize > 36 ?
					box_find(entry + 36, entry + esize, "esds", &l) : NULL;
				uint8_t *asc = NULL;
				int asc_len = 0;

				if (esds && esds_get_asc(esds, l, &asc, &asc_len) == 0) {
					a->asc = asc;
					a->asc_len = asc_len;
				}
				if (parse_stbl(stbl, spl, a, timescale) < 0)
					goto next_trak;
				got_audio = 1;
			}
next_trak:
			p += size;
		}
	}

out:
	free(moov);
	close(fd);
	return got_track ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* framebuffer                                                         */
/* ------------------------------------------------------------------ */

static int fb_fd = -1;
static uint8_t *fb_mem;
static struct fb_var_screeninfo fb_var;
static struct fb_fix_screeninfo fb_fix;
static int out_w, out_h, out_bpp;

static int vx, vy, vw, vh;
static int *xmap;
static int *ymap;

static int fb_open(void)
{
	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0) {
		perror("/dev/fb0");
		return -1;
	}
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_var) ||
	    ioctl(fb_fd, FBIOGET_FSCREENINFO, &fb_fix)) {
		perror("fb ioctl");
		return -1;
	}
	out_w = fb_var.xres;
	out_h = fb_var.yres;
	out_bpp = fb_var.bits_per_pixel;
	fb_mem = mmap(NULL, fb_fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb_mem == MAP_FAILED) {
		perror("mmap fb");
		return -1;
	}
	return 0;
}

static void fb_clear(void)
{
	memset(fb_mem, 0, (size_t)fb_fix.line_length * out_h);
}

static void fb_layout(int src_w, int src_h)
{
	if (src_w <= 0 || src_h <= 0)
		return;
	if ((unsigned long long)src_w * out_h > (unsigned long long)src_h * out_w) {
		vw = out_w;
		vh = (int)((unsigned long long)out_w * src_h / src_w);
	} else {
		vh = out_h;
		vw = (int)((unsigned long long)out_h * src_w / src_h);
	}
	vx = (out_w - vw) / 2;
	vy = (out_h - vh) / 2;
	if (vw <= 0 || vh <= 0)
		return;

	free(xmap);
	free(ymap);
	xmap = malloc(sizeof(int) * vw);
	ymap = malloc(sizeof(int) * vh);
	if (!xmap || !ymap)
		return;
	for (int i = 0; i < vw; i++)
		xmap[i] = (int)((long long)i * src_w / vw);
	for (int i = 0; i < vh; i++)
		ymap[i] = (int)((long long)i * src_h / vh);
}

static inline int clamp8(int v)
{
	return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* ------------------------------------------------------------------ */
/* MB32 (tiled) -> NV21                                               */
/*                                                                    */
/* The F1C100S VE only writes tiled "MB32" surfaces: the luma plane   */
/* is a raster sequence of 32x32 tiles (32 rows of 32 bytes each) and */
/* the chroma plane is a raster sequence of 32x32 byte tiles holding  */
/* interleaved UV pairs.  This mirrors libcedarc's                   */
/* ConvertMb32420ToNv21Y/C(), which is compiled out for ARMv5.        */
/* ------------------------------------------------------------------ */

static uint8_t *nv_buf;
static size_t nv_buf_size;

static void mb32_to_nv21_y(const uint8_t *src, uint8_t *dst, int w, int h)
{
	int stride = (w + 15) & ~15;
	int mbw = ((w + 31) & ~31) / 32;
	int mbh = ((h + 31) & ~31) / 32;
	const uint8_t *p = src;
	int i, j, m;

	for (i = 0; i < mbh; i++)
		for (j = 0; j < mbw; j++)
			for (m = 0; m < 32; m++) {
				int line = i * 32 + m;
				int off, n;

				if (line >= h) {
					p += 32;
					continue;
				}
				off = line * stride + j * 32;
				n = stride - j * 32;
				if (n > 32)
					n = 32;
				if (n > 0)
					memcpy(dst + off, p, (size_t)n);
				p += 32;
			}
}

static void mb32_to_nv21_c(const uint8_t *src, uint8_t *dst, int w, int h)
{
	int cw = (w + 1) / 2, ch = (h + 1) / 2;
	int stride = (cw * 2 + 15) & ~15;
	int mbw = ((cw * 2 + 31) & ~31) / 32;
	int mbh = ((ch + 31) & ~31) / 32;
	const uint8_t *p = src;
	int i, j, m, k;

	for (i = 0; i < mbh; i++)
		for (j = 0; j < mbw; j++)
			for (m = 0; m < 32; m++) {
				int line = i * 32 + m;
				int off;

				if (line >= ch) {
					p += 32;
					continue;
				}
				off = line * stride + j * 32;
				for (k = 0; k < 16; k++) {
					if (j * 32 + 2 * k >= stride)
						break;
					dst[off + 2 * k] = p[2 * k + 1];      /* V */
					dst[off + 2 * k + 1] = p[2 * k];      /* U */
				}
				p += 32;
			}
}

static int64_t now_us(void);

static int pic_dumped;

/* ------------------------------------------------------------------ */
/* colour conversion lookup tables                                     */
/* ------------------------------------------------------------------ */

/* Pull one 32 byte tile row out of the VE surface.
 *
 * The source pointer is only known as a byte pointer, so a plain memcpy
 * makes gcc emit byte copies - and each of those is a separate bus
 * transaction because the ION buffers are mapped uncached.  Word loads cut
 * the transaction count by four, which is most of the win here.
 */
static int use_ldm;
static int64_t late_limit_us;
static int rebases;
static int64_t av_offset_ms;
static int64_t av_shown;
static volatile int g_vstarted;
static int64_t g_pts0;
static int64_t frame_iv;
static int64_t slot;
static double play_rate = 1.0;
static volatile sig_atomic_t g_stop;

/* The autoplay instance is started by init, so it is not in the console's
 * foreground process group and Ctrl-C never reaches it.  Handle the stop
 * signals explicitly and honour a stop file, so there is always a way out. */
static void on_stop(int sig)
{
	(void)sig;
	g_stop = 1;
}

static int stop_requested(void)
{
	if (g_stop)
		return 1;
	if (access("/tmp/llp.stop", F_OK) == 0) {
		unlink("/tmp/llp.stop");
		g_stop = 1;
		return 1;
	}
	return 0;
}

static int row_step = 2;
static double audio_pos;
static int resample_needed;
static int underruns;
static double ais_step = 1.0;
static double a_srcpos, a_frame_base;

typedef struct { uint32_t w[8]; } tile_row_t;

static inline void load_tile_row(uint32_t *dst, const uint8_t *src)
{
	if (use_ldm) {
		/* one ldmia: the whole 32 byte tile row in a single bus burst */
		*(tile_row_t *)dst = *(const tile_row_t *)src;
	} else {
		/* eight independent uncached reads, ~87 cycles each on this
		 * board, which is what dominates the frame time */
		const uint32_t *s = (const uint32_t *)src;

		dst[0] = s[0];
		dst[1] = s[1];
		dst[2] = s[2];
		dst[3] = s[3];
		dst[4] = s[4];
		dst[5] = s[5];
		dst[6] = s[6];
		dst[7] = s[7];
	}
}

#define CLIP_OFF 256
static uint8_t clip_tab[1024];
/* pre-shifted RGB565 clip tables: the inner loop then needs no shifts */
static unsigned short clip_r565[1024], clip_g565[1024], clip_b565[1024];
static short tab_r[256], tab_b[256], tab_gu[256], tab_gv[256];
static int tabs_ready;

static void init_tabs(void)
{
	int i;

	for (i = 0; i < 256; i++) {
		int c = i - 128;

		tab_r[i] = (short)(1.402f * c + (c >= 0 ? 0.5f : -0.5f));
		tab_b[i] = (short)(1.772f * c + (c >= 0 ? 0.5f : -0.5f));
		tab_gu[i] = (short)(-0.344136f * c + (c >= 0 ? 0.5f : -0.5f));
		tab_gv[i] = (short)(-0.714136f * c + (c >= 0 ? 0.5f : -0.5f));
	}
	for (i = 0; i < 1024; i++) {
		int v = i - CLIP_OFF;

		if (v < 0)
			v = 0;
		else if (v > 255)
			v = 255;
		clip_tab[i] = (uint8_t)v;
		clip_r565[i] = (unsigned short)((v >> 3) << 11);
		clip_g565[i] = (unsigned short)((v >> 2) << 5);
		clip_b565[i] = (unsigned short)(v >> 3);
	}
	tabs_ready = 1;
}

/* ------------------------------------------------------------------ */
/* DRM/KMS output                                                      */
/*                                                                     */
/* The display engine can scan out YUYV directly and do the colour     */
/* space conversion in hardware, so the CPU only has to de-interleave  */
/* the VE's planar chroma into packed 4:2:2 - no per-pixel maths.      */
/* ------------------------------------------------------------------ */

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static int drm_fd = -1;
static uint32_t drm_crtc, drm_conn;
static uint32_t drm_fb[3];
static uint8_t *drm_map[3];
static uint32_t drm_pitch;
static int drm_cur;
static int drm_on;
static int drm_warned;
static drmModeModeInfo drm_mode;
static volatile int drm_flip_done;
static int64_t conv_us, flip_us, dec_us, feed_us, ret_us;
static int bench_mode;
static int half_rows;
static int wait_flip;

/* The video engine, the CMA pool and the sound card are all exclusive.
 * Two players at once corrupt the VE state (and the next runs then fail
 * with "done, 0 frames"), so refuse to start a second one. */
static int take_single_instance_lock(void)
{
	int fd = open("/tmp/llp-play.lock", O_CREAT | O_RDWR, 0644);

	if (fd < 0) {
		fprintf(stderr, "llp-play: cannot create lock file: %s\n",
			strerror(errno));
		return 0;
	}
	if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
		fprintf(stderr,
			"llp-play: another instance is already running.\n"
			"          run '/etc/init.d/S99play stop' first.\n");
		return -1;
	}
	printf("lock: single instance (pid %d)\n", (int)getpid());
	return fd;                      /* keep it open for the process lifetime */
}

static void drm_flip_handler(int fd, unsigned int seq, unsigned int sec,
			     unsigned int usec, unsigned int crtc, void *data)
{
	(void)fd;
	(void)seq;
	(void)sec;
	(void)usec;
	(void)crtc;
	(void)data;
	drm_flip_done = 1;
}

static uint32_t drm_fourcc = DRM_FORMAT_RGB565;

static int drm_setup(int want_w, int want_h)
{
	drmModeRes *res;
	drmModeConnector *conn = NULL;
	drmModeEncoder *enc;
	struct drm_mode_create_dumb creq;
	struct drm_mode_map_dumb mreq;
	uint32_t handles[4], pitches[4], offsets[4];
	int i, b;

	drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) {
		printf("drm: open /dev/dri/card0: %s\n", strerror(errno));
		return -1;
	}
	drmSetMaster(drm_fd);            /* best effort: fbcon may own it */

	res = drmModeGetResources(drm_fd);
	if (!res) {
		printf("drm: drmModeGetResources failed\n");
		return -1;
	}

	for (i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(drm_fd, res->connectors[i]);

		if (!c)
			continue;
		if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
			conn = c;
			break;
		}
		drmModeFreeConnector(c);
	}
	if (!conn) {
		printf("drm: no connected connector\n");
		drmModeFreeResources(res);
		return -1;
	}

	drm_conn = conn->connector_id;
	enc = conn->encoder_id ? drmModeGetEncoder(drm_fd, conn->encoder_id) : NULL;
	if (!enc) {
		for (i = 0; i < res->count_encoders && !enc; i++)
			enc = drmModeGetEncoder(drm_fd, res->encoders[i]);
	}
	if (!enc || !enc->crtc_id) {
		printf("drm: no usable crtc\n");
		drmModeFreeConnector(conn);
		drmModeFreeResources(res);
		return -1;
	}
	drm_crtc = enc->crtc_id;
	drmModeFreeEncoder(enc);

	printf("drm: connector %u crtc %u mode %dx%d%s\n", drm_conn, drm_crtc,
	       conn->modes[0].hdisplay, conn->modes[0].vdisplay, conn->modes[0].name);

	if (conn->modes[0].hdisplay != want_w || conn->modes[0].vdisplay != want_h)
		printf("drm: mode differs from video (%dx%d)\n", want_w, want_h);

	for (b = 0; b < 3; b++) {
		memset(&creq, 0, sizeof(creq));
		creq.width = (uint32_t)want_w;
		creq.height = (uint32_t)want_h;
		creq.bpp = 16;                  /* RGB565 */
		if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
			printf("drm: create_dumb failed: %s\n", strerror(errno));
			return -1;
		}
		drm_pitch = creq.pitch;

		handles[0] = creq.handle;
		pitches[0] = creq.pitch;
		offsets[0] = 0;
		if (drmModeAddFB2(drm_fd, (uint32_t)want_w, (uint32_t)want_h,
				  drm_fourcc, handles, pitches, offsets,
				  &drm_fb[b], 0) != 0) {
			printf("drm: AddFB2(0x%x) failed: %s\n", drm_fourcc,
			       strerror(errno));
			return -1;
		}

		memset(&mreq, 0, sizeof(mreq));
		mreq.handle = creq.handle;
		if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
			printf("drm: map_dumb failed: %s\n", strerror(errno));
			return -1;
		}
		drm_map[b] = mmap(NULL, creq.size, PROT_READ | PROT_WRITE,
				  MAP_SHARED, drm_fd, (off_t)mreq.offset);
		if (drm_map[b] == MAP_FAILED) {
			printf("drm: mmap failed: %s\n", strerror(errno));
			return -1;
		}
		memset(drm_map[b], 0, creq.size);
	}

	if (drmModeSetCrtc(drm_fd, drm_crtc, drm_fb[0], 0, 0, &drm_conn, 1,
			   &conn->modes[0]) != 0) {
		printf("drm: SetCrtc failed: %s\n", strerror(errno));
		return -1;
	}
	drm_mode = conn->modes[0];

	drmModeFreeConnector(conn);
	drmModeFreeResources(res);
	fcntl(drm_fd, F_SETFL, fcntl(drm_fd, F_GETFL) | O_NONBLOCK);
	drm_on = 1;
	return 0;
}

static void drm_flip(void)
{
	drmEventContext ev;
	int guard = 0;

	drm_flip_done = 0;
	if (drmModePageFlip(drm_fd, drm_crtc, drm_fb[drm_cur],
			    DRM_MODE_PAGE_FLIP_EVENT, NULL) != 0) {
		drmModeSetCrtc(drm_fd, drm_crtc, drm_fb[drm_cur], 0, 0, &drm_conn, 1,
			       &drm_mode);
		drm_cur ^= 1;
		return;
	}
	/* With three buffers there is always a free one to draw into, so the
	 * vblank wait can go away entirely - it was costing most of a frame
	 * period.  Events are still drained so the DRM queue cannot fill up
	 * (a full queue makes drmModePageFlip fail, and the SetCrtc fallback
	 * below is a full modeset that costs far more than the wait). */
	memset(&ev, 0, sizeof(ev));
	ev.version = 2;
	ev.page_flip_handler = drm_flip_handler;
	if (wait_flip) {
		while (!drm_flip_done && guard++ < 100) {
			if (drmHandleEvent(drm_fd, &ev) < 0)
				break;
		}
	} else {
		/* one event is produced per flip, so consuming exactly one per
		 * frame keeps the queue drained; the fd is non blocking so this
		 * never stalls */
		(void)drmHandleEvent(drm_fd, &ev);
	}
	drm_cur = (drm_cur + 1) % 3;
}

/* MB32 (tiled) -> RGB565 straight into the scanout buffer.
 *
 * Two things matter for speed on this box:
 *  - the VE surfaces are mapped uncached, so every access is a separate bus
 *    transaction.  Whole 32 byte tile rows are pulled in with memcpy (word
 *    transfers) and then consumed from the cache.
 *  - two pixels are packed into one 32 bit store, halving the write traffic.
 */
/* MB32 (tiled) -> packed YUYV for the display engine's YUV layer.
 * Only de-interleaving: the DE does the colour space conversion. */
static void draw_mb32_yuyv(const VideoPicture *pic)
{
	int w = pic->nWidth;
	int mbw = ((w + 31) & ~31) / 32;
	int cw = (w + 1) / 2;
	int cmbw = ((cw * 2 + 31) & ~31) / 32;
	const uint8_t *Yb = (const uint8_t *)pic->pData0;
	const uint8_t *Ub = (const uint8_t *)pic->pData1;
	uint8_t *base = drm_map[drm_cur];
	int step = row_step;
	int y;

	for (y = 0; y < vh; y += step) {
		int sy = ymap[y];
		int ty = sy >> 5, my = sy & 31;
		int cy = sy >> 1;
		int cty, cmy;
		const uint8_t *yl, *cl;
		uint8_t *rp = base + (size_t)(vy + y) * drm_pitch;
		int nrep = step;
		int j, k;

		if (step > 1)
			cy &= ~1;      /* one chroma row serves a whole band */
		if (y + nrep > vh)
			nrep = vh - y;

		cty = cy >> 5;
		cmy = cy & 31;
		yl = Yb + (size_t)(ty * mbw) * 1024 + (size_t)my * 32;
		cl = Ub + (size_t)(cty * cmbw) * 1024 + (size_t)cmy * 32;

		for (j = 0; j < mbw; j++) {
			uint32_t yw[8], cwr[8], ow[16];
			const uint8_t *Y8 = (const uint8_t *)yw;
			const uint8_t *C8 = (const uint8_t *)cwr;
			uint32_t *cp = (uint32_t *)rp + j * 16;

			load_tile_row(yw, yl + (size_t)j * 1024);
			load_tile_row(cwr, cl + (size_t)j * 1024);

			/* byte view of the staged rows: indexing the uint32_t
			 * arrays with byte offsets would read past the end */
			for (k = 0; k < 32; k += 2)
				ow[k >> 1] = (uint32_t)Y8[k] | ((uint32_t)C8[k] << 8) |
					    ((uint32_t)Y8[k + 1] << 16) |
					    ((uint32_t)C8[k + 1] << 24);

			for (k = 0; k < nrep; k++) {
				memcpy(cp, ow, 64);
				cp = (uint32_t *)((uint8_t *)cp + drm_pitch);
			}
		}
	}
}

static void draw_mb32_565(const VideoPicture *pic, uint8_t *base, uint32_t pitch)
{
	int w = pic->nWidth;
	int mbw = ((w + 31) & ~31) / 32;
	int cw = (w + 1) / 2;
	int cmbw = ((cw * 2 + 31) & ~31) / 32;
	const uint8_t *Yb = (const uint8_t *)pic->pData0;
	const uint8_t *Ub = (const uint8_t *)pic->pData1;
	int y;

	for (y = 0; y < vh; y++) {
		int sy = ymap[y];
		int ty = sy >> 5, my = sy & 31;
		int cy = sy >> 1;
		int cty = cy >> 5, cmy = cy & 31;
		const uint8_t *yl = Yb + (size_t)(ty * mbw) * 1024 + (size_t)my * 32;
		const uint8_t *cl = Ub + (size_t)(cty * cmbw) * 1024 + (size_t)cmy * 32;
		uint32_t *out = (uint32_t *)(base + (size_t)(vy + y) * pitch) + (vx >> 1);
		int j;

		for (j = 0; j < mbw; j++) {
			uint32_t yw[8], cwr[8];
			const uint8_t *Y8, *C8;
			int k;

			load_tile_row(yw, yl + (size_t)j * 1024);
			load_tile_row(cwr, cl + (size_t)j * 1024);
			Y8 = (const uint8_t *)yw;
			C8 = (const uint8_t *)cwr;

			for (k = 0; k < 32; k += 2) {
				int y0 = Y8[k], y1 = Y8[k + 1];
				int u = C8[k], v = C8[k + 1];
				int rv = tab_r[v], bu = tab_b[u];
				int guv = tab_gu[u] + tab_gv[v];
				uint32_t p0 = clip_r565[y0 + rv + CLIP_OFF] |
					      clip_g565[y0 + guv + CLIP_OFF] |
					      clip_b565[y0 + bu + CLIP_OFF];
				uint32_t p1 = clip_r565[y1 + rv + CLIP_OFF] |
					      clip_g565[y1 + guv + CLIP_OFF] |
					      clip_b565[y1 + bu + CLIP_OFF];

				out[j * 16 + (k >> 1)] = p0 | (p1 << 16);
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* fused MB32 -> RGB, no scaling, 32bpp framebuffer                    */
/*                                                                    */
/* Reads the tiled surfaces directly so the intermediate NV21 buffer   */
/* is not needed; the colour maths is table driven.                    */
/* ------------------------------------------------------------------ */
static void draw_mb32_fast(const VideoPicture *pic)
{
	int w = pic->nWidth;
	int mbw = ((w + 31) & ~31) / 32;
	int cw = (w + 1) / 2;
	int cmbw = ((cw * 2 + 31) & ~31) / 32;
	const uint8_t *Yb = (const uint8_t *)pic->pData0;
	const uint8_t *Ub = (const uint8_t *)pic->pData1;
	int y;

	for (y = 0; y < vh; y++) {
		int sy = ymap[y];
		int ty = sy >> 5, my = sy & 31;
		int cy = sy >> 1;
		int cty = cy >> 5, cmy = cy & 31;
		const uint8_t *yl = Yb + (size_t)(ty * mbw) * 1024 + (size_t)my * 32;
		const uint8_t *cl = Ub + (size_t)(cty * cmbw) * 1024 + (size_t)cmy * 32;
		uint32_t *dst = (uint32_t *)(fb_mem + (size_t)(vy + y) * fb_fix.line_length) + vx;
		int j;

		for (j = 0; j < mbw; j++) {
			uint32_t yw[8], cwr[8];
			const uint8_t *Y8, *C8;
			uint32_t *o = dst + j * 32;
			int k;

			load_tile_row(yw, yl + (size_t)j * 1024);
			load_tile_row(cwr, cl + (size_t)j * 1024);
			Y8 = (const uint8_t *)yw;
			C8 = (const uint8_t *)cwr;

			for (k = 0; k < 32; k += 2) {
				int p = k >> 1;
				int uu = C8[2 * p];
				int vv = C8[2 * p + 1];
				int y0 = Y8[k], y1 = Y8[k + 1];
				int rv = tab_r[vv];
				int bu = tab_b[uu];
				int guv = tab_gu[uu] + tab_gv[vv];

				o[k] = (uint32_t)clip_tab[y0 + rv + CLIP_OFF] << 16 |
				       (uint32_t)clip_tab[y0 + guv + CLIP_OFF] << 8 |
				       (uint32_t)clip_tab[y0 + bu + CLIP_OFF];
				o[k + 1] = (uint32_t)clip_tab[y1 + rv + CLIP_OFF] << 16 |
					   (uint32_t)clip_tab[y1 + guv + CLIP_OFF] << 8 |
					   (uint32_t)clip_tab[y1 + bu + CLIP_OFF];
			}
		}
	}
}



static void draw_frame(const VideoPicture *pic)
{
	const uint8_t *Y = (const uint8_t *)pic->pData0;
	const uint8_t *UV = (const uint8_t *)pic->pData1;
	const uint8_t *V = (const uint8_t *)pic->pData2;
	int stride = pic->nLineStride;
	int semi = 1;
	int vu = 0;
	int y;

	if (!pic_dumped) {
		pic_dumped = 1;
		printf("pic: %dx%d fmt=%d stride=%d Y=%p U=%p V=%p\n",
		       pic->nWidth, pic->nHeight, pic->ePixelFormat, stride,
		       (void *)pic->pData0, (void *)pic->pData1, (void *)pic->pData2);
	}

	if (!Y || !UV)
		return;

	if (!tabs_ready)
		init_tabs();

	/* KMS path: converted buffer is page flipped, DE does the rest */
	if (drm_on) {
		if (pic->ePixelFormat == PIXEL_FORMAT_YUV_MB32_420 &&
		    vw == pic->nWidth && vy == 0) {
			int64_t c0 = now_us();

			if (bench_mode) {
				/* SMPTE style bars in YUYV, written without touching
				 * the decoder: proves whether the DE/scanout path
				 * itself is sane */
				static const uint8_t bar[8][3] = {
					{235,128,128}, {210, 16,146}, {170,166, 16},
					{145, 54, 34}, {106,202,222}, { 81, 90,240},
					{ 41,240,110}, { 16,128,128},
				};
				uint32_t *b = (uint32_t *)drm_map[drm_cur];
				int yy, xx;

				for (yy = 0; yy < vh; yy++) {
					uint32_t *row = (uint32_t *)((uint8_t *)b +
								     (size_t)yy * drm_pitch);
					for (xx = 0; xx < vw / 2; xx++) {
						int bi = xx * 8 / (vw / 2);

						row[xx] = (uint32_t)bar[bi][0] |
							  ((uint32_t)bar[bi][1] << 8) |
							  ((uint32_t)bar[bi][0] << 16) |
							  ((uint32_t)bar[bi][2] << 24);
					}
				}
				conv_us += now_us() - c0;
				c0 = now_us();
				drm_flip();
				flip_us += now_us() - c0;
				return;
			}

			if (drm_fourcc == DRM_FORMAT_YUYV)
				draw_mb32_yuyv(pic);
			else
				draw_mb32_565(pic, drm_map[drm_cur], drm_pitch);
			conv_us += now_us() - c0;
			c0 = now_us();
			drm_flip();
			flip_us += now_us() - c0;
		} else if (!drm_warned) {
			drm_warned = 1;
			printf("drm: unsupported picture (fmt=%d %dx%d vw=%d)\n",
			       pic->ePixelFormat, pic->nWidth, pic->nHeight, vw);
		}
		return;
	}

	/* fused, unscaled paths */
	if (pic->ePixelFormat == PIXEL_FORMAT_YUV_MB32_420 &&
	    vw == pic->nWidth && vy == 0) {
		if (out_bpp == 16) {
			draw_mb32_565(pic, fb_mem, fb_fix.line_length);
			return;
		}
		if (out_bpp == 32) {
			draw_mb32_fast(pic);
			return;
		}
	}

	if (pic->ePixelFormat == PIXEL_FORMAT_YUV_MB32_420) {
		int w = pic->nWidth, h = pic->nHeight;
		int s = (w + 15) & ~15;
		int hp = (h + 15) & ~15;
		size_t need = (size_t)s * hp * 3 / 2;

		if (nv_buf_size < need) {
			free(nv_buf);
			nv_buf = malloc(need);
			nv_buf_size = nv_buf ? need : 0;
		}
		if (!nv_buf)
			return;
		mb32_to_nv21_y(Y, nv_buf, w, h);
		mb32_to_nv21_c(UV, nv_buf + (size_t)s * hp, w, h);
		Y = nv_buf;
		UV = nv_buf + (size_t)s * hp;
		stride = s;
		semi = 1;
		vu = 1;                 /* NV21: V first, then U */
	} else if (V != NULL) {
		semi = 0;
	}

	if (!xmap || !ymap || vw <= 0 || vh <= 0)
		return;

	for (y = 0; y < vh; y++) {
		int sy = ymap[y];
		const uint8_t *yr = Y + (size_t)sy * stride;
		const uint8_t *urow = NULL, *vrow = NULL;
		uint8_t *dst = fb_mem + (size_t)(vy + y) * fb_fix.line_length +
			       (size_t)vx * (out_bpp / 8);
		int x;

		if (semi) {
			urow = UV + (size_t)(sy >> 1) * stride;
		} else {
			urow = UV + (size_t)(sy >> 1) * (stride / 2);
			vrow = V + (size_t)(sy >> 1) * (stride / 2);
		}

		if (out_bpp == 32) {
			uint32_t *d32 = (uint32_t *)dst;

			for (x = 0; x < vw; x++) {
				int sx = xmap[x];
				int c = sx >> 1;
				int yy = yr[sx];
				int uu, vv;

				if (semi) {
					uu = urow[2 * c + (vu ? 1 : 0)] - 128;
					vv = urow[2 * c + (vu ? 0 : 1)] - 128;
				} else {
					uu = urow[c] - 128;
					vv = vrow[c] - 128;
				}
				d32[x] = (uint32_t)clamp8(yy + ((91881 * vv) >> 16)) << 16 |
					 (uint32_t)clamp8(yy - ((22554 * uu + 46802 * vv) >> 16)) << 8 |
					 (uint32_t)clamp8(yy + ((116130 * uu) >> 16));
			}
		} else if (out_bpp == 16) {
			uint16_t *d16 = (uint16_t *)dst;

			for (x = 0; x < vw; x++) {
				int sx = xmap[x];
				int c = sx >> 1;
				int yy = yr[sx];
				int uu, vv, r, g, b;

				if (semi) {
					uu = urow[2 * c + (vu ? 1 : 0)] - 128;
					vv = urow[2 * c + (vu ? 0 : 1)] - 128;
				} else {
					uu = urow[c] - 128;
					vv = vrow[c] - 128;
				}
				r = clamp8(yy + ((91881 * vv) >> 16));
				g = clamp8(yy - ((22554 * uu + 46802 * vv) >> 16));
				b = clamp8(yy + ((116130 * uu) >> 16));
				d16[x] = (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
			}
		} else {
			return;
		}
	}
}

/* ------------------------------------------------------------------ */
/* player                                                              */
/* ------------------------------------------------------------------ */

static int64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

#define MAX_SAMPLE (8 << 20)

static int sample_to_annexb(const uint8_t *src, int len, int nalsize,
			    const uint8_t *avcc, int avcc_len, int keyframe,
			    uint8_t *dst, int dstmax)
{
	int off = 0;
	const uint8_t *p = src;
	const uint8_t *end = src + len;

	if (keyframe && avcc && avcc_len > 8) {
		int i = 6;
		int nsps = avcc[5] & 0x1f;

		for (int s = 0; s < nsps && i + 2 <= avcc_len; s++) {
			int l = rd16(avcc + i);

			i += 2;
			if (l <= 0 || i + l > avcc_len || off + 4 + l > dstmax)
				break;
			dst[off++] = 0; dst[off++] = 0; dst[off++] = 0; dst[off++] = 1;
			memcpy(dst + off, avcc + i, l);
			off += l;
			i += l;
		}
		if (i < avcc_len) {
			int npps = avcc[i++];

			for (int s = 0; s < npps && i + 2 <= avcc_len; s++) {
				int l = rd16(avcc + i);

				i += 2;
				if (l <= 0 || i + l > avcc_len || off + 4 + l > dstmax)
					break;
				dst[off++] = 0; dst[off++] = 0; dst[off++] = 0; dst[off++] = 1;
				memcpy(dst + off, avcc + i, l);
				off += l;
				i += l;
			}
		}
	}

	while (p + nalsize <= end) {
		uint32_t n = 0;

		for (int i = 0; i < nalsize; i++)
			n = (n << 8) | p[i];
		p += nalsize;
		if (n == 0 || p + n > end)
			break;
		if (off + 4 + (int)n > dstmax)
			return -1;
		dst[off++] = 0; dst[off++] = 0; dst[off++] = 0; dst[off++] = 1;
		memcpy(dst + off, p, n);
		off += n;
		p += n;
	}
	return off;
}

#include <signal.h>
#include <ucontext.h>

static void on_crash(int sig, siginfo_t *si, void *uc)
{
	char buf[256];
	int n;
#ifndef __arm__
	(void)uc;
	n = snprintf(buf, sizeof(buf), "\nCRASH sig=%d addr=%p\n", sig, si->si_addr);
#else
	ucontext_t *u = (ucontext_t *)uc;

	n = snprintf(buf, sizeof(buf), "\nCRASH sig=%d addr=%p pc=%p lr=%p sp=%p\n",
		     sig, si->si_addr, (void *)(uintptr_t)u->uc_mcontext.arm_pc,
		     (void *)(uintptr_t)u->uc_mcontext.arm_lr,
		     (void *)(uintptr_t)u->uc_mcontext.arm_sp);
#endif
	write(2, buf, (size_t)n);

	/* dump the memory map so the faulting pc can be attributed to a library */
	{
		int fd = open("/proc/self/maps", O_RDONLY);
		char mb[512];
		ssize_t r;

		if (fd >= 0) {
			while ((r = read(fd, mb, sizeof(mb))) > 0)
				write(2, mb, (size_t)r);
			close(fd);
		}
	}
	_exit(3);
}

/* ------------------------------------------------------------------ */
/* audio: AAC (faad2) -> S16 PCM -> ALSA                               */
/* ------------------------------------------------------------------ */

#include <alsa/asoundlib.h>
#include <neaacdec.h>

static snd_pcm_t *pcm;
static int audio_rate, audio_ch;
static volatile int64_t audio_written;
static volatile int audio_done;

/* The F1C100s codec comes up muted with an arbitrary mixer state; push every
 * playback control to "on" and max so that something is actually audible. */
static void audio_unmute(void)
{
	snd_mixer_t *mix;
	snd_mixer_elem_t *e;

	if (snd_mixer_open(&mix, 0) < 0)
		return;
	if (snd_mixer_attach(mix, "hw:0") < 0 ||
	    snd_mixer_selem_register(mix, NULL, NULL) < 0 ||
	    snd_mixer_load(mix) < 0) {
		snd_mixer_close(mix);
		return;
	}

	/* Enable the playback path and nothing else.  Switching on every
	 * control (as the early versions did) also routes the capture mixers
	 * with mic boost into the output, which is audible as constant hiss -
	 * the mic preamp amplifies its own noise and it lands on the DAC mix. */
	for (e = snd_mixer_first_elem(mix); e; e = snd_mixer_elem_next(e)) {
		const char *n = snd_mixer_selem_get_name(e);
		long lo = 0, hi = 0;
		/* "Left Mixer Mic In switch" and friends route the capture
		 * stages into the output mix; they do not start with "Mic",
		 * so they have to be matched anywhere in the name. */
		int is_input = (strcasestr(n, "mic") != NULL ||
				strcasestr(n, "line in") != NULL ||
				strcasestr(n, "fm in") != NULL);

		if (!snd_mixer_selem_is_active(e))
			continue;

		if (is_input) {
			/* input stages stay off: they only add noise */
			if (snd_mixer_selem_has_playback_switch(e)) {
				snd_mixer_selem_set_playback_switch_all(e, 0);
				printf("mixer: off  %s\n", n);
			}
			if (snd_mixer_selem_has_playback_volume(e)) {
				snd_mixer_selem_get_playback_volume_range(e, &lo, &hi);
				if (hi > lo)
					snd_mixer_selem_set_playback_volume_all(e, lo);
			}
			continue;
		}

		if (snd_mixer_selem_has_playback_switch(e)) {
			snd_mixer_selem_set_playback_switch_all(e, 1);
			printf("mixer: on   %s\n", n);
		}
		if (snd_mixer_selem_has_playback_volume(e)) {
			snd_mixer_selem_get_playback_volume_range(e, &lo, &hi);
			/* leave DAC/headphone at ~75% of the range: max on this
			 * codec clips on loud material */
			if (hi > lo)
				snd_mixer_selem_set_playback_volume_all(
					e, lo + (hi - lo) * 3 / 4);
		}
	}
	printf("mixer: playback on, capture muted\n");
	snd_mixer_close(mix);
}

static int audio_open(int rate, int ch)
{
	snd_pcm_hw_params_t *hw;
	unsigned int r = (unsigned int)rate;
	int rc;

	rc = snd_pcm_open(&pcm, "hw:0,0", SND_PCM_STREAM_PLAYBACK, 0);
	if (rc < 0)
		rc = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
	if (rc < 0) {
		printf("audio: snd_pcm_open: %s\n", snd_strerror(rc));
		return -1;
	}
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);
	snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
	snd_pcm_hw_params_set_channels(pcm, hw, (unsigned int)ch);
	snd_pcm_hw_params_set_rate_near(pcm, hw, &r, 0);

	/* A deep buffer absorbs the long stretches where the single core is
	 * busy converting a frame; without it the DAC underruns and every
	 * refill clicks.  Smallish periods keep the latency sane. */
	{
		unsigned int btime = 900000;    /* as deep as the codec allows */
		unsigned int ptime = 40000;     /* 40 ms */

		snd_pcm_hw_params_set_buffer_time_near(pcm, hw, &btime, 0);
		snd_pcm_hw_params_set_period_time_near(pcm, hw, &ptime, 0);
	}
	if ((rc = snd_pcm_hw_params(pcm, hw)) < 0) {
		printf("audio: hw_params: %s\n", snd_strerror(rc));
		return -1;
	}
	snd_pcm_prepare(pcm);
	audio_rate = (int)r;
	audio_ch = ch;
	{
		unsigned int got = 0;

		snd_pcm_hw_params_get_rate(hw, &got, NULL);
		if (got && rate > 0) {
			double dev = (double)got / (double)rate - 1.0;

			/* the codec takes continuous rates, so speed changes
			 * usually cost nothing; only resample if the card
			 * cannot give us the rate we asked for */
			resample_needed = (dev > 0.01 || dev < -0.01);
			/* media samples consumed per device sample: when the
			 * card cannot run at media*rate we still have to end up
			 * at media*rate of audio per second, so the ratio is
			 * (wanted) / (actual), not the playback rate itself. */
			ais_step = (double)rate / (double)got;
			if (resample_needed)
				printf("audio: card gave %u Hz for %d Hz, resampling"
				       " (step %.3f)\n", got, rate, ais_step);
		}
	}
	{
		snd_pcm_uframes_t bfr = 0, prd = 0;

		snd_pcm_get_params(pcm, &bfr, &prd);
		printf("audio: asked %d Hz, got %d Hz, %d ch s16, "
		       "buffer %u frames (%.0f ms), period %u\n",
		       rate, audio_rate, audio_ch, (unsigned)bfr,
		       audio_rate ? 1000.0 * (double)bfr / audio_rate : 0.0,
		       (unsigned)prd);
	}
	return 0;
}

static void audio_write(const int16_t *buf, int frames)
{
	int off = 0;

	while (off < frames) {
		snd_pcm_sframes_t n = snd_pcm_writei(pcm,
						     buf + (size_t)off * audio_ch,
						     (snd_pcm_uframes_t)(frames - off));

		if (n < 0) {
			if (n == -EPIPE) {
				underruns++;
				snd_pcm_prepare(pcm);
				continue;
			}
			if (n == -EAGAIN) {
				usleep(2000);
				continue;
			}
			break;
		}
		off += (int)n;
		audio_written += n;
	}
}

/* Linear resampling, used only when the playback rate is not 1.0.
 * Pitch shifts along with the speed, which is what cheap speed control
 * does; a pitch preserving time stretch is far too heavy for this CPU. */
static void audio_write_resampled(const int16_t *pcm, int frames)
{
	static int16_t *out;
	static int out_cap;
	int need, o = 0;

	if (!resample_needed) {
		audio_write(pcm, frames);
		a_frame_base += frames;
		a_srcpos = a_frame_base;
		return;
	}

	need = (int)(frames / (ais_step > 0 ? ais_step : 1.0)) + 8;
	if (need > out_cap) {
		free(out);
		out = malloc((size_t)need * (size_t)audio_ch * sizeof(int16_t));
		if (!out) {
			out_cap = 0;
			a_frame_base += frames;
			return;
		}
		out_cap = need;
	}

	/* Absolute position in the source stream, so the fractional phase
	 * carries across frames without a per-frame reset (that reset is
	 * what made transposed playback sound gritty). */
	while (a_srcpos < a_frame_base + frames - 1 && o < need) {
		double rel = a_srcpos - a_frame_base;
		int i = (int)rel;
		double f = rel - (double)i;
		int c;

		for (c = 0; c < audio_ch; c++)
			out[o * audio_ch + c] =
				(int16_t)(pcm[i * audio_ch + c] * (1.0 - f) +
					  pcm[(i + 1) * audio_ch + c] * f);
		o++;
		a_srcpos += ais_step;
	}
	a_frame_base += frames;

	if (o > 0)
		audio_write(out, o);
}

typedef struct {
	const char *path;
	track_t *t;
} audio_arg_t;

static void *audio_thread(void *arg)
{
	audio_arg_t *aa = (audio_arg_t *)arg;
	const char *path = aa->path;
	track_t *t = aa->t;
	NeAACDecHandle dec;
	NeAACDecConfigurationPtr cfg;
	unsigned long rate = 0;
	unsigned char ch = 0;
	uint8_t *buf;
	int fd, i;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return NULL;
	buf = malloc(1 << 16);
	if (!buf) {
		close(fd);
		return NULL;
	}

	dec = NeAACDecOpen();
	if (!dec) {
		free(buf);
		close(fd);
		return NULL;
	}
	cfg = NeAACDecGetCurrentConfiguration(dec);
	cfg->defSampleRate = 44100;
	cfg->outputFormat = FAAD_FMT_16BIT;
	cfg->downMatrix = 1;
	NeAACDecSetConfiguration(dec, cfg);

	if (NeAACDecInit2(dec, t->asc, (unsigned long)t->asc_len, &rate, &ch) < 0) {
		printf("audio: aac init failed (asc %d bytes)\n", t->asc_len);
		NeAACDecClose(dec);
		free(buf);
		close(fd);
		return NULL;
	}
	printf("audio: aac %lu Hz %d ch, %d access units\n", rate, (int)ch, t->n);

	audio_unmute();
	if (audio_open((int)((double)rate * play_rate), (int)ch) < 0) {
		NeAACDecClose(dec);
		free(buf);
		close(fd);
		return NULL;
	}

	/* A mild priority bump only: SCHED_FIFO here made the kernel enable
	 * RT throttling (the audio thread spins when the DAC is full), which
	 * then starved the video thread for whole slices. */
	/* Below the video thread on purpose: AAC decoding plus ALSA writes
	 * were taking ~30 ms per video frame when given a priority boost,
	 * which is what made playback fall behind.  The 400 ms buffer easily
	 * absorbs a lower priority. */
	setpriority(PRIO_PROCESS, 0, 0);

	/* Hold off until the video clock exists, otherwise the audio starts
	 * half a second early (the first frame takes that long to decode) and
	 * the two never line up. */
	while (!g_vstarted && !stop_requested())
		usleep(5000);
	printf("audio: synced to video clock\n");

	for (i = 0; i < t->n; i++) {
		NeAACDecFrameInfo fi;
		void *pcmbuf;
		int sz = (int)t->sizes[i];

		if (sz <= 0 || sz > 65536)
			continue;
		if (file_pread(fd, buf, (size_t)sz, (off_t)t->offs[i]) != sz)
			break;
		pcmbuf = NeAACDecDecode(dec, &fi, buf, (unsigned long)sz);
		if (fi.error || !pcmbuf)
			continue;
		if (fi.channels != (unsigned char)audio_ch)
			continue;
		audio_write_resampled((const int16_t *)pcmbuf,
				      (int)(fi.samples / (unsigned long)audio_ch));
	}
	printf("audio: done, %lld frames\n", (long long)audio_written);
	audio_done = 1;
	if (pcm)
		snd_pcm_drain(pcm);
	if (pcm)
		snd_pcm_close(pcm);
	pcm = NULL;
	NeAACDecClose(dec);
	free(buf);
	close(fd);
	return NULL;
}

int main(int argc, char **argv)
{
	const char *path = NULL;
	int verbose = 0;

	/* First thing: the VE, the CMA pool and the audio device are all
	 * exclusive, and a second instance corrupts the decoder. */
	if (take_single_instance_lock() < 0)
		return 1;
	track_t trk, atrk;
	static audio_arg_t aarg;
	pthread_t athr;
	int have_audio = 0;

	{
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sa.sa_sigaction = on_crash;
		sa.sa_flags = SA_SIGINFO;
		sigaction(SIGSEGV, &sa, NULL);
		sigaction(SIGBUS, &sa, NULL);

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = on_stop;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGINT, &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);
		sigaction(SIGHUP, &sa, NULL);
	}

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v"))
			verbose = 1;
		else if (!path)
			path = argv[i];
	}
	if (!path) {
		fprintf(stderr, "usage: %s file.mp4 [-v]\n", argv[0]);
		return 2;
	}

	memset(&atrk, 0, sizeof(atrk));
	if (mp4_parse(path, &trk, &atrk) < 0) {
		fprintf(stderr, "%s: no usable H.264 video track\n", path);
		return 1;
	}
	printf("mp4: %dx%d, %d samples, nal_size=%d, avcC=%d bytes\n",
	       trk.width, trk.height, trk.n, trk.nal_size, trk.avcc_len);
	if (verbose)
		for (int i = 0; i < trk.n && i < 8; i++)
			printf("  sample %d: off=%u size=%u dts=%lld cts=%lld\n", i,
			       trk.offs[i], trk.sizes[i], (long long)trk.dts[i],
			       (long long)trk.cts[i]);

	/* Frame interval straight from the sample decode times: pts deltas
	 * are unreliable here because the decoder returns pictures out of
	 * presentation order when the stream has B frames. */
	if (trk.n > 1 && trk.dts) {
		int64_t span = trk.dts[trk.n - 1] - trk.dts[0];

		if (span > 0)
			frame_iv = span / (trk.n - 1);
	}
	if (frame_iv <= 0)
		frame_iv = 40000;
	printf("pacing: %.2f fps (interval %lld us)\n",
	       frame_iv ? 1000000.0 / (double)frame_iv : 0.0,
	       (long long)frame_iv);

	if (fb_open() < 0)
		return 1;
	fb_layout(trk.width, trk.height);
	fb_clear();

	/* Output selection, LLP_OUT=de|565|fb (default de):
	 *   de  - DRM/KMS, YUYV buffer, display engine does the CSC
	 *   565 - DRM/KMS, RGB565 buffer, converted on the CPU
	 *   fb  - classic /dev/fb0, converted on the CPU
	 */
	map_file(path);
	bench_mode = getenv("LLP_BENCH") != NULL;
	/* Reading the VE surfaces is bandwidth bound at roughly 20 MB/s,
	 * so halving the number of rows read is the cheapest way to buy
	 * frame rate.  The skipped rows are duplicated on output. */
	half_rows = getenv("LLP_FULL") == NULL;
	wait_flip = getenv("LLP_BLOCKFLIP") != NULL;
	use_ldm = getenv("LLP_LDM") != NULL;

	{
		const char *r = getenv("LLP_RATE");

		if (r) {
			double v = atof(r);

			if (v >= 0.25 && v <= 4.0)
				play_rate = v;
		}
		/* reading the VE surface is bandwidth bound, so buy frame rate
		 * with vertical detail: full rate keeps 2 of every 2 rows,
		 * fast forward keeps 1 of every 4. */
		if (getenv("LLP_FULL"))
			row_step = 1;
		else if (play_rate > 1.25)
			row_step = 4;
		else
			row_step = 2;
		printf("rate: %.2fx, sampling 1 of every %d rows\n",
		       play_rate, row_step);
	}

	{
		const char *mode = getenv("LLP_OUT");

		if (!mode)
			mode = "de";
		if (!strcmp(mode, "fb")) {
			printf("output: fbdev %dbpp (software)\n", out_bpp);
		} else {
			drm_fourcc = !strcmp(mode, "565") ? DRM_FORMAT_RGB565
							  : DRM_FORMAT_YUYV;
			if (drm_setup(vw, vh) == 0) {
				printf("output: KMS %s\n",
				       drm_fourcc == DRM_FORMAT_YUYV ? "YUYV (DE colour conversion)"
								     : "RGB565 (software)");
			} else {
				printf("output: fbdev %dbpp (KMS setup failed)\n", out_bpp);
			}
		}
	}

	struct ScMemOpsS *memops = MemAdapterGetOpsS();

	if (!memops) {
		fprintf(stderr, "MemAdapterGetOpsS failed\n");
		return 1;
	}
	CdcMemOpen(memops);

	/* CedarX registers its codec back ends (libawh264.so, ...) through a
	 * plugin table; without this every format looks unsupported. */
	AddVDPlugin();

	VideoDecoder *dec = CreateVideoDecoder();

	if (!dec) {
		fprintf(stderr, "CreateVideoDecoder failed\n");
		return 1;
	}
	printf("ve: decoder created\n");

	VideoStreamInfo vi;
	VConfig cfg;

	memset(&vi, 0, sizeof(vi));
	vi.eCodecFormat = VIDEO_CODEC_FORMAT_H264;
	vi.nWidth = trk.width;
	vi.nHeight = trk.height;
	vi.nFrameRate = 24000;
	vi.nFrameDuration = 1000;
	vi.pCodecSpecificData = (char *)trk.avcc;
	vi.nCodecSpecificDataLen = trk.avcc_len;

	memset(&cfg, 0, sizeof(cfg));
	cfg.memops = memops;
	cfg.eOutputPixelFormat = PIXEL_FORMAT_YUV_PLANER_420;
	cfg.nFrameBufferNum = 4;
	cfg.bDisable3D = 1;
	/* these must not be 0, otherwise the picture queue is not set up */
	cfg.nDeInterlaceHoldingFrameBufferNum = 1;
	cfg.nDisplayHoldingFrameBufferNum = 1;
	cfg.nRotateHoldingFrameBufferNum = 0;
	cfg.nDecodeSmoothFrameBufferNum = 1;
	cfg.nVbvBufferSize = 1024 * 1024;

	int ret = InitializeVideoDecoder(dec, &vi, &cfg);

	if (ret != 0) {
		fprintf(stderr, "InitializeVideoDecoder failed: %d\n", ret);
		return 1;
	}
	printf("ve: decoder initialised, memops=%p\n", (void *)memops);

	if (atrk.n > 0 && atrk.asc && atrk.asc_len > 0) {
		aarg.path = path;
		aarg.t = &atrk;
		if (pthread_create(&athr, NULL, audio_thread, &aarg) == 0)
			have_audio = 1;
		else
			printf("audio: thread start failed\n");
	} else {
		printf("audio: no aac track in this file\n");
	}

	int fd = open(path, O_RDONLY);
	uint8_t *sample = malloc(MAX_SAMPLE);
	uint8_t *annexb = malloc(MAX_SAMPLE);

	if (fd < 0 || !sample || !annexb)
		return 1;

	int idx = 0;
	int eos = 0;
	int64_t t0 = -1;
	int shown = 0;
	int dropped = 0;
	int64_t prev_pts = 0;
	int64_t draw_us = 0;
	int64_t last_report = now_us();

	while (1) {
		/* feed one sample, but keep the decoder's queue drained */
		if (!eos && idx < trk.n && VideoStreamFrameNum(dec, 0) < 64) {
			int sz = (int)trk.sizes[idx];
			char *pbuf = NULL, *ringbuf = NULL;
			int psize = 0, ringsize = 0;
			int n = 0;
			int is_key = 0;

			if (sz <= 0 || sz > MAX_SAMPLE) {
				idx++;
				continue;
			}
			if (file_pread(fd, sample, sz, (off_t)trk.offs[idx]) != sz) {
				eos = 1;
				continue;
			}

			if (trk.nal_size == 4 && sz > 5) {
				for (int o = 0; o + 5 < sz;) {
					uint32_t nl = rd32(sample + o);
					int ty;

					if (nl == 0 || (int)nl > sz - o - 4)
						break;
					ty = sample[o + 4] & 0x1f;
					if (ty == 5 || ty == 7)
						is_key = 1;
					o += 4 + (int)nl;
				}
			}

			n = sample_to_annexb(sample, sz, trk.nal_size, trk.avcc, trk.avcc_len,
					     is_key, annexb, MAX_SAMPLE);

			if (n > 0) {
				int rr = RequestVideoStreamBuffer(dec, n, &pbuf, &psize,
								  &ringbuf, &ringsize, 0);

				if (rr != 0 || psize + ringsize < n) {
					if (idx < 3 || (idx % 200) == 0)
						printf("stream buffer: rr=%d buf=%d ring=%d need=%d\n",
						       rr, psize, ringsize, n);
					pbuf = NULL;
				}
			}

			if (n > 0 && pbuf) {
				VideoStreamDataInfo di;
				int first = psize < n ? psize : n;
				int64_t f0 = now_us();

				/* a request may be split between buf and the ring */
				memcpy(pbuf, annexb, (size_t)first);
				if (first < n && ringbuf)
					memcpy(ringbuf, annexb + first, (size_t)(n - first));

				memset(&di, 0, sizeof(di));
				di.pData = pbuf;
				di.nLength = n;
				di.nPts = trk.cts[idx];
				di.bIsFirstPart = 1;
				di.bIsLastPart = 1;
				di.bValid = 1;
				if (SubmitVideoStreamData(dec, &di, 0) != 0) {
					printf("SubmitVideoStreamData failed\n");
					break;
				}
				feed_us += now_us() - f0;
			}
			idx++;
			if (idx >= trk.n)
				eos = 1;
		} else if (idx >= trk.n) {
			eos = 1;
		}

		/* drain everything the decoder can produce right now */
		ret = VDECODE_RESULT_OK;
		for (;;) {
			{
				int64_t d0 = now_us();

				ret = DecodeVideoStream(dec, eos && idx >= trk.n, 0, 0, 0);
				dec_us += now_us() - d0;
			}
			if (ret == VDECODE_RESULT_FRAME_DECODED ||
			    ret == VDECODE_RESULT_KEYFRAME_DECODED) {
				VideoPicture *pic = RequestPicture(dec, 0);

				if (pic) {
					int64_t target;

					if (t0 < 0) {
						/* media time 0 for the audio thread is
						 * this instant; the first picture sits at
						 * pts0, so it gets shown pts0 later. */
						t0 = now_us();
						g_pts0 = pic->nPts;
						g_vstarted = 1;
						/* keep the layout from the container size:
						 * pic->nHeight is 32-aligned padding (608), not
						 * the displayed height (600). */
					}
					/* Pace on the presentation slot, not on pts.
					 * With B frames the decoder hands pictures over
					 * out of pts order, so pts based pacing keeps
					 * seeing "late" frames and drops them all. */
					if (late_limit_us < 200000)
						late_limit_us = 200000;
					prev_pts = pic->nPts;
					target = t0 + (int64_t)((double)(g_pts0 + slot * frame_iv) / play_rate);
					slot++;
					while (now_us() < target) {
						int64_t d = target - now_us();

						if (d > 2000)
							usleep(1000);
						else if (d > 0)
							usleep((useconds_t)d);
					}
					/* Behind by more than one frame?  Skip the
					 * conversion to catch up with the sound.  This
					 * only works because the wait above is skipped
					 * as well, so the next frames go through at
					 * decode speed until we are back on time. */
					if (now_us() - target > frame_iv) {
						dropped++;
					} else {
						int64_t dt0 = now_us();

						draw_frame(pic);
						draw_us += now_us() - dt0;
					}
					{
						int64_t r0 = now_us();

						ReturnPicture(dec, pic);
						ret_us += now_us() - r0;
					}
					shown++;
					if (shown == 1 || (shown % 48) == 0) {
						double dm = shown ? (double)draw_us / 1000.0 / shown : 0;
						double cm = shown ? (double)conv_us / 1000.0 / shown : 0;
						double fm = shown ? (double)flip_us / 1000.0 / shown : 0;

						/* how far the picture is ahead of
						 * the sound, in media milliseconds */
						if (pcm && audio_written > 0) {
							snd_pcm_sframes_t delay = 0;

							if (snd_pcm_delay(pcm, &delay) == 0) {
								int64_t played = audio_written - (int64_t)delay;

								if (played > 0)
									av_offset_ms =
										(int64_t)((double)(slot * frame_iv) / 1000.0 -
											  (double)played * 1000.0 / 44100.0);
							}
							/* close the loop: nudge the video
							 * timeline towards the audio clock */
							av_shown = av_offset_ms;
							if (av_offset_ms > 100 || av_offset_ms < -100) {
								t0 += av_offset_ms * 1000;
								av_offset_ms = 0;
							}
						}
						printf("frame %d fps=%.1f drop=%d"
						       " draw=%.1f conv=%.1f flip=%.1f"
						       " dec=%.1f feed=%.1f ret=%.1f rb=%d av=%lld ur=%d\n",
						       shown,
						       (t0 >= 0 && now_us() > t0)
						       ? (double)shown * 1000000.0 /
							 (double)(now_us() - t0) : 0.0,
						       dropped, dm, cm, fm,
						       (double)dec_us / 1000.0 / shown,
						       (double)feed_us / 1000.0 / shown,
						       (double)ret_us / 1000.0 / shown,
						       rebases, (long long)av_shown, underruns);
					}
				}
				continue;
			}
			break;
		}

		if (ret < 0) {
			printf("DecodeVideoStream error %d\n", ret);
			break;
		}
		if (ret == VDECODE_RESULT_NO_BITSTREAM && eos && idx >= trk.n)
			break;

		if (stop_requested())
			break;
		if (verbose && (now_us() - last_report) > 2000000) {
			last_report = now_us();
			printf("progress: %d frames, sample %d/%d\n", shown, idx, trk.n);
		}
	}

	printf("done, %d frames\n", shown);
	if (have_audio) {
		/* Drop whatever is still queued: a blocked snd_pcm_writei would
		 * otherwise keep the thread (and the VE) alive for the whole
		 * drain window, and the next run then fails with "the first
		 * slice of the frame is not 0". */
		if (pcm)
			snd_pcm_drop(pcm);
		for (int i = 0; i < 40 && !audio_done; i++)
			usleep(25000);
	}
	/* Blank the panel: without this the CRTC keeps scanning out the last
	 * frame and playback looks like it never stopped. */
	if (drm_on) {
		int b;

		for (b = 0; b < 3; b++)
			memset(drm_map[b], 0, (size_t)drm_pitch * vh);
		drm_flip();
	}
	fb_clear();
	DestroyVideoDecoder(dec);
	CdcMemClose(memops);
	return 0;
}
