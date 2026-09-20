#ifndef DARKOS_HARDWARE_INFERENCE_TYPES_H
#define DARKOS_HARDWARE_INFERENCE_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 推理 HAL 的输入格式。
 * 数值使用稳定的 V4L2 fourcc；Gray8 是推理接口额外支持的单通道格式。
 */
typedef enum inference_pixel_format {
    INFERENCE_PIX_FMT_NV12 = 0x3231564e,  /* 'NV12' */
    INFERENCE_PIX_FMT_NV21 = 0x3132564e,  /* 'NV21' */
    INFERENCE_PIX_FMT_YUYV = 0x56595559,  /* 'YUYV' */
    INFERENCE_PIX_FMT_RGB24 = 0x33424752, /* 'RGB3' */
    INFERENCE_PIX_FMT_GRAY8 = 0x20303859, /* 'Y08 ' */
} inference_pixel_format_t;

typedef enum inference_data_type {
    INFERENCE_DATA_UINT8 = 0,
    INFERENCE_DATA_INT8 = 1,
    INFERENCE_DATA_FLOAT32 = 2,
} inference_data_type_t;

/* 通用输入预处理参数；具体平台可选择硬件或 CPU 实现。 */
typedef struct inference_preprocess {
    uint32_t swap_red_blue;
    uint32_t normalize_to_unit;
    float mean[3];
    float standard_deviation[3];
} inference_preprocess_t;

/* 模型由 SvcKit 以路径形式交给平台；模型格式由平台适配器解释。 */
typedef struct inference_model {
    const char *path;
    const inference_preprocess_t *preprocess;
} inference_model_t;

/*
 * 输入帧优先携带 dma-buf fd；fd 无效时使用 data。
 * data/fd 只在一次 run 调用期间借用，平台实现不得异步持有。
 */
typedef struct inference_frame {
    int fd;
    const void *data;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format; /* inference_pixel_format_t */
    uint64_t timestamp_ns;
    void *priv; /* 平台私有帧句柄，接口调用期间有效 */
} inference_frame_t;

#define INFERENCE_MAX_TENSOR_DIMS 8

/*
 * 推理输出张量只读视图。
 * run 成功后由平台填充，release_outputs 之前有效；SvcKit 会在释放前拷贝。
 */
typedef struct inference_tensor {
    const void *data;
    uint32_t size;
    uint32_t data_type; /* inference_data_type_t */
    uint32_t dimension_count;
    uint32_t dimensions[INFERENCE_MAX_TENSOR_DIMS];
    float quant_scale;
    int32_t quant_zero_point;
} inference_tensor_t;

typedef struct inference_output_set {
    inference_tensor_t *tensors;
    uint32_t capacity;
    uint32_t count;
} inference_output_set_t;

typedef struct inference_caps {
    uint32_t input_formats; /* 位图，见 INFERENCE_CAPS_FMT_* */
    uint32_t max_width;
    uint32_t max_height;
    uint32_t max_output_tensors;
} inference_caps_t;

#define INFERENCE_CAPS_FMT_NV12 (1u << 0)
#define INFERENCE_CAPS_FMT_NV21 (1u << 1)
#define INFERENCE_CAPS_FMT_YUYV (1u << 2)
#define INFERENCE_CAPS_FMT_RGB24 (1u << 3)
#define INFERENCE_CAPS_FMT_GRAY8 (1u << 4)

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_INFERENCE_TYPES_H */
