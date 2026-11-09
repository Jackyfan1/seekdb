#pragma once

#include <string>

namespace mvd {

// 评测协议相关的一切约定都集中在这里，方便与正式题包对齐。
struct Config {
  std::string data_dir = "/data";

  // 结果行分隔符（默认制表符）。
  std::string row_separator = "\t";
  // 状态行前缀（INSERT/UPDATE/DELETE → "OK <n>"，DDL/事务 → "OK"）。
  std::string ok_prefix = "OK";
  std::string error_prefix = "ERROR";
  // 就绪标记：每条语句响应后追加一行（默认空 = 不输出）。
  std::string ready_marker;

  static Config fromArgs(int argc, char** argv);
};

}  // namespace mvd
