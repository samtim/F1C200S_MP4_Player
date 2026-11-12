/* SPDX-License-Identifier: GPL-2.0 */
/*
 * lcdtest - draw a known test pattern on /dev/fb0 so a mis-timed RGB panel
 * can be diagnosed by eye instead of by guesswork.
 *
 *   lcdtest bars      vertical colour bars + 1px markers
 *   lcdtest checker   8x8 checkerboard in panel pixels
 *   lcdtest grid      1px grid every 64 px (shows shear / wrap immediately)
 *   lcdtest solid R G B
 *
 * Works with 16bpp (RGB565) and 32bpp (XRGB8888) framebuffers.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int bpp;
static unsigned char *mem;
static struct fb_var_screeninfo var;
static struct fb_fix_screeninfo fix;

static inline void put_px(int x, int y, unsigned int r, unsigned int g, unsigned int b)
{
	unsigned char *p;

	if (x < 0 || y < 0 || (unsigned)x >= var.xres || (unsigned)y >= var.yres)
		return;
	p = mem + (unsigned long)y * fix.line_length + (unsigned long)x * (bpp / 8);

	if (bpp == 32) {
		p[0] = b;
		p[1] = g;
		p[2] = r;
		p[3] = 0xff;
	} else if (bpp == 16) {
		unsigned short v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
		p[0] = v & 0xff;
		p[1] = v >> 8;
	}
}

static void fill(unsigned int r, unsigned int g, unsigned int b)
{
	int x, y;

	for (y = 0; y < (int)var.yres; y++)
		for (x = 0; x < (int)var.xres; x++)
			put_px(x, y, r, g, b);
}

static void bars(void)
{
	static const unsigned int col[8][3] = {
		{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
		{255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {0, 0, 0},
	};
	int x, y, i, w = var.xres / 8;

	for (y = 0; y < (int)var.yres; y++)
		for (x = 0; x < (int)var.xres; x++) {
			i = x / w;
			if (i > 7)
				i = 7;
			put_px(x, y, col[i][0], col[i][1], col[i][2]);
		}

	/* 4 reference pixels in the top-left corner: red/green/blue/white */
	put_px(0, 0, 255, 0, 0);
	put_px(1, 0, 0, 255, 0);
	put_px(2, 0, 0, 0, 255);
	put_px(3, 0, 255, 255, 255);

	/* 1px border: any shift or wrap shows up as a broken edge */
	for (x = 0; x < (int)var.xres; x++) {
		put_px(x, 0, 255, 255, 255);
		put_px(x, var.yres - 1, 255, 255, 255);
	}
	for (y = 0; y < (int)var.yres; y++) {
		put_px(0, y, 255, 255, 255);
		put_px(var.xres - 1, y, 255, 255, 255);
	}
}

static void checker(void)
{
	int x, y;

	for (y = 0; y < (int)var.yres; y++)
		for (x = 0; x < (int)var.xres; x++)
			if (((x >> 3) + (y >> 3)) & 1)
				put_px(x, y, 255, 255, 255);
			else
				put_px(x, y, 0, 0, 0);
}

static void grid(void)
{
	int x, y;

	fill(0, 0, 0);
	for (y = 0; y < (int)var.yres; y++)
		for (x = 0; x < (int)var.xres; x++)
			if ((x % 64) == 0 || (y % 64) == 0)
				put_px(x, y, 255, 255, 255);
	/* diagonal: visually straight only when timing is correct */
	for (x = 0; x < (int)var.xres && x < (int)var.yres; x++)
		put_px(x, x, 255, 0, 0);
}

int main(int argc, char **argv)
{
	const char *pat = (argc > 1) ? argv[1] : "bars";
	int fd;

	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		perror("/dev/fb0");
		return 1;
	}
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix)) {
		perror("ioctl");
		return 1;
	}
	bpp = var.bits_per_pixel;
	printf("fb0: %ux%u %dbpp line_length=%u\n", var.xres, var.yres, bpp, fix.line_length);

	mem = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mem == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	if (!strcmp(pat, "bars"))
		bars();
	else if (!strcmp(pat, "checker"))
		checker();
	else if (!strcmp(pat, "grid"))
		grid();
	else if (!strcmp(pat, "solid") && argc >= 5)
		fill(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
	else {
		fprintf(stderr, "usage: %s bars|checker|grid|solid R G B\n", argv[0]);
		return 2;
	}

	msync(mem, fix.smem_len, MS_SYNC);
	munmap(mem, fix.smem_len);
	close(fd);
	return 0;
}
