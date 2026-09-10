#ifndef DARKOS_HAL_ROCKCHIP_MPI_SYS_GUARD_H
#define DARKOS_HAL_ROCKCHIP_MPI_SYS_GUARD_H

/* ---------------------------------------------------------------------------
 * rockit RK_MPI_SYS_Init/Exit 进程级守卫（hal.rockchip.so 内多模块共享）
 *
 * camera(VI) / codec(VENC/VDEC) / audio(AI/AO) / display(VO) 各模块的
 * start/stop 时机相互独立，而 SYS_Init/Exit 是进程级的：任一模块在用就不能
 * Exit。用引用计数串行化：首个 acquire 执行 SYS_Init，计数归零才 SYS_Exit。
 * ------------------------------------------------------------------------- */

#ifdef __cplusplus
extern "C" {
#endif

/* 引用计数 +1；首个调用者执行 RK_MPI_SYS_Init。返回 0 成功，负 errno 失败 */
int rk_mpi_sys_acquire(void);

/* 引用计数 -1；归零时执行 RK_MPI_SYS_Exit。与 acquire 成对调用 */
void rk_mpi_sys_release(void);

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HAL_ROCKCHIP_MPI_SYS_GUARD_H */
