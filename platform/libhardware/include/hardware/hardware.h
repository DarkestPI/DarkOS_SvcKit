#ifndef DARKOS_HARDWARE_HARDWARE_H
#define DARKOS_HARDWARE_HARDWARE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HARDWARE_MAKE_TAG_CONSTANT(a, b, c, d)                                 \
  ((((uint32_t)(a)) << 24) | (((uint32_t)(b)) << 16) |                         \
   (((uint32_t)(c)) << 8) | ((uint32_t)(d)))

#define HARDWARE_MODULE_TAG HARDWARE_MAKE_TAG_CONSTANT('H', 'W', 'M', 'T')
#define HARDWARE_DEVICE_TAG HARDWARE_MAKE_TAG_CONSTANT('H', 'W', 'D', 'T')

#define HARDWARE_MAKE_API_VERSION(major, minor)                                \
  ((((uint16_t)(major)) << 8) | ((uint16_t)(minor)))
#define HARDWARE_API_VERSION_1_0 HARDWARE_MAKE_API_VERSION(1, 0)

typedef struct hw_module_t hw_module_t;
typedef struct hw_device_t hw_device_t;

typedef struct hw_module_methods_t {
  int (*open)(const hw_module_t *module, const char *id, hw_device_t **device);
} hw_module_methods_t;

struct hw_module_t {
  uint32_t tag;
  uint16_t module_api_version;
  uint16_t hal_api_version;
  const char *id;
  const char *name;
  const char *author;
  const hw_module_methods_t *methods;
  void *dso;
  uint64_t reserved[8];
};

struct hw_device_t {
  uint32_t tag;
  uint32_t version;
  hw_module_t *module;
  int (*close)(hw_device_t *device);
  uint64_t reserved[8];
};

/*
 * 从 hal.<variant>.so 加载 HMI_<id>。
 *
 * variant 优先取 DARKOS_HAL_VARIANT，未设置时使用构建目标的默认值；搜索目录
 * 优先取 DARKOS_HAL_LIBRARY_PATH（冒号分隔）。未设置时依次查找可执行文件旁边
 * 的 ../lib 和系统默认目录。返回 0 成功，失败返回负 errno。成功返回的 module
 * 在进程生命周期内有效。
 */
int hw_get_module(const char *id, const hw_module_t **module);

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_HARDWARE_H */
