/*
 * Copyright (c) 2023 Rockchip, Inc. All Rights Reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */
#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "ui_advanced_setup.c"

#include "audio.h"
#include "common.h"
#include "evdev.h"
#include "lvgl/porting/lv_port_indev.h"
#include "storage.h"
#include "ui_common.h"
#include "ui_dialog.h"
#include "ui_dialog_format.h"
#include "ui_dialog_storage.h"
#include "ui_page_manager.h"
#include "ui_player.h"
#include "ui_resource_manage.h"
#include "video.h"
#include <linux/input.h>

/**********************
 *  STATIC PROTOTYPES
 **********************/
typedef struct {
	lv_obj_t *container_obj;

	lv_obj_t *exit_obj;
	lv_obj_t *exit_label_obj;

	lv_obj_t *eis_dialog;
	lv_group_t *eis_group;

	lv_obj_t *eis_obj;
	lv_obj_t *eis_label_obj;
	lv_obj_t *hdr_obj;
	lv_obj_t *debug_obj;
	lv_obj_t *audio_obj;

	lv_group_t *group;

} UI_ADVANCED_SETUP_CONTROL_S;

/**********************
 *  STATIC VARIABLES
 **********************/
static UI_ADVANCED_SETUP_CONTROL_S advanced_setup_ctrl;

extern lv_ft_info_t ttf_info_14;
extern lv_ft_info_t ttf_info_12;
extern lv_ft_info_t ttf_info_10;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void create_eis_config_box(void);
static void destroy_eis_config_box(void);

static void return_event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);

	if (code == LV_EVENT_CLICKED) {
		ui_page_pop_page();
	}
}

static void audio_event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);
	if (code == LV_EVENT_CLICKED) {
		if (evdev_get_current_code() == KEY_POWER) {
			ui_page_pop_page();
			return;
		}
		if (!rk_param_get_int("audio.0:enable", 0)) {
			ui_dialog_create("WARNING", "当前模式不允许配置AI音频!");
			// restore focus status
			if (lv_obj_has_state(obj, LV_STATE_CHECKED))
				lv_obj_clear_state(obj, LV_STATE_CHECKED);
			else
				lv_obj_add_state(obj, LV_STATE_CHECKED);
			return ;
		}
		if (lv_obj_has_state(obj, LV_STATE_CHECKED)) {
			rk_param_set_int("audio.0:enable_vqe", 1);
			LOG_INFO("enable vqe\n");
		} else {
			rk_param_set_int("audio.0:enable_vqe", 0);
			LOG_INFO("disable vqe\n");
		}
		rk_audio_restart();
	}
}

static void eis_event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);
	if (code == LV_EVENT_CLICKED) {
		if (evdev_get_current_code() == KEY_POWER) {
			ui_page_pop_page();
			return;
		}
		create_eis_config_box();
	}
}

static void hdr_event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);
	if (code == LV_EVENT_CLICKED) {
		if (evdev_get_current_code() == KEY_POWER) {
			ui_page_pop_page();
			return;
		}
        if (lv_obj_has_state(obj, LV_STATE_CHECKED))
			rk_set_hdr(true);
		else
			rk_set_hdr(false);
	}
}

static void debug_event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);
	if (code == LV_EVENT_CLICKED) {
		if (evdev_get_current_code() == KEY_POWER) {
			ui_page_pop_page();
			return;
		}
        if (lv_obj_has_state(obj, LV_STATE_CHECKED))
			rk_set_eis_debug(true);
		else
			rk_set_eis_debug(false);
	}
}

static void btn_event_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *btn = lv_event_get_target(e);
	lv_obj_t *parent = lv_obj_get_parent(btn);
	lv_obj_t *child;
	int child_idx = 0, child_cnt = 0;

	if (code == LV_EVENT_CLICKED) {
		if (evdev_get_current_code() == KEY_POWER) {
			destroy_eis_config_box();
			return;
		}
		if (rk_get_mode() != RK_VIDEO_MODE && rk_get_mode() != RK_PHOTO_MODE) {
			ui_dialog_create("WARNING", "请先切换到录像或拍照模式!");
			lv_obj_clear_state(btn, LV_STATE_CHECKED);
			return ;
		}
		child_cnt = lv_obj_get_child_cnt(parent);
		while (child_idx < child_cnt) {
			child = lv_obj_get_child(parent, child_idx);
			if (!child)
				break;
			++child_idx;
			lv_obj_clear_state(child, LV_STATE_CHECKED);
		}

		lv_obj_add_state(btn, LV_STATE_CHECKED);
		rk_set_eis_mode(lv_obj_get_index(btn));
		LOG_INFO("change new index %d\n", lv_obj_get_index(btn));

		destroy_eis_config_box();
	}
}

