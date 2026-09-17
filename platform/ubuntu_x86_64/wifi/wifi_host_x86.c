/*
 * 主机参考实现（host_x86 变体）：无硬件依赖，模拟 STA 扫描/连接。
 *
 * 用途：在宿主机上验证 wifi 接口语义（caps 查询、扫描回调、连接状态机、
 *       状态查询），供 frameworks 上层联调，不依赖 wpa_supplicant。
 *       真实 STA/AP 由 rockchip 实现（wpa_supplicant / hostapd）提供。
 *
 * 与 wifi.rockchip.so（待开发）实现同一套 wifi_device_ops，上层无感知。
 * 用环境变量切换：DARKOS_HAL_VARIANT=host_x86。
 *
 * 模拟行为：内置 3 个假热点，scan 以 100ms 间隔逐个经回调上报；
 * connect 仅当 ssid 命中假热点表且（热点为 OPEN 或 psk 非空）时成功。
 */

#define _POSIX_C_SOURCE 200809L    /* clock_gettime 在 -std=c17 严格模式下需要特性宏（gnu17 下冗余，防御保留） */
#define _DEFAULT_SOURCE            /* usleep 同上（BSD 扩展） */

#include <hardware/hardware.h>
#include <wifi/IWifi.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* 扫描上报间隔（毫秒） */
#define HOST_WIFI_SCAN_INTERVAL_MS 100

/* 连接成功后上报的假 IP */
#define HOST_WIFI_FAKE_IP "192.168.1.100"

typedef struct host_wifi_priv {
    int enabled;
    uint32_t state;                  /* wifi_state_t */
    char ssid[WIFI_SSID_MAX_LEN + 1]; /* 已连接热点 ssid */
    int32_t rssi;                    /* 已连接热点 rssi（dBm） */
    uint32_t frequency_mhz;          /* 已连接热点频率 */
} host_wifi_priv_t;

/* 内置假热点表：模拟周围无线环境 */
static const wifi_ap_info_t host_wifi_fake_aps[] = {
    {.ssid = "DarkOS-Office",
     .bssid = {0x02, 0x11, 0x22, 0x33, 0x44, 0x01},
     .rssi = -45,
     .frequency_mhz = 2437,
     .security = WIFI_SECURITY_WPA2_PSK},
    {.ssid = "DarkOS-Guest",
     .bssid = {0x02, 0x11, 0x22, 0x33, 0x44, 0x02},
     .rssi = -60,
     .frequency_mhz = 2437,
     .security = WIFI_SECURITY_WPA2_PSK},
    {.ssid = "CMCC-Free",
     .bssid = {0x02, 0x11, 0x22, 0x33, 0x44, 0x03},
     .rssi = -75,
     .frequency_mhz = 5180,
     .security = WIFI_SECURITY_OPEN},
};

#define HOST_WIFI_FAKE_AP_COUNT (sizeof(host_wifi_fake_aps) / sizeof(host_wifi_fake_aps[0]))

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* ---------------------------------------------------------------------------
 * 设备操作实现
 * ------------------------------------------------------------------------- */

static int host_wifi_get_capabilities(wifi_device_t *dev, wifi_caps_t *caps) {
    (void)dev;
    if (caps == NULL)
        return -EINVAL;
    memset(caps, 0, sizeof(*caps));
    caps->supported_modes = WIFI_CAPS_MODE_STA;
    caps->supported_bands = WIFI_CAPS_BAND_2G4;
    return 0;
}

static int host_wifi_scan(wifi_device_t *dev, wifi_scan_cb cb, void *ctx, int timeout_ms) {
    uint64_t start;
    size_t i;

    host_wifi_priv_t *priv = (host_wifi_priv_t *)dev->priv;
    if (cb == NULL)
        return -EINVAL;
    if (!priv->enabled)
        return -ENETDOWN;

    start = now_ms();
    for (i = 0; i < HOST_WIFI_FAKE_AP_COUNT; i++) {
        /* 每次上报前检查累计耗时，超时即停（timeout_ms < 0 表示不限时） */
        if (timeout_ms >= 0 && now_ms() - start >= (uint64_t)timeout_ms)
            break;
        if (cb(ctx, &host_wifi_fake_aps[i]) != 0)
            break; /* cb 返回非 0，上层要求提前结束 */
        if (i + 1 < HOST_WIFI_FAKE_AP_COUNT)
            usleep(HOST_WIFI_SCAN_INTERVAL_MS * 1000);
    }
    return 0;
}

