#pragma once

#include <ProtocolService.h>

#include <string>

namespace darkos::protocols::rtmp {

struct RtmpPublisherConfig { std::string publishUrl; };

class RtmpPublisher : public ProtocolService {
public:
  ~RtmpPublisher() override = default;

protected:
  RtmpPublisher() = default;
};

} // namespace darkos::protocols::rtmp
