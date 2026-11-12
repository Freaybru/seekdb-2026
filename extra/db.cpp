#include "db.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <new>
#include <numeric>
#include <type_traits>
#include <utility>

#include <sys/resource.h>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace mini {
namespace {

constexpr size_t missing = std::numeric_limits<size_t>::max();
constexpr size_t index_budget = 4 * 1024 * 1024;
constexpr size_t bound_budget = 2 * 1024 * 1024;
constexpr uint32_t wide_row = 8 * 1024;

struct RowRef {
  uint64_t offset;
  uint32_t length;
};
// One tree node covers a sorted page of row locations. Page boundaries stay
// valid when the first key is removed, so only splits and merges edit the tree.
class Rows {
 public:
  using value_type = std::pair<int64_t, RowRef>;

 private:
  static constexpr size_t page_size = 128;
  using Page = std::vector<value_type>;
  using Pages = std::map<int64_t, Page>;

 public:
  class const_iterator {
   public:
    using iterator_category = std::bidirectional_iterator_tag;
    using value_type = Rows::value_type;
    using difference_type = std::ptrdiff_t;
    using pointer = const value_type*;
    using reference = const value_type&;

    const_iterator() = default;
    reference operator*() const { return page_->second[position_]; }
    pointer operator->() const { return &**this; }

    const_iterator& operator++() {
      if (++position_ == page_->second.size()) {
        ++page_;
        position_ = 0;
      }
      return *this;
    }
    const_iterator operator++(int) {
      auto copy = *this;
      ++*this;
      return copy;
    }
    const_iterator& operator--() {
      if (position_ == 0) position_ = (--page_)->second.size();
      --position_;
      return *this;
    }
    const_iterator operator--(int) {
      auto copy = *this;
      --*this;
      return copy;
    }
    bool operator==(const const_iterator&) const = default;

   private:
    friend class Rows;
    const_iterator(Pages::const_iterator page, size_t position)
        : page_(page), position_(position) {}
    Pages::const_iterator page_;
    size_t position_ = 0;
  };

  size_t size() const { return size_; }
  const_iterator begin() const { return size_ == 0 ? end() : const_iterator(pages_.begin(), 0); }
  const_iterator end() const { return {pages_.end(), 0}; }
  const_iterator lower_bound(int64_t key) const {
    if (size_ == 0) return end();
    auto page = std::prev(pages_.upper_bound(key));
    const size_t at = position(page->second, key);
    if (at == page->second.size()) return {std::next(page), 0};
    return {page, at};
  }
  const_iterator upper_bound(int64_t key) const {
    auto entry = lower_bound(key);
    if (entry != end() && entry->first == key) ++entry;
    return entry;
  }
  bool contains(int64_t key) const {
    const auto entry = lower_bound(key);
    return entry != end() && entry->first == key;
  }

  void insert_or_assign(int64_t key, RowRef row) {
    if (pages_.empty()) add_page(std::numeric_limits<int64_t>::min());
    auto page = std::prev(pages_.upper_bound(key));
    size_t at = position(page->second, key);
    if (at < page->second.size() && page->second[at].first == key) {
      page->second[at].second = row;
      return;
    }
    if (page->second.size() == page_size) {
      if (at == page_size && std::next(page) == pages_.end()) {
        page = add_page(key);
        at = 0;
      } else {
        const size_t middle = page_size / 2;
        auto& entries = page->second;
        auto right = add_page(entries[middle].first);
        right->second.insert(right->second.end(), entries.begin() + middle, entries.end());
        entries.resize(middle);
        if (at > middle) {
          page = right;
          at -= middle;
        }
      }
    }
    page->second.insert(page->second.begin() + at, {key, row});
    ++size_;
  }

  void erase(int64_t key) {
    if (size_ == 0) return;
    auto page = std::prev(pages_.upper_bound(key));
    auto& entries = page->second;
    const size_t at = position(entries, key);
    if (at == entries.size() || entries[at].first != key) return;
    entries.erase(entries.begin() + at);
    --size_;
    if (entries.size() >= page_size / 4) return;
    const auto right = std::next(page);
    if (right != pages_.end() && entries.size() + right->second.size() <= page_size) {
      merge(page, right);
    } else if (page != pages_.begin()) {
      const auto left = std::prev(page);
      if (left->second.size() + entries.size() <= page_size) merge(left, page);
    }
  }

 private:
  static size_t position(const Page& page, int64_t key) {
    return std::ranges::lower_bound(page, key, {}, &value_type::first) - page.begin();
  }
  Pages::iterator add_page(int64_t boundary) {
    Page entries;
    entries.reserve(page_size);
    return pages_.emplace(boundary, std::move(entries)).first;
  }
  void merge(Pages::iterator left, Pages::iterator right) {
    left->second.insert(left->second.end(), right->second.begin(), right->second.end());
    pages_.erase(right);
  }

  Pages pages_;
  size_t size_ = 0;
};

struct ProjectionEntry {
  float pivot;
  int64_t key;
  RowRef row;
};

struct BoundRow {
  int64_t key;
  RowRef row;
  int32_t biased_norm;
};

struct BoundGroups {
  struct Bucket { uint64_t tag; size_t offset; size_t count; };
  size_t column = missing;
  size_t used = 0;
  std::array<Bucket, 32> buckets{};
  std::vector<uint16_t> slots;

  size_t bytes() const { return sizeof(*this) + slots.capacity() * sizeof(uint16_t); }
  std::span<const uint16_t> lookup(uint64_t tag) const {
    for (const auto& bucket : std::span(buckets).first(used)) {
      if (bucket.tag == tag) return std::span(slots).subspan(bucket.offset, bucket.count);
    }
    return {};
  }
};

struct BoundCache {
  size_t stride = 0;
  double inverse_step = 0;
  std::vector<BoundRow> rows;
  std::vector<int8_t> codes;
  bool groups_attempted = false;
  std::unique_ptr<BoundGroups> groups;

  size_t bytes() const {
    return rows.capacity() * sizeof(BoundRow) + codes.capacity() + (groups ? groups->bytes() : 0);
  }
};

}  // namespace

struct Table {
  uint32_t id;
  std::string name;
  std::vector<Column> columns;
  std::map<std::string, size_t, std::less<>> ordinals;
  std::vector<size_t> layout;
  size_t primary = missing;
  size_t vector = missing;
  uint32_t vector_bytes = 0;
  Rows rows;
  std::string index_name;
  bool index_current = false;
  std::vector<ProjectionEntry> index;
  bool bounds_attempted = false;
  std::unique_ptr<BoundCache> bounds;

