/*
 * Copyright (c) 2026 Microchip Technology Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * nano2D GC520UL draw unit for LVGL 9.x.
 *
 * Model (exploits the GPU's queue-many / commit-once architecture):
 *   - evaluate_cb marks FILL + IMAGE tasks (within the supported subset) for
 *     this unit with preference_score 0 so it wins over the SW renderer.
 *   - dispatch_cb takes one available task per call, translates it to an
 *     n2d_fill/n2d_blit (which only QUEUE into the GPU command buffer), and
 *     accumulates it. When no more tasks are available for this unit (or the
 *     batch cap is hit) it issues a single blocking n2d_commit() for the whole
 *     batch, then marks every queued task FINISHED.
 *
 * Cache protocol (cached Cortex-A7; LVGL VDB is cacheable DRAM):
 *   - Before a blended op that READS the dest (translucent fill / alpha image),
 *     clean (writeback) the dest region so the GPU sees current CPU pixels.
 *   - Before any blit, clean the source image so the GPU reads fresh data.
 *   - After commit, INVALIDATE each written dest region so the XLCDC flush's
 *     full-buffer writeback cannot clobber GPU output with stale CPU lines.
 *
 * Clipping: every op sets the GPU clip rectangle to the task's clipped area,
 * so partial/edge-clipped fills and (scaled/rotated) images are scissored by
 * hardware — no inverse-transform source math needed.
 */

#include "lv_draw_nano2d.h"

#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>
#include "draw/lv_draw_private.h"
#include "misc/lv_area_private.h"

#include "nano2D.h"

LOG_MODULE_REGISTER(nano2d_draw, LOG_LEVEL_INF);

#define DRAW_UNIT_ID_NANO2D 10

/* Batch ceiling. The 64 KB command buffer holds far more than this; the cap
 * just bounds the per-commit task array and latency. */
#define NANO2D_BATCH_MAX 32

/* RGB565 destination only (the panel/VDB format on this board). */
#define DEST_BPP 2

/* The GC520 fetches the source surface from a base address that must be aligned
 * (nano2D's own surface convention aligns stride to 64; an under-aligned base
 * mis-fetches and the blit comes out torn/sheared). Sources that don't meet
 * this are declined in evaluate_cb and rendered by the SW unit instead. */
#define GPU_SRC_ALIGN 64U

typedef struct {
	lv_draw_unit_t base_unit;

	/* Tasks queued into the GPU command buffer, awaiting commit. */
	lv_draw_task_t *queued[NANO2D_BATCH_MAX];
	/* Clipped (scissored) dest area per queued task, absolute coords —
	 * used for precise post-commit cache invalidation. */
	lv_area_t       written[NANO2D_BATCH_MAX];
	uint32_t        queued_cnt;

	/* Destination surface, built once per batch. */
	n2d_buffer_t    dest;
	bool            dest_valid;

#if NANO2D_ASYNC_COMMIT
	/* A kicked batch is executing on the GPU, awaiting n2d_wait + finalize. */
	bool            commit_pending;
	/* Layer the in-flight batch draws into. LVGL dispatches per layer, so a
	 * later dispatch_cb may run for a different layer; finalize must use this
	 * one (its buffer matches the queued `written[]` coords), not the arg. */
	const lv_layer_t *pending_layer;
#endif
} nano2d_unit_t;

/* ====================================================================== *
 *  Format mapping
 * ====================================================================== */

/* Map an LVGL color format to an nano2D source format. Returns false if the
 * format is not GPU-blittable. *has_alpha is set when per-pixel alpha must
 * drive blending. */
static bool cf_to_n2d(lv_color_format_t cf, n2d_buffer_format_t *fmt,
		      bool *has_alpha)
{
	switch (cf) {
	case LV_COLOR_FORMAT_RGB565:
		*fmt = N2D_RGB565;
		*has_alpha = false;
		return true;
	case LV_COLOR_FORMAT_RGB888:
		*fmt = N2D_RGB888;
		*has_alpha = false;
		return true;
	case LV_COLOR_FORMAT_XRGB8888:
		*fmt = N2D_XRGB8888; /* X treated as opaque */
		*has_alpha = false;
		return true;
	case LV_COLOR_FORMAT_ARGB8888:
		*fmt = N2D_ARGB8888;
		*has_alpha = true;
		return true;
	default:
		return false;
	}
}

