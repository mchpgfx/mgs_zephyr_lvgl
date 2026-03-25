/*
 * Copyright (c) 2026 Microchip Technology Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * XLCDC parent device types and helpers.
 *
 * The parent device manages shared hardware resources (clocks, timing engine,
 * LVDSPLL, start/stop, backlight). Each layer child device registers as its
 * own display device implementing display_driver_api.
 *
 * Register access uses the HAL-provided lcdc_registers_t struct and
 * LCDC_* / LVDSC_* bitfield macros from the Microchip device pack.
 * No register offsets or bitfields are duplicated here.
 */

#ifndef MICROCHIP_SAM_XLCDC_H_
#define MICROCHIP_SAM_XLCDC_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>

#define XLCDC_MAX_VSYNC_CBS 4U

/* SIP (System In Progress) timeout in microseconds */
#define XLCDC_SIP_TIMEOUT_US 1000U

/* HEO scaler fixed-point format: 1 integer + 19 fractional bits */
#define XLCDC_SCALE_FRAC_BITS 20U
#define XLCDC_SCALE_UNITY     (1U << XLCDC_SCALE_FRAC_BITS)

/*
 * RGBA_8888 (R[31:24] G[23:16] B[15:8] A[7:0]) is supported by the XLCDC
 * hardware (RGBMODE=13) but not defined in Zephyr's standard pixel formats.
 * Define a private format using the Zephyr-provided extension range.
 */
#define PIXEL_FORMAT_RGBA_8888 PIXEL_FORMAT_PRIV_START

/* ========================================================================= */
/*                       Enumerations                                        */
/* ========================================================================= */

/* Layer types */
enum xlcdc_layer_type {
	XLCDC_LAYER_BASE = 0,
	XLCDC_LAYER_OVR1,
	XLCDC_LAYER_HEO,
	XLCDC_LAYER_OVR2,
	XLCDC_LAYER_COUNT,
};

/* RGB color modes (RGBMODE field values) */
enum xlcdc_rgb_mode {
	XLCDC_RGB_444          = 0,
	XLCDC_ARGB_4444        = 1,
	XLCDC_RGBA_4444        = 2,
	XLCDC_RGB_565          = 3,
	XLCDC_ARGB_1555        = 4,
	XLCDC_RGB_666          = 5,
	XLCDC_RGB_666_PACKED   = 6,
	XLCDC_ARGB_1666        = 7,
	XLCDC_ARGB_1666_PACKED = 8,
	XLCDC_RGB_888          = 9,
	XLCDC_RGB_888_PACKED   = 10,
	XLCDC_ARGB_1888        = 11,
	XLCDC_ARGB_8888        = 12,
	XLCDC_RGBA_8888        = 13,
};

/* Output interface mode */
enum xlcdc_output_mode {
	XLCDC_OUTPUT_DPI_24     = 0,
	XLCDC_OUTPUT_DPI_18     = 1,
	XLCDC_OUTPUT_DPI_16     = 2,
	XLCDC_OUTPUT_LEGACY_24  = 3,
	XLCDC_OUTPUT_LEGACY_18  = 4,
	XLCDC_OUTPUT_LEGACY_16  = 5,
	XLCDC_OUTPUT_LEGACY_12  = 6,
};

/* ========================================================================= */
/*                       Parent structures                                   */
/* ========================================================================= */

struct xlcdc_parent_config {
	lcdc_registers_t *regs;
	uint16_t width;
	uint16_t height;
	uint32_t pclk;

	/* Timing */
	uint16_t hspw;
	uint16_t hfpw;
	uint16_t hbpw;
	uint16_t vspw;
	uint16_t vfpw;
	uint16_t vbpw;

	/* Polarity */
	uint8_t  hsync_active;
	uint8_t  vsync_active;
	uint8_t  de_active;
	uint8_t  clk_active;

	/* Output */
	uint8_t  output_mode;
	uint8_t  guard_time;

	/* Backlight */
	uint8_t  pwm_source;
	uint8_t  pwm_prescaler;
	uint8_t  pwm_brightness;

	/* Default pixel format for layers */
	uint8_t  pixel_format;

	/* SoC variant */
	bool     has_ovr2;

	/* Bridge device (optional) */
	const struct device *bridge;

	/* IRQ configuration function (NULL if no interrupts) */
	void (*irq_config)(void);
};

struct xlcdc_vsync_cb {
	display_event_cb_t cb;
	void *user_data;
	const struct device *dev;  /* layer device that registered */
};

struct xlcdc_parent_data {
	uint8_t       brightness;
	struct k_mutex attre_lock;
	struct xlcdc_vsync_cb vsync_cbs[XLCDC_MAX_VSYNC_CBS];
	uint8_t       vsync_cb_count;
};

/* ========================================================================= */
/*                       Parent helper API (used by layer driver)            */
/* ========================================================================= */

/**
 * @brief Commit attribute changes for a layer atomically.
 *
 * Acquires the ATTRE mutex, waits for any pending SIP, writes the ATTRE
 * mask, waits for completion, then releases the mutex.
 */
void xlcdc_parent_commit_attrs(const struct device *parent, uint32_t attre_mask);

/** Enable PWM backlight (called by base layer blanking_off). */
void xlcdc_parent_backlight_enable(const struct device *parent);

/** Disable PWM backlight (called by base layer blanking_on). */
void xlcdc_parent_backlight_disable(const struct device *parent);

/** Update PWM brightness value (forwarded from layer set_brightness). */
int xlcdc_parent_set_brightness(const struct device *parent, uint8_t brightness);

/** Wait for SIP status to clear (bounded timeout). Returns 0 or -ETIMEDOUT. */
int xlcdc_wait_sipsts(lcdc_registers_t *regs);

/** Return bytes per pixel for a given RGBMODE value. */
uint8_t xlcdc_bpp(uint8_t rgbmode);

/** Return ATTRE mask bit for a given layer type. */
uint32_t xlcdc_layer_attre_mask(enum xlcdc_layer_type type);

/**
 * @brief Register a VSYNC callback on the parent device.
 *
 * @param parent     Parent XLCDC device.
 * @param layer_dev  Layer device to pass to the callback.
 * @param cb         Callback function.
 * @param user_data  User data passed to callback.
 * @param out_handle Returned handle for unregistration (1-indexed).
 * @return 0 on success, -ENOSPC if all slots are full.
 */
int xlcdc_parent_register_vsync(const struct device *parent,
				const struct device *layer_dev,
				display_event_cb_t cb, void *user_data,
				uint32_t *out_handle);

/**
 * @brief Unregister a VSYNC callback.
 *
 * @param parent  Parent XLCDC device.
 * @param handle  Handle returned from registration.
 * @return 0 on success, -EINVAL if handle is invalid.
 */
int xlcdc_parent_unregister_vsync(const struct device *parent, uint32_t handle);

#endif /* MICROCHIP_SAM_XLCDC_H_ */
