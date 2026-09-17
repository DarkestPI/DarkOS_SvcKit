#pragma once

#include <ProtocolService.h>

#include <cstdint>
#include <string>

namespace darkos::protocols::web {

struct WebServiceConfig {
  std::string bindAddress{"0.0.0.0"};
  std::uint16_t port{8080};
};

class WebService : public ProtocolService {
public:
  ~WebService() override = default;

protected:
  WebService() = default;
};

} // namespace darkos::protocols::web
