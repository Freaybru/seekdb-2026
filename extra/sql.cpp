#include "sql.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

namespace mini {
namespace {

[[noreturn]] void syntax(const char* message = "invalid SQL syntax") {
  throw SqlError("SYNTAX", message);
}

bool space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}
bool digit(char ch) { return ch >= '0' && ch <= '9'; }
bool letter(char ch) {
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

size_t number_end(std::string_view text, size_t pos) {
  const size_t start = pos;
  if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
  const size_t whole = pos;
  while (pos < text.size() && digit(text[pos])) ++pos;
  bool has_digit = pos != whole;
  if (pos < text.size() && text[pos] == '.') {
    const size_t fraction = ++pos;
    while (pos < text.size() && digit(text[pos])) ++pos;
    has_digit = has_digit || pos != fraction;
  }
  if (!has_digit) return start;
  if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
    const size_t exponent = pos++;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
    const size_t digits = pos;
    while (pos < text.size() && digit(text[pos])) ++pos;
    if (pos == digits) return exponent;
  }
  return pos;
}

void lowercase(std::string& value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
}

enum class TokenKind { end, word, identifier, number, string, vector, symbol };
struct Token {
  TokenKind kind;
  SqlText value;

  std::string_view text() const { return text_view(value); }
};

class Lexer {
 public:
  explicit Lexer(std::string_view sql) : sql_(sql) {}

  Token next() {
    skip();
    if (position_ == sql_.size()) return {TokenKind::end, {}};
    const size_t start = position_++;
    const char ch = sql_[start];
    if (letter(ch)) {
      while (position_ < sql_.size() &&
             (letter(sql_[position_]) || digit(sql_[position_]))) {
        ++position_;
      }
      std::string word(sql_.substr(start, position_ - start));
      lowercase(word);
      return {TokenKind::word, std::move(word)};
    }
    if (ch == '\'' || ch == 0x60) return quoted(ch);
    if (ch == '[') {
      const size_t close = sql_.find(']', position_);
      if (close == sql_.npos) syntax("unterminated vector");
      position_ = close + 1;
      return {TokenKind::vector, sql_.substr(start, position_ - start)};
    }
    if (digit(ch) || ch == '+' || ch == '-' || ch == '.') {
      position_ = number_end(sql_, start);
      if (position_ != start) {
        return {TokenKind::number, sql_.substr(start, position_ - start)};
      }
      position_ = start + 1;
    }
    if (std::string_view("(),;*=<>").find(ch) != std::string_view::npos) {
      if ((ch == '<' || ch == '>') && position_ < sql_.size() && sql_[position_] == '=') {
        ++position_;
      }
      return {TokenKind::symbol, sql_.substr(start, position_ - start)};
    }
    syntax("unexpected token");
  }

 private:
  void skip() {
    while (position_ < sql_.size()) {
      if (space(sql_[position_])) {
        ++position_;
      } else if (sql_[position_] == '#' ||
                 (sql_.substr(position_, 2) == "--" &&
                  (position_ + 2 == sql_.size() || space(sql_[position_ + 2])))) {
        position_ = sql_.find('\n', position_);
        if (position_ == sql_.npos) position_ = sql_.size();
      } else if (sql_.substr(position_, 2) == "/*") {
        const size_t close = sql_.find("*/", position_ + 2);
        if (close == sql_.npos) syntax("unterminated comment");
        position_ = close + 2;
      } else {
        break;
      }
    }
  }

