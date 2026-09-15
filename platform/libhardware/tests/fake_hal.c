#include <hardware/hardware.h>

#include <errno.h>

static int fake_open(const hw_module_t *module, const char *id,
                     hw_device_t **device) {
  (void)module;
  (void)id;
  (void)device;
  return -ENOTSUP;
}

static hw_module_methods_t fake_methods = {
    .open = fake_open,
};

hw_module_t HMI_loader_test = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = HARDWARE_MAKE_API_VERSION(1, 0),
    .hal_api_version = HARDWARE_API_VERSION_1_0,
    .id = "loader_test",
    .name = "DarkOS loader test HAL",
    .author = "DarkOS",
    .methods = &fake_methods,
};
