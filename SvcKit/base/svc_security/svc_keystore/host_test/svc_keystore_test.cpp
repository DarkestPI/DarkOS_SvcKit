#include <svc_keystore.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
        std::snprintf(pattern.data(), pattern.size(), "/tmp/svc_keystore_test.XXXXXX");
        char *created = ::mkdtemp(pattern.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::string &path() const {
        return path_;
    }

  private:
    std::string path_;
};

void testRoundTripAndPersistence(const std::string &directory) {
    constexpr char secret[] = "cloud-token-123";
    svc_keystore_t *keystore = nullptr;
    CHECK(svc_keystore_open(directory.c_str(), &keystore) == SVC_KEYSTORE_OK);
    CHECK(keystore != nullptr);
    CHECK(svc_keystore_store_secret(keystore, "cloud.token", secret, sizeof(secret) - 1) == SVC_KEYSTORE_OK);
    CHECK(svc_keystore_contains(keystore, "cloud.token") == 1);

    size_t size = 0;
    CHECK(svc_keystore_load_secret(keystore, "cloud.token", nullptr, &size) == SVC_KEYSTORE_BUFFER_TOO_SMALL);
    CHECK(size == sizeof(secret) - 1);

    std::vector<char> output(size);
    CHECK(svc_keystore_load_secret(keystore, "cloud.token", output.data(), &size) == SVC_KEYSTORE_OK);
    CHECK(size == sizeof(secret) - 1);
    CHECK(std::memcmp(output.data(), secret, size) == 0);
    svc_keystore_close(keystore);

    keystore = nullptr;
    CHECK(svc_keystore_open(directory.c_str(), &keystore) == SVC_KEYSTORE_OK);
    output.assign(sizeof(secret) - 1, 0);
    size = output.size();
    CHECK(svc_keystore_load_secret(keystore, "cloud.token", output.data(), &size) == SVC_KEYSTORE_OK);
    CHECK(std::memcmp(output.data(), secret, size) == 0);
    CHECK(svc_keystore_remove(keystore, "cloud.token") == SVC_KEYSTORE_OK);
    CHECK(svc_keystore_contains(keystore, "cloud.token") == 0);
    CHECK(svc_keystore_remove(keystore, "cloud.token") == SVC_KEYSTORE_NOT_FOUND);
    svc_keystore_close(keystore);
}

void testPermissionsAndValidation(const std::string &directory) {
    svc_keystore_t *keystore = nullptr;
    CHECK(svc_keystore_open(directory.c_str(), &keystore) == SVC_KEYSTORE_OK);
    CHECK(svc_keystore_store_secret(keystore, "../escape", "x", 1) == SVC_KEYSTORE_INVALID_ARGUMENT);

    struct stat status {};
    CHECK(::stat((directory + "/master.key").c_str(), &status) == 0);
    CHECK((status.st_mode & 0777) == 0600);
    CHECK(::stat(directory.c_str(), &status) == 0);
    CHECK((status.st_mode & 0777) == 0700);
    svc_keystore_close(keystore);
}

void testTamperDetection(const std::string &directory) {
    svc_keystore_t *keystore = nullptr;
    CHECK(svc_keystore_open(directory.c_str(), &keystore) == SVC_KEYSTORE_OK);
    CHECK(svc_keystore_store_secret(keystore, "wifi.password", "12345678", 8) == SVC_KEYSTORE_OK);

    const std::string path = directory + "/entries/wifi.password.secret";
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    file.seekg(-1, std::ios::end);
    char value = 0;
    file.read(&value, 1);
    value ^= 1;
    file.seekp(-1, std::ios::end);
    file.write(&value, 1);
    file.close();

    std::array<char, 16> output{};
    size_t size = output.size();
    CHECK(svc_keystore_load_secret(keystore, "wifi.password", output.data(), &size) == SVC_KEYSTORE_AUTH_FAILED);
    svc_keystore_close(keystore);
}

} // namespace

int main() {
    TemporaryDirectory temporary;
    CHECK(!temporary.path().empty());
    if (temporary.path().empty()) {
        return 1;
    }

    testRoundTripAndPersistence(temporary.path() + "/roundtrip");
    testPermissionsAndValidation(temporary.path() + "/permissions");
    testTamperDetection(temporary.path() + "/tamper");

    if (failures != 0) {
        std::fprintf(stderr, "%d svc_keystore host test(s) failed\n", failures);
        return 1;
    }
    std::puts("svc_keystore host tests passed");
    return 0;
}
