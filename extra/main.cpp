#include "db.hpp"
#include "wire.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <iostream>
#include <limits>
#include <system_error>
#include <type_traits>

#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr std::string_view whitespace = " \r\n\t\f";
enum class Input { json, line, sql };

volatile std::sig_atomic_t stopping = 0;
volatile std::sig_atomic_t eof_input = -1;

void stop(int) {
  stopping = 1;
  const int saved_errno = errno;
  // A buffered stream can retry an interrupted read or write. The read end of
  // an empty pipe gives EOF on stdin and rejects writes to stdout. Keep both
  // descriptors occupied so opening a log cannot accidentally reuse them.
  while (::dup2(eof_input, STDIN_FILENO) < 0 && errno == EINTR) {}
  while (::dup2(eof_input, STDOUT_FILENO) < 0 && errno == EINTR) {}
  errno = saved_errno;
}

void install_signals() {
  int input[2];
  if (::pipe2(input, O_CLOEXEC) < 0) {
    throw std::system_error(errno, std::generic_category(), "open shutdown pipe");
  }
  ::close(input[1]);
  eof_input = input[0];
  struct sigaction action {};
  action.sa_handler = stop;
  ::sigemptyset(&action.sa_mask);
  ::sigaddset(&action.sa_mask, SIGTERM);
  ::sigaddset(&action.sa_mask, SIGINT);
  if (::sigaction(SIGTERM, &action, nullptr) < 0 || ::sigaction(SIGINT, &action, nullptr) < 0) {
    throw std::system_error(errno, std::generic_category(), "install shutdown signal");
  }
  action.sa_handler = SIG_IGN;
  if (::sigaction(SIGPIPE, &action, nullptr) < 0) {
    throw std::system_error(errno, std::generic_category(), "ignore broken stdout pipe");
  }
}

std::string_view trim(std::string_view line) {
  const auto start = line.find_first_not_of(whitespace);
  if (start == line.npos) return {};
  return line.substr(start, line.find_last_not_of(whitespace) - start + 1);
}

bool read_sql(std::string& statement) {
  statement.clear();
  char delimiter = 0;
  for (char ch; !stopping && std::cin.get(ch);) {
    if (delimiter) {
      statement.push_back(ch);
      if (ch == delimiter) delimiter = 0;
      else if (delimiter == '\'' && ch == '\\' && std::cin.get(ch)) statement.push_back(ch);
      continue;
    }
    if (ch == ';') return true;
    bool line_comment = ch == '#';
    if (ch == '-' && std::cin.peek() == '-') {
      std::cin.get();
      const int next = std::cin.peek();
      line_comment = next == std::char_traits<char>::eof() ||
                     whitespace.find(static_cast<char>(next)) != whitespace.npos;
      if (!line_comment) {
        statement += "--";
        continue;
      }
    }
    if (line_comment) {
      std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
      statement.push_back(' ');
      continue;
    }
    if (ch == '/' && std::cin.peek() == '*') {
      std::cin.get();
      bool closed = false;
      while (!stopping && std::cin.get(ch)) {
        if (ch == '*' && std::cin.peek() == '/') {
          std::cin.get();
          closed = true;
          break;
        }
      }
      statement += closed ? " " : " /*";
      continue;
    }
    if (ch == '\'' || ch == 0x60) delimiter = ch;
    else if (ch == '[') delimiter = ']';
    statement.push_back(ch);
  }
  return !statement.empty();
}

bool command_is(std::string_view word, std::string_view lower) {
  return std::ranges::equal(word, lower, [](char ch, char expected) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return ch == expected;
  });
}

void escape(std::ostream& out, std::string_view value) {
  for (char ch : value) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '\t': out << "\\t"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\0': out << "\\0"; break;
      default: out.put(ch); break;
    }
  }
}

template <typename Number>
void number(std::ostream& out, Number value) {
  char buffer[48];
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (error != std::errc{}) throw std::runtime_error("number formatting failed");
  out.write(buffer, end - buffer);
}

void write_cell(std::ostream& out, const mini::Cell& cell, bool json) {
  std::visit([&](auto value) {
    using Value = decltype(value);
    if constexpr (std::is_same_v<Value, std::string_view>) {
      if (json) mini::wire::quote(out, value);
      else escape(out, value);
    } else if constexpr (std::is_same_v<Value, mini::VectorView>) {
      out.put('[');
      for (size_t at = 0; at < value.bytes.size(); at += sizeof(float)) {
        if (at) out.put(',');
        number(out, mini::load<float>(value.bytes.substr(at, sizeof(float))));
      }
      out.put(']');
    } else {
      number(out, value);
    }
  }, cell);
}

class TextProtocol : public mini::ResultSink {
 public:
  void ok(uint64_t affected) override {
    std::cout << "OK " << affected << '\n';
  }

  void begin_rows(std::span<const std::string> columns, size_t count) override {
    std::cout << "RESULT " << columns.size() << ' ' << count << '\n';
    for (size_t i = 0; i < columns.size(); ++i) {
      if (i) std::cout.put('\t');
      escape(std::cout, columns[i]);
    }
    std::cout.put('\n');
  }

  void row(std::span<const mini::Cell> cells) override {
    for (size_t i = 0; i < cells.size(); ++i) {
      if (i) std::cout.put('\t');
      write_cell(std::cout, cells[i], false);
    }
    std::cout.put('\n');
  }

  void end_rows() override { std::cout << "END\n"; }
};

class JsonProtocol : public mini::ResultSink {
 public:
  explicit JsonProtocol(std::string_view id) : id_(id) {}

