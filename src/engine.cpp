#include "engine.h"

#include "sql/parser.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace mvd {

namespace {

const char kMagic[4] = {'M', 'V', 'D', '1'};

// ---- 二进制序列化辅助（小端，目标 x86_64）----

void put8(std::ostream& os, uint8_t v) { os.write(reinterpret_cast<const char*>(&v), 1); }
void put32(std::ostream& os, uint32_t v) { os.write(reinterpret_cast<const char*>(&v), 4); }
void put64(std::ostream& os, int64_t v) { os.write(reinterpret_cast<const char*>(&v), 8); }
void putStr(std::ostream& os, const std::string& s) {
  put32(os, static_cast<uint32_t>(s.size()));
  os.write(s.data(), static_cast<std::streamsize>(s.size()));
}

uint8_t get8(std::istream& is) { uint8_t v = 0; is.read(reinterpret_cast<char*>(&v), 1); return v; }
uint32_t get32(std::istream& is) { uint32_t v = 0; is.read(reinterpret_cast<char*>(&v), 4); return v; }
int64_t get64(std::istream& is) { int64_t v = 0; is.read(reinterpret_cast<char*>(&v), 8); return v; }
std::string getStr(std::istream& is) {
  uint32_t n = get32(is);
  std::string s(n, '\0');
  is.read(&s[0], static_cast<std::streamsize>(n));
  return s;
}

void putCell(std::ostream& os, const Cell& c) {
  switch (c.kind) {
    case Cell::Kind::Int:
      put8(os, 0); put64(os, c.i); break;
    case Cell::Kind::Real:
      put8(os, 1);
      os.write(reinterpret_cast<const char*>(&c.r), sizeof(double));
      break;
    case Cell::Kind::Str:
      put8(os, 2); putStr(os, c.s); break;
    case Cell::Kind::Vec:
      put8(os, 3);
      put32(os, static_cast<uint32_t>(c.v.size()));
      for (float f : c.v) os.write(reinterpret_cast<const char*>(&f), sizeof(float));
      break;
  }
}

Cell getCell(std::istream& is) {
  uint8_t k = get8(is);
  Cell c;
  switch (k) {
    case 0:
      c.kind = Cell::Kind::Int; c.i = get64(is); break;
    case 1:
      c.kind = Cell::Kind::Real;
      is.read(reinterpret_cast<char*>(&c.r), sizeof(double));
      break;
    case 2:
      c.kind = Cell::Kind::Str; c.s = getStr(is); break;
    case 3: {
      c.kind = Cell::Kind::Vec;
      uint32_t n = get32(is);
      c.v.resize(n);
      for (uint32_t i = 0; i < n; ++i) {
        is.read(reinterpret_cast<char*>(&c.v[i]), sizeof(float));
      }
      break;
    }
    default:
      throw SqlError(ErrKind::InternalError, "corrupt snapshot cell kind");
  }
  return c;
}

}  // namespace

Engine::Engine(const Config& cfg) : cfg_(cfg) {}

int Engine::Table::colIndex(const std::string& name) const {
  for (size_t i = 0; i < columns.size(); ++i) {
    if (columns[i].name == name) return static_cast<int>(i);
  }
  throw SqlError(ErrKind::UnknownColumn, "unknown column: " + name);
}

Engine::Table* Engine::findTable(const std::string& name) {
  auto it = tables_.find(name);
  if (it == tables_.end()) throw SqlError(ErrKind::UnknownTable, "unknown table: " + name);
  return it->second.get();
}

const Engine::Table* Engine::findTable(const std::string& name) const {
  auto it = tables_.find(name);
  if (it == tables_.end()) throw SqlError(ErrKind::UnknownTable, "unknown table: " + name);
  return it->second.get();
}

void Engine::startup() { load(); }

void Engine::shutdown() {
  if (in_txn_) {
    rollbackTo(0);
    in_txn_ = false;
  }
  persist();
}

