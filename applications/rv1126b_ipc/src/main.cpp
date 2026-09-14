#include <svc_log.h>

namespace {

constexpr char kTag[] = "rv1126b_ipc";

} // namespace

void initialize()
{
    SVC_LOGI(kTag, "initialize");
}

int main()
{
    SVC_LOGI(kTag, "application started");

    initialize();
    return 0;
}