static lv_obj_t *create_radio_dialog(lv_obj_t *parent) {
	lv_obj_t *dialog = lv_obj_create(lv_scr_act());
	lv_color_t text_color = lv_color_make(0XFF, 0XFF, 0XFF);
	lv_color_t bg_color = lv_palette_darken(LV_PALETTE_GREY, 3);
	lv_obj_set_style_bg_color(dialog, bg_color, 0);
	lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(dialog, bg_color, 0);
	lv_obj_set_size(dialog, lv_pct(60), lv_pct(60));
	lv_obj_align(dialog, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_flex_flow(dialog, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(dialog, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	return dialog;
}

static void create_radio_buttons(lv_obj_t *parent, lv_group_t *g) {
	const char *options[] = {
		"关闭防抖",
		"普通防抖",
		"地平线防抖",
		"畸变校正",
	};
	int select_btn_idx = 0;
	lv_obj_t *btn = NULL, *label = NULL;
	static lv_style_t btn_style, btn_focus_style;

	lv_style_reset(&btn_style);
	lv_style_init(&btn_style);
	lv_style_set_bg_color(&btn_style, lv_palette_lighten(LV_PALETTE_GREY, 3));
	lv_style_set_text_color(&btn_style, lv_color_hex(0x000000));
	lv_style_set_bg_opa(&btn_style, LV_OPA_COVER);
	lv_style_set_radius(&btn_style, 10);

	lv_style_reset(&btn_focus_style);
	lv_style_init(&btn_focus_style);
	lv_style_set_bg_color(&btn_focus_style, lv_palette_main(LV_PALETTE_GREY));
	lv_style_set_text_color(&btn_focus_style, lv_color_hex(0x000000));
	lv_style_set_bg_opa(&btn_focus_style, LV_OPA_COVER);
	lv_style_set_outline_opa(&btn_focus_style, LV_OPA_TRANSP);
	lv_style_set_radius(&btn_focus_style, 10);

	for (int i = 0; i < 4; i++) {
		//btn = lv_btn_create(parent);
		btn = lv_checkbox_create(parent);
		lv_obj_set_size(btn, lv_pct(80), lv_pct(25));
		lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
		lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_FOCUSED, NULL);
		lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_DEFOCUSED, NULL);
		lv_checkbox_set_text(btn, options[i]);
		lv_obj_set_style_text_font(btn, ttf_info_14.font, LV_PART_MAIN);
		lv_obj_add_style(btn, &btn_style, LV_PART_MAIN);
		lv_obj_add_style(btn, &btn_focus_style, LV_STATE_FOCUS_KEY);

		lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_INDICATOR);
		lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
		lv_obj_set_style_text_align(btn, LV_TEXT_ALIGN_RIGHT, LV_PART_INDICATOR);

		lv_group_add_obj(g, btn);
	}
	select_btn_idx = rk_get_eis_mode();
	LOG_INFO("select index %d\n", select_btn_idx);
	btn = lv_obj_get_child(parent, select_btn_idx);
	lv_obj_add_state(btn, LV_STATE_CHECKED);
	lv_group_focus_obj(btn);
}

static void create_eis_config_box(void) {
	advanced_setup_ctrl.eis_group = lv_port_indev_group_create();
	if (advanced_setup_ctrl.eis_group == NULL)
		return ;
	advanced_setup_ctrl.eis_dialog = create_radio_dialog(lv_scr_act());

	create_radio_buttons(advanced_setup_ctrl.eis_dialog, advanced_setup_ctrl.eis_group);
}

static void destroy_eis_config_box(void) {
	if (advanced_setup_ctrl.eis_group) {
		lv_port_indev_group_destroy(advanced_setup_ctrl.eis_group);
		advanced_setup_ctrl.eis_group = NULL;
	}
	if (advanced_setup_ctrl.eis_dialog) {
		lv_obj_del(advanced_setup_ctrl.eis_dialog);
		advanced_setup_ctrl.eis_dialog = NULL;
	}
}

