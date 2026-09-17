#include <RtspServer.h>

#include <base/Hash.h>
#include <media_buffer.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <memory>
#include <sstream>
#include <set>
#include <string>
#include <thread>

namespace {

int connectTo(std::uint16_t port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  timeval timeout{3, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

bool sendAll(int fd, const std::string &request) {
  std::size_t offset = 0;
  while (offset < request.size()) {
    const auto count = send(fd, request.data() + offset,
                            request.size() - offset, MSG_NOSIGNAL);
    if (count <= 0)
      return false;
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

std::string response(int fd) {
  std::string output;
  char bytes[4096];
  while (output.find("\r\n\r\n") == std::string::npos) {
    const auto count = recv(fd, bytes, sizeof(bytes), 0);
    if (count <= 0)
      return output;
    output.append(bytes, static_cast<std::size_t>(count));
  }
  const auto headerEnd = output.find("\r\n\r\n");
  std::size_t contentLength = 0;
  const auto length = output.find("Content-Length:");
  if (length != std::string::npos && length < headerEnd)
    contentLength = std::strtoul(output.c_str() + length + 15, nullptr, 10);
  while (output.size() < headerEnd + 4 + contentLength) {
    const auto count = recv(fd, bytes, sizeof(bytes), 0);
    if (count <= 0)
      break;
    output.append(bytes, static_cast<std::size_t>(count));
  }
  return output;
}

std::string headerValue(const std::string &message, const std::string &name) {
  const auto position = message.find(name + ":");
  if (position == std::string::npos)
    return {};
  const auto begin = message.find_first_not_of(' ', position + name.size() + 1);
  const auto end = message.find("\r\n", begin);
  return message.substr(begin, end - begin);
}

std::string quotedValue(const std::string &value, const std::string &name) {
  const auto position = value.find(name + "=\"");
  if (position == std::string::npos)
    return {};
  const auto begin = position + name.size() + 2;
  const auto end = value.find('"', begin);
  return value.substr(begin, end - begin);
}

std::string sessionOf(const std::string &message) {
  const std::string value = headerValue(message, "Session");
  return value.substr(0, value.find(';'));
}

std::string digestHeader(const std::string &method, const std::string &uri,
                         const std::string &nonce) {
  static unsigned nonceCount = 0;
  std::ostringstream count;
  count << std::hex << std::setw(8) << std::setfill('0') << ++nonceCount;
  const std::string nc = count.str();
  const std::string cnonce = "darkos-test";
  const std::string ha1 = darkos::md5Hex("admin:DarkOS:secret");
  const std::string ha2 = darkos::md5Hex(method + ":" + uri);
  const std::string digest = darkos::md5Hex(
      ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":auth:" + ha2);
  return "Authorization: Digest username=\"admin\", realm=\"DarkOS\", "
         "nonce=\"" + nonce + "\", uri=\"" + uri +
         "\", response=\"" + digest + "\", qop=auth, nc=" + nc +
         ", cnonce=\"" + cnonce + "\"\r\n";
}

bool collectInterleaved(int fd) {
  std::string pending;
  std::set<unsigned> rtpChannels;
  std::set<unsigned> rtcpChannels;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    char bytes[4096];
    const auto count = recv(fd, bytes, sizeof(bytes), 0);
    if (count <= 0)
      return false;
    pending.append(bytes, static_cast<std::size_t>(count));
    while (pending.size() >= 4 && pending.front() == '$') {
      const std::size_t size =
          (static_cast<unsigned char>(pending[2]) << 8U) |
          static_cast<unsigned char>(pending[3]);
      if (pending.size() < size + 4)
        break;
      const unsigned channel = static_cast<unsigned char>(pending[1]);
      if (size >= 2) {
        const unsigned type = static_cast<unsigned char>(pending[5]);
        if ((channel == 0 || channel == 2) && (type & 0x7fU) >= 96)
          rtpChannels.insert(channel);
        if ((channel == 1 || channel == 3) && type == 200)
          rtcpChannels.insert(channel);
      }
      pending.erase(0, size + 4);
    }
    if (rtpChannels.size() == 2 && rtcpChannels.size() == 2)
      return true;
  }
  return false;
}

} // namespace

int main() {
  std::unique_ptr<darkos::EventLoop> loop(darkos::EventLoop::create());
  if (!loop)
    return 1;
  darkos::protocols::rtsp::RtspServerOptions options;
  options.bindAddress = "127.0.0.1";
  options.port = 0;
  options.username = "admin";
  options.password = "secret";
  options.sessionTimeoutSeconds = 1;
  options.rtcpReportIntervalMs = 100;
  options.enableMulticast = true;
  options.multicastAddress = "239.255.0.77";
  std::string error;
  auto server = darkos::protocols::rtsp::RtspServer::create(*loop, options, error);
  if (!server || server->start() != 0)
    return 1;
  std::thread worker([&] { loop->run(); });
  const int fd = connectTo(server->listeningPort());
  if (fd < 0) {
    loop->quit();
    worker.join();
    return 1;
  }

  const std::string url = "rtsp://127.0.0.1:" +
                          std::to_string(server->listeningPort()) + "/live";
  bool valid = sendAll(fd, "DESCRIBE " + url +
                               " RTSP/1.0\r\nCSeq: 1\r\n\r\n");
  std::string value = response(fd);
  const std::string challenge = headerValue(value, "WWW-Authenticate");
  const std::string nonce = quotedValue(challenge, "nonce");
  valid = valid && value.find("401 Unauthorized") != std::string::npos &&
          !nonce.empty();

  const std::string describeAuthorization =
      digestHeader("DESCRIBE", url, nonce);
  valid = valid && sendAll(fd, "DESCRIBE " + url +
                                   " RTSP/1.0\r\nCSeq: 2\r\n" +
                                   describeAuthorization + "\r\n");
  value = response(fd);
  valid = valid && value.find("200 OK") != std::string::npos &&
          value.find("a=rtpmap:97 PCMA/16000/1") != std::string::npos &&
          value.find("trackID=1") != std::string::npos;
  valid = valid && sendAll(fd, "DESCRIBE " + url +
                                   " RTSP/1.0\r\nCSeq: 21\r\n" +
                                   describeAuthorization + "\r\n");
  value = response(fd);
  valid = valid && value.find("401 Unauthorized") != std::string::npos;

  const std::string videoUrl = url + "/trackID=0";
  valid = valid && sendAll(fd, "SETUP " + videoUrl +
                                   " RTSP/1.0\r\nCSeq: 3\r\n" +
                                   digestHeader("SETUP", videoUrl, nonce) +
                                   "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
  value = response(fd);
  const std::string session = sessionOf(value);
  valid = valid && value.find("200 OK") != std::string::npos && !session.empty();

  const std::string audioUrl = url + "/trackID=1";
  valid = valid && sendAll(fd, "SETUP " + audioUrl +
                                   " RTSP/1.0\r\nCSeq: 4\r\n" +
                                   digestHeader("SETUP", audioUrl, nonce) +
                                   "Session: " + session + "\r\n"
                                   "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n\r\n");
  value = response(fd);
  valid = valid && value.find("200 OK") != std::string::npos;

  valid = valid && sendAll(fd, "PLAY " + url +
                                   " RTSP/1.0\r\nCSeq: 5\r\n" +
                                   digestHeader("PLAY", url, nonce) +
                                   "Session: " + session + "\r\n\r\n");
  value = response(fd);
  valid = valid && value.find("200 OK") != std::string::npos;

  static const std::uint8_t frame[] = {
      0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x1f,
      0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80,
      0, 0, 0, 1, 0x65, 0x88, 0x84, 0x21};
  auto video = std::make_shared<darkos::media::VideoPacket>();
  video->buffer = darkos::media::copyMediaBuffer(frame, sizeof(frame));
  video->timestampNs = 1'000'000'000ULL;
  video->codec = darkos::media::VideoCodec::H264;
  video->keyframe = true;
  std::uint8_t audioBytes[320]{};
  auto audio = std::make_shared<darkos::media::AudioPacket>();
  audio->buffer = darkos::media::copyMediaBuffer(audioBytes, sizeof(audioBytes));
  audio->timestampNs = 1'000'000'000ULL;
  audio->codec = darkos::media::AudioCodec::G711A;
  audio->sampleRate = 16000;
  audio->channelCount = 1;
  valid = valid && server->consume(video) == 0 && server->consume(audio) == 0 &&
          collectInterleaved(fd);

  std::this_thread::sleep_for(std::chrono::milliseconds(1300));
  valid = valid && sendAll(fd, "GET_PARAMETER " + url +
                                   " RTSP/1.0\r\nCSeq: 6\r\n" +
                                   digestHeader("GET_PARAMETER", url, nonce) +
                                   "Session: " + session + "\r\n\r\n");
  value = response(fd);
  valid = valid && value.find("454 Session Not Found") != std::string::npos;

  valid = valid && sendAll(fd, "SETUP " + videoUrl +
                                   " RTSP/1.0\r\nCSeq: 7\r\n" +
                                   digestHeader("SETUP", videoUrl, nonce) +
                                   "Transport: RTP/AVP;multicast\r\n\r\n");
  value = response(fd);
  valid = valid && value.find("200 OK") != std::string::npos &&
          value.find("destination=239.255.0.77") != std::string::npos &&
          value.find("port=5004-5005") != std::string::npos;

  close(fd);
  loop->quit();
  worker.join();
  server->stop();
  const auto stats = server->stats();
  return valid && stats.authenticationFailures == 2 &&
                 stats.audioPackets >= 1 && stats.rtpPackets >= 2 &&
                 stats.rtcpReports >= 2 && stats.expiredSessions >= 1
             ? 0
             : 1;
}
