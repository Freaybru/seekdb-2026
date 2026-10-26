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

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/engine/basic/ob_file_parquet_reader.h"
#include "lib/string/ob_sql_string.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>

namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER(ObFileScanOptions, format_, json_layout_, header_, delim_, quote_, escape_,
                    null_str_, skip_rows_, ignore_errors_, null_padding_, all_varchar_,
                    max_record_size_);
OB_SERIALIZE_MEMBER(ObFileParquetColInfo, physical_type_, conv_, scale_, type_length_, max_def_);
OB_SERIALIZE_MEMBER(ObFileColumnDesc, name_, type_, nullable_, max_length_, scale_, src_idx_, pq_);
OB_SERIALIZE_MEMBER(ObFileIdentity, size_, mtime_ns_, inode_, dev_);

int ObFileScanOptions::deep_copy(const ObFileScanOptions &other, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  *this = other;
  null_str_.reset();
  if (OB_FAIL(ob_write_string(allocator, other.null_str_, null_str_))) {
    LOG_WARN("failed to copy null str", K(ret));
  }
  return ret;
}

int ObFileColumnDesc::deep_copy(const ObFileColumnDesc &other, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  *this = other;
  name_.reset();
  if (OB_FAIL(ob_write_string(allocator, other.name_, name_))) {
    LOG_WARN("failed to copy column name", K(ret));
  }
  return ret;
}

/////////////////////////////// ObFileTypeUtil ///////////////////////////////

const char *ObFileTypeUtil::type_name(int8_t type)
{
  const char *name = "VARCHAR";
  switch (type) {
    case FILE_COL_BOOLEAN: name = "BOOLEAN"; break;
    case FILE_COL_BIGINT: name = "BIGINT"; break;
    case FILE_COL_DOUBLE: name = "DOUBLE"; break;
    case FILE_COL_DATE: name = "DATE"; break;
    case FILE_COL_DATETIME: name = "DATETIME"; break;
    default: name = "VARCHAR"; break;
  }
  return name;
}

ObFileColType ObFileTypeUtil::widen(ObFileColType a, ObFileColType b)
{
  ObFileColType res = FILE_COL_VARCHAR;
  if (a == b) {
    res = a;
  } else if (FILE_COL_NULL == a) {
    res = b;
  } else if (FILE_COL_NULL == b) {
    res = a;
  } else if ((FILE_COL_BIGINT == a && FILE_COL_DOUBLE == b) || (FILE_COL_DOUBLE == a && FILE_COL_BIGINT == b)) {
    res = FILE_COL_DOUBLE;
  } else if ((FILE_COL_DATE == a && FILE_COL_DATETIME == b) || (FILE_COL_DATETIME == a && FILE_COL_DATE == b)) {
    res = FILE_COL_DATETIME;
  } else {
    res = FILE_COL_VARCHAR;
  }
  return res;
}

static inline bool fs_is_digit(char c) { return c >= '0' && c <= '9'; }

bool ObFileTypeUtil::parse_int64(const char *p, int64_t len, int64_t &v, bool &overflow)
{
  bool ok = false;
  overflow = false;
  int64_t i = 0;
  bool neg = false;
  if (len > 0 && (p[0] == '-' || p[0] == '+')) {
    neg = (p[0] == '-');
    i = 1;
  }
  if (i < len) {
    uint64_t acc = 0;
    const uint64_t limit = neg ? static_cast<uint64_t>(INT64_MAX) + 1 : static_cast<uint64_t>(INT64_MAX);
    ok = true;
    for (; ok && i < len; ++i) {
      if (!fs_is_digit(p[i])) {
        ok = false;
      } else if (!overflow) {
        uint64_t d = static_cast<uint64_t>(p[i] - '0');
        if (acc > (limit - d) / 10) {
          overflow = true;
        } else {
          acc = acc * 10 + d;
        }
      }
    }
    if (ok && overflow) {
      ok = false;
    } else if (ok) {
      v = neg ? static_cast<int64_t>(0 - acc) : static_cast<int64_t>(acc);
    } else {
      overflow = false;
    }
  }
  return ok;
}

bool ObFileTypeUtil::parse_double(const char *p, int64_t len, double &v)
{
  // strict decimal grammar: [+-] (digits [. digits*] | . digits) [(e|E) [+-] digits]
  bool ok = false;
  int64_t i = 0;
  if (len > 0 && len < 256) {
    if (p[i] == '+' || p[i] == '-') { ++i; }
    int64_t int_digits = 0;
    int64_t frac_digits = 0;
    while (i < len && fs_is_digit(p[i])) { ++i; ++int_digits; }
    if (i < len && p[i] == '.') {
      ++i;
      while (i < len && fs_is_digit(p[i])) { ++i; ++frac_digits; }
    }
    ok = (int_digits + frac_digits) > 0;
    if (ok && i < len && (p[i] == 'e' || p[i] == 'E')) {
      ++i;
      if (i < len && (p[i] == '+' || p[i] == '-')) { ++i; }
      int64_t exp_digits = 0;
      while (i < len && fs_is_digit(p[i])) { ++i; ++exp_digits; }
      ok = exp_digits > 0;
    }
    ok = ok && (i == len);
    if (ok) {
      char buf[256];
      MEMCPY(buf, p, len);
      buf[len] = '\0';
      char *endp = NULL;
      double d = strtod(buf, &endp);
      if (endp != buf + len || isinf(d) || isnan(d)) {
        ok = false;
      } else {
        v = d;
      }
    }
  }
  return ok;
}

bool ObFileTypeUtil::parse_bool(const char *p, int64_t len, bool &v)
{
  bool ok = false;
  if (4 == len && 0 == strncasecmp(p, "true", 4)) {
    v = true;
    ok = true;
  } else if (5 == len && 0 == strncasecmp(p, "false", 5)) {
    v = false;
    ok = true;
  }
  return ok;
}