  Table(uint32_t id, std::string name, std::vector<Column> columns)
      : id(id), name(std::move(name)), columns(std::move(columns)) {
    for (size_t i = 0; i < this->columns.size(); ++i) {
      const auto& column = this->columns[i];
      if (!ordinals.emplace(column.name, i).second) {
        throw SqlError("SCHEMA", "duplicate column name");
      }
      if (column.primary) {
        if (primary != missing || column.type != Type::bigint) {
          throw SqlError("SCHEMA", "primary key must be a single BIGINT column");
        }
        primary = i;
      }
      if (column.type == Type::vector) {
        if (vector != missing || column.length == 0 ||
            column.length > (std::numeric_limits<uint32_t>::max() - 32) / sizeof(float)) {
          throw SqlError("SCHEMA", "invalid vector column");
        }
        vector = i;
        vector_bytes = column.length * sizeof(float);
      }
    }
    if (primary == missing) throw SqlError("SCHEMA", "missing primary key");
    if (vector != missing) layout.push_back(vector);
    for (size_t i = 0; i < this->columns.size(); ++i) {
      if (i != primary && i != vector) layout.push_back(i);
    }
  }

  size_t column(std::string_view name) const {
    const auto it = ordinals.find(name);
    if (it == ordinals.end()) throw SqlError("UNKNOWN_COLUMN", "column does not exist");
    return it->second;
  }
};

namespace {

std::string_view payload(Log& log, RowRef row) {
  // RowRef uses the frame offset so it can also serve as an undo pointer.
  return log.read(row.offset + Frame::header_size, row.length);
}

void encode(std::string& out, const Column& column, const Literal& value) {
  switch (column.type) {
    case Type::bigint:
      store(out, bigint(value));
      break;
    case Type::varchar:
      store_text(out, varchar(value, column.length));
      break;
    case Type::vector: {
      const auto floats = vector_value(value, column.length);
      out.append(reinterpret_cast<const char*>(floats.data()), floats.size() * sizeof(float));
      break;
    }
  }
}

Cell read_cell(Reader& reader, const Column& column) {
  switch (column.type) {
    case Type::bigint: return reader.read<int64_t>();
    case Type::varchar: return reader.text();
    case Type::vector: return VectorView{reader.take(size_t{column.length} * sizeof(float))};
  }
  throw std::runtime_error("unknown column type");
}

void decode(const Table& table, int64_t key, std::string_view bytes, std::span<Cell> cells) {
  Reader reader(bytes);
  cells[table.primary] = key;
  for (size_t column : table.layout) cells[column] = read_cell(reader, table.columns[column]);
}

struct Filter {
  size_t column;
  Comparison comparison;
  Cell value;
};

std::vector<Filter> bind_filters(const Table& table, const std::vector<Predicate>& predicates) {
  std::vector<Filter> filters;
  for (const auto& predicate : predicates) {
    const size_t column = table.column(predicate.column);
    Cell value;
    switch (table.columns[column].type) {
      case Type::bigint:
        value = bigint(predicate.value);
        break;
      case Type::varchar:
        value = varchar(predicate.value, std::numeric_limits<uint32_t>::max());
        break;
      case Type::vector:
        throw SqlError("unsupported_sql", "vector comparison requires L2_DISTANCE");
    }
    filters.push_back({column, predicate.comparison, value});
  }
  return filters;
}

template <typename T>
int compare(const T& a, const T& b) {
  return a < b ? -1 : (b < a ? 1 : 0);
}

bool comparison_holds(Comparison comparison, int order) {
  switch (comparison) {
    case Comparison::eq: return order == 0;
    case Comparison::lt: return order < 0;
    case Comparison::le: return order <= 0;
    case Comparison::gt: return order > 0;
    case Comparison::ge: return order >= 0;
  }
  return false;
}

bool matches(std::span<const Filter> filters, std::span<const Cell> cells) {
  for (const auto& filter : filters) {
    const auto& cell = cells[filter.column];
    const auto* number = std::get_if<int64_t>(&filter.value);
    const int order = number ? compare(std::get<int64_t>(cell), *number)
                             : std::get<std::string_view>(cell).compare(
                                   std::get<std::string_view>(filter.value));
    if (!comparison_holds(filter.comparison, order)) return false;
  }
  return true;
}

// Scalar fields follow the vector in schema order. A VARCHAR length prefix
// lets us skip unrelated text and compare just the bytes shared with a literal.
bool matches_wide_row(Log& log, const Table& table, RowRef row, std::span<const Filter> filters) {
  uint64_t next = row.offset + Frame::header_size;
  const uint64_t end = next + row.length;
  const auto take = [&](uint32_t length) {
    if (length > end - next) throw std::runtime_error("truncated record");
    const auto start = next;
    next += length;
    return start;
  };
  take(table.vector_bytes);
  for (size_t column : table.layout) {
    if (filters.empty()) return true;
    if (column == table.vector) continue;
    const bool text_column = table.columns[column].type == Type::varchar;
    const uint32_t length = text_column
        ? load<uint32_t>(log.read(take(sizeof(uint32_t)), sizeof(uint32_t)))
        : sizeof(int64_t);
    const uint64_t offset = take(length);
    if (column != filters.front().column) continue;
    do {
      const auto& filter = filters.front();
      int order;
      if (text_column) {
        const auto text = std::get<std::string_view>(filter.value);
        const auto common = static_cast<uint32_t>(std::min<size_t>(length, text.size()));
        order = common == 0 ? 0 : log.read(offset, common).compare(text.substr(0, common));
        if (order == 0) order = compare<size_t>(length, text.size());
      } else {
        order = compare(load<int64_t>(log.read(offset, length)), std::get<int64_t>(filter.value));
      }
      if (!comparison_holds(filter.comparison, order)) return false;
      filters = filters.subspan(1);
    } while (!filters.empty() && filters.front().column == column);
  }
  return filters.empty();
}

struct KeyRange {
  int64_t low = std::numeric_limits<int64_t>::min();
  int64_t high = std::numeric_limits<int64_t>::max();

  bool contains(int64_t key) const { return key >= low && key <= high; }

