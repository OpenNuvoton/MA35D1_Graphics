/*
 * libetna2d.c — GC520 2D op builder. See etna2d.h for the API contract.
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xf86drm.h"
#include "etnaviv_drmif.h"
#include "etnaviv_drm.h"     /* ETNA_BO_WC */

#include "state.xml.h"
#include "state_2d.xml.h"
#include "cmdstream.xml.h"

#include "etna2d.h"

/* ---------------------------------------------------------------------- */
/* Logging                                                                */
/* ---------------------------------------------------------------------- */
static etna2d_log_fn g_log;
static void         *g_log_user;

void etna2d_set_logger(etna2d_log_fn fn, void *user)
{
	g_log = fn;
	g_log_user = user;
}

static void logf_(int level, const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	if (!g_log)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	g_log(level, buf, g_log_user);
}

#define LOG_ERR(...)  logf_(0, __VA_ARGS__)
#define LOG_WARN(...) logf_(1, __VA_ARGS__)
#define LOG_INFO(...) logf_(2, __VA_ARGS__)
#define LOG_DBG(...)  logf_(3, __VA_ARGS__)

const char *etna2d_strerror(int rc)
{
	switch (rc) {
	case ETNA2D_OK:     return "ok";
	case ETNA2D_EINVAL: return "invalid argument";
	case ETNA2D_ENODEV: return "no device / no 2D core";
	case ETNA2D_ENOMEM: return "out of memory";
	case ETNA2D_EIO:    return "I/O / submit failure";
	default:            return "unknown error";
	}
}

/* ---------------------------------------------------------------------- */
/* Opaque structs                                                         */
/* ---------------------------------------------------------------------- */
struct etna2d_device {
	int                 fd;
	struct etna_device *dev;
	struct etna_gpu    *gpu;
	struct etna_pipe   *pipe;
	int                 core;
	uint64_t            model, revision, features0;
};

struct etna2d_surface {
	struct etna2d_device *dev;
	struct etna_bo       *bo;
	int                   w, h;
	uint32_t              stride;   /* bytes */
	void                 *map;
	int                   imported; /* from dma-buf: don't assume ownership sizing */
};

struct etna2d_context {
	struct etna2d_device   *dev;
	struct etna_cmd_stream *stream;
	unsigned                pending; /* ops since last flush */
};

/* GC520 surface base alignment (bytes). Per Vivante alignment doc, DE
 * addresses/strides align to 64B; 4bpp * 16px = 64B, so round width to 16. */
#define ETNA2D_ALIGN 64u
static inline uint32_t align_up(uint32_t v, uint32_t a)
{
	return (v + a - 1) & ~(a - 1);
}

/* ---------------------------------------------------------------------- */
/* Low-level command emit (identical to the validated Phase 1 sequences)  */
/* ---------------------------------------------------------------------- */
static inline void emit_load_state(struct etna_cmd_stream *s,
				   uint16_t offset, uint16_t count)
{
	etna_cmd_stream_emit(s,
		VIV_FE_LOAD_STATE_HEADER_OP_LOAD_STATE |
		VIV_FE_LOAD_STATE_HEADER_OFFSET(offset) |
		(VIV_FE_LOAD_STATE_HEADER_COUNT(count) &
		 VIV_FE_LOAD_STATE_HEADER_COUNT__MASK));
}

static void ss(struct etna_cmd_stream *s, uint32_t addr, uint32_t val)
{
	etna_cmd_stream_reserve(s, 2);
	emit_load_state(s, addr >> 2, 1);
	etna_cmd_stream_emit(s, val);
}

static void ss_reloc(struct etna_cmd_stream *s, uint32_t addr,
		     struct etna_bo *bo, uint32_t flags)
{
	etna_cmd_stream_reserve(s, 2);
	emit_load_state(s, addr >> 2, 1);
	etna_cmd_stream_reloc(s, &(struct etna_reloc){
		.bo = bo, .flags = flags, .offset = 0,
	});
}

static void pe_reset(struct etna_cmd_stream *s)
{
	ss(s, VIVS_DE_SRC_COLOR_BG,          0);
	ss(s, VIVS_DE_SRC_COLOR_FG,          0);
	ss(s, VIVS_DE_DEST_COLOR_KEY,        0);
	ss(s, VIVS_DE_GLOBAL_SRC_COLOR,      0);
	ss(s, VIVS_DE_GLOBAL_DEST_COLOR,     0);
	ss(s, VIVS_DE_COLOR_MULTIPLY_MODES,  0);
	ss(s, VIVS_DE_PE_TRANSPARENCY,       0);
	ss(s, VIVS_DE_PE_CONTROL,            0);
	ss(s, VIVS_DE_PE_DITHER_LOW,         0xffffffff);
	ss(s, VIVS_DE_PE_DITHER_HIGH,        0xffffffff);
}

