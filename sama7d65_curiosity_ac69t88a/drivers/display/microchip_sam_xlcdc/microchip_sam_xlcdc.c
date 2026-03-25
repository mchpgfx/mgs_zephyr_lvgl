/*
 * Copyright (c) 2026 Microchip Technology Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Microchip SAM LCD controller parent driver. Initializes clocks,
 * PLL, display timing, and output sequencing. Individual display
 * layers are managed by the companion layer driver.
 */

#define DT_DRV_COMPAT microchip_sam9x7_xlcdc

#include <string.h>

#include <soc.h>
#include <zephyr/device.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "microchip_sam_xlcdc.h"

LOG_MODULE_REGISTER(microchip_sam_xlcdc, CONFIG_DISPLAY_LOG_LEVEL);

/* ========================================================================= */
/*                       Shared helpers (exported)                           */
/* ========================================================================= */

uint8_t xlcdc_bpp(uint8_t rgbmode)
{
	switch (rgbmode) {
	case XLCDC_RGB_444:
	case XLCDC_ARGB_4444:
	case XLCDC_RGBA_4444:
	case XLCDC_RGB_565:
	case XLCDC_ARGB_1555:
		return 2;
	case XLCDC_RGB_666_PACKED:
	case XLCDC_ARGB_1666_PACKED:
	case XLCDC_RGB_888_PACKED:
		return 3;
	case XLCDC_RGB_666:
	case XLCDC_RGB_888:
	case XLCDC_ARGB_1666:
	case XLCDC_ARGB_1888:
	case XLCDC_ARGB_8888:
	case XLCDC_RGBA_8888:
		return 4;
	default:
		return 4;
	}
}

int xlcdc_wait_sipsts(lcdc_registers_t *regs)
{
	uint32_t start = k_cycle_get_32();

	while (regs->LCDC_LCDSR & LCDC_LCDSR_SIPSTS_Msk) {
		if (k_cyc_to_us_ceil32(k_cycle_get_32() - start) >
		    XLCDC_SIP_TIMEOUT_US) {
			LOG_WRN("SIP status timeout");
			return -ETIMEDOUT;
		}
	}
	return 0;
}

static inline int xlcdc_wait_attrs_sip(lcdc_registers_t *regs)
{
	uint32_t start = k_cycle_get_32();

	while (regs->LCDC_ATTRS & LCDC_ATTRS_SIP_Msk) {
		if (k_cyc_to_us_ceil32(k_cycle_get_32() - start) >
		    XLCDC_SIP_TIMEOUT_US) {
			LOG_WRN("ATTRS SIP timeout");
			return -ETIMEDOUT;
		}
	}
	return 0;
}

uint32_t xlcdc_layer_attre_mask(enum xlcdc_layer_type type)
{
	switch (type) {
	case XLCDC_LAYER_BASE:
		return LCDC_ATTRE_BASE_Msk;
	case XLCDC_LAYER_OVR1:
		return LCDC_ATTRE_OVR1_Msk;
	case XLCDC_LAYER_HEO:
		return LCDC_ATTRE_HEO_Msk;
#if defined(LCDC_OVR2CFG0_BLEN_Pos)
	case XLCDC_LAYER_OVR2:
		return LCDC_ATTRE_OVR2_Msk;
#endif
	default:
		return 0;
	}
}

void xlcdc_parent_commit_attrs(const struct device *parent, uint32_t attre_mask)
{
	const struct xlcdc_parent_config *cfg = parent->config;
	struct xlcdc_parent_data *data = parent->data;
	lcdc_registers_t *regs = cfg->regs;
	uint32_t start;

	k_mutex_lock(&data->attre_lock, K_FOREVER);

	start = k_cycle_get_32();
	while (regs->LCDC_ATTRE) {
		if (k_cyc_to_us_ceil32(k_cycle_get_32() - start) >
		    XLCDC_SIP_TIMEOUT_US) {
			LOG_WRN("ATTRE busy timeout");
			break;
		}
	}
	regs->LCDC_ATTRE = attre_mask;
	xlcdc_wait_attrs_sip(regs);

	k_mutex_unlock(&data->attre_lock);
}

