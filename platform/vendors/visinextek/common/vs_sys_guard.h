#ifndef DARKOS_VISINEXTEK_VS_SYS_GUARD_H
#define DARKOS_VISINEXTEK_VS_SYS_GUARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SYS/VB are process-global in the VS MAL. The first client fixes the common
 * pool size; later users share it until the final release. */
int vs_sys_acquire(uint32_t max_width, uint32_t max_height);
void vs_sys_release(void);

#ifdef __cplusplus
}
#endif

#endif
