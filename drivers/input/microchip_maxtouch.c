/*
 * Copyright (c) 2025 Microchip Technology Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Microchip maXTouch capacitive touchscreen controller driver.
 * Supports multitouch with interrupt-driven and polling modes.
 */

#define DT_DRV_COMPAT microchip_maxtouch

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/input/input_touch.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/byteorder.h>

#include "microchip_maxtouch.h"

LOG_MODULE_REGISTER(maxtouch, CONFIG_INPUT_LOG_LEVEL);

struct mxt_config {
	struct input_touchscreen_common_config common;
	struct i2c_dt_spec i2c;
	struct gpio_dt_spec rst_gpio;
	struct gpio_dt_spec irq_gpio;
	uint8_t max_touches;
	uint16_t startup_delay_ms;
};

struct mxt_data {
	const struct device *dev;
	struct k_work work;
	struct k_mutex i2c_lock;

	/* Resolved I2C address (may differ from DT after probe) */
	uint16_t actual_address;

#ifdef CONFIG_INPUT_MICROCHIP_MAXTOUCH_INTERRUPT
	struct gpio_callback irq_cb;
#else
	struct k_timer poll_timer;
#endif

	/* Object table addresses and report IDs */
	uint16_t t5_address;
	uint8_t t5_msg_size;
	uint16_t t6_address;
	uint8_t t6_reportid;
	uint16_t t7_address;
	uint16_t t18_address;
	uint16_t t44_address;
	uint16_t t100_address;
	uint8_t t100_reportid_min;
	uint8_t t100_reportid_max;
	uint8_t num_touchids;
	uint8_t max_reportid;

	/* Chip info */
	struct mxt_info info;
	uint32_t info_crc;
	uint32_t config_crc;
	uint8_t t6_status;
	struct mxt_t7_config t7_cfg;

	/* Touch state for release detection between poll cycles */
	struct mxt_touch_point prev_touches[CONFIG_INPUT_MICROCHIP_MAXTOUCH_MAX_TOUCHES];
	uint8_t prev_touch_count;

	/* Message buffer for T44+T5 bulk read */
	uint8_t msg_buf[MXT_MAX_MSG_COUNT * MXT_MAX_MSG_SIZE];

#ifdef CONFIG_INPUT_MICROCHIP_MAXTOUCH_INTERRUPT
	bool chg_warned;
#endif
};

INPUT_TOUCH_STRUCT_CHECK(struct mxt_config);

/* I2C helpers - use runtime-resolved address from dual-address probe */

static int mxt_i2c_read_reg(const struct device *dev, uint16_t reg,
			     void *buf, size_t len)
{
	const struct mxt_config *cfg = dev->config;
	struct mxt_data *data = dev->data;
	uint8_t addr[2] = { reg & 0xFF, (reg >> 8) & 0xFF };

	return i2c_write_read(cfg->i2c.bus, data->actual_address,
			      addr, sizeof(addr), buf, len);
}

static int mxt_i2c_write_reg(const struct device *dev, uint16_t reg,
			      const void *buf, size_t len)
{
	const struct mxt_config *cfg = dev->config;
	struct mxt_data *data = dev->data;
	uint8_t hdr[2] = { reg & 0xFF, (reg >> 8) & 0xFF };
	struct i2c_msg msgs[2] = {
		{ .buf = hdr, .len = 2, .flags = I2C_MSG_WRITE },
		{ .buf = (uint8_t *)buf, .len = len,
		  .flags = I2C_MSG_WRITE | I2C_MSG_STOP },
	};

	return i2c_transfer(cfg->i2c.bus, msgs, 2, data->actual_address);
}

static int mxt_i2c_read_reg_locked(const struct device *dev, uint16_t reg,
				    void *buf, size_t len)
{
	struct mxt_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->i2c_lock, K_FOREVER);
	ret = mxt_i2c_read_reg(dev, reg, buf, len);
	k_mutex_unlock(&data->i2c_lock);
	return ret;
}

static int mxt_i2c_write_reg_locked(const struct device *dev, uint16_t reg,
				     const void *buf, size_t len)
{
	struct mxt_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->i2c_lock, K_FOREVER);
	ret = mxt_i2c_write_reg(dev, reg, buf, len);
	k_mutex_unlock(&data->i2c_lock);
	return ret;
}

/* I2C address probe with timeout via system workqueue */

struct mxt_probe_ctx {
	const struct device *bus;
	uint16_t addr;
	int result;
	struct k_work work;
	struct k_sem done;
};

