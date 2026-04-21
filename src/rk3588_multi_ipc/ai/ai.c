#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include "log.h"
#include "rk_mpi_vpss.h"
#include "rk_mpi_mb.h"
#include "param.h"
#include <rknn_api.h>
#include <im2d.h>
#include <time.h>
#include "ai.h"
#include "ai_postprocess.h"

static rk_ai_context_t g_ai_ctx;
static volatile int    g_ai_run = 0;
static int             g_vpss_grp, g_vpss_chn;
static int             g_frame_w, g_frame_h;
static int             g_model_w, g_model_h;
static float           g_conf_thresh, g_nms_thresh;
static int             g_skip_n;

static unsigned char* load_file(const char *filename, int *model_size) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    int size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char *data = malloc(size);
    if (!data) { fclose(fp); return NULL; }
    fread(data, 1, size, fp);
    fclose(fp);
    *model_size = size;
    return data;
}

static void load_labels(const char *filename, rk_ai_context_t *ctx) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        ctx->labels = NULL;
        ctx->num_classes = 80;
        return;
    }
    int count = 0;
    char line[OBJ_NAME_MAX_SIZE];
    while (fgets(line, sizeof(line), fp)) {
        count++;
    }
    ctx->num_classes = count;
    ctx->labels = malloc(count * OBJ_NAME_MAX_SIZE);
    fseek(fp, 0, SEEK_SET);
    count = 0;
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = 0;
        strncpy(ctx->labels[count], line, OBJ_NAME_MAX_SIZE - 1);
        ctx->labels[count][OBJ_NAME_MAX_SIZE - 1] = '\0';
        count++;
    }
    fclose(fp);
}