void Engine::removeRow(Table* t, size_t idx) {
  int64_t pk = t->rows[idx][t->pk_col].i;
  size_t last = t->rows.size() - 1;
  if (idx != last) {
    t->rows[idx] = std::move(t->rows[last]);
    t->pk_index[t->rows[idx][t->pk_col].i] = idx;
  }
  t->rows.pop_back();
  t->pk_index.erase(pk);
}

Cell Engine::coerceValue(const Column& col, const Cell& in) const {
  switch (col.type) {
    case Type::BigInt:
      if (in.kind != Cell::Kind::Int) {
        throw SqlError(ErrKind::TypeMismatch, "column " + col.name + " expects BIGINT");
      }
      return in;
    case Type::Varchar:
      if (in.kind != Cell::Kind::Str) {
        throw SqlError(ErrKind::TypeMismatch, "column " + col.name + " expects VARCHAR");
      }
      if (static_cast<int>(in.s.size()) > col.varchar_len) {
        throw SqlError(ErrKind::TypeMismatch, "value too long for VARCHAR(" +
                                                 std::to_string(col.varchar_len) + ")");
      }
      return in;
    case Type::Vector: {
      if (in.kind == Cell::Kind::Vec) {
        if (static_cast<int>(in.v.size()) != col.vector_dim) {
          throw SqlError(ErrKind::BadVector, "vector dimension mismatch");
        }
        return in;
      }
      if (in.kind == Cell::Kind::Str) {
        std::vector<float> v = parseVectorStr(in.s);
        if (static_cast<int>(v.size()) != col.vector_dim) {
          throw SqlError(ErrKind::BadVector, "vector dimension mismatch");
        }
        Cell c;
        c.kind = Cell::Kind::Vec;
        c.v = std::move(v);
        return c;
      }
      throw SqlError(ErrKind::TypeMismatch, "column " + col.name + " expects VECTOR");
    }
  }
  throw SqlError(ErrKind::InternalError, "unknown column type");
}

bool Engine::compare(const Cell& a, const Cell& b, Condition::Op op) const {
  if (a.kind == Cell::Kind::Int && b.kind == Cell::Kind::Int) {
    switch (op) {
      case Condition::Eq: return a.i == b.i;
      case Condition::Lt: return a.i < b.i;
      case Condition::Le: return a.i <= b.i;
      case Condition::Gt: return a.i > b.i;
      case Condition::Ge: return a.i >= b.i;
    }
  } else if (a.kind == Cell::Kind::Str && b.kind == Cell::Kind::Str) {
    // std::string 按字节序比较 == Unicode 码点序（UTF-8）。
    switch (op) {
      case Condition::Eq: return a.s == b.s;
      case Condition::Lt: return a.s < b.s;
      case Condition::Le: return a.s <= b.s;
      case Condition::Gt: return a.s > b.s;
      case Condition::Ge: return a.s >= b.s;
    }
  } else if (a.kind == Cell::Kind::Real && b.kind == Cell::Kind::Real) {
    switch (op) {
      case Condition::Eq: return a.r == b.r;
      case Condition::Lt: return a.r < b.r;
      case Condition::Le: return a.r <= b.r;
      case Condition::Gt: return a.r > b.r;
      case Condition::Ge: return a.r >= b.r;
    }
  }
  throw SqlError(ErrKind::TypeMismatch, "cannot compare values of different types");
}

bool Engine::matchRow(const Table& t, const Row& row, const Predicate& pred) const {
  for (const auto& c : pred.conds) {
    int li = t.colIndex(c.left_col);
    const Cell& lv = row[li];
    Cell rv;
    if (c.right_is_col) {
      int ri = t.colIndex(c.right_col);
      rv = row[ri];
    } else {
      rv = coerceValue(t.columns[li], c.right_val);
    }
    if (!compare(lv, rv, c.op)) return false;
  }
  return true;
}

