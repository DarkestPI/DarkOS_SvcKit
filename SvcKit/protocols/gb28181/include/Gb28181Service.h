#pragma once

#include <ProtocolService.h>

#include <cstdint>
#include <string>

namespace darkos::protocols::gb28181 {

struct Gb28181ServiceConfig {
  std::string deviceId;
  std::string domain;
  std::string registrarAddress;
  std::uint16_t registrarPort{5060};
  std::uint32_t heartbeatSeconds{60};
};

/** Phase-3 contract: SIP registration/control and PS-over-RTP publishing. */
class Gb28181Service : public ProtocolService {
public:
  ~Gb28181Service() override = default;
  virtual bool registered() const noexcept = 0;

protected:
  Gb28181Service() = default;
};

} // namespace darkos::protocols::gb28181
