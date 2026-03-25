/*
 * Copyright (c) 2026 Microchip Technology Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * LCDC POC application with two tabs:
 *   1. LVGL widget demo (button counter, arc, slider)
 *   2. Multitouch visualizer using raw input events
 */

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

LOG_MODULE_REGISTER(main_app, LOG_LEVEL_INF);

#define MAX_TOUCHES 10
#define INDICATOR_SIZE 70
#define TAB_BAR_HEIGHT 50

static const struct gpio_dt_spec lvds_pwm =
	GPIO_DT_SPEC_GET(DT_NODELABEL(gpio_lvds_pwm), gpios);

#define TOUCH_DEV DEVICE_DT_GET(DT_CHOSEN(zephyr_touch))

/* Demo tab state */

static lv_obj_t *counter_label;
static lv_obj_t *arc_obj;
static lv_obj_t *arc_label;
static uint32_t counter;

/* Touch tab state */

struct touch_point {
	int32_t x;
	int32_t y;
	bool active;
};

static struct {
	struct touch_point points[MAX_TOUCHES];
	int32_t current_slot;
	int32_t pending_x;
	int32_t pending_y;
	bool has_x;
	bool has_y;
} touch_state;

struct touch_indicator {
	lv_obj_t *circle;
	lv_obj_t *id_label;
	lv_obj_t *coord_label;
};

static struct touch_indicator indicators[MAX_TOUCHES];
static lv_obj_t *touch_tab_obj;
static lv_obj_t *count_label;
static lv_obj_t *max_label;
static lv_obj_t *hint_label;
static lv_obj_t *hold_btn;
static bool hold_mode;
static uint8_t max_touches;

static const lv_color_t slot_colors[] = {
	LV_COLOR_MAKE(231, 76, 60),   /* Red */
	LV_COLOR_MAKE(46, 204, 113),  /* Green */
	LV_COLOR_MAKE(52, 152, 219),  /* Blue */
	LV_COLOR_MAKE(241, 196, 15),  /* Yellow */
	LV_COLOR_MAKE(155, 89, 182),  /* Purple */
	LV_COLOR_MAKE(230, 126, 34),  /* Orange */
	LV_COLOR_MAKE(26, 188, 156),  /* Teal */
	LV_COLOR_MAKE(236, 240, 241), /* White */
	LV_COLOR_MAKE(52, 73, 94),    /* Dark */
	LV_COLOR_MAKE(149, 165, 166), /* Gray */
};

/* Multitouch input event callback */

static void touch_input_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (evt->type) {
	case INPUT_EV_ABS:
		switch (evt->code) {
		case INPUT_ABS_MT_SLOT:
			if (evt->value >= 0 && evt->value < MAX_TOUCHES) {
				touch_state.current_slot = evt->value;
			}
			touch_state.has_x = false;
			touch_state.has_y = false;
			break;
		case INPUT_ABS_X:
			touch_state.pending_x = evt->value;
			touch_state.has_x = true;
			break;
		case INPUT_ABS_Y:
			touch_state.pending_y = evt->value;
			touch_state.has_y = true;
			break;
		default:
			break;
		}
		break;

	case INPUT_EV_KEY:
		if (evt->code == INPUT_BTN_TOUCH) {
			int32_t slot = touch_state.current_slot;

			if (slot < 0 || slot >= MAX_TOUCHES) {
				break;
			}

			if (evt->value) {
				if (touch_state.has_x) {
					touch_state.points[slot].x =
						touch_state.pending_x;
				}
				if (touch_state.has_y) {
					touch_state.points[slot].y =
						touch_state.pending_y;
				}
				touch_state.points[slot].active = true;
			} else {
				touch_state.points[slot].active = false;
			}
		}
		break;

	default:
		break;
	}
}

INPUT_CALLBACK_DEFINE(TOUCH_DEV, touch_input_cb, NULL);

/* Demo tab */

static void btn_event_cb(lv_event_t *e)
{
	counter++;
	lv_label_set_text_fmt(counter_label, "Count: %u", counter);
}

static void arc_event_cb(lv_event_t *e)
{
	int32_t val = lv_arc_get_value(arc_obj);

	lv_label_set_text_fmt(arc_label, "%d%%", val);
	lv_obj_align_to(arc_label, arc_obj, LV_ALIGN_CENTER, 0, 0);
}

