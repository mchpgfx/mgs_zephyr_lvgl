/*
 * Copyright (c) 2026 Microchip Technology Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Microchip SAM LCD controller per-layer display driver. Each hardware
 * layer registers as an independent Zephyr display device. The parent
 * driver handles shared controller resources.
 */

#include <string.h>

#include <soc.h>
#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "microchip_sam_xlcdc.h"
#include "microchip_sam_xlcdc_layer.h"

LOG_MODULE_REGISTER(microchip_sam_xlcdc_layer, CONFIG_DISPLAY_LOG_LEVEL);

/* ========================================================================= */
/*                       Layer config / data structures                      */
/* ========================================================================= */

struct xlcdc_layer_config {
	const struct device   *parent;
	enum xlcdc_layer_type  type;
	uint8_t                pixel_format;
	uint8_t                default_r;
	uint8_t                default_g;
	uint8_t                default_b;
	uint8_t                global_alpha;
	bool                   heo_video_priority;
	uint16_t               init_width;   /* 0 = inherit parent */
	uint16_t               init_height;  /* 0 = inherit parent */
	uint16_t               x_origin;
	uint16_t               y_origin;
	bool                   no_alloc_fb;
};

struct xlcdc_layer_data {
	void    *fb[2];        /* fb[0] = primary, fb[1] = secondary (double-buf) */
	uint32_t fb_size;      /* size of a single framebuffer */
	uint8_t  front_idx;    /* which fb[] is currently displayed */
	uint16_t win_w;
	uint16_t win_h;
	uint8_t  pixel_format;
	uint8_t  alpha;
	bool     dma_enabled;
	const void *active_buf; /* buffer address currently in DMA registers */
	uint32_t vsync_handle;  /* VSYNC callback handle (0 = none) */
};

/* ========================================================================= */
/*                       Per-type register helpers                           */
/* ========================================================================= */

static void xlcdc_layer_setup_base(lcdc_registers_t *regs,
				   const struct xlcdc_layer_config *cfg,
				   uint16_t width, uint16_t height)
{
	LOG_INF("Setting up BASE layer (RGBMODE=%u)", cfg->pixel_format);

	regs->LCDC_BASECFG0 = LCDC_BASECFG0_BLEN(4);

	regs->LCDC_BASECFG1 = LCDC_BASECFG1_CLUTEN(0) |
			       LCDC_BASECFG1_GAM(0) |
			       LCDC_BASECFG1_RGBMODE(cfg->pixel_format) |
			       LCDC_BASECFG1_CLUTMODE(3);

	regs->LCDC_BASECFG2 = LCDC_BASECFG2_XSTRIDE(0);

	regs->LCDC_BASECFG3 = LCDC_BASECFG3_RDEF(cfg->default_r) |
			       LCDC_BASECFG3_GDEF(cfg->default_g) |
			       LCDC_BASECFG3_BDEF(cfg->default_b);

	regs->LCDC_BASECFG4 = LCDC_BASECFG4_DMA(0) |
			       LCDC_BASECFG4_REP(1) |
			       LCDC_BASECFG4_DISCEN(0);

	regs->LCDC_BASECFG5 = 0;
	regs->LCDC_BASECFG6 = 0;
	regs->LCDC_BASECLA = 0;
	regs->LCDC_BASEFBA = 0;
	regs->LCDC_BASEEN = LCDC_BASEEN_ENABLE_Msk;
}

static void xlcdc_layer_setup_ovr1(lcdc_registers_t *regs,
				   const struct xlcdc_layer_config *cfg,
				   uint16_t width, uint16_t height)
{
	LOG_INF("Setting up OVR1 layer (RGBMODE=%u, alpha=%u)",
		cfg->pixel_format, cfg->global_alpha);

	regs->LCDC_OVR1CFG0 = LCDC_OVR1CFG0_BLEN(4);

	regs->LCDC_OVR1CFG1 = LCDC_OVR1CFG1_CLUTEN(0) |
			       LCDC_OVR1CFG1_GAM(0) |
			       LCDC_OVR1CFG1_RGBMODE(cfg->pixel_format) |
			       LCDC_OVR1CFG1_CLUTMODE(3);

	regs->LCDC_OVR1CFG2 = LCDC_OVR1CFG2_XPOS(0) |
			       LCDC_OVR1CFG2_YPOS(0);

	regs->LCDC_OVR1CFG3 = LCDC_OVR1CFG3_XSIZE(width - 1) |
			       LCDC_OVR1CFG3_YSIZE(height - 1);

	regs->LCDC_OVR1CFG4 = LCDC_OVR1CFG4_XSTRIDE(0);
	regs->LCDC_OVR1CFG5 = LCDC_OVR1CFG5_PSTRIDE(0);

	regs->LCDC_OVR1CFG6 = LCDC_OVR1CFG6_RDEF(cfg->default_r) |
			       LCDC_OVR1CFG6_GDEF(cfg->default_g) |
			       LCDC_OVR1CFG6_BDEF(cfg->default_b);

	regs->LCDC_OVR1CFG7 = 0;
	regs->LCDC_OVR1CFG8 = 0;

	uint8_t sfactc = (cfg->pixel_format == XLCDC_ARGB_8888) ? 5 : 4;

	regs->LCDC_OVR1CFG9 = LCDC_OVR1CFG9_DMA(0) |
			       LCDC_OVR1CFG9_REP(1) |
			       LCDC_OVR1CFG9_CRKEY(0) |
			       LCDC_OVR1CFG9_DSTKEY(0) |
			       LCDC_OVR1CFG9_SFACTC(sfactc) |
			       LCDC_OVR1CFG9_SFACTA(1) |
			       LCDC_OVR1CFG9_DFACTC(6) |
			       LCDC_OVR1CFG9_DFACTA(2) |
			       LCDC_OVR1CFG9_A0(cfg->global_alpha) |
			       LCDC_OVR1CFG9_A1(0);

	regs->LCDC_OVR1CLA = 0;
	regs->LCDC_OVR1FBA = 0;
	regs->LCDC_OVR1EN = LCDC_OVR1EN_ENABLE_Msk;
}

