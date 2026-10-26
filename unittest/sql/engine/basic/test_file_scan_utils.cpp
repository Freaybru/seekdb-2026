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

// Unit tests for the file-as-database readers: type parsing, schema inference,
// streaming scans (CSV / NDJSON / JSON array), COPY option encoding and the CSV writer.

#include <gtest/gtest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string>
#include "lib/allocator/page_arena.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{
using namespace common;

class TestFileScanUtils : public ::testing::Test
{
protected:
  virtual void SetUp() override
  {
    char tmpl[] = "/tmp/test_file_scan_utilsXXXXXX";
    ASSERT_NE(nullptr, mkdtemp(tmpl));
    dir_ = tmpl;
  }
  virtual void TearDown() override
  {
    std::string cmd = "rm -rf '" + dir_ + "'";
    (void)system(cmd.c_str());
  }
  std::string write_file(const char *name, const std::string &content)
  {
    std::string path = dir_ + "/" + name;
    FILE *fp = fopen(path.c_str(), "wb");
    EXPECT_NE(nullptr, fp);
    if (nullptr != fp) {
      fwrite(content.data(), 1, content.size(), fp);
      fclose(fp);
    }
    return path;
  }
  static std::string read_file(const std::string &path)
  {
    std::string out;
    FILE *fp = fopen(path.c_str(), "rb");
    if (nullptr != fp) {
      char buf[4096];
      size_t n = 0;
      while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        out.append(buf, n);
      }
      fclose(fp);
    }
    return out;
  }
  int infer(const std::string &path, const ObFileScanOptions &opts,
            ObIArray<ObFileColumnDesc> &cols, int64_t &rows)
  {
    ObFileIdentity identity;
    ObFileErrorInfo err;
    return ObFileSchemaInferrer::infer(path.c_str(), opts, alloc_, cols, rows, identity, err);
  }
  static std::string str(const ObString &s) { return std::string(s.ptr(), s.length()); }

  std::string dir_;
  ObArenaAllocator alloc_;
};

TEST_F(TestFileScanUtils, parse_scalars)
{
  int64_t iv = 0;
  bool overflow = false;
  ASSERT_TRUE(ObFileTypeUtil::parse_int64("-42", 3, iv, overflow));
  ASSERT_EQ(-42, iv);
  ASSERT_FALSE(ObFileTypeUtil::parse_int64("4x", 2, iv, overflow));
  ASSERT_FALSE(ObFileTypeUtil::parse_int64("99999999999999999999", 20, iv, overflow));
  ASSERT_TRUE(overflow);

  double dv = 0;
  ASSERT_TRUE(ObFileTypeUtil::parse_double("98.5", 4, dv));
  ASSERT_DOUBLE_EQ(98.5, dv);
  ASSERT_TRUE(ObFileTypeUtil::parse_double("1e3", 3, dv));
  ASSERT_DOUBLE_EQ(1000.0, dv);
  ASSERT_FALSE(ObFileTypeUtil::parse_double("abc", 3, dv));

  bool bv = false;
  ASSERT_TRUE(ObFileTypeUtil::parse_bool("true", 4, bv));
  ASSERT_TRUE(bv);
  ASSERT_TRUE(ObFileTypeUtil::parse_bool("FALSE", 5, bv));
  ASSERT_FALSE(bv);

  int32_t days = 0;
  ASSERT_TRUE(ObFileTypeUtil::parse_date("1970-01-02", 10, days));
  ASSERT_EQ(1, days);
  ASSERT_TRUE(ObFileTypeUtil::parse_date("2024-02-29", 10, days));
  ASSERT_FALSE(ObFileTypeUtil::parse_date("2023-02-29", 10, days));
  ASSERT_FALSE(ObFileTypeUtil::parse_date("2023-13-01", 10, days));

  int64_t usec = 0;
  bool has_frac = false;
  ASSERT_TRUE(ObFileTypeUtil::parse_datetime("1970-01-01 00:00:01", 19, usec, has_frac, false));
  ASSERT_EQ(1000000, usec);
  ASSERT_FALSE(has_frac);
  ASSERT_TRUE(ObFileTypeUtil::parse_datetime("1970-01-01T00:00:01.5", 21, usec, has_frac, false));
  ASSERT_EQ(1500000, usec);
  ASSERT_TRUE(has_frac);
  ASSERT_FALSE(ObFileTypeUtil::parse_datetime("1970-01-01", 10, usec, has_frac, false));
  ASSERT_TRUE(ObFileTypeUtil::parse_datetime("1970-01-01", 10, usec, has_frac, true));
  ASSERT_EQ(0, usec);
}