static void draw_2d_rect(struct etna_cmd_stream *s,
			 int x0, int y0, int x1, int y1)
{
	etna_cmd_stream_reserve(s, 4);
	etna_cmd_stream_emit(s,
		VIV_FE_DRAW_2D_HEADER_OP_DRAW_2D |
		VIV_FE_DRAW_2D_HEADER_COUNT(1));
	etna_cmd_stream_emit(s, 0x0);
	etna_cmd_stream_emit(s,
		VIV_FE_DRAW_2D_TOP_LEFT_X(x0) | VIV_FE_DRAW_2D_TOP_LEFT_Y(y0));
	etna_cmd_stream_emit(s,
		VIV_FE_DRAW_2D_BOTTOM_RIGHT_X(x1) | VIV_FE_DRAW_2D_BOTTOM_RIGHT_Y(y1));
}

static void op_tail(struct etna_cmd_stream *s)
{
	ss(s, 1, 0);
	ss(s, 1, 0);
	ss(s, 1, 0);
	/* NOTE: the PE 2D cache is NOT flushed here. Flushing after every op
	 * forces a full pipeline stall between back-to-back tiles, which
	 * dominates frame time when a frame is hundreds of small ops (e.g. a
	 * heavily-tiled downscale). The flush is only needed once, before the
	 * destination is consumed by another engine (scanout) or read back on
	 * the CPU; etna2d_flush()/etna2d_finish() emit it exactly once per
	 * submit. Overlapping writes to the same dest within a submit are not
	 * a concern for the 2D pipe (rects are processed in order). */
}

/* Emit a single PE 2D cache flush. Called once per submit by flush/finish. */
static void emit_pe_flush(struct etna_cmd_stream *s)
{
	etna_cmd_stream_reserve(s, 2);
	ss(s, VIVS_GL_FLUSH_CACHE, VIVS_GL_FLUSH_CACHE_PE2D);
}

 /* Worst-case command words for a single op (blit-with-blend is the largest:
  * ~40 state writes @2w + draw_2d @4w + tail @8w). Round up generously. */
 #define ETNA2D_OP_WORDS 128u

 /* Hard cap on stream buffer size.
  *
  * The etnaviv kernel driver rejects any gem_submit with stream_size > SZ_128K
  * (131,072 bytes = 32,768 words):
  *   if (args->stream_size > SZ_128K || ...)
  *       return -EINVAL;  // "submit arguments out of size limits"
  *
  * Allocating a buffer larger than this means etna_cmd_stream_reserve() never
  * sees it as "full", so no auto-flush fires — the oversized submit reaches the
  * kernel and fails.  Cap the allocation here so the buffer fills and
  * auto-flushes before that limit is reached. */
 #define ETNA2D_STREAM_WORDS_MAX (131072u / sizeof(uint32_t))  /* SZ_128K / 4 = 32 768 words */

/* Reserve a whole op's worth of space up front.
 *
 * etna_cmd_stream_reserve() does NOT grow the buffer — when space runs out
 * it *auto-flushes* (submits) the stream. If that happened partway through
 * an op, the op's BO relocation (address patch) would land in one submit
 * while the DRAW_2D that consumes it landed in the next, leaving the DE to
 * draw from a stale/garbage GPU address -> "MMU page not present" fault and
 * a GPU hang. Reserving the whole op here forces any flush to occur at a
 * clean op boundary, keeping every op atomic. */
static void op_begin(struct etna2d_context *ctx)
{
	uint32_t before = etna_cmd_stream_offset(ctx->stream);
	etna_cmd_stream_reserve(ctx->stream, ETNA2D_OP_WORDS);
	/* If reserve triggered an auto-flush the stream reset to offset 0 and
	 * the previously-accumulated ops were already submitted; drop them
	 * from our pending count so flush()/finish() don't double-submit. */
	if (etna_cmd_stream_offset(ctx->stream) < before)
		ctx->pending = 0;
}


static void set_source(struct etna_cmd_stream *s,
		       struct etna_bo *src, uint32_t src_stride,
		       int src_w, int src_h, int src_x, int src_y)
{
	ss(s, VIVS_DE_SRC_STRIDE, src_stride);
	ss(s, VIVS_DE_SRC_ROTATION_CONFIG, 0);
	ss(s, VIVS_DE_SRC_CONFIG,
	   VIVS_DE_SRC_CONFIG_PE10_SOURCE_FORMAT(DE_FORMAT_A8R8G8B8) |
	   VIVS_DE_SRC_CONFIG_SOURCE_FORMAT(DE_FORMAT_A8R8G8B8) |
	   VIVS_DE_SRC_CONFIG_SWIZZLE(DE_SWIZZLE_ARGB) |
	   VIVS_DE_SRC_CONFIG_LOCATION_MEMORY);
	ss(s, VIVS_DE_SRC_ORIGIN,
	   VIVS_DE_SRC_ORIGIN_X(src_x) | VIVS_DE_SRC_ORIGIN_Y(src_y));
	ss(s, VIVS_DE_SRC_SIZE,
	   VIVS_DE_SRC_SIZE_X(src_w) | VIVS_DE_SRC_SIZE_Y(src_h));
	ss_reloc(s, VIVS_DE_SRC_ADDRESS, src, ETNA_RELOC_READ);
}