  void ok(uint64_t affected) override {
    success_prefix();
    std::cout << ",\"affected_rows\":";
    number(std::cout, affected);
    std::cout << "}\n";
  }
  void shutdown() {
    success_prefix();
    std::cout << "}\n";
  }
  void error(const mini::SqlError& error) {
    std::cout << "{\"id\":" << id_ << ",\"ok\":false,\"error\":{\"code\":";
    mini::wire::quote(std::cout, error.code);
    std::cout << ",\"message\":";
    mini::wire::quote(std::cout, error.what());
    std::cout << "}}\n";
  }
  void begin_rows(std::span<const std::string> columns, size_t) override {
    success_prefix();
    std::cout << ",\"columns\":[";
    for (size_t i = 0; i < columns.size(); ++i) {
      if (i) std::cout.put(',');
      mini::wire::quote(std::cout, columns[i]);
    }
    std::cout << "],\"rows\":[";
  }
  void row(std::span<const mini::Cell> cells) override {
    if (!first_row_) std::cout.put(',');
    first_row_ = false;
    std::cout.put('[');
    for (size_t i = 0; i < cells.size(); ++i) {
      if (i) std::cout.put(',');
      write_cell(std::cout, cells[i], true);
    }
    std::cout.put(']');
  }
  void end_rows() override { std::cout << "]}\n"; }

 private:
  void success_prefix() const { std::cout << "{\"id\":" << id_ << ",\"ok\":true"; }

  std::string_view id_;
  bool first_row_ = true;
};

mini::SearchMode mode_value(std::string_view mode) {
  if (command_is(mode, "exact")) return mini::SearchMode::exact;
  if (command_is(mode, "ann")) return mini::SearchMode::ann;
  throw mini::SqlError("MODE", "expected exact or ann");
}

void serve_json(mini::Database& db) {
  std::cout << "{\"type\":\"ready\"}\n" << std::flush;
  std::string line;
  while (!stopping && std::getline(std::cin, line)) {
    if (stopping) break;
    std::string_view id = "null";
    try {
      const auto request = mini::wire::RequestReader(line).read();
      id = request.id;
      JsonProtocol protocol(id);
      if (request.op == "shutdown") {
        db.close();
        protocol.shutdown();
        std::cout.flush();
        return;
      }
      if (request.op != "execute") throw mini::SqlError("unsupported_op", "expected execute or shutdown");
      if (!request.sql) mini::wire::invalid("execute needs a sql string");
      db.execute(mini::parse(mini::text_view(*request.sql)), mode_value(request.mode), protocol);
    } catch (const mini::SqlError& error) {
      JsonProtocol(id).error(error);
    }
    std::cout.flush();
  }
}

void serve_text(mini::Database& db, mini::SearchMode mode, Input input) {
  TextProtocol protocol;
  std::cout << "READY\n" << std::flush;
  std::string statement;
  bool first = true;
  while (!stopping && (input == Input::line ? bool(std::getline(std::cin, statement))
                                            : read_sql(statement))) {
    if (stopping) break;
    auto request = trim(statement);
    if (first && request.starts_with("\xef\xbb\xbf")) request = trim(request.substr(3));
    first = false;
    if (request.empty() || request.starts_with("--") || request.starts_with('#')) continue;
    const auto control = request.ends_with(';') ? trim(request.substr(0, request.size() - 1))
                                                : request;
    if (command_is(control, "quit")) {
      db.close();
      std::cout << "BYE\n" << std::flush;
      return;
    }
    try {
      const auto boundary = control.find_first_of(whitespace);
      if (command_is(control.substr(0, boundary), "mode")) {
        mode = mode_value(boundary == control.npos ? std::string_view{}
                                                    : trim(control.substr(boundary)));
        protocol.ok(0);
      } else {
        db.execute(mini::parse(request), mode, protocol);
      }
    } catch (const mini::SqlError& error) {
      std::cout << "ERROR " << error.code << ' ' << error.what() << '\n';
    }
    std::cout.flush();
    if (!std::cout) break;
  }
}

void run(const std::filesystem::path& directory, mini::SearchMode mode, Input input) {
  mini::Database db(directory);
  std::cout.exceptions(std::ios::badbit);
  try {
    if (input == Input::json) serve_json(db);
    else serve_text(db, mode, input);
  } catch (const std::ios_base::failure&) {
    if (!std::cout.bad()) throw;
  }
  db.close();
}

}  // namespace

int main(int argc, char** argv) {
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);
  std::filesystem::path directory = "/data";
  mini::SearchMode mode = mini::SearchMode::exact;
  Input input = Input::json;
  try {
    install_signals();
    for (int i = 1; i < argc; ++i) {
      const std::string_view option = argv[i];
      const auto separator = option.find('=');
      const auto name = option.substr(0, separator);
      const std::string_view value = separator != option.npos ? option.substr(separator + 1)
                                   : i + 1 < argc ? argv[++i] : "";
      if (name == "--data-dir" && !value.empty()) directory = value;
      else if (name == "--mode" && !value.empty()) mode = mode_value(value);
      else if (name == "--input" && value == "json") input = Input::json;
      else if (name == "--input" && value == "line") input = Input::line;
      else if (name == "--input" && value == "sql") input = Input::sql;
      else {
        std::cerr << "Usage: minivectordb [--data-dir DIRECTORY] [--mode exact|ann] "
                     "[--input json|line|sql]\n";
        return 2;
      }
    }
    run(directory, mode, input);
  } catch (const std::exception& error) {
    std::cerr << "minivectordb: " << error.what() << '\n';
    return 1;
  }
}