static int host_wifi_connect(wifi_device_t *dev, const wifi_config_t *cfg) {
    host_wifi_priv_t *priv = (host_wifi_priv_t *)dev->priv;
    size_t i;

    if (cfg == NULL)
        return -EINVAL;
    if (!priv->enabled)
        return -ENETDOWN;

    for (i = 0; i < HOST_WIFI_FAKE_AP_COUNT; i++) {
        const wifi_ap_info_t *ap = &host_wifi_fake_aps[i];
        if (strncmp(cfg->ssid, ap->ssid, WIFI_SSID_MAX_LEN) != 0)
            continue;
        /* OPEN 热点直连；加密热点要求 psk 非空 */
        if (ap->security != WIFI_SECURITY_OPEN && cfg->psk[0] == '\0')
            return -ENOENT;
        priv->state = WIFI_STATE_CONNECTED;
        strncpy(priv->ssid, ap->ssid, WIFI_SSID_MAX_LEN);
        priv->ssid[WIFI_SSID_MAX_LEN] = '\0';
        priv->rssi = ap->rssi;
        priv->frequency_mhz = ap->frequency_mhz;
        return 0;
    }
    return -ENOENT; /* ssid 不在假热点表中 */
}

static int host_wifi_disconnect(wifi_device_t *dev) {
    host_wifi_priv_t *priv = (host_wifi_priv_t *)dev->priv;
    priv->state = WIFI_STATE_DISCONNECTED;
    priv->ssid[0] = '\0';
    priv->rssi = 0;
    priv->frequency_mhz = 0;
    return 0;
}

static int host_wifi_enable(wifi_device_t *dev) {
    host_wifi_priv_t *priv = (host_wifi_priv_t *)dev->priv;
    priv->enabled = 1;
    return 0;
}

static int host_wifi_disable(wifi_device_t *dev) {
    host_wifi_priv_t *priv = (host_wifi_priv_t *)dev->priv;
    host_wifi_disconnect(dev);
    priv->enabled = 0;
    return 0;
}

static int host_wifi_get_status(wifi_device_t *dev, wifi_status_t *status) {
    host_wifi_priv_t *priv = (host_wifi_priv_t *)dev->priv;

    if (status == NULL)
        return -EINVAL;
    memset(status, 0, sizeof(*status));
    status->state = priv->state;
    if (priv->state == WIFI_STATE_CONNECTED) {
        strncpy(status->ssid, priv->ssid, WIFI_SSID_MAX_LEN);
        status->ssid[WIFI_SSID_MAX_LEN] = '\0';
        status->rssi = priv->rssi;
        status->frequency_mhz = priv->frequency_mhz;
        strncpy(status->ip_addr, HOST_WIFI_FAKE_IP, sizeof(status->ip_addr) - 1);
        status->ip_addr[sizeof(status->ip_addr) - 1] = '\0';
    }
    /* 未连接时 ssid/ip_addr 保持空串（memset 已保证） */
    return 0;
}

static const wifi_device_ops_t host_wifi_ops = {
    .get_capabilities = host_wifi_get_capabilities,
    .wifi_enable = host_wifi_enable,
    .wifi_get_status = host_wifi_get_status,
    .wifi_disable = host_wifi_disable,
    .scan = host_wifi_scan,
    .connect = host_wifi_connect,
    .disconnect = host_wifi_disconnect,
    .get_status = host_wifi_get_status,
};

/* ---------------------------------------------------------------------------
 * 设备生命周期
 * ------------------------------------------------------------------------- */

static int host_wifi_close(hw_device_t *device) {
    wifi_device_t *dev = (wifi_device_t *)device;

    if (dev == NULL)
        return 0;
    free(dev->priv);
    free(dev);
    return 0;
}

static int host_wifi_open(const hw_module_t *module, const char *id, hw_device_t **device) {
    wifi_device_t *dev;
    host_wifi_priv_t *priv;

    (void)id;
    if (device == NULL)
        return -EINVAL;

    dev = (wifi_device_t *)calloc(1, sizeof(*dev));
    priv = (host_wifi_priv_t *)calloc(1, sizeof(*priv));
    if (dev == NULL || priv == NULL) {
        free(dev);
        free(priv);
        return -ENOMEM;
    }

    priv->state = WIFI_STATE_DISCONNECTED;

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = WIFI_DEVICE_API_VERSION_1_0;
    dev->common.module = (hw_module_t *)module;
    dev->common.close = host_wifi_close;
    dev->ops = &host_wifi_ops;
    dev->priv = priv;

    *device = (hw_device_t *)dev;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 模块导出
 * ------------------------------------------------------------------------- */

static struct hw_module_methods_t host_wifi_methods = {
    .open = host_wifi_open,
};

struct hw_module_t HMI_wifi = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = WIFI_MODULE_API_VERSION_1_0,
    .hal_api_version = HARDWARE_API_VERSION_1_0,
    .id = WIFI_HARDWARE_MODULE_ID,
    .name = "DarkOS Host Wifi HAL (simulated)",
    .author = "DarkOS",
    .methods = &host_wifi_methods,
};