/* ====================================================================== *
 *  GPU state helpers
 * ====================================================================== */

static void n2d_global_alpha(n2d_global_alpha_t mode, uint8_t value)
{
	n2d_state_config_t sc;

	memset(&sc, 0, sizeof(sc));
	sc.state = N2D_SET_GLOBAL_ALPHA;
	sc.config.globalAlpha.srcMode = mode;
	sc.config.globalAlpha.srcValue = value;
	sc.config.globalAlpha.dstMode = N2D_GLOBAL_ALPHA_OFF;
	sc.config.globalAlpha.dstValue = 0;
	(void)n2d_set(&sc);
}

/* Enable/disable source colour premultiply.
 *
 * The GPU's SRC_OVER uses srcFactor=ONE (out = src + dst*(1-srcA)) — i.e. it
 * expects a PREMULTIPLIED source. LVGL provides STRAIGHT (non-premultiplied)
 * colour+alpha, so without premultiplying the source first the blend adds the
 * source colour at full intensity and the result is oversaturated / wrong hue
 * (a 50%-red-over-blue fill renders solid red instead of pink). Enabling pixel
 * premultiply makes the GPU compute src.rgb *= srcA before the blend, which is
 * exactly straight-alpha SRC_OVER. `global` additionally folds a global alpha
 * (fade) into the source for the opa<255 cases. */
static void set_src_premultiply(n2d_pixel_color_multiply_mode_t pixel,
				n2d_global_color_multiply_mode_t global)
{
	n2d_state_config_t sc;

	memset(&sc, 0, sizeof(sc));
	sc.state = N2D_SET_PIXEL_MULTIPLY_MODE;
	sc.config.pixelMultiplyMode.srcPremult = pixel;
	sc.config.pixelMultiplyMode.dstPremult = N2D_COLOR_MULTIPLY_DISABLE;
	sc.config.pixelMultiplyMode.srcGlobal = global;
	sc.config.pixelMultiplyMode.dstDemult = N2D_COLOR_MULTIPLY_DISABLE;
	(void)n2d_set(&sc);
}

/* Scissor all subsequent ops to `clip` (absolute coords), expressed relative
 * to the layer buffer. */
static void n2d_clip_to(const lv_layer_t *layer, const lv_area_t *clip)
{
	n2d_state_config_t sc;

	memset(&sc, 0, sizeof(sc));
	sc.state = N2D_SET_CLIP_RECTANGLE;
	sc.config.clipRect.x = clip->x1 - layer->buf_area.x1;
	sc.config.clipRect.y = clip->y1 - layer->buf_area.y1;
	sc.config.clipRect.width = lv_area_get_width(clip);
	sc.config.clipRect.height = lv_area_get_height(clip);
	(void)n2d_set(&sc);
}

/* ====================================================================== *
 *  Cache helpers (row-by-row so we touch only the affected pixels)
 * ====================================================================== */

#define CACHE_LINE 64U

static void cache_rows(const lv_layer_t *layer, const lv_area_t *a, bool invalidate)
{
	int32_t w = lv_area_get_width(a);
	size_t bytes = (size_t)w * DEST_BPP;

	for (int32_t y = a->y1; y <= a->y2; y++) {
		uint8_t *row = lv_draw_layer_go_to_xy(
			(lv_layer_t *)layer, a->x1 - layer->buf_area.x1,
			y - layer->buf_area.y1);
		/* Round to cache-line bounds. An unaligned invalidate leaves the
		 * partial boundary lines un-invalidated, so stale CPU pixels at
		 * the row edges survive and the display's full-buffer clean writes
		 * them back over GPU output (edge lines / dropped tiles). The
		 * rounded-in neighbours are GPU-drawn background, so this is safe. */
		uintptr_t s = (uintptr_t)row & ~(uintptr_t)(CACHE_LINE - 1U);
		uintptr_t e = ((uintptr_t)row + bytes + CACHE_LINE - 1U) &
			      ~(uintptr_t)(CACHE_LINE - 1U);

		if (invalidate) {
			sys_cache_data_invd_range((void *)s, e - s);
		} else {
			sys_cache_data_flush_range((void *)s, e - s);
		}
	}
}

