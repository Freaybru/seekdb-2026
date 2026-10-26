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
#include "ob_expr_ai_rerank.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"
 
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_RERANK, 
                    N_AI_RERANK, 
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIRerank::~ObExprAIRerank() 
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_tc(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_tc(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (doc_key_type == ObJsonType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {  
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_query = nullptr;
  ObDatum *arg_documents = nullptr;
  ObDatum *arg_doc_key = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_query, arg_documents, arg_doc_key))) {
  } else if (arg_model_id->is_null() || arg_query->is_null() || arg_documents->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query or documents is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_RERANK));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString query = arg_query->get_string();
    ObArray<ObString> header_array;
    ObJsonArray *document_array = nullptr;
    ObJsonArray *result_array = nullptr;
    ObIJsonBase *j_base = nullptr;
    bool is_null_result = false;
    // A SQL NULL doc key selects the string-array protocol.
    const bool use_doc_key = OB_NOT_NULL(arg_doc_key) && !arg_doc_key->is_null();
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, temp_allocator, DOCUMENTS_IDX,
          j_base, is_null_result))) {
    } else if (OB_ISNULL(j_base)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (j_base->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is not array");
    } else if (OB_ISNULL(document_array = static_cast<ObJsonArray *>(j_base))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (document_array->element_count() == 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is empty");
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < document_array->element_count(); i++) {
        ObJsonNode *doc_node = document_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (use_doc_key && doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else if (!use_doc_key && doc_node->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not string");
        }
      }
    }
    
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else if (use_doc_key) {
      ObString doc_key = arg_doc_key->get_string();
      if (OB_FAIL(eval_ai_rerank_with_doc_key(expr, ctx, temp_allocator, model_id,
                  query, document_array, doc_key, *info, *endpoint_info, res))) {
      }
    } else {
      RerankHit *hits = nullptr;
      int64_t hit_count = 0;
      if (OB_ISNULL(info)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
      } else if (OB_FAIL(ObAIFuncUtils::get_header(temp_allocator, *info, *endpoint_info, header_array))) {
      } else if (OB_FAIL(collect_rerank_hits(temp_allocator, *info, *endpoint_info, header_array,
                                             query, document_array, hits, hit_count))) {
      } else if (OB_FAIL(append_sorted_hit_nodes(temp_allocator, hits, hit_count, result_array))) {
      } else {
        ObString raw_str;
        if (OB_FAIL(result_array->get_raw_binary(raw_str, &temp_allocator))) {
        } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank_with_doc_key(const ObExpr &expr, ObEvalCtx &ctx, ObIAllocator &allocator, 
                                                ObString& model_id, ObString& query, ObJsonArray *document_array,
                                                ObString& doc_key, const ObAIFuncExprInfo &info,
                                                const share::ObAiModelEndpointInfo &endpoint_info, ObDatum &res)
{
  INIT_SUCC(ret);
  ObJsonArray *doc_array = nullptr;
  ObJsonArray *sorted_document_array = nullptr;
  ObArray<ObString> header_array;
  RerankHit *hits = nullptr;
  int64_t hit_count = 0;
  if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(&info))) {
  } else if (OB_FAIL(ObAIFuncUtils::get_header(allocator, info, endpoint_info, header_array))) {
  } else if (OB_FAIL(get_doc_array_from_documents_array_with_key(allocator, document_array, doc_key, doc_array))) {
  } else if (OB_FAIL(collect_rerank_hits(allocator, info, endpoint_info, header_array, query, doc_array, hits, hit_count))) {
  } else if (OB_FAIL(build_sorted_object_docs(allocator, document_array, hits, hit_count, sorted_document_array))) {
  } else {
    ObString raw_str;
    if (OB_FAIL(sorted_document_array->get_raw_binary(raw_str, &allocator))) {
    } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
    }
  }
  UNUSED(model_id);
  return ret;
}

int ObExprAIRerank::absorb_rerank_batch_result(ObIAllocator &allocator,
                                               ObJsonArray *batch_result,
                                               int64_t start,
                                               int64_t batch_len,
                                               int64_t doc_count,
                                               char *seen,
                                               RerankHit *hits,
                                               int64_t &collected)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(batch_result) || OB_ISNULL(seen) || OB_ISNULL(hits) || batch_len <= 0 || doc_count <= 0) {
    ret = OB_INVALID_DATA;
  } else if (batch_result->element_count() == 0) {
    ret = OB_INVALID_DATA;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model result is empty");
  } else {
    for (uint64_t i = 0; OB_SUCC(ret) && i < batch_result->element_count(); ++i) {
      ObJsonNode *item = batch_result->get_value(i);
      if (OB_ISNULL(item) || item->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_DATA;
      } else {
        ObJsonObject *obj = static_cast<ObJsonObject *>(item);
        ObJsonNode *index_node = obj->get_value(INDEX_KEY);
        ObJsonNode *score_node = obj->get_value(SCORE_KEY);
        int64_t local_index = 0;
        double score = 0.0;
        if (OB_ISNULL(index_node) || OB_ISNULL(score_node)) {
          ret = OB_INVALID_DATA;
        } else if (OB_FAIL(ObAIFuncUtils::read_json_scalar_int(index_node, local_index))) {
          ret = OB_INVALID_DATA;
        } else if (OB_FAIL(ObAIFuncUtils::read_json_scalar_double(score_node, score))) {
          ret = OB_INVALID_DATA;
        } else if (local_index < 0 || local_index >= batch_len) {
          ret = OB_INVALID_DATA;
        } else {
          const int64_t global = local_index + start;
          if (global < 0 || global >= doc_count || seen[global] != 0) {
            ret = OB_INVALID_DATA;
          } else if (index_node->json_type() == ObJsonNodeType::J_INT
                     || index_node->json_type() == ObJsonNodeType::J_OINT) {
            static_cast<ObJsonInt *>(index_node)->set_value(global);
          } else {
            ObJsonInt *global_index = nullptr;
            if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, global, global_index))) {
            } else if (OB_FAIL(obj->add(INDEX_KEY, global_index))) {
            }
          }
          if (OB_SUCC(ret)) {
            seen[global] = 1;
            hits[collected].index_ = global;
            hits[collected].score_ = score;
            hits[collected].node_ = obj;
            ++collected;
          }
        }
      }
    }
  }
  return ret;
}

