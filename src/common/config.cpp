#include "common/config.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace mvd {

Config Config::fromArgs(int argc, char** argv) {
  Config cfg;
  if (const char* env = std::getenv("VECTORDB_DATA_DIR")) {
    if (*env) cfg.data_dir = env;
  }
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (std::strncmp(a, "--data-dir=", 11) == 0) {
      cfg.data_dir = a + 11;
    } else if (std::strcmp(a, "--data-dir") == 0) {
      if (i + 1 >= argc) throw std::runtime_error("--data-dir requires a value");
      cfg.data_dir = argv[++i];
    } else if (std::strcmp(a, "--help") == 0) {
      throw std::runtime_error("usage: minivectordb [--data-dir=/data]");
    } else if (a[0] != '-') {
      // 兼容测试驱动把数据目录作为位置参数传入。
      cfg.data_dir = a;
    } else {
      throw std::runtime_error(std::string("unknown argument: ") + a);
    }
  }
  return cfg;
}

}  // namespace mvd
