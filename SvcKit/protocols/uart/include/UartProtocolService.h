#pragma once

#include <ProtocolService.h>

#include <cstddef>

namespace darkos::protocols::uart {

struct UartProtocolConfig {
  std::size_t maximumLineBytes{256};
};

/** Serial framing/command adapter; SerialPort ownership remains Peripheral. */
class UartProtocolService : public ProtocolService {
public:
  ~UartProtocolService() override = default;

protected:
  UartProtocolService() = default;
};

} // namespace darkos::protocols::uart
