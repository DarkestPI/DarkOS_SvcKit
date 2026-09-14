# svc_log

`svc_log` 是 SvcKit 的基础日志组件，使用
`SVC_LOGE`、`SVC_LOGW`、`SVC_LOGI`、`SVC_LOGD` 和 `SVC_LOGV` 输出日志。

组件仅依赖 C++ 标准库和系统线程库，可用于 Linux 宿主机以及 DarkOS 的 Linux
交叉编译目标。公开头文件兼容 C17 和 C++17。

## 目录结构

```text
svc_log/
├── CMakeLists.txt
├── inc/
│   └── svc_log.h
├── src/
│   └── svc_log.cpp
└── host_test/
    ├── CMakeLists.txt
    ├── c_api_compile_test.c
    └── svc_log_test.cpp
```

## CMake 接入

应用链接 `DarkOS::SvcKit` 或 `DarkOS::Base` 后，会自动获得 `svc_log`：

```cmake
target_link_libraries(my_app PRIVATE DarkOS::SvcKit)
```

只需要日志组件时，可以直接链接：

```cmake
target_link_libraries(my_app PRIVATE DarkOS::Log)
```

组件实际 CMake 目标名为 `svc_log`，生成静态库 `libsvc_log.a`。

## 基本使用

```c
#include <svc_log.h>

static const char *TAG = "camera";

void camera_start(int channel) {
    SVC_LOGI(TAG, "启动通道：%d", channel);
    SVC_LOGW(TAG, "这是警告日志");
    SVC_LOGE(TAG, "启动失败，错误码：%d", -1);
}
```

没有额外格式化参数时可以直接传入字符串：

```c
SVC_LOGI(TAG, "服务启动完成");
```

默认输出到 `stderr`，格式如下：

```text
I (12) camera: 启动通道：0
```

其中：

- `I` 是日志等级；
- `12` 是程序启动后的毫秒数；
- `camera` 是日志 tag；
- 最后部分是格式化后的日志内容。

单条日志的格式化缓冲区为 1024 字节，超出部分会被截断。

## 日志等级

```c
typedef enum {
    SVC_LOG_NONE,
    SVC_LOG_ERROR,
    SVC_LOG_WARN,
    SVC_LOG_INFO,
    SVC_LOG_DEBUG,
    SVC_LOG_VERBOSE,
} svc_log_level_t;
```

日志等级越大，输出越详细。默认等级是 `SVC_LOG_INFO`，因此默认输出 Error、
Warn 和 Info，不输出 Debug 与 Verbose。

修改默认日志等级：

```c
svc_log_set_default_level(SVC_LOG_DEBUG);

svc_log_level_t level = svc_log_get_default_level();
```

关闭所有默认日志：

```c
svc_log_set_default_level(SVC_LOG_NONE);
```

## 按 tag 控制等级

可以只为指定模块开启详细日志：

```c
svc_log_set_default_level(SVC_LOG_WARN);
svc_log_level_set("camera", SVC_LOG_DEBUG);

SVC_LOGD("camera", "这条日志会输出");
SVC_LOGI("network", "这条日志不会输出");
```

查询 tag 当前等级：

```c
svc_log_level_t level = svc_log_level_get("camera");
```

使用 `"*"` 修改默认等级时，会同时清除所有 tag 的单独配置：

```c
svc_log_level_set("*", SVC_LOG_INFO);
```

在执行开销较大的日志参数计算前，可以先判断日志是否启用：

```c
if (svc_log_is_enabled(SVC_LOG_DEBUG, TAG)) {
    SVC_LOGD(TAG, "状态：%s", build_expensive_status_string());
}
```

## 编译期日志裁剪

在包含 `svc_log.h` 之前定义 `SVC_LOG_LOCAL_LEVEL`，可以按源文件裁剪详细日志：

```c
#define SVC_LOG_LOCAL_LEVEL SVC_LOG_WARN
#include <svc_log.h>
```

该源文件中的 Info、Debug 和 Verbose 宏不会调用日志后端。

也可以通过 CMake 为整个目标设置：

```cmake
target_compile_definitions(my_app PRIVATE
    SVC_LOG_LOCAL_LEVEL=SVC_LOG_INFO
)
```

编译期等级限制和运行时等级限制会同时生效。

## 重定向日志输出

`svc_log_set_vprintf()` 可以把日志重定向到文件、串口或其他日志系统。回调函数
签名与 `vprintf` 一致：

```c
#include <stdarg.h>
#include <stdio.h>
#include <svc_log.h>

static FILE *log_file;

static int file_log_output(const char *format, va_list args) {
    return vfprintf(log_file, format, args);
}

void enable_file_log(void) {
    log_file = fopen("service.log", "a");
    if (log_file != NULL) {
        svc_log_set_vprintf(file_log_output);
    }
}

void restore_stderr_log(void) {
    svc_log_set_vprintf(NULL);
}
```

`svc_log_set_vprintf()` 返回之前的回调，可用于临时替换后恢复。日志组件内部会
串行化输出；回调内不要再次调用 `SVC_LOGx`，避免递归输出。

## 直接调用日志函数

一般使用 `SVC_LOGx` 宏即可。需要封装其他日志系统时，可以调用：

```c
svc_log_write(SVC_LOG_INFO, TAG, "value=%d", value);
svc_log_writev(SVC_LOG_INFO, TAG, format, args);
```

获取单调毫秒时间戳：

```c
uint64_t milliseconds = svc_log_timestamp();
```

## 宿主机测试

测试只在以下条件同时满足时加入：

- `DARKOS_BUILD_TESTS=ON`；
- 当前不是交叉编译。

在仓库根目录运行：

```bash
cmake -S applications/rv1126b_ipc \
    -B build/host-test \
    -DDARKOS_BUILD_TESTS=ON \
    -DDARKOS_ENABLE_OUTPUT_LAYOUT=OFF

cmake --build build/host-test --target svc_log_host_test -j

ctest --test-dir build/host-test \
    -R svc_log.host \
    --output-on-failure
```

必须先执行 `cmake --build` 生成 `svc_log_host_test`，CTest 只负责运行测试，
不会自动编译测试程序。

测试内容包括 C17 头文件兼容、默认等级、按 tag 过滤、格式化、输出重定向和
单调时间戳。

## 许可证

本组件遵循 Apache-2.0 许可证，具体版权声明见源码文件头。
