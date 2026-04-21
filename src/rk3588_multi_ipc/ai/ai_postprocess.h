#ifndef AI_POSTPROCESS_H
#define AI_POSTPROCESS_H

#include <rknn_api.h>

#define OBJ_NAME_MAX_SIZE  64
#define OBJ_NUMB_MAX_SIZE  128

typedef struct {
    int left, top, right, bottom;
} object_detect_box_t;

typedef struct {
    char name[OBJ_NAME_MAX_SIZE];
    float prop;
    int class_id;
    object_detect_box_t box;
} object_detect_result_t;

typedef struct {
    int count;
    object_detect_result_t results[OBJ_NUMB_MAX_SIZE];
} object_detect_result_list;

/* AI 推理上下文（供 ai.c 定义实例，后处理访问） */
typedef struct {
    rknn_context rknn_ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr *output_attrs;
    int model_input_w;
    int model_input_h;
    int num_classes;
    char (*labels)[OBJ_NAME_MAX_SIZE];
    unsigned char *rgb_buf;
    pthread_t thread;
} rk_ai_context_t;

int post_process_yolo(rk_ai_context_t *ctx,
                      rknn_output *outputs,
                      object_detect_result_list *result_list,
                      float conf_thresh,
                      float nms_thresh);

#endif /* AI_POSTPROCESS_H */
