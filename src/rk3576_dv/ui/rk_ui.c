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

#ifdef DRAW_UI_BY_VO
#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "rk_ui.c"

#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "common.h"
#include "disp.h"
#include <lvgl.h>

#include <rga/RgaApi.h>
#include <rga/rga.h>
#include <rk_mpi_mb.h>
#include <rk_mpi_mmz.h>
#include <rk_mpi_rgn.h>
#include <rk_mpi_sys.h>
#include <rk_mpi_tde.h>
#include <rk_mpi_venc.h>
#include <rk_mpi_vi.h>
#include <rk_mpi_vo.h>
#include <rk_mpi_vpss.h>

/**********************
 *      MACROS
 **********************/
#define DRM_CARD "/dev/dri/card0"
#define DRM_CONNECTOR_ID -1 /* -1 for the first connected one */

#define DBG_TAG "drm"

#define DIV_ROUND_UP(n, d) (((n) + (d)-1) / (d))

/**********************
 *  GLOBAL PROTOTYPES
 **********************/

/**********************
 *  GLOBAL VARIABLES
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
struct disp_buffer {
	VIDEO_FRAME_INFO_S frame_info;
	unsigned long int size;
	int32_t fd;
	void *map;
};

struct disp_dev {
	lv_disp_rot_t rot;
	struct disp_buffer *cur_bufs[2]; /* double buffering handling */
	struct disp_buffer bufs[2];
	uint32_t width, height;
	uint32_t mm_width, mm_height;
} disp_dev;

/**********************
 *  STATIC VARIABLES
 **********************/

static RK_U32 lcd_vo_layer = 5; // esmart 0
static int lcd_vo_dev_id = 1;   // mcu
static int lcd_vo_chn_id = 2;

/**********************
 *   STATIC FUNCTIONS
 **********************/
static int32_t drm_open(const char *path) {
	int32_t fd, flags;
	uint64_t has_dumb;
	int32_t ret;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		LOG_ERROR("cannot open \"%s\" \n", path);
		return -1;
	}

	/* set FD_CLOEXEC flag */
	if ((flags = fcntl(fd, F_GETFD)) < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
		LOG_ERROR("fcntl FD_CLOEXEC failed\n");
		goto err;
	}

	/* check capability */
	ret = drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb);
	if (ret < 0 || has_dumb == 0) {
		LOG_ERROR("drmGetCap DRM_CAP_DUMB_BUFFER failed or \"%s\" doesn't have dumb "
		          "buffer\n",
		          path);
		goto err;
	}

	return fd;
err:
	close(fd);
	return -1;
}

static int drm_find_connector(int32_t dev_fd) {
	drmModeConnector *conn = NULL;
	drmModeRes *res = NULL;
	int32_t i;

	if ((res = drmModeGetResources(dev_fd)) == NULL) {
		LOG_ERROR("drmModeGetResources() failed\n");
		return -1;
	}

	if (res->count_crtcs <= 0) {
		LOG_ERROR("no Crtcs\n");
		goto free_res;
	}

	/* find all available connectors */
	for (i = 0; i < res->count_connectors; i++) {
		conn = drmModeGetConnector(dev_fd, res->connectors[i]);
		if (!conn)
			continue;

#if DRM_CONNECTOR_ID >= 0
		if (conn->connector_id != DRM_CONNECTOR_ID) {
			drmModeFreeConnector(conn);
			continue;
		}
#endif

		if (conn->connection == DRM_MODE_CONNECTED) {
			LOG_DEBUG("drm: connector %d: connected\n", conn->connector_id);
		} else if (conn->connection == DRM_MODE_DISCONNECTED) {
			LOG_DEBUG("drm: connector %d: disconnected\n", conn->connector_id);
		} else if (conn->connection == DRM_MODE_UNKNOWNCONNECTION) {
			LOG_DEBUG("drm: connector %d: unknownconnection\n", conn->connector_id);
		} else {
			LOG_DEBUG("drm: connector %d: unknown\n", conn->connector_id);
		}

		if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
			break;

		drmModeFreeConnector(conn);
		conn = NULL;
	};

	if (!conn) {
		LOG_ERROR("suitable connector not found\n");
		goto free_res;
	}

	disp_dev.width = conn->modes[0].hdisplay;
	disp_dev.height = conn->modes[0].vdisplay;
	disp_dev.mm_width = conn->mmWidth;
	disp_dev.mm_height = conn->mmHeight;

	return 0;

free_res:
	drmModeFreeResources(res);

	return -1;
}

