#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <hardware/hardware.h>
#include <serial/ISerial.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

typedef struct termios_serial_priv {
  int fd;
} termios_serial_priv_t;

static int baud_to_speed(uint32_t baud, speed_t *speed) {
  switch (baud) {
  case 4800:
    *speed = B4800;
    return 0;
  case 9600:
    *speed = B9600;
    return 0;
  case 19200:
    *speed = B19200;
    return 0;
  case 38400:
    *speed = B38400;
    return 0;
  case 57600:
    *speed = B57600;
    return 0;
  case 115200:
    *speed = B115200;
    return 0;
#ifdef B230400
  case 230400:
    *speed = B230400;
    return 0;
#endif
#ifdef B460800
  case 460800:
    *speed = B460800;
    return 0;
#endif
#ifdef B921600
  case 921600:
    *speed = B921600;
    return 0;
#endif
  default:
    return -EINVAL;
  }
}

static int termios_serial_open_device(serial_device_t *device,
                                      const serial_config_t *config) {
  termios_serial_priv_t *priv;
  struct termios attributes;
  speed_t speed;
  int fd;
  int result;

  if (device == NULL || config == NULL || config->device == NULL ||
      config->device[0] == '\0')
    return -EINVAL;
  priv = (termios_serial_priv_t *)device->priv;
  if (priv == NULL)
    return -EINVAL;
  if (priv->fd >= 0)
    return -EBUSY;
  result = baud_to_speed(config->baud, &speed);
  if (result != 0)
    return result;

  fd = open(config->device, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0)
    return -errno;
  if (tcgetattr(fd, &attributes) != 0) {
    result = -errno;
    close(fd);
    return result;
  }

  attributes.c_iflag = IGNBRK;
  attributes.c_oflag = 0;
  attributes.c_lflag = 0;
  attributes.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB);
  attributes.c_cflag |= CS8 | CLOCAL | CREAD;
#ifdef CRTSCTS
  attributes.c_cflag &= ~CRTSCTS;
#endif
  attributes.c_cc[VMIN] = 0;
  attributes.c_cc[VTIME] = 0;
  if (cfsetispeed(&attributes, speed) != 0 ||
      cfsetospeed(&attributes, speed) != 0 ||
      tcsetattr(fd, TCSANOW, &attributes) != 0) {
    result = -errno;
    close(fd);
    return result;
  }
  tcflush(fd, TCIOFLUSH);
  priv->fd = fd;
  return 0;
}

static int termios_serial_close_device(serial_device_t *device) {
  termios_serial_priv_t *priv;
  int result = 0;
  if (device == NULL || device->priv == NULL)
    return -EINVAL;
  priv = (termios_serial_priv_t *)device->priv;
  if (priv->fd >= 0) {
    if (close(priv->fd) != 0)
      result = -errno;
    priv->fd = -1;
  }
  return result;
}

static int termios_serial_fd(serial_device_t *device) {
  if (device == NULL || device->priv == NULL)
    return -1;
  return ((termios_serial_priv_t *)device->priv)->fd;
}

static const serial_device_ops_t g_serial_ops = {
    .open = termios_serial_open_device,
    .close = termios_serial_close_device,
    .fd = termios_serial_fd,
};

static int termios_serial_destroy(hw_device_t *hardware_device) {
  serial_device_t *device = (serial_device_t *)hardware_device;
  if (device == NULL)
    return -EINVAL;
  termios_serial_close_device(device);
  free(device->priv);
  free(device);
  return 0;
}

static int termios_serial_create(const hw_module_t *module, const char *id,
                                 hw_device_t **output) {
  serial_device_t *device;
  termios_serial_priv_t *priv;

  if (output == NULL || id == NULL ||
      strcmp(id, SERIAL_HARDWARE_MODULE_ID) != 0)
    return -EINVAL;
  *output = NULL;
  device = (serial_device_t *)calloc(1, sizeof(*device));
  priv = (termios_serial_priv_t *)calloc(1, sizeof(*priv));
  if (device == NULL || priv == NULL) {
    free(device);
    free(priv);
    return -ENOMEM;
  }
  priv->fd = -1;
  device->common.tag = HARDWARE_DEVICE_TAG;
  device->common.version = SERIAL_DEVICE_API_VERSION_1_0;
  device->common.module = (hw_module_t *)module;
  device->common.close = termios_serial_destroy;
  device->ops = &g_serial_ops;
  device->priv = priv;
  *output = &device->common;
  return 0;
}

static const hw_module_methods_t g_serial_module_methods = {
    .open = termios_serial_create,
};

hw_module_t HMI_serial = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = SERIAL_MODULE_API_VERSION_1_0,
    .hal_api_version = HARDWARE_API_VERSION_1_0,
    .id = SERIAL_HARDWARE_MODULE_ID,
    .name = "DarkOS Linux termios Serial HAL",
    .author = "DarkOS",
    .methods = &g_serial_module_methods,
};
