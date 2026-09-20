/*
 * Host inference HAL：验证 SvcKit IVA 与 Platform inference SPI 的连接。
 *
 * Ubuntu 没有板端 NPU，这个实现只接受模型、返回空 Tensor 集合，不执行真实
 * 推理。它的作用是让 Ubuntu 案例可以跑通“加载模型 -> 提交帧 -> 输出解码 ->
 * IVA Manager”的生命周期；真实模型推理由 SoC vendor HAL 替换。
 */

#include <inference/IInferenceDevice.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct host_inference_priv {
    int loaded;
} host_inference_priv_t;

static int host_inference_get_capabilities(inference_device_t *device,
                                           inference_caps_t *caps) {
    (void)device;
    if (caps == NULL)
        return -EINVAL;
    memset(caps, 0, sizeof(*caps));
    caps->input_formats = INFERENCE_CAPS_FMT_NV12 |
                          INFERENCE_CAPS_FMT_NV21 |
                          INFERENCE_CAPS_FMT_YUYV |
                          INFERENCE_CAPS_FMT_RGB24 |
                          INFERENCE_CAPS_FMT_GRAY8;
    /* Mock 不产生输出，真实平台在模型加载后填写实际输出上限。 */
    caps->max_output_tensors = 0;
    return 0;
}

static int host_inference_load_model(inference_device_t *device,
                                     const inference_model_t *model) {
    host_inference_priv_t *priv = (host_inference_priv_t *)device->priv;
    if (model == NULL || model->path == NULL || model->path[0] == '\0')
        return -EINVAL;
    priv->loaded = 1;
    return 0;
}

static int host_inference_unload_model(inference_device_t *device) {
    host_inference_priv_t *priv = (host_inference_priv_t *)device->priv;
    priv->loaded = 0;
    return 0;
}

static int host_inference_run(inference_device_t *device,
                              const inference_frame_t *frame,
                              inference_output_set_t *outputs,
                              int timeout_ms) {
    host_inference_priv_t *priv = (host_inference_priv_t *)device->priv;
    (void)timeout_ms;
    if (!priv->loaded)
        return -EPIPE;
    if (frame == NULL || outputs == NULL || outputs->tensors == NULL)
        return -EINVAL;
    outputs->count = 0;
    return 0;
}

static int host_inference_release_outputs(inference_device_t *device,
                                          inference_output_set_t *outputs) {
    (void)device;
    if (outputs == NULL)
        return -EINVAL;
    outputs->count = 0;
    return 0;
}

static int host_inference_close(hw_device_t *common) {
    inference_device_t *device = (inference_device_t *)common;
    free(device->priv);
    free(device);
    return 0;
}

static const inference_device_ops_t host_inference_ops = {
    .get_capabilities = host_inference_get_capabilities,
    .load_model = host_inference_load_model,
    .unload_model = host_inference_unload_model,
    .run = host_inference_run,
    .release_outputs = host_inference_release_outputs,
};

static int host_inference_open(const hw_module_t *module, const char *id,
                               hw_device_t **device) {
    inference_device_t *inferenceDevice;
    host_inference_priv_t *priv;

    if (module == NULL || id == NULL || device == NULL ||
        (strcmp(id, "inference") != 0 && strcmp(id, "inference0") != 0))
        return -EINVAL;
    inferenceDevice = (inference_device_t *)calloc(1, sizeof(*inferenceDevice));
    priv = (host_inference_priv_t *)calloc(1, sizeof(*priv));
    if (inferenceDevice == NULL || priv == NULL) {
        free(inferenceDevice);
        free(priv);
        return -ENOMEM;
    }
    inferenceDevice->common.tag = HARDWARE_DEVICE_TAG;
    inferenceDevice->common.version = INFERENCE_DEVICE_API_VERSION_1_0;
    inferenceDevice->common.module = (hw_module_t *)module;
    inferenceDevice->common.close = host_inference_close;
    inferenceDevice->ops = &host_inference_ops;
    inferenceDevice->priv = priv;
    *device = &inferenceDevice->common;
    return 0;
}

static const hw_module_methods_t host_inference_methods = {
    .open = host_inference_open,
};

struct hw_module_t HMI_inference = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = INFERENCE_MODULE_API_VERSION_1_0,
    .hal_api_version = HARDWARE_API_VERSION_1_0,
    .id = INFERENCE_HARDWARE_MODULE_ID,
    .name = "DarkOS Host Inference HAL (mock)",
    .author = "DarkOS",
    .methods = &host_inference_methods,
};
