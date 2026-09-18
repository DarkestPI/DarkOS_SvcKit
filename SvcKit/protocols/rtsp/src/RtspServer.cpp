#include <RtspServer.h>

#include <H264RtpPacketizer.h>
#include <base/Hash.h>
#include <base/TimeUtil.h>
#include <network_connection.h>

#include <sys/epoll.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <new>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace darkos::protocols::rtsp {
namespace {

constexpr std::size_t kMaximumRequestBytes = 64 * 1024;
constexpr std::uint8_t kVideoPayloadType = 96;
constexpr std::uint8_t kAudioPayloadType = 97;
constexpr std::uint64_t kMaintenanceIntervalNs = 250'000'000ULL;
constexpr std::uint64_t kNtpUnixEpochOffset = 2'208'988'800ULL;
constexpr char kPublicMethods[] =
    "OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER, "
    "SET_PARAMETER";

enum class TrackKind : std::size_t { Video = 0, Audio = 1 };

std::string trim(const std::string &value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos)
    return {};
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string pathFromUrl(const std::string &url) {
  std::string value = url;
  const auto scheme = value.find("://");
  if (scheme != std::string::npos) {
    const auto slash = value.find('/', scheme + 3);
    value = slash == std::string::npos ? std::string{} : value.substr(slash);
  }
  const auto query = value.find('?');
  if (query != std::string::npos)
    value.resize(query);
  while (!value.empty() && value.front() == '/')
    value.erase(value.begin());
  while (!value.empty() && value.back() == '/')
    value.pop_back();
  return value;
}

std::string baseUrl(std::string url) {
  const auto query = url.find('?');
  if (query != std::string::npos)
    url.resize(query);
  while (!url.empty() && url.back() == '/')
    url.pop_back();
  for (const char *suffix : {"/trackID=0", "/trackID=1", "/track0", "/track1"}) {
    const std::string value(suffix);
    if (url.size() >= value.size() &&
        url.compare(url.size() - value.size(), value.size(), value) == 0) {
      url.resize(url.size() - value.size());
      break;
    }
  }
  return url;
}

std::string randomHex() {
  static std::atomic<std::uint64_t> sequence{1};
  std::random_device random;
  const std::uint64_t value =
      (static_cast<std::uint64_t>(random()) << 32U) ^ random() ^ sequence++;
  std::ostringstream output;
  output << std::hex << value;
  return output.str();
}

bool parsePair(const std::string &value, const std::string &key, int &first,
               int &second) {
  const auto position = value.find(key);
  if (position == std::string::npos)
    return false;
  const char *begin = value.c_str() + position + key.size();
  char *end = nullptr;
  const long left = std::strtol(begin, &end, 10);
  if (end == begin || left < 0 || left > 65535)
    return false;
  long right = left + 1;
  if (*end == '-') {
    char *rightEnd = nullptr;
    right = std::strtol(end + 1, &rightEnd, 10);
    if (rightEnd == end + 1 || right < 0 || right > 65535)
      return false;
  }
  first = static_cast<int>(left);
  second = static_cast<int>(right);
  return true;
}

struct Request {
  std::string method;
  std::string url;
  std::string version;
  std::map<std::string, std::string> headers;
  std::string body;

  std::string header(const std::string &name) const {
    const auto entry = headers.find(lower(name));
    return entry == headers.end() ? std::string{} : entry->second;
  }
};

struct Transport {
  bool tcp{false};
  bool multicast{false};
  bool interleavedSpecified{false};
  int rtpChannel{0};
  int rtcpChannel{1};
  std::uint16_t clientRtpPort{0};
  std::uint16_t clientRtcpPort{0};
};

bool parseTransport(const std::string &header, Transport &output) {
  const std::string value = lower(header);
  if (value.find("rtp/avp/tcp") != std::string::npos) {
    output.tcp = true;
    output.interleavedSpecified = value.find("interleaved=") != std::string::npos;
    int first = 0;
    int second = 1;
    if (output.interleavedSpecified &&
        !parsePair(value, "interleaved=", first, second))
      return false;
    output.rtpChannel = first;
    output.rtcpChannel = second;
    return first >= 0 && first <= 255 && second >= 0 && second <= 255;
  }
  if (value.find("rtp/avp") == std::string::npos)
    return false;
  output.multicast = value.find("multicast") != std::string::npos;
  if (output.multicast)
    return true;
  int first = 0;
  int second = 0;
  if (!parsePair(value, "client_port=", first, second) || first == 0)
    return false;
  output.clientRtpPort = static_cast<std::uint16_t>(first);
  output.clientRtcpPort = static_cast<std::uint16_t>(second);
  return true;
}

std::map<std::string, std::string> parseDigestFields(const std::string &header) {
  std::map<std::string, std::string> fields;
  if (header.size() < 7 || lower(header.substr(0, 7)) != "digest ")
    return fields;
  std::size_t offset = 7;
  while (offset < header.size()) {
    while (offset < header.size() &&
           (header[offset] == ',' ||
            std::isspace(static_cast<unsigned char>(header[offset]))))
      ++offset;
    const auto equals = header.find('=', offset);
    if (equals == std::string::npos)
      break;
    const std::string key = lower(trim(header.substr(offset, equals - offset)));
    offset = equals + 1;
    std::string value;
    if (offset < header.size() && header[offset] == '"') {
      ++offset;
      while (offset < header.size() && header[offset] != '"') {
        if (header[offset] == '\\' && offset + 1 < header.size())
          ++offset;
        value.push_back(header[offset++]);
      }
      if (offset < header.size())
        ++offset;
    } else {
      const auto comma = header.find(',', offset);
      value = trim(header.substr(offset, comma - offset));
      offset = comma == std::string::npos ? header.size() : comma + 1;
    }
    if (!key.empty())
      fields[key] = value;
  }
  return fields;
}

bool constantTimeEqual(const std::string &left, const std::string &right) {
  if (left.size() != right.size())
    return false;
  unsigned difference = 0;
  for (std::size_t index = 0; index < left.size(); ++index)
    difference |= static_cast<unsigned>(left[index] ^ right[index]);
  return difference == 0;
}

std::uint32_t mediaTimestamp(std::uint64_t timestampNs,
                             std::uint32_t clockRate) {
  const std::uint64_t seconds = timestampNs / 1'000'000'000ULL;
  const std::uint64_t remainder = timestampNs % 1'000'000'000ULL;
  return static_cast<std::uint32_t>(seconds * clockRate +
                                    remainder * clockRate / 1'000'000'000ULL);
}

bool isIpv4MulticastAddress(const std::string &address) {
  const auto dot = address.find('.');
  if (dot == std::string::npos)
    return false;
  char *end = nullptr;
  const long first = std::strtol(address.substr(0, dot).c_str(), &end, 10);
  return end != nullptr && *end == '\0' && first >= 224 && first <= 239;
}

void put16(std::uint8_t *output, std::uint16_t value) {
  output[0] = static_cast<std::uint8_t>(value >> 8U);
  output[1] = static_cast<std::uint8_t>(value);
}

void put32(std::uint8_t *output, std::uint32_t value) {
  output[0] = static_cast<std::uint8_t>(value >> 24U);
  output[1] = static_cast<std::uint8_t>(value >> 16U);
  output[2] = static_cast<std::uint8_t>(value >> 8U);
  output[3] = static_cast<std::uint8_t>(value);
}

struct TrackState {
  bool configured{false};
  bool tcp{true};
  bool multicast{false};
  bool firstPacket{true};
  int rtpChannel{0};
  int rtcpChannel{1};
  network::Socket udpRtp;
  network::Socket udpRtcp;
  network::Address rtpTarget;
  network::Address rtcpTarget;
  std::uint16_t sequence{0};
  std::uint32_t ssrc{0};
  std::uint32_t lastTimestamp{1};
  bool timestampInitialized{false};
  std::uint64_t packetCount{0};
  std::uint64_t octetCount{0};
  std::uint64_t lastReportNs{0};
};

} // namespace

class Connection final {
public:
  Connection(std::weak_ptr<RtspServer::Impl> server, std::uint64_t id,
             network::Socket socket, network::Address peer,
             std::size_t maximumBacklog);
  ~Connection() { close(); }