static void mxt_probe_work_fn(struct k_work *work)
{
	struct mxt_probe_ctx *ctx = CONTAINER_OF(work, struct mxt_probe_ctx, work);
	uint8_t reg[2] = { 0, 0 };
	uint8_t dummy;

	ctx->result = i2c_write_read(ctx->bus, ctx->addr,
				     reg, sizeof(reg), &dummy, sizeof(dummy));
	k_sem_give(&ctx->done);
}

static int mxt_probe_address(const struct device *bus, uint16_t addr)
{
	struct mxt_probe_ctx ctx = {
		.bus = bus,
		.addr = addr,
		.result = -ETIMEDOUT,
	};

	k_work_init(&ctx.work, mxt_probe_work_fn);
	k_sem_init(&ctx.done, 0, 1);
	k_work_submit(&ctx.work);

	if (k_sem_take(&ctx.done, K_MSEC(MXT_PROBE_TIMEOUT_MS)) != 0) {
		LOG_WRN("I2C probe timeout at 0x%02X (bus may be stuck)", addr);
		return -ETIMEDOUT;
	}

	return ctx.result;
}

/* CRC-24 over 16-bit LE words, zero-padded for odd lengths */

static void mxt_crc24_step(uint32_t *crc, uint8_t byte0, uint8_t byte1)
{
	uint32_t result;

	result = ((*crc << 1) ^ ((uint32_t)byte1 << 8 | byte0));
	if (result & 0x1000000U) {
		result ^= MXT_CRC_POLY;
	}
	*crc = result;
}

static uint32_t mxt_crc24(const uint8_t *buf, size_t len)
{
	uint32_t crc = 0;
	const uint8_t *ptr = buf;
	const uint8_t *last = buf + len - 1;

	while (ptr < last) {
		mxt_crc24_step(&crc, *ptr, *(ptr + 1));
		ptr += 2;
	}

	if (ptr == last) {
		mxt_crc24_step(&crc, *ptr, 0);
	}

	return crc & 0x00FFFFFFU;
}

/* Object table helpers */

static uint16_t mxt_obj_address(const struct mxt_object *obj)
{
	return (uint16_t)obj->start_pos_lsb | ((uint16_t)obj->start_pos_msb << 8);
}

static uint8_t mxt_obj_size(const struct mxt_object *obj)
{
	return obj->size_minus_one + 1;
}

static uint8_t mxt_obj_instances(const struct mxt_object *obj)
{
	return obj->instances_minus_one + 1;
}

