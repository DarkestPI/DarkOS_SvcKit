#pragma once

/* ---------------------------------------------------------------------------
 * UartAdapter：串口行协议适配器（SvcKit/Protocol/uart）
 *
 * 控制面的第一个落地前端（见 docs/控制面设计.md 第 4 节）：接收外部已打开的
 * serial fd，挂 EventLoop watchFd（EPOLLIN，一次性语义，回调里重新注册），
 * 把行协议命令翻译成 ControlService 的 get/set——只做翻译，不含业务判断。
 *
 * 线协议（串口终端可手敲调试）：一行命令进（\n 结尾，兼容 \r\n），
 * 多行文本出，"." 一行结束。命令：HELP / LIST / GET / SET / SAVE / REBOOT。
 *
 * 用法：
 *   UartAdapter *uart = UartAdapter::create(loop, serialPort->fd());
 *   if (uart->start() != 0) {
 *       delete uart; uart = nullptr;
 *   }
 *   ... uart->stop(); delete uart;
 *
 * 线程约定：create/start/stop 与所有内部回调都运行在 loop 线程
 * （teardown 在 loop 退出后、delete loop 之前调用即可）。
 * ------------------------------------------------------------------------- */

#include <cstdint>

namespace darkos {

class EventLoop;

class UartAdapter {
  public:
    /* 创建适配器（返回对象由调用方 delete）。fd 非拥有，生命周期由调用方保证。 */
    static UartAdapter *create(EventLoop *loop, int fd);

    virtual ~UartAdapter() = default;

    /* fd 挂 loop->watchFd(EPOLLIN)。返回 0 成功；负值失败。 */
    virtual int start() = 0;

    /* unwatch；不关闭外部 fd。幂等。 */
    virtual void stop() = 0;
};

} // namespace darkos
