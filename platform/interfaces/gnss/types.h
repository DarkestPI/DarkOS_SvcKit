#ifndef DARKOS_HARDWARE_GNSS_TYPES_H
#define DARKOS_HARDWARE_GNSS_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * gnss 接口共享类型（对应 Android 的 gnss/）
 *
 * 定位数据经回调上报（推送模型），与 Android IGnssCallback 一致。
 * 实现基于串口 AT/ubx 模组或板载 GNSS 芯片。
 * ------------------------------------------------------------------------- */

/* 能力位图：支持的星座 */
#define GNSS_CONSTELLATION_GPS (1u << 0)
#define GNSS_CONSTELLATION_GLONASS (1u << 1)
#define GNSS_CONSTELLATION_BEIDOU (1u << 2)
#define GNSS_CONSTELLATION_GALILEO (1u << 3)

/* 能力描述 */
typedef struct gnss_caps {
    uint32_t supported_constellations; /* GNSS_CONSTELLATION_* 位图 */
    uint32_t max_frequency_hz;         /* 最高定位输出频率 */
} gnss_caps_t;

/* location flags：标出哪些字段有效（对齐 Android GnssLocationFlags） */
#define GNSS_LOCATION_HAS_LAT_LON (1u << 0)
#define GNSS_LOCATION_HAS_ALTITUDE (1u << 1)
#define GNSS_LOCATION_HAS_SPEED (1u << 2)
#define GNSS_LOCATION_HAS_BEARING (1u << 3)
#define GNSS_LOCATION_HAS_ACCURACY (1u << 4)

/* 定位结果 */
typedef struct gnss_location {
    uint32_t flags; /* GNSS_LOCATION_HAS_* 位图 */
    double latitude_deg;
    double longitude_deg;
    double altitude_m;
    float speed_mps;
    float bearing_deg;  /* 航向，真北顺时针 0-360 */
    float accuracy_m;   /* 水平精度（68% 置信） */
    uint64_t timestamp_ns;
} gnss_location_t;

/* 定位回调：返回 0 继续；非 0 停止上报 */
typedef int (*gnss_location_cb)(void *ctx, const gnss_location_t *location);

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_GNSS_TYPES_H */
