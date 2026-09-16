#!/usr/bin/env bash
set -e

ROOT="."
AUTHOR="your_name"
DATE=$(date +%Y-%m-%d)

# ---------- 目录 ----------
DIRS=(
    "$ROOT/inc"
    "$ROOT/src/core"
    "$ROOT/src/socket"
    "$ROOT/src/event"
    "$ROOT/src/buffer"
    "$ROOT/src/connection"
    "$ROOT/src/protocol/http"
    "$ROOT/src/protocol/mqtt"
    "$ROOT/src/protocol/websocket"
    "$ROOT/src/protocol/rtsp"
    "$ROOT/src/protocol/rtp"
    "$ROOT/src/security"
    "$ROOT/src/interface"
    "$ROOT/src/diagnostic"
    "$ROOT/src/factory"
    "$ROOT/test/unit"
    "$ROOT/test/example"
)

for d in "${DIRS[@]}"; do
    mkdir -p "$d"
done

# ---------- 工具函数 ----------
mk_header() {
    local path="$1"
    local brief="$2"
    cat > "$path" <<EOF
/**
 * @file $(basename "$path")
 * @brief $brief
 * @author $AUTHOR
 * @date $DATE
 */
#pragma once

namespace network {

} // namespace network
EOF
}

mk_source() {
    local path="$1"
    local brief="$2"
    local include="$3"
    cat > "$path" <<EOF
/**
 * @file $(basename "$path")
 * @brief $brief
 * @author $AUTHOR
 * @date $DATE
 */

#include "$include"

namespace network {

} // namespace network
EOF
}

# ---------- 头文件 ----------
mk_header "$ROOT/inc/network_types.h"       "网络基础类型定义（地址、错误、配置、枚举）"
mk_header "$ROOT/inc/network_socket.h"      "Socket 抽象接口（TCP/UDP/Unix）"
mk_header "$ROOT/inc/network_event.h"       "事件循环与定时器接口"
mk_header "$ROOT/inc/network_buffer.h"      "网络缓冲区与内存池接口"
mk_header "$ROOT/inc/network_connection.h"  "连接管理接口"
mk_header "$ROOT/inc/network_protocol.h"    "协议公共接口"
mk_header "$ROOT/inc/network_security.h"    "安全接口（TLS/DTLS）"
mk_header "$ROOT/inc/network_interface.h"   "网络接口与连接管理接口（有线/WiFi/4G）"
mk_header "$ROOT/inc/network_diagnostic.h"  "网络诊断接口（ping/统计/日志）"
mk_header "$ROOT/inc/network_manager.h"     "网络管理器门面"

# ---------- core ----------
mk_source "$ROOT/src/core/network_manager.cpp"   "网络管理器实现"       "network_manager.h"
mk_source "$ROOT/src/core/network_scheduler.cpp" "网络任务调度器"       "network_manager.h"
mk_source "$ROOT/src/core/network_stats.cpp"     "网络统计信息"         "network_manager.h"

# ---------- socket ----------
mk_source "$ROOT/src/socket/tcp_socket.cpp"   "TCP Socket 实现"      "network_socket.h"
mk_source "$ROOT/src/socket/udp_socket.cpp"   "UDP Socket 实现"      "network_socket.h"
mk_source "$ROOT/src/socket/unix_socket.cpp"  "Unix Domain Socket"   "network_socket.h"
mk_source "$ROOT/src/socket/address.cpp"      "地址解析与转换"        "network_socket.h"

# ---------- event ----------
mk_source "$ROOT/src/event/epoll_loop.cpp"    "epoll 事件循环"        "network_event.h"
mk_source "$ROOT/src/event/timer.cpp"         "定时器实现"            "network_event.h"
mk_source "$ROOT/src/event/task_queue.cpp"    "任务队列"              "network_event.h"

# ---------- buffer ----------
mk_source "$ROOT/src/buffer/ring_buffer.cpp"   "环形缓冲区"           "network_buffer.h"
mk_source "$ROOT/src/buffer/buffer_pool.cpp"   "网络内存池"           "network_buffer.h"
mk_source "$ROOT/src/buffer/chain_buffer.cpp"  "链式缓冲区"           "network_buffer.h"