static void xlcdc_layer_setup_heo(lcdc_registers_t *regs,
				  const struct xlcdc_layer_config *cfg,
				  uint16_t width, uint16_t height)
{
	LOG_INF("Setting up HEO layer (RGBMODE=%u, alpha=%u, vidpri=%u)",
		cfg->pixel_format, cfg->global_alpha,
		cfg->heo_video_priority ? 1 : 0);

	regs->LCDC_HEOCFG0 = LCDC_HEOCFG0_BLEN(4) |
			      LCDC_HEOCFG0_BLENCC(4);

	regs->LCDC_HEOCFG1 = LCDC_HEOCFG1_CLUTEN(0) |
			      LCDC_HEOCFG1_YCCEN(0) |
			      LCDC_HEOCFG1_GAM(0) |
			      LCDC_HEOCFG1_RGBMODE(cfg->pixel_format) |
			      LCDC_HEOCFG1_CLUTMODE(3) |
			      LCDC_HEOCFG1_YCCMODE(0) |
			      LCDC_HEOCFG1_YCC422ROT(0) |
			      LCDC_HEOCFG1_ILD(0);

	regs->LCDC_HEOCFG2 = LCDC_HEOCFG2_XPOS(0) |
			      LCDC_HEOCFG2_YPOS(0);

	regs->LCDC_HEOCFG3 = LCDC_HEOCFG3_XSIZE(width - 1) |
			      LCDC_HEOCFG3_YSIZE(height - 1);

	regs->LCDC_HEOCFG4 = LCDC_HEOCFG4_XMEMSIZE(width - 1) |
			      LCDC_HEOCFG4_YMEMSIZE(height - 1);

	regs->LCDC_HEOCFG5 = 0;
	regs->LCDC_HEOCFG6 = 0;
	regs->LCDC_HEOCFG7 = 0;
	regs->LCDC_HEOCFG8 = 0;

	regs->LCDC_HEOCFG9 = LCDC_HEOCFG9_RDEF(cfg->default_r) |
			      LCDC_HEOCFG9_GDEF(cfg->default_g) |
			      LCDC_HEOCFG9_BDEF(cfg->default_b);

	regs->LCDC_HEOCFG10 = 0;
	regs->LCDC_HEOCFG11 = 0;

	/* HEO blending: SFACTC=4 (A0*As) for per-pixel alpha */
	regs->LCDC_HEOCFG12 = LCDC_HEOCFG12_DMA(0) |
			       LCDC_HEOCFG12_REP(1) |
			       LCDC_HEOCFG12_CRKEY(0) |
			       LCDC_HEOCFG12_DSTKEY(0) |
			       LCDC_HEOCFG12_VIDPRI(cfg->heo_video_priority ? 1 : 0) |
			       LCDC_HEOCFG12_SFACTC(4) |
			       LCDC_HEOCFG12_SFACTA(1) |
			       LCDC_HEOCFG12_DFACTC(6) |
			       LCDC_HEOCFG12_DFACTA(2) |
			       LCDC_HEOCFG12_A0(cfg->global_alpha) |
			       LCDC_HEOCFG12_A1(0);

	regs->LCDC_HEOCFG13 = 0;
	regs->LCDC_HEOCFG14 = 0;
	regs->LCDC_HEOCFG15 = 0;
	regs->LCDC_HEOCFG16 = 0;
	regs->LCDC_HEOCFG17 = 0;
	regs->LCDC_HEOCFG18 = 0;
	regs->LCDC_HEOCFG19 = 0;
	regs->LCDC_HEOCFG20 = 0;
	regs->LCDC_HEOCFG21 = 0;
	regs->LCDC_HEOCFG22 = 0;

	/* Scaler disabled (1:1) */
	regs->LCDC_HEOCFG23 = 0;
	regs->LCDC_HEOCFG24 = XLCDC_SCALE_UNITY;
	regs->LCDC_HEOCFG25 = XLCDC_SCALE_UNITY;
	regs->LCDC_HEOCFG26 = XLCDC_SCALE_UNITY;
	regs->LCDC_HEOCFG27 = XLCDC_SCALE_UNITY;
	regs->LCDC_HEOCFG28 = 0;
	regs->LCDC_HEOCFG29 = 0;
	regs->LCDC_HEOCFG30 = LCDC_HEOCFG30_VXSYCFG(1) |
			       LCDC_HEOCFG30_VXSCCFG(1);
	regs->LCDC_HEOCFG31 = LCDC_HEOCFG31_HXSYCFG(1) |
			       LCDC_HEOCFG31_HXSCCFG(1);

	/* Zero all filter tap coefficients */
	for (int i = 0; i < LCDC_HEOVTAP_NUMBER; i++) {
		regs->LCDC_HEOVTAP[i].LCDC_HEOVTAP10P = 0;
		regs->LCDC_HEOVTAP[i].LCDC_HEOVTAP32P = 0;
	}
	for (int i = 0; i < LCDC_HEOHTAP_NUMBER; i++) {
		regs->LCDC_HEOHTAP[i].LCDC_HEOHTAP10P = 0;
		regs->LCDC_HEOHTAP[i].LCDC_HEOHTAP32P = 0;
	}

	regs->LCDC_HEOCLA = 0;
	regs->LCDC_HEO[0].LCDC_HEOYFBA = 0;
	regs->LCDC_HEO[0].LCDC_HEOCBFBA = 0;
	regs->LCDC_HEO[0].LCDC_HEOCRFBA = 0;

	regs->LCDC_HEOEN = LCDC_HEOEN_ENABLE_Msk;
}

#if defined(LCDC_OVR2CFG0_BLEN_Pos)
static void xlcdc_layer_setup_ovr2(lcdc_registers_t *regs,
				   const struct xlcdc_layer_config *cfg,
				   uint16_t width, uint16_t height)
{
	LOG_INF("Setting up OVR2 layer (RGBMODE=%u, alpha=%u)",
		cfg->pixel_format, cfg->global_alpha);

	regs->LCDC_OVR2CFG0 = LCDC_OVR2CFG0_BLEN(4);

	regs->LCDC_OVR2CFG1 = LCDC_OVR2CFG1_CLUTEN(0) |
			       LCDC_OVR2CFG1_GAM(0) |
			       LCDC_OVR2CFG1_RGBMODE(cfg->pixel_format) |
			       LCDC_OVR2CFG1_CLUTMODE(3);

	regs->LCDC_OVR2CFG2 = LCDC_OVR2CFG2_XPOS(0) |
			       LCDC_OVR2CFG2_YPOS(0);

	regs->LCDC_OVR2CFG3 = LCDC_OVR2CFG3_XSIZE(width - 1) |
			       LCDC_OVR2CFG3_YSIZE(height - 1);

	regs->LCDC_OVR2CFG4 = LCDC_OVR2CFG4_XSTRIDE(0);
	regs->LCDC_OVR2CFG5 = LCDC_OVR2CFG5_PSTRIDE(0);

	regs->LCDC_OVR2CFG6 = LCDC_OVR2CFG6_RDEF(cfg->default_r) |
			       LCDC_OVR2CFG6_GDEF(cfg->default_g) |
			       LCDC_OVR2CFG6_BDEF(cfg->default_b) |
			       LCDC_OVR2CFG6_ADEF(0);

	regs->LCDC_OVR2CFG7 = 0;
	regs->LCDC_OVR2CFG8 = 0;

	uint8_t sfactc = (cfg->pixel_format == XLCDC_ARGB_8888) ? 5 : 4;

	regs->LCDC_OVR2CFG9 = LCDC_OVR2CFG9_DMA(0) |
			       LCDC_OVR2CFG9_REP(1) |
			       LCDC_OVR2CFG9_CRKEY(0) |
			       LCDC_OVR2CFG9_DSTKEY(0) |
			       LCDC_OVR2CFG9_SFACTC(sfactc) |
			       LCDC_OVR2CFG9_SFACTA(1) |
			       LCDC_OVR2CFG9_DFACTC(6) |
			       LCDC_OVR2CFG9_DFACTA(2) |
			       LCDC_OVR2CFG9_A0(cfg->global_alpha) |
			       LCDC_OVR2CFG9_A1(0);

	regs->LCDC_OVR2CLA = 0;
	regs->LCDC_OVR2FBA = 0;
	regs->LCDC_OVR2EN = LCDC_OVR2EN_ENABLE_Msk;
}
#endif /* LCDC_OVR2CFG0_BLEN_Pos */

/* ========================================================================= */
/*                       Layer DMA and address helpers                       */
/* ========================================================================= */