/* ====================================================================== *
 *  Destination surface
 * ====================================================================== */

static void build_dest(const lv_layer_t *layer, n2d_buffer_t *b)
{
	const lv_draw_buf_t *db = layer->draw_buf;
	void *base = (void *)db->data;


	memset(b, 0, sizeof(*b));
	b->width = (n2d_int32_t)db->header.w;
	b->height = (n2d_int32_t)db->header.h;
	b->stride = (n2d_int32_t)db->header.stride; /* use the VDB's real pitch */
	b->format = N2D_RGB565;
	b->tiling = N2D_LINEAR;
	b->memory = base;
	b->gpu = (n2d_uintptr_t)base;
}

/* ====================================================================== *
 *  FILL / IMAGE translation (queue only — no commit here)
 * ====================================================================== */

static bool queue_fill(nano2d_unit_t *u, const lv_layer_t *layer,
		       lv_draw_task_t *t, const lv_area_t *clip)
{
	lv_draw_fill_dsc_t *dsc = t->draw_dsc;
	n2d_rectangle_t r = {
		.x = clip->x1 - layer->buf_area.x1,
		.y = clip->y1 - layer->buf_area.y1,
		.width = lv_area_get_width(clip),
		.height = lv_area_get_height(clip),
	};
	n2d_blend_t blend;
	uint32_t argb;

	n2d_clip_to(layer, clip);

	/* Clean the destination rows before the GPU writes them. This pushes any
	 * dirty CPU cache lines covering this region out to DRAM so the cache can't
	 * later evict one on top of the GPU's output (the post-commit invalidate
	 * can't undo an eviction that already reached DRAM - that race shows up as
	 * stale gaps along a fill edge). It also hands the GPU the current pixels
	 * for the translucent case below, which reads the destination. */
	cache_rows(layer, clip, false);

	if (dsc->opa < LV_OPA_MAX) {
		/* Translucent: straight-alpha SRC_OVER. The colour's alpha byte
		 * (set below) carries opa; pixel premultiply turns it into
		 * colour*opa before the blend. */
		n2d_global_alpha(N2D_GLOBAL_ALPHA_OFF, 255);
		set_src_premultiply(N2D_COLOR_MULTIPLY_ENABLE,
				    N2D_GLOBAL_COLOR_MULTIPLY_DISABLE);
		blend = N2D_BLEND_SRC_OVER;
	} else {
		n2d_global_alpha(N2D_GLOBAL_ALPHA_OFF, 255);
		set_src_premultiply(N2D_COLOR_MULTIPLY_DISABLE,
				    N2D_GLOBAL_COLOR_MULTIPLY_DISABLE);
		blend = N2D_BLEND_NONE;
	}

	argb = (lv_color_to_u32(dsc->color) & 0x00FFFFFFu) |
	       ((uint32_t)dsc->opa << 24);

	n2d_error_t fe = n2d_fill(&u->dest, &r, (n2d_color_t)argb, blend);

	if (fe != N2D_SUCCESS) {
		LOG_ERR("n2d_fill err 0x%x (%dx%d blend %d)", fe, r.width,
			r.height, (int)blend);
	}
	return true;
}