static int mxt_read_info_block(const struct device *dev)
{
	const struct mxt_config *cfg = dev->config;
	struct mxt_data *data = dev->data;
	int ret;

	ret = mxt_i2c_read_reg_locked(dev, 0x0000, &data->info, MXT_INFO_SIZE);
	if (ret < 0) {
		LOG_ERR("Failed to read info block: %d", ret);
		return ret;
	}

	LOG_INF("family=0x%02X variant=0x%02X ver=%u.%u objects=%u matrix=%ux%u",
		data->info.family_id, data->info.variant_id,
		data->info.version >> 4, data->info.version & 0xF,
		data->info.num_objects,
		data->info.matrix_x_size, data->info.matrix_y_size);

	if (data->info.num_objects == 0 || data->info.num_objects > MXT_MAX_OBJECTS) {
		LOG_ERR("Invalid object count: %u", data->info.num_objects);
		return -ENODEV;
	}

	/* Read full info block: header + object table + 3-byte CRC */
	size_t table_size = data->info.num_objects * MXT_OBJECT_SIZE;
	size_t block_size = MXT_INFO_SIZE + table_size + MXT_INFO_CHECKSUM_SIZE;
	uint8_t info_block[MXT_MAX_INFO_SIZE];

	if (block_size > sizeof(info_block)) {
		LOG_ERR("Info block too large: %zu", block_size);
		return -ENOMEM;
	}

	ret = mxt_i2c_read_reg_locked(dev, 0x0000, info_block, block_size);
	if (ret < 0) {
		LOG_ERR("Failed to read full info block: %d", ret);
		return ret;
	}

	/* Validate CRC-24 over header + object table */
	size_t crc_offset = MXT_INFO_SIZE + table_size;
	uint32_t stored_crc = (uint32_t)info_block[crc_offset] |
			      ((uint32_t)info_block[crc_offset + 1] << 8) |
			      ((uint32_t)info_block[crc_offset + 2] << 16);
	uint32_t calc_crc = mxt_crc24(info_block, crc_offset);

	if (stored_crc != calc_crc) {
		LOG_ERR("Info block CRC mismatch: stored=0x%06X calc=0x%06X",
			stored_crc, calc_crc);
		return -EINVAL;
	}

	data->info_crc = stored_crc;
	LOG_INF("Info block CRC: 0x%06X (OK)", stored_crc);

	/* Parse object table and extract addresses / report IDs */
	const struct mxt_object *obj_table =
		(const struct mxt_object *)&info_block[MXT_INFO_SIZE];
	uint8_t reportid = 1;

	data->t5_address = 0;
	data->t6_address = 0;
	data->t7_address = 0;
	data->t18_address = 0;
	data->t44_address = 0;
	data->t100_reportid_min = 0;
	data->t100_reportid_max = 0;

	for (uint8_t i = 0; i < data->info.num_objects; i++) {
		const struct mxt_object *obj = &obj_table[i];
		uint16_t addr = mxt_obj_address(obj);
		uint8_t size = mxt_obj_size(obj);
		uint8_t instances = mxt_obj_instances(obj);
		uint8_t num_ids = obj->num_report_ids;

		LOG_DBG("T%u: addr=0x%04X size=%u instances=%u report_ids=%u",
			obj->type, addr, size, instances, num_ids);

		switch (obj->type) {
		case MXT_GEN_MESSAGE_T5:
			data->t5_address = addr;
			/* Exclude optional trailing CRC byte */
			data->t5_msg_size = size - 1;
			break;
		case MXT_GEN_COMMAND_T6:
			data->t6_address = addr;
			data->t6_reportid = reportid;
			break;
		case MXT_GEN_POWER_T7:
			data->t7_address = addr;
			break;
		case MXT_SPT_COMMSCONFIG_T18:
			data->t18_address = addr;
			break;
		case MXT_SPT_MESSAGECOUNT_T44:
			data->t44_address = addr;
			break;
		case MXT_TOUCH_MULTITOUCHSCREEN_T100:
			data->t100_address = addr;
			data->t100_reportid_min = reportid;
			data->num_touchids = num_ids - 2;
			data->t100_reportid_max = reportid + num_ids - 1;
			break;
		}

		reportid += num_ids * instances;
	}

	data->max_reportid = reportid - 1;

	if (data->t5_address == 0 || data->t6_address == 0 ||
	    data->t44_address == 0) {
		LOG_ERR("Missing required objects (T5/T6/T44)");
		return -ENODEV;
	}

	if (data->t100_reportid_min == 0) {
		LOG_ERR("T100 multitouch object not found");
		return -ENODEV;
	}

	if (data->num_touchids > CONFIG_INPUT_MICROCHIP_MAXTOUCH_MAX_TOUCHES) {
		data->num_touchids = CONFIG_INPUT_MICROCHIP_MAXTOUCH_MAX_TOUCHES;
	}

	if (cfg->max_touches > 0 && cfg->max_touches < data->num_touchids) {
		data->num_touchids = cfg->max_touches;
	}

	LOG_INF("T5=0x%04X(%u) T6=0x%04X T7=0x%04X T44=0x%04X "
		"T100 ids=%u..%u touches=%u",
		data->t5_address, data->t5_msg_size, data->t6_address,
		data->t7_address, data->t44_address,
		data->t100_reportid_min, data->t100_reportid_max,
		data->num_touchids);

	return 0;
}

/* T7 power configuration */

static int mxt_read_t7_config(const struct device *dev)
{
	struct mxt_data *data = dev->data;

	if (data->t7_address == 0) {
		LOG_WRN("T7 not available");
		return 0;
	}

	int ret = mxt_i2c_read_reg_locked(dev, data->t7_address,
					   &data->t7_cfg, sizeof(data->t7_cfg));
	if (ret < 0) {
		LOG_ERR("Failed to read T7: %d", ret);
		return ret;
	}

	LOG_INF("T7 power: active=%ums idle=%ums actv2idle=%u",
		data->t7_cfg.active, data->t7_cfg.idle, data->t7_cfg.actv2idle);

	return 0;
}