/* ---------------------------------------------------------------------- */
/* Device                                                                 */
/* ---------------------------------------------------------------------- */
int etna2d_device_open(const char *path, struct etna2d_device **out)
{
	struct etna2d_device *d;
	drmVersionPtr ver;

	if (!path || !out)
		return ETNA2D_EINVAL;

	d = calloc(1, sizeof(*d));
	if (!d)
		return ETNA2D_ENOMEM;

	d->fd = open(path, O_RDWR);
	if (d->fd < 0) {
		LOG_ERR("open(%s): %m", path);
		free(d);
		return ETNA2D_ENODEV;
	}

	ver = drmGetVersion(d->fd);
	if (ver) {
		LOG_INFO("drm: %s %d.%d.%d (%s)", ver->name,
			 ver->version_major, ver->version_minor,
			 ver->version_patchlevel, ver->desc);
		drmFreeVersion(ver);
	}

	d->dev = etna_device_new(d->fd);
	if (!d->dev) {
		LOG_ERR("etna_device_new: %m");
		goto err_fd;
	}

	/* Find a core advertising 2D (FEATURES_0 bit 9). */
	for (d->core = 0; ; d->core++) {
		struct etna_gpu *gpu = etna_gpu_new(d->dev, d->core);
		uint64_t feat;

		if (!gpu) {
			LOG_ERR("no 2D-capable core (scanned %d)", d->core);
			goto err_dev;
		}
		if (etna_gpu_get_param(gpu, ETNA_GPU_FEATURES_0, &feat) == 0 &&
		    (feat & (1u << 9))) {
			d->gpu = gpu;
			d->features0 = feat;
			etna_gpu_get_param(gpu, ETNA_GPU_MODEL, &d->model);
			etna_gpu_get_param(gpu, ETNA_GPU_REVISION, &d->revision);
			break;
		}
		etna_gpu_del(gpu);
	}

	LOG_INFO("gpu: model GC%03x rev 0x%04x core %d FEATURES_0=0x%08llx",
		 (unsigned)d->model, (unsigned)d->revision, d->core,
		 (unsigned long long)d->features0);

	d->pipe = etna_pipe_new(d->gpu, ETNA_PIPE_2D);
	if (!d->pipe) {
		LOG_ERR("etna_pipe_new(2D): %m");
		goto err_gpu;
	}

	*out = d;
	return ETNA2D_OK;

err_gpu:
	etna_gpu_del(d->gpu);
err_dev:
	etna_device_del(d->dev);
err_fd:
	close(d->fd);
	free(d);
	return ETNA2D_ENODEV;
}

void etna2d_device_close(struct etna2d_device *d)
{
	if (!d)
		return;
	if (d->pipe) etna_pipe_del(d->pipe);
	if (d->gpu)  etna_gpu_del(d->gpu);
	if (d->dev)  etna_device_del(d->dev);
	if (d->fd >= 0) close(d->fd);
	free(d);
}

uint32_t etna2d_device_model(const struct etna2d_device *d) { return d ? (uint32_t)d->model : 0; }
uint32_t etna2d_device_revision(const struct etna2d_device *d) { return d ? (uint32_t)d->revision : 0; }
uint64_t etna2d_device_features0(const struct etna2d_device *d) { return d ? d->features0 : 0; }

/* ---------------------------------------------------------------------- */
/* Surfaces                                                               */
/* ---------------------------------------------------------------------- */
int etna2d_surface_create(struct etna2d_device *dev, int w, int h,
			  struct etna2d_surface **out)
{
	struct etna2d_surface *s;
	uint32_t stride, size;

	if (!dev || !out || w <= 0 || h <= 0)
		return ETNA2D_EINVAL;

	stride = align_up((uint32_t)w * 4u, ETNA2D_ALIGN);
	size   = stride * (uint32_t)h;

	s = calloc(1, sizeof(*s));
	if (!s)
		return ETNA2D_ENOMEM;

	s->bo = etna_bo_new(dev->dev, size, ETNA_BO_WC);
	if (!s->bo) {
		LOG_ERR("etna_bo_new(%u): %m", size);
		free(s);
		return ETNA2D_ENOMEM;
	}
	s->dev = dev;
	s->w = w;
	s->h = h;
	s->stride = stride;
	LOG_DBG("surface %dx%d stride=%u size=%u handle=%u",
		w, h, stride, size, etna_bo_handle(s->bo));
	*out = s;
	return ETNA2D_OK;
}

int etna2d_surface_from_dmabuf(struct etna2d_device *dev, int fd,
			       int w, int h, uint32_t stride,
			       struct etna2d_surface **out)
{
	struct etna2d_surface *s;

	if (!dev || !out || fd < 0 || w <= 0 || h <= 0 || stride < (uint32_t)w * 4u)
		return ETNA2D_EINVAL;

	s = calloc(1, sizeof(*s));
	if (!s)
		return ETNA2D_ENOMEM;