static int32_t get_disp_info(void) {
	int32_t dev_fd, ret;
	const char *device_path = NULL;

	device_path = getenv("DRM_CARD");
	if (!device_path)
		device_path = DRM_CARD;

	dev_fd = drm_open(device_path);
	if (dev_fd < 0)
		return -1;

	ret = drm_find_connector(dev_fd);
	if (ret) {
		LOG_ERROR("available drm devices not found\n");
		close(dev_fd);
		return -1;
	}

	close(dev_fd);

	LOG_INFO("rk_disp: %dx%d (%dmm X% dmm)\n", disp_dev.width, disp_dev.height, disp_dev.mm_width,
	         disp_dev.mm_height);

	return 0;
}

static int32_t rk_disp_setup(void) {
	int32_t ret;
	VO_PUB_ATTR_S VoPubAttr;
	VO_VIDEO_LAYER_ATTR_S stLayerAttr;
	VO_CSC_S VideoCSC;
	VO_CHN_ATTR_S VoChnAttr;
	RK_U32 u32DispBufLen;
	RK_U32 lcd_vo_w = rk_param_get_int("display:width", 240);
	RK_U32 lcd_vo_h = rk_param_get_int("display:height", 320);
	RK_U32 lcd_vo_layer = rk_param_get_int("display:layer_id", 5);
	int lcd_vo_dev_id = rk_param_get_int("display:dev_id", 1);
	int lcd_vo_chn_id = rk_param_get_int("display:ui_chn_id", 2);
	uint32_t fps = rk_param_get_int("display:fps", 30);
	const char *intf_type = rk_param_get_string("display:intf_type", "LCD");

	memset(&VoPubAttr, 0, sizeof(VO_PUB_ATTR_S));
	memset(&stLayerAttr, 0, sizeof(VO_VIDEO_LAYER_ATTR_S));
	memset(&VideoCSC, 0, sizeof(VO_CSC_S));
	memset(&VoChnAttr, 0, sizeof(VoChnAttr));
	if (!strcmp(intf_type, "LCD"))
		VoPubAttr.enIntfType = VO_INTF_LCD;
	else if (!strcmp(intf_type, "MIPI"))
		VoPubAttr.enIntfType = VO_INTF_MIPI;
	else if (!strcmp(intf_type, "DP"))
		VoPubAttr.enIntfType = VO_INTF_DP;
	else {
		LOG_ERROR("bad intf_type!\n");
		return -1;
	}
	VoPubAttr.enIntfSync = VO_OUTPUT_DEFAULT;

	ret = RK_MPI_VO_SetPubAttr(lcd_vo_dev_id, &VoPubAttr);
	if (ret)
		LOG_ERROR("RK_MPI_VO_SetPubAttr failed %#X\n", ret);
	ret = RK_MPI_VO_Enable(lcd_vo_dev_id);
	if (ret)
		LOG_ERROR("RK_MPI_VO_Enable failed %#X\n", ret);
	RK_MPI_VO_GetLayerDispBufLen(lcd_vo_layer, &u32DispBufLen);
	LOG_INFO("Get lcd_vo_layer %d disp buf len is %d.\n", lcd_vo_layer, u32DispBufLen);
	u32DispBufLen = 3;
	ret = RK_MPI_VO_SetLayerDispBufLen(lcd_vo_layer, u32DispBufLen);
	LOG_INFO("Agin Get lcd_vo_layer %d disp buf len is %d.\n", lcd_vo_layer, u32DispBufLen);

	ret = RK_MPI_VO_GetPubAttr(lcd_vo_dev_id, &VoPubAttr);
	if ((VoPubAttr.stSyncInfo.u16Hact == 0) || (VoPubAttr.stSyncInfo.u16Vact == 0)) {
		VoPubAttr.stSyncInfo.u16Hact = lcd_vo_w;
		VoPubAttr.stSyncInfo.u16Vact = lcd_vo_h;
	}

	stLayerAttr.stDispRect.s32X = 0;
	stLayerAttr.stDispRect.s32Y = 0;
	stLayerAttr.stDispRect.u32Width = lcd_vo_w;
	stLayerAttr.stDispRect.u32Height = lcd_vo_h;
	stLayerAttr.stImageSize.u32Width = lcd_vo_w;
	stLayerAttr.stImageSize.u32Height = lcd_vo_h;
	LOG_INFO("stLayerAttr W=%d, H=%d\n", stLayerAttr.stDispRect.u32Width,
	         stLayerAttr.stDispRect.u32Height);

	stLayerAttr.u32DispFrmRt = fps;
	stLayerAttr.enPixFormat = RK_FMT_RGB888;
	VideoCSC.enCscMatrix = VO_CSC_MATRIX_IDENTITY;
	VideoCSC.u32Contrast = 50;
	VideoCSC.u32Hue = 50;
	VideoCSC.u32Luma = 50;
	VideoCSC.u32Satuature = 50;

	/*bind layer0 to device hd0*/
	ret = RK_MPI_VO_BindLayer(lcd_vo_layer, lcd_vo_dev_id, VO_LAYER_MODE_GRAPHIC);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("failed to bind layer\n");
		return ret;
	}
	ret = RK_MPI_VO_SetLayerAttr(lcd_vo_layer, &stLayerAttr);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("failed to set layer attr\n");
		return ret;
	}
	ret = RK_MPI_VO_SetLayerSpliceMode(lcd_vo_layer, VO_SPLICE_MODE_RGA);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("failed to set layer splice mode\n");
		return ret;
	}
	ret = RK_MPI_VO_EnableLayer(lcd_vo_layer);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("failed to enable layer\n");
		return ret;
	}
	ret = RK_MPI_VO_SetLayerCSC(lcd_vo_layer, &VideoCSC);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("failed to set layer csc\n");
		return ret;
	}

	VoChnAttr.bDeflicker = RK_FALSE;
	VoChnAttr.u32Priority = rk_param_get_int("display:ui_chn_priority", 3);
	VoChnAttr.stRect.s32X = 0;
	VoChnAttr.stRect.s32Y = 0;
	VoChnAttr.stRect.u32Width = stLayerAttr.stDispRect.u32Width;
	VoChnAttr.stRect.u32Height = stLayerAttr.stDispRect.u32Height;
	VoChnAttr.enRotation = 0;
	ret = RK_MPI_VO_SetChnAttr(lcd_vo_layer, lcd_vo_chn_id, &VoChnAttr);
	ret = RK_MPI_VO_EnableChn(lcd_vo_layer, lcd_vo_chn_id);
	if (ret != RK_SUCCESS) {
		LOG_ERROR("create %d layer %d ch vo failed!\n", lcd_vo_layer, lcd_vo_chn_id);
		return ret;
	}
	LOG_INFO("rk_disp: ui created successfully.\n");

	return 0;
}

