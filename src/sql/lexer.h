#pragma once

#include <string>

#include "common/errors.h"

namespace mvd {

enum class TokKind {
  Ident, IntLit, StrLit,
  KwCreate, KwTable, KwPrimary, KwKey, KwInsert, KwInto, KwValues,
  KwSelect, KwFrom, KwWhere, KwAnd, KwOrder, KwBy, KwAsc, KwDesc,
  KwLimit, KwUpdate, KwSet, KwDelete, KwBegin, KwCommit, KwRollback,
  KwAs, KwBigInt, KwVarchar, KwVector, KwL2Distance, KwShow, KwTables,
  Comma, LParen, RParen, Semicolon, Star, Eq, Lt, Le, Gt, Ge, Minus,
  Eof,
};

struct Token {
  TokKind kind = TokKind::Eof;
  std::string text;   // 标识符名 / 字符串内容 / 整数文本
  size_t offset = 0;  // 在输入流中的位置（用于跳过坏语句）
};

// 词法分析器：在给定缓冲区上按需取 token。
class Lexer {
 public:
  Lexer(const std::string& buf, size_t start);

  Token next();       // 读取下一个 token
  size_t offset() const { return pos_; }

 private:
  void skipWsAndComments();
  Token lexIdent();
  Token lexNumber();
  Token lexString();

  const std::string& buf_;
  size_t pos_;
};

}  // namespace mvd