int64_t ObFileTypeUtil::days_from_civil(int64_t y, int64_t m, int64_t d)
{
  y -= m <= 2 ? 1 : 0;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static inline bool fs_parse_fixed_digits(const char *p, int64_t n, int64_t &v)
{
  bool ok = true;
  v = 0;
  for (int64_t i = 0; ok && i < n; ++i) {
    if (!fs_is_digit(p[i])) {
      ok = false;
    } else {
      v = v * 10 + (p[i] - '0');
    }
  }
  return ok;
}

bool ObFileTypeUtil::parse_date(const char *p, int64_t len, int32_t &days)
{
  bool ok = false;
  int64_t y = 0, m = 0, d = 0;
  if (10 == len && '-' == p[4] && '-' == p[7]
      && fs_parse_fixed_digits(p, 4, y) && fs_parse_fixed_digits(p + 5, 2, m)
      && fs_parse_fixed_digits(p + 8, 2, d)) {
    static const int8_t DAYS_IN_MONTH[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (y >= 1 && m >= 1 && m <= 12 && d >= 1) {
      bool leap = (0 == y % 4 && 0 != y % 100) || 0 == y % 400;
      int64_t dim = DAYS_IN_MONTH[m - 1] + ((2 == m && leap) ? 1 : 0);
      if (d <= dim) {
        days = static_cast<int32_t>(days_from_civil(y, m, d));
        ok = true;
      }
    }
  }
  return ok;
}

bool ObFileTypeUtil::parse_datetime(const char *p, int64_t len, int64_t &usec, bool &has_frac,
                                    bool allow_date_only)
{
  bool ok = false;
  int32_t days = 0;
  has_frac = false;
  if (len >= 10 && parse_date(p, 10, days)) {
    if (10 == len) {
      if (allow_date_only) {
        usec = static_cast<int64_t>(days) * 86400LL * 1000000LL;
        ok = true;
      }
    } else if (len >= 19 && (' ' == p[10] || 'T' == p[10]) && ':' == p[13] && ':' == p[16]) {
      int64_t hh = 0, mi = 0, ss = 0, frac = 0;
      if (fs_parse_fixed_digits(p + 11, 2, hh) && fs_parse_fixed_digits(p + 14, 2, mi)
          && fs_parse_fixed_digits(p + 17, 2, ss) && hh < 24 && mi < 60 && ss < 60) {
        ok = true;
        if (len > 19) {
          const int64_t frac_len = len - 20;
          if ('.' != p[19] || frac_len < 1 || frac_len > 6 || !fs_parse_fixed_digits(p + 20, frac_len, frac)) {
            ok = false;
          } else {
            for (int64_t i = frac_len; i < 6; ++i) { frac *= 10; }
            has_frac = true;
          }
        }
        if (ok) {
          usec = ((static_cast<int64_t>(days) * 86400LL + hh * 3600 + mi * 60 + ss) * 1000000LL) + frac;
        }
      }
    }
  }
  return ok;
}

ObFileColType ObFileTypeUtil::classify_text(const char *p, int64_t len, bool &has_frac)
{
  ObFileColType type = FILE_COL_VARCHAR;
  int64_t iv = 0;
  double dv = 0;
  bool bv = false;
  bool overflow = false;
  int32_t days = 0;
  int64_t usec = 0;
  has_frac = false;
  if (len <= 0) {
    type = FILE_COL_VARCHAR;
  } else if (parse_int64(p, len, iv, overflow)) {
    type = FILE_COL_BIGINT;
  } else if (parse_double(p, len, dv)) {
    type = FILE_COL_DOUBLE;
  } else if (parse_bool(p, len, bv)) {
    type = FILE_COL_BOOLEAN;
  } else if (10 == len && parse_date(p, len, days)) {
    type = FILE_COL_DATE;
  } else if (parse_datetime(p, len, usec, has_frac, false)) {
    type = FILE_COL_DATETIME;
  }
  return type;
}

ObFileColType ObFileTypeUtil::classify_json_string(const char *p, int64_t len, bool &has_frac)
{
  ObFileColType type = FILE_COL_VARCHAR;
  int32_t days = 0;
  int64_t usec = 0;
  has_frac = false;
  if (10 == len && parse_date(p, len, days)) {
    type = FILE_COL_DATE;
  } else if (len >= 19 && parse_datetime(p, len, usec, has_frac, false)) {
    type = FILE_COL_DATETIME;
  }
  return type;
}

bool ObFileTypeUtil::is_valid_utf8(const char *p, int64_t len, int64_t &char_cnt)
{
  bool ok = true;
  char_cnt = 0;
  const unsigned char *s = reinterpret_cast<const unsigned char *>(p);
  int64_t i = 0;
  while (ok && i < len) {
    unsigned char c = s[i];
    int64_t n = 0;
    uint32_t cp = 0;
    if (c < 0x80) {
      n = 1; cp = c;
    } else if ((c & 0xE0) == 0xC0) {
      n = 2; cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      n = 3; cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      n = 4; cp = c & 0x07;
    } else {
      ok = false;
    }
    if (ok && i + n > len) {
      ok = false;
    }
    for (int64_t k = 1; ok && k < n; ++k) {
      if ((s[i + k] & 0xC0) != 0x80) {
        ok = false;
      } else {
        cp = (cp << 6) | (s[i + k] & 0x3F);
      }
    }
    if (ok && n > 1) {
      // reject overlong encodings, surrogates and out of range code points
      if ((2 == n && cp < 0x80) || (3 == n && cp < 0x800) || (4 == n && cp < 0x10000)
          || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        ok = false;
      }
    }
    if (ok) {
      i += n;
      ++char_cnt;
    }
  }
  return ok;
}

/////////////////////////////// ObFileCharReader ///////////////////////////////

ObFileCharReader::ObFileCharReader()
  : fd_(-1), buf_(NULL), buf_size_(0), pos_(0), end_(0), file_offset_(0), identity_()
{}

int ObFileCharReader::open(const char *path, ObIAllocator &allocator, int64_t buf_size,
                           const ObFileIdentity *expect_identity, ObFileIdentity *identity)
{
  int ret = OB_SUCCESS;
  struct stat st;
  close();
  if (OB_ISNULL(path) || buf_size <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(path), K(buf_size));
  } else if ((fd_ = ::open(path, O_RDONLY | O_CLOEXEC)) < 0) {
    ret = (ENOENT == errno || ENOTDIR == errno) ? OB_FILE_NOT_EXIST : OB_IO_ERROR;
    LOG_WARN("failed to open file", K(ret), K(errno), K(path));
  } else if (0 != ::fstat(fd_, &st)) {
    ret = OB_IO_ERROR;
    LOG_WARN("failed to stat file", K(ret), K(errno), K(path));
  } else if (!S_ISREG(st.st_mode)) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("not a regular file", K(ret), K(path));
  } else {
    identity_.size_ = st.st_size;
    identity_.mtime_ns_ = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
    identity_.inode_ = static_cast<int64_t>(st.st_ino);
    identity_.dev_ = static_cast<int64_t>(st.st_dev);
    if (NULL != identity) {
      *identity = identity_;
    }
    if (NULL != expect_identity && expect_identity->is_valid() && *expect_identity != identity_) {
      ret = OB_FILE_ALREADY_EXIST;   // mapped to "file changed" by callers
      LOG_WARN("file changed after schema inference", K(ret), K(path), KPC(expect_identity), K(identity_));
    } else if (NULL == buf_ || buf_size_ < buf_size) {
      if (OB_ISNULL(buf_ = static_cast<char *>(allocator.alloc(buf_size)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to alloc read buffer", K(ret), K(buf_size));
      } else {
        buf_size_ = buf_size;
      }
    }
  }
  if (OB_SUCC(ret)) {
    pos_ = end_ = file_offset_ = 0;
    // skip a UTF-8 BOM
    if (OB_FAIL(fill())) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    } else if (end_ >= 3 && (unsigned char)buf_[0] == 0xEF && (unsigned char)buf_[1] == 0xBB
               && (unsigned char)buf_[2] == 0xBF) {
      pos_ = 3;
    }
  } else if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  return ret;
}

void ObFileCharReader::close()
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  // the buffer belongs to the caller's allocator, forget it
  buf_ = NULL;
  buf_size_ = 0;
  pos_ = end_ = file_offset_ = 0;
}

int ObFileCharReader::rewind()
{
  int ret = OB_SUCCESS;
  if (fd_ < 0) {
    ret = OB_NOT_INIT;
  } else {
    pos_ = end_ = file_offset_ = 0;
    if (OB_FAIL(fill())) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    } else if (end_ >= 3 && (unsigned char)buf_[0] == 0xEF && (unsigned char)buf_[1] == 0xBB
               && (unsigned char)buf_[2] == 0xBF) {
      pos_ = 3;
    }
  }
  return ret;
}

int ObFileCharReader::fill()
{
  int ret = OB_SUCCESS;
  if (fd_ < 0 || OB_ISNULL(buf_)) {
    ret = OB_NOT_INIT;
  } else {
    ssize_t n = 0;
    do {
      n = ::pread(fd_, buf_, buf_size_, file_offset_);
    } while (n < 0 && EINTR == errno);
    if (n < 0) {
      ret = OB_IO_ERROR;
      LOG_WARN("failed to read file", K(ret), K(errno));
    } else if (0 == n) {
      pos_ = end_ = 0;
      ret = OB_ITER_END;
    } else {
      pos_ = 0;
      end_ = n;
      file_offset_ += n;
    }
  }
  return ret;
}

/////////////////////////////// ObFileByteBuffer ///////////////////////////////

int ObFileByteBuffer::reserve(int64_t n)
{
  int ret = OB_SUCCESS;
  if (n <= cap_) {
  } else if (OB_ISNULL(allocator_)) {
    ret = OB_NOT_INIT;
  } else if (n > limit_) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("record exceeds size limit", K(ret), K(n), K(limit_));
  } else {
    int64_t new_cap = MAX(cap_ * 2, 4096);
    while (new_cap < n) { new_cap *= 2; }
    new_cap = MIN(new_cap, limit_);
    char *nbuf = static_cast<char *>(allocator_->alloc(new_cap));
    if (OB_ISNULL(nbuf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to alloc record buffer", K(ret), K(new_cap));
    } else {
      if (len_ > 0) {
        MEMCPY(nbuf, buf_, len_);
      }
      if (NULL != buf_) {
        allocator_->free(buf_);
      }
      buf_ = nbuf;
      cap_ = new_cap;
    }
  }
  return ret;
}

int ObFileByteBuffer::append(const char *p, int64_t n)
{
  int ret = OB_SUCCESS;
  if (n <= 0) {
  } else if (len_ + n > cap_ && OB_FAIL(reserve(len_ + n))) {
  } else {
    MEMCPY(buf_ + len_, p, n);
    len_ += n;
  }
  return ret;
}

/////////////////////////////// ObCsvRecordReader ///////////////////////////////

int ObCsvRecordReader::init(ObFileCharReader &reader, const ObFileScanOptions &opts, ObIAllocator &allocator)
{
  reader_ = &reader;
  opts_ = &opts;
  data_.init(allocator, opts.max_record_size_ > 0 ? opts.max_record_size_ : ObFileScanOptions::DEFAULT_MAX_RECORD_SIZE);
  reset_position();
  return OB_SUCCESS;
}

bool ObCsvRecordReader::is_null(int64_t i) const
{
  const Field &f = fields_.at(i);
  bool res = false;
  if (!f.quoted_) {
    if (0 == f.len_) {
      res = true;
    } else if (!opts_->null_str_.empty()
               && opts_->null_str_.length() == f.len_
               && 0 == MEMCMP(opts_->null_str_.ptr(), data_.ptr() + f.off_, f.len_)) {
      res = true;
    }
  }
  return res;
}

int ObCsvRecordReader::end_field(bool quoted, int64_t start)
{
  int ret = OB_SUCCESS;
  Field f;
  f.off_ = start;
  f.len_ = data_.length() - start;
  f.quoted_ = quoted;
  if (fields_.count() >= ObFileScanOptions::MAX_COLUMN_COUNT) {
    ret = OB_SIZE_OVERFLOW;
  } else if (OB_FAIL(fields_.push_back(f))) {
    LOG_WARN("failed to push field", K(ret));
  }
  return ret;
}