static void xlcdc_layer_set_fb_addr(lcdc_registers_t *regs,
				    enum xlcdc_layer_type type,
				    uint32_t addr)
{
	switch (type) {
	case XLCDC_LAYER_BASE:
		regs->LCDC_BASEFBA = addr;
		break;
	case XLCDC_LAYER_OVR1:
		regs->LCDC_OVR1FBA = addr;
		break;
	case XLCDC_LAYER_HEO:
		regs->LCDC_HEO[0].LCDC_HEOYFBA = addr;
		break;
	case XLCDC_LAYER_OVR2:
#if defined(LCDC_OVR2CFG0_BLEN_Pos)
		regs->LCDC_OVR2FBA = addr;
#endif
		break;
	default:
		break;
	}
}

static void xlcdc_layer_set_dma(lcdc_registers_t *regs,
				enum xlcdc_layer_type type,
				const struct xlcdc_layer_config *cfg,
				bool enable)
{
	switch (type) {
	case XLCDC_LAYER_BASE:
		regs->LCDC_BASECFG4 = LCDC_BASECFG4_DMA(enable ? 1 : 0) |
				       LCDC_BASECFG4_REP(1) |
				       LCDC_BASECFG4_DISCEN(0);
		break;
	case XLCDC_LAYER_OVR1: {
		uint8_t sfactc = (cfg->pixel_format == XLCDC_ARGB_8888) ? 5 : 4;

		regs->LCDC_OVR1CFG9 = LCDC_OVR1CFG9_DMA(enable ? 1 : 0) |
				       LCDC_OVR1CFG9_REP(1) |
				       LCDC_OVR1CFG9_CRKEY(0) |
				       LCDC_OVR1CFG9_DSTKEY(0) |
				       LCDC_OVR1CFG9_SFACTC(sfactc) |
				       LCDC_OVR1CFG9_SFACTA(1) |
				       LCDC_OVR1CFG9_DFACTC(6) |
				       LCDC_OVR1CFG9_DFACTA(2) |
				       LCDC_OVR1CFG9_A0(cfg->global_alpha) |
				       LCDC_OVR1CFG9_A1(0);
		break;
	}
	case XLCDC_LAYER_HEO: {
		regs->LCDC_HEOCFG12 = LCDC_HEOCFG12_DMA(enable ? 1 : 0) |
				       LCDC_HEOCFG12_REP(1) |
				       LCDC_HEOCFG12_CRKEY(0) |
				       LCDC_HEOCFG12_DSTKEY(0) |
				       LCDC_HEOCFG12_VIDPRI(cfg->heo_video_priority ? 1 : 0) |
				       LCDC_HEOCFG12_SFACTC(4) |
				       LCDC_HEOCFG12_SFACTA(1) |
				       LCDC_HEOCFG12_DFACTC(6) |
				       LCDC_HEOCFG12_DFACTA(2) |
				       LCDC_HEOCFG12_A0(cfg->global_alpha) |
				       LCDC_HEOCFG12_A1(0);
		break;
	}
	case XLCDC_LAYER_OVR2:
#if defined(LCDC_OVR2CFG0_BLEN_Pos)
	{
		uint8_t sfactc = (cfg->pixel_format == XLCDC_ARGB_8888) ? 5 : 4;

		regs->LCDC_OVR2CFG9 = LCDC_OVR2CFG9_DMA(enable ? 1 : 0) |
				       LCDC_OVR2CFG9_REP(1) |
				       LCDC_OVR2CFG9_CRKEY(0) |
				       LCDC_OVR2CFG9_DSTKEY(0) |
				       LCDC_OVR2CFG9_SFACTC(sfactc) |
				       LCDC_OVR2CFG9_SFACTA(1) |
				       LCDC_OVR2CFG9_DFACTC(6) |
				       LCDC_OVR2CFG9_DFACTA(2) |
				       LCDC_OVR2CFG9_A0(cfg->global_alpha) |
				       LCDC_OVR2CFG9_A1(0);
	}
#endif
		break;
	default:
		break;
	}
}

/* ========================================================================= */
/*                       Display API implementation                          */
/* ========================================================================= */

static int xlcdc_layer_blanking_on(const struct device *dev)
{
	const struct xlcdc_layer_config *cfg = dev->config;
	struct xlcdc_layer_data *data = dev->data;
	const struct xlcdc_parent_config *pcfg = cfg->parent->config;

	LOG_DBG("%s: Blanking ON (DMA off)", dev->name);

	xlcdc_layer_set_dma(pcfg->regs, cfg->type, cfg, false);
	xlcdc_parent_commit_attrs(cfg->parent, xlcdc_layer_attre_mask(cfg->type));
	data->dma_enabled = false;

	if (cfg->type == XLCDC_LAYER_BASE) {
		xlcdc_parent_backlight_disable(cfg->parent);
	}

	return 0;
}

static int xlcdc_layer_blanking_off(const struct device *dev)
{
	const struct xlcdc_layer_config *cfg = dev->config;
	struct xlcdc_layer_data *data = dev->data;
	const struct xlcdc_parent_config *pcfg = cfg->parent->config;

	LOG_DBG("%s: Blanking OFF (DMA on)", dev->name);

	if (data->fb[data->front_idx] != NULL) {
		xlcdc_layer_set_fb_addr(pcfg->regs, cfg->type,
					(uint32_t)(uintptr_t)data->fb[data->front_idx]);
		data->active_buf = data->fb[data->front_idx];
	}
	xlcdc_layer_set_dma(pcfg->regs, cfg->type, cfg, true);
	xlcdc_parent_commit_attrs(cfg->parent, xlcdc_layer_attre_mask(cfg->type));
	data->dma_enabled = true;

	if (cfg->type == XLCDC_LAYER_BASE) {
		xlcdc_parent_backlight_enable(cfg->parent);
	}

	return 0;
}

static void *xlcdc_layer_get_framebuffer(const struct device *dev)
{
	struct xlcdc_layer_data *data = dev->data;

	/* In zero-copy mode, return the active (externally-owned) buffer */
	if (data->fb[0] == NULL && data->active_buf != NULL) {
		return (void *)data->active_buf;
	}

#ifdef CONFIG_MICROCHIP_SAM_XLCDC_DOUBLE_BUFFER
	/* Return back buffer (the one not currently displayed) */
	return data->fb[1 - data->front_idx];
#else
	return data->fb[0];
#endif
}

static int xlcdc_layer_set_brightness_ctrl(const struct device *dev,
					   const uint8_t brightness)
{
	const struct xlcdc_layer_config *cfg = dev->config;

	return xlcdc_parent_set_brightness(cfg->parent, brightness);
}