/* Set T18 CHG line to Mode 1 (level-triggered) for reliable drain loop */
static int mxt_set_chg_mode1(const struct device *dev)
{
	struct mxt_data *data = dev->data;
	uint8_t ctrl;
	int ret;

	if (data->t18_address == 0) {
		LOG_WRN("T18 not available, CHG mode unchanged");
		return -ENOTSUP;
	}

	ret = mxt_i2c_read_reg_locked(dev, data->t18_address, &ctrl, 1);
	if (ret < 0) {
		LOG_ERR("Failed to read T18: %d", ret);
		return ret;
	}

	if (ctrl & MXT_T18_CTRL_MODE) {
		LOG_INF("T18: CHG already in Mode 1 (level-triggered)");
		return 0;
	}

	ctrl |= MXT_T18_CTRL_MODE;
	ret = mxt_i2c_write_reg_locked(dev, data->t18_address, &ctrl, 1);
	if (ret < 0) {
		LOG_ERR("Failed to write T18: %d", ret);
		return ret;
	}

	LOG_INF("T18: CHG set to Mode 1 (level-triggered)");
	return 0;
}

/* Configure T100 NUMTCH to match available report IDs */
static int mxt_configure_t100(const struct device *dev)
{
	const struct mxt_config *cfg = dev->config;
	struct mxt_data *data = dev->data;
	uint8_t numtch;
	uint8_t target;
	int ret;

	if (data->t100_address == 0) {
		return -ENOTSUP;
	}

	ret = mxt_i2c_read_reg_locked(dev,
				      data->t100_address + MXT_T100_NUMTCH,
				      &numtch, 1);
	if (ret < 0) {
		LOG_ERR("Failed to read T100 NUMTCH: %d", ret);
		return ret;
	}

	target = data->num_touchids;
	if (cfg->max_touches > 0 && cfg->max_touches < target) {
		target = cfg->max_touches;
	}

	LOG_INF("T100: NUMTCH=%u (available=%u, target=%u)",
		numtch, data->num_touchids, target);

	if (numtch == target) {
		return 0;
	}

	numtch = target;
	ret = mxt_i2c_write_reg_locked(dev,
				       data->t100_address + MXT_T100_NUMTCH,
				       &numtch, 1);
	if (ret < 0) {
		LOG_ERR("Failed to write T100 NUMTCH: %d", ret);
		return ret;
	}

	LOG_INF("T100: NUMTCH set to %u", numtch);
	return 0;
}

/* Message processing */

static void mxt_process_t6_message(const struct device *dev, const uint8_t *msg)
{
	struct mxt_data *data = dev->data;
	uint8_t status = msg[1];

	data->t6_status = status;

	if (status & MXT_T6_STATUS_RESET) {
		LOG_INF("T6: reset complete");
	}
	if (status & MXT_T6_STATUS_CAL) {
		LOG_INF("T6: calibrating");
	}
	if (status & MXT_T6_STATUS_CFGERR) {
		LOG_ERR("T6: configuration error");
	}
	if (status & MXT_T6_STATUS_SIGERR) {
		LOG_ERR("T6: signal error");
	}
	if (status & MXT_T6_STATUS_OFL) {
		LOG_ERR("T6: acquisition overflow");
	}

	data->config_crc = (uint32_t)msg[2] |
			   ((uint32_t)msg[3] << 8) |
			   ((uint32_t)msg[4] << 16);

	if (status == 0) {
		LOG_INF("T6: config CRC=0x%06X", data->config_crc);
	}
}