	/* etnaviv imports the dma-buf into its own MMU v2 address space. */
	s->bo = etna_bo_from_dmabuf(dev->dev, fd);
	if (!s->bo) {
		LOG_ERR("etna_bo_from_dmabuf(fd=%d): %m", fd);
		free(s);
		return ETNA2D_EIO;
	}
	s->dev = dev;
	s->w = w;
	s->h = h;
	s->stride = stride;
	s->imported = 1;
	LOG_DBG("surface(imported) %dx%d stride=%u handle=%u",
		w, h, stride, etna_bo_handle(s->bo));
	*out = s;
	return ETNA2D_OK;
}

void etna2d_surface_destroy(struct etna2d_surface *s)
{
	if (!s)
		return;
	if (s->bo)
		etna_bo_del(s->bo);
	free(s);
}

int      etna2d_surface_width(const struct etna2d_surface *s)  { return s ? s->w : 0; }
int      etna2d_surface_height(const struct etna2d_surface *s) { return s ? s->h : 0; }
uint32_t etna2d_surface_stride(const struct etna2d_surface *s) { return s ? s->stride : 0; }

void *etna2d_surface_map(struct etna2d_surface *s)
{
	if (!s)
		return NULL;
	if (!s->map)
		s->map = etna_bo_map(s->bo);
	return s->map;
}

void etna2d_surface_cpu_fini(struct etna2d_surface *s)
{
	(void)s;
	/* WC mappings need a store barrier before the GPU reads the pixels. */
	__sync_synchronize();
}

/* ---------------------------------------------------------------------- */
/* Context                                                                */
/* ---------------------------------------------------------------------- */
int etna2d_context_create(struct etna2d_device *dev, unsigned hint_ops,
			  struct etna2d_context **out)
{
	struct etna2d_context *ctx;
	uint32_t words;

	if (!dev || !out)
		return ETNA2D_EINVAL;

	/* Size the buffer to hold hint_ops full ops without splitting.
	 * ETNA2D_OP_WORDS is the worst-case op size; op_begin() keeps every
	 * op atomic even if this estimate is exceeded.
	 * Clamp to ETNA2D_STREAM_WORDS_MAX so the buffer never exceeds the
	 * kernel's SZ_128K per-submit stream size hard limit. */
	words = (hint_ops ? hint_ops : 16) * ETNA2D_OP_WORDS;
	if (words < 0x400) words = 0x400;
	if (words > ETNA2D_STREAM_WORDS_MAX)
		words = ETNA2D_STREAM_WORDS_MAX;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return ETNA2D_ENOMEM;

	ctx->stream = etna_cmd_stream_new(dev->pipe, words, NULL, NULL);
	if (!ctx->stream) {
		LOG_ERR("etna_cmd_stream_new: %m");
		free(ctx);
		return ETNA2D_ENOMEM;
	}
	ctx->dev = dev;
	*out = ctx;
	return ETNA2D_OK;
}

void etna2d_context_destroy(struct etna2d_context *ctx)
{
	if (!ctx)
		return;
	if (ctx->stream)
		etna_cmd_stream_del(ctx->stream);
	free(ctx);
}

int etna2d_flush(struct etna2d_context *ctx)
{
	if (!ctx)
		return ETNA2D_EINVAL;
	if (ctx->pending) {
		emit_pe_flush(ctx->stream);         /* one flush per submit */
		etna_cmd_stream_flush(ctx->stream); /* resets the stream */
		ctx->pending = 0;
	}
	return ETNA2D_OK;
}

int etna2d_finish(struct etna2d_context *ctx)
{
	if (!ctx)
		return ETNA2D_EINVAL;
	if (ctx->pending)
		emit_pe_flush(ctx->stream);         /* one flush per submit */
	etna_cmd_stream_finish(ctx->stream);    /* submits + waits + resets */
	ctx->pending = 0;
	return ETNA2D_OK;
}

