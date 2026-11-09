#include "sql/lexer.h"

#include <cctype>

namespace mvd {

namespace {

std::string upper(const std::string& s) {
  std::string r = s;
  for (auto& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return r;
}

struct KwEntry {
  const char* text;
  TokKind kind;
};

const KwEntry kKeywords[] = {
    {"CREATE", TokKind::KwCreate},   {"TABLE", TokKind::KwTable},
    {"PRIMARY", TokKind::KwPrimary}, {"KEY", TokKind::KwKey},
    {"INSERT", TokKind::KwInsert},   {"INTO", TokKind::KwInto},
    {"VALUES", TokKind::KwValues},   {"SELECT", TokKind::KwSelect},
    {"FROM", TokKind::KwFrom},       {"WHERE", TokKind::KwWhere},
    {"AND", TokKind::KwAnd},         {"ORDER", TokKind::KwOrder},
    {"BY", TokKind::KwBy},           {"ASC", TokKind::KwAsc},
    {"DESC", TokKind::KwDesc},       {"LIMIT", TokKind::KwLimit},
    {"UPDATE", TokKind::KwUpdate},   {"SET", TokKind::KwSet},
    {"DELETE", TokKind::KwDelete},   {"BEGIN", TokKind::KwBegin},
    {"COMMIT", TokKind::KwCommit},   {"ROLLBACK", TokKind::KwRollback},
    {"AS", TokKind::KwAs},           {"BIGINT", TokKind::KwBigInt},
    {"VARCHAR", TokKind::KwVarchar}, {"VECTOR", TokKind::KwVector},
    {"L2_DISTANCE", TokKind::KwL2Distance},
    {"SHOW", TokKind::KwShow},       {"TABLES", TokKind::KwTables},
};

TokKind keywordKind(const std::string& text) {
  for (const auto& e : kKeywords) {
    if (text == e.text) return e.kind;
  }
  return TokKind::Ident;
}

}  // namespace

Lexer::Lexer(const std::string& buf, size_t start) : buf_(buf), pos_(start) {}

void Lexer::skipWsAndComments() {
  while (pos_ < buf_.size()) {
    char c = buf_[pos_];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      ++pos_;
      continue;
    }
    if (c == '-' && pos_ + 1 < buf_.size() && buf_[pos_ + 1] == '-') {
      while (pos_ < buf_.size() && buf_[pos_] != '\n') ++pos_;
      continue;
    }
    if (c == '#') {
      while (pos_ < buf_.size() && buf_[pos_] != '\n') ++pos_;
      continue;
    }
    if (c == '/' && pos_ + 1 < buf_.size() && buf_[pos_ + 1] == '*') {
      pos_ += 2;
      while (pos_ + 1 < buf_.size() && !(buf_[pos_] == '*' && buf_[pos_ + 1] == '/')) ++pos_;
      pos_ = (pos_ + 1 < buf_.size()) ? pos_ + 2 : buf_.size();
      continue;
    }
    break;
  }
}

Token Lexer::next() {
  skipWsAndComments();
  Token t;
  t.offset = pos_;
  if (pos_ >= buf_.size()) {
    t.kind = TokKind::Eof;
    return t;
  }
  char c = buf_[pos_];

  if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
    return lexIdent();
  }
  if (std::isdigit(static_cast<unsigned char>(c))) {
    return lexNumber();
  }
  if (c == '\'') {
    return lexString();
  }

  // 单字符标点
  switch (c) {
    case ',': t.kind = TokKind::Comma; t.text = ","; ++pos_; return t;
    case '(': t.kind = TokKind::LParen; t.text = "("; ++pos_; return t;
    case ')': t.kind = TokKind::RParen; t.text = ")"; ++pos_; return t;
    case ';': t.kind = TokKind::Semicolon; t.text = ";"; ++pos_; return t;
    case '*': t.kind = TokKind::Star; t.text = "*"; ++pos_; return t;
    case '=': t.kind = TokKind::Eq; t.text = "="; ++pos_; return t;
    case '<':
      if (pos_ + 1 < buf_.size() && buf_[pos_ + 1] == '=') {
        t.kind = TokKind::Le; t.text = "<="; pos_ += 2;
      } else {
        t.kind = TokKind::Lt; t.text = "<"; ++pos_;
      }
      return t;
    case '>':
      if (pos_ + 1 < buf_.size() && buf_[pos_ + 1] == '=') {
        t.kind = TokKind::Ge; t.text = ">="; pos_ += 2;
      } else {
        t.kind = TokKind::Gt; t.text = ">"; ++pos_;
      }
      return t;
    default:
      throw SqlError(ErrKind::SyntaxError,
                     std::string("unexpected character '") + c + "'");
  }
}

Token Lexer::lexIdent() {
  size_t start = pos_;
  while (pos_ < buf_.size()) {
    char c = buf_[pos_];
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
      ++pos_;
    } else {
      break;
    }
  }
  Token t;
  t.offset = start;
  t.text = buf_.substr(start, pos_ - start);
  t.kind = keywordKind(upper(t.text));
  return t;
}

Token Lexer::lexNumber() {
  size_t start = pos_;
  while (pos_ < buf_.size() && std::isdigit(static_cast<unsigned char>(buf_[pos_]))) ++pos_;
  Token t;
  t.offset = start;
  t.text = buf_.substr(start, pos_ - start);
  t.kind = TokKind::IntLit;
  return t;
}

Token Lexer::lexString() {
  // 起始单引号
  ++pos_;
  std::string out;
  while (pos_ < buf_.size()) {
    char c = buf_[pos_];
    if (c == '\'') {
      if (pos_ + 1 < buf_.size() && buf_[pos_ + 1] == '\'') {
        out += '\'';  // '' 转义
        pos_ += 2;
        continue;
      }
      ++pos_;  // 结束单引号
      Token t;
      t.offset = pos_ - 1;
      t.text = out;
      t.kind = TokKind::StrLit;
      return t;
    }
    out += c;
    ++pos_;
  }
  throw SqlError(ErrKind::SyntaxError, "unterminated string literal");
}

}  // namespace mvd