  template <typename RowMap>
  auto iterators(RowMap& rows) const {
    if (low > high) return std::pair(rows.end(), rows.end());
    return std::pair(rows.lower_bound(low), rows.upper_bound(high));
  }
};

KeyRange restrict_keys(const Table& table, std::vector<Filter>& filters) {
  KeyRange range;
  for (const auto& filter : filters) {
    if (filter.column != table.primary) continue;
    const auto value = std::get<int64_t>(filter.value);
    switch (filter.comparison) {
      case Comparison::eq:
        range.low = std::max(range.low, value);
        range.high = std::min(range.high, value);
        break;
      case Comparison::lt:
        if (value == std::numeric_limits<int64_t>::min()) return {1, 0};
        range.high = std::min(range.high, value - 1);
        break;
      case Comparison::le:
        range.high = std::min(range.high, value);
        break;
      case Comparison::gt:
        if (value == std::numeric_limits<int64_t>::max()) return {1, 0};
        range.low = std::max(range.low, value + 1);
        break;
      case Comparison::ge:
        range.low = std::max(range.low, value);
        break;
    }
  }
  std::erase_if(filters, [&](const Filter& filter) { return filter.column == table.primary; });
  return range;
}

double scalar_l2(const char* vector, const float* query, size_t dimension) {
  double sum = 0;
  for (size_t i = 0; i < dimension; ++i) {
    float component;
    std::memcpy(&component, vector + i * sizeof(float), sizeof(float));
    const double delta = double{component} - query[i];
    sum += delta * delta;
  }
  return sum;
}

#if defined(__x86_64__)
__attribute__((target("avx2")))
double avx2_l2(const char* vector, const float* query, size_t dimension) {
  __m256d even = _mm256_setzero_pd();
  __m256d odd = _mm256_setzero_pd();
  size_t i = 0;
  for (; i + 8 <= dimension; i += 8) {
    __m256 data;
    std::memcpy(&data, vector + i * sizeof(float), sizeof(data));
    const __m256 target = _mm256_loadu_ps(query + i);
    const __m256d a = _mm256_sub_pd(_mm256_cvtps_pd(_mm256_castps256_ps128(data)),
                                    _mm256_cvtps_pd(_mm256_castps256_ps128(target)));
    const __m256d b = _mm256_sub_pd(_mm256_cvtps_pd(_mm256_extractf128_ps(data, 1)),
                                    _mm256_cvtps_pd(_mm256_extractf128_ps(target, 1)));
    even = _mm256_add_pd(even, _mm256_mul_pd(a, a));
    odd = _mm256_add_pd(odd, _mm256_mul_pd(b, b));
  }
  alignas(32) double lanes[4];
  _mm256_store_pd(lanes, _mm256_add_pd(even, odd));
  return (lanes[0] + lanes[1] + lanes[2] + lanes[3]) +
         scalar_l2(vector + i * sizeof(float), query + i, dimension - i);
}

__attribute__((target("avx2")))
double avx2_lower_square(const char* vector, const float* query, size_t dimension) {
  __m256 sum = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 8 <= dimension; i += 8) {
    __m256 data;
    std::memcpy(&data, vector + i * sizeof(float), sizeof(data));
    const __m256 delta = _mm256_sub_ps(data, _mm256_loadu_ps(query + i));
    sum = _mm256_add_ps(sum, _mm256_mul_ps(delta, delta));
  }
  alignas(32) float lanes[8];
  _mm256_store_ps(lanes, sum);
  double approximate = scalar_l2(vector + i * sizeof(float), query + i, dimension - i);
  for (float lane : lanes) approximate += lane;
  if (!std::isfinite(approximate)) return 0;
  // Used only for 32..4096 components. Every lane sums nonnegative squares,
  // with at most dimension/8 additions. This deliberately loose allowance
  // covers subtraction, multiplication, accumulation and the double reduction.
  // The absolute allowance also covers gradual underflow (or flushed tiny
  // products). Overflow falls back to the existing double calculation.
  const double allowance = (4.0 * dimension + 32.0) * std::numeric_limits<float>::epsilon();
  return std::max(0.0, approximate * (1.0 - allowance) -
                       dimension * double{std::numeric_limits<float>::min()});
}

uint64_t filter_tag(const Cell& value) {
  if (const auto* number = std::get_if<int64_t>(&value)) return static_cast<uint64_t>(*number);
  return std::hash<std::string_view>{}(std::get<std::string_view>(value));
}

std::unique_ptr<BoundGroups> group_slots(Log& log, const Table& table, size_t column, size_t budget) {
  const auto& rows = table.bounds->rows;
  const size_t count = rows.size();
  if (count > std::numeric_limits<uint16_t>::max() ||
      sizeof(BoundGroups) + count * (sizeof(uint16_t) + sizeof(uint8_t)) > budget) return {};
  auto groups = std::make_unique<BoundGroups>();
  groups->column = column;
  std::vector<uint8_t> labels;
  labels.reserve(count);
  for (const auto& entry : rows) {
    // Avoid reading unrelated wide payloads to build an optional shortcut.
    if (entry.row.length > wide_row) return {};
    Reader reader(payload(log, entry.row).substr(table.vector_bytes));
    for (size_t field : table.layout) {
      if (field == table.vector) continue;
      const auto value = read_cell(reader, table.columns[field]);
      if (field != column) continue;
      const uint64_t tag = filter_tag(value);
      size_t bucket = 0;
      while (bucket < groups->used && groups->buckets[bucket].tag != tag) ++bucket;
      if (bucket == groups->used) {
        if (bucket == groups->buckets.size()) return {};
        groups->buckets[groups->used++].tag = tag;
      }
      ++groups->buckets[bucket].count;
      labels.push_back(static_cast<uint8_t>(bucket));
      break;
    }
  }
  if (labels.size() != count) return {};
  size_t offset = 0;
  for (auto& bucket : std::span(groups->buckets).first(groups->used)) {
    bucket.offset = offset;
    offset += bucket.count;
    bucket.count = 0;
  }
  groups->slots.resize(count);
  for (size_t i = 0; i < labels.size(); ++i) {
    auto& bucket = groups->buckets[labels[i]];
    groups->slots[bucket.offset + bucket.count++] = static_cast<uint16_t>(i);
  }
  return groups;
}

struct BoundQuery {
  std::array<uint8_t, 4096> codes;
  int32_t norm = 0;
};

int quantize(double value) {
  return static_cast<int>(value + std::copysign(0.5, value));
}

bool make_bound_query(const BoundCache& cache, const std::vector<float>& query,
                      BoundQuery& packed) {
  for (size_t i = 0; i < query.size(); ++i) {
    const double scaled = query[i] * (cache.inverse_step * 0.5);
    // Query codes use twice the row step, so unsigned dot-product inputs
    // fit in 0..126 and each AVX2 pair sum fits in a signed 16-bit lane.
    if (std::abs(scaled) > 63.5) return false;
    const int code = std::clamp(quantize(scaled), -63, 63);
    packed.codes[i] = static_cast<uint8_t>(code + 63);
    packed.norm += 4 * code * code;
  }
  std::fill(packed.codes.begin() + query.size(), packed.codes.begin() + cache.stride, 63);
  return true;
}

int32_t bound_cutoff(double worst, double margin, size_t dimension, double inverse_step) {
  // Both quantizers round to nearest, up to the error of the double scale.
  // In row-code units the two per-component errors sum to less than 1.51.
  // The triangle inequality makes this a necessary condition for entry into
  // the heap. Round the squared cutoff up and retain equality for exact ties.
  const double radius = std::sqrt(worst) * (1.0 + margin) * inverse_step +
                        1.51 * std::sqrt(static_cast<double>(dimension));
  const double square = radius * radius;
  const auto maximum = std::numeric_limits<int32_t>::max();
  return square >= maximum ? maximum : static_cast<int32_t>(std::ceil(square));
}

