#include <svc_board/AppConfig.h>

#include <svc_board/BoardConfig.h>

#include <cJSON.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace darkos {

namespace {

constexpr std::size_t kMaximumConfigSize = 1024U * 1024U;
using JsonDocument = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

bool loadDocument(const std::string &path, JsonDocument &document,
                  std::string &error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "cannot open '" + path + "'";
    return false;
  }
  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  if (size < 0 || static_cast<std::uint64_t>(size) > kMaximumConfigSize) {
    error = "configuration file is unreadable or larger than 1 MiB";
    return false;
  }
  file.seekg(0, std::ios::beg);
  std::string content{std::istreambuf_iterator<char>(file),
                      std::istreambuf_iterator<char>()};
  if (!file.good() && !file.eof()) {
    error = "failed while reading '" + path + "'";
    return false;
  }

  const char *parseEnd = nullptr;
  cJSON *root = cJSON_ParseWithLengthOpts(content.c_str(), content.size() + 1,
                                          &parseEnd, true);
  if (root == nullptr) {
    const std::size_t offset =
        parseEnd != nullptr
            ? static_cast<std::size_t>(parseEnd - content.c_str())
            : 0;
    std::size_t line = 1;
    std::size_t column = 1;
    for (std::size_t index = 0; index < offset && index < content.size();
         ++index) {
      if (content[index] == '\n') {
        ++line;
        column = 1;
      } else {
        ++column;
      }
    }
    error = path + ": invalid JSON at line " + std::to_string(line) +
            ", column " + std::to_string(column);
    return false;
  }
  document.reset(root);
  return true;
}

bool validIdentifier(const std::string &value) {
  if (value.empty() || value.size() > 64)
    return false;
  for (const unsigned char character : value) {
    const bool valid = (character >= 'a' && character <= 'z') ||
                       (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9') ||
                       character == '_' || character == '-' || character == '.';
    if (!valid)
      return false;
  }
  return true;
}

bool readUint32(const cJSON *object, const char *name, std::uint32_t &output,
                std::string &error) {
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
      value->valuedouble < 0 ||
      value->valuedouble > std::numeric_limits<std::uint32_t>::max() ||
      std::floor(value->valuedouble) != value->valuedouble) {
    error = std::string("'") + name + "' must be an unsigned 32-bit integer";
    return false;
  }
  output = static_cast<std::uint32_t>(value->valuedouble);
  return true;
}

bool readOptionalUint32(const cJSON *object, const char *name,
                        std::uint32_t &output, std::string &error) {
  if (cJSON_GetObjectItemCaseSensitive(object, name) == nullptr)
    return true;
  return readUint32(object, name, output, error);
}

bool readOptionalBool(const cJSON *object, const char *name, bool &output,
                      std::string &error) {
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
  if (value == nullptr)
    return true;
  if (!cJSON_IsBool(value)) {
    error = std::string("'") + name + "' must be a boolean";
    return false;
  }
  output = cJSON_IsTrue(value);
  return true;
}

bool readOptionalString(const cJSON *object, const char *name,
                        std::string &output, std::string &error) {
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
  if (value == nullptr)
    return true;
  if (!cJSON_IsString(value) || value->valuestring == nullptr) {
    error = std::string("'") + name + "' must be a string";
    return false;
  }
  output = value->valuestring;
  return true;
}

bool checkMembers(const cJSON *object,
                  const std::unordered_set<std::string> &allowed,
                  const std::string &context, std::string &error);

bool validEnvironmentName(const std::string &value) {
  if (value.empty() || value.size() > 128 ||
      !(value.front() == '_' ||
        (value.front() >= 'A' && value.front() <= 'Z')))
    return false;
  return std::all_of(value.begin() + 1, value.end(), [](unsigned char c) {
    return c == '_' || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
  });
}

bool validIpv4Multicast(const std::string &value) {
  const auto dot = value.find('.');
  if (dot == std::string::npos)
    return false;
  char *end = nullptr;
  const long first = std::strtol(value.substr(0, dot).c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || first < 224 || first > 239)
    return false;
  unsigned octets[4]{};
  char trailing = 0;
  return std::sscanf(value.c_str(), "%u.%u.%u.%u%c", &octets[0], &octets[1],
                     &octets[2], &octets[3], &trailing) == 4 &&
         std::all_of(std::begin(octets), std::end(octets),
                     [](unsigned octet) { return octet <= 255; });
}

