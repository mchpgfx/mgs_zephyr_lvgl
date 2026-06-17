/*
 * Copyright (c) 2026 Microchip Technology Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * LCDC POC — LVGL Benchmark Demo
 */

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

/* -----------------------------------------------------------------------
 * Benchmark knobs — edit these to switch modes, then rebuild.
 * ----------------------------------------------------------------------- */

/* 1 = register the GPU (GC520) draw unit before the benchmark runs.
 * 0 = pure software renderer — use this for the SW baseline comparison. */
#define USE_NANO2D_DRAW_UNIT 1

/* 1 = kick the GPU non-blocking (n2d_commit_ex) and reclaim via n2d_wait()
 *     so the SW unit can render independent tasks during GPU execution.
 *     Measured flat on this full-refresh benchmark (no independent SW work
 *     to overlap); kept for threaded-SW or GPU-heavy workloads.
 * 0 = blocking n2d_commit() per batch (default, proven path). */
#define NANO2D_ASYNC_COMMIT 0

/* 1 = run the GPU-vs-CPU per-op microbenchmark at boot (prints a CSV table
 *     over UART, then halts). GPU is initialised by the driver regardless.
 *     See src/microbench.c and benchmark_results.md for results.
 * 0 = run the normal LVGL benchmark demo (default). */
#define RUN_MICROBENCH 0

/* ----------------------------------------------------------------------- */

#include "draw_nano2d/lv_draw_nano2d.h"
#include "microbench.h"

/* LVGL benchmark demo — declared here to avoid include path issues */
void lv_demo_benchmark(void);

LOG_MODULE_REGISTER(main_app, LOG_LEVEL_INF);

static const struct gpio_dt_spec lvds_pwm =
	GPIO_DT_SPEC_GET(DT_NODELABEL(gpio_lvds_pwm), gpios);

int main(void)
{
	int ret;

	if (!gpio_is_ready_dt(&lvds_pwm)) {
		LOG_ERR("GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&lvds_pwm, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		return ret;
	}

	const struct device *display_dev =
		DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(display_dev)) {
		LOG_ERR("Display device not ready");
		return -ENODEV;
	}

	display_blanking_off(display_dev);

#if USE_NANO2D_DRAW_UNIT
	/* Register the GPU draw unit before the benchmark creates any draw tasks.
	 * lv_init() has already run via the Zephyr LVGL display init by this point. */
	const struct device *gpu_dev = DEVICE_DT_GET(DT_NODELABEL(gpu2dc));

	if (device_is_ready(gpu_dev)) {
		lv_draw_nano2d_init();
	} else {
		LOG_WRN("GPU not ready; LVGL will render on SW only");
	}
#endif

#if RUN_MICROBENCH
	/* GPU-vs-CPU per-op microbenchmark, then halt (skips the demo). The GPU is
	 * already initialised by the driver at boot. */
	microbench_run();
	while (1) {
		k_sleep(K_MSEC(1000));
	}
#endif

	LOG_INF("Starting LVGL Benchmark");
	lv_demo_benchmark();

	while (1) {
		lv_timer_handler();
		k_sleep(K_MSEC(LV_DEF_REFR_PERIOD));
	}

	return 0;
}
