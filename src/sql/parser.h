#pragma once

#include <memory>

#include "sql/ast.h"
#include "sql/lexer.h"

namespace mvd {

class Parser {
 public:
  Parser(const std::string& buf, size_t start);

  // 解析一条以分号结尾的语句；返回 nullptr 表示没有更多语句可解析。
  std::unique_ptr<Statement> parseStatement();
  bool incomplete() const { return incomplete_; }
  size_t statementEnd() const { return end_; }

 private:
  void advance();
  const Token& cur() const { return cur_; }
  void expect(TokKind k, const char* what);
  void expectSemicolon();

  std::unique_ptr<Statement> parseCreate();
  std::unique_ptr<Statement> parseInsert();
  std::unique_ptr<Statement> parseSelect();
  std::unique_ptr<Statement> parseUpdate();
  std::unique_ptr<Statement> parseDelete();
  std::unique_ptr<Statement> parseTxn(Statement::Kind k);
  std::unique_ptr<Statement> parseShow();

  Column parseColumnDef();
  void parseType(Column& col);
  Cell parseLiteral();
  Predicate parseWhere();
  Condition parseCondition();
  std::vector<OrderItem> parseOrderBy();
  SelectItem parseSelectItem();
  std::string parseIdent();
  int64_t parseIntLiteral();

  Lexer lexer_;
  Token cur_;
  bool incomplete_ = false;
  size_t end_ = 0;
};

// 把 '[a, b, c]' 形式的字符串解析为 float32 向量。
std::vector<float> parseVectorStr(const std::string& s);

}  // namespace mvd