static int xlcdc_layer_write(const struct device *dev,
			     const uint16_t x, const uint16_t y,
			     const struct display_buffer_descriptor *desc,
			     const void *buf)
{
	const struct xlcdc_layer_config *cfg = dev->config;
	struct xlcdc_layer_data *data = dev->data;
	const struct xlcdc_parent_config *pcfg = cfg->parent->config;
	uint8_t bpp = xlcdc_bpp(data->pixel_format);

	if (x + desc->width > data->win_w || y + desc->height > data->win_h) {
		LOG_ERR("Write out of bounds: (%u,%u)+(%u,%u) > %ux%u",
			x, y, desc->width, desc->height,
			data->win_w, data->win_h);
		return -EINVAL;
	}

	/* Full-screen write: zero-copy pointer swap */
	if (x == 0 && y == 0 &&
	    desc->width == data->win_w &&
	    desc->height == data->win_h &&
	    desc->pitch == desc->width) {
		if (buf != data->active_buf) {
			sys_cache_data_flush_range((void *)buf, data->fb_size);
			xlcdc_layer_set_fb_addr(pcfg->regs, cfg->type,
						(uint32_t)(uintptr_t)buf);
			xlcdc_parent_commit_attrs(cfg->parent,
						  xlcdc_layer_attre_mask(cfg->type));
			data->active_buf = buf;
		}
		return 0;
	}

	/* Partial write requires a driver-owned framebuffer */
	if (cfg->no_alloc_fb) {
		LOG_ERR("Partial writes not supported without driver framebuffer");
		return -ENOTSUP;
	}

	/* Existing memcpy path for partial updates */
	uint8_t *fb = (uint8_t *)data->fb[data->front_idx];
	uint32_t row_stride = data->win_w * bpp;
	const uint8_t *src = buf;
	uint32_t src_stride = desc->pitch * bpp;

	for (uint16_t row = 0; row < desc->height; row++) {
		uint32_t dst_off = ((y + row) * row_stride) + (x * bpp);

		memcpy(fb + dst_off, src + (row * src_stride), desc->width * bpp);
	}

	sys_cache_data_flush_range(data->fb[data->front_idx], data->fb_size);

	/* Update DMA address if partial write changed the active buffer */
	if (data->fb[data->front_idx] != data->active_buf &&
	    !desc->frame_incomplete) {
		xlcdc_layer_set_fb_addr(pcfg->regs, cfg->type,
					(uint32_t)(uintptr_t)data->fb[data->front_idx]);
		xlcdc_parent_commit_attrs(cfg->parent,
					  xlcdc_layer_attre_mask(cfg->type));
		data->active_buf = data->fb[data->front_idx];
	}

	return 0;
}

static void xlcdc_layer_get_capabilities(const struct device *dev,
					 struct display_capabilities *caps)
{
	struct xlcdc_layer_data *data = dev->data;

	memset(caps, 0, sizeof(*caps));
	caps->x_resolution = data->win_w;
	caps->y_resolution = data->win_h;

	switch (data->pixel_format) {
	case XLCDC_RGB_565:
		caps->current_pixel_format = PIXEL_FORMAT_RGB_565;
		caps->supported_pixel_formats = PIXEL_FORMAT_RGB_565;
		break;
	case XLCDC_RGB_888:
	case XLCDC_RGB_888_PACKED:
		caps->current_pixel_format = PIXEL_FORMAT_RGB_888;
		caps->supported_pixel_formats = PIXEL_FORMAT_RGB_888;
		break;
	case XLCDC_ARGB_8888:
		caps->current_pixel_format = PIXEL_FORMAT_ARGB_8888;
		caps->supported_pixel_formats = PIXEL_FORMAT_ARGB_8888;
		break;
	case XLCDC_RGBA_8888:
		caps->current_pixel_format = PIXEL_FORMAT_RGBA_8888;
		caps->supported_pixel_formats = PIXEL_FORMAT_RGBA_8888;
		break;
	default:
		caps->current_pixel_format = PIXEL_FORMAT_ARGB_8888;
		caps->supported_pixel_formats = PIXEL_FORMAT_ARGB_8888;
		break;
	}

	caps->current_orientation = DISPLAY_ORIENTATION_NORMAL;
}

/* ========================================================================= */
/*                       Event callback API                                  */
/* ========================================================================= */

static int xlcdc_layer_register_event_cb(const struct device *dev,
					 display_event_cb_t cb,
					 void *user_data,
					 uint32_t event_mask,
					 bool in_isr,
					 uint32_t *out_reg_handle)
{
	const struct xlcdc_layer_config *cfg = dev->config;
	struct xlcdc_layer_data *data = dev->data;

	if (!(event_mask & DISPLAY_EVENT_VSYNC)) {
		return -ENOTSUP;
	}

	if (!in_isr) {
		/* The VSYNC callback is invoked directly from the SOF interrupt
		 * handler; this driver has no workqueue/thread path to defer it
		 * to. Callers must therefore accept ISR-context invocation
		 * (in_isr == true). Reject any request for thread-context delivery.
		 */
		return -ENOTSUP;
	}

	int ret = xlcdc_parent_register_vsync(cfg->parent, dev, cb,
					      user_data, out_reg_handle);
	if (ret == 0) {
		data->vsync_handle = *out_reg_handle;
	}

	return ret;
}

static int xlcdc_layer_unregister_event_cb(const struct device *dev,
					   uint32_t reg_handle)
{
	const struct xlcdc_layer_config *cfg = dev->config;
	struct xlcdc_layer_data *data = dev->data;

	int ret = xlcdc_parent_unregister_vsync(cfg->parent, reg_handle);

	if (ret == 0 && data->vsync_handle == reg_handle) {
		data->vsync_handle = 0;
	}

	return ret;
}

static const struct display_driver_api xlcdc_layer_display_api = {
	.blanking_on        = xlcdc_layer_blanking_on,
	.blanking_off       = xlcdc_layer_blanking_off,
	.write              = xlcdc_layer_write,
	.get_framebuffer    = xlcdc_layer_get_framebuffer,
	.set_brightness     = xlcdc_layer_set_brightness_ctrl,
	.get_capabilities   = xlcdc_layer_get_capabilities,
	.register_event_cb  = xlcdc_layer_register_event_cb,
	.unregister_event_cb = xlcdc_layer_unregister_event_cb,
};

/* ========================================================================= */
/*                       Vendor Extension API                                */
/* ========================================================================= */

int xlcdc_layer_set_position(const struct device *dev, uint16_t x, uint16_t y)
{
	const struct xlcdc_layer_config *cfg = dev->config;
	const struct xlcdc_parent_config *pcfg = cfg->parent->config;
	lcdc_registers_t *regs = pcfg->regs;

	if (cfg->type == XLCDC_LAYER_BASE) {
		return -ENOTSUP;
	}

	switch (cfg->type) {
	case XLCDC_LAYER_OVR1:
		regs->LCDC_OVR1CFG2 = LCDC_OVR1CFG2_XPOS(x) |
				       LCDC_OVR1CFG2_YPOS(y);
		break;
	case XLCDC_LAYER_HEO:
		regs->LCDC_HEOCFG2 = LCDC_HEOCFG2_XPOS(x) |
				      LCDC_HEOCFG2_YPOS(y);
		break;
	case XLCDC_LAYER_OVR2:
#if defined(LCDC_OVR2CFG0_BLEN_Pos)
		regs->LCDC_OVR2CFG2 = LCDC_OVR2CFG2_XPOS(x) |
				       LCDC_OVR2CFG2_YPOS(y);
#endif
		break;
	default:
		return -ENOTSUP;
	}

	xlcdc_parent_commit_attrs(cfg->parent, xlcdc_layer_attre_mask(cfg->type));
	return 0;
}

