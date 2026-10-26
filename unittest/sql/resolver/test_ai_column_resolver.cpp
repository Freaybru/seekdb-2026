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

// Resolver tests for AI COLUMN definitions: the validation core
// (ObAIColumnResolveHelper::build_definition) driven from real parse trees,
// plus the text utilities used by the resolver and RETRY.

#include <gtest/gtest.h>
#include <string.h>
#include <string>
#include <vector>
#include "lib/allocator/page_arena.h"
#include "sql/parser/ob_parser.h"
#include "sql/parser/parse_node.h"
#include "sql/ai_column/ob_ai_column_define.h"
#include "sql/ai_column/ob_ai_column_resolver_helper.h"

namespace oceanbase
{
namespace sql
{
using namespace common;

// table columns visible to the AI column being resolved
class MockLookup : public ObAIColumnLookup
{
public:
  struct Col { std::string name_; uint64_t id_; bool ai_; bool hidden_; };
  void add(const char *name, uint64_t id, bool ai = false, bool hidden = false)
  {
    cols_.push_back(Col{name, id, ai, hidden});
  }
  virtual int find_column(const ObString &name, bool &exists, uint64_t &column_id,
                          bool &is_ai_column, bool &is_hidden) const override
  {
    exists = false;
    column_id = OB_INVALID_ID;
    is_ai_column = false;
    is_hidden = false;
    for (const Col &c : cols_) {
      if (0 == name.case_compare(ObString::make_string(c.name_.c_str()))) {
        exists = true;
        column_id = c.id_;
        is_ai_column = c.ai_;
        is_hidden = c.hidden_;
      }
    }
    return OB_SUCCESS;
  }
private:
  std::vector<Col> cols_;
};

class TestAIColumnResolver : public ::testing::Test
{
protected:
  void SetUp() override
  {
    lookup_.add("id", 16);
    lookup_.add("title", 17);
    lookup_.add("content", 18);
    lookup_.add("summary", 19, true /*AI column*/);
    lookup_.add("__ai_state_summary", 20, false, true /*hidden*/);
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
  // Parse "create table t (..., <column_ddl>)" and resolve its AI COLUMN attribute.
  int resolve(const char *column_ddl, ObObjType type, int64_t dim, ObAIColumnDef &def)
  {
    int ret = OB_SUCCESS;
    std::string sql = std::string("create table t (id bigint primary key, ") + column_ddl + ")";
    ObParser parser(alloc_, SMO_DEFAULT);
    ParseResult result;
    memset(&result, 0, sizeof(result));
    ObSEArray<ObString, 4> deps;
    ObSEArray<ObAIColumnOption, 4> options;
    const ParseNode *attr = nullptr;
    char *buf = static_cast<char *>(alloc_.alloc(sql.size() + 1));
    memcpy(buf, sql.c_str(), sql.size() + 1);
    if (OB_FAIL(parser.parse(ObString::make_string(buf), result))) {
    } else if (nullptr == (attr = find(result.result_tree_, T_AI_COLUMN_ATTR))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ObAIColumnResolveHelper::collect_column_refs(attr->children_[0], deps))) {
    } else if (OB_FAIL(ObAIColumnResolveHelper::resolve_options(attr->children_[1], options))) {
    } else {
      ObString expr_text(static_cast<int32_t>(attr->children_[0]->str_len_),
                         attr->children_[0]->str_value_);
      ret = ObAIColumnResolveHelper::build_definition(alloc_, ObString::make_string("c"), type, dim,
                                                      expr_text, deps, options, lookup_, def);
    }
    return ret;
  }
  static std::string s(const ObString &str) { return std::string(str.ptr(), str.length()); }
  ObArenaAllocator alloc_;
  MockLookup lookup_;
};

TEST_F(TestAIColumnResolver, complete_json)
{
  ObAIColumnDef def;
  ASSERT_EQ(OB_SUCCESS, resolve("c json AI COLUMN (AI_COMPLETE('p0', concat('Classify: ', content))) "
                                "WITH (sync_mode='async', max_retries=2)",
                                ObJsonType, 0, def));
  EXPECT_EQ(AI_COLUMN_FUNC_COMPLETE, def.func_type_);
  EXPECT_EQ("p0", s(def.model_name_));
  EXPECT_EQ("concat('Classify: ', content)", s(def.input_expr_));
  EXPECT_EQ(2, def.max_retries_);
  EXPECT_EQ("async", s(def.sync_mode_));
  ASSERT_EQ(1, def.dependent_column_ids_.count());
  EXPECT_EQ(18u, def.dependent_column_ids_.at(0));
  // the type contract is part of the persisted, normalized definition
  EXPECT_EQ(s(ObString::make_string(ObAIColumnUtil::JSON_TYPE_PROMPT)), s(def.type_prompt_));
  EXPECT_NE(std::string::npos, s(def.normalized_expr_).find("Return exactly one valid JSON value"));
}

TEST_F(TestAIColumnResolver, type_prompts)
{
  EXPECT_STREQ(ObAIColumnUtil::INTEGER_TYPE_PROMPT, ObAIColumnUtil::get_type_prompt(ObIntType));
  EXPECT_STREQ(ObAIColumnUtil::JSON_TYPE_PROMPT, ObAIColumnUtil::get_type_prompt(ObJsonType));
  EXPECT_EQ(nullptr, ObAIColumnUtil::get_type_prompt(ObVarcharType));
  ObAIColumnDef def;
  ASSERT_EQ(OB_SUCCESS, resolve("c int AI COLUMN (AI_COMPLETE('p0', content))", ObIntType, 0, def));
  EXPECT_EQ(s(ObString::make_string(ObAIColumnUtil::INTEGER_TYPE_PROMPT)), s(def.type_prompt_));
  // character results get no extra instruction
  ObAIColumnDef def2;
  ASSERT_EQ(OB_SUCCESS, resolve("c varchar(100) AI COLUMN (AI_COMPLETE('p0', content))",
                                ObVarcharType, 0, def2));
  EXPECT_TRUE(def2.type_prompt_.empty());
}

TEST_F(TestAIColumnResolver, embed_dimension)
{
  ObAIColumnDef def;
  ASSERT_EQ(OB_SUCCESS, resolve("c vector(4) AI COLUMN (AI_EMBED('e', content))",
                                ObCollectionSQLType, 4, def));
  EXPECT_EQ(AI_COLUMN_FUNC_EMBED, def.func_type_);
  EXPECT_EQ(4, def.vector_dim_);
  ObAIColumnDef def2;
  EXPECT_EQ(OB_SUCCESS, resolve("c vector(4) AI COLUMN (AI_EMBED('e', content, 4))",
                                ObCollectionSQLType, 4, def2));
  ObAIColumnDef def3;
  EXPECT_EQ(OB_ERR_INVALID_VECTOR_DIM, resolve("c vector(4) AI COLUMN (AI_EMBED('e', content, 3))",
                                                ObCollectionSQLType, 4, def3));
  // AI_EMBED must produce a VECTOR, AI_COMPLETE must not
  ObAIColumnDef def4;
  EXPECT_EQ(OB_NOT_SUPPORTED, resolve("c text AI COLUMN (AI_EMBED('e', content))",
                                      ObLongTextType, 0, def4));
  ObAIColumnDef def5;
  EXPECT_EQ(OB_NOT_SUPPORTED, resolve("c vector(4) AI COLUMN (AI_COMPLETE('m', content))",
                                      ObCollectionSQLType, 4, def5));
}

TEST_F(TestAIColumnResolver, dependencies)
{
  // depending on another AI column is rejected (no chains)
  ObAIColumnDef def;
  EXPECT_EQ(OB_NOT_SUPPORTED, resolve("c varchar(100) AI COLUMN (AI_COMPLETE('m', summary))",
                                      ObVarcharType, 0, def));
  // unknown and hidden columns are not visible
  ObAIColumnDef def2;
  EXPECT_EQ(OB_ERR_BAD_FIELD_ERROR, resolve("c varchar(100) AI COLUMN (AI_COMPLETE('m', nope))",
                                            ObVarcharType, 0, def2));
  ObAIColumnDef def3;
  EXPECT_EQ(OB_ERR_BAD_FIELD_ERROR,
            resolve("c varchar(100) AI COLUMN (AI_COMPLETE('m', __ai_state_summary))",
                    ObVarcharType, 0, def3));
  // self reference
  ObAIColumnDef def4;
  EXPECT_EQ(OB_NOT_SUPPORTED, resolve("c varchar(100) AI COLUMN (AI_COMPLETE('m', c))",
                                      ObVarcharType, 0, def4));
  // constant input
  ObAIColumnDef def5;
  EXPECT_EQ(OB_NOT_SUPPORTED, resolve("c varchar(100) AI COLUMN (AI_COMPLETE('m', 'x'))",
                                      ObVarcharType, 0, def5));
  // several columns, deduplicated
  ObAIColumnDef def6;
  ASSERT_EQ(OB_SUCCESS, resolve("c varchar(100) AI COLUMN "
                                "(AI_COMPLETE('m', concat(title, content, title)))",
                                ObVarcharType, 0, def6));
  EXPECT_EQ(2, def6.dependent_column_ids_.count());
  EXPECT_TRUE(def6.depends_on(17));
  EXPECT_TRUE(def6.depends_on(18));
  EXPECT_FALSE(def6.depends_on(16));
}

TEST_F(TestAIColumnResolver, options)
{
  ObAIColumnDef def;
  EXPECT_EQ(OB_NOT_SUPPORTED, resolve("c text AI COLUMN (AI_COMPLETE('m', content)) "
                                      "WITH (sync_mode='sync')", ObLongTextType, 0, def));
  ObAIColumnDef def2;
  EXPECT_EQ(OB_INVALID_ARGUMENT, resolve("c text AI COLUMN (AI_COMPLETE('m', content)) "
                                         "WITH (max_retries=101)", ObLongTextType, 0, def2));
  ObAIColumnDef def3;
  EXPECT_EQ(OB_NOT_SUPPORTED, resolve("c text AI COLUMN (AI_COMPLETE('m', content)) "
                                      "WITH (foo=1)", ObLongTextType, 0, def3));
  ObAIColumnDef def4;
  EXPECT_EQ(OB_ERR_PARSER_SYNTAX, resolve("c text AI COLUMN (AI_COMPLETE('m', content)) "
                                          "WITH (max_retries=1, max_retries=2)",
                                          ObLongTextType, 0, def4));
  ObAIColumnDef def5;
  ASSERT_EQ(OB_SUCCESS, resolve("c text AI COLUMN (AI_COMPLETE('m', content)) WITH (max_retries=0)",
                                ObLongTextType, 0, def5));
  EXPECT_EQ(0, def5.max_retries_);
}

TEST_F(TestAIColumnResolver, function_shape)
{
  ObAIColumnDef def;
  EXPECT_EQ(OB_NOT_SUPPORTED, resolve("c text AI COLUMN (upper(content))", ObLongTextType, 0, def));
  ObAIColumnDef def2;
  EXPECT_EQ(OB_ERR_PARAM_SIZE, resolve("c text AI COLUMN (AI_COMPLETE('m', content, 3))",
                                       ObLongTextType, 0, def2));
  ObAIColumnDef def3;
  EXPECT_EQ(OB_NOT_SUPPORTED, resolve("c text AI COLUMN (AI_COMPLETE(title, content))",
                                      ObLongTextType, 0, def3));
}

TEST_F(TestAIColumnResolver, split_function_call)
{
  ObString name;
  ObSEArray<ObString, 4> args;
  ASSERT_EQ(OB_SUCCESS, ObAIColumnUtil::split_function_call(
      ObString::make_string("AI_COMPLETE('a,b', concat(x, ')'), `c,d`)"), name, args));
  EXPECT_EQ("AI_COMPLETE", s(name));
  ASSERT_EQ(3, args.count());
  EXPECT_EQ("'a,b'", s(args.at(0)));
  EXPECT_EQ("concat(x, ')')", s(args.at(1)));
  EXPECT_EQ("`c,d`", s(args.at(2)));
  EXPECT_EQ(AI_COLUMN_FUNC_COMPLETE, ObAIColumnUtil::get_func_type(ObString::make_string("ai_complete")));
  EXPECT_EQ(AI_COLUMN_FUNC_EMBED, ObAIColumnUtil::get_func_type(ObString::make_string("AI_EMBED")));
  EXPECT_EQ(AI_COLUMN_FUNC_INVALID, ObAIColumnUtil::get_func_type(ObString::make_string("AI_PROMPT")));
}

TEST_F(TestAIColumnResolver, retry_status_rewrite)
{
  ObString out;
  ASSERT_EQ(OB_SUCCESS, ObAIColumnUtil::rewrite_status_pseudo_column(
      ObString::make_string("status = 'FAILED' and `status` = 1 and t.status = 2 and note = 'status'"),
      ObString::make_string("__ai_state_tags"), alloc_, out));
  std::string r = s(out);
  // only the bare pseudo column is rewritten
  EXPECT_NE(std::string::npos, r.find("__ai_state_tags"));
  EXPECT_NE(std::string::npos, r.find("`status` = 1"));
  EXPECT_NE(std::string::npos, r.find("t.status = 2"));
  EXPECT_NE(std::string::npos, r.find("note = 'status'"));
  EXPECT_EQ(std::string::npos, r.find("(status = 'FAILED'"));
  EXPECT_STREQ("FAILED", ObAIColumnUtil::row_state_name(AI_ROW_FAILED));
  EXPECT_STREQ("READY", ObAIColumnUtil::row_state_name(AI_ROW_READY));
  EXPECT_STREQ("PENDING", ObAIColumnUtil::row_state_name(AI_ROW_PENDING));
}

TEST_F(TestAIColumnResolver, escape)
{
  ObString out;
  ASSERT_EQ(OB_SUCCESS, ObAIColumnUtil::escape_sql_string(ObString::make_string("a'b\\c"), alloc_, out));
  EXPECT_EQ("a''b\\\\c", s(out));
}

} // namespace sql
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
