# svc_log 宿主机测试

本目录用于在 Linux 宿主机上验证 `svc_log`，不会加入交叉编译构建。

当前覆盖：

- C17 公开头文件和日志宏编译；
- 默认日志等级；
- 按 tag 设置日志等级；
- 运行时日志过滤；
- printf 参数格式化；
- `svc_log_set_vprintf()` 输出重定向；
- 单调毫秒时间戳。

在任意 DarkOS Application 中开启测试后构建和运行：

```bash
cmake -S . -B build -DDARKOS_BUILD_TESTS=ON
cmake --build build --target svc_log_host_test
ctest --test-dir build -R svc_log.host --output-on-failure
```
