#include <RtspServer.h>

#include <media_buffer.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <memory>
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

std::string sessionOf(const std::string &value) {
  const auto position = value.find("Session:");
  if (position == std::string::npos)
    return {};
  const auto begin = value.find_first_not_of(' ', position + 8);
  const auto end = value.find_first_of(";\r\n", begin);
  return value.substr(begin, end - begin);
}

} // namespace

int main() {
  std::unique_ptr<darkos::EventLoop> loop(darkos::EventLoop::create());
  if (!loop)
    return 1;
  darkos::protocols::rtsp::RtspServerOptions options;
  options.bindAddress = "127.0.0.1";
  options.port = 0;
  std::string error;
  auto server = darkos::protocols::rtsp::RtspServer::create(*loop, options, error);
  if (!server || server->start() != 0 || server->listeningPort() == 0)
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
  bool valid = sendAll(fd, "OPTIONS " + url +
                               " RTSP/1.0\r\nCSeq: 1\r\n\r\n");
  auto value = response(fd);
  valid = valid && value.find("200 OK") != std::string::npos &&
          value.find("DESCRIBE") != std::string::npos;
  valid = valid && sendAll(fd, "DESCRIBE " + url +
                                   " RTSP/1.0\r\nCSeq: 2\r\n"
                                   "Accept: application/sdp\r\n\r\n");
  value = response(fd);
  valid = valid && value.find("a=rtpmap:96 H264/90000") != std::string::npos &&
          value.find("a=control:trackID=0") != std::string::npos;
  valid = valid && sendAll(fd, "SETUP " + url +
                                   "/trackID=0 RTSP/1.0\r\nCSeq: 3\r\n"
                                   "Transport: RTP/AVP/TCP;unicast;"
                                   "interleaved=0-1\r\n\r\n");
  value = response(fd);
  const std::string session = sessionOf(value);
  valid = valid && value.find("200 OK") != std::string::npos && !session.empty();
  valid = valid && sendAll(fd, "PLAY " + url +
                                   " RTSP/1.0\r\nCSeq: 4\r\nSession: " +
                                   session + "\r\n\r\n");
  value = response(fd);
  valid = valid && value.find("200 OK") != std::string::npos &&
          value.find("RTP-Info:") != std::string::npos;

  static const std::uint8_t frame[] = {
      0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x1f,
      0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80,
      0, 0, 0, 1, 0x65, 0x88, 0x84, 0x21};
  auto packet = std::make_shared<darkos::media::VideoPacket>();
  packet->buffer = darkos::media::copyMediaBuffer(frame, sizeof(frame));
  packet->timestampNs = 1'000'000'000ULL;
  packet->codec = darkos::media::VideoCodec::H264;
  packet->keyframe = true;
  valid = valid && server->consume(packet) == 0;
  char interleaved[512]{};
  const auto received = recv(fd, interleaved, sizeof(interleaved), 0);
  valid = valid && received > 16 && interleaved[0] == '$' &&
          static_cast<unsigned char>(interleaved[4]) == 0x80;

  sendAll(fd, "TEARDOWN " + url +
                  " RTSP/1.0\r\nCSeq: 5\r\nSession: " + session + "\r\n\r\n");
  value = response(fd);
  valid = valid && value.find("200 OK") != std::string::npos;
  close(fd);
  loop->quit();
  worker.join();
  server->stop();
  const auto stats = server->stats();
  return valid && stats.requests >= 5 && stats.rtpPackets >= 1 ? 0 : 1;
}
