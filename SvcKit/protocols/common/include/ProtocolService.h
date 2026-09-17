#pragma once

namespace darkos::protocols {

class ProtocolService {
public:
  virtual ~ProtocolService() = default;
  virtual int start() = 0;
  virtual int stop() = 0;

protected:
  ProtocolService() = default;
};

} // namespace darkos::protocols