static bool queue_blit(nano2d_unit_t *u, const lv_layer_t *layer,
		       lv_draw_task_t *t, const lv_area_t *clip)
{
	lv_draw_image_dsc_t *dsc = t->draw_dsc;
	const lv_image_dsc_t *img = dsc->src;
	n2d_buffer_format_t fmt;
	bool has_alpha;
	n2d_buffer_t src;
	n2d_blend_t blend;
	uint32_t stride;

	if (!cf_to_n2d(dsc->header.cf, &fmt, &has_alpha)) {
		return false;
	}

	stride = dsc->header.stride;
	if (stride == 0) {
		stride = (uint32_t)dsc->header.w *
			 lv_color_format_get_size(dsc->header.cf);
	}

	memset(&src, 0, sizeof(src));
	src.width = (n2d_int32_t)dsc->header.w;
	src.height = (n2d_int32_t)dsc->header.h;
	src.stride = (n2d_int32_t)stride;
	src.format = fmt;
	src.tiling = N2D_LINEAR;
	src.memory = (void *)img->data;
	src.gpu = (n2d_uintptr_t)img->data;

	/* LVGL's rotation is clockwise for a positive angle, but nano2D's N2D_90 /
	 * N2D_270 orientations rotate the source the opposite (counter-clockwise)
	 * way. Swap 90 and 270 so the GPU output matches LVGL (and the SW renderer).
	 * 0 and 180 are the same under either handedness. */
	switch (dsc->rotation) {
	case 0:
		src.orientation = N2D_0;
		break;
	case 900:
		src.orientation = N2D_270;
		break;
	case 1800:
		src.orientation = N2D_180;
		break;
	case 2700:
		src.orientation = N2D_90;
		break;
	default:
		return false;
	}

	/* Geometry. t->_real_area is the image's full transformed on-screen rect
	 * (== image_area when there is no transform); the source maps onto it with
	 * src.orientation doing 90/180/270 and an unequal src/dst size doing scale.
	 *
	 * For an UNROTATED image we blit only the visible sub-region: the dest is
	 * the (in-bounds) clipped rect and the source is the proportional sub-rect.
	 * This keeps the src and dst sizes consistent, so an image hanging off the
	 * framebuffer edge is cropped rather than squished - n2d_blit clamps the
	 * dest rect to the surface but still scales from the full requested dst
	 * size, so a dst rect that runs past the edge would otherwise rescale the
	 * whole source into the remaining strip. When the image is fully visible
	 * this reduces to (full source -> full _real_area).
	 *
	 * For 90/180/270 the visible dst can't be mapped back to a source sub-rect
	 * by a simple axis swap, so evaluate_cb only accepts rotated images that are
	 * fully visible; here we blit the whole source onto its full rect. */
	n2d_rectangle_t sr;
	n2d_rectangle_t dr;

	if (src.orientation == N2D_0) {
		int32_t real_w = lv_area_get_width(&t->_real_area);
		int32_t real_h = lv_area_get_height(&t->_real_area);
		int32_t ox = clip->x1 - t->_real_area.x1;
		int32_t oy = clip->y1 - t->_real_area.y1;
		int32_t vw = lv_area_get_width(clip);
		int32_t vh = lv_area_get_height(clip);

		sr.x = (n2d_int32_t)((int64_t)ox * dsc->header.w / real_w);
		sr.y = (n2d_int32_t)((int64_t)oy * dsc->header.h / real_h);
		sr.width = (n2d_int32_t)((int64_t)vw * dsc->header.w / real_w);
		sr.height = (n2d_int32_t)((int64_t)vh * dsc->header.h / real_h);

		/* Rounding can nudge the sub-rect past the source; clamp it in. */
		if (sr.width < 1) {
			sr.width = 1;
		}
		if (sr.height < 1) {
			sr.height = 1;
		}
		if (sr.x + sr.width > (n2d_int32_t)dsc->header.w) {
			sr.width = (n2d_int32_t)dsc->header.w - sr.x;
		}
		if (sr.y + sr.height > (n2d_int32_t)dsc->header.h) {
			sr.height = (n2d_int32_t)dsc->header.h - sr.y;
		}

		dr.x = clip->x1 - layer->buf_area.x1;
		dr.y = clip->y1 - layer->buf_area.y1;
		dr.width = vw;
		dr.height = vh;
	} else {
		sr.x = 0;
		sr.y = 0;
		sr.width = (n2d_int32_t)dsc->header.w;
		sr.height = (n2d_int32_t)dsc->header.h;

		dr.x = t->_real_area.x1 - layer->buf_area.x1;
		dr.y = t->_real_area.y1 - layer->buf_area.y1;
		dr.width = lv_area_get_width(&t->_real_area);
		dr.height = lv_area_get_height(&t->_real_area);
	}

	n2d_clip_to(layer, clip);

	/* GPU reads the source -> clean it. */
	sys_cache_data_flush_range((void *)img->data, (size_t)stride * dsc->header.h);

	/* Clean the destination rows before the GPU writes them: stops a dirty CPU
	 * cache line from later evicting over the GPU output (which the post-commit
	 * invalidate can't undo), and supplies current dest pixels to the blended
	 * (SRC_OVER) cases below, which read the destination. */
	cache_rows(layer, clip, false);

	if (dsc->opa < LV_OPA_MAX && has_alpha) {
		/* Per-pixel alpha AND a global fade: premultiply by both the
		 * pixel alpha and the global alpha. */
		n2d_global_alpha(N2D_GLOBAL_ALPHA_SCALE, dsc->opa);
		set_src_premultiply(N2D_COLOR_MULTIPLY_ENABLE,
				    N2D_GLOBAL_COLOR_MULTIPLY_ALPHA);
		blend = N2D_BLEND_SRC_OVER;
	} else if (dsc->opa < LV_OPA_MAX) {
		/* Opaque-format source faded by a global alpha: premultiply the
		 * source colour by the global alpha only. */
		n2d_global_alpha(N2D_GLOBAL_ALPHA_ON, dsc->opa);
		set_src_premultiply(N2D_COLOR_MULTIPLY_DISABLE,
				    N2D_GLOBAL_COLOR_MULTIPLY_ALPHA);
		blend = N2D_BLEND_SRC_OVER;
	} else if (has_alpha) {
		/* Per-pixel alpha, full opacity: straight-alpha SRC_OVER via
		 * pixel premultiply. */
		n2d_global_alpha(N2D_GLOBAL_ALPHA_OFF, 255);
		set_src_premultiply(N2D_COLOR_MULTIPLY_ENABLE,
				    N2D_GLOBAL_COLOR_MULTIPLY_DISABLE);
		blend = N2D_BLEND_SRC_OVER;
	} else {
		/* Fully opaque copy. */
		n2d_global_alpha(N2D_GLOBAL_ALPHA_OFF, 255);
		set_src_premultiply(N2D_COLOR_MULTIPLY_DISABLE,
				    N2D_GLOBAL_COLOR_MULTIPLY_DISABLE);
		blend = N2D_BLEND_NONE;
	}

	n2d_error_t be = n2d_blit(&u->dest, &dr, &src, &sr, blend);

	if (be != N2D_SUCCESS) {
		LOG_ERR("n2d_blit err 0x%x (cf=%u %dx%d->%dx%d blend %d alpha %d)",
			be, (unsigned)dsc->header.cf, src.width, src.height,
			dr.width, dr.height, (int)blend, (int)has_alpha);
	}
	return true;
}

