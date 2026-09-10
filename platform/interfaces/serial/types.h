#ifndef DARKOS_HARDWARE_SERIAL_TYPES_H
#define DARKOS_HARDWARE_SERIAL_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * serial 接口共享类型（串口/UART）
 *
 * IPC 相机的串口用途：接 MCU（云台/补光控制）、调试控制面（UART 行协议，
 * 见 docs/控制面设计.md）。
 * v1 固定 8N1、无流控，只暴露设备路径与波特率两个可配项。
 * ------------------------------------------------------------------------- */

/* 串口打开配置 */
typedef struct serial_config {
    const char *device; /* 设备路径，如 /dev/ttyS1；host 自测可用 PTY 路径 */
    uint32_t baud;      /* 波特率（常用档位：9600/19200/38400/57600/115200…） */
} serial_config_t;

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_SERIAL_TYPES_H */
