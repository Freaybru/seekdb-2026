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
#include "sql/engine/basic/ob_file_parquet_reader.h"
#include "sql/engine/cmd/ob_load_data_file_reader.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <math.h>

namespace oceanbase
{
using namespace common;
namespace sql
{

namespace pq
{
enum PhysicalType { BOOLEAN = 0, INT32 = 1, INT64 = 2, INT96 = 3, FLOAT = 4, DOUBLE = 5,
                    BYTE_ARRAY = 6, FIXED_LEN_BYTE_ARRAY = 7 };
enum Codec { UNCOMPRESSED = 0, SNAPPY = 1, GZIP = 2, LZO = 3, BROTLI = 4, LZ4 = 5, ZSTD = 6, LZ4_RAW = 7 };
enum Encoding { PLAIN = 0, PLAIN_DICTIONARY = 2, RLE = 3, BIT_PACKED = 4, DELTA_BINARY_PACKED = 5,
                DELTA_LENGTH_BYTE_ARRAY = 6, DELTA_BYTE_ARRAY = 7, RLE_DICTIONARY = 8,
                BYTE_STREAM_SPLIT = 9 };
enum PageType { DATA_PAGE = 0, INDEX_PAGE = 1, DICTIONARY_PAGE = 2, DATA_PAGE_V2 = 3 };
enum TType { T_STOP = 0, T_TRUE = 1, T_FALSE = 2, T_BYTE = 3, T_I16 = 4, T_I32 = 5, T_I64 = 6,
             T_DOUBLE = 7, T_BINARY = 8, T_LIST = 9, T_SET = 10, T_MAP = 11, T_STRUCT = 12 };

static inline uint32_t le32(const char *p)
{
  const uint8_t *u = reinterpret_cast<const uint8_t *>(p);
  return static_cast<uint32_t>(u[0]) | (static_cast<uint32_t>(u[1]) << 8)
         | (static_cast<uint32_t>(u[2]) << 16) | (static_cast<uint32_t>(u[3]) << 24);
}
static inline uint64_t le64(const char *p)
{
  return static_cast<uint64_t>(le32(p)) | (static_cast<uint64_t>(le32(p + 4)) << 32);
}

// Thrift compact protocol reader (read only, bounds checked)
class ThriftReader
{
public:
  ThriftReader(const char *p, int64_t len)
    : p_(reinterpret_cast<const uint8_t *>(p)), begin_(p_), end_(p_ + len), ok_(true) {}
  bool ok() const { return ok_; }
  int64_t consumed() const { return p_ - begin_; }
  bool varint(uint64_t &v)
  {
    v = 0;
    int shift = 0;
    while (ok_) {
      if (p_ >= end_ || shift > 63) {
        ok_ = false;
      } else {
        uint8_t b = *p_++;
        v |= static_cast<uint64_t>(b & 0x7F) << shift;
        shift += 7;
        if (0 == (b & 0x80)) {
          break;
        }
      }
    }
    return ok_;
  }
  int64_t zigzag()
  {
    uint64_t v = 0;
    varint(v);
    return static_cast<int64_t>((v >> 1) ^ (~(v & 1) + 1));
  }
  uint8_t byte()
  {
    uint8_t b = 0;
    if (p_ >= end_) {
      ok_ = false;
    } else {
      b = *p_++;
    }
    return b;
  }
  // returns false at STOP
  bool field(int16_t &last, int16_t &fid, uint8_t &type)
  {
    bool has = false;
    uint8_t b = byte();
    if (ok_ && 0 != b) {
      type = b & 0x0F;
      uint8_t delta = b >> 4;
      if (0 != delta) {
        fid = static_cast<int16_t>(last + delta);
      } else {
        fid = static_cast<int16_t>(zigzag());
      }
      last = fid;
      has = ok_;
    }
    return has;
  }
  bool binary(ObString &s)
  {
    uint64_t len = 0;
    if (varint(len)) {
      if (len > static_cast<uint64_t>(end_ - p_)) {
        ok_ = false;
      } else {
        s.assign_ptr(reinterpret_cast<const char *>(p_), static_cast<int32_t>(len));
        p_ += len;
      }
    }
    return ok_;
  }
  bool list_header(uint8_t &elem_type, int64_t &size)
  {
    uint8_t b = byte();
    elem_type = b & 0x0F;
    size = b >> 4;
    if (ok_ && 15 == size) {
      uint64_t v = 0;
      varint(v);
      size = static_cast<int64_t>(v);
      if (size < 0 || size > (end_ - p_)) {
        ok_ = false;
      }
    }
    return ok_;
  }
  bool skip(uint8_t type, int depth = 0, bool in_list = false)
  {
    if (depth > 64) {
      ok_ = false;
    } else {
      switch (type) {
        case T_TRUE:
        case T_FALSE:
          if (in_list) { byte(); }
          break;
        case T_BYTE: byte(); break;
        case T_I16:
        case T_I32:
        case T_I64: { uint64_t v; varint(v); break; }
        case T_DOUBLE:
          if (end_ - p_ < 8) { ok_ = false; } else { p_ += 8; }
          break;
        case T_BINARY: { ObString s; binary(s); break; }
        case T_LIST:
        case T_SET: {
          uint8_t et = 0;
          int64_t n = 0;
          if (list_header(et, n)) {
            for (int64_t i = 0; ok_ && i < n; ++i) { skip(et, depth + 1, true); }
          }
          break;
        }
        case T_MAP: {
          uint64_t n = 0;
          if (varint(n) && n > 0) {
            uint8_t kv = byte();
            for (uint64_t i = 0; ok_ && i < n; ++i) {
              skip(kv >> 4, depth + 1, true);
              skip(kv & 0x0F, depth + 1, true);
            }
          }
          break;
        }
        case T_STRUCT: {
          int16_t last = 0, fid = 0;
          uint8_t t = 0;
          while (ok_ && field(last, fid, t)) { skip(t, depth + 1); }
          break;
        }
        default:
          ok_ = false;
          break;
      }
    }
    return ok_;
  }
  int64_t i_val() { return zigzag(); }
  bool bool_val(uint8_t type) { return T_TRUE == type; }
private:
  const uint8_t *p_;
  const uint8_t *begin_;
  const uint8_t *end_;
  bool ok_;
};

struct PageHeader
{
  int32_t type_;
  int32_t uncompressed_size_;
  int32_t compressed_size_;
  int32_t num_values_;
  int32_t encoding_;
  int32_t def_len_;
  int32_t rep_len_;
  bool is_compressed_;
  PageHeader() : type_(-1), uncompressed_size_(0), compressed_size_(0), num_values_(0), encoding_(0),
                 def_len_(0), rep_len_(0), is_compressed_(true) {}
};

static void parse_data_page_header(ThriftReader &r, PageHeader &h, bool v2)
{
  int16_t last = 0, fid = 0;
  uint8_t t = 0;
  while (r.ok() && r.field(last, fid, t)) {
    if (1 == fid && T_I32 == t) {
      h.num_values_ = static_cast<int32_t>(r.i_val());
    } else if (!v2 && 2 == fid && T_I32 == t) {
      h.encoding_ = static_cast<int32_t>(r.i_val());
    } else if (v2 && 4 == fid && T_I32 == t) {
      h.encoding_ = static_cast<int32_t>(r.i_val());
    } else if (v2 && 5 == fid && T_I32 == t) {
      h.def_len_ = static_cast<int32_t>(r.i_val());
    } else if (v2 && 6 == fid && T_I32 == t) {
      h.rep_len_ = static_cast<int32_t>(r.i_val());
    } else if (v2 && 7 == fid && (T_TRUE == t || T_FALSE == t)) {
      h.is_compressed_ = r.bool_val(t);
    } else {
      r.skip(t);
    }
  }
}

static void parse_page_header(ThriftReader &r, PageHeader &h)
{
  int16_t last = 0, fid = 0;
  uint8_t t = 0;
  while (r.ok() && r.field(last, fid, t)) {
    if (1 == fid && T_I32 == t) {
      h.type_ = static_cast<int32_t>(r.i_val());
    } else if (2 == fid && T_I32 == t) {
      h.uncompressed_size_ = static_cast<int32_t>(r.i_val());
    } else if (3 == fid && T_I32 == t) {
      h.compressed_size_ = static_cast<int32_t>(r.i_val());
    } else if (5 == fid && T_STRUCT == t) {
      parse_data_page_header(r, h, false);
    } else if (7 == fid && T_STRUCT == t) {
      parse_data_page_header(r, h, false);   // dictionary header: 1 num_values, 2 encoding
    } else if (8 == fid && T_STRUCT == t) {
      parse_data_page_header(r, h, true);
    } else {
      r.skip(t);
    }
  }
}

static void parse_logical_type(ThriftReader &r, ObParquetLeafMeta &leaf)
{
  int16_t last = 0, fid = 0;
  uint8_t t = 0;
  while (r.ok() && r.field(last, fid, t)) {
    if (T_STRUCT != t) {
      r.skip(t);
      continue;
    }
    leaf.logical_kind_ = fid;
    int16_t l2 = 0, f2 = 0;
    uint8_t t2 = 0;
    while (r.ok() && r.field(l2, f2, t2)) {
      if (5 == fid && 1 == f2 && T_I32 == t2) {
        leaf.scale_ = static_cast<int32_t>(r.i_val());
      } else if (5 == fid && 2 == f2 && T_I32 == t2) {
        leaf.precision_ = static_cast<int32_t>(r.i_val());
      } else if ((8 == fid || 7 == fid) && 2 == f2 && T_STRUCT == t2) {
        int16_t l3 = 0, f3 = 0;
        uint8_t t3 = 0;
        while (r.ok() && r.field(l3, f3, t3)) {
          leaf.ts_unit_ = f3;
          r.skip(t3);
        }
      } else if (10 == fid && 1 == f2 && T_BYTE == t2) {
        leaf.int_bits_ = static_cast<int8_t>(r.byte());
      } else if (10 == fid && 2 == f2 && (T_TRUE == t2 || T_FALSE == t2)) {
        leaf.int_signed_ = r.bool_val(t2);
      } else {
        r.skip(t2);
      }
    }
  }
}

} // namespace pq

using namespace pq;

/////////////////////////////// snappy ///////////////////////////////

int ObSnappyUtil::get_uncompressed_length(const char *src, int64_t src_len, int64_t &len)
{
  int ret = OB_SUCCESS;
  uint64_t v = 0;
  int shift = 0;
  int64_t i = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    if (i >= src_len || shift > 32) {
      ret = OB_INVALID_DATA;
    } else {
      uint8_t b = static_cast<uint8_t>(src[i++]);
      v |= static_cast<uint64_t>(b & 0x7F) << shift;
      shift += 7;
      done = (0 == (b & 0x80));
    }
  }
  if (OB_SUCC(ret)) {
    len = static_cast<int64_t>(v);
  }
  return ret;
}

int ObSnappyUtil::decompress(const char *src, int64_t src_len, char *dst, int64_t dst_len)
{
  int ret = OB_SUCCESS;
  const uint8_t *p = reinterpret_cast<const uint8_t *>(src);
  const uint8_t *end = p + src_len;
  int64_t expect = 0;
  // skip the uncompressed length preamble
  while (p < end && (*p & 0x80)) { ++p; }
  ++p;
  if (OB_FAIL(get_uncompressed_length(src, src_len, expect))) {
  } else if (expect != dst_len || p > end) {
    ret = OB_INVALID_DATA;
  }
  int64_t op = 0;
  while (OB_SUCC(ret) && p < end) {
    const uint8_t tag = *p++;
    const uint8_t kind = tag & 0x03;
    if (0 == kind) {
      int64_t len = (tag >> 2) + 1;
      if (len > 60) {
        const int64_t extra = len - 60;
        if (end - p < extra) {
          ret = OB_INVALID_DATA;
        } else {
          len = 0;
          for (int64_t k = 0; k < extra; ++k) {
            len |= static_cast<int64_t>(p[k]) << (8 * k);
          }
          len += 1;
          p += extra;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (end - p < len || dst_len - op < len) {
        ret = OB_INVALID_DATA;
      } else {
        MEMCPY(dst + op, p, len);
        p += len;
        op += len;
      }
    } else {
      int64_t len = 0;
      int64_t offset = 0;
      if (1 == kind) {
        if (end - p < 1) {
          ret = OB_INVALID_DATA;
        } else {
          len = 4 + ((tag >> 2) & 0x07);
          offset = (static_cast<int64_t>(tag >> 5) << 8) | *p++;
        }
      } else if (2 == kind) {
        if (end - p < 2) {
          ret = OB_INVALID_DATA;
        } else {
          len = (tag >> 2) + 1;
          offset = p[0] | (static_cast<int64_t>(p[1]) << 8);
          p += 2;
        }
      } else {
        if (end - p < 4) {
          ret = OB_INVALID_DATA;
        } else {
          len = (tag >> 2) + 1;
          offset = static_cast<int64_t>(le32(reinterpret_cast<const char *>(p)));
          p += 4;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (offset <= 0 || offset > op || dst_len - op < len) {
        ret = OB_INVALID_DATA;
      } else {
        // byte-by-byte copy handles overlapping runs
        for (int64_t k = 0; k < len; ++k) {
          dst[op + k] = dst[op - offset + k];
        }
        op += len;
      }
    }
  }
  if (OB_SUCC(ret) && op != dst_len) {
    ret = OB_INVALID_DATA;
  }
  return ret;
}

/////////////////////////////// RLE / bit-packed hybrid ///////////////////////////////

void ObParquetRleDecoder::init(const uint8_t *data, int64_t len, int bit_width)
{
  data_ = data;
  end_ = data + (len > 0 ? len : 0);
  bit_width_ = bit_width;
  rle_left_ = 0;
  bp_left_ = 0;
  bp_pos_ = NULL;
  bp_bit_ = 0;
}

int ObParquetRleDecoder::read_header()
{
  int ret = OB_SUCCESS;
  uint64_t h = 0;
  int shift = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    if (data_ >= end_ || shift > 56) {
      ret = OB_INVALID_DATA;
    } else {
      uint8_t b = *data_++;
      h |= static_cast<uint64_t>(b & 0x7F) << shift;
      shift += 7;
      done = (0 == (b & 0x80));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (h & 1) {
    const int64_t groups = static_cast<int64_t>(h >> 1);
    const int64_t bytes = groups * bit_width_;
    bp_left_ = groups * 8;
    bp_pos_ = data_;
    bp_bit_ = 0;
    // a trailing partial group may be truncated by writers; clamp to available bytes
    if (bytes > end_ - data_) {
      if (bit_width_ > 0) {
        bp_left_ = MIN(bp_left_, ((end_ - data_) * 8) / bit_width_);
      }
      data_ = end_;
    } else {
      data_ += bytes;
    }
    if (bp_left_ <= 0) {
      ret = OB_INVALID_DATA;
    }
  } else {
    rle_left_ = static_cast<int64_t>(h >> 1);
    const int nbytes = (bit_width_ + 7) / 8;
    rle_value_ = 0;
    if (end_ - data_ < nbytes) {
      ret = OB_INVALID_DATA;
    } else {
      for (int k = 0; k < nbytes; ++k) {
        rle_value_ |= static_cast<uint32_t>(data_[k]) << (8 * k);
      }
      data_ += nbytes;
    }
    if (OB_SUCC(ret) && rle_left_ <= 0) {
      ret = OB_INVALID_DATA;
    }
  }
  return ret;
}

int ObParquetRleDecoder::next(uint32_t &value)
{
  int ret = OB_SUCCESS;
  if (0 == bit_width_) {
    value = 0;
  } else if (rle_left_ <= 0 && bp_left_ <= 0 && OB_FAIL(read_header())) {
  } else if (rle_left_ > 0) {
    --rle_left_;
    value = rle_value_;
  } else {
    uint32_t v = 0;
    for (int b = 0; b < bit_width_; ++b) {
      const int64_t bit = bp_bit_ + b;
      v |= static_cast<uint32_t>((bp_pos_[bit >> 3] >> (bit & 7)) & 1) << b;
    }
    bp_bit_ += bit_width_;
    --bp_left_;
    value = v;
  }
  return ret;
}

/////////////////////////////// DELTA_BINARY_PACKED ///////////////////////////////

void ObParquetDeltaDecoder::reset()
{
  data_ = end_ = NULL;
  block_size_ = mini_blocks_ = total_ = returned_ = 0;
  last_ = min_delta_ = 0;
  mini_idx_ = in_mini_left_ = values_per_mini_ = 0;
  mini_pos_ = NULL;
  mini_bit_ = 0;
  first_ = true;
  need_block_ = false;
}

int ObParquetDeltaDecoder::read_uleb(uint64_t &v)
{
  int ret = OB_SUCCESS;
  v = 0;
  int shift = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    if (data_ >= end_ || shift > 63) {
      ret = OB_INVALID_DATA;
    } else {
      uint8_t b = *data_++;
      v |= static_cast<uint64_t>(b & 0x7F) << shift;
      shift += 7;
      done = (0 == (b & 0x80));
    }
  }
  return ret;
}

int ObParquetDeltaDecoder::init(const uint8_t *data, int64_t len)
{
  int ret = OB_SUCCESS;
  uint64_t first = 0;
  reset();
  data_ = data;
  end_ = data + len;
  if (OB_FAIL(read_uleb(block_size_)) || OB_FAIL(read_uleb(mini_blocks_))
      || OB_FAIL(read_uleb(total_)) || OB_FAIL(read_uleb(first))) {
  } else if (0 == mini_blocks_ || mini_blocks_ > 256 || 0 == block_size_ || 0 != block_size_ % mini_blocks_) {
    ret = OB_INVALID_DATA;
  } else {
    values_per_mini_ = block_size_ / mini_blocks_;
    last_ = static_cast<int64_t>((first >> 1) ^ (~(first & 1) + 1));
  }
  return ret;
}

int ObParquetDeltaDecoder::read_block_header()
{
  int ret = OB_SUCCESS;
  uint64_t md = 0;
  if (OB_FAIL(read_uleb(md))) {
  } else if (static_cast<uint64_t>(end_ - data_) < mini_blocks_) {
    ret = OB_INVALID_DATA;
  } else {
    min_delta_ = static_cast<int64_t>((md >> 1) ^ (~(md & 1) + 1));
    MEMCPY(widths_, data_, mini_blocks_);
    data_ += mini_blocks_;
    mini_idx_ = 0;
    mini_pos_ = data_;
    mini_bit_ = 0;
    in_mini_left_ = values_per_mini_;
  }
  return ret;
}

int ObParquetDeltaDecoder::next(int64_t &value)
{
  int ret = OB_SUCCESS;
  if (returned_ >= total_) {
    ret = OB_ITER_END;
  } else if (first_) {
    first_ = false;
    value = last_;
    ++returned_;
    need_block_ = true;
  } else {
    if (need_block_) {
      need_block_ = false;
      ret = read_block_header();
    } else if (0 == in_mini_left_) {
      mini_pos_ += (values_per_mini_ * widths_[mini_idx_]) / 8;
      ++mini_idx_;
      mini_bit_ = 0;
      in_mini_left_ = values_per_mini_;
      if (mini_idx_ >= mini_blocks_) {
        data_ = mini_pos_;
        ret = read_block_header();
      }
    }
    if (OB_SUCC(ret)) {
      const int w = widths_[mini_idx_];
      uint64_t v = 0;
      if (w > 64) {
        ret = OB_INVALID_DATA;
      } else if (mini_pos_ + ((mini_bit_ + w + 7) / 8) > end_) {
        ret = OB_INVALID_DATA;
      } else {
        for (int b = 0; b < w; ++b) {
          const int64_t bit = mini_bit_ + b;
          v |= static_cast<uint64_t>((mini_pos_[bit >> 3] >> (bit & 7)) & 1) << b;
        }
        mini_bit_ += w;
        --in_mini_left_;
        last_ = static_cast<int64_t>(static_cast<uint64_t>(last_) + static_cast<uint64_t>(min_delta_) + v);
        value = last_;
        ++returned_;
      }
    }
  }
  return ret;
}

/////////////////////////////// ObParquetColumnReader ///////////////////////////////

int ObParquetColumnReader::set_err(ObFileErrorInfo &err, const char *msg)
{
  snprintf(err.msg_, sizeof(err.msg_), "parquet column %ld: %s", col_idx_ + 1, msg);
  return OB_INVALID_ARGUMENT;
}

int ObParquetColumnReader::init(int fd, const ObParquetChunkMeta &chunk, const ObFileParquetColInfo &info,
                                int64_t col_idx, int64_t file_size)
{
  int ret = OB_SUCCESS;
  fd_ = fd;
  chunk_ = &chunk;
  info_ = &info;
  col_idx_ = col_idx;
  offset_ = chunk.data_page_offset_;
  if (chunk.dict_page_offset_ > 0 && chunk.dict_page_offset_ < offset_) {
    offset_ = chunk.dict_page_offset_;
  }
  end_ = offset_ + chunk.total_compressed_size_;
  if (offset_ < 4 || end_ > file_size || chunk.total_compressed_size_ < 0) {
    ret = OB_INVALID_DATA;
  }
  page_left_ = 0;
  dict_cnt_ = 0;
  values_read_ = 0;
  return ret;
}

int ObParquetColumnReader::ensure(char *&buf, int64_t &cap, int64_t need)
{
  int ret = OB_SUCCESS;
  if (need > cap) {
    int64_t ncap = MAX(need, MAX(cap * 2, 4096));
    char *nbuf = static_cast<char *>(allocator_.alloc(ncap));
    if (OB_ISNULL(nbuf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      buf = nbuf;
      cap = ncap;
    }
  }
  return ret;
}

int ObParquetColumnReader::read_at(int64_t offset, char *buf, int64_t len)
{
  int ret = OB_SUCCESS;
  int64_t done = 0;
  while (OB_SUCC(ret) && done < len) {
    ssize_t n = ::pread(fd_, buf + done, len - done, offset + done);
    if (n < 0 && EINTR == errno) {
    } else if (n <= 0) {
      ret = OB_IO_ERROR;
      LOG_WARN("failed to read parquet file", K(ret), K(errno), K(offset), K(len));
    } else {
      done += n;
    }
  }
  return ret;
}

int ObParquetColumnReader::decompress(const char *src, int64_t src_len, char *dst, int64_t dst_len,
                                      ObFileErrorInfo &err)
{
  int ret = OB_SUCCESS;
  if (SNAPPY == chunk_->codec_) {
    if (OB_FAIL(ObSnappyUtil::decompress(src, src_len, dst, dst_len))) {
      ret = set_err(err, "corrupted snappy page");
    }
  } else if (GZIP == chunk_->codec_ || ZSTD == chunk_->codec_) {
    ObDecompressor *dec = NULL;
    ObCSVGeneralFormat::ObCSVCompression fmt = (GZIP == chunk_->codec_)
        ? ObCSVGeneralFormat::ObCSVCompression::GZIP : ObCSVGeneralFormat::ObCSVCompression::ZSTD;
    if (OB_FAIL(ObDecompressor::create(fmt, allocator_, dec))) {
      LOG_WARN("failed to create decompressor", K(ret));
    } else {
      int64_t in = 0;
      int64_t out = 0;
      int64_t guard = 0;
      while (OB_SUCC(ret) && out < dst_len && in < src_len && guard++ < 1000000) {
        int64_t consumed = 0;
        int64_t produced = 0;
        if (OB_FAIL(dec->decompress(src + in, src_len - in, consumed, dst + out, dst_len - out, produced))) {
          LOG_WARN("failed to decompress page", K(ret));
        } else if (0 == consumed && 0 == produced) {
          break;
        } else {
          in += consumed;
          out += produced;
        }
      }
      if (OB_SUCC(ret) && out != dst_len) {
        ret = OB_INVALID_DATA;
      }
      if (OB_FAIL(ret)) {
        ret = set_err(err, "corrupted compressed page");
      }
      ObDecompressor::destroy(dec);
    }
  } else {
    char msg[64];
    snprintf(msg, sizeof(msg), "compression codec %d is not supported", chunk_->codec_);
    ret = set_err(err, msg);
  }
  return ret;
}

int ObParquetColumnReader::load_dictionary(const char *data, int64_t len, int64_t num_values,
                                           ObFileErrorInfo &err)
{
  int ret = OB_SUCCESS;
  const int32_t pt = info_->physical_type_;
  const char *p = data;
  const char *end = data + len;
  dict_cnt_ = 0;
  if (num_values < 0 || num_values > len * 8 + 1) {
    ret = set_err(err, "invalid dictionary page");
  } else if (INT32 == pt || INT64 == pt) {
    if (OB_ISNULL(dict_ints_ = static_cast<int64_t *>(allocator_.alloc(sizeof(int64_t) * MAX(num_values, 1))))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
    const int64_t w = (INT32 == pt) ? 4 : 8;
    for (int64_t i = 0; OB_SUCC(ret) && i < num_values; ++i) {
      if (end - p < w) {
        ret = set_err(err, "truncated dictionary page");
      } else {
        dict_ints_[i] = (4 == w) ? static_cast<int32_t>(le32(p)) : static_cast<int64_t>(le64(p));
        p += w;
      }
    }
  } else if (FLOAT == pt || DOUBLE == pt) {
    if (OB_ISNULL(dict_dbls_ = static_cast<double *>(allocator_.alloc(sizeof(double) * MAX(num_values, 1))))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < num_values; ++i) {
      if (FLOAT == pt) {
        if (end - p < 4) {
          ret = set_err(err, "truncated dictionary page");
        } else {
          uint32_t u = le32(p);
          float f;
          MEMCPY(&f, &u, 4);
          dict_dbls_[i] = f;
          p += 4;
        }
      } else if (end - p < 8) {
        ret = set_err(err, "truncated dictionary page");
      } else {
        uint64_t u = le64(p);
        MEMCPY(&dict_dbls_[i], &u, 8);
        p += 8;
      }
    }
  } else if (BYTE_ARRAY == pt || FIXED_LEN_BYTE_ARRAY == pt || INT96 == pt) {
    if (OB_ISNULL(dict_strs_ = static_cast<ObString *>(allocator_.alloc(sizeof(ObString) * MAX(num_values, 1))))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < num_values; ++i) {
      int64_t l = (FIXED_LEN_BYTE_ARRAY == pt) ? info_->type_length_ : 12;
      if (BYTE_ARRAY == pt) {
        if (end - p < 4) {
          ret = set_err(err, "truncated dictionary page");
        } else {
          l = le32(p);
          p += 4;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (l < 0 || end - p < l) {
        ret = set_err(err, "truncated dictionary page");
      } else {
        new (&dict_strs_[i]) ObString(static_cast<int32_t>(l), p);
        p += l;
      }
    }
  } else {
    ret = set_err(err, "dictionary encoding is not supported for this type");
  }
  if (OB_SUCC(ret)) {
    dict_cnt_ = num_values;
  }
  return ret;
}

int ObParquetColumnReader::read_next_page(ObFileErrorInfo &err)
{
  int ret = OB_SUCCESS;
  bool got_data_page = false;
  while (OB_SUCC(ret) && !got_data_page) {
    if (offset_ >= end_) {
      ret = set_err(err, "unexpected end of column chunk");
      break;
    }
    PageHeader h;
    int64_t hdr_len = 0;
    int64_t try_len = MIN(static_cast<int64_t>(64 << 10), end_ - offset_);
    bool parsed = false;
    while (OB_SUCC(ret) && !parsed) {
      if (OB_FAIL(ensure(hdr_buf_, hdr_cap_, try_len))) {
      } else if (OB_FAIL(read_at(offset_, hdr_buf_, try_len))) {
      } else {
        ThriftReader r(hdr_buf_, try_len);
        h = PageHeader();
        parse_page_header(r, h);
        if (r.ok()) {
          parsed = true;
          hdr_len = r.consumed();
        } else if (try_len >= end_ - offset_ || try_len >= (16L << 20)) {
          ret = set_err(err, "corrupted page header");
        } else {
          try_len = MIN(try_len * 4, end_ - offset_);
        }
      }
    }
    if (OB_FAIL(ret)) {
      break;
    }
    offset_ += hdr_len;
    const int64_t comp = h.compressed_size_;
    const int64_t uncomp = h.uncompressed_size_;
    if (comp < 0 || uncomp < 0 || offset_ + comp > end_ || uncomp > (1L << 30)) {
      ret = set_err(err, "invalid page size");
      break;
    }
    if (OB_FAIL(ensure(raw_buf_, raw_cap_, MAX(comp, 1)))) {
      break;
    } else if (comp > 0 && OB_FAIL(read_at(offset_, raw_buf_, comp))) {
      break;
    }
    offset_ += comp;
    if (DICTIONARY_PAGE == h.type_) {
      if (OB_FAIL(ensure(dict_buf_, dict_cap_, MAX(uncomp, 1)))) {
      } else if (UNCOMPRESSED == chunk_->codec_) {
        if (comp != uncomp) {
          ret = set_err(err, "invalid dictionary page size");
        } else {
          MEMCPY(dict_buf_, raw_buf_, comp);
        }
      } else if (OB_FAIL(decompress(raw_buf_, comp, dict_buf_, uncomp, err))) {
      }
      if (OB_SUCC(ret)) {
        ret = load_dictionary(dict_buf_, uncomp, h.num_values_, err);
      }
    } else if (DATA_PAGE == h.type_ || DATA_PAGE_V2 == h.type_) {
      const char *values = NULL;
      int64_t values_len = 0;
      if (DATA_PAGE == h.type_) {
        const char *data = raw_buf_;
        if (UNCOMPRESSED != chunk_->codec_) {
          if (OB_FAIL(ensure(page_buf_, page_cap_, MAX(uncomp, 1)))) {
          } else if (OB_FAIL(decompress(raw_buf_, comp, page_buf_, uncomp, err))) {
          } else {
            data = page_buf_;
          }
        } else if (comp != uncomp) {
          ret = set_err(err, "invalid data page size");
        }
        values = data;
        values_len = uncomp;
        if (OB_SUCC(ret) && info_->max_def_ > 0) {
          if (values_len < 4) {
            ret = set_err(err, "truncated definition levels");
          } else {
            int64_t dl = le32(values);
            if (dl < 0 || dl > values_len - 4) {
              ret = set_err(err, "invalid definition levels");
            } else {
              def_dec_.init(reinterpret_cast<const uint8_t *>(values + 4), dl, 1);
              values += 4 + dl;
              values_len -= 4 + dl;
            }
          }
        }
      } else {
        const int64_t lv = static_cast<int64_t>(h.def_len_) + h.rep_len_;
        if (h.rep_len_ != 0) {
          ret = set_err(err, "repeated columns are not supported");
        } else if (h.def_len_ < 0 || lv > comp) {
          ret = set_err(err, "invalid v2 page levels");
        } else {
          if (info_->max_def_ > 0) {
            def_dec_.init(reinterpret_cast<const uint8_t *>(raw_buf_), h.def_len_, 1);
          }
          if (UNCOMPRESSED != chunk_->codec_ && h.is_compressed_ && comp - lv > 0) {
            if (OB_FAIL(ensure(page_buf_, page_cap_, MAX(uncomp - lv, 1)))) {
            } else if (OB_FAIL(decompress(raw_buf_ + lv, comp - lv, page_buf_, uncomp - lv, err))) {
            } else {
              values = page_buf_;
              values_len = uncomp - lv;
            }
          } else {
            values = raw_buf_ + lv;
            values_len = comp - lv;
          }
        }
      }
      if (OB_SUCC(ret)) {
        encoding_ = h.encoding_;
        values_ptr_ = values;
        values_end_ = values + values_len;
        bool_bit_ = 0;
        if (PLAIN == encoding_) {
        } else if (PLAIN_DICTIONARY == encoding_ || RLE_DICTIONARY == encoding_) {
          if (values_len < 1) {
            // all values of the page are null
            idx_dec_.init(NULL, 0, 0);
          } else {
            const int bw = static_cast<uint8_t>(values[0]);
            if (bw > 32) {
              ret = set_err(err, "invalid dictionary index width");
            } else {
              idx_dec_.init(reinterpret_cast<const uint8_t *>(values + 1), values_len - 1, bw);
            }
          }
        } else if (RLE == encoding_ && BOOLEAN == info_->physical_type_) {
          if (values_len < 4) {
            bool_rle_dec_.init(NULL, 0, 1);
          } else {
            int64_t l = le32(values);
            bool_rle_dec_.init(reinterpret_cast<const uint8_t *>(values + 4), MIN(l, values_len - 4), 1);
          }
        } else if (DELTA_BINARY_PACKED == encoding_ && (INT32 == info_->physical_type_ || INT64 == info_->physical_type_)) {
          if (values_len > 0 && OB_FAIL(delta_dec_.init(reinterpret_cast<const uint8_t *>(values), values_len))) {
            ret = set_err(err, "corrupted DELTA_BINARY_PACKED page");
          }
        } else {
          char msg[64];
          snprintf(msg, sizeof(msg), "page encoding %d is not supported", encoding_);
          ret = set_err(err, msg);
        }
      }
      if (OB_SUCC(ret)) {
        page_left_ = h.num_values_;
        got_data_page = page_left_ > 0;
      }
    } else {
      // index pages and unknown pages are skipped
    }
  }
  return ret;
}

void ObParquetColumnReader::convert_int(int64_t raw, ObFileCell &cell)
{
  cell.is_null_ = false;
  switch (info_->conv_) {
    case ObFileParquetColInfo::CONV_DATE:
      cell.int_ = static_cast<int32_t>(raw);
      break;
    case ObFileParquetColInfo::CONV_TS_MILLIS:
      cell.int_ = raw * 1000;
      break;
    case ObFileParquetColInfo::CONV_TS_NANOS:
      cell.int_ = (raw >= 0) ? raw / 1000 : -((-raw + 999) / 1000);
      break;
    case ObFileParquetColInfo::CONV_DECIMAL: {
      double d = static_cast<double>(raw);
      for (int32_t i = 0; i < info_->scale_; ++i) { d /= 10; }
      cell.double_ = d;
      break;
    }
    case ObFileParquetColInfo::CONV_UINT32:
      cell.int_ = static_cast<int64_t>(static_cast<uint32_t>(raw));
      break;
    default:
      cell.int_ = raw;
      break;
  }
}

static double fs_decimal_bytes_to_double(const char *p, int64_t len, int32_t scale)
{
  double d = 0;
  const uint8_t *u = reinterpret_cast<const uint8_t *>(p);
  const bool neg = len > 0 && (u[0] & 0x80);
  if (len <= 8) {
    int64_t v = neg ? -1 : 0;
    for (int64_t i = 0; i < len; ++i) {
      v = static_cast<int64_t>((static_cast<uint64_t>(v) << 8) | u[i]);
    }
    d = static_cast<double>(v);
  } else {
    for (int64_t i = 0; i < len; ++i) {
      d = d * 256 + (neg ? static_cast<uint8_t>(~u[i]) : u[i]);
    }
    if (neg) {
      d = -(d + 1);
    }
  }
  for (int32_t i = 0; i < scale; ++i) { d /= 10; }
  return d;
}

static void fs_set_bytes(const ObFileParquetColInfo &info, const char *p, int64_t len, ObFileCell &cell)
{
  cell.is_null_ = false;
  if (ObFileParquetColInfo::CONV_DECIMAL == info.conv_) {
    cell.double_ = fs_decimal_bytes_to_double(p, len, info.scale_);
  } else if (ObFileParquetColInfo::CONV_INT96_TS == info.conv_ && 12 == len) {
    const int64_t nanos = static_cast<int64_t>(le64(p));
    const int64_t julian = static_cast<int32_t>(le32(p + 8));
    cell.int_ = (julian - 2440588) * 86400LL * 1000000LL + nanos / 1000;
  } else {
    cell.str_.assign_ptr(p, static_cast<int32_t>(len));
  }
}

int ObParquetColumnReader::decode_plain(ObFileCell &cell, ObFileErrorInfo &err)
{
  int ret = OB_SUCCESS;
  const int64_t left = values_end_ - values_ptr_;
  switch (info_->physical_type_) {
    case BOOLEAN: {
      const int64_t byte_idx = bool_bit_ >> 3;
      if (byte_idx >= left) {
        ret = set_err(err, "truncated boolean page");
      } else {
        cell.is_null_ = false;
        cell.int_ = (static_cast<uint8_t>(values_ptr_[byte_idx]) >> (bool_bit_ & 7)) & 1;
        ++bool_bit_;
      }
      break;
    }
    case INT32:
      if (left < 4) {
        ret = set_err(err, "truncated page");
      } else {
        convert_int(static_cast<int32_t>(le32(values_ptr_)), cell);
        values_ptr_ += 4;
      }
      break;
    case INT64:
      if (left < 8) {
        ret = set_err(err, "truncated page");
      } else {
        convert_int(static_cast<int64_t>(le64(values_ptr_)), cell);
        values_ptr_ += 8;
      }
      break;
    case INT96:
      if (left < 12) {
        ret = set_err(err, "truncated page");
      } else {
        fs_set_bytes(*info_, values_ptr_, 12, cell);
        values_ptr_ += 12;
      }
      break;
    case FLOAT:
      if (left < 4) {
        ret = set_err(err, "truncated page");
      } else {
        uint32_t u = le32(values_ptr_);
        float f;
        MEMCPY(&f, &u, 4);
        cell.is_null_ = false;
        cell.double_ = f;
        values_ptr_ += 4;
      }
      break;
    case DOUBLE:
      if (left < 8) {
        ret = set_err(err, "truncated page");
      } else {
        uint64_t u = le64(values_ptr_);
        cell.is_null_ = false;
        MEMCPY(&cell.double_, &u, 8);
        values_ptr_ += 8;
      }
      break;
    case BYTE_ARRAY:
      if (left < 4) {
        ret = set_err(err, "truncated page");
      } else {
        const int64_t l = le32(values_ptr_);
        if (l < 0 || l > left - 4) {
          ret = set_err(err, "truncated page");
        } else {
          fs_set_bytes(*info_, values_ptr_ + 4, l, cell);
          values_ptr_ += 4 + l;
        }
      }
      break;
    case FIXED_LEN_BYTE_ARRAY:
      if (info_->type_length_ < 0 || left < info_->type_length_) {
        ret = set_err(err, "truncated page");
      } else {
        fs_set_bytes(*info_, values_ptr_, info_->type_length_, cell);
        values_ptr_ += info_->type_length_;
      }
      break;
    default:
      ret = set_err(err, "unsupported physical type");
      break;
  }
  return ret;
}

int ObParquetColumnReader::decode_value(ObFileCell &cell, ObFileErrorInfo &err)
{
  int ret = OB_SUCCESS;
  if (PLAIN == encoding_) {
    ret = decode_plain(cell, err);
  } else if (PLAIN_DICTIONARY == encoding_ || RLE_DICTIONARY == encoding_) {
    uint32_t idx = 0;
    if (OB_FAIL(idx_dec_.next(idx))) {
      ret = set_err(err, "corrupted dictionary indices");
    } else if (idx >= dict_cnt_) {
      ret = set_err(err, "dictionary index out of range");
    } else {
      const int32_t pt = info_->physical_type_;
      if (INT32 == pt || INT64 == pt) {
        convert_int(dict_ints_[idx], cell);
      } else if (FLOAT == pt || DOUBLE == pt) {
        cell.is_null_ = false;
        cell.double_ = dict_dbls_[idx];
      } else {
        fs_set_bytes(*info_, dict_strs_[idx].ptr(), dict_strs_[idx].length(), cell);
      }
    }
  } else if (RLE == encoding_) {
    uint32_t v = 0;
    if (OB_FAIL(bool_rle_dec_.next(v))) {
      ret = set_err(err, "corrupted RLE boolean page");
    } else {
      cell.is_null_ = false;
      cell.int_ = v ? 1 : 0;
    }
  } else if (DELTA_BINARY_PACKED == encoding_) {
    int64_t v = 0;
    if (OB_FAIL(delta_dec_.next(v))) {
      ret = set_err(err, "corrupted DELTA_BINARY_PACKED page");
    } else {
      convert_int(INT32 == info_->physical_type_ ? static_cast<int32_t>(v) : v, cell);
    }
  } else {
    ret = set_err(err, "unsupported encoding");
  }
  return ret;
}

int ObParquetColumnReader::next(ObFileCell &cell, ObFileErrorInfo &err)
{
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret) && page_left_ <= 0) {
    ret = read_next_page(err);
  }
  if (OB_SUCC(ret)) {
    --page_left_;
    uint32_t def = 1;
    if (info_->max_def_ > 0 && OB_FAIL(def_dec_.next(def))) {
      ret = set_err(err, "corrupted definition levels");
    } else if (info_->max_def_ > 0 && static_cast<int32_t>(def) < info_->max_def_) {
      cell.is_null_ = true;
    } else {
      ret = decode_value(cell, err);
    }
  }
  return ret;
}

/////////////////////////////// ObParquetFileReader ///////////////////////////////

ObParquetFileReader::ObParquetFileReader(ObIAllocator &allocator)
  : allocator_(allocator), rg_alloc_("FileSqlPqRg"), fd_(-1), identity_(), num_rows_(0),
    columns_(NULL), cur_rg_(-1), rg_rows_left_(0)
{}

ObParquetFileReader::~ObParquetFileReader()
{
  close();
}

void ObParquetFileReader::close()
{
  for (int64_t i = 0; i < readers_.count(); ++i) {
    if (NULL != readers_.at(i)) {
      readers_.at(i)->~ObParquetColumnReader();
    }
  }
  readers_.reset();
  for (int64_t i = 0; i < row_groups_.count(); ++i) {
    if (NULL != row_groups_.at(i)) {
      row_groups_.at(i)->~ObParquetRowGroupMeta();
    }
  }
  row_groups_.reset();
  rg_alloc_.reset();
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

int ObParquetFileReader::open(const char *path, const ObFileIdentity *expect_identity, ObFileErrorInfo &err)
{
  int ret = OB_SUCCESS;
  struct stat st;
  close();
  if ((fd_ = ::open(path, O_RDONLY | O_CLOEXEC)) < 0) {
    ret = (ENOENT == errno || ENOTDIR == errno) ? OB_FILE_NOT_EXIST : OB_IO_ERROR;
  } else if (0 != ::fstat(fd_, &st)) {
    ret = OB_IO_ERROR;
  } else if (!S_ISREG(st.st_mode)) {
    ret = OB_FILE_NOT_EXIST;
  } else {
    identity_.size_ = st.st_size;
    identity_.mtime_ns_ = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
    identity_.inode_ = static_cast<int64_t>(st.st_ino);
    identity_.dev_ = static_cast<int64_t>(st.st_dev);
    if (NULL != expect_identity && expect_identity->is_valid() && *expect_identity != identity_) {
      ret = OB_FILE_ALREADY_EXIST;
    } else if (OB_FAIL(parse_footer(err))) {
      LOG_WARN("failed to parse parquet footer", K(ret));
    }
  }
  return ret;
}

int ObParquetFileReader::parse_footer(ObFileErrorInfo &err)
{
  int ret = OB_SUCCESS;
  char tail[8];
  const int64_t size = identity_.size_;
  char *meta = NULL;
  int64_t meta_len = 0;
  if (size < 12) {
    ret = OB_INVALID_ARGUMENT;
    snprintf(err.msg_, sizeof(err.msg_), "not a parquet file (too small)");
  } else if (8 != ::pread(fd_, tail, 8, size - 8) || 0 != MEMCMP(tail + 4, "PAR1", 4)) {
    ret = OB_INVALID_ARGUMENT;
    snprintf(err.msg_, sizeof(err.msg_), "not a parquet file (missing PAR1 footer magic)");
  } else if ((meta_len = le32(tail)) <= 0 || meta_len > size - 12 || meta_len > (256L << 20)) {
    ret = OB_INVALID_ARGUMENT;
    snprintf(err.msg_, sizeof(err.msg_), "invalid parquet footer length %ld", meta_len);
  } else if (OB_ISNULL(meta = static_cast<char *>(allocator_.alloc(meta_len)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    int64_t done = 0;
    while (OB_SUCC(ret) && done < meta_len) {
      ssize_t n = ::pread(fd_, meta + done, meta_len - done, size - 8 - meta_len + done);
      if (n < 0 && EINTR == errno) {
      } else if (n <= 0) {
        ret = OB_IO_ERROR;
      } else {
        done += n;
      }
    }
  }
  if (OB_SUCC(ret)) {
    ThriftReader r(meta, meta_len);
    int16_t last = 0, fid = 0;
    uint8_t t = 0;
    int64_t root_children = -1;
    bool nested = false;
    ObString nested_name;
    while (OB_SUCC(ret) && r.ok() && r.field(last, fid, t)) {
      if (2 == fid && T_LIST == t) {
        uint8_t et = 0;
        int64_t n = 0;
        r.list_header(et, n);
        for (int64_t i = 0; OB_SUCC(ret) && r.ok() && i < n; ++i) {
          ObParquetLeafMeta leaf;
          int32_t num_children = 0;
          int16_t l2 = 0, f2 = 0;
          uint8_t t2 = 0;
          while (r.ok() && r.field(l2, f2, t2)) {
            if (1 == f2 && T_I32 == t2) { leaf.physical_type_ = static_cast<int32_t>(r.i_val()); }
            else if (2 == f2 && T_I32 == t2) { leaf.type_length_ = static_cast<int32_t>(r.i_val()); }
            else if (3 == f2 && T_I32 == t2) { leaf.repetition_ = static_cast<int32_t>(r.i_val()); }
            else if (4 == f2 && T_BINARY == t2) { r.binary(leaf.name_); }
            else if (5 == f2 && T_I32 == t2) { num_children = static_cast<int32_t>(r.i_val()); }
            else if (6 == f2 && T_I32 == t2) { leaf.converted_type_ = static_cast<int32_t>(r.i_val()); }
            else if (7 == f2 && T_I32 == t2) { leaf.scale_ = static_cast<int32_t>(r.i_val()); }
            else if (8 == f2 && T_I32 == t2) { leaf.precision_ = static_cast<int32_t>(r.i_val()); }
            else if (10 == f2 && T_STRUCT == t2) { parse_logical_type(r, leaf); }
            else { r.skip(t2); }
          }
          if (0 == i) {
            root_children = num_children;
          } else if (num_children > 0 || 2 == leaf.repetition_ || leaf.physical_type_ < 0) {
            if (!nested) {
              nested = true;
              nested_name = leaf.name_;
            }
          } else if (OB_FAIL(leaves_.push_back(leaf))) {
          }
        }
      } else if (3 == fid && T_I64 == t) {
        num_rows_ = r.i_val();
      } else if (4 == fid && T_LIST == t) {
        uint8_t et = 0;
        int64_t n = 0;
        r.list_header(et, n);
        for (int64_t i = 0; OB_SUCC(ret) && r.ok() && i < n; ++i) {
          void *buf = allocator_.alloc(sizeof(ObParquetRowGroupMeta));
          ObParquetRowGroupMeta *rg = NULL;
          if (OB_ISNULL(buf)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (FALSE_IT(rg = new (buf) ObParquetRowGroupMeta())) {
          } else if (OB_FAIL(row_groups_.push_back(rg))) {
            rg->~ObParquetRowGroupMeta();
          }
          int16_t l2 = 0, f2 = 0;
          uint8_t t2 = 0;
          while (OB_SUCC(ret) && r.ok() && r.field(l2, f2, t2)) {
            if (1 == f2 && T_LIST == t2) {
              uint8_t et2 = 0;
              int64_t nc = 0;
              r.list_header(et2, nc);
              for (int64_t c = 0; OB_SUCC(ret) && r.ok() && c < nc; ++c) {
                ObParquetChunkMeta chunk;
                int16_t l3 = 0, f3 = 0;
                uint8_t t3 = 0;
                while (r.ok() && r.field(l3, f3, t3)) {
                  if (3 == f3 && T_STRUCT == t3) {
                    int16_t l4 = 0, f4 = 0;
                    uint8_t t4 = 0;
                    while (r.ok() && r.field(l4, f4, t4)) {
                      if (1 == f4 && T_I32 == t4) { chunk.physical_type_ = static_cast<int32_t>(r.i_val()); }
                      else if (4 == f4 && T_I32 == t4) { chunk.codec_ = static_cast<int32_t>(r.i_val()); }
                      else if (5 == f4 && T_I64 == t4) { chunk.num_values_ = r.i_val(); }
                      else if (7 == f4 && T_I64 == t4) { chunk.total_compressed_size_ = r.i_val(); }
                      else if (9 == f4 && T_I64 == t4) { chunk.data_page_offset_ = r.i_val(); }
                      else if (11 == f4 && T_I64 == t4) { chunk.dict_page_offset_ = r.i_val(); }
                      else if (12 == f4 && T_STRUCT == t4) {
                        int16_t l5 = 0, f5 = 0;
                        uint8_t t5 = 0;
                        while (r.ok() && r.field(l5, f5, t5)) {
                          if (3 == f5 && T_I64 == t5) { chunk.null_count_ = r.i_val(); }
                          else { r.skip(t5); }
                        }
                      } else { r.skip(t4); }
                    }
                  } else {
                    r.skip(t3);
                  }
                }
                if (OB_FAIL(rg->chunks_.push_back(chunk))) {
                }
              }
            } else if (3 == f2 && T_I64 == t2) {
              rg->num_rows_ = r.i_val();
            } else {
              r.skip(t2);
            }
          }
        }
      } else {
        r.skip(t);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (!r.ok()) {
      ret = OB_INVALID_ARGUMENT;
      snprintf(err.msg_, sizeof(err.msg_), "corrupted parquet footer metadata");
    } else if (nested || (root_children >= 0 && root_children != leaves_.count())) {
      ret = OB_NOT_SUPPORTED;
      snprintf(err.msg_, sizeof(err.msg_), "nested parquet schema (column '%.*s') is",
               nested_name.length(), nested_name.ptr());
    } else {
      int64_t total = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i < row_groups_.count(); ++i) {
        if (row_groups_.at(i)->chunks_.count() != leaves_.count() || row_groups_.at(i)->num_rows_ < 0) {
          ret = OB_INVALID_ARGUMENT;
          snprintf(err.msg_, sizeof(err.msg_), "corrupted parquet row group %ld metadata", i);
        } else {
          total += row_groups_.at(i)->num_rows_;
        }
      }
      if (OB_SUCC(ret) && total != num_rows_) {
        ret = OB_INVALID_ARGUMENT;
        snprintf(err.msg_, sizeof(err.msg_), "parquet row count mismatch");
      }
    }
  }
  return ret;
}

int ObParquetFileReader::leaf_to_column(const ObParquetLeafMeta &leaf, ObFileColumnDesc &col,
                                        ObFileErrorInfo &err)
{
  int ret = OB_SUCCESS;
  ObFileParquetColInfo &pq = col.pq_;
  pq.physical_type_ = static_cast<int8_t>(leaf.physical_type_);
  pq.type_length_ = leaf.type_length_;
  pq.max_def_ = (1 == leaf.repetition_) ? 1 : 0;
  pq.conv_ = ObFileParquetColInfo::CONV_NONE;
  const bool is_decimal = (5 == leaf.logical_kind_) || (5 == leaf.converted_type_);
  const bool is_date = (6 == leaf.logical_kind_) || (6 == leaf.converted_type_);
  const bool is_ts = (8 == leaf.logical_kind_) || (9 == leaf.converted_type_) || (10 == leaf.converted_type_);
  const bool is_unsigned = (10 == leaf.logical_kind_ && !leaf.int_signed_)
                           || (leaf.converted_type_ >= 11 && leaf.converted_type_ <= 14);
  col.scale_ = 0;
  col.max_length_ = 0;
  switch (leaf.physical_type_) {
    case BOOLEAN:
      col.type_ = FILE_COL_BOOLEAN;
      break;
    case INT32:
      if (is_date) {
        col.type_ = FILE_COL_DATE;
        pq.conv_ = ObFileParquetColInfo::CONV_DATE;
      } else if (is_decimal) {
        col.type_ = FILE_COL_DOUBLE;
        pq.conv_ = ObFileParquetColInfo::CONV_DECIMAL;
        pq.scale_ = leaf.scale_;
      } else {
        col.type_ = FILE_COL_BIGINT;
        if (is_unsigned) {
          pq.conv_ = ObFileParquetColInfo::CONV_UINT32;
        }
      }
      break;
    case INT64:
      if (is_ts) {
        col.type_ = FILE_COL_DATETIME;
        int32_t unit = leaf.ts_unit_;
        if (0 == unit) {
          unit = (9 == leaf.converted_type_) ? 1 : 2;
        }
        pq.conv_ = (1 == unit) ? ObFileParquetColInfo::CONV_TS_MILLIS
                 : (3 == unit) ? ObFileParquetColInfo::CONV_TS_NANOS
                 : ObFileParquetColInfo::CONV_TS_MICROS;
        col.scale_ = (1 == unit) ? 3 : 6;
      } else if (is_decimal) {
        col.type_ = FILE_COL_DOUBLE;
        pq.conv_ = ObFileParquetColInfo::CONV_DECIMAL;
        pq.scale_ = leaf.scale_;
      } else {
        col.type_ = FILE_COL_BIGINT;
        if (is_unsigned) {
          pq.conv_ = ObFileParquetColInfo::CONV_UINT64;
        }
      }
      break;
    case INT96:
      col.type_ = FILE_COL_DATETIME;
      col.scale_ = 6;
      pq.conv_ = ObFileParquetColInfo::CONV_INT96_TS;
      break;
    case FLOAT:
    case DOUBLE:
      col.type_ = FILE_COL_DOUBLE;
      break;
    case BYTE_ARRAY:
    case FIXED_LEN_BYTE_ARRAY:
      if (is_decimal) {
        col.type_ = FILE_COL_DOUBLE;
        pq.conv_ = ObFileParquetColInfo::CONV_DECIMAL;
        pq.scale_ = leaf.scale_;
      } else {
        col.type_ = FILE_COL_VARCHAR;
        col.max_length_ = (FIXED_LEN_BYTE_ARRAY == leaf.physical_type_ && leaf.type_length_ > 0)
                          ? leaf.type_length_ : 65535;
      }
      break;
    default:
      ret = OB_NOT_SUPPORTED;
      snprintf(err.msg_, sizeof(err.msg_), "parquet physical type %d is", leaf.physical_type_);
      break;
  }
  return ret;
}

int ObParquetFileReader::infer_schema(ObIAllocator &allocator, ObIArray<ObFileColumnDesc> &columns,
                                      int64_t &row_count, ObFileIdentity &identity, ObFileErrorInfo &err)
{
  int ret = OB_SUCCESS;
  columns.reset();
  for (int64_t i = 0; OB_SUCC(ret) && i < leaves_.count(); ++i) {
    ObFileColumnDesc col;
    const ObParquetLeafMeta &leaf = leaves_.at(i);
    if (OB_FAIL(leaf_to_column(leaf, col, err))) {
    } else if (OB_FAIL(ob_write_string(allocator, leaf.name_, col.name_))) {
    } else {
      col.src_idx_ = static_cast<int32_t>(i);
      // nullable only if the column is optional and statistics do not prove there are no nulls
      col.nullable_ = false;
      if (1 == leaf.repetition_) {
        for (int64_t g = 0; g < row_groups_.count(); ++g) {
          const ObParquetChunkMeta &chunk = row_groups_.at(g)->chunks_.at(i);
          if (chunk.null_count_ != 0) {
            col.nullable_ = true;
          }
        }
      }
      if (OB_FAIL(columns.push_back(col))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    row_count = num_rows_;
    identity = identity_;
  }
  return ret;
}

int ObParquetFileReader::start_scan(const ObIArray<ObFileColumnDesc> &columns, const ObIArray<bool> &needed,
                                    ObFileErrorInfo &err)
{
  int ret = OB_SUCCESS;
  columns_ = &columns;
  scan_cols_.reset();
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    if (i < needed.count() && needed.at(i)) {
      const int32_t src = columns.at(i).src_idx_;
      if (src < 0 || src >= leaves_.count() || leaves_.at(src).physical_type_ != columns.at(i).pq_.physical_type_) {
        ret = OB_INVALID_ARGUMENT;
        snprintf(err.msg_, sizeof(err.msg_), "file changed after its schema was inferred, please rerun the query");
      } else if (OB_FAIL(scan_cols_.push_back(i))) {
      }
    }
  }
  cur_rg_ = -1;
  rg_rows_left_ = 0;
  return ret;
}

int ObParquetFileReader::rescan(ObFileErrorInfo &err)
{
  int ret = OB_SUCCESS;
  UNUSED(err);
  for (int64_t i = 0; i < readers_.count(); ++i) {
    if (NULL != readers_.at(i)) {
      readers_.at(i)->~ObParquetColumnReader();
    }
  }
  readers_.reset();
  rg_alloc_.reset();
  cur_rg_ = -1;
  rg_rows_left_ = 0;
  return ret;
}

int ObParquetFileReader::open_row_group(int64_t rg, ObFileErrorInfo &err)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; i < readers_.count(); ++i) {
    if (NULL != readers_.at(i)) {
      readers_.at(i)->~ObParquetColumnReader();
    }
  }
  readers_.reset();
  rg_alloc_.reset();
  const ObParquetRowGroupMeta *meta = row_groups_.at(rg);
  for (int64_t k = 0; OB_SUCC(ret) && k < scan_cols_.count(); ++k) {
    const ObFileColumnDesc &col = columns_->at(scan_cols_.at(k));
    void *buf = rg_alloc_.alloc(sizeof(ObParquetColumnReader));
    ObParquetColumnReader *reader = NULL;
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (FALSE_IT(reader = new (buf) ObParquetColumnReader(rg_alloc_))) {
    } else if (OB_FAIL(readers_.push_back(reader))) {
      reader->~ObParquetColumnReader();
    } else if (OB_FAIL(reader->init(fd_, meta->chunks_.at(col.src_idx_), col.pq_, col.src_idx_, identity_.size_))) {
      ret = OB_INVALID_ARGUMENT;
      snprintf(err.msg_, sizeof(err.msg_), "corrupted parquet column chunk metadata (row group %ld)", rg);
    }
  }
  if (OB_SUCC(ret)) {
    cur_rg_ = rg;
    rg_rows_left_ = meta->num_rows_;
  }
  return ret;
}

int ObParquetFileReader::next_row(ObIArray<ObFileCell> &cells, ObFileErrorInfo &err)
{
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret) && rg_rows_left_ <= 0) {
    if (cur_rg_ + 1 >= row_groups_.count()) {
      ret = OB_ITER_END;
    } else {
      ret = open_row_group(cur_rg_ + 1, err);
    }
  }
  if (OB_SUCC(ret)) {
    --rg_rows_left_;
    for (int64_t k = 0; OB_SUCC(ret) && k < readers_.count(); ++k) {
      ret = readers_.at(k)->next(cells.at(scan_cols_.at(k)), err);
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
