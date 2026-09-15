#include <svc_board/AppConfig.h>
#include <svc_board/BoardConfig.h>

#include <unistd.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    ++failures;
  }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    std::array<char, 64> pattern{};
    std::snprintf(pattern.data(), pattern.size(), "/tmp/svc_board_test.XXXXXX");
    if (char *created = ::mkdtemp(pattern.data()))
      path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  std::string write(const std::string &name, const std::string &content) const {
    const std::string path = path_ + "/" + name;
    std::ofstream file(path);
    file << content;
    return path;
  }

private:
  std::string path_;
};

const char *kBoardJson = R"json({
  "schema_version": "0.0.1",
  "board": "rv1126b_ipc_v1",
  "compatible_soc": "rockchip_rv1126b",
  "serial_ports": {
    "uart_ptz": {
      "device": "/dev/ttyS1",
      "electrical": "rs485",
      "baud_rates": [9600, 19200],
      "rs485_direction_gpio": "rs485_ptz_de"
    },
    "uart_control": {
      "device": "/dev/ttyS3",
      "electrical": "ttl",
      "baud_rates": [115200]
    }
  }
})json";

void testValidConfiguration(const TemporaryDirectory &temporary) {
  const std::string boardPath = temporary.write("board.json", kBoardJson);
  const std::string appPath = temporary.write("app.json", R"json({
      "schema_version": "0.0.1",
      "serial_bindings": {
        "ptz": {"resource": "uart_ptz", "baud": 9600},
        "device_control": {"resource": "uart_control", "baud": 115200}
      }
    })json");

  darkos::BoardConfig board;
  darkos::AppConfig app;
  std::string error;
  CHECK(darkos::BoardConfig::load(boardPath, board, error));
  CHECK(board.schemaVersion() == "0.0.1");
  CHECK(board.boardId() == "rv1126b_ipc_v1");
  CHECK(board.compatibleSoc() == "rockchip_rv1126b");
  CHECK(board.serialPorts().size() == 2);
  const auto *ptz = board.findSerialPort("uart_ptz");
  CHECK(ptz != nullptr);
  if (ptz != nullptr) {
    CHECK(ptz->device == "/dev/ttyS1");
    CHECK(ptz->electrical == darkos::SerialElectrical::kRs485);
    CHECK(ptz->supportsBaud(9600));
    CHECK(!ptz->supportsBaud(115200));
  }
  CHECK(darkos::AppConfig::load(appPath, app, error));
  CHECK(app.validate(board, error));
  CHECK(app.findSerialBinding("ptz") != nullptr);
}

void testSemanticFailures(const TemporaryDirectory &temporary) {
  darkos::BoardConfig board;
  darkos::AppConfig app;
  std::string error;
  CHECK(darkos::BoardConfig::load(temporary.write("board2.json", kBoardJson),
                                  board, error));

  const std::string missingResource = temporary.write("missing.json", R"json({
      "schema_version": "0.0.1",
      "serial_bindings": {"ptz": {"resource": "no_such_uart", "baud": 9600}}
    })json");
  CHECK(darkos::AppConfig::load(missingResource, app, error));
  CHECK(!app.validate(board, error));
  CHECK(error.find("unknown resource") != std::string::npos);

  const std::string wrongBaud = temporary.write("baud.json", R"json({
      "schema_version": "0.0.1",
      "serial_bindings": {"ptz": {"resource": "uart_ptz", "baud": 115200}}
    })json");
  CHECK(darkos::AppConfig::load(wrongBaud, app, error));
  CHECK(!app.validate(board, error));
  CHECK(error.find("unsupported baud") != std::string::npos);

  const std::string conflict = temporary.write("conflict.json", R"json({
      "schema_version": "0.0.1",
      "serial_bindings": {
        "ptz": {"resource": "uart_ptz", "baud": 9600},
        "backup_ptz": {"resource": "uart_ptz", "baud": 19200}
      }
    })json");
  CHECK(darkos::AppConfig::load(conflict, app, error));
  CHECK(!app.validate(board, error));
  CHECK(error.find("exclusive serial resource") != std::string::npos);
}

void testSyntaxAndSchemaFailures(const TemporaryDirectory &temporary) {
  darkos::BoardConfig board;
  std::string error;
  CHECK(!darkos::BoardConfig::load(temporary.write("broken.json", "{bad"),
                                   board, error));
  CHECK(error.find("line 1") != std::string::npos);

  const std::string unknown = temporary.write("unknown.json", R"json({
      "schema_version": "0.0.1",
      "board": "test",
      "compatible_soc": "test_soc",
      "serial_ports": {},
      "serial_port": {}
    })json");
  CHECK(!darkos::BoardConfig::load(unknown, board, error));
  CHECK(error.find("unknown member") != std::string::npos);

  const std::string numericVersion =
      temporary.write("numeric-version.json", R"json({
      "schema_version": 1,
      "board": "test",
      "compatible_soc": "test_soc",
      "serial_ports": {}
    })json");
  CHECK(!darkos::BoardConfig::load(numericVersion, board, error));
  CHECK(error.find("schema_version") != std::string::npos);

  const std::string duplicate = temporary.write("duplicate.json", R"json({
      "schema_version": "0.0.1",
      "board": "first",
      "board": "second",
      "compatible_soc": "test_soc",
      "serial_ports": {}
    })json");
  CHECK(!darkos::BoardConfig::load(duplicate, board, error));
  CHECK(error.find("duplicate member") != std::string::npos);
}

} // namespace

int main() {
  TemporaryDirectory temporary;
  testValidConfiguration(temporary);
  testSemanticFailures(temporary);
  testSyntaxAndSchemaFailures(temporary);

  if (failures != 0) {
    std::fprintf(stderr, "%d svc_board host test(s) failed\n", failures);
    return 1;
  }
  std::puts("svc_board host tests passed");
  return 0;
}