  void start();
  void close();
  void maintain(std::uint64_t nowNs);
  void sendVideo(const media::VideoPacket &packet,
                 const common::H264RtpPacketizer &packetizer);
  void sendAudio(const media::AudioPacket &packet);

private:
  void rearm();
  void onReady(std::uint32_t events);
  void parseInput();
  void handle(const Request &request);
  bool authorized(const Request &request);
  void challenge(const Request &request);
  bool sessionMatches(const Request &request) const;
  void respond(const Request &request, int status, const std::string &reason,
               const std::string &headers = {}, const std::string &body = {});
  bool setupTrack(const Request &request, TrackKind kind,
                  const Transport &transport);
  void resetSession();
  void resetTrack(TrackState &track);
  bool hasConfiguredTrack() const;
  bool hasMulticastTrack() const;
  bool queue(const void *data, std::size_t size);
  void flush();
  void disconnect();
  bool sendRtp(TrackState &track, std::uint8_t payloadType,
               const std::uint8_t *payload, std::size_t size, bool marker,
               std::uint32_t timestamp);
  bool sendControl(TrackState &track, const std::uint8_t *packet,
                   std::size_t size);
  void sendSenderReport(TrackState &track, std::uint64_t nowNs);

  std::weak_ptr<RtspServer::Impl> server_;
  std::uint64_t id_{0};
  network::Socket socket_;
  network::Address peer_;
  std::string input_;
  std::string output_;
  std::string session_;
  std::string presentationUrl_;
  std::string nonce_;
  std::string lastClientNonce_;
  std::size_t maximumBacklog_{0};
  std::uint32_t lastNonceCount_{0};
  TrackState video_;
  TrackState audio_;
  std::uint64_t lastActivityNs_{0};
  bool playing_{false};
  bool awaitingKeyframe_{true};
  bool ownsMulticast_{false};
  bool closed_{false};
};

class RtspServer::Impl final : public std::enable_shared_from_this<Impl> {
public:
  Impl(EventLoop &loop, RtspServerOptions options)
      : loop_(loop), options_(std::move(options)),
        packetizer_(options_.maximumRtpPayloadBytes) {}

  int start(std::string &error) {
    if (running_.exchange(true))
      return -EALREADY;
    network::Address address;
    int result = network::Address::fromIp(options_.bindAddress, options_.port,
                                          address);
    if (result != 0) {
      running_ = false;
      error = "invalid RTSP bind address";
      return result;
    }
    server_ = network::TcpServer::create(loop_, address, error);
    if (!server_) {
      running_ = false;
      return -EADDRNOTAVAIL;
    }
    const std::weak_ptr<Impl> weak = shared_from_this();
    result = server_->start(
        [weak](network::Socket socket, const network::Address &peer) mutable {
          if (auto self = weak.lock())
            self->accept(std::move(socket), peer);
        });
    if (result != 0) {
      server_.reset();
      running_ = false;
      error = "start RTSP acceptor failed";
      return result;
    }
    maintenanceTimer_ = loop_.scheduleEvery(
        kMaintenanceIntervalNs, kMaintenanceIntervalNs, [weak] {
          if (auto self = weak.lock())
            self->maintain();
        });
    if (maintenanceTimer_ == 0) {
      server_->stop();
      server_.reset();
      running_ = false;
      error = "start RTSP maintenance timer failed";
      return -EAGAIN;
    }
    port_ = server_->localAddress().port();
    error.clear();
    return 0;
  }

