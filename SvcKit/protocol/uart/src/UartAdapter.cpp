/*
 * UartAdapter：串口行协议适配器实现（见公开头头注与 docs/控制面设计.md 第 4 节）
 *
 * 实现要点：
 * - fd 由 Services/peripheral 以 O_NONBLOCK 打开并注入；onReadable 里 read 排到 EAGAIN，
 *   响应直接 write（非阻塞写不完截断告警——响应都很小，PTY/串口缓冲够用）；
 * - watchFd 一次性语义：回调末尾重新注册（同 RtspServer::onAccept 写法）；
 * - 行缓冲 256B：超限置丢弃标志，到下一个 \n 断行重同步并回 ERR；
 * - 命令处理只翻译：校验/权限/持久化全在 ControlService，这里只做
 *   文本解析与结果码 → 回包文本的映射。
 */

#include "uart/UartAdapter.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unistd.h>

#include "base/EventLoop.h"
#include <svc_log.h>
#include "control/ControlService.h"
#include "control/Events.h" /* kParamUart */

namespace darkos {

namespace {

const char *kTag = "UartAdapter";

constexpr size_t kLineMax = 256; /* 行缓冲上限（含 NUL），超限断行重同步 */

const char *typeName(ParamType t) {
    switch (t) {
    case ParamType::kInt:
        return "int";
    case ParamType::kBool:
        return "bool";
    case ParamType::kString:
        return "string";
    }
    return "?";
}

/* ControlService::set 结果码 → 原因文本 */
const char *setErrorText(int rc) {
    switch (rc) {
    case -ENOENT:
        return "no such param";
    case -EPERM:
        return "read-only";
    case -ERANGE:
        return "out of range";
    case -EINVAL:
        return "invalid value";
    default:
        return "error";
    }
}

class UartAdapterImpl : public UartAdapter {
  public:
    UartAdapterImpl(EventLoop *loop, int fd) : loop_(loop), fd_(fd) {}

    ~UartAdapterImpl() override {
        stop();
    }

    int start() override {
        if (loop_ == nullptr || fd_ < 0)
            return -1;
        if (!loop_->watchFd(fd_, EPOLLIN, [this](uint32_t ev) { onReadable(ev); }))
            return -1;
        watching_ = true;
        SVC_LOGI(kTag, "started: fd=%d", fd_);
        return 0;
    }

    void stop() override {
        if (!watching_)
            return;
        loop_->unwatchFd(fd_);
        watching_ = false;
        SVC_LOGI(kTag, "stopped");
    }

  private:
    /* fd 可读（loop 线程）：read 排到 EAGAIN，字节喂给行缓冲 */
    void onReadable(uint32_t events) {
        if (events & (EPOLLERR | EPOLLHUP))
            SVC_LOGW(kTag, "fd=%d: events=0x%x", fd_, events);

        char buf[128];
        for (;;) {
            ssize_t n = read(fd_, buf, sizeof(buf));
            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                    SVC_LOGE(kTag, "read failed: %s", strerror(errno));
                break;
            }
            if (n == 0)
                break;
            feed(buf, (size_t)n);
        }

        /* 一次性语义：重新注册 */
        if (watching_)
            watching_ = loop_->watchFd(fd_, EPOLLIN, [this](uint32_t ev) { onReadable(ev); });
    }

    void feed(const char *data, size_t len) {
        for (size_t i = 0; i < len; i++) {
            char c = data[i];
            if (discarding_) { /* 超长行重同步：丢到下一个 \n */
                if (c == '\n') {
                    discarding_ = false;
                    lineLen_ = 0;
                    respond("ERR line too long");
                }
                continue;
            }
            if (c == '\n') {
                line_[lineLen_] = '\0';
                /* 兼容 \r\n */
                if (lineLen_ > 0 && line_[lineLen_ - 1] == '\r')
                    line_[--lineLen_] = '\0';
                if (lineLen_ > 0)
                    dispatch(line_);
                lineLen_ = 0;
                continue;
            }
            if (lineLen_ >= kLineMax - 1) {
                SVC_LOGW(kTag, "line over %zu bytes, resync", kLineMax - 1);
                discarding_ = true;
                continue;
            }
            line_[lineLen_++] = c;
        }
    }

