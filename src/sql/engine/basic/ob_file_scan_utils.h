/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// File-as-database ("file sql") support utilities:
//  * streaming CSV / JSON(L) record readers (bounded memory, chunked IO)
//  * full-file schema inference (BIGINT/DOUBLE/BOOLEAN/DATE/DATETIME/VARCHAR)
//  * a row scanner used by the values-table access operator for file tables
// Parquet decoding lives in ob_file_parquet_reader.{h,cpp}.
// No external query engine is involved: these classes only decode bytes,
// filtering/aggregation/join are done by the seekdb SQL engine.

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_

#include <limits.h>
#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/container/ob_se_array.h"
#include "lib/container/ob_iarray.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/allocator/page_arena.h"
#include "lib/utility/ob_unify_serialize.h"
#include "lib/utility/ob_print_utils.h"

namespace oceanbase
{
namespace sql
{
class ObParquetFileReader;
struct ObFileErrorInfo;

enum ObFileFormatType : int8_t
{
  FILE_FORMAT_INVALID = 0,
  FILE_FORMAT_CSV = 1,
  FILE_FORMAT_JSON = 2,
  FILE_FORMAT_PARQUET = 3,
  FILE_FORMAT_GLOB = 4,
};

enum ObFileJsonLayout : int8_t
{
  FILE_JSON_AUTO = 0,
  FILE_JSON_NDJSON = 1,
  FILE_JSON_ARRAY = 2,
};

// logical column types produced by inference, ordered loosely by generality
enum ObFileColType : int8_t
{
  FILE_COL_NULL = 0,      // only NULL values seen so far
  FILE_COL_BOOLEAN = 1,
  FILE_COL_BIGINT = 2,
  FILE_COL_DOUBLE = 3,
  FILE_COL_DATE = 4,
  FILE_COL_DATETIME = 5,
  FILE_COL_VARCHAR = 6,
};

struct ObFileScanOptions
{
  OB_UNIS_VERSION(1);
public:
  static const int64_t DEFAULT_MAX_RECORD_SIZE = 16L << 20;   // 16MB per CSV record / JSON object
  static const int64_t DEFAULT_IO_BUFFER_SIZE = 1L << 20;     // read files in 1MB chunks
  static const int64_t MAX_COLUMN_COUNT = 4096;
  ObFileScanOptions() { reset(); }
  void reset()
  {
    format_ = FILE_FORMAT_INVALID;
    json_layout_ = FILE_JSON_AUTO;
    header_ = true;
    delim_ = ',';
    quote_ = '"';
    escape_ = '"';
    null_str_.reset();
    skip_rows_ = 0;
    ignore_errors_ = false;
    null_padding_ = false;
    all_varchar_ = false;
    max_record_size_ = DEFAULT_MAX_RECORD_SIZE;
  }
  int deep_copy(const ObFileScanOptions &other, common::ObIAllocator &allocator);
  TO_STRING_KV(K_(format), K_(json_layout), K_(header), K_(delim), K_(quote), K_(escape),
               K_(null_str), K_(skip_rows), K_(ignore_errors), K_(null_padding),
               K_(all_varchar), K_(max_record_size));
public:
  int8_t format_;
  int8_t json_layout_;
  bool header_;
  int8_t delim_;
  int8_t quote_;
  int8_t escape_;
  common::ObString null_str_;
  int64_t skip_rows_;
  bool ignore_errors_;
  bool null_padding_;
  bool all_varchar_;
  int64_t max_record_size_;
};

// parquet physical/logical info needed to decode a column, filled by the parquet reader
struct ObFileParquetColInfo
{
  OB_UNIS_VERSION(1);
public:
  ObFileParquetColInfo() : physical_type_(-1), conv_(0), scale_(0), type_length_(0), max_def_(0) {}
  TO_STRING_KV(K_(physical_type), K_(conv), K_(scale), K_(type_length), K_(max_def));
  enum Conv : int8_t {
    CONV_NONE = 0,
    CONV_DATE = 1,           // int32 days
    CONV_TS_MILLIS = 2,
    CONV_TS_MICROS = 3,
    CONV_TS_NANOS = 4,
    CONV_DECIMAL = 5,        // to double, scale_
    CONV_INT96_TS = 6,
    CONV_UINT32 = 7,
    CONV_UINT64 = 8,
    CONV_UINT8_16 = 9,
  };
  int8_t physical_type_;
  int8_t conv_;
  int32_t scale_;
  int32_t type_length_;
  int8_t max_def_;
};

struct ObFileColumnDesc
{
  OB_UNIS_VERSION(1);
public:
  ObFileColumnDesc()
    : name_(), type_(FILE_COL_NULL), nullable_(false), max_length_(0), scale_(0),
      src_idx_(-1), pq_()
  {}
  int deep_copy(const ObFileColumnDesc &other, common::ObIAllocator &allocator);
  TO_STRING_KV(K_(name), K_(type), K_(nullable), K_(max_length), K_(scale), K_(src_idx), K_(pq));
public:
  common::ObString name_;
  int8_t type_;            // ObFileColType
  bool nullable_;
  int64_t max_length_;     // VARCHAR: max character length seen
  int16_t scale_;          // DATETIME: 0 or 6
  int32_t src_idx_;        // parquet leaf column index
  ObFileParquetColInfo pq_;
};

// identity of a file, used to detect changes between inference (resolve) and scan (execution)
struct ObFileIdentity
{
  OB_UNIS_VERSION(1);
public:
  ObFileIdentity() : size_(-1), mtime_ns_(0), inode_(0), dev_(0) {}
  bool is_valid() const { return size_ >= 0; }
  bool operator==(const ObFileIdentity &o) const
  { return size_ == o.size_ && mtime_ns_ == o.mtime_ns_ && inode_ == o.inode_ && dev_ == o.dev_; }
  bool operator!=(const ObFileIdentity &o) const { return !(*this == o); }
  TO_STRING_KV(K_(size), K_(mtime_ns), K_(inode), K_(dev));
  int64_t size_;
  int64_t mtime_ns_;
  int64_t inode_;
  int64_t dev_;
};

// everything the resolver learns about one file table function call
struct ObFileTableInfo
{
public:
  ObFileTableInfo() : path_(), func_name_(), options_(), columns_(), identity_(), row_count_(0) {}
  int deep_copy(const ObFileTableInfo &other, common::ObIAllocator &allocator);
  TO_STRING_KV(K_(path), K_(func_name), K_(options), K_(columns), K_(identity), K_(row_count));
  common::ObString path_;        // real path, NUL terminated
  common::ObString func_name_;   // read_csv / read_json / ...
  ObFileScanOptions options_;
  common::ObSEArray<ObFileColumnDesc, 8, common::ModulePageAllocator, true> columns_;
  ObFileIdentity identity_;
  int64_t row_count_;
};

class ObFileSqlErrorReporter
{
public:
  // turn a reader/inferrer failure into a user visible error that names the function, file and line
  static int report(int ret, const common::ObString &func, const common::ObString &path,
                    const ObFileErrorInfo &err);
};

class ObFileTypeUtil
{
public:
  static const char *type_name(int8_t type);
  // widening lattice: NULL < any; BIGINT < DOUBLE; DATE < DATETIME; otherwise VARCHAR
  static ObFileColType widen(ObFileColType a, ObFileColType b);
  // classify a non-null CSV text value
  static ObFileColType classify_text(const char *p, int64_t len, bool &has_frac);
  // classify a JSON string value (only temporal types or VARCHAR)
  static ObFileColType classify_json_string(const char *p, int64_t len, bool &has_frac);
  static bool parse_int64(const char *p, int64_t len, int64_t &v, bool &overflow);
  static bool parse_double(const char *p, int64_t len, double &v);
  static bool parse_bool(const char *p, int64_t len, bool &v);
  // strict YYYY-MM-DD with calendar validation; days since 1970-01-01
  static bool parse_date(const char *p, int64_t len, int32_t &days);
  // YYYY-MM-DD[ |T]HH:MM:SS[.ffffff]; microseconds since epoch (no time zone)
  // allow_date_only: accept a bare date (midnight), used when DATE widened to DATETIME
  static bool parse_datetime(const char *p, int64_t len, int64_t &usec, bool &has_frac,
                             bool allow_date_only);
  static int64_t days_from_civil(int64_t y, int64_t m, int64_t d);
  static bool is_valid_utf8(const char *p, int64_t len, int64_t &char_cnt);
};

// buffered sequential reader over a local file descriptor
class ObFileCharReader
{
public:
  ObFileCharReader();
  ~ObFileCharReader() { close(); }
  int open(const char *path, common::ObIAllocator &allocator, int64_t buf_size,
           const ObFileIdentity *expect_identity, ObFileIdentity *identity);
  void close();
  int rewind();
  // returns OB_ITER_END at eof
  inline int get(char &c)
  {
    int ret = common::OB_SUCCESS;
    if (OB_UNLIKELY(pos_ >= end_) && OB_FAIL(fill())) {
    } else {
      c = buf_[pos_++];
    }
    return ret;
  }
  inline int peek(char &c)
  {
    int ret = common::OB_SUCCESS;
    if (OB_UNLIKELY(pos_ >= end_) && OB_FAIL(fill())) {
    } else {
      c = buf_[pos_];
    }
    return ret;
  }
  int64_t offset() const { return file_offset_ - (end_ - pos_); }
  int fd() const { return fd_; }
  int64_t file_size() const { return identity_.size_; }
  const ObFileIdentity &identity() const { return identity_; }
private:
  int fill();
private:
  int fd_;
  char *buf_;
  int64_t buf_size_;
  int64_t pos_;
  int64_t end_;
  int64_t file_offset_;
  ObFileIdentity identity_;
};

// growable byte buffer with a hard size limit
class ObFileByteBuffer
{
public:
  ObFileByteBuffer() : allocator_(NULL), buf_(NULL), len_(0), cap_(0), limit_(0) {}
  void init(common::ObIAllocator &allocator, int64_t limit)
  { allocator_ = &allocator; limit_ = limit; buf_ = NULL; len_ = 0; cap_ = 0; }
  void reuse() { len_ = 0; }
  inline int append(char c)
  {
    int ret = common::OB_SUCCESS;
    if (OB_UNLIKELY(len_ >= cap_) && OB_FAIL(reserve(len_ + 1))) {
    } else {
      buf_[len_++] = c;
    }
    return ret;
  }
  int append(const char *p, int64_t n);
  int reserve(int64_t n);
  char *ptr() { return buf_; }
  const char *ptr() const { return buf_; }
  int64_t length() const { return len_; }
private:
  common::ObIAllocator *allocator_;
  char *buf_;
  int64_t len_;
  int64_t cap_;
  int64_t limit_;
};

struct ObFileErrorInfo
{
  ObFileErrorInfo() : line_(0) { msg_[0] = '\0'; }
  void reset() { line_ = 0; msg_[0] = '\0'; }
  int64_t line_;
  char msg_[512];
};

// streaming RFC4180-style CSV record reader
class ObCsvRecordReader
{
public:
  struct Field { int64_t off_; int64_t len_; bool quoted_; };
  ObCsvRecordReader() : reader_(NULL), opts_(NULL), line_(1), at_start_(true) {}
  int init(ObFileCharReader &reader, const ObFileScanOptions &opts, common::ObIAllocator &allocator);
  void reset_position() { line_ = 1; at_start_ = true; }
  // returns OB_ITER_END at eof; on malformed input returns OB_INVALID_ARGUMENT and fills err
  int next_record(ObFileErrorInfo &err);
  int64_t field_count() const { return fields_.count(); }
  common::ObString field(int64_t i) const
  { return common::ObString(fields_.at(i).len_, data_.ptr() + fields_.at(i).off_); }
  bool is_quoted(int64_t i) const { return fields_.at(i).quoted_; }
  bool is_null(int64_t i) const;
  int64_t record_line() const { return record_line_; }
private:
  int end_field(bool quoted, int64_t start);
private:
  ObFileCharReader *reader_;
  const ObFileScanOptions *opts_;
  ObFileByteBuffer data_;
  common::ObSEArray<Field, 32> fields_;
  int64_t line_;
  int64_t record_line_;
  bool at_start_;
};

// streaming reader for newline-delimited JSON objects or a top-level JSON array of objects.
// Only flat objects are accepted; nested objects/arrays are rejected with their location.
class ObJsonRecordReader
{
public:
  enum ValueKind : int8_t { JV_NULL = 0, JV_STRING, JV_INT, JV_DOUBLE, JV_TRUE, JV_FALSE };
  struct Member { int64_t key_off_; int64_t key_len_; int64_t val_off_; int64_t val_len_; int8_t kind_; };
  ObJsonRecordReader() : reader_(NULL), opts_(NULL), line_(1), layout_(FILE_JSON_AUTO), state_(0) {}
  int init(ObFileCharReader &reader, const ObFileScanOptions &opts, common::ObIAllocator &allocator);
  void reset_position() { line_ = 1; state_ = 0; }
  int next_record(ObFileErrorInfo &err);
  int64_t member_count() const { return members_.count(); }
  common::ObString key(int64_t i) const
  { return common::ObString(members_.at(i).key_len_, data_.ptr() + members_.at(i).key_off_); }
  common::ObString value(int64_t i) const
  { return common::ObString(members_.at(i).val_len_, data_.ptr() + members_.at(i).val_off_); }
  int8_t kind(int64_t i) const { return members_.at(i).kind_; }
  int64_t record_line() const { return record_line_; }
private:
  int skip_ws(char &c, bool &eof);
  int parse_object(ObFileErrorInfo &err);
  int parse_string(ObFileErrorInfo &err, int64_t &off, int64_t &len);
  int parse_scalar(char first, ObFileErrorInfo &err, int64_t &off, int64_t &len, int8_t &kind);
  int set_error(ObFileErrorInfo &err, const char *msg);
private:
  ObFileCharReader *reader_;
  const ObFileScanOptions *opts_;
  ObFileByteBuffer data_;
  common::ObSEArray<Member, 32> members_;
  int64_t line_;
  int64_t record_line_;
  int8_t layout_;
  int8_t state_;   // 0: before first record, 1: inside stream, 2: finished
};

class ObFileSchemaInferrer
{
public:
  // path must be an absolute, NUL-terminated, already security-checked path
  static int infer(const char *path, const ObFileScanOptions &opts,
                   common::ObIAllocator &allocator,
                   common::ObIArray<ObFileColumnDesc> &columns,
                   int64_t &row_count, ObFileIdentity &identity,
                   ObFileErrorInfo &err);
  // make header names unique: duplicates become name__2, name__3, ...; empty -> column<i>
  static int dedup_names(common::ObIAllocator &allocator, common::ObIArray<ObFileColumnDesc> &columns);
private:
  static int infer_csv(const char *path, const ObFileScanOptions &opts, common::ObIAllocator &allocator,
                       common::ObIArray<ObFileColumnDesc> &columns, int64_t &row_count,
                       ObFileIdentity &identity, ObFileErrorInfo &err);
  static int infer_json(const char *path, const ObFileScanOptions &opts, common::ObIAllocator &allocator,
                        common::ObIArray<ObFileColumnDesc> &columns, int64_t &row_count,
                        ObFileIdentity &identity, ObFileErrorInfo &err);
};

struct ObFileCell
{
  ObFileCell() : is_null_(true), int_(0), double_(0), str_() {}
  bool is_null_;
  int64_t int_;       // BOOLEAN / BIGINT / DATE (days) / DATETIME (usec)
  double double_;
  common::ObString str_;
};

// streaming row scanner over one file; cells are valid until the next get_next_row()
class ObFileRowScanner
{
public:
  explicit ObFileRowScanner(common::ObIAllocator &allocator);
  ~ObFileRowScanner();
  int open(const char *path, const ObFileScanOptions &opts,
           const common::ObIArray<ObFileColumnDesc> &columns,
           const ObFileIdentity *expect_identity,
           const common::ObIArray<bool> *needed);
  int get_next_row();
  int rescan();
  void close();
  const ObFileCell &cell(int64_t i) const { return cells_.at(i); }
  int64_t column_count() const { return cells_.count(); }
  const ObFileErrorInfo &error_info() const { return err_; }
private:
  int next_csv_row();
  int next_json_row();
  int convert_text(int64_t col, const common::ObString &text, bool is_json_string, ObFileCell &cell);
private:
  common::ObIAllocator &allocator_;
  common::ObArenaAllocator inner_alloc_;
  const ObFileScanOptions *opts_;
  common::ObSEArray<ObFileColumnDesc, 16> columns_;
  common::ObSEArray<bool, 16> needed_;
  common::ObSEArray<ObFileCell, 16> cells_;
  common::ObSEArray<int64_t, 16> json_map_;
  ObFileCharReader reader_;
  ObCsvRecordReader csv_;
  ObJsonRecordReader json_;
  ObParquetFileReader *parquet_;
  ObFileErrorInfo err_;
  int64_t rows_returned_;
  bool header_skipped_;
  bool opened_;
};

// options of COPY ... TO 'file' (format csv, header, delimiter, quote, escape, nullstr).
// They travel from the resolver to ObSelectIntoOp as a compact text string so no new
// plan/serialization plumbing is needed (stored in ObSelectIntoItem::external_properties_).
struct ObFileCopyOptions
{
public:
  static const char *const MAGIC;   // prefix identifying an encoded COPY option string
  ObFileCopyOptions() : header_(true), delim_(','), quote_('"'), escape_('"'), null_str_(), col_names_() {}
  void reset();
  int encode(common::ObIAllocator &allocator, common::ObString &out) const;
  int decode(const common::ObString &in, common::ObIAllocator &allocator);
  static bool is_copy_options(const common::ObString &in);
  TO_STRING_KV(K_(header), K_(delim), K_(quote), K_(escape), K_(null_str), K_(col_names));
  bool header_;
  char delim_;
  char quote_;
  char escape_;
  common::ObString null_str_;
  common::ObSEArray<common::ObString, 8, common::ModulePageAllocator, true> col_names_;
};

// CSV writer for COPY TO: writes into a temp file in the target directory and atomically
// publishes it with link(2) on commit, so the target is never partially written and an
// existing target is never overwritten. abort() (or destruction) removes the temp file.
class ObFileCsvWriter
{
public:
  ObFileCsvWriter();
  ~ObFileCsvWriter();
  // target must be an absolute path whose directory was already security checked
  int open(const char *target, const ObFileCopyOptions &opts, common::ObIAllocator &allocator);
  int write_header();
  int begin_row() { field_idx_ = 0; return common::OB_SUCCESS; }
  int write_field(const common::ObString &value, bool is_null);
  int end_row();
  int commit();
  void abort();
  bool is_open() const { return fd_ >= 0; }
  int64_t rows_written() const { return rows_; }
  // pure formatting helper, exposed for unit tests
  static bool need_quote(const common::ObString &v, const ObFileCopyOptions &opts);
private:
  int append(const char *p, int64_t n);
  int append_char(char c) { return append(&c, 1); }
  int flush();
private:
  int fd_;
  const ObFileCopyOptions *opts_;
  char *buf_;
  int64_t buf_pos_;
  int64_t buf_cap_;
  int64_t field_idx_;
  int64_t rows_;
  char target_[PATH_MAX + 1];
  char tmp_path_[PATH_MAX + 1];
};

} // namespace sql
} // namespace oceanbase
#endif
