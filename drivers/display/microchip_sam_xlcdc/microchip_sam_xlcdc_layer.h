/*
 * Copyright (c) 2026 Microchip Technology Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * XLCDC Layer Vendor Extension API.
 *
 * Provides runtime control of overlay layers (position, size, alpha,
 * enable/disable) and HEO-specific features (scaling, YUV) beyond what
 * the standard display_driver_api offers.
 */

#ifndef MICROCHIP_SAM_XLCDC_LAYER_H_
#define MICROCHIP_SAM_XLCDC_LAYER_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

/* ========================================================================= */
/*                       Overlay control API                                 */
/* ========================================================================= */

/**
 * @brief Set overlay position on screen.
 *
 * @param dev  XLCDC layer device (ovr1, heo, or ovr2).
 * @param x    Horizontal position in pixels.
 * @param y    Vertical position in pixels.
 * @return 0 on success, -ENOTSUP for base layer.
 */
int xlcdc_layer_set_position(const struct device *dev, uint16_t x, uint16_t y);

/**
 * @brief Set overlay window size.
 *
 * @param dev  XLCDC layer device (ovr1, heo, or ovr2).
 * @param w    Window width in pixels.
 * @param h    Window height in pixels.
 * @return 0 on success, -ENOTSUP for base layer.
 */
int xlcdc_layer_set_window_size(const struct device *dev, uint16_t w, uint16_t h);

/**
 * @brief Set global alpha for overlay blending.
 *
 * @param dev    XLCDC layer device (ovr1, heo, or ovr2).
 * @param alpha  Global alpha value (0=transparent, 255=opaque).
 * @return 0 on success, -ENOTSUP for base layer.
 */
int xlcdc_layer_set_alpha(const struct device *dev, uint8_t alpha);

/**
 * @brief Enable or disable layer DMA.
 *
 * Unlike blanking_on/off, this does not affect the backlight.
 * Use for overlay layers that should be shown/hidden independently.
 *
 * @param dev     XLCDC layer device.
 * @param enable  true to enable DMA (show layer), false to disable.
 * @return 0 on success.
 */
int xlcdc_layer_enable(const struct device *dev, bool enable);

/**
 * @brief Swap front and back framebuffers (double-buffering).
 *
 * Toggles the front/back buffer index, updates the hardware FB address
 * to display the new front buffer, and commits via ATTRE. The previously
 * displayed buffer becomes the new back buffer for drawing.
 *
 * Typically called from a VSYNC callback after drawing to the back buffer.
 *
 * @param dev  XLCDC layer device.
 * @return 0 on success, -ENOTSUP if double-buffering is not enabled.
 */
int xlcdc_layer_swap_buffers(const struct device *dev);

/* ========================================================================= */
/*                       HEO surface API                                     */
/* ========================================================================= */

/**
 * @brief HEO RGB surface descriptor for scaled display.
 *
 * The HEO hardware scaler automatically interpolates when img_w/h
 * differs from win_w/h. Both upscaling and downscaling are supported.
 */
struct xlcdc_heo_rgb_surface {
	void    *buf;           /**< RGB framebuffer data */
	uint8_t  pixel_format;  /**< RGBMODE value (e.g. XLCDC_ARGB_8888) */
	uint16_t img_w;         /**< Source image width in pixels */
	uint16_t img_h;         /**< Source image height in pixels */
	uint16_t win_w;         /**< Target window width (scaler output) */
	uint16_t win_h;         /**< Target window height (scaler output) */
};

/**
 * @brief Display an RGB surface on the HEO layer with optional scaling.
 *
 * Configures the HEO scaler if img_w/h != win_w/h, sets the FB address,
 * enables DMA, and commits. The layer position should be set separately
 * via xlcdc_layer_set_position().
 *
 * @param dev  XLCDC HEO layer device.
 * @param s    RGB surface descriptor.
 * @return 0 on success, -ENOTSUP if not HEO layer, -EINVAL on bad params.
 */
int xlcdc_layer_heo_display_rgb(const struct device *dev,
				const struct xlcdc_heo_rgb_surface *s);

/**
 * @brief HEO YCbCr color modes (YCCMODE field values).
 */
enum xlcdc_heo_yuv_mode {
	XLCDC_YCC_AYCBCR_444    = 0,  /**< AYCbCr 4:4:4 packed (VUYA) */
	XLCDC_YCC_YUYV_422      = 1,  /**< YCbCr 4:2:2 packed (YUYV) */
	XLCDC_YCC_UYVY_422      = 2,  /**< YCbCr 4:2:2 packed (UYVY) */
	XLCDC_YCC_YVYU_422      = 3,  /**< YCbCr 4:2:2 packed (YVYU) */
	XLCDC_YCC_422_M3        = 4,  /**< YCbCr 4:2:2 packed mode 3 */
	XLCDC_YCC_422_SP        = 5,  /**< YCbCr 4:2:2 semi-planar */
	XLCDC_YCC_422_PL        = 6,  /**< YCbCr 4:2:2 planar */
	XLCDC_YCC_420_SP        = 7,  /**< YCbCr 4:2:0 semi-planar */
	XLCDC_YCC_420_PL        = 8,  /**< YCbCr 4:2:0 planar */
};

/**
 * @brief HEO YUV surface descriptor for packed formats.
 */
struct xlcdc_heo_yuv_surface {
	enum xlcdc_heo_yuv_mode mode; /**< YCbCr color mode */
	void    *buf;                 /**< Packed YUV data (Y plane for planar) */
	void    *cb_buf;              /**< Cb plane (planar/semi-planar only) */
	void    *cr_buf;              /**< Cr plane (planar only, NULL for SP) */
	uint16_t img_w;               /**< Source image width */
	uint16_t img_h;               /**< Source image height */
	uint16_t win_w;               /**< Target window width */
	uint16_t win_h;               /**< Target window height */
};

/**
 * @brief Display a YCbCr surface on the HEO layer with CSC and optional scaling.
 *
 * Configures HEOCFG1 for YCbCr mode, loads BT.709 CSC coefficients,
 * sets plane addresses, configures scaler, enables DMA, and commits.
 *
 * @param dev  XLCDC HEO layer device.
 * @param s    YUV surface descriptor.
 * @return 0 on success, -ENOTSUP if not HEO layer, -EINVAL on bad params.
 */
int xlcdc_layer_heo_display_yuv(const struct device *dev,
				const struct xlcdc_heo_yuv_surface *s);

#endif /* MICROCHIP_SAM_XLCDC_LAYER_H_ */