/* ---------------------------------------------------------------------- */
/* Operations                                                             */
/* ---------------------------------------------------------------------- */
int etna2d_fill(struct etna2d_context *ctx, struct etna2d_surface *dst,
		int x, int y, int w, int h, uint32_t argb)
{
	struct etna_cmd_stream *s;

	if (!ctx || !dst || w <= 0 || h <= 0)
		return ETNA2D_EINVAL;
	/* Clip to the destination; the DE clip window bounds the write but a
	 * rect starting off-surface would still program a bad base offset. */
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x >= dst->w || y >= dst->h)
		return ETNA2D_OK;
	if (x + w > dst->w) w = dst->w - x;
	if (y + h > dst->h) h = dst->h - y;
	if (w <= 0 || h <= 0)
		return ETNA2D_OK;
	s = ctx->stream;

	LOG_DBG("fill (%d,%d %dx%d) argb=0x%08x", x, y, w, h, argb);
	op_begin(ctx);

	ss(s, VIVS_DE_SRC_STRIDE, 0);
	ss(s, VIVS_DE_SRC_ROTATION_CONFIG, 0);
	ss(s, VIVS_DE_SRC_CONFIG, 0);
	ss(s, VIVS_DE_SRC_ORIGIN, 0);
	ss(s, VIVS_DE_SRC_SIZE, 0);
	ss(s, VIVS_DE_STRETCH_FACTOR_LOW, 0);
	ss(s, VIVS_DE_STRETCH_FACTOR_HIGH, 0);
	ss_reloc(s, VIVS_DE_DEST_ADDRESS, dst->bo, ETNA_RELOC_WRITE);
	ss(s, VIVS_DE_DEST_STRIDE, dst->stride);
	ss(s, VIVS_DE_DEST_ROTATION_CONFIG, 0);
	ss(s, VIVS_DE_DEST_CONFIG,
	   VIVS_DE_DEST_CONFIG_FORMAT(DE_FORMAT_A8R8G8B8) |
	   VIVS_DE_DEST_CONFIG_COMMAND_CLEAR |
	   VIVS_DE_DEST_CONFIG_SWIZZLE(DE_SWIZZLE_ARGB) |
	   VIVS_DE_DEST_CONFIG_TILED_DISABLE |
	   VIVS_DE_DEST_CONFIG_MINOR_TILED_DISABLE);
	ss(s, VIVS_DE_ROP,
	   VIVS_DE_ROP_ROP_FG(0xcc) | VIVS_DE_ROP_ROP_BG(0xcc) |
	   VIVS_DE_ROP_TYPE_ROP4);
	ss(s, VIVS_DE_CLIP_TOP_LEFT,
	   VIVS_DE_CLIP_TOP_LEFT_X(x) | VIVS_DE_CLIP_TOP_LEFT_Y(y));
	ss(s, VIVS_DE_CLIP_BOTTOM_RIGHT,
	   VIVS_DE_CLIP_BOTTOM_RIGHT_X(x + w) | VIVS_DE_CLIP_BOTTOM_RIGHT_Y(y + h));
	ss(s, VIVS_DE_CONFIG, 0);
	ss(s, VIVS_DE_SRC_ORIGIN_FRACTION, 0);
	ss(s, VIVS_DE_ALPHA_CONTROL, 0);
	ss(s, VIVS_DE_ALPHA_MODES, 0);
	ss(s, VIVS_DE_DEST_ROTATION_HEIGHT, 0);
	ss(s, VIVS_DE_SRC_ROTATION_HEIGHT, 0);
	ss(s, VIVS_DE_ROT_ANGLE, 0);

	ss(s, VIVS_DE_CLEAR_PIXEL_VALUE32, argb);
	ss(s, VIVS_DE_CLEAR_BYTE_MASK, 0xff);
	ss(s, VIVS_DE_CLEAR_PIXEL_VALUE_LOW, argb);
	ss(s, VIVS_DE_CLEAR_PIXEL_VALUE_HIGH, argb);

	pe_reset(s);
	draw_2d_rect(s, x, y, x + w, y + h);
	op_tail(s);
	ctx->pending++;
	return ETNA2D_OK;
}

