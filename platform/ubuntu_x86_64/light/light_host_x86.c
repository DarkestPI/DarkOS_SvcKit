/*
 * 主机参考实现（host_x86 变体）：LIGHT 灯设备的软件模拟。
 *
 * 用途：在宿主机上验证 light 接口语义（caps 查询、set/get 状态存取、
 *       非法 id 与参数校验），供 frameworks 上层联调，不依赖 GPIO/PWM。
 *
 * 与 light.rockchip.so（待开发）实现同一套 light_device_ops，上层无感知。
 * 用环境变量切换：DARKOS_HAL_VARIANT=host_x86。
 *
 * 模拟语义：状态只保存在内存数组里（按 LIGHT_ID_* 索引）；TIMED 闪烁
 * 只记录目标状态，不真的跑定时器——真实实现由 GPIO/PWM 定时器完成。
 */

#include <hardware/hardware.h>
#include <light/ILight.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST_LIGHT_COUNT 3 /* STATUS/IR/WHITE 三盏灯 */

typedef struct host_light_priv {
    light_state_t states[HOST_LIGHT_COUNT]; /* 每盏灯最近一次设置的状态 */
} host_light_priv_t;

/* id 合法性：0..HOST_LIGHT_COUNT-1 */
static int host_light_check_id(uint32_t id) {
    return id < HOST_LIGHT_COUNT ? 0 : -EINVAL;
}

/* ---------------------------------------------------------------------------
 * 设备操作实现
 * ------------------------------------------------------------------------- */

static int host_light_get_capabilities(light_device_t *dev, light_caps_t *caps) {
    (void)dev;
    if (caps == NULL)
        return -EINVAL;
    memset(caps, 0, sizeof(*caps));
    caps->supported_lights = LIGHT_CAPS_STATUS | LIGHT_CAPS_IR | LIGHT_CAPS_WHITE;
    return 0;
}

static int host_light_set_light(light_device_t *dev, uint32_t id, const light_state_t *state) {
    host_light_priv_t *priv = (host_light_priv_t *)dev->priv;
    light_state_t s;

    if (state == NULL)
        return -EINVAL;
    if (host_light_check_id(id) != 0)
        return -EINVAL;

    s = *state;
    /* 亮度超界处理策略：截断到 255（而非报错），与真实 PWM 驱动的
     * 常见做法一致，上层传多大都不会失败。 */
    if (s.brightness > 255)
        s.brightness = 255;

    /* TIMED 闪烁：只存下 on/off 时长等目标状态，不启动任何定时器。
     * 真实实现由 GPIO/PWM 定时器按 flash_on_ms/flash_off_ms 翻转电平。 */
    priv->states[id] = s;

    printf("[light] id=%u color=0x%06x bright=%u flash=%u\n", id, s.color_rgb, s.brightness,
           s.flash_mode);
    return 0;
}

static int host_light_get_light(light_device_t *dev, uint32_t id, light_state_t *state) {
    host_light_priv_t *priv = (host_light_priv_t *)dev->priv;

    if (state == NULL)
        return -EINVAL;
    if (host_light_check_id(id) != 0)
        return -EINVAL;

    *state = priv->states[id];
    return 0;
}

static const light_device_ops_t host_light_ops = {
    .get_capabilities = host_light_get_capabilities,
    .set_light = host_light_set_light,
    .get_light = host_light_get_light,
};

/* ---------------------------------------------------------------------------
 * 设备生命周期
 * ------------------------------------------------------------------------- */

static int host_light_close(hw_device_t *device) {
    light_device_t *dev = (light_device_t *)device;

    if (dev == NULL)
        return 0;
    free(dev->priv);
    free(dev);
    return 0;
}

static int host_light_open(const hw_module_t *module, const char *id, hw_device_t **device) {
    light_device_t *dev;
    host_light_priv_t *priv;

    (void)id;
    if (device == NULL)
        return -EINVAL;

    dev = (light_device_t *)calloc(1, sizeof(*dev));
    priv = (host_light_priv_t *)calloc(1, sizeof(*priv));
    if (dev == NULL || priv == NULL) {
        free(dev);
        free(priv);
        return -ENOMEM;
    }

    /* states 由 calloc 清零：默认全灭、常亮模式，无需再初始化 */

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (hw_module_t *)module;
    dev->common.close = host_light_close;
    dev->ops = &host_light_ops;
    dev->priv = priv;

    *device = (hw_device_t *)dev;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 模块导出
 * ------------------------------------------------------------------------- */

static struct hw_module_methods_t host_light_methods = {
    .open = host_light_open,
};

struct hw_module_t HMI_light = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = LIGHT_MODULE_API_VERSION_1_0,
    .hal_api_version = HARDWARE_API_VERSION_1_0,
    .id = LIGHT_HARDWARE_MODULE_ID,
    .name = "DarkOS Host Light HAL (software emulated)",
    .author = "DarkOS",
    .methods = &host_light_methods,
};