TEST_F(TestFileScanUtils, classify_and_widen)
{
  bool frac = false;
  ASSERT_EQ(FILE_COL_BIGINT, ObFileTypeUtil::classify_text("12", 2, frac));
  ASSERT_EQ(FILE_COL_DOUBLE, ObFileTypeUtil::classify_text("1.5", 3, frac));
  ASSERT_EQ(FILE_COL_BOOLEAN, ObFileTypeUtil::classify_text("true", 4, frac));
  ASSERT_EQ(FILE_COL_DATE, ObFileTypeUtil::classify_text("2026-08-01", 10, frac));
  ASSERT_EQ(FILE_COL_DATETIME, ObFileTypeUtil::classify_text("2026-08-01 10:11:12", 19, frac));
  ASSERT_EQ(FILE_COL_VARCHAR, ObFileTypeUtil::classify_text("hello", 5, frac));

  ASSERT_EQ(FILE_COL_BIGINT, ObFileTypeUtil::widen(FILE_COL_NULL, FILE_COL_BIGINT));
  ASSERT_EQ(FILE_COL_DOUBLE, ObFileTypeUtil::widen(FILE_COL_BIGINT, FILE_COL_DOUBLE));
  ASSERT_EQ(FILE_COL_DATETIME, ObFileTypeUtil::widen(FILE_COL_DATE, FILE_COL_DATETIME));
  ASSERT_EQ(FILE_COL_VARCHAR, ObFileTypeUtil::widen(FILE_COL_BOOLEAN, FILE_COL_BIGINT));
  ASSERT_EQ(FILE_COL_VARCHAR, ObFileTypeUtil::widen(FILE_COL_DATE, FILE_COL_DOUBLE));
  ASSERT_STREQ("BIGINT", ObFileTypeUtil::type_name(FILE_COL_BIGINT));
  ASSERT_STREQ("BOOLEAN", ObFileTypeUtil::type_name(FILE_COL_BOOLEAN));
}

TEST_F(TestFileScanUtils, csv_inference_and_scan)
{
  std::string path = write_file("a.csv",
      "id,name,score,active,day,created,note,id\n"
      "1,Alice,98.5,true,2026-08-01,2026-08-01 10:11:12,\"hello,\nworld\",101\n"
      "2,Bob,87,false,,,plain,102\n"
      "3,Chen,90.25,true,2026-08-03,2026-08-03T12:13:14,\"say \"\"hi\"\"\",103\n");
  ObFileScanOptions opts;
  opts.format_ = FILE_FORMAT_CSV;
  ObSEArray<ObFileColumnDesc, 8> cols;
  int64_t rows = 0;
  ASSERT_EQ(OB_SUCCESS, infer(path, opts, cols, rows));
  ASSERT_EQ(3, rows);
  ASSERT_EQ(8, cols.count());
  const char *names[] = {"id", "name", "score", "active", "day", "created", "note", "id__2"};
  const int8_t types[] = {FILE_COL_BIGINT, FILE_COL_VARCHAR, FILE_COL_DOUBLE, FILE_COL_BOOLEAN,
                          FILE_COL_DATE, FILE_COL_DATETIME, FILE_COL_VARCHAR, FILE_COL_BIGINT};
  const bool nullable[] = {false, false, false, false, true, true, false, false};
  for (int64_t i = 0; i < cols.count(); ++i) {
    EXPECT_EQ(std::string(names[i]), str(cols.at(i).name_)) << i;
    EXPECT_EQ(types[i], cols.at(i).type_) << i;
    EXPECT_EQ(nullable[i], cols.at(i).nullable_) << i;
  }

  ObFileRowScanner scanner(alloc_);
  ObSEArray<bool, 8> needed;
  for (int64_t i = 0; i < cols.count(); ++i) {
    ASSERT_EQ(OB_SUCCESS, needed.push_back(true));
  }
  ASSERT_EQ(OB_SUCCESS, scanner.open(path.c_str(), opts, cols, nullptr, &needed));
  ASSERT_EQ(OB_SUCCESS, scanner.get_next_row());
  ASSERT_EQ(1, scanner.cell(0).int_);
  ASSERT_EQ("Alice", str(scanner.cell(1).str_));
  ASSERT_DOUBLE_EQ(98.5, scanner.cell(2).double_);
  ASSERT_EQ(1, scanner.cell(3).int_);
  ASSERT_EQ("hello,\nworld", str(scanner.cell(6).str_));
  ASSERT_EQ(OB_SUCCESS, scanner.get_next_row());
  ASSERT_TRUE(scanner.cell(4).is_null_);
  ASSERT_TRUE(scanner.cell(5).is_null_);
  ASSERT_DOUBLE_EQ(87.0, scanner.cell(2).double_);
  ASSERT_EQ(OB_SUCCESS, scanner.get_next_row());
  ASSERT_EQ("say \"hi\"", str(scanner.cell(6).str_));
  ASSERT_EQ(103, scanner.cell(7).int_);
  ASSERT_EQ(OB_ITER_END, scanner.get_next_row());
  // rescan restarts from the first data row
  ASSERT_EQ(OB_SUCCESS, scanner.rescan());
  ASSERT_EQ(OB_SUCCESS, scanner.get_next_row());
  ASSERT_EQ(1, scanner.cell(0).int_);
  scanner.close();
}