void xlcdc_parent_backlight_enable(const struct device *parent)
{
	const struct xlcdc_parent_config *cfg = parent->config;
	lcdc_registers_t *regs = cfg->regs;

	regs->LCDC_LCDEN = LCDC_LCDEN_PWMEN_Msk;
	xlcdc_wait_sipsts(regs);
	while (!(regs->LCDC_LCDSR & LCDC_LCDSR_PWMSTS_Msk)) {
	}
}

void xlcdc_parent_backlight_disable(const struct device *parent)
{
	const struct xlcdc_parent_config *cfg = parent->config;
	lcdc_registers_t *regs = cfg->regs;

	regs->LCDC_LCDDIS = LCDC_LCDDIS_PWMDIS_Msk;
	xlcdc_wait_sipsts(regs);
	while (regs->LCDC_LCDSR & LCDC_LCDSR_PWMSTS_Msk) {
	}
}

int xlcdc_parent_set_brightness(const struct device *parent, uint8_t brightness)
{
	const struct xlcdc_parent_config *cfg = parent->config;
	struct xlcdc_parent_data *data = parent->data;
	lcdc_registers_t *regs = cfg->regs;

	xlcdc_wait_sipsts(regs);
	regs->LCDC_LCDCFG6 = (regs->LCDC_LCDCFG6 & ~LCDC_LCDCFG6_PWMCVAL_Msk) |
			      LCDC_LCDCFG6_PWMCVAL(brightness);
	xlcdc_wait_sipsts(regs);

	data->brightness = brightness;
	LOG_DBG("Brightness set to %u", brightness);
	return 0;
}

/* ========================================================================= */
/*                       VSYNC interrupt support                             */
/* ========================================================================= */

static void xlcdc_isr(const struct device *dev)
{
	const struct xlcdc_parent_config *cfg = dev->config;
	struct xlcdc_parent_data *data = dev->data;
	uint32_t isr = cfg->regs->LCDC_LCDISR; /* read clears flags */

	if (isr & LCDC_LCDISR_SOF_Msk) {
		struct display_event_data evt = {
			.timestamp = k_cycle_get_64(),
		};

		for (int i = 0; i < data->vsync_cb_count; i++) {
			if (data->vsync_cbs[i].cb) {
				data->vsync_cbs[i].cb(data->vsync_cbs[i].dev,
						      DISPLAY_EVENT_VSYNC,
						      &evt,
						      data->vsync_cbs[i].user_data);
			}
		}
	}
}

int xlcdc_parent_register_vsync(const struct device *parent,
				const struct device *layer_dev,
				display_event_cb_t cb, void *user_data,
				uint32_t *out_handle)
{
	struct xlcdc_parent_data *data = parent->data;

	if (cb == NULL || out_handle == NULL) {
		return -EINVAL;
	}

	if (data->vsync_cb_count >= XLCDC_MAX_VSYNC_CBS) {
		return -ENOSPC;
	}

	unsigned int key = irq_lock();
	uint8_t idx = data->vsync_cb_count;

	data->vsync_cbs[idx].cb = cb;
	data->vsync_cbs[idx].user_data = user_data;
	data->vsync_cbs[idx].dev = layer_dev;
	data->vsync_cb_count++;
	*out_handle = idx + 1; /* 1-indexed */
	irq_unlock(key);

	LOG_DBG("VSYNC callback registered (handle=%u)", *out_handle);
	return 0;
}

