#include <svc_board/BoardConfig.h>

#include <cJSON.h>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
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

bool readString(const cJSON *object, const char *name, std::string &output,
                std::string &error) {
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!cJSON_IsString(value) || value->valuestring == nullptr ||
      value->valuestring[0] == '\0') {
    error = std::string("'") + name + "' must be a non-empty string";
    return false;
  }
  output = value->valuestring;
  return true;
}

bool readOptionalBoolean(const cJSON *object, const char *name,
                         bool defaultValue, bool &output, std::string &error) {
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
  if (value == nullptr) {
    output = defaultValue;
    return true;
  }
  if (!cJSON_IsBool(value)) {
    error = std::string("'") + name + "' must be a boolean";
    return false;
  }
  output = cJSON_IsTrue(value);
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

bool checkUniqueResourceNames(const cJSON *object, const std::string &context,
                              std::string &error) {
  std::unordered_set<std::string> seen;
  const cJSON *member = nullptr;
  cJSON_ArrayForEach(member, object) {
    if (member->string == nullptr || !seen.emplace(member->string).second) {
      error = context + " contains a duplicate or unnamed resource";
      return false;
    }
  }
  return true;
}

bool parseElectrical(const std::string &text, SerialElectrical &output) {
  if (text == "ttl")
    output = SerialElectrical::kTtl;
  else if (text == "rs232")
    output = SerialElectrical::kRs232;
  else if (text == "rs485")
    output = SerialElectrical::kRs485;
  else
    return false;
  return true;
}

} // namespace

bool BoardSerialPort::supportsBaud(std::uint32_t baud) const {
  for (const std::uint32_t candidate : baudRates) {
    if (candidate == baud)
      return true;
  }
  return false;
}

const BoardSerialPort *
BoardConfig::findSerialPort(const std::string &id) const noexcept {
  const auto entry = serialPorts_.find(id);
  return entry == serialPorts_.end() ? nullptr : &entry->second;
}

bool BoardConfig::load(const std::string &path, BoardConfig &output,
                       std::string &error) {
  JsonDocument root(nullptr, cJSON_Delete);
  if (!loadDocument(path, root, error))
    return false;
  if (!cJSON_IsObject(root.get())) {
    error = "board configuration root must be an object";
    return false;
  }
  if (!checkMembers(
          root.get(),
          {"schema_version", "board", "compatible_soc", "serial_ports"},
          "board configuration", error))
    return false;

  BoardConfig parsed;
  if (!readString(root.get(), "schema_version", parsed.schemaVersion_, error) ||
      !readString(root.get(), "board", parsed.boardId_, error) ||
      !readString(root.get(), "compatible_soc", parsed.compatibleSoc_, error))
    return false;
  if (parsed.schemaVersion_ != "0.0.1") {
    error = "unsupported board schema_version '" + parsed.schemaVersion_ + "'";
    return false;
  }
  if (!validIdentifier(parsed.boardId_) ||
      !validIdentifier(parsed.compatibleSoc_)) {
    error = "'board' and 'compatible_soc' must be identifiers of at most 64 "
            "characters";
    return false;
  }

  const cJSON *ports =
      cJSON_GetObjectItemCaseSensitive(root.get(), "serial_ports");
  if (!cJSON_IsObject(ports)) {
    error = "'serial_ports' must be an object";
    return false;
  }
  if (!checkUniqueResourceNames(ports, "serial_ports", error))
    return false;

  const cJSON *entry = nullptr;
  cJSON_ArrayForEach(entry, ports) {
    const std::string id = entry->string != nullptr ? entry->string : "";
    const std::string context = "serial port '" + id + "'";
    if (!validIdentifier(id)) {
      error = context + " has an invalid resource identifier";
      return false;
    }
    if (!cJSON_IsObject(entry)) {
      error = context + " must be an object";
      return false;
    }
    if (!checkMembers(entry,
                      {"device", "electrical", "baud_rates",
                       "rs485_direction_gpio", "exclusive"},
                      context, error))
      return false;

    BoardSerialPort port;
    port.id = id;
    std::string electrical;
    if (!readString(entry, "device", port.device, error) ||
        !readString(entry, "electrical", electrical, error)) {
      error = context + ": " + error;
      return false;
    }
    if (!parseElectrical(electrical, port.electrical)) {
      error = context + ": 'electrical' must be ttl, rs232, or rs485";
      return false;
    }
    if (!readOptionalBoolean(entry, "exclusive", true, port.exclusive, error)) {
      error = context + ": " + error;
      return false;
    }

    const cJSON *baudRates =
        cJSON_GetObjectItemCaseSensitive(entry, "baud_rates");
    if (!cJSON_IsArray(baudRates) || cJSON_GetArraySize(baudRates) == 0) {
      error = context + ": 'baud_rates' must be a non-empty array";
      return false;
    }
    const cJSON *value = nullptr;
    cJSON_ArrayForEach(value, baudRates) {
      if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
          value->valuedouble <= 0 ||
          value->valuedouble > std::numeric_limits<std::uint32_t>::max() ||
          std::floor(value->valuedouble) != value->valuedouble) {
        error = context + ": baud rates must be positive unsigned integers";
        return false;
      }
      const auto baud = static_cast<std::uint32_t>(value->valuedouble);
      if (port.supportsBaud(baud)) {
        error = context + ": duplicate baud rate " + std::to_string(baud);
        return false;
      }
      port.baudRates.push_back(baud);
    }

    const cJSON *direction =
        cJSON_GetObjectItemCaseSensitive(entry, "rs485_direction_gpio");
    if (direction != nullptr) {
      if (!cJSON_IsString(direction) || direction->valuestring == nullptr ||
          !validIdentifier(direction->valuestring)) {
        error =
            context + ": 'rs485_direction_gpio' must be a resource identifier";
        return false;
      }
      port.rs485DirectionGpio = direction->valuestring;
    }
    if (port.electrical != SerialElectrical::kRs485 &&
        !port.rs485DirectionGpio.empty()) {
      error = context + ": rs485_direction_gpio is only valid for rs485";
      return false;
    }
    parsed.serialPorts_.emplace(port.id, std::move(port));
  }

  output = std::move(parsed);
  error.clear();
  return true;
}

const char *serialElectricalName(SerialElectrical electrical) noexcept {
  switch (electrical) {
  case SerialElectrical::kTtl:
    return "ttl";
  case SerialElectrical::kRs232:
    return "rs232";
  case SerialElectrical::kRs485:
    return "rs485";
  }
  return "unknown";
}

} // namespace darkos