int etna2d_blit(struct etna2d_context *ctx,
		struct etna2d_surface *src, int sx, int sy,
		struct etna2d_surface *dst, int dx, int dy,
		int w, int h, enum etna2d_blend blend, unsigned rop)
{
	struct etna_cmd_stream *s;
	int blending = (blend == ETNA2D_BLEND_SRC_OVER);

	if (!ctx || !src || !dst || w <= 0 || h <= 0)
		return ETNA2D_EINVAL;

	/* Clip the transfer rect to BOTH the source and destination extents.
	 *
	 * A 1:1 blit reads src[sx..sx+w, sy..sy+h] and writes/reads
	 * dst[dx..dx+w, dy..dy+h]. The DE has no implicit bounds check: if any
	 * edge exceeds a surface it walks past the BO's mapped pages and takes
	 * an "MMU page not present" fault (hanging the GPU). The DE_CLIP window
	 * bounds the *write* but NOT the source read nor the destination read
	 * done for SRC_OVER blending, so we must shrink w/h here. Also drop the
	 * op entirely if it starts outside either surface. */
	if (dx < 0 || dy < 0 || sx < 0 || sy < 0 ||
	    dx >= dst->w || dy >= dst->h || sx >= src->w || sy >= src->h)
		return ETNA2D_OK;   /* fully clipped: nothing to draw */
	if (dx + w > dst->w) w = dst->w - dx;
	if (dy + h > dst->h) h = dst->h - dy;
	if (sx + w > src->w) w = src->w - sx;
	if (sy + h > src->h) h = src->h - sy;
	if (w <= 0 || h <= 0)
		return ETNA2D_OK;

	if (!rop)
		rop = ETNA2D_ROP_COPY;
	s = ctx->stream;

	LOG_DBG("blit src(%d,%d) -> dst(%d,%d) %dx%d blend=%d rop=0x%02x",
		sx, sy, dx, dy, w, h, blend, rop);

	op_begin(ctx);
	set_source(s, src->bo, src->stride, src->w, src->h, sx, sy);
	ss(s, VIVS_DE_STRETCH_FACTOR_LOW, 0);
	ss(s, VIVS_DE_STRETCH_FACTOR_HIGH, 0);
	ss_reloc(s, VIVS_DE_DEST_ADDRESS, dst->bo,
		 blending ? (ETNA_RELOC_READ | ETNA_RELOC_WRITE) : ETNA_RELOC_WRITE);
	ss(s, VIVS_DE_DEST_STRIDE, dst->stride);
	ss(s, VIVS_DE_DEST_ROTATION_CONFIG, 0);
	ss(s, VIVS_DE_DEST_CONFIG,
	   VIVS_DE_DEST_CONFIG_FORMAT(DE_FORMAT_A8R8G8B8) |
	   VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT |
	   VIVS_DE_DEST_CONFIG_SWIZZLE(DE_SWIZZLE_ARGB) |
	   VIVS_DE_DEST_CONFIG_TILED_DISABLE |
	   VIVS_DE_DEST_CONFIG_MINOR_TILED_DISABLE);
	ss(s, VIVS_DE_ROP,
	   VIVS_DE_ROP_ROP_FG(rop & 0xff) | VIVS_DE_ROP_ROP_BG(rop & 0xff) |
	   VIVS_DE_ROP_TYPE_ROP4);
	ss(s, VIVS_DE_CLIP_TOP_LEFT,
	   VIVS_DE_CLIP_TOP_LEFT_X(dx) | VIVS_DE_CLIP_TOP_LEFT_Y(dy));
	ss(s, VIVS_DE_CLIP_BOTTOM_RIGHT,
	   VIVS_DE_CLIP_BOTTOM_RIGHT_X(dx + w) | VIVS_DE_CLIP_BOTTOM_RIGHT_Y(dy + h));
	ss(s, VIVS_DE_CONFIG, 0);
	ss(s, VIVS_DE_SRC_ORIGIN_FRACTION, 0);

	if (blending) {
		/* premultiplied source-over (validated Phase 1) */
		ss(s, VIVS_DE_ALPHA_CONTROL, VIVS_DE_ALPHA_CONTROL_ENABLE_ON);
		ss(s, VIVS_DE_ALPHA_MODES,
		   VIVS_DE_ALPHA_MODES_SRC_ALPHA_MODE_NORMAL |
		   VIVS_DE_ALPHA_MODES_DST_ALPHA_MODE_NORMAL |
		   VIVS_DE_ALPHA_MODES_GLOBAL_SRC_ALPHA_MODE_NORMAL |
		   VIVS_DE_ALPHA_MODES_GLOBAL_DST_ALPHA_MODE_NORMAL |
		   VIVS_DE_ALPHA_MODES_SRC_BLENDING_MODE(DE_BLENDMODE_ONE) |
		   VIVS_DE_ALPHA_MODES_DST_BLENDING_MODE(DE_BLENDMODE_INVERSED));
	} else {
		ss(s, VIVS_DE_ALPHA_CONTROL, 0);
		ss(s, VIVS_DE_ALPHA_MODES, 0);
	}

	ss(s, VIVS_DE_DEST_ROTATION_HEIGHT, 0);
	ss(s, VIVS_DE_SRC_ROTATION_HEIGHT, 0);
	ss(s, VIVS_DE_ROT_ANGLE, 0);

	pe_reset(s);
	draw_2d_rect(s, dx, dy, dx + w, dy + h);
	op_tail(s);
	ctx->pending++;
	return ETNA2D_OK;
}

int etna2d_blend_over(struct etna2d_context *ctx,
		      struct etna2d_surface *src,
		      struct etna2d_surface *dst,
		      int dx, int dy, int w, int h)
{
	return etna2d_blit(ctx, src, 0, 0, dst, dx, dy, w, h,
			   ETNA2D_BLEND_SRC_OVER, ETNA2D_ROP_COPY);
}

