#include <svc_log.h>

#include <cstdarg>
#include <cstdio>
#include <string>

extern "C" void svc_log_c_api_smoke(void);

namespace {

std::string g_output;
int g_failures = 0;

int captureOutput(const char *format, va_list args) {
    char buffer[2048];
    const int length = std::vsnprintf(buffer, sizeof(buffer), format, args);
    if (length > 0) {
        const auto size = static_cast<std::size_t>(length);
        g_output.append(buffer, size < sizeof(buffer) ? size : sizeof(buffer) - 1);
    }
    return length;
}

void check(bool condition, const char *expression, int line) {
    if (condition) {
        return;
    }

    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    ++g_failures;
}

#define CHECK(expression) check((expression), #expression, __LINE__)

void resetLog(svc_log_level_t level) {
    svc_log_level_set("*", level);
    g_output.clear();
}

void testDefaultLevel() {
    resetLog(SVC_LOG_INFO);

    CHECK(svc_log_get_default_level() == SVC_LOG_INFO);
    CHECK(svc_log_level_get("unknown") == SVC_LOG_INFO);
    CHECK(svc_log_is_enabled(SVC_LOG_ERROR, "unknown") != 0);
    CHECK(svc_log_is_enabled(SVC_LOG_INFO, "unknown") != 0);
    CHECK(svc_log_is_enabled(SVC_LOG_DEBUG, "unknown") == 0);
}

void testTagLevel() {
    resetLog(SVC_LOG_WARN);
    svc_log_level_set("network", SVC_LOG_DEBUG);

    CHECK(svc_log_level_get("network") == SVC_LOG_DEBUG);
    CHECK(svc_log_is_enabled(SVC_LOG_DEBUG, "network") != 0);
    CHECK(svc_log_is_enabled(SVC_LOG_INFO, "other") == 0);

    svc_log_level_set("*", SVC_LOG_ERROR);
    CHECK(svc_log_level_get("network") == SVC_LOG_ERROR);
}

void testFormattingAndFiltering() {
    resetLog(SVC_LOG_INFO);

    SVC_LOGD("format", "hidden");
    CHECK(g_output.empty());

    SVC_LOGI("format", "value=%d", 42);
    CHECK(g_output.size() >= 24);
    CHECK(g_output[4] == '-' && g_output[7] == '-' && g_output[10] == ' ');
    CHECK(g_output.find(" I format: value=42\n") != std::string::npos);

    g_output.clear();
    SVC_LOGE(nullptr, "failure");
    CHECK(g_output.find(" E SvcKit: failure\n") != std::string::npos);
}

void testCInterface() {
    resetLog(SVC_LOG_INFO);
    svc_log_c_api_smoke();

    CHECK(g_output.find(" I c_api: message only\n") != std::string::npos);
    CHECK(g_output.find(" W c_api: value=7\n") != std::string::npos);
}

void testTimestamp() {
    const uint64_t first = svc_log_timestamp();
    const uint64_t second = svc_log_timestamp();
    CHECK(second >= first);
}

} // namespace

int main() {
    const auto oldOutput = svc_log_set_vprintf(captureOutput);

    testDefaultLevel();
    testTagLevel();
    testFormattingAndFiltering();
    testCInterface();
    testTimestamp();

    svc_log_set_vprintf(oldOutput);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d svc_log host test(s) failed\n", g_failures);
        return 1;
    }

    std::puts("svc_log host tests passed");
    return 0;
}
