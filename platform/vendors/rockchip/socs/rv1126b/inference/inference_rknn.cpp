/**
 * @file inference_rknn.cpp
 * @brief RV1126B inference HAL：RKNN Runtime 适配
 *
 * 这是 Platform 层实现。SvcKit IVA 不会直接包含 rknn_api.h，也不会链接
 * librknnrt.so；它只通过 platform/interfaces/inference 的通用 SPI 访问这里。
 */

#include <inference/IInferenceDevice.h>

#include <rknn_api.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {

struct RgbPixel {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

uint8_t clamp_byte(int value) {
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return (uint8_t)value;
}

RgbPixel yuv_to_rgb(int y, int u, int v) {
    const int c = y - 16;
    const int d = u - 128;
    const int e = v - 128;
    return {clamp_byte((298 * c + 409 * e + 128) >> 8),
            clamp_byte((298 * c - 100 * d - 208 * e + 128) >> 8),
            clamp_byte((298 * c + 516 * d + 128) >> 8)};
}

RgbPixel sample_pixel(const inference_frame_t *frame, uint32_t x, uint32_t y) {
    const size_t width = frame->width;
    const size_t height = frame->height;
    const size_t stride = frame->stride != 0 ? frame->stride : width;
    const uint8_t *data = (const uint8_t *)frame->data;

    if (data == NULL || x >= width || y >= height)
        return {0, 0, 0};

    switch (frame->pixel_format) {
    case INFERENCE_PIX_FMT_RGB24: {
        const size_t offset = (size_t)y * stride + (size_t)x * 3u;
        if (offset + 2u >= frame->size)
            return {0, 0, 0};
        return {data[offset], data[offset + 1u], data[offset + 2u]};
    }
    case INFERENCE_PIX_FMT_GRAY8: {
        const size_t offset = (size_t)y * stride + x;
        if (offset >= frame->size)
            return {0, 0, 0};
        return {data[offset], data[offset], data[offset]};
    }
    case INFERENCE_PIX_FMT_YUYV: {
        const size_t yuyv_stride = frame->stride != 0 ? frame->stride : width * 2u;
        const size_t offset = (size_t)y * yuyv_stride + (size_t)(x / 2u) * 4u;
        if (offset + 3u >= frame->size)
            return {0, 0, 0};
        const int luminance = data[offset + (x % 2u) * 2u];
        return yuv_to_rgb(luminance, data[offset + 1u], data[offset + 3u]);
    }
    case INFERENCE_PIX_FMT_NV12:
    case INFERENCE_PIX_FMT_NV21: {
        const size_t y_offset = (size_t)y * stride + x;
        const size_t uv_offset = stride * height + (size_t)(y / 2u) * stride +
                                 (x & ~1u);
        if (y_offset >= frame->size || uv_offset + 1u >= frame->size)
            return {0, 0, 0};
        const int first = data[uv_offset];
        const int second = data[uv_offset + 1u];
        const int u = frame->pixel_format == INFERENCE_PIX_FMT_NV12 ? first : second;
        const int v = frame->pixel_format == INFERENCE_PIX_FMT_NV12 ? second : first;
        return yuv_to_rgb(data[y_offset], u, v);
    }
    default:
        return {0, 0, 0};
    }
}

size_t tensor_element_size(rknn_tensor_type type) {
    switch (type) {
    case RKNN_TENSOR_UINT8:
    case RKNN_TENSOR_INT8:
        return 1u;
    case RKNN_TENSOR_FLOAT32:
        return sizeof(float);
    default:
        return 0u;
    }
}

class RknnDevice final {
public:
    ~RknnDevice() { unload_model(); }

    int get_capabilities(inference_caps_t *caps) const {
        if (caps == NULL)
            return -EINVAL;
        *caps = {};
        caps->input_formats = INFERENCE_CAPS_FMT_NV12 |
                              INFERENCE_CAPS_FMT_NV21 |
                              INFERENCE_CAPS_FMT_YUYV |
                              INFERENCE_CAPS_FMT_RGB24 |
                              INFERENCE_CAPS_FMT_GRAY8;
        caps->max_output_tensors = (uint32_t)output_attributes_.size();
        if (input_attribute_.fmt == RKNN_TENSOR_NHWC &&
            input_attribute_.n_dims == 4) {
            caps->max_height = input_attribute_.dims[1];
            caps->max_width = input_attribute_.dims[2];
        } else if (input_attribute_.fmt == RKNN_TENSOR_NCHW &&
                   input_attribute_.n_dims == 4) {
            caps->max_height = input_attribute_.dims[2];
            caps->max_width = input_attribute_.dims[3];
        }
        return 0;
    }