  int stop() {
    if (!running_.exchange(false))
      return 0;
    if (maintenanceTimer_ != 0) {
      loop_.cancel(maintenanceTimer_);
      maintenanceTimer_ = 0;
    }
    if (server_)
      server_->stop();
    for (auto &entry : connections_)
      entry.second->close();
    connections_.clear();
    multicastOwner_ = 0;
    server_.reset();
    activeConnections_ = 0;
    return 0;
  }

  int consume(media::VideoPacketPtr packet) {
    if (!packet || !packet->buffer)
      return -EINVAL;
    if (packet->codec != media::VideoCodec::H264)
      return -ENOTSUP;
    if (!running_)
      return -ESHUTDOWN;
    const std::weak_ptr<Impl> weak = shared_from_this();
    return loop_.post([weak, packet = std::move(packet)] {
             if (auto self = weak.lock())
               self->distributeVideo(*packet);
           })
               ? 0
               : -ESHUTDOWN;
  }

  int consume(media::AudioPacketPtr packet) {
    if (!packet || !packet->buffer)
      return -EINVAL;
    if (!options_.enableAudio || packet->codec != options_.audioCodec ||
        packet->sampleRate != options_.audioSampleRate ||
        packet->channelCount != options_.audioChannelCount)
      return -ENOTSUP;
    if (packet->buffer->size() > 65523)
      return -EMSGSIZE;
    if (!running_)
      return -ESHUTDOWN;
    const std::weak_ptr<Impl> weak = shared_from_this();
    return loop_.post([weak, packet = std::move(packet)] {
             if (auto self = weak.lock())
               self->distributeAudio(*packet);
           })
               ? 0
               : -ESHUTDOWN;
  }

  void remove(std::uint64_t id) {
    const std::weak_ptr<Impl> weak = shared_from_this();
    loop_.post([weak, id] {
      if (auto self = weak.lock()) {
        self->connections_.erase(id);
        self->activeConnections_ = self->connections_.size();
      }
    });
  }

  bool acquireMulticast(std::uint64_t id) {
    if (multicastOwner_ != 0 && multicastOwner_ != id)
      return false;
    multicastOwner_ = id;
    return true;
  }

  void releaseMulticast(std::uint64_t id) {
    if (multicastOwner_ == id)
      multicastOwner_ = 0;
  }

  EventLoop &loop() noexcept { return loop_; }
  const RtspServerOptions &options() const noexcept { return options_; }
  const std::string &mountPath() const noexcept { return options_.mountPath; }
  void countRequest() noexcept { ++requests_; }
  void countRtp() noexcept { ++rtpPackets_; }
  void countRtcp() noexcept { ++rtcpReports_; }
  void countKeyframeDrop() noexcept { ++droppedBeforeKeyframe_; }
  void countAuthFailure() noexcept { ++authenticationFailures_; }
  void countExpiredSession() noexcept { ++expiredSessions_; }
  std::uint16_t port() const noexcept { return port_; }

  RtspServerStats stats() const noexcept {
    RtspServerStats result;
    result.acceptedConnections = acceptedConnections_.load();
    result.activeConnections = activeConnections_.load();
    result.requests = requests_.load();
    result.videoPackets = videoPackets_.load();
    result.audioPackets = audioPackets_.load();
    result.rtpPackets = rtpPackets_.load();
    result.rtcpReports = rtcpReports_.load();
    result.droppedBeforeKeyframe = droppedBeforeKeyframe_.load();
    result.authenticationFailures = authenticationFailures_.load();
    result.expiredSessions = expiredSessions_.load();
    return result;
  }

private:
  friend class Connection;

  void accept(network::Socket socket, const network::Address &peer) {
    if (!running_)
      return;
    const std::uint64_t id = nextConnectionId_++;
    auto connection = std::make_unique<Connection>(
        weak_from_this(), id, std::move(socket), peer,
        options_.maximumClientBacklogBytes);
    Connection *raw = connection.get();
    connections_.emplace(id, std::move(connection));
    ++acceptedConnections_;
    activeConnections_ = connections_.size();
    raw->start();
  }

  void maintain() {
    if (!running_)
      return;
    const std::uint64_t now = monoNowNs();
    for (auto &entry : connections_)
      entry.second->maintain(now);
  }

  void distributeVideo(const media::VideoPacket &packet) {
    if (!running_)
      return;
    ++videoPackets_;
    for (auto &entry : connections_)
      entry.second->sendVideo(packet, packetizer_);
  }

  void distributeAudio(const media::AudioPacket &packet) {
    if (!running_)
      return;
    ++audioPackets_;
    for (auto &entry : connections_)
      entry.second->sendAudio(packet);
  }

