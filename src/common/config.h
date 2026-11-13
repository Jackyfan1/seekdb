#pragma once

#include <string>

namespace mvd {

// 运行参数：数据统一写入 data_dir（默认 /data）。
struct Config {
  std::string data_dir = "/data";

  // 解析命令行；也支持环境变量 VECTORDB_DATA_DIR（仅作本地自测便利）。
  static Config fromArgs(int argc, char** argv);
};

}  // namespace mvd
