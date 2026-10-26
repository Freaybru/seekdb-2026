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

// Minimal, dependency free Apache Parquet reader for file sql.
//  * Thrift compact protocol footer decoding (FileMetaData / RowGroup / ColumnChunk / PageHeader)
//  * flat schemas only (nested groups / repeated fields are rejected)
//  * data page v1 and v2, dictionary pages
//  * encodings: PLAIN, PLAIN_DICTIONARY / RLE_DICTIONARY, RLE (levels, booleans), DELTA_BINARY_PACKED
//  * codecs: UNCOMPRESSED, SNAPPY (built-in decoder), GZIP (zlib), ZSTD
// Rows are produced by streaming pages column chunk by column chunk; only the
// columns requested by the plan are decoded (column pruning) and memory is
// bounded by the page size, not the file size.

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_PARQUET_READER_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_PARQUET_READER_H_

#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{

class ObSnappyUtil
{
public:
  static int get_uncompressed_length(const char *src, int64_t src_len, int64_t &len);
  static int decompress(const char *src, int64_t src_len, char *dst, int64_t dst_len);
};

// RLE / bit-packed hybrid decoder (parquet levels and dictionary indices)
class ObParquetRleDecoder
{
public:
  ObParquetRleDecoder() : data_(NULL), end_(NULL), bit_width_(0), rle_left_(0), rle_value_(0),
                          bp_left_(0), bp_pos_(NULL), bp_bit_(0) {}
  void init(const uint8_t *data, int64_t len, int bit_width);
  int next(uint32_t &value);
private:
  int read_header();
  const uint8_t *data_;
  const uint8_t *end_;
  int bit_width_;
  int64_t rle_left_;
  uint32_t rle_value_;
  int64_t bp_left_;
  const uint8_t *bp_pos_;
  int64_t bp_bit_;
};

class ObParquetDeltaDecoder
{
public:
  ObParquetDeltaDecoder() { reset(); }
  void reset();
  int init(const uint8_t *data, int64_t len);
  int next(int64_t &value);
private:
  int read_block_header();
  int read_uleb(uint64_t &v);
  const uint8_t *data_;
  const uint8_t *end_;
  uint64_t block_size_;
  uint64_t mini_blocks_;
  uint64_t total_;
  uint64_t returned_;
  int64_t last_;
  int64_t min_delta_;
  uint8_t widths_[256];
  uint64_t mini_idx_;
  uint64_t in_mini_left_;
  uint64_t values_per_mini_;
  const uint8_t *mini_pos_;
  int64_t mini_bit_;
  bool first_;
  bool need_block_;
};

struct ObParquetChunkMeta
{
  int32_t physical_type_;
  int32_t codec_;
  int64_t num_values_;
  int64_t data_page_offset_;
  int64_t dict_page_offset_;
  int64_t total_compressed_size_;
  int64_t null_count_;     // -1 if unknown
  ObParquetChunkMeta() : physical_type_(-1), codec_(0), num_values_(0), data_page_offset_(-1),
                         dict_page_offset_(-1), total_compressed_size_(0), null_count_(-1) {}
};

struct ObParquetRowGroupMeta
{
  int64_t num_rows_;
  common::ObSEArray<ObParquetChunkMeta, 8> chunks_;
  ObParquetRowGroupMeta() : num_rows_(0) {}
};

struct ObParquetLeafMeta
{
  common::ObString name_;
  int32_t physical_type_;
  int32_t type_length_;
  int32_t repetition_;     // 0 required, 1 optional, 2 repeated
  int32_t converted_type_; // -1 none
  int32_t scale_;
  int32_t precision_;
  int32_t logical_kind_;   // thrift LogicalType union field id, 0 none
  int32_t ts_unit_;        // 1 millis 2 micros 3 nanos
  int32_t int_bits_;
  bool int_signed_;
  ObParquetLeafMeta() : physical_type_(-1), type_length_(0), repetition_(0), converted_type_(-1),
                        scale_(0), precision_(0), logical_kind_(0), ts_unit_(0), int_bits_(0),
                        int_signed_(true) {}
};

