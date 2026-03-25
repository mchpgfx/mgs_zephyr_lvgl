/*
 * Copyright (c) 2026 Microchip Technology Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Microchip SAM LVDSC (LVDS Serializer Controller) Bridge Driver.
 * Uses HAL-provided lvdsc_registers_t and LVDSC_* bitfield macros.
 */

#define DT_DRV_COMPAT microchip_sam_lvdsc

#include <string.h>

#include <soc.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(microchip_sam_lvdsc, CONFIG_DISPLAY_LOG_LEVEL);

struct lvdsc_config {
	lvdsc_registers_t *regs;
	uint8_t  dpi_input;      /* 0=24-bit, 1=18-bit */
	uint8_t  dc_mode;        /* 0=unbalanced, 1=balanced */
	uint8_t  mapping;        /* 0=JEIDA, 1=VESA */
	uint8_t  lane_a3_bit;
	uint8_t  dc_bias;
	uint8_t  pre_emphasis_clk;
	uint8_t  pre_emphasis_a0;
	uint8_t  pre_emphasis_a1;
	uint8_t  pre_emphasis_a2;
	uint8_t  pre_emphasis_a3;
};

static int microchip_sam_lvdsc_init(const struct device *dev)
{
	const struct lvdsc_config *cfg = dev->config;
	lvdsc_registers_t *regs = cfg->regs;

	LOG_INF("Initializing LVDSC bridge at %p", regs);
	LOG_INF("  DPI input: %s, DC mode: %s, Mapping: %s",
		cfg->dpi_input ? "18-bit" : "24-bit",
		cfg->dc_mode ? "balanced" : "unbalanced",
		cfg->mapping ? "VESA" : "JEIDA");

	/* Enable LVDSC peripheral clock via PMC */
	PMC_REGS->PMC_PCR = PMC_PCR_CMD_Msk |
			    PMC_PCR_PID(ID_LVDSC) |
			    PMC_PCR_EN_Msk;

	/* Disable LVDS Serializer */
	regs->LVDSC_CR = 0;
	while (regs->LVDSC_SR & LVDSC_SR_CS_Msk) {
	}

	/* Configuration register */
	uint32_t cfgr = LVDSC_CFGR_LCDC_PIXSIZE(cfg->dpi_input) |
			LVDSC_CFGR_DC_BAL(cfg->dc_mode);

	if (cfg->dc_mode == 0) { /* unbalanced */
		cfgr |= LVDSC_CFGR_MAPPING(cfg->mapping);
	}

	regs->LVDSC_CFGR = cfgr;

	/* Unbalanced control bits */
	regs->LVDSC_UCBR = LVDSC_UCBR_RESA3(cfg->lane_a3_bit);

	/* Analog control */
	regs->LVDSC_ACR = LVDSC_ACR_DCBIAS(cfg->dc_bias) |
			  LVDSC_ACR_PREEMP_A0(cfg->pre_emphasis_a0) |
			  LVDSC_ACR_PREEMP_A1(cfg->pre_emphasis_a1) |
			  LVDSC_ACR_PREEMP_A2(cfg->pre_emphasis_a2) |
			  LVDSC_ACR_PREEMP_A3(cfg->pre_emphasis_a3) |
			  LVDSC_ACR_PREEMP_CLK1(cfg->pre_emphasis_clk);

	/* Enable LVDS Serializer */
	regs->LVDSC_CR = LVDSC_CR_SER_EN_Msk;

	LOG_INF("LVDSC bridge initialized");
	return 0;
}

/*
 * DT enum index mapping (compile-time):
 *   dpi-input-type: "24-bit"=0, "18-bit"=1
 *   dc-mode:        "unbalanced"=0, "balanced"=1
 *   mapping:        "jeida"=0, "vesa"=1
 */

#define MICROCHIP_SAM_LVDSC_INIT(inst)                                         \
                                                                               \
	static const struct lvdsc_config lvdsc_config_##inst = {               \
		.regs = (lvdsc_registers_t *)DT_INST_REG_ADDR(inst),          \
		.dpi_input = DT_INST_ENUM_IDX(inst, dpi_input_type),          \
		.dc_mode = DT_INST_ENUM_IDX_OR(inst, dc_mode, 0),             \
		.mapping = DT_INST_ENUM_IDX_OR(inst, mapping, 0),             \
		.lane_a3_bit = DT_INST_PROP_OR(inst, lane_a3_bit, 0),         \
		.dc_bias = DT_INST_PROP_OR(inst, dc_bias, 9),                 \
		.pre_emphasis_clk = DT_INST_PROP_OR(inst, pre_emphasis_clk, 0),\
		.pre_emphasis_a0 = DT_INST_PROP_OR(inst, pre_emphasis_a0, 0), \
		.pre_emphasis_a1 = DT_INST_PROP_OR(inst, pre_emphasis_a1, 0), \
		.pre_emphasis_a2 = DT_INST_PROP_OR(inst, pre_emphasis_a2, 0), \
		.pre_emphasis_a3 = DT_INST_PROP_OR(inst, pre_emphasis_a3, 0), \
	};                                                                     \
                                                                               \
	DEVICE_DT_INST_DEFINE(inst,                                            \
			      microchip_sam_lvdsc_init,                         \
			      NULL,                                            \
			      NULL,                                            \
			      &lvdsc_config_##inst,                             \
			      POST_KERNEL,                                     \
			      CONFIG_MICROCHIP_SAM_LVDSC_INIT_PRIORITY,        \
			      NULL);

DT_INST_FOREACH_STATUS_OKAY(MICROCHIP_SAM_LVDSC_INIT)
