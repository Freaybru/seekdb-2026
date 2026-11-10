#pragma once

#include "sql.hpp"

#include <ostream>
#include <set>

namespace mini::wire {

struct Request {
  std::string_view id;
  std::string op;
  std::optional<std::string> sql;
  std::string mode = "exact";
};

[[noreturn]] inline void invalid(const char* message) {
  throw SqlError("invalid_request", message);
}

// Decode the fields used by the engine and validate the other JSON values.
// Keep the ID's original JSON span so even large integers round-trip exactly.
class RequestReader {
 public:
  explicit RequestReader(std::string_view line) : line_(line) {}

  Request read() {
    try {
      utf8_length(line_);
    } catch (const SqlError&) {
      invalid("request is not valid UTF-8");
    }
    Request request;
    object([&](std::string_view key) {
      if (key == "id") {
        skip_space();
        const size_t start = at_;
        value(0);
        request.id = line_.substr(start, at_ - start);
      } else if (key == "op") {
        request.op = string();
      } else if (key == "sql") {
        request.sql = string();
      } else if (key == "search_mode") {
        request.mode = string();
      } else {
        value(0);
      }
    });
    skip_space();
    if (at_ != line_.size()) invalid("unexpected data after request");
    if (request.id.empty()) invalid("request needs an id");
    return request;
  }

 private:
  static bool digit(char ch) { return ch >= '0' && ch <= '9'; }
  char peek() const { return at_ < line_.size() ? line_[at_] : '\0'; }
  char next() {
    if (at_ == line_.size()) invalid("incomplete JSON");
    return line_[at_++];
  }
  bool take(char ch) {
    if (peek() != ch) return false;
    ++at_;
    return true;
  }
  void skip_space() {
    while (at_ < line_.size() && std::string_view(" \r\n\t").find(peek()) != line_.npos) ++at_;
  }
  bool accept(char ch) {
    skip_space();
    return take(ch);
  }
  void expect(char ch) {
    if (!accept(ch)) invalid("unexpected JSON token");
  }
  void literal(std::string_view text) {
    if (!line_.substr(at_).starts_with(text)) invalid("invalid JSON literal");
    at_ += text.size();
  }

  uint32_t hex_word() {
    uint32_t word = 0;
    for (int i = 0; i < 4; ++i) {
      const char ch = next();
      word <<= 4;
      if (digit(ch)) word |= ch - '0';
      else if (ch >= 'a' && ch <= 'f') word |= ch - 'a' + 10;
      else if (ch >= 'A' && ch <= 'F') word |= ch - 'A' + 10;
      else invalid("invalid JSON Unicode escape");
    }
    return word;
  }

  static void append_utf8(std::string& out, uint32_t point) {
    const auto continuation = [&](unsigned shift) {
      out.push_back(static_cast<char>(0x80 | ((point >> shift) & 0x3f)));
    };
    if (point < 0x80) {
      out.push_back(static_cast<char>(point));
    } else if (point < 0x800) {
      out.push_back(static_cast<char>(0xc0 | (point >> 6)));
      continuation(0);
    } else if (point < 0x10000) {
      out.push_back(static_cast<char>(0xe0 | (point >> 12)));
      continuation(6);
      continuation(0);
    } else {
      out.push_back(static_cast<char>(0xf0 | (point >> 18)));
      continuation(12);
      continuation(6);
      continuation(0);
    }
  }

  std::string string() {
    expect('"');
    std::string result;
    for (char ch = next(); ch != '"'; ch = next()) {
      if (static_cast<unsigned char>(ch) < 0x20) invalid("unescaped JSON control character");
      if (ch != '\\') {
        result.push_back(ch);
        continue;
      }
      switch (next()) {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case 'u': {
          uint32_t point = hex_word();
          if (point >= 0xd800 && point <= 0xdbff) {
            literal("\\u");
            const uint32_t low = hex_word();
            if (low < 0xdc00 || low > 0xdfff) invalid("invalid JSON surrogate pair");
            point = 0x10000 + ((point - 0xd800) << 10) + low - 0xdc00;
          } else if (point >= 0xdc00 && point <= 0xdfff) {
            invalid("unpaired JSON surrogate");
          }
          append_utf8(result, point);
          break;
        }
        default: invalid("invalid JSON escape");
      }
    }
    return result;
  }

  void digits() {
    if (!digit(peek())) invalid("invalid JSON number");
    while (digit(peek())) ++at_;
  }
  void number() {
    take('-');
    if (!take('0')) digits();
    if (take('.')) digits();
    if (take('e') || take('E')) {
      if (!take('+')) take('-');
      digits();
    }
  }

  template <typename Field>
  void object(Field field) {
    expect('{');
    if (accept('}')) return;
    std::set<std::string> keys;
    do {
      auto key = string();
      if (!keys.insert(key).second) invalid("duplicate JSON key");
      expect(':');
      field(key);
    } while (accept(','));
    expect('}');
  }

  void value(unsigned depth) {
    if (depth >= 64) invalid("JSON nesting is too deep");
    skip_space();
    switch (peek()) {
      case '{': object([&](std::string_view) { value(depth + 1); }); break;
      case '[':
        ++at_;
        if (!accept(']')) {
          do { value(depth + 1); } while (accept(','));
          expect(']');
        }
        break;
      case '"': string(); break;
      case 't': literal("true"); break;
      case 'f': literal("false"); break;
      case 'n': literal("null"); break;
      default: number(); break;
    }
  }

  std::string_view line_;
  size_t at_ = 0;
};

inline void quote(std::ostream& out, std::string_view text) {
  constexpr std::string_view hex = "0123456789abcdef";
  out.put('"');
  for (unsigned char ch : text) {
    if (ch == '"' || ch == '\\') {
      out.put('\\');
      out.put(static_cast<char>(ch));
    } else if (ch < 0x20) {
      out << "\\u00" << hex[ch >> 4] << hex[ch & 0xf];
    } else {
      out.put(static_cast<char>(ch));
    }
  }
  out.put('"');
}

}  // namespace mini::wire
