#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/config.h"
#include "common/errors.h"
#include "common/types.h"
#include "sql/ast.h"

namespace mvd {

class Engine {
 public:
  explicit Engine(const Config& cfg);

  // 启动：加载持久化数据（若存在）。
  void startup();
  // 正常关闭：回滚未提交事务并落盘。
  void shutdown();
  // 执行一条语句；失败抛 SqlError（引擎保证语句级原子性）。
  void execute(const Statement& st, QueryResult& res);

 private:
  struct Table {
    std::string name;
    std::vector<Column> columns;
    std::vector<Row> rows;
    std::unordered_map<int64_t, size_t> pk_index;  // 主键 -> rows 下标
    int pk_col = -1;       // 主键列下标
    int vector_col = -1;   // 向量列下标（-1 表示无）
    std::string vector_index_name;  // 已建的向量索引名（精确扫描后端，仅元数据）
    int colIndex(const std::string& name) const;
  };

  struct Undo {
    enum Type { Ins, Upd, Del } type = Ins;
    std::string table_name;
    int64_t pk = 0;
    Row old_row;  // Upd/Del 使用
  };

  Table* findTable(const std::string& name);
  const Table* findTable(const std::string& name) const;

  void execCreate(const Statement& st, QueryResult& res);
  void execCreateIndex(const Statement& st, QueryResult& res);
  void execInsert(const Statement& st, QueryResult& res);
  void execSelect(const Statement& st, QueryResult& res);
  void execUpdate(const Statement& st, QueryResult& res);
  void execDelete(const Statement& st, QueryResult& res);
  void execShowTables(QueryResult& res);

  // 值类型转换（按列类型）。
  Cell coerceValue(const Column& col, const Cell& in) const;
  bool matchRow(const Table& t, const Row& row, const Predicate& pred) const;
  bool compare(const Cell& a, const Cell& b, Condition::Op op) const;
  double l2Distance(const std::vector<float>& a, const std::vector<float>& b) const;
  void removeRow(Table* t, size_t idx);

  void rollbackTo(size_t mark);
  void persist();
  void load();

  Config cfg_;
  std::unordered_map<std::string, std::unique_ptr<Table>> tables_;
  bool in_txn_ = false;
  std::vector<Undo> undo_log_;
};

}  // namespace mvd
