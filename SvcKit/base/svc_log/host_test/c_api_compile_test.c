#include <svc_log.h>

/* 由 C++ 测试入口调用，同时验证公开头文件和宏可以用 C17 编译。 */
void svc_log_c_api_smoke(void) {
    SVC_LOGI("c_api", "message only");
    SVC_LOGW("c_api", "value=%d", 7);
}