double Engine::l2Distance(const std::vector<float>& a, const std::vector<float>& b) const {
  if (a.size() != b.size()) {
    throw SqlError(ErrKind::BadVector, "vector dimension mismatch");
  }
  double sum = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += d * d;
  }
  return std::sqrt(sum);
}

void Engine::execCreate(const Statement& st, QueryResult& res) {
  if (tables_.count(st.table_name)) {
    throw SqlError(ErrKind::DuplicateTable, "table already exists: " + st.table_name);
  }
  auto t = std::make_unique<Table>();
  t->name = st.table_name;
  t->columns = st.columns;

  int pk_count = 0, vec_count = 0;
  for (size_t i = 0; i < t->columns.size(); ++i) {
    const Column& c = t->columns[i];
    if (c.primary_key) {
      ++pk_count;
      if (c.type != Type::BigInt) {
        throw SqlError(ErrKind::PrimaryKeyRequired, "primary key must be BIGINT");
      }
      t->pk_col = static_cast<int>(i);
    }
    if (c.type == Type::Vector) {
      ++vec_count;
      t->vector_col = static_cast<int>(i);
    }
  }
  if (pk_count != 1) {
    throw SqlError(ErrKind::PrimaryKeyRequired, "table must have exactly one BIGINT primary key");
  }
  if (vec_count > 1) {
    throw SqlError(ErrKind::Unsupported, "at most one VECTOR column per table");
  }

  tables_[st.table_name] = std::move(t);
  // res.affected 保持 -1，使会话层输出 "OK"（而非 "OK 0"）。
}

void Engine::execInsert(const Statement& st, QueryResult& res) {
  Table* t = findTable(st.table_name);
  size_t ncols = t->columns.size();

  // 建立「目标列下标」：显式列名 or 全部列。
  std::vector<int> target;
  if (st.insert_cols.empty()) {
    if (!st.insert_rows.empty() && st.insert_rows[0].size() != ncols) {
      throw SqlError(ErrKind::TypeMismatch, "column count does not match");
    }
    target.resize(ncols);
    for (size_t i = 0; i < ncols; ++i) target[i] = static_cast<int>(i);
  } else {
    if (st.insert_cols.size() > ncols) {
      throw SqlError(ErrKind::TypeMismatch, "too many columns");
    }
    std::vector<bool> seen(ncols, false);
    for (const auto& cn : st.insert_cols) {
      int ci = t->colIndex(cn);
      if (seen[ci]) throw SqlError(ErrKind::TypeMismatch, "duplicate column: " + cn);
      seen[ci] = true;
      target.push_back(ci);
    }
  }

  int64_t affected = 0;
  for (const Row& in_row : st.insert_rows) {
    if (in_row.size() != target.size()) {
      throw SqlError(ErrKind::TypeMismatch, "value count does not match column list");
    }
    // 构造完整行：未指定的列填默认值。
    Row full(ncols);
    for (size_t i = 0; i < ncols; ++i) {
      switch (t->columns[i].type) {
        case Type::BigInt:
          full[i].kind = Cell::Kind::Int; full[i].i = 0; break;
        case Type::Varchar:
          full[i].kind = Cell::Kind::Str; full[i].s = ""; break;
        case Type::Vector:
          full[i].kind = Cell::Kind::Vec; full[i].v.assign(t->columns[i].vector_dim, 0.0f); break;
      }
    }
    for (size_t k = 0; k < target.size(); ++k) {
      full[target[k]] = coerceValue(t->columns[target[k]], in_row[k]);
    }

    int64_t pk = full[t->pk_col].i;
    if (t->pk_index.count(pk)) {
      throw SqlError(ErrKind::DuplicateKey, "duplicate primary key: " + std::to_string(pk));
    }
    t->rows.push_back(std::move(full));
    t->pk_index[pk] = t->rows.size() - 1;

    Undo u;
    u.type = Undo::Ins;
    u.table_name = t->name;
    u.pk = pk;
    undo_log_.push_back(std::move(u));
    ++affected;
  }
  res.affected = affected;
}

