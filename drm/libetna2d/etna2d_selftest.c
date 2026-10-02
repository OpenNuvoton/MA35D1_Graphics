/*
 * etna2d_selftest.c — libetna2d acceptance test.
 *
 * Exercises the whole public API surface — device open, surface create,
 * batched context, all four ops, flush/finish — and CPU-verifies the
 * resulting pixels. Unlike the Phase 1 tests (one op per submit, raw
 * register sequences), this proves the *library* abstraction and its
 * batching path: several ops accumulate into one command stream and are
 * submitted together.
 *
 * Usage: etna2d_selftest /dev/dri/renderD128
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "etna2d.h"

#define W 256
#define H 256

static int fails;

static void logger(int level, const char *msg, void *user)
{
	static const char *tag[] = { "FAIL", "WARN", "INFO", "DBG " };
	(void)user;
	fprintf(stderr, "[%s] %s\n", tag[level < 0 ? 0 : (level > 3 ? 3 : level)], msg);
}

static void ok(const char *what, int cond)
{
	if (cond) {
		printf("[ OK ] %s\n", what);
	} else {
		printf("[FAIL] %s\n", what);
		fails++;
	}
}

static int tol(uint32_t g, uint32_t e, int t)
{
	for (int sh = 0; sh < 32; sh += 8) {
		int d = ((int)((g >> sh) & 0xff)) - ((int)((e >> sh) & 0xff));
		if (d < 0) d = -d;
		if (d > t) return 0;
	}
	return 1;
}

int main(int argc, char **argv)
{
	struct etna2d_device  *dev = NULL;
	struct etna2d_context *ctx = NULL;
	struct etna2d_surface *src = NULL, *dst = NULL, *big = NULL;
	uint32_t *sp, *dp, *bp;
	int rc, verbose = !!getenv("ETNA2D_VERBOSE");

	if (argc < 2) {
		fprintf(stderr, "usage: %s /dev/dri/renderD128\n", argv[0]);
		return 2;
	}
	if (verbose)
		etna2d_set_logger(logger, NULL);

	rc = etna2d_device_open(argv[1], &dev);
	ok("device_open", rc == ETNA2D_OK);
	if (rc != ETNA2D_OK)
		return 1;
	printf("       GC%03x rev 0x%04x FEATURES_0=0x%08llx\n",
	       etna2d_device_model(dev), etna2d_device_revision(dev),
	       (unsigned long long)etna2d_device_features0(dev));

	ok("surface_create src", etna2d_surface_create(dev, W, H, &src) == ETNA2D_OK);
	ok("surface_create dst", etna2d_surface_create(dev, W, H, &dst) == ETNA2D_OK);
	ok("surface_create big", etna2d_surface_create(dev, W, H, &big) == ETNA2D_OK);
	ok("context_create",     etna2d_context_create(dev, 8, &ctx) == ETNA2D_OK);
	if (!src || !dst || !big || !ctx)
		goto out;

	/* premultiplied semi-transparent red source (A=0x80, R=0x80) */
	sp = etna2d_surface_map(src);
	for (int i = 0; i < W * H; i++)
		sp[i] = 0x80800000u;
	etna2d_surface_cpu_fini(src);

	/* --- Batch 1: fill + opaque blit + blend, all in ONE submit ------- */
	etna2d_fill(ctx, dst, 0, 0, W, H, 0xff0000ffu);          /* blue bg    */
	etna2d_fill(ctx, dst, 32, 32, 64, 64, 0xff00ff00u);      /* green box  */
	etna2d_blend_over(ctx, src, dst, 0, 0, W, H);            /* red 50% over */
	rc = etna2d_finish(ctx);
	ok("batched finish (3 ops, 1 submit)", rc == ETNA2D_OK);

	dp = etna2d_surface_map(dst);
	/* blend over blue bg: out = src.rgb + dst.rgb*(1-0x80/255)
	 *   R=0x80, G=0x00, B~0x7f, A~0xff -> 0xff80007f */
	ok("blend over bg pixel", tol(dp[(H/2)*W + W/2], 0xff80007fu, 2));
	/* blend over the green box region (px 48,48): dst was 0xff00ff00
	 *   R=0x80, G=0x00+0xff*127/255=0x7f, B=0x00, A=0xff -> 0xff807f00 */
	ok("blend over box pixel", tol(dp[48*W + 48], 0xff807f00u, 2));

	/* --- Batch 2: opaque blit sub-rect, separate submit --------------- */
	etna2d_fill(ctx, big, 0, 0, W, H, 0xff123456u);
	etna2d_blit(ctx, src, 0, 0, big, 64, 64, 128, 96,
		    ETNA2D_BLEND_NONE, ETNA2D_ROP_COPY);
	rc = etna2d_finish(ctx);
	ok("blit finish", rc == ETNA2D_OK);

	bp = etna2d_surface_map(big);
	ok("blit copied region", bp[(64+10)*W + (64+10)] == 0x80800000u);
	ok("blit background intact", bp[10*W + 10] == 0xff123456u);
	ok("blit outside region", bp[(64+96+5)*W + (64+128+5)] == 0xff123456u);

	/* --- Batch 3: 2x stretch --------------------------------------- */
	{
		struct etna2d_surface *small = NULL;
		uint32_t *smp;
		etna2d_surface_create(dev, 128, 128, &small);
		smp = etna2d_surface_map(small);
		for (int y = 0; y < 128; y++)
			for (int x = 0; x < 128; x++) {
				int q = (x >= 64) | ((y >= 64) << 1);
				static const uint32_t col[4] = {
					0xffff0000u, 0xff00ff00u,
					0xff0000ffu, 0xffffffffu };
				smp[y*128 + x] = col[q];
			}
		etna2d_surface_cpu_fini(small);

		etna2d_fill(ctx, dst, 0, 0, W, H, 0xff000000u);
		etna2d_stretch(ctx, small, dst, 0, 0, W, H);
		etna2d_finish(ctx);

		dp = etna2d_surface_map(dst);
		ok("stretch TL red",   dp[64*W + 64]   == 0xffff0000u);
		ok("stretch TR green", dp[64*W + 192]  == 0xff00ff00u);
		ok("stretch BL blue",  dp[192*W + 64]  == 0xff0000ffu);
		ok("stretch BR white", dp[192*W + 192] == 0xffffffffu);
		/* Same source stretched into an OFFSET rect. The DE addresses the
		 * source relative to the draw rect's top-left, so a placed rect
		 * needs no origin correction. Stretch a 64x64 copy into a 64x64
		 * dest at (128,128) and verify all four quadrants land at their
		 * expected sub-positions (regression for placed StretchBlt). */
		etna2d_fill(ctx, dst, 0, 0, W, H, 0xff000000u);
		etna2d_stretch(ctx, small, dst, 128, 128, 64, 64);
		etna2d_finish(ctx);
		dp = etna2d_surface_map(dst);
		ok("stretch@offset TL red",   dp[(128+16)*W + (128+16)] == 0xffff0000u);
		ok("stretch@offset TR green", dp[(128+16)*W + (128+48)] == 0xff00ff00u);
		ok("stretch@offset BL blue",  dp[(128+48)*W + (128+16)] == 0xff0000ffu);
		ok("stretch@offset BR white", dp[(128+48)*W + (128+48)] == 0xffffffffu);
		ok("stretch@offset bg above", dp[100*W + (128+16)] == 0xff000000u);

		/* Batched stretch: same source tiled into a 3x3 grid of 40x40
		 * dest rects via the ONE-submit batch API. Verifies the batched
		 * path (shared state emitted once, only clip+draw per tile) does
		 * not hang and produces the same quadrant pattern as the per-op
		 * path — including a tile that overhangs the surface edge. */
		etna2d_fill(ctx, dst, 0, 0, W, H, 0xff000000u);
		{
			struct etna2d_rect grid[9];
			int gi = 0;
			for (int gy = 0; gy < 3; gy++)
				for (int gx = 0; gx < 3; gx++) {
					grid[gi].x = 10 + gx * 44;
					grid[gi].y = 10 + gy * 44;
					grid[gi].w = 40;
					grid[gi].h = 40;
					gi++;
				}
			/* push the last tile partly off the right/bottom edge */
			grid[8].x = W - 20;
			grid[8].y = H - 20;
			ok("stretch_batch returns ok",
			   etna2d_stretch_batch(ctx, small, dst, grid, 9) == ETNA2D_OK);
		}
		etna2d_finish(ctx);
		dp = etna2d_surface_map(dst);
		/* tile 0 at (10,10) size 40: quadrant centres at +10 / +30 */
		ok("batch tile0 TL red",   dp[(10+10)*W + (10+10)] == 0xffff0000u);
		ok("batch tile0 BR white", dp[(10+30)*W + (10+30)] == 0xffffffffu);
		/* centre tile at (54,54) */
		ok("batch tile4 TL red",   dp[(54+10)*W + (54+10)] == 0xffff0000u);
		ok("batch tile4 BR white", dp[(54+30)*W + (54+30)] == 0xffffffffu);
		/* overhanging tile: its in-bounds top-left must be red, and the
		 * pixel just outside the surface must not have been touched (no
		 * hang / no OOB write is implied by finish succeeding). */
		ok("batch overhang TL red", dp[(H-20+10)*W + (W-20+10)] == 0xffff0000u);

		etna2d_surface_destroy(small);
	}

	/* --- Batch 4: out-of-bounds clipping (robustness) -----------------
	 * Ops whose destination rect runs past the surface must be clipped by
	 * the library, never handed to the DE (which has no bounds check and
	 * would MMU-fault / hang the GPU on an out-of-range read/write). We
	 * push fill, blend and stretch off the edges, then verify the GPU
	 * still runs, in-bounds pixels are correct, and untouched regions stay
	 * untouched. Ops are placed to avoid overlap so each check is exact. */
	{
		etna2d_fill(ctx, dst, 0, 0, W, H, 0xff111111u);   /* known bg */

		/* fill straddling the top-left (negative origin): the in-bounds
		 * part [0..16] x [0..16] gets 0xffff0000. */
		etna2d_fill(ctx, dst, -32, -32, 48, 48, 0xffff0000u);

		/* fill straddling the bottom-right corner: only [W-16..W] x
		 * [H-16..H] should be written green. */
		etna2d_fill(ctx, dst, W - 16, H - 16, 64, 64, 0xff00ff00u);

		/* opaque blit whose source overhangs the RIGHT edge: dest
		 * [W-16..W] x [0..16], reads src[0..16, 0..16] (clipped w). */
		etna2d_blit(ctx, src, 0, 0, dst, W - 16, 0, 64, 16,
			    ETNA2D_BLEND_NONE, ETNA2D_ROP_COPY);

		/* stretch overhanging the bottom edge only, in a clear column
		 * band [8..40] x [H-40..H+88] -> clipped to [..H]. Must not
		 * fault; the last row in that band must be written. */
		etna2d_stretch(ctx, src, dst, 8, H - 40, 32, 128);

		/* fully off-surface: pure no-ops, must be dropped cleanly. */
		etna2d_fill(ctx, dst, W + 10, H + 10, 32, 32, 0xffabcdefu);
		etna2d_blend_over(ctx, src, dst, W + 100, 0, 32, 32);
		etna2d_stretch(ctx, src, dst, -300, -300, 64, 64);

		rc = etna2d_finish(ctx);
		ok("clipped ops finish (no GPU hang)", rc == ETNA2D_OK);

		dp = etna2d_surface_map(dst);
		ok("clip: top-left fill in-bounds",
		   dp[4*W + 4] == 0xffff0000u);
		ok("clip: bottom-right fill in-bounds",
		   dp[(H - 8)*W + (W - 8)] == 0xff00ff00u);
		ok("clip: right-overhang blit in-bounds",
		   dp[4*W + (W - 8)] == 0x80800000u);
		ok("clip: bottom-overhang stretch reached last row",
		   dp[(H - 1)*W + 20] != 0xff111111u);
		ok("clip: interior bg untouched",
		   dp[(H/2)*W + (W/2)] == 0xff111111u);
	}

out:
	etna2d_context_destroy(ctx);
	etna2d_surface_destroy(big);
	etna2d_surface_destroy(dst);
	etna2d_surface_destroy(src);
	etna2d_device_close(dev);

	printf(fails ? "\n=== %d CHECK(S) FAILED ===\n" : "\n=== ALL CHECKS PASSED ===\n", fails);
	return fails ? 1 : 0;
}
