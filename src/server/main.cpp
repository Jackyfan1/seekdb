#include <signal.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

#include "common/config.h"
#include "common/errors.h"
#include "engine.h"
#include "sql/ast.h"
#include "sql/parser.h"

namespace mvd {

namespace {

volatile sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

// ---------- JSON 输出 ----------

void appendJsonString(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);  // UTF-8 原样输出
        }
    }
  }
  out += '"';
}

std::string formatReal(double d) {
  if (!std::isfinite(d)) return "0.0";  // 协议不允许 NaN/Infinity
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6f", d);
  return buf;
}

std::string formatVecComponent(float f) {
  if (!std::isfinite(f)) return "0.0";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(f));
  return buf;
}

void appendCell(std::string& out, const Cell& c) {
  switch (c.kind) {
    case Cell::Kind::Int:
      out += std::to_string(c.i);
      break;
    case Cell::Kind::Real:
      out += formatReal(c.r);
      break;
    case Cell::Kind::Str:
      appendJsonString(out, c.s);
      break;
    case Cell::Kind::Vec: {
      out += '[';
      for (size_t i = 0; i < c.v.size(); ++i) {
        if (i) out += ',';
        out += formatVecComponent(c.v[i]);
      }
      out += ']';
      break;
    }
  }
}

// ---------- JSON 请求解析（JSONL，每行一个请求对象） ----------

class JsonReader {
 public:
  explicit JsonReader(const std::string& s) : s_(s) {}

  bool parseRequest(std::string& id_raw, std::string& op, std::string& sql,
                    std::string& mode, std::string& err) {
    ws();
    if (!take('{')) return fail("request must be a JSON object", err);
    ws();
    if (peek() == '}') {
      ++i_;
      ws();
      return finish(err);
    }
    while (true) {
      ws();
      if (peek() != '"') return fail("expected object key", err);
      std::string key = readString(err);
      if (!err.empty()) return false;
      ws();
      if (!take(':')) return fail("expected ':'", err);
      ws();
      size_t start = i_;
      if (key == "id") {
        if (!skipValue(err)) return false;
        id_raw = s_.substr(start, i_ - start);
      } else if (key == "op") {
        if (peek() != '"') return fail("op must be a string", err);
        op = readString(err);
        if (!err.empty()) return false;
      } else if (key == "sql") {
        if (peek() != '"') return fail("sql must be a string", err);
        sql = readString(err);
        if (!err.empty()) return false;
      } else if (key == "search_mode") {
        if (peek() != '"') return fail("search_mode must be a string", err);
        mode = readString(err);
        if (!err.empty()) return false;
      } else {
        if (!skipValue(err)) return false;
      }
      ws();
      if (take(',')) continue;
      if (take('}')) break;
      return fail("expected ',' or '}'", err);
    }
    ws();
    return finish(err);
  }

 private:
  bool finish(std::string& err) {
    if (i_ != s_.size()) return fail("trailing characters after JSON object", err);
    return true;
  }

  bool fail(const char* msg, std::string& err) {
    err = std::string(msg) + " at offset " + std::to_string(i_);
    return false;
  }

  void ws() {
    while (i_ < s_.size() &&
           (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n')) {
      ++i_;
    }
  }

  char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }

  bool take(char c) {
    if (i_ < s_.size() && s_[i_] == c) {
      ++i_;
      return true;
    }
    return false;
  }

  static void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  bool readHex4(unsigned& v, std::string& err) {
    v = 0;
    for (int k = 0; k < 4; ++k) {
      if (i_ >= s_.size()) return fail("truncated \\u escape", err);
      char c = s_[i_++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
      else return fail("invalid \\u escape", err);
    }
    return true;
  }

  std::string readString(std::string& err) {
    std::string out;
    if (!take('"')) {
      fail("expected string", err);
      return out;
    }
    while (i_ < s_.size()) {
      unsigned char c = static_cast<unsigned char>(s_[i_++]);
      if (c == '"') return out;
      if (c == '\\') {
        if (i_ >= s_.size()) {
          fail("unterminated escape", err);
          return out;
        }
        char e = s_[i_++];
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            unsigned cp = 0;
            if (!readHex4(cp, err)) return out;
            if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 5 < s_.size() &&
                s_[i_] == '\\' && s_[i_ + 1] == 'u') {
              i_ += 2;
              unsigned low = 0;
              if (!readHex4(low, err)) return out;
              if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
              }
            }
            appendUtf8(out, cp);
            break;
          }
          default:
            fail("invalid escape", err);
            return out;
        }
        continue;
      }
      out += static_cast<char>(c);
    }
    fail("unterminated string", err);
    return out;
  }

  bool skipValue(std::string& err) {
    ws();
    char c = peek();
    if (c == '"') {
      readString(err);
      return err.empty();
    }
    if (c == '{' || c == '[') {
      std::string stack;
      while (i_ < s_.size()) {
        char ch = s_[i_];
        if (ch == '"') {
          readString(err);
          if (!err.empty()) return false;
          continue;
        }
        ++i_;
        if (ch == '{' || ch == '[') {
          stack.push_back(ch);
        } else if (ch == '}' || ch == ']') {
          if (stack.empty()) return fail("unbalanced JSON value", err);
          char open = stack.back();
          stack.pop_back();
          if ((ch == '}') != (open == '{')) return fail("mismatched JSON brackets", err);
          if (stack.empty()) return true;
        }
      }
      return fail("unterminated JSON value", err);
    }
    // number / true / false / null
    size_t start = i_;
    while (i_ < s_.size()) {
      char ch = s_[i_];
      if (ch == ',' || ch == '}' || ch == ']' || ch == ' ' || ch == '\t' ||
          ch == '\r' || ch == '\n') {
        break;
      }
      ++i_;
    }
    if (i_ == start) return fail("empty JSON value", err);
    return true;
  }

  const std::string& s_;
  size_t i_ = 0;
};