bool parseRtsp(const cJSON *object, RtspAppConfig &config,
               std::string &error) {
  if (!cJSON_IsObject(object)) {
    error = "'rtsp' must be an object";
    return false;
  }
  if (!checkMembers(object,
                    {"enabled", "bind_address", "port", "mount_path",
                     "session_timeout_seconds", "rtcp_report_interval_ms",
                     "maximum_rtp_payload_bytes",
                     "maximum_client_backlog_bytes", "authentication",
                     "multicast"},
                    "rtsp configuration", error))
    return false;
  std::uint32_t port = config.port;
  std::uint32_t payloadBytes = config.maximumRtpPayloadBytes;
  std::uint32_t backlogBytes = config.maximumClientBacklogBytes;
  if (!readOptionalBool(object, "enabled", config.enabled, error) ||
      !readOptionalString(object, "bind_address", config.bindAddress, error) ||
      !readOptionalUint32(object, "port", port, error) ||
      !readOptionalString(object, "mount_path", config.mountPath, error) ||
      !readOptionalUint32(object, "session_timeout_seconds",
                          config.sessionTimeoutSeconds, error) ||
      !readOptionalUint32(object, "rtcp_report_interval_ms",
                          config.rtcpReportIntervalMs, error) ||
      !readOptionalUint32(object, "maximum_rtp_payload_bytes", payloadBytes,
                          error) ||
      !readOptionalUint32(object, "maximum_client_backlog_bytes", backlogBytes,
                          error))
    return false;
  if (port == 0 || port > 65535 || config.bindAddress.empty() ||
      config.mountPath.empty() || config.mountPath.find('/') != std::string::npos ||
      payloadBytes < 3 || backlogBytes == 0) {
    error = "rtsp configuration contains an invalid address, port, mount path, or buffer size";
    return false;
  }
  config.port = static_cast<std::uint16_t>(port);
  config.maximumRtpPayloadBytes = payloadBytes;
  config.maximumClientBacklogBytes = backlogBytes;

  const cJSON *authentication =
      cJSON_GetObjectItemCaseSensitive(object, "authentication");
  if (authentication != nullptr) {
    if (!cJSON_IsObject(authentication) ||
        !checkMembers(authentication, {"username", "password", "password_env"},
                      "rtsp authentication", error) ||
        !readOptionalString(authentication, "username",
                            config.authentication.username, error) ||
        !readOptionalString(authentication, "password",
                            config.authentication.password, error) ||
        !readOptionalString(authentication, "password_env",
                            config.authentication.passwordEnvironment, error))
      return false;
  }
  if (!config.authentication.username.empty() &&
      config.authentication.password.empty() &&
      !validEnvironmentName(config.authentication.passwordEnvironment)) {
    error = "rtsp authentication requires 'password' or a valid 'password_env'";
    return false;
  }

  const cJSON *multicast = cJSON_GetObjectItemCaseSensitive(object, "multicast");
  if (multicast != nullptr) {
    if (!cJSON_IsObject(multicast) ||
        !checkMembers(multicast,
                      {"enabled", "address", "video_port", "audio_port", "ttl"},
                      "rtsp multicast", error))
      return false;
    std::uint32_t videoPort = config.multicast.videoPort;
    std::uint32_t audioPort = config.multicast.audioPort;
    std::uint32_t ttl = config.multicast.ttl;
    if (!readOptionalBool(multicast, "enabled", config.multicast.enabled, error) ||
        !readOptionalString(multicast, "address", config.multicast.address, error) ||
        !readOptionalUint32(multicast, "video_port", videoPort, error) ||
        !readOptionalUint32(multicast, "audio_port", audioPort, error) ||
        !readOptionalUint32(multicast, "ttl", ttl, error))
      return false;
    if (videoPort == 0 || videoPort >= 65535 || audioPort == 0 ||
        audioPort >= 65535 || ttl == 0 || ttl > 255 ||
        (config.multicast.enabled && !validIpv4Multicast(config.multicast.address))) {
      error = "rtsp multicast contains an invalid address, port, or ttl";
      return false;
    }
    config.multicast.videoPort = static_cast<std::uint16_t>(videoPort);
    config.multicast.audioPort = static_cast<std::uint16_t>(audioPort);
    config.multicast.ttl = static_cast<std::uint8_t>(ttl);
  }
  return true;
}

bool parseIva(const cJSON *object, IvaAppConfig &config,
              std::string &error) {
  if (!cJSON_IsObject(object)) {
    error = "'iva' must be an object";
    return false;
  }
  if (!checkMembers(object, {"enabled", "model_path"}, "iva configuration",
                    error))
    return false;
  if (!readOptionalBool(object, "enabled", config.enabled, error) ||
      !readOptionalString(object, "model_path", config.modelPath, error))
    return false;
  if (config.enabled && config.modelPath.empty()) {
    error = "iva configuration requires a non-empty 'model_path' when enabled";
    return false;
  }
  return true;
}

bool checkMembers(const cJSON *object,
                  const std::unordered_set<std::string> &allowed,
                  const std::string &context, std::string &error) {
  std::unordered_set<std::string> seen;
  const cJSON *member = nullptr;
  cJSON_ArrayForEach(member, object) {
    if (member->string == nullptr) {
      error = context + " contains an unnamed member";
      return false;
    }
    if (!seen.emplace(member->string).second) {
      error = context + " contains duplicate member '" + member->string + "'";
      return false;
    }
    if (allowed.count(member->string) == 0) {
      error = context + " contains unknown member '" + member->string + "'";
      return false;
    }
  }
  return true;
}

