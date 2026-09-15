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
  setenv("DARKOS_HAL_LIBRARY_PATH", TEST_HAL_DIRECTORY, 1);

  if (hw_get_module("loader_test", &first) != 0 || first == NULL) {
    fprintf(stderr, "failed to load test module\n");
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
  if (hw_get_module("missing", &second) != -ENOENT) {
    fprintf(stderr, "missing module did not return -ENOENT\n");
    ++failures;
  }
  if (hw_get_module("../bad", &second) != -EINVAL) {
    fprintf(stderr, "invalid module id did not return -EINVAL\n");
    ++failures;
  }

  return failures == 0 ? 0 : 1;
}
