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

// Parser tests for the file-as-database grammar: read_csv / read_json / read_ndjson /
// read_parquet / glob table functions with named options and aliases, DESCRIBE of a
// query over a file, and COPY (query | table) TO 'path' (options).

#include <gtest/gtest.h>
#include <string.h>
#include <string>
#include "lib/allocator/page_arena.h"
#include "sql/parser/ob_parser.h"
#include "sql/parser/parse_node.h"

namespace oceanbase
{
namespace sql
{
using namespace common;

class TestFileSqlParser : public ::testing::Test
{
protected:
  int parse(const char *sql, ParseResult &result)
  {
    ObParser parser(alloc_, SMO_DEFAULT);
    memset(&result, 0, sizeof(result));
    return parser.parse(ObString::make_string(sql), result);
  }
  static const ParseNode *find(const ParseNode *node, ObItemType type)
  {
    const ParseNode *found = nullptr;
    if (nullptr != node) {
      if (node->type_ == type) {
        found = node;
      }
      for (int64_t i = 0; nullptr == found && i < node->num_child_; ++i) {
        found = find(node->children_[i], type);
      }
    }
    return found;
  }
  static int64_t count(const ParseNode *node, ObItemType type)
  {
    int64_t cnt = 0;
    if (nullptr != node) {
      cnt += (node->type_ == type) ? 1 : 0;
      for (int64_t i = 0; i < node->num_child_; ++i) {
        cnt += count(node->children_[i], type);
      }
    }
    return cnt;
  }
  static std::string str(const ParseNode *node)
  {
    return nullptr == node ? std::string() : std::string(node->str_value_, node->str_len_);
  }
  const ParseNode *parse_ok(const char *sql)
  {
    ParseResult result;
    int ret = parse(sql, result);
    EXPECT_EQ(OB_SUCCESS, ret) << sql;
    return OB_SUCCESS == ret ? result.result_tree_ : nullptr;
  }
  void parse_fail(const char *sql)
  {
    ParseResult result;
    EXPECT_NE(OB_SUCCESS, parse(sql, result)) << sql;
  }
  ObArenaAllocator alloc_;
};

TEST_F(TestFileSqlParser, table_functions)
{
  const char *funcs[] = {"read_csv", "READ_CSV", "read_csv_auto", "read_json", "read_json_auto",
                         "read_ndjson", "read_parquet", "parquet_scan", "glob"};
  for (int64_t i = 0; i < static_cast<int64_t>(sizeof(funcs) / sizeof(funcs[0])); ++i) {
    std::string sql = std::string("select * from ") + funcs[i] + "('/tmp/x')";
    const ParseNode *tree = parse_ok(sql.c_str());
    const ParseNode *fn = find(tree, T_FILE_TABLE_FUNCTION);
    ASSERT_NE(nullptr, fn) << sql;
    ASSERT_EQ(4, fn->num_child_);
    ASSERT_EQ(std::string(funcs[i]), str(fn->children_[0]));
    ASSERT_EQ("/tmp/x", str(fn->children_[1]));
    ASSERT_EQ(nullptr, fn->children_[2]);
    ASSERT_EQ(nullptr, fn->children_[3]);
  }
}

TEST_F(TestFileSqlParser, options_and_alias)
{
  const ParseNode *tree = parse_ok(
      "select t.id from read_csv('/d/a.csv', header = true, delim = '|', sample_size = -1, "
      "nullstr = 'NA', skip = 2) as t where t.id > 1");
  const ParseNode *fn = find(tree, T_FILE_TABLE_FUNCTION);
  ASSERT_NE(nullptr, fn);
  const ParseNode *opts = fn->children_[2];
  ASSERT_NE(nullptr, opts);
  ASSERT_EQ(T_FILE_TABLE_OPTION_LIST, opts->type_);
  ASSERT_EQ(5, opts->num_child_);
  ASSERT_EQ("header", str(opts->children_[0]->children_[0]));
  ASSERT_EQ("delim", str(opts->children_[1]->children_[0]));
  ASSERT_EQ("|", str(opts->children_[1]->children_[1]));
  const ParseNode *sample = opts->children_[2]->children_[1];
  ASSERT_EQ(T_INT, sample->type_);
  ASSERT_EQ(-1, sample->value_);
  ASSERT_NE(nullptr, fn->children_[3]);
  ASSERT_EQ("t", str(fn->children_[3]));

  // alias without AS
  tree = parse_ok("select * from read_json('/d/a.json', format = 'array') j");
  fn = find(tree, T_FILE_TABLE_FUNCTION);
  ASSERT_NE(nullptr, fn);
  ASSERT_EQ("j", str(fn->children_[3]));
}

TEST_F(TestFileSqlParser, joins_and_subqueries)
{
  const ParseNode *tree = parse_ok(
      "select a.id, b.name from read_csv('/d/a.csv') a left join read_ndjson('/d/b.jsonl') b "
      "on a.id = b.id join read_parquet('/d/c.parquet') c on c.id = a.id "
      "where a.id in (select id from read_csv('/d/a.csv'))");
  ASSERT_EQ(4, count(tree, T_FILE_TABLE_FUNCTION));
  tree = parse_ok("select count(*) from glob('/d/*.csv')");
  ASSERT_EQ(1, count(tree, T_FILE_TABLE_FUNCTION));
}

TEST_F(TestFileSqlParser, describe)
{
  const ParseNode *tree = parse_ok("describe select * from read_csv('/d/a.csv')");
  ASSERT_NE(nullptr, find(tree, T_EXPLAIN));
  ASSERT_NE(nullptr, find(tree, T_FILE_TABLE_FUNCTION));
  tree = parse_ok("desc select id from read_parquet('/d/a.parquet')");
  ASSERT_NE(nullptr, find(tree, T_FILE_TABLE_FUNCTION));
}

TEST_F(TestFileSqlParser, copy_to)
{
  const ParseNode *tree = parse_ok(
      "copy (select id, name from read_csv('/d/a.csv') order by id) to '/tmp/out.csv' "
      "(format csv, header true, delimiter '|', quote '\"', escape '\\\\', nullstr 'NULL')");
  const ParseNode *copy = find(tree, T_COPY_TO);
  ASSERT_NE(nullptr, copy);
  ASSERT_EQ(4, copy->num_child_);
  ASSERT_NE(nullptr, copy->children_[0]);
  ASSERT_EQ(nullptr, copy->children_[1]);
  ASSERT_EQ("/tmp/out.csv", str(copy->children_[2]));
  const ParseNode *opts = copy->children_[3];
  ASSERT_NE(nullptr, opts);
  ASSERT_EQ(T_COPY_OPTION_LIST, opts->type_);
  ASSERT_EQ(6, opts->num_child_);
  ASSERT_EQ("format", str(opts->children_[0]->children_[0]));
  ASSERT_EQ("|", str(opts->children_[2]->children_[1]));

  tree = parse_ok("copy t1 to '/tmp/t1.csv'");
  copy = find(tree, T_COPY_TO);
  ASSERT_NE(nullptr, copy);
  ASSERT_EQ(nullptr, copy->children_[0]);
  ASSERT_NE(nullptr, copy->children_[1]);
  ASSERT_EQ(nullptr, copy->children_[3]);
  ASSERT_EQ("t1", std::string(copy->str_value_, copy->str_len_));

  tree = parse_ok("copy db1.t1 to '/tmp/t1.csv' (header = false, delimiter = ';')");
  copy = find(tree, T_COPY_TO);
  ASSERT_NE(nullptr, copy);
  ASSERT_EQ(2, copy->children_[3]->num_child_);
}

TEST_F(TestFileSqlParser, errors_and_compat)
{
  parse_fail("select * from read_csv()");
  parse_fail("select * from read_csv('/d/a.csv', header)");
  parse_fail("select * from read_csv(concat('/d', '/a.csv'))");
  parse_fail("select * from not_a_file_func('/d/a.csv')");
  parse_fail("copy (select 1) to");
  parse_fail("copy (select 1)");
  // ordinary SQL keeps working, including identifiers named like the new keywords
  parse_ok("select 1");
  parse_ok("create table copy_t (glob int, read_csv int)");
  parse_ok("select glob, read_csv from copy_t");
  parse_ok("select * from t1 join t2 on t1.a = t2.a");
}

} // namespace sql
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