int xlcdc_layer_set_window_size(const struct device *dev, uint16_t w, uint16_t h)
{
	const struct xlcdc_layer_config *cfg = dev->config;
	struct xlcdc_layer_data *data = dev->data;
	const struct xlcdc_parent_config *pcfg = cfg->parent->config;
	lcdc_registers_t *regs = pcfg->regs;

	if (cfg->type == XLCDC_LAYER_BASE) {
		return -ENOTSUP;
	}

	switch (cfg->type) {
	case XLCDC_LAYER_OVR1:
		regs->LCDC_OVR1CFG3 = LCDC_OVR1CFG3_XSIZE(w - 1) |
				       LCDC_OVR1CFG3_YSIZE(h - 1);
		break;
	case XLCDC_LAYER_HEO:
		regs->LCDC_HEOCFG3 = LCDC_HEOCFG3_XSIZE(w - 1) |
				      LCDC_HEOCFG3_YSIZE(h - 1);
		regs->LCDC_HEOCFG4 = LCDC_HEOCFG4_XMEMSIZE(w - 1) |
				      LCDC_HEOCFG4_YMEMSIZE(h - 1);
		break;
	case XLCDC_LAYER_OVR2:
#if defined(LCDC_OVR2CFG0_BLEN_Pos)
		regs->LCDC_OVR2CFG3 = LCDC_OVR2CFG3_XSIZE(w - 1) |
				       LCDC_OVR2CFG3_YSIZE(h - 1);
#endif
		break;
	default:
		return -ENOTSUP;
	}

	data->win_w = w;
	data->win_h = h;

	xlcdc_parent_commit_attrs(cfg->parent, xlcdc_layer_attre_mask(cfg->type));
	return 0;
}

int xlcdc_layer_set_alpha(const struct device *dev, uint8_t alpha)
{
	const struct xlcdc_layer_config *cfg = dev->config;
	struct xlcdc_layer_data *data = dev->data;
	const struct xlcdc_parent_config *pcfg = cfg->parent->config;
	lcdc_registers_t *regs = pcfg->regs;

	if (cfg->type == XLCDC_LAYER_BASE) {
		return -ENOTSUP;
	}

	data->alpha = alpha;

	/* Rewrite the blending config register with the new alpha */
	uint8_t sfactc = (cfg->pixel_format == XLCDC_ARGB_8888) ? 5 : 4;

	switch (cfg->type) {
	case XLCDC_LAYER_OVR1:
		regs->LCDC_OVR1CFG9 = LCDC_OVR1CFG9_DMA(data->dma_enabled ? 1 : 0) |
				       LCDC_OVR1CFG9_REP(1) |
				       LCDC_OVR1CFG9_CRKEY(0) |
				       LCDC_OVR1CFG9_DSTKEY(0) |
				       LCDC_OVR1CFG9_SFACTC(sfactc) |
				       LCDC_OVR1CFG9_SFACTA(1) |
				       LCDC_OVR1CFG9_DFACTC(6) |
				       LCDC_OVR1CFG9_DFACTA(2) |
				       LCDC_OVR1CFG9_A0(alpha) |
				       LCDC_OVR1CFG9_A1(0);
		break;
	case XLCDC_LAYER_HEO:
		regs->LCDC_HEOCFG12 = LCDC_HEOCFG12_DMA(data->dma_enabled ? 1 : 0) |
				       LCDC_HEOCFG12_REP(1) |
				       LCDC_HEOCFG12_CRKEY(0) |
				       LCDC_HEOCFG12_DSTKEY(0) |
				       LCDC_HEOCFG12_VIDPRI(cfg->heo_video_priority ? 1 : 0) |
				       LCDC_HEOCFG12_SFACTC(4) |
				       LCDC_HEOCFG12_SFACTA(1) |
				       LCDC_HEOCFG12_DFACTC(6) |
				       LCDC_HEOCFG12_DFACTA(2) |
				       LCDC_HEOCFG12_A0(alpha) |
				       LCDC_HEOCFG12_A1(0);
		break;
	case XLCDC_LAYER_OVR2:
#if defined(LCDC_OVR2CFG0_BLEN_Pos)
		regs->LCDC_OVR2CFG9 = LCDC_OVR2CFG9_DMA(data->dma_enabled ? 1 : 0) |
				       LCDC_OVR2CFG9_REP(1) |
				       LCDC_OVR2CFG9_CRKEY(0) |
				       LCDC_OVR2CFG9_DSTKEY(0) |
				       LCDC_OVR2CFG9_SFACTC(sfactc) |
				       LCDC_OVR2CFG9_SFACTA(1) |
				       LCDC_OVR2CFG9_DFACTC(6) |
				       LCDC_OVR2CFG9_DFACTA(2) |
				       LCDC_OVR2CFG9_A0(alpha) |
				       LCDC_OVR2CFG9_A1(0);
#endif
		break;
	default:
		return -ENOTSUP;
	}

	xlcdc_parent_commit_attrs(cfg->parent, xlcdc_layer_attre_mask(cfg->type));
	return 0;
}

int xlcdc_layer_enable(const struct device *dev, bool enable)
{
	const struct xlcdc_layer_config *cfg = dev->config;
	struct xlcdc_layer_data *data = dev->data;
	const struct xlcdc_parent_config *pcfg = cfg->parent->config;

	if (enable && data->fb[data->front_idx] != NULL) {
		xlcdc_layer_set_fb_addr(pcfg->regs, cfg->type,
					(uint32_t)(uintptr_t)data->fb[data->front_idx]);
		data->active_buf = data->fb[data->front_idx];
	}
	xlcdc_layer_set_dma(pcfg->regs, cfg->type, cfg, enable);
	xlcdc_parent_commit_attrs(cfg->parent, xlcdc_layer_attre_mask(cfg->type));
	data->dma_enabled = enable;

	return 0;
}

int xlcdc_layer_swap_buffers(const struct device *dev)
{
#ifdef CONFIG_MICROCHIP_SAM_XLCDC_DOUBLE_BUFFER
	const struct xlcdc_layer_config *cfg = dev->config;
	struct xlcdc_layer_data *data = dev->data;
	const struct xlcdc_parent_config *pcfg = cfg->parent->config;

	/* Toggle front/back */
	data->front_idx = 1 - data->front_idx;

	/* Flush new front buffer before giving address to HW */
	sys_cache_data_flush_range(data->fb[data->front_idx], data->fb_size);

	/* Update hardware FB address to new front buffer */
	xlcdc_layer_set_fb_addr(pcfg->regs, cfg->type,
				(uint32_t)(uintptr_t)data->fb[data->front_idx]);

	/* Commit the address change */
	xlcdc_parent_commit_attrs(cfg->parent,
				  xlcdc_layer_attre_mask(cfg->type));
	data->active_buf = data->fb[data->front_idx];

	return 0;
#else
	return -ENOTSUP;
#endif
}

/* ========================================================================= */
/*                       HEO Scaler + Surface API                           */
/* ========================================================================= */

/*
 * HEO scaling factor computation.
 *
 * The scaler factor is a fixed-point 20-bit value (1 integer + 19 fractional).
 * Factor = (source_size / target_size) encoded as:
 *   factor_reg = (src << 20) / dst    (if src != dst)
 *   factor_reg = 1 << 20 = 0x100000   (if src == dst, i.e. 1:1)
 *
 * When src < dst (upscale), factor < 1.0, register < 0x100000.
 * When src > dst (downscale), factor > 1.0, register > 0x100000.
 */
static uint32_t xlcdc_heo_scale_factor(uint16_t src, uint16_t dst)
{
	if (src == dst || dst == 0) {
		return XLCDC_SCALE_UNITY;
	}
	return ((uint32_t)src << XLCDC_SCALE_FRAC_BITS) / dst;
}