void Engine::execSelect(const Statement& st, QueryResult& res) {
  const Table* t = findTable(st.table_name);

  struct ViewRow {
    Row projected;
    std::vector<Cell> keys;
    int64_t pk = 0;
  };
  std::vector<ViewRow> views;

  res.has_rows = true;

  for (const Row& row : t->rows) {
    if (!st.where.conds.empty() && !matchRow(*t, row, st.where)) continue;

    ViewRow vr;
    vr.pk = row[t->pk_col].i;
    Row projected;
    std::map<std::string, Cell> alias_map;  // 别名 -> 值

    for (const auto& item : st.select_items) {
      if (item.kind == SelectItem::Star) {
        for (size_t ci = 0; ci < t->columns.size(); ++ci) {
          projected.push_back(row[ci]);
        }
      } else if (item.kind == SelectItem::Column) {
        int ci = t->colIndex(item.column);
        projected.push_back(row[ci]);
        alias_map[item.alias.empty() ? item.column : item.alias] = row[ci];
      } else {  // Distance
        int ci = t->colIndex(item.column);
        double d = l2Distance(row[ci].v, item.query_vec);
        Cell c;
        c.kind = Cell::Kind::Real;
        c.r = d;
        projected.push_back(c);
        alias_map[item.alias.empty() ? "distance" : item.alias] = c;
      }
    }

    // 排序键
    for (const auto& oi : st.order_by) {
      auto it = alias_map.find(oi.expr);
      if (it != alias_map.end()) {
        vr.keys.push_back(it->second);
      } else {
        int ci = t->colIndex(oi.expr);
        vr.keys.push_back(row[ci]);
      }
    }

    vr.projected = std::move(projected);
    views.push_back(std::move(vr));
  }

  // 排序
  const auto& order_by = st.order_by;
  if (!order_by.empty()) {
    std::stable_sort(views.begin(), views.end(),
                     [&](const ViewRow& a, const ViewRow& b) {
                       for (size_t k = 0; k < order_by.size(); ++k) {
                         const Cell& ca = a.keys[k];
                         const Cell& cb = b.keys[k];
                         bool less = false, greater = false;
                         if (ca.kind == Cell::Kind::Int && cb.kind == Cell::Kind::Int) {
                           less = ca.i < cb.i; greater = ca.i > cb.i;
                         } else if (ca.kind == Cell::Kind::Real && cb.kind == Cell::Kind::Real) {
                           less = ca.r < cb.r; greater = ca.r > cb.r;
                         } else if (ca.kind == Cell::Kind::Str && cb.kind == Cell::Kind::Str) {
                           less = ca.s < cb.s; greater = ca.s > cb.s;
                         }
                         if (less) return !order_by[k].desc;
                         if (greater) return order_by[k].desc;
                       }
                       return a.pk < b.pk;  // 并列按主键升序
                     });
  }

  // 列名
  for (const auto& item : st.select_items) {
    if (item.kind == SelectItem::Star) {
      for (const auto& col : t->columns) res.columns.push_back(col.name);
    } else if (item.kind == SelectItem::Column) {
      res.columns.push_back(item.alias.empty() ? item.column : item.alias);
    } else {
      res.columns.push_back(item.alias.empty() ? "distance" : item.alias);
    }
  }

  // LIMIT
  size_t n = views.size();
  if (st.limit >= 0 && static_cast<size_t>(st.limit) < n) n = static_cast<size_t>(st.limit);
  for (size_t i = 0; i < n; ++i) {
    res.rows.push_back(std::move(views[i].projected));
  }
}