  EventLoop &loop_;
  RtspServerOptions options_;
  common::H264RtpPacketizer packetizer_;
  std::unique_ptr<network::TcpServer> server_;
  std::unordered_map<std::uint64_t, std::unique_ptr<Connection>> connections_;
  std::atomic<bool> running_{false};
  EventLoop::TimerId maintenanceTimer_{0};
  std::uint64_t nextConnectionId_{1};
  std::uint64_t multicastOwner_{0};
  std::uint16_t port_{0};
  std::atomic<std::uint64_t> acceptedConnections_{0};
  std::atomic<std::uint64_t> activeConnections_{0};
  std::atomic<std::uint64_t> requests_{0};
  std::atomic<std::uint64_t> videoPackets_{0};
  std::atomic<std::uint64_t> audioPackets_{0};
  std::atomic<std::uint64_t> rtpPackets_{0};
  std::atomic<std::uint64_t> rtcpReports_{0};
  std::atomic<std::uint64_t> droppedBeforeKeyframe_{0};
  std::atomic<std::uint64_t> authenticationFailures_{0};
  std::atomic<std::uint64_t> expiredSessions_{0};
};

Connection::Connection(std::weak_ptr<RtspServer::Impl> server,
                       std::uint64_t id, network::Socket socket,
                       network::Address peer, std::size_t maximumBacklog)
    : server_(std::move(server)), id_(id), socket_(std::move(socket)),
      peer_(peer), nonce_(randomHex()), maximumBacklog_(maximumBacklog),
      lastActivityNs_(monoNowNs()) {
  std::random_device random;
  video_.sequence = static_cast<std::uint16_t>(random());
  video_.ssrc = (static_cast<std::uint32_t>(random()) << 16U) ^ random();
  audio_.sequence = static_cast<std::uint16_t>(random());
  audio_.ssrc = (static_cast<std::uint32_t>(random()) << 16U) ^ random();
  audio_.rtpChannel = 2;
  audio_.rtcpChannel = 3;
}

void Connection::start() { rearm(); }

void Connection::resetTrack(TrackState &track) {
  track.udpRtp.close();
  track.udpRtcp.close();
  track.configured = false;
  track.packetCount = 0;
  track.octetCount = 0;
  track.lastReportNs = 0;
  track.firstPacket = true;
}

bool Connection::hasConfiguredTrack() const {
  return video_.configured || audio_.configured;
}

bool Connection::hasMulticastTrack() const {
  return (video_.configured && video_.multicast) ||
         (audio_.configured && audio_.multicast);
}

void Connection::resetSession() {
  resetTrack(video_);
  resetTrack(audio_);
  playing_ = false;
  awaitingKeyframe_ = true;
  session_.clear();
  presentationUrl_.clear();
  if (ownsMulticast_) {
    if (auto server = server_.lock())
      server->releaseMulticast(id_);
    ownsMulticast_ = false;
  }
}

void Connection::close() {
  if (closed_)
    return;
  closed_ = true;
  resetSession();
  if (auto server = server_.lock())
    server->loop().unwatchFd(socket_.nativeHandle());
  socket_.close();
}

void Connection::disconnect() {
  if (closed_)
    return;
  close();
  if (auto server = server_.lock())
    server->remove(id_);
}

void Connection::maintain(std::uint64_t nowNs) {
  if (closed_)
    return;
  auto server = server_.lock();
  if (!server)
    return;
  const auto &options = server->options();
  if (!session_.empty() && options.sessionTimeoutSeconds != 0 &&
      nowNs - lastActivityNs_ >=
          static_cast<std::uint64_t>(options.sessionTimeoutSeconds) *
              1'000'000'000ULL) {
    resetSession();
    server->countExpiredSession();
    return;
  }
  if (!playing_ || options.rtcpReportIntervalMs == 0)
    return;
  const std::uint64_t interval =
      static_cast<std::uint64_t>(options.rtcpReportIntervalMs) * 1'000'000ULL;
  for (TrackState *track : {&video_, &audio_}) {
    if (track->configured && track->packetCount != 0 &&
        (track->lastReportNs == 0 || nowNs - track->lastReportNs >= interval))
      sendSenderReport(*track, nowNs);
  }
}

void Connection::rearm() {
  if (closed_)
    return;
  auto server = server_.lock();
  if (!server || !server->loop().watchFd(
                     socket_.nativeHandle(),
                     EPOLLIN | (output_.empty() ? 0U : EPOLLOUT),
                     [this](std::uint32_t events) { onReady(events); }))
    disconnect();
}

void Connection::onReady(std::uint32_t events) {
  if (closed_)
    return;
  if ((events & EPOLLOUT) != 0)
    flush();
  if (closed_)
    return;
  if ((events & EPOLLIN) != 0) {
    char buffer[8192];
    for (;;) {
      const auto count = socket_.receive(buffer, sizeof(buffer));
      if (count > 0) {
        input_.append(buffer, static_cast<std::size_t>(count));
        if (input_.size() > kMaximumRequestBytes) {
          disconnect();
          return;
        }
        continue;
      }
      if (count == 0) {
        disconnect();
        return;
      }
      if (count != -EAGAIN && count != -EWOULDBLOCK) {
        disconnect();
        return;
      }
      break;
    }
    parseInput();
  }
  if (!closed_)
    rearm();
}

void Connection::parseInput() {
  while (!closed_) {
    if (!input_.empty() && input_.front() == '$') {
      if (input_.size() < 4)
        return;
      const std::size_t size =
          (static_cast<std::uint8_t>(input_[2]) << 8U) |
          static_cast<std::uint8_t>(input_[3]);
      if (input_.size() < size + 4)
        return;
      input_.erase(0, size + 4);
      lastActivityNs_ = monoNowNs();
      continue;
    }
    const auto headerEnd = input_.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
      return;
    Request request;
    std::istringstream stream(input_.substr(0, headerEnd));
    std::string line;
    if (!std::getline(stream, line)) {
      disconnect();
      return;
    }
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    std::istringstream first(line);
    if (!(first >> request.method >> request.url >> request.version)) {
      disconnect();
      return;
    }
    while (std::getline(stream, line)) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      const auto colon = line.find(':');
      if (colon != std::string::npos)
        request.headers[lower(trim(line.substr(0, colon)))] =
            trim(line.substr(colon + 1));
    }
    std::size_t contentLength = 0;
    const std::string length = request.header("content-length");
    if (!length.empty())
      contentLength = static_cast<std::size_t>(
          std::strtoul(length.c_str(), nullptr, 10));
    if (contentLength > kMaximumRequestBytes) {
      disconnect();
      return;
    }
    if (input_.size() < headerEnd + 4 + contentLength)
      return;
    request.body = input_.substr(headerEnd + 4, contentLength);
    input_.erase(0, headerEnd + 4 + contentLength);
    handle(request);
  }
}

