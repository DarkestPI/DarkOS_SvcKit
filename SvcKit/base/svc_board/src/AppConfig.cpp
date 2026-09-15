#include <svc_board/AppConfig.h>

#include <svc_board/BoardConfig.h>

#include <cJSON.h>

#include <cmath>
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
  if (!checkMembers(root.get(), {"schema_version", "serial_bindings"},
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
