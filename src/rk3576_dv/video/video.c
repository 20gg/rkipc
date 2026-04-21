// Copyright 2025 Rockchip Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "video.h"
#include "list.h"
#include "rk_mpi_gdc.h"
#include "rk_sysfs.h"
#include <linux/media.h>
#include <linux/v4l2-mediabus.h>
#include <linux/v4l2-subdev.h>
#include <rk_mpi_vdec.h>
#include <stdint.h>
#include <sys/types.h>
#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "video.c"

#define VIDEO_PIPE_NORMAL 0
#define JPEG_VENC_CHN 3

#define RK3576_VO_DEV_HDMI 0
#define RK3576_VO_DEV_MIPI 1
#define RK3576_VO_DEV_LCD 1
#define RK3576_VO_DEV_DP 2
#define RK3576_VOP_LAYER_CLUSTER0 0
#define RK3576_VOP_LAYER_ESMART0 4
#define RK3576_VOP_LAYER_ESMART1 5
#define RK3576_VOP_LAYER_ESMART2 6

#define RTSP_URL_0 "/live/0"
#define RTSP_URL_1 "/live/1"
#define RTSP_URL_2 "/live/2"
#define RTMP_URL_0 "rtmp://127.0.0.1:1935/live/mainstream"
#define RTMP_URL_1 "rtmp://127.0.0.1:1935/live/substream"
#define RTMP_URL_2 "rtmp://127.0.0.1:1935/live/thirdstream"

static RK_MODE_E g_current_mode = RK_VIDEO_MODE;
static RK_EIS_MODE_E g_current_eis_mode = RK_NORMAL_STEADY;

static int pipe_id_ = 0;
static int g_vi_chn_id = 0;
static int g_vi_ext_chn_id = 4; // 3:vpss_scale_1 4:vpss_scale_0 5:vpss_scale_2 6:vpss_scale_3
static int g_vi_for_vo_chn_id = 1;

static int max_capture_num = 5;
static int remain_capture_num = 0;
static int enable_npu, enable_gdc, enable_dp, enable_lcd, enable_eis_debug, enable_hdr;
static int g_enable_vo;
static int dp_vo_dev_id = RK3576_VO_DEV_DP;
static int lcd_vo_dev_id = RK3576_VO_DEV_LCD;
static int g_video_run_ = 1;
static int g_start_record_ = 0;
static const char *tmp_output_data_type = "H.264";
static const char *tmp_rc_mode;
static const char *tmp_h264_profile;
static const char *tmp_smart;
static const char *tmp_gop_mode;
static const char *tmp_rc_quality;
static pthread_t venc_thread_normal, venc_thread_slowmotion, venc_thread_timelapse,
    jpeg_venc_thread_id;

static MPP_CHN_S vi_chn[2], gdc_chn[2], vi_for_vo_chn, vo_chn[2], venc_chn[4];
static VO_DEV dp_vo_layer, lcd_vo_layer;
static GDC_INFO_CB_S gdc_info_cb;
static int dp_vo_w = 1920;
static int dp_vo_h = 1080;
static int lcd_vo_w = 240;
static int lcd_vo_h = 320;
static int gdc_display_w = 704;
static int gdc_display_h = 512;
static FILE *vi_info_file;
static FILE *imu_info_file;
static char gdc_vi_info_buffer[256];
static char gdc_imu_info_buffer[256];
static int gdc_debug_cnt = 0;

#define SUBDEV_PATH "/dev/v4l-subdev3" // 根据实际设备节点修改
#define FIXED_PAD 0
#define FIXED_FORMAT MEDIA_BUS_FMT_SGRBG10_1X10

static int set_camera_format(void) {
	uint32_t width = rk_param_get_int("video.source:width", -1);
	uint32_t height = rk_param_get_int("video.source:height", -1);
	uint32_t fps = rk_param_get_int("isp.0.adjustment:fps", 30);
	int fd = open(SUBDEV_PATH, O_RDWR);
	if (fd < 0) {
		LOG_ERROR("Failed to open subdev\n");
		return -1;
	}

	struct v4l2_subdev_format fmt = {.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	                                 .pad = FIXED_PAD,
	                                 .format = {.width = width,
	                                            .height = height,
	                                            .code = FIXED_FORMAT,
	                                            .field = V4L2_FIELD_NONE,
	                                            .colorspace = V4L2_COLORSPACE_RAW}};
	struct v4l2_subdev_frame_interval ival = {.pad = FIXED_PAD,
	                                          .interval = {.numerator = 1, .denominator = fps}};

	if (ioctl(fd, VIDIOC_SUBDEV_S_FMT, &fmt) < 0) {
		LOG_ERROR("Failed to set format\n");
		close(fd);
		return -1;
	}
	if (ioctl(fd, VIDIOC_SUBDEV_S_FRAME_INTERVAL, &ival) < 0) {
		LOG_ERROR("Failed to set FPS\n");
		close(fd);
		return -1;
	}
	close(fd);
	LOG_INFO("update sensor setting width %d height %d fps %d\n", width, height, fps);
	return 0;
}

static void set_soc_freq(void) {
	uint32_t ddr_freq = rk_param_get_int("common:ddr_freq", -1);
	uint32_t cpu_freq = rk_param_get_int("common:cpu_freq", -1);
	write_sysfs_int("scaling_setspeed", "/sys/devices/system/cpu/cpu0/cpufreq/", cpu_freq);
	write_sysfs_int("set_freq", "/sys/class/devfreq/dmc/userspace/", ddr_freq);
	LOG_INFO("set ddr freq %d, cpu freq %d\n", ddr_freq, cpu_freq);
}

static void *rkipc_video_loop(void *arg) {
	printf("#Start %s thread, arg:%p\n", __func__, arg);
	VENC_STREAM_S stFrame;
	VI_CHN_STATUS_S stChnStatus;
	int loopCount = 0;
	int ret = 0;
	FILE *fp = fopen("/tmp/venc.h265", "wb");
	stFrame.pstPack = malloc(sizeof(VENC_PACK_S));
	long long before_time = 0;

	while (g_start_record_) {
		// 5.get the frame
		ret = RK_MPI_VENC_GetStream(VIDEO_PIPE_NORMAL, &stFrame, 1000);
		if (ret == RK_SUCCESS) {
			before_time = rkipc_get_curren_time_ms();
			void *data = RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
			if (enable_eis_debug) {
				fwrite(data, 1, stFrame.pstPack->u32Len, fp);
				fflush(fp);
			}
			// LOG_INFO("seq:%d, Len:%d, PTS is %" PRId64", enH264EType is %d\n", stFrame.u32Seq,
			// stFrame.pstPack->u32Len, stFrame.pstPack->u64PTS,
			// stFrame.pstPack->DataType.enH264EType);

			rkipc_rtsp_write_video_frame(0, data, stFrame.pstPack->u32Len, stFrame.pstPack->u64PTS);
			if ((stFrame.pstPack->DataType.enH264EType == H264E_NALU_IDRSLICE) ||
			    (stFrame.pstPack->DataType.enH264EType == H264E_NALU_ISLICE) ||
			    (stFrame.pstPack->DataType.enH265EType == H265E_NALU_IDRSLICE) ||
			    (stFrame.pstPack->DataType.enH265EType == H265E_NALU_ISLICE)) {
				rk_storage_write_video_frame(0, data, stFrame.pstPack->u32Len,
				                             stFrame.pstPack->u64PTS, 1);
				// rk_rtmp_write_video_frame(0, data, stFrame.pstPack->u32Len,
				// stFrame.pstPack->u64PTS,
				//                           1);
			} else {
				rk_storage_write_video_frame(0, data, stFrame.pstPack->u32Len,
				                             stFrame.pstPack->u64PTS, 0);
				// rk_rtmp_write_video_frame(0, data, stFrame.pstPack->u32Len,
				// stFrame.pstPack->u64PTS,
				//                           0);
			}
			if (rkipc_get_curren_time_ms() - before_time > 100)
				LOG_INFO("write cost time is %lldms\n", rkipc_get_curren_time_ms() - before_time);
			LOG_DEBUG("Count:%d, Len:%d, PTS is %" PRId64 ", enH264EType is %d, ref type %d\n",
			          loopCount, stFrame.pstPack->u32Len, stFrame.pstPack->u64PTS,
			          stFrame.pstPack->DataType.enH265EType, stFrame.stH265Info.enRefType);

			// 7.release the frame
			ret = RK_MPI_VENC_ReleaseStream(VIDEO_PIPE_NORMAL, &stFrame);
			if (ret != RK_SUCCESS) {
				LOG_ERROR("RK_MPI_VENC_ReleaseStream fail %x\n", ret);
			}
			loopCount++;
		} else {
			LOG_ERROR("RK_MPI_VENC_GetStream timeout %x\n", ret);
		}
	}
	if (stFrame.pstPack)
		free(stFrame.pstPack);
	if (fp)
		fclose(fp);

	return 0;
}

static void *rkipc_slowmotion_loop(void *arg) {
	printf("#Start %s thread, arg:%p\n", __func__, arg);
	VENC_STREAM_S stFrame;
	VI_CHN_STATUS_S stChnStatus;
	RK_U64 modified_pts = 0;
	RK_U64 step = 33333; // isp 120fps output -> storage by 30fps
	int loopCount = 0;
	int ret = 0;
	FILE *fp = fopen("/tmp/venc.h265", "wb");
	stFrame.pstPack = malloc(sizeof(VENC_PACK_S));

	while (g_start_record_) {
		// 5.get the frame
		ret = RK_MPI_VENC_GetStream(VIDEO_PIPE_NORMAL, &stFrame, 1000);
		if (ret == RK_SUCCESS) {
			if (modified_pts)
				modified_pts += step;
			else
				modified_pts = stFrame.pstPack->u64PTS;
			stFrame.pstPack->u64PTS = modified_pts;
			void *data = RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
			if (enable_eis_debug) {
				fwrite(data, 1, stFrame.pstPack->u32Len, fp);
				fflush(fp);
			}
			LOG_DEBUG("Count:%d, Len:%d, PTS is %" PRId64 ", enH264EType is %d\n", loopCount,
			          stFrame.pstPack->u32Len, stFrame.pstPack->u64PTS,
			          stFrame.pstPack->DataType.enH264EType);
			if ((stFrame.pstPack->DataType.enH264EType == H264E_NALU_IDRSLICE) ||
			    (stFrame.pstPack->DataType.enH264EType == H264E_NALU_ISLICE) ||
			    (stFrame.pstPack->DataType.enH265EType == H265E_NALU_IDRSLICE) ||
			    (stFrame.pstPack->DataType.enH265EType == H265E_NALU_ISLICE)) {
				rk_storage_write_video_frame(0, data, stFrame.pstPack->u32Len,
				                             stFrame.pstPack->u64PTS, 1);
			} else {
				rk_storage_write_video_frame(0, data, stFrame.pstPack->u32Len,
				                             stFrame.pstPack->u64PTS, 0);
			}
			// 7.release the frame
			ret = RK_MPI_VENC_ReleaseStream(VIDEO_PIPE_NORMAL, &stFrame);
			if (ret != RK_SUCCESS)
				LOG_ERROR("RK_MPI_VENC_ReleaseStream fail %x\n", ret);
			loopCount++;
		} else {
			LOG_ERROR("RK_MPI_VENC_GetStream timeout %x\n", ret);
		}
	}
	if (stFrame.pstPack)
		free(stFrame.pstPack);
	if (fp)
		fclose(fp);

	return 0;
}

#define MAX_STREAM_CACHE_LEN (512 * 1024 * 1024)

struct stream_cache_packet {
	uint32_t offset;
	uint32_t size;
	struct list_head list;
};

struct stream_cache {
	void *data;
	uint32_t used_size;
	uint32_t total_size;
	uint32_t packet_num;
	struct list_head packet_list;
};

static bool need_save_stream(VENC_STREAM_S *frame) {
	if (!frame)
		return false;
	if ((frame->pstPack->DataType.enH264EType == H264E_NALU_IDRSLICE) ||
	    (frame->pstPack->DataType.enH264EType == H264E_NALU_ISLICE) ||
	    (frame->pstPack->DataType.enH265EType == H265E_NALU_IDRSLICE) ||
	    (frame->pstPack->DataType.enH265EType == H265E_NALU_ISLICE))
		return true;
	if ((!strcmp(tmp_gop_mode, "smartP")) &&
	    ((frame->stH264Info.enRefType == BASE_PSLICE_REFTOIDR) ||
	     (frame->stH265Info.enRefType == BASE_PSLICE_REFTOIDR)))
		return true;
	return false;
}

static void drop_stream_cache(struct stream_cache *cache) {
	struct stream_cache_packet *packet = NULL;
	struct list_head *cur = NULL, *tmp = NULL;
	int step = 0, ret = 0, i = 0;
	int gop = rk_param_get_int("video.0:gop", 15);
	int fps = rk_param_get_int("isp.0.adjustment:fps", 30);
	RK_U64 pts = 0;

	LOG_INFO("enter\n");
	if ((1.0 / fps * gop) * cache->packet_num >= 30 * 60) // 30min
		step = 4;
	else if ((1.0 / fps * gop) * cache->packet_num >= 10 * 60) // 10min
		step = 2;
	else
		step = 1;
	LOG_INFO("fps %d, gop %d, step %d\n", fps, gop, step);
	list_for_each_safe(cur, tmp, &cache->packet_list) {
		packet = container_of(cur, struct stream_cache_packet, list);
		if ((i % step) == 0) {
			pts += 33333; // force 30fps output
			rk_storage_write_video_frame(0, cache->data + packet->offset, packet->size, pts, 1);
			LOG_INFO("save frame pts %lld, len %d, index %d\n", pts, packet->size, i);
		}
		++i;
		LOG_DEBUG("frame pts %lld, len %d\n", pts, packet->size);
		list_del(cur);
		free(packet);
	}
	free(cache->data);
	free(cache);
	LOG_INFO("exit\n");
}

static void *create_new_stream_cache(void) {
	struct stream_cache *cache = NULL;
	cache = malloc(sizeof(*cache));
	memset(cache, 0, sizeof(*cache));
	cache->data = malloc(MAX_STREAM_CACHE_LEN);
	cache->total_size = MAX_STREAM_CACHE_LEN;
	INIT_LIST_HEAD(&cache->packet_list);
	return cache;
}

