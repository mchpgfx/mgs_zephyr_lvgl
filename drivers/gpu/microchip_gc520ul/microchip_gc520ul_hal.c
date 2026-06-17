/*
 * Copyright (c) 2026 Microchip Technology Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * nano2D HAL implementation for Zephyr (SAMA7D65). Implements the 12 functions
 * declared in nano2D_hal.h on top of Zephyr kernel primitives: k_timer for the
 * GPU watchdog timers, k_busy_wait/k_msleep for delays, k_uptime for ticks,
 * k_sem for completion signals, and the PMC for GPU power/clock gating.
 */

#include <soc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>

#include "nano2D.h"
#include "nano2D_hal.h"

/* ---- Timers (GPU stuck/recovery watchdog) ---- */

struct n2d_zephyr_timer {
	struct k_timer timer;
	n2d_timer_func func;
	n2d_pointer    data;
};

static void n2d_timer_expiry(struct k_timer *t)
{
	struct n2d_zephyr_timer *w =
		CONTAINER_OF(t, struct n2d_zephyr_timer, timer);

	if (w->func != NULL) {
		w->func(w->data);
	}
}

n2d_error_t n2d_hal_create_timer(n2d_timer_func function, n2d_pointer data,
				 n2d_pointer *timer)
{
	struct n2d_zephyr_timer *w;

	if (timer == NULL) {
		return N2D_INVALID_ARGUMENT;
	}

	w = k_malloc(sizeof(*w));
	if (w == NULL) {
		return N2D_OUT_OF_MEMORY;
	}

	w->func = function;
	w->data = data;
	k_timer_init(&w->timer, n2d_timer_expiry, NULL);

	*timer = (n2d_pointer)w;
	return N2D_SUCCESS;
}

n2d_error_t n2d_hal_destroy_timer(n2d_pointer timer)
{
	struct n2d_zephyr_timer *w = timer;

	if (w == NULL) {
		return N2D_INVALID_ARGUMENT;
	}

	k_timer_stop(&w->timer);
	k_free(w);
	return N2D_SUCCESS;
}

n2d_error_t n2d_hal_start_timer(n2d_pointer timer, uint32_t delay_ms)
{
	struct n2d_zephyr_timer *w = timer;

	if (w == NULL) {
		return N2D_INVALID_ARGUMENT;
	}

	/* One-shot. */
	k_timer_start(&w->timer, K_MSEC(delay_ms), K_NO_WAIT);
	return N2D_SUCCESS;
}

n2d_error_t n2d_hal_stop_timer(n2d_pointer timer)
{
	struct n2d_zephyr_timer *w = timer;

	if (w == NULL) {
		return N2D_INVALID_ARGUMENT;
	}

	k_timer_stop(&w->timer);
	return N2D_SUCCESS;
}

/* ---- Delays ---- */

void n2d_hal_delay_us(n2d_uint_t us)
{
	k_busy_wait(us);
}

void n2d_hal_delay_ms(n2d_uint_t ms)
{
	k_msleep(ms);
}

/* ---- Power / clock ---- */

void n2d_hal_set_gpu_power(n2d_bool_t power)
{
	if (power) {
		/*
		 * Enable both the peripheral clock (EN) and the generic core
		 * clock (GCLKEN). The 2D pipeline runs off GCLK; MCK3 alone only
		 * clocks the register interface (enough to read the chip ID).
		 * GCLK = GPUPLL / (GCLKDIV + 1) = 1064 / 2 = 532 MHz, the
		 * GC520UL's rated max core clock.
		 */
		PMC_REGS->PMC_PCR = PMC_PCR_CMD(1) |
				    PMC_PCR_GCLKEN(1) |
				    PMC_PCR_GCLKCSS(PMC_PCR_GCLKCSS_GPUPLL_Val) |
				    PMC_PCR_GCLKDIV(1) |
				    PMC_PCR_EN(1) |
				    PMC_PCR_PID(ID_GPU2DC);
	} else {
		PMC_REGS->PMC_PCR = PMC_PCR_CMD(1) |
				    PMC_PCR_PID(ID_GPU2DC);
	}
}

/* ---- System ticks (milliseconds) ---- */