  Token quoted(char delimiter) {
    const auto close = sql_.find(delimiter, position_);
    if (close == sql_.npos) syntax("unterminated quoted value");
    const auto plain = sql_.substr(position_, close - position_);
    if ((close + 1 == sql_.size() || sql_[close + 1] != delimiter) &&
        (delimiter != '\'' || plain.find('\\') == plain.npos)) {
      position_ = close + 1;
      if (delimiter == '\'') return {TokenKind::string, plain};
      if (plain.empty()) syntax("empty identifier");
      std::string identifier(plain);
      lowercase(identifier);
      return {TokenKind::identifier, std::move(identifier)};
    }
    std::string value;
    while (position_ < sql_.size()) {
      char ch = sql_[position_++];
      if (ch == delimiter) {
        if (position_ == sql_.size() || sql_[position_] != delimiter) {
          if (delimiter == '\'') return {TokenKind::string, std::move(value)};
          if (value.empty()) syntax("empty identifier");
          lowercase(value);
          return {TokenKind::identifier, std::move(value)};
        }
        ++position_;
      } else if (ch == '\\' && delimiter == '\'' && position_ < sql_.size()) {
        ch = sql_[position_++];
        switch (ch) {
          case '0': ch = '\0'; break;
          case 'b': ch = '\b'; break;
          case 'n': ch = '\n'; break;
          case 'r': ch = '\r'; break;
          case 't': ch = '\t'; break;
          case 'Z': ch = '\x1a'; break;
        }
      }
      value.push_back(ch);
    }
    syntax("unterminated quoted value");
  }

