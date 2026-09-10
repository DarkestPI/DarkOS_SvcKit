/*
 * 主机参考实现（host_x86 变体）：无硬件依赖，软件模拟传感器数据源。
 *
 * 用途：在宿主机上验证 sensors 接口语义（枚举、activate/set_delay、
 *       阻塞 poll 超时），供 frameworks 上层联调，不依赖真实传感器。
 *
 * 与 sensors.rockchip.so（待开发）实现同一套 sensors_device_ops，上层无感知。
 * 用环境变量切换：DARKOS_HAL_VARIANT=host_x86。
 *
 * 内置 2 个传感器：
 *   - 光敏（handle 1，lux）：0~1000 lux 按时间正弦起伏，模拟昼夜变化；
 *   - 温度（handle 2，℃）：25℃ 附近伪随机缓漂，模拟缓慢热变化。
 */

#define _POSIX_C_SOURCE 200809L /* clock_gettime / pthread 在严格 ISO 模式下需要特性宏 */

#include <hardware/hardware.h>
#include <sensors/ISensors.h>

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HOST_SENSOR_COUNT 2
#define HOST_HANDLE_LIGHT 1
#define HOST_HANDLE_TEMPERATURE 2

#define HOST_LIGHT_MIN_DELAY_US 20000u      /* 50 Hz */
#define HOST_TEMP_MIN_DELAY_US 100000u      /* 10 Hz */
#define HOST_LIGHT_DEFAULT_DELAY_US 100000u /* 100 ms */
#define HOST_TEMP_DEFAULT_DELAY_US 500000u  /* 500 ms */

#define HOST_LIGHT_PERIOD_NS (60ull * 1000000000ull) /* 昼夜周期 60 s */

typedef struct host_sensor_slot {
    sensor_info_t info;
    int enabled;
    uint32_t delay_us;
    uint64_t next_due_ns; /* 下一次采样到期的 MONOTONIC 时间戳 */
    float value;          /* 温度缓漂的当前值（光敏按时间计算，不用） */
    uint32_t rand_state;  /* 温度缓漂用的 LCG 状态 */
} host_sensor_slot_t;

typedef struct host_sensors_priv {
    pthread_mutex_t lock;
    pthread_cond_t cond; /* activate 唤醒阻塞中的 poll */
    host_sensor_slot_t slots[HOST_SENSOR_COUNT];
} host_sensors_priv_t;

/* ---------------------------------------------------------------------------
 * 时间 / 数值辅助
 * ------------------------------------------------------------------------- */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void ns_to_abstime(uint64_t ns, struct timespec *ts) {
    ts->tv_sec = (time_t)(ns / 1000000000ull);
    ts->tv_nsec = (long)(ns % 1000000000ull);
}

/* 简易正弦（Taylor 展开，输入先约化到 [-π, π]），避免额外链接 libm */
static float host_sinf(float x) {
    const float pi = 3.14159265358979f;
    const float two_pi = 2.0f * pi;
    float x2, r;

    while (x > pi)
        x -= two_pi;
    while (x < -pi)
        x += two_pi;

    x2 = x * x;
    r = x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f *
                                                          (1.0f - x2 / 72.0f))));
    return r;
}

/* 光敏：0~1000 lux 按时间正弦起伏，模拟昼夜 */
static float simulate_light_lux(uint64_t ns) {
    float phase = (float)(ns % HOST_LIGHT_PERIOD_NS) / (float)HOST_LIGHT_PERIOD_NS;
    float s = host_sinf(phase * 2.0f * 3.14159265358979f);
    return 500.0f + 500.0f * s;
}

/* 温度：25℃ 附近伪随机缓漂（LCG，每步 ±0.05℃，钳位 [23, 27]） */
static float simulate_temperature_c(host_sensor_slot_t *slot) {
    float delta;

    slot->rand_state = slot->rand_state * 1103515245u + 12345u;
    delta = ((float)((slot->rand_state >> 16) & 0xffffu) / 65535.0f - 0.5f) * 0.1f;
    slot->value += delta;
    if (slot->value < 23.0f)
        slot->value = 23.0f;
    if (slot->value > 27.0f)
        slot->value = 27.0f;
    return slot->value;
}

/* ---------------------------------------------------------------------------
 * 设备操作实现
 * ------------------------------------------------------------------------- */

