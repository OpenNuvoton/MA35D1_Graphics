/*
 * etna2d.h — libetna2d: thin userspace 2D op builder for Vivante GC520.
 *
 * Layering:
 *   caller (benchmark app, Qt QPA plugin)
 *       │  etna2d_fill / blit / blend / stretch  (this API)
 *       ▼
 *   libetna2d  ──►  libdrm_etnaviv  ──►  etnaviv.ko  (GC520 DE)
 *
 * All surfaces are 32bpp ARGB8888 (DE_FORMAT_A8R8G8B8 / SWIZZLE_ARGB),
 * which is what Qt's raster engine and the KMS scanout buffers use.
 *
 * Threading: a context and its surfaces are NOT thread-safe; use one
 * context per thread. Different contexts on the same device are fine.
 *
 * Batching: ops accumulate in the context's command stream and are only
 * handed to the GPU on etna2d_flush() (async) or etna2d_finish() (blocks
 * for the fence). Batch N ops into one submit for throughput.
 */
#ifndef LIBETNA2D_H
#define LIBETNA2D_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------- */
/* Opaque types                                                           */
/* ---------------------------------------------------------------------- */
struct etna2d_device;    /* a DRM render node + 2D pipe                   */
struct etna2d_context;   /* an accumulating command stream                */
struct etna2d_surface;   /* a 32bpp ARGB drawable backed by an etna BO    */

/* ---------------------------------------------------------------------- */
/* Result codes                                                           */
/* ---------------------------------------------------------------------- */
enum {
	ETNA2D_OK          =  0,
	ETNA2D_EINVAL      = -1,   /* bad argument                          */
	ETNA2D_ENODEV      = -2,   /* open / no 2D-capable core             */
	ETNA2D_ENOMEM      = -3,   /* allocation / mapping failure          */
	ETNA2D_EIO         = -4,   /* submit / import failure               */
};

/* ---------------------------------------------------------------------- */
/* Blend modes (Porter-Duff subset the GC520 PE supports directly)        */
/* ---------------------------------------------------------------------- */
enum etna2d_blend {
	ETNA2D_BLEND_NONE = 0, /* opaque copy (ROP SRCCOPY), alpha ignored  */
	ETNA2D_BLEND_SRC_OVER, /* premultiplied source over dest            */
};

/* Common raster ops for etna2d_blit(). Value is the Vivante ROP4 code. */
enum {
	ETNA2D_ROP_COPY = 0xcc, /* dst = src  (SRCCOPY)                     */
	ETNA2D_ROP_XOR  = 0x66, /* dst = src ^ dst                          */
	ETNA2D_ROP_AND  = 0x88, /* dst = src & dst                          */
	ETNA2D_ROP_OR   = 0xee, /* dst = src | dst                          */
};

/* Optional log sink. level: 0=err 1=warn 2=info 3=debug. NULL = silent. */
typedef void (*etna2d_log_fn)(int level, const char *msg, void *user);
void etna2d_set_logger(etna2d_log_fn fn, void *user);

/* Human-readable string for a result code. */
const char *etna2d_strerror(int rc);

/* ---------------------------------------------------------------------- */
/* Device                                                                 */
/* ---------------------------------------------------------------------- */

/* Open a DRM render node (e.g. "/dev/dri/renderD128"), locate a
 * 2D-capable core (FEATURES_0 bit 9) and create an ETNA_PIPE_2D pipe.
 * On success *out is set and ETNA2D_OK returned. */
int  etna2d_device_open(const char *path, struct etna2d_device **out);
void etna2d_device_close(struct etna2d_device *dev);

/* Introspection (valid after open). */
uint32_t etna2d_device_model(const struct etna2d_device *dev);
uint32_t etna2d_device_revision(const struct etna2d_device *dev);
uint64_t etna2d_device_features0(const struct etna2d_device *dev);

/* ---------------------------------------------------------------------- */
/* Surfaces                                                               */
/* ---------------------------------------------------------------------- */

/* Allocate a fresh ARGB8888 surface (w*h), write-combine mapping.
 * stride is chosen automatically (w*4, honouring GC520 alignment). */
int etna2d_surface_create(struct etna2d_device *dev, int w, int h,
			  struct etna2d_surface **out);

