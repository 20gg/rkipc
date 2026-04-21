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

#include <limits.h>

#include "ui_resource_manage.h"

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/
#define ICON_RELEASE(icon_index)                                                                   \
	do {                                                                                           \
		if (NULL != icon_index) {                                                                  \
			lv_free_img(&icon_index);                                                              \
			icon_index = NULL;                                                                     \
		}                                                                                          \
	} while (0)

/**********************
 *  GLOBAL VARIABLES
 **********************/
lv_ft_info_t ttf_info_10;
lv_ft_info_t ttf_info_12;
lv_ft_info_t ttf_info_14;
lv_ft_info_t ttf_info_28;

lv_img_dsc_t *index_bg = NULL;
lv_img_dsc_t *index_icon_photo_p = NULL;
lv_img_dsc_t *index_icon_photo_r = NULL;
lv_img_dsc_t *index_icon_set_p = NULL;
lv_img_dsc_t *index_icon_set_r = NULL;
lv_img_dsc_t *index_icon_media_p = NULL;
lv_img_dsc_t *index_icon_media_r = NULL;
lv_img_dsc_t *index_icon_record_nor = NULL;
lv_img_dsc_t *index_icon_record = NULL;
lv_img_dsc_t *index_icon_sw_p = NULL;
lv_img_dsc_t *index_icon_sw_r = NULL;

lv_img_dsc_t *index_icon_boxbg_r = NULL;
lv_img_dsc_t *index_icon_boxbg_p = NULL;
lv_img_dsc_t *index_icon_store_01 = NULL;
lv_img_dsc_t *index_icon_format_01 = NULL;
lv_img_dsc_t *index_icon_video = NULL;
lv_img_dsc_t *index_icon_capture = NULL;
lv_img_dsc_t *index_icon_slow_motion = NULL;

lv_img_dsc_t *index_icon_bg_r = NULL;
lv_img_dsc_t *index_icon_bg_p = NULL;

lv_img_dsc_t *icon_record_stop_r = NULL;
lv_img_dsc_t *icon_record_stop_p = NULL;
lv_img_dsc_t *icon_record_start_r = NULL;
lv_img_dsc_t *icon_record_start_p = NULL;
/**********************
 *   STATIC FUNCTIONS
 **********************/
static lv_img_dsc_t *ui_dec_icon(const char *const icon_name) {
	if (NULL == icon_name) {
		LV_LOG_ERROR("The icon named pointer is NULL.");
		return NULL;
	}

	char icon_path[PATH_MAX] = {0};

	snprintf(icon_path, sizeof(icon_path), "%s%s", "/usr/share/res/ui/", icon_name);
	return lv_dec_img(icon_path);
}

static int32_t ui_font_init(lv_ft_info_t *info, uint32_t weight) {
	if (NULL == info || 0 == weight) {
		LV_LOG_ERROR("Invalid font configuration parameter.");
		return -1;
	}

	info->name = "/usr/share/PuHuiTi.ttf";
	info->weight = weight;
	info->style = FT_FONT_STYLE_NORMAL;

	if (!lv_ft_font_init(info)) {
		LV_LOG_ERROR("Failed to create font.");
		return -1;
	}

	return 0;
}

static void setting_release_res(void) {
	ICON_RELEASE(index_icon_boxbg_r);
	ICON_RELEASE(index_icon_boxbg_p);
	ICON_RELEASE(index_icon_store_01);
	ICON_RELEASE(index_icon_format_01);
	ICON_RELEASE(index_icon_video);
	ICON_RELEASE(index_icon_capture);
	ICON_RELEASE(index_icon_slow_motion);
}

static void setting_load_res(void) {
	setting_release_res();

	index_icon_boxbg_r = ui_dec_icon("set_boxbg_01.png");
	index_icon_boxbg_p = ui_dec_icon("set_boxbg_02.png");
	index_icon_store_01 = ui_dec_icon("set_icon_store_01.png");
	index_icon_format_01 = ui_dec_icon("set_icon_format_01.png");
	index_icon_video = ui_dec_icon("set_icon_video.png");
	index_icon_capture = ui_dec_icon("set_icon_capture.png");
	index_icon_slow_motion = ui_dec_icon("set_icon_slow_motion.png");
}

