/*
 * 主机参考实现（host_x86 变体）：无硬件依赖的蓝牙适配器模拟。
 *
 * 用途：在宿主机上验证 bluetooth 接口语义（开关状态机、本机名称、
 *       设备发现回调），供 frameworks 配网流程联调，不依赖 BlueZ。
 *       真实 Classic/BLE 由 rockchip 实现（BlueZ over D-Bus）提供。
 *
 * 与 bluetooth.rockchip.so（待开发）实现同一套 bluetooth_device_ops，
 * 上层无感知。用环境变量切换：DARKOS_HAL_VARIANT=host_x86。
 *
 * 发现语义：start_discovery 起线程，每约 300ms 上报一个假设备
 * （2 个 BLE + 1 个 Classic），循环往复，直至 stop_discovery
 * 或回调返回非 0。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* pthread_setname_np */
#endif
#define _DEFAULT_SOURCE /* usleep 在 -std=c17 严格模式下需要特性宏（gnu17 下冗余，防御保留） */

#include <bluetooth/IBluetooth.h>
#include <hardware/hardware.h>

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HOST_BT_DEFAULT_NAME "DarkOS-IPC"
#define HOST_BT_DISCOVERY_INTERVAL_MS 300

typedef struct host_bt_priv {
    bt_state_t state;
    char name[BT_NAME_MAX_LEN + 1];

    bt_discovery_cb cb;
    void *cb_ctx;
    pthread_t thread;
    volatile int discovering;
    int thread_started; /* 线程已创建，stop 时需要 join */
} host_bt_priv_t;

/* ---------------------------------------------------------------------------
 * 假设备表与发现线程
 * ------------------------------------------------------------------------- */

static const bt_device_info_t host_bt_fake_devices[] = {
    {.addr = {0x24, 0x6f, 0x28, 0x11, 0x22, 0x33},
     .name = "DarkOS-Phone",
     .rssi = -48,
     .flags = BT_DEVICE_FLAG_BLE},
    {.addr = {0xc4, 0x7c, 0x8d, 0x44, 0x55, 0x66},
     .name = "BLE-Temp-Sensor",
     .rssi = -72,
     .flags = BT_DEVICE_FLAG_BLE},
    {.addr = {0x00, 0x1a, 0x7d, 0x77, 0x88, 0x99},
     .name = "BT-Speaker",
     .rssi = -60,
     .flags = 0},
};

#define HOST_BT_FAKE_DEVICE_COUNT (sizeof(host_bt_fake_devices) / sizeof(host_bt_fake_devices[0]))

/* 按毫秒分片 sleep，期间检查退出标志，使 stop 能及时响应 */
static void sleep_ms_interruptible(host_bt_priv_t *priv, unsigned ms) {
    unsigned i;
    for (i = 0; i < ms && priv->discovering; i++)
        usleep(1000);
}

static void *discovery_thread(void *arg) {
    host_bt_priv_t *priv = (host_bt_priv_t *)arg;
    size_t idx = 0;

    pthread_setname_np(pthread_self(), "bt.disc");

    while (priv->discovering) {
        const bt_device_info_t *dev = &host_bt_fake_devices[idx];
        if (priv->cb != NULL && priv->cb(priv->cb_ctx, dev) != 0)
            break; /* 上层要求提前结束发现 */
        idx = (idx + 1) % HOST_BT_FAKE_DEVICE_COUNT;
        sleep_ms_interruptible(priv, HOST_BT_DISCOVERY_INTERVAL_MS);
    }

    priv->discovering = 0;
    return NULL;
}

/* ---------------------------------------------------------------------------
 * 设备操作实现
 * ------------------------------------------------------------------------- */

static int host_bt_get_capabilities(bluetooth_device_t *dev, bt_caps_t *caps) {
    (void)dev;
    if (caps == NULL)
        return -EINVAL;
    memset(caps, 0, sizeof(*caps));
    caps->supported = BT_CAPS_CLASSIC | BT_CAPS_BLE; /* 以 BLE 配网为主，Classic 预留 */
    return 0;
}

static int host_bt_enable(bluetooth_device_t *dev) {
    host_bt_priv_t *priv = (host_bt_priv_t *)dev->priv;
    priv->state = BT_STATE_ON;
    return 0;
}

static int host_bt_disable(bluetooth_device_t *dev) {
    host_bt_priv_t *priv = (host_bt_priv_t *)dev->priv;

    /* 关闭适配器前先停掉进行中的发现 */
    if (priv->discovering)
        dev->ops->stop_discovery(dev);

    priv->state = BT_STATE_OFF;
    return 0;
}

