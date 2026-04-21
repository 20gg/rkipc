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
#define LOG_TAG "ui_settings.c"

#include "ui_settings.h"
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

	lv_obj_t *mode_imgbtn_obj;
	lv_obj_t *mode_img_obj;
	lv_obj_t *mode_label_obj;

	lv_obj_t *fps_imgbtn_obj;
	lv_obj_t *fps_img_obj;
	lv_obj_t *fps_label_obj;

	lv_obj_t *cap_imgbtn_obj;
	lv_obj_t *cap_img_obj;
	lv_obj_t *cap_label_obj;

	lv_obj_t *set_imgbtn_obj;
	lv_obj_t *set_img_obj;
	lv_obj_t *set_label_obj;

	lv_obj_t *store_imgbtn_obj;
	lv_obj_t *store_img_obj;
	lv_obj_t *store_label_obj;

	lv_obj_t *format_imgbtn_obj;
	lv_obj_t *format_img_obj;
	lv_obj_t *format_label_obj;

	lv_obj_t *select_dialog;
	lv_group_t *select_group;

	lv_group_t *group;

} UI_SETTINGS_CONTROL_S;

/**********************
 *  STATIC VARIABLES
 **********************/
static UI_SETTINGS_CONTROL_S settings_ctrl;

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
static void setting_set_text(void);

static void mode_update(void) {
	if (rk_get_mode() == RK_PHOTO_MODE) {
		lv_label_set_text(settings_ctrl.fps_img_obj, "4K30");
		lv_img_set_src(settings_ctrl.mode_img_obj, index_icon_capture);
		lv_label_set_text(settings_ctrl.mode_label_obj, "拍照模式");
	} else if (rk_get_mode() == RK_SLOW_MOTION_MODE) {
		lv_label_set_text(settings_ctrl.fps_img_obj, "4K30");
		lv_img_set_src(settings_ctrl.mode_img_obj, index_icon_slow_motion);
		lv_label_set_text(settings_ctrl.mode_label_obj, "慢动作");
	} else if (rk_get_mode() == RK_VIDEO_MODE) {
		lv_label_set_text(settings_ctrl.fps_img_obj, "4K30");
		lv_img_set_src(settings_ctrl.mode_img_obj, index_icon_video);
		lv_label_set_text(settings_ctrl.mode_label_obj, "录像模式");
	} else {
		lv_label_set_text(settings_ctrl.fps_img_obj, "4K30");
		lv_img_set_src(settings_ctrl.mode_img_obj, index_icon_slow_motion);
		lv_label_set_text(settings_ctrl.mode_label_obj, "延时摄影");
	}
}

static void fps_update(void) {
	int fps = 0;
	fps = rk_video_get_fps();
	if (fps == 30) {
		rk_video_set_fps(58);
		lv_label_set_text(settings_ctrl.fps_img_obj, "4K60");
	} else {
		rk_video_set_fps(30);
		lv_label_set_text(settings_ctrl.fps_img_obj, "4K30");
	}
	return;
}

static void capture_update(void) {
	if (rk_photo_get_max_num() == 5) {
		lv_label_set_text(settings_ctrl.cap_img_obj, "5P1S");
	} else if (rk_photo_get_max_num() == 10) {
		lv_label_set_text(settings_ctrl.cap_img_obj, "10P2S");
	} else {
		lv_label_set_text(settings_ctrl.cap_img_obj, "20P3S");
	}
}

static void settings_disk_event_handler(rkipc_mount_status disk_state) {
	ui_format_dialog_destory();
	ui_storage_status_dialog_destroy();
	switch (disk_state) {
	case DISK_UNMOUNTED:
		ui_nosdcard_dialog_create();
		break;
	case DISK_FORMAT_ERR:
	case DISK_NOT_FORMATTED:
		ui_noformat_dialog_create();
		break;
	case DISK_SCANNING:
		ui_scansdcard_dialog_create();
		break;
	case DISK_MOUNTED:
		ui_nosdcard_dialog_destroy();
		ui_noformat_dialog_destroy();
		ui_scansdcard_dialog_destroy();
		break;

	default:
		break;
	}
}

static void destroy_mode_select_box(void);