// ---------- 输出 ----------

class Output {
 public:
  void emit(const std::string& line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
  }

  void ready() { emit("{\"type\":\"ready\"}"); }

  void ok(const std::string& id) { emit("{\"id\":" + id + ",\"ok\":true}"); }

  void result(const std::string& id, const QueryResult& res) {
    std::string out = "{\"id\":" + id + ",\"ok\":true";
    if (res.affected >= 0) {
      out += ",\"affected_rows\":" + std::to_string(res.affected);
    } else if (res.has_rows) {
      out += ",\"columns\":[";
      for (size_t i = 0; i < res.columns.size(); ++i) {
        if (i) out += ',';
        appendJsonString(out, res.columns[i]);
      }
      out += "],\"rows\":[";
      for (size_t r = 0; r < res.rows.size(); ++r) {
        if (r) out += ',';
        out += '[';
        for (size_t c = 0; c < res.rows[r].size(); ++c) {
          if (c) out += ',';
          appendCell(out, res.rows[r][c]);
        }
        out += ']';
      }
      out += ']';
    }
    out += '}';
    emit(out);
  }

  void error(const std::string& id, const std::string& code, const std::string& message) {
    std::string out = "{\"id\":" + id + ",\"ok\":false,\"error\":{\"code\":";
    appendJsonString(out, code.empty() ? "internal_error" : code);
    out += ",\"message\":";
    appendJsonString(out, message);
    out += "}}";
    emit(out);
  }
};

// ---------- 会话 ----------

std::string trimCopy(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

class Session {
 public:
  explicit Session(Engine& engine) : engine_(engine) {}

  int run() {
    std::string line;
    while (!g_stop && std::getline(std::cin, line)) {
      if (line.empty() || trimCopy(line).empty()) continue;  // 空行不是请求
      std::string id_raw = "null", op, sql, mode, perr;
      if (!JsonReader(line).parseRequest(id_raw, op, sql, mode, perr)) {
        out_.error(id_raw, "bad_request", perr);
        continue;
      }
      if (op == "shutdown") {
        engine_.shutdown();
        out_.ok(id_raw);
        return 0;
      }
      if (op != "execute") {
        out_.error(id_raw, "unsupported_op", "unsupported op: " + op);
        continue;
      }
      execute(id_raw, sql);
    }
    engine_.shutdown();
    return 0;
  }

 private:
  void execute(const std::string& id, const std::string& sql) {
    std::string text = trimCopy(sql);
    if (text.empty()) {
      out_.ok(id);
      return;
    }
    if (text.back() != ';') text += ';';  // 协议外的容错：缺分号也按一条语句执行

    QueryResult res;
    try {
      Parser parser(text, 0);
      std::unique_ptr<Statement> st = parser.parseStatement();
      if (!st) {
        out_.ok(id);
        return;
      }
      engine_.execute(*st, res);
      out_.result(id, res);
    } catch (const SqlError& e) {
      out_.error(id, ErrCode(e.kind_of()), e.what());
    } catch (const std::exception& e) {
      out_.error(id, "internal_error", e.what());
    }
  }

  Engine& engine_;
  Output out_;
};

int realMain(int argc, char** argv) {
  Config cfg;
  try {
    cfg = Config::fromArgs(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "argument error: %s\n", e.what());
    return 2;
  }

  std::unique_ptr<Engine> engine;
  try {
    engine = std::make_unique<Engine>(cfg);
    engine->startup();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "startup failed: %s\n", e.what());
    return 1;
  }

  Output out;
  out.ready();  // 必须在等待第一条请求之前主动输出并 flush

  Session session(*engine);
  return session.run();
}

}  // namespace

}  // namespace mvd

int main(int argc, char** argv) {
  ::signal(SIGPIPE, SIG_IGN);
  ::signal(SIGINT, mvd::onSignal);
  ::signal(SIGTERM, mvd::onSignal);
  return mvd::realMain(argc, argv);
}
