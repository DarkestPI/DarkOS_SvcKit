/*
 * Rockchip RV1126B Light HAL：基于 sysfs GPIO 的补光/状态灯实现。
 *
 * IPC 相机的灯：状态指示灯、红外补光（夜视）、白光补光（全彩夜视/警戒），
 * 对应 hardware/interfaces/light 的 LIGHT_ID_STATUS / IR / WHITE。
 *
 * 引脚是板级设计，经环境变量配置（内核 GPIO 编号）：
 *   DARKOS_LIGHT_IR_PIN        红外补光灯 GPIO（未配置则 caps 不含 IR）
 *   DARKOS_LIGHT_WHITE_PIN     白光补光灯 GPIO（未配置则 caps 不含 WHITE）
 *   DARKOS_LIGHT_STATUS_PIN    状态灯 GPIO（未配置则 caps 不含 STATUS）
 *   DARKOS_LIGHT_<X>_ACTIVE_LOW=1  可选，低电平点亮（默认高电平点亮）
 *
 * 语义说明：
 *   - GPIO 无 PWM：brightness==0 灭、>0 亮；
 *   - LIGHT_FLASH_TIMED 只记录目标状态（get_light 可读回），不跑软件闪烁
 *     定时器——与 host_x86 模拟语义一致；真闪烁需求出现时再加定时线程；
 *   - 依赖 /sys/class/gpio（vendor 内核默认开启）；引脚编号与有效电平
 *     需按实际板型验证。
 */

#include <hardware/hardware.h>
#include <light/ILight.h>

#include "gpio_sysfs.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RK_LIGHT_COUNT 3 /* STATUS/IR/WHITE，按 light_id_t 索引 */

typedef struct rk_light_gpio {
    int pin;        /* sysfs GPIO 编号，<0 表示未配置 */
    int active_low; /* 1=低电平点亮 */
    int exported;   /* 已 export + 设方向 */
} rk_light_gpio_t;

typedef struct rk_light_priv {
    rk_light_gpio_t gpios[RK_LIGHT_COUNT];
    light_state_t states[RK_LIGHT_COUNT]; /* 每盏灯最近一次设置的状态 */
} rk_light_priv_t;

/* env 读取引脚配置；未设置/非法返回 -1 */
static int rk_light_env_pin(const char *light_name) {
    char env_name[64];
    const char *val;
    long pin;
    char *end;

    snprintf(env_name, sizeof(env_name), "DARKOS_LIGHT_%s_PIN", light_name);
    val = getenv(env_name);
    if (val == NULL || val[0] == '\0')
        return -1;
    pin = strtol(val, &end, 10);
    if (*end != '\0' || pin < 0)
        return -1;
    return (int)pin;
}

static int rk_light_env_active_low(const char *light_name) {
    char env_name[64];
    const char *val;

    snprintf(env_name, sizeof(env_name), "DARKOS_LIGHT_%s_ACTIVE_LOW", light_name);
    val = getenv(env_name);
    return (val != NULL && strcmp(val, "1") == 0) ? 1 : 0;
}

/* export 并设为输出方向（已 export 过时直接设方向）。幂等。 */
static int rk_light_gpio_prepare(rk_light_gpio_t *gpio) {
    int rc;

    if (gpio->exported)
        return 0;

    rc = linux_gpio_sysfs_prepare_output(gpio->pin);
    if (rc != 0)
        return rc;
    gpio->exported = 1;
    return 0;
}

static int rk_light_gpio_set(rk_light_gpio_t *gpio, int on) {
    int level = on ? !gpio->active_low : gpio->active_low;
    int rc;

    rc = rk_light_gpio_prepare(gpio);
    if (rc != 0)
        return rc;
    return linux_gpio_sysfs_write_value(gpio->pin, level);
}

/* ---------------------------------------------------------------------------
 * 设备操作实现
 * ------------------------------------------------------------------------- */

