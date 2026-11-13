#include "sql/parser.h"

#include <cctype>
#include <cstdlib>
#include <cmath>

namespace mvd {

namespace {

std::string upperAscii(const std::string& s) {
  std::string r = s;
  for (auto& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return r;
}

bool isWord(const Token& t, const char* word) {
  return t.kind == TokKind::Ident && upperAscii(t.text) == word;
}

}  // namespace

Parser::Parser(const std::string& buf, size_t start) : lexer_(buf, start) {}

void Parser::advance() { cur_ = lexer_.next(); }

void Parser::expect(TokKind k, const char* what) {
  if (cur_.kind != k) {
    if (cur_.kind == TokKind::Eof) incomplete_ = true;
    throw SqlError(ErrKind::SyntaxError,
                   std::string("expected ") + what + " near '" + cur_.text + "'");
  }
  advance();
}

void Parser::expectSemicolon() {
  if (cur_.kind != TokKind::Semicolon) {
    if (cur_.kind == TokKind::Eof) incomplete_ = true;
    throw SqlError(ErrKind::SyntaxError, "expected ';' to terminate statement");
  }
  end_ = cur_.offset + 1;
}

std::string Parser::parseIdent() {
  if (cur_.kind != TokKind::Ident) {
    if (cur_.kind == TokKind::Eof) incomplete_ = true;
    throw SqlError(ErrKind::SyntaxError, "expected identifier");
  }
  std::string name = cur_.text;
  advance();
  return name;
}

int64_t Parser::parseIntLiteral() {
  if (cur_.kind != TokKind::IntLit) {
    if (cur_.kind == TokKind::Eof) incomplete_ = true;
    throw SqlError(ErrKind::SyntaxError, "expected integer literal");
  }
  int64_t v = std::strtoll(cur_.text.c_str(), nullptr, 10);
  advance();
  return v;
}

std::vector<float> parseVectorStr(const std::string& s) {
  std::vector<float> out;
  size_t i = 0;
  auto skipWs = [&]() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  };
  skipWs();
  if (i >= s.size() || s[i] != '[') {
    throw SqlError(ErrKind::BadVector, "vector must start with '['");
  }
  ++i;
  skipWs();
  if (i < s.size() && s[i] == ']') { ++i; return out; }  // 空向量（维度 0，通常不允许）
  while (true) {
    skipWs();
    size_t start = i;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) ||
                            s[i] == '-' || s[i] == '+' || s[i] == '.' ||
                            s[i] == 'e' || s[i] == 'E')) {
      ++i;
    }
    if (i == start) {
      throw SqlError(ErrKind::BadVector, "invalid number in vector");
    }
    std::string num = s.substr(start, i - start);
    char* endp = nullptr;
    double d = std::strtod(num.c_str(), &endp);
    if (endp == num.c_str() || !std::isfinite(d) ||
        d < -3.402823466e38 || d > 3.402823466e38) {
      throw SqlError(ErrKind::BadVector, "vector component not a finite float32: " + num);
    }
    out.push_back(static_cast<float>(d));
    skipWs();
    if (i < s.size() && s[i] == ',') { ++i; continue; }
    if (i < s.size() && s[i] == ']') { ++i; break; }
    throw SqlError(ErrKind::BadVector, "expected ',' or ']' in vector");
  }
  return out;
}

std::unique_ptr<Statement> Parser::parseStatement() {
  advance();
  if (cur_.kind == TokKind::Eof) return nullptr;
  std::unique_ptr<Statement> st;
  switch (cur_.kind) {
    case TokKind::KwCreate: st = parseCreate(); break;
    case TokKind::KwInsert: st = parseInsert(); break;
    case TokKind::KwSelect: st = parseSelect(); break;
    case TokKind::KwUpdate: st = parseUpdate(); break;
    case TokKind::KwDelete: st = parseDelete(); break;
    case TokKind::KwBegin: st = parseTxn(Statement::Begin); break;
    case TokKind::KwCommit: st = parseTxn(Statement::Commit); break;
    case TokKind::KwRollback: st = parseTxn(Statement::Rollback); break;
    case TokKind::KwShow: st = parseShow(); break;
    default:
      // ALTER TABLE 等题目明确不支持的语句：返回 unsupported_sql，而不是静默执行。
      throw SqlError(ErrKind::Unsupported,
                     "unsupported statement near '" + cur_.text + "'");
  }
  expectSemicolon();
  return st;
}

std::unique_ptr<Statement> Parser::parseTxn(Statement::Kind k) {
  advance();  // 跳过 BEGIN/COMMIT/ROLLBACK
  auto st = std::make_unique<Statement>();
  st->kind = k;
  return st;
}

std::unique_ptr<Statement> Parser::parseShow() {
  advance();  // SHOW
  expect(TokKind::KwTables, "TABLES");
  auto st = std::make_unique<Statement>();
  st->kind = Statement::ShowTables;
  return st;
}