# ---------- connection ----------
mk_source "$ROOT/src/connection/tcp_client.cpp"      "TCP 客户端"       "network_connection.h"
mk_source "$ROOT/src/connection/tcp_server.cpp"      "TCP 服务端"       "network_connection.h"
mk_source "$ROOT/src/connection/udp_endpoint.cpp"    "UDP 端点"         "network_connection.h"
mk_source "$ROOT/src/connection/connection_pool.cpp" "连接池"           "network_connection.h"
mk_source "$ROOT/src/connection/reconnect.cpp"       "自动重连与心跳"    "network_connection.h"

# ---------- protocol / http ----------
mk_source "$ROOT/src/protocol/http/http_client.cpp"  "HTTP 客户端"     "network_protocol.h"
mk_source "$ROOT/src/protocol/http/http_server.cpp"  "HTTP 服务端"     "network_protocol.h"
mk_source "$ROOT/src/protocol/http/http_parser.cpp"  "HTTP 解析器"     "network_protocol.h"

# ---------- protocol / mqtt ----------
mk_source "$ROOT/src/protocol/mqtt/mqtt_client.cpp"  "MQTT 客户端"     "network_protocol.h"
mk_source "$ROOT/src/protocol/mqtt/mqtt_packet.cpp"  "MQTT 报文编解码" "network_protocol.h"

# ---------- protocol / websocket ----------
mk_source "$ROOT/src/protocol/websocket/ws_client.cpp" "WebSocket 客户端" "network_protocol.h"
mk_source "$ROOT/src/protocol/websocket/ws_server.cpp" "WebSocket 服务端" "network_protocol.h"

# ---------- protocol / rtsp ----------
mk_source "$ROOT/src/protocol/rtsp/rtsp_server.cpp"  "RTSP 服务端"     "network_protocol.h"
mk_source "$ROOT/src/protocol/rtsp/rtsp_client.cpp"  "RTSP 客户端"     "network_protocol.h"
mk_source "$ROOT/src/protocol/rtsp/rtsp_session.cpp" "RTSP 会话管理"   "network_protocol.h"

# ---------- protocol / rtp ----------
mk_source "$ROOT/src/protocol/rtp/rtp_packet.cpp"  "RTP 包处理"     "network_protocol.h"
mk_source "$ROOT/src/protocol/rtp/rtcp_packet.cpp" "RTCP 包处理"    "network_protocol.h"
mk_source "$ROOT/src/protocol/rtp/rtp_sender.cpp"  "RTP 发送器"     "network_protocol.h"

# ---------- security ----------
mk_source "$ROOT/src/security/tls_context.cpp"  "TLS 上下文管理"     "network_security.h"
mk_source "$ROOT/src/security/tls_socket.cpp"   "TLS Socket"         "network_security.h"
mk_source "$ROOT/src/security/dtls_socket.cpp"  "DTLS Socket"        "network_security.h"
mk_source "$ROOT/src/security/cert_manager.cpp" "证书与密钥管理"     "network_security.h"

# ---------- interface ----------
mk_source "$ROOT/src/interface/interface_manager.cpp" "网络接口管理（枚举/状态/IP）" "network_interface.h"
mk_source "$ROOT/src/interface/ethernet_manager.cpp"  "有线网络管理"                 "network_interface.h"
mk_source "$ROOT/src/interface/wifi_manager.cpp"      "WiFi 管理（wpa_supplicant）"  "network_interface.h"
mk_source "$ROOT/src/interface/cellular_manager.cpp"  "蜂窝管理（ModemManager/AT）"  "network_interface.h"
mk_source "$ROOT/src/interface/route_manager.cpp"     "路由与 DNS 管理"              "network_interface.h"
mk_source "$ROOT/src/interface/link_monitor.cpp"      "链路状态与优先级监控"          "network_interface.h"

# ---------- diagnostic ----------
mk_source "$ROOT/src/diagnostic/ping.cpp"          "Ping 实现"        "network_diagnostic.h"
mk_source "$ROOT/src/diagnostic/net_stats.cpp"     "网络统计"         "network_diagnostic.h"
mk_source "$ROOT/src/diagnostic/net_log.cpp"       "网络日志"         "network_diagnostic.h"

# ---------- factory ----------
mk_source "$ROOT/src/factory/network_factory.cpp" "网络工厂" "network_manager.h"

echo "✅ $ROOT 网络子系统目录结构已生成"
echo
find "$ROOT" -type d | sort
echo
echo "文件总数: $(find "$ROOT" -type f | wc -l)"