int ObCsvRecordReader::next_record(ObFileErrorInfo &err)
{
  int ret = OB_SUCCESS;
  char c = 0;
  const char delim = static_cast<char>(opts_->delim_);
  const char quote = static_cast<char>(opts_->quote_);
  const char escape = static_cast<char>(opts_->escape_);
  const bool has_quote = (0 != opts_->quote_);
  const bool has_escape = (0 != opts_->escape_);
  fields_.reuse();
  data_.reuse();
  // skip blank lines
  bool found = false;
  while (OB_SUCC(ret) && !found) {
    record_line_ = line_;
    if (OB_FAIL(reader_->get(c))) {
    } else if ('\n' == c) {
      ++line_;
    } else if ('\r' == c) {
      char n = 0;
      if (OB_SUCC(reader_->peek(n)) && '\n' == n) {
        (void)reader_->get(n);
      }
      ret = (OB_ITER_END == ret) ? OB_SUCCESS : ret;
      ++line_;
    } else {
      found = true;
    }
  }
  bool record_end = false;
  bool bad_row = false;
  while (OB_SUCC(ret) && !record_end) {
    int64_t start = data_.length();
    if (has_quote && c == quote) {
      // quoted field
      bool closed = false;
      while (OB_SUCC(ret) && !closed) {
        char c2 = 0;
        if (OB_FAIL(reader_->get(c2))) {
          if (OB_ITER_END == ret) {
            ret = OB_INVALID_ARGUMENT;
            err.line_ = record_line_;
            snprintf(err.msg_, sizeof(err.msg_), "unterminated quoted field");
          }
        } else if (has_escape && escape != quote && c2 == escape) {
          char c3 = 0;
          if (OB_FAIL(reader_->get(c3))) {
            if (OB_ITER_END == ret) {
              ret = OB_INVALID_ARGUMENT;
              err.line_ = record_line_;
              snprintf(err.msg_, sizeof(err.msg_), "unterminated quoted field");
            }
          } else {
            if ('\n' == c3) { ++line_; }
            ret = data_.append(c3);
          }
        } else if (c2 == quote) {
          char c3 = 0;
          int tmp_ret = reader_->peek(c3);
          if (OB_SUCCESS == tmp_ret && has_escape && escape == quote && c3 == quote) {
            (void)reader_->get(c3);
            ret = data_.append(quote);
          } else if (OB_SUCCESS != tmp_ret && OB_ITER_END != tmp_ret) {
            ret = tmp_ret;
          } else {
            closed = true;
          }
        } else {
          if ('\n' == c2) {
            ++line_;
          }
          ret = data_.append(c2);
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(reader_->get(c))) {
          if (OB_ITER_END == ret) {
            ret = end_field(true, start);
            record_end = true;
          }
        } else if (c == delim) {
          if (OB_SUCC(end_field(true, start))) {
            if (OB_FAIL(reader_->get(c))) {
              if (OB_ITER_END == ret) {
                // trailing delimiter at eof: one more empty field
                ret = end_field(false, data_.length());
                record_end = true;
              }
            }
          }
        } else if ('\n' == c) {
          ++line_;
          ret = end_field(true, start);
          record_end = true;
        } else if ('\r' == c) {
          char n = 0;
          if (OB_SUCC(reader_->peek(n)) && '\n' == n) {
            (void)reader_->get(n);
          }
          ret = (OB_ITER_END == ret) ? OB_SUCCESS : ret;
          ++line_;
          if (OB_SUCC(ret)) {
            ret = end_field(true, start);
          }
          record_end = true;
        } else {
          ret = OB_INVALID_ARGUMENT;
          bad_row = true;
          err.line_ = record_line_;
          snprintf(err.msg_, sizeof(err.msg_), "unexpected character '%c' after closing quote", c);
        }
      }
    } else {
      // unquoted field
      bool field_end = false;
      while (OB_SUCC(ret) && !field_end) {
        if (c == delim) {
          field_end = true;
          if (OB_SUCC(end_field(false, start))) {
            if (OB_FAIL(reader_->get(c))) {
              if (OB_ITER_END == ret) {
                ret = end_field(false, data_.length());
                record_end = true;
              }
            }
          }
        } else if ('\n' == c) {
          ++line_;
          field_end = true;
          record_end = true;
          ret = end_field(false, start);
        } else if ('\r' == c) {
          char n = 0;
          if (OB_SUCC(reader_->peek(n)) && '\n' == n) {
            (void)reader_->get(n);
          }
          ret = (OB_ITER_END == ret) ? OB_SUCCESS : ret;
          ++line_;
          field_end = true;
          record_end = true;
          if (OB_SUCC(ret)) {
            ret = end_field(false, start);
          }
        } else {
          if (has_escape && escape != quote && c == escape) {
            char c3 = 0;
            if (OB_FAIL(reader_->get(c3))) {
              if (OB_ITER_END == ret) {
                ret = OB_SUCCESS;
                c3 = c;
              }
            }
            if (OB_SUCC(ret)) {
              if ('\n' == c3) { ++line_; }
              c = c3;
            }
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(data_.append(c))) {
          } else if (OB_FAIL(reader_->get(c))) {
            if (OB_ITER_END == ret) {
              ret = end_field(false, start);
              field_end = true;
              record_end = true;
            }
          }
        }
      }
    }
  }
  if (OB_SIZE_OVERFLOW == ret) {
    ret = OB_INVALID_ARGUMENT;
    err.line_ = record_line_;
    snprintf(err.msg_, sizeof(err.msg_), "record exceeds the limit of %ld bytes or %ld columns",
             opts_->max_record_size_, ObFileScanOptions::MAX_COLUMN_COUNT);
    bad_row = true;
  }
  if (bad_row) {
    // resynchronize at the end of the current physical line so ignore_errors can continue
    int tmp_ret = OB_SUCCESS;
    char n = c;
    while (OB_SUCCESS == tmp_ret && '\n' != n) {
      tmp_ret = reader_->get(n);
    }
    if ('\n' == n) {
      ++line_;
    }
  }
  return ret;
}

/////////////////////////////// ObJsonRecordReader ///////////////////////////////

int ObJsonRecordReader::init(ObFileCharReader &reader, const ObFileScanOptions &opts, ObIAllocator &allocator)
{
  reader_ = &reader;
  opts_ = &opts;
  layout_ = opts.json_layout_;
  data_.init(allocator, opts.max_record_size_ > 0 ? opts.max_record_size_ : ObFileScanOptions::DEFAULT_MAX_RECORD_SIZE);
  reset_position();
  return OB_SUCCESS;
}

int ObJsonRecordReader::set_error(ObFileErrorInfo &err, const char *msg)
{
  err.line_ = line_;
  snprintf(err.msg_, sizeof(err.msg_), "%s", msg);
  return OB_INVALID_ARGUMENT;
}

int ObJsonRecordReader::skip_ws(char &c, bool &eof)
{
  int ret = OB_SUCCESS;
  eof = false;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    if (OB_FAIL(reader_->get(c))) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
        eof = true;
        done = true;
      }
    } else if ('\n' == c) {
      ++line_;
    } else if (' ' == c || '\t' == c || '\r' == c) {
    } else {
      done = true;
    }
  }
  return ret;
}

int ObJsonRecordReader::next_record(ObFileErrorInfo &err)
{
  int ret = OB_SUCCESS;
  char c = 0;
  bool eof = false;
  members_.reuse();
  data_.reuse();
  if (2 == state_) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(skip_ws(c, eof))) {
  } else if (0 == state_) {
    if (eof) {
      state_ = 2;
      ret = OB_ITER_END;
    } else {
      if (FILE_JSON_AUTO == layout_) {
        layout_ = ('[' == c) ? FILE_JSON_ARRAY : FILE_JSON_NDJSON;
      }
      if (FILE_JSON_ARRAY == layout_) {
        if ('[' != c) {
          ret = set_error(err, "expected '[' at the beginning of a JSON array file");
        } else if (OB_FAIL(skip_ws(c, eof))) {
        } else if (eof) {
          ret = set_error(err, "unterminated JSON array");
        } else if (']' == c) {
          state_ = 2;
          if (OB_FAIL(skip_ws(c, eof))) {
          } else if (!eof) {
            ret = set_error(err, "unexpected content after the end of JSON array");
          } else {
            ret = OB_ITER_END;
          }
        }
      }
      if (OB_SUCC(ret)) {
        state_ = 1;
      }
    }
  } else if (FILE_JSON_NDJSON == layout_) {
    if (eof) {
      state_ = 2;
      ret = OB_ITER_END;
    }
  } else {
    // array layout, between elements
    if (eof) {
      ret = set_error(err, "unterminated JSON array");
    } else if (']' == c) {
      state_ = 2;
      if (OB_FAIL(skip_ws(c, eof))) {
      } else if (!eof) {
        ret = set_error(err, "unexpected content after the end of JSON array");
      } else {
        ret = OB_ITER_END;
      }
    } else if (',' != c) {
      ret = set_error(err, "expected ',' or ']' between JSON array elements");
    } else if (OB_FAIL(skip_ws(c, eof))) {
    } else if (eof) {
      ret = set_error(err, "unterminated JSON array");
    }
  }
  if (OB_SUCC(ret)) {
    record_line_ = line_;
    if ('{' == c) {
      ret = parse_object(err);
    } else if ('[' == c) {
      ret = set_error(err, "nested JSON arrays are not supported, each record must be a flat JSON object");
    } else {
      ret = set_error(err, "each record must be a JSON object");
    }
  }
  if (OB_SIZE_OVERFLOW == ret) {
    ret = OB_INVALID_ARGUMENT;
    err.line_ = record_line_;
    snprintf(err.msg_, sizeof(err.msg_), "JSON object exceeds the limit of %ld bytes or %ld keys",
             opts_->max_record_size_, ObFileScanOptions::MAX_COLUMN_COUNT);
  }
  if (OB_FAIL(ret) && OB_ITER_END != ret) {
    state_ = 2;   // cannot resynchronize a broken JSON stream reliably
  }
  return ret;
}

