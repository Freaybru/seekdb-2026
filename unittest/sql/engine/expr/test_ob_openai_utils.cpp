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
#include "share/ai_service/ob_ai_service_struct.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/engine/expr/ob_expr_ai/ob_expr_ai_rerank.h"

namespace oceanbase
{
namespace sql
{
class ObAIFuncOpenAIUtilsTest : public ::testing::Test
{
protected:
  static int parse_json(common::ObIAllocator &allocator, const char *text, common::ObIJsonBase *&base)
  {
    common::ObString input(text);
    base = nullptr;
    return common::ObJsonBaseFactory::get_json_base(&allocator, input, common::ObJsonInType::JSON_TREE,
                                                    common::ObJsonInType::JSON_TREE, base);
  }

  static bool contains_text(const common::ObString &text, const char *needle)
  {
    const int64_t needle_len = static_cast<int64_t>(strlen(needle));
    bool found = false;
    if (needle_len <= text.length()) {
      for (int64_t i = 0; i + needle_len <= text.length(); ++i) {
        if (MEMCMP(text.ptr() + i, needle, needle_len) == 0) {
          found = true;
          break;
        }
      }
    }
    return found;
  }
};

TEST_F(ObAIFuncOpenAIUtilsTest, embedding_reorder_and_validation)
{
  using namespace oceanbase::common;
  ObArenaAllocator allocator;
  ObIJsonBase *response_base = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json(allocator,
      "{\"data\":[{\"index\":1,\"embedding\":[0,1]},{\"index\":0,\"embedding\":[1,0]}]}",
      response_base));
  ASSERT_EQ(ObJsonNodeType::J_OBJECT, response_base->json_type());
  ObOpenAIUtils::ObOpenAIEmbed embed;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, embed.parse_output(allocator, static_cast<ObJsonObject *>(response_base), result));
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, result->json_type());
  ObJsonArray *rows = static_cast<ObJsonArray *>(result);
  ASSERT_EQ(2, rows->element_count());
  double first = 0.0;
  double second = 0.0;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::read_json_scalar_double(
      static_cast<ObJsonArray *>(rows->get_value(0))->get_value(0), first));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::read_json_scalar_double(
      static_cast<ObJsonArray *>(rows->get_value(1))->get_value(1), second));
  EXPECT_DOUBLE_EQ(1.0, first);
  EXPECT_DOUBLE_EQ(1.0, second);
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::check_embedding_vector(rows->get_value(0), 2));
  ASSERT_EQ(OB_INVALID_ARGUMENT, ObAIFuncUtils::check_embedding_vector(rows->get_value(0), 3));

  ObIJsonBase *bad_base = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json(allocator, "[\"x\"]", bad_base));
  ASSERT_EQ(OB_INVALID_ARGUMENT, ObAIFuncUtils::check_embedding_vector(bad_base, 1));
  ObIJsonBase *empty_base = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json(allocator, "[]", empty_base));
  ASSERT_EQ(OB_INVALID_ARGUMENT, ObAIFuncUtils::check_embedding_vector(empty_base, 0));

  ObIJsonBase *mixed_base = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json(allocator,
      "{\"data\":[{\"index\":0,\"embedding\":[1]},{\"embedding\":[2]}]}", mixed_base));
  ObIJsonBase *mixed_result = nullptr;
  ASSERT_EQ(OB_INVALID_DATA, embed.parse_output(allocator, static_cast<ObJsonObject *>(mixed_base), mixed_result));
}

TEST_F(ObAIFuncOpenAIUtilsTest, complete_response_and_config)
{
  using namespace oceanbase::common;
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIComplete complete;
  ObIJsonBase *empty_base = nullptr;
  ObIJsonBase *ok_base = nullptr;
  ObIJsonBase *empty_result = nullptr;
  ObIJsonBase *ok_result = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json(allocator,
      "{\"choices\":[{\"message\":{\"content\":\"\"}}]}", empty_base));
  ASSERT_EQ(OB_INVALID_DATA, complete.parse_output(allocator, static_cast<ObJsonObject *>(empty_base), empty_result));
  ASSERT_EQ(OB_SUCCESS, parse_json(allocator,
      "{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}", ok_base));
  ASSERT_EQ(OB_SUCCESS, complete.parse_output(allocator, static_cast<ObJsonObject *>(ok_base), ok_result));
  ASSERT_EQ(ObJsonNodeType::J_STRING, ok_result->json_type());

  ObString model("gpt-test");
  ObString system_prompt("");
  ObString content("hello");
  ObIJsonBase *config_base = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json(allocator, "{\"temperature\":0.2,\"max_tokens\":16}", config_base));
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, complete.get_body(allocator, model, system_prompt, content,
                                          static_cast<ObJsonObject *>(config_base), body));
  ObString printed;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::print_json_to_str(allocator, body, printed));
  EXPECT_TRUE(contains_text(printed, "\"temperature\":"));
  EXPECT_TRUE(contains_text(printed, "\"max_tokens\": 16"));
  EXPECT_TRUE(contains_text(printed, "hello"));
  EXPECT_TRUE(contains_text(printed, "gpt-test"));
}

