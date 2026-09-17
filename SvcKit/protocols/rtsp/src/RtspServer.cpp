#include <RtspServer.h>

#include <network_connection.h>
#include <H264RtpPacketizer.h>

#include <sys/epoll.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
constexpr std::uint8_t kPayloadType = 96;
constexpr char kPublicMethods[] =
    "OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER, "
    "SET_PARAMETER";

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
  const auto track = url.rfind("/trackID=0");
  if (track != std::string::npos && track + 10 == url.size())
    url.resize(track);
  return url;
}

std::string sessionId() {
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
  int rtpChannel{0};
  int rtcpChannel{1};
  std::uint16_t clientRtpPort{0};
  std::uint16_t clientRtcpPort{0};
};

bool parseTransport(const std::string &header, Transport &output) {
  const std::string value = lower(header);
  if (value.find("rtp/avp/tcp") != std::string::npos) {
    output.tcp = true;
    int first = 0;
    int second = 1;
    if (value.find("interleaved=") != std::string::npos &&
        !parsePair(value, "interleaved=", first, second))
      return false;
    output.rtpChannel = first;
    output.rtcpChannel = second;
    return first >= 0 && first <= 255 && second >= 0 && second <= 255;
  }
  if (value.find("rtp/avp") == std::string::npos)
    return false;
  int first = 0;
  int second = 0;
  if (!parsePair(value, "client_port=", first, second) || first == 0)
    return false;
  output.clientRtpPort = static_cast<std::uint16_t>(first);
  output.clientRtcpPort = static_cast<std::uint16_t>(second);
  return true;
}

} // namespace

class Connection final {
public:
  Connection(std::weak_ptr<RtspServer::Impl> server, std::uint64_t id,
             network::Socket socket, network::Address peer,
             std::size_t maximumBacklog)
      : server_(std::move(server)), id_(id), socket_(std::move(socket)),
        peer_(peer), maximumBacklog_(maximumBacklog) {
    std::random_device random;
    sequence_ = static_cast<std::uint16_t>(random());
    ssrc_ = (static_cast<std::uint32_t>(random()) << 16U) ^ random();
  }

  ~Connection() { close(); }
  void start();
  void close();
  void sendVideo(const media::VideoPacket &packet,
                 const common::H264RtpPacketizer &packetizer);

private:
  void rearm();
  void onReady(std::uint32_t events);
  void parseInput();
  void handle(const Request &request);
  void respond(const Request &request, int status, const std::string &reason,
               const std::string &headers = {}, const std::string &body = {});
  bool queue(const void *data, std::size_t size);
  void flush();
  void disconnect();
  bool sendRtp(const std::uint8_t *payload, std::size_t size, bool marker,
               std::uint32_t timestamp);

  std::weak_ptr<RtspServer::Impl> server_;
  std::uint64_t id_{0};
  network::Socket socket_;
  network::Address peer_;
  network::Socket udpRtp_;
  network::Address udpTarget_;
  std::string input_;
  std::string output_;
  std::string session_;
  std::string presentationUrl_;
  std::size_t maximumBacklog_{0};
  std::uint16_t sequence_{0};
  std::uint32_t ssrc_{0};
  std::uint32_t lastTimestamp_{1};
  int rtpChannel_{0};
  bool tcp_{true};
  bool setup_{false};
  bool playing_{false};
  bool awaitingKeyframe_{true};
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
    port_ = server_->localAddress().port();
    error.clear();
    return 0;
  }

  int stop() {
    if (!running_.exchange(false))
      return 0;
    if (server_)
      server_->stop();
    for (auto &entry : connections_)
      entry.second->close();
    connections_.clear();
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
               self->distribute(*packet);
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

  EventLoop &loop() noexcept { return loop_; }
  const RtspServerOptions &options() const noexcept { return options_; }
  const std::string &mountPath() const noexcept { return options_.mountPath; }
  void countRequest() noexcept { ++requests_; }
  void countRtp() noexcept { ++rtpPackets_; }
  void countKeyframeDrop() noexcept { ++droppedBeforeKeyframe_; }
  std::uint16_t port() const noexcept { return port_; }

  RtspServerStats stats() const noexcept {
    return {acceptedConnections_.load(), activeConnections_.load(),
            requests_.load(), videoPackets_.load(), rtpPackets_.load(),
            droppedBeforeKeyframe_.load()};
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

  void distribute(const media::VideoPacket &packet) {
    if (!running_)
      return;
    ++videoPackets_;
    for (auto &entry : connections_)
      entry.second->sendVideo(packet, packetizer_);
  }

  EventLoop &loop_;
  RtspServerOptions options_;
  common::H264RtpPacketizer packetizer_;
  std::unique_ptr<network::TcpServer> server_;
  std::unordered_map<std::uint64_t, std::unique_ptr<Connection>> connections_;
  std::atomic<bool> running_{false};
  std::uint64_t nextConnectionId_{1};
  std::uint16_t port_{0};
  std::atomic<std::uint64_t> acceptedConnections_{0};
  std::atomic<std::uint64_t> activeConnections_{0};
  std::atomic<std::uint64_t> requests_{0};
  std::atomic<std::uint64_t> videoPackets_{0};
  std::atomic<std::uint64_t> rtpPackets_{0};
  std::atomic<std::uint64_t> droppedBeforeKeyframe_{0};
};

void Connection::start() { rearm(); }

void Connection::close() {
  if (closed_)
    return;
  closed_ = true;
  if (auto server = server_.lock())
    server->loop().unwatchFd(socket_.nativeHandle());
  socket_.close();
  udpRtp_.close();
}

void Connection::disconnect() {
  if (closed_)
    return;
  close();
  if (auto server = server_.lock())
    server->remove(id_);
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
      contentLength = static_cast<std::size_t>(std::strtoul(length.c_str(), nullptr, 10));
    if (contentLength > kMaximumRequestBytes ||
        input_.size() < headerEnd + 4 + contentLength)
      return;
    request.body = input_.substr(headerEnd + 4, contentLength);
    input_.erase(0, headerEnd + 4 + contentLength);
    handle(request);
  }
}

