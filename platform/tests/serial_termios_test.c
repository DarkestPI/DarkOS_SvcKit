#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <serial/ISerial.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern hw_module_t HMI_serial;

int main(void) {
  serial_device_t *serial = NULL;
  serial_config_t config;
  char *slave;
  int master;
  int failures = 0;

  master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
    perror("posix_openpt");
    return 1;
  }
  slave = ptsname(master);
  if (slave == NULL) {
    perror("ptsname");
    close(master);
    return 1;
  }

  if (serial_open(&HMI_serial, &serial) != 0 || serial == NULL) {
    fprintf(stderr, "serial module open failed\n");
    close(master);
    return 1;
  }
  config.device = slave;
  config.baud = 115200;
  if (serial->ops->open(serial, &config) != 0 || serial->ops->fd(serial) < 0) {
    fprintf(stderr, "termios serial open failed\n");
    ++failures;
  }
  if (serial->ops->open(serial, &config) != -EBUSY) {
    fprintf(stderr, "second serial open did not return -EBUSY\n");
    ++failures;
  }
  if (serial->ops->close(serial) != 0 || serial->ops->close(serial) != 0 ||
      serial->ops->fd(serial) != -1) {
    fprintf(stderr, "serial close is not idempotent\n");
    ++failures;
  }
  if (serial_close(serial) != 0) {
    fprintf(stderr, "serial device destroy failed\n");
    ++failures;
  }
  close(master);
  return failures == 0 ? 0 : 1;
}