/* ====================================================================== *
 *  Batch commit
 * ====================================================================== */

/* Reclaim a completed batch: the GPU has finished writing DRAM, so drop stale
 * CPU lines for each written region (the XLCDC full-buffer flush would otherwise
 * write them back over the GPU output) and release the tasks. */
static void finalize_batch(nano2d_unit_t *u, const lv_layer_t *layer)
{
	for (uint32_t i = 0; i < u->queued_cnt; i++) {
		cache_rows(layer, &u->written[i], true);
		u->queued[i]->state = LV_DRAW_TASK_STATE_FINISHED;
	}

	u->queued_cnt = 0;
	u->dest_valid = false;
#if NANO2D_ASYNC_COMMIT
	u->commit_pending = false;
#endif
}

static void commit_batch(nano2d_unit_t *u, const lv_layer_t *layer)
{
	if (u->queued_cnt == 0) {
		u->dest_valid = false;
		return;
	}

#if NANO2D_ASYNC_COMMIT
	/* Kick without blocking and mark the batch in-flight; reclaim it later
	 * (dispatch_cb) with n2d_wait(). Between now and then the dispatch loop runs
	 * the SW unit on tasks independent of these IN_PROGRESS ones - the overlap. */
	n2d_error_t err = n2d_commit_ex(N2D_FALSE);

	if (err != N2D_SUCCESS) {
		LOG_ERR("n2d_commit_ex failed: 0x%x (%u tasks)", err, u->queued_cnt);
	}
	for (uint32_t i = 0; i < u->queued_cnt; i++) {
		u->queued[i]->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
	}
	u->commit_pending = true;
	u->pending_layer = layer;
#else
	n2d_error_t err = n2d_commit(); /* submit whole batch + block on IRQ */

	if (err != N2D_SUCCESS) {
		LOG_ERR("n2d_commit failed: 0x%x (%u tasks)", err, u->queued_cnt);
	}
	finalize_batch(u, layer);
#endif
}