template <size_t FixedStride, bool Grouped, typename Probe>
__attribute__((target("avx2")))
void scan_bounds(const BoundCache& cache, const BoundQuery& query,
                 const KeyRange& range, std::span<const uint16_t> slots, Probe probe) {
  size_t start;
  size_t end;
  if constexpr (Grouped) {
    if (slots.empty()) return;
    const auto key_at = [&](uint16_t slot) { return cache.rows[slot].key; };
    start = std::ranges::lower_bound(slots, range.low, {}, key_at) - slots.begin();
    end = slots.size();
  } else {
    start = std::ranges::lower_bound(cache.rows, range.low, {}, &BoundRow::key) - cache.rows.begin();
    end = cache.rows.size();
  }
  const size_t stride = FixedStride ? FixedStride : cache.stride;
  const auto maximum = std::numeric_limits<int32_t>::max();
  int32_t cutoff = maximum;
  const __m256i ones = _mm256_set1_epi16(1);
  for (; start < end; ++start) {
    const size_t slot = Grouped ? slots[start] : start;
    const auto& row = cache.rows[slot];
    if (row.key > range.high) break;
    if (cutoff != maximum) {
      const auto* code = cache.codes.data() + slot * stride;
      __m256i sum = _mm256_setzero_si256();
      for (size_t at = 0; at < stride; at += 32) {
        __m256i x, y;
        std::memcpy(&x, code + at, sizeof(x));
        std::memcpy(&y, query.codes.data() + at, sizeof(y));
        sum = _mm256_add_epi32(sum, _mm256_madd_epi16(_mm256_maddubs_epi16(y, x), ones));
      }
      alignas(32) int32_t lanes[8];
      _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), sum);
      const int32_t dot = std::accumulate(std::begin(lanes), std::end(lanes), int32_t{0});
      // Recenter the unsigned query via the bias stored in the row norm.
      if (row.biased_norm + query.norm - 4 * dot > cutoff) continue;
    }
    cutoff = probe(row, cutoff);
  }
}
#endif

double squared_distance(std::string_view vector, const std::vector<float>& query) {
#if defined(__x86_64__)
  static const auto distance = __builtin_cpu_supports("avx2") ? avx2_l2 : scalar_l2;
#else
  static constexpr auto distance = scalar_l2;
#endif
  // Short vectors are faster on the scalar path.
  return query.size() < 8 ? scalar_l2(vector.data(), query.data(), query.size())
                          : distance(vector.data(), query.data(), query.size());
}

// A binary32 component is an integer significand times 2^(shift - 149).
// Accumulate squared distances in units of 2^-298 only for close comparisons.
// A finite component difference is less than 2^129, and dimensions are
// below 2^30, so the sum needs at most 586 bits.
class ExactSquare {
 public:
  void add(uint64_t coefficient, unsigned shift, bool subtract = false) {
    const size_t index = shift / 64;
    const unsigned bits = shift % 64;
    const uint64_t low = coefficient << bits;
    const uint64_t high = bits ? coefficient >> (64 - bits) : 0;
    if (subtract) {
      subtract_word(index, low);
      subtract_word(index + 1, high);
    } else {
      add_word(index, low);
      add_word(index + 1, high);
    }
  }

  int compare_to(const ExactSquare& other) const {
    for (size_t i = words_.size(); i != 0;) {
      --i;
      const int order = compare(words_[i], other.words_[i]);
      if (order != 0) return order;
    }
    return 0;
  }

 private:
  void add_word(size_t index, uint64_t value) {
    while (value) {
      const uint64_t sum = words_[index] + value;
      value = sum < words_[index];
      words_[index++] = sum;
    }
  }

  void subtract_word(size_t index, uint64_t value) {
    while (value) {
      const uint64_t previous = words_[index];
      words_[index++] = previous - value;
      value = previous < value;
    }
  }

  std::array<uint64_t, 10> words_{};
};

struct FloatParts {
  uint64_t significand;
  unsigned shift;
  bool negative;
};

FloatParts float_parts(float value) {
  static_assert(sizeof(float) == sizeof(uint32_t) && std::numeric_limits<float>::is_iec559 &&
                std::numeric_limits<float>::digits == 24);
  const auto bits = std::bit_cast<uint32_t>(value);
  const unsigned exponent = (bits >> 23) & 0xff;
  const uint32_t fraction = bits & 0x7fffff;
  const uint32_t significand = exponent ? fraction | 0x800000u : fraction;
  return {significand, exponent ? exponent - 1 : 0, (bits >> 31) != 0};
}

ExactSquare exact_square(std::string_view vector, const std::vector<float>& query) {
  ExactSquare result;
  for (size_t i = 0; i < query.size(); ++i) {
    const auto value = load<float>(vector.substr(i * sizeof(float), sizeof(float)));
    if (value == query[i]) continue;
    const auto a = float_parts(value);
    const auto b = float_parts(query[i]);
    // Add both squares before a possible subtraction so the sum stays nonnegative.
    result.add(a.significand * a.significand, 2 * a.shift);
    result.add(b.significand * b.significand, 2 * b.shift);
    result.add(a.significand * b.significand, a.shift + b.shift + 1, a.negative == b.negative);
  }
  return result;
}

double distance_margin(size_t dimension) {
  // This covers both operands' rounding from subtraction, multiplication and
  // accumulation. Float32 inputs cannot overflow or underflow these double sums.
  return (4.0 * dimension + 16.0) * std::numeric_limits<double>::epsilon();
}

int separated_squares(double left, double right, double margin) {
  if (std::abs(left - right) <= std::max(left, right) * margin) return 0;
  return compare(left, right);
}

int compare_distance(Log& log, RowRef left, double a, RowRef right, double b,
                     const std::vector<float>& query, double margin) {
  if (const int order = separated_squares(a, b, margin); order != 0) return order;
  if ((a == 0 && b == 0) || left.offset == right.offset) return 0;
  const auto length = static_cast<uint32_t>(query.size() * sizeof(float));
  const uint64_t left_vector = left.offset + Frame::header_size;
  const uint64_t right_vector = right.offset + Frame::header_size;
  if (log.compare_bytes(left_vector, length, right_vector, length) == 0) return 0;
  const auto left_square = exact_square(log.read(left_vector, length), query);
  const auto right_square = exact_square(log.read(right_vector, length), query);
  return left_square.compare_to(right_square);
}

}  // namespace

Database::Database(const std::filesystem::path& directory) : log_(directory) { recover(); }
Database::~Database() = default;