void Connection::respond(const Request &request, int status,
                         const std::string &reason,
                         const std::string &headers,
                         const std::string &body) {
  std::string response =
      "RTSP/1.0 " + std::to_string(status) + " " + reason + "\r\n";
  const std::string cseq = request.header("cseq");
  if (!cseq.empty())
    response += "CSeq: " + cseq + "\r\n";
  response += "Server: DarkOS-SvcKit/2.0\r\n";
  response += headers;
  response += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  response += body;
  queue(response.data(), response.size());
}

void Connection::challenge(const Request &request) {
  auto server = server_.lock();
  if (!server)
    return;
  const auto &options = server->options();
  respond(request, 401, "Unauthorized",
          "WWW-Authenticate: Digest realm=\"" +
              options.authenticationRealm + "\", nonce=\"" +
              nonce_ + "\", algorithm=MD5, qop=\"auth\"\r\n");
}

bool Connection::authorized(const Request &request) {
  auto server = server_.lock();
  if (!server)
    return false;
  const auto &options = server->options();
  if (options.username.empty())
    return true;
  const auto fields = parseDigestFields(request.header("authorization"));
  const auto get = [&](const char *name) -> std::string {
    const auto entry = fields.find(name);
    return entry == fields.end() ? std::string{} : entry->second;
  };
  const std::string username = get("username");
  const std::string realm = get("realm");
  const std::string nonce = get("nonce");
  const std::string uri = get("uri");
  const std::string response = lower(get("response"));
  const std::string qop = lower(get("qop"));
  const std::string nc = get("nc");
  const std::string cnonce = get("cnonce");
  // live555 signs SETUP with the presentation URL even though the request
  // target is the individual track URL. Its presentation URL can also retain
  // the trailing slash from Content-Base. Compare normalized presentation
  // URLs for SETUP, but keep exact URI matching for every other method.
  const bool uriMatches =
      uri == request.url ||
      (request.method == "SETUP" && baseUrl(uri) == baseUrl(request.url));
  bool valid = username == options.username &&
               realm == options.authenticationRealm &&
               nonce == nonce_ && uriMatches &&
               !response.empty();
  std::string expected;
  std::uint32_t nonceCount = 0;
  if (valid) {
    const std::string ha1 = md5Hex(options.username + ":" +
                                   options.authenticationRealm + ":" +
                                   options.password);
    const std::string ha2 = md5Hex(request.method + ":" + uri);
    if (qop.empty()) {
      expected = md5Hex(ha1 + ":" + nonce + ":" + ha2);
    } else if (qop == "auth" && !nc.empty() && !cnonce.empty()) {
      char *end = nullptr;
      const unsigned long parsed = std::strtoul(nc.c_str(), &end, 16);
      if (end == nc.c_str() || *end != '\0' || parsed > 0xffffffffUL)
        valid = false;
      else
        nonceCount = static_cast<std::uint32_t>(parsed);
      expected = md5Hex(ha1 + ":" + nonce + ":" + nc + ":" + cnonce +
                        ":auth:" + ha2);
    } else {
      valid = false;
    }
  }
  valid = valid && constantTimeEqual(expected, response);
  if (valid && qop == "auth" && cnonce == lastClientNonce_ &&
      nonceCount <= lastNonceCount_)
    valid = false;
  if (valid && qop == "auth") {
    lastClientNonce_ = cnonce;
    lastNonceCount_ = nonceCount;
  }
  if (!valid) {
    server->countAuthFailure();
    challenge(request);
  }
  return valid;
}

bool Connection::sessionMatches(const Request &request) const {
  if (session_.empty())
    return false;
  const std::string supplied = request.header("session");
  const auto separator = supplied.find(';');
  return trim(supplied.substr(0, separator)) == session_;
}