    /* 回包：多行文本 + "." 结束行（appctl 范式） */
    void respond(const std::string &body) {
        std::string out = body;
        out += ".\n";
        size_t off = 0;
        while (off < out.size()) {
            ssize_t n = write(fd_, out.data() + off, out.size() - off);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    SVC_LOGW(kTag, "response truncated（%zu/%zu bytes sent）", off, out.size());
                    break;
                }
                SVC_LOGE(kTag, "write failed: %s", strerror(errno));
                break;
            }
            off += (size_t)n;
        }
    }

    /* 拆命令行：CMD [arg1 [arg2]]（按空白分，最多两段参数） */
    void dispatch(char *line) {
        char *cmd = strtok(line, " \t");
        char *arg1 = strtok(NULL, " \t");
        char *arg2 = strtok(NULL, " \t");
        if (cmd == NULL)
            return;

        if (strcasecmp(cmd, "HELP") == 0) {
            respond("HELP                 命令清单\n"
                    "LIST                 全部参数（ro=只读 reboot=重启生效）\n"
                    "GET <name>           读参数\n"
                    "SET <name> <value>   设参数\n"
                    "SAVE                 立即落盘\n"
                    "REBOOT               重启（v1 占位，不真重启）\n");
        } else if (strcasecmp(cmd, "LIST") == 0) {
            cmdList();
        } else if (strcasecmp(cmd, "GET") == 0) {
            cmdGet(arg1);
        } else if (strcasecmp(cmd, "SET") == 0) {
            cmdSet(arg1, arg2);
        } else if (strcasecmp(cmd, "SAVE") == 0) {
            respond(ControlService::instance().save() == 0 ? "OK\n" : "ERR save failed\n");
        } else if (strcasecmp(cmd, "REBOOT") == 0) {
            respond("OK reboot on next power cycle\n");
        } else {
            respond("ERR unknown command (try HELP)\n");
        }
    }

    void cmdList() {
        ControlService &cs = ControlService::instance();
        std::string out;
        char line[160], value[64];
        for (size_t i = 0; i < cs.paramCount(); i++) {
            const ParamMeta *m = cs.paramAt(i);
            if (m == nullptr || cs.get(m->name, value, sizeof(value)) != 0)
                continue;
            int n = snprintf(line, sizeof(line), "%s %s %s", m->name, typeName(m->type), value);
            if (!m->writable)
                n += snprintf(line + n, sizeof(line) - (size_t)n, " ro");
            if (m->apply == ParamApply::kReboot)
                n += snprintf(line + n, sizeof(line) - (size_t)n, " reboot");
            line[n++] = '\n';
            line[n] = '\0';
            out += line;
        }
        respond(out);
    }

    void cmdGet(const char *name) {
        if (name == NULL) {
            respond("ERR usage: GET <name>\n");
            return;
        }
        char value[64];
        if (ControlService::instance().get(name, value, sizeof(value)) != 0) {
            respond("ERR no such param\n");
            return;
        }
        char line[160];
        snprintf(line, sizeof(line), "%s=%s\n", name, value);
        respond(line);
    }

    void cmdSet(const char *name, const char *value) {
        if (name == NULL || value == NULL) {
            respond("ERR usage: SET <name> <value>\n");
            return;
        }
        int rc = ControlService::instance().set(name, value, kParamUart);
        if (rc == 0)
            respond("OK\n");
        else if (rc == 1)
            respond("OK reboot required\n");
        else {
            char line[64];
            snprintf(line, sizeof(line), "ERR %s\n", setErrorText(rc));
            respond(line);
        }
    }

    EventLoop *loop_;
    int fd_;
    bool watching_ = false;
    char line_[kLineMax];
    size_t lineLen_ = 0;
    bool discarding_ = false; /* 超长行丢弃中（到 \n 重同步） */
};

} // namespace

UartAdapter *UartAdapter::create(EventLoop *loop, int fd) {
    return new UartAdapterImpl(loop, fd);
}

} // namespace darkos