void Engine::execUpdate(const Statement& st, QueryResult& res) {
  Table* t = findTable(st.table_name);

  // 预校验：更新列存在，且不允许主键更新。
  std::vector<int> update_cols;
  for (const auto& kv : st.updates) {
    int ci = t->colIndex(kv.first);
    if (ci == t->pk_col) throw SqlError(ErrKind::Unsupported, "primary key update is not supported");
    update_cols.push_back(ci);
  }

  int64_t affected = 0;
  for (size_t i = 0; i < t->rows.size(); ++i) {
    if (!matchRow(*t, t->rows[i], st.where)) continue;

    Row old = t->rows[i];
    for (size_t k = 0; k < st.updates.size(); ++k) {
      t->rows[i][update_cols[k]] = coerceValue(t->columns[update_cols[k]], st.updates[k].second);
    }

    Undo u;
    u.type = Undo::Upd;
    u.table_name = t->name;
    u.pk = old[t->pk_col].i;
    u.old_row = std::move(old);
    undo_log_.push_back(std::move(u));
    ++affected;
  }
  res.affected = affected;
}

void Engine::execDelete(const Statement& st, QueryResult& res) {
  Table* t = findTable(st.table_name);

  std::vector<int64_t> to_delete;
  for (const Row& row : t->rows) {
    if (matchRow(*t, row, st.where)) to_delete.push_back(row[t->pk_col].i);
  }
  for (int64_t pk : to_delete) {
    auto it = t->pk_index.find(pk);
    size_t idx = it->second;
    Undo u;
    u.type = Undo::Del;
    u.table_name = t->name;
    u.pk = pk;
    u.old_row = t->rows[idx];
    undo_log_.push_back(std::move(u));
    removeRow(t, idx);
  }
  res.affected = static_cast<int64_t>(to_delete.size());
}

void Engine::execShowTables(QueryResult& res) {
  res.has_rows = true;
  res.columns = {"Tables_in_minivectordb"};
  for (const auto& kv : tables_) {
    Row r;
    Cell c;
    c.kind = Cell::Kind::Str;
    c.s = kv.first;
    r.push_back(c);
    res.rows.push_back(std::move(r));
  }
}

void Engine::rollbackTo(size_t mark) {
  while (undo_log_.size() > mark) {
    Undo u = std::move(undo_log_.back());
    undo_log_.pop_back();
    Table* t = findTable(u.table_name);
    if (u.type == Undo::Ins) {
      auto it = t->pk_index.find(u.pk);
      if (it != t->pk_index.end()) removeRow(t, it->second);
    } else if (u.type == Undo::Upd) {
      auto it = t->pk_index.find(u.pk);
      if (it != t->pk_index.end()) t->rows[it->second] = std::move(u.old_row);
    } else {  // Del
      t->rows.push_back(std::move(u.old_row));
      t->pk_index[u.pk] = t->rows.size() - 1;
    }
  }
}

void Engine::execute(const Statement& st, QueryResult& res) {
  if (st.kind == Statement::Begin) {
    if (in_txn_) throw SqlError(ErrKind::InTransaction, "nested transaction");
    in_txn_ = true;
    return;
  }
  if (st.kind == Statement::Commit) {
    if (!in_txn_) throw SqlError(ErrKind::NotInTransaction, "no active transaction");
    in_txn_ = false;
    undo_log_.clear();
    persist();
    return;
  }
  if (st.kind == Statement::Rollback) {
    if (!in_txn_) throw SqlError(ErrKind::NotInTransaction, "no active transaction");
    rollbackTo(0);
    in_txn_ = false;
    return;
  }

  size_t mark = undo_log_.size();
  try {
    switch (st.kind) {
      case Statement::CreateTable: execCreate(st, res); break;
      case Statement::Insert: execInsert(st, res); break;
      case Statement::Select: execSelect(st, res); break;
      case Statement::Update: execUpdate(st, res); break;
      case Statement::Delete: execDelete(st, res); break;
      case Statement::ShowTables: execShowTables(res); break;
      default: throw SqlError(ErrKind::Unsupported, "unsupported statement");
    }
  } catch (...) {
    rollbackTo(mark);  // 语句级原子性：撤销本语句的部分修改
    throw;
  }

  bool modifying = (st.kind == Statement::Insert || st.kind == Statement::Update ||
                    st.kind == Statement::Delete || st.kind == Statement::CreateTable);
  if (modifying && !in_txn_) {
    persist();        // 自动提交
    undo_log_.clear();  // 已持久化，无需保留 undo
  }
}