int xlcdc_parent_unregister_vsync(const struct device *parent, uint32_t handle)
{
	struct xlcdc_parent_data *data = parent->data;

	if (handle == 0 || handle > data->vsync_cb_count) {
		return -EINVAL;
	}

	unsigned int key = irq_lock();
	uint8_t idx = handle - 1;

	/* Shift remaining entries down */
	for (uint8_t i = idx; i < data->vsync_cb_count - 1; i++) {
		data->vsync_cbs[i] = data->vsync_cbs[i + 1];
	}
	data->vsync_cb_count--;
	memset(&data->vsync_cbs[data->vsync_cb_count], 0,
	       sizeof(struct xlcdc_vsync_cb));
	irq_unlock(key);

	LOG_DBG("VSYNC callback unregistered (handle=%u)", handle);
	return 0;
}

/* ========================================================================= */
/*                       Output mode decode                                  */
/* ========================================================================= */

struct output_mode_desc {
	uint8_t dpi;
	uint8_t mode;
};

static struct output_mode_desc xlcdc_decode_output_mode(uint8_t output_mode)
{
	struct output_mode_desc d;

	switch (output_mode) {
	case XLCDC_OUTPUT_DPI_24:
		d.dpi = 1;
		d.mode = LCDC_LCDCFG5_DPI_MODE_OUTPUT_DPI_24BPP_Val;
		break;
	case XLCDC_OUTPUT_DPI_18:
		d.dpi = 1;
		d.mode = LCDC_LCDCFG5_DPI_MODE_OUTPUT_DPI_18BPPCFG1_Val;
		break;
	case XLCDC_OUTPUT_DPI_16:
		d.dpi = 1;
		d.mode = LCDC_LCDCFG5_DPI_MODE_OUTPUT_DPI_16BPPCFG1_Val;
		break;
	case XLCDC_OUTPUT_LEGACY_24:
		d.dpi = 0;
		d.mode = LCDC_LCDCFG5_LEGACY_MODE_OUTPUT_24BPP_Val;
		break;
	case XLCDC_OUTPUT_LEGACY_18:
		d.dpi = 0;
		d.mode = LCDC_LCDCFG5_LEGACY_MODE_OUTPUT_18BPP_Val;
		break;
	case XLCDC_OUTPUT_LEGACY_16:
		d.dpi = 0;
		d.mode = LCDC_LCDCFG5_LEGACY_MODE_OUTPUT_16BPP_Val;
		break;
	case XLCDC_OUTPUT_LEGACY_12:
		d.dpi = 0;
		d.mode = LCDC_LCDCFG5_LEGACY_MODE_OUTPUT_12BPP_Val;
		break;
	default:
		d.dpi = 1;
		d.mode = LCDC_LCDCFG5_DPI_MODE_OUTPUT_DPI_24BPP_Val;
		break;
	}

	return d;
}

/* ========================================================================= */
/*                       Clock Setup                                         */
/* ========================================================================= */

/*
 * LVDSPLL feeds the XLCDC pixel clock.
 *
 * The LVDS serializer requires a clock at 7x the pixel clock frequency.
 * LVDSPLL provides this: PLL_out / 7 = pixel clock (fixed internal divider).
 *
 * Formula:
 *   PLL_core = MOSCXT * (MUL + FRACR / 2^22)   [must be 600-1200 MHz]
 *   PLL_out  = PLL_core / DIVPMC
 *   pixel_clk = PLL_out / 7
 *
 * So: PLL_out = pixel_clk * 7
 *     PLL_core = PLL_out * DIVPMC
 *
 * We choose DIVPMC to keep PLL_core in 600-1200 MHz range, then compute
 * MUL and FRACR from: MUL + FRACR/2^22 = PLL_core / MOSCXT
 *
 * LVDSPLL index: 7 on SAMA7D65, 3 on SAM9X7x
 */

#define LVDSPLL_CORE_MIN_HZ  600000000ULL
#define LVDSPLL_CORE_MAX_HZ  1200000000ULL
#define LVDSPLL_FRAC_BITS    22
#define LVDSPLL_MOSCXT_HZ    DT_PROP(DT_NODELABEL(main_xtal), clock_frequency)