void Parser::parseType(Column& col) {
  switch (cur_.kind) {
    case TokKind::KwBigInt:
      col.type = Type::BigInt;
      advance();
      break;
    case TokKind::KwVarchar:
      advance();
      expect(TokKind::LParen, "(");
      col.type = Type::Varchar;
      col.varchar_len = static_cast<int>(parseIntLiteral());
      expect(TokKind::RParen, ")");
      break;
    case TokKind::KwVector:
      advance();
      expect(TokKind::LParen, "(");
      col.type = Type::Vector;
      col.vector_dim = static_cast<int>(parseIntLiteral());
      expect(TokKind::RParen, ")");
      break;
    default:
      if (cur_.kind == TokKind::Eof) incomplete_ = true;
      throw SqlError(ErrKind::SyntaxError, "expected a column type");
  }
}

Column Parser::parseColumnDef() {
  Column col;
  col.name = parseIdent();
  parseType(col);
  if (cur_.kind == TokKind::KwPrimary) {
    advance();
    expect(TokKind::KwKey, "KEY");
    col.primary_key = true;
  }
  return col;
}

std::unique_ptr<Statement> Parser::parseCreate() {
  advance();  // CREATE
  if (cur_.kind == TokKind::KwVector) {
    // CREATE VECTOR INDEX index_name ON table_name(vector_column)
    advance();
    if (!isWord(cur_, "INDEX")) {
      throw SqlError(ErrKind::SyntaxError, "expected INDEX after CREATE VECTOR");
    }
    advance();
    auto st = std::make_unique<Statement>();
    st->kind = Statement::CreateVectorIndex;
    st->index_name = parseIdent();
    if (!isWord(cur_, "ON")) {
      throw SqlError(ErrKind::SyntaxError, "expected ON after index name");
    }
    advance();
    st->index_table = parseIdent();
    expect(TokKind::LParen, "(");
    st->index_column = parseIdent();
    expect(TokKind::RParen, ")");
    return st;
  }
  if (cur_.kind != TokKind::KwTable) {
    throw SqlError(ErrKind::Unsupported, "only CREATE TABLE / CREATE VECTOR INDEX is supported");
  }
  expect(TokKind::KwTable, "TABLE");
  auto st = std::make_unique<Statement>();
  st->kind = Statement::CreateTable;
  st->table_name = parseIdent();
  expect(TokKind::LParen, "(");
  if (cur_.kind == TokKind::RParen) {
    throw SqlError(ErrKind::SyntaxError, "table must have at least one column");
  }
  while (true) {
    st->columns.push_back(parseColumnDef());
    if (cur_.kind == TokKind::Comma) { advance(); continue; }
    break;
  }
  expect(TokKind::RParen, ")");
  return st;
}

Cell Parser::parseLiteral() {
  if (cur_.kind == TokKind::Minus) {
    // 负数字面量：-9223372036854775808 也必须能精确解析。
    advance();
    if (cur_.kind != TokKind::IntLit) {
      if (cur_.kind == TokKind::Eof) incomplete_ = true;
      throw SqlError(ErrKind::SyntaxError, "expected integer literal after '-'");
    }
    Cell c;
    c.kind = Cell::Kind::Int;
    c.i = std::strtoll(("-" + cur_.text).c_str(), nullptr, 10);
    advance();
    return c;
  }
  if (cur_.kind == TokKind::IntLit) {
    Cell c; c.kind = Cell::Kind::Int; c.i = parseIntLiteral(); return c;
  }
  if (cur_.kind == TokKind::StrLit) {
    Cell c; c.kind = Cell::Kind::Str; c.s = cur_.text; advance(); return c;
  }
  if (cur_.kind == TokKind::Eof) incomplete_ = true;
  throw SqlError(ErrKind::SyntaxError, "expected a literal value");
}

std::unique_ptr<Statement> Parser::parseInsert() {
  advance();  // INSERT
  expect(TokKind::KwInto, "INTO");
  auto st = std::make_unique<Statement>();
  st->kind = Statement::Insert;
  st->table_name = parseIdent();

  if (cur_.kind == TokKind::LParen) {
    advance();
    while (true) {
      st->insert_cols.push_back(parseIdent());
      if (cur_.kind == TokKind::Comma) { advance(); continue; }
      break;
    }
    expect(TokKind::RParen, ")");
  }

  expect(TokKind::KwValues, "VALUES");
  while (true) {
    expect(TokKind::LParen, "(");
    Row row;
    while (true) {
      row.push_back(parseLiteral());
      if (cur_.kind == TokKind::Comma) { advance(); continue; }
      break;
    }
    expect(TokKind::RParen, ")");
    st->insert_rows.push_back(std::move(row));
    if (cur_.kind == TokKind::Comma) { advance(); continue; }
    break;
  }
  return st;
}