int ObJsonRecordReader::parse_object(ObFileErrorInfo &err)
{
  int ret = OB_SUCCESS;
  char c = 0;
  bool eof = false;
  bool done = false;
  if (OB_FAIL(skip_ws(c, eof))) {
  } else if (eof) {
    ret = set_error(err, "unexpected end of file inside JSON object");
  } else if ('}' == c) {
    done = true;
  }
  while (OB_SUCC(ret) && !done) {
    Member m;
    if ('"' != c) {
      ret = set_error(err, "expected a string key in JSON object");
    } else if (OB_FAIL(parse_string(err, m.key_off_, m.key_len_))) {
    } else if (OB_FAIL(skip_ws(c, eof))) {
    } else if (eof || ':' != c) {
      ret = set_error(err, "expected ':' after JSON object key");
    } else if (OB_FAIL(skip_ws(c, eof))) {
    } else if (eof) {
      ret = set_error(err, "unexpected end of file inside JSON object");
    } else if ('"' == c) {
      m.kind_ = JV_STRING;
      ret = parse_string(err, m.val_off_, m.val_len_);
    } else if ('{' == c || '[' == c) {
      char msg[256];
      snprintf(msg, sizeof(msg), "nested JSON %s for key \"%.*s\" is not supported, only flat objects are allowed",
               '{' == c ? "object" : "array", static_cast<int>(MIN(m.key_len_, 64)), data_.ptr() + m.key_off_);
      ret = set_error(err, msg);
    } else {
      ret = parse_scalar(c, err, m.val_off_, m.val_len_, m.kind_);
    }
    if (OB_FAIL(ret)) {
    } else if (members_.count() >= ObFileScanOptions::MAX_COLUMN_COUNT) {
      ret = OB_SIZE_OVERFLOW;
    } else if (OB_FAIL(members_.push_back(m))) {
    } else if (OB_FAIL(skip_ws(c, eof))) {
    } else if (eof) {
      ret = set_error(err, "unexpected end of file inside JSON object");
    } else if ('}' == c) {
      done = true;
    } else if (',' != c) {
      ret = set_error(err, "expected ',' or '}' in JSON object");
    } else if (OB_FAIL(skip_ws(c, eof))) {
    } else if (eof) {
      ret = set_error(err, "unexpected end of file inside JSON object");
    }
  }
  return ret;
}

static inline int fs_hex_val(char c)
{
  int v = -1;
  if (c >= '0' && c <= '9') { v = c - '0'; }
  else if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; }
  else if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; }
  return v;
}

int ObJsonRecordReader::parse_string(ObFileErrorInfo &err, int64_t &off, int64_t &len)
{
  int ret = OB_SUCCESS;
  off = data_.length();
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    char c = 0;
    if (OB_FAIL(reader_->get(c))) {
      if (OB_ITER_END == ret) {
        ret = set_error(err, "unterminated JSON string");
      }
    } else if ('"' == c) {
      done = true;
    } else if ('\\' == c) {
      char e = 0;
      if (OB_FAIL(reader_->get(e))) {
        if (OB_ITER_END == ret) {
          ret = set_error(err, "unterminated JSON string");
        }
      } else {
        switch (e) {
          case '"': ret = data_.append('"'); break;
          case '\\': ret = data_.append('\\'); break;
          case '/': ret = data_.append('/'); break;
          case 'b': ret = data_.append('\b'); break;
          case 'f': ret = data_.append('\f'); break;
          case 'n': ret = data_.append('\n'); break;
          case 'r': ret = data_.append('\r'); break;
          case 't': ret = data_.append('\t'); break;
          case 'u': {
            uint32_t cp = 0;
            for (int i = 0; OB_SUCC(ret) && i < 4; ++i) {
              char h = 0;
              int v = 0;
              if (OB_FAIL(reader_->get(h)) || (v = fs_hex_val(h)) < 0) {
                ret = set_error(err, "invalid \\u escape in JSON string");
              } else {
                cp = (cp << 4) | v;
              }
            }
            if (OB_SUCC(ret) && cp >= 0xD800 && cp <= 0xDBFF) {
              // surrogate pair
              char b1 = 0, b2 = 0;
              uint32_t lo = 0;
              if (OB_FAIL(reader_->get(b1)) || OB_FAIL(reader_->get(b2)) || '\\' != b1 || 'u' != b2) {
                ret = set_error(err, "invalid surrogate pair in JSON string");
              }
              for (int i = 0; OB_SUCC(ret) && i < 4; ++i) {
                char h = 0;
                int v = 0;
                if (OB_FAIL(reader_->get(h)) || (v = fs_hex_val(h)) < 0) {
                  ret = set_error(err, "invalid \\u escape in JSON string");
                } else {
                  lo = (lo << 4) | v;
                }
              }
              if (OB_SUCC(ret)) {
                if (lo < 0xDC00 || lo > 0xDFFF) {
                  ret = set_error(err, "invalid surrogate pair in JSON string");
                } else {
                  cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
              }
            } else if (OB_SUCC(ret) && cp >= 0xDC00 && cp <= 0xDFFF) {
              ret = set_error(err, "invalid surrogate pair in JSON string");
            }
            if (OB_SUCC(ret)) {
              char u[4];
              int n = 0;
              if (cp < 0x80) { u[0] = static_cast<char>(cp); n = 1; }
              else if (cp < 0x800) { u[0] = static_cast<char>(0xC0 | (cp >> 6)); u[1] = static_cast<char>(0x80 | (cp & 0x3F)); n = 2; }
              else if (cp < 0x10000) { u[0] = static_cast<char>(0xE0 | (cp >> 12)); u[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); u[2] = static_cast<char>(0x80 | (cp & 0x3F)); n = 3; }
              else { u[0] = static_cast<char>(0xF0 | (cp >> 18)); u[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); u[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); u[3] = static_cast<char>(0x80 | (cp & 0x3F)); n = 4; }
              ret = data_.append(u, n);
            }
            break;
          }
          default:
            ret = set_error(err, "invalid escape sequence in JSON string");
            break;
        }
      }
    } else if (static_cast<unsigned char>(c) < 0x20) {
      ret = set_error(err, "unescaped control character in JSON string");
    } else {
      ret = data_.append(c);
    }
  }
  if (OB_SUCC(ret)) {
    len = data_.length() - off;
    int64_t char_cnt = 0;
    if (!ObFileTypeUtil::is_valid_utf8(data_.ptr() + off, len, char_cnt)) {
      ret = set_error(err, "invalid UTF-8 in JSON string");
    }
  }
  return ret;
}

int ObJsonRecordReader::parse_scalar(char first, ObFileErrorInfo &err, int64_t &off, int64_t &len, int8_t &kind)
{
  int ret = OB_SUCCESS;
  off = data_.length();
  char c = first;
  bool done = false;
  if (OB_FAIL(data_.append(c))) {
  }
  while (OB_SUCC(ret) && !done) {
    if (OB_FAIL(reader_->peek(c))) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
        done = true;
      }
    } else if (',' == c || '}' == c || ']' == c || ' ' == c || '\t' == c || '\r' == c || '\n' == c) {
      done = true;
    } else if (data_.length() - off > 512) {
      ret = set_error(err, "invalid JSON value");
    } else {
      (void)reader_->get(c);
      ret = data_.append(c);
    }
  }
  if (OB_SUCC(ret)) {
    len = data_.length() - off;
    const char *p = data_.ptr() + off;
    if (4 == len && 0 == MEMCMP(p, "true", 4)) {
      kind = JV_TRUE;
    } else if (5 == len && 0 == MEMCMP(p, "false", 5)) {
      kind = JV_FALSE;
    } else if (4 == len && 0 == MEMCMP(p, "null", 4)) {
      kind = JV_NULL;
    } else {
      // JSON number: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
      int64_t i = 0;
      bool ok = true;
      bool is_int = true;
      if (i < len && '-' == p[i]) { ++i; }
      if (i < len && '0' == p[i]) {
        ++i;
      } else if (i < len && p[i] >= '1' && p[i] <= '9') {
        while (i < len && fs_is_digit(p[i])) { ++i; }
      } else {
        ok = false;
      }
      if (ok && i < len && '.' == p[i]) {
        ++i;
        is_int = false;
        int64_t n = 0;
        while (i < len && fs_is_digit(p[i])) { ++i; ++n; }
        ok = n > 0;
      }
      if (ok && i < len && ('e' == p[i] || 'E' == p[i])) {
        ++i;
        is_int = false;
        if (i < len && ('+' == p[i] || '-' == p[i])) { ++i; }
        int64_t n = 0;
        while (i < len && fs_is_digit(p[i])) { ++i; ++n; }
        ok = n > 0;
      }
      ok = ok && i == len;
      if (!ok) {
        char msg[128];
        snprintf(msg, sizeof(msg), "invalid JSON value '%.*s'", static_cast<int>(MIN(len, 32)), p);
        ret = set_error(err, msg);
      } else if (is_int) {
        int64_t v = 0;
        bool overflow = false;
        kind = ObFileTypeUtil::parse_int64(p, len, v, overflow) ? JV_INT : JV_DOUBLE;
      } else {
        kind = JV_DOUBLE;
      }
      if (OB_SUCC(ret) && JV_DOUBLE == kind) {
        double d = 0;
        if (!ObFileTypeUtil::parse_double(p, len, d)) {
          ret = set_error(err, "JSON number out of range");
        }
      }
    }
  }
  return ret;
}