/* ====================================================================== *
 *  Draw-unit callbacks
 * ====================================================================== */

static int32_t evaluate_cb(lv_draw_unit_t *draw_unit, lv_draw_task_t *task)
{
	LV_UNUSED(draw_unit);

	switch (task->type) {
	case LV_DRAW_TASK_TYPE_FILL: {
		lv_draw_fill_dsc_t *dsc = task->draw_dsc;

		if (!(dsc->radius == 0 && dsc->grad.dir == LV_GRAD_DIR_NONE &&
		      dsc->opa > LV_OPA_TRANSP &&
		      dsc->base.layer->color_format == LV_COLOR_FORMAT_RGB565)) {
			return 0;
		}
		break;
	}
	case LV_DRAW_TASK_TYPE_IMAGE: {
		lv_draw_image_dsc_t *dsc = task->draw_dsc;
		n2d_buffer_format_t fmt;
		bool has_alpha;

		if (!(dsc->base.layer->color_format == LV_COLOR_FORMAT_RGB565 &&
		      lv_image_src_get_type(dsc->src) == LV_IMAGE_SRC_VARIABLE &&
		      /* Source base must meet the GPU's alignment (see GPU_SRC_ALIGN);
		       * otherwise let SW draw it. Safe to deref dsc->src here — the
		       * SRC_VARIABLE check above guarantees it is an lv_image_dsc_t. */
		      (((uintptr_t)((const lv_image_dsc_t *)dsc->src)->data &
			(GPU_SRC_ALIGN - 1U)) == 0) &&
		      cf_to_n2d(dsc->header.cf, &fmt, &has_alpha) &&
		      dsc->clip_radius == 0 && dsc->bitmap_mask_src == NULL &&
		      dsc->sup == NULL && dsc->tile == 0 &&
		      dsc->blend_mode == LV_BLEND_MODE_NORMAL &&
		      dsc->recolor_opa <= LV_OPA_MIN && dsc->skew_x == 0 &&
		      dsc->skew_y == 0 && dsc->opa > LV_OPA_TRANSP &&
		      dsc->scale_x > 0 && dsc->scale_y > 0 &&
		      (dsc->rotation == 0 || dsc->rotation == 900 ||
		       dsc->rotation == 1800 || dsc->rotation == 2700))) {
			return 0;
		}

		/* Rotated blits use the full-source path (queue_blit can't map a
		 * clipped dest back to a source sub-rect through the rotation), so a
		 * rotated image must be fully visible. Leave partially-clipped rotated
		 * images to the SW renderer. */
		if (dsc->rotation != 0) {
			const lv_area_t *ra = &task->_real_area;
			const lv_area_t *ca = &task->clip_area;

			if (ra->x1 < ca->x1 || ra->y1 < ca->y1 ||
			    ra->x2 > ca->x2 || ra->y2 > ca->y2) {
				return 0;
			}
		}
		break;
	}
	default:
		return 0;
	}

	task->preferred_draw_unit_id = DRAW_UNIT_ID_NANO2D;
	task->preference_score = 0; /* beats SW (100) */
	return 0;
}

