#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mini {

struct SqlError : std::runtime_error {
  std::string_view code;
  SqlError(std::string_view code, const char* message) : std::runtime_error(message), code(code) {}
};

enum class Type : uint8_t { bigint, varchar, vector };
struct Column {
  std::string name;
  Type type;
  uint32_t length = 0;
  bool primary = false;
};

enum class LiteralKind { number, string, vector };
using SqlText = std::variant<std::string_view, std::string>;

inline std::string_view text_view(const SqlText& text) {
  if (const auto* view = std::get_if<std::string_view>(&text)) return *view;
  return std::get<std::string>(text);
}

struct Literal {
  LiteralKind kind;
  SqlText text;
};

enum class Comparison { eq, lt, le, gt, ge };
struct Predicate {
  std::string column;
  Comparison comparison;
  Literal value;
};

struct Expression {
  std::string column;
  std::optional<Literal> query;
  std::string alias;
};
struct Ordering {
  Expression key;
  bool descending;
};
struct CreateTable { std::string name; std::vector<Column> columns; };
struct CreateIndex { std::string name, table, column; };
struct Insert {
  std::string table;
  std::vector<std::string> columns;
  std::vector<std::vector<Literal>> rows;
};
struct Select {
  std::string table;
  bool star = false;
  std::vector<Expression> projections;
  std::vector<Predicate> filter;
  std::vector<Ordering> order;
  std::optional<uint64_t> limit;
};
struct Update {
  std::string table;
  std::vector<std::pair<std::string, Literal>> assignments;
  std::vector<Predicate> filter;
};
struct Delete { std::string table; std::vector<Predicate> filter; };
enum class Transaction { begin, commit, rollback };
using Statement = std::variant<CreateTable, CreateIndex, Insert, Select, Update, Delete, Transaction>;

// Unescaped literals borrow sql, which must outlive execution of the statement.
Statement parse(std::string_view sql);
size_t utf8_length(std::string_view value);
int64_t bigint(const Literal& value);
std::string_view varchar(const Literal& value, uint32_t max_chars);
std::vector<float> vector_value(const Literal& value, uint32_t dimension);

}  // namespace mini