#ifndef LVDSPLL_INDEX
#if defined(CONFIG_SOC_SERIES_SAMA7D6)
#define LVDSPLL_INDEX        7
#define LVDSPLL_ISR0_LOCK_Msk PMC_PLL_ISR0_LOCK7_Msk
#else
#define LVDSPLL_INDEX        3
#define LVDSPLL_ISR0_LOCK_Msk (1U << 3)
#endif
#endif

static void xlcdc_setup_lvdspll(uint32_t pixel_clk_hz)
{
	uint64_t pll_out = (uint64_t)pixel_clk_hz * 7;
	uint32_t divpmc = 1;
	uint64_t pll_core;

	for (divpmc = 1; divpmc <= 256; divpmc++) {
		pll_core = pll_out * divpmc;
		if (pll_core >= LVDSPLL_CORE_MIN_HZ &&
		    pll_core <= LVDSPLL_CORE_MAX_HZ) {
			break;
		}
	}

	if (divpmc > 256) {
		LOG_ERR("Cannot find valid LVDSPLL divider for %u Hz pixel clock",
			pixel_clk_hz);
		return;
	}

	pll_core = pll_out * divpmc;

	uint32_t mul = (uint32_t)(pll_core / LVDSPLL_MOSCXT_HZ);
	uint64_t remainder = pll_core - (uint64_t)mul * LVDSPLL_MOSCXT_HZ;
	uint32_t fracr = (uint32_t)((remainder << LVDSPLL_FRAC_BITS) / LVDSPLL_MOSCXT_HZ);

	LOG_INF("LVDSPLL: pixel=%u Hz, PLL_out=%llu Hz, PLL_core=%llu Hz",
		pixel_clk_hz, pll_out, pll_core);
	LOG_INF("  DIVPMC=%u, MUL=%u, FRACR=%u (index=%u)",
		divpmc, mul, fracr, LVDSPLL_INDEX);

	PMC_REGS->PMC_PLL_UPDT = PMC_PLL_UPDT_STUPTIM(0x6) |
				  PMC_PLL_UPDT_ID(LVDSPLL_INDEX);

	PMC_REGS->PMC_PLL_ACR = PMC_PLL_ACR_LOOP_FILTER(0x1B) |
				 PMC_PLL_ACR_LOCK_THR(0x4) |
				 PMC_PLL_ACR_CONTROL(0x10);

	PMC_REGS->PMC_PLL_CTRL1 = PMC_PLL_CTRL1_MUL(mul - 1) |
				   PMC_PLL_CTRL1_FRACR(fracr);

	PMC_REGS->PMC_PLL_UPDT |= PMC_PLL_UPDT_UPDATE_Msk;

	PMC_REGS->PMC_PLL_CTRL0 = PMC_PLL_CTRL0_ENLOCK_Msk |
				   PMC_PLL_CTRL0_ENPLL_Msk |
				   PMC_PLL_CTRL0_DIVPMC(divpmc - 1) |
				   PMC_PLL_CTRL0_ENPLLCK_Msk;

	PMC_REGS->PMC_PLL_UPDT |= PMC_PLL_UPDT_UPDATE_Msk;

	while (!(PMC_REGS->PMC_PLL_ISR0 & LVDSPLL_ISR0_LOCK_Msk)) {
	}

	LOG_INF("LVDSPLL locked");
}