static void create_demo_tab(lv_obj_t *parent)
{
	/* Title label */
	lv_obj_t *title = lv_label_create(parent);

	lv_label_set_text(title, "LVGL on Zephyr");
	lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

	/* Info label */
	lv_obj_t *info = lv_label_create(parent);

	lv_label_set_text(info,
			  "SAMA7D65 Curiosity Kit with AC69T88A LVDS Display");
	lv_obj_set_style_text_font(info, &lv_font_montserrat_14, 0);
	lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 48);

	/* Counter button */
	lv_obj_t *btn = lv_btn_create(parent);

	lv_obj_set_size(btn, 200, 60);
	lv_obj_align(btn, LV_ALIGN_CENTER, 0, -20);
	lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *btn_label = lv_label_create(btn);

	lv_label_set_text(btn_label, "Tap Me");
	lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_20, 0);
	lv_obj_center(btn_label);

	/* Counter display */
	counter_label = lv_label_create(parent);
	lv_label_set_text(counter_label, "Count: 0");
	lv_obj_set_style_text_font(counter_label, &lv_font_montserrat_20, 0);
	lv_obj_align(counter_label, LV_ALIGN_CENTER, 0, 30);

	/* Arc widget */
	arc_obj = lv_arc_create(parent);
	lv_arc_set_range(arc_obj, 0, 100);
	lv_arc_set_value(arc_obj, 70);
	lv_obj_set_size(arc_obj, 180, 180);
	lv_obj_set_ext_click_area(arc_obj, 20);
	lv_obj_align(arc_obj, LV_ALIGN_BOTTOM_LEFT, 40, -20);
	lv_obj_add_event_cb(arc_obj, arc_event_cb,
			    LV_EVENT_VALUE_CHANGED, NULL);

	/* Arc percentage label */
	arc_label = lv_label_create(parent);
	lv_label_set_text(arc_label, "70%");
	lv_obj_set_style_text_font(arc_label, &lv_font_montserrat_20, 0);
	lv_obj_align_to(arc_label, arc_obj, LV_ALIGN_CENTER, 0, 0);

	/* Brightness slider */
	lv_obj_t *slider = lv_slider_create(parent);

	lv_slider_set_range(slider, 0, 255);
	lv_slider_set_value(slider, 128, LV_ANIM_OFF);
	lv_obj_set_width(slider, 280);
	lv_obj_set_ext_click_area(slider, 20);

	lv_obj_update_layout(parent);
	int32_t arc_cy = lv_obj_get_y(arc_obj) +
			 lv_obj_get_height(arc_obj) / 2;
	int32_t slider_h = lv_obj_get_height(slider);
	int32_t parent_cw = lv_obj_get_content_width(parent);

	lv_obj_set_pos(slider,
		       parent_cw - 40 - 280,
		       arc_cy - slider_h / 2);

	lv_obj_t *slider_label = lv_label_create(parent);

	lv_label_set_text(slider_label, "Brightness");
	lv_obj_set_style_text_font(slider_label, &lv_font_montserrat_14, 0);
	lv_obj_align_to(slider_label, slider, LV_ALIGN_OUT_BOTTOM_MID,
			0, 8);
}

/* Touch tab */

static void hold_btn_cb(lv_event_t *e)
{
	hold_mode = !hold_mode;

	lv_obj_t *label = lv_obj_get_child(hold_btn, 0);

	if (hold_mode) {
		lv_label_set_text(label, "Hold: ON");
	} else {
		lv_label_set_text(label, "Hold: OFF");
		max_touches = 0;
	}
}