    int load_model(const inference_model_t *model) {
        if (model == NULL || model->path == NULL || model->path[0] == '\0')
            return -EINVAL;

        std::ifstream input(model->path, std::ios::binary | std::ios::ate);
        if (!input)
            return -ENOENT;
        const std::streamoff file_size = input.tellg();
        if (file_size <= 0 ||
            (uint64_t)file_size > std::numeric_limits<uint32_t>::max())
            return -EINVAL;

        std::vector<uint8_t> model_bytes((size_t)file_size);
        input.seekg(0);
        if (!input.read((char *)model_bytes.data(), file_size))
            return -EIO;

        int result = unload_model();
        if (result != 0)
            return result;

        rknn_context context = 0;
        result = rknn_init(&context, model_bytes.data(), (uint32_t)model_bytes.size(),
                           0, NULL);
        if (result < 0)
            return -EIO;

        rknn_input_output_num io = {};
        if (rknn_query(context, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)) < 0 ||
            io.n_input != 1 || io.n_output == 0) {
            rknn_destroy(context);
            return -ENOTSUP;
        }

        rknn_tensor_attr input_attribute = {};
        input_attribute.index = 0;
        if (rknn_query(context, RKNN_QUERY_INPUT_ATTR, &input_attribute,
                       sizeof(input_attribute)) < 0) {
            rknn_destroy(context);
            return -EIO;
        }

        std::vector<rknn_tensor_attr> output_attributes(io.n_output);
        for (uint32_t index = 0; index < io.n_output; ++index) {
            output_attributes[index].index = index;
            if (rknn_query(context, RKNN_QUERY_OUTPUT_ATTR,
                           &output_attributes[index],
                           sizeof(output_attributes[index])) < 0) {
                rknn_destroy(context);
                return -EIO;
            }
        }

        context_ = context;
        model_bytes_ = std::move(model_bytes);
        input_attribute_ = input_attribute;
        output_attributes_ = std::move(output_attributes);
        preprocess_ = model->preprocess != NULL
                          ? *model->preprocess
                          : inference_preprocess_t{0u, 0u,
                                                   {0.0f, 0.0f, 0.0f},
                                                   {1.0f, 1.0f, 1.0f}};
        return 0;
    }

    int unload_model() {
        int result = release_outputs(NULL);
        if (context_ != 0 && rknn_destroy(context_) < 0)
            result = result == 0 ? -EIO : result;
        context_ = 0;
        model_bytes_.clear();
        output_attributes_.clear();
        input_attribute_ = {};
        preprocess_ = {0u, 0u, {0.0f, 0.0f, 0.0f},
                       {1.0f, 1.0f, 1.0f}};
        return result;
    }