static void rk_disp_teardown(void) {
	int ret = 0;
	ret = RK_MPI_VO_DisableChn(lcd_vo_layer, lcd_vo_chn_id);
	if (ret) {
		LOG_ERROR("RK_MPI_VO_DisableChn failed, ret is %#x\n", ret);
		return;
	}
	ret = RK_MPI_VO_DisableLayer(lcd_vo_layer);
	if (ret) {
		LOG_ERROR("RK_MPI_VO_DisableLayer failed, ret is %#x\n", ret);
		return;
	}
	ret = RK_MPI_VO_Disable(lcd_vo_dev_id);
	if (ret) {
		LOG_ERROR("RK_MPI_VO_Disable failed, ret is %#x\n", ret);
		return;
	}
	ret = RK_MPI_VO_UnBindLayer(lcd_vo_layer, lcd_vo_dev_id);
	if (ret) {
		LOG_ERROR("RK_MPI_VO_UnBindLayer failed, ret is %#x\n", ret);
		return;
	}
	LOG_INFO("rk_disp: ui destroyed successfully.\n");
}

static int32_t rk_disp_setup_buffers(void) {
	int32_t ret;
	uint32_t i, size;
	void *blk = NULL;
	PIXEL_FORMAT_E format;

	if (LV_COLOR_DEPTH == 32) {
		format = RK_FORMAT_ARGB_8888;
	} else {
		format = 0;
		LOG_ERROR("drm_flush rga not supported format\n");
		return -1;
	}

	size = disp_dev.width * disp_dev.height * (LV_COLOR_SIZE / 8);
	for (i = 0; i < sizeof(disp_dev.bufs) / sizeof(disp_dev.bufs[0]); i++) {
		ret = RK_MPI_MMZ_Alloc(&blk, size, RK_MMZ_ALLOC_CACHEABLE);
		if (0 != ret) {
			LOG_ERROR("alloc failed!");
			break;
		}

		disp_dev.bufs[i].frame_info.stVFrame.enPixelFormat = format;
		disp_dev.bufs[i].frame_info.stVFrame.pMbBlk = blk;
		disp_dev.bufs[i].frame_info.stVFrame.u32Width = disp_dev.width;
		disp_dev.bufs[i].frame_info.stVFrame.u32Height = disp_dev.height;
		disp_dev.bufs[i].size = size;
		disp_dev.bufs[i].map = RK_MPI_MMZ_Handle2VirAddr(blk);
		disp_dev.bufs[i].fd = RK_MPI_MMZ_Handle2Fd(blk);
		memset(disp_dev.bufs[i].map, 0x00, size);
		LOG_INFO("buffer mb %p, w %d, height %d, format %d, fd %d, viraddr %p\n", blk,
		         disp_dev.width, disp_dev.height, format, disp_dev.bufs[i].fd,
		         disp_dev.bufs[i].map);
	}

	/* Set buffering handling */
	disp_dev.cur_bufs[0] = NULL;
	disp_dev.cur_bufs[1] = &disp_dev.bufs[0];

	return 0;
}

