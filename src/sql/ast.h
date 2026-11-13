#pragma once

#include <string>
#include <vector>
#include <utility>

#include "common/types.h"

namespace mvd {

// 比较条件：left_col op (right_col | right_val)。
struct Condition {
  enum Op { Eq, Lt, Le, Gt, Ge } op = Eq;
  std::string left_col;
  bool right_is_col = false;
  std::string right_col;
  Cell right_val;
};

struct Predicate {
  std::vector<Condition> conds;  // AND 连接
};

struct OrderItem {
  std::string expr;  // 列名或别名
  bool desc = false;
};

struct SelectItem {
  enum Kind { Star, Column, Distance } kind = Column;
  std::string column;   // Column / Distance 对应的列
  std::string alias;
  std::vector<float> query_vec;  // Distance 的查询向量
};

struct Statement {
  enum Kind {
    CreateTable, Insert, Select, Update, Delete,
    CreateVectorIndex, Begin, Commit, Rollback, ShowTables
  } kind = Select;

  // CreateTable
  std::string table_name;
  std::vector<Column> columns;

  // CreateVectorIndex
  std::string index_name;
  std::string index_table;
  std::string index_column;

  // Insert
  std::vector<std::string> insert_cols;
  std::vector<Row> insert_rows;

  // Select
  std::vector<SelectItem> select_items;
  Predicate where;
  std::vector<OrderItem> order_by;
  int64_t limit = -1;  // -1 = 无 LIMIT

  // Update
  std::vector<std::pair<std::string, Cell>> updates;

  // Delete / Update / Select 复用 where
};

}  // namespace mvd