static void view_release_res(void) {
	ICON_RELEASE(index_bg);
	ICON_RELEASE(index_icon_photo_p);
	ICON_RELEASE(index_icon_photo_r);
	ICON_RELEASE(icon_record_stop_r);
	ICON_RELEASE(icon_record_stop_p);
	ICON_RELEASE(icon_record_start_r);
	ICON_RELEASE(icon_record_start_p);
	ICON_RELEASE(index_icon_set_p);
	ICON_RELEASE(index_icon_set_r);
	ICON_RELEASE(index_icon_media_p);
	ICON_RELEASE(index_icon_media_r);
	ICON_RELEASE(index_icon_record_nor);
	ICON_RELEASE(index_icon_record);
	ICON_RELEASE(index_icon_sw_p);
	ICON_RELEASE(index_icon_sw_r);
}

static void view_load_res(void) {
	view_release_res();

	index_bg = ui_dec_icon("index_bg.png");
	index_icon_photo_p = ui_dec_icon("index_icon_photo_p.png");
	index_icon_photo_r = ui_dec_icon("index_icon_photo_r.png");
	icon_record_start_r = ui_dec_icon("icon_record_start_r.png");
	icon_record_start_p = ui_dec_icon("icon_record_start_p.png");
	icon_record_stop_r = ui_dec_icon("icon_record_stop_r.png");
	icon_record_stop_p = ui_dec_icon("icon_record_stop_p.png");
	index_icon_set_p = ui_dec_icon("index_icon_set_p.png");
	index_icon_set_r = ui_dec_icon("index_icon_set_r.png");
	index_icon_media_p = ui_dec_icon("index_icon_media_p.png");
	index_icon_media_r = ui_dec_icon("index_icon_media_r.png");
	index_icon_record_nor = ui_dec_icon("index_icon_record_nor.png");
	index_icon_record = ui_dec_icon("index_icon_record.png");
	index_icon_sw_p = ui_dec_icon("index_icon_sw_p.png");
	index_icon_sw_r = ui_dec_icon("index_icon_sw_r.png");
}

static void media_release_res(void) {
	ICON_RELEASE(index_icon_bg_r);
	ICON_RELEASE(index_icon_bg_p);
}

static void media_load_res(void) {
	media_release_res();

	index_icon_bg_r = ui_dec_icon("media_icon_bg_01.png");
	index_icon_bg_p = ui_dec_icon("media_icon_bg_02.png");
}

static void ui_font_release(void) {
	if (NULL != ttf_info_10.font) {
		lv_ft_font_destroy(ttf_info_10.font);
		ttf_info_10.font = NULL;
	}

	if (NULL != ttf_info_12.font) {
		lv_ft_font_destroy(ttf_info_12.font);
		ttf_info_12.font = NULL;
	}

	if (NULL != ttf_info_14.font) {
		lv_ft_font_destroy(ttf_info_14.font);
		ttf_info_14.font = NULL;
	}

	if (NULL != ttf_info_28.font) {
		lv_ft_font_destroy(ttf_info_28.font);
		ttf_info_28.font = NULL;
	}
}

static int32_t ui_font_load(void) {
	/*Create a font*/

	if (0 != ui_font_init(&ttf_info_10, 10))
		LV_LOG_ERROR("Title font initialization failed.");

	if (0 != ui_font_init(&ttf_info_12, 12))
		LV_LOG_ERROR("Content font initialization failed.");

	if (0 != ui_font_init(&ttf_info_14, 14))
		LV_LOG_ERROR("Content font initialization failed.");

	if (0 != ui_font_init(&ttf_info_28, 28))
		LV_LOG_ERROR("Content font initialization failed.");

	return 0;
}

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void ui_resource_release(void) {
	setting_release_res();
	view_release_res();
	media_release_res();

	ui_font_release();

}

void ui_resource_load(void) {
	ui_font_load();

	setting_load_res();
	view_load_res();
	media_load_res();
}