static void xlcdc_enable_clocks(const struct xlcdc_parent_config *cfg)
{
	lcdc_registers_t *regs = cfg->regs;

	LOG_INF("Configuring XLCDC clocks (pixel clock: %u Hz)", cfg->pclk);

	xlcdc_setup_lvdspll(cfg->pclk);

	PMC_REGS->PMC_PCR = PMC_PCR_CMD_Msk |
			    PMC_PCR_PID(ID_LCDC) |
			    PMC_PCR_EN_Msk;

	regs->LCDC_LCDCFG0 = LCDC_LCDCFG0_CLKPOL(1) |
			      LCDC_LCDCFG0_CLKBYP(1) |
			      LCDC_LCDCFG0_CLKPWMSEL(cfg->pwm_source) |
			      LCDC_LCDCFG0_CLKDIV(0);
	xlcdc_wait_sipsts(regs);

	regs->LCDC_LCDCFG6 = LCDC_LCDCFG6_PWMCVAL(cfg->pwm_brightness) |
			      LCDC_LCDCFG6_PWMPOL(1) |
			      LCDC_LCDCFG6_PWMPS(cfg->pwm_prescaler);
	xlcdc_wait_sipsts(regs);
}

/* ========================================================================= */
/*                       Timing Engine Setup                                 */
/* ========================================================================= */

static void xlcdc_setup_timing_engine(const struct xlcdc_parent_config *cfg)
{
	lcdc_registers_t *regs = cfg->regs;
	struct output_mode_desc om = xlcdc_decode_output_mode(cfg->output_mode);

	LOG_INF("Timing: %ux%u, HSPW=%u HFP=%u HBP=%u, VSPW=%u VFP=%u VBP=%u",
		cfg->width, cfg->height,
		cfg->hspw, cfg->hfpw, cfg->hbpw,
		cfg->vspw, cfg->vfpw, cfg->vbpw);

	regs->LCDC_LCDCFG1 = LCDC_LCDCFG1_VSPW(cfg->vspw - 1) |
			      LCDC_LCDCFG1_HSPW(cfg->hspw - 1);
	xlcdc_wait_sipsts(regs);

	regs->LCDC_LCDCFG2 = LCDC_LCDCFG2_VBPW(cfg->vbpw - 1) |
			      LCDC_LCDCFG2_VFPW(cfg->vfpw - 1);
	xlcdc_wait_sipsts(regs);

	regs->LCDC_LCDCFG3 = LCDC_LCDCFG3_HBPW(cfg->hbpw - 1) |
			      LCDC_LCDCFG3_HFPW(cfg->hfpw - 1);
	xlcdc_wait_sipsts(regs);

	regs->LCDC_LCDCFG4 = LCDC_LCDCFG4_RPF(cfg->height - 1) |
			      LCDC_LCDCFG4_PPL(cfg->width - 1);
	xlcdc_wait_sipsts(regs);

	uint32_t cfg5 = LCDC_LCDCFG5_HSPOL(cfg->hsync_active) |
			LCDC_LCDCFG5_VSPOL(cfg->vsync_active) |
			LCDC_LCDCFG5_VSPDLYS(1) |
			LCDC_LCDCFG5_VSPDLYE(0) |
			LCDC_LCDCFG5_DISPPOL(cfg->de_active) |
			LCDC_LCDCFG5_DITHER(0) |
			LCDC_LCDCFG5_DISPDLY(1) |
			LCDC_LCDCFG5_DPI(om.dpi) |
			LCDC_LCDCFG5_GUARDTIME(cfg->guard_time);

	if (om.dpi) {
		cfg5 |= LCDC_LCDCFG5_DPI_MODE(om.mode);
	} else {
		cfg5 |= LCDC_LCDCFG5_LEGACY_MODE(om.mode);
	}

	regs->LCDC_LCDCFG5 = cfg5;
	xlcdc_wait_sipsts(regs);

	LOG_INF("Output mode: DPI=%u MODE=%u, Guard=%u", om.dpi, om.mode, cfg->guard_time);
}

/* ========================================================================= */
/*                       Start                                               */
/* ========================================================================= */

/*
 * Pre-enable all layer registers before XLCDC_Start().
 *
 * Harmony initializes all layers (including HEOEN=ENABLE + ATTRE commit)
 * BEFORE starting the display pipeline. The HEO layer's DMA engine
 * requires the layer to be enabled before CLKEN/SYNCEN/DISPEN/SDEN.
 * BASE and OVR1 tolerate post-start enable, but HEO does not.
 *
 * This function enables all layers with DMA=0 and default color (0,0,0).
 * The per-layer drivers then configure registers and enable DMA later.
 */
