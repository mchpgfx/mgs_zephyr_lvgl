/*
 * Copyright (c) 2025 Microchip Technology Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DRIVERS_INPUT_MICROCHIP_MAXTOUCH_H_
#define DRIVERS_INPUT_MICROCHIP_MAXTOUCH_H_

#include <stdbool.h>
#include <stdint.h>

/* Information Block (7 bytes at address 0x0000) */
struct mxt_info {
	uint8_t family_id;
	uint8_t variant_id;
	uint8_t version;
	uint8_t build;
	uint8_t matrix_x_size;
	uint8_t matrix_y_size;
	uint8_t num_objects;
} __packed;

/* Object Table Element (6 bytes per entry) */
struct mxt_object {
	uint8_t type;
	uint8_t start_pos_lsb;
	uint8_t start_pos_msb;
	uint8_t size_minus_one;
	uint8_t instances_minus_one;
	uint8_t num_report_ids;
} __packed;

/* T7 Power Configuration */
struct mxt_t7_config {
	uint8_t idle;
	uint8_t active;
	uint8_t actv2idle;
} __packed;

/* T100 config field offsets */
#define MXT_T100_NUMTCH 6

/* T100 Touch Status byte flags */
#define MXT_T100_STATUS_DETECT    BIT(7)
#define MXT_T100_STATUS_TYPE_MASK  0x70U
#define MXT_T100_STATUS_TYPE_SHIFT 4
#define MXT_T100_STATUS_EVENT_MASK 0x0FU

/* T100 touch types */
#define MXT_T100_TYPE_FINGER 1
#define MXT_T100_TYPE_STYLUS 2
#define MXT_T100_TYPE_GLOVE  5

/* T6 Command Processor status flags */
#define MXT_T6_STATUS_RESET   BIT(7)
#define MXT_T6_STATUS_OFL     BIT(6)
#define MXT_T6_STATUS_SIGERR  BIT(5)
#define MXT_T6_STATUS_CAL     BIT(4)
#define MXT_T6_STATUS_CFGERR  BIT(3)
#define MXT_T6_STATUS_COMSERR BIT(2)

/* T6 Command Processor field offsets */
#define MXT_T6_CMD_RESET      0
#define MXT_T6_CMD_BACKUPNV   1
#define MXT_T6_CMD_CALIBRATE  2
#define MXT_T6_CMD_REPORTALL  3
#define MXT_T6_CMD_DIAGNOSTIC 5

/* Object types */
#define MXT_GEN_MESSAGE_T5                5
#define MXT_GEN_COMMAND_T6                6
#define MXT_GEN_POWER_T7                  7
#define MXT_GEN_ACQUIRE_T8                8
#define MXT_SPT_COMMSCONFIG_T18           18
#define MXT_SPT_MESSAGECOUNT_T44          44
#define MXT_TOUCH_MULTITOUCHSCREEN_T100   100

/* T18 SPT_COMMSCONFIG: MODE bit selects CHG line behavior */
#define MXT_T18_CTRL_MODE BIT(6)

/* Protocol limits */
#define MXT_INFO_SIZE          sizeof(struct mxt_info)
#define MXT_OBJECT_SIZE        sizeof(struct mxt_object)
#define MXT_INFO_CHECKSUM_SIZE 3
#define MXT_MAX_OBJECTS        48
#define MXT_MAX_INFO_SIZE      (MXT_INFO_SIZE + \
				(MXT_MAX_OBJECTS * MXT_OBJECT_SIZE) + \
				MXT_INFO_CHECKSUM_SIZE)
#define MXT_MAX_MSG_SIZE       16
#define MXT_MAX_MSG_COUNT      20

/* CRC-24 polynomial used by maXTouch info block validation */
#define MXT_CRC_POLY 0x80001BU

/* I2C addresses (selected by hardware ADDR pin) */
#define MXT_I2C_ADDR_PRIMARY   0x4A
#define MXT_I2C_ADDR_SECONDARY 0x4B

/* I2C probe timeout in milliseconds */
#define MXT_PROBE_TIMEOUT_MS 2000

/* Touch point state for release detection */
struct mxt_touch_point {
	uint16_t x;
	uint16_t y;
	uint8_t id;
	bool active;
};

#endif /* DRIVERS_INPUT_MICROCHIP_MAXTOUCH_H_ */