/////////////////////////////// ObFileSchemaInferrer ///////////////////////////////

int ObFileSchemaInferrer::dedup_names(ObIAllocator &allocator, ObIArray<ObFileColumnDesc> &columns)
{
  int ret = OB_SUCCESS;
  char buf[OB_MAX_COLUMN_NAME_LENGTH + 32];
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    ObFileColumnDesc &col = columns.at(i);
    if (col.name_.empty()) {
      int n = snprintf(buf, sizeof(buf), "column%ld", i);
      if (OB_FAIL(ob_write_string(allocator, ObString(n, buf), col.name_))) {
      }
    } else if (col.name_.length() > OB_MAX_COLUMN_NAME_LENGTH) {
      // keep a valid prefix of the name
      int64_t len = OB_MAX_COLUMN_NAME_LENGTH;
      while (len > 0 && (static_cast<unsigned char>(col.name_.ptr()[len]) & 0xC0) == 0x80) { --len; }
      col.name_.assign_ptr(col.name_.ptr(), static_cast<int32_t>(len));
    }
  }
  for (int64_t i = 1; OB_SUCC(ret) && i < columns.count(); ++i) {
    ObString base = columns.at(i).name_;
    bool dup = true;
    int64_t suffix = 1;
    while (OB_SUCC(ret) && dup) {
      dup = false;
      for (int64_t j = 0; !dup && j < i; ++j) {
        if (0 == columns.at(j).name_.case_compare(columns.at(i).name_)) {
          dup = true;
        }
      }
      if (dup) {
        ++suffix;
        int n = snprintf(buf, sizeof(buf), "%.*s__%ld", MIN(base.length(), OB_MAX_COLUMN_NAME_LENGTH - 8),
                         base.ptr(), suffix);
        if (OB_FAIL(ob_write_string(allocator, ObString(n, buf), columns.at(i).name_))) {
        }
      }
    }
  }
  return ret;
}

static void fs_update_col(ObFileColumnDesc &col, ObFileColType t, bool has_frac, int64_t char_len)
{
  col.type_ = ObFileTypeUtil::widen(static_cast<ObFileColType>(col.type_), t);
  if (has_frac) {
    col.scale_ = 6;
  }
  if (char_len > col.max_length_) {
    col.max_length_ = char_len;
  }
}

static void fs_finish_columns(const ObFileScanOptions &opts, ObIArray<ObFileColumnDesc> &columns)
{
  for (int64_t i = 0; i < columns.count(); ++i) {
    ObFileColumnDesc &col = columns.at(i);
    if (opts.all_varchar_ || FILE_COL_NULL == col.type_) {
      if (FILE_COL_NULL == col.type_) {
        col.nullable_ = true;
      }
      col.type_ = FILE_COL_VARCHAR;
    }
    if (FILE_COL_DATETIME != col.type_) {
      col.scale_ = 0;
    }
    if (col.max_length_ <= 0) {
      col.max_length_ = 1;
    }
  }
}

int ObFileSchemaInferrer::infer(const char *path, const ObFileScanOptions &opts, ObIAllocator &allocator,
                                ObIArray<ObFileColumnDesc> &columns, int64_t &row_count,
                                ObFileIdentity &identity, ObFileErrorInfo &err)
{
  int ret = OB_SUCCESS;
  columns.reset();
  row_count = 0;
  err.reset();
  if (FILE_FORMAT_CSV == opts.format_) {
    ret = infer_csv(path, opts, allocator, columns, row_count, identity, err);
  } else if (FILE_FORMAT_JSON == opts.format_) {
    ret = infer_json(path, opts, allocator, columns, row_count, identity, err);
  } else if (FILE_FORMAT_PARQUET == opts.format_) {
    ObArenaAllocator tmp_alloc("FileSqlInfer");
    ObParquetFileReader reader(tmp_alloc);
    if (OB_FAIL(reader.open(path, NULL, err))) {
      LOG_WARN("failed to open parquet file", K(ret));
    } else if (OB_FAIL(reader.infer_schema(allocator, columns, row_count, identity, err))) {
      LOG_WARN("failed to infer parquet schema", K(ret));
    }
    reader.close();
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  if (OB_SUCC(ret) && OB_FAIL(dedup_names(allocator, columns))) {
    LOG_WARN("failed to dedup names", K(ret));
  }
  if (OB_SUCC(ret) && 0 == columns.count()) {
    ret = OB_INVALID_ARGUMENT;
    snprintf(err.msg_, sizeof(err.msg_), "no columns found in file");
  }
  return ret;
}

int ObFileSchemaInferrer::infer_csv(const char *path, const ObFileScanOptions &opts, ObIAllocator &allocator,
                                    ObIArray<ObFileColumnDesc> &columns, int64_t &row_count,
                                    ObFileIdentity &identity, ObFileErrorInfo &err)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator tmp_alloc("FileSqlInfer");
  ObFileCharReader reader;
  ObCsvRecordReader csv;
  int64_t ncols = -1;
  bool header_done = !opts.header_;
  if (OB_FAIL(reader.open(path, tmp_alloc, ObFileScanOptions::DEFAULT_IO_BUFFER_SIZE, NULL, &identity))) {
    LOG_WARN("failed to open file", K(ret), K(path));
  } else if (OB_FAIL(csv.init(reader, opts, tmp_alloc))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < opts.skip_rows_; ++i) {
    if (OB_FAIL(csv.next_record(err))) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
        break;
      }
    }
  }
  while (OB_SUCC(ret)) {
    if (OB_FAIL(csv.next_record(err))) {
      if (OB_INVALID_ARGUMENT == ret && opts.ignore_errors_ && header_done && ncols > 0) {
        ret = OB_SUCCESS;
        continue;
      }
      break;
    }
    const int64_t cnt = csv.field_count();
    if (!header_done) {
      header_done = true;
      ncols = cnt;
      for (int64_t i = 0; OB_SUCC(ret) && i < cnt; ++i) {
        ObFileColumnDesc col;
        ObString name = csv.field(i);
        int64_t char_cnt = 0;
        if (!ObFileTypeUtil::is_valid_utf8(name.ptr(), name.length(), char_cnt)) {
          ret = OB_INVALID_ARGUMENT;
          err.line_ = csv.record_line();
          snprintf(err.msg_, sizeof(err.msg_), "invalid UTF-8 in header column %ld", i + 1);
        } else if (OB_FAIL(ob_write_string(allocator, name, col.name_))) {
        } else if (OB_FAIL(columns.push_back(col))) {
        }
      }
      continue;
    }
    if (ncols < 0) {
      ncols = cnt;
      for (int64_t i = 0; OB_SUCC(ret) && i < cnt; ++i) {
        ObFileColumnDesc col;
        if (OB_FAIL(columns.push_back(col))) {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (cnt != ncols && !(opts.null_padding_ && cnt < ncols)) {
      if (opts.ignore_errors_) {
        continue;
      }
      ret = OB_INVALID_ARGUMENT;
      err.line_ = csv.record_line();
      snprintf(err.msg_, sizeof(err.msg_), "expected %ld columns but found %ld", ncols, cnt);
    } else {
      bool row_ok = true;
      for (int64_t i = 0; row_ok && i < cnt; ++i) {
        ObString v = csv.field(i);
        int64_t char_cnt = 0;
        if (!ObFileTypeUtil::is_valid_utf8(v.ptr(), v.length(), char_cnt)) {
          row_ok = false;
          if (!opts.ignore_errors_) {
            ret = OB_INVALID_ARGUMENT;
            err.line_ = csv.record_line();
            snprintf(err.msg_, sizeof(err.msg_), "invalid UTF-8 in column %ld", i + 1);
          }
        }
      }
      for (int64_t i = 0; row_ok && i < ncols; ++i) {
        ObFileColumnDesc &col = columns.at(i);
        if (i >= cnt || csv.is_null(i)) {
          col.nullable_ = true;
        } else {
          ObString v = csv.field(i);
          bool has_frac = false;
          int64_t char_cnt = 0;
          (void)ObFileTypeUtil::is_valid_utf8(v.ptr(), v.length(), char_cnt);
          ObFileColType t = (opts.all_varchar_ || FILE_COL_VARCHAR == col.type_)
              ? FILE_COL_VARCHAR : ObFileTypeUtil::classify_text(v.ptr(), v.length(), has_frac);
          fs_update_col(col, t, has_frac, char_cnt);
        }
      }
      if (row_ok) {
        ++row_count;
      }
    }
  }
  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
  }
  if (OB_SUCC(ret)) {
    fs_finish_columns(opts, columns);
  }
  reader.close();
  return ret;
}

static int fs_find_json_col(const ObIArray<ObFileColumnDesc> &columns, const ObString &key, int64_t guess)
{
  int64_t idx = -1;
  if (guess >= 0 && guess < columns.count() && columns.at(guess).name_ == key) {
    idx = guess;
  } else {
    for (int64_t i = 0; idx < 0 && i < columns.count(); ++i) {
      if (columns.at(i).name_ == key) {
        idx = i;
      }
    }
  }
  return static_cast<int>(idx);
}

int ObFileSchemaInferrer::infer_json(const char *path, const ObFileScanOptions &opts, ObIAllocator &allocator,
                                     ObIArray<ObFileColumnDesc> &columns, int64_t &row_count,
                                     ObFileIdentity &identity, ObFileErrorInfo &err)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator tmp_alloc("FileSqlInfer");
  ObFileCharReader reader;
  ObJsonRecordReader json;
  ObSEArray<int64_t, 16> last_seen;
  if (OB_FAIL(reader.open(path, tmp_alloc, ObFileScanOptions::DEFAULT_IO_BUFFER_SIZE, NULL, &identity))) {
    LOG_WARN("failed to open file", K(ret), K(path));
  } else if (OB_FAIL(json.init(reader, opts, tmp_alloc))) {
  }
  while (OB_SUCC(ret)) {
    if (OB_FAIL(json.next_record(err))) {
      break;
    }
    for (int64_t m = 0; OB_SUCC(ret) && m < json.member_count(); ++m) {
      ObString key = json.key(m);
      int64_t idx = fs_find_json_col(columns, key, m);
      if (idx < 0) {
        ObFileColumnDesc col;
        col.nullable_ = (row_count > 0);
        if (columns.count() >= ObFileScanOptions::MAX_COLUMN_COUNT) {
          ret = OB_INVALID_ARGUMENT;
          err.line_ = json.record_line();
          snprintf(err.msg_, sizeof(err.msg_), "too many distinct keys (limit %ld)", ObFileScanOptions::MAX_COLUMN_COUNT);
        } else if (OB_FAIL(ob_write_string(allocator, key, col.name_))) {
        } else if (OB_FAIL(columns.push_back(col))) {
        } else if (OB_FAIL(last_seen.push_back(-1))) {
        } else {
          idx = columns.count() - 1;
        }
      }
      if (OB_SUCC(ret)) {
        ObFileColumnDesc &col = columns.at(idx);
        ObString v = json.value(m);
        last_seen.at(idx) = row_count;
        bool has_frac = false;
        int64_t char_cnt = v.length();
        ObFileColType t = FILE_COL_NULL;
        switch (json.kind(m)) {
          case ObJsonRecordReader::JV_NULL:
            col.nullable_ = true;
            break;
          case ObJsonRecordReader::JV_STRING:
            (void)ObFileTypeUtil::is_valid_utf8(v.ptr(), v.length(), char_cnt);
            t = ObFileTypeUtil::classify_json_string(v.ptr(), v.length(), has_frac);
            break;
          case ObJsonRecordReader::JV_INT:
            t = FILE_COL_BIGINT;
            break;
          case ObJsonRecordReader::JV_DOUBLE:
            t = FILE_COL_DOUBLE;
            break;
          default:
            t = FILE_COL_BOOLEAN;
            break;
        }
        if (FILE_COL_NULL != t) {
          // a JSON string can never share a column with numbers/booleans except as VARCHAR
          const bool cur_is_str = (ObJsonRecordReader::JV_STRING == json.kind(m));
          ObFileColType prev = static_cast<ObFileColType>(col.type_);
          bool prev_is_str = (FILE_COL_DATE == prev || FILE_COL_DATETIME == prev || FILE_COL_VARCHAR == prev);
          if (FILE_COL_NULL != prev && cur_is_str != prev_is_str) {
            t = FILE_COL_VARCHAR;
          }
          fs_update_col(col, t, has_frac, char_cnt);
        }
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
      if (last_seen.at(i) != row_count) {
        columns.at(i).nullable_ = true;
      }
    }
    ++row_count;
  }
  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
  }
  if (OB_SUCC(ret)) {
    fs_finish_columns(opts, columns);
  }
  reader.close();
  return ret;
}