static void btn_event_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *btn = lv_event_get_target(e);
	lv_obj_t *parent = lv_obj_get_parent(btn);
	lv_obj_t *child;
	int child_idx = 0, child_cnt = 0;

	if (code == LV_EVENT_CLICKED) {
		if (evdev_get_current_code() == KEY_POWER) {
			destroy_mode_select_box();
			return;
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
		rk_set_mode(lv_obj_get_index(btn) - 1);
		LOG_INFO("change new index %d\n", lv_obj_get_index(btn) - 1);
		mode_update();

		destroy_mode_select_box();
	}
}

static lv_obj_t *create_radio_dialog(lv_obj_t *parent) {
	lv_obj_t *dialog = lv_obj_create(lv_scr_act());
	lv_color_t text_color = lv_color_make(0XFF, 0XFF, 0XFF);
	lv_color_t bg_color = lv_palette_darken(LV_PALETTE_GREY, 4);
	lv_obj_set_style_bg_color(dialog, bg_color, 0);
	lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(dialog, bg_color, 0);
	lv_obj_set_size(dialog, lv_pct(60), lv_pct(60));
	lv_obj_align(dialog, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_flex_flow(dialog, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(dialog, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *title = lv_label_create(dialog);
	static lv_style_t style;
	lv_label_set_text(title, "模式选择");
	lv_style_reset(&style);
	lv_style_init(&style);
	lv_style_set_text_font(&style, ttf_info_14.font);
	lv_style_set_text_color(&style, text_color);
	lv_style_set_text_align(&style, LV_TEXT_ALIGN_CENTER);
	lv_obj_add_style(title, &style, 0);

	return dialog;
}

static void create_radio_buttons(lv_obj_t *parent, lv_group_t *g) {
	const char *options[] = {
	    [RK_VIDEO_MODE] = "录像模式",
	    [RK_PHOTO_MODE] = "拍照模式",
	    [RK_SLOW_MOTION_MODE] = "慢动作",
	    [RK_TIME_LAPSE_MODE] = "延时摄影",
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

		lv_group_add_obj(g, btn);
	}
	select_btn_idx = rk_get_mode() + 1; // index 0 is title obj
	LOG_INFO("select index %d\n", select_btn_idx);
	btn = lv_obj_get_child(parent, select_btn_idx);
	lv_obj_add_state(btn, LV_STATE_CHECKED);
	lv_group_focus_obj(btn);
}

static void create_mode_select_box(void) {
	settings_ctrl.select_group = lv_port_indev_group_create();
	if (settings_ctrl.select_group == NULL)
		return ;
	settings_ctrl.select_dialog = create_radio_dialog(lv_scr_act());

	create_radio_buttons(settings_ctrl.select_dialog, settings_ctrl.select_group);
}

static void destroy_mode_select_box(void) {
	if (settings_ctrl.select_group) {
		lv_port_indev_group_destroy(settings_ctrl.select_group);
		settings_ctrl.select_group = NULL;
	}
	if (settings_ctrl.select_dialog) {
		// lv_obj_remove_style_all(dialog);
		lv_obj_del(settings_ctrl.select_dialog);
		settings_ctrl.select_dialog = NULL;
	}
}

static void mode_event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);

	if (code == LV_EVENT_CLICKED) {
		if (evdev_get_current_code() == KEY_POWER) {
			lv_event_send(settings_ctrl.exit_obj, LV_EVENT_CLICKED, NULL);
			return;
		}
		create_mode_select_box();
	} else if (code == LV_EVENT_FOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_p, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	} else if (code == LV_EVENT_DEFOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	}
}

static void fps_event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);

	if (code == LV_EVENT_CLICKED) {
		if (evdev_get_current_code() == KEY_POWER) {
			lv_event_send(settings_ctrl.exit_obj, LV_EVENT_CLICKED, NULL);
			return;
		}
		if (rk_get_mode() != RK_VIDEO_MODE) {
			ui_dialog_create("WARNING", "请先切换到录像模式!");
			return;
		}
		if (rk_get_eis_mode() == RK_HORIZON_STEADY) {
			ui_dialog_create("WARNING", "地平线防抖不支持切换帧率!");
			return;
		}
		fps_update();
	} else if (code == LV_EVENT_FOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_p, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	} else if (code == LV_EVENT_DEFOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	}
}

