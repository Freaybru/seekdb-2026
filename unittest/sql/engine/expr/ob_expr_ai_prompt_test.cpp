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

#include <gtest/gtest.h>
#include "lib/allocator/page_arena.h"
#include "common/json_type/ob_json_base.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

namespace oceanbase
{
namespace common
{
namespace
{

int parse_object(ObIAllocator &allocator, const char *text, ObJsonObject *&object)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;
  ObString input(text);
  object = nullptr;
  if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, input, ObJsonInType::JSON_TREE,
                                               ObJsonInType::JSON_TREE, base))) {
  } else if (OB_ISNULL(base) || base->json_type() != ObJsonNodeType::J_OBJECT) {
    ret = OB_INVALID_DATA;
  } else {
    object = static_cast<ObJsonObject *>(base);
  }
  return ret;
}

void expect_text(const ObString &actual, const char *expected)
{
  const int64_t expected_len = static_cast<int64_t>(strlen(expected));
  ASSERT_EQ(expected_len, actual.length());
  if (expected_len > 0) {
    ASSERT_EQ(0, MEMCMP(expected, actual.ptr(), expected_len));
  }
}

} // namespace

TEST(ObExprAIPromptTest, json_array_and_object_arguments)
{
  ObArenaAllocator allocator;
  ObJsonObject *prompt = nullptr;
  ObString replaced;
  ASSERT_EQ(OB_SUCCESS, parse_object(allocator,
      "{\"template\":\"obj={0};arr={1}\",\"args\":[{\"a\":\"1\"},[\"a\",\"b\"]]}", prompt));
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, replaced));
  expect_text(replaced, "obj={\"a\": \"1\"};arr=[\"a\", \"b\"]");
}

TEST(ObExprAIPromptTest, repeated_placeholder)
{
  ObArenaAllocator allocator;
  ObJsonObject *prompt = nullptr;
  ObString replaced;
  ASSERT_EQ(OB_SUCCESS, parse_object(allocator, "{\"template\":\"{0}{0}{0}\",\"args\":[\"A\"]}", prompt));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, replaced));
  expect_text(replaced, "AAA");
}

TEST(ObExprAIPromptTest, nested_prompt)
{
  ObArenaAllocator allocator;
  ObJsonObject *prompt = nullptr;
  ObString replaced;
  ASSERT_EQ(OB_SUCCESS, parse_object(allocator,
      "{\"template\":\"A{0}B\",\"args\":[{\"template\":\"[{0}]\",\"args\":[\"z\"]}]}", prompt));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, replaced));
  expect_text(replaced, "A[z]B");
}

TEST(ObExprAIPromptTest, illegal_placeholder_index)
{
  ObArenaAllocator allocator;
  ObJsonObject *prompt = nullptr;
  ObString replaced;
  ASSERT_EQ(OB_SUCCESS, parse_object(allocator, "{\"template\":\"{1}\",\"args\":[\"only\"]}", prompt));
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt));
  ASSERT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, replaced));
}

TEST(ObExprAIPromptTest, unicode_and_escapes)
{
  ObArenaAllocator allocator;
  ObJsonObject *unicode_prompt = nullptr;
  ObJsonObject *slash_prompt = nullptr;
  ObJsonObject *brace_prompt = nullptr;
  ObString replaced;
  ASSERT_EQ(OB_SUCCESS, parse_object(allocator, "{\"template\":\"你好{0}\",\"args\":[\"世界\"]}", unicode_prompt));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, unicode_prompt, replaced));
  expect_text(replaced, "你好世界");
  ASSERT_EQ(OB_SUCCESS, parse_object(allocator, "{\"template\":\"\\\\{0\\\\}\",\"args\":[\"NO\"]}", slash_prompt));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, slash_prompt, replaced));
  expect_text(replaced, "{0}");
  ASSERT_EQ(OB_SUCCESS, parse_object(allocator, "{\"template\":\"{{0}}\",\"args\":[\"NO\"]}", brace_prompt));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, brace_prompt, replaced));
  expect_text(replaced, "{0}");
}

} // namespace common
} // namespace oceanbase

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
