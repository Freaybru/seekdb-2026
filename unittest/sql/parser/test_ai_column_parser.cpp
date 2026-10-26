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

// Parser tests for AI COLUMN DDL:
//   <col> <type> AI COLUMN (AI_COMPLETE(...) | AI_EMBED(...)) [WITH (k = v, ...)]
//   ALTER TABLE t RETRY AI COLUMN <col> [WHERE <expr>]

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

class TestAIColumnParser : public ::testing::Test
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
    return (nullptr == node || nullptr == node->str_value_)
           ? std::string() : std::string(node->str_value_, node->str_len_);
  }
  ObArenaAllocator alloc_;
};

TEST_F(TestAIColumnParser, create_table_with_ai_columns)
{
  ParseResult result;
  const char *sql =
      "create table docs (id bigint primary key, content varchar(200), "
      "tags json AI COLUMN (AI_COMPLETE('m1', concat('Classify: ', content))) "
      "WITH (sync_mode='async', max_retries=2), "
      "emb vector(4) AI COLUMN (AI_EMBED('m2', content)) WITH (sync_mode='async', max_retries=3))";
  ASSERT_EQ(OB_SUCCESS, parse(sql, result));
  ASSERT_EQ(2, count(result.result_tree_, T_AI_COLUMN_ATTR));
  ASSERT_EQ(2, count(result.result_tree_, T_AI_COLUMN_OPTION_LIST));
  ASSERT_EQ(4, count(result.result_tree_, T_AI_COLUMN_OPTION));
  const ParseNode *attr = find(result.result_tree_, T_AI_COLUMN_ATTR);
  ASSERT_NE(nullptr, attr);
  ASSERT_EQ(2, attr->num_child_);
  ASSERT_NE(nullptr, attr->children_[0]);
  // the expression text is kept verbatim for the metadata
  EXPECT_EQ("AI_COMPLETE('m1', concat('Classify: ', content))", str(attr->children_[0]));
  const ParseNode *opts = attr->children_[1];
  ASSERT_NE(nullptr, opts);
  ASSERT_EQ(T_AI_COLUMN_OPTION_LIST, opts->type_);
  ASSERT_EQ(2, opts->num_child_);
  EXPECT_EQ("sync_mode", str(opts->children_[0]->children_[0]));
  EXPECT_EQ("max_retries", str(opts->children_[1]->children_[0]));
  EXPECT_EQ(2, opts->children_[1]->children_[1]->value_);
}

TEST_F(TestAIColumnParser, options_are_optional)
{
  ParseResult result;
  ASSERT_EQ(OB_SUCCESS, parse("create table t (id int primary key, c text, "
                              "s varchar(100) AI COLUMN (AI_COMPLETE('m', c)))", result));
  const ParseNode *attr = find(result.result_tree_, T_AI_COLUMN_ATTR);
  ASSERT_NE(nullptr, attr);
  EXPECT_EQ(nullptr, attr->children_[1]);
}

TEST_F(TestAIColumnParser, alter_add_ai_column)
{
  ParseResult result;
  ASSERT_EQ(OB_SUCCESS, parse("alter table t add column tags json AI COLUMN "
                              "(AI_COMPLETE('m', content)) WITH (sync_mode='async', max_retries=1)",
                              result));
  EXPECT_EQ(1, count(result.result_tree_, T_AI_COLUMN_ATTR));
}

TEST_F(TestAIColumnParser, retry_ai_column)
{
  ParseResult result;
  ASSERT_EQ(OB_SUCCESS, parse("alter table t retry ai column tags", result));
  const ParseNode *retry = find(result.result_tree_, T_ALTER_AI_COLUMN_RETRY);
  ASSERT_NE(nullptr, retry);
  ASSERT_EQ(2, retry->num_child_);
  EXPECT_EQ(nullptr, retry->children_[1]);

  ParseResult result2;
  ASSERT_EQ(OB_SUCCESS, parse("alter table t retry ai column tags where status = 'FAILED' and id > 3",
                              result2));
  retry = find(result2.result_tree_, T_ALTER_AI_COLUMN_RETRY);
  ASSERT_NE(nullptr, retry);
  ASSERT_NE(nullptr, retry->children_[1]);
  EXPECT_EQ("status = 'FAILED' and id > 3", str(retry->children_[1]));
}

TEST_F(TestAIColumnParser, syntax_errors)
{
  ParseResult result;
  // AI COLUMN needs a parenthesized expression
  EXPECT_NE(OB_SUCCESS, parse("create table t (id int primary key, c text, "
                              "s text AI COLUMN AI_COMPLETE('m', c))", result));
  // options are key = value pairs
  ParseResult result2;
  EXPECT_NE(OB_SUCCESS, parse("create table t (id int primary key, c text, "
                              "s text AI COLUMN (AI_COMPLETE('m', c)) WITH (async))", result2));
  // RETRY needs a column name
  ParseResult result3;
  EXPECT_NE(OB_SUCCESS, parse("alter table t retry ai column", result3));
}

TEST_F(TestAIColumnParser, ai_is_not_reserved)
{
  // ordinary identifiers named ai / column-like words keep working
  ParseResult result;
  EXPECT_EQ(OB_SUCCESS, parse("create table ai (ai int primary key, retry int)", result));
  ParseResult result2;
  EXPECT_EQ(OB_SUCCESS, parse("select ai, retry from ai", result2));
}

} // namespace sql
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