bool checkUniqueBindingNames(const cJSON *object, std::string &error) {
  std::unordered_set<std::string> seen;
  const cJSON *member = nullptr;
  cJSON_ArrayForEach(member, object) {
    if (member->string == nullptr || !seen.emplace(member->string).second) {
      error = "serial_bindings contains a duplicate or unnamed service";
      return false;
    }
  }
  return true;
}

} // namespace

const SerialBinding *
AppConfig::findSerialBinding(const std::string &service) const noexcept {
  const auto entry = serialBindings_.find(service);
  return entry == serialBindings_.end() ? nullptr : &entry->second;
}

bool AppConfig::load(const std::string &path, AppConfig &output,
                     std::string &error) {
  JsonDocument root(nullptr, cJSON_Delete);
  if (!loadDocument(path, root, error))
    return false;
  if (!cJSON_IsObject(root.get())) {
    error = "application configuration root must be an object";
    return false;
  }
  if (!checkMembers(root.get(),
                    {"schema_version", "serial_bindings", "rtsp", "iva"},
                    "application configuration", error))
    return false;

  AppConfig parsed;
  const cJSON *schemaVersion =
      cJSON_GetObjectItemCaseSensitive(root.get(), "schema_version");
  if (!cJSON_IsString(schemaVersion) || schemaVersion->valuestring == nullptr ||
      schemaVersion->valuestring[0] == '\0') {
    error = "'schema_version' must be a non-empty string";
    return false;
  }
  parsed.schemaVersion_ = schemaVersion->valuestring;
  if (parsed.schemaVersion_ != "0.0.1") {
    error = "unsupported application schema_version '" + parsed.schemaVersion_ +
            "'";
    return false;
  }

  const cJSON *bindings =
      cJSON_GetObjectItemCaseSensitive(root.get(), "serial_bindings");
  if (!cJSON_IsObject(bindings)) {
    error = "'serial_bindings' must be an object";
    return false;
  }
  if (!checkUniqueBindingNames(bindings, error))
    return false;

  const cJSON *entry = nullptr;
  cJSON_ArrayForEach(entry, bindings) {
    const std::string service = entry->string != nullptr ? entry->string : "";
    const std::string context = "serial binding '" + service + "'";
    if (!validIdentifier(service)) {
      error = context + " has an invalid service identifier";
      return false;
    }
    if (!cJSON_IsObject(entry)) {
      error = context + " must be an object";
      return false;
    }
    if (!checkMembers(entry, {"resource", "baud"}, context, error))
      return false;

    const cJSON *resource = cJSON_GetObjectItemCaseSensitive(entry, "resource");
    SerialBinding binding;
    binding.service = service;
    if (!cJSON_IsString(resource) || resource->valuestring == nullptr ||
        !validIdentifier(resource->valuestring)) {
      error = context + ": 'resource' must be a resource identifier";
      return false;
    }
    binding.resource = resource->valuestring;
    if (!readUint32(entry, "baud", binding.baud, error) || binding.baud == 0) {
      error = context + ": 'baud' must be a positive unsigned integer";
      return false;
    }
    parsed.serialBindings_.emplace(binding.service, std::move(binding));
  }

  const cJSON *rtsp = cJSON_GetObjectItemCaseSensitive(root.get(), "rtsp");
  if (rtsp != nullptr && !parseRtsp(rtsp, parsed.rtsp_, error))
    return false;

  const cJSON *iva = cJSON_GetObjectItemCaseSensitive(root.get(), "iva");
  if (iva != nullptr && !parseIva(iva, parsed.iva_, error))
    return false;

  output = std::move(parsed);
  error.clear();
  return true;
}

bool AppConfig::validate(const BoardConfig &board, std::string &error) const {
  std::unordered_map<std::string, std::string> exclusiveOwners;
  for (const auto &entry : serialBindings_) {
    const SerialBinding &binding = entry.second;
    const BoardSerialPort *port = board.findSerialPort(binding.resource);
    if (port == nullptr) {
      error = "serial binding '" + binding.service +
              "' references unknown resource '" + binding.resource + "'";
      return false;
    }
    if (!port->supportsBaud(binding.baud)) {
      error = "serial binding '" + binding.service +
              "' requests unsupported baud " + std::to_string(binding.baud) +
              " on resource '" + binding.resource + "'";
      return false;
    }
    if (port->exclusive) {
      const auto owner = exclusiveOwners.emplace(port->id, binding.service);
      if (!owner.second) {
        error = "exclusive serial resource '" + port->id +
                "' is requested by both '" + owner.first->second + "' and '" +
                binding.service + "'";
        return false;
      }
    }
  }
  error.clear();
  return true;
}

} // namespace darkos
