#include "db.hpp"

#include <charconv>
#include <csignal>
#include <iostream>
#include <type_traits>

namespace {

volatile std::sig_atomic_t stopping = 0;

void stop(int) { stopping = 1; }

void install_signals() {
  struct sigaction action {};
  action.sa_handler = stop;
  ::sigemptyset(&action.sa_mask);
  ::sigaction(SIGTERM, &action, nullptr);
  ::sigaction(SIGINT, &action, nullptr);
}

std::string_view trim(std::string_view line) {
  constexpr std::string_view spaces = " \r\t\f";
  const auto start = line.find_first_not_of(spaces);
  if (start == line.npos) return {};
  return line.substr(start, line.find_last_not_of(spaces) - start + 1);
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
      std::visit([](auto value) {
        using Value = decltype(value);
        if constexpr (std::is_same_v<Value, std::string_view>) {
          escape(std::cout, value);
        } else if constexpr (std::is_same_v<Value, mini::VectorView>) {
          std::cout.put('[');
          for (size_t at = 0; at < value.bytes.size(); at += sizeof(float)) {
            if (at) std::cout.put(',');
            number(std::cout, mini::load<float>(value.bytes.substr(at, sizeof(float))));
          }
          std::cout.put(']');
        } else {
          number(std::cout, value);
        }
      }, cells[i]);
    }
    std::cout.put('\n');
  }

  void end_rows() override { std::cout << "END\n"; }
};

mini::SearchMode mode_value(std::string_view mode) {
  if (mode == "exact") return mini::SearchMode::exact;
  if (mode == "ann") return mini::SearchMode::ann;
  throw mini::SqlError("MODE", "expected exact or ann");
}

void run(const std::filesystem::path& directory, mini::SearchMode mode) {
  mini::Database db(directory);
  TextProtocol protocol;
  std::cout << "READY\n" << std::flush;
  std::string line;
  while (!stopping && std::getline(std::cin, line)) {
    if (stopping) break;
    const auto request = trim(line);
    if (request.empty() || request.starts_with("--") || request.starts_with('#')) continue;
    if (request == "QUIT" || request == "QUIT;") {
      db.close();
      std::cout << "BYE\n" << std::flush;
      return;
    }
    try {
      if (request.starts_with("MODE ")) {
        mode = mode_value(trim(request.substr(5)));
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
  db.close();
}

}  // namespace

int main(int argc, char** argv) {
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);
  install_signals();
  std::filesystem::path directory = "/data";
  mini::SearchMode mode = mini::SearchMode::exact;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string_view option = argv[i];
      if (option == "--data-dir" && i + 1 < argc) directory = argv[++i];
      else if (option == "--mode" && i + 1 < argc) mode = mode_value(argv[++i]);
      else {
        std::cerr << "Usage: minivectordb [--data-dir DIRECTORY] [--mode exact|ann]\n";
        return 2;
      }
    }
    run(directory, mode);
  } catch (const std::exception& error) {
    std::cerr << "minivectordb: " << error.what() << '\n';
    return 1;
  }
}