/////////////////////////////// ObFileRowScanner ///////////////////////////////

ObFileRowScanner::ObFileRowScanner(ObIAllocator &allocator)
  : allocator_(allocator), inner_alloc_("FileSqlScan"), opts_(NULL), parquet_(NULL),
    rows_returned_(0), header_skipped_(false), opened_(false)
{}

ObFileRowScanner::~ObFileRowScanner()
{
  close();
}

int ObFileRowScanner::open(const char *path, const ObFileScanOptions &opts,
                           const ObIArray<ObFileColumnDesc> &columns,
                           const ObFileIdentity *expect_identity,
                           const ObIArray<bool> *needed)
{
  int ret = OB_SUCCESS;
  close();
  opts_ = &opts;
  err_.reset();
  rows_returned_ = 0;
  header_skipped_ = false;
  if (OB_FAIL(columns_.assign(columns))) {
  } else if (OB_FAIL(cells_.prepare_allocate(columns.count()))) {
  } else if (OB_FAIL(needed_.prepare_allocate(columns.count()))) {
  } else if (OB_FAIL(json_map_.prepare_allocate(columns.count()))) {
  } else {
    for (int64_t i = 0; i < columns.count(); ++i) {
      needed_.at(i) = (NULL == needed || i >= needed->count()) ? true : needed->at(i);
      cells_.at(i) = ObFileCell();
    }
  }
  if (OB_FAIL(ret)) {
  } else if (FILE_FORMAT_PARQUET == opts.format_) {
    void *buf = inner_alloc_.alloc(sizeof(ObParquetFileReader));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      parquet_ = new (buf) ObParquetFileReader(inner_alloc_);
      if (OB_FAIL(parquet_->open(path, expect_identity, err_))) {
        LOG_WARN("failed to open parquet", K(ret));
      } else if (OB_FAIL(parquet_->start_scan(columns_, needed_, err_))) {
        LOG_WARN("failed to start parquet scan", K(ret));
      }
    }
  } else if (OB_FAIL(reader_.open(path, inner_alloc_, ObFileScanOptions::DEFAULT_IO_BUFFER_SIZE,
                                  expect_identity, NULL))) {
    LOG_WARN("failed to open file", K(ret), K(path));
  } else if (FILE_FORMAT_CSV == opts.format_) {
    ret = csv_.init(reader_, opts, inner_alloc_);
  } else if (FILE_FORMAT_JSON == opts.format_) {
    ret = json_.init(reader_, opts, inner_alloc_);
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  if (OB_FILE_ALREADY_EXIST == ret) {
    ret = OB_INVALID_ARGUMENT;
    snprintf(err_.msg_, sizeof(err_.msg_), "file changed after its schema was inferred, please rerun the query");
  }
  opened_ = OB_SUCC(ret);
  return ret;
}

int ObFileRowScanner::rescan()
{
  int ret = OB_SUCCESS;
  err_.reset();
  rows_returned_ = 0;
  header_skipped_ = false;
  if (!opened_) {
    ret = OB_NOT_INIT;
  } else if (NULL != parquet_) {
    ret = parquet_->rescan(err_);
  } else if (OB_FAIL(reader_.rewind())) {
  } else if (FILE_FORMAT_CSV == opts_->format_) {
    csv_.reset_position();
  } else {
    json_.reset_position();
  }
  return ret;
}

void ObFileRowScanner::close()
{
  if (NULL != parquet_) {
    parquet_->close();
    parquet_->~ObParquetFileReader();
    parquet_ = NULL;
  }
  reader_.close();
  opened_ = false;
  inner_alloc_.reset();
}

int ObFileRowScanner::get_next_row()
{
  int ret = OB_SUCCESS;
  if (!opened_) {
    ret = OB_NOT_INIT;
  } else if (NULL != parquet_) {
    ret = parquet_->next_row(cells_, err_);
  } else if (FILE_FORMAT_CSV == opts_->format_) {
    ret = next_csv_row();
  } else {
    ret = next_json_row();
  }
  if (OB_SUCC(ret)) {
    ++rows_returned_;
  }
  return ret;
}

