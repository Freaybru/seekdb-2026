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
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_content->is_null()) {
    // NULL input propagates as NULL so a filtered or sparse scan does not fail the statement.
    res.set_null();
  } else if (arg_model_id->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or content is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content = arg_content->get_string();
    if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim)) {
      if (arg_dim->is_null()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      } else {
        dim = arg_dim->get_int();
        if (dim <= 0) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
          res.set_null();
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
        } else if (OB_FAIL(config->add("dimensions", dim_json))) {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
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
  //   ObObj const_data;
  //   bool got_data = false;
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
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

namespace {

static const int64_t AI_EMBED_REQUEST_BATCH = 32;

struct EmbedWorkItem {
  int64_t row_idx_;
  ObString model_;
  ObString content_;
  int64_t dim_;
};

int embed_group(ObIAllocator &allocator,
                const ObString &model_id,
                const ObArray<ObString> &contents,
                int64_t dim,
                ObArray<ObString> &results)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *info = nullptr;
  share::ObAiModelEndpointInfo endpoint;
  query::ObIAiEndpointResolver *endpoint_resolver =
      ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
  if (model_id.empty() || contents.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_id, info))) {
  } else if (OB_ISNULL(info) || OB_ISNULL(endpoint_resolver)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(model_id, allocator, endpoint))) {
  } else if (OB_FAIL(ObAIFuncUtils::check_info_type_dense_embedding(info))) {
  } else {
    ObAIFuncModel model(allocator, *info, endpoint);
    for (int64_t start = 0; OB_SUCC(ret) && start < contents.count(); start += AI_EMBED_REQUEST_BATCH) {
      int64_t end = start + AI_EMBED_REQUEST_BATCH;
      if (end > contents.count()) {
        end = contents.count();
      }
      ObArray<ObString> chunk;
      ObArray<ObString> chunk_results;
      ObJsonObject *config = nullptr;
      for (int64_t i = start; OB_SUCC(ret) && i < end; ++i) {
        if (OB_FAIL(chunk.push_back(contents.at(i)))) {
        }
      }
      if (OB_FAIL(ret)) {
      } else if (dim > 0) {
        ObJsonInt *dim_json = nullptr;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dim, dim_json))) {
        } else if (OB_FAIL(config->add("dimensions", dim_json))) {
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(model.call_dense_embedding_vector_v2(chunk, config, chunk_results))) {
      } else if (chunk_results.count() != chunk.count()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("embedding batch result count mismatch", K(ret), K(chunk.count()), K(chunk_results.count()));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < chunk_results.count(); ++i) {
          if (OB_FAIL(results.push_back(chunk_results.at(i)))) {
          }
        }
      }
    }
  }
  return ret;
}

} // namespace

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                       const ObBitVector &skip, const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  ObDatum *results = expr.locate_batch_datums(ctx);
  if (OB_ISNULL(results) || batch_size < 0) {
    ret = OB_ERR_UNEXPECTED;
  } else if (batch_size == 0) {
  } else if (OB_FAIL(expr.args_[MODEL_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else if (OB_FAIL(expr.args_[CONTENT_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else if (expr.arg_cnt_ == 3 && OB_FAIL(expr.args_[DIM_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
    char *is_null_row = static_cast<char *>(temp_allocator.alloc(batch_size));
    char *ready = static_cast<char *>(temp_allocator.alloc(batch_size));
    ObString *result_text = static_cast<ObString *>(temp_allocator.alloc(sizeof(ObString) * batch_size));
    ObArray<EmbedWorkItem> work;
    if (OB_ISNULL(is_null_row) || OB_ISNULL(ready) || OB_ISNULL(result_text)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMSET(is_null_row, 0, batch_size);
      MEMSET(ready, 0, batch_size);
      for (int64_t i = 0; i < batch_size; ++i) {
        new (result_text + i) ObString();
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; ++i) {
      if (skip.at(i) || eval_flags.at(i)) {
        ready[i] = 1;
      } else {
        const ObDatum &content_datum = expr.args_[CONTENT_IDX]->locate_expr_datum(ctx, i);
        if (content_datum.is_null()) {
          is_null_row[i] = 1;
          ready[i] = 1;
        } else {
          const ObDatum &model_datum = expr.args_[MODEL_IDX]->locate_expr_datum(ctx, i);
          EmbedWorkItem item;
          item.row_idx_ = i;
          item.dim_ = 0;
          if (model_datum.is_null()) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or content is null");
          } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(
                         ctx.exec_ctx_, temp_allocator, model_datum, expr.args_[MODEL_IDX]->datum_meta_,
                         expr.args_[MODEL_IDX]->obj_meta_.has_lob_header(), item.model_))) {
          } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(
                         ctx.exec_ctx_, temp_allocator, content_datum, expr.args_[CONTENT_IDX]->datum_meta_,
                         expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), item.content_))) {
          } else if (item.model_.empty() || item.content_.empty()) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
          } else if (expr.arg_cnt_ == 3) {
            const ObDatum &dim_datum = expr.args_[DIM_IDX]->locate_expr_datum(ctx, i);
            if (dim_datum.is_null() || dim_datum.get_int() <= 0) {
              ret = OB_INVALID_ARGUMENT;
              LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
            } else {
              item.dim_ = dim_datum.get_int();
            }
          }
          if (OB_SUCC(ret) && OB_FAIL(work.push_back(item))) {
          }
        }
      }
    }

    char *grouped = nullptr;
    if (OB_FAIL(ret) || work.empty()) {
    } else if (OB_ISNULL(grouped = static_cast<char *>(temp_allocator.alloc(work.count())))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMSET(grouped, 0, work.count());
      for (int64_t i = 0; OB_SUCC(ret) && i < work.count(); ++i) {
        if (grouped[i]) {
          continue;
        }
        ObArray<ObString> contents;
        ObArray<int64_t> rows;
        const ObString model_id = work.at(i).model_;
        const int64_t dim = work.at(i).dim_;
        for (int64_t j = i; OB_SUCC(ret) && j < work.count(); ++j) {
          if (!grouped[j] && work.at(j).dim_ == dim && work.at(j).model_.compare(model_id) == 0) {
            grouped[j] = 1;
            if (OB_FAIL(contents.push_back(work.at(j).content_))) {
            } else if (OB_FAIL(rows.push_back(work.at(j).row_idx_))) {
            }
          }
        }
        ObArray<ObString> embedded;
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(embed_group(temp_allocator, model_id, contents, dim, embedded))) {
        } else if (embedded.count() != rows.count()) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          for (int64_t k = 0; k < rows.count(); ++k) {
            const int64_t row = rows.at(k);
            result_text[row] = embedded.at(k);
            ready[row] = 1;
          }
        }
      }
    }

    for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; ++i) {
      if (skip.at(i) || eval_flags.at(i)) {
      } else if (!ready[i]) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; ++i) {
        if (skip.at(i) || eval_flags.at(i)) {
        } else if (is_null_row[i]) {
          results[i].set_null();
        } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, results[i], result_text[i], i))) {
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; i < batch_size; ++i) {
        if (!skip.at(i) && !eval_flags.at(i)) {
          eval_flags.set(i);
        }
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
