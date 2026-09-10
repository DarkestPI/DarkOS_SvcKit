#ifndef DARKOS_HARDWARE_SENSORS_TYPES_H
#define DARKOS_HARDWARE_SENSORS_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * sensors 接口共享类型（对应 Android 的 sensors/）
 *
 * IPC 相机常见传感器：光敏（日夜切换）、温度（过热保护）、加速度（位移告警）。
 * type 编号对齐 Android SENSOR_TYPE_*，vendor 实现无需转换。
 * ------------------------------------------------------------------------- */

/* 传感器类型（对齐 Android hardware/sensors.h 的 SENSOR_TYPE_* 编号） */
typedef enum sensor_type {
    SENSOR_TYPE_ACCELEROMETER = 1,      /* m/s^2，xyz */
    SENSOR_TYPE_MAGNETIC_FIELD = 2,     /* uT，xyz */
    SENSOR_TYPE_GYROSCOPE = 4,          /* rad/s，xyz */
    SENSOR_TYPE_LIGHT = 5,              /* lux，data[0] */
    SENSOR_TYPE_PRESSURE = 6,           /* hPa，data[0] */
    SENSOR_TYPE_PROXIMITY = 8,          /* cm，data[0] */
    SENSOR_TYPE_RELATIVE_HUMIDITY = 12, /* %，data[0] */
    SENSOR_TYPE_TEMPERATURE = 13,       /* ℃，data[0] */
} sensor_type_t;

#define SENSOR_NAME_MAX_LEN 32

/* 传感器描述（枚举用） */
typedef struct sensor_info {
    int32_t handle; /* 激活/订阅用的句柄，由实现分配 */
    uint32_t type;  /* sensor_type_t */
    char name[SENSOR_NAME_MAX_LEN];
    char vendor[SENSOR_NAME_MAX_LEN];
    float max_range;
    float resolution;
    uint32_t min_delay_us; /* 最短采样间隔 */
} sensor_info_t;

/* 采样事件 */
typedef struct sensor_event {
    int32_t handle; /* 对应 sensor_info_t.handle */
    uint64_t timestamp_ns;
    float data[3]; /* 单值传感器用 data[0] */
} sensor_event_t;

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_SENSORS_TYPES_H */
