/*
 * Copyright (c) 2026 Microchip Technology Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPU-vs-CPU per-op microbenchmark (see microbench.h).
 *
 * For each op type and size it reports three per-op times (ns):
 *   SW       - software reference (plain C; the compiler may vectorise, but it
 *              is NOT the LVGL NEON renderer, so treat it as a *lower bound* on
 *              SW speed: if the GPU loses to this, it loses to LVGL SW too).
 *   GPU/op   - one op + its own n2d_commit (worst case: full per-commit cost).
 *   GPU marg - many ops in a single commit, divided out (best case: the commit
 *              cost is amortised; this is the marginal cost a size-gate cares
 *              about, since the real draw unit batches).
 * The crossover size (GPU marg < SW) is the size-gate sweet spot, if any.
 *
 * The GPU path includes the same cache maintenance the draw unit does (clean
 * source + dest before, invalidate dest after) so the numbers are comparable.
 */

#include "microbench.h"

#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "nano2D.h"

/* Destination is allocated tall enough for a 2x upscale of the largest source
 * (2 * 256 = 512 rows). Width matches a typical panel. */
#define DW       800
#define DH       512
#define SMAX     256
#define CL       64U

enum { OP_FILL, OP_COPY, OP_ALPHA, OP_SCALE, OP_CNT };
static const char *op_name[OP_CNT] = { "fill", "copy(RGB)", "alpha(ARGB)", "scale2x(RGB)" };

static uint16_t *dst;
static uint16_t *src565;
static uint32_t *srcargb;
static n2d_buffer_t dbuf, sbuf565, sbufargb;

static void mkbuf(n2d_buffer_t *b, void *p, int w, int h,
		  n2d_buffer_format_t fmt, int bpp)
{
	memset(b, 0, sizeof(*b));
	b->width = w;
	b->height = h;
	b->stride = w * bpp;
	b->format = fmt;
	b->tiling = N2D_LINEAR;
	b->memory = p;
	b->gpu = (n2d_uintptr_t)p;
}

/* Clean/invalidate a w x h dest region at (0,0), row by row (stride DW), like
 * the draw unit's cache_rows. */
static void dst_cache(int w, int h, bool inv)
{
	size_t bytes = (size_t)w * 2;

	for (int y = 0; y < h; y++) {
		uintptr_t row = (uintptr_t)(dst + (size_t)y * DW);
		uintptr_t s = row & ~(uintptr_t)(CL - 1U);
		uintptr_t e = (row + bytes + CL - 1U) & ~(uintptr_t)(CL - 1U);

		if (inv) {
			sys_cache_data_invd_range((void *)s, e - s);
		} else {
			sys_cache_data_flush_range((void *)s, e - s);
		}
	}
}

/* Clean the w x h source region the GPU will read (stride SMAX). */
static void src_clean(const void *base, int w, int h, int bpp)
{
	size_t bytes = (size_t)w * bpp;

	for (int y = 0; y < h; y++) {
		const uint8_t *row = (const uint8_t *)base + (size_t)y * SMAX * bpp;

		sys_cache_data_flush_range((void *)row, bytes);
	}
}

static void gpu_set_blend(bool alpha)
{
	n2d_state_config_t sc;

	memset(&sc, 0, sizeof(sc));
	sc.state = N2D_SET_PIXEL_MULTIPLY_MODE;
	sc.config.pixelMultiplyMode.srcPremult =
		alpha ? N2D_COLOR_MULTIPLY_ENABLE : N2D_COLOR_MULTIPLY_DISABLE;
	sc.config.pixelMultiplyMode.dstPremult = N2D_COLOR_MULTIPLY_DISABLE;
	sc.config.pixelMultiplyMode.srcGlobal = N2D_GLOBAL_COLOR_MULTIPLY_DISABLE;
	sc.config.pixelMultiplyMode.dstDemult = N2D_COLOR_MULTIPLY_DISABLE;
	(void)n2d_set(&sc);
}