void Connection::respond(const Request &request, int status,
                         const std::string &reason, const std::string &headers,
                         const std::string &body) {
  std::string response = "RTSP/1.0 " + std::to_string(status) + " " + reason + "\r\n";
  const std::string cseq = request.header("cseq");
  if (!cseq.empty())
    response += "CSeq: " + cseq + "\r\n";
  response += "Server: DarkOS-SvcKit/1.0\r\n";
  response += headers;
  response += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  response += body;
  queue(response.data(), response.size());
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
  const std::string mount = server->mountPath();
  const std::string path = pathFromUrl(request.url);
  const bool presentation = path == mount;
  const bool track = path == mount + "/trackID=0" || path == mount + "/track0";

  if (request.method == "OPTIONS") {
    respond(request, 200, "OK", std::string("Public: ") + kPublicMethods + "\r\n");
  } else if (request.method == "DESCRIBE") {
    if (!presentation) {
      respond(request, 404, "Not Found");
      return;
    }
    const std::string sdp =
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
    respond(request, 200, "OK",
            "Content-Type: application/sdp\r\nContent-Base: " +
                baseUrl(request.url) + "/\r\n",
            sdp);
  } else if (request.method == "SETUP") {
    if (!track) {
      respond(request, 404, "Not Found");
      return;
    }
    Transport transport;
    if (!parseTransport(request.header("transport"), transport)) {
      respond(request, 461, "Unsupported Transport");
      return;
    }
    tcp_ = transport.tcp;
    rtpChannel_ = transport.rtpChannel;
    std::string transportResponse;
    if (tcp_) {
      transportResponse = "Transport: RTP/AVP/TCP;unicast;interleaved=" +
                          std::to_string(transport.rtpChannel) + "-" +
                          std::to_string(transport.rtcpChannel) + "\r\n";
    } else {
      udpRtp_ = network::Socket::udp(peer_.family());
      network::Address local;
      network::Address bindAddress;
      if (!udpRtp_.valid() ||
          network::Address::fromIp(peer_.family() == network::AddressFamily::IPv6
                                      ? "::"
                                      : "0.0.0.0",
                                  0, bindAddress) != 0 ||
          udpRtp_.bind(bindAddress) != 0 || udpRtp_.localAddress(local) != 0 ||
          network::Address::fromIp(peer_.ip(), transport.clientRtpPort,
                                   udpTarget_) != 0) {
        respond(request, 500, "Internal Server Error");
        return;
      }
      transportResponse =
          "Transport: RTP/AVP;unicast;client_port=" +
          std::to_string(transport.clientRtpPort) + "-" +
          std::to_string(transport.clientRtcpPort) + ";server_port=" +
          std::to_string(local.port()) + "-" +
          std::to_string(static_cast<unsigned>(local.port()) + 1U) + "\r\n";
    }
    if (session_.empty())
      session_ = sessionId();
    presentationUrl_ = baseUrl(request.url);
    setup_ = true;
    playing_ = false;
    awaitingKeyframe_ = true;
    respond(request, 200, "OK", transportResponse + "Session: " + session_ + "\r\n");
  } else if (request.method == "PLAY") {
    if (!setup_ || request.header("session").find(session_) != 0) {
      respond(request, 454, "Session Not Found");
      return;
    }
    playing_ = true;
    awaitingKeyframe_ = true;
    const std::string url = presentationUrl_.empty() ? baseUrl(request.url) : presentationUrl_;
    respond(request, 200, "OK",
            "Session: " + session_ + "\r\nRange: npt=0.000-\r\nRTP-Info: url=" +
                url + "/trackID=0;seq=" + std::to_string(sequence_) +
                ";rtptime=" + std::to_string(lastTimestamp_) + "\r\n");
  } else if (request.method == "PAUSE") {
    if (!setup_ || request.header("session").find(session_) != 0) {
      respond(request, 454, "Session Not Found");
      return;
    }
    playing_ = false;
    respond(request, 200, "OK", "Session: " + session_ + "\r\n");
  } else if (request.method == "TEARDOWN") {
    if (!setup_ || request.header("session").find(session_) != 0) {
      respond(request, 454, "Session Not Found");
      return;
    }
    playing_ = false;
    setup_ = false;
    udpRtp_.close();
    respond(request, 200, "OK", "Session: " + session_ + "\r\n");
  } else if (request.method == "GET_PARAMETER") {
    respond(request, 200, "OK", session_.empty() ? std::string{} :
                                             "Session: " + session_ + "\r\n");
  } else if (request.method == "SET_PARAMETER") {
    respond(request, request.body.empty() ? 200 : 406,
            request.body.empty() ? "OK" : "Not Acceptable");
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
  if (!playing_ || closed_)
    return;
  if (awaitingKeyframe_) {
    if (!packet.keyframe) {
      if (auto server = server_.lock())
        server->countKeyframeDrop();
      return;
    }
    awaitingKeyframe_ = false;
  }
  lastTimestamp_ = common::videoTimestamp90k(packet.timestampNs);
  if (lastTimestamp_ == 0)
    lastTimestamp_ = 1;
  packetizer.packetize(
      packet.buffer->data(), packet.buffer->size(),
      [&](const std::uint8_t *payload, std::size_t size, bool marker) {
        return sendRtp(payload, size, marker, lastTimestamp_);
      });
}

bool Connection::sendRtp(const std::uint8_t *payload, std::size_t size,
                         bool marker, std::uint32_t timestamp) {
  std::vector<std::uint8_t> packet(12 + size);
  packet[0] = 0x80;
  packet[1] = static_cast<std::uint8_t>(kPayloadType | (marker ? 0x80U : 0U));
  packet[2] = static_cast<std::uint8_t>(sequence_ >> 8U);
  packet[3] = static_cast<std::uint8_t>(sequence_);
  packet[4] = static_cast<std::uint8_t>(timestamp >> 24U);
  packet[5] = static_cast<std::uint8_t>(timestamp >> 16U);
  packet[6] = static_cast<std::uint8_t>(timestamp >> 8U);
  packet[7] = static_cast<std::uint8_t>(timestamp);
  packet[8] = static_cast<std::uint8_t>(ssrc_ >> 24U);
  packet[9] = static_cast<std::uint8_t>(ssrc_ >> 16U);
  packet[10] = static_cast<std::uint8_t>(ssrc_ >> 8U);
  packet[11] = static_cast<std::uint8_t>(ssrc_);
  std::copy_n(payload, size, packet.data() + 12);
  ++sequence_;
  bool sent = false;
  if (tcp_) {
    std::vector<std::uint8_t> framed(4 + packet.size());
    framed[0] = '$';
    framed[1] = static_cast<std::uint8_t>(rtpChannel_);
    framed[2] = static_cast<std::uint8_t>(packet.size() >> 8U);
    framed[3] = static_cast<std::uint8_t>(packet.size());
    std::copy(packet.begin(), packet.end(), framed.begin() + 4);
    sent = queue(framed.data(), framed.size());
  } else {
    sent = udpRtp_.sendTo(packet.data(), packet.size(), udpTarget_) ==
           static_cast<std::ptrdiff_t>(packet.size());
  }
  if (sent) {
    if (auto server = server_.lock())
      server->countRtp();
  }
  return sent;
}

RtspServer::RtspServer(std::shared_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

RtspServer::~RtspServer() { stop(); }

std::shared_ptr<RtspServer>
RtspServer::create(EventLoop &eventLoop, const RtspServerOptions &options,
                   std::string &error) {
  if (options.mountPath.empty() || options.mountPath.find('/') != std::string::npos ||
      options.maximumRtpPayloadBytes < 3) {
    error = "invalid RTSP mount path or RTP payload size";
    return nullptr;
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
  std::string error;
  return implementation_->start(error);
}

int RtspServer::stop() { return implementation_->stop(); }

int RtspServer::consume(media::VideoPacketPtr packet) {
  return implementation_->consume(std::move(packet));
}

std::uint16_t RtspServer::listeningPort() const noexcept {
  return implementation_->port();
}

RtspServerStats RtspServer::stats() const noexcept {
  return implementation_->stats();
}

} // namespace darkos::protocols::rtsp