static void rk_disp_teardown_buffers(void) {
	for (uint32_t i = 0; i < sizeof(disp_dev.bufs) / sizeof(disp_dev.bufs[0]); i++) {
		if (NULL != disp_dev.bufs[i].frame_info.stVFrame.pMbBlk) {
			RK_MPI_MMZ_Free(disp_dev.bufs[i].frame_info.stVFrame.pMbBlk);
			memset(&disp_dev.bufs[i], 0, sizeof(disp_dev.bufs[i]));
			LOG_INFO("rk_disp: ui bufs[%d] released successfully.\n", i);
		}
	}
}

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
int32_t rk_disp_init(lv_disp_rot_t rotate_disp) {
	int32_t ret;

	ret = get_disp_info();
	if (0 != ret) {
		LOG_ERROR("get display info failed\n");
		return -1;
	}

	ret = rk_disp_setup();
	if (0 != ret) {
		LOG_ERROR("rk_disp_setup failed");
		return -1;
	}

	ret = rk_disp_setup_buffers();
	if (0 != ret) {
		LOG_ERROR("Allocating display buffer failed\n");
		goto err;
	}

	disp_dev.rot = rotate_disp;

	return 0;
err:
	rk_disp_teardown();

	return -1;
}

void rk_disp_exit(void) {
	rk_disp_teardown_buffers();
	rk_disp_teardown();
}

static void draw_buf_rotate_90(lv_color_t *color_p, const lv_area_t *area, lv_color_t *dst_buf,
                               lv_coord_t canvas_w, lv_coord_t canvas_h) {
	lv_coord_t area_w = (area->x2 - area->x1 + 1);
	lv_coord_t area_h = (area->y2 - area->y1 + 1);
	uint32_t initial_i = area->x1 * canvas_w + (canvas_w - area->y1 - 1);
	for (lv_coord_t y = 0; y < area_h; y++) {
		uint32_t i = initial_i - y;
		for (lv_coord_t x = 0; x < area_w; x++) {
			dst_buf[i] = *(color_p++);
			i += canvas_w;
		}
	}
}