static void mxt_process_messages(const struct device *dev, uint8_t count)
{
	struct mxt_data *data = dev->data;
	struct mxt_touch_point touches[CONFIG_INPUT_MICROCHIP_MAXTOUCH_MAX_TOUCHES];
	uint8_t touch_count = data->prev_touch_count;
	uint8_t touch_reportid_start = data->t100_reportid_min + 2;

	/* Carry forward previously active touches (stationary fingers) */
	memcpy(touches, data->prev_touches,
	       touch_count * sizeof(struct mxt_touch_point));

	for (uint8_t i = 0; i < count; i++) {
		uint8_t *msg = &data->msg_buf[i * data->t5_msg_size];
		uint8_t report_id = msg[0];

		if (report_id == 0 || report_id == 0xFF) {
			continue;
		}

		if (report_id == data->t6_reportid) {
			mxt_process_t6_message(dev, msg);
			continue;
		}

		if (report_id >= touch_reportid_start &&
		    report_id <= data->t100_reportid_max) {
			uint8_t touch_id = report_id - touch_reportid_start;
			uint8_t status = msg[1];

			if (touch_id >= CONFIG_INPUT_MICROCHIP_MAXTOUCH_MAX_TOUCHES) {
				continue;
			}

			if (status & MXT_T100_STATUS_DETECT) {
				uint16_t x = sys_get_le16(&msg[2]);
				uint16_t y = sys_get_le16(&msg[4]);
				uint8_t slot = touch_count;

				for (uint8_t s = 0; s < touch_count; s++) {
					if (touches[s].id == touch_id) {
						slot = s;
						break;
					}
				}

				if (slot < CONFIG_INPUT_MICROCHIP_MAXTOUCH_MAX_TOUCHES) {
					touches[slot].id = touch_id;
					touches[slot].x = x;
					touches[slot].y = y;
					touches[slot].active = true;
					if (slot == touch_count) {
						touch_count++;
					}
				}

				LOG_DBG("TOUCH[%u]: x=%u y=%u status=0x%02X",
					touch_id, x, y, status);
			} else {
				for (uint8_t s = 0; s < touch_count; s++) {
					if (touches[s].id == touch_id) {
						touches[s].active = false;
						break;
					}
				}

				LOG_DBG("RELEASE[%u]: status=0x%02X",
					touch_id, status);
			}
			continue;
		}

		LOG_DBG("Unhandled report_id=%u "
			"[%02X %02X %02X %02X %02X %02X]",
			report_id,
			msg[0], msg[1], msg[2], msg[3], msg[4], msg[5]);
	}

	/* Count active and released touches for sync placement */
	uint8_t active_count = 0;
	uint8_t release_count = 0;

	for (uint8_t i = 0; i < touch_count; i++) {
		if (touches[i].active) {
			active_count++;
		} else {
			release_count++;
		}
	}

	/* Report active touches */
	uint8_t reported = 0;

	for (uint8_t i = 0; i < touch_count; i++) {
		if (!touches[i].active) {
			continue;
		}

		reported++;
		bool is_last = (reported == active_count) &&
			       (release_count == 0);

		if (CONFIG_INPUT_MICROCHIP_MAXTOUCH_MAX_TOUCHES > 1) {
			input_report_abs(dev, INPUT_ABS_MT_SLOT,
					 touches[i].id,
					 false, K_NO_WAIT);
		}
		input_touchscreen_report_pos(dev, touches[i].x,
					     touches[i].y, K_NO_WAIT);
		input_report_key(dev, INPUT_BTN_TOUCH, 1, is_last, K_NO_WAIT);
	}

	/* Report released fingers */
	uint8_t released = 0;

	for (uint8_t i = 0; i < touch_count; i++) {
		if (touches[i].active) {
			continue;
		}

		released++;
		bool is_last = (released == release_count);

		if (CONFIG_INPUT_MICROCHIP_MAXTOUCH_MAX_TOUCHES > 1) {
			input_report_abs(dev, INPUT_ABS_MT_SLOT,
					 touches[i].id,
					 false, K_NO_WAIT);
		}
		input_touchscreen_report_pos(dev, touches[i].x,
					     touches[i].y, K_NO_WAIT);
		input_report_key(dev, INPUT_BTN_TOUCH, 0, is_last,
				 K_NO_WAIT);
	}

	/* Save active touches for next cycle */
	uint8_t saved = 0;

	for (uint8_t i = 0; i < touch_count; i++) {
		if (touches[i].active) {
			data->prev_touches[saved++] = touches[i];
		}
	}
	data->prev_touch_count = saved;
}

/*
 * Read pending messages via T44 count + T5 bulk read.
 * Returns the number of messages processed, or negative on error.
 */
static int mxt_read_messages(const struct device *dev)
{
	struct mxt_data *data = dev->data;
	uint8_t count = 0;
	int ret;

	k_mutex_lock(&data->i2c_lock, K_FOREVER);

	ret = mxt_i2c_read_reg(dev, data->t44_address, &count, 1);
	if (ret < 0) {
		LOG_ERR("Failed to read T44: %d", ret);
		k_mutex_unlock(&data->i2c_lock);
		return ret;
	}

	if (count == 0) {
		/* Read T5 once to deassert CHG and catch late messages */
		ret = mxt_i2c_read_reg(dev, data->t5_address,
				       data->msg_buf, data->t5_msg_size);
		k_mutex_unlock(&data->i2c_lock);
		if (ret == 0 && data->msg_buf[0] != 0 &&
		    data->msg_buf[0] != 0xFF) {
			mxt_process_messages(dev, 1);
			return 1;
		}
		return 0;
	}

	if (count > MXT_MAX_MSG_COUNT) {
		count = MXT_MAX_MSG_COUNT;
	}
	if (count > data->max_reportid) {
		count = data->max_reportid;
	}

	size_t read_size = (size_t)count * data->t5_msg_size;

	ret = mxt_i2c_read_reg(dev, data->t5_address, data->msg_buf, read_size);
	k_mutex_unlock(&data->i2c_lock);

	if (ret < 0) {
		LOG_ERR("Failed to bulk read messages: %d", ret);
		return ret;
	}

	mxt_process_messages(dev, count);
	return (int)count;
}

