#ifndef DARKOS_HARDWARE_LIGHT_TYPES_H
#define DARKOS_HARDWARE_LIGHT_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * light 接口共享类型（对应 Android 的 light/）
 *
 * IPC 相机的灯：状态指示灯、红外补光、白光补光（声光警戒）。
 * 实现基于 GPIO/PWM。
 * ------------------------------------------------------------------------- */

/* 灯 id */
typedef enum light_id {
    LIGHT_ID_STATUS = 0, /* 状态指示灯（红/绿等） */
    LIGHT_ID_IR = 1,     /* 红外补光（夜视） */
    LIGHT_ID_WHITE = 2,  /* 白光补光（全彩夜视/警戒） */
} light_id_t;

/* 能力位图（与 light_id_t 一一对应） */
#define LIGHT_CAPS_STATUS (1u << LIGHT_ID_STATUS)
#define LIGHT_CAPS_IR (1u << LIGHT_ID_IR)
#define LIGHT_CAPS_WHITE (1u << LIGHT_ID_WHITE)

/* 闪烁模式（对齐 Android light.h 的 LIGHT_FLASH_*） */
typedef enum light_flash_mode {
    LIGHT_FLASH_NONE = 0,  /* 常亮 */
    LIGHT_FLASH_TIMED = 1, /* 按 on/off 时长软件闪烁 */
} light_flash_mode_t;

/* 灯状态 */
typedef struct light_state {
    uint32_t color_rgb;  /* 0x00RRGGBB；单色灯忽略 */
    uint32_t brightness; /* 0-255 */
    uint32_t flash_mode; /* light_flash_mode_t */
    uint32_t flash_on_ms;
    uint32_t flash_off_ms;
} light_state_t;

/* 能力描述 */
typedef struct light_caps {
    uint32_t supported_lights; /* LIGHT_CAPS_* 位图 */
} light_caps_t;

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_LIGHT_TYPES_H */