static int host_sensors_get_sensors_list(sensors_device_t *dev, sensor_info_t *list,
                                         uint32_t *count) {
    host_sensors_priv_t *priv = (host_sensors_priv_t *)dev->priv;
    uint32_t n, i;

    if (count == NULL)
        return -EINVAL;

    if (list == NULL) { /* 只查询数量 */
        *count = HOST_SENSOR_COUNT;
        return 0;
    }

    n = *count < HOST_SENSOR_COUNT ? *count : HOST_SENSOR_COUNT;
    pthread_mutex_lock(&priv->lock);
    for (i = 0; i < n; i++)
        list[i] = priv->slots[i].info;
    pthread_mutex_unlock(&priv->lock);
    *count = n;
    return 0;
}

static host_sensor_slot_t *find_slot(host_sensors_priv_t *priv, int32_t handle) {
    size_t i;
    for (i = 0; i < HOST_SENSOR_COUNT; i++)
        if (priv->slots[i].info.handle == handle)
            return &priv->slots[i];
    return NULL;
}

static int host_sensors_activate(sensors_device_t *dev, int32_t handle, int enabled) {
    host_sensors_priv_t *priv = (host_sensors_priv_t *)dev->priv;
    host_sensor_slot_t *slot;

    pthread_mutex_lock(&priv->lock);
    slot = find_slot(priv, handle);
    if (slot == NULL) {
        pthread_mutex_unlock(&priv->lock);
        return -EINVAL;
    }

    slot->enabled = enabled ? 1 : 0;
    if (slot->enabled) {
        slot->next_due_ns = now_ns() + (uint64_t)slot->delay_us * 1000ull;
        pthread_cond_broadcast(&priv->cond); /* 唤醒可能阻塞的 poll */
    }
    pthread_mutex_unlock(&priv->lock);
    return 0;
}

static int host_sensors_set_delay(sensors_device_t *dev, int32_t handle, uint32_t delay_us) {
    host_sensors_priv_t *priv = (host_sensors_priv_t *)dev->priv;
    host_sensor_slot_t *slot;

    pthread_mutex_lock(&priv->lock);
    slot = find_slot(priv, handle);
    if (slot == NULL || delay_us < slot->info.min_delay_us) {
        pthread_mutex_unlock(&priv->lock);
        return -EINVAL;
    }

    slot->delay_us = delay_us;
    if (slot->enabled) /* 激活中的按新间隔重新排期 */
        slot->next_due_ns = now_ns() + (uint64_t)delay_us * 1000ull;
    pthread_mutex_unlock(&priv->lock);
    return 0;
}

static void fill_event(host_sensor_slot_t *slot, uint64_t ts, sensor_event_t *ev) {
    memset(ev, 0, sizeof(*ev));
    ev->handle = slot->info.handle;
    ev->timestamp_ns = ts;
    if (slot->info.type == SENSOR_TYPE_LIGHT)
        ev->data[0] = simulate_light_lux(ts);
    else
        ev->data[0] = simulate_temperature_c(slot);
}

static int host_sensors_poll(sensors_device_t *dev, sensor_event_t *events,
                             uint32_t max_count, int timeout_ms) {
    host_sensors_priv_t *priv = (host_sensors_priv_t *)dev->priv;
    uint64_t deadline_ns = UINT64_MAX; /* timeout_ms < 0 表示无限等待 */
    int n = 0;

    if (events == NULL || max_count == 0)
        return -EINVAL;

    pthread_mutex_lock(&priv->lock);
    if (timeout_ms >= 0)
        deadline_ns = now_ns() + (uint64_t)timeout_ms * 1000000ull;

    for (;;) {
        uint64_t now = now_ns();
        uint64_t wake_ns = UINT64_MAX;
        uint64_t target;
        struct timespec abstime;
        size_t i;

        /* 找最近一个到期事件的时间 */
        for (i = 0; i < HOST_SENSOR_COUNT; i++)
            if (priv->slots[i].enabled && priv->slots[i].next_due_ns < wake_ns)
                wake_ns = priv->slots[i].next_due_ns;

        if (wake_ns == UINT64_MAX) {
            /* 无激活传感器：睡到超时返回 0（无限等待则等 activate 唤醒） */
            if (now >= deadline_ns)
                break;
            if (deadline_ns == UINT64_MAX) {
                pthread_cond_wait(&priv->cond, &priv->lock);
            } else {
                ns_to_abstime(deadline_ns, &abstime);
                pthread_cond_timedwait(&priv->cond, &priv->lock, &abstime);
            }
            continue;
        }

        if (wake_ns <= now) {
            /* 为所有到期且激活的传感器各填一个事件 */
            for (i = 0; i < HOST_SENSOR_COUNT && (uint32_t)n < max_count; i++) {
                host_sensor_slot_t *slot = &priv->slots[i];
                if (!slot->enabled || slot->next_due_ns > now)
                    continue;
                fill_event(slot, now, &events[n]);
                n++;
                slot->next_due_ns = now + (uint64_t)slot->delay_us * 1000ull;
            }
            break;
        }

        /* 睡到 min(最近到期, 超时) */
        target = wake_ns < deadline_ns ? wake_ns : deadline_ns;
        if (target <= now)
            break; /* 超时，无到期事件 */
        ns_to_abstime(target, &abstime);
        pthread_cond_timedwait(&priv->cond, &priv->lock, &abstime);
    }

    pthread_mutex_unlock(&priv->lock);
    return n;
}