/*
 * Work handler: drain messages via T44+T5 bulk reads.
 * IRQ mode: ONESHOT pattern - drain until CHG goes high, then re-enable.
 * Poll mode: drain until T44 returns 0.
 */
static void mxt_work_handler(struct k_work *work)
{
	struct mxt_data *data = CONTAINER_OF(work, struct mxt_data, work);
	const struct device *dev = data->dev;

#ifdef CONFIG_INPUT_MICROCHIP_MAXTOUCH_INTERRUPT
	const struct mxt_config *cfg = dev->config;

	int passes = 0;

	do {
		for (int tries = 0; tries < 10; tries++) {
			int ret = mxt_read_messages(dev);

			if (ret <= 0) {
				break;
			}
		}
		passes++;
		if (passes >= 50 && !data->chg_warned) {
			data->chg_warned = true;
			LOG_WRN("CHG stuck low after drain - "
				"check pull-up on CHG line "
				"(open-drain, needs pull-up to VddIO)");
		}
		k_sleep(K_MSEC(1));
	} while (cfg->irq_gpio.port != NULL &&
		 gpio_pin_get_dt(&cfg->irq_gpio));

	/* Re-enable edge interrupt now that CHG is deasserted */
	if (cfg->irq_gpio.port != NULL) {
		gpio_pin_interrupt_configure_dt(&cfg->irq_gpio,
						GPIO_INT_EDGE_TO_ACTIVE);
	}
#else
	for (int tries = 0; tries < 10; tries++) {
		int ret = mxt_read_messages(dev);

		if (ret <= 0) {
			break;
		}
	}
#endif
}

/* IRQ / polling handlers */

#ifdef CONFIG_INPUT_MICROCHIP_MAXTOUCH_INTERRUPT
static void mxt_irq_handler(const struct device *gpio, struct gpio_callback *cb,
			     uint32_t pins)
{
	struct mxt_data *data = CONTAINER_OF(cb, struct mxt_data, irq_cb);
	const struct mxt_config *cfg = data->dev->config;

	/* Disable interrupt; work handler re-enables after drain */
	gpio_pin_interrupt_configure_dt(&cfg->irq_gpio, GPIO_INT_DISABLE);
	k_work_submit(&data->work);
}
#else
static void mxt_timer_handler(struct k_timer *timer)
{
	struct mxt_data *data = CONTAINER_OF(timer, struct mxt_data, poll_timer);

	k_work_submit(&data->work);
}
#endif

/* Reset helpers */

static int mxt_hw_reset(const struct device *dev)
{
	const struct mxt_config *cfg = dev->config;

	if (cfg->rst_gpio.port == NULL) {
		return -ENOTSUP;
	}

	gpio_pin_set_dt(&cfg->rst_gpio, 1);
	k_sleep(K_MSEC(20));
	gpio_pin_set_dt(&cfg->rst_gpio, 0);
	k_sleep(K_MSEC(1000));

	return 0;
}

/* Soft reset via T6 command, polls for reset-complete message */
static int mxt_soft_reset(const struct device *dev)
{
	struct mxt_data *data = dev->data;
	uint8_t cmd = 1;
	int ret;

	ret = mxt_i2c_write_reg_locked(dev, data->t6_address + MXT_T6_CMD_RESET,
				       &cmd, sizeof(cmd));
	if (ret < 0) {
		LOG_ERR("Failed to send soft reset: %d", ret);
		return ret;
	}

	k_sleep(K_MSEC(100));

	for (int i = 0; i < 30; i++) {
		uint8_t count = 0;

		ret = mxt_i2c_read_reg_locked(dev, data->t44_address,
					      &count, 1);
		if (ret < 0 || count == 0) {
			k_sleep(K_MSEC(100));
			continue;
		}

		uint8_t msg[MXT_MAX_MSG_SIZE];

		for (uint8_t m = 0; m < count && m < MXT_MAX_MSG_COUNT; m++) {
			ret = mxt_i2c_read_reg_locked(dev, data->t5_address,
						      msg, data->t5_msg_size);
			if (ret < 0) {
				break;
			}

			if (msg[0] == data->t6_reportid) {
				mxt_process_t6_message(dev, msg);
				if (data->t6_status & MXT_T6_STATUS_RESET) {
					return 0;
				}
			}
		}

		k_sleep(K_MSEC(100));
	}

	LOG_WRN("Soft reset timeout (continuing anyway)");
	return 0;
}