void rk_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
	int32_t format = 0;
	int ret = 0;
	bool partial_update = false;
	struct disp_buffer *fbuf = disp_dev.cur_bufs[1];
	lv_coord_t w = (area->x2 - area->x1 + 1);
	lv_coord_t h = (area->y2 - area->y1 + 1);

	LOG_DEBUG("x %d:%d y %d:%d w %d h %d\n", area->x1, area->x2, area->y1, area->y2, w, h);

	if (LV_COLOR_DEPTH == 16) {
		format = RK_FORMAT_RGB_565;
	} else if (LV_COLOR_DEPTH == 32) {
		format = RK_FORMAT_ARGB_8888;
	} else {
		format = -1;
		LOG_INFO("drm_flush rga not supported format\n");
		return;
	}

	rga_info_t src;
	rga_info_t dst;

	/* Partial update */
	if (disp_dev.rot == LV_DISP_ROT_90 || disp_dev.rot == LV_DISP_ROT_270) {
		if ((w != disp_dev.height || h != disp_dev.width) && disp_dev.cur_bufs[0])
			partial_update = true;
	} else {
		if ((w != disp_dev.width || h != disp_dev.height) && disp_dev.cur_bufs[0])
			partial_update = true;
	}

	if (true == partial_update) {
		memset(&src, 0, sizeof(rga_info_t));
		memset(&dst, 0, sizeof(rga_info_t));

		src.fd = disp_dev.cur_bufs[0]->fd;
		src.mmuFlag = 1;
		dst.fd = fbuf->fd;
		dst.mmuFlag = 1;
		rga_set_rect(&src.rect, 0, 0, disp_dev.width, disp_dev.height, disp_dev.width,
		             disp_dev.height, format);
		rga_set_rect(&dst.rect, 0, 0, disp_dev.width, disp_dev.height, disp_dev.width,
		             disp_dev.height, format);
		if (c_RkRgaBlit(&src, &dst, NULL))
			LOG_INFO("c_RkRgaBlit2 error : %s\n", strerror(errno));
	}

	memset(&src, 0, sizeof(rga_info_t));
	memset(&dst, 0, sizeof(rga_info_t));

	if (disp_dev.rot == LV_DISP_ROT_90 || disp_dev.rot == LV_DISP_ROT_270) {
		if (w < 2 || h < 2) {
			draw_buf_rotate_90(color_p, area, fbuf->map, disp_dev.width, disp_dev.height);
		} else {
			src.virAddr = color_p;
			src.mmuFlag = 1;
			src.rotation = HAL_TRANSFORM_ROT_90;
			dst.fd = fbuf->fd;
			dst.mmuFlag = 1;
			rga_set_rect(&src.rect, 0, 0, w, h, w, h, format);
			rga_set_rect(&dst.rect, (disp_dev.width - (area->y1 + h)), area->x1, h, w,
			             disp_dev.width, disp_dev.height, format);
			if (c_RkRgaBlit(&src, &dst, NULL))
				LOG_INFO("c_RkRgaBlit2 error : %s\n", strerror(errno));
		}
	} else {
		if (w < 2 || h < 2) {
			for (uint32_t y = 0, i = area->y1; i <= area->y2; ++i, ++y) {
				memcpy((uint8_t *)fbuf->map + (area->x1 + disp_dev.width * i) * (LV_COLOR_SIZE / 8),
				       (uint8_t *)color_p + (w * (LV_COLOR_SIZE / 8) * y), w * (LV_COLOR_SIZE / 8));
			}
		} else {
			src.virAddr = color_p;
			src.mmuFlag = 1;
			dst.fd = fbuf->fd;
			dst.mmuFlag = 1;
			rga_set_rect(&src.rect, 0, 0, w, h, w, h, format);
			rga_set_rect(&dst.rect, area->x1, area->y1, w, h, disp_dev.width, disp_dev.height,
			             format);
			if (c_RkRgaBlit(&src, &dst, NULL))
				LOG_INFO("c_RkRgaBlit2 error : %s\n", strerror(errno));
		}
	}

	/* show fbuf plane */
	RK_MPI_SYS_MmzFlushCache(fbuf->frame_info.stVFrame.pMbBlk, RK_FALSE);

	VIDEO_FRAME_INFO_S stVoVFrame;
	memset(&stVoVFrame, 0, sizeof(VIDEO_FRAME_INFO_S));

	stVoVFrame.stVFrame.u32Width = fbuf->frame_info.stVFrame.u32Width;
	stVoVFrame.stVFrame.u32Height = fbuf->frame_info.stVFrame.u32Height;
	stVoVFrame.stVFrame.u32VirWidth = fbuf->frame_info.stVFrame.u32Width;
	stVoVFrame.stVFrame.u32VirHeight = fbuf->frame_info.stVFrame.u32Height;
	stVoVFrame.stVFrame.enPixelFormat = RK_FMT_BGRA8888;
	stVoVFrame.stVFrame.pMbBlk = fbuf->frame_info.stVFrame.pMbBlk;

	ret = RK_MPI_VO_SendFrame(lcd_vo_layer, lcd_vo_chn_id, &stVoVFrame, 1000);
	if (ret != 0)
		LOG_ERROR("RK_MPI_VO_SendFrame failed %#X\n", ret);
	if (!disp_dev.cur_bufs[0])
		disp_dev.cur_bufs[1] = &disp_dev.bufs[1];
	else
		disp_dev.cur_bufs[1] = disp_dev.cur_bufs[0];

	disp_dev.cur_bufs[0] = fbuf;

	lv_disp_flush_ready(disp_drv);
}

void rk_disp_get_sizes(lv_coord_t *width, lv_coord_t *height, uint32_t *dpi) {
	if (width)
		*width = disp_dev.width;

	if (height)
		*height = disp_dev.height;

	if (dpi && disp_dev.mm_width)
		*dpi = DIV_ROUND_UP(disp_dev.width * 25400, disp_dev.mm_width * 1000);

	return;
}

disp_ops_t rk_ui_ops = {
    rk_disp_init,
    rk_disp_exit,
    rk_disp_get_sizes,
    rk_disp_flush,
};
#endif // DRAW_UI_BY_VO
