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

// Schema-level tests for AI COLUMN: column flags that must survive a schema
// round trip through __all_column, hidden column naming/typing, and the
// ObAIColumnDef metadata object (serialization, deep copy, id remapping).

#include <gtest/gtest.h>
#include <string>
#include "lib/allocator/page_arena.h"
#include "share/schema/ob_schema_struct.h"
#include "share/schema/ob_column_schema.h"
#include "sql/ai_column/ob_ai_column_define.h"
#include "sql/ai_column/ob_ai_column_resolver_helper.h"

namespace oceanbase
{
namespace sql
{
using namespace common;
using namespace share::schema;

static std::string s(const ObString &str) { return std::string(str.ptr(), str.length()); }

static void fill_def(ObAIColumnDef &def)
{
  def.reset();
  def.table_id_ = 500010;
  def.column_id_ = 19;
  def.column_name_ = ObString::make_string("tags");
  def.state_column_id_ = 20;
  def.request_column_id_ = 21;
  def.result_spec_column_id_ = 22;
  def.func_type_ = AI_COLUMN_FUNC_COMPLETE;
  def.model_name_ = ObString::make_string("m");
  def.input_expr_ = ObString::make_string("concat('Classify: ', content)");
  def.normalized_expr_ = ObString::make_string("AI_COMPLETE('m', concat('Classify: ', content))");
  def.type_prompt_ = ObString::make_string(ObAIColumnUtil::JSON_TYPE_PROMPT);
  def.sync_mode_ = ObString::make_string("async");
  def.max_retries_ = 2;
  def.def_version_ = 3;
  def.result_type_ = ObJsonType;
  def.dependent_column_ids_.push_back(18);
  def.dependent_column_ids_.push_back(17);
}

TEST(TestAIColumnSchema, flags_are_persisted)
{
  // __all_column only stores the lower 32 bits of column_flags; AI column flags
  // must be among them or the CG/DML layers lose them after a schema refresh.
  EXPECT_EQ(AI_RESULT_COLUMN_FLAG, AI_RESULT_COLUMN_FLAG & STORED_COLUMN_FLAGS_MASK);
  EXPECT_EQ(AI_HIDDEN_COLUMN_FLAG, AI_HIDDEN_COLUMN_FLAG & STORED_COLUMN_FLAGS_MASK);
  EXPECT_NE(AI_RESULT_COLUMN_FLAG, AI_HIDDEN_COLUMN_FLAG);
  ObColumnSchemaV2 col;
  col.add_column_flag(AI_RESULT_COLUMN_FLAG);
  EXPECT_TRUE(col.is_ai_result_column());
  EXPECT_FALSE(col.is_ai_hidden_column());
  ObColumnSchemaV2 reloaded;
  reloaded.set_column_flags(col.get_stored_column_flags());
  EXPECT_TRUE(reloaded.is_ai_result_column());
  // no overlap with existing flags
  const int64_t others[] = {VIRTUAL_GENERATED_COLUMN_FLAG, STORED_GENERATED_COLUMN_FLAG,
                            INVISIBLE_COLUMN_FLAG, HEAP_ALTER_ROWKEY_FLAG,
                            HEAP_TABLE_SORT_ROWKEY_FLAG, COLUMN_NOT_NULL_CONSTRAINT_FLAG,
                            UNUSED_COLUMN_FLAG, STRING_LOB_COLUMN_FLAG, HEAP_TABLE_PRIMARY_KEY_FLAG};
  for (int64_t f : others) {
    EXPECT_EQ(0, f & (AI_RESULT_COLUMN_FLAG | AI_HIDDEN_COLUMN_FLAG));
  }
}

TEST(TestAIColumnSchema, hidden_column_names)
{
  ObArenaAllocator alloc;
  ObString name;
  ASSERT_EQ(OB_SUCCESS, ObAIColumnUtil::build_hidden_column_name(AI_HIDDEN_STATE,
      ObString::make_string("tags"), alloc, name));
  EXPECT_EQ("__ai_state_tags", s(name));
  EXPECT_TRUE(ObAIColumnUtil::is_hidden_column_name(name));
  ASSERT_EQ(OB_SUCCESS, ObAIColumnUtil::build_hidden_column_name(AI_HIDDEN_REQUEST,
      ObString::make_string("tags"), alloc, name));
  EXPECT_EQ("__ai_req_tags", s(name));
  ASSERT_EQ(OB_SUCCESS, ObAIColumnUtil::build_hidden_column_name(AI_HIDDEN_RESULT_SPEC,
      ObString::make_string("tags"), alloc, name));
  EXPECT_EQ("__ai_spec_tags", s(name));
  EXPECT_TRUE(ObAIColumnUtil::is_hidden_column_name(name));
  EXPECT_FALSE(ObAIColumnUtil::is_hidden_column_name(ObString::make_string("tags")));
}

TEST(TestAIColumnSchema, hidden_columns_built_for_result_column)
{
  ObArenaAllocator alloc;
  ObAIColumnDef def;
  fill_def(def);
  ObColumnSchemaV2 result_col;
  result_col.set_column_name("tags");
  result_col.set_data_type(ObJsonType);
  result_col.add_column_flag(AI_RESULT_COLUMN_FLAG);
  ObSEArray<ObColumnSchemaV2, 3> hidden;
  ASSERT_EQ(OB_SUCCESS, ObAIColumnResolveHelper::build_hidden_columns(alloc, def, result_col, hidden));
  ASSERT_EQ(3, hidden.count());
  for (int64_t i = 0; i < hidden.count(); ++i) {
    EXPECT_TRUE(hidden.at(i).is_hidden());
    EXPECT_TRUE(hidden.at(i).is_ai_hidden_column());
    EXPECT_FALSE(hidden.at(i).is_ai_result_column());
    EXPECT_TRUE(hidden.at(i).is_nullable());
    EXPECT_TRUE(ObAIColumnUtil::is_hidden_column_name(hidden.at(i).get_column_name_str()));
    EXPECT_EQ(hidden.at(i).get_column_flags(),
              hidden.at(i).get_column_flags() & STORED_COLUMN_FLAGS_MASK);
  }
}

TEST(TestAIColumnSchema, def_serialize_round_trip)
{
  ObAIColumnDef def;
  fill_def(def);
  ASSERT_TRUE(def.is_valid());
  char buf[4096];
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, def.serialize(buf, sizeof(buf), pos));
  EXPECT_EQ(def.get_serialize_size(), pos);
  ObAIColumnDef out;
  int64_t rpos = 0;
  ASSERT_EQ(OB_SUCCESS, out.deserialize(buf, pos, rpos));
  EXPECT_EQ(pos, rpos);
  EXPECT_EQ(def.table_id_, out.table_id_);
  EXPECT_EQ(def.column_id_, out.column_id_);
  EXPECT_EQ(s(def.column_name_), s(out.column_name_));
  EXPECT_EQ(def.state_column_id_, out.state_column_id_);
  EXPECT_EQ(def.result_spec_column_id_, out.result_spec_column_id_);
  EXPECT_EQ(s(def.model_name_), s(out.model_name_));
  EXPECT_EQ(s(def.normalized_expr_), s(out.normalized_expr_));
  EXPECT_EQ(s(def.type_prompt_), s(out.type_prompt_));
  EXPECT_EQ(def.max_retries_, out.max_retries_);
  EXPECT_EQ(def.def_version_, out.def_version_);
  ASSERT_EQ(2, out.dependent_column_ids_.count());
  EXPECT_EQ(18u, out.dependent_column_ids_.at(0));
}

TEST(TestAIColumnSchema, deep_copy_and_remap)
{
  ObArenaAllocator alloc;
  ObAIColumnDef def;
  fill_def(def);
  ObAIColumnDef copy;
  ASSERT_EQ(OB_SUCCESS, copy.deep_copy(def, alloc));
  EXPECT_NE(def.model_name_.ptr(), copy.model_name_.ptr());
  EXPECT_EQ(s(def.input_expr_), s(copy.input_expr_));
  ObSEArray<ObAIColumnIdPair, 4> pairs;
  pairs.push_back(ObAIColumnIdPair(19, 30));
  pairs.push_back(ObAIColumnIdPair(20, 31));
  pairs.push_back(ObAIColumnIdPair(18, 16));
  ASSERT_EQ(OB_SUCCESS, copy.remap_column_ids(pairs));
  EXPECT_EQ(30u, copy.column_id_);
  EXPECT_EQ(31u, copy.state_column_id_);
  EXPECT_EQ(21u, copy.request_column_id_);  // not in the map: kept
  EXPECT_EQ(AI_HIDDEN_STATE, 0);
  EXPECT_EQ(31u, copy.get_hidden_column_id(AI_HIDDEN_STATE));
  EXPECT_TRUE(copy.depends_on(16));
  EXPECT_TRUE(copy.depends_on(17));
  EXPECT_FALSE(copy.depends_on(18));
}

TEST(TestAIColumnSchema, dependent_ids_text)
{
  ObArenaAllocator alloc;
  ObAIColumnDef def;
  fill_def(def);
  ObString text;
  ASSERT_EQ(OB_SUCCESS, def.dependent_ids_to_string(alloc, text));
  EXPECT_EQ("18,17", s(text));
  ObAIColumnDef parsed;
  ASSERT_EQ(OB_SUCCESS, parsed.parse_dependent_ids(text));
  ASSERT_EQ(2, parsed.dependent_column_ids_.count());
  EXPECT_EQ(17u, parsed.dependent_column_ids_.at(1));
  ASSERT_EQ(OB_SUCCESS, parsed.parse_dependent_ids(ObString()));
  EXPECT_EQ(0, parsed.dependent_column_ids_.count());
}

} // namespace sql
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
