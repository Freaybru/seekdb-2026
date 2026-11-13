#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mini {

static_assert(std::endian::native == std::endian::little);

template <typename T>
T load(std::string_view bytes) {
  T value;
  std::memcpy(&value, bytes.data(), sizeof(value));
  return value;
}

template <typename T>
void store(std::string& bytes, T value) {
  bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

class Reader {
 public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {}

  std::string_view take(size_t length) {
    if (length > bytes_.size()) throw std::runtime_error("truncated record");
    const auto result = bytes_.substr(0, length);
    bytes_.remove_prefix(length);
    return result;
  }

  template <typename T>
  T read() { return load<T>(take(sizeof(T))); }

  std::string_view text() { return take(read<uint32_t>()); }
  bool empty() const { return bytes_.empty(); }
  size_t remaining() const { return bytes_.size(); }

 private:
  std::string_view bytes_;
};

void store_text(std::string& bytes, std::string_view value);

enum class Op : uint32_t { create = 1, index, put, erase, commit, packed_put };
struct Frame {
  static constexpr uint32_t header_size = 28;
  uint64_t offset;
  uint32_t length;
  Op op;
  uint32_t table;
  int64_t key;
  uint64_t previous;
  uint64_t payload_offset() const;
  uint64_t end() const;
};

class Log {
 public:
  explicit Log(const std::filesystem::path& directory);
  ~Log();
  Log(const Log&) = delete;
  Log& operator=(const Log&) = delete;

  static constexpr uint64_t start = 8;
  uint64_t end() const { return disk_end_ + pending_.size(); }
  bool supports_packed_rows() const { return packed_rows_; }
  Frame append(Op op, uint32_t table = 0, int64_t key = 0,
               uint64_t previous = 0, std::string_view payload = {});
  void commit();
  void discard_after(uint64_t offset);
  std::optional<Frame> frame_at(uint64_t offset, uint64_t limit);
  Frame frame_before(uint64_t end);
  std::string_view read(uint64_t offset, uint32_t length);
  int compare_bytes(uint64_t left, uint32_t left_size, uint64_t right, uint32_t right_size);
  void prepare_reads();
  void release_reads() { unmap(); }

 private:
  void flush();
  void unmap();
  std::optional<std::string_view> cached(uint64_t offset, uint32_t length) const;
  void read_disk(uint64_t offset, char* destination, size_t length);
  void write_disk(uint64_t offset, std::string_view data);

  int fd_ = -1;
  bool packed_rows_ = false;
  uint64_t disk_end_ = 0;
  std::string pending_;
  uint64_t block_start_ = 0;
  std::string block_;
  const char* mapping_ = nullptr;
  size_t mapped_size_ = 0;
};

}  // namespace mini
