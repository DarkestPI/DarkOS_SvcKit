#include "iva_manager.h"
#include "iva_inference.h"

#include <cerrno>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

iva::Detection person(float x) {
  iva::Detection detection;
  detection.objectClass = iva::ObjectClass::Person;
  detection.confidence = 0.95F;
  detection.box = {x, 20.0F, 40.0F, 10.0F};
  return detection;
}

bool hasEvent(const std::vector<iva::Event> &events, iva::EventType type) {
  for (const auto &event : events) {
    if (event.type == type)
      return true;
  }
  return false;
}

int fail(const char *message) {
  std::fprintf(stderr, "%s\n", message);
  return 1;
}

} // namespace

int main() {
  // Platform backend 的实现由运行时 HAL 决定：宿主机可能装有 Mock HAL，
  // 也可能没有任何 inference HAL。两种环境都必须给出一致、可诊断的结果。
  for (const iva::InferenceBackend backend :
       {iva::InferenceBackend::Platform, iva::InferenceBackend::Software}) {
    iva::InferenceEngineConfig config;
    config.backend = backend;
    std::string backendError;
    auto engine = iva::createInferenceEngine(config, backendError);
    if (backend == iva::InferenceBackend::Software) {
      if (engine != nullptr || backendError.empty())
        return fail("unavailable software backend did not fail clearly");
    } else {
      iva::ModelInfo model{"host", "1.0", "/not-present/model"};
      const int loadResult =
          engine == nullptr ? -ENODEV : engine->loadModel(model, backendError);
      if (loadResult != 0 && backendError.empty())
        return fail("unavailable platform inference did not fail clearly");
    }
  }

  iva::TrackerConfig trackerConfig;
  trackerConfig.minimumIou = 0.2F;
  auto tracker = iva::createIouTracker(trackerConfig);
  if (!tracker)
    return fail("failed to create IoU tracker");

  std::vector<iva::Track> tracks;
  if (tracker->update({person(20.0F)}, tracks) != 0 || tracks.size() != 1)
    return fail("tracker did not create a track");
  const std::uint64_t targetId = tracks.front().id;
  if (tracker->update({person(24.0F)}, tracks) != 0 || tracks.size() != 1 ||
      tracks.front().id != targetId || !tracks.front().hasPrevious)
    return fail("tracker did not preserve target identity");

  auto manager = iva::Manager::create({trackerConfig});
  if (!manager)
    return fail("failed to create IVA manager");
  auto algorithm = iva::createCallbackAlgorithm(
      [](const iva::FrameView &frame, std::vector<iva::Detection> &output,
         std::string &) {
        output.push_back(person(frame.timestampNs < 20 ? 20.0F : 40.0F));
        return 0;
      });
  if (!algorithm || manager->setAlgorithm(algorithm) != 0)
    return fail("failed to install algorithm");

  std::string error;
  iva::RegionConfig regionConfig;
  regionConfig.id = "front-door-enter";
  regionConfig.polygon = {{45.0F, 0.0F}, {90.0F, 0.0F}, {90.0F, 100.0F},
                          {45.0F, 100.0F}};
  auto enterRule = iva::createRegionEnterRule(regionConfig, error);
  if (!enterRule || manager->addRule(enterRule) != 0)
    return fail("failed to install region rule");

  iva::RegionConfig leaveConfig = regionConfig;
  leaveConfig.id = "front-door-leave";
  auto leaveRule = iva::createRegionLeaveRule(leaveConfig, error);
  if (!leaveRule || manager->addRule(leaveRule) != 0)
    return fail("failed to install region leave rule");

  iva::RegionIntrusionConfig intrusionConfig;
  intrusionConfig.region = regionConfig;
  intrusionConfig.region.id = "front-door-intrusion";
  intrusionConfig.dwellTimeNs = 10;
  auto intrusionRule = iva::createRegionIntrusionRule(intrusionConfig, error);
  if (!intrusionRule || manager->addRule(intrusionRule) != 0)
    return fail("failed to install intrusion rule");

  iva::LineRuleConfig lineConfig;
  lineConfig.id = "front-door-line";
  lineConfig.start = {50.0F, 0.0F};
  lineConfig.end = {50.0F, 100.0F};
  auto lineRule = iva::createLineCrossRule(lineConfig, error);
  if (!lineRule || manager->addRule(lineRule) != 0)
    return fail("failed to install line rule");

  std::vector<iva::Event> events;
  if (manager->process({nullptr, 0, 10, 100, 100, 100,
                        iva::PixelFormat::Gray8},
                       events) != 0 || !events.empty())
    return fail("unexpected event on first frame");
  if (manager->process({nullptr, 0, 20, 100, 100, 100,
                        iva::PixelFormat::Gray8},
                       events) != 0 ||
      !hasEvent(events, iva::EventType::RegionEntered) ||
      !hasEvent(events, iva::EventType::LineCrossed))
    return fail("region enter or line cross was not emitted");
  if (manager->processDetections(35, {person(45.0F)}, events) != 0 ||
      !hasEvent(events, iva::EventType::Intrusion))
    return fail("intrusion event was not emitted after dwell time");

  if (manager->removeRule("front-door-line") != 0 ||
      manager->processDetections(45, {person(20.0F)}, events) != 0 ||
      !hasEvent(events, iva::EventType::RegionLeft))
    return fail("region leave event was not emitted");

  const iva::IvaStats stats = manager->stats();
  if (stats.processedFrames != 4 || stats.detections != 4 ||
      stats.activeTracks != 1 || stats.emittedEvents != events.size() + 3)
    return fail("IVA statistics are inconsistent");

  if (manager->removeRule("missing") != -ENOENT)
    return fail("rule removal contract failed");
  manager->reset();
  if (manager->stats().processedFrames != 0)
    return fail("manager reset did not clear statistics");
  return 0;
}