static void xlcdc_pre_enable_layers(lcdc_registers_t *regs, bool has_ovr2)
{
	/* BASE: minimal config, enable, commit */
	regs->LCDC_BASECFG0 = LCDC_BASECFG0_BLEN(4);
	regs->LCDC_BASECFG4 = LCDC_BASECFG4_DMA(0) | LCDC_BASECFG4_REP(1);
	regs->LCDC_BASEEN = LCDC_BASEEN_ENABLE_Msk;
	regs->LCDC_ATTRE = LCDC_ATTRE_BASE_Msk;
	while (regs->LCDC_ATTRS & LCDC_ATTRS_SIP_Msk) {
	}

	/* OVR1: minimal config, enable, commit */
	regs->LCDC_OVR1CFG0 = LCDC_OVR1CFG0_BLEN(4);
	regs->LCDC_OVR1CFG9 = LCDC_OVR1CFG9_DMA(0) | LCDC_OVR1CFG9_REP(1) |
			       LCDC_OVR1CFG9_SFACTC(4) | LCDC_OVR1CFG9_SFACTA(1) |
			       LCDC_OVR1CFG9_DFACTC(6) | LCDC_OVR1CFG9_DFACTA(2) |
			       LCDC_OVR1CFG9_A0(255);
	regs->LCDC_OVR1EN = LCDC_OVR1EN_ENABLE_Msk;
	regs->LCDC_ATTRE = LCDC_ATTRE_OVR1_Msk;
	while (regs->LCDC_ATTRS & LCDC_ATTRS_SIP_Msk) {
	}

	/* HEO: minimal config, enable, commit */
	regs->LCDC_HEOCFG0 = LCDC_HEOCFG0_BLEN(4) | LCDC_HEOCFG0_BLENCC(4);
	regs->LCDC_HEOCFG12 = LCDC_HEOCFG12_DMA(0) | LCDC_HEOCFG12_REP(1) |
			       LCDC_HEOCFG12_SFACTC(4) | LCDC_HEOCFG12_SFACTA(1) |
			       LCDC_HEOCFG12_DFACTC(6) | LCDC_HEOCFG12_DFACTA(2) |
			       LCDC_HEOCFG12_A0(255);
	regs->LCDC_HEO[0].LCDC_HEOYFBA = 0;
	regs->LCDC_HEO[0].LCDC_HEOCBFBA = 0;
	regs->LCDC_HEO[0].LCDC_HEOCRFBA = 0;
	regs->LCDC_HEO[1].LCDC_HEOYFBA = 0;
	regs->LCDC_HEO[1].LCDC_HEOCBFBA = 0;
	regs->LCDC_HEO[1].LCDC_HEOCRFBA = 0;
	regs->LCDC_HEOEN = LCDC_HEOEN_ENABLE_Msk;
	regs->LCDC_ATTRE = LCDC_ATTRE_HEO_Msk;
	while (regs->LCDC_ATTRS & LCDC_ATTRS_SIP_Msk) {
	}

#if defined(LCDC_OVR2CFG0_BLEN_Pos)
	if (has_ovr2) {
		regs->LCDC_OVR2CFG0 = LCDC_OVR2CFG0_BLEN(4);
		regs->LCDC_OVR2CFG9 = LCDC_OVR2CFG9_DMA(0) | LCDC_OVR2CFG9_REP(1) |
				       LCDC_OVR2CFG9_SFACTC(4) | LCDC_OVR2CFG9_SFACTA(1) |
				       LCDC_OVR2CFG9_DFACTC(6) | LCDC_OVR2CFG9_DFACTA(2) |
				       LCDC_OVR2CFG9_A0(255);
		regs->LCDC_OVR2EN = LCDC_OVR2EN_ENABLE_Msk;
		regs->LCDC_ATTRE = LCDC_ATTRE_OVR2_Msk;
		while (regs->LCDC_ATTRS & LCDC_ATTRS_SIP_Msk) {
		}
	}
#endif

	LOG_INF("All layers pre-enabled (DMA off)");
}