  std::string_view sql_;
  size_t position_ = 0;
};

template <typename Number>
bool integer_value(std::string_view text, Number& value) {
  if (!text.empty() && text.front() == '+') text.remove_prefix(1);
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

class Parser {
 public:
  explicit Parser(std::string_view sql) : lexer_(sql), token_(lexer_.next()) {}

  Statement parse() {
    Statement statement = command();
    accept(";");
    if (token_.kind != TokenKind::end) syntax("unexpected trailing SQL");
    return statement;
  }

 private:
  Token take() { return std::exchange(token_, lexer_.next()); }

  bool accept(std::string_view word) {
    if ((token_.kind != TokenKind::word && token_.kind != TokenKind::symbol) ||
        token_.text() != word) {
      return false;
    }
    take();
    return true;
  }

  void expect(std::string_view word) {
    if (!accept(word)) syntax();
  }

  std::string identifier() {
    if (token_.kind != TokenKind::word && token_.kind != TokenKind::identifier) {
      syntax("expected identifier");
    }
    auto token = take();
    return std::get<std::string>(std::move(token.value));
  }

  uint64_t nonnegative() {
    uint64_t value = 0;
    if (token_.kind != TokenKind::number || !integer_value(token_.text(), value)) {
      syntax("expected nonnegative integer");
    }
    take();
    return value;
  }

  Literal literal() {
    LiteralKind kind;
    switch (token_.kind) {
      case TokenKind::number: kind = LiteralKind::number; break;
      case TokenKind::string: kind = LiteralKind::string; break;
      case TokenKind::vector: kind = LiteralKind::vector; break;
      default: syntax("expected literal");
    }
    return {kind, take().value};
  }

  Statement command() {
    if (accept("create")) {
      if (accept("table")) return create_table();
      if (accept("vector")) {
        expect("index");
        CreateIndex index;
        index.name = identifier();
        expect("on");
        index.table = identifier();
        expect("(");
        index.column = identifier();
        expect(")");
        return index;
      }
    } else if (accept("insert")) {
      return insert();
    } else if (accept("select")) {
      return select();
    } else if (accept("update")) {
      Update result;
      result.table = identifier();
      expect("set");
      do {
        std::string column = identifier();
        expect("=");
        result.assignments.emplace_back(std::move(column), literal());
      } while (accept(","));
      if (accept("where")) filter(result.filter);
      return result;
    } else if (accept("delete")) {
      expect("from");
      Delete result{identifier(), {}};
      if (accept("where")) filter(result.filter);
      return result;
    } else if (accept("begin")) {
      return Transaction::begin;
    } else if (accept("commit")) {
      return Transaction::commit;
    } else if (accept("rollback")) {
      return Transaction::rollback;
    }
    throw SqlError("unsupported_sql", "unsupported SQL statement");
  }

  CreateTable create_table() {
    CreateTable result{identifier(), {}};
    std::optional<std::string> primary;
    expect("(");
    do {
      if (accept("primary")) {
        if (primary) throw SqlError("SCHEMA", "multiple primary keys");
        expect("key");
        expect("(");
        primary = identifier();
        expect(")");
        continue;
      }
      Column column{identifier(), Type::bigint};
      if (!accept("bigint")) {
        if (accept("varchar")) {
          column.type = Type::varchar;
        } else if (accept("vector")) {
          column.type = Type::vector;
        } else {
          throw SqlError("unsupported_sql", "unsupported column type");
        }
        expect("(");
        const uint64_t length = nonnegative();
        if (length == 0 || length > std::numeric_limits<uint32_t>::max()) {
          throw SqlError("SCHEMA", "invalid column length");
        }
        column.length = static_cast<uint32_t>(length);
        expect(")");
      }
      if (accept("primary")) {
        expect("key");
        if (primary) throw SqlError("SCHEMA", "multiple primary keys");
        primary = column.name;
      }
      result.columns.push_back(std::move(column));
    } while (accept(","));
    expect(")");
    if (primary) {
      const auto it = std::find_if(result.columns.begin(), result.columns.end(),
                                   [&](const Column& column) { return column.name == *primary; });
      if (it == result.columns.end()) throw SqlError("SCHEMA", "unknown primary key column");
      it->primary = true;
    }
    return result;
  }

  Insert insert() {
    expect("into");
    Insert result{identifier(), {}, {}};
    if (accept("(")) {
      do {
        result.columns.push_back(identifier());
      } while (accept(","));
      expect(")");
    }
    expect("values");
    do {
      expect("(");
      auto& row = result.rows.emplace_back();
      do {
        row.push_back(literal());
      } while (accept(","));
      expect(")");
    } while (accept(","));
    return result;
  }

  Expression expression() {
    const bool distance = token_.kind == TokenKind::word && token_.text() == "l2_distance";
    Expression result{identifier(), {}, {}};
    if (distance && accept("(")) {
      result.column = identifier();
      expect(",");
      result.query = literal();
      expect(")");
    }
    return result;
  }

  Select select() {
    Select result;
    if (accept("*")) {
      result.star = true;
    } else {
      do {
        auto projection = expression();
        if (accept("as")) projection.alias = identifier();
        result.projections.push_back(std::move(projection));
      } while (accept(","));
    }
    expect("from");
    result.table = identifier();
    if (accept("where")) filter(result.filter);
    if (accept("order")) {
      expect("by");
      do {
        auto key = expression();
        const bool descending = accept("desc");
        if (!descending) accept("asc");
        result.order.push_back({std::move(key), descending});
      } while (accept(","));
    }
    if (accept("limit")) result.limit = nonnegative();
    return result;
  }

  void filter(std::vector<Predicate>& out, size_t depth = 0) {
    if (depth >= 64) syntax("WHERE expression nested too deeply");
    do {
      if (accept("(")) {
        filter(out, depth + 1);
        expect(")");
      } else {
        std::string column = identifier();
        Comparison comparison;
        if (accept("=")) comparison = Comparison::eq;
        else if (accept("<")) comparison = Comparison::lt;
        else if (accept("<=")) comparison = Comparison::le;
        else if (accept(">")) comparison = Comparison::gt;
        else if (accept(">=")) comparison = Comparison::ge;
        else syntax("expected comparison operator");
        out.push_back({std::move(column), comparison, literal()});
      }
    } while (accept("and"));
  }

  Lexer lexer_;
  Token token_;
};

}  // namespace

size_t utf8_length(std::string_view value) {
  size_t characters = 0;
  for (size_t i = 0; i < value.size();) {
    if (value.size() - i >= sizeof(uint64_t)) {
      uint64_t bytes;
      std::memcpy(&bytes, value.data() + i, sizeof(bytes));
      if ((bytes & 0x8080808080808080ull) == 0) {
        i += sizeof(bytes);
        characters += sizeof(bytes);
        continue;
      }
    }
    ++characters;
    const auto first = static_cast<unsigned char>(value[i++]);
    if (first <= 0x7f) continue;
    unsigned count = 0;
    uint32_t codepoint = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      count = 1;
      codepoint = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      count = 2;
      codepoint = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      count = 3;
      codepoint = first & 0x07;
    }
    if (!count || value.size() - i < count) throw SqlError("VALUE", "invalid UTF-8");
    for (unsigned j = 0; j < count; ++j) {
      const auto ch = static_cast<unsigned char>(value[i++]);
      if ((ch & 0xc0) != 0x80) throw SqlError("VALUE", "invalid UTF-8");
      codepoint = (codepoint << 6) | (ch & 0x3f);
    }
    if ((count == 2 && codepoint < 0x800) || (count == 3 && codepoint < 0x10000) ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) {
      throw SqlError("VALUE", "invalid UTF-8");
    }
  }
  return characters;
}