n2d_uint64_t n2d_hal_get_ticks(void)
{
	return (n2d_uint64_t)k_uptime_get();
}

/* ---- Signals (GPU completion) ---- */

n2d_error_t n2d_hal_signal_create(n2d_pointer **signal, n2d_bool_t manual_reset)
{
	struct k_sem *sem;

	/* nano2D models these as Windows-style auto-reset events (it always
	 * creates them with manual_reset == N2D_FALSE): "set" is idempotent and
	 * "wait" consumes-and-resets. A BINARY semaphore (max count 1) matches
	 * that exactly. A counting semaphore would let GPU-completion "gives"
	 * accumulate, so a later n2d_commit() wait could return on a stale count
	 * before the GPU finished that batch -> intermittent missing/torn draws.
	 */
	ARG_UNUSED(manual_reset);

	if (signal == NULL) {
		return N2D_INVALID_ARGUMENT;
	}

	sem = k_malloc(sizeof(*sem));
	if (sem == NULL) {
		return N2D_OUT_OF_MEMORY;
	}

	k_sem_init(sem, 0, 1);
	*signal = (n2d_pointer *)sem;
	return N2D_SUCCESS;
}

n2d_error_t n2d_hal_signal_destroy(n2d_pointer signal)
{
	if (signal == NULL) {
		return N2D_INVALID_ARGUMENT;
	}

	k_free(signal);
	return N2D_SUCCESS;
}

n2d_error_t n2d_hal_signal_wait(n2d_pointer signal, uint32_t timeout_ms)
{
	struct k_sem *sem = signal;
	k_timeout_t timeout;

	if (sem == NULL) {
		return N2D_INVALID_ARGUMENT;
	}

	if (timeout_ms == 0U) {
		timeout = K_NO_WAIT;
	} else if (timeout_ms == UINT32_MAX) {
		timeout = K_FOREVER;
	} else {
		timeout = K_MSEC(timeout_ms);
	}

	if (k_sem_take(sem, timeout) != 0) {
		return N2D_TIMEOUT;
	}

	return N2D_SUCCESS;
}

n2d_error_t n2d_hal_signal_set(n2d_pointer signal, n2d_bool_t state)
{
	struct k_sem *sem = signal;

	if (sem == NULL) {
		return N2D_INVALID_ARGUMENT;
	}

	if (state) {
		k_sem_give(sem);  /* ISR-safe */
	} else {
		k_sem_reset(sem);
	}

	return N2D_SUCCESS;
}

/* ---- GPU working heap (nano2D allocator hooks) ----
 *
 * nano2D allocates its device context and per-commit command buffers through
 * these hooks, backed by a Zephyr k_heap over the region the GPU driver hands
 * to n2d_init() (microchip_gc520ul.c: gpu_heap, which is __nocache). Because
 * that region is non-cached there is no CPU/GPU cache to maintain for these
 * allocations - which is why this HAL provides no cache clean/invalidate/flush
 * hooks, only the ordering barrier below.
 *
 * k_heap_alloc/free are internally locked, so these are safe from any thread.
 * nano2D never allocates from interrupt context (its ISR path only frees), so
 * K_NO_WAIT is correct and never blocks.
 */
static struct k_heap n2d_heap;

n2d_error_t n2d_hal_heap_init(n2d_pointer base, n2d_uint32_t size)
{
	if (base == NULL || size == 0U) {
		return N2D_INVALID_ARGUMENT;
	}

	k_heap_init(&n2d_heap, base, size);
	return N2D_SUCCESS;
}

n2d_pointer n2d_hal_allocate(n2d_uint32_t size)
{
	return k_heap_alloc(&n2d_heap, size, K_NO_WAIT); /* NULL when exhausted */
}

void n2d_hal_free(n2d_pointer ptr)
{
	if (ptr != NULL) {
		k_heap_free(&n2d_heap, ptr);
	}
}

/* ---- Memory barrier ----
 *
 * Orders the CPU's writes to GPU command memory before the register write that
 * starts the GPU. Required even though that memory is non-cached: the fence
 * guarantees the stores have drained (are globally observable) before the GPU's
 * AXI master fetches them.
 */
void n2d_hal_memory_barrier(void)
{
	barrier_dmem_fence_full();
}