void ObExprAIRerank::sort_rerank_hits(RerankHit *hits, int64_t count)
{
  if (OB_ISNULL(hits) || count <= 1) {
    return;
  }
  for (int64_t i = 1; i < count; ++i) {
    RerankHit key = hits[i];
    int64_t j = i - 1;
    while (j >= 0 && (hits[j].score_ < key.score_
                      || (hits[j].score_ == key.score_ && hits[j].index_ > key.index_))) {
      hits[j + 1] = hits[j];
      --j;
    }
    hits[j + 1] = key;
  }
}

int ObExprAIRerank::collect_rerank_hits(ObIAllocator &allocator,
                                        const ObAIFuncExprInfo &info,
                                        const share::ObAiModelEndpointInfo &endpoint_info,
                                        ObArray<ObString> &header_array,
                                        ObString &query,
                                        ObJsonArray *text_array,
                                        RerankHit *&hits,
                                        int64_t &hit_count)
{
  INIT_SUCC(ret);
  hits = nullptr;
  hit_count = 0;
  const int64_t batch_size = 20;
  if (OB_ISNULL(text_array) || text_array->element_count() == 0) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const int64_t doc_count = static_cast<int64_t>(text_array->element_count());
    RerankHit *all_hits = static_cast<RerankHit *>(allocator.alloc(sizeof(RerankHit) * doc_count));
    char *seen = static_cast<char *>(allocator.alloc(doc_count));
    int64_t collected = 0;
    if (OB_ISNULL(all_hits) || OB_ISNULL(seen)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMSET(seen, 0, doc_count);
      for (int64_t start = 0; OB_SUCC(ret) && start < doc_count; start += batch_size) {
        int64_t end = start + batch_size;
        if (end > doc_count) {
          end = doc_count;
        }
        ObJsonArray *batch_document_array = nullptr;
        ObJsonArray *batch_result_array = nullptr;
        if (OB_FAIL(construct_batch_document_array(allocator, text_array, start, end, batch_document_array))) {
        } else if (OB_FAIL(inner_eval_ai_rerank(allocator, info, endpoint_info, header_array, query,
                                                batch_document_array, batch_result_array))) {
        } else if (OB_FAIL(absorb_rerank_batch_result(allocator, batch_result_array, start, end - start,
                                                      doc_count, seen, all_hits, collected))) {
        }
      }
      if (OB_SUCC(ret) && collected == 0) {
        ret = OB_INVALID_DATA;
      }
      if (OB_SUCC(ret)) {
        sort_rerank_hits(all_hits, collected);
        hits = all_hits;
        hit_count = collected;
      }
    }
  }
  return ret;
}

int ObExprAIRerank::append_sorted_hit_nodes(ObIAllocator &allocator, RerankHit *hits, int64_t hit_count, ObJsonArray *&result_array)
{
  INIT_SUCC(ret);
  ObJsonArray *array = nullptr;
  if (OB_ISNULL(hits) || hit_count <= 0) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, array))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < hit_count; ++i) {
      if (OB_ISNULL(hits[i].node_)) {
        ret = OB_INVALID_DATA;
      } else if (OB_FAIL(array->append(hits[i].node_))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    result_array = array;
  }
  return ret;
}