Statement parse(std::string_view sql) { return Parser(sql).parse(); }

int64_t bigint(const Literal& value) {
  int64_t result = 0;
  if (value.kind != LiteralKind::number || !integer_value(text_view(value.text), result)) {
    throw SqlError("VALUE", "invalid BIGINT");
  }
  return result;
}

std::string_view varchar(const Literal& value, uint32_t max_chars) {
  if (value.kind != LiteralKind::string) throw SqlError("VALUE", "expected a string");
  const auto text = text_view(value.text);
  if (utf8_length(text) > max_chars) throw SqlError("VALUE", "VARCHAR is too long");
  return text;
}

std::vector<float> vector_value(const Literal& value, uint32_t dimension) {
  const std::string_view text = text_view(value.text);
  size_t pos = 0;
  const auto consume = [&](char ch) {
    while (pos < text.size() && space(text[pos])) ++pos;
    if (pos == text.size() || text[pos] != ch) return false;
    ++pos;
    return true;
  };
  if (value.kind == LiteralKind::number || !consume('[')) {
    throw SqlError("VALUE", "expected a vector");
  }
  std::vector<float> result;
  result.reserve(std::min<size_t>(dimension, text.size() / 2));
  size_t count = 0;
  if (!consume(']')) {
    do {
      while (pos < text.size() && space(text[pos])) ++pos;
      // strtof handles rounding at the float32 subnormal boundary, including
      // finite zero, when from_chars reports an out-of-range value.
      float component = 0;
      const char* first = text.data() + pos;
      const char* last = text.data() + text.size();
      const char* start = first;
      if (start != last && (*start == '+' || *start == '-')) ++start;
      if (start == last || (!digit(*start) && *start != '.')) {
        throw SqlError("VALUE", "invalid vector component");
      }
      const auto converted = std::from_chars(*first == '+' ? first + 1 : first, last, component);
      if (converted.ec == std::errc::result_out_of_range) {
        const std::string token(first, converted.ptr);
        char* tail = nullptr;
        component = std::strtof(token.c_str(), &tail);
        if (tail != token.data() + token.size()) throw SqlError("VALUE", "invalid vector component");
      } else if (converted.ec != std::errc{}) {
        throw SqlError("VALUE", "invalid vector component");
      }
      if (!std::isfinite(component)) throw SqlError("VALUE", "nonfinite float32 component");
      if (count < dimension) result.push_back(component);
      ++count;
      pos = converted.ptr - text.data();
    } while (consume(','));
    if (!consume(']')) throw SqlError("VALUE", "invalid vector syntax");
  }
  while (pos < text.size() && space(text[pos])) ++pos;
  if (pos != text.size()) throw SqlError("VALUE", "invalid vector syntax");
  if (count != dimension) throw SqlError("dimension_mismatch", "wrong vector dimension");
  return result;
}

}  // namespace mini