Table& Database::table(std::string_view name) {
  const auto it = names_.find(name);
  if (it == names_.end()) throw SqlError("UNKNOWN_TABLE", "table does not exist");
  return *tables_[it->second];
}

void Database::recover() {
  uint64_t committed = Log::start;
  uint64_t cursor = committed;
  std::array<Frame, 128> staged;
  size_t staged_count = 0;
  const uint64_t limit = log_.end();
  while (const auto frame = log_.frame_at(cursor, limit)) {
    cursor = frame->end();
    if (frame->op != Op::commit) {
      if (staged_count < staged.size()) staged[staged_count++] = *frame;
      continue;
    }
    // Keep small transactions in the frame buffer. For a large committed
    // transaction, reread its remaining frames after finding the commit.
    uint64_t replay = committed;
    for (const auto& change : std::span(staged).first(staged_count)) {
      apply(change);
      replay = change.end();
    }
    while (replay < frame->offset) {
      const auto change = log_.frame_at(replay, frame->offset);
      if (!change) throw std::runtime_error("invalid committed frame");
      apply(*change);
      replay = change->end();
    }
    committed = cursor;
    staged_count = 0;
  }
  log_.discard_after(committed);
  for (auto& current : tables_) {
    if (!current->index_name.empty()) build_index(*current);
  }
}

void Database::apply(const Frame& frame) {
  if (frame.op == Op::create) {
    if (frame.table != tables_.size()) throw std::runtime_error("invalid table ID");
    Reader reader(log_.read(frame.payload_offset(), frame.length));
    std::string name(reader.text());
    std::vector<Column> columns;
    const uint32_t count = reader.read<uint32_t>();
    for (uint32_t i = 0; i < count; ++i) {
      Column column{std::string(reader.text()), static_cast<Type>(reader.read<uint8_t>()),
                    reader.read<uint32_t>(), reader.read<uint8_t>() != 0};
      if (column.type > Type::vector) throw std::runtime_error("invalid column type");
      columns.push_back(std::move(column));
    }
    if (!reader.empty()) throw std::runtime_error("unexpected schema bytes");
    auto new_table = std::make_unique<Table>(frame.table, std::move(name), std::move(columns));
    names_.emplace(new_table->name, frame.table);
    tables_.push_back(std::move(new_table));
    return;
  }
  Table& current = *tables_.at(frame.table);
  switch (frame.op) {
    case Op::index:
      current.index_name = log_.read(frame.payload_offset(), frame.length);
      break;
    case Op::put:
      current.rows.insert_or_assign(frame.key, RowRef{frame.offset, frame.length});
      break;
    case Op::erase:
      current.rows.erase(frame.key);
      break;
    default:
      throw std::runtime_error("invalid mutation");
  }
}

void Database::undo(uint64_t target, uint64_t cursor) {
  log_.release_reads();
  while (cursor > target) {
    const Frame frame = log_.frame_before(cursor);
    cursor = frame.offset;
    Table& current = *tables_.at(frame.table);
    invalidate_index(current);
    switch (frame.op) {
      case Op::create:
        names_.erase(current.name);
        tables_.pop_back();
        break;
      case Op::index:
        current.index_name.clear();
        break;
      case Op::put:
      case Op::erase:
        if (frame.previous == 0) {
          current.rows.erase(frame.key);
        } else {
          const auto previous = log_.frame_at(frame.previous, cursor);
          if (!previous || previous->op != Op::put) {
            throw std::runtime_error("invalid undo pointer");
          }
          current.rows.insert_or_assign(frame.key, RowRef{previous->offset, previous->length});
        }
        break;
      default:
        throw std::runtime_error("invalid undo operation");
    }
  }
}

void Database::invalidate_index(Table& current) {
  current.index_current = false;
  index_bytes_ -= current.index.capacity() * sizeof(ProjectionEntry);
  std::vector<ProjectionEntry>().swap(current.index);
  current.bounds_attempted = false;
  if (current.bounds) {
    bound_bytes_ -= current.bounds->bytes();
    current.bounds.reset();
  }
}

void Database::prepare_bounds(Table& current) {
#if defined(__x86_64__)
  if (current.bounds_attempted) return;
  current.bounds_attempted = true;
  const size_t dimension = current.vector_bytes / sizeof(float);
  const size_t stride = (dimension + 31) & ~size_t{31};
  const size_t count = current.rows.size();
  const size_t budget = bound_budget - bound_bytes_;
  if (dimension < 32 || dimension > 4096 || count < 512 ||
      count > budget / (stride + sizeof(BoundRow))) return;
  struct rlimit limit {};
  if (::getrlimit(RLIMIT_AS, &limit) == 0 && limit.rlim_cur < 64 * 1024 * 1024) return;

  log_.prepare_reads();
  const auto vector = [&](RowRef row) {
    return log_.read(row.offset + Frame::header_size, current.vector_bytes);
  };
  double largest = 0;
  for (const auto& entry : current.rows) {
    const auto bytes = vector(entry.second);
    for (size_t i = 0; i < dimension; ++i) {
      const auto value = load<float>(bytes.substr(i * sizeof(float), sizeof(float)));
      if (!std::isfinite(value)) return;
      largest = std::max(largest, std::abs(double{value}));
    }
  }
  if (largest == 0) return;

  std::unique_ptr<BoundCache> cache;
  try {
    cache = std::make_unique<BoundCache>();
    cache->rows.reserve(count);
    cache->codes.resize(count * stride);
  } catch (const std::bad_alloc&) {
    return;
  }
  if (cache->bytes() > budget) return;
  cache->stride = stride;
  cache->inverse_step = 126.0 / largest;
  for (const auto& [key, row] : current.rows) {
    const auto bytes = vector(row);
    int32_t norm = 0;
    auto* out = cache->codes.data() + cache->rows.size() * stride;
    for (size_t i = 0; i < dimension; ++i) {
      const auto value = load<float>(bytes.substr(i * sizeof(float), sizeof(float)));
      const int code = quantize(value * cache->inverse_step);
      out[i] = static_cast<int8_t>(code);
      norm += code * (code + 4 * 63);
    }
    cache->rows.push_back({key, row, norm});
  }
  bound_bytes_ += cache->bytes();
  current.bounds = std::move(cache);
#else
  (void)current;
#endif
}

void Database::build_index(Table& current) {
  if (current.index_current) return;
  current.index_current = true;
  if (current.rows.size() > (index_budget - index_bytes_) / sizeof(ProjectionEntry)) return;
  try {
    current.index.reserve(current.rows.size());
  } catch (const std::bad_alloc&) {
    // Exact search stays available when the optional projection cannot fit.
    return;
  }
  log_.prepare_reads();
  for (const auto& [key, row] : current.rows) {
    const float pivot = load<float>(log_.read(row.offset + Frame::header_size, sizeof(float)));
    current.index.push_back({pivot, key, row});
  }
  index_bytes_ += current.index.capacity() * sizeof(ProjectionEntry);
  std::sort(current.index.begin(), current.index.end(),
            [](const ProjectionEntry& a, const ProjectionEntry& b) {
              return a.pivot == b.pivot ? a.key < b.key : a.pivot < b.pivot;
            });
}