/* Wrap a scanout/shared buffer imported from a PRIME dma-buf fd
 * (Phase 3 flow: dumb buffer on card0 -> export -> import here).
 * The caller owns the fd's lifetime beyond this call is NOT required;
 * libetna2d dup-imports it. stride is the exporter's stride in bytes. */
int etna2d_surface_from_dmabuf(struct etna2d_device *dev, int fd,
			       int w, int h, uint32_t stride,
			       struct etna2d_surface **out);

void etna2d_surface_destroy(struct etna2d_surface *s);

/* Accessors. */
int      etna2d_surface_width(const struct etna2d_surface *s);
int      etna2d_surface_height(const struct etna2d_surface *s);
uint32_t etna2d_surface_stride(const struct etna2d_surface *s);

/* CPU map (write-combine). Returns NULL on failure. After CPU writes and
 * before a GPU op reads the surface, call etna2d_surface_cpu_fini() to
 * flush the write-combine buffer. */
void *etna2d_surface_map(struct etna2d_surface *s);
void  etna2d_surface_cpu_fini(struct etna2d_surface *s); /* WC drain barrier */

/* ---------------------------------------------------------------------- */
/* Context (batched command stream)                                       */
/* ---------------------------------------------------------------------- */

/* Create a context bound to dev. hint_ops sizes the initial command
 * buffer (0 = default). */
int  etna2d_context_create(struct etna2d_device *dev, unsigned hint_ops,
			   struct etna2d_context **out);
void etna2d_context_destroy(struct etna2d_context *ctx);

/* Submit accumulated ops. flush() returns without waiting; finish()
 * blocks until the GPU fence signals (surface pixels are then coherent
 * for CPU readback). Both reset the stream for the next batch. */
int etna2d_flush(struct etna2d_context *ctx);
int etna2d_finish(struct etna2d_context *ctx);

/* ---------------------------------------------------------------------- */
/* 2D operations — each appends to ctx; nothing runs until flush/finish.  */
/* Coordinates and sizes are in pixels; rectangles are half-open.         */
/* ---------------------------------------------------------------------- */

/* Solid fill of dst[x,y,w,h] with a 0xAARRGGBB value (DE CLEAR). */
int etna2d_fill(struct etna2d_context *ctx, struct etna2d_surface *dst,
		int x, int y, int w, int h, uint32_t argb);

/* Copy src[sx,sy,w,h] -> dst[dx,dy] at 1:1 using ROP (default COPY).
 * blend selects opaque copy vs premultiplied source-over. */
int etna2d_blit(struct etna2d_context *ctx,
		struct etna2d_surface *src, int sx, int sy,
		struct etna2d_surface *dst, int dx, int dy,
		int w, int h, enum etna2d_blend blend, unsigned rop);

/* Premultiplied source-over of the whole src onto dst[dx,dy,w,h].
 * Convenience wrapper over etna2d_blit(..., ETNA2D_BLEND_SRC_OVER). */
int etna2d_blend_over(struct etna2d_context *ctx,
		      struct etna2d_surface *src,
		      struct etna2d_surface *dst,
		      int dx, int dy, int w, int h);

/* Point-sampled scaled copy of the whole src into dst[dx,dy,dw,dh]. */
int etna2d_stretch(struct etna2d_context *ctx,
		   struct etna2d_surface *src,
		   struct etna2d_surface *dst,
		   int dx, int dy, int dw, int dh);

/* Destination rectangle for the batched stretch API. */
struct etna2d_rect { int x, y, w, h; };

/* Batched point-sampled stretch: scale the whole src into each of the `n`
 * destination rectangles in `rects`. The heavy per-op DE state (source,
 * dest, ROP, format, pe_reset) is emitted ONCE for the whole batch; each
 * rect then costs only its stretch factor, clip window and DRAW_2D. This
 * cuts the fixed per-tile cost several-fold versus calling etna2d_stretch()
 * in a loop, which matters when a frame is hundreds/thousands of small
 * tiles (heavy downscale). Rects are clipped individually; fully-off rects
 * are skipped. The batch auto-flushes internally to respect the kernel's
 * per-submit size cap, re-emitting shared state as needed. */
int etna2d_stretch_batch(struct etna2d_context *ctx,
			 struct etna2d_surface *src,
			 struct etna2d_surface *dst,
			 const struct etna2d_rect *rects, int n);

#ifdef __cplusplus
}
#endif

#endif /* LIBETNA2D_H */
