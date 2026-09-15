#define _POSIX_C_SOURCE 200809L

#include <hardware/hardware.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TEST_HAL_DIRECTORY
#error TEST_HAL_DIRECTORY must be defined
#endif

int main(void) {
  const hw_module_t *first = NULL;
  const hw_module_t *second = NULL;
  int failures = 0;

  setenv("DARKOS_HAL_VARIANT", "loader_test", 1);
  unsetenv("DARKOS_HAL_LIBRARY_PATH");

  if (hw_get_module("loader_test", &first) != 0 || first == NULL) {
    fprintf(stderr, "failed to load test module from executable-relative lib\n");
    ++failures;
  }
  if (hw_get_module("loader_test", &second) != 0 || second != first) {
    fprintf(stderr, "module cache did not return the same module\n");
    ++failures;
  }
  if (first != NULL && (strcmp(first->name, "DarkOS loader test HAL") != 0 ||
                        first->dso == NULL)) {
    fprintf(stderr, "loaded module metadata is invalid\n");
    ++failures;
  }

  /* 显式路径仍具有最高优先级，并参与缓存键。 */
  setenv("DARKOS_HAL_LIBRARY_PATH", TEST_HAL_DIRECTORY, 1);
  if (hw_get_module("loader_test", &second) != 0 || second == NULL) {
    fprintf(stderr, "failed to load test module from explicit path\n");
    ++failures;
  }
  if (hw_get_module("missing", &second) != -ENOENT) {
    fprintf(stderr, "missing module did not return -ENOENT\n");
    ++failures;
  }
  if (hw_get_module("../bad", &second) != -EINVAL) {
    fprintf(stderr, "invalid module id did not return -EINVAL\n");
    ++failures;
  }

  /* 版本兼容语义：主版本一致且实现 minor >= 调用方 minor */
  if (!hw_version_compatible(HARDWARE_MAKE_API_VERSION(1, 0),
                             HARDWARE_MAKE_API_VERSION(1, 0)) ||
      !hw_version_compatible(HARDWARE_MAKE_API_VERSION(1, 2),
                             HARDWARE_MAKE_API_VERSION(1, 0)) ||
      hw_version_compatible(HARDWARE_MAKE_API_VERSION(1, 0),
                            HARDWARE_MAKE_API_VERSION(1, 2)) ||
      hw_version_compatible(HARDWARE_MAKE_API_VERSION(2, 0),
                            HARDWARE_MAKE_API_VERSION(1, 0)) ||
      hw_version_compatible(HARDWARE_MAKE_API_VERSION(1, 0),
                            HARDWARE_MAKE_API_VERSION(2, 0))) {
    fprintf(stderr, "hw_version_compatible semantics are wrong\n");
    ++failures;
  }
  /* fake 模块声明 1.0：满足 1.0 需求，不满足 1.1 / 2.0 */
  if (first != NULL &&
      (!hw_module_supports(first, HARDWARE_MAKE_API_VERSION(1, 0)) ||
       hw_module_supports(first, HARDWARE_MAKE_API_VERSION(1, 1)) ||
       hw_module_supports(first, HARDWARE_MAKE_API_VERSION(2, 0)))) {
    fprintf(stderr, "hw_module_supports did not match fake module version\n");
    ++failures;
  }

  return failures == 0 ? 0 : 1;
}