int ObFileRowScanner::convert_text(int64_t col, const ObString &text, bool is_json_string, ObFileCell &cell)
{
  int ret = OB_SUCCESS;
  bool ok = true;
  bool overflow = false;
  bool has_frac = false;
  bool bv = false;
  int32_t days = 0;
  cell.is_null_ = false;
  switch (columns_.at(col).type_) {
    case FILE_COL_BOOLEAN:
      ok = !is_json_string && ObFileTypeUtil::parse_bool(text.ptr(), text.length(), bv);
      cell.int_ = bv ? 1 : 0;
      break;
    case FILE_COL_BIGINT:
      ok = !is_json_string && ObFileTypeUtil::parse_int64(text.ptr(), text.length(), cell.int_, overflow);
      break;
    case FILE_COL_DOUBLE:
      ok = !is_json_string && ObFileTypeUtil::parse_double(text.ptr(), text.length(), cell.double_);
      break;
    case FILE_COL_DATE:
      ok = ObFileTypeUtil::parse_date(text.ptr(), text.length(), days);
      cell.int_ = days;
      break;
    case FILE_COL_DATETIME:
      ok = ObFileTypeUtil::parse_datetime(text.ptr(), text.length(), cell.int_, has_frac, true);
      break;
    default:
      cell.str_ = text;
      break;
  }
  if (!ok) {
    ret = OB_INVALID_ARGUMENT;
    snprintf(err_.msg_, sizeof(err_.msg_),
             "cannot convert value '%.*s' of column '%.*s' to %s (file changed after schema inference?)",
             static_cast<int>(MIN(text.length(), 64)), text.ptr(),
             columns_.at(col).name_.length(), columns_.at(col).name_.ptr(),
             ObFileTypeUtil::type_name(columns_.at(col).type_));
  }
  return ret;
}

int ObFileRowScanner::next_csv_row()
{
  int ret = OB_SUCCESS;
  if (!header_skipped_) {
    header_skipped_ = true;
    for (int64_t i = 0; OB_SUCC(ret) && i < opts_->skip_rows_; ++i) {
      ret = csv_.next_record(err_);
    }
    if (OB_SUCC(ret) && opts_->header_) {
      ret = csv_.next_record(err_);
    }
  }
  bool got = false;
  while (OB_SUCC(ret) && !got) {
    if (OB_FAIL(csv_.next_record(err_))) {
      if (OB_INVALID_ARGUMENT == ret && opts_->ignore_errors_) {
        ret = OB_SUCCESS;
      }
    } else {
      const int64_t cnt = csv_.field_count();
      const int64_t ncols = columns_.count();
      if (cnt != ncols && !(opts_->null_padding_ && cnt < ncols)) {
        if (!opts_->ignore_errors_) {
          ret = OB_INVALID_ARGUMENT;
          err_.line_ = csv_.record_line();
          snprintf(err_.msg_, sizeof(err_.msg_), "expected %ld columns but found %ld", ncols, cnt);
        }
      } else {
        bool row_ok = true;
        if (opts_->ignore_errors_) {
          for (int64_t i = 0; row_ok && i < cnt; ++i) {
            ObString v = csv_.field(i);
            int64_t char_cnt = 0;
            row_ok = ObFileTypeUtil::is_valid_utf8(v.ptr(), v.length(), char_cnt);
          }
        }
        for (int64_t i = 0; OB_SUCC(ret) && row_ok && i < ncols; ++i) {
          ObFileCell &cell = cells_.at(i);
          if (!needed_.at(i)) {
          } else if (i >= cnt || csv_.is_null(i)) {
            cell.is_null_ = true;
          } else if (OB_FAIL(convert_text(i, csv_.field(i), false, cell))) {
            err_.line_ = csv_.record_line();
          }
        }
        got = row_ok;
      }
    }
  }
  return ret;
}

int ObFileRowScanner::next_json_row()
{
  int ret = OB_SUCCESS;
  static const ObString TRUE_STR("true");
  static const ObString FALSE_STR("false");
  if (OB_FAIL(json_.next_record(err_))) {
  } else {
    for (int64_t i = 0; i < cells_.count(); ++i) {
      cells_.at(i).is_null_ = true;
    }
    for (int64_t m = 0; OB_SUCC(ret) && m < json_.member_count(); ++m) {
      ObString key = json_.key(m);
      int64_t idx = fs_find_json_col(columns_, key, m);
      if (idx < 0) {
        ret = OB_INVALID_ARGUMENT;
        err_.line_ = json_.record_line();
        snprintf(err_.msg_, sizeof(err_.msg_), "unknown key \"%.*s\" (file changed after schema inference?)",
                 static_cast<int>(MIN(key.length(), 64)), key.ptr());
      } else if (!needed_.at(idx)) {
      } else {
        ObFileCell &cell = cells_.at(idx);
        const int8_t kind = json_.kind(m);
        const bool is_varchar = (FILE_COL_VARCHAR == columns_.at(idx).type_);
        if (ObJsonRecordReader::JV_NULL == kind) {
          cell.is_null_ = true;
        } else if (ObJsonRecordReader::JV_TRUE == kind || ObJsonRecordReader::JV_FALSE == kind) {
          cell.is_null_ = false;
          if (is_varchar) {
            cell.str_ = (ObJsonRecordReader::JV_TRUE == kind) ? TRUE_STR : FALSE_STR;
          } else if (FILE_COL_BOOLEAN == columns_.at(idx).type_) {
            cell.int_ = (ObJsonRecordReader::JV_TRUE == kind) ? 1 : 0;
          } else {
            ret = convert_text(idx, json_.value(m), false, cell);
          }
        } else if (OB_FAIL(convert_text(idx, json_.value(m), ObJsonRecordReader::JV_STRING == kind, cell))) {
        }
        if (OB_FAIL(ret)) {
          err_.line_ = json_.record_line();
        }
      }
    }
  }
  return ret;
}

int ObFileTableInfo::deep_copy(const ObFileTableInfo &other, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  columns_.reset();
  if (OB_FAIL(ob_write_string(allocator, other.path_, path_, true /*c_style*/))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.func_name_, func_name_))) {
  } else if (OB_FAIL(options_.deep_copy(other.options_, allocator))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < other.columns_.count(); ++i) {
      ObFileColumnDesc col;
      if (OB_FAIL(col.deep_copy(other.columns_.at(i), allocator))) {
      } else if (OB_FAIL(columns_.push_back(col))) {
      }
    }
    identity_ = other.identity_;
    row_count_ = other.row_count_;
  }
  return ret;
}

int ObFileSqlErrorReporter::report(int ret, const ObString &func, const ObString &path,
                                   const ObFileErrorInfo &err)
{
  char buf[1024];
  const char *msg = ('\0' == err.msg_[0]) ? "malformed input" : err.msg_;
  if (OB_INVALID_ARGUMENT == ret || OB_INVALID_DATA == ret) {
    ret = OB_INVALID_ARGUMENT;
    if (err.line_ > 0) {
      snprintf(buf, sizeof(buf), "%.*s: %.*s line %ld: %s", func.length(), func.ptr(),
               path.length(), path.ptr(), err.line_, msg);
    } else {
      snprintf(buf, sizeof(buf), "%.*s: %.*s: %s", func.length(), func.ptr(),
               path.length(), path.ptr(), msg);
    }
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, buf);
  } else if (OB_NOT_SUPPORTED == ret && '\0' != err.msg_[0]) {
    snprintf(buf, sizeof(buf), "%.*s: %s", func.length(), func.ptr(), err.msg_);
    LOG_USER_ERROR(OB_NOT_SUPPORTED, buf);
  } else if (OB_FILE_NOT_EXIST == ret) {
    LOG_USER_ERROR(OB_FILE_NOT_EXIST);
  }
  return ret;
}

/* ------------------------------------------------------------------ */
/*  COPY TO: option encoding and CSV writer                            */
/* ------------------------------------------------------------------ */
const char *const ObFileCopyOptions::MAGIC = "OBFILECOPY1;";

void ObFileCopyOptions::reset()
{
  header_ = true;
  delim_ = ',';
  quote_ = '"';
  escape_ = '"';
  null_str_.reset();
  col_names_.reset();
}

static int fcopy_append_hex(ObSqlString &out, const ObString &v)
{
  int ret = OB_SUCCESS;
  static const char *HEX = "0123456789abcdef";
  for (int64_t i = 0; OB_SUCC(ret) && i < v.length(); ++i) {
    unsigned char c = static_cast<unsigned char>(v.ptr()[i]);
    char two[2] = { HEX[c >> 4], HEX[c & 0xf] };
    ret = out.append(two, 2);
  }
  return ret;
}

static int fcopy_hex_val(char c)
{
  int v = -1;
  if (c >= '0' && c <= '9') {
    v = c - '0';
  } else if (c >= 'a' && c <= 'f') {
    v = c - 'a' + 10;
  } else if (c >= 'A' && c <= 'F') {
    v = c - 'A' + 10;
  }
  return v;
}

static int fcopy_decode_hex(const char *p, int64_t len, ObIAllocator &allocator, ObString &out)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  out.reset();
  if (len % 2 != 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (0 == len) {
    // empty
  } else if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(len / 2)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < len; i += 2) {
      int hi = fcopy_hex_val(p[i]);
      int lo = fcopy_hex_val(p[i + 1]);
      if (hi < 0 || lo < 0) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        buf[i / 2] = static_cast<char>((hi << 4) | lo);
      }
    }
    if (OB_SUCC(ret)) {
      out.assign_ptr(buf, static_cast<int32_t>(len / 2));
    }
  }
  return ret;
}

bool ObFileCopyOptions::is_copy_options(const ObString &in)
{
  const int64_t mlen = STRLEN(MAGIC);
  return in.length() >= mlen && 0 == MEMCMP(in.ptr(), MAGIC, mlen);
}