/* Queue one GPU op (does not commit). Also cleans the source/dest it touches. */
static void gpu_queue(int op, int w, int h)
{
	switch (op) {
	case OP_FILL: {
		n2d_rectangle_t r = {0, 0, w, h};

		dst_cache(w, h, false);
		(void)n2d_fill(&dbuf, &r, (n2d_color_t)0xFFFF5050u, N2D_BLEND_NONE);
		break;
	}
	case OP_COPY: {
		n2d_rectangle_t dr = {0, 0, w, h};
		n2d_rectangle_t sr = {0, 0, w, h};

		gpu_set_blend(false);
		src_clean(src565, w, h, 2);
		dst_cache(w, h, false);
		(void)n2d_blit(&dbuf, &dr, &sbuf565, &sr, N2D_BLEND_NONE);
		break;
	}
	case OP_ALPHA: {
		n2d_rectangle_t dr = {0, 0, w, h};
		n2d_rectangle_t sr = {0, 0, w, h};

		gpu_set_blend(true);
		src_clean(srcargb, w, h, 4);
		dst_cache(w, h, false);
		(void)n2d_blit(&dbuf, &dr, &sbufargb, &sr, N2D_BLEND_SRC_OVER);
		break;
	}
	case OP_SCALE: {
		n2d_rectangle_t dr = {0, 0, 2 * w, 2 * h};
		n2d_rectangle_t sr = {0, 0, w, h};

		gpu_set_blend(false);
		src_clean(src565, w, h, 2);
		dst_cache(2 * w, 2 * h, false);
		(void)n2d_blit(&dbuf, &dr, &sbuf565, &sr, N2D_BLEND_NONE);
		break;
	}
	default:
		break;
	}
}

static void gpu_invd(int op, int w, int h)
{
	if (op == OP_SCALE) {
		dst_cache(2 * w, 2 * h, true);
	} else {
		dst_cache(w, h, true);
	}
}

/* ------------------------- software reference ------------------------- */

static void sw_op(int op, int w, int h)
{
	switch (op) {
	case OP_FILL:
		for (int y = 0; y < h; y++) {
			uint16_t *row = dst + (size_t)y * DW;

			for (int x = 0; x < w; x++) {
				row[x] = 0xFAAAu;
			}
		}
		break;
	case OP_COPY:
		for (int y = 0; y < h; y++) {
			memcpy(dst + (size_t)y * DW, src565 + (size_t)y * SMAX,
			       (size_t)w * 2);
		}
		break;
	case OP_ALPHA:
		for (int y = 0; y < h; y++) {
			uint16_t *d = dst + (size_t)y * DW;
			const uint32_t *s = srcargb + (size_t)y * SMAX;

			for (int x = 0; x < w; x++) {
				uint32_t px = s[x];
				uint32_t a = px >> 24;
				uint32_t sr = (px >> 16) & 0xFF;
				uint32_t sg = (px >> 8) & 0xFF;
				uint32_t sb = px & 0xFF;
				uint16_t dv = d[x];
				uint32_t dr = (dv >> 11) << 3;
				uint32_t dg = ((dv >> 5) & 0x3F) << 2;
				uint32_t db = (dv & 0x1F) << 3;
				uint32_t rr = (sr * a + dr * (255 - a)) / 255;
				uint32_t rg = (sg * a + dg * (255 - a)) / 255;
				uint32_t rb = (sb * a + db * (255 - a)) / 255;

				d[x] = (uint16_t)(((rr & 0xF8) << 8) |
						  ((rg & 0xFC) << 3) | (rb >> 3));
			}
		}
		break;
	case OP_SCALE:
		for (int y = 0; y < 2 * h; y++) {
			uint16_t *d = dst + (size_t)y * DW;
			const uint16_t *s = src565 + (size_t)(y >> 1) * SMAX;

			for (int x = 0; x < 2 * w; x++) {
				d[x] = s[x >> 1];
			}
		}
		break;
	default:
		break;
	}
}

/* ------------------------------ timing -------------------------------- */

static uint64_t ns_per(uint32_t cyc_total, uint32_t reps)
{
	return k_cyc_to_ns_floor64(cyc_total) / reps;
}

/* Many ops in a single commit -> marginal per-op cost (commit amortised). */
static uint64_t gpu_marginal_ns(int op, int w, int h, uint32_t k)
{
	uint32_t c0 = k_cycle_get_32();

	for (uint32_t i = 0; i < k; i++) {
		gpu_queue(op, w, h);
	}
	(void)n2d_commit();
	gpu_invd(op, w, h);

	uint32_t c1 = k_cycle_get_32();

	return ns_per(c1 - c0, k);
}