int etna2d_stretch(struct etna2d_context *ctx,
		   struct etna2d_surface *src,
		   struct etna2d_surface *dst,
		   int dx, int dy, int dw, int dh)
{
	struct etna_cmd_stream *s;
	uint32_t fx, fy;
	int cx0, cy0, cx1, cy1;

	if (!ctx || !src || !dst || dw <= 0 || dh <= 0)
		return ETNA2D_EINVAL;
	s = ctx->stream;

	/* Vivante .16 fixed-point stretch factor, point sampled:
	 *   factor = ((srcSize - 1) << 16) / (dstSize - 1)
	 * Computed from the FULL requested dw/dh so the scaling geometry is
	 * preserved even when the destination rect is partially off-surface. */
	fx = (dw > 1) ? (uint32_t)(((uint64_t)(src->w - 1) << 16) / (dw - 1)) : 0;
	fy = (dh > 1) ? (uint32_t)(((uint64_t)(src->h - 1) << 16) / (dh - 1)) : 0;

	/* The DE derives the stretch geometry from the DRAW_2D rect size
	 * (source spans SRC_SIZE across the full draw rect), while the DE_CLIP
	 * window bounds the actual writes. So the DRAW rect must stay the FULL
	 * requested dw x dh — clipping it would rescale the tile into the
	 * smaller rect (the "compressed" partially-off tiles). We instead clip
	 * only the CLIP window to the surface and keep the draw rect intact.
	 * The draw rect may legitimately extend past the surface; the DE walks
	 * source pixels but only WRITES inside the clip window, so no unmapped
	 * dest page is touched. */
	cx0 = dx < 0 ? 0 : dx;
	cy0 = dy < 0 ? 0 : dy;
	cx1 = dx + dw; if (cx1 > dst->w) cx1 = dst->w;
	cy1 = dy + dh; if (cy1 > dst->h) cy1 = dst->h;
	if (cx0 >= cx1 || cy0 >= cy1)
		return ETNA2D_OK;   /* fully clipped: nothing to draw */

	/* SRC_ORIGIN / SRC_SIZE define the source sampling window; the DE
	 * addresses the source RELATIVE to the draw rect's top-left, so a
	 * placed dest rect samples from source (0,0) with no correction. */

	LOG_DBG("stretch src %dx%d -> dst(%d,%d) %dx%d factor x=0x%05x y=0x%05x",
		src->w, src->h, dx, dy, dw, dh, fx, fy);

	op_begin(ctx);
	set_source(s, src->bo, src->stride, src->w, src->h, 0, 0);
	ss(s, VIVS_DE_STRETCH_FACTOR_LOW,  VIVS_DE_STRETCH_FACTOR_LOW_X(fx));
	ss(s, VIVS_DE_STRETCH_FACTOR_HIGH, VIVS_DE_STRETCH_FACTOR_HIGH_Y(fy));
	ss_reloc(s, VIVS_DE_DEST_ADDRESS, dst->bo, ETNA_RELOC_WRITE);
	ss(s, VIVS_DE_DEST_STRIDE, dst->stride);
	ss(s, VIVS_DE_DEST_ROTATION_CONFIG, 0);
	ss(s, VIVS_DE_DEST_CONFIG,
	   VIVS_DE_DEST_CONFIG_FORMAT(DE_FORMAT_A8R8G8B8) |
	   VIVS_DE_DEST_CONFIG_COMMAND_STRETCH_BLT |
	   VIVS_DE_DEST_CONFIG_SWIZZLE(DE_SWIZZLE_ARGB) |
	   VIVS_DE_DEST_CONFIG_TILED_DISABLE |
	   VIVS_DE_DEST_CONFIG_MINOR_TILED_DISABLE);
	ss(s, VIVS_DE_ROP,
	   VIVS_DE_ROP_ROP_FG(0xcc) | VIVS_DE_ROP_ROP_BG(0xcc) |
	   VIVS_DE_ROP_TYPE_ROP4);
	ss(s, VIVS_DE_CLIP_TOP_LEFT,
	   VIVS_DE_CLIP_TOP_LEFT_X(cx0) | VIVS_DE_CLIP_TOP_LEFT_Y(cy0));
	ss(s, VIVS_DE_CLIP_BOTTOM_RIGHT,
	   VIVS_DE_CLIP_BOTTOM_RIGHT_X(cx1) | VIVS_DE_CLIP_BOTTOM_RIGHT_Y(cy1));
	ss(s, VIVS_DE_CONFIG, 0);
	ss(s, VIVS_DE_SRC_ORIGIN_FRACTION, 0);
	ss(s, VIVS_DE_ALPHA_CONTROL, 0);
	ss(s, VIVS_DE_ALPHA_MODES, 0);
	ss(s, VIVS_DE_DEST_ROTATION_HEIGHT, 0);
	ss(s, VIVS_DE_SRC_ROTATION_HEIGHT, 0);
	ss(s, VIVS_DE_ROT_ANGLE, 0);

	pe_reset(s);
	/* FULL draw rect: preserves the stretch geometry. Writes are bounded
	 * by the DE_CLIP window (cx0,cy0)-(cx1,cy1) set above. */
	draw_2d_rect(s, dx, dy, dx + dw, dy + dh);
	op_tail(s);
	ctx->pending++;
	return ETNA2D_OK;
}

/* Emit the DE state shared by every rect of a stretch batch: source setup,
 * dest surface/format, ROP and pe_reset. Factor, clip and the DRAW_2D are
 * emitted per rect by the batch loop. Costs ~30 words; done once per submit
 * instead of once per tile. */
static void stretch_batch_state(struct etna_cmd_stream *s,
				struct etna2d_surface *src,
				struct etna2d_surface *dst)
{
	set_source(s, src->bo, src->stride, src->w, src->h, 0, 0);
	ss_reloc(s, VIVS_DE_DEST_ADDRESS, dst->bo, ETNA_RELOC_WRITE);
	ss(s, VIVS_DE_DEST_STRIDE, dst->stride);
	ss(s, VIVS_DE_DEST_ROTATION_CONFIG, 0);
	ss(s, VIVS_DE_DEST_CONFIG,
	   VIVS_DE_DEST_CONFIG_FORMAT(DE_FORMAT_A8R8G8B8) |
	   VIVS_DE_DEST_CONFIG_COMMAND_STRETCH_BLT |
	   VIVS_DE_DEST_CONFIG_SWIZZLE(DE_SWIZZLE_ARGB) |
	   VIVS_DE_DEST_CONFIG_TILED_DISABLE |
	   VIVS_DE_DEST_CONFIG_MINOR_TILED_DISABLE);
	ss(s, VIVS_DE_ROP,
	   VIVS_DE_ROP_ROP_FG(0xcc) | VIVS_DE_ROP_ROP_BG(0xcc) |
	   VIVS_DE_ROP_TYPE_ROP4);
	ss(s, VIVS_DE_CONFIG, 0);
	ss(s, VIVS_DE_SRC_ORIGIN_FRACTION, 0);
	ss(s, VIVS_DE_ALPHA_CONTROL, 0);
	ss(s, VIVS_DE_ALPHA_MODES, 0);
	ss(s, VIVS_DE_DEST_ROTATION_HEIGHT, 0);
	ss(s, VIVS_DE_SRC_ROTATION_HEIGHT, 0);
	ss(s, VIVS_DE_ROT_ANGLE, 0);
	pe_reset(s);
}

