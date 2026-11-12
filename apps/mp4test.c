/* SPDX-License-Identifier: GPL-2.0 */
/* Host-side harness: exercise the MP4 demuxer and Annex-B converter from
 * llp-play.c on x86 so they can be validated without the board. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define main llp_main
#include "llp-play.c"
#undef main

int main(int argc, char **argv)
{
	track_t t, atrk;

	memset(&atrk, 0, sizeof(atrk));

	if (argc < 2) {
		fprintf(stderr, "usage: %s file.mp4 [dump.h264]\n", argv[0]);
		return 2;
	}
	track_t a;

	memset(&a, 0, sizeof(a));
	if (mp4_parse(argv[1], &t, &a) < 0) {
		fprintf(stderr, "mp4_parse failed\n");
		return 1;
	}
	printf("track: %dx%d samples=%d nal_size=%d avcc=%d bytes\n",
	       t.width, t.height, t.n, t.nal_size, t.avcc_len);
	printf("audio: samples=%d asc=%d bytes\n", a.n, a.asc_len);

	int64_t total = 0;

	for (int i = 0; i < t.n; i++) {
		total += t.sizes[i];
		if (i < 5 || i == t.n - 1)
			printf("  [%d] off=%u size=%u dts=%lld cts=%lld\n", i, t.offs[i],
			       t.sizes[i], (long long)t.dts[i], (long long)t.cts[i]);
	}
	printf("total sample bytes: %lld, duration=%.3fs\n", (long long)total,
	       t.n > 0 ? (double)(t.cts[t.n - 1] + (t.cts[t.n - 1] - t.cts[t.n - 2])) / 1e6 : 0.0);

	if (argc >= 3) {
		int fd = open(argv[1], O_RDONLY);
		FILE *out = fopen(argv[2], "wb");
		uint8_t *s = malloc(MAX_SAMPLE);
		uint8_t *a = malloc(MAX_SAMPLE);
		int nkey = 0, nconv = 0;

		for (int i = 0; i < t.n; i++) {
			int sz = (int)t.sizes[i];
			int is_key = 0;

			if (pread(fd, s, sz, t.offs[i]) != sz)
				return 1;
			for (int o = 0; o + 5 < sz;) {
				uint32_t nl = rd32(s + o);

				if (nl == 0 || (int)nl > sz - o - 4)
					break;
				if (o == 0 && (s[4] & 0x1f) == 5)
					is_key = 1;
				o += 4 + (int)nl;
			}
			int n = sample_to_annexb(s, sz, t.nal_size, t.avcc, t.avcc_len, is_key,
						 a, MAX_SAMPLE);
			if (n <= 0) {
				fprintf(stderr, "sample %d convert failed\n", i);
				return 1;
			}
			nconv++;
			nkey += is_key;
			fwrite(a, 1, n, out);
		}
		printf("wrote %s: %d samples, %d keyframes\n", argv[2], nconv, nkey);
		fclose(out);
		close(fd);
	}
	return 0;
}