bool Connection::setupTrack(const Request &request, TrackKind kind,
                            const Transport &transport) {
  auto server = server_.lock();
  if (!server)
    return false;
  const auto &options = server->options();
  TrackState &track = kind == TrackKind::Video ? video_ : audio_;
  const bool replacingMulticast = track.configured && track.multicast;
  if (track.configured)
    resetTrack(track);
  if (replacingMulticast && !hasMulticastTrack() && ownsMulticast_) {
    server->releaseMulticast(id_);
    ownsMulticast_ = false;
  }

  track.tcp = transport.tcp;
  track.multicast = transport.multicast;
  if (transport.tcp) {
    track.rtpChannel = transport.interleavedSpecified
                           ? transport.rtpChannel
                           : (kind == TrackKind::Video ? 0 : 2);
    track.rtcpChannel = transport.interleavedSpecified
                            ? transport.rtcpChannel
                            : track.rtpChannel + 1;
  } else {
    const auto family = transport.multicast ? network::AddressFamily::IPv4
                                            : peer_.family();
    track.udpRtp = network::Socket::udp(family);
    track.udpRtcp = network::Socket::udp(family);
    network::Address bindAddress;
    const char *anyAddress = family == network::AddressFamily::IPv6
                                 ? "::"
                                 : "0.0.0.0";
    if (!track.udpRtp.valid() || !track.udpRtcp.valid() ||
        network::Address::fromIp(anyAddress, 0, bindAddress) != 0 ||
        track.udpRtp.bind(bindAddress) != 0 ||
        track.udpRtcp.bind(bindAddress) != 0) {
      resetTrack(track);
      respond(request, 500, "Internal Server Error");
      return false;
    }
    if (transport.multicast) {
      if (!options.enableMulticast ||
          peer_.family() != network::AddressFamily::IPv4 ||
          !server->acquireMulticast(id_)) {
        resetTrack(track);
        respond(request, options.enableMulticast ? 453 : 461,
                options.enableMulticast ? "Not Enough Bandwidth"
                                        : "Unsupported Transport");
        return false;
      }
      ownsMulticast_ = true;
      const std::uint16_t rtpPort = kind == TrackKind::Video
                                        ? options.multicastVideoPort
                                        : options.multicastAudioPort;
      if (network::Address::fromIp(options.multicastAddress, rtpPort,
                                   track.rtpTarget) != 0 ||
          network::Address::fromIp(options.multicastAddress,
                                   static_cast<std::uint16_t>(rtpPort + 1),
                                   track.rtcpTarget) != 0 ||
          track.udpRtp.setMulticastTtl(options.multicastTtl) != 0 ||
          track.udpRtcp.setMulticastTtl(options.multicastTtl) != 0) {
        resetTrack(track);
        if (!hasMulticastTrack()) {
          server->releaseMulticast(id_);
          ownsMulticast_ = false;
        }
        respond(request, 500, "Internal Server Error");
        return false;
      }
    } else if (network::Address::fromIp(peer_.ip(), transport.clientRtpPort,
                                        track.rtpTarget) != 0 ||
               network::Address::fromIp(peer_.ip(), transport.clientRtcpPort,
                                        track.rtcpTarget) != 0) {
      resetTrack(track);
      respond(request, 500, "Internal Server Error");
      return false;
    }
  }

  track.configured = true;
  track.firstPacket = true;
  track.packetCount = 0;
  track.octetCount = 0;
  track.lastReportNs = 0;
  if (session_.empty())
    session_ = randomHex();
  presentationUrl_ = baseUrl(request.url);
  playing_ = false;
  awaitingKeyframe_ = true;

  std::string transportResponse;
  if (transport.tcp) {
    transportResponse = "Transport: RTP/AVP/TCP;unicast;interleaved=" +
                        std::to_string(track.rtpChannel) + "-" +
                        std::to_string(track.rtcpChannel) + "\r\n";
  } else if (transport.multicast) {
    const std::uint16_t port = kind == TrackKind::Video
                                   ? options.multicastVideoPort
                                   : options.multicastAudioPort;
    transportResponse = "Transport: RTP/AVP;multicast;destination=" +
                        options.multicastAddress + ";port=" +
                        std::to_string(port) + "-" +
                        std::to_string(static_cast<unsigned>(port) + 1U) +
                        ";ttl=" + std::to_string(options.multicastTtl) +
                        "\r\n";
  } else {
    network::Address localRtp;
    network::Address localRtcp;
    if (track.udpRtp.localAddress(localRtp) != 0 ||
        track.udpRtcp.localAddress(localRtcp) != 0) {
      resetTrack(track);
      respond(request, 500, "Internal Server Error");
      return false;
    }
    transportResponse =
        "Transport: RTP/AVP;unicast;client_port=" +
        std::to_string(transport.clientRtpPort) + "-" +
        std::to_string(transport.clientRtcpPort) + ";server_port=" +
        std::to_string(localRtp.port()) + "-" +
        std::to_string(localRtcp.port()) + "\r\n";
  }
  respond(request, 200, "OK",
          transportResponse + "Session: " + session_ + ";timeout=" +
              std::to_string(options.sessionTimeoutSeconds) + "\r\n");
  return true;
}

void Connection::handle(const Request &request) {
  auto server = server_.lock();
  if (!server)
    return;
  server->countRequest();
  if (request.version != "RTSP/1.0" || request.header("cseq").empty()) {
    respond(request, 400, "Bad Request");
    return;
  }
  if (request.method != "OPTIONS" && !authorized(request))
    return;
  lastActivityNs_ = monoNowNs();

  const auto &options = server->options();
  const std::string mount = server->mountPath();
  const std::string path = pathFromUrl(request.url);
  const bool presentation = path == mount;
  const bool videoTrack = path == mount + "/trackID=0" || path == mount + "/track0";
  const bool audioTrack = path == mount + "/trackID=1" || path == mount + "/track1";

  if (request.method == "OPTIONS") {
    respond(request, 200, "OK", std::string("Public: ") + kPublicMethods + "\r\n");
  } else if (request.method == "DESCRIBE") {
    if (!presentation) {
      respond(request, 404, "Not Found");
      return;
    }
    std::string sdp =
        "v=0\r\n"
        "o=- 0 0 IN IP4 0.0.0.0\r\n"
        "s=DarkOS IPC\r\n"
        "t=0 0\r\n"
        "a=control:*\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1\r\n"
        "a=control:trackID=0\r\n";
    if (options.enableAudio) {
      const char *encoding = options.audioCodec == media::AudioCodec::G711U
                                 ? "PCMU"
                                 : "PCMA";
      sdp += "m=audio 0 RTP/AVP 97\r\n"
             "c=IN IP4 0.0.0.0\r\n"
             "a=rtpmap:97 " + std::string(encoding) + "/" +
             std::to_string(options.audioSampleRate) + "/" +
             std::to_string(options.audioChannelCount) + "\r\n"
             "a=control:trackID=1\r\n";
    }
    respond(request, 200, "OK",
            "Content-Type: application/sdp\r\nContent-Base: " +
                baseUrl(request.url) + "/\r\n",
            sdp);
  } else if (request.method == "SETUP") {
    if (!videoTrack && !(audioTrack && options.enableAudio)) {
      respond(request, 404, "Not Found");
      return;
    }
    Transport transport;
    if (!parseTransport(request.header("transport"), transport)) {
      respond(request, 461, "Unsupported Transport");
      return;
    }
    if (!session_.empty() && !sessionMatches(request)) {
      respond(request, 454, "Session Not Found");
      return;
    }
    setupTrack(request, videoTrack ? TrackKind::Video : TrackKind::Audio,
               transport);
  } else if (request.method == "PLAY") {
    if (!hasConfiguredTrack() || !sessionMatches(request)) {
      respond(request, 454, "Session Not Found");
      return;
    }
    playing_ = true;
    awaitingKeyframe_ = true;
    video_.firstPacket = true;
    audio_.firstPacket = true;
    video_.timestampInitialized = false;
    audio_.timestampInitialized = false;
    // 不发送 RTP-Info 的 rtptime/seq 提示：此时首个 RTP 包尚未发出，
    // lastTimestamp 仍是占位值，填入它会让客户端错误地重建首段 PTS。
    // 客户端会以实际收到的第一个 RTP 包建立 live 时钟。
    respond(request, 200, "OK",
            "Session: " + session_ + ";timeout=" +
                std::to_string(options.sessionTimeoutSeconds) +
                "\r\nRange: npt=0.000-\r\n");
  } else if (request.method == "PAUSE") {
    if (!sessionMatches(request)) {
      respond(request, 454, "Session Not Found");
      return;
    }
    playing_ = false;
    respond(request, 200, "OK", "Session: " + session_ + "\r\n");
  } else if (request.method == "TEARDOWN") {
    if (!sessionMatches(request)) {
      respond(request, 454, "Session Not Found");
      return;
    }
    const std::string oldSession = session_;
    resetSession();
    respond(request, 200, "OK", "Session: " + oldSession + "\r\n");
  } else if (request.method == "GET_PARAMETER" ||
             request.method == "SET_PARAMETER") {
    if (!request.header("session").empty() && !sessionMatches(request)) {
      respond(request, 454, "Session Not Found");
      return;
    }
    if (request.method == "SET_PARAMETER" && !request.body.empty()) {
      respond(request, 406, "Not Acceptable");
      return;
    }
    respond(request, 200, "OK",
            session_.empty() ? std::string{}
                             : "Session: " + session_ + ";timeout=" +
                                   std::to_string(options.sessionTimeoutSeconds) +
                                   "\r\n");
  } else if (request.method == "ANNOUNCE" || request.method == "RECORD") {
    respond(request, 501, "Not Implemented");
  } else {
    respond(request, 405, "Method Not Allowed",
            std::string("Allow: ") + kPublicMethods + "\r\n");
  }
}

