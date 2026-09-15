#include <hardware/hardware.h>

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DARKOS_DEFAULT_HAL_VARIANT
#define DARKOS_DEFAULT_HAL_VARIANT ""
#endif

#define HAL_MAX_ID_LENGTH 63
#define HAL_MAX_VARIANT_LENGTH 63
#define HAL_MAX_PATH_LENGTH 1023
#define HAL_MAX_CACHED_MODULES 32
#define HAL_DEFAULT_LIBRARY_PATH "/usr/lib/darkos/hal:/usr/lib:/lib"

typedef struct module_cache_entry {
  char id[HAL_MAX_ID_LENGTH + 1];
  char variant[HAL_MAX_VARIANT_LENGTH + 1];
  char search_path[HAL_MAX_PATH_LENGTH + 1];
  const hw_module_t *module;
} module_cache_entry_t;

static pthread_mutex_t g_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static module_cache_entry_t g_cache[HAL_MAX_CACHED_MODULES];
static size_t g_cache_size;

static int valid_component(const char *value, size_t maximum) {
  size_t length = 0;
  if (value == NULL || value[0] == '\0')
    return 0;
  while (value[length] != '\0') {
    const char character = value[length];
    if (!((character >= 'a' && character <= 'z') ||
          (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9') || character == '_' ||
          character == '-' || character == '.'))
      return 0;
    if (++length > maximum)
      return 0;
  }
  return 1;
}

static const hw_module_t *find_cached(const char *id, const char *variant,
                                      const char *search_path) {
  size_t index;
  for (index = 0; index < g_cache_size; ++index) {
    if (strcmp(g_cache[index].id, id) == 0 &&
        strcmp(g_cache[index].variant, variant) == 0 &&
        strcmp(g_cache[index].search_path, search_path) == 0)
      return g_cache[index].module;
  }
  return NULL;
}

static int load_from_directory(const char *directory, size_t directory_length,
                               const char *variant, const char *id,
                               const hw_module_t **module) {
  char library[HAL_MAX_PATH_LENGTH + 1];
  char symbol[HAL_MAX_ID_LENGTH + 5];
  hw_module_t *candidate;
  void *handle;
  int written;

  if (directory_length == 0)
    return -ENOENT;
  written = snprintf(library, sizeof(library), "%.*s/hal.%s.so",
                     (int)directory_length, directory, variant);
  if (written < 0 || (size_t)written >= sizeof(library))
    return -ENAMETOOLONG;

  handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
  if (handle == NULL)
    return -ENOENT;
  written = snprintf(symbol, sizeof(symbol), "HMI_%s", id);
  if (written < 0 || (size_t)written >= sizeof(symbol)) {
    dlclose(handle);
    return -ENAMETOOLONG;
  }
  dlerror();
  candidate = (hw_module_t *)dlsym(handle, symbol);
  if (candidate == NULL || dlerror() != NULL) {
    dlclose(handle);
    return -ENOENT;
  }
  if (candidate->tag != HARDWARE_MODULE_TAG || candidate->id == NULL ||
      strcmp(candidate->id, id) != 0 || candidate->methods == NULL ||
      candidate->methods->open == NULL ||
      (candidate->hal_api_version >> 8) != (HARDWARE_API_VERSION_1_0 >> 8)) {
    dlclose(handle);
    return -ELIBBAD;
  }
  candidate->dso = handle;
  *module = candidate;
  return 0;
}

int hw_get_module(const char *id, const hw_module_t **module) {
  const char *variant;
  const char *search_path;
  const char *cursor;
  const hw_module_t *cached;
  int result = -ENOENT;

  if (module == NULL || !valid_component(id, HAL_MAX_ID_LENGTH))
    return -EINVAL;
  *module = NULL;

  variant = getenv("DARKOS_HAL_VARIANT");
  if (variant == NULL || variant[0] == '\0')
    variant = DARKOS_DEFAULT_HAL_VARIANT;
  if (!valid_component(variant, HAL_MAX_VARIANT_LENGTH))
    return -EINVAL;

  search_path = getenv("DARKOS_HAL_LIBRARY_PATH");
  if (search_path == NULL || search_path[0] == '\0')
    search_path = HAL_DEFAULT_LIBRARY_PATH;
  if (strlen(search_path) > HAL_MAX_PATH_LENGTH)
    return -ENAMETOOLONG;

  pthread_mutex_lock(&g_cache_mutex);
  cached = find_cached(id, variant, search_path);
  if (cached != NULL) {
    *module = cached;
    pthread_mutex_unlock(&g_cache_mutex);
    return 0;
  }

  cursor = search_path;
  while (*cursor != '\0') {
    const char *separator = strchr(cursor, ':');
    const size_t length =
        separator != NULL ? (size_t)(separator - cursor) : strlen(cursor);
    result = load_from_directory(cursor, length, variant, id, module);
    if (result == 0 || result == -ELIBBAD || result == -ENAMETOOLONG)
      break;
    if (separator == NULL)
      break;
    cursor = separator + 1;
  }

  if (result == 0) {
    module_cache_entry_t *entry;
    if (g_cache_size >= HAL_MAX_CACHED_MODULES) {
      dlclose(((hw_module_t *)*module)->dso);
      ((hw_module_t *)*module)->dso = NULL;
      *module = NULL;
      pthread_mutex_unlock(&g_cache_mutex);
      return -ENOSPC;
    }
    entry = &g_cache[g_cache_size++];
    snprintf(entry->id, sizeof(entry->id), "%s", id);
    snprintf(entry->variant, sizeof(entry->variant), "%s", variant);
    snprintf(entry->search_path, sizeof(entry->search_path), "%s", search_path);
    entry->module = *module;
  }
  pthread_mutex_unlock(&g_cache_mutex);
  return result;
}
