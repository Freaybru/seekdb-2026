#include "log.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <system_error>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mini {
namespace {

constexpr std::string_view magic = "MINIVDB1";
static_assert(magic.size() == Log::start);
constexpr uint32_t header_size = Frame::header_size;
constexpr uint32_t frame_overhead = header_size + sizeof(uint32_t);
constexpr size_t block_size = 256 * 1024;
constexpr size_t map_limit = 16 * 1024 * 1024;

[[noreturn]] void io_error(const char* operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}

void sync_file(int fd) {
  while (::fdatasync(fd) < 0) {
    if (errno != EINTR) io_error("sync log");
  }
}

}  // namespace

void store_text(std::string& bytes, std::string_view value) {
  if (value.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::length_error("encoded string is too large");
  }
  store(bytes, static_cast<uint32_t>(value.size()));
  if (!value.empty()) bytes.append(value);
}

uint64_t Frame::payload_offset() const { return offset + header_size; }
uint64_t Frame::end() const { return offset + frame_overhead + length; }

Log::Log(const std::filesystem::path& directory) {
  std::filesystem::create_directories(directory);
  const auto path = directory / "minivectordb.log";
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd_ < 0) io_error("open log");
  try {
    if (::flock(fd_, LOCK_EX | LOCK_NB) < 0) io_error("lock log");
    struct stat info {};
    if (::fstat(fd_, &info) < 0) io_error("stat log");
    if (!S_ISREG(info.st_mode)) throw std::runtime_error("log must be a regular file");
    disk_end_ = static_cast<uint64_t>(info.st_size);
    if (disk_end_ < start) {
      // No frame can exist if initialization was interrupted here.
      write_disk(0, magic);
      disk_end_ = start;
      sync_file(fd_);
      const int dir = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (dir < 0) io_error("open data directory");
      int result;
      do {
        result = ::fsync(dir);
      } while (result < 0 && errno == EINTR);
      const int error = errno;
      ::close(dir);
      if (result < 0) throw std::system_error(error, std::generic_category(), "sync data directory");
    } else if (read(0, start) != magic) {
      throw std::runtime_error("unknown log format");
    }
  } catch (...) {
    ::close(fd_);
    throw;
  }
}

Log::~Log() {
  unmap();
  ::close(fd_);
}

void Log::read_disk(uint64_t offset, char* destination, size_t length) {
  while (length) {
    const ssize_t count = ::pread(fd_, destination, length, static_cast<off_t>(offset));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) io_error("read log");
    if (!count) throw std::runtime_error("unexpected end of log");
    offset += count;
    destination += count;
    length -= count;
  }
}

void Log::write_disk(uint64_t offset, std::string_view data) {
  while (!data.empty()) {
    const ssize_t count = ::pwrite(fd_, data.data(), data.size(), static_cast<off_t>(offset));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) io_error("write log");
    if (!count) throw std::runtime_error("zero-length log write");
    offset += count;
    data.remove_prefix(count);
  }
}

void Log::flush() {
  write_disk(disk_end_, pending_);
  disk_end_ += pending_.size();
  pending_.clear();
}

void Log::prepare_reads() {
  if (mapping_ || disk_end_ > map_limit || !pending_.empty()) return;
  void* mapped = ::mmap(nullptr, disk_end_, PROT_READ, MAP_SHARED, fd_, 0);
  if (mapped != MAP_FAILED) {
    mapping_ = static_cast<const char*>(mapped);
    mapped_size_ = disk_end_;
  }
}

void Log::unmap() {
  if (!mapping_) return;
  ::munmap(const_cast<char*>(mapping_), mapped_size_);
  mapping_ = nullptr;
  mapped_size_ = 0;
}

std::optional<std::string_view> Log::cached(uint64_t offset, uint32_t length) const {
  if (offset >= disk_end_) {
    return std::string_view(pending_).substr(offset - disk_end_, length);
  }
  if (mapping_ && offset + length <= mapped_size_) {
    return std::string_view(mapping_ + offset, length);
  }
  if (offset >= block_start_ && offset + length <= block_start_ + block_.size()) {
    return std::string_view(block_.data() + (offset - block_start_), length);
  }
  return std::nullopt;
}

