/*
 * Copyright (c) 2026 Microchip Technology Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr device driver for the Verisilicon GC520UL 2D GPU (GPU2DC) on the
 * SAMA7D65. Thin wrapper around the nano2D library: enables the peripheral
 * clock, hands the library an identity-mapped GPU heap, brings the core up via
 * n2d_init()/n2d_open(), and routes the GPU interrupt to n2d_handle_events().
 */

#define DT_DRV_COMPAT microchip_gc520ul

#include <soc.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/sections.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>

#include "nano2D.h"
#include "nano2D_hal.h"

LOG_MODULE_REGISTER(gc520ul, CONFIG_GPU_MICROCHIP_GC520UL_LOG_LEVEL);

/* Chip identification register (AQ chip id), relative to the register base. */
#define GC520UL_REG_CHIP_ID 0x0020U

struct gc520ul_config {
	mem_addr_t reg_base;
	void (*irq_config)(void);
};

struct gc520ul_data {
	void *heap;
};

/*
 * GPU working heap for the nano2D umm allocator (command buffers + small GPU
 * context structures). MUST be non-cached.
 *
 * The umm allocator hands out 32-byte blocks - finer than the 64-byte cache
 * line - so two unrelated nano2D allocations routinely share a single cache
 * line. In *cacheable* memory that makes nano2D's per-allocation clean/
 * invalidate cache ops race on the shared line: a clean can write a neighbour
 * allocation's stale CPU bytes back over a GPU-written line, or an invalidate
 * can drop a neighbour's still-dirty line. Either way the GPU reads a corrupted
 * command and silently drops the op -> intermittent missing fills/blits that
 * scale with commit count. A non-cached heap needs no cache maintenance at all,
 * which removes the hazard entirely (this is why the baremetal port placed the
 * heap in a .region_nocache section). Caller-provided pixel buffers - the LVGL
 * VDB and source images - stay cacheable and are maintained explicitly by the
 * draw unit; only this internal heap is non-cached.
 */
static uint8_t gpu_heap[CONFIG_GPU_MICROCHIP_GC520UL_HEAP_SIZE]
	__nocache __aligned(64);

static void gc520ul_isr(const struct device *dev)
{
	ARG_UNUSED(dev);

	/* Acknowledge the GPU interrupt and notify any waiting event/signal. */
	(void)n2d_handle_events();
}

static int gc520ul_init(const struct device *dev)
{
	const struct gc520ul_config *cfg = dev->config;
	struct gc520ul_data *data = dev->data;
	n2d_error_t err;

	LOG_INF("GC520UL init: enabling clocks");

	/* Enable the GPU2DC peripheral + generic core clock before register access. */
	n2d_hal_set_gpu_power(N2D_TRUE);

	/* Core clock needs a moment to settle before the first register access. */
	k_busy_wait(100);

	/* Read chip ID directly first - proves the core is clocked/responding. */
	uint32_t chip_id = sys_read32(cfg->reg_base + GC520UL_REG_CHIP_ID);

	LOG_INF("GC520UL chip id 0x%08x", chip_id);

	/*
	 * Hand nano2D the static non-cached heap (see gpu_heap above). It is a
	 * single contiguous, identity-mapped (phys==virt) DDR region; the MMU
	 * nocache mapping only changes its cacheability attribute, so the GPU's
	 * gpu==(uintptr_t)ptr identity assumption still holds.
	 */
	data->heap = gpu_heap;

	LOG_INF("GC520UL init: n2d_init (heap %u @ %p, non-cached)",
		(unsigned)sizeof(gpu_heap), data->heap);

	err = n2d_init(data->heap, sizeof(gpu_heap));
	if (err != N2D_SUCCESS) {
		LOG_ERR("n2d_init failed: 0x%x", err);
		return -EIO;
	}

	LOG_INF("GC520UL init: n2d_open");

	err = n2d_open();
	if (err != N2D_SUCCESS) {
		LOG_ERR("n2d_open failed: 0x%x", err);
		return -EIO;
	}

	LOG_INF("GC520UL 2D GPU ready");

	if (cfg->irq_config != NULL) {
		cfg->irq_config();
	}

	return 0;
}

static void gc520ul_irq_config(void)
{
	IRQ_CONNECT(DT_INST_IRQN(0), DT_INST_IRQ(0, priority),
		    gc520ul_isr, DEVICE_DT_INST_GET(0), 0);
	irq_enable(DT_INST_IRQN(0));
}

static const struct gc520ul_config gc520ul_cfg = {
	.reg_base = DT_INST_REG_ADDR(0),
	.irq_config = gc520ul_irq_config,
};

static struct gc520ul_data gc520ul_data;

DEVICE_DT_INST_DEFINE(0, gc520ul_init, NULL,
		      &gc520ul_data, &gc520ul_cfg,
		      POST_KERNEL, CONFIG_GPU_MICROCHIP_GC520UL_INIT_PRIORITY,
		      NULL);
