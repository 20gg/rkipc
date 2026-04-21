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

#ifndef _UI_RES_MANAGE_H_
#define _UI_RES_MANAGE_H_

#include "lvgl.h"
#include "lvgl/common/lv_img_dec.h"

#ifdef __cplusplus
extern "C" {
#endif

extern lv_img_dsc_t *index_bg;
extern lv_img_dsc_t *index_icon_sw_p;
extern lv_img_dsc_t *index_icon_sw_r;
extern lv_img_dsc_t *index_icon_photo_p;
extern lv_img_dsc_t *index_icon_photo_r;
extern lv_img_dsc_t *index_icon_set_p;
extern lv_img_dsc_t *index_icon_set_r;
extern lv_img_dsc_t *index_icon_media_p;
extern lv_img_dsc_t *index_icon_media_r;
extern lv_img_dsc_t *index_icon_record_nor;
extern lv_img_dsc_t *index_icon_record;
extern lv_img_dsc_t *index_icon_slow_motion;

extern lv_img_dsc_t *index_icon_boxbg_r;
extern lv_img_dsc_t *index_icon_boxbg_p;
extern lv_img_dsc_t *index_icon_video;
extern lv_img_dsc_t *index_icon_capture;
extern lv_img_dsc_t *index_icon_store_01;
extern lv_img_dsc_t *index_icon_format_01;

extern lv_img_dsc_t *icon_record_stop_r;
extern lv_img_dsc_t *icon_record_stop_p;
extern lv_img_dsc_t *icon_record_start_r;
extern lv_img_dsc_t *icon_record_start_p;

extern lv_img_dsc_t *index_icon_bg_r;
extern lv_img_dsc_t *index_icon_bg_p;

void ui_resource_release(void);
void ui_resource_load(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
