#pragma once

#include <stdexcept>
#include <string>

namespace mvd {

enum class ErrKind {
  SyntaxError,
  UnknownTable,
  UnknownColumn,
  DuplicateTable,
  DuplicateKey,
  TypeMismatch,
  PrimaryKeyRequired,
  BadVector,
  Unsupported,
  NotInTransaction,
  InTransaction,
  InternalError,
};

inline const char* ErrName(ErrKind k) {
  switch (k) {
    case ErrKind::SyntaxError: return "SYNTAX_ERROR";
    case ErrKind::UnknownTable: return "UNKNOWN_TABLE";
    case ErrKind::UnknownColumn: return "UNKNOWN_COLUMN";
    case ErrKind::DuplicateTable: return "DUPLICATE_TABLE";
    case ErrKind::DuplicateKey: return "DUPLICATE_KEY";
    case ErrKind::TypeMismatch: return "TYPE_MISMATCH";
    case ErrKind::PrimaryKeyRequired: return "PRIMARY_KEY_REQUIRED";
    case ErrKind::BadVector: return "BAD_VECTOR";
    case ErrKind::Unsupported: return "UNSUPPORTED";
    case ErrKind::NotInTransaction: return "NOT_IN_TRANSACTION";
    case ErrKind::InTransaction: return "IN_TRANSACTION";
    case ErrKind::InternalError: return "INTERNAL_ERROR";
  }
  return "INTERNAL_ERROR";
}

struct SqlError : std::runtime_error {
  ErrKind kind;
  SqlError(ErrKind k, const std::string& msg)
      : std::runtime_error(msg), kind(k) {}
  ErrKind kind_of() const { return kind; }
};

}  // namespace mvd
