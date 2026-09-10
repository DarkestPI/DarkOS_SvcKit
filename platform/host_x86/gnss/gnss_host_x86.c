/*
 * 主机参考实现（host_x86 变体）：无硬件依赖，生成合成定位数据。
 *
 * 用途：在宿主机上开发/联调 frameworks、IPC 等上层，不依赖真实 GNSS
 *       模组（串口 AT/ubx）。真实定位由后续 rockchip/模组实现提供。
 *
 * 与 gnss.rockchip.so（待开发）实现同一套 gnss_device_ops，上层无感知。
 * 用环境变量切换：DARKOS_HAL_VARIANT=host_x86。
 *
 * 合成轨迹：起点 39.9042N, 116.4074E（北京），以 1Hz 每次向东微移
 * （约 1.5m/次，与 speed=1.5m/s、bearing=90° 自洽），flags 全开。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* pthread_setname_np */
#endif
/* 严格 ISO C 模式下暴露 POSIX/BSD 声明（clock_gettime / usleep）；
 * gnu17 下冗余，防御保留 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <gnss/IGnss.h>
#include <hardware/hardware.h>

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* 合成轨迹参数 */
#define HOST_GNSS_START_LAT 39.9042   /* 起点纬度（度） */
#define HOST_GNSS_START_LON 116.4074  /* 起点经度（度） */
#define HOST_GNSS_ALTITUDE_M 50.0     /* 固定海拔（米） */
#define HOST_GNSS_SPEED_MPS 1.5f      /* 固定速度（米/秒） */
#define HOST_GNSS_BEARING_DEG 90.0f   /* 固定航向：正东 */
#define HOST_GNSS_ACCURACY_M 3.0f     /* 固定水平精度（米） */
#define HOST_GNSS_PERIOD_MS 1000u     /* 上报周期：1Hz */

/* 该纬度下每米对应的经度增量：1 / (111320 * cos(39.9042°)) ≈ 1.171e-5 度/米，
 * 1Hz 下每次东移 1.5m（与 speed 自洽） */
#define HOST_GNSS_LON_STEP_DEG (1.5 * 1.171e-5)

typedef struct host_gnss_priv {
    gnss_location_cb cb;
    void *cb_ctx;
    pthread_t thread;
    volatile int running;
    int started;
    int cb_muted; /* cb 返回非 0 后置位：线程继续跑，但不再回调 */
    uint32_t seq; /* 已上报点数，驱动轨迹东移 */
} host_gnss_priv_t;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* 按毫秒分片 sleep，期间检查退出标志，使 stop 能及时响应 */
static void sleep_ms_interruptible(host_gnss_priv_t *priv, unsigned ms) {
    unsigned i;
    for (i = 0; i < ms && priv->running; i++)
        usleep(1000);
}

/* 按序号合成一个定位点：起点 + seq 步东移 */
static void fill_location(host_gnss_priv_t *priv, gnss_location_t *loc) {
    memset(loc, 0, sizeof(*loc));
    loc->flags = GNSS_LOCATION_HAS_LAT_LON | GNSS_LOCATION_HAS_ALTITUDE |
                 GNSS_LOCATION_HAS_SPEED | GNSS_LOCATION_HAS_BEARING |
                 GNSS_LOCATION_HAS_ACCURACY;
    loc->latitude_deg = HOST_GNSS_START_LAT;
    loc->longitude_deg = HOST_GNSS_START_LON + (double)priv->seq * HOST_GNSS_LON_STEP_DEG;
    loc->altitude_m = HOST_GNSS_ALTITUDE_M;
    loc->speed_mps = HOST_GNSS_SPEED_MPS;
    loc->bearing_deg = HOST_GNSS_BEARING_DEG;
    loc->accuracy_m = HOST_GNSS_ACCURACY_M;
    loc->timestamp_ns = now_ns();
}

/* ---------------------------------------------------------------------------
 * 定位上报线程（回调模型）
 * ------------------------------------------------------------------------- */

