/*
 * Copyright (c) 2026 Microchip Technology Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * LVGL 9.x draw unit backed by the Verisilicon nano2D GC520UL 2D GPU.
 * Accelerates FILL and IMAGE draw tasks; everything else falls back to the
 * software renderer.
 *
 * Register from application code AFTER lv_init() has run (the Zephyr LVGL
 * subsystem runs it at kernel init). lv_draw_create_unit() prepends to the
 * unit list, so a unit registered post-init is evaluated before the SW unit
 * and its preference wins — no edits to the LVGL module are required.
 */

#ifndef LV_DRAW_NANO2D_H_
#define LV_DRAW_NANO2D_H_

#ifdef __cplusplus
extern "C" {
#endif

/* 0 = blocking n2d_commit() per batch (default).
 * 1 = non-blocking kick via n2d_commit_ex() + n2d_wait() reclaim, allowing
 *     the SW draw unit to render independent tasks while the GPU executes.
 *     Define before including this header to override. */
#ifndef NANO2D_ASYNC_COMMIT
#define NANO2D_ASYNC_COMMIT 0
#endif

/** Create and register the nano2D draw unit. Call once, after lv_init(). */
void lv_draw_nano2d_init(void);

#ifdef __cplusplus
}
#endif

#endif /* LV_DRAW_NANO2D_H_ */