static int host_bt_get_state(bluetooth_device_t *dev, uint32_t *state) {
    host_bt_priv_t *priv = (host_bt_priv_t *)dev->priv;
    if (state == NULL)
        return -EINVAL;
    *state = (uint32_t)priv->state;
    return 0;
}

static int host_bt_set_name(bluetooth_device_t *dev, const char *name) {
    host_bt_priv_t *priv = (host_bt_priv_t *)dev->priv;

    if (name == NULL)
        return -EINVAL;

    strncpy(priv->name, name, BT_NAME_MAX_LEN);
    priv->name[BT_NAME_MAX_LEN] = '\0'; /* 超长输入截断，保证以 NUL 结尾 */
    return 0;
}

static int host_bt_get_name(bluetooth_device_t *dev, char *name, uint32_t size) {
    host_bt_priv_t *priv = (host_bt_priv_t *)dev->priv;
    size_t len;

    if (name == NULL || size == 0)
        return -EINVAL;

    len = strlen(priv->name);
    if (len + 1 > size)
        len = size - 1; /* 缓冲不足时截断，仍保证 NUL 结尾 */
    memcpy(name, priv->name, len);
    name[len] = '\0';
    return 0;
}

static int host_bt_start_discovery(bluetooth_device_t *dev, bt_discovery_cb cb, void *ctx) {
    host_bt_priv_t *priv = (host_bt_priv_t *)dev->priv;
    int rc;

    if (cb == NULL)
        return -EINVAL;
    if (priv->state != BT_STATE_ON)
        return -EINVAL; /* 适配器未开启，不能发现 */
    if (priv->discovering)
        return -EBUSY;

    priv->cb = cb;
    priv->cb_ctx = ctx;
    priv->discovering = 1;
    rc = pthread_create(&priv->thread, NULL, discovery_thread, priv);
    if (rc != 0) {
        priv->discovering = 0;
        return -rc;
    }
    priv->thread_started = 1;
    return 0;
}

static int host_bt_stop_discovery(bluetooth_device_t *dev) {
    host_bt_priv_t *priv = (host_bt_priv_t *)dev->priv;

    if (!priv->discovering && !priv->thread_started)
        return 0;

    priv->discovering = 0;
    if (priv->thread_started) {
        pthread_join(priv->thread, NULL);
        priv->thread_started = 0;
    }
    return 0;
}

static const bluetooth_device_ops_t host_bt_ops = {
    .get_capabilities = host_bt_get_capabilities,
    .enable = host_bt_enable,
    .disable = host_bt_disable,
    .get_state = host_bt_get_state,
    .set_name = host_bt_set_name,
    .get_name = host_bt_get_name,
    .start_discovery = host_bt_start_discovery,
    .stop_discovery = host_bt_stop_discovery,
};

/* ---------------------------------------------------------------------------
 * 设备生命周期
 * ------------------------------------------------------------------------- */

static int host_bt_close(hw_device_t *device) {
    bluetooth_device_t *dev = (bluetooth_device_t *)device;
    host_bt_priv_t *priv;

    if (dev == NULL)
        return 0;
    priv = (host_bt_priv_t *)dev->priv;
    if (priv != NULL) {
        if (priv->discovering || priv->thread_started)
            host_bt_stop_discovery(dev);
        free(priv);
    }
    free(dev);
    return 0;
}

static int host_bt_open(const hw_module_t *module, const char *id, hw_device_t **device) {
    bluetooth_device_t *dev;
    host_bt_priv_t *priv;

    (void)id;
    if (device == NULL)
        return -EINVAL;

    dev = (bluetooth_device_t *)calloc(1, sizeof(*dev));
    priv = (host_bt_priv_t *)calloc(1, sizeof(*priv));
    if (dev == NULL || priv == NULL) {
        free(dev);
        free(priv);
        return -ENOMEM;
    }

    priv->state = BT_STATE_OFF;
    strncpy(priv->name, HOST_BT_DEFAULT_NAME, BT_NAME_MAX_LEN);

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (hw_module_t *)module;
    dev->common.close = host_bt_close;
    dev->ops = &host_bt_ops;
    dev->priv = priv;

    *device = (hw_device_t *)dev;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 模块导出
 * ------------------------------------------------------------------------- */

static struct hw_module_methods_t host_bt_methods = {
    .open = host_bt_open,
};

struct hw_module_t HMI_bluetooth = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = BLUETOOTH_MODULE_API_VERSION_1_0,
    .hal_api_version = HARDWARE_API_VERSION_1_0,
    .id = BLUETOOTH_HARDWARE_MODULE_ID,
    .name = "DarkOS Host Bluetooth HAL (synthetic)",
    .author = "DarkOS",
    .methods = &host_bt_methods,
};