int etna2d_stretch_batch(struct etna2d_context *ctx,
			 struct etna2d_surface *src,
			 struct etna2d_surface *dst,
			 const struct etna2d_rect *rects, int n)
{
	struct etna_cmd_stream *s;
	int state_dirty = 1;

	if (!ctx || !src || !dst || (n > 0 && !rects))
		return ETNA2D_EINVAL;
	if (n <= 0)
		return ETNA2D_OK;
	s = ctx->stream;

	/* Per-rect footprint in words: factor (2×2) + src origin (2) +
	 * clip (2×2) + draw_2d (4) ≈ 16; STATE_WORDS covers the shared block
	 * (set_source + dest cfg + ROP + pe_reset ≈ 30×2). Keep each submit
	 * well under the kernel's 128 KB (32 K-word) cap with margin for
	 * relocs: flush after ~800 rects (~13 K words). */
	const uint32_t PER_RECT_WORDS = 32;
	const uint32_t STATE_WORDS    = 96;
	const int RECTS_PER_SUBMIT = 800;
	int since_flush = 0;

	for (int i = 0; i < n; i++) {
		int dx = rects[i].x, dy = rects[i].y;
		int dw = rects[i].w, dh = rects[i].h;
		int cx0, cy0, cx1, cy1;
		uint32_t fx, fy;

		if (dw <= 0 || dh <= 0)
			continue;

		cx0 = dx < 0 ? 0 : dx;
		cy0 = dy < 0 ? 0 : dy;
		cx1 = dx + dw; if (cx1 > dst->w) cx1 = dst->w;
		cy1 = dy + dh; if (cy1 > dst->h) cy1 = dst->h;
		if (cx0 >= cx1 || cy0 >= cy1)
			continue;   /* fully clipped */

		/* Periodic flush to bound the submit under the kernel cap. */
		if (since_flush >= RECTS_PER_SUBMIT) {
			emit_pe_flush(s);
			etna_cmd_stream_flush(s);   /* resets stream */
			ctx->pending = 0;
			state_dirty = 1;
			since_flush = 0;
		}

		/* Reserve enough for this rect, plus the shared state block when
		 * it must be (re-)emitted. etna_cmd_stream_reserve() does not
		 * grow the buffer: if the reservation does not fit it auto-
		 * flushes (submitting and resetting the stream), which drops the
		 * ops accumulated so far and requires re-emitting shared state.
		 * Detect that via the offset going backwards. */
		uint32_t need = PER_RECT_WORDS + (state_dirty ? STATE_WORDS : 0);
		uint32_t before = etna_cmd_stream_offset(s);
		etna_cmd_stream_reserve(s, need);
		if (etna_cmd_stream_offset(s) < before) {
			ctx->pending = 0;
			state_dirty = 1;
		}
		if (state_dirty) {
			stretch_batch_state(s, src, dst);
			state_dirty = 0;
		}

		fx = (dw > 1) ? (uint32_t)(((uint64_t)(src->w - 1) << 16) / (dw - 1)) : 0;
		fy = (dh > 1) ? (uint32_t)(((uint64_t)(src->h - 1) << 16) / (dh - 1)) : 0;

		ss(s, VIVS_DE_STRETCH_FACTOR_LOW,  VIVS_DE_STRETCH_FACTOR_LOW_X(fx));
		ss(s, VIVS_DE_STRETCH_FACTOR_HIGH, VIVS_DE_STRETCH_FACTOR_HIGH_Y(fy));
		ss(s, VIVS_DE_SRC_ORIGIN, 0);
		ss(s, VIVS_DE_CLIP_TOP_LEFT,
		   VIVS_DE_CLIP_TOP_LEFT_X(cx0) | VIVS_DE_CLIP_TOP_LEFT_Y(cy0));
		ss(s, VIVS_DE_CLIP_BOTTOM_RIGHT,
		   VIVS_DE_CLIP_BOTTOM_RIGHT_X(cx1) | VIVS_DE_CLIP_BOTTOM_RIGHT_Y(cy1));
		draw_2d_rect(s, dx, dy, dx + dw, dy + dh);
		/* No per-rect cache flush: the DE processes DRAW_2D in order and
		 * the batch is flushed once at submit time. */
		ctx->pending++;
		since_flush++;
	}
	return ETNA2D_OK;
}