static void *location_thread(void *arg) {
    host_gnss_priv_t *priv = (host_gnss_priv_t *)arg;

    pthread_setname_np(pthread_self(), "gnss.loc");

    while (priv->running) {
        gnss_location_t loc;
        fill_location(priv, &loc);
        priv->seq++;

        /* cb 返回非 0 视为"暂停上报"：线程继续运行（保持定位状态机），
         * 仅置 cb_muted 丢弃后续回调；重新 set_location_callback 可恢复 */
        if (!priv->cb_muted && priv->cb != NULL) {
            if (priv->cb(priv->cb_ctx, &loc) != 0)
                priv->cb_muted = 1;
        }

        sleep_ms_interruptible(priv, HOST_GNSS_PERIOD_MS);
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * 设备操作实现
 * ------------------------------------------------------------------------- */

static int host_gnss_get_capabilities(gnss_device_t *dev, gnss_caps_t *caps) {
    (void)dev;
    if (caps == NULL)
        return -EINVAL;
    memset(caps, 0, sizeof(*caps));
    caps->supported_constellations = GNSS_CONSTELLATION_GPS | GNSS_CONSTELLATION_BEIDOU;
    caps->max_frequency_hz = 1;
    return 0;
}

static int host_gnss_set_location_callback(gnss_device_t *dev, gnss_location_cb cb, void *ctx) {
    host_gnss_priv_t *priv = (host_gnss_priv_t *)dev->priv;
    priv->cb = cb;
    priv->cb_ctx = ctx;
    priv->cb_muted = 0; /* 重新注册（含 start 之后）即恢复上报 */
    return 0;
}

static int host_gnss_start(gnss_device_t *dev) {
    host_gnss_priv_t *priv = (host_gnss_priv_t *)dev->priv;
    int rc;

    if (priv->started)
        return -EBUSY;

    /* 未注册回调也正常 start：线程照常跑，数据生成后被丢弃 */
    priv->started = 1;
    priv->running = 1;
    priv->seq = 0;
    rc = pthread_create(&priv->thread, NULL, location_thread, priv);
    if (rc != 0) {
        priv->running = 0;
        priv->started = 0;
        return -rc;
    }
    return 0;
}

static int host_gnss_stop(gnss_device_t *dev) {
    host_gnss_priv_t *priv = (host_gnss_priv_t *)dev->priv;

    if (!priv->started)
        return 0;

    priv->started = 0;
    priv->running = 0;
    pthread_join(priv->thread, NULL);
    return 0;
}

static const gnss_device_ops_t host_gnss_ops = {
    .get_capabilities = host_gnss_get_capabilities,
    .set_location_callback = host_gnss_set_location_callback,
    .start = host_gnss_start,
    .stop = host_gnss_stop,
};

/* ---------------------------------------------------------------------------
 * 设备生命周期
 * ------------------------------------------------------------------------- */

static int host_gnss_close(hw_device_t *device) {
    gnss_device_t *dev = (gnss_device_t *)device;
    host_gnss_priv_t *priv;

    if (dev == NULL)
        return 0;
    priv = (host_gnss_priv_t *)dev->priv;
    if (priv != NULL) {
        if (priv->started)
            host_gnss_stop(dev);
        free(priv);
    }
    free(dev);
    return 0;
}

static int host_gnss_open(const hw_module_t *module, const char *id, hw_device_t **device) {
    gnss_device_t *dev;
    host_gnss_priv_t *priv;

    (void)id;
    if (device == NULL)
        return -EINVAL;

    dev = (gnss_device_t *)calloc(1, sizeof(*dev));
    priv = (host_gnss_priv_t *)calloc(1, sizeof(*priv));
    if (dev == NULL || priv == NULL) {
        free(dev);
        free(priv);
        return -ENOMEM;
    }

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (hw_module_t *)module;
    dev->common.close = host_gnss_close;
    dev->ops = &host_gnss_ops;
    dev->priv = priv;

    *device = (hw_device_t *)dev;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 模块导出
 * ------------------------------------------------------------------------- */

static struct hw_module_methods_t host_gnss_methods = {
    .open = host_gnss_open,
};

struct hw_module_t HMI_gnss = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = GNSS_MODULE_API_VERSION_1_0,
    .hal_api_version = HARDWARE_API_VERSION_1_0,
    .id = GNSS_HARDWARE_MODULE_ID,
    .name = "DarkOS Host Gnss HAL (synthetic)",
    .author = "DarkOS",
    .methods = &host_gnss_methods,
};