TEST_F(TestFileScanUtils, csv_options)
{
  std::string path = write_file("b.csv", "skip me\n1|NA|x\n2|7|y\n");
  ObFileScanOptions opts;
  opts.format_ = FILE_FORMAT_CSV;
  opts.header_ = false;
  opts.delim_ = '|';
  opts.skip_rows_ = 1;
  opts.null_str_ = ObString::make_string("NA");
  ObSEArray<ObFileColumnDesc, 8> cols;
  int64_t rows = 0;
  ASSERT_EQ(OB_SUCCESS, infer(path, opts, cols, rows));
  ASSERT_EQ(2, rows);
  ASSERT_EQ(3, cols.count());
  ASSERT_EQ(FILE_COL_BIGINT, cols.at(1).type_);
  ASSERT_TRUE(cols.at(1).nullable_);

  opts.all_varchar_ = true;
  cols.reset();
  ASSERT_EQ(OB_SUCCESS, infer(path, opts, cols, rows));
  ASSERT_EQ(FILE_COL_VARCHAR, cols.at(0).type_);
}

TEST_F(TestFileScanUtils, csv_errors)
{
  // unterminated quote must fail with a location, not crash or hang
  std::string bad = write_file("bad.csv", "a,b\n1,\"oops\n");
  ObFileScanOptions opts;
  opts.format_ = FILE_FORMAT_CSV;
  ObSEArray<ObFileColumnDesc, 8> cols;
  ObFileIdentity identity;
  ObFileErrorInfo err;
  int64_t rows = 0;
  ASSERT_NE(OB_SUCCESS, ObFileSchemaInferrer::infer(bad.c_str(), opts, alloc_, cols, rows, identity, err));
  ASSERT_GT(err.line_, 0);

  // ragged rows are rejected unless null_padding / ignore_errors
  std::string ragged = write_file("ragged.csv", "a,b\n1,2\n3\n");
  cols.reset();
  ASSERT_NE(OB_SUCCESS, infer(ragged, opts, cols, rows));
  opts.null_padding_ = true;
  cols.reset();
  ASSERT_EQ(OB_SUCCESS, infer(ragged, opts, cols, rows));
  ASSERT_EQ(2, rows);
  ASSERT_TRUE(cols.at(1).nullable_);

  // missing file
  cols.reset();
  std::string missing = dir_ + "/nope.csv";
  ASSERT_NE(OB_SUCCESS, infer(missing, opts, cols, rows));
}