/* Drain pending messages by reading T5 until invalid report ID */
static int mxt_drain_messages(const struct device *dev)
{
	struct mxt_data *data = dev->data;
	uint8_t msg[MXT_MAX_MSG_SIZE];
	int ret;
	int count = 0;

	for (int i = 0; i < 50; i++) {
		ret = mxt_i2c_read_reg_locked(dev, data->t5_address, msg,
					      data->t5_msg_size);
		if (ret < 0) {
			break;
		}

		if (msg[0] == 0 || msg[0] == 0xFF) {
			break;
		}

		if (msg[0] == data->t6_reportid) {
			mxt_process_t6_message(dev, msg);
		}

		count++;
	}

	if (count > 0) {
		LOG_DBG("Drained %d pending messages", count);
	}

	return 0;
}

/* PM support */

#ifdef CONFIG_PM_DEVICE
static int mxt_pm_action(const struct device *dev, enum pm_device_action action)
{
	struct mxt_data *data = dev->data;

	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND: {
		struct mxt_t7_config sleep_cfg = {
			.idle = 0, .active = 0, .actv2idle = 0
		};

		mxt_i2c_write_reg_locked(dev, data->t7_address,
					 &sleep_cfg, sizeof(sleep_cfg));

#ifdef CONFIG_INPUT_MICROCHIP_MAXTOUCH_INTERRUPT
		const struct mxt_config *cfg = dev->config;

		if (cfg->irq_gpio.port != NULL) {
			gpio_pin_interrupt_configure_dt(&cfg->irq_gpio,
							GPIO_INT_DISABLE);
		}
#else
		k_timer_stop(&data->poll_timer);
#endif
		LOG_INF("Suspended");
		break;
	}
	case PM_DEVICE_ACTION_RESUME: {
		mxt_i2c_write_reg_locked(dev, data->t7_address,
					 &data->t7_cfg, sizeof(data->t7_cfg));

#ifdef CONFIG_INPUT_MICROCHIP_MAXTOUCH_INTERRUPT
		const struct mxt_config *cfg = dev->config;

		if (cfg->irq_gpio.port != NULL) {
			gpio_pin_interrupt_configure_dt(&cfg->irq_gpio,
							GPIO_INT_EDGE_TO_ACTIVE);
		}
#else
		k_timer_start(&data->poll_timer,
			      K_MSEC(CONFIG_INPUT_MICROCHIP_MAXTOUCH_PERIOD_MS),
			      K_MSEC(CONFIG_INPUT_MICROCHIP_MAXTOUCH_PERIOD_MS));
#endif
		mxt_drain_messages(dev);
		LOG_INF("Resumed");
		break;
	}
	default:
		return -ENOTSUP;
	}

	return 0;
}
#endif /* CONFIG_PM_DEVICE */

/* Device initialization */

