#pragma once

#include "log.hpp"
#include "sql.hpp"

#include <map>
#include <memory>
#include <span>

namespace mini {

struct VectorView { std::string_view bytes; };
using Cell = std::variant<int64_t, std::string_view, VectorView, double>;
enum class SearchMode { exact, ann };

class ResultSink {
 public:
  virtual ~ResultSink() = default;
  virtual void ok(uint64_t affected) = 0;
  virtual void begin_rows(std::span<const std::string> columns, size_t count) = 0;
  virtual void row(std::span<const Cell> cells) = 0;
  virtual void end_rows() = 0;
};

struct Table;

class Database {
 public:
  explicit Database(const std::filesystem::path& directory);
  ~Database();
  void execute(const Statement& statement, SearchMode mode, ResultSink& sink);
  void close();

 private:
  Table& table(std::string_view name);
  void recover();
  void apply(const Frame& frame);
  void undo(uint64_t target, uint64_t cursor);
  void invalidate_index(Table& table);
  void build_index(Table& table);

  uint64_t write(const CreateTable& statement);
  uint64_t write(const CreateIndex& statement);
  uint64_t write(const Insert& statement);
  uint64_t write(const Update& statement);
  uint64_t write(const Delete& statement);
  void transact(Transaction operation);
  void select(const Select& statement, SearchMode mode, ResultSink& sink);

  Log log_;
  std::optional<uint64_t> transaction_;
  std::vector<std::unique_ptr<Table>> tables_;
  std::map<std::string, uint32_t, std::less<>> names_;
  size_t index_bytes_ = 0;
};

}  // namespace mini
