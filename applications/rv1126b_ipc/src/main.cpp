#include "base/Log.h"

namespace {

constexpr char kTag[] = "rv1126b_ipc";

} // namespace

void initialize()
{
    LOGI(kTag, "initialize");
}

int main()
{
    LOGI(kTag, "application started");

    initialize();
    return 0;
}