static void* ai_inference_thread(void *arg) {
    rga_buffer_t src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));

    while (g_ai_run) {
        VIDEO_FRAME_INFO_S frame;
        int ret = RK_MPI_VPSS_GetChnFrame(g_vpss_grp, g_vpss_chn, &frame, 1000);
        if (ret != 0) {
            continue;
        }

        static int skip_cnt = 0;
        skip_cnt++;
        if (skip_cnt < g_skip_n) {
            RK_MPI_VPSS_ReleaseChnFrame(g_vpss_grp, g_vpss_chn, &frame);
            continue;
        }
        skip_cnt = 0;

        struct timespec start, end;
        double pre_time, run_time, post_time, draw_time;

        int fd = RK_MPI_MB_Handle2Fd(frame.stVFrame.pMbBlk);
        src = wrapbuffer_fd(fd, g_frame_w, g_frame_h, RK_FORMAT_YCbCr_420_SP);
        dst = wrapbuffer_virtualaddr(g_ai_ctx.rgb_buf, g_model_w, g_model_h, RK_FORMAT_RGB_888);

        clock_gettime(CLOCK_MONOTONIC, &start);
        imresize(src, dst);
        clock_gettime(CLOCK_MONOTONIC, &end);
        pre_time = (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1000000.0;

        rknn_input inputs[1];
        memset(inputs, 0, sizeof(inputs));
        inputs[0].index = 0;
        inputs[0].type = RKNN_TENSOR_UINT8;
        inputs[0].size = g_model_w * g_model_h * 3;
        inputs[0].fmt = RKNN_TENSOR_NHWC;
        inputs[0].pass_through = 0;
        inputs[0].buf = g_ai_ctx.rgb_buf;
        rknn_inputs_set(g_ai_ctx.rknn_ctx, g_ai_ctx.io_num.n_input, inputs);

        clock_gettime(CLOCK_MONOTONIC, &start);
        rknn_run(g_ai_ctx.rknn_ctx, NULL);
        clock_gettime(CLOCK_MONOTONIC, &end);
        run_time = (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1000000.0;

        rknn_output outputs[g_ai_ctx.io_num.n_output];
        memset(outputs, 0, sizeof(outputs));
        for (int i = 0; i < (int)g_ai_ctx.io_num.n_output; i++) {
            outputs[i].want_float = 0;
        }
        rknn_outputs_get(g_ai_ctx.rknn_ctx, g_ai_ctx.io_num.n_output, outputs, NULL);

        object_detect_result_list result_list;
        clock_gettime(CLOCK_MONOTONIC, &start);
        post_process_yolo(&g_ai_ctx, outputs, &result_list, g_conf_thresh, g_nms_thresh);
        clock_gettime(CLOCK_MONOTONIC, &end);
        post_time = (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1000000.0;

        for (int i = 0; i < result_list.count; i++) {
            object_detect_result_t *res = &result_list.results[i];
            LOG_INFO("[AI] cam=%d %s %.2f (%d,%d,%d,%d)\n",
                     g_vpss_chn, res->name, res->prop,
                     res->box.left, res->box.top, res->box.right, res->box.bottom);
        }

        // Draw results to the original frame using RGA
        clock_gettime(CLOCK_MONOTONIC, &start);
        //rk_ai_draw_results(&frame, &result_list);
        clock_gettime(CLOCK_MONOTONIC, &end);
        draw_time = (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1000000.0;

        LOG_INFO("[AI] cost: pre=%.2fms, run=%.2fms, post=%.2fms, draw=%.2fms, total=%.2fms\n",
                 pre_time, run_time, post_time, draw_time, pre_time+run_time+post_time+draw_time);

        rknn_outputs_release(g_ai_ctx.rknn_ctx, g_ai_ctx.io_num.n_output, outputs);
        RK_MPI_VPSS_ReleaseChnFrame(g_vpss_grp, g_vpss_chn, &frame);
    }
    return NULL;
}

int rk_ai_init(void) {
    if (!rk_param_get_int("ai:enable", 0)) {
        return 0;
    }

    g_frame_w = rk_param_get_int("ai:frame_width", 1920);
    g_frame_h = rk_param_get_int("ai:frame_height", 1088);
    g_model_w = rk_param_get_int("ai:input_width", 640);
    g_model_h = rk_param_get_int("ai:input_height", 640);
    g_conf_thresh = rk_param_get_int("ai:conf_threshold", 25) / 100.0f;
    g_nms_thresh = rk_param_get_int("ai:nms_threshold", 45) / 100.0f;
    g_skip_n = rk_param_get_int("ai:skip_frame_num", 5);
    g_vpss_grp = rk_param_get_int("ai:vpss_grp", 1);
    g_vpss_chn = rk_param_get_int("ai:vpss_chn", 1);

    assert(g_frame_w % 16 == 0 && g_frame_h % 16 == 0);
    assert(g_model_w % 16 == 0 && g_model_h % 16 == 0);

    // Disable channel before enable just in case
    RK_MPI_VPSS_DisableChn(g_vpss_grp, g_vpss_chn);

    VPSS_CHN_ATTR_S attr;
    memset(&attr, 0, sizeof(attr));
    attr.enChnMode = VPSS_CHN_MODE_USER;
    attr.enDynamicRange = DYNAMIC_RANGE_SDR8;
    attr.enPixelFormat = RK_FMT_YUV420SP;
    attr.enCompressMode = COMPRESS_MODE_NONE;
    attr.u32Width = g_frame_w;
    attr.u32Height = g_frame_h;
    attr.u32Depth = 1;
    attr.stFrameRate.s32SrcFrameRate = -1;
    attr.stFrameRate.s32DstFrameRate = -1;

    int ret = RK_MPI_VPSS_SetChnAttr(g_vpss_grp, g_vpss_chn, &attr);
    if (ret != 0) {
        LOG_ERROR("RK_MPI_VPSS_SetChnAttr failed! ret=%d\n", ret);
        return -1;
    }

    ret = RK_MPI_VPSS_EnableChn(g_vpss_grp, g_vpss_chn);
    if (ret != 0) {
        LOG_ERROR("RK_MPI_VPSS_EnableChn failed! ret=%d\n", ret);
        return -1;
    }

    const char *model_path = rk_param_get_string("ai:model_path", "/usr/share/yolo26n.rknn");
    int model_size;
    unsigned char* model_data = load_file(model_path, &model_size);
    if (!model_data) {
        LOG_ERROR("Failed to load model %s\n", model_path);
        return -1;
    }

    ret = rknn_init(&g_ai_ctx.rknn_ctx, model_data, model_size, 0, NULL);
    free(model_data);
    if (ret < 0) {
        LOG_ERROR("rknn_init failed! ret=%d\n", ret);
        return -1;
    }

    int core_mask = rk_param_get_int("ai:npu_core_mask", 3);
    rknn_set_core_mask(g_ai_ctx.rknn_ctx, (rknn_core_mask)core_mask);

    rknn_query(g_ai_ctx.rknn_ctx, RKNN_QUERY_IN_OUT_NUM, &g_ai_ctx.io_num, sizeof(g_ai_ctx.io_num));

    g_ai_ctx.output_attrs = malloc(g_ai_ctx.io_num.n_output * sizeof(rknn_tensor_attr));
    for (int i = 0; i < (int)g_ai_ctx.io_num.n_output; i++) {
        g_ai_ctx.output_attrs[i].index = i;
        rknn_query(g_ai_ctx.rknn_ctx, RKNN_QUERY_OUTPUT_ATTR, &(g_ai_ctx.output_attrs[i]), sizeof(rknn_tensor_attr));
    }

    g_ai_ctx.model_input_w = g_model_w;
    g_ai_ctx.model_input_h = g_model_h;

    const char *labels_path = rk_param_get_string("ai:labels_path", "/usr/share/coco_labels.txt");
    load_labels(labels_path, &g_ai_ctx);

    g_ai_ctx.rgb_buf = malloc(g_model_w * g_model_h * 3);

    g_ai_run = 1;
    pthread_create(&g_ai_ctx.thread, NULL, ai_inference_thread, NULL);

    LOG_INFO("rk_ai_init success\n");
    return 0;
}

int rk_ai_deinit(void) {
    if (!g_ai_run) return 0;
    
    g_ai_run = 0;
    pthread_join(g_ai_ctx.thread, NULL);
    
    RK_MPI_VPSS_DisableChn(g_vpss_grp, g_vpss_chn);
    rknn_destroy(g_ai_ctx.rknn_ctx);
    
    if (g_ai_ctx.rgb_buf) free(g_ai_ctx.rgb_buf);
    if (g_ai_ctx.output_attrs) free(g_ai_ctx.output_attrs);
    if (g_ai_ctx.labels) free(g_ai_ctx.labels);
    
    LOG_INFO("rk_ai_deinit end\n");
    return 0;
}