TEST_F(TestFileScanUtils, json_inference_and_scan)
{
  std::string nd = write_file("a.jsonl",
      "{\"id\":1,\"name\":\"Alice\",\"score\":98.5,\"active\":true}\n"
      "{\"id\":2,\"name\":\"Bob\",\"active\":false,\"day\":\"2026-08-02\"}\n"
      "\n"
      "{\"id\":3,\"name\":\"Chen\",\"score\":90,\"active\":true,\"note\":\"ok\"}\n");
  ObFileScanOptions opts;
  opts.format_ = FILE_FORMAT_JSON;
  opts.json_layout_ = FILE_JSON_NDJSON;
  ObSEArray<ObFileColumnDesc, 8> cols;
  int64_t rows = 0;
  ASSERT_EQ(OB_SUCCESS, infer(nd, opts, cols, rows));
  ASSERT_EQ(3, rows);
  ASSERT_EQ(6, cols.count());
  ASSERT_EQ("id", str(cols.at(0).name_));
  ASSERT_EQ(FILE_COL_BIGINT, cols.at(0).type_);
  ASSERT_EQ(FILE_COL_DOUBLE, cols.at(2).type_);
  ASSERT_TRUE(cols.at(2).nullable_);
  ASSERT_EQ(FILE_COL_BOOLEAN, cols.at(3).type_);
  ASSERT_EQ("day", str(cols.at(4).name_));
  ASSERT_EQ(FILE_COL_DATE, cols.at(4).type_);
  ASSERT_EQ("note", str(cols.at(5).name_));

  ObFileRowScanner scanner(alloc_);
  ASSERT_EQ(OB_SUCCESS, scanner.open(nd.c_str(), opts, cols, nullptr, nullptr));
  ASSERT_EQ(OB_SUCCESS, scanner.get_next_row());
  ASSERT_TRUE(scanner.cell(4).is_null_);
  ASSERT_EQ(OB_SUCCESS, scanner.get_next_row());
  ASSERT_TRUE(scanner.cell(2).is_null_);
  ASSERT_FALSE(scanner.cell(4).is_null_);
  ASSERT_EQ(OB_SUCCESS, scanner.get_next_row());
  ASSERT_DOUBLE_EQ(90.0, scanner.cell(2).double_);
  ASSERT_EQ("ok", str(scanner.cell(5).str_));
  ASSERT_EQ(OB_ITER_END, scanner.get_next_row());
  scanner.close();

  // a top level array is detected automatically
  std::string arr = write_file("a.json", "[{\"a\":1,\"b\":\"x\\ny\"},\n {\"a\":2.5,\"b\":null}]");
  opts.json_layout_ = FILE_JSON_AUTO;
  cols.reset();
  ASSERT_EQ(OB_SUCCESS, infer(arr, opts, cols, rows));
  ASSERT_EQ(2, rows);
  ASSERT_EQ(FILE_COL_DOUBLE, cols.at(0).type_);
  ASSERT_EQ(FILE_COL_VARCHAR, cols.at(1).type_);
  ObFileRowScanner scanner2(alloc_);
  ASSERT_EQ(OB_SUCCESS, scanner2.open(arr.c_str(), opts, cols, nullptr, nullptr));
  ASSERT_EQ(OB_SUCCESS, scanner2.get_next_row());
  ASSERT_EQ("x\ny", str(scanner2.cell(1).str_));
  scanner2.close();

  // malformed json reports the line
  std::string bad = write_file("bad.jsonl", "{\"a\":1}\n{\"a\":}\n");
  opts.json_layout_ = FILE_JSON_NDJSON;
  cols.reset();
  ObFileIdentity identity;
  ObFileErrorInfo err;
  ASSERT_NE(OB_SUCCESS, ObFileSchemaInferrer::infer(bad.c_str(), opts, alloc_, cols, rows, identity, err));
  ASSERT_EQ(2, err.line_);
}

TEST_F(TestFileScanUtils, identity_change_detected)
{
  std::string path = write_file("c.csv", "a\n1\n");
  ObFileScanOptions opts;
  opts.format_ = FILE_FORMAT_CSV;
  ObSEArray<ObFileColumnDesc, 8> cols;
  ObFileIdentity identity;
  ObFileErrorInfo err;
  int64_t rows = 0;
  ASSERT_EQ(OB_SUCCESS, ObFileSchemaInferrer::infer(path.c_str(), opts, alloc_, cols, rows, identity, err));
  ASSERT_TRUE(identity.is_valid());
  write_file("c.csv", "a\n1\n2\n");
  ObFileRowScanner scanner(alloc_);
  ASSERT_NE(OB_SUCCESS, scanner.open(path.c_str(), opts, cols, &identity, nullptr));
}

TEST_F(TestFileScanUtils, copy_options_roundtrip)
{
  ObFileCopyOptions in;
  in.header_ = false;
  in.delim_ = '|';
  in.quote_ = '\'';
  in.escape_ = '\\';
  in.null_str_ = ObString::make_string("N;A");
  ASSERT_EQ(OB_SUCCESS, in.col_names_.push_back(ObString::make_string("a,b")));
  ASSERT_EQ(OB_SUCCESS, in.col_names_.push_back(ObString::make_string("c;d=")));
  ObString encoded;
  ASSERT_EQ(OB_SUCCESS, in.encode(alloc_, encoded));
  ASSERT_TRUE(ObFileCopyOptions::is_copy_options(encoded));
  ASSERT_FALSE(ObFileCopyOptions::is_copy_options(ObString::make_string("FORMAT=(TYPE='CSV')")));
  ObFileCopyOptions out;
  ASSERT_EQ(OB_SUCCESS, out.decode(encoded, alloc_));
  ASSERT_FALSE(out.header_);
  ASSERT_EQ('|', out.delim_);
  ASSERT_EQ('\'', out.quote_);
  ASSERT_EQ('\\', out.escape_);
  ASSERT_EQ("N;A", str(out.null_str_));
  ASSERT_EQ(2, out.col_names_.count());
  ASSERT_EQ("a,b", str(out.col_names_.at(0)));
  ASSERT_EQ("c;d=", str(out.col_names_.at(1)));
  ASSERT_NE(OB_SUCCESS, out.decode(ObString::make_string("garbage"), alloc_));
}

