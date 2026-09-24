#include <hardware/hardware.h>
#include <light/ILight.h>

#include "gpio_sysfs.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VS_LIGHT_COUNT 3

typedef struct vs_light_gpio {
    int pin;
    int active_low;
    int prepared;
} vs_light_gpio_t;

typedef struct vs_light_priv {
    vs_light_gpio_t gpio[VS_LIGHT_COUNT];
    light_state_t state[VS_LIGHT_COUNT];
} vs_light_priv_t;

static int env_pin(const char *name) {
    char key[64];
    char *end;
    const char *value;
    long pin;
    snprintf(key, sizeof(key), "DARKOS_LIGHT_%s_PIN", name);
    value = getenv(key);
    if (value == NULL || *value == '\0')
        return -1;
    pin = strtol(value, &end, 10);
    return (*end == '\0' && pin >= 0) ? (int)pin : -1;
}

static int env_active_low(const char *name) {
    char key[64];
    const char *value;
    snprintf(key, sizeof(key), "DARKOS_LIGHT_%s_ACTIVE_LOW", name);
    value = getenv(key);
    return value != NULL && strcmp(value, "1") == 0;
}

static int get_caps(light_device_t *dev, light_caps_t *caps) {
    vs_light_priv_t *priv = (vs_light_priv_t *)dev->priv;
    unsigned i;
    if (caps == NULL)
        return -EINVAL;
    memset(caps, 0, sizeof(*caps));
    for (i = 0; i < VS_LIGHT_COUNT; ++i)
        if (priv->gpio[i].pin >= 0)
            caps->supported_lights |= 1u << i;
    return 0;
}

static int set_light(light_device_t *dev, uint32_t id, const light_state_t *state) {
    vs_light_priv_t *priv = (vs_light_priv_t *)dev->priv;
    vs_light_gpio_t *gpio;
    int rc;
    if (state == NULL || id >= VS_LIGHT_COUNT)
        return -EINVAL;
    gpio = &priv->gpio[id];
    if (gpio->pin < 0)
        return -ENOTSUP;
    if (!gpio->prepared) {
        rc = linux_gpio_sysfs_prepare_output(gpio->pin);
        if (rc != 0)
            return rc;
        gpio->prepared = 1;
    }
    rc = linux_gpio_sysfs_write_value(
        gpio->pin, state->brightness > 0 ? !gpio->active_low : gpio->active_low);
    if (rc == 0)
        priv->state[id] = *state;
    return rc;
}

static int get_light(light_device_t *dev, uint32_t id, light_state_t *state) {
    vs_light_priv_t *priv = (vs_light_priv_t *)dev->priv;
    if (state == NULL || id >= VS_LIGHT_COUNT)
        return -EINVAL;
    if (priv->gpio[id].pin < 0)
        return -ENOTSUP;
    *state = priv->state[id];
    return 0;
}

static const light_device_ops_t ops = {
    .get_capabilities = get_caps, .set_light = set_light, .get_light = get_light,
};

static int close_device(hw_device_t *hw) {
    light_device_t *dev = (light_device_t *)hw;
    if (dev != NULL) {
        free(dev->priv);
        free(dev);
    }
    return 0;
}

static int open_device(const hw_module_t *module, const char *id, hw_device_t **out) {
    static const char *const names[VS_LIGHT_COUNT] = {"STATUS", "IR", "WHITE"};
    light_device_t *dev;
    vs_light_priv_t *priv;
    unsigned i;
    if (out == NULL || id == NULL || strcmp(id, LIGHT_HARDWARE_MODULE_ID) != 0)
        return -EINVAL;
    dev = calloc(1, sizeof(*dev));
    priv = calloc(1, sizeof(*priv));
    if (dev == NULL || priv == NULL) {
        free(dev); free(priv); return -ENOMEM;
    }
    for (i = 0; i < VS_LIGHT_COUNT; ++i) {
        priv->gpio[i].pin = env_pin(names[i]);
        priv->gpio[i].active_low = env_active_low(names[i]);
    }
    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = LIGHT_DEVICE_API_VERSION_1_0;
    dev->common.module = (hw_module_t *)module;
    dev->common.close = close_device;
    dev->ops = &ops;
    dev->priv = priv;
    *out = &dev->common;
    return 0;
}

static hw_module_methods_t methods = {.open = open_device};
hw_module_t HMI_light = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = LIGHT_MODULE_API_VERSION_1_0,
    .hal_api_version = HARDWARE_API_VERSION_1_0,
    .id = LIGHT_HARDWARE_MODULE_ID,
    .name = "Visinextek VS816 Light HAL (sysfs GPIO)",
    .author = "DarkOS",
    .methods = &methods,
};