uint64_t Database::write(const CreateTable& statement) {
  if (names_.contains(statement.name)) throw SqlError("TABLE_EXISTS", "table already exists");
  if (tables_.size() >= std::numeric_limits<uint32_t>::max() ||
      statement.columns.size() >= std::numeric_limits<uint32_t>::max()) {
    throw SqlError("SCHEMA", "too many tables or columns");
  }
  const auto id = static_cast<uint32_t>(tables_.size());
  auto current = std::make_unique<Table>(id, statement.name, statement.columns);
  std::string definition;
  store_text(definition, current->name);
  store(definition, static_cast<uint32_t>(current->columns.size()));
  for (const auto& column : current->columns) {
    store_text(definition, column.name);
    store(definition, static_cast<uint8_t>(column.type));
    store(definition, column.length);
    store(definition, static_cast<uint8_t>(column.primary));
  }
  log_.append(Op::create, id, 0, 0, definition);
  names_.emplace(current->name, id);
  tables_.push_back(std::move(current));
  return 0;
}

uint64_t Database::write(const CreateIndex& statement) {
  Table& current = table(statement.table);
  if (current.column(statement.column) != current.vector) {
    throw SqlError("SCHEMA", "index column must be VECTOR");
  }
  if (!current.index_name.empty()) throw SqlError("INDEX_EXISTS", "vector index already exists");
  // Map the committed prefix before the index definition enters the write buffer.
  log_.prepare_reads();
  apply(log_.append(Op::index, current.id, 0, 0, statement.name));
  build_index(current);
  return 0;
}

uint64_t Database::write(const Insert& statement) {
  Table& current = table(statement.table);
  const size_t column_count = current.columns.size();
  std::vector<size_t> positions(column_count, missing);
  if (statement.columns.empty()) {
    std::iota(positions.begin(), positions.end(), size_t{0});
  } else {
    if (statement.columns.size() != column_count) throw SqlError("VALUE", "all columns are required");
    for (size_t i = 0; i < column_count; ++i) {
      auto& position = positions[current.column(statement.columns[i])];
      if (position != missing) throw SqlError("SCHEMA", "duplicate insert column");
      position = i;
    }
  }

  invalidate_index(current);
  std::string bytes;
  for (const auto& row : statement.rows) {
    if (row.size() != column_count) throw SqlError("VALUE", "incorrect number of values");
    const int64_t key = bigint(row[positions[current.primary]]);
    if (current.rows.contains(key)) throw SqlError("duplicate_key", "primary key already exists");
    bytes.clear();
    for (size_t column : current.layout) {
      encode(bytes, current.columns[column], row[positions[column]]);
    }
    apply(log_.append(Op::put, current.id, key, 0, bytes));
  }
  return statement.rows.size();
}

uint64_t Database::write(const Update& statement) {
  Table& current = table(statement.table);
  auto filters = bind_filters(current, statement.filter);
  const KeyRange range = restrict_keys(current, filters);
  std::vector<std::optional<std::string>> replacements(current.columns.size());
  for (const auto& [name, value] : statement.assignments) {
    const size_t column = current.column(name);
    if (column == current.primary) throw SqlError("unsupported_sql", "cannot update primary key");
    if (replacements[column]) throw SqlError("SCHEMA", "duplicate SET column");
    encode(replacements[column].emplace(), current.columns[column], value);
  }

  invalidate_index(current);
  log_.prepare_reads();
  const auto [first, last] = range.iterators(current.rows);
  uint64_t affected = 0;
  std::string bytes;
  std::vector<Cell> cells(current.columns.size());
  for (auto row = first; row != last; ++row) {
    const auto old = payload(log_, row->second);
    if (!filters.empty()) {
      decode(current, row->first, old, cells);
      if (!matches(filters, cells)) continue;
    }
    bytes.clear();
    Reader reader(old);
    for (size_t column : current.layout) {
      const auto before = old.substr(old.size() - reader.remaining());
      read_cell(reader, current.columns[column]);
      if (replacements[column]) {
        bytes += *replacements[column];
      } else {
        bytes += before.substr(0, before.size() - reader.remaining());
      }
    }
    ++affected;
    if (bytes != old) {
      apply(log_.append(Op::put, current.id, row->first, row->second.offset, bytes));
    }
  }
  return affected;
}

uint64_t Database::write(const Delete& statement) {
  Table& current = table(statement.table);
  auto filters = bind_filters(current, statement.filter);
  const KeyRange range = restrict_keys(current, filters);
  invalidate_index(current);
  log_.prepare_reads();
  uint64_t affected = 0;
  std::vector<Cell> cells(current.columns.size());
  auto row = current.rows.lower_bound(range.low);
  while (row != current.rows.end() && range.contains(row->first)) {
    const auto [key, location] = *row;
    if (!filters.empty()) {
      decode(current, key, payload(log_, location), cells);
      if (!matches(filters, cells)) {
        ++row;
        continue;
      }
    }
    apply(log_.append(Op::erase, current.id, key, location.offset));
    ++affected;
    // Erasing can shift entries or merge pages, so resume by key.
    row = current.rows.upper_bound(key);
  }
  return affected;
}

void Database::transact(Transaction operation) {
  log_.release_reads();
  if (operation == Transaction::begin) {
    if (transaction_) throw SqlError("TRANSACTION", "transaction already active");
    transaction_ = log_.end();
    return;
  }
  if (!transaction_) throw SqlError("TRANSACTION", "no active transaction");
  if (operation == Transaction::rollback) {
    close();
  } else {
    if (*transaction_ != log_.end()) log_.commit();
    transaction_.reset();
  }
}

void Database::close() {
  if (!transaction_) return;
  undo(*transaction_, log_.end());
  log_.discard_after(*transaction_);
  transaction_.reset();
}

void Database::execute(const Statement& statement, SearchMode mode, ResultSink& sink) {
  std::visit([&](const auto& command) {
    using Command = std::decay_t<decltype(command)>;
    if constexpr (std::is_same_v<Command, Select>) {
      select(command, mode, sink);
    } else if constexpr (std::is_same_v<Command, Transaction>) {
      transact(command);
      sink.ok(0);
    } else {
      log_.release_reads();
      const uint64_t mark = log_.end();
      uint64_t affected;
      try {
        affected = write(command);
      } catch (const SqlError&) {
        undo(mark, log_.end());
        log_.discard_after(mark);
        throw;
      }
      if (!transaction_ && mark != log_.end()) log_.commit();
      sink.ok(affected);
    }
  }, statement);
}

