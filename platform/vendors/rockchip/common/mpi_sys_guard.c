/*
 * rockit RK_MPI_SYS_Init/Exit 进程级守卫：见头文件注释。
 */

#include "mpi_sys_guard.h"

#include <errno.h>
#include <pthread.h>
#include <rk_mpi_sys.h>
#include <stdio.h>

#include "rockit_log.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_refcount = 0;

int rk_mpi_sys_acquire(void) {
    int rc = 0;
    int log_rc;

    pthread_mutex_lock(&g_lock);
    if (g_refcount == 0) {
        /* 部署环境可用 rt_log_level 覆盖；默认值不能成为 HAL 启动失败条件。 */
        log_rc = rockit_log_set_default_startup_level(0);
        if (log_rc != 0)
            fprintf(stderr, "mpi_sys_guard: set Rockit default log level failed: %d\n", log_rc);

        if (RK_MPI_SYS_Init() != 0) {
            fprintf(stderr, "mpi_sys_guard: RK_MPI_SYS_Init failed\n");
            rc = -EIO;
        } else {
            g_refcount++;
        }
    } else {
        g_refcount++;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

void rk_mpi_sys_release(void) {
    pthread_mutex_lock(&g_lock);
    if (g_refcount > 0 && --g_refcount == 0)
        RK_MPI_SYS_Exit();
    pthread_mutex_unlock(&g_lock);
}
