#include "vs_sys_guard.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <vs_mal_sys.h>
#include <vs_mal_vbm.h>

static pthread_mutex_t g_vs_sys_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_vs_sys_users;

static uint64_t align_up_u64(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1u) / alignment * alignment;
}

int vs_sys_acquire(uint32_t max_width, uint32_t max_height) {
    vs_vb_cfg_s cfg;
    uint64_t y_stride;
    uint64_t frame_size;
    int rc = 0;

    pthread_mutex_lock(&g_vs_sys_lock);
    if (g_vs_sys_users != 0) {
        ++g_vs_sys_users;
        pthread_mutex_unlock(&g_vs_sys_lock);
        return 0;
    }

    /* SYS/VB is process-global and cannot be enlarged after another HAL has
     * acquired it.  Reserve for the largest supported VS816 camera route even
     * when a 1080p codec instance happens to be opened first. */
    if (max_width < 3840)
        max_width = 3840;
    if (max_height < 2160)
        max_height = 2160;
    y_stride = align_up_u64(max_width, 64);
    /* Two bytes per pixel also accommodates unpacked RAW12/RAW16 sensor
     * buffers; NV12 consumers simply use the leading 3/4 of each block. */
    frame_size = align_up_u64(y_stride * max_height * 2u, 4096);

    memset(&cfg, 0, sizeof(cfg));
    cfg.pool_cnt = 1;
    cfg.ast_commpool[0].blk_size = frame_size;
    /* Orion samples add five buffers to the normal pipeline budget. Sixteen
     * blocks cover VII, VENC staging and one display consumer for the v1 HAL. */
    cfg.ast_commpool[0].blk_cnt = 16;
    cfg.ast_commpool[0].remap_mode = VB_REMAP_MODE_NONE;

    if (vs_mal_vb_cfg_set(&cfg) != VS_SUCCESS ||
        vs_mal_vb_init() != VS_SUCCESS) {
        fprintf(stderr, "vs_sys_guard: VB initialization failed\n");
        rc = -1;
        goto out;
    }
    if (vs_mal_sys_init() != VS_SUCCESS) {
        fprintf(stderr, "vs_sys_guard: SYS initialization failed\n");
        vs_mal_vb_exit();
        rc = -1;
        goto out;
    }
    g_vs_sys_users = 1;
out:
    pthread_mutex_unlock(&g_vs_sys_lock);
    return rc;
}

void vs_sys_release(void) {
    pthread_mutex_lock(&g_vs_sys_lock);
    if (g_vs_sys_users != 0 && --g_vs_sys_users == 0) {
        vs_mal_sys_exit();
        vs_mal_vb_exit();
    }
    pthread_mutex_unlock(&g_vs_sys_lock);
}
