#ifndef DARKOS_HARDWARE_INFERENCE_IINFERENCEDEVICE_H
#define DARKOS_HARDWARE_INFERENCE_IINFERENCEDEVICE_H

#include <hardware/hardware.h>
#include <inference/types.h>

#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 推理设备 SPI。
 *
 * SvcKit 只依赖这里的通用模型、帧和 Tensor 契约；Rockchip、HiSilicon、
 * Novatek 等厂商 SDK 只能出现在各自 platform/vendors 实现中。
 */
#define INFERENCE_HARDWARE_MODULE_ID "inference"
#define INFERENCE_MODULE_API_VERSION_1_0 HARDWARE_MAKE_API_VERSION(1, 0)
#define INFERENCE_DEVICE_API_VERSION_1_0 HARDWARE_MAKE_API_VERSION(1, 0)

typedef struct inference_device inference_device_t;

typedef struct inference_device_ops {
    int (*get_capabilities)(inference_device_t *device,
                            inference_caps_t *caps);
    int (*load_model)(inference_device_t *device,
                      const inference_model_t *model);
    int (*unload_model)(inference_device_t *device);
    int (*run)(inference_device_t *device, const inference_frame_t *frame,
               inference_output_set_t *outputs, int timeout_ms);
    int (*release_outputs)(inference_device_t *device,
                           inference_output_set_t *outputs);
} inference_device_ops_t;

struct inference_device {
    hw_device_t common;
    const inference_device_ops_t *ops;
    void *priv;
};

static inline int inference_open_by_id(const hw_module_t *module, const char *id,
                                       inference_device_t **device) {
    hw_device_t *hwdev = NULL;
    inference_device_t *inferenceDevice;
    int result;

    if (module == NULL || module->methods == NULL || module->methods->open == NULL ||
        id == NULL || device == NULL ||
        !hw_module_supports(module, INFERENCE_MODULE_API_VERSION_1_0))
        return -EINVAL;
    result = module->methods->open(module, id, &hwdev);
    if (result != 0)
        return result;
    inferenceDevice = (inference_device_t *)hwdev;
    if (inferenceDevice == NULL || inferenceDevice->common.close == NULL ||
        inferenceDevice->ops == NULL ||
        !hw_device_supports(&inferenceDevice->common,
                            INFERENCE_DEVICE_API_VERSION_1_0)) {
        if (inferenceDevice != NULL && inferenceDevice->common.close != NULL)
            inferenceDevice->common.close(&inferenceDevice->common);
        return -EPROTONOSUPPORT;
    }
    *device = inferenceDevice;
    return 0;
}

static inline int inference_open(const hw_module_t *module,
                                 inference_device_t **device) {
    return inference_open_by_id(module, INFERENCE_HARDWARE_MODULE_ID, device);
}

static inline int inference_close(inference_device_t *device) {
    return device == NULL || device->common.close == NULL
               ? -EINVAL
               : device->common.close(&device->common);
}

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_INFERENCE_IINFERENCEDEVICE_H */