int ObFileCopyOptions::encode(ObIAllocator &allocator, ObString &out) const
{
  int ret = OB_SUCCESS;
  ObSqlString buf;
  char d = delim_, q = quote_, e = escape_;
  if (OB_FAIL(buf.append(MAGIC))) {
  } else if (OB_FAIL(buf.append_fmt("h=%d;d=", header_ ? 1 : 0))) {
  } else if (OB_FAIL(fcopy_append_hex(buf, ObString(1, &d)))) {
  } else if (OB_FAIL(buf.append(";q="))) {
  } else if (OB_FAIL(fcopy_append_hex(buf, ObString(1, &q)))) {
  } else if (OB_FAIL(buf.append(";e="))) {
  } else if (OB_FAIL(fcopy_append_hex(buf, ObString(1, &e)))) {
  } else if (OB_FAIL(buf.append(";n="))) {
  } else if (OB_FAIL(fcopy_append_hex(buf, null_str_))) {
  } else if (OB_FAIL(buf.append(";c="))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < col_names_.count(); ++i) {
    if (i > 0 && OB_FAIL(buf.append(","))) {
    } else if (OB_FAIL(fcopy_append_hex(buf, col_names_.at(i)))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(buf.append(";"))) {
  } else if (OB_SUCC(ret) && OB_FAIL(ob_write_string(allocator, buf.string(), out))) {
  }
  return ret;
}

int ObFileCopyOptions::decode(const ObString &in, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  reset();
  if (!is_copy_options(in)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const char *p = in.ptr() + STRLEN(MAGIC);
    const char *end = in.ptr() + in.length();
    while (OB_SUCC(ret) && p < end) {
      const char *semi = static_cast<const char *>(memchr(p, ';', end - p));
      if (NULL == semi || semi - p < 2 || '=' != p[1]) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const char key = p[0];
        const char *v = p + 2;
        const int64_t vlen = semi - v;
        ObString tmp;
        if ('h' == key) {
          header_ = (1 == vlen && '1' == v[0]);
        } else if ('d' == key || 'q' == key || 'e' == key) {
          if (OB_FAIL(fcopy_decode_hex(v, vlen, allocator, tmp))) {
          } else if (1 != tmp.length()) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            ('d' == key ? delim_ : ('q' == key ? quote_ : escape_)) = tmp.ptr()[0];
          }
        } else if ('n' == key) {
          ret = fcopy_decode_hex(v, vlen, allocator, null_str_);
        } else if ('c' == key) {
          const char *c = v;
          while (OB_SUCC(ret) && c < semi) {
            const char *comma = static_cast<const char *>(memchr(c, ',', semi - c));
            const char *cend = (NULL == comma) ? semi : comma;
            if (OB_FAIL(fcopy_decode_hex(c, cend - c, allocator, tmp))) {
            } else if (OB_FAIL(col_names_.push_back(tmp))) {
            }
            c = cend + 1;
          }
        } else {
          ret = OB_INVALID_ARGUMENT;
        }
        p = semi + 1;
      }
    }
  }
  return ret;
}

ObFileCsvWriter::ObFileCsvWriter()
  : fd_(-1), opts_(NULL), buf_(NULL), buf_pos_(0), buf_cap_(0), field_idx_(0), rows_(0)
{
  target_[0] = '\0';
  tmp_path_[0] = '\0';
}

ObFileCsvWriter::~ObFileCsvWriter()
{
  abort();
}

int ObFileCsvWriter::open(const char *target, const ObFileCopyOptions &opts, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  struct stat st;
  static int64_t seq = 0;
  const int64_t cap = 256L << 10;
  if (OB_ISNULL(target) || '/' != target[0] || STRLEN(target) + 64 > PATH_MAX) {
    ret = OB_INVALID_ARGUMENT;
  } else if (0 == ::lstat(target, &st)) {
    ret = OB_FILE_ALREADY_EXIST;
    LOG_WARN("copy target already exists", K(ret), K(target));
  } else if (OB_ISNULL(buf_ = static_cast<char *>(allocator.alloc(cap)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    buf_cap_ = cap;
    buf_pos_ = 0;
    rows_ = 0;
    opts_ = &opts;
    snprintf(target_, sizeof(target_), "%s", target);
    for (int64_t retry = 0; OB_SUCC(ret) && fd_ < 0 && retry < 16; ++retry) {
      snprintf(tmp_path_, sizeof(tmp_path_), "%s.tmp.%d.%ld", target, static_cast<int>(getpid()),
               ATOMIC_AAF(&seq, 1));
      fd_ = ::open(tmp_path_, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
      if (fd_ < 0 && EEXIST != errno) {
        ret = (EACCES == errno || EPERM == errno) ? OB_ERR_NO_PRIVILEGE : OB_IO_ERROR;
        LOG_WARN("failed to create copy temp file", K(ret), K(errno), K(tmp_path_));
      }
    }
    if (OB_SUCC(ret) && fd_ < 0) {
      ret = OB_IO_ERROR;
    }
    if (OB_FAIL(ret)) {
      tmp_path_[0] = '\0';
    }
  }
  return ret;
}

bool ObFileCsvWriter::need_quote(const ObString &v, const ObFileCopyOptions &opts)
{
  bool need = false;
  if (0 == v.length()) {
    // an unquoted empty field reads back as NULL, so an empty string is always quoted
    need = true;
  } else if (v == opts.null_str_) {
    need = true;
  } else {
    for (int64_t i = 0; !need && i < v.length(); ++i) {
      const char c = v.ptr()[i];
      need = (c == opts.delim_ || c == opts.quote_ || c == '\n' || c == '\r'
              || (c == opts.escape_ && opts.escape_ != opts.quote_));
    }
  }
  return need;
}

int ObFileCsvWriter::flush()
{
  int ret = OB_SUCCESS;
  int64_t off = 0;
  while (OB_SUCC(ret) && off < buf_pos_) {
    ssize_t n = ::write(fd_, buf_ + off, buf_pos_ - off);
    if (n < 0) {
      if (EINTR != errno) {
        ret = OB_IO_ERROR;
        LOG_WARN("failed to write copy file", K(ret), K(errno));
      }
    } else {
      off += n;
    }
  }
  buf_pos_ = 0;
  return ret;
}

int ObFileCsvWriter::append(const char *p, int64_t n)
{
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret) && n > 0) {
    if (buf_pos_ == buf_cap_ && OB_FAIL(flush())) {
    } else {
      const int64_t c = MIN(n, buf_cap_ - buf_pos_);
      MEMCPY(buf_ + buf_pos_, p, c);
      buf_pos_ += c;
      p += c;
      n -= c;
    }
  }
  return ret;
}

int ObFileCsvWriter::write_field(const ObString &value, bool is_null)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(fd_ < 0) || OB_ISNULL(opts_)) {
    ret = OB_NOT_INIT;
  } else if (field_idx_ > 0 && OB_FAIL(append_char(opts_->delim_))) {
  } else if (is_null) {
    ret = append(opts_->null_str_.ptr(), opts_->null_str_.length());
  } else if (!need_quote(value, *opts_)) {
    ret = append(value.ptr(), value.length());
  } else if (OB_FAIL(append_char(opts_->quote_))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
      const char c = value.ptr()[i];
      if (c == opts_->quote_ || (c == opts_->escape_ && opts_->escape_ != opts_->quote_)) {
        // doubled quote ("" when escape == quote) or escape char prefix
        ret = append_char(opts_->escape_);
      }
      if (OB_SUCC(ret)) {
        ret = append_char(c);
      }
    }
    if (OB_SUCC(ret)) {
      ret = append_char(opts_->quote_);
    }
  }
  if (OB_SUCC(ret)) {
    ++field_idx_;
  }
  return ret;
}

int ObFileCsvWriter::end_row()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(append_char('\n'))) {
  } else {
    ++rows_;
    field_idx_ = 0;
  }
  return ret;
}

int ObFileCsvWriter::write_header()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(opts_)) {
    ret = OB_NOT_INIT;
  } else if (opts_->header_ && opts_->col_names_.count() > 0) {
    field_idx_ = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < opts_->col_names_.count(); ++i) {
      ret = write_field(opts_->col_names_.at(i), false);
    }
    if (OB_SUCC(ret) && OB_FAIL(append_char('\n'))) {
    }
    field_idx_ = 0;
  }
  return ret;
}

int ObFileCsvWriter::commit()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(fd_ < 0)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(flush())) {
  } else if (0 != ::fsync(fd_)) {
    ret = OB_IO_ERROR;
    LOG_WARN("failed to fsync copy file", K(ret), K(errno));
  } else {
    ::close(fd_);
    fd_ = -1;
    // link(2) fails with EEXIST instead of replacing: never overwrite a concurrently created target
    if (0 != ::link(tmp_path_, target_)) {
      ret = (EEXIST == errno) ? OB_FILE_ALREADY_EXIST : OB_IO_ERROR;
      LOG_WARN("failed to publish copy file", K(ret), K(errno), K(target_));
    }
    ::unlink(tmp_path_);
    tmp_path_[0] = '\0';
  }
  if (OB_FAIL(ret)) {
    abort();
  }
  return ret;
}

void ObFileCsvWriter::abort()
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if ('\0' != tmp_path_[0]) {
    ::unlink(tmp_path_);
    tmp_path_[0] = '\0';
  }
}

} // namespace sql
} // namespace oceanbase
