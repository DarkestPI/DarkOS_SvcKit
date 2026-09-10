#ifndef DARKOS_HARDWARE_WIFI_TYPES_H
#define DARKOS_HARDWARE_WIFI_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * wifi 接口共享类型（对应 Android 的 wifi/，仅保留 IPC 相机所需子集）
 *
 * 典型场景：STA 模式连路由器出流；AP 模式用于出厂配网（手机直连热点）。
 * Rockchip 实现基于 wpa_supplicant（STA）/ hostapd（AP）。
 * ------------------------------------------------------------------------- */

/* 能力位图：工作模式 */
#define WIFI_CAPS_MODE_STA (1u << 0)
#define WIFI_CAPS_MODE_AP (1u << 1)

/* 能力位图：频段 */
#define WIFI_CAPS_BAND_2G4 (1u << 0)
#define WIFI_CAPS_BAND_5G (1u << 1)

#define WIFI_SSID_MAX_LEN 32 /* IEEE 802.11 上限 */
#define WIFI_PSK_MAX_LEN 63  /* WPA  passphrase 上限 */
#define WIFI_BSSID_LEN 6

/* 加密方式 */
typedef enum wifi_security {
    WIFI_SECURITY_OPEN = 0,
    WIFI_SECURITY_WPA2_PSK = 1,
    WIFI_SECURITY_WPA3_SAE = 2,
} wifi_security_t;

/* 能力描述 */
typedef struct wifi_caps {
    uint32_t supported_modes; /* WIFI_CAPS_MODE_* 位图 */
    uint32_t supported_bands; /* WIFI_CAPS_BAND_* 位图 */
} wifi_caps_t;

/* 扫描到的热点 */
typedef struct wifi_ap_info {
    char ssid[WIFI_SSID_MAX_LEN + 1];
    uint8_t bssid[WIFI_BSSID_LEN];
    int32_t rssi; /* dBm */
    uint32_t frequency_mhz;
    uint32_t security; /* wifi_security_t */
} wifi_ap_info_t;

/* 连接配置（STA 模式） */
typedef struct wifi_config {
    char ssid[WIFI_SSID_MAX_LEN + 1];
    char psk[WIFI_PSK_MAX_LEN + 1]; /* OPEN 时忽略 */
    uint32_t security;              /* wifi_security_t */
} wifi_config_t;

/* 连接状态 */
typedef enum wifi_state {
    WIFI_STATE_DISCONNECTED = 0,
    WIFI_STATE_CONNECTING = 1,
    WIFI_STATE_CONNECTED = 2,
} wifi_state_t;

typedef struct wifi_status {
    uint32_t state; /* wifi_state_t */
    char ssid[WIFI_SSID_MAX_LEN + 1];
    int32_t rssi;
    uint32_t frequency_mhz;
    char ip_addr[16]; /* "192.168.1.100"，未连接时为空串 */
} wifi_status_t;

/* 扫描结果回调：返回 0 继续；非 0 提前结束扫描 */
typedef int (*wifi_scan_cb)(void *ctx, const wifi_ap_info_t *ap);

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_WIFI_TYPES_H */