int ObExprAIRerank::build_sorted_object_docs(ObIAllocator &allocator,
                                             ObJsonArray *document_array,
                                             RerankHit *hits,
                                             int64_t hit_count,
                                             ObJsonArray *&sorted_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *sorted = nullptr;
  if (OB_ISNULL(document_array) || OB_ISNULL(hits) || hit_count <= 0) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, sorted))) {
  } else {
    const int64_t doc_count = static_cast<int64_t>(document_array->element_count());
    for (int64_t i = 0; OB_SUCC(ret) && i < hit_count; ++i) {
      ObJsonNode *origin = nullptr;
      ObJsonObject *cloned = nullptr;
      ObJsonDouble *score = nullptr;
      if (hits[i].index_ < 0 || hits[i].index_ >= doc_count) {
        ret = OB_INVALID_DATA;
      } else if (OB_ISNULL(origin = document_array->get_value(static_cast<uint64_t>(hits[i].index_)))
                 || origin->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_DATA;
      } else if (OB_ISNULL(cloned = static_cast<ObJsonObject *>(origin->clone(&allocator, true)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_ISNULL(score = OB_NEWx(ObJsonDouble, &allocator, hits[i].score_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(cloned->add(MODEL_SCORE_KEY, score))) {
      } else if (OB_FAIL(sorted->append(cloned))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    sorted_document_array = sorted;
  }
  return ret;
}

int ObExprAIRerank::get_doc_array_from_documents_array_with_key(ObIAllocator &allocator, ObJsonArray *document_object_array, ObString &doc_key, ObJsonArray *&doc_array)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(document_object_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (doc_key.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonArray *doc_array_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, doc_array_obj))) {
    } else {
      int64_t count = document_object_array->element_count();
      ObJsonNode *doc_node = nullptr;
      ObJsonNode *doc_value_node = nullptr;
      ObJsonObject *doc_obj = nullptr;
      for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
        doc_node = document_object_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is null");
        } else if (doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else {
          doc_obj = static_cast<ObJsonObject *>(doc_node);
          doc_value_node = doc_obj->get_value(doc_key);
          if (OB_ISNULL(doc_value_node)) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is null");
          } else if (doc_value_node->json_type() != ObJsonNodeType::J_STRING) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is not string");
          } else if (OB_FAIL(doc_array_obj->append(doc_value_node))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      doc_array = doc_array_obj;
    }
  }
  return ret;
}

int ObExprAIRerank::sort_document_array_by_model_result(ObIAllocator &allocator, ObJsonArray *document_array, ObJsonArray *model_result_array, ObJsonArray *&sorted_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *sorted_document_array_obj = nullptr;
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, sorted_document_array_obj))) {
  } else {
    int64_t count = model_result_array->element_count();
    for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
      ObJsonObject *model_result_obj = static_cast<ObJsonObject *>(model_result_array->get_value(i));
      if (OB_ISNULL(model_result_obj)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObJsonInt *index_value = static_cast<ObJsonInt *>(model_result_obj->get_value(INDEX_KEY));
        ObJsonDouble *score_value = static_cast<ObJsonDouble *>(model_result_obj->get_value(SCORE_KEY));
        if (OB_ISNULL(index_value) || OB_ISNULL(score_value)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          int64_t index = index_value->value();
          ObJsonObject *origin_obj = static_cast<ObJsonObject *>(document_array->get_value(index));
          ObJsonObject *new_obj = nullptr;
          if (OB_ISNULL(origin_obj)) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_ISNULL(new_obj = static_cast<ObJsonObject *>(origin_obj->clone(&allocator, true)))) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(new_obj->add(MODEL_SCORE_KEY, score_value))) {
          } else if (OB_FAIL(sorted_document_array_obj->append(new_obj))) {
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    sorted_document_array = sorted_document_array_obj;
  }
  return ret;
}