bool Connection::queue(const void *data, std::size_t size) {
  if (closed_ || size == 0)
    return false;
  if (maximumBacklog_ != 0 && output_.size() + size > maximumBacklog_) {
    disconnect();
    return false;
  }
  output_.append(static_cast<const char *>(data), size);
  flush();
  return !closed_;
}

void Connection::flush() {
  while (!closed_ && !output_.empty()) {
    const auto count = socket_.send(output_.data(), output_.size());
    if (count > 0) {
      output_.erase(0, static_cast<std::size_t>(count));
      continue;
    }
    if (count == -EAGAIN || count == -EWOULDBLOCK)
      return;
    disconnect();
  }
}

void Connection::sendVideo(const media::VideoPacket &packet,
                           const common::H264RtpPacketizer &packetizer) {
  if (!playing_ || closed_ || !video_.configured)
    return;
  if (awaitingKeyframe_) {
    if (!packet.keyframe) {
      if (auto server = server_.lock())
        server->countKeyframeDrop();
      return;
    }
    awaitingKeyframe_ = false;
  }
  const std::uint32_t timestamp = common::videoTimestamp90k(packet.timestampNs);
  if (!video_.timestampInitialized) {
    video_.lastTimestamp = timestamp == 0 ? 1 : timestamp;
    video_.timestampInitialized = true;
  } else if (static_cast<std::int32_t>(timestamp - video_.lastTimestamp) >= 0) {
    video_.lastTimestamp = timestamp == 0 ? 1 : timestamp;
  } else {
    // 首个 IDR 后可能还有旧 P 帧从异步队列晚到；继续发送会让 VLC
    // 看到 PTS 回退，而且旧帧内容也不应插入当前 GOP。直接丢弃。
    return;
  }
  packetizer.packetize(
      packet.buffer->data(), packet.buffer->size(),
      [&](const std::uint8_t *payload, std::size_t size, bool marker) {
        return sendRtp(video_, kVideoPayloadType, payload, size, marker,
                       video_.lastTimestamp);
      });
}

void Connection::sendAudio(const media::AudioPacket &packet) {
  if (!playing_ || closed_ || !audio_.configured)
    return;
  const std::uint32_t timestamp =
      mediaTimestamp(packet.timestampNs, packet.sampleRate);
  if (!audio_.timestampInitialized) {
    audio_.lastTimestamp = timestamp == 0 ? 1 : timestamp;
    audio_.timestampInitialized = true;
  } else if (static_cast<std::int32_t>(timestamp - audio_.lastTimestamp) >= 0) {
    audio_.lastTimestamp = timestamp == 0 ? 1 : timestamp;
  } else {
    // 音频同样丢弃异步队列中晚到的旧包，避免破坏解码时钟。
    return;
  }
  const bool marker = audio_.firstPacket;
  if (sendRtp(audio_, kAudioPayloadType, packet.buffer->data(),
              packet.buffer->size(), marker, audio_.lastTimestamp))
    audio_.firstPacket = false;
}

bool Connection::sendRtp(TrackState &track, std::uint8_t payloadType,
                         const std::uint8_t *payload, std::size_t size,
                         bool marker, std::uint32_t timestamp) {
  std::vector<std::uint8_t> packet(12 + size);
  packet[0] = 0x80;
  packet[1] = static_cast<std::uint8_t>(payloadType | (marker ? 0x80U : 0U));
  put16(packet.data() + 2, track.sequence);
  put32(packet.data() + 4, timestamp);
  put32(packet.data() + 8, track.ssrc);
  std::copy_n(payload, size, packet.data() + 12);
  bool sent = false;
  if (track.tcp) {
    std::vector<std::uint8_t> framed(4 + packet.size());
    framed[0] = '$';
    framed[1] = static_cast<std::uint8_t>(track.rtpChannel);
    put16(framed.data() + 2, static_cast<std::uint16_t>(packet.size()));
    std::copy(packet.begin(), packet.end(), framed.begin() + 4);
    sent = queue(framed.data(), framed.size());
  } else {
    sent = track.udpRtp.sendTo(packet.data(), packet.size(), track.rtpTarget) ==
           static_cast<std::ptrdiff_t>(packet.size());
  }
  if (sent) {
    ++track.sequence;
    ++track.packetCount;
    track.octetCount += size;
    if (auto server = server_.lock())
      server->countRtp();
  }
  return sent;
}

