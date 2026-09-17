#include <peripheral/SerialPort.h>

#include <fcntl.h>
#include <cstdlib>
#include <string>
#include <unistd.h>

int main() {
  const int master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0)
    return 1;
  const char *slave = ptsname(master);
  if (slave == nullptr) {
    close(master);
    return 1;
  }
  std::string error;
  auto port = darkos::peripheral::SerialPort::open(slave, 115200, error);
  const bool valid = port != nullptr && port->fd() >= 0;
  port.reset();
  close(master);
  return valid ? 0 : 1;
}
