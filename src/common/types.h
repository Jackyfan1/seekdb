#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace mvd {

// 列类型：BIGINT、VARCHAR(n)、VECTOR(n)。
enum class Type { BigInt, Varchar, Vector };

struct Column {
  std::string name;
  Type type = Type::BigInt;
  int varchar_len = 0;   // VARCHAR(n) 的 n，按 Unicode 字符数
  int vector_dim = 0;    // VECTOR(n) 的 n
  bool primary_key = false;
};

// 一个单元格的值。
struct Cell {
  enum class Kind { Int, Real, Str, Vec } kind = Kind::Int;
  int64_t i = 0;
  double r = 0.0;
  std::string s;
  std::vector<float> v;
};

using Row = std::vector<Cell>;

// 查询/语句执行结果。
struct QueryResult {
  bool has_rows = false;          // SELECT 产生结果集
  std::vector<std::string> columns;
  std::vector<Row> rows;
  int64_t affected = -1;          // INSERT/UPDATE/DELETE 影响行数；-1 表示无
};

}  // namespace mvd