std::string_view Log::read(uint64_t offset, uint32_t length) {
  if (offset > end() || length > end() - offset) throw std::runtime_error("log read out of bounds");
  if (offset < disk_end_ && length > disk_end_ - offset) flush();
  if (const auto existing = cached(offset, length)) return *existing;
  const uint64_t aligned = offset & ~uint64_t{4095};
  block_start_ = aligned;
  const uint64_t needed = offset - aligned + length;
  block_.resize(std::min<uint64_t>(disk_end_ - aligned, std::max<uint64_t>(block_size, needed)));
  read_disk(aligned, block_.data(), block_.size());
  return {block_.data() + (offset - block_start_), length};
}

int Log::compare_bytes(uint64_t left, uint32_t left_size, uint64_t right, uint32_t right_size) {
  if (left > end() || left_size > end() - left || right > end() || right_size > end() - right) {
    throw std::runtime_error("log comparison out of bounds");
  }
  const auto left_cached = cached(left, left_size);
  const auto right_cached = cached(right, right_size);
  if (left_cached && right_cached) return left_cached->compare(*right_cached);

  // Each range is inside one row payload. Read misses into separate bounded
  // buffers so comparing two values leaves the sequential read cache intact.
  constexpr uint32_t chunk_size = 16 * 1024;
  std::array<char, chunk_size> left_buffer;
  std::array<char, chunk_size> right_buffer;
  const auto piece = [&](uint64_t offset, uint32_t size, char* buffer) -> std::string_view {
    if (const auto existing = cached(offset, size)) return *existing;
    read_disk(offset, buffer, size);
    return {buffer, size};
  };
  const uint32_t common_size = std::min(left_size, right_size);
  for (uint32_t at = 0; at < common_size;) {
    const uint32_t size = std::min(chunk_size, common_size - at);
    const auto a = piece(left + at, size, left_buffer.data());
    const auto b = piece(right + at, size, right_buffer.data());
    const int order = a.compare(b);
    if (order != 0) return order;
    at += size;
  }
  return (left_size > right_size) - (left_size < right_size);
}

Frame Log::append(Op op, uint32_t table, int64_t key, uint64_t previous, std::string_view payload) {
  if (payload.size() > std::numeric_limits<uint32_t>::max() - frame_overhead) {
    throw std::length_error("row is too large");
  }
  const Frame frame{end(), static_cast<uint32_t>(payload.size()), op, table, key, previous};
  store(pending_, frame.length);
  store(pending_, static_cast<uint32_t>(op));
  store(pending_, table);
  store(pending_, key);
  store(pending_, previous);
  if (!payload.empty()) pending_.append(payload);
  store(pending_, frame.length + frame_overhead);
  if (pending_.size() >= block_size) flush();
  return frame;
}

void Log::commit() {
  append(Op::commit);
  flush();
  sync_file(fd_);
}

void Log::discard_after(uint64_t offset) {
  if (offset > end() || offset < start) throw std::runtime_error("invalid rollback point");
  if (offset >= disk_end_) {
    pending_.resize(offset - disk_end_);
    return;
  }
  unmap();
  while (::ftruncate(fd_, static_cast<off_t>(offset)) < 0) {
    if (errno != EINTR) io_error("truncate log");
  }
  pending_.clear();
  block_.clear();
  disk_end_ = offset;
}

std::optional<Frame> Log::frame_at(uint64_t offset, uint64_t limit) {
  if (offset > limit || limit - offset < frame_overhead) return std::nullopt;
  Reader input(read(offset, header_size));
  const uint32_t length = input.read<uint32_t>();
  const auto op = static_cast<Op>(input.read<uint32_t>());
  const Frame frame{offset, length, op, input.read<uint32_t>(), input.read<int64_t>(),
                    input.read<uint64_t>()};
  if (length > limit - offset - frame_overhead ||
      length > std::numeric_limits<uint32_t>::max() - frame_overhead ||
      op < Op::create || op > Op::commit ||
      (op == Op::commit && (length || frame.table || frame.key || frame.previous)) ||
      load<uint32_t>(read(frame.end() - sizeof(uint32_t), sizeof(uint32_t))) !=
          length + frame_overhead) {
    return std::nullopt;
  }
  return frame;
}

Frame Log::frame_before(uint64_t cursor) {
  if (cursor < start + frame_overhead) throw std::runtime_error("invalid rollback frame");
  const uint32_t size = load<uint32_t>(read(cursor - sizeof(uint32_t), sizeof(uint32_t)));
  if (size < frame_overhead || size > cursor - start) throw std::runtime_error("invalid frame size");
  const auto frame = frame_at(cursor - size, cursor);
  if (!frame || frame->end() != cursor) throw std::runtime_error("invalid rollback frame");
  return *frame;
}

}  // namespace mini
