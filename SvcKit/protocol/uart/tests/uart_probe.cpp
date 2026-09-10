/*
 * uart_probe：串口行协议端到端自测（openpty 主从对，不入设备）
 *
 * 拓扑：probe 主线程持有 PTY 主端（模拟串口终端），UartAdapter 挂从端
 * （经 Services/peripheral 打开 HAL serial），EventLoop 跑在独立线程；
 * 控制面用真实 ControlService（settingsDir = mkdtemp 的 /tmp 目录）。
 *
 * 覆盖：HELP / LIST（含 ro/reboot 标注）/ GET 存在与不存在 /
 * SET 正常·只读·越界·类型错 / SET reboot 参数回 "OK reboot required" /
 * 超长行（>256B）断行重同步 / SAVE 落盘后 settings.cfg 内容校验。
 *
 * 用法：DARKOS_HAL_VARIANT=host_x86 DARKOS_HAL_LIBRARY_PATH=<build>/oem/lib \
 *       <build>/oem/bin/uart_probe
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <unistd.h>

#include "base/EventLoop.h"
#include "base/Log.h"
#include "control/ControlService.h"
#include "peripheral/SerialPort.h"
#include "uart/UartAdapter.h"

using namespace darkos;

namespace {

const char *kTag = "uart_probe";
int g_failures = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            LOGI(kTag, "PASS: %s", msg);                                                           \
        } else {                                                                                   \
            LOGE(kTag, "FAIL: %s", msg);                                                           \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

/* 发一行命令，收响应直到 "." 行（2s 超时）。返回完整响应文本。 */
std::string transact(int master, const std::string &cmd) {
    std::string req = cmd + "\n";
    if (write(master, req.data(), req.size()) != (ssize_t)req.size())
        LOGE(kTag, "write master failed: %s", strerror(errno));

    std::string resp;
    char buf[256];
    for (;;) {
        struct pollfd pfd = {master, POLLIN, 0};
        if (poll(&pfd, 1, 2000) <= 0) {
            LOGE(kTag, "timeout waiting response of \"%s\"", cmd.c_str());
            break;
        }
        ssize_t n = read(master, buf, sizeof(buf));
        if (n <= 0)
            break;
        resp.append(buf, (size_t)n);
        /* 结束标志：最后一行为 "." */
        if (resp.size() >= 2 && resp.compare(resp.size() - 2, 2, ".\n") == 0)
            break;
    }
    return resp;
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

int main() {
    setenv("DARKOS_HAL_VARIANT", "host_x86", 1);

    /* 控制面：注册 v1 参数（对齐 app_ipc 的注册集） */
    char dirTemplate[] = "/tmp/uart_probe.XXXXXX";
    const char *settingsDir = mkdtemp(dirTemplate);
    if (settingsDir == NULL) {
        LOGE(kTag, "mkdtemp failed: %s", strerror(errno));
        return 1;
    }
    ControlService &cs = ControlService::instance();
    cs.registerParam(ParamMeta{"video0.bitrate_kbps", ParamType::kInt, true, true,
                               ParamApply::kImmediate, 128, 8192},
                     "2048");
    cs.registerParam(
        ParamMeta{"device.name", ParamType::kString, true, true, ParamApply::kImmediate, 0, 32},
        "app_ipc");
    cs.registerParam(
        ParamMeta{"rtsp.user", ParamType::kString, true, true, ParamApply::kReboot, 0, 32},
        "admin");
    cs.registerParam(
        ParamMeta{"device.fw", ParamType::kString, false, false, ParamApply::kImmediate, 0, 32},
        "v1.0.0");
    if (cs.start(settingsDir) != 0) {
        LOGE(kTag, "control service start failed");
        return 1;
    }

    /* PTY 主从对：adapter 挂从端 */
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        LOGE(kTag, "openpty failed: %s", strerror(errno));
        return 1;
    }
    char *slave = ptsname(master);
    if (slave == NULL) {
        LOGE(kTag, "ptsname failed");
        return 1;
    }

    EventLoop *loop = EventLoop::create();
    SerialPort *serial = SerialPort::open(slave, 115200);
    if (serial == nullptr) {
        LOGE(kTag, "serial open failed (pty %s)", slave);
        return 1;
    }
    UartAdapter *uart = UartAdapter::create(loop, serial->fd());
    if (uart->start() != 0) {
        LOGE(kTag, "uart start failed (pty %s)", slave);
        return 1;
    }
    std::thread loopThread([loop] { loop->run(); });

    /* ---- HELP ---- */
    std::string r = transact(master, "HELP");
    CHECK(contains(r, "LIST") && contains(r, "GET") && contains(r, "SET"), "HELP 命令清单");

    /* ---- LIST：含注册参数与 ro/reboot 标注 ---- */
    r = transact(master, "LIST");
    CHECK(contains(r, "video0.bitrate_kbps int 2048"), "LIST 含 video0.bitrate_kbps int 2048");
    CHECK(contains(r, "device.fw string v1.0.0 ro"), "LIST 只读参数带 ro 标注");
    CHECK(contains(r, "rtsp.user string admin reboot"), "LIST reboot 参数带 reboot 标注");

    /* ---- GET ---- */
    r = transact(master, "GET video0.bitrate_kbps");
    CHECK(contains(r, "video0.bitrate_kbps=2048"), "GET 存在参数");
    r = transact(master, "GET no.such");
    CHECK(contains(r, "ERR"), "GET 不存在参数回 ERR");

    /* ---- SET 正常 / 只读 / 越界 / 类型错 / reboot ---- */
    r = transact(master, "SET video0.bitrate_kbps 1024");
    CHECK(contains(r, "OK") && !contains(r, "reboot"), "SET immediate 参数回 OK");
    r = transact(master, "GET video0.bitrate_kbps");
    CHECK(contains(r, "video0.bitrate_kbps=1024"), "SET 后 GET 读到新值");
    r = transact(master, "SET device.fw x");
    CHECK(contains(r, "ERR read-only"), "SET 只读参数回 ERR read-only");
    r = transact(master, "SET video0.bitrate_kbps 9000");
    CHECK(contains(r, "ERR out of range"), "SET 越界回 ERR out of range");
    r = transact(master, "SET video0.bitrate_kbps abc");
    CHECK(contains(r, "ERR invalid value"), "SET 类型错回 ERR invalid value");
    r = transact(master, "SET rtsp.user operator");
    CHECK(contains(r, "OK reboot required"), "SET reboot 参数回 OK reboot required");

    /* ---- 超长行（>256B）断行重同步：ERR 之后后续命令仍正常 ---- */
    std::string longLine = "GET " + std::string(300, 'x');
    r = transact(master, longLine);
    CHECK(contains(r, "ERR line too long"), "超长行回 ERR line too long");
    r = transact(master, "GET device.name");
    CHECK(contains(r, "device.name=app_ipc"), "超长行后重同步，后续命令正常");

    /* ---- REBOOT 占位 ---- */
    r = transact(master, "REBOOT");
    CHECK(contains(r, "OK reboot on next power cycle"), "REBOOT 占位回复");

    /* ---- SAVE 落盘：settings.cfg 含 persist 参数新值 ---- */
    r = transact(master, "SAVE");
    CHECK(contains(r, "OK"), "SAVE 回 OK");
    char cfgPath[128];
    snprintf(cfgPath, sizeof(cfgPath), "%s/settings.cfg", settingsDir);
    FILE *fp = fopen(cfgPath, "r");
    CHECK(fp != NULL, "settings.cfg 已生成");
    if (fp != NULL) {
        std::string content;
        char line[128];
        while (fgets(line, sizeof(line), fp) != NULL)
            content += line;
        fclose(fp);
        CHECK(contains(content, "video0.bitrate_kbps=1024"), "settings.cfg 含码率新值");
        CHECK(contains(content, "rtsp.user=operator"), "settings.cfg 含 reboot 参数新值");
        CHECK(!contains(content, "device.fw"), "settings.cfg 不含非持久化参数");
    }

    /* ---- 逆序关停 ---- */
    loop->post([loop] { loop->quit(); });
    loopThread.join();
    uart->stop();
    delete uart;
    delete serial;
    delete loop;
    cs.stop();
    close(master);

    /* 清理临时目录 */
    unlink(cfgPath);
    rmdir(settingsDir);

    if (g_failures == 0)
        LOGI(kTag, "ALL PASS");
    else
        LOGE(kTag, "%d FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