static void *rkipc_timelapse_loop(void *arg) {
	printf("#Start %s thread, arg:%p\n", __func__, arg);
	VENC_STREAM_S stFrame;
	struct stream_cache *cache;
	struct stream_cache_packet *packet = NULL;
	int loopCount = 0;
	int ret = 0;
	int enable_stream_cache = rk_param_get_int("video.0:enable_stream_cache", 1);
	long long before_time = 0, cost_time = 0;
	RK_U64 pts = 0;
	FILE *fp = fopen("/tmp/venc.h265", "wb");

	before_time = rkipc_get_curren_time_ms();
	stFrame.pstPack = malloc(sizeof(VENC_PACK_S));
	if (enable_stream_cache)
		cache = create_new_stream_cache();
	while (g_start_record_) {
		// 5.get the frame
		ret = RK_MPI_VENC_GetStream(VIDEO_PIPE_NORMAL, &stFrame, 1000);
		if (ret == RK_SUCCESS) {
			void *data = RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
			if (enable_eis_debug) {
				fwrite(data, 1, stFrame.pstPack->u32Len, fp);
				fflush(fp);
			}
			if (need_save_stream(&stFrame)) {
				if (enable_stream_cache) {
					if (cache->total_size - cache->used_size < stFrame.pstPack->u32Len) {
						LOG_WARN("cache is full\n");
						drop_stream_cache(cache);
						cache = create_new_stream_cache();
					}
					packet = malloc(sizeof(*packet));
					memset(packet, 0, sizeof(*packet));
					packet->size = stFrame.pstPack->u32Len;
					packet->offset = cache->used_size;
					INIT_LIST_HEAD(&packet->list);

					memcpy(cache->data + packet->offset, data, packet->size);
					list_add_tail(&packet->list, &cache->packet_list);
					cache->packet_num += 1;
					cache->used_size += packet->size;
				} else {
					if (!pts)
						pts = stFrame.pstPack->u64PTS;
					else
						pts += 33333;
					rk_storage_write_video_frame(0, data, stFrame.pstPack->u32Len,
					                             stFrame.pstPack->u64PTS, 1);
				}
				LOG_DEBUG("Count:%d, Len:%d, PTS is %" PRId64 ", enH264EType is %d, ref type %d\n",
				          loopCount, stFrame.pstPack->u32Len, stFrame.pstPack->u64PTS,
				          stFrame.pstPack->DataType.enH265EType, stFrame.stH265Info.enRefType);
			}
			// 7.release the frame
			ret = RK_MPI_VENC_ReleaseStream(VIDEO_PIPE_NORMAL, &stFrame);
			if (ret != RK_SUCCESS)
				LOG_ERROR("RK_MPI_VENC_ReleaseStream fail %x\n", ret);
			loopCount++;
		} else {
			LOG_ERROR("RK_MPI_VENC_GetStream timeout %x\n", ret);
		}
	}
	if (stFrame.pstPack)
		free(stFrame.pstPack);
	if (fp)
		fclose(fp);
	cost_time = rkipc_get_curren_time_ms() - before_time;
	LOG_INFO("total record time %lld ms\n", cost_time);
	if (enable_stream_cache)
		drop_stream_cache(cache);
	return 0;
}

static void *rkipc_get_jpeg(void *arg) {
	printf("#Start %s thread, arg:%p\n", __func__, arg);
	VENC_STREAM_S stFrame;
	VI_CHN_STATUS_S stChnStatus;
	int loopCount = 0;
	int ret = 0;
	char file_name[128] = {0};
	const char *mount_path = rk_param_get_string("storage:mount_path", "/mnt/sdcard");
	const char *photo_folder_name = rk_param_get_string("storage.3:folder_name", "photo");
	stFrame.pstPack = malloc(sizeof(VENC_PACK_S));

	while (g_video_run_) {
		if (remain_capture_num <= 0) {
			usleep(300 * 1000);
			continue;
		}
		// 5.get the frame
		ret = RK_MPI_VENC_GetStream(JPEG_VENC_CHN, &stFrame, 1000);
		if (ret == RK_SUCCESS) {
			void *data = RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
			LOG_INFO("Count:%d, Len:%d, PTS is %" PRId64 ", enH264EType is %d\n", loopCount,
			         stFrame.pstPack->u32Len, stFrame.pstPack->u64PTS,
			         stFrame.pstPack->DataType.enH264EType);
			// save jpeg file
			time_t t = time(NULL);
			struct tm tm = *localtime(&t);
			snprintf(file_name, 128, "%s/%s/%d%02d%02d%02d%02d%02d_%d.jpeg", mount_path,
			         photo_folder_name, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
			         tm.tm_min, tm.tm_sec, max_capture_num - remain_capture_num);
			LOG_INFO("file_name is %s\n", file_name);
			FILE *fp = fopen(file_name, "wb");
			fwrite(data, 1, stFrame.pstPack->u32Len, fp);
			fflush(fp);
			fclose(fp);
			--remain_capture_num;
			// 7.release the frame
			ret = RK_MPI_VENC_ReleaseStream(JPEG_VENC_CHN, &stFrame);
			if (ret != RK_SUCCESS) {
				LOG_ERROR("RK_MPI_VENC_ReleaseStream fail %x\n", ret);
			}
			loopCount++;
		} else {
			LOG_ERROR("RK_MPI_VENC_GetStream timeout %x\n", ret);
		}
		usleep(100 * 1000);
	}
	if (stFrame.pstPack)
		free(stFrame.pstPack);

	return 0;
}

static int rkipc_rtmp_init() {
	int ret = 0;
	ret |= rk_rtmp_init(0, RTMP_URL_0);
	ret |= rk_rtmp_init(1, RTMP_URL_1);
	ret |= rk_rtmp_init(2, RTMP_URL_2);

	return ret;
}

static int rkipc_rtmp_deinit() {
	int ret = 0;
	ret |= rk_rtmp_deinit(0);
	ret |= rk_rtmp_deinit(1);
	ret |= rk_rtmp_deinit(2);

	return ret;
}

static int rkipc_vi_dev_init() {
	LOG_INFO("%s\n", __func__);
	int ret = 0;
	VI_DEV_ATTR_S stDevAttr;
	VI_DEV_BIND_PIPE_S stBindPipe;
	memset(&stDevAttr, 0, sizeof(stDevAttr));
	memset(&stBindPipe, 0, sizeof(stBindPipe));
	// 0. get dev config status
	ret = RK_MPI_VI_GetDevAttr(pipe_id_, &stDevAttr);
	if (ret == RK_ERR_VI_NOT_CONFIG) {
		// 0-1.config dev
		ret = RK_MPI_VI_SetDevAttr(pipe_id_, &stDevAttr);
		if (ret != RK_SUCCESS) {
			LOG_ERROR("RK_MPI_VI_SetDevAttr %x\n", ret);
			return -1;
		}
	} else {
		LOG_ERROR("RK_MPI_VI_SetDevAttr already\n");
	}
	// 1.get dev enable status
	ret = RK_MPI_VI_GetDevIsEnable(pipe_id_);
	if (ret != RK_SUCCESS) {
		// 1-2.enable dev
		ret = RK_MPI_VI_EnableDev(pipe_id_);
		if (ret != RK_SUCCESS) {
			LOG_ERROR("RK_MPI_VI_EnableDev %x\n", ret);
			return -1;
		}
		// 1-3.bind dev/pipe
		stBindPipe.u32Num = pipe_id_;
		stBindPipe.PipeId[0] = pipe_id_;
		ret = RK_MPI_VI_SetDevBindPipe(pipe_id_, &stBindPipe);
		if (ret != RK_SUCCESS) {
			LOG_ERROR("RK_MPI_VI_SetDevBindPipe %x\n", ret);
			return -1;
		}
	} else {
		LOG_ERROR("RK_MPI_VI_EnableDev already\n");
	}
	VI_PARAM_MOD_S stModParam;
	if (g_current_eis_mode == RK_HORIZON_STEADY) {
		memset(&stModParam, 0, sizeof(stModParam));
		// INFO: vblank is too short to support isp online mode for 4032*3016 resolution
		stModParam.enViModType = VI_DEV_PIPE_MODE;
		stModParam.stDevPipeModParam.enDevPipeMode = VI_DEV_PIPE_OFFLINE;
		ret = RK_MPI_VI_SetModParam(&stModParam);
		if (ret)
			LOG_ERROR("RK_MPI_VI_SetModParam fail:%#X\n", ret);
	} else {
		memset(&stModParam, 0, sizeof(stModParam));
		stModParam.enViModType = VI_DEV_PIPE_MODE;
		stModParam.stDevPipeModParam.enDevPipeMode = VI_DEV_PIPE_ONLINE;
		ret = RK_MPI_VI_SetModParam(&stModParam);
		if (ret)
			LOG_ERROR("RK_MPI_VI_SetModParam fail:%#X\n", ret);

		memset(&stModParam, 0, sizeof(stModParam));
		stModParam.enViModType = VI_EXT_CHN_MODE;
		stModParam.stExtChnParam.mirrorCmsc = 0; // 1 is for vpss, 0 is for vi ext
		stModParam.stExtChnParam.extChn[0] = 0;
		stModParam.stExtChnParam.extChn[1] = 0;
		ret = RK_MPI_VI_SetModParam(&stModParam);
		if (ret)
			LOG_ERROR("RK_MPI_VI_SetModParam fail:%#X\n", ret);

		memset(&stModParam, 0, sizeof(stModParam));
		stModParam.enViModType = VI_EXT_CHN_MODE;
		ret = RK_MPI_VI_GetModParam(&stModParam);
		if (ret)
			LOG_ERROR("RK_MPI_VI_GetModParam fail:%#X\n", ret);

		LOG_INFO("vi mod:%d mirror:%d ext_chn_mode:%d ext_chn1_mode:%d"
		         "ext_chn2_mode:%d ext_chn3_mode:%d\n",
		         stModParam.enViModType, stModParam.stExtChnParam.mirrorCmsc,
		         stModParam.stExtChnParam.extChn[0], stModParam.stExtChnParam.extChn[1],
		         stModParam.stExtChnParam.extChn[2], stModParam.stExtChnParam.extChn[3]);
	}

	return 0;
}

static int rkipc_vi_dev_deinit() {
	RK_MPI_VI_DisableDev(pipe_id_);

	return 0;
}

static int rkipc_vi_chn_init() {
	int ret;
	int video_width = 0;
	int video_height = 0;
	int buf_cnt = rk_param_get_int("video.0:buf_cnt", 32);

	if (g_current_eis_mode == RK_EIS_OFF) {
		// resize by vi
		video_width = rk_param_get_int("video.0:width", -1);
		video_height = rk_param_get_int("video.0:height", -1);
	} else {
		// resize by gdc
		video_width = rk_param_get_int("video.source:width", -1);
		video_height = rk_param_get_int("video.source:height", -1);
	}
	// VI init
	VI_CHN_ATTR_S vi_chn_attr;
	memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
	vi_chn_attr.stIspOpt.u32BufCount = buf_cnt;
	vi_chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_MMAP;
	vi_chn_attr.stSize.u32Width = video_width;
	vi_chn_attr.stSize.u32Height = video_height;
	vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
	vi_chn_attr.u32Depth = rk_param_get_int("video.0:depth", 16);
	if (g_current_eis_mode != RK_EIS_OFF)
		vi_chn_attr.stIspOpt.bAttchFrmInfo = true;
	ret = RK_MPI_VI_SetChnAttr(pipe_id_, g_vi_chn_id, &vi_chn_attr);
	ret |= RK_MPI_VI_EnableChn(pipe_id_, g_vi_chn_id);
	if (ret) {
		LOG_ERROR("ERROR: create VI error! ret=%d\n", ret);
		return ret;
	}

	vi_chn_attr.stIspOpt.u32BufCount = 4;
	if (g_current_eis_mode == RK_HORIZON_STEADY) {
		vi_chn_attr.stSize.u32Width = 704;
		vi_chn_attr.stSize.u32Height = 512;
	} else {
		vi_chn_attr.stSize.u32Width = 704;
		vi_chn_attr.stSize.u32Height = 384;
	}
	vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
	vi_chn_attr.u32Depth = 0;
	if (g_current_eis_mode != RK_EIS_OFF)
		vi_chn_attr.stIspOpt.bAttchFrmInfo = true;
	vi_chn_attr.stFrameRate.s32SrcFrameRate = -1;
	vi_chn_attr.stFrameRate.s32DstFrameRate = -1;
	if (g_current_eis_mode == RK_HORIZON_STEADY) {
		// INFO: vblank is too short to support vpss online mode for 4032*3016 resolution
		ret = RK_MPI_VI_SetChnAttr(pipe_id_, g_vi_for_vo_chn_id, &vi_chn_attr);
		ret |= RK_MPI_VI_EnableChn(pipe_id_, g_vi_for_vo_chn_id);
		if (ret) {
			LOG_ERROR("ERROR: create VI error! ret=%d\n", ret);
			return ret;
		}
	} else {
		ret = RK_MPI_VI_SetChnAttr(pipe_id_, g_vi_ext_chn_id, &vi_chn_attr);
		ret |= RK_MPI_VI_EnableChn(pipe_id_, g_vi_ext_chn_id);
		if (ret) {
			LOG_ERROR("ERROR: create VI error! ret=%d\n", ret);
			return ret;
		}
	}

	return ret;
}

int rkipc_vi_chn_deinit() {
	int ret = 0;
	ret = RK_MPI_VI_DisableChn(pipe_id_, g_vi_chn_id);
	if (ret)
		LOG_ERROR("ERROR: RK_MPI_VI_DisableChn VI error! ret=%x\n", ret);
	if (g_current_eis_mode == RK_HORIZON_STEADY)
		ret = RK_MPI_VI_DisableChn(pipe_id_, g_vi_for_vo_chn_id);
	else
		ret = RK_MPI_VI_DisableChn(pipe_id_, g_vi_ext_chn_id);
	if (ret)
		LOG_ERROR("ERROR: RK_MPI_VI_DisableChn VI error! ret=%x\n", ret);

	return ret;
}

