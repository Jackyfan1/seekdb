#include <signal.h>

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
#include "sql/lexer.h"
#include "sql/parser.h"

namespace mvd {

namespace {

volatile sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

std::string renderFloat(double d) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6f", d);
  return buf;
}

std::string renderVector(const std::vector<float>& v) {
  std::string out = "[";
  char buf[64];
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += ",";
    std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(v[i]));
    out += buf;
  }
  out += "]";
  return out;
}

std::string renderCell(const Cell& c, const Config& cfg) {
  switch (c.kind) {
    case Cell::Kind::Int: return std::to_string(c.i);
    case Cell::Kind::Real: return renderFloat(c.r);
    case Cell::Kind::Str: return c.s;
    case Cell::Kind::Vec: return renderVector(c.v);
  }
  return "";
}

class Output {
 public:
  explicit Output(const Config& cfg) : cfg_(cfg) {}

  void emit(const std::string& line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
  }

  void rows(const QueryResult& r) {
    if (!r.has_rows) return;
    for (const auto& row : r.rows) {
      std::string line;
      for (size_t i = 0; i < row.size(); ++i) {
        if (i) line += cfg_.row_separator;
        line += renderCell(row[i], cfg_);
      }
      emit(line);
    }
  }

  void okN(int64_t n) { emit(cfg_.ok_prefix + " " + std::to_string(n)); }
  void ok() { emit(cfg_.ok_prefix); }

  void error(const SqlError& e) {
    emit(cfg_.error_prefix + " " + ErrName(e.kind_of()) + ": " + e.what());
  }

  void internalError(const std::string& msg) {
    emit(cfg_.error_prefix + " INTERNAL_ERROR: " + msg);
  }

  void ready() {
    if (!cfg_.ready_marker.empty()) emit(cfg_.ready_marker);
  }

  void flush() { std::fflush(stdout); }

 private:
  const Config& cfg_;
};

class Session {
 public:
  Session(Engine& engine, const Config& cfg) : engine_(engine), out_(cfg) {}

  int run() {
    std::string line;
    while (!g_stop) {
      line.clear();
      if (!std::getline(std::cin, line)) {
        drain(true);
        break;
      }
      buffer_ += line;
      buffer_ += '\n';
      drain(false);
    }
    return 0;
  }

 private:
  void drain(bool at_eof) {
    while (!g_stop) {
      Parser parser(buffer_, consumed_);
      std::unique_ptr<Statement> st;
      try {
        st = parser.parseStatement();
      } catch (const SqlError& e) {
        if (parser.incomplete() && !at_eof) return;
        out_.error(e);
        out_.ready();
        out_.flush();
        skipToNextStatement();
        continue;
      } catch (const std::exception& e) {
        out_.internalError(e.what());
        out_.ready();
        out_.flush();
        skipToNextStatement();
        continue;
      }

      if (!st) {
        // 无更多语句：等待更多输入（或结束）。
        if (at_eof) consumed_ = buffer_.size();
        return;
      }

      consumed_ = parser.statementEnd();
      QueryResult res;
      try {
        engine_.execute(*st, res);
        out_.rows(res);
        if (res.affected >= 0) out_.okN(res.affected);
        else if (!res.has_rows) out_.ok();
      } catch (const SqlError& e) {
        out_.error(e);
      } catch (const std::exception& e) {
        out_.internalError(e.what());
      }
      out_.ready();
      out_.flush();
      trim();
    }
  }

  void skipToNextStatement() {
    Lexer lex(buffer_, consumed_);
    while (true) {
      Token t = lex.next();
      if (t.kind == TokKind::Eof) {
        consumed_ = buffer_.size();
        return;
      }
      if (t.kind == TokKind::Semicolon) {
        consumed_ = lex.offset();
        return;
      }
    }
  }

  void trim() {
    if (consumed_ >= buffer_.size()) {
      buffer_.clear();
      consumed_ = 0;
      return;
    }
    if (buffer_.size() > 64 * 1024) {
      buffer_.erase(0, consumed_);
      consumed_ = 0;
    }
  }

  Engine& engine_;
  Output out_;
  std::string buffer_;
  size_t consumed_ = 0;
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

  Session session(*engine, cfg);
  int rc = session.run();
  try {
    engine->shutdown();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "shutdown failed: %s\n", e.what());
  }
  return rc;
}

}  // namespace

}  // namespace mvd

int main(int argc, char** argv) {
  ::signal(SIGPIPE, SIG_IGN);
  ::signal(SIGINT, mvd::onSignal);
  ::signal(SIGTERM, mvd::onSignal);
  return mvd::realMain(argc, argv);
}