static void create_touch_tab(lv_obj_t *parent)
{
	touch_tab_obj = parent;
	lv_obj_set_style_pad_all(parent, 0, 0);

	/* Title */
	lv_obj_t *title = lv_label_create(parent);

	lv_label_set_text(title, "Multitouch Test");
	lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

	/* Touch count labels */
	count_label = lv_label_create(parent);
	lv_label_set_text(count_label, "Active: 0");
	lv_obj_set_style_text_font(count_label, &lv_font_montserrat_20, 0);
	lv_obj_align(count_label, LV_ALIGN_TOP_MID, -60, 45);

	max_label = lv_label_create(parent);
	lv_label_set_text(max_label, "Max: 0");
	lv_obj_set_style_text_font(max_label, &lv_font_montserrat_20, 0);
	lv_obj_align(max_label, LV_ALIGN_TOP_MID, 60, 45);

	/* Hint */
	hint_label = lv_label_create(parent);
	lv_label_set_text(hint_label, "Touch the screen with 1..10 fingers");
	lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_opa(hint_label, LV_OPA_50, 0);
	lv_obj_align(hint_label, LV_ALIGN_TOP_MID, 0, 72);

	/* Hold toggle button */
	hold_btn = lv_btn_create(parent);
	lv_obj_set_size(hold_btn, 120, 40);
	lv_obj_align(hold_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
	lv_obj_add_event_cb(hold_btn, hold_btn_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *btn_label = lv_label_create(hold_btn);

	lv_label_set_text(btn_label, "Hold: OFF");
	lv_obj_center(btn_label);

	/* Pre-allocate indicator objects for each slot */
	for (int i = 0; i < MAX_TOUCHES; i++) {
		struct touch_indicator *ind = &indicators[i];

		ind->circle = lv_obj_create(parent);
		lv_obj_set_size(ind->circle, INDICATOR_SIZE, INDICATOR_SIZE);
		lv_obj_set_style_radius(ind->circle, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(ind->circle, slot_colors[i], 0);
		lv_obj_set_style_bg_opa(ind->circle, LV_OPA_80, 0);
		lv_obj_set_style_border_color(ind->circle,
					      lv_color_white(), 0);
		lv_obj_set_style_border_width(ind->circle, 2, 0);
		lv_obj_clear_flag(ind->circle,
				  LV_OBJ_FLAG_CLICKABLE |
				  LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_flag(ind->circle, LV_OBJ_FLAG_HIDDEN);

		ind->id_label = lv_label_create(ind->circle);
		lv_label_set_text_fmt(ind->id_label, "%d", i);
		lv_obj_set_style_text_font(ind->id_label,
					   &lv_font_montserrat_20, 0);
		lv_obj_align(ind->id_label, LV_ALIGN_CENTER, 0, -8);

		ind->coord_label = lv_label_create(ind->circle);
		lv_label_set_text(ind->coord_label, "");
		lv_obj_set_style_text_font(ind->coord_label,
					   &lv_font_montserrat_14, 0);
		lv_obj_align(ind->coord_label, LV_ALIGN_CENTER, 0, 12);
	}
}

/* UI creation and update */

static void create_ui(void)
{
	lv_display_t *disp = lv_display_get_default();

	lv_theme_default_init(disp,
			      lv_palette_main(LV_PALETTE_BLUE),
			      lv_palette_main(LV_PALETTE_CYAN),
			      true, &lv_font_montserrat_14);

	lv_obj_t *scr = lv_screen_active();

	/* Tabview */
	lv_obj_t *tv = lv_tabview_create(scr);

	lv_tabview_set_tab_bar_size(tv, TAB_BAR_HEIGHT);
	lv_obj_set_size(tv, LV_PCT(100), LV_PCT(100));

	/* Disable scrolling on the content pager and tabs */
	lv_obj_t *content = lv_tabview_get_content(tv);

	lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *tab_demo = lv_tabview_add_tab(tv, "Demo");
	lv_obj_t *tab_touch = lv_tabview_add_tab(tv, "Touch Test");

	lv_obj_clear_flag(tab_demo, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_clear_flag(tab_touch, LV_OBJ_FLAG_SCROLLABLE);

	create_demo_tab(tab_demo);
	create_touch_tab(tab_touch);

	LOG_INF("UI created");
}

static void update_touch_indicators(void)
{
	lv_area_t tab_area;

	lv_obj_get_coords(touch_tab_obj, &tab_area);
	int32_t off_x = tab_area.x1;
	int32_t off_y = tab_area.y1;

	uint8_t active_count = 0;

	for (int i = 0; i < MAX_TOUCHES; i++) {
		struct touch_indicator *ind = &indicators[i];

		if (touch_state.points[i].active) {
			int32_t x = touch_state.points[i].x;
			int32_t y = touch_state.points[i].y;

			lv_obj_set_pos(ind->circle,
				       x - off_x - INDICATOR_SIZE / 2,
				       y - off_y - INDICATOR_SIZE / 2);
			lv_label_set_text_fmt(ind->coord_label,
					      "%d,%d", x, y);
			lv_obj_clear_flag(ind->circle, LV_OBJ_FLAG_HIDDEN);
			active_count++;
		} else if (!hold_mode) {
			lv_obj_add_flag(ind->circle, LV_OBJ_FLAG_HIDDEN);
		}
	}

	if (active_count > max_touches) {
		max_touches = active_count;
	}

	lv_label_set_text_fmt(count_label, "Active: %u", active_count);
	lv_label_set_text_fmt(max_label, "Max: %u", max_touches);

	if (active_count > 0) {
		lv_obj_add_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
	} else if (!hold_mode) {
		lv_obj_clear_flag(hint_label, LV_OBJ_FLAG_HIDDEN);
	}
}

int main(void)
{
	int ret;

	LOG_INF("Setting up GPIOs...");

	if (!gpio_is_ready_dt(&lvds_pwm)) {
		LOG_ERR("GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&lvds_pwm, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		return ret;
	}

	LOG_INF("LCDC POC Demo");

	const struct device *display_dev =
		DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(display_dev)) {
		LOG_ERR("Display device not ready");
		return -ENODEV;
	}

	if (!device_is_ready(TOUCH_DEV)) {
		LOG_ERR("Touch device not ready");
		return -ENODEV;
	}

	create_ui();

	display_blanking_off(display_dev);

	int64_t last_tick = k_uptime_get();

	while (1) {
		/* Auto-increment counter every second */
		int64_t now = k_uptime_get();

		if ((now - last_tick) >= 1000) {
			last_tick = now;
			counter++;
			lv_label_set_text_fmt(counter_label, "Count: %u",
					      counter);
		}

		update_touch_indicators();
		lv_timer_handler();
		k_sleep(K_MSEC(LV_DEF_REFR_PERIOD));
	}

	return 0;
}