int ObExprAIRerank::construct_batch_document_array(ObIAllocator &allocator, ObJsonArray *document_array, int64_t start_idx, int64_t end_idx, ObJsonArray *&batch_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *result_array = nullptr;
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    for (int64_t i = start_idx; i < end_idx; i++) {
      if (OB_FAIL(result_array->append(document_array->get_value(i)))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    batch_document_array = result_array;
  }
  return ret;
}

int ObExprAIRerank::inner_eval_ai_rerank(ObIAllocator &allocator, 
                                          const ObAIFuncExprInfo &info, 
                                          const share::ObAiModelEndpointInfo &endpoint_info,
                                          ObArray<ObString> &header_array, 
                                          ObString &query, 
                                          ObJsonArray *document_array, 
                                          ObJsonArray *&result_array)
{
  INIT_SUCC(ret);
  ObJsonArray *res = nullptr;
  ObJsonObject *config_json = nullptr;
  ObJsonObject *body = nullptr;
  ObJsonObject *http_response = nullptr;
  ObIJsonBase *response = nullptr;
  ObAIFuncClient ai_client;
  if (OB_FAIL(ObAIFuncUtils::get_rerank_body(allocator, info, endpoint_info, query, document_array, config_json, body))) {
  } else if (OB_FAIL(ai_client.send_post(allocator, endpoint_info.get_url(), header_array, body, http_response))) {
  } else if (OB_FAIL(ObAIFuncUtils::parse_rerank_output(allocator, endpoint_info, http_response, response))) {
  } else if (response->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(res = static_cast<ObJsonArray *>(response))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    result_array = res;
  }
  return ret;
}

int ObExprAIRerank::batch_result_add_base(ObIAllocator &allocator, ObJsonArray *array, int64_t start_idx)
{
  INIT_SUCC(ret);
  int64_t count = array->element_count();
  ObString index_key(INDEX_KEY);
  for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
    ObJsonObject *obj = static_cast<ObJsonObject *>(array->get_value(i));
    if (OB_ISNULL(obj)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObJsonInt *int_obj = static_cast<ObJsonInt *>(obj->get_value(index_key));
      if (OB_ISNULL(int_obj)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        int64_t id = int_obj->value() + start_idx;
        int_obj->set_value(id);
      }
    }
  }
  return ret;
}

int ObExprAIRerank::compact_json_array_by_key(ObIAllocator &allocator, ObJsonArray *array1, ObJsonArray *array2, ObString &score_key, ObJsonArray *&compact_array)
{
  INIT_SUCC(ret);
  ObJsonArray *result_array = nullptr;
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    int64_t count1 = array1->element_count();
    int64_t count2 = array2->element_count();
    int64_t min_count = count1 < count2 ? count1 : count2;
    int64_t i = 0;
    int64_t j = 0;
    while (i < min_count && j < min_count) {
      ObJsonObject *obj1 = static_cast<ObJsonObject *>(array1->get_value(i));
      ObJsonObject *obj2 = static_cast<ObJsonObject *>(array2->get_value(j));
      if (OB_ISNULL(obj1) || OB_ISNULL(obj2)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObJsonDouble *double1 = static_cast<ObJsonDouble *>(obj1->get_value(score_key));
        ObJsonDouble *double2 = static_cast<ObJsonDouble *>(obj2->get_value(score_key));
        if (OB_ISNULL(double1) || OB_ISNULL(double2)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          if (double1->value() > double2->value()) {
            if (OB_FAIL(result_array->append(obj1))) {
            }
            i++;
          } else {
            if (OB_FAIL(result_array->append(obj2))) {
            }
            j++;
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      while (i < count1) {
        if (OB_FAIL(result_array->append(array1->get_value(i)))) {
        }
        i++;
      }
    }
    if (OB_SUCC(ret)) {
      while (j < count2) {
        if (OB_FAIL(result_array->append(array2->get_value(j)))) {
        }
        j++;
      }
    }
  }
  if (OB_SUCC(ret)) {
    compact_array = result_array;
  }
  return ret;
}

int ObExprAIRerank::construct_config_json(ObIAllocator &allocator, int64_t top_n, int64_t return_doc, ObJsonObject *&config_json)
{
  INIT_SUCC(ret);
  ObJsonObject *config_obj = nullptr;
  ObJsonInt *top_n_obj = nullptr;
  ObJsonBoolean *return_doc_obj = nullptr;
  bool return_doc_value = true;
  if (return_doc <= 0) {
    return_doc_value = false;
  }
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config_obj))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_boolean(allocator, return_doc_value, return_doc_obj))) {
  } else if (OB_FAIL(config_obj->add("return_documents", return_doc_obj))) {
  } else if (top_n > 0) {
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, top_n, top_n_obj))) {
    } else if (OB_FAIL(config_obj->add("top_n", top_n_obj))) {
    }
  }
  if (OB_SUCC(ret)) {
    config_json = config_obj;
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   bool got_data = false;
  //   ObObj const_data;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else {
  //     ObAIFuncExprInfo *info = OB_NEWx(ObAIFuncExprInfo, allocator, *allocator, T_FUN_SYS_AI_RERANK);
  //     if (OB_ISNULL(info)) {
  //       ret = OB_ALLOCATE_MEMORY_FAILED;
  //       LOG_WARN("failed to allocate memory for ai rerank info", K(ret));
  //     } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //       LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //     } else {
  //       rt_expr.extra_info_ = info;
  //     }
  //   }
  // }
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