static const sensors_device_ops_t host_sensors_ops = {
    .get_sensors_list = host_sensors_get_sensors_list,
    .activate = host_sensors_activate,
    .set_delay = host_sensors_set_delay,
    .poll = host_sensors_poll,
};

/* ---------------------------------------------------------------------------
 * 设备生命周期
 * ------------------------------------------------------------------------- */

static int host_sensors_close(hw_device_t *device) {
    sensors_device_t *dev = (sensors_device_t *)device;
    host_sensors_priv_t *priv;

    if (dev == NULL)
        return 0;
    priv = (host_sensors_priv_t *)dev->priv;
    if (priv != NULL) {
        pthread_mutex_destroy(&priv->lock);
        pthread_cond_destroy(&priv->cond);
        free(priv);
    }
    free(dev);
    return 0;
}

static void init_slot(host_sensor_slot_t *slot, int32_t handle, uint32_t type,
                      const char *name, float max_range, float resolution,
                      uint32_t min_delay_us, uint32_t default_delay_us, float init_value,
                      uint32_t rand_seed) {
    memset(slot, 0, sizeof(*slot));
    slot->info.handle = handle;
    slot->info.type = type;
    snprintf(slot->info.name, SENSOR_NAME_MAX_LEN, "%s", name);
    snprintf(slot->info.vendor, SENSOR_NAME_MAX_LEN, "%s", "DarkOS");
    slot->info.max_range = max_range;
    slot->info.resolution = resolution;
    slot->info.min_delay_us = min_delay_us;
    slot->delay_us = default_delay_us;
    slot->value = init_value;
    slot->rand_state = rand_seed;
}

static int host_sensors_open(const hw_module_t *module, const char *id, hw_device_t **device) {
    sensors_device_t *dev;
    host_sensors_priv_t *priv;

    (void)id;
    if (device == NULL)
        return -EINVAL;

    dev = (sensors_device_t *)calloc(1, sizeof(*dev));
    priv = (host_sensors_priv_t *)calloc(1, sizeof(*priv));
    if (dev == NULL || priv == NULL) {
        free(dev);
        free(priv);
        return -ENOMEM;
    }

    pthread_mutex_init(&priv->lock, NULL);
    pthread_cond_init(&priv->cond, NULL);

    init_slot(&priv->slots[0], HOST_HANDLE_LIGHT, SENSOR_TYPE_LIGHT, "host-light",
              1000.0f, 1.0f, HOST_LIGHT_MIN_DELAY_US, HOST_LIGHT_DEFAULT_DELAY_US,
              0.0f, 0x12345678u);
    init_slot(&priv->slots[1], HOST_HANDLE_TEMPERATURE, SENSOR_TYPE_TEMPERATURE,
              "host-temperature", 125.0f, 0.1f, HOST_TEMP_MIN_DELAY_US,
              HOST_TEMP_DEFAULT_DELAY_US, 25.0f, 0x9e3779b9u);

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (hw_module_t *)module;
    dev->common.close = host_sensors_close;
    dev->ops = &host_sensors_ops;
    dev->priv = priv;

    *device = (hw_device_t *)dev;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 模块导出
 * ------------------------------------------------------------------------- */

static struct hw_module_methods_t host_sensors_methods = {
    .open = host_sensors_open,
};

struct hw_module_t HMI_sensors = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = SENSORS_MODULE_API_VERSION_1_0,
    .hal_api_version = HARDWARE_API_VERSION_1_0,
    .id = SENSORS_HARDWARE_MODULE_ID,
    .name = "DarkOS Host Sensors HAL (simulated light/temperature)",
    .author = "DarkOS",
    .methods = &host_sensors_methods,
};