namespace {

struct BoundExpression {
  size_t column;
  size_t query;
};

struct OrderKey {
  BoundExpression expression;
  bool descending;
};

struct Selection {
  int64_t key;
  RowRef row;
};

struct Neighbor {
  Selection selection;
  double squared;
};

struct TextPosition {
  uint64_t offset;
  uint32_t length;
};

using SortValue = std::variant<int64_t, TextPosition, double>;
struct OrderedRow {
  Selection selection;
  std::vector<SortValue> values;
};

template <typename Entry, typename Less>
void collect(std::vector<Entry>& rows, Entry entry, size_t limit, bool bounded, Less less) {
  if (!bounded) {
    rows.push_back(std::move(entry));
  } else if (rows.size() < limit) {
    rows.push_back(std::move(entry));
    std::push_heap(rows.begin(), rows.end(), less);
  } else if (less(entry, rows.front())) {
    std::pop_heap(rows.begin(), rows.end(), less);
    rows.back() = std::move(entry);
    std::push_heap(rows.begin(), rows.end(), less);
  }
}

}  // namespace

void Database::select(const Select& statement, SearchMode mode, ResultSink& sink) {
  Table& current = table(statement.table);
  auto filters = bind_filters(current, statement.filter);
  const KeyRange range = restrict_keys(current, filters);
  std::ranges::sort(filters, {}, &Filter::column);

  std::vector<std::vector<float>> queries;
  const auto bind = [&](const Expression& expression) {
    const size_t column = current.column(expression.column);
    if (!expression.query) return BoundExpression{column, missing};
    if (column != current.vector) throw SqlError("VALUE", "L2_DISTANCE requires a VECTOR column");
    auto value = vector_value(*expression.query, current.columns[column].length);
    const auto existing = std::find(queries.begin(), queries.end(), value);
    const size_t index = static_cast<size_t>(existing - queries.begin());
    if (existing == queries.end()) queries.push_back(std::move(value));
    return BoundExpression{column, index};
  };

  std::vector<BoundExpression> projections;
  std::vector<std::string> names;
  if (statement.star) {
    for (size_t i = 0; i < current.columns.size(); ++i) {
      projections.push_back({i, missing});
      names.push_back(current.columns[i].name);
    }
  } else {
    for (const auto& expression : statement.projections) {
      projections.push_back(bind(expression));
      names.push_back(!expression.alias.empty() ? expression.alias :
                      expression.query ? "l2_distance" : expression.column);
    }
  }
  std::vector<OrderKey> order;
  for (const auto& ordering : statement.order) {
    const auto& expression = ordering.key;
    const auto alias = std::find_if(statement.projections.begin(), statement.projections.end(),
                                    [&](const Expression& projection) {
      return !expression.query && !projection.alias.empty() && projection.alias == expression.column;
    });
    const BoundExpression bound = alias == statement.projections.end()
        ? bind(expression) : projections[alias - statement.projections.begin()];
    if (bound.query == missing && bound.column == current.vector) {
      throw SqlError("unsupported_sql", "cannot order by a vector value");
    }
    order.push_back({bound, ordering.descending});
  }

  const size_t limit = std::min<uint64_t>(current.rows.size(),
      statement.limit.value_or(current.rows.size()));
  if (limit == 0) {
    sink.begin_rows(names, 0);
    sink.end_rows();
    return;
  }

  log_.prepare_reads();
  const auto [first, last] = range.iterators(current.rows);
  std::vector<Cell> decoded(current.columns.size());
  std::vector<Cell> output(projections.size());
  const bool project_columns = std::any_of(projections.begin(), projections.end(),
      [&](const BoundExpression& projection) {
        return projection.query == missing && projection.column != current.primary;
      });

  const auto accepted = [&](int64_t key, std::string_view bytes) {
    if (filters.empty()) return true;
    decode(current, key, bytes, decoded);
    return matches(filters, decoded);
  };
  const auto included = [&](int64_t key, RowRef row) {
    if (filters.empty()) return true;
    if (row.length > wide_row) return matches_wide_row(log_, current, row, filters);
    return accepted(key, payload(log_, row));
  };
  const auto distance = [&](std::string_view bytes, size_t query) {
    return squared_distance(bytes.substr(0, current.vector_bytes), queries[query]);
  };
  const auto vector_payload = [&](RowRef row) {
    return log_.read(row.offset + Frame::header_size, current.vector_bytes);
  };
  const auto emit = [&](Selection selection, size_t known_query = missing, double known_square = 0) {
    std::string_view bytes;
    decoded[current.primary] = selection.key;
    if (project_columns) {
      bytes = payload(log_, selection.row);
      decode(current, selection.key, bytes, decoded);
    }
    for (size_t i = 0; i < projections.size(); ++i) {
      const auto& expression = projections[i];
      if (expression.query == missing) {
        output[i] = decoded[expression.column];
      } else {
        if (expression.query != known_query && bytes.empty()) {
          bytes = vector_payload(selection.row);
        }
        const double square = expression.query == known_query
            ? known_square : distance(bytes, expression.query);
        output[i] = std::sqrt(square);
      }
    }
    sink.row(output);
  };

  const bool primary_order = !order.empty() && order.front().expression.column == current.primary;
  if (order.empty() || primary_order) {
    const auto traverse = [&](auto visit) {
      if (primary_order && order.front().descending) {
        for (auto row = std::make_reverse_iterator(last);
             row != std::make_reverse_iterator(first); ++row) {
          if (!visit(*row)) break;
        }
      } else {
        for (auto row = first; row != last; ++row) {
          if (!visit(*row)) break;
        }
      }
    };
    size_t count = 0;
    if (filters.empty() && first == current.rows.begin() && last == current.rows.end()) {
      count = limit;
    } else {
      traverse([&](const auto& row) {
        count += included(row.first, row.second);
        return count < limit;
      });
    }
    sink.begin_rows(names, count);
    if (count != 0) {
      size_t sent = 0;
      traverse([&](const auto& row) {
        if (included(row.first, row.second)) {
          emit({row.first, row.second});
          ++sent;
        }
        return sent < count;
      });
    }
  } else if (order.size() == 1 && order.front().expression.query != missing) {
    const size_t query = order.front().expression.query;
    const bool descending = order.front().descending;
    const double margin = distance_margin(queries[query].size());
    const auto better = [&](const Neighbor& a, const Neighbor& b) {
      const int cmp = compare_distance(log_, a.selection.row, a.squared, b.selection.row,
                                       b.squared, queries[query], margin);
      if (cmp == 0) return a.selection.key < b.selection.key;
      return descending ? cmp > 0 : cmp < 0;
    };
    std::vector<Neighbor> nearest;
    const bool bounded = statement.limit.has_value();
    if (bounded) nearest.reserve(std::min<size_t>(limit, 1024));
#if defined(__x86_64__)
    const bool float_bound = bounded && !descending && queries[query].size() >= 32 &&
                             queries[query].size() <= 4096 && __builtin_cpu_supports("avx2");
#endif
    const auto probe = [&](int64_t key, RowRef row) {
      if (included(key, row)) {
        const auto bytes = vector_payload(row);
#if defined(__x86_64__)
        if (float_bound && nearest.size() == limit &&
            separated_squares(avx2_lower_square(bytes.data(), queries[query].data(),
                                                queries[query].size()),
                              nearest.front().squared, margin) > 0) return;
#endif
        collect(nearest, Neighbor{{key, row}, distance(bytes, query)}, limit, bounded, better);
      }
    };
    if (mode == SearchMode::ann && bounded && !descending && !current.index_name.empty()) {
      build_index(current);
    }
    bool used_bounds = false;
#if defined(__x86_64__)
    if (mode == SearchMode::exact && float_bound && limit <= 64 && first != last && !transaction_) {
      prepare_bounds(current);
      BoundQuery packed;
      if (current.bounds && make_bound_query(*current.bounds, queries[query], packed)) {
        double previous_worst = -1;
        const auto visit = [&](const BoundRow& entry, int32_t cutoff) {
          probe(entry.key, entry.row);
          if (nearest.size() == limit && nearest.front().squared != previous_worst) {
            previous_worst = nearest.front().squared;
            return bound_cutoff(previous_worst, margin, queries[query].size(),
                                current.bounds->inverse_step);
          }
          return cutoff;
        };
        auto& cache = *current.bounds;
        const Filter* equality = filters.size() == 1 && filters.front().comparison == Comparison::eq
            ? &filters.front() : nullptr;
        if (equality && !cache.groups_attempted) {
          cache.groups_attempted = true;
          const size_t budget = bound_budget - bound_bytes_;
          try {
            auto groups = group_slots(log_, current, equality->column, budget);
            if (groups && groups->bytes() <= budget) {
              bound_bytes_ += groups->bytes();
              cache.groups = std::move(groups);
            }
          } catch (const std::bad_alloc&) {
            // The bounded scan needs no filter index.
          }
        }
        if (equality && cache.groups && cache.groups->column == equality->column) {
          // Equal values always share a bucket; the ordinary predicate check
          // in probe still rejects collisions before admitting any neighbor.
          const auto slots = cache.groups->lookup(filter_tag(equality->value));
          if (cache.stride == 128) scan_bounds<128, true>(cache, packed, range, slots, visit);
          else scan_bounds<0, true>(cache, packed, range, slots, visit);
        } else if (cache.stride == 128) {
          scan_bounds<128, false>(cache, packed, range, {}, visit);
        } else {
          scan_bounds<0, false>(cache, packed, range, {}, visit);
        }
        used_bounds = true;
      }
    }
#endif
    if (!used_bounds && mode == SearchMode::ann && bounded && !descending && !current.index.empty()) {
      // A first-coordinate projection bounds the full L2 distance from
      // below. Walk outward in order of that bound and visit ties before
      // stopping so exact and indexed searches agree on primary-key order.
      const double pivot = queries[query].front();
      const auto split = std::lower_bound(current.index.begin(), current.index.end(), pivot,
          [](const ProjectionEntry& entry, double value) { return entry.pivot < value; });
      size_t left = split - current.index.begin();
      size_t right = left;
      while (left || right < current.index.size()) {
        const double left_delta = left ? pivot - current.index[left - 1].pivot
                                       : std::numeric_limits<double>::infinity();
        const double right_delta = right < current.index.size() ? current.index[right].pivot - pivot
                                                               : std::numeric_limits<double>::infinity();
        const double delta = std::min(left_delta, right_delta);
        if (nearest.size() == limit &&
            separated_squares(delta * delta, nearest.front().squared, margin) > 0) {
          break;
        }
        const auto& entry = left_delta < right_delta ? current.index[--left] : current.index[right++];
        if (range.contains(entry.key)) probe(entry.key, entry.row);
      }
    } else if (!used_bounds) {
      for (auto row = first; row != last; ++row) probe(row->first, row->second);
    }
    std::sort(nearest.begin(), nearest.end(), better);
    sink.begin_rows(names, nearest.size());
    for (const auto& row : nearest) emit(row.selection, query, row.squared);
  } else {
    const auto better = [&](const OrderedRow& a, const OrderedRow& b) {
      for (size_t i = 0; i < order.size(); ++i) {
        const auto query = order[i].expression.query;
        int cmp;
        if (query != missing) {
          cmp = compare_distance(log_, a.selection.row, std::get<double>(a.values[i]),
                                 b.selection.row, std::get<double>(b.values[i]),
                                 queries[query], distance_margin(queries[query].size()));
        } else if (const auto* number = std::get_if<int64_t>(&a.values[i])) {
          cmp = compare(*number, std::get<int64_t>(b.values[i]));
        } else {
          const auto left = std::get<TextPosition>(a.values[i]);
          const auto right = std::get<TextPosition>(b.values[i]);
          cmp = log_.compare_bytes(left.offset, left.length, right.offset, right.length);
        }
        if (cmp != 0) return order[i].descending ? cmp > 0 : cmp < 0;
      }
      return a.selection.key < b.selection.key;
    };
    std::vector<OrderedRow> sorted;
    for (auto row = first; row != last; ++row) {
      const auto bytes = payload(log_, row->second);
      if (!accepted(row->first, bytes)) continue;
      if (filters.empty()) decode(current, row->first, bytes, decoded);
      OrderedRow entry{{row->first, row->second}, {}};
      entry.values.reserve(order.size());
      for (const auto& key : order) {
        const auto& expression = key.expression;
        if (expression.query != missing) {
          entry.values.emplace_back(distance(bytes, expression.query));
        } else if (const auto* number = std::get_if<int64_t>(&decoded[expression.column])) {
          entry.values.emplace_back(*number);
        } else {
          const auto text = std::get<std::string_view>(decoded[expression.column]);
          const auto relative = static_cast<uint64_t>(text.data() - bytes.data());
          entry.values.emplace_back(TextPosition{row->second.offset + Frame::header_size + relative,
                                                 static_cast<uint32_t>(text.size())});
        }
      }
      collect(sorted, std::move(entry), limit, statement.limit.has_value(), better);
    }
    std::sort(sorted.begin(), sorted.end(), better);
    sink.begin_rows(names, sorted.size());
    for (const auto& row : sorted) emit(row.selection);
  }
  sink.end_rows();
}

}  // namespace mini