SelectItem Parser::parseSelectItem() {
  SelectItem item;
  if (cur_.kind == TokKind::Star) {
    item.kind = SelectItem::Star;
    advance();
    return item;
  }
  if (cur_.kind == TokKind::KwL2Distance) {
    item.kind = SelectItem::Distance;
    advance();
    expect(TokKind::LParen, "(");
    item.column = parseIdent();
    expect(TokKind::Comma, ",");
    if (cur_.kind != TokKind::StrLit) {
      throw SqlError(ErrKind::SyntaxError, "L2_DISTANCE query vector must be a string literal");
    }
    item.query_vec = parseVectorStr(cur_.text);
    advance();
    expect(TokKind::RParen, ")");
    if (cur_.kind == TokKind::KwAs) {
      advance();
      item.alias = parseIdent();
    } else if (cur_.kind == TokKind::Ident) {
      item.alias = cur_.text;
      advance();
    }
    return item;
  }
  item.kind = SelectItem::Column;
  item.column = parseIdent();
  if (cur_.kind == TokKind::KwAs) {
    advance();
    item.alias = parseIdent();
  } else if (cur_.kind == TokKind::Ident) {
    item.alias = cur_.text;
    advance();
  }
  return item;
}

Condition Parser::parseCondition() {
  Condition c;
  c.left_col = parseIdent();
  switch (cur_.kind) {
    case TokKind::Eq: c.op = Condition::Eq; break;
    case TokKind::Lt: c.op = Condition::Lt; break;
    case TokKind::Le: c.op = Condition::Le; break;
    case TokKind::Gt: c.op = Condition::Gt; break;
    case TokKind::Ge: c.op = Condition::Ge; break;
    default:
      if (cur_.kind == TokKind::Eof) incomplete_ = true;
      throw SqlError(ErrKind::SyntaxError, "expected a comparison operator");
  }
  advance();
  if (cur_.kind == TokKind::Ident) {
    c.right_is_col = true;
    c.right_col = cur_.text;
    advance();
  } else {
    c.right_val = parseLiteral();
  }
  return c;
}

Predicate Parser::parseWhere() {
  Predicate p;
  while (true) {
    p.conds.push_back(parseCondition());
    if (cur_.kind == TokKind::KwAnd) { advance(); continue; }
    break;
  }
  return p;
}

std::vector<OrderItem> Parser::parseOrderBy() {
  std::vector<OrderItem> items;
  while (true) {
    OrderItem it;
    it.expr = parseIdent();
    if (cur_.kind == TokKind::KwAsc) { advance(); }
    else if (cur_.kind == TokKind::KwDesc) { it.desc = true; advance(); }
    items.push_back(std::move(it));
    if (cur_.kind == TokKind::Comma) { advance(); continue; }
    break;
  }
  return items;
}

std::unique_ptr<Statement> Parser::parseSelect() {
  advance();  // SELECT
  auto st = std::make_unique<Statement>();
  st->kind = Statement::Select;
  while (true) {
    st->select_items.push_back(parseSelectItem());
    if (cur_.kind == TokKind::Comma) { advance(); continue; }
    break;
  }
  expect(TokKind::KwFrom, "FROM");
  st->table_name = parseIdent();
  if (cur_.kind == TokKind::KwWhere) {
    advance();
    st->where = parseWhere();
  }
  if (cur_.kind == TokKind::KwOrder) {
    advance();
    expect(TokKind::KwBy, "BY");
    st->order_by = parseOrderBy();
  }
  if (cur_.kind == TokKind::KwLimit) {
    advance();
    st->limit = parseIntLiteral();
  }
  return st;
}

std::unique_ptr<Statement> Parser::parseUpdate() {
  advance();  // UPDATE
  auto st = std::make_unique<Statement>();
  st->kind = Statement::Update;
  st->table_name = parseIdent();
  expect(TokKind::KwSet, "SET");
  while (true) {
    std::string col = parseIdent();
    expect(TokKind::Eq, "=");
    Cell val = parseLiteral();
    st->updates.emplace_back(col, val);
    if (cur_.kind == TokKind::Comma) { advance(); continue; }
    break;
  }
  if (cur_.kind == TokKind::KwWhere) {
    advance();
    st->where = parseWhere();
  }
  return st;
}

std::unique_ptr<Statement> Parser::parseDelete() {
  advance();  // DELETE
  expect(TokKind::KwFrom, "FROM");
  auto st = std::make_unique<Statement>();
  st->kind = Statement::Delete;
  st->table_name = parseIdent();
  if (cur_.kind == TokKind::KwWhere) {
    advance();
    st->where = parseWhere();
  }
  return st;
}

}  // namespace mvd