static void advanced_setup_layout(lv_obj_t *page_obj) {
	lv_obj_set_flex_flow(advanced_setup_ctrl.container_obj, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(advanced_setup_ctrl.container_obj, LV_FLEX_ALIGN_START
			, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollbar_mode(advanced_setup_ctrl.container_obj, LV_SCROLLBAR_MODE_OFF);
}

static void advanced_setup_create_ctrl(lv_obj_t *page_obj) {
	lv_color_t bg_color = lv_color_hex(0xFF04171D);
	lv_obj_t *cont_obj = NULL, *obj = NULL;

	lv_disp_set_bg_opa(NULL, LV_OPA_TRANSP);
	lv_obj_set_style_bg_opa(page_obj, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(page_obj, bg_color, 0);

	advanced_setup_ctrl.exit_obj = lv_btn_create(page_obj);
	lv_obj_set_style_bg_opa(advanced_setup_ctrl.exit_obj, LV_OPA_TRANSP, 0);
	lv_obj_add_event_cb(advanced_setup_ctrl.exit_obj, return_event_handler, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(advanced_setup_ctrl.exit_obj, return_event_handler, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(advanced_setup_ctrl.exit_obj, return_event_handler, LV_EVENT_DEFOCUSED, NULL);
	advanced_setup_ctrl.exit_label_obj = lv_label_create(advanced_setup_ctrl.exit_obj);

	advanced_setup_ctrl.container_obj = cont_obj = lv_obj_create(page_obj);
	lv_obj_set_size(cont_obj, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_opa(cont_obj, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_opa(cont_obj, LV_OPA_TRANSP, 0);
	lv_obj_set_style_radius(cont_obj, 0, 0);
	lv_obj_align_to(cont_obj, advanced_setup_ctrl.exit_obj, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 0);

	advanced_setup_ctrl.eis_obj = lv_btn_create(cont_obj);
	lv_obj_set_size(advanced_setup_ctrl.eis_obj, lv_pct(100), lv_pct(20));
	lv_obj_add_event_cb(advanced_setup_ctrl.eis_obj, eis_event_handler, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(advanced_setup_ctrl.eis_obj, eis_event_handler, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(advanced_setup_ctrl.eis_obj, eis_event_handler, LV_EVENT_DEFOCUSED, NULL);
	advanced_setup_ctrl.eis_label_obj = lv_label_create(advanced_setup_ctrl.eis_obj);

	advanced_setup_ctrl.hdr_obj = lv_checkbox_create(cont_obj);
	lv_obj_add_flag(advanced_setup_ctrl.hdr_obj, LV_OBJ_FLAG_CHECKABLE);
	lv_obj_set_size(advanced_setup_ctrl.hdr_obj, lv_pct(100), lv_pct(20));
	lv_obj_add_event_cb(advanced_setup_ctrl.hdr_obj, hdr_event_handler, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(advanced_setup_ctrl.hdr_obj, hdr_event_handler, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(advanced_setup_ctrl.hdr_obj, hdr_event_handler, LV_EVENT_DEFOCUSED, NULL);

	advanced_setup_ctrl.debug_obj = lv_checkbox_create(cont_obj);
	lv_obj_add_flag(advanced_setup_ctrl.debug_obj, LV_OBJ_FLAG_CHECKABLE);
	lv_obj_set_size(advanced_setup_ctrl.debug_obj, lv_pct(100), lv_pct(20));
	lv_obj_add_event_cb(advanced_setup_ctrl.debug_obj, debug_event_handler, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(advanced_setup_ctrl.debug_obj, debug_event_handler, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(advanced_setup_ctrl.debug_obj, debug_event_handler, LV_EVENT_DEFOCUSED, NULL);

	advanced_setup_ctrl.audio_obj = lv_checkbox_create(cont_obj);
	lv_obj_add_flag(advanced_setup_ctrl.audio_obj, LV_OBJ_FLAG_CHECKABLE);
	lv_obj_set_size(advanced_setup_ctrl.audio_obj, lv_pct(100), lv_pct(20));
	lv_obj_add_event_cb(advanced_setup_ctrl.audio_obj, audio_event_handler, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(advanced_setup_ctrl.audio_obj, audio_event_handler, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(advanced_setup_ctrl.audio_obj, audio_event_handler, LV_EVENT_DEFOCUSED, NULL);
}

static void advanced_setup_set_text() {
	static lv_style_t symbol_style, symbol_focus_style, option_style, option_focus_style;
	lv_style_reset(&option_style);
	lv_style_init(&option_style);
	lv_style_set_bg_opa(&option_style, LV_OPA_COVER);
	lv_style_set_bg_color(&option_style, lv_palette_lighten(LV_PALETTE_GREY, 3));
	lv_style_set_text_color(&option_style, lv_color_hex(0x000000));
	lv_style_set_text_font(&option_style, ttf_info_14.font);
	lv_style_set_radius(&option_style, 5);
	lv_style_set_border_opa(&option_style, LV_OPA_TRANSP);

	lv_style_reset(&option_focus_style);
	lv_style_init(&option_focus_style);
	lv_style_set_bg_opa(&option_focus_style, LV_OPA_COVER);
	lv_style_set_bg_color(&option_focus_style, lv_palette_main(LV_PALETTE_GREY));
	lv_style_set_text_color(&option_focus_style, lv_color_hex(0x000000));
	lv_style_set_text_font(&option_focus_style, ttf_info_14.font);
	lv_style_set_radius(&option_focus_style, 5);
	lv_style_set_border_opa(&option_focus_style, LV_OPA_TRANSP);
	lv_style_set_outline_opa(&option_focus_style, LV_OPA_TRANSP);
	lv_style_set_outline_width(&option_focus_style, 0);

	lv_style_reset(&symbol_style);
	lv_style_init(&symbol_style);
	lv_style_set_bg_opa(&symbol_style, LV_OPA_TRANSP);
	lv_style_set_text_font(&symbol_style, &lv_font_montserrat_32);
	lv_style_set_text_color(&symbol_style, lv_color_hex(0xFFFFFF));
	lv_style_set_text_align(&symbol_style, LV_TEXT_ALIGN_LEFT);
	lv_style_set_outline_opa(&symbol_style, LV_OPA_TRANSP);
	lv_style_set_outline_width(&symbol_style, 0);
	lv_style_set_pad_all(&symbol_style, 5);

	lv_style_reset(&symbol_focus_style);
	lv_style_init(&symbol_focus_style);
	lv_style_set_bg_opa(&symbol_focus_style, LV_OPA_TRANSP);
	lv_style_set_text_font(&symbol_focus_style, &lv_font_montserrat_32);
	lv_style_set_text_color(&symbol_focus_style, lv_palette_main(LV_PALETTE_GREY));
	lv_style_set_text_align(&symbol_focus_style, LV_TEXT_ALIGN_LEFT);
	lv_style_set_outline_opa(&symbol_focus_style, LV_OPA_TRANSP);
	lv_style_set_outline_width(&symbol_focus_style, 0);
	lv_style_set_pad_all(&symbol_focus_style, 5);

	lv_obj_remove_style_all(advanced_setup_ctrl.exit_obj);
	lv_label_set_text(advanced_setup_ctrl.exit_label_obj, LV_SYMBOL_LEFT);
	lv_obj_add_style(advanced_setup_ctrl.exit_obj, &symbol_style, 0);
	lv_obj_add_style(advanced_setup_ctrl.exit_obj, &symbol_focus_style, LV_STATE_FOCUS_KEY);

	lv_obj_add_style(advanced_setup_ctrl.eis_obj, &option_style, 0);
	lv_obj_add_style(advanced_setup_ctrl.eis_obj, &option_focus_style, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
	lv_label_set_text(advanced_setup_ctrl.eis_label_obj, "EIS模式");

	lv_checkbox_set_text(advanced_setup_ctrl.hdr_obj, "HDR模式");
	lv_obj_add_style(advanced_setup_ctrl.hdr_obj, &option_style, 0);
	lv_obj_add_style(advanced_setup_ctrl.hdr_obj, &option_focus_style, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
	lv_obj_set_style_radius(advanced_setup_ctrl.hdr_obj, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_style_pad_top(advanced_setup_ctrl.hdr_obj, 8, 0);
	if (rk_get_hdr())
		lv_obj_add_state(advanced_setup_ctrl.hdr_obj, LV_STATE_CHECKED);

	lv_checkbox_set_text(advanced_setup_ctrl.debug_obj, "开发者模式");
	lv_obj_add_style(advanced_setup_ctrl.debug_obj, &option_style, 0);
	lv_obj_add_style(advanced_setup_ctrl.debug_obj, &option_focus_style, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
	lv_obj_set_style_radius(advanced_setup_ctrl.debug_obj, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_style_pad_top(advanced_setup_ctrl.debug_obj, 8, 0);
	if (rk_get_eis_debug())
		lv_obj_add_state(advanced_setup_ctrl.debug_obj, LV_STATE_CHECKED);

	lv_checkbox_set_text(advanced_setup_ctrl.audio_obj, "AI音频降噪");
	lv_obj_add_style(advanced_setup_ctrl.audio_obj, &option_style, 0);
	lv_obj_add_style(advanced_setup_ctrl.audio_obj, &option_focus_style, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
	lv_obj_set_style_radius(advanced_setup_ctrl.audio_obj, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_style_pad_top(advanced_setup_ctrl.audio_obj, 8, 0);
	if (rk_param_get_int("audio.0:enable_vqe", 0))
		lv_obj_add_state(advanced_setup_ctrl.audio_obj, LV_STATE_CHECKED);
}

static void advanced_setup_destroy_ctrl(void) {
	if (NULL != advanced_setup_ctrl.eis_obj) {
		ui_common_remove_style_all(advanced_setup_ctrl.eis_obj);
		lv_obj_del(advanced_setup_ctrl.eis_obj);
		advanced_setup_ctrl.eis_obj = NULL;
	}
	if (NULL != advanced_setup_ctrl.hdr_obj) {
		ui_common_remove_style_all(advanced_setup_ctrl.hdr_obj);
		lv_obj_del(advanced_setup_ctrl.hdr_obj);
		advanced_setup_ctrl.hdr_obj = NULL;
	}
	if (NULL != advanced_setup_ctrl.debug_obj) {
		ui_common_remove_style_all(advanced_setup_ctrl.debug_obj);
		lv_obj_del(advanced_setup_ctrl.debug_obj);
		advanced_setup_ctrl.debug_obj = NULL;
	}
	if (NULL != advanced_setup_ctrl.audio_obj) {
		ui_common_remove_style_all(advanced_setup_ctrl.audio_obj);
		lv_obj_del(advanced_setup_ctrl.audio_obj);
		advanced_setup_ctrl.audio_obj = NULL;
	}
	if (NULL != advanced_setup_ctrl.exit_obj) {
		ui_common_remove_style_all(advanced_setup_ctrl.exit_obj);
		lv_obj_del(advanced_setup_ctrl.exit_obj);
		advanced_setup_ctrl.exit_obj = NULL;
	}
	if (NULL != advanced_setup_ctrl.container_obj) {
		ui_common_remove_style_all(advanced_setup_ctrl.container_obj);
		lv_obj_del(advanced_setup_ctrl.container_obj);
		advanced_setup_ctrl.container_obj = NULL;
	}
}

static void advanced_setup_page_create(lv_obj_t *page_obj) {

	advanced_setup_create_ctrl(page_obj);
	advanced_setup_layout(page_obj);
	advanced_setup_set_text();
}

static void advanced_setup_add_indev(void) {
	lv_group_t *group = lv_port_indev_group_create();
	if (NULL == group)
		return;

	lv_group_add_obj(group, advanced_setup_ctrl.exit_obj);
	lv_group_add_obj(group, advanced_setup_ctrl.eis_obj);
	lv_group_add_obj(group, advanced_setup_ctrl.hdr_obj);
	lv_group_add_obj(group, advanced_setup_ctrl.debug_obj);
	lv_group_add_obj(group, advanced_setup_ctrl.audio_obj);

	advanced_setup_ctrl.group = group;
}

static void advanced_setup_delete_indev(void) {
	if (NULL != advanced_setup_ctrl.group) {
		lv_port_indev_group_destroy(advanced_setup_ctrl.group);
		advanced_setup_ctrl.group = NULL;
	}
}

static void advanced_setup_page_enter(lv_obj_t *page_obj) {
	advanced_setup_add_indev();
}

static void advanced_setup_page_exit(lv_obj_t *page_obj) {}

static void advanced_setup_page_destroy(lv_obj_t *page_obj) {
	advanced_setup_delete_indev();
	advanced_setup_destroy_ctrl();
}

static UI_PAGE_HANDLER_T advanced_setup_page = {.name = "advanced_setup",
                                          .init = NULL,
                                          .create = advanced_setup_page_create,
                                          .enter = advanced_setup_page_enter,
                                          .destroy = advanced_setup_page_destroy,
                                          .exit = advanced_setup_page_exit};

UI_PAGE_REGISTER(advanced_setup_page)