    int run(const inference_frame_t *frame, inference_output_set_t *outputs) {
        if (context_ == 0)
            return -EPIPE;
        if (frame == NULL || frame->data == NULL || frame->size == 0 ||
            frame->width == 0 || frame->height == 0 || outputs == NULL ||
            outputs->tensors == NULL)
            return -EINVAL;
        if (outputs->capacity < output_attributes_.size())
            return -ENOSPC;

        int result = release_outputs(outputs);
        if (result != 0)
            return result;

        std::vector<uint8_t> input_buffer;
        result = make_input(frame, input_buffer);
        if (result != 0)
            return result;

        rknn_input input = {};
        input.index = 0;
        input.buf = input_buffer.data();
        input.size = (uint32_t)input_buffer.size();
        input.pass_through = 0;
        input.type = input_attribute_.type;
        input.fmt = input_attribute_.fmt;
        if (rknn_inputs_set(context_, 1, &input) < 0)
            return -EIO;
        if (rknn_run(context_, NULL) < 0)
            return -EIO;

        outputs_.assign(output_attributes_.size(), rknn_output{});
        for (size_t index = 0; index < outputs_.size(); ++index) {
            outputs_[index].want_float = 1;
            outputs_[index].is_prealloc = 0;
            outputs_[index].index = (uint32_t)index;
        }
        if (rknn_outputs_get(context_, (uint32_t)outputs_.size(),
                             outputs_.data(), NULL) < 0) {
            outputs_.clear();
            return -EIO;
        }

        for (size_t index = 0; index < outputs_.size(); ++index) {
            inference_tensor_t &tensor = outputs->tensors[index];
            tensor = {};
            tensor.data = outputs_[index].buf;
            tensor.size = outputs_[index].size;
            tensor.data_type = INFERENCE_DATA_FLOAT32;
            tensor.dimension_count = std::min<uint32_t>(
                output_attributes_[index].n_dims, INFERENCE_MAX_TENSOR_DIMS);
            for (uint32_t dimension = 0; dimension < tensor.dimension_count;
                 ++dimension)
                tensor.dimensions[dimension] =
                    output_attributes_[index].dims[dimension];
        }
        outputs->count = (uint32_t)outputs_.size();
        outputs_held_ = true;
        return 0;
    }

    int release_outputs(inference_output_set_t *outputs) {
        int result = 0;
        if (outputs_held_) {
            if (context_ != 0 &&
                rknn_outputs_release(context_, (uint32_t)outputs_.size(),
                                     outputs_.data()) < 0)
                result = -EIO;
            outputs_.clear();
            outputs_held_ = false;
        }
        if (outputs != NULL) {
            outputs->count = 0;
            if (outputs->tensors != NULL) {
                for (uint32_t index = 0; index < outputs->capacity; ++index)
                    outputs->tensors[index] = {};
            }
        }
        return result;
    }

private:
    int make_input(const inference_frame_t *frame,
                   std::vector<uint8_t> &output) const {
        if (input_attribute_.n_dims != 4 ||
            (input_attribute_.fmt != RKNN_TENSOR_NHWC &&
             input_attribute_.fmt != RKNN_TENSOR_NCHW))
            return -ENOTSUP;

        uint32_t width;
        uint32_t height;
        uint32_t channels;
        if (input_attribute_.fmt == RKNN_TENSOR_NHWC) {
            height = input_attribute_.dims[1];
            width = input_attribute_.dims[2];
            channels = input_attribute_.dims[3];
        } else {
            channels = input_attribute_.dims[1];
            height = input_attribute_.dims[2];
            width = input_attribute_.dims[3];
        }
        if (width == 0 || height == 0 || (channels != 1 && channels != 3))
            return -EINVAL;

        const size_t element_size = tensor_element_size(input_attribute_.type);
        if (element_size == 0)
            return -ENOTSUP;
        const size_t element_count = (size_t)width * height * channels;
        if (element_count > std::numeric_limits<size_t>::max() / element_size)
            return -EOVERFLOW;
        output.resize(element_count * element_size);

        for (uint32_t y = 0; y < height; ++y) {
            const uint32_t source_y = (uint32_t)(((uint64_t)y * frame->height) /
                                                 height);
            for (uint32_t x = 0; x < width; ++x) {
                const uint32_t source_x = (uint32_t)(((uint64_t)x * frame->width) /
                                                     width);
                const RgbPixel pixel = sample_pixel(frame, source_x, source_y);
                const uint8_t channel_values[3] = {
                    preprocess_.swap_red_blue ? pixel.b : pixel.r,
                    pixel.g,
                    preprocess_.swap_red_blue ? pixel.r : pixel.b};
                for (uint32_t channel = 0; channel < channels; ++channel) {
                    if (preprocess_.standard_deviation[channel] == 0.0f)
                        return -EINVAL;
                    float value = (float)(channels == 1 ? pixel.r
                                                         : channel_values[channel]);
                    if (preprocess_.normalize_to_unit)
                        value /= 255.0f;
                    value = (value - preprocess_.mean[channel]) /
                            preprocess_.standard_deviation[channel];
                    const size_t linear_index =
                        input_attribute_.fmt == RKNN_TENSOR_NHWC
                            ? ((size_t)y * width + x) * channels + channel
                            : ((size_t)channel * height + y) * width + x;
                    uint8_t *destination = output.data() + linear_index * element_size;
                    if (input_attribute_.type == RKNN_TENSOR_FLOAT32) {
                        std::memcpy(destination, &value, sizeof(value));
                    } else if (input_attribute_.type == RKNN_TENSOR_UINT8) {
                        destination[0] = clamp_byte((int)std::lround(value));
                    } else {
                        float quantized = value;
                        if (input_attribute_.qnt_type ==
                                RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC &&
                            input_attribute_.scale != 0.0f)
                            quantized = value / input_attribute_.scale +
                                        input_attribute_.zp;
                        const int rounded = (int)std::lround(quantized);
                        destination[0] = (uint8_t)(int8_t)
                            std::max(-128, std::min(127, rounded));
                    }
                }
            }
        }
        return 0;
    }