static int rkipc_venc_normal_init() {
	int ret = 0;
	int video_width = rk_param_get_int("video.0:width", -1);
	int video_height = rk_param_get_int("video.0:height", -1);

	// VENC[0] init
	VENC_CHN_ATTR_S venc_chn_attr;
	memset(&venc_chn_attr, 0, sizeof(venc_chn_attr));
	tmp_output_data_type = rk_param_get_string("video.0:output_data_type", NULL);
	tmp_rc_mode = rk_param_get_string("video.0:rc_mode", NULL);
	tmp_h264_profile = rk_param_get_string("video.0:h264_profile", NULL);
	if ((tmp_output_data_type == NULL) || (tmp_rc_mode == NULL)) {
		LOG_ERROR("tmp_output_data_type or tmp_rc_mode is NULL\n");
		return -1;
	}
	LOG_INFO("tmp_output_data_type is %s, tmp_rc_mode is %s, tmp_h264_profile is %s\n",
	         tmp_output_data_type, tmp_rc_mode, tmp_h264_profile);
	if (!strcmp(tmp_output_data_type, "H.264")) {
		venc_chn_attr.stVencAttr.enType = RK_VIDEO_ID_AVC;

		if (!strcmp(tmp_h264_profile, "high"))
			venc_chn_attr.stVencAttr.u32Profile = 100;
		else if (!strcmp(tmp_h264_profile, "main"))
			venc_chn_attr.stVencAttr.u32Profile = 77;
		else if (!strcmp(tmp_h264_profile, "baseline"))
			venc_chn_attr.stVencAttr.u32Profile = 66;
		else
			LOG_ERROR("tmp_h264_profile is %s\n", tmp_h264_profile);

		if (!strcmp(tmp_rc_mode, "CBR")) {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
			venc_chn_attr.stRcAttr.stH264Cbr.u32Gop = rk_param_get_int("video.0:gop", -1);
			venc_chn_attr.stRcAttr.stH264Cbr.u32BitRate = rk_param_get_int("video.0:max_rate", 0);
			venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen =
			    rk_param_get_int("video.0:dst_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum =
			    rk_param_get_int("video.0:dst_frame_rate_num", -1);
			venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen =
			    rk_param_get_int("video.0:src_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum =
			    rk_param_get_int("video.0:src_frame_rate_num", -1);
		} else {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264VBR;
			venc_chn_attr.stRcAttr.stH264Vbr.u32Gop = rk_param_get_int("video.0:gop", -1);
			venc_chn_attr.stRcAttr.stH264Vbr.u32BitRate = rk_param_get_int("video.0:max_rate", 0);
			venc_chn_attr.stRcAttr.stH264Vbr.fr32DstFrameRateDen =
			    rk_param_get_int("video.0:dst_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH264Vbr.fr32DstFrameRateNum =
			    rk_param_get_int("video.0:dst_frame_rate_num", -1);
			venc_chn_attr.stRcAttr.stH264Vbr.u32SrcFrameRateDen =
			    rk_param_get_int("video.0:src_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH264Vbr.u32SrcFrameRateNum =
			    rk_param_get_int("video.0:src_frame_rate_num", -1);
		}
	} else if (!strcmp(tmp_output_data_type, "H.265")) {
		venc_chn_attr.stVencAttr.enType = RK_VIDEO_ID_HEVC;
		if (!strcmp(tmp_rc_mode, "CBR")) {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
			venc_chn_attr.stRcAttr.stH265Cbr.u32Gop = rk_param_get_int("video.0:gop", -1);
			venc_chn_attr.stRcAttr.stH265Cbr.u32BitRate = rk_param_get_int("video.0:max_rate", 0);
			venc_chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen =
			    rk_param_get_int("video.0:dst_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum =
			    rk_param_get_int("video.0:dst_frame_rate_num", -1);
			venc_chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen =
			    rk_param_get_int("video.0:src_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum =
			    rk_param_get_int("video.0:src_frame_rate_num", -1);
		} else {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265VBR;
			venc_chn_attr.stRcAttr.stH265Vbr.u32Gop = rk_param_get_int("video.0:gop", -1);
			venc_chn_attr.stRcAttr.stH265Vbr.u32BitRate = rk_param_get_int("video.0:max_rate", 0);
			venc_chn_attr.stRcAttr.stH265Vbr.fr32DstFrameRateDen =
			    rk_param_get_int("video.0:dst_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH265Vbr.fr32DstFrameRateNum =
			    rk_param_get_int("video.0:dst_frame_rate_num", -1);
			venc_chn_attr.stRcAttr.stH265Vbr.u32SrcFrameRateDen =
			    rk_param_get_int("video.0:src_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH265Vbr.u32SrcFrameRateNum =
			    rk_param_get_int("video.0:src_frame_rate_num", -1);
		}
	} else {
		LOG_ERROR("tmp_output_data_type is %s, not support\n", tmp_output_data_type);
		return -1;
	}
	tmp_smart = rk_param_get_string("video.0:smart", NULL);
	tmp_gop_mode = rk_param_get_string("video.0:gop_mode", NULL);
	if (!strcmp(tmp_gop_mode, "normalP")) {
		venc_chn_attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
	} else if (!strcmp(tmp_gop_mode, "smartP")) {
		venc_chn_attr.stGopAttr.enGopMode = VENC_GOPMODE_SMARTP;
		venc_chn_attr.stGopAttr.s32VirIdrLen = rk_param_get_int("video.0:smartp_viridrlen", 25);
	} else if (!strcmp(tmp_gop_mode, "TSVC4")) {
		venc_chn_attr.stGopAttr.enGopMode = VENC_GOPMODE_TSVC4;
	}
	// venc_chn_attr.stGopAttr.u32GopSize = rk_param_get_int("video.0:gop", -1);

	venc_chn_attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
	venc_chn_attr.stVencAttr.u32PicWidth = video_width;
	venc_chn_attr.stVencAttr.u32PicHeight = video_height;
	venc_chn_attr.stVencAttr.u32VirWidth = video_width;
	venc_chn_attr.stVencAttr.u32VirHeight = video_height;
	venc_chn_attr.stVencAttr.u32StreamBufCnt = 60 * 3;              // 3s
	venc_chn_attr.stVencAttr.u32BufSize = 65 * 1024 * 1024 * 3 / 8; // 3s * 65Mbps, 24MB
	// venc_chn_attr.stVencAttr.u32Depth = 1;
	ret = RK_MPI_VENC_CreateChn(VIDEO_PIPE_NORMAL, &venc_chn_attr);
	if (ret) {
		LOG_ERROR("ERROR: create VENC error! ret=%d\n", ret);
		return -1;
	}

	tmp_rc_quality = rk_param_get_string("video.0:rc_quality", NULL);
	VENC_RC_PARAM_S venc_rc_param;
	RK_MPI_VENC_GetRcParam(VIDEO_PIPE_NORMAL, &venc_rc_param);
	if (!strcmp(tmp_output_data_type, "H.264")) {
		if (!strcmp(tmp_rc_quality, "highest")) {
			venc_rc_param.stParamH264.u32MinQp = 10;
		} else if (!strcmp(tmp_rc_quality, "higher")) {
			venc_rc_param.stParamH264.u32MinQp = 15;
		} else if (!strcmp(tmp_rc_quality, "high")) {
			venc_rc_param.stParamH264.u32MinQp = 20;
		} else if (!strcmp(tmp_rc_quality, "medium")) {
			venc_rc_param.stParamH264.u32MinQp = 25;
		} else if (!strcmp(tmp_rc_quality, "low")) {
			venc_rc_param.stParamH264.u32MinQp = 30;
		} else if (!strcmp(tmp_rc_quality, "lower")) {
			venc_rc_param.stParamH264.u32MinQp = 35;
		} else {
			venc_rc_param.stParamH264.u32MinQp = 40;
		}
	} else if (!strcmp(tmp_output_data_type, "H.265")) {
		if (!strcmp(tmp_rc_quality, "highest")) {
			venc_rc_param.stParamH265.u32MinQp = 10;
		} else if (!strcmp(tmp_rc_quality, "higher")) {
			venc_rc_param.stParamH265.u32MinQp = 15;
		} else if (!strcmp(tmp_rc_quality, "high")) {
			venc_rc_param.stParamH265.u32MinQp = 20;
		} else if (!strcmp(tmp_rc_quality, "medium")) {
			venc_rc_param.stParamH265.u32MinQp = 25;
		} else if (!strcmp(tmp_rc_quality, "low")) {
			venc_rc_param.stParamH265.u32MinQp = 30;
		} else if (!strcmp(tmp_rc_quality, "lower")) {
			venc_rc_param.stParamH265.u32MinQp = 35;
		} else {
			venc_rc_param.stParamH265.u32MinQp = 40;
		}
	} else {
		LOG_ERROR("tmp_output_data_type is %s, not support\n", tmp_output_data_type);
		return -1;
	}
	RK_MPI_VENC_SetRcParam(VIDEO_PIPE_NORMAL, &venc_rc_param);

	int rotation = rk_param_get_int("video.source:rotation", 0);
	if (rotation == 0) {
		RK_MPI_VENC_SetChnRotation(VIDEO_PIPE_NORMAL, ROTATION_0);
	} else if (rotation == 90) {
		RK_MPI_VENC_SetChnRotation(VIDEO_PIPE_NORMAL, ROTATION_90);
	} else if (rotation == 180) {
		RK_MPI_VENC_SetChnRotation(VIDEO_PIPE_NORMAL, ROTATION_180);
	} else if (rotation == 270) {
		RK_MPI_VENC_SetChnRotation(VIDEO_PIPE_NORMAL, ROTATION_270);
	}

	// INFO: need a better way to config
	int fps = rk_param_get_int("isp.0.adjustment:fps", 30);
	if (fps == 58) {
		ret = RK_MPI_VENC_RequestPskip(VIDEO_PIPE_NORMAL, 1, 29);
		if (ret)
			LOG_ERROR("RK_MPI_VENC_RequestPskip failed %#X\n", ret);
		else
			LOG_ERROR("RK_MPI_VENC_RequestPskip success\n");
	}
	return ret;
}

int rkipc_venc_normal_deinit() {
	int ret = 0;
	ret |= RK_MPI_VENC_DestroyChn(VIDEO_PIPE_NORMAL);
	if (ret)
		LOG_ERROR("ERROR: Destroy VENC error! ret=%#x\n", ret);
	else
		LOG_INFO("RK_MPI_VENC_DestroyChn success\n");

	return ret;
}

static int rkipc_venc_slowmotion_init() {
	int ret = 0;
	int video_width = rk_param_get_int("video.0:width", -1);
	int video_height = rk_param_get_int("video.0:height", -1);

	// VENC[1] init
	VENC_CHN_ATTR_S venc_chn_attr;
	memset(&venc_chn_attr, 0, sizeof(venc_chn_attr));
	tmp_output_data_type = rk_param_get_string("video.0:output_data_type", NULL);
	tmp_rc_mode = rk_param_get_string("video.0:rc_mode", NULL);
	tmp_h264_profile = rk_param_get_string("video.0:h264_profile", NULL);
	if ((tmp_output_data_type == NULL) || (tmp_rc_mode == NULL)) {
		LOG_ERROR("tmp_output_data_type or tmp_rc_mode is NULL\n");
		return -1;
	}
	LOG_INFO("tmp_output_data_type is %s, tmp_rc_mode is %s, tmp_h264_profile is %s\n",
	         tmp_output_data_type, tmp_rc_mode, tmp_h264_profile);
	if (!strcmp(tmp_output_data_type, "H.264")) {
		venc_chn_attr.stVencAttr.enType = RK_VIDEO_ID_AVC;

		if (!strcmp(tmp_h264_profile, "high"))
			venc_chn_attr.stVencAttr.u32Profile = 100;
		else if (!strcmp(tmp_h264_profile, "main"))
			venc_chn_attr.stVencAttr.u32Profile = 77;
		else if (!strcmp(tmp_h264_profile, "baseline"))
			venc_chn_attr.stVencAttr.u32Profile = 66;
		else
			LOG_ERROR("tmp_h264_profile is %s\n", tmp_h264_profile);

		if (!strcmp(tmp_rc_mode, "CBR")) {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
			venc_chn_attr.stRcAttr.stH264Cbr.u32Gop = rk_param_get_int("video.0:gop", -1);
			venc_chn_attr.stRcAttr.stH264Cbr.u32BitRate = rk_param_get_int("video.0:max_rate", 0);
			venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen =
			    rk_param_get_int("video.0:dst_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum =
			    rk_param_get_int("video.0:dst_frame_rate_num", -1);
			venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen =
			    rk_param_get_int("video.0:src_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum =
			    rk_param_get_int("video.0:src_frame_rate_num", -1);
		} else {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264VBR;
			venc_chn_attr.stRcAttr.stH264Vbr.u32Gop = rk_param_get_int("video.0:gop", -1);
			venc_chn_attr.stRcAttr.stH264Vbr.u32BitRate = rk_param_get_int("video.0:max_rate", 0);
			venc_chn_attr.stRcAttr.stH264Vbr.fr32DstFrameRateDen =
			    rk_param_get_int("video.0:dst_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH264Vbr.fr32DstFrameRateNum =
			    rk_param_get_int("video.0:dst_frame_rate_num", -1);
			venc_chn_attr.stRcAttr.stH264Vbr.u32SrcFrameRateDen =
			    rk_param_get_int("video.0:src_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH264Vbr.u32SrcFrameRateNum =
			    rk_param_get_int("video.0:src_frame_rate_num", -1);
		}
	} else if (!strcmp(tmp_output_data_type, "H.265")) {
		venc_chn_attr.stVencAttr.enType = RK_VIDEO_ID_HEVC;
		if (!strcmp(tmp_rc_mode, "CBR")) {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
			venc_chn_attr.stRcAttr.stH265Cbr.u32Gop = rk_param_get_int("video.0:gop", -1);
			venc_chn_attr.stRcAttr.stH265Cbr.u32BitRate = rk_param_get_int("video.0:max_rate", 0);
			venc_chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen =
			    rk_param_get_int("video.0:dst_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum =
			    rk_param_get_int("video.0:dst_frame_rate_num", -1);
			venc_chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen =
			    rk_param_get_int("video.0:src_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum =
			    rk_param_get_int("video.0:src_frame_rate_num", -1);
		} else {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265VBR;
			venc_chn_attr.stRcAttr.stH265Vbr.u32Gop = rk_param_get_int("video.0:gop", -1);
			venc_chn_attr.stRcAttr.stH265Vbr.u32BitRate = rk_param_get_int("video.0:max_rate", 0);
			venc_chn_attr.stRcAttr.stH265Vbr.fr32DstFrameRateDen =
			    rk_param_get_int("video.0:dst_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH265Vbr.fr32DstFrameRateNum =
			    rk_param_get_int("video.0:dst_frame_rate_num", -1);
			venc_chn_attr.stRcAttr.stH265Vbr.u32SrcFrameRateDen =
			    rk_param_get_int("video.0:src_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH265Vbr.u32SrcFrameRateNum =
			    rk_param_get_int("video.0:src_frame_rate_num", -1);
		}
	} else {
		LOG_ERROR("tmp_output_data_type is %s, not support\n", tmp_output_data_type);
		return -1;
	}
	tmp_smart = rk_param_get_string("video.0:smart", NULL);
	tmp_gop_mode = rk_param_get_string("video.0:gop_mode", NULL);
	if (!strcmp(tmp_gop_mode, "normalP")) {
		venc_chn_attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
	} else if (!strcmp(tmp_gop_mode, "smartP")) {
		venc_chn_attr.stGopAttr.enGopMode = VENC_GOPMODE_SMARTP;
		venc_chn_attr.stGopAttr.s32VirIdrLen = rk_param_get_int("video.0:smartp_viridrlen", 25);
	} else if (!strcmp(tmp_gop_mode, "TSVC4")) {
		venc_chn_attr.stGopAttr.enGopMode = VENC_GOPMODE_TSVC4;
	}
	// venc_chn_attr.stGopAttr.u32GopSize = rk_param_get_int("video.0:gop", -1);

	venc_chn_attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
	venc_chn_attr.stVencAttr.u32PicWidth = video_width;
	venc_chn_attr.stVencAttr.u32PicHeight = video_height;
	venc_chn_attr.stVencAttr.u32VirWidth = video_width;
	venc_chn_attr.stVencAttr.u32VirHeight = video_height;
	venc_chn_attr.stVencAttr.u32StreamBufCnt = 60 * 3;              // 3s
	venc_chn_attr.stVencAttr.u32BufSize = 65 * 1024 * 1024 * 3 / 8; // 3s * 65Mbps, 24MB
	// venc_chn_attr.stVencAttr.u32Depth = 1;
	ret = RK_MPI_VENC_CreateChn(VIDEO_PIPE_NORMAL, &venc_chn_attr);
	if (ret) {
		LOG_ERROR("ERROR: create VENC error! ret=%d\n", ret);
		return -1;
	}

	tmp_rc_quality = rk_param_get_string("video.0:rc_quality", NULL);
	VENC_RC_PARAM_S venc_rc_param;
	RK_MPI_VENC_GetRcParam(VIDEO_PIPE_NORMAL, &venc_rc_param);
	if (!strcmp(tmp_output_data_type, "H.264")) {
		if (!strcmp(tmp_rc_quality, "highest")) {
			venc_rc_param.stParamH264.u32MinQp = 10;
		} else if (!strcmp(tmp_rc_quality, "higher")) {
			venc_rc_param.stParamH264.u32MinQp = 15;
		} else if (!strcmp(tmp_rc_quality, "high")) {
			venc_rc_param.stParamH264.u32MinQp = 20;
		} else if (!strcmp(tmp_rc_quality, "medium")) {
			venc_rc_param.stParamH264.u32MinQp = 25;
		} else if (!strcmp(tmp_rc_quality, "low")) {
			venc_rc_param.stParamH264.u32MinQp = 30;
		} else if (!strcmp(tmp_rc_quality, "lower")) {
			venc_rc_param.stParamH264.u32MinQp = 35;
		} else {
			venc_rc_param.stParamH264.u32MinQp = 40;
		}
	} else if (!strcmp(tmp_output_data_type, "H.265")) {
		if (!strcmp(tmp_rc_quality, "highest")) {
			venc_rc_param.stParamH265.u32MinQp = 10;
		} else if (!strcmp(tmp_rc_quality, "higher")) {
			venc_rc_param.stParamH265.u32MinQp = 15;
		} else if (!strcmp(tmp_rc_quality, "high")) {
			venc_rc_param.stParamH265.u32MinQp = 20;
		} else if (!strcmp(tmp_rc_quality, "medium")) {
			venc_rc_param.stParamH265.u32MinQp = 25;
		} else if (!strcmp(tmp_rc_quality, "low")) {
			venc_rc_param.stParamH265.u32MinQp = 30;
		} else if (!strcmp(tmp_rc_quality, "lower")) {
			venc_rc_param.stParamH265.u32MinQp = 35;
		} else {
			venc_rc_param.stParamH265.u32MinQp = 40;
		}
	} else {
		LOG_ERROR("tmp_output_data_type is %s, not support\n", tmp_output_data_type);
		return -1;
	}
	RK_MPI_VENC_SetRcParam(VIDEO_PIPE_NORMAL, &venc_rc_param);

	int rotation = rk_param_get_int("video.source:rotation", 0);
	if (rotation == 0) {
		RK_MPI_VENC_SetChnRotation(VIDEO_PIPE_NORMAL, ROTATION_0);
	} else if (rotation == 90) {
		RK_MPI_VENC_SetChnRotation(VIDEO_PIPE_NORMAL, ROTATION_90);
	} else if (rotation == 180) {
		RK_MPI_VENC_SetChnRotation(VIDEO_PIPE_NORMAL, ROTATION_180);
	} else if (rotation == 270) {
		RK_MPI_VENC_SetChnRotation(VIDEO_PIPE_NORMAL, ROTATION_270);
	}

	return ret;
}

static int rkipc_venc_slowmotion_deinit() {
	int ret = 0;
	ret = RK_MPI_VENC_StopRecvFrame(VIDEO_PIPE_NORMAL);
	ret |= RK_MPI_VENC_DestroyChn(VIDEO_PIPE_NORMAL);
	if (ret)
		LOG_ERROR("ERROR: Destroy VENC error! ret=%#x\n", ret);
	else
		LOG_INFO("RK_MPI_VENC_DestroyChn success\n");

	return ret;
}

static int rkipc_venc_timelapse_init() {
	int ret = 0;
	int video_width = rk_param_get_int("video.0:width", -1);
	int video_height = rk_param_get_int("video.0:height", -1);

	// VENC[2] init
	VENC_CHN_ATTR_S venc_chn_attr;
	memset(&venc_chn_attr, 0, sizeof(venc_chn_attr));
	tmp_output_data_type = rk_param_get_string("video.0:output_data_type", NULL);
	tmp_rc_mode = rk_param_get_string("video.0:rc_mode", NULL);
	tmp_h264_profile = rk_param_get_string("video.0:h264_profile", NULL);
	if ((tmp_output_data_type == NULL) || (tmp_rc_mode == NULL)) {
		LOG_ERROR("tmp_output_data_type or tmp_rc_mode is NULL\n");
		return -1;
	}
	LOG_INFO("tmp_output_data_type is %s, tmp_rc_mode is %s, tmp_h264_profile is %s\n",
	         tmp_output_data_type, tmp_rc_mode, tmp_h264_profile);
	if (!strcmp(tmp_output_data_type, "H.264")) {
		venc_chn_attr.stVencAttr.enType = RK_VIDEO_ID_AVC;

		if (!strcmp(tmp_h264_profile, "high"))
			venc_chn_attr.stVencAttr.u32Profile = 100;
		else if (!strcmp(tmp_h264_profile, "main"))
			venc_chn_attr.stVencAttr.u32Profile = 77;
		else if (!strcmp(tmp_h264_profile, "baseline"))
			venc_chn_attr.stVencAttr.u32Profile = 66;
		else
			LOG_ERROR("tmp_h264_profile is %s\n", tmp_h264_profile);

		if (!strcmp(tmp_rc_mode, "CBR")) {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
			venc_chn_attr.stRcAttr.stH264Cbr.u32Gop = rk_param_get_int("video.0:gop", -1);
			venc_chn_attr.stRcAttr.stH264Cbr.u32BitRate = rk_param_get_int("video.0:max_rate", 0);
			venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen =
			    rk_param_get_int("video.0:dst_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum =
			    rk_param_get_int("video.0:dst_frame_rate_num", -1);
			venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen =
			    rk_param_get_int("video.0:src_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum =
			    rk_param_get_int("video.0:src_frame_rate_num", -1);
		} else {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264VBR;
			venc_chn_attr.stRcAttr.stH264Vbr.u32Gop = rk_param_get_int("video.0:gop", -1);
			venc_chn_attr.stRcAttr.stH264Vbr.u32BitRate = rk_param_get_int("video.0:max_rate", 0);
			venc_chn_attr.stRcAttr.stH264Vbr.fr32DstFrameRateDen =
			    rk_param_get_int("video.0:dst_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH264Vbr.fr32DstFrameRateNum =
			    rk_param_get_int("video.0:dst_frame_rate_num", -1);
			venc_chn_attr.stRcAttr.stH264Vbr.u32SrcFrameRateDen =
			    rk_param_get_int("video.0:src_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH264Vbr.u32SrcFrameRateNum =
			    rk_param_get_int("video.0:src_frame_rate_num", -1);
		}
	} else if (!strcmp(tmp_output_data_type, "H.265")) {
		venc_chn_attr.stVencAttr.enType = RK_VIDEO_ID_HEVC;
		if (!strcmp(tmp_rc_mode, "CBR")) {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
			venc_chn_attr.stRcAttr.stH265Cbr.u32Gop = rk_param_get_int("video.0:gop", -1);
			venc_chn_attr.stRcAttr.stH265Cbr.u32BitRate = rk_param_get_int("video.0:max_rate", 0);
			venc_chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen =
			    rk_param_get_int("video.0:dst_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum =
			    rk_param_get_int("video.0:dst_frame_rate_num", -1);
			venc_chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen =
			    rk_param_get_int("video.0:src_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum =
			    rk_param_get_int("video.0:src_frame_rate_num", -1);
		} else {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265VBR;
			venc_chn_attr.stRcAttr.stH265Vbr.u32Gop = rk_param_get_int("video.0:gop", -1);
			venc_chn_attr.stRcAttr.stH265Vbr.u32BitRate = rk_param_get_int("video.0:max_rate", 0);
			venc_chn_attr.stRcAttr.stH265Vbr.fr32DstFrameRateDen =
			    rk_param_get_int("video.0:dst_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH265Vbr.fr32DstFrameRateNum =
			    rk_param_get_int("video.0:dst_frame_rate_num", -1);
			venc_chn_attr.stRcAttr.stH265Vbr.u32SrcFrameRateDen =
			    rk_param_get_int("video.0:src_frame_rate_den", -1);
			venc_chn_attr.stRcAttr.stH265Vbr.u32SrcFrameRateNum =
			    rk_param_get_int("video.0:src_frame_rate_num", -1);
		}
	} else {
		LOG_ERROR("tmp_output_data_type is %s, not support\n", tmp_output_data_type);
		return -1;
	}
	tmp_smart = rk_param_get_string("video.0:smart", NULL);
	tmp_gop_mode = rk_param_get_string("video.0:gop_mode", NULL);
	if (!strcmp(tmp_gop_mode, "normalP")) {
		venc_chn_attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
	} else if (!strcmp(tmp_gop_mode, "smartP")) {
		venc_chn_attr.stGopAttr.enGopMode = VENC_GOPMODE_SMARTP;
		venc_chn_attr.stGopAttr.s32VirIdrLen = rk_param_get_int("video.0:smartp_viridrlen", 25);
	} else if (!strcmp(tmp_gop_mode, "TSVC4")) {
		venc_chn_attr.stGopAttr.enGopMode = VENC_GOPMODE_TSVC4;
	}
	// venc_chn_attr.stGopAttr.u32GopSize = rk_param_get_int("video.0:gop", -1);

	venc_chn_attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
	venc_chn_attr.stVencAttr.u32PicWidth = video_width;
	venc_chn_attr.stVencAttr.u32PicHeight = video_height;
	venc_chn_attr.stVencAttr.u32VirWidth = video_width;
	venc_chn_attr.stVencAttr.u32VirHeight = video_height;
	venc_chn_attr.stVencAttr.u32StreamBufCnt = 60 * 3;              // 3s
	venc_chn_attr.stVencAttr.u32BufSize = 65 * 1024 * 1024 * 3 / 8; // 3s * 65Mbps, 24MB
	// venc_chn_attr.stVencAttr.u32Depth = 1;
	ret = RK_MPI_VENC_CreateChn(VIDEO_PIPE_NORMAL, &venc_chn_attr);
	if (ret) {
		LOG_ERROR("ERROR: create VENC error! ret=%d\n", ret);
		return -1;
	}

	tmp_rc_quality = rk_param_get_string("video.0:rc_quality", NULL);
	VENC_RC_PARAM_S venc_rc_param;
	RK_MPI_VENC_GetRcParam(VIDEO_PIPE_NORMAL, &venc_rc_param);
	if (!strcmp(tmp_output_data_type, "H.264")) {
		if (!strcmp(tmp_rc_quality, "highest")) {
			venc_rc_param.stParamH264.u32MinQp = 10;
		} else if (!strcmp(tmp_rc_quality, "higher")) {
			venc_rc_param.stParamH264.u32MinQp = 15;
		} else if (!strcmp(tmp_rc_quality, "high")) {
			venc_rc_param.stParamH264.u32MinQp = 20;
		} else if (!strcmp(tmp_rc_quality, "medium")) {
			venc_rc_param.stParamH264.u32MinQp = 25;
		} else if (!strcmp(tmp_rc_quality, "low")) {
			venc_rc_param.stParamH264.u32MinQp = 30;
		} else if (!strcmp(tmp_rc_quality, "lower")) {
			venc_rc_param.stParamH264.u32MinQp = 35;
		} else {
			venc_rc_param.stParamH264.u32MinQp = 40;
		}
	} else if (!strcmp(tmp_output_data_type, "H.265")) {
		if (!strcmp(tmp_rc_quality, "highest")) {
			venc_rc_param.stParamH265.u32MinQp = 10;
		} else if (!strcmp(tmp_rc_quality, "higher")) {
			venc_rc_param.stParamH265.u32MinQp = 15;
		} else if (!strcmp(tmp_rc_quality, "high")) {
			venc_rc_param.stParamH265.u32MinQp = 20;
		} else if (!strcmp(tmp_rc_quality, "medium")) {
			venc_rc_param.stParamH265.u32MinQp = 25;
		} else if (!strcmp(tmp_rc_quality, "low")) {
			venc_rc_param.stParamH265.u32MinQp = 30;
		} else if (!strcmp(tmp_rc_quality, "lower")) {
			venc_rc_param.stParamH265.u32MinQp = 35;
		} else {
			venc_rc_param.stParamH265.u32MinQp = 40;
		}
	} else {
		LOG_ERROR("tmp_output_data_type is %s, not support\n", tmp_output_data_type);
		return -1;
	}
	RK_MPI_VENC_SetRcParam(VIDEO_PIPE_NORMAL, &venc_rc_param);

	int rotation = rk_param_get_int("video.source:rotation", 0);
	if (rotation == 0) {
		RK_MPI_VENC_SetChnRotation(VIDEO_PIPE_NORMAL, ROTATION_0);
	} else if (rotation == 90) {
		RK_MPI_VENC_SetChnRotation(VIDEO_PIPE_NORMAL, ROTATION_90);
	} else if (rotation == 180) {
		RK_MPI_VENC_SetChnRotation(VIDEO_PIPE_NORMAL, ROTATION_180);
	} else if (rotation == 270) {
		RK_MPI_VENC_SetChnRotation(VIDEO_PIPE_NORMAL, ROTATION_270);
	}

	return ret;
}

static int rkipc_venc_timelapse_deinit() {
	int ret = 0;
	ret = RK_MPI_VENC_StopRecvFrame(VIDEO_PIPE_NORMAL);
	ret |= RK_MPI_VENC_DestroyChn(VIDEO_PIPE_NORMAL);
	if (ret)
		LOG_ERROR("ERROR: Destroy VENC error! ret=%#x\n", ret);
	else
		LOG_INFO("RK_MPI_VENC_DestroyChn success\n");

	return ret;
}

static int rkipc_jpeg_init() {
	// jpeg resolution same to video.0
	int ret;
	int video_width = rk_param_get_int("video.0:width", -1);
	int video_height = rk_param_get_int("video.0:height", -1);
	// VENC[3] init
	VENC_CHN_ATTR_S jpeg_chn_attr;
	memset(&jpeg_chn_attr, 0, sizeof(jpeg_chn_attr));
	jpeg_chn_attr.stVencAttr.enType = RK_VIDEO_ID_JPEG;
	jpeg_chn_attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
	jpeg_chn_attr.stVencAttr.u32PicWidth = video_width;
	jpeg_chn_attr.stVencAttr.u32PicHeight = video_height;
	jpeg_chn_attr.stVencAttr.u32VirWidth = video_width;
	jpeg_chn_attr.stVencAttr.u32VirHeight = video_height;
	jpeg_chn_attr.stVencAttr.u32StreamBufCnt = 2;
	jpeg_chn_attr.stVencAttr.u32BufSize = video_width * video_height * 3 / 2;
	// jpeg_chn_attr.stVencAttr.u32Depth = 1;
	ret = RK_MPI_VENC_CreateChn(JPEG_VENC_CHN, &jpeg_chn_attr);
	if (ret) {
		LOG_ERROR("ERROR: create VENC error! ret=%d\n", ret);
		return -1;
	}
	VENC_JPEG_PARAM_S stJpegParam;
	memset(&stJpegParam, 0, sizeof(stJpegParam));
	stJpegParam.u32Qfactor = 95;
	RK_MPI_VENC_SetJpegParam(JPEG_VENC_CHN, &stJpegParam);

	int rotation = rk_param_get_int("video.source:rotation", 0);
	if (rotation == 0) {
		RK_MPI_VENC_SetChnRotation(JPEG_VENC_CHN, ROTATION_0);
	} else if (rotation == 90) {
		RK_MPI_VENC_SetChnRotation(JPEG_VENC_CHN, ROTATION_90);
	} else if (rotation == 180) {
		RK_MPI_VENC_SetChnRotation(JPEG_VENC_CHN, ROTATION_180);
	} else if (rotation == 270) {
		RK_MPI_VENC_SetChnRotation(JPEG_VENC_CHN, ROTATION_270);
	}

	VENC_RECV_PIC_PARAM_S stRecvParam;
	memset(&stRecvParam, 0, sizeof(VENC_RECV_PIC_PARAM_S));
	stRecvParam.s32RecvPicNum = 1;
	RK_MPI_VENC_StartRecvFrame(JPEG_VENC_CHN,
	                           &stRecvParam); // must, for no streams callback running failed
	RK_MPI_VENC_StopRecvFrame(JPEG_VENC_CHN);
	pthread_create(&jpeg_venc_thread_id, NULL, rkipc_get_jpeg, NULL);

	return ret;
}

static int rkipc_jpeg_deinit() {
	int ret = 0;
	ret = RK_MPI_VENC_StopRecvFrame(JPEG_VENC_CHN);
	ret |= RK_MPI_VENC_DestroyChn(JPEG_VENC_CHN);
	if (ret)
		LOG_ERROR("ERROR: Destroy VENC error! ret=%#x\n", ret);
	else
		LOG_INFO("RK_MPI_VENC_DestroyChn success\n");

	return ret;
}

static int rkipc_bind_init() {
	int ret;
	MPP_CHN_S target_venc_chn;

	vi_chn[0].enModId = RK_ID_VI;
	vi_chn[0].s32DevId = 0;
	vi_chn[0].s32ChnId = g_vi_chn_id;
	vi_chn[1].enModId = RK_ID_VI;
	vi_chn[1].s32DevId = 0;
	if (g_current_eis_mode == RK_HORIZON_STEADY)
		vi_chn[1].s32ChnId = g_vi_for_vo_chn_id;
	else
		vi_chn[1].s32ChnId = g_vi_ext_chn_id;
	for (int i = 0; i < 2; i++) {
		gdc_chn[i].enModId = RK_ID_GDC;
		gdc_chn[i].s32DevId = 0;
		gdc_chn[i].s32ChnId = i;
	}
	for (int i = 0; i < 4; i++) {
		venc_chn[i].enModId = RK_ID_VENC;
		venc_chn[i].s32DevId = 0;
		venc_chn[i].s32ChnId = i;
	}
	vo_chn[0].enModId = RK_ID_VO;
	vo_chn[0].s32DevId = dp_vo_layer;
	vo_chn[0].s32ChnId = 0;

	vo_chn[1].enModId = RK_ID_VO;
	vo_chn[1].s32DevId = lcd_vo_layer;
	vo_chn[1].s32ChnId = 0;

	memset(&target_venc_chn, 0, sizeof(target_venc_chn));
	target_venc_chn.enModId = RK_ID_VENC;
	target_venc_chn.s32DevId = 0;
	if (g_current_mode == RK_PHOTO_MODE)
		target_venc_chn.s32ChnId = JPEG_VENC_CHN;
	else
		target_venc_chn.s32ChnId = VIDEO_PIPE_NORMAL;

	if (g_current_eis_mode != RK_EIS_OFF &&
	    (g_current_mode == RK_PHOTO_MODE || g_current_mode == RK_VIDEO_MODE)) {
		ret = RK_MPI_SYS_Bind(&vi_chn[0], &gdc_chn[0]);
		if (ret != RK_SUCCESS)
			LOG_ERROR("vi 0 bind gdc 0 fail:%x", ret);
		if (!enable_eis_debug) {
			ret = RK_MPI_SYS_Bind(&gdc_chn[0], &target_venc_chn);
			if (ret != RK_SUCCESS)
				LOG_ERROR("vi 0 bind venc %d fail:%x", target_venc_chn.s32ChnId, ret);
		}
		if (enable_lcd || enable_dp) {
			ret = RK_MPI_SYS_Bind(&vi_chn[1], &gdc_chn[1]);
			if (ret != RK_SUCCESS)
				LOG_ERROR("vi 1 bind gdc 1 fail:%x", ret);
			ret = RK_MPI_SYS_Bind(&gdc_chn[1], &vo_chn[1]);
			if (ret != RK_SUCCESS)
				LOG_ERROR("vi 1 bind gdc 1 fail:%x", ret);
		}
	} else {
		ret = RK_MPI_SYS_Bind(&vi_chn[0], &target_venc_chn);
		if (ret != RK_SUCCESS)
			LOG_ERROR("vi 0 bind venc %d fail:%x", target_venc_chn.s32ChnId, ret);
		if (enable_lcd || enable_dp) {
			ret = RK_MPI_SYS_Bind(&vi_chn[1], &vo_chn[1]);
			if (ret != RK_SUCCESS)
				LOG_ERROR("vi 1 bind vo 1 fail:%x", ret);
		}
	}
	// HACK: avoid rockit error
	VENC_RECV_PIC_PARAM_S stRecvParam;
	VENC_STREAM_S stFrame;
	memset(&stRecvParam, 0, sizeof(stRecvParam));
	stRecvParam.s32RecvPicNum = 1;
	stFrame.pstPack = malloc(sizeof(VENC_PACK_S));
	if (g_current_mode != RK_PHOTO_MODE) {
		RK_MPI_VENC_StartRecvFrame(VIDEO_PIPE_NORMAL, &stRecvParam);
		ret = RK_MPI_VENC_GetStream(VIDEO_PIPE_NORMAL, &stFrame, 1000);
		if (!ret)
			RK_MPI_VENC_ReleaseStream(VIDEO_PIPE_NORMAL, &stFrame);
		else
			LOG_ERROR("RK_MPI_VENC_GetStream failed %#X!", ret);
		RK_MPI_VENC_StopRecvFrame(VIDEO_PIPE_NORMAL);
	}
	free(stFrame.pstPack);

	return ret;
}

static int rkipc_bind_deinit() {
	int ret;
	MPP_CHN_S target_venc_chn;

	memset(&target_venc_chn, 0, sizeof(target_venc_chn));
	target_venc_chn.enModId = RK_ID_VENC;
	target_venc_chn.s32DevId = 0;
	if (g_current_mode == RK_PHOTO_MODE)
		target_venc_chn.s32ChnId = JPEG_VENC_CHN;
	else
		target_venc_chn.s32ChnId = VIDEO_PIPE_NORMAL;

	if (g_current_eis_mode != RK_EIS_OFF &&
	    (g_current_mode == RK_PHOTO_MODE || g_current_mode == RK_VIDEO_MODE)) {
		if (!enable_eis_debug) {
			ret = RK_MPI_SYS_UnBind(&gdc_chn[0], &target_venc_chn);
			if (ret != RK_SUCCESS)
				LOG_ERROR("vi 0 unbind venc %d fail:%x", target_venc_chn.s32ChnId, ret);
		}
		ret = RK_MPI_SYS_UnBind(&vi_chn[0], &gdc_chn[0]);
		if (ret != RK_SUCCESS)
			LOG_ERROR("vi 0 unbind gdc 0 fail:%x", ret);

		if (enable_lcd || enable_dp) {
			ret = RK_MPI_SYS_UnBind(&gdc_chn[1], &vo_chn[1]);
			if (ret != RK_SUCCESS)
				LOG_ERROR("vi 1 unbind gdc 1 fail:%x", ret);
			ret = RK_MPI_SYS_UnBind(&vi_chn[1], &gdc_chn[1]);
			if (ret != RK_SUCCESS)
				LOG_ERROR("vi 1 unbind gdc 1 fail:%x", ret);
		}
	} else {
		ret = RK_MPI_SYS_UnBind(&vi_chn[0], &target_venc_chn);
		if (ret != RK_SUCCESS)
			LOG_ERROR("vi 0 unbind venc %d fail:%x", target_venc_chn.s32ChnId, ret);
		if (enable_lcd || enable_dp) {
			ret = RK_MPI_SYS_UnBind(&vi_chn[1], &vo_chn[1]);
			if (ret != RK_SUCCESS)
				LOG_ERROR("vi 1 unbind vo 1 fail:%x", ret);
		}
	}

	return ret;
}

static int rkipc_vo_init() {
	int ret = 0;
	// VO init
	VO_PUB_ATTR_S VoPubAttr;
	VO_VIDEO_LAYER_ATTR_S stLayerAttr;
	VO_CSC_S VideoCSC;
	VO_CHN_ATTR_S VoChnAttr;
	RK_U32 u32DispBufLen;
	memset(&VoPubAttr, 0, sizeof(VO_PUB_ATTR_S));
	memset(&stLayerAttr, 0, sizeof(VO_VIDEO_LAYER_ATTR_S));
	memset(&VideoCSC, 0, sizeof(VO_CSC_S));
	memset(&VoChnAttr, 0, sizeof(VoChnAttr));

	if (enable_dp) {
		VoPubAttr.enIntfType = VO_INTF_DP;
		VoPubAttr.enIntfSync = VO_OUTPUT_1080P60;
		dp_vo_layer = RK3576_VOP_LAYER_CLUSTER0;

		ret = RK_MPI_VO_SetPubAttr(dp_vo_dev_id, &VoPubAttr);
		ret = RK_MPI_VO_Enable(dp_vo_dev_id);
		ret = RK_MPI_VO_GetLayerDispBufLen(dp_vo_layer, &u32DispBufLen);
		LOG_INFO("Get dp_vo_layer %d disp buf len is %d.\n", dp_vo_layer, u32DispBufLen);
		u32DispBufLen = 3;
		ret = RK_MPI_VO_SetLayerDispBufLen(dp_vo_layer, u32DispBufLen);
		LOG_INFO("Agin Get dp_vo_layer %d disp buf len is %d.\n", dp_vo_layer, u32DispBufLen);

		ret = RK_MPI_VO_GetPubAttr(dp_vo_dev_id, &VoPubAttr);
		if ((VoPubAttr.stSyncInfo.u16Hact == 0) || (VoPubAttr.stSyncInfo.u16Vact == 0)) {
			VoPubAttr.stSyncInfo.u16Hact = dp_vo_w;
			VoPubAttr.stSyncInfo.u16Vact = dp_vo_h;
		}

		stLayerAttr.stDispRect.s32X = 0;
		stLayerAttr.stDispRect.s32Y = 0;
		stLayerAttr.stDispRect.u32Width = dp_vo_w;
		stLayerAttr.stDispRect.u32Height = dp_vo_h;
		stLayerAttr.stImageSize.u32Width = dp_vo_w;
		stLayerAttr.stImageSize.u32Height = dp_vo_h;
		LOG_INFO("stLayerAttr W=%d, H=%d\n", stLayerAttr.stDispRect.u32Width,
		         stLayerAttr.stDispRect.u32Height);

		stLayerAttr.u32DispFrmRt = 30;
		stLayerAttr.enPixFormat = RK_FMT_YUV420SP;
		VideoCSC.enCscMatrix = VO_CSC_MATRIX_IDENTITY;
		VideoCSC.u32Contrast = 50;
		VideoCSC.u32Hue = 50;
		VideoCSC.u32Luma = 50;
		VideoCSC.u32Satuature = 50;
		RK_S32 u32VoChn = 0;

		/*bind layer0 to device hd0*/
		ret = RK_MPI_VO_BindLayer(dp_vo_layer, dp_vo_dev_id, VO_LAYER_MODE_GRAPHIC);
		ret = RK_MPI_VO_SetLayerAttr(dp_vo_layer, &stLayerAttr);
		ret = RK_MPI_VO_SetLayerSpliceMode(dp_vo_layer, VO_SPLICE_MODE_RGA);
		ret = RK_MPI_VO_EnableLayer(dp_vo_layer);
		ret = RK_MPI_VO_SetLayerCSC(dp_vo_layer, &VideoCSC);

		VoChnAttr.bDeflicker = RK_FALSE;
		VoChnAttr.u32Priority = 1;
		VoChnAttr.stRect.s32X = 0;
		VoChnAttr.stRect.s32Y = 0;
		VoChnAttr.stRect.u32Width = stLayerAttr.stDispRect.u32Width;
		VoChnAttr.stRect.u32Height = stLayerAttr.stDispRect.u32Height;
		ret = RK_MPI_VO_SetChnAttr(dp_vo_layer, 0, &VoChnAttr);
		ret = RK_MPI_VO_EnableChn(dp_vo_layer, u32VoChn);
		if (ret != RK_SUCCESS) {
			LOG_ERROR("create %d layer %d ch vo failed!\n", dp_vo_layer, u32VoChn);
			// return ret;
		}
		LOG_INFO("RK_MPI_VO_EnableChn success\n");
	}

	// LCD device is create by ui, and never disabled.
	if (enable_lcd) {
		RK_S32 u32VoChn = 0;
		VoPubAttr.enIntfType = VO_INTF_LCD;
		VoPubAttr.enIntfSync = VO_OUTPUT_DEFAULT;
		lcd_vo_layer = RK3576_VOP_LAYER_ESMART1;

#ifndef DRAW_UI_BY_VO
		ret = RK_MPI_VO_SetPubAttr(lcd_vo_dev_id, &VoPubAttr);
		ret = RK_MPI_VO_Enable(lcd_vo_dev_id);
		ret = RK_MPI_VO_GetLayerDispBufLen(lcd_vo_layer, &u32DispBufLen);
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

		stLayerAttr.u32DispFrmRt = 30;
		stLayerAttr.enPixFormat = RK_FMT_YUV420SP;
		VideoCSC.enCscMatrix = VO_CSC_MATRIX_IDENTITY;
		VideoCSC.u32Contrast = 50;
		VideoCSC.u32Hue = 50;
		VideoCSC.u32Luma = 50;
		VideoCSC.u32Satuature = 50;
		RK_S32 u32VoChn = 0;

		/*bind layer0 to device hd0*/
		ret = RK_MPI_VO_BindLayer(lcd_vo_layer, lcd_vo_dev_id, VO_LAYER_MODE_GRAPHIC);
		ret = RK_MPI_VO_SetLayerAttr(lcd_vo_layer, &stLayerAttr);
		ret = RK_MPI_VO_SetLayerSpliceMode(lcd_vo_layer, VO_SPLICE_MODE_RGA);
		ret = RK_MPI_VO_EnableLayer(lcd_vo_layer);
		ret = RK_MPI_VO_SetLayerCSC(lcd_vo_layer, &VideoCSC);
#endif // DRAW_UI_BY_VO
		VoChnAttr.bDeflicker = RK_FALSE;
		VoChnAttr.u32Priority = 1;
		VoChnAttr.stRect.s32X = 0;
		VoChnAttr.stRect.s32Y = 0;
		VoChnAttr.stRect.u32Width = lcd_vo_w;
		VoChnAttr.stRect.u32Height = lcd_vo_h;
		VoChnAttr.enRotation = ROTATION_90;
		ret = RK_MPI_VO_SetChnAttr(lcd_vo_layer, u32VoChn, &VoChnAttr);
		ret = RK_MPI_VO_EnableChn(lcd_vo_layer, u32VoChn);
		if (ret != RK_SUCCESS) {
			LOG_ERROR("create %d layer %d ch vo failed!\n", lcd_vo_layer, u32VoChn);
			// return ret;
		}
		LOG_INFO("RK_MPI_VO_EnableChn success\n");
	}

	return 0;
}

static int rkipc_vo_deinit() {
	int ret;
	RK_S32 u32VoChn = 0;

	if (enable_dp) {
		ret = RK_MPI_VO_DisableChn(dp_vo_layer, u32VoChn);
		if (ret) {
			LOG_ERROR("RK_MPI_VO_DisableChn failed, ret is %#x\n", ret);
			return -1;
		}
		ret = RK_MPI_VO_DisableLayer(dp_vo_layer);
		if (ret) {
			LOG_ERROR("RK_MPI_VO_DisableLayer failed, ret is %#x\n", ret);
			return -1;
		}
		ret = RK_MPI_VO_Disable(dp_vo_dev_id);
		if (ret) {
			LOG_ERROR("RK_MPI_VO_Disable failed, ret is %#x\n", ret);
			return -1;
		}
		ret = RK_MPI_VO_UnBindLayer(dp_vo_layer, dp_vo_dev_id);
		if (ret) {
			LOG_ERROR("RK_MPI_VO_UnBindLayer failed, ret is %#x\n", ret);
			return -1;
		}
	}
	// LCD device is create by ui, and never disabled.
	if (enable_lcd) {
		ret = RK_MPI_VO_DisableChn(lcd_vo_layer, u32VoChn);
		if (ret) {
			LOG_ERROR("RK_MPI_VO_DisableChn failed, ret is %#x\n", ret);
			return -1;
		}
#ifndef DRAW_UI_BY_VO
		ret = RK_MPI_VO_DisableLayer(lcd_vo_layer);
		if (ret) {
			LOG_ERROR("RK_MPI_VO_DisableLayer failed, ret is %#x\n", ret);
			return -1;
		}
		ret = RK_MPI_VO_Disable(lcd_vo_dev_id);
		if (ret) {
			LOG_ERROR("RK_MPI_VO_Disable failed, ret is %#x\n", ret);
			return -1;
		}
		ret = RK_MPI_VO_UnBindLayer(lcd_vo_layer, lcd_vo_dev_id);
		if (ret) {
			LOG_ERROR("RK_MPI_VO_UnBindLayer failed, ret is %#x\n", ret);
			return -1;
		}
#endif // DRAW_UI_BY_VO
	}

#ifndef DRAW_UI_BY_VO
	ret = RK_MPI_VO_CloseFd();
	if (ret) {
		LOG_ERROR("RK_MPI_VO_CloseFd failed, ret is %#x\n", ret);
		return -1;
	}
#endif
	return 0;
}

// export API
int rk_video_get_gop(int stream_id, int *value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.%d:gop", stream_id);
	*value = rk_param_get_int(entry, -1);

	return 0;
}

int rk_video_set_gop(int stream_id, int value) {
	char entry[128] = {'\0'};
	VENC_CHN_ATTR_S venc_chn_attr;
	memset(&venc_chn_attr, 0, sizeof(venc_chn_attr));
	RK_MPI_VENC_GetChnAttr(stream_id, &venc_chn_attr);
	snprintf(entry, 127, "video.%d:output_data_type", stream_id);
	tmp_output_data_type = rk_param_get_string(entry, "H.264");
	snprintf(entry, 127, "video.%d:rc_mode", stream_id);
	tmp_rc_mode = rk_param_get_string(entry, "CBR");
	if (!strcmp(tmp_output_data_type, "H.264")) {
		if (!strcmp(tmp_rc_mode, "CBR"))
			venc_chn_attr.stRcAttr.stH264Cbr.u32Gop = value;
		else
			venc_chn_attr.stRcAttr.stH264Vbr.u32Gop = value;
	} else if (!strcmp(tmp_output_data_type, "H.265")) {
		if (!strcmp(tmp_rc_mode, "CBR"))
			venc_chn_attr.stRcAttr.stH265Cbr.u32Gop = value;
		else
			venc_chn_attr.stRcAttr.stH265Vbr.u32Gop = value;
	} else {
		LOG_ERROR("tmp_output_data_type is %s, not support\n", tmp_output_data_type);
		return -1;
	}
	RK_MPI_VENC_SetChnAttr(stream_id, &venc_chn_attr);
	snprintf(entry, 127, "video.%d:gop", stream_id);
	rk_param_set_int(entry, value);

	return 0;
}

int rk_video_get_max_rate(int stream_id, int *value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.%d:max_rate", stream_id);
	*value = rk_param_get_int(entry, -1);

	return 0;
}

int rk_video_set_max_rate(int stream_id, int value) {
	VENC_CHN_ATTR_S venc_chn_attr;
	memset(&venc_chn_attr, 0, sizeof(venc_chn_attr));
	RK_MPI_VENC_GetChnAttr(stream_id, &venc_chn_attr);
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.%d:output_data_type", stream_id);
	tmp_output_data_type = rk_param_get_string(entry, "H.264");
	snprintf(entry, 127, "video.%d:rc_mode", stream_id);
	tmp_rc_mode = rk_param_get_string(entry, "CBR");
	if (!strcmp(tmp_output_data_type, "H.264")) {
		if (!strcmp(tmp_rc_mode, "CBR")) {
			venc_chn_attr.stRcAttr.stH264Cbr.u32BitRate = value;
		} else {
			venc_chn_attr.stRcAttr.stH264Vbr.u32MinBitRate = value / 3;
			venc_chn_attr.stRcAttr.stH264Vbr.u32BitRate = value / 3 * 2;
			venc_chn_attr.stRcAttr.stH264Vbr.u32MaxBitRate = value;
		}
	} else if (!strcmp(tmp_output_data_type, "H.265")) {
		if (!strcmp(tmp_rc_mode, "CBR")) {
			venc_chn_attr.stRcAttr.stH265Cbr.u32BitRate = value;
		} else {
			venc_chn_attr.stRcAttr.stH265Vbr.u32MinBitRate = value / 3;
			venc_chn_attr.stRcAttr.stH265Vbr.u32BitRate = value / 3 * 2;
			venc_chn_attr.stRcAttr.stH265Vbr.u32MaxBitRate = value;
		}
	} else {
		LOG_ERROR("tmp_output_data_type is %s, not support\n", tmp_output_data_type);
		return -1;
	}
	RK_MPI_VENC_SetChnAttr(stream_id, &venc_chn_attr);
	snprintf(entry, 127, "video.%d:max_rate", stream_id);
	rk_param_set_int(entry, value);
	snprintf(entry, 127, "video.%d:mid_rate", stream_id);
	rk_param_set_int(entry, value / 3 * 2);
	snprintf(entry, 127, "video.%d:min_rate", stream_id);
	rk_param_set_int(entry, value / 3);

	return 0;
}

int rk_video_get_RC_mode(int stream_id, const char **value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.%d:rc_mode", stream_id);
	*value = rk_param_get_string(entry, "CBR");

	return 0;
}

int rk_video_set_RC_mode(int stream_id, const char *value) {
	char entry_output_data_type[128] = {'\0'};
	char entry_gop[128] = {'\0'};
	char entry_max_rate[128] = {'\0'};
	char entry_dst_frame_rate_den[128] = {'\0'};
	char entry_dst_frame_rate_num[128] = {'\0'};
	char entry_src_frame_rate_den[128] = {'\0'};
	char entry_src_frame_rate_num[128] = {'\0'};
	char entry_rc_mode[128] = {'\0'};
	snprintf(entry_output_data_type, 127, "video.%d:output_data_type", stream_id);
	snprintf(entry_gop, 127, "video.%d:gop", stream_id);
	snprintf(entry_max_rate, 127, "video.%d:max_rate", stream_id);
	snprintf(entry_dst_frame_rate_den, 127, "video.%d:dst_frame_rate_den", stream_id);
	snprintf(entry_dst_frame_rate_num, 127, "video.%d:dst_frame_rate_num", stream_id);
	snprintf(entry_src_frame_rate_den, 127, "video.%d:src_frame_rate_den", stream_id);
	snprintf(entry_src_frame_rate_num, 127, "video.%d:src_frame_rate_num", stream_id);
	snprintf(entry_rc_mode, 127, "video.%d:rc_mode", stream_id);

	VENC_CHN_ATTR_S venc_chn_attr;
	memset(&venc_chn_attr, 0, sizeof(venc_chn_attr));
	RK_MPI_VENC_GetChnAttr(stream_id, &venc_chn_attr);
	tmp_output_data_type = rk_param_get_string(entry_output_data_type, "H.264");
	if (!strcmp(tmp_output_data_type, "H.264")) {
		if (!strcmp(value, "CBR")) {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
			venc_chn_attr.stRcAttr.stH264Cbr.u32Gop = rk_param_get_int(entry_gop, -1);
			venc_chn_attr.stRcAttr.stH264Cbr.u32BitRate = rk_param_get_int(entry_max_rate, -1);
			venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen =
			    rk_param_get_int(entry_dst_frame_rate_den, -1);
			venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum =
			    rk_param_get_int(entry_dst_frame_rate_num, -1);
			venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen =
			    rk_param_get_int(entry_src_frame_rate_den, -1);
			venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum =
			    rk_param_get_int(entry_src_frame_rate_num, -1);
		} else {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264VBR;
			venc_chn_attr.stRcAttr.stH264Vbr.u32Gop = rk_param_get_int(entry_gop, -1);
			venc_chn_attr.stRcAttr.stH264Vbr.u32BitRate = rk_param_get_int(entry_max_rate, -1);
			venc_chn_attr.stRcAttr.stH264Vbr.fr32DstFrameRateDen =
			    rk_param_get_int(entry_dst_frame_rate_den, -1);
			venc_chn_attr.stRcAttr.stH264Vbr.fr32DstFrameRateNum =
			    rk_param_get_int(entry_dst_frame_rate_num, -1);
			venc_chn_attr.stRcAttr.stH264Vbr.u32SrcFrameRateDen =
			    rk_param_get_int(entry_src_frame_rate_den, -1);
			venc_chn_attr.stRcAttr.stH264Vbr.u32SrcFrameRateNum =
			    rk_param_get_int(entry_src_frame_rate_num, -1);
		}
	} else if (!strcmp(tmp_output_data_type, "H.265")) {
		if (!strcmp(value, "CBR")) {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
			venc_chn_attr.stRcAttr.stH265Cbr.u32Gop = rk_param_get_int(entry_gop, -1);
			venc_chn_attr.stRcAttr.stH265Cbr.u32BitRate = rk_param_get_int(entry_max_rate, -1);
			venc_chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen =
			    rk_param_get_int(entry_dst_frame_rate_den, -1);
			venc_chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum =
			    rk_param_get_int(entry_dst_frame_rate_num, -1);
			venc_chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen =
			    rk_param_get_int(entry_src_frame_rate_den, -1);
			venc_chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum =
			    rk_param_get_int(entry_src_frame_rate_num, -1);
		} else {
			venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265VBR;
			venc_chn_attr.stRcAttr.stH265Vbr.u32Gop = rk_param_get_int(entry_gop, -1);
			venc_chn_attr.stRcAttr.stH265Vbr.u32BitRate = rk_param_get_int(entry_max_rate, -1);
			venc_chn_attr.stRcAttr.stH265Vbr.fr32DstFrameRateDen =
			    rk_param_get_int(entry_dst_frame_rate_den, -1);
			venc_chn_attr.stRcAttr.stH265Vbr.fr32DstFrameRateNum =
			    rk_param_get_int(entry_dst_frame_rate_num, -1);
			venc_chn_attr.stRcAttr.stH265Vbr.u32SrcFrameRateDen =
			    rk_param_get_int(entry_src_frame_rate_den, -1);
			venc_chn_attr.stRcAttr.stH265Vbr.u32SrcFrameRateNum =
			    rk_param_get_int(entry_src_frame_rate_num, -1);
		}
	} else {
		LOG_ERROR("tmp_output_data_type is %s, not support\n", tmp_output_data_type);
		return -1;
	}
	RK_MPI_VENC_SetChnAttr(stream_id, &venc_chn_attr);
	rk_param_set_string(entry_rc_mode, value);

	return 0;
}

int rk_video_get_output_data_type(int stream_id, const char **value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.%d:output_data_type", stream_id);
	*value = rk_param_get_string(entry, "H.265");

	return 0;
}

int rk_video_set_output_data_type(int stream_id, const char *value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.%d:output_data_type", stream_id);
	rk_param_set_string(entry, value);
	rk_video_restart();

	return 0;
}

int rk_video_get_rc_quality(int stream_id, const char **value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.%d:rc_quality", stream_id);
	*value = rk_param_get_string(entry, "high");

	return 0;
}

int rk_video_set_rc_quality(int stream_id, const char *value) {
	char entry_rc_quality[128] = {'\0'};
	char entry_output_data_type[128] = {'\0'};

	snprintf(entry_rc_quality, 127, "video.%d:rc_quality", stream_id);
	snprintf(entry_output_data_type, 127, "video.%d:output_data_type", stream_id);
	tmp_output_data_type = rk_param_get_string(entry_output_data_type, "H.264");

	VENC_RC_PARAM_S venc_rc_param;
	RK_MPI_VENC_GetRcParam(stream_id, &venc_rc_param);
	if (!strcmp(tmp_output_data_type, "H.264")) {
		if (!strcmp(value, "highest")) {
			venc_rc_param.stParamH264.u32MinQp = 10;
		} else if (!strcmp(value, "higher")) {
			venc_rc_param.stParamH264.u32MinQp = 15;
		} else if (!strcmp(value, "high")) {
			venc_rc_param.stParamH264.u32MinQp = 20;
		} else if (!strcmp(value, "medium")) {
			venc_rc_param.stParamH264.u32MinQp = 25;
		} else if (!strcmp(value, "low")) {
			venc_rc_param.stParamH264.u32MinQp = 30;
		} else if (!strcmp(value, "lower")) {
			venc_rc_param.stParamH264.u32MinQp = 35;
		} else {
			venc_rc_param.stParamH264.u32MinQp = 40;
		}
	} else if (!strcmp(tmp_output_data_type, "H.265")) {
		if (!strcmp(value, "highest")) {
			venc_rc_param.stParamH265.u32MinQp = 10;
		} else if (!strcmp(value, "higher")) {
			venc_rc_param.stParamH265.u32MinQp = 15;
		} else if (!strcmp(value, "high")) {
			venc_rc_param.stParamH265.u32MinQp = 20;
		} else if (!strcmp(value, "medium")) {
			venc_rc_param.stParamH265.u32MinQp = 25;
		} else if (!strcmp(value, "low")) {
			venc_rc_param.stParamH265.u32MinQp = 30;
		} else if (!strcmp(value, "lower")) {
			venc_rc_param.stParamH265.u32MinQp = 35;
		} else {
			venc_rc_param.stParamH265.u32MinQp = 40;
		}
	} else {
		LOG_ERROR("tmp_output_data_type is %s, not support\n", tmp_output_data_type);
		return -1;
	}
	RK_MPI_VENC_SetRcParam(stream_id, &venc_rc_param);
	rk_param_set_string(entry_rc_quality, value);

	return 0;
}

int rk_video_get_smart(int stream_id, const char **value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.%d:smart", stream_id);
	*value = rk_param_get_string(entry, "close");

	return 0;
}

int rk_video_set_smart(int stream_id, const char *value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.%d:smart", stream_id);
	rk_param_set_string(entry, value);
	rk_video_restart();

	return 0;
}

int rk_video_get_gop_mode(int stream_id, const char **value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.%d:gop_mode", stream_id);
	*value = rk_param_get_string(entry, "close");

	return 0;
}

int rk_video_set_gop_mode(int stream_id, const char *value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.%d:gop_mode", stream_id);
	rk_param_set_string(entry, value);
	rk_video_restart();

	return 0;
}

int rk_video_get_stream_type(int stream_id, const char **value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.%d:stream_type", stream_id);
	*value = rk_param_get_string(entry, "mainStream");

	return 0;
}

int rk_video_set_stream_type(int stream_id, const char *value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.%d:stream_type", stream_id);
	rk_param_set_string(entry, value);

	return 0;
}

int rk_video_get_h264_profile(int stream_id, const char **value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.%d:h264_profile", stream_id);
	*value = rk_param_get_string(entry, "high");

	return 0;
}

int rk_video_set_h264_profile(int stream_id, const char *value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.%d:h264_profile", stream_id);
	rk_param_set_string(entry, value);
	rk_video_restart();

	return 0;
}

int rk_video_get_resolution(int stream_id, char **value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.%d:width", stream_id);
	int width = rk_param_get_int(entry, 0);
	snprintf(entry, 127, "video.%d:height", stream_id);
	int height = rk_param_get_int(entry, 0);
	sprintf(*value, "%d*%d", width, height);

	return 0;
}

int rk_video_set_resolution(int stream_id, const char *value) {
	char entry[128] = {'\0'};
	int width, height;

	sscanf(value, "%d*%d", &width, &height);
	LOG_INFO("value is %s, width is %d, height is %d\n", value, width, height);
	snprintf(entry, 127, "video.%d:width", stream_id);
	rk_param_set_int(entry, width);
	snprintf(entry, 127, "video.%d:height", stream_id);
	rk_param_set_int(entry, height);
	rk_video_restart();

	return 0;
}

int rk_video_get_frame_rate(int stream_id, char **value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.%d:dst_frame_rate_den", stream_id);
	int den = rk_param_get_int(entry, -1);
	snprintf(entry, 127, "video.%d:dst_frame_rate_num", stream_id);
	int num = rk_param_get_int(entry, -1);
	if (den == 1)
		sprintf(*value, "%d", num);
	else
		sprintf(*value, "%d/%d", num, den);

	return 0;
}

int rk_video_set_frame_rate(int stream_id, const char *value) {
	char entry[128] = {'\0'};
	int den, num;
	if (strchr(value, '/') == NULL) {
		den = 1;
		sscanf(value, "%d", &num);
	} else {
		sscanf(value, "%d/%d", &num, &den);
	}
	LOG_INFO("num is %d, den is %d\n", num, den);

	VENC_CHN_ATTR_S venc_chn_attr;
	memset(&venc_chn_attr, 0, sizeof(venc_chn_attr));
	RK_MPI_VENC_GetChnAttr(stream_id, &venc_chn_attr);
	snprintf(entry, 127, "video.%d:output_data_type", stream_id);
	tmp_output_data_type = rk_param_get_string(entry, "H.264");
	snprintf(entry, 127, "video.%d:rc_mode", stream_id);
	tmp_rc_mode = rk_param_get_string(entry, "CBR");
	if (!strcmp(tmp_output_data_type, "H.264")) {
		venc_chn_attr.stVencAttr.enType = RK_VIDEO_ID_AVC;
		if (!strcmp(tmp_rc_mode, "CBR")) {
			venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = den;
			venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = num;
		} else {
			venc_chn_attr.stRcAttr.stH264Vbr.fr32DstFrameRateDen = den;
			venc_chn_attr.stRcAttr.stH264Vbr.fr32DstFrameRateNum = num;
		}
	} else if (!strcmp(tmp_output_data_type, "H.265")) {
		venc_chn_attr.stVencAttr.enType = RK_VIDEO_ID_HEVC;
		if (!strcmp(tmp_rc_mode, "CBR")) {
			venc_chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen = den;
			venc_chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum = num;
		} else {
			venc_chn_attr.stRcAttr.stH265Vbr.fr32DstFrameRateDen = den;
			venc_chn_attr.stRcAttr.stH265Vbr.fr32DstFrameRateNum = num;
		}
	} else {
		LOG_ERROR("tmp_output_data_type is %s, not support\n", tmp_output_data_type);
		return -1;
	}
	RK_MPI_VENC_SetChnAttr(stream_id, &venc_chn_attr);

	snprintf(entry, 127, "video.%d:dst_frame_rate_den", stream_id);
	rk_param_set_int(entry, den);
	snprintf(entry, 127, "video.%d:dst_frame_rate_num", stream_id);
	rk_param_set_int(entry, num);

	return 0;
}

int rk_video_reset_frame_rate(int stream_id) {
	int ret = 0;
	char *value = malloc(20);
	ret |= rk_video_get_frame_rate(stream_id, &value);
	ret |= rk_video_set_frame_rate(stream_id, value);
	free(value);

	return 0;
}

int rk_video_get_frame_rate_in(int stream_id, char **value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.%d:src_frame_rate_den", stream_id);
	int den = rk_param_get_int(entry, -1);
	snprintf(entry, 127, "video.%d:src_frame_rate_num", stream_id);
	int num = rk_param_get_int(entry, -1);
	if (den == 1)
		sprintf(*value, "%d", num);
	else
		sprintf(*value, "%d/%d", num, den);

	return 0;
}

int rk_video_set_frame_rate_in(int stream_id, const char *value) {
	char entry[128] = {'\0'};
	int den, num;
	if (strchr(value, '/') == NULL) {
		den = 1;
		sscanf(value, "%d", &num);
	} else {
		sscanf(value, "%d/%d", &num, &den);
	}
	LOG_INFO("num is %d, den is %d\n", num, den);
	snprintf(entry, 127, "video.%d:src_frame_rate_den", stream_id);
	rk_param_set_int(entry, den);
	snprintf(entry, 127, "video.%d:src_frame_rate_num", stream_id);
	rk_param_set_int(entry, num);
	rk_video_restart();

	return 0;
}

// jpeg
int rk_video_get_enable_cycle_snapshot(int *value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.jpeg:enable_cycle_snapshot");
	*value = rk_param_get_int(entry, -1);

	return 0;
}

int rk_video_set_enable_cycle_snapshot(int value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.jpeg:enable_cycle_snapshot");
	rk_param_set_int(entry, value);

	return 0;
}

int rk_video_get_image_quality(int *value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.jpeg:jpeg_qfactor");
	*value = rk_param_get_int(entry, -1);

	return 0;
}

int rk_video_set_image_quality(int value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.jpeg:jpeg_qfactor");
	rk_param_set_int(entry, value);

	return 0;
}

int rk_video_get_snapshot_interval_ms(int *value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.jpeg:snapshot_interval_ms");
	*value = rk_param_get_int(entry, 0);

	return 0;
}

int rk_video_set_snapshot_interval_ms(int value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.jpeg:snapshot_interval_ms");
	rk_param_set_int(entry, value);

	return 0;
}

int rk_video_get_jpeg_resolution(char **value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.jpeg:width");
	int width = rk_param_get_int(entry, 0);
	snprintf(entry, 127, "video.jpeg:height");
	int height = rk_param_get_int(entry, 0);
	sprintf(*value, "%d*%d", width, height);

	return 0;
}

int rk_video_set_jpeg_resolution(const char *value) {
	int width, height, ret;
	char entry[128] = {'\0'};
	sscanf(value, "%d*%d", &width, &height);
	snprintf(entry, 127, "video.jpeg:width");
	rk_param_set_int(entry, width);
	snprintf(entry, 127, "video.jpeg:height");
	rk_param_set_int(entry, height);

	return 0;
}

int rk_video_get_rotation(int *value) {
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.source:rotation");
	*value = rk_param_get_int(entry, 0);

	return 0;
}

int rk_video_set_rotation(int value) {
	LOG_INFO("value is %d\n", value);
	int rotation = 0;
	int ret = 0;
	char entry[128] = {'\0'};
	snprintf(entry, 127, "video.source:rotation");
	rk_param_set_int(entry, value);
	rk_video_restart();

	return 0;
}

static void *gdc_thread(void *arg) {
	fd_set readfds, writefds;
	int s32DevFd = RK_MPI_GDC_GetFd(0);
	LOG_INFO("s32DevFd is %d\n", s32DevFd);
	VIDEO_FRAME_INFO_S getFrame = {0};
	RK_S32 result = 0;
	struct timeval timeout = {0};
	int loopCount = 0;
	int ret;

	while (g_video_run_) {
		result = RK_MPI_GDC_GetFrame(0, &getFrame, 1000);
		if (result == 0) {
			void *data = RK_MPI_MB_Handle2VirAddr(getFrame.stVFrame.pMbBlk);
			RK_U64 len = RK_MPI_MB_GetSize(getFrame.stVFrame.pMbBlk);
			LOG_INFO("RK_MPI_GDC_GetFrame ok:count: %d data %p seq:%d pts:%lld ms, len=%llu\n",
			         loopCount, data, getFrame.stVFrame.u32TimeRef, getFrame.stVFrame.u64PTS / 1000,
			         len);
			// if (g_enable_vo) {
			// 	// ret = RK_MPI_VO_SendLayerFrame(lcd_vo_layer, vi_frame);
			// 	// if (ret)
			// 	// 	LOG_ERROR("RK_MPI_VO_SendLayerFrame timeout %x\n", ret);

			// 	ret = RK_MPI_VO_SendLayerFrame(dp_vo_layer, &getFrame);
			// 	if (ret)
			// 		LOG_ERROR("RK_MPI_VO_SendLayerFrame timeout %x\n", ret);
			// }

			// ret = RK_MPI_VENC_SendFrame(VIDEO_PIPE_NORMAL, &getFrame, 1000);
			// if (ret)
			// 	LOG_ERROR("RK_MPI_VENC_SendFrame timeout %x\n", ret);

			RK_MPI_GDC_ReleaseFrame(0, &getFrame);
			loopCount++;
			if ((getFrame.stVFrame.u32FrameFlag & FRAME_FLAG_SNAP_END) == FRAME_FLAG_SNAP_END) {
				RK_LOGI("reach eos frame.");
				break;
			}
		} else {
			LOG_WARN("RK_MPI_GDC_GetFrame timeout\n");
			break;
		}
	}

	return 0;
}

RK_S32 RKGdcSensorCB(RK_VOID *pUsr, GDC_SENSOR_INFO_S *pInfo) {
	// LOG_INFO("time stamp = %lld, temp is %lf,"
	// "gyro data is %lf %lf %lf, acc data is %lf %lf %lf\n",
	// pInfo->s64Timestamp, pInfo->dTemp,
	// pInfo->dGyroData[0], pInfo->dGyroData[1], pInfo->dGyroData[2],
	// pInfo->dAccData[0], pInfo->dAccData[1], pInfo->dAccData[2]);
	if (!g_start_record_)
		return GDC_INFO_CB_SUCCESS;
	sprintf(gdc_imu_info_buffer, "%lf %lf %lf %lf %lf %lf %lf %lld\n", pInfo->dAccData[0],
	        pInfo->dAccData[1], pInfo->dAccData[2], pInfo->dGyroData[0], pInfo->dGyroData[1],
	        pInfo->dGyroData[2], pInfo->dTemp, pInfo->s64Timestamp);

	fwrite(gdc_imu_info_buffer, sizeof(char), strlen(gdc_imu_info_buffer), imu_info_file);
	fflush(imu_info_file);

	return GDC_INFO_CB_SUCCESS;
}

RK_S32 RKGdcVframeCB(RK_VOID *pUsr, GDC_VFAME_INFO_S *pInfo) {
	VIDEO_FRAME_INFO_S gdc_frame;
	// void *data = RK_MPI_MB_Handle2VirAddr(pInfo->pMbBlk);
	// RK_U64 len = RK_MPI_MB_GetSize(pInfo->pMbBlk);
	// LOG_INFO("vframe seq = %d, data = %p, len = %lld, time stamp = %llu\n",
	//		pInfo->u32Seq, data, len, pInfo->u64PTS);
	// LOG_INFO("u64ExtraPts = %llu, u32RsSkew = %d, u32ExpTime = %d,"
	//		"u32Again = %d, u32Dgain = %d, u32Ispgain = %d, u32Ispgain = %lf\n",
	// 		pInfo->u64ExtraPts, pInfo->u32RsSkew, pInfo->u32ExpTime, pInfo->u32Again,
	//		pInfo->u32Dgain, pInfo->u32Ispgain, pInfo->dIso);

	if (!g_start_record_) {
		RK_MPI_MB_ReleaseMB(pInfo->pMbBlk);
		return GDC_INFO_CB_SUCCESS;
	}
	// 为保证imu数据记录早于vi数据，丢弃vi的前30帧
	gdc_debug_cnt++;
	if (gdc_debug_cnt < 30) {
		LOG_INFO("drop gdc frame cnt %d\n", gdc_debug_cnt);
		RK_MPI_MB_ReleaseMB(pInfo->pMbBlk);
		return GDC_INFO_CB_SUCCESS;
	}

	sprintf(gdc_vi_info_buffer, "%lld %d %d %d %d %d %lf\n", pInfo->u64ExtraPts, pInfo->u32RsSkew,
	        pInfo->u32ExpTime, pInfo->u32Again, pInfo->u32Dgain, pInfo->u32Ispgain, pInfo->dIso);
	fwrite(gdc_vi_info_buffer, sizeof(char), strlen(gdc_vi_info_buffer), vi_info_file);
	fflush(vi_info_file);
	memset(&gdc_frame, 0, sizeof(VIDEO_FRAME_INFO_S));
	gdc_frame.stVFrame.pMbBlk = pInfo->pMbBlk;
	gdc_frame.stVFrame.u32Width = pInfo->u32Width;
	gdc_frame.stVFrame.u32Height = pInfo->u32Height;
	gdc_frame.stVFrame.u32VirWidth = pInfo->u32VirWidth;
	gdc_frame.stVFrame.u32VirHeight = pInfo->u32VirHeight;
	gdc_frame.stVFrame.enPixelFormat = pInfo->enPixelFormat;
	gdc_frame.stVFrame.enCompressMode = pInfo->enCompressMode;
	gdc_frame.stVFrame.u32TimeRef = pInfo->u32Seq;
	gdc_frame.stVFrame.u64PTS = pInfo->u64PTS;
	RK_MPI_VENC_SendFrame(VIDEO_PIPE_NORMAL, &gdc_frame, 1000);
	RK_MPI_MB_ReleaseMB(pInfo->pMbBlk);

	return GDC_INFO_CB_SUCCESS;
}

int rkipc_gdc_init() {
	RK_S32 ret = RK_SUCCESS;
	GDC_CHN_ATTR_S stAttr = {0};
	if (g_current_eis_mode == RK_EIS_OFF)
		return ret;

	stAttr.u32MaxInQueue = 8;
	stAttr.u32MaxOutQueue = 8;
	stAttr.s32SrcWidth = rk_param_get_int("video.source:width", 4032);
	stAttr.s32SrcHeight = rk_param_get_int("video.source:height", 2256);
	stAttr.s32DstWidth = rk_param_get_int("video.0:width", 3840);
	stAttr.s32DstHeight = rk_param_get_int("video.0:height", 2160);
	stAttr.enPixelFormat = RK_FMT_YUV420SP;
	stAttr.enCompMode = COMPRESS_MODE_NONE;
	memset(stAttr.cfgFile, 0, sizeof(stAttr.cfgFile));
	if (g_current_eis_mode == RK_HORIZON_STEADY) {
		memcpy(stAttr.cfgFile,
		       "/usr/share/rkeis_config_imx386_horizon_4032x3016_3840x2160_30fps.json",
		       strlen("/usr/share/rkeis_config_imx386_horizon_4032x3016_3840x2160_30fps.json"));
	} else if (g_current_eis_mode == RK_DISTORTION_CORRECTION) {
		memcpy(stAttr.cfgFile, "/usr/share/rkeis_config_imx386_4032x2256_3840x2160_30fps-distortion.json",
		       strlen("/usr/share/rkeis_config_imx386_4032x2256_3840x2160_30fps-distortion.json"));
	} else if (rk_param_get_int("isp.0.adjustment:fps", 30) == 58) {
		memcpy(stAttr.cfgFile, "/usr/share/rkeis_config_imx386_4032x2256_3840x2160_60fps.json",
		       strlen("/usr/share/rkeis_config_imx386_4032x2256_3840x2160_60fps.json"));
	} else {
		memcpy(stAttr.cfgFile, "/usr/share/rkeis_config_imx386_4032x2256_3840x2160_30fps.json",
		       strlen("/usr/share/rkeis_config_imx386_4032x2256_3840x2160_30fps.json"));
	}
	if (enable_eis_debug) {
		vi_info_file = fopen("/tmp/vi_info.txt", "w");
		imu_info_file = fopen("/tmp/imu_info.txt", "w");
		gdc_info_cb.pfnSensorCB = RKGdcSensorCB;
		gdc_info_cb.pfnVframeCB = RKGdcVframeCB;
		RK_MPI_GDC_Register_InfoCB(0, NULL, &gdc_info_cb);
	}
	ret = RK_MPI_GDC_CreateChn(0, &stAttr);

	if (enable_lcd || enable_dp) {
		stAttr.u32MaxInQueue = 8;
		stAttr.u32MaxOutQueue = 8;
		if (g_current_eis_mode == RK_HORIZON_STEADY) {
			stAttr.s32SrcWidth = 704;
			stAttr.s32SrcHeight = 512;
			stAttr.s32DstWidth = 672;
			stAttr.s32DstHeight = 368;
		} else {
			stAttr.s32SrcWidth = 704;
			stAttr.s32SrcHeight = 384;
			stAttr.s32DstWidth = 704;
			stAttr.s32DstHeight = 384;
		}
		stAttr.enPixelFormat = RK_FMT_YUV420SP;
		stAttr.enCompMode = COMPRESS_MODE_NONE;
		memset(stAttr.cfgFile, 0, sizeof(stAttr.cfgFile));
		if (g_current_eis_mode == RK_HORIZON_STEADY) {
			memcpy(stAttr.cfgFile,
			       "/usr/share/rkeis_config_imx386_horizon_704x512_672x368_30fps.json",
			       strlen("/usr/share/rkeis_config_imx386_horizon_704x512_672x368_30fps.json"));
		} else if (g_current_eis_mode == RK_DISTORTION_CORRECTION) {
			memcpy(stAttr.cfgFile, "/usr/share/rkeis_config_imx386_704x384_704x384_30fps-distortion.json",
			       strlen("/usr/share/rkeis_config_imx386_704x384_704x384_30fps-distortion.json"));
		} else {
			memcpy(stAttr.cfgFile, "/usr/share/rkeis_config_imx386_704x384_704x384_30fps.json",
			       strlen("/usr/share/rkeis_config_imx386_704x384_704x384_30fps.json"));
		}
		ret = RK_MPI_GDC_CreateChn(1, &stAttr);
	}

	// pthread_t gdc;
	// pthread_create(&gdc, NULL, gdc_thread, NULL);

	return ret;
}

static int rkipc_gdc_deinit() {
	int ret = 0;
	if (g_current_eis_mode == RK_EIS_OFF)
		return ret;
	ret = RK_MPI_GDC_DestroyChn(0);
	if (enable_lcd || enable_dp)
		ret |= RK_MPI_GDC_DestroyChn(1);
	if (enable_eis_debug) {
		fclose(vi_info_file);
		fclose(imu_info_file);
	}

	return ret;
}

int rk_video_init() {
	LOG_INFO("begin\n");

	int ret = 0;
	enable_npu = rk_param_get_int("video.source:enable_npu", 0);
	enable_eis_debug = rk_param_get_int("video.source:enable_eis_debug", 0);
	pipe_id_ = rk_param_get_int("video.source:camera_id", 0);
	g_vi_chn_id = rk_param_get_int("video.source:vi_chn_id", 0);
	g_enable_vo = rk_param_get_int("video.source:enable_vo", 1);
	dp_vo_dev_id = rk_param_get_int("video.source:vo_dev_id", 3);
	enable_lcd = rk_param_get_int("video.source:enable_lcd", 0);
	enable_dp = rk_param_get_int("video.source:enable_dp", 0);
	LOG_INFO("g_vi_chn_id is %d, g_enable_vo is %d, dp_vo_dev_id is %d\n", g_vi_chn_id, g_enable_vo,
	         dp_vo_dev_id);
	g_video_run_ = 1;

	ret |= rkipc_vi_dev_init();
	ret |= rkipc_vi_chn_init();
	if (g_current_mode == RK_VIDEO_MODE || g_current_mode == RK_PHOTO_MODE)
		ret |= rkipc_gdc_init();
	ret |= rkipc_rtsp_init(RTSP_URL_0, RTSP_URL_1, RTSP_URL_2);
	ret |= rkipc_rtmp_init();
	if (g_current_mode == RK_VIDEO_MODE)
		ret |= rkipc_venc_normal_init();
	else if (g_current_mode == RK_SLOW_MOTION_MODE)
		ret |= rkipc_venc_slowmotion_init();
	else if (g_current_mode == RK_TIME_LAPSE_MODE)
		ret |= rkipc_venc_timelapse_init();
	else if (g_current_mode == RK_PHOTO_MODE)
		ret |= rkipc_jpeg_init();
	if (g_enable_vo)
		ret |= rkipc_vo_init();
	ret |= rkipc_bind_init();
	LOG_INFO("over\n");

	return ret;
}

int rk_video_deinit() {
	LOG_INFO("%s\n", __func__);

	g_video_run_ = 0;
	int ret = 0;
	rk_video_stop_record();
	ret |= rkipc_bind_deinit();
	if (g_enable_vo)
		ret |= rkipc_vo_deinit();
	if (g_current_mode == RK_VIDEO_MODE)
		ret |= rkipc_venc_normal_deinit();
	else if (g_current_mode == RK_SLOW_MOTION_MODE)
		ret |= rkipc_venc_slowmotion_deinit();
	else if (g_current_mode == RK_TIME_LAPSE_MODE)
		ret |= rkipc_venc_timelapse_deinit();
	else if (g_current_mode == RK_PHOTO_MODE) {
		pthread_join(jpeg_venc_thread_id, NULL);
		ret |= rkipc_jpeg_deinit();
	}
	if (g_current_mode == RK_VIDEO_MODE || g_current_mode == RK_PHOTO_MODE)
		ret |= rkipc_gdc_deinit();
	ret |= rkipc_vi_chn_deinit();
	ret |= rkipc_vi_dev_deinit();
	ret |= rkipc_rtmp_deinit();
	ret |= rkipc_rtsp_deinit();

	return ret;
}

extern char *rkipc_iq_file_path_;

int rk_video_restart() {
	int ret;
	ret = rk_storage_deinit();
	ret |= rk_video_deinit();
	ret |= rk_isp_deinit(0);
	ret |= rk_isp_init(0, rkipc_iq_file_path_);
	ret |= rk_video_init();
	ret |= rk_storage_init();

	return ret;
}

int rk_video_start_record(void) {
	VENC_RECV_PIC_PARAM_S stRecvParam;
	if (g_start_record_)
		return 0;
	g_start_record_ = 1;
	memset(&stRecvParam, 0, sizeof(VENC_RECV_PIC_PARAM_S));
	stRecvParam.s32RecvPicNum = -1;
	rk_storage_record_start(0);
	RK_MPI_VENC_StartRecvFrame(VIDEO_PIPE_NORMAL, &stRecvParam);
	if (g_current_mode == RK_VIDEO_MODE) {
		pthread_create(&venc_thread_normal, NULL, rkipc_video_loop, NULL);
	} else if (g_current_mode == RK_SLOW_MOTION_MODE) {
		pthread_create(&venc_thread_slowmotion, NULL, rkipc_slowmotion_loop, NULL);
	} else if (g_current_mode == RK_TIME_LAPSE_MODE) {
		pthread_create(&venc_thread_timelapse, NULL, rkipc_timelapse_loop, NULL);
	}
	return 0;
}

int rk_video_stop_record(void) {
	if (!g_start_record_)
		return 0;
	g_start_record_ = 0;
	RK_MPI_VENC_StopRecvFrame(VIDEO_PIPE_NORMAL);
	if (g_current_mode == RK_VIDEO_MODE) {
		pthread_join(venc_thread_normal, NULL);
	} else if (g_current_mode == RK_SLOW_MOTION_MODE) {
		pthread_join(venc_thread_slowmotion, NULL);
	} else if (g_current_mode == RK_TIME_LAPSE_MODE) {
		pthread_join(venc_thread_timelapse, NULL);
	}
	rk_storage_record_stop(0);
	return 0;
}

int rk_set_mode(RK_MODE_E mode) {
	int ret = 0;
	long long start_time = rkipc_get_curren_time_ms();
	if (g_current_mode == mode)
		return 0;
	rk_video_deinit();
	rk_isp_deinit(0);
	rk_storage_deinit();
	rk_param_deinit();
	if (mode == RK_VIDEO_MODE) {
		g_current_eis_mode = RK_NORMAL_STEADY;
		rk_param_init("/usr/share/rkipc-imx386-800w-30fps.ini");
	} else if (mode == RK_PHOTO_MODE) {
		g_current_eis_mode = RK_EIS_OFF;
		rk_param_init("/usr/share/rkipc-imx386-800w-30fps.ini");
	} else if (mode == RK_SLOW_MOTION_MODE) {
		g_current_eis_mode = RK_EIS_OFF;
		rk_param_init("/usr/share/rkipc-imx386-200w-120fps.ini");
	} else if (mode == RK_TIME_LAPSE_MODE) {
		g_current_eis_mode = RK_EIS_OFF;
		rk_param_init("/usr/share/rkipc-imx386-800w-30fps-timelapse.ini");
	}
	rk_storage_init();
	set_soc_freq();
	set_camera_format();
	g_current_mode = mode;
	rk_isp_init(0, rkipc_iq_file_path_);
	rk_isp_set_frame_rate_without_ini(0, rk_param_get_int("isp.0.adjustment:fps", 30));
	rk_video_init();
	LOG_INFO("set new mode %d, cost time %lld ms\n", mode, rkipc_get_curren_time_ms() - start_time);
	return 0;
}

RK_MODE_E rk_get_mode(void) { return g_current_mode; }

int rk_video_get_fps(void) {
	int fps = rk_param_get_int("isp.0.adjustment:fps", 30);
	return fps;
}

int rk_video_set_fps(int new_fps) {
	int fps = rk_param_get_int("isp.0.adjustment:fps", 30);
	if (fps == new_fps)
		return 0;
	if (g_current_mode == RK_SLOW_MOTION_MODE || g_current_mode == RK_TIME_LAPSE_MODE)
		return 0;
	fps = new_fps;
	rk_video_deinit();
	rk_isp_deinit(0);
	rk_storage_deinit();
	rk_param_deinit();
	if (fps == 30)
		rk_param_init("/usr/share/rkipc-imx386-800w-30fps.ini");
	else
		rk_param_init("/usr/share/rkipc-imx386-800w-58fps.ini");
	rk_storage_init();
	set_soc_freq();
	set_camera_format();
	rk_isp_init(0, rkipc_iq_file_path_);
	rk_isp_set_frame_rate_without_ini(0, fps);
	rk_video_init();
	return 0;
}

int rk_photo_set_max_num(int num) { max_capture_num = num; }

int rk_photo_get_max_num(void) { return max_capture_num; }

int rk_photo_get_done_num(void) { return max_capture_num - remain_capture_num; }

int rk_take_photo(void) {
	LOG_INFO("start\n");
	if (remain_capture_num != 0) {
		LOG_ERROR("there have unfinished tasks!\n");
		return -1;
	}
	VENC_RECV_PIC_PARAM_S stRecvParam;
	memset(&stRecvParam, 0, sizeof(VENC_RECV_PIC_PARAM_S));
	stRecvParam.s32RecvPicNum = max_capture_num;
	RK_MPI_VENC_StartRecvFrame(JPEG_VENC_CHN, &stRecvParam);
	remain_capture_num = max_capture_num;

	return 0;
}

int rk_init_mode() {
	set_camera_format();
	set_soc_freq();
	return 0;
}

RK_EIS_MODE_E rk_get_eis_mode(void) { return g_current_eis_mode; }

int rk_set_eis_mode(RK_EIS_MODE_E eis_mode) {
	long long start_time = rkipc_get_curren_time_ms();
	if (g_current_eis_mode == eis_mode)
		return 0;
	rk_video_deinit();
	rk_isp_deinit(0);
	// BUG: fix some isp bug. If no delay here, isp would be hang...
	usleep(100 * 1000);
	g_current_eis_mode = eis_mode;
	if (eis_mode == RK_HORIZON_STEADY)
		rk_param_init("/usr/share/rkipc-imx386-1200w-30fps.ini");
	else
		rk_param_init("/usr/share/rkipc-imx386-800w-30fps.ini");
	set_soc_freq();
	set_camera_format();
	rk_isp_init(0, rkipc_iq_file_path_);
	rk_video_init();
	LOG_INFO("set new eis mode %d, cost time %lld ms\n", eis_mode,
	         rkipc_get_curren_time_ms() - start_time);
	return 0;
}

int rk_set_hdr(bool enable) {
	if (enable_hdr == enable)
		return 0;
	enable_hdr = enable;
	LOG_INFO("%s hdr\n", enable ? "enable" : "disable");
}

int rk_get_hdr(void) { return enable_hdr; }

int rk_set_eis_debug(bool enable) {
	if (enable_eis_debug == enable)
		return 0;
	gdc_debug_cnt = 0;
	rk_video_deinit();
	rk_isp_deinit(0);
	usleep(100 * 1000);
	rk_param_set_int("video.source:enable_eis_debug", enable);
	rk_isp_init(0, rkipc_iq_file_path_);
	rk_video_init();
	LOG_INFO("%s eis debug mode\n", enable ? "enable" : "disable");
}

int rk_get_eis_debug(void) { return enable_eis_debug; }

int rk_enter_sleep(void) {
	LOG_INFO("enter\n");
	static struct timespec last_sleep_time = {};
	struct timespec current_time;
	clock_gettime(CLOCK_MONOTONIC, &current_time);
	LOG_INFO("last_sleep_time is %ld, current_time is %ld\n", last_sleep_time.tv_sec, current_time.tv_sec);

	// FIXME: need to unbind all node and make sure VO has no remaining frame
	// but gdc not support rebind in runtime now...
	if (last_sleep_time.tv_sec == 0) {
		rk_video_stop_record();
		// enter sleep
		write_sysfs_string("state", "/sys/power", "mem");
		clock_gettime(CLOCK_MONOTONIC, &last_sleep_time);
		return 0;
	}
	if (current_time.tv_sec - last_sleep_time.tv_sec < 5) {
		LOG_INFO("exit, skip sleep\n");
		return 0;
	}
	rk_video_stop_record();
	// enter sleep
	write_sysfs_string("state", "/sys/power", "mem");
	clock_gettime(CLOCK_MONOTONIC, &last_sleep_time);
	LOG_INFO("exit\n");
	return 0;
}