// decodes one column chunk page by page
class ObParquetColumnReader
{
public:
  ObParquetColumnReader(common::ObIAllocator &allocator)
    : allocator_(allocator), fd_(-1), chunk_(NULL), info_(NULL), col_idx_(0), offset_(0), end_(0),
      page_buf_(NULL), page_cap_(0), raw_buf_(NULL), raw_cap_(0), hdr_buf_(NULL), hdr_cap_(0),
      dict_buf_(NULL), dict_cap_(0), dict_cnt_(0), dict_ints_(NULL), dict_dbls_(NULL), dict_strs_(NULL),
      page_left_(0), values_ptr_(NULL), values_end_(NULL), encoding_(0), bool_bit_(0),
      values_read_(0) {}
  ~ObParquetColumnReader() {}
  int init(int fd, const ObParquetChunkMeta &chunk, const ObFileParquetColInfo &info, int64_t col_idx,
           int64_t file_size);
  int next(ObFileCell &cell, ObFileErrorInfo &err);
private:
  int read_next_page(ObFileErrorInfo &err);
  int read_at(int64_t offset, char *buf, int64_t len);
  int ensure(char *&buf, int64_t &cap, int64_t need);
  int decompress(const char *src, int64_t src_len, char *dst, int64_t dst_len, ObFileErrorInfo &err);
  int load_dictionary(const char *data, int64_t len, int64_t num_values, ObFileErrorInfo &err);
  int decode_plain(ObFileCell &cell, ObFileErrorInfo &err);
  int decode_value(ObFileCell &cell, ObFileErrorInfo &err);
  void convert_int(int64_t raw, ObFileCell &cell);
  int set_err(ObFileErrorInfo &err, const char *msg);
private:
  common::ObIAllocator &allocator_;
  int fd_;
  const ObParquetChunkMeta *chunk_;
  const ObFileParquetColInfo *info_;
  int64_t col_idx_;
  int64_t offset_;
  int64_t end_;
  char *page_buf_;
  int64_t page_cap_;
  char *raw_buf_;
  int64_t raw_cap_;
  char *hdr_buf_;
  int64_t hdr_cap_;
  char *dict_buf_;
  int64_t dict_cap_;
  int64_t dict_cnt_;
  int64_t *dict_ints_;
  double *dict_dbls_;
  common::ObString *dict_strs_;
  int64_t page_left_;
  ObParquetRleDecoder def_dec_;
  ObParquetRleDecoder idx_dec_;
  ObParquetRleDecoder bool_rle_dec_;
  ObParquetDeltaDecoder delta_dec_;
  const char *values_ptr_;
  const char *values_end_;
  int32_t encoding_;
  int64_t bool_bit_;
  int64_t values_read_;
};

class ObParquetFileReader
{
public:
  explicit ObParquetFileReader(common::ObIAllocator &allocator);
  ~ObParquetFileReader();
  int open(const char *path, const ObFileIdentity *expect_identity, ObFileErrorInfo &err);
  int infer_schema(common::ObIAllocator &allocator, common::ObIArray<ObFileColumnDesc> &columns,
                   int64_t &row_count, ObFileIdentity &identity, ObFileErrorInfo &err);
  int start_scan(const common::ObIArray<ObFileColumnDesc> &columns, const common::ObIArray<bool> &needed,
                 ObFileErrorInfo &err);
  int next_row(common::ObIArray<ObFileCell> &cells, ObFileErrorInfo &err);
  int rescan(ObFileErrorInfo &err);
  void close();
  int64_t num_rows() const { return num_rows_; }
private:
  int parse_footer(ObFileErrorInfo &err);
  int open_row_group(int64_t rg, ObFileErrorInfo &err);
  int leaf_to_column(const ObParquetLeafMeta &leaf, ObFileColumnDesc &col, ObFileErrorInfo &err);
private:
  common::ObIAllocator &allocator_;
  common::ObArenaAllocator rg_alloc_;
  int fd_;
  ObFileIdentity identity_;
  int64_t num_rows_;
  common::ObSEArray<ObParquetLeafMeta, 16> leaves_;
  common::ObSEArray<ObParquetRowGroupMeta *, 4> row_groups_;
  const common::ObIArray<ObFileColumnDesc> *columns_;
  common::ObSEArray<int64_t, 16> scan_cols_;     // output column index of each decoded column
  common::ObSEArray<ObParquetColumnReader *, 16> readers_;
  int64_t cur_rg_;
  int64_t rg_rows_left_;
};

} // namespace sql
} // namespace oceanbase
#endif