static void capture_event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);

	if (code == LV_EVENT_CLICKED) {
		if (evdev_get_current_code() == KEY_POWER) {
			lv_event_send(settings_ctrl.exit_obj, LV_EVENT_CLICKED, NULL);
			return;
		}
		if (rk_get_mode() != RK_PHOTO_MODE) {
			ui_dialog_create("WARNING", "请先切换到拍照模式!");
			return;
		}
		if (rk_photo_get_max_num() == 5)
			rk_photo_set_max_num(10);
		else if (rk_photo_get_max_num() == 10)
			rk_photo_set_max_num(20);
		else
			rk_photo_set_max_num(5);
		capture_update();
	} else if (code == LV_EVENT_FOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_p, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	} else if (code == LV_EVENT_DEFOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	}
}

static void set_event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);

	if (code == LV_EVENT_CLICKED) {
		if (evdev_get_current_code() == KEY_POWER) {
			lv_event_send(settings_ctrl.exit_obj, LV_EVENT_CLICKED, NULL);
			return;
		}
		ui_page_push_page("advanced_setup", NULL);
	} else if (code == LV_EVENT_FOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_p, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	} else if (code == LV_EVENT_DEFOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	}
}

static void store_event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);

	if (code == LV_EVENT_CLICKED) {
		if (evdev_get_current_code() == KEY_POWER) {
			lv_event_send(settings_ctrl.exit_obj, LV_EVENT_CLICKED, NULL);
			return;
		}
		switch (rkipc_storage_dev_mount_status_get()) {
		case DISK_UNMOUNTED:
			ui_nosdcard_dialog_create();
			break;
		case DISK_FORMAT_ERR:
		case DISK_NOT_FORMATTED:
			ui_noformat_dialog_create();
			break;
		case DISK_SCANNING:
			ui_scansdcard_dialog_create();
			break;
		case DISK_MOUNTED:
			ui_storage_status_dialog_create();
			break;
		default:
			break;
		}
	} else if (code == LV_EVENT_FOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_p, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	} else if (code == LV_EVENT_DEFOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	}
}

static void format_event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);

	if (code == LV_EVENT_CLICKED) {
		if (evdev_get_current_code() == KEY_POWER) {
			lv_event_send(settings_ctrl.exit_obj, LV_EVENT_CLICKED, NULL);
			return;
		}
		switch (rkipc_storage_dev_mount_status_get()) {
		case DISK_UNMOUNTED:
			ui_nosdcard_dialog_create();
			break;
		case DISK_FORMAT_ERR:
		case DISK_NOT_FORMATTED:
		case DISK_SCANNING:
		case DISK_MOUNTED:
			ui_format_dialog_create();
			break;
		default:
			break;
		}
	} else if (code == LV_EVENT_FOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_p, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	} else if (code == LV_EVENT_DEFOCUSED) {
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
		lv_imgbtn_set_src(obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	}
}

static void return_event_handler(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_target(e);

	if (code == LV_EVENT_CLICKED) {
		ui_page_pop_page();
	}
}

static void setting_layout(void) {
	static lv_coord_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
	                               LV_GRID_TEMPLATE_LAST};
	static lv_coord_t row_dsc[] = {LV_GRID_FR(1), LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_FR(1),
	                               LV_GRID_TEMPLATE_LAST};

	lv_obj_set_layout(settings_ctrl.container_obj, LV_LAYOUT_GRID);

	lv_obj_set_style_grid_column_dsc_array(settings_ctrl.container_obj, col_dsc, 0);
	lv_obj_set_style_grid_row_dsc_array(settings_ctrl.container_obj, row_dsc, 0);

	lv_obj_set_grid_cell(settings_ctrl.exit_obj, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, 0,
	                     1);

	lv_obj_set_grid_cell(settings_ctrl.mode_imgbtn_obj, LV_GRID_ALIGN_CENTER, 0, 1,
	                     LV_GRID_ALIGN_CENTER, 1, 1);

	lv_obj_set_grid_cell(settings_ctrl.fps_imgbtn_obj, LV_GRID_ALIGN_CENTER, 1, 1,
	                     LV_GRID_ALIGN_CENTER, 1, 1);

	lv_obj_set_grid_cell(settings_ctrl.cap_imgbtn_obj, LV_GRID_ALIGN_CENTER, 2, 1,
	                     LV_GRID_ALIGN_CENTER, 1, 1);

	lv_obj_set_grid_cell(settings_ctrl.set_imgbtn_obj, LV_GRID_ALIGN_CENTER, 0, 1,
	                     LV_GRID_ALIGN_CENTER, 2, 1);

	lv_obj_set_grid_cell(settings_ctrl.store_imgbtn_obj, LV_GRID_ALIGN_CENTER, 1, 1,
	                     LV_GRID_ALIGN_CENTER, 2, 1);

	lv_obj_set_grid_cell(settings_ctrl.format_imgbtn_obj, LV_GRID_ALIGN_CENTER, 2, 1,
	                     LV_GRID_ALIGN_CENTER, 2, 1);
}

