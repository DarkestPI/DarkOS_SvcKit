#include "IvaExample.h"

#include <iva_inference.h>
#include <iva_manager.h>
#include <svc_log.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace generic_ipc {
namespace {

constexpr char kTag[] = "iva_example";

int decodeExampleOutputs(const std::vector<iva::InferenceTensor> &outputs,
                         const iva::FrameView &frame,
                         std::vector<iva::Detection> &detections,
                         std::string &error) {
    (void)frame;
    (void)error;
    detections.clear();
    SVC_LOGI(kTag,
             "Platform inference returned %zu tensor(s); replace the example "
             "decoder with the model-specific YOLO/NMS decoder",
             outputs.size());
    return 0;
}

} // namespace

bool runIvaExample(const std::string &modelPath) {
    iva::InferenceEngineConfig config;
    config.backend = iva::InferenceBackend::Platform;
    config.decoder = decodeExampleOutputs;

    std::string error;
    auto engine = iva::createInferenceEngine(config, error);
    if (engine == nullptr) {
        SVC_LOGE(kTag, "create IVA Platform engine failed: %s", error.c_str());
        return false;
    }

    const iva::ModelInfo model{"example", "1.0", modelPath};
    if (engine->loadModel(model, error) != 0) {
        SVC_LOGE(kTag, "load IVA model '%s' failed: %s", modelPath.c_str(),
                 error.c_str());
        return false;
    }

    constexpr std::uint32_t width = 640;
    constexpr std::uint32_t height = 640;
    std::vector<std::uint8_t> nv12(
        static_cast<std::size_t>(width) * height * 3u / 2u, 128u);
    iva::FrameView frame;
    frame.data = nv12.data();
    frame.size = nv12.size();
    frame.timestampNs = 1;
    frame.width = width;
    frame.height = height;
    frame.stride = width;
    frame.pixelFormat = iva::PixelFormat::Nv12;

    std::vector<iva::Detection> detections;
    if (engine->infer(frame, detections, error) != 0) {
        SVC_LOGE(kTag, "IVA inference failed: %s", error.c_str());
        return false;
    }

    auto manager = iva::Manager::create();
    if (manager == nullptr) {
        SVC_LOGE(kTag, "create IVA manager failed");
        return false;
    }
    std::vector<iva::Event> events;
    if (manager->processDetections(frame.timestampNs, detections, events) != 0) {
        SVC_LOGE(kTag, "process IVA detections failed");
        return false;
    }
    SVC_LOGI(kTag, "IVA example completed: detections=%zu events=%zu",
             detections.size(), events.size());
    return true;
}

} // namespace generic_ipc
