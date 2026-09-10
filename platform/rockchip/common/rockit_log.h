#ifndef DARKOS_HAL_ROCKCHIP_ROCKIT_LOG_H
#define DARKOS_HAL_ROCKCHIP_ROCKIT_LOG_H

/*
 * Rockit 私有日志控制。
 *
 * startup level 对应 Rockit 的 rt_log_level 环境变量，必须在首次调用 Rockit
 * API 前设置；module level 通过 Rockit 监听的 /tmp/rt_log_level 动态生效。
 * 该接口只供 hal.rockchip.so 内部使用，不向 SvcKit 或产品层暴露。
 */

#ifdef __cplusplus
extern "C" {
#endif

/* 设置 Rockit 启动日志等级（0..6），成功返回 0，失败返回负 errno。 */
int rockit_log_set_startup_level(int level);

/* rt_log_level 未由部署环境指定时设置默认等级；已有值不覆盖。 */
int rockit_log_set_default_startup_level(int level);

/* 动态设置模块日志等级（模块名见实现中的白名单，level 为 0..6）。 */
int rockit_log_set_module_level(const char *module, int level);

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HAL_ROCKCHIP_ROCKIT_LOG_H */