static void xlcdc_heo_configure_scaler(lcdc_registers_t *regs,
				       uint16_t img_w, uint16_t img_h,
				       uint16_t win_w, uint16_t win_h)
{
	bool scale_h = (img_w != win_w);
	bool scale_v = (img_h != win_h);

	uint32_t hfact = xlcdc_heo_scale_factor(img_w, win_w);
	uint32_t vfact = xlcdc_heo_scale_factor(img_h, win_h);

	LOG_INF("HEO scaler: %ux%u -> %ux%u (hfact=0x%x, vfact=0x%x)",
		img_w, img_h, win_w, win_h, hfact, vfact);

	/* HEOCFG23: Enable scaler components as needed */
	regs->LCDC_HEOCFG23 = (scale_v ? LCDC_HEOCFG23_VXSYEN_Msk : 0) |
			       (scale_v ? LCDC_HEOCFG23_VXSCEN_Msk : 0) |
			       (scale_h ? LCDC_HEOCFG23_HXSYEN_Msk : 0) |
			       (scale_h ? LCDC_HEOCFG23_HXSCEN_Msk : 0);

	/* HEOCFG24-27: Scaling factors (luma = chroma for RGB) */
	regs->LCDC_HEOCFG24 = LCDC_HEOCFG24_VXSYFACT(vfact);
	regs->LCDC_HEOCFG25 = LCDC_HEOCFG25_VXSCFACT(vfact);
	regs->LCDC_HEOCFG26 = LCDC_HEOCFG26_HXSYFACT(hfact);
	regs->LCDC_HEOCFG27 = LCDC_HEOCFG27_HXSCFACT(hfact);

	/* HEOCFG28-29: Phase offsets (0 for bilinear) */
	regs->LCDC_HEOCFG28 = 0;
	regs->LCDC_HEOCFG29 = 0;

	/*
	 * HEOCFG30-31: Scaler configuration.
	 * Use bilinear 2-tap filtering for both luma and chroma.
	 * CFG=1 (bilinear), TAP2=1 (2-tap mode).
	 */
	regs->LCDC_HEOCFG30 = LCDC_HEOCFG30_VXSYCFG(1) |
			       LCDC_HEOCFG30_VXSYTAP2_Msk |
			       LCDC_HEOCFG30_VXSCCFG(1) |
			       LCDC_HEOCFG30_VXSCTAP2_Msk;

	regs->LCDC_HEOCFG31 = LCDC_HEOCFG31_HXSYCFG(1) |
			       LCDC_HEOCFG31_HXSYTAP2_Msk |
			       LCDC_HEOCFG31_HXSCCFG(1) |
			       LCDC_HEOCFG31_HXSCTAP2_Msk;

	/* Zero tap coefficients (hardware uses default bilinear with TAP2) */
	for (int i = 0; i < LCDC_HEOVTAP_NUMBER; i++) {
		regs->LCDC_HEOVTAP[i].LCDC_HEOVTAP10P = 0;
		regs->LCDC_HEOVTAP[i].LCDC_HEOVTAP32P = 0;
	}
	for (int i = 0; i < LCDC_HEOHTAP_NUMBER; i++) {
		regs->LCDC_HEOHTAP[i].LCDC_HEOHTAP10P = 0;
		regs->LCDC_HEOHTAP[i].LCDC_HEOHTAP32P = 0;
	}
}

int xlcdc_layer_heo_display_rgb(const struct device *dev,
				const struct xlcdc_heo_rgb_surface *s)
{
	const struct xlcdc_layer_config *cfg = dev->config;
	struct xlcdc_layer_data *data = dev->data;
	const struct xlcdc_parent_config *pcfg = cfg->parent->config;
	lcdc_registers_t *regs = pcfg->regs;

	if (cfg->type != XLCDC_LAYER_HEO) {
		return -ENOTSUP;
	}

	if (s->buf == NULL || s->img_w == 0 || s->img_h == 0 ||
	    s->win_w == 0 || s->win_h == 0) {
		return -EINVAL;
	}

	/* Update pixel format in HEOCFG1 (RGB mode, no YCC) */
	regs->LCDC_HEOCFG1 = LCDC_HEOCFG1_CLUTEN(0) |
			      LCDC_HEOCFG1_YCCEN(0) |
			      LCDC_HEOCFG1_GAM(0) |
			      LCDC_HEOCFG1_RGBMODE(s->pixel_format) |
			      LCDC_HEOCFG1_CLUTMODE(3) |
			      LCDC_HEOCFG1_YCCMODE(0) |
			      LCDC_HEOCFG1_YCC422ROT(0) |
			      LCDC_HEOCFG1_ILD(0);

	/* HEOCFG3: Output window size on screen */
	regs->LCDC_HEOCFG3 = LCDC_HEOCFG3_XSIZE(s->win_w - 1) |
			      LCDC_HEOCFG3_YSIZE(s->win_h - 1);

	/* HEOCFG4: Source image size in memory */
	regs->LCDC_HEOCFG4 = LCDC_HEOCFG4_XMEMSIZE(s->img_w - 1) |
			      LCDC_HEOCFG4_YMEMSIZE(s->img_h - 1);

	/* HEOCFG5: X stride = 0 (contiguous rows) */
	regs->LCDC_HEOCFG5 = 0;
	/* HEOCFG6: Pixel stride = 0 */
	regs->LCDC_HEOCFG6 = 0;

	/* Configure the scaler */
	xlcdc_heo_configure_scaler(regs, s->img_w, s->img_h,
				   s->win_w, s->win_h);

	/* Set FB address and flush cache */
	uint32_t fb_size = (uint32_t)s->img_w * s->img_h * xlcdc_bpp(s->pixel_format);

	sys_cache_data_flush_range(s->buf, fb_size);

	regs->LCDC_HEO[0].LCDC_HEOYFBA = (uint32_t)(uintptr_t)s->buf;
	regs->LCDC_HEO[0].LCDC_HEOCBFBA = 0;
	regs->LCDC_HEO[0].LCDC_HEOCRFBA = 0;

	/* Enable DMA with per-pixel alpha blending */
	regs->LCDC_HEOCFG12 = LCDC_HEOCFG12_DMA(1) |
			       LCDC_HEOCFG12_REP(1) |
			       LCDC_HEOCFG12_CRKEY(0) |
			       LCDC_HEOCFG12_DSTKEY(0) |
			       LCDC_HEOCFG12_VIDPRI(cfg->heo_video_priority ? 1 : 0) |
			       LCDC_HEOCFG12_SFACTC(4) |
			       LCDC_HEOCFG12_SFACTA(1) |
			       LCDC_HEOCFG12_DFACTC(6) |
			       LCDC_HEOCFG12_DFACTA(2) |
			       LCDC_HEOCFG12_A0(data->alpha) |
			       LCDC_HEOCFG12_A1(0);

	/* Update tracked state */
	data->win_w = s->win_w;
	data->win_h = s->win_h;
	data->pixel_format = s->pixel_format;
	data->dma_enabled = true;

	/* Commit all changes atomically */
	xlcdc_parent_commit_attrs(cfg->parent, xlcdc_layer_attre_mask(cfg->type));

	LOG_INF("HEO: displaying %ux%u RGB (fmt=%u) scaled to %ux%u",
		s->img_w, s->img_h, s->pixel_format, s->win_w, s->win_h);

	return 0;
}

/* ========================================================================= */
/*                       HEO YUV Display                                     */
/* ========================================================================= */

/*
 * BT.709 YCbCr→RGB color space conversion matrix.
 * Each coefficient is scaled by 1024 and stored as 13-bit signed (& 0x1FFF).
 *
 * R = 1.0*Y + 0.0*Cb     + 1.28033*Cr
 * G = 1.0*Y - 0.21482*Cb - 0.38059*Cr
 * B = 1.0*Y + 2.12798*Cb + 0.0*Cr
 */
