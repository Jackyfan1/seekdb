#include "common/config.h"

#include <cstring>
#include <stdexcept>

namespace mvd {

Config Config::fromArgs(int argc, char** argv) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto match = [&](const char* key) { return std::strncmp(a, key, std::strlen(key)) == 0; };
    if (match("--data-dir=")) {
      cfg.data_dir = a + std::strlen("--data-dir=");
    } else if (match("--row-separator=")) {
      cfg.row_separator = a + std::strlen("--row-separator=");
    } else if (match("--ok-prefix=")) {
      cfg.ok_prefix = a + std::strlen("--ok-prefix=");
    } else if (match("--error-prefix=")) {
      cfg.error_prefix = a + std::strlen("--error-prefix=");
    } else if (match("--ready-marker=")) {
      cfg.ready_marker = a + std::strlen("--ready-marker=");
    } else if (std::strcmp(a, "--help") == 0) {
      throw std::runtime_error("usage: minivectordb [--data-dir=/data] [--ready-marker=TOKEN]");
    } else {
      throw std::runtime_error(std::string("unknown argument: ") + a);
    }
  }
  return cfg;
}

}  // namespace mvd