// ---- 持久化：全量快照（写临时文件 -> fsync -> rename -> fsync 目录）----

void Engine::persist() {
  std::string dir = cfg_.data_dir;
  std::string tmp = dir + "/minivectordb.snap.tmp";
  std::string final = dir + "/minivectordb.snap";

  {
    std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
    if (!os) throw SqlError(ErrKind::InternalError, "cannot open snapshot for writing");
    os.write(kMagic, 4);
    put32(os, static_cast<uint32_t>(tables_.size()));
    for (const auto& kv : tables_) {
      const Table& t = *kv.second;
      putStr(os, t.name);
      put32(os, static_cast<uint32_t>(t.columns.size()));
      for (const Column& col : t.columns) {
        putStr(os, col.name);
        put8(os, static_cast<uint8_t>(col.type));
        put32(os, static_cast<uint32_t>(col.varchar_len));
        put32(os, static_cast<uint32_t>(col.vector_dim));
        put8(os, col.primary_key ? 1 : 0);
      }
      put32(os, static_cast<uint32_t>(t.rows.size()));
      for (const Row& row : t.rows) {
        for (const Cell& c : row) putCell(os, c);
      }
    }
    os.flush();
    if (!os) throw SqlError(ErrKind::InternalError, "failed to write snapshot");
  }

  // fsync 临时文件
  int fd = ::open(tmp.c_str(), O_WRONLY);
  if (fd < 0) throw SqlError(ErrKind::InternalError, "cannot open snapshot for fsync");
  ::fsync(fd);
  ::close(fd);

  if (::rename(tmp.c_str(), final.c_str()) != 0) {
    throw SqlError(ErrKind::InternalError, "cannot rename snapshot");
  }
  // fsync 目录，保证 rename 落盘
  int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }
}

void Engine::load() {
  std::string path = cfg_.data_dir + "/minivectordb.snap";
  std::ifstream is(path, std::ios::binary);
  if (!is) return;  // 无快照：全新数据库

  char magic[4];
  is.read(magic, 4);
  if (!is || std::memcmp(magic, kMagic, 4) != 0) {
    throw SqlError(ErrKind::InternalError, "corrupt snapshot");
  }
  uint32_t ntab = get32(is);
  for (uint32_t ti = 0; ti < ntab; ++ti) {
    auto t = std::make_unique<Table>();
    t->name = getStr(is);
    uint32_t ncol = get32(is);
    t->columns.resize(ncol);
    for (uint32_t ci = 0; ci < ncol; ++ci) {
      Column& col = t->columns[ci];
      col.name = getStr(is);
      col.type = static_cast<Type>(get8(is));
      col.varchar_len = static_cast<int>(get32(is));
      col.vector_dim = static_cast<int>(get32(is));
      col.primary_key = get8(is) != 0;
      if (col.primary_key) t->pk_col = static_cast<int>(ci);
      if (col.type == Type::Vector) t->vector_col = static_cast<int>(ci);
    }
    uint32_t nrow = get32(is);
    for (uint32_t ri = 0; ri < nrow; ++ri) {
      Row row;
      row.reserve(ncol);
      for (uint32_t ci = 0; ci < ncol; ++ci) row.push_back(getCell(is));
      int64_t pk = row[t->pk_col].i;
      t->rows.push_back(std::move(row));
      t->pk_index[pk] = t->rows.size() - 1;
    }
    tables_[t->name] = std::move(t);
  }
  if (!is) throw SqlError(ErrKind::InternalError, "corrupt snapshot (truncated)");
}

}  // namespace mvd