#define CSC_RYGAIN   (1024U  & 0x1FFFU)  /* 1.0 * 1024 */
#define CSC_RCBGAIN  (0U     & 0x1FFFU)
#define CSC_RCRGAIN  (1311U  & 0x1FFFU)  /* 1.28033 * 1024 */
#define CSC_GYGAIN   (1024U  & 0x1FFFU)
#define CSC_GCBGAIN  ((-220) & 0x1FFF)   /* -0.21482 * 1024 (signed) */
#define CSC_GCRGAIN  ((-390) & 0x1FFF)   /* -0.38059 * 1024 (signed) */
#define CSC_BYGAIN   (1024U  & 0x1FFFU)
#define CSC_BCBGAIN  (2179U  & 0x1FFFU)  /* 2.12798 * 1024 */
#define CSC_BCRGAIN  (0U     & 0x1FFFU)

static void xlcdc_heo_load_bt709_csc(lcdc_registers_t *regs)
{
	regs->LCDC_HEOCFG16 = LCDC_HEOCFG16_RYGAIN(CSC_RYGAIN) |
			       LCDC_HEOCFG16_RCBGAIN(CSC_RCBGAIN);
	regs->LCDC_HEOCFG17 = LCDC_HEOCFG17_RCRGAIN(CSC_RCRGAIN);

	regs->LCDC_HEOCFG18 = LCDC_HEOCFG18_GYGAIN(CSC_GYGAIN) |
			       LCDC_HEOCFG18_GCBGAIN(CSC_GCBGAIN);
	regs->LCDC_HEOCFG19 = LCDC_HEOCFG19_GCRGAIN(CSC_GCRGAIN);

	regs->LCDC_HEOCFG20 = LCDC_HEOCFG20_BYGAIN(CSC_BYGAIN) |
			       LCDC_HEOCFG20_BCBGAIN(CSC_BCBGAIN);
	regs->LCDC_HEOCFG21 = LCDC_HEOCFG21_BCRGAIN(CSC_BCRGAIN);

	regs->LCDC_HEOCFG22 = LCDC_HEOCFG22_YOFF(1) |
			       LCDC_HEOCFG22_CBOFF(1) |
			       LCDC_HEOCFG22_CROFF(1);
}

int xlcdc_layer_heo_display_yuv(const struct device *dev,
				const struct xlcdc_heo_yuv_surface *s)
{
	const struct xlcdc_layer_config *cfg = dev->config;
	struct xlcdc_layer_data *data = dev->data;
	const struct xlcdc_parent_config *pcfg = cfg->parent->config;
	lcdc_registers_t *regs = pcfg->regs;

	if (cfg->type != XLCDC_LAYER_HEO) {
		return -ENOTSUP;
	}

	if (s->buf == NULL || s->img_w == 0 || s->img_h == 0 ||
	    s->win_w == 0 || s->win_h == 0) {
		return -EINVAL;
	}

	/* Enable YCbCr mode in HEOCFG1 */
	regs->LCDC_HEOCFG1 = LCDC_HEOCFG1_CLUTEN(0) |
			      LCDC_HEOCFG1_YCCEN(1) |
			      LCDC_HEOCFG1_GAM(0) |
			      LCDC_HEOCFG1_RGBMODE(0) |
			      LCDC_HEOCFG1_CLUTMODE(3) |
			      LCDC_HEOCFG1_YCCMODE(s->mode) |
			      LCDC_HEOCFG1_YCC422ROT(0) |
			      LCDC_HEOCFG1_ILD(0);

	/* Load BT.709 CSC coefficients */
	xlcdc_heo_load_bt709_csc(regs);

	/* Window and memory sizes */
	regs->LCDC_HEOCFG3 = LCDC_HEOCFG3_XSIZE(s->win_w - 1) |
			      LCDC_HEOCFG3_YSIZE(s->win_h - 1);
	regs->LCDC_HEOCFG4 = LCDC_HEOCFG4_XMEMSIZE(s->img_w - 1) |
			      LCDC_HEOCFG4_YMEMSIZE(s->img_h - 1);

	/* Strides: 0 for contiguous */
	regs->LCDC_HEOCFG5 = 0;
	regs->LCDC_HEOCFG6 = 0;
	regs->LCDC_HEOCFG7 = 0;
	regs->LCDC_HEOCFG8 = 0;

	/* Configure scaler */
	xlcdc_heo_configure_scaler(regs, s->img_w, s->img_h,
				   s->win_w, s->win_h);

	/* Halve chroma scaler factor for 4:2:2 horizontal subsampling */
	if (s->mode >= XLCDC_YCC_YUYV_422 && s->mode <= XLCDC_YCC_422_PL) {
		uint32_t hfact = xlcdc_heo_scale_factor(s->img_w, s->win_w);

		regs->LCDC_HEOCFG27 = LCDC_HEOCFG27_HXSCFACT(hfact / 2);
	}

	/* Set plane addresses */
	regs->LCDC_HEO[0].LCDC_HEOYFBA = (uint32_t)(uintptr_t)s->buf;
	regs->LCDC_HEO[0].LCDC_HEOCBFBA = s->cb_buf ?
		(uint32_t)(uintptr_t)s->cb_buf : 0;
	regs->LCDC_HEO[0].LCDC_HEOCRFBA = s->cr_buf ?
		(uint32_t)(uintptr_t)s->cr_buf : 0;

	/* Enable DMA */
	regs->LCDC_HEOCFG12 = LCDC_HEOCFG12_DMA(1) |
			       LCDC_HEOCFG12_REP(1) |
			       LCDC_HEOCFG12_CRKEY(0) |
			       LCDC_HEOCFG12_DSTKEY(0) |
			       LCDC_HEOCFG12_VIDPRI(cfg->heo_video_priority ? 1 : 0) |
			       LCDC_HEOCFG12_SFACTC(4) |
			       LCDC_HEOCFG12_SFACTA(1) |
			       LCDC_HEOCFG12_DFACTC(6) |
			       LCDC_HEOCFG12_DFACTA(2) |
			       LCDC_HEOCFG12_A0(data->alpha) |
			       LCDC_HEOCFG12_A1(0);

	data->win_w = s->win_w;
	data->win_h = s->win_h;
	data->dma_enabled = true;

	xlcdc_parent_commit_attrs(cfg->parent, xlcdc_layer_attre_mask(cfg->type));

	LOG_INF("HEO: displaying %ux%u YUV (mode=%u) scaled to %ux%u",
		s->img_w, s->img_h, s->mode, s->win_w, s->win_h);

	return 0;
}

/* ========================================================================= */
/*                       Layer Initialization                                */
/* ========================================================================= */

