#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include "ai_postprocess.h"
#include "param.h"

#define MAX_DETECT_NUM 1024

// No anchors needed for YOLO26 (YOLOv8 style)


static float calculate_overlap(float xmin0, float ymin0, float xmax0, float ymax0,
                               float xmin1, float ymin1, float xmax1, float ymax1) {
    float w = fmaxf(0.f, fminf(xmax0, xmax1) - fmaxf(xmin0, xmin1) + 1.0f);
    float h = fmaxf(0.f, fminf(ymax0, ymax1) - fmaxf(ymin0, ymin1) + 1.0f);
    float i = w * h;
    float u = (xmax0 - xmin0 + 1.0f) * (ymax0 - ymin0 + 1.0f) +
              (xmax1 - xmin1 + 1.0f) * (ymax1 - ymin1 + 1.0f) - i;
    return u <= 0.f ? 0.f : (i / u);
}

static void quick_sort_indice_inverse(float* input, int left, int right, int* indices) {
    if (left < right) {
        int key_index = indices[left];
        float key = input[left];
        int low = left;
        int high = right;
        while (low < high) {
            while (low < high && input[high] <= key) {
                high--;
            }
            input[low] = input[high];
            indices[low] = indices[high];
            while (low < high && input[low] >= key) {
                low++;
            }
            input[high] = input[low];
            indices[high] = indices[low];
        }
        input[low] = key;
        indices[low] = key_index;
        quick_sort_indice_inverse(input, left, low - 1, indices);
        quick_sort_indice_inverse(input, low + 1, right, indices);
    }
}

inline static int32_t clip_i32(float val, float min, float max) {
    float f = val <= min ? min : (val >= max ? max : val);
    return (int32_t)f;
}

static int8_t qnt_f32_to_affine(float f32, int32_t zp, float scale) {
    float dst_val = (f32 / scale) + zp;
    int8_t res = (int8_t)clip_i32(dst_val, -128, 127);
    return res;
}

static float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale) {
    return ((float)qnt - (float)zp) * scale;
}

static int decode_yolo_output(int8_t *input, int grid_h, int grid_w, int stride,
                               float* boxes, float* objProbs, int* classId, int* validCount, float threshold,
                               int32_t zp, float scale, int num_classes) {
    int grid_len = grid_h * grid_w;
    int8_t thres_i8 = qnt_f32_to_affine(threshold, zp, scale);
    int input_loc_len = 4;

    for (int i = 0; i < grid_h; i++) {
        for (int j = 0; j < grid_w; j++) {
            for (int k = 0; k < num_classes; k++) {
                int8_t score_i8 = input[(input_loc_len + k) * grid_len + i * grid_w + j];
                if (score_i8 >= thres_i8) {
                    if (*validCount >= MAX_DETECT_NUM) return 0;

                    float score_f32 = deqnt_affine_to_f32(score_i8, zp, scale);
                    float loc[4];
                    for (int l = 0; l < 4; l++) {
                        loc[l] = deqnt_affine_to_f32(input[l * grid_len + i * grid_w + j], zp, scale);
                    }

                    float box_x = (j + 0.5f - loc[0]) * stride;
                    float box_y = (i + 0.5f - loc[1]) * stride;
                    float box_w = (loc[0] + loc[2]) * stride;
                    float box_h = (loc[1] + loc[3]) * stride;

                    objProbs[*validCount] = score_f32;
                    classId[*validCount] = k;
                    boxes[(*validCount) * 4 + 0] = box_x;
                    boxes[(*validCount) * 4 + 1] = box_y;
                    boxes[(*validCount) * 4 + 2] = box_w;
                    boxes[(*validCount) * 4 + 3] = box_h;
                    (*validCount)++;
                }
            }
        }
    }
    return 0;
}