TEST_F(TestFileScanUtils, csv_writer)
{
  ObFileCopyOptions opts;
  opts.null_str_ = ObString::make_string("NULL");
  ASSERT_EQ(OB_SUCCESS, opts.col_names_.push_back(ObString::make_string("id")));
  ASSERT_EQ(OB_SUCCESS, opts.col_names_.push_back(ObString::make_string("note")));
  ASSERT_FALSE(ObFileCsvWriter::need_quote(ObString::make_string("plain"), opts));
  ASSERT_TRUE(ObFileCsvWriter::need_quote(ObString::make_string("a,b"), opts));
  ASSERT_TRUE(ObFileCsvWriter::need_quote(ObString::make_string("a\nb"), opts));
  ASSERT_TRUE(ObFileCsvWriter::need_quote(ObString::make_string("say \"hi\""), opts));
  // a real string equal to nullstr must be quoted so it reads back as a string
  ASSERT_TRUE(ObFileCsvWriter::need_quote(ObString::make_string("NULL"), opts));
  // an empty string is quoted so it does not read back as NULL
  ASSERT_TRUE(ObFileCsvWriter::need_quote(ObString(), opts));

  std::string target = dir_ + "/out.csv";
  {
    ObFileCsvWriter writer;
    ASSERT_EQ(OB_SUCCESS, writer.open(target.c_str(), opts, alloc_));
    ASSERT_EQ(OB_SUCCESS, writer.write_header());
    ASSERT_EQ(OB_SUCCESS, writer.begin_row());
    ASSERT_EQ(OB_SUCCESS, writer.write_field(ObString::make_string("1"), false));
    ASSERT_EQ(OB_SUCCESS, writer.write_field(ObString::make_string("say \"hi\",\nok"), false));
    ASSERT_EQ(OB_SUCCESS, writer.end_row());
    ASSERT_EQ(OB_SUCCESS, writer.begin_row());
    ASSERT_EQ(OB_SUCCESS, writer.write_field(ObString::make_string("2"), false));
    ASSERT_EQ(OB_SUCCESS, writer.write_field(ObString(), true));
    ASSERT_EQ(OB_SUCCESS, writer.end_row());
    // nothing is visible before commit
    struct stat st;
    ASSERT_NE(0, stat(target.c_str(), &st));
    ASSERT_EQ(OB_SUCCESS, writer.commit());
    ASSERT_EQ(2, writer.rows_written());
  }
  ASSERT_EQ("id,note\n1,\"say \"\"hi\"\",\nok\"\n2,NULL\n", read_file(target));

  // the written file reads back with the same values
  ObFileScanOptions ropts;
  ropts.format_ = FILE_FORMAT_CSV;
  ropts.null_str_ = ObString::make_string("NULL");
  ObSEArray<ObFileColumnDesc, 8> cols;
  int64_t rows = 0;
  ASSERT_EQ(OB_SUCCESS, infer(target, ropts, cols, rows));
  ASSERT_EQ(2, rows);
  ASSERT_EQ(FILE_COL_BIGINT, cols.at(0).type_);
  ASSERT_TRUE(cols.at(1).nullable_);

  // an existing target is never overwritten
  {
    ObFileCsvWriter writer;
    ASSERT_EQ(OB_FILE_ALREADY_EXIST, writer.open(target.c_str(), opts, alloc_));
  }
  // abort (or destruction) leaves no temp file and no target
  std::string target2 = dir_ + "/out2.csv";
  {
    ObFileCsvWriter writer;
    ASSERT_EQ(OB_SUCCESS, writer.open(target2.c_str(), opts, alloc_));
    ASSERT_EQ(OB_SUCCESS, writer.write_header());
  }
  struct stat st;
  ASSERT_NE(0, stat(target2.c_str(), &st));
  std::string cmd = "ls '" + dir_ + "' | grep -c tmp";
  FILE *p = popen(cmd.c_str(), "r");
  ASSERT_NE(nullptr, p);
  char buf[16] = {0};
  ASSERT_NE(nullptr, fgets(buf, sizeof(buf), p));
  pclose(p);
  ASSERT_EQ(0, atoi(buf));
}

} // namespace sql
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