static int xlcdc_layer_init(const struct device *dev)
{
	const struct xlcdc_layer_config *cfg = dev->config;
	struct xlcdc_layer_data *data = dev->data;
	const struct xlcdc_parent_config *pcfg;
	lcdc_registers_t *regs;

	if (!device_is_ready(cfg->parent)) {
		LOG_ERR("Parent XLCDC device not ready");
		return -ENODEV;
	}

	pcfg = cfg->parent->config;
	regs = pcfg->regs;

	data->pixel_format = cfg->pixel_format;
	data->win_w = cfg->init_width ? cfg->init_width : pcfg->width;
	data->win_h = cfg->init_height ? cfg->init_height : pcfg->height;
	data->alpha = cfg->global_alpha;
	data->dma_enabled = false;
	data->front_idx = 0;
	data->active_buf = NULL;
	data->vsync_handle = 0;

	/* Compute framebuffer size */
	uint32_t fb_needed = (uint32_t)data->win_w * data->win_h *
			     xlcdc_bpp(data->pixel_format);

	if (cfg->no_alloc_fb) {
		data->fb[0] = NULL;
		data->fb[1] = NULL;
		data->fb_size = fb_needed;
		LOG_INF("%s: No-alloc FB mode (zero-copy), expected buffer size %u",
			dev->name, data->fb_size);
	} else {
		data->fb[0] = k_aligned_alloc(128, fb_needed);
		if (data->fb[0] == NULL) {
			LOG_ERR("Failed to allocate %u bytes for %s framebuffer",
				fb_needed, dev->name);
			return -ENOMEM;
		}
		data->fb_size = fb_needed;

		memset(data->fb[0], 0, data->fb_size);
		sys_cache_data_flush_range(data->fb[0], data->fb_size);

#ifdef CONFIG_MICROCHIP_SAM_XLCDC_DOUBLE_BUFFER
		data->fb[1] = k_aligned_alloc(128, fb_needed);
		if (data->fb[1] == NULL) {
			LOG_ERR("Failed to allocate %u bytes for %s back buffer",
				fb_needed, dev->name);
			return -ENOMEM;
		}
		memset(data->fb[1], 0, data->fb_size);
		sys_cache_data_flush_range(data->fb[1], data->fb_size);
		LOG_INF("%s: Double-buffer FBs allocated %u bytes each at %p, %p",
			dev->name, fb_needed, data->fb[0], data->fb[1]);
#else
		data->fb[1] = NULL;
		LOG_INF("%s: FB allocated %u bytes at %p",
			dev->name, fb_needed, data->fb[0]);
#endif
	}

	/* Setup layer hardware registers */
	switch (cfg->type) {
	case XLCDC_LAYER_BASE:
		xlcdc_layer_setup_base(regs, cfg, pcfg->width, pcfg->height);
		break;
	case XLCDC_LAYER_OVR1:
		xlcdc_layer_setup_ovr1(regs, cfg, data->win_w, data->win_h);
		break;
	case XLCDC_LAYER_HEO:
		xlcdc_layer_setup_heo(regs, cfg, data->win_w, data->win_h);
		break;
	case XLCDC_LAYER_OVR2:
#if defined(LCDC_OVR2CFG0_BLEN_Pos)
		xlcdc_layer_setup_ovr2(regs, cfg, data->win_w, data->win_h);
#else
		LOG_WRN("OVR2 HAL macros not available for this SoC");
		return -ENOTSUP;
#endif
		break;
	default:
		LOG_ERR("Unknown layer type %d", cfg->type);
		return -EINVAL;
	}

	/* Apply initial DT position for overlay layers */
	if (cfg->type != XLCDC_LAYER_BASE) {
		xlcdc_layer_set_window_size(dev, data->win_w, data->win_h);
		xlcdc_layer_set_position(dev, cfg->x_origin, cfg->y_origin);
	}

	/* Commit initial layer configuration */
	xlcdc_parent_commit_attrs(cfg->parent, xlcdc_layer_attre_mask(cfg->type));

	LOG_INF("%s: Layer init complete (type=%d, %ux%u, fmt=%u)",
		dev->name, cfg->type, data->win_w, data->win_h, data->pixel_format);

	return 0;
}

/* ========================================================================= */
/*                       Device Instantiation                                */
/* ========================================================================= */

/*
 * Each layer child node has compatible "microchip,sam-xlcdc-layer" and gets
 * its own DT instance number. We iterate all layer instances directly using
 * DT_INST_FOREACH_STATUS_OKAY with DT_DRV_COMPAT set to the layer compat.
 */

#define DT_DRV_COMPAT microchip_sam_xlcdc_layer

#define XLCDC_LAYER_DEFINE(inst) \
	BUILD_ASSERT(DT_INST_PROP_OR(inst, x_origin, 0) <= 2047, \
		"XLCDC layer " #inst ": x-origin exceeds 11-bit max (2047)"); \
	BUILD_ASSERT(DT_INST_PROP_OR(inst, y_origin, 0) <= 2047, \
		"XLCDC layer " #inst ": y-origin exceeds 11-bit max (2047)"); \
	BUILD_ASSERT(DT_INST_PROP_OR(inst, width, 0) <= 2048, \
		"XLCDC layer " #inst ": width exceeds max (2048)"); \
	BUILD_ASSERT(DT_INST_PROP_OR(inst, height, 0) <= 2048, \
		"XLCDC layer " #inst ": height exceeds max (2048)"); \
	BUILD_ASSERT(DT_INST_PROP_OR(inst, x_origin, 0) + \
		DT_INST_PROP_OR(inst, width, 0) <= \
		DT_PROP(DT_INST_PARENT(inst), width), \
		"XLCDC layer " #inst ": x-origin + width exceeds parent " \
		"width"); \
	BUILD_ASSERT(DT_INST_PROP_OR(inst, y_origin, 0) + \
		DT_INST_PROP_OR(inst, height, 0) <= \
		DT_PROP(DT_INST_PARENT(inst), height), \
		"XLCDC layer " #inst ": y-origin + height exceeds parent " \
		"height"); \
	static struct xlcdc_layer_data xlcdc_layer_data_##inst; \
	static const struct xlcdc_layer_config xlcdc_layer_cfg_##inst = { \
		.parent = DEVICE_DT_GET(DT_INST_PARENT(inst)), \
		.type = (enum xlcdc_layer_type)DT_INST_ENUM_IDX(inst, \
							layer_type), \
		.pixel_format = DT_INST_PROP_OR(inst, pixel_format, \
			DT_PROP(DT_INST_PARENT(inst), pixel_format)), \
		.default_r = COND_CODE_1( \
			DT_INST_NODE_HAS_PROP(inst, default_color), \
			(DT_INST_PROP_BY_IDX(inst, default_color, 0)), (0)), \
		.default_g = COND_CODE_1( \
			DT_INST_NODE_HAS_PROP(inst, default_color), \
			(DT_INST_PROP_BY_IDX(inst, default_color, 1)), (0)), \
		.default_b = COND_CODE_1( \
			DT_INST_NODE_HAS_PROP(inst, default_color), \
			(DT_INST_PROP_BY_IDX(inst, default_color, 2)), (0)), \
		.global_alpha = DT_INST_PROP_OR(inst, global_alpha, 255), \
		.heo_video_priority = DT_INST_PROP_OR(inst, \
						heo_video_priority, false), \
		.init_width = DT_INST_PROP_OR(inst, width, 0), \
		.init_height = DT_INST_PROP_OR(inst, height, 0), \
		.x_origin = DT_INST_PROP_OR(inst, x_origin, 0), \
		.y_origin = DT_INST_PROP_OR(inst, y_origin, 0), \
		.no_alloc_fb = DT_INST_PROP(inst, no_alloc_framebuffer), \
	}; \
	DEVICE_DT_INST_DEFINE(inst, xlcdc_layer_init, NULL, \
			      &xlcdc_layer_data_##inst, \
			      &xlcdc_layer_cfg_##inst, \
			      POST_KERNEL, \
			      CONFIG_XLCDC_LAYER_INIT_PRIORITY, \
			      &xlcdc_layer_display_api);

DT_INST_FOREACH_STATUS_OKAY(XLCDC_LAYER_DEFINE)
