#pragma once

#include <base/EventLoop.h>
#include <media_audio_sink.h>
#include <media_video_sink.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace darkos::protocols::rtsp {

struct RtspServerOptions {
  std::string bindAddress{"0.0.0.0"};
  std::uint16_t port{8554};
  std::string mountPath{"live"};
  std::size_t maximumRtpPayloadBytes{1200};
  std::size_t maximumClientBacklogBytes{2 * 1024 * 1024};
  std::string username;
  std::string password;
  std::string authenticationRealm{"DarkOS"};
  std::uint32_t sessionTimeoutSeconds{60};
  std::uint32_t rtcpReportIntervalMs{5000};
  bool enableAudio{true};
  media::AudioCodec audioCodec{media::AudioCodec::G711A};
  std::uint32_t audioSampleRate{16000};
  std::uint32_t audioChannelCount{1};
  bool enableMulticast{false};
  std::string multicastAddress{"239.255.0.1"};
  std::uint16_t multicastVideoPort{5004};
  std::uint16_t multicastAudioPort{5006};
  std::uint8_t multicastTtl{16};
};

struct RtspServerStats {
  std::uint64_t acceptedConnections{0};
  std::uint64_t activeConnections{0};
  std::uint64_t requests{0};
  std::uint64_t videoPackets{0};
  std::uint64_t audioPackets{0};
  std::uint64_t rtpPackets{0};
  std::uint64_t rtcpReports{0};
  std::uint64_t droppedBeforeKeyframe{0};
  std::uint64_t authenticationFailures{0};
  std::uint64_t expiredSessions{0};
};

/**
 * RTSP/1.0 H.264 source server.
 *
 * It is a Media VideoPacketSink, so Application wires it into MediaPipeline.
 * Signalling and RTP I/O run on the supplied EventLoop; consume() may be called
 * from a media worker thread.
 */
class RtspServer final : public media::VideoPacketSink,
                         public media::AudioPacketSink {
public:
  class Impl;

  static std::shared_ptr<RtspServer>
  create(EventLoop &eventLoop, const RtspServerOptions &options,
         std::string &error);
  ~RtspServer() override;

  RtspServer(const RtspServer &) = delete;
  RtspServer &operator=(const RtspServer &) = delete;

  int start() override;
  int stop() override;
  int consume(media::VideoPacketPtr packet) override;
  int consume(media::AudioPacketPtr packet) override;

  std::uint16_t listeningPort() const noexcept;
  RtspServerStats stats() const noexcept;

private:
  explicit RtspServer(std::shared_ptr<Impl> implementation) noexcept;
  std::shared_ptr<Impl> implementation_;
  std::atomic<unsigned> startReferences_{0};
};

} // namespace darkos::protocols::rtsp