    rknn_context context_{0};
    std::vector<uint8_t> model_bytes_;
    rknn_tensor_attr input_attribute_{};
    std::vector<rknn_tensor_attr> output_attributes_;
    inference_preprocess_t preprocess_{0u, 0u, {0.0f, 0.0f, 0.0f},
                                       {1.0f, 1.0f, 1.0f}};
    std::vector<rknn_output> outputs_;
    bool outputs_held_{false};
};

int rk_inference_get_capabilities(inference_device_t *device,
                                  inference_caps_t *caps) {
    return static_cast<RknnDevice *>(device->priv)->get_capabilities(caps);
}

int rk_inference_load_model(inference_device_t *device,
                            const inference_model_t *model) {
    return static_cast<RknnDevice *>(device->priv)->load_model(model);
}

int rk_inference_unload_model(inference_device_t *device) {
    return static_cast<RknnDevice *>(device->priv)->unload_model();
}

int rk_inference_run(inference_device_t *device, const inference_frame_t *frame,
                     inference_output_set_t *outputs, int /*timeout_ms*/) {
    return static_cast<RknnDevice *>(device->priv)->run(frame, outputs);
}

int rk_inference_release_outputs(inference_device_t *device,
                                 inference_output_set_t *outputs) {
    return static_cast<RknnDevice *>(device->priv)->release_outputs(outputs);
}

int rk_inference_close(hw_device_t *common) {
    auto *device = reinterpret_cast<inference_device_t *>(common);
    delete static_cast<RknnDevice *>(device->priv);
    delete device;
    return 0;
}

const inference_device_ops_t rk_inference_ops = {
    rk_inference_get_capabilities,
    rk_inference_load_model,
    rk_inference_unload_model,
    rk_inference_run,
    rk_inference_release_outputs,
};

int rk_inference_open(const hw_module_t *module, const char *id,
                      hw_device_t **device) {
    if (module == NULL || id == NULL || device == NULL ||
        (std::strcmp(id, "inference") != 0 &&
         std::strcmp(id, "inference0") != 0))
        return -EINVAL;

    auto *inferenceDevice = new (std::nothrow) inference_device_t{};
    if (inferenceDevice == NULL)
        return -ENOMEM;
    auto *privateDevice = new (std::nothrow) RknnDevice();
    if (privateDevice == NULL) {
        delete inferenceDevice;
        return -ENOMEM;
    }
    inferenceDevice->common.tag = HARDWARE_DEVICE_TAG;
    inferenceDevice->common.version = INFERENCE_DEVICE_API_VERSION_1_0;
    inferenceDevice->common.module = const_cast<hw_module_t *>(module);
    inferenceDevice->common.close = rk_inference_close;
    inferenceDevice->ops = &rk_inference_ops;
    inferenceDevice->priv = privateDevice;
    *device = &inferenceDevice->common;
    return 0;
}

const hw_module_methods_t rk_inference_methods = {
    rk_inference_open,
};

} // namespace

extern "C" {
hw_module_t HMI_inference = {
    HARDWARE_MODULE_TAG,
    INFERENCE_MODULE_API_VERSION_1_0,
    HARDWARE_API_VERSION_1_0,
    INFERENCE_HARDWARE_MODULE_ID,
    "Rockchip RV1126B Inference HAL (RKNN)",
    "DarkOS",
    &rk_inference_methods,
    NULL,
    {0, 0, 0, 0, 0, 0, 0, 0},
};
}