static int mxt_init(const struct device *dev)
{
	const struct mxt_config *cfg = dev->config;
	struct mxt_data *data = dev->data;
	int ret;

	data->dev = dev;

	if (!i2c_is_ready_dt(&cfg->i2c)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

	k_mutex_init(&data->i2c_lock);
	k_work_init(&data->work, mxt_work_handler);

	/* Hardware reset if GPIO available */
	if (cfg->rst_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->rst_gpio)) {
			LOG_ERR("Reset GPIO not ready");
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(&cfg->rst_gpio,
					    GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			LOG_ERR("Failed to configure reset GPIO: %d", ret);
			return ret;
		}

		ret = mxt_hw_reset(dev);
		if (ret < 0) {
			LOG_WRN("HW reset failed: %d (continuing)", ret);
		}
	}

	if (cfg->startup_delay_ms > 0) {
		k_sleep(K_MSEC(cfg->startup_delay_ms));
	}

	/* Probe configured address, fall back to alternate (ADDR pin) */
	uint16_t dt_addr = cfg->i2c.addr;
	uint16_t alt_addr = (dt_addr == MXT_I2C_ADDR_PRIMARY)
			    ? MXT_I2C_ADDR_SECONDARY : MXT_I2C_ADDR_PRIMARY;

	LOG_INF("Probing on %s at 0x%02X...", cfg->i2c.bus->name, dt_addr);
	ret = mxt_probe_address(cfg->i2c.bus, dt_addr);

	if (ret == 0) {
		data->actual_address = dt_addr;
	} else {
		LOG_WRN("No response at 0x%02X (%d), trying 0x%02X...",
			dt_addr, ret, alt_addr);
		ret = mxt_probe_address(cfg->i2c.bus, alt_addr);
		if (ret == 0) {
			data->actual_address = alt_addr;
			LOG_WRN("Device found at 0x%02X but 0x%02X was "
				"configured in devicetree",
				alt_addr, dt_addr);
		} else {
			LOG_ERR("Not found at 0x%02X or 0x%02X "
				"(touch disabled)", dt_addr, alt_addr);
			return -ENODEV;
		}
	}

	LOG_INF("Detected at 0x%02X", data->actual_address);

	ret = mxt_read_info_block(dev);
	if (ret < 0) {
		return ret;
	}

	ret = mxt_read_t7_config(dev);
	if (ret < 0) {
		return ret;
	}

	/* Reset and drain pending messages before enabling IRQ/polling */
	mxt_soft_reset(dev);
	mxt_drain_messages(dev);

	/* Configure T100 touch count and T18 CHG mode */
	mxt_configure_t100(dev);

#ifdef CONFIG_INPUT_MICROCHIP_MAXTOUCH_INTERRUPT
	mxt_set_chg_mode1(dev);

	if (cfg->irq_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->irq_gpio)) {
			LOG_ERR("IRQ GPIO not ready");
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(&cfg->irq_gpio,
					    GPIO_INPUT | GPIO_PULL_UP);
		if (ret < 0) {
			LOG_ERR("Failed to configure IRQ GPIO: %d", ret);
			return ret;
		}

		gpio_init_callback(&data->irq_cb, mxt_irq_handler,
				   BIT(cfg->irq_gpio.pin));

		ret = gpio_add_callback(cfg->irq_gpio.port, &data->irq_cb);
		if (ret < 0) {
			LOG_ERR("Failed to add IRQ callback: %d", ret);
			return ret;
		}

		ret = gpio_pin_interrupt_configure_dt(&cfg->irq_gpio,
						      GPIO_INT_EDGE_TO_ACTIVE);
		if (ret < 0) {
			LOG_ERR("Failed to configure IRQ: %d", ret);
			return ret;
		}

		LOG_INF("Interrupt mode");

		/* Drain messages that arrived before interrupt was enabled */
		mxt_drain_messages(dev);
	}
#else
	k_timer_init(&data->poll_timer, mxt_timer_handler, NULL);
	k_timer_start(&data->poll_timer,
		      K_MSEC(CONFIG_INPUT_MICROCHIP_MAXTOUCH_PERIOD_MS),
		      K_MSEC(CONFIG_INPUT_MICROCHIP_MAXTOUCH_PERIOD_MS));
	LOG_INF("Polling mode (%dms)", CONFIG_INPUT_MICROCHIP_MAXTOUCH_PERIOD_MS);
#endif

	LOG_INF("Initialized: %u touch points", data->num_touchids);

	return 0;
}

/* Device instantiation */

#define MXT_INIT(inst) \
	static const struct mxt_config mxt_config_##inst = { \
		.common = INPUT_TOUCH_DT_INST_COMMON_CONFIG_INIT(inst), \
		.i2c = I2C_DT_SPEC_INST_GET(inst), \
		.rst_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios, {0}), \
		.irq_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, irq_gpios, {0}), \
		.max_touches = DT_INST_PROP_OR(inst, max_touches, 0), \
		.startup_delay_ms = DT_INST_PROP(inst, startup_delay_ms), \
	}; \
	static struct mxt_data mxt_data_##inst; \
	PM_DEVICE_DT_INST_DEFINE(inst, mxt_pm_action); \
	DEVICE_DT_INST_DEFINE(inst, mxt_init, \
			      PM_DEVICE_DT_INST_GET(inst), \
			      &mxt_data_##inst, &mxt_config_##inst, \
			      POST_KERNEL, CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(MXT_INIT)