TEST_F(ObAIFuncOpenAIUtilsTest, ollama_request_response_and_empty_key)
{
  using namespace oceanbase::common;
  ObArenaAllocator allocator;
  ObAIFuncIComplete *complete = nullptr;
  ObAIFuncIEmbed *embed = nullptr;
  ObString ollama("ollama");
  ObString ollama_mixed("Ollama");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(allocator, ollama, complete));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_embed_provider(allocator, ollama_mixed, embed));
  ASSERT_NE(nullptr, complete);
  ASSERT_NE(nullptr, embed);

  ObString model("llama3.1");
  ObString system_prompt("");
  ObString content("hello");
  ObIJsonBase *config_base = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json(allocator, "{\"stream\":true}", config_base));
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, complete->get_body(allocator, model, system_prompt, content,
                                           static_cast<ObJsonObject *>(config_base), body));
  ObString printed;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::print_json_to_str(allocator, body, printed));
  EXPECT_TRUE(contains_text(printed, "\"stream\": false"));
  EXPECT_TRUE(contains_text(printed, "\"prompt\": \"hello\""));
  EXPECT_FALSE(contains_text(printed, "\"messages\""));

  ObArray<ObString> headers;
  ObString empty_key("");
  ASSERT_EQ(OB_SUCCESS, complete->get_header(allocator, empty_key, headers));
  ASSERT_EQ(1, headers.count());
  EXPECT_TRUE(contains_text(headers.at(0), "Content-Type: application/json"));

  ObArray<ObString> inputs;
  ObString one("a");
  ObString two("b");
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(one));
  ASSERT_EQ(OB_SUCCESS, inputs.push_back(two));
  ObJsonObject *embed_body = nullptr;
  ASSERT_EQ(OB_SUCCESS, embed->get_body(allocator, model, inputs, nullptr, embed_body));
  ObString embed_printed;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::print_json_to_str(allocator, embed_body, embed_printed));
  EXPECT_TRUE(contains_text(embed_printed, "\"input\": [\"a\", \"b\"]"));

  ObIJsonBase *embed_response = nullptr;
  ObIJsonBase *embed_result = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json(allocator, "{\"embeddings\":[[1,0],[0,1]]}", embed_response));
  ASSERT_EQ(OB_SUCCESS, embed->parse_output(allocator, static_cast<ObJsonObject *>(embed_response), embed_result));
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, embed_result->json_type());
  ASSERT_EQ(2, embed_result->element_count());

  ObIJsonBase *bad_complete = nullptr;
  ObIJsonBase *bad_result = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json(allocator, "{\"response\":\"\"}", bad_complete));
  ASSERT_EQ(OB_INVALID_DATA, complete->parse_output(allocator, static_cast<ObJsonObject *>(bad_complete), bad_result));

  share::ObAiModelEndpointInfo endpoint;
  ObIJsonBase *endpoint_json = nullptr;
  ObString endpoint_name("ollama_ep");
  ASSERT_EQ(OB_SUCCESS, parse_json(allocator,
      "{\"ai_model_name\":\"m\",\"url\":\"http://127.0.0.1:8000/api/generate\",\"provider\":\"ollama\",\"access_key\":\"\"}",
      endpoint_json));
  ASSERT_EQ(OB_SUCCESS, endpoint.parse_from_json_base(allocator, endpoint_name, *endpoint_json));
  EXPECT_EQ(0, endpoint.get_provider().case_compare("OLLAMA"));
  EXPECT_TRUE(endpoint.get_encrypted_access_key().empty());

  share::ObAiModelEndpointInfo openai_endpoint;
  ObIJsonBase *openai_json = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json(allocator,
      "{\"ai_model_name\":\"m\",\"url\":\"http://127.0.0.1:8000/v1/chat/completions\",\"provider\":\"openai\",\"access_key\":\"\"}",
      openai_json));
  ASSERT_EQ(OB_AI_FUNC_PARAM_EMPTY, openai_endpoint.parse_from_json_base(allocator, endpoint_name, *openai_json));
}