static int32_t dispatch_cb(lv_draw_unit_t *draw_unit, lv_layer_t *layer)
{
	nano2d_unit_t *u = (nano2d_unit_t *)draw_unit;
	lv_draw_task_t *t;
	lv_area_t clip;
	bool ok;

#if NANO2D_ASYNC_COMMIT
	/* Drain any in-flight batch first - before the alloc-buf check and
	 * regardless of which layer we're called for. The SW unit has had a chance
	 * to render independent tasks (the overlap); now sleep on the completion
	 * signal and reclaim against the batch's own layer. */
	if (u->commit_pending) {
		(void)n2d_wait(N2D_INFINITE);
		finalize_batch(u, u->pending_layer);
		lv_draw_dispatch_request();
		return 1;
	}
#endif

	if (lv_draw_layer_alloc_buf(layer) == NULL) {
		return LV_DRAW_UNIT_IDLE;
	}

	t = lv_draw_get_available_task(layer, NULL, DRAW_UNIT_ID_NANO2D);
	if (t == NULL || t->preferred_draw_unit_id != DRAW_UNIT_ID_NANO2D) {
		/* No more work for us: flush any accumulated batch. */
		if (u->queued_cnt > 0) {
			commit_batch(u, layer);
			lv_draw_dispatch_request();
			return 1;
		}
		return LV_DRAW_UNIT_IDLE;
	}

	if (!u->dest_valid) {
		build_dest(layer, &u->dest);
		u->dest_valid = true;
	}

	/* An image's on-screen extent is its transformed area (_real_area), which
	 * differs from t->area once scaled/rotated; fills use t->area directly. */
	const lv_area_t *extent =
		(t->type == LV_DRAW_TASK_TYPE_IMAGE) ? &t->_real_area : &t->area;

	if (!lv_area_intersect(&clip, extent, &t->clip_area)) {
		t->state = LV_DRAW_TASK_STATE_FINISHED; /* nothing visible */
		lv_draw_dispatch_request();
		return 1;
	}

	t->state = LV_DRAW_TASK_STATE_QUEUED;
	t->draw_unit = draw_unit;

	if (t->type == LV_DRAW_TASK_TYPE_FILL) {
		ok = queue_fill(u, layer, t, &clip);
	} else {
		ok = queue_blit(u, layer, t, &clip);
	}

	if (ok) {
		u->queued[u->queued_cnt] = t;
		u->written[u->queued_cnt] = clip;
		u->queued_cnt++;
	} else {
		/* Translation declined the task after all; finish it so SW
		 * dependents can proceed (rare — evaluate already screened). */
		t->state = LV_DRAW_TASK_STATE_FINISHED;
	}

	if (u->queued_cnt >= NANO2D_BATCH_MAX) {
		commit_batch(u, layer);
	}

	lv_draw_dispatch_request();
	return 1;
}

static int32_t delete_cb(lv_draw_unit_t *draw_unit)
{
	LV_UNUSED(draw_unit);
	return 0;
}

void lv_draw_nano2d_init(void)
{
	nano2d_unit_t *u = lv_draw_create_unit(sizeof(nano2d_unit_t));

	u->base_unit.evaluate_cb = evaluate_cb;
	u->base_unit.dispatch_cb = dispatch_cb;
	u->base_unit.delete_cb = delete_cb;
	u->base_unit.name = "NANO2D";

	LOG_INF("nano2D draw unit registered (id %d)", DRAW_UNIT_ID_NANO2D);
}
