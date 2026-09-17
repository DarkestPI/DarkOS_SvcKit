#pragma once

#include <ProtocolService.h>

#include <cstdint>
#include <string>

namespace darkos::protocols::onvif {

struct OnvifServiceConfig {
  std::string bindAddress{"0.0.0.0"};
  std::uint16_t httpPort{80};
  std::string deviceUrn;
};

/** Phase-2 contract: WS-Discovery plus Device/Media/PTZ/Events adapters. */
class OnvifService : public ProtocolService {
public:
  ~OnvifService() override = default;
  virtual std::string deviceServiceUrl() const = 0;

protected:
  OnvifService() = default;
};

} // namespace darkos::protocols::onvif