TEST_F(ObAIFuncOpenAIUtilsTest, rerank_global_index_and_stable_order)
{
  using namespace oceanbase::common;
  ObArenaAllocator allocator;
  ObIJsonBase *batch_base = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json(allocator,
      "[{\"index\":1,\"relevance_score\":0.2},{\"index\":0,\"relevance_score\":0.9},{\"index\":2,\"relevance_score\":0.9}]",
      batch_base));
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, batch_base->json_type());
  const int64_t doc_count = 25;
  char seen[25];
  MEMSET(seen, 0, sizeof(seen));
  ObExprAIRerank::RerankHit hits[8];
  int64_t collected = 0;
  ASSERT_EQ(OB_SUCCESS, ObExprAIRerank::absorb_rerank_batch_result(
      allocator, static_cast<ObJsonArray *>(batch_base), 20, 3, doc_count, seen, hits, collected));
  ASSERT_EQ(3, collected);
  EXPECT_EQ(21, hits[0].index_);
  EXPECT_EQ(20, hits[1].index_);
  EXPECT_EQ(22, hits[2].index_);
  ObExprAIRerank::sort_rerank_hits(hits, collected);
  EXPECT_EQ(20, hits[0].index_);
  EXPECT_DOUBLE_EQ(0.9, hits[0].score_);
  EXPECT_EQ(22, hits[1].index_);
  EXPECT_DOUBLE_EQ(0.9, hits[1].score_);
  EXPECT_EQ(21, hits[2].index_);
  EXPECT_DOUBLE_EQ(0.2, hits[2].score_);

  ObJsonArray *sorted_nodes = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObExprAIRerank::append_sorted_hit_nodes(allocator, hits, collected, sorted_nodes));
  ASSERT_EQ(3, sorted_nodes->element_count());
  int64_t first_index = -1;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::read_json_scalar_int(
      static_cast<ObJsonObject *>(sorted_nodes->get_value(0))->get_value("index"), first_index));
  EXPECT_EQ(20, first_index);

  ObIJsonBase *duplicate_base = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json(allocator,
      "[{\"index\":0,\"relevance_score\":1},{\"index\":0,\"relevance_score\":2}]", duplicate_base));
  char seen_dup[2] = {0, 0};
  ObExprAIRerank::RerankHit dup_hits[2];
  int64_t dup_count = 0;
  ASSERT_EQ(OB_INVALID_DATA, ObExprAIRerank::absorb_rerank_batch_result(
      allocator, static_cast<ObJsonArray *>(duplicate_base), 0, 2, 2, seen_dup, dup_hits, dup_count));

  ObIJsonBase *docs_base = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json(allocator,
      "[{\"id\":\"a\"},{\"id\":\"b\"},{\"id\":\"c\"}]", docs_base));
  ObExprAIRerank::RerankHit object_hits[2];
  object_hits[0].index_ = 2;
  object_hits[0].score_ = 0.8;
  object_hits[0].node_ = nullptr;
  object_hits[1].index_ = 0;
  object_hits[1].score_ = 0.1;
  object_hits[1].node_ = nullptr;
  ObJsonArray *sorted_docs = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObExprAIRerank::build_sorted_object_docs(
      allocator, static_cast<ObJsonArray *>(docs_base), object_hits, 2, sorted_docs));
  ASSERT_EQ(2, sorted_docs->element_count());
  ObJsonObject *first_doc = static_cast<ObJsonObject *>(sorted_docs->get_value(0));
  ObJsonNode *score_node = first_doc->get_value("_model_score");
  double score = 0.0;
  ASSERT_NE(nullptr, score_node);
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::read_json_scalar_double(score_node, score));
  EXPECT_DOUBLE_EQ(0.8, score);
  EXPECT_EQ(0, static_cast<ObJsonString *>(first_doc->get_value("id"))->get_str().compare("c"));
}

} // namespace sql
} // namespace oceanbase

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