static void xlcdc_start(lcdc_registers_t *regs)
{
	LOG_INF("Starting XLCDC...");

	regs->LCDC_LCDEN = LCDC_LCDEN_CLKEN_Msk;
	xlcdc_wait_sipsts(regs);
	while (!(regs->LCDC_LCDSR & LCDC_LCDSR_CLKSTS_Msk)) {
	}

	regs->LCDC_LCDEN = LCDC_LCDEN_SYNCEN_Msk;
	xlcdc_wait_sipsts(regs);
	while (!(regs->LCDC_LCDSR & LCDC_LCDSR_LCDSTS_Msk)) {
	}

	regs->LCDC_LCDEN = LCDC_LCDEN_DISPEN_Msk;
	xlcdc_wait_sipsts(regs);
	while (!(regs->LCDC_LCDSR & LCDC_LCDSR_DISPSTS_Msk)) {
	}

	regs->LCDC_LCDEN = LCDC_LCDEN_SDEN_Msk;
	xlcdc_wait_sipsts(regs);
	while (regs->LCDC_LCDSR & LCDC_LCDSR_SDSTS_Msk) {
	}

	LOG_INF("XLCDC started");
}

/* ========================================================================= */
/*                       Parent Initialization                               */
/* ========================================================================= */

static int microchip_sam_xlcdc_init(const struct device *dev)
{
	const struct xlcdc_parent_config *cfg = dev->config;
	struct xlcdc_parent_data *data = dev->data;

	LOG_INF("Initializing Microchip SAM XLCDC parent driver");
	LOG_INF("  Regs: %p, Resolution: %ux%u, Pixel format: %u",
		cfg->regs, cfg->width, cfg->height, cfg->pixel_format);
	LOG_INF("  OVR2 support: %s", cfg->has_ovr2 ? "yes" : "no");

	data->brightness = cfg->pwm_brightness;

	k_mutex_init(&data->attre_lock);

	if (cfg->bridge != NULL) {
		if (!device_is_ready(cfg->bridge)) {
			LOG_ERR("Bridge device not ready");
			return -ENODEV;
		}
		LOG_INF("Bridge device %s ready", cfg->bridge->name);
	}

	xlcdc_enable_clocks(cfg);
	xlcdc_setup_timing_engine(cfg);
	xlcdc_pre_enable_layers(cfg->regs, cfg->has_ovr2);
	xlcdc_start(cfg->regs);

	/* Setup VSYNC (SOF) interrupt if configured in DT */
	if (cfg->irq_config != NULL) {
		cfg->irq_config();
		cfg->regs->LCDC_LCDIER = LCDC_LCDIER_SOFIE_Msk;
		LOG_INF("VSYNC (SOF) interrupt enabled");
	}

	LOG_INF("XLCDC parent initialization complete");
	return 0;
}

/* ========================================================================= */
/*                       Device Instantiation                                */
/* ========================================================================= */

/* Generate per-instance IRQ config function when interrupts are defined */
#define XLCDC_IRQ_CONFIG_FUNC(inst) \
	static void xlcdc_irq_config_##inst(void) \
	{ \
		IRQ_CONNECT(DT_INST_IRQN(inst), \
			    DT_INST_IRQ(inst, priority), \
			    xlcdc_isr, \
			    DEVICE_DT_INST_GET(inst), 0); \
		irq_enable(DT_INST_IRQN(inst)); \
	}

#define XLCDC_IRQ_CONFIG_INIT(inst) \
	IF_ENABLED(DT_INST_IRQ_HAS_IDX(inst, 0), \
		   (XLCDC_IRQ_CONFIG_FUNC(inst)))

