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
  DimensionMismatch,
  ValueTooLong,
  Unsupported,
  NotInTransaction,
  InTransaction,
  InternalError,
};

// 错误码：stdout 协议中的 error.code（小写语义名，非空字符串）。
inline const char* ErrCode(ErrKind k) {
  switch (k) {
    case ErrKind::SyntaxError: return "syntax_error";
    case ErrKind::UnknownTable: return "unknown_table";
    case ErrKind::UnknownColumn: return "unknown_column";
    case ErrKind::DuplicateTable: return "duplicate_table";
    case ErrKind::DuplicateKey: return "duplicate_key";
    case ErrKind::TypeMismatch: return "type_mismatch";
    case ErrKind::PrimaryKeyRequired: return "primary_key_required";
    case ErrKind::BadVector: return "invalid_vector";
    case ErrKind::DimensionMismatch: return "dimension_mismatch";
    case ErrKind::ValueTooLong: return "value_too_long";
    case ErrKind::Unsupported: return "unsupported_sql";
    case ErrKind::NotInTransaction: return "no_transaction";
    case ErrKind::InTransaction: return "transaction_active";
    case ErrKind::InternalError: return "internal_error";
  }
  return "internal_error";
}

struct SqlError : std::runtime_error {
  ErrKind kind;
  SqlError(ErrKind k, const std::string& msg)
      : std::runtime_error(msg), kind(k) {}
  ErrKind kind_of() const { return kind; }
};

}  // namespace mvd