int post_process_yolo(rk_ai_context_t *ctx,
                      rknn_output *outputs,
                      object_detect_result_list *result_list,
                      float conf_thresh,
                      float nms_thresh) {
    memset(result_list, 0, sizeof(object_detect_result_list));

    float* filterBoxes = (float*)malloc(MAX_DETECT_NUM * 4 * sizeof(float));
    float* objProbs = (float*)malloc(MAX_DETECT_NUM * sizeof(float));
    int* classId = (int*)malloc(MAX_DETECT_NUM * sizeof(int));
    int* indexArray = (int*)malloc(MAX_DETECT_NUM * sizeof(int));
    
    if(!filterBoxes || !objProbs || !classId || !indexArray) {
        if(filterBoxes) free(filterBoxes);
        if(objProbs) free(objProbs);
        if(classId) free(classId);
        if(indexArray) free(indexArray);
        return -1;
    }

    int validCount = 0;
    int model_in_h = ctx->model_input_h;
    int model_in_w = ctx->model_input_w;
    int num_classes = ctx->num_classes;

    decode_yolo_output((int8_t*)outputs[0].buf, model_in_h / 8, model_in_w / 8, 8,
            filterBoxes, objProbs, classId, &validCount, conf_thresh,
            ctx->output_attrs[0].zp, ctx->output_attrs[0].scale, num_classes);
    decode_yolo_output((int8_t*)outputs[1].buf, model_in_h / 16, model_in_w / 16, 16,
            filterBoxes, objProbs, classId, &validCount, conf_thresh,
            ctx->output_attrs[1].zp, ctx->output_attrs[1].scale, num_classes);
    decode_yolo_output((int8_t*)outputs[2].buf, model_in_h / 32, model_in_w / 32, 32,
            filterBoxes, objProbs, classId, &validCount, conf_thresh,
            ctx->output_attrs[2].zp, ctx->output_attrs[2].scale, num_classes);

    if (validCount <= 0) {
        free(filterBoxes); free(objProbs); free(classId); free(indexArray);
        return 0;
    }

    for (int i = 0; i < validCount; ++i) {
        indexArray[i] = i;
    }

    quick_sort_indice_inverse(objProbs, 0, validCount - 1, indexArray);

    for (int i = 0; i < validCount; ++i) {
        int n = indexArray[i];
        if (n == -1) continue;
        int filterId = classId[n];
        for (int j = i + 1; j < validCount; ++j) {
            int m = indexArray[j];
            if (m == -1 || classId[m] != filterId) continue;
            
            float xmin0 = filterBoxes[n * 4 + 0];
            float ymin0 = filterBoxes[n * 4 + 1];
            float xmax0 = filterBoxes[n * 4 + 0] + filterBoxes[n * 4 + 2];
            float ymax0 = filterBoxes[n * 4 + 1] + filterBoxes[n * 4 + 3];

            float xmin1 = filterBoxes[m * 4 + 0];
            float ymin1 = filterBoxes[m * 4 + 1];
            float xmax1 = filterBoxes[m * 4 + 0] + filterBoxes[m * 4 + 2];
            float ymax1 = filterBoxes[m * 4 + 1] + filterBoxes[m * 4 + 3];

            float iou = calculate_overlap(xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1);
            if (iou > nms_thresh) {
                indexArray[j] = -1;
            }
        }
    }

    int frame_w = rk_param_get_int("ai:frame_width", 1920);
    int frame_h = rk_param_get_int("ai:frame_height", 1088);
    float scale_w = (float)frame_w / model_in_w;
    float scale_h = (float)frame_h / model_in_h;

    int last_count = 0;
    for (int i = 0; i < validCount; ++i) {
        if (indexArray[i] == -1 || last_count >= OBJ_NUMB_MAX_SIZE) continue;
        int n = indexArray[i];

        float x1 = filterBoxes[n * 4 + 0];
        float y1 = filterBoxes[n * 4 + 1];
        float x2 = x1 + filterBoxes[n * 4 + 2];
        float y2 = y1 + filterBoxes[n * 4 + 3];

        int id = classId[n];
        float obj_conf = objProbs[i];

        x1 *= scale_w;
        y1 *= scale_h;
        x2 *= scale_w;
        y2 *= scale_h;

        int left = clip_i32(x1, 0, frame_w);
        int top = clip_i32(y1, 0, frame_h);
        int right = clip_i32(x2, 0, frame_w);
        int bottom = clip_i32(y2, 0, frame_h);

        result_list->results[last_count].box.left = left;
        result_list->results[last_count].box.top = top;
        result_list->results[last_count].box.right = right;
        result_list->results[last_count].box.bottom = bottom;
        result_list->results[last_count].prop = obj_conf;
        result_list->results[last_count].class_id = id;
        if (ctx->labels && id < ctx->num_classes) {
            strncpy(result_list->results[last_count].name, ctx->labels[id], OBJ_NAME_MAX_SIZE - 1);
        } else {
            snprintf(result_list->results[last_count].name, OBJ_NAME_MAX_SIZE, "class_%d", id);
        }
        last_count++;
    }
    result_list->count = last_count;

    free(filterBoxes); free(objProbs); free(classId); free(indexArray);
    return 0;
}