#define XLCDC_IRQ_CONFIG_PTR(inst) \
	COND_CODE_1(DT_INST_IRQ_HAS_IDX(inst, 0), \
		    (xlcdc_irq_config_##inst), (NULL))

#define XLCDC_DEFINE_CONFIG(inst, _has_ovr2) \
	XLCDC_IRQ_CONFIG_INIT(inst) \
	static const struct xlcdc_parent_config xlcdc_config_##inst = { \
		.regs = (lcdc_registers_t *)DT_INST_REG_ADDR(inst), \
		.width = DT_INST_PROP(inst, width), \
		.height = DT_INST_PROP(inst, height), \
		.pclk = DT_PROP(DT_CHILD(DT_DRV_INST(inst), \
				display_timings), clock_frequency), \
		.hspw = DT_PROP(DT_CHILD(DT_DRV_INST(inst), \
				display_timings), hsync_len), \
		.hfpw = DT_PROP(DT_CHILD(DT_DRV_INST(inst), \
				display_timings), hfront_porch), \
		.hbpw = DT_PROP(DT_CHILD(DT_DRV_INST(inst), \
				display_timings), hback_porch), \
		.vspw = DT_PROP(DT_CHILD(DT_DRV_INST(inst), \
				display_timings), vsync_len), \
		.vfpw = DT_PROP(DT_CHILD(DT_DRV_INST(inst), \
				display_timings), vfront_porch), \
		.vbpw = DT_PROP(DT_CHILD(DT_DRV_INST(inst), \
				display_timings), vback_porch), \
		.hsync_active = DT_PROP(DT_CHILD(DT_DRV_INST(inst), \
					display_timings), hsync_active), \
		.vsync_active = DT_PROP(DT_CHILD(DT_DRV_INST(inst), \
					display_timings), vsync_active), \
		.de_active = DT_PROP(DT_CHILD(DT_DRV_INST(inst), \
				     display_timings), de_active), \
		.clk_active = DT_PROP(DT_CHILD(DT_DRV_INST(inst), \
				      display_timings), pixelclk_active), \
		.output_mode = DT_INST_ENUM_IDX(inst, output_mode), \
		.guard_time = DT_INST_PROP_OR(inst, guard_time, 30), \
		.pwm_source = DT_INST_ENUM_IDX_OR(inst, \
				backlight_pwm_source, 0), \
		.pwm_prescaler = DT_INST_PROP_OR(inst, \
				backlight_pwm_prescaler, 6), \
		.pwm_brightness = DT_INST_PROP_OR(inst, \
				backlight_brightness, 255), \
		.pixel_format = DT_INST_PROP(inst, pixel_format), \
		.has_ovr2 = _has_ovr2, \
		.bridge = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, port), \
			(DEVICE_DT_GET(DT_INST_PHANDLE(inst, port))), \
			(NULL)), \
		.irq_config = XLCDC_IRQ_CONFIG_PTR(inst), \
	}; \
	static struct xlcdc_parent_data xlcdc_data_##inst; \
	DEVICE_DT_INST_DEFINE(inst, \
			      microchip_sam_xlcdc_init, \
			      NULL, \
			      &xlcdc_data_##inst, \
			      &xlcdc_config_##inst, \
			      POST_KERNEL, \
			      CONFIG_DISPLAY_INIT_PRIORITY, \
			      NULL);

#define MICROCHIP_SAM_XLCDC_INIT(inst) XLCDC_DEFINE_CONFIG(inst, true)

DT_INST_FOREACH_STATUS_OKAY(MICROCHIP_SAM_XLCDC_INIT)

/* SAMA7D65 variant: 3 layers, no OVR2 */
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT microchip_sama7d65_xlcdc

#define MICROCHIP_SAMA7D65_XLCDC_INIT(inst) XLCDC_DEFINE_CONFIG(inst, false)

DT_INST_FOREACH_STATUS_OKAY(MICROCHIP_SAMA7D65_XLCDC_INIT)
