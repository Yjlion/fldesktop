// Helpers for fuzz targets whose parser reads a file by name.
#pragma once
#include <sys/mman.h>
#include <unistd.h>
#include <cstdint>
#include <string>

// The input as an anonymous file; its name is /proc/self/fd/N. Close the fd when done.
inline int fuzz_memfd(const uint8_t *data, size_t size, std::string &path) {
  int fd = memfd_create("fuzz", 0);
  if (fd < 0) return -1;
  size_t off = 0;
  while (off < size) {
    ssize_t n = write(fd, data + off, size - off);
    if (n <= 0) break;
    off += n;
  }
  path = "/proc/self/fd/" + std::to_string(fd);
  return fd;
}