static void setting_create_ctrl(lv_obj_t *page_obj) {
	lv_color_t bg_color = lv_color_hex(0xFF04171D);
	lv_obj_t *cont_obj = NULL, *imgbtn_obj = NULL, *img_obj = NULL, *label_obj = NULL;

	lv_obj_set_style_bg_opa(page_obj, LV_OPA_TRANSP, 0);
	lv_disp_set_bg_opa(NULL, LV_OPA_TRANSP);

	settings_ctrl.container_obj = cont_obj = lv_obj_create(page_obj);
	lv_obj_set_size(cont_obj, lv_pct(100), lv_pct(100));
	lv_obj_align(cont_obj, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
	lv_obj_set_style_bg_color(cont_obj, bg_color, 0);
	lv_obj_set_style_border_color(cont_obj, bg_color, 0);
	lv_obj_set_style_radius(cont_obj, 0, 0);

	settings_ctrl.exit_obj = lv_btn_create(cont_obj);
	lv_obj_set_style_bg_opa(settings_ctrl.exit_obj, LV_OPA_TRANSP, 0);
	lv_obj_add_event_cb(settings_ctrl.exit_obj, return_event_handler, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(settings_ctrl.exit_obj, return_event_handler, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(settings_ctrl.exit_obj, return_event_handler, LV_EVENT_DEFOCUSED, NULL);
	settings_ctrl.exit_label_obj = lv_label_create(settings_ctrl.exit_obj);

	settings_ctrl.mode_imgbtn_obj = imgbtn_obj = lv_imgbtn_create(cont_obj);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	lv_obj_add_event_cb(imgbtn_obj, mode_event_handler, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, mode_event_handler, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, mode_event_handler, LV_EVENT_DEFOCUSED, NULL);
	lv_obj_set_size(imgbtn_obj, index_icon_boxbg_r->header.w, index_icon_boxbg_r->header.h);
	settings_ctrl.mode_img_obj = img_obj = lv_img_create(imgbtn_obj);
	lv_obj_align(img_obj, LV_ALIGN_CENTER, 0, 0);
	if (rk_get_mode() == RK_PHOTO_MODE)
		lv_img_set_src(settings_ctrl.mode_img_obj, index_icon_capture);
	else if (rk_get_mode() == RK_VIDEO_MODE)
		lv_img_set_src(settings_ctrl.mode_img_obj, index_icon_video);
	else
		lv_img_set_src(settings_ctrl.mode_img_obj, index_icon_slow_motion);
	settings_ctrl.mode_label_obj = label_obj = lv_label_create(imgbtn_obj);
	lv_obj_set_width(label_obj, lv_pct(100));
	lv_obj_align_to(label_obj, img_obj, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

	settings_ctrl.fps_imgbtn_obj = imgbtn_obj = lv_imgbtn_create(cont_obj);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	lv_obj_add_event_cb(imgbtn_obj, fps_event_handler, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, fps_event_handler, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, fps_event_handler, LV_EVENT_DEFOCUSED, NULL);
	lv_obj_set_size(imgbtn_obj, index_icon_boxbg_r->header.w, index_icon_boxbg_r->header.h);
	settings_ctrl.fps_img_obj = img_obj = lv_label_create(imgbtn_obj);
	lv_obj_set_size(settings_ctrl.fps_img_obj, lv_pct(60), lv_pct(50));
	lv_obj_align(img_obj, LV_ALIGN_CENTER, 0, 0);
	settings_ctrl.fps_label_obj = label_obj = lv_label_create(imgbtn_obj);
	lv_obj_set_width(label_obj, lv_pct(100));
	lv_obj_align(label_obj, LV_ALIGN_BOTTOM_MID, 0, 0);

	settings_ctrl.cap_imgbtn_obj = imgbtn_obj = lv_imgbtn_create(cont_obj);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	lv_obj_add_event_cb(imgbtn_obj, capture_event_handler, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, capture_event_handler, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, capture_event_handler, LV_EVENT_DEFOCUSED, NULL);
	lv_obj_set_size(imgbtn_obj, index_icon_boxbg_r->header.w, index_icon_boxbg_r->header.h);
	settings_ctrl.cap_img_obj = img_obj = lv_label_create(imgbtn_obj);
	lv_obj_set_size(settings_ctrl.cap_img_obj, lv_pct(60), lv_pct(50));
	lv_obj_align(img_obj, LV_ALIGN_CENTER, 0, 0);
	capture_update();
	settings_ctrl.cap_label_obj = label_obj = lv_label_create(imgbtn_obj);
	lv_obj_set_width(label_obj, lv_pct(100));
	lv_obj_align(label_obj, LV_ALIGN_BOTTOM_MID, 0, 0);

	settings_ctrl.set_imgbtn_obj = imgbtn_obj = lv_imgbtn_create(cont_obj);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	lv_obj_add_event_cb(imgbtn_obj, set_event_handler, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, set_event_handler, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, set_event_handler, LV_EVENT_DEFOCUSED, NULL);
	lv_obj_set_size(imgbtn_obj, index_icon_boxbg_r->header.w, index_icon_boxbg_r->header.h);
	settings_ctrl.set_img_obj = img_obj = lv_label_create(imgbtn_obj);
	lv_obj_set_size(settings_ctrl.set_img_obj, lv_pct(60), lv_pct(50));
	lv_obj_align(img_obj, LV_ALIGN_CENTER, 0, 0);
	settings_ctrl.set_label_obj = label_obj = lv_label_create(imgbtn_obj);
	lv_obj_set_width(label_obj, lv_pct(100));
	lv_obj_align_to(label_obj, img_obj, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

	settings_ctrl.store_imgbtn_obj = imgbtn_obj = lv_imgbtn_create(cont_obj);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	lv_obj_add_event_cb(imgbtn_obj, store_event_handler, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, store_event_handler, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, store_event_handler, LV_EVENT_DEFOCUSED, NULL);
	lv_obj_set_size(imgbtn_obj, index_icon_boxbg_r->header.w, index_icon_boxbg_r->header.h);
	settings_ctrl.store_img_obj = img_obj = lv_img_create(imgbtn_obj);
	lv_img_set_src(img_obj, index_icon_store_01);
	lv_obj_align(img_obj, LV_ALIGN_CENTER, 0, 0);
	settings_ctrl.store_label_obj = label_obj = lv_label_create(imgbtn_obj);
	lv_obj_set_width(label_obj, lv_pct(100));
	lv_obj_align_to(label_obj, img_obj, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

	settings_ctrl.format_imgbtn_obj = imgbtn_obj = lv_imgbtn_create(cont_obj);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_RELEASED, NULL, index_icon_boxbg_r, NULL);
	lv_imgbtn_set_src(imgbtn_obj, LV_IMGBTN_STATE_PRESSED, NULL, index_icon_boxbg_p, NULL);
	lv_obj_add_event_cb(imgbtn_obj, format_event_handler, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, format_event_handler, LV_EVENT_FOCUSED, NULL);
	lv_obj_add_event_cb(imgbtn_obj, format_event_handler, LV_EVENT_DEFOCUSED, NULL);
	lv_obj_set_size(imgbtn_obj, index_icon_boxbg_r->header.w, index_icon_boxbg_r->header.h);
	settings_ctrl.format_img_obj = img_obj = lv_img_create(imgbtn_obj);
	lv_img_set_src(img_obj, index_icon_format_01);
	lv_obj_align(img_obj, LV_ALIGN_CENTER, 0, 0);
	settings_ctrl.format_label_obj = label_obj = lv_label_create(imgbtn_obj);
	lv_obj_set_width(label_obj, lv_pct(100));
	lv_obj_align_to(label_obj, img_obj, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
}

static void setting_set_text() {
	static lv_style_t style, symbol_style, icon_style, symbol_focus_style;
	lv_color_t text_color = lv_color_hex(0xFFFFFF);

	lv_style_reset(&style);
	lv_style_init(&style);
	lv_style_set_text_font(&style, ttf_info_10.font);
	lv_style_set_text_color(&style, text_color);
	lv_style_set_text_align(&style, LV_TEXT_ALIGN_CENTER);

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

	lv_style_reset(&icon_style);
	lv_style_init(&icon_style);
	lv_style_set_radius(&icon_style, 5);
	lv_style_set_text_font(&icon_style, &lv_font_montserrat_14);
	lv_style_set_text_color(&icon_style, text_color);
	lv_style_set_text_align(&icon_style, LV_TEXT_ALIGN_CENTER);
	lv_style_set_border_opa(&icon_style, LV_OPA_COVER);
	lv_style_set_border_side(&icon_style, LV_BORDER_SIDE_FULL);
	lv_style_set_border_width(&icon_style, 2);
	lv_style_set_border_color(&icon_style, text_color);
	lv_style_set_pad_top(&icon_style, 7);
	lv_style_set_pad_bottom(&icon_style, 7);

	if (rk_get_mode() == RK_VIDEO_MODE)
		lv_label_set_text(settings_ctrl.mode_label_obj, "录像模式");
	else if (rk_get_mode() == RK_PHOTO_MODE)
		lv_label_set_text(settings_ctrl.mode_label_obj, "拍照模式");
	else if (rk_get_mode() == RK_SLOW_MOTION_MODE)
		lv_label_set_text(settings_ctrl.mode_label_obj, "慢动作");
	else
		lv_label_set_text(settings_ctrl.mode_label_obj, "延时摄影");

	lv_obj_add_style(settings_ctrl.mode_label_obj, &style, 0);

	lv_label_set_text(settings_ctrl.fps_label_obj, "录像配置");
	lv_obj_add_style(settings_ctrl.fps_label_obj, &style, 0);

	lv_label_set_text(settings_ctrl.cap_label_obj, "连拍模式");
	lv_obj_add_style(settings_ctrl.cap_label_obj, &style, 0);

	lv_label_set_text(settings_ctrl.set_label_obj, "高级设置");
	lv_obj_add_style(settings_ctrl.set_label_obj, &style, 0);
	lv_label_set_text(settings_ctrl.set_img_obj, LV_SYMBOL_SETTINGS);
	lv_obj_set_style_text_font(settings_ctrl.set_img_obj, &lv_font_montserrat_32, 0);
	lv_obj_set_style_text_color(settings_ctrl.set_img_obj, text_color, 0);
	lv_obj_set_style_text_align(settings_ctrl.set_img_obj, LV_TEXT_ALIGN_CENTER, 0);

	lv_label_set_text(settings_ctrl.store_label_obj, "存储状态");
	lv_obj_add_style(settings_ctrl.store_label_obj, &style, 0);

	lv_label_set_text(settings_ctrl.format_label_obj, "格式化");
	lv_obj_add_style(settings_ctrl.format_label_obj, &style, 0);

	lv_obj_remove_style_all(settings_ctrl.exit_obj);
	lv_label_set_text(settings_ctrl.exit_label_obj, LV_SYMBOL_LEFT);
	lv_obj_add_style(settings_ctrl.exit_obj, &symbol_style, 0);
	lv_obj_add_style(settings_ctrl.exit_obj, &symbol_focus_style, LV_STATE_FOCUS_KEY);

	lv_label_set_text(settings_ctrl.cap_img_obj, "5P1S");
	lv_obj_add_style(settings_ctrl.cap_img_obj, &icon_style, 0);

	lv_obj_add_style(settings_ctrl.fps_img_obj, &icon_style, 0);
	int fps = rk_param_get_int("isp.0.adjustment:fps", 30);
	if (fps == 30)
		lv_label_set_text(settings_ctrl.fps_img_obj, "4K30");
	else
		lv_label_set_text(settings_ctrl.fps_img_obj, "4K60");
}

static void setting_destroy_ctrl(void) {
	if (NULL != settings_ctrl.mode_imgbtn_obj) {
		ui_common_remove_style_all(settings_ctrl.mode_imgbtn_obj);
		lv_obj_del(settings_ctrl.mode_imgbtn_obj);
		settings_ctrl.mode_imgbtn_obj = NULL;
	}
	if (NULL != settings_ctrl.fps_imgbtn_obj) {
		ui_common_remove_style_all(settings_ctrl.fps_imgbtn_obj);
		lv_obj_del(settings_ctrl.fps_imgbtn_obj);
		settings_ctrl.fps_imgbtn_obj = NULL;
	}
	if (NULL != settings_ctrl.cap_imgbtn_obj) {
		ui_common_remove_style_all(settings_ctrl.cap_imgbtn_obj);
		lv_obj_del(settings_ctrl.cap_imgbtn_obj);
		settings_ctrl.cap_imgbtn_obj = NULL;
	}
	if (NULL != settings_ctrl.set_imgbtn_obj) {
		ui_common_remove_style_all(settings_ctrl.set_imgbtn_obj);
		lv_obj_del(settings_ctrl.set_imgbtn_obj);
		settings_ctrl.set_imgbtn_obj = NULL;
	}
	if (NULL != settings_ctrl.store_imgbtn_obj) {
		ui_common_remove_style_all(settings_ctrl.store_imgbtn_obj);
		lv_obj_del(settings_ctrl.store_imgbtn_obj);
		settings_ctrl.exit_obj = NULL;
	}
	if (NULL != settings_ctrl.format_imgbtn_obj) {
		ui_common_remove_style_all(settings_ctrl.format_imgbtn_obj);
		lv_obj_del(settings_ctrl.format_imgbtn_obj);
		settings_ctrl.format_imgbtn_obj = NULL;
	}
	if (NULL != settings_ctrl.exit_obj) {
		ui_common_remove_style_all(settings_ctrl.exit_obj);
		lv_obj_del(settings_ctrl.exit_obj);
		settings_ctrl.exit_obj = NULL;
	}
	if (NULL != settings_ctrl.container_obj) {
		ui_common_remove_style_all(settings_ctrl.container_obj);
		lv_obj_del(settings_ctrl.container_obj);
		settings_ctrl.container_obj = NULL;
	}
}

static void settings_page_create(lv_obj_t *page_obj) {

	setting_create_ctrl(page_obj);
	setting_set_text();
	setting_layout();
}

static void settings_add_indev(void) {
	lv_group_t *group = lv_port_indev_group_create();
	if (NULL == group)
		return;

	lv_group_add_obj(group, settings_ctrl.exit_obj);
	lv_group_add_obj(group, settings_ctrl.mode_imgbtn_obj);
	lv_group_add_obj(group, settings_ctrl.fps_imgbtn_obj);
	lv_group_add_obj(group, settings_ctrl.cap_imgbtn_obj);
	lv_group_add_obj(group, settings_ctrl.set_imgbtn_obj);
	lv_group_add_obj(group, settings_ctrl.store_imgbtn_obj);
	lv_group_add_obj(group, settings_ctrl.format_imgbtn_obj);

	settings_ctrl.group = group;
}

static void settings_delete_indev(void) {
	if (NULL != settings_ctrl.group) {
		lv_port_indev_group_destroy(settings_ctrl.group);
		settings_ctrl.group = NULL;
	}
}

static void settings_page_enter(lv_obj_t *page_obj) { settings_add_indev(); }

static void settings_page_exit(lv_obj_t *page_obj) {}

static void settings_page_destroy(lv_obj_t *page_obj) {
	settings_delete_indev();
	setting_destroy_ctrl();
}

static UI_PAGE_HANDLER_T settings_page = {.name = "settings",
                                          .init = NULL,
                                          .create = settings_page_create,
                                          .enter = settings_page_enter,
                                          .destroy = settings_page_destroy,
                                          .exit = settings_page_exit};

UI_PAGE_REGISTER(settings_page)
