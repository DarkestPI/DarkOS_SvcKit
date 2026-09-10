#ifndef DARKOS_HARDWARE_BLUETOOTH_TYPES_H
#define DARKOS_HARDWARE_BLUETOOTH_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * bluetooth 接口共享类型（对应 Android 的 bluetooth/）
 *
 * IPC 相机场景以 BLE 配网为主（手机 APP 发现设备、下发 wifi 凭据）；
 * Classic 仅作能力预留。Rockchip 实现基于 BlueZ（D-Bus）。
 * ------------------------------------------------------------------------- */

/* 能力位图 */
#define BT_CAPS_CLASSIC (1u << 0)
#define BT_CAPS_BLE (1u << 1)

#define BT_ADDR_LEN 6
#define BT_NAME_MAX_LEN 248 /* BlueZ 设备名上限 */

/* 发现到的设备 flags */
#define BT_DEVICE_FLAG_BLE (1u << 0) /* 该设备经 BLE 发现 */

/* 能力描述 */
typedef struct bt_caps {
    uint32_t supported; /* BT_CAPS_* 位图 */
} bt_caps_t;

/* 适配器状态 */
typedef enum bt_state {
    BT_STATE_OFF = 0,
    BT_STATE_ON = 1,
} bt_state_t;

/* 发现到的对端设备 */
typedef struct bt_device_info {
    uint8_t addr[BT_ADDR_LEN];
    char name[BT_NAME_MAX_LEN + 1];
    int32_t rssi; /* dBm，未知时为 0 */
    uint32_t flags; /* BT_DEVICE_FLAG_* */
} bt_device_info_t;

/* 发现回调：返回 0 继续；非 0 提前结束发现 */
typedef int (*bt_discovery_cb)(void *ctx, const bt_device_info_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_BLUETOOTH_TYPES_H */