bool Connection::sendControl(TrackState &track, const std::uint8_t *packet,
                             std::size_t size) {
  if (track.tcp) {
    std::vector<std::uint8_t> framed(4 + size);
    framed[0] = '$';
    framed[1] = static_cast<std::uint8_t>(track.rtcpChannel);
    put16(framed.data() + 2, static_cast<std::uint16_t>(size));
    std::copy_n(packet, size, framed.data() + 4);
    return queue(framed.data(), framed.size());
  }
  return track.udpRtcp.sendTo(packet, size, track.rtcpTarget) ==
         static_cast<std::ptrdiff_t>(size);
}

void Connection::sendSenderReport(TrackState &track, std::uint64_t nowNs) {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const std::uint64_t unixNs = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
  const std::uint64_t unixSeconds = unixNs / 1'000'000'000ULL;
  const std::uint64_t fraction =
      ((unixNs % 1'000'000'000ULL) << 32U) / 1'000'000'000ULL;
  const std::string cname = "darkos-" + std::to_string(track.ssrc);
  const std::size_t sdesSize = (8 + 2 + cname.size() + 1 + 3) & ~std::size_t(3);
  std::vector<std::uint8_t> report(28 + sdesSize, 0);
  report[0] = 0x80;
  report[1] = 200;
  put16(report.data() + 2, 6);
  put32(report.data() + 4, track.ssrc);
  put32(report.data() + 8,
        static_cast<std::uint32_t>(unixSeconds + kNtpUnixEpochOffset));
  put32(report.data() + 12, static_cast<std::uint32_t>(fraction));
  put32(report.data() + 16, track.lastTimestamp);
  put32(report.data() + 20, static_cast<std::uint32_t>(track.packetCount));
  put32(report.data() + 24, static_cast<std::uint32_t>(track.octetCount));
  std::uint8_t *sdes = report.data() + 28;
  sdes[0] = 0x81;
  sdes[1] = 202;
  put16(sdes + 2, static_cast<std::uint16_t>(sdesSize / 4 - 1));
  put32(sdes + 4, track.ssrc);
  sdes[8] = 1;
  sdes[9] = static_cast<std::uint8_t>(cname.size());
  std::copy(cname.begin(), cname.end(), sdes + 10);
  if (sendControl(track, report.data(), report.size())) {
    track.lastReportNs = nowNs;
    if (auto server = server_.lock())
      server->countRtcp();
  }
}

RtspServer::RtspServer(std::shared_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

RtspServer::~RtspServer() { implementation_->stop(); }

std::shared_ptr<RtspServer>
RtspServer::create(EventLoop &eventLoop, const RtspServerOptions &options,
                   std::string &error) {
  const bool credentialsValid =
      options.username.empty() == options.password.empty();
  const bool audioValid =
      !options.enableAudio ||
      ((options.audioCodec == media::AudioCodec::G711A ||
        options.audioCodec == media::AudioCodec::G711U) &&
       options.audioSampleRate != 0 && options.audioChannelCount != 0 &&
       options.audioChannelCount <= 2);
  const bool multicastPortsValid =
      options.multicastVideoPort != 0 && options.multicastVideoPort < 65535 &&
      options.multicastAudioPort != 0 && options.multicastAudioPort < 65535;
  if (options.mountPath.empty() ||
      options.mountPath.find('/') != std::string::npos ||
      options.maximumRtpPayloadBytes < 3 ||
      !credentialsValid ||
      options.authenticationRealm.find('"') != std::string::npos ||
      !audioValid || (options.enableMulticast && !multicastPortsValid)) {
    error = "invalid RTSP server options";
    return nullptr;
  }
  if (options.enableMulticast) {
    network::Address multicast;
    if (network::Address::fromIp(options.multicastAddress,
                                 options.multicastVideoPort, multicast) != 0 ||
        !isIpv4MulticastAddress(options.multicastAddress)) {
      error = "invalid IPv4 multicast address";
      return nullptr;
    }
  }
  auto implementation = std::make_shared<Impl>(eventLoop, options);
  auto server = std::shared_ptr<RtspServer>(
      new (std::nothrow) RtspServer(std::move(implementation)));
  if (!server)
    error = "out of memory creating RTSP server";
  else
    error.clear();
  return server;
}

int RtspServer::start() {
  const unsigned previous = startReferences_.fetch_add(1);
  if (previous != 0)
    return 0;
  std::string error;
  const int result = implementation_->start(error);
  if (result != 0)
    startReferences_.fetch_sub(1);
  return result;
}

int RtspServer::stop() {
  unsigned references = startReferences_.load();
  while (references != 0 &&
         !startReferences_.compare_exchange_weak(references, references - 1)) {
  }
  return references == 1 ? implementation_->stop() : 0;
}

int RtspServer::consume(media::VideoPacketPtr packet) {
  return implementation_->consume(std::move(packet));
}

int RtspServer::consume(media::AudioPacketPtr packet) {
  return implementation_->consume(std::move(packet));
}

std::uint16_t RtspServer::listeningPort() const noexcept {
  return implementation_->port();
}

RtspServerStats RtspServer::stats() const noexcept {
  return implementation_->stats();
}

} // namespace darkos::protocols::rtsp