/* One op + its own commit, repeated -> full per-commit per-op cost. */
static uint64_t gpu_percommit_ns(int op, int w, int h, uint32_t m)
{
	uint32_t c0 = k_cycle_get_32();

	for (uint32_t i = 0; i < m; i++) {
		gpu_queue(op, w, h);
		(void)n2d_commit();
		gpu_invd(op, w, h);
	}

	uint32_t c1 = k_cycle_get_32();

	return ns_per(c1 - c0, m);
}

static uint64_t sw_ns(int op, int w, int h, uint32_t m)
{
	uint32_t c0 = k_cycle_get_32();

	for (uint32_t i = 0; i < m; i++) {
		sw_op(op, w, h);
	}

	uint32_t c1 = k_cycle_get_32();

	return ns_per(c1 - c0, m);
}

static void fill_sources(void)
{
	for (int y = 0; y < SMAX; y++) {
		for (int x = 0; x < SMAX; x++) {
			uint8_t r = (uint8_t)x, g = (uint8_t)y, b = 0x80;

			src565[y * SMAX + x] = (uint16_t)(((r & 0xF8) << 8) |
						((g & 0xFC) << 3) | (b >> 3));
			srcargb[y * SMAX + x] = ((uint32_t)((x ^ y) & 0xFF) << 24) |
						((uint32_t)r << 16) |
						((uint32_t)g << 8) | b;
		}
	}
	sys_cache_data_flush_range(src565, (size_t)SMAX * SMAX * 2);
	sys_cache_data_flush_range(srcargb, (size_t)SMAX * SMAX * 4);
}

void microbench_run(void)
{
	static const int sizes[] = {16, 32, 64, 128, 256};

	dst = k_aligned_alloc(CL, (size_t)DW * DH * 2);
	src565 = k_aligned_alloc(CL, (size_t)SMAX * SMAX * 2);
	srcargb = k_aligned_alloc(CL, (size_t)SMAX * SMAX * 4);
	if (!dst || !src565 || !srcargb) {
		printk("@MB alloc failed\n");
		return;
	}
	fill_sources();
	mkbuf(&dbuf, dst, DW, DH, N2D_RGB565, 2);
	mkbuf(&sbuf565, src565, SMAX, SMAX, N2D_RGB565, 2);
	mkbuf(&sbufargb, srcargb, SMAX, SMAX, N2D_ARGB8888, 4);

	/* Warm up the GPU (first commit pays one-time setup). */
	gpu_queue(OP_FILL, 32, 32);
	(void)n2d_commit();
	gpu_invd(OP_FILL, 32, 32);

	printk("@MB GPU vs CPU per-op (ns), GPU @532MHz. SW = plain C (lower bound).\n");
	printk("@MB op, size, SW_ns, GPU_op_ns, GPU_marg_ns, marg_winner\n");

	for (int o = 0; o < OP_CNT; o++) {
		for (size_t s = 0; s < ARRAY_SIZE(sizes); s++) {
			int w = sizes[s], h = sizes[s];
			uint64_t sw = sw_ns(o, w, h, 64);
			uint64_t gop = gpu_percommit_ns(o, w, h, 64);
			uint64_t gmarg = gpu_marginal_ns(o, w, h, 32);
			const char *win = (gmarg < sw) ? "GPU" : "CPU";

			printk("@MB %s, %d, %llu, %llu, %llu, %s\n",
			       op_name[o], w, (unsigned long long)sw,
			       (unsigned long long)gop,
			       (unsigned long long)gmarg, win);
		}
	}

	/* Full-screen fill (most GPU-favourable fill). */
	{
		uint64_t sw = sw_ns(OP_FILL, DW, 480, 16);
		uint64_t gop = gpu_percommit_ns(OP_FILL, DW, 480, 16);
		uint64_t gmarg = gpu_marginal_ns(OP_FILL, DW, 480, 8);

		printk("@MB fill, %dx480, %llu, %llu, %llu, %s\n", DW,
		       (unsigned long long)sw, (unsigned long long)gop,
		       (unsigned long long)gmarg, (gmarg < sw) ? "GPU" : "CPU");
	}

	printk("@MB done\n");
}