static int rk_light_get_capabilities(light_device_t *dev, light_caps_t *caps) {
    rk_light_priv_t *priv = (rk_light_priv_t *)dev->priv;
    uint32_t i;

    if (caps == NULL)
        return -EINVAL;
    memset(caps, 0, sizeof(*caps));
    for (i = 0; i < RK_LIGHT_COUNT; i++) {
        if (priv->gpios[i].pin >= 0)
            caps->supported_lights |= (1u << i);
    }
    return 0;
}

static int rk_light_set_light(light_device_t *dev, uint32_t id, const light_state_t *state) {
    rk_light_priv_t *priv = (rk_light_priv_t *)dev->priv;
    int rc;

    if (state == NULL || id >= RK_LIGHT_COUNT)
        return -EINVAL;
    if (priv->gpios[id].pin < 0)
        return -ENOTSUP; /* 该灯未配置引脚 */

    rc = rk_light_gpio_set(&priv->gpios[id], state->brightness > 0);
    if (rc != 0)
        return rc;
    priv->states[id] = *state;
    return 0;
}

static int rk_light_get_light(light_device_t *dev, uint32_t id, light_state_t *state) {
    rk_light_priv_t *priv = (rk_light_priv_t *)dev->priv;

    if (state == NULL || id >= RK_LIGHT_COUNT)
        return -EINVAL;
    if (priv->gpios[id].pin < 0)
        return -ENOTSUP;
    *state = priv->states[id];
    return 0;
}

static const light_device_ops_t rk_light_ops = {
    .get_capabilities = rk_light_get_capabilities,
    .set_light = rk_light_set_light,
    .get_light = rk_light_get_light,
};

/* ---------------------------------------------------------------------------
 * 设备生命周期
 * ------------------------------------------------------------------------- */

static int rk_light_close(hw_device_t *device) {
    light_device_t *dev = (light_device_t *)device;

    if (dev == NULL)
        return 0;
    free(dev->priv);
    free(dev);
    return 0;
}

static int rk_light_open(const hw_module_t *module, const char *id, hw_device_t **device) {
    static const char *const k_names[RK_LIGHT_COUNT] = {"STATUS", "IR", "WHITE"};
    light_device_t *dev;
    rk_light_priv_t *priv;
    uint32_t i;

    if (device == NULL)
        return -EINVAL;
    if (id == NULL || strcmp(id, LIGHT_HARDWARE_MODULE_ID) != 0) /* 灯无多实例 */
        return -EINVAL;

    dev = (light_device_t *)calloc(1, sizeof(*dev));
    priv = (rk_light_priv_t *)calloc(1, sizeof(*priv));
    if (dev == NULL || priv == NULL) {
        free(dev);
        free(priv);
        return -ENOMEM;
    }

    for (i = 0; i < RK_LIGHT_COUNT; i++) {
        priv->gpios[i].pin = rk_light_env_pin(k_names[i]);
        priv->gpios[i].active_low = rk_light_env_active_low(k_names[i]);
    }

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = LIGHT_DEVICE_API_VERSION_1_0;
    dev->common.module = (hw_module_t *)module;
    dev->common.close = rk_light_close;
    dev->ops = &rk_light_ops;
    dev->priv = priv;

    *device = (hw_device_t *)dev;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 模块导出（hal.rockchip.rv1126b.so，dlsym("HMI_light")）
 * ------------------------------------------------------------------------- */

static struct hw_module_methods_t rk_light_methods = {
    .open = rk_light_open,
};

struct hw_module_t HMI_light = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = LIGHT_MODULE_API_VERSION_1_0,
    .hal_api_version = HARDWARE_API_VERSION_1_0,
    .id = LIGHT_HARDWARE_MODULE_ID,
    .name = "Rockchip RV1126B Light HAL (sysfs GPIO)",
    .author = "DarkOS",
    .methods = &rk_light_methods,
};
