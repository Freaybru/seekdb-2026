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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/ai_column/ob_ai_column_resolver_helper.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "query/vector/ob_vector_index_util.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

namespace oceanbase
{
using namespace common;
using namespace share::schema;
namespace sql
{

int ObAIColumnSchemaLookup::find_column(const ObString &name, bool &exists, uint64_t &column_id,
                                        bool &is_ai_column, bool &is_hidden) const
{
  int ret = OB_SUCCESS;
  exists = false;
  column_id = OB_INVALID_ID;
  is_ai_column = false;
  is_hidden = false;
  const ObColumnSchemaV2 *col = new_schema_.get_column_schema(name);
  if (OB_ISNULL(col) && OB_NOT_NULL(base_schema_)) {
    col = base_schema_->get_column_schema(name);
  }
  if (OB_NOT_NULL(col)) {
    exists = true;
    column_id = col->get_column_id();
    is_ai_column = col->is_ai_result_column();
    is_hidden = col->is_hidden() || col->is_ai_hidden_column();
  }
  for (int64_t i = 0; !is_ai_column && i < pending_ai_names_.count(); ++i) {
    if (0 == pending_ai_names_.at(i).case_compare(name)) {
      is_ai_column = true;
      exists = true;
    }
  }
  return ret;
}

static bool ai_col_parse_int(const ObString &text, int64_t &value)
{
  bool ok = text.length() > 0;
  value = 0;
  for (int64_t i = 0; ok && i < text.length(); ++i) {
    const char c = text.ptr()[i];
    if (c >= '0' && c <= '9') {
      value = value * 10 + (c - '0');
      ok = value < (INT64_C(1) << 40);
    } else {
      ok = false;
    }
  }
  return ok;
}

int ObAIColumnResolveHelper::build_definition(ObIAllocator &alloc,
                                              const ObString &column_name,
                                              const ObObjType result_type,
                                              const int64_t column_vector_dim,
                                              const ObString &expr_text,
                                              const ObIArray<ObString> &dep_names,
                                              const ObIArray<ObAIColumnOption> &options,
                                              const ObAIColumnLookup &lookup,
                                              ObAIColumnDef &def)
{
  int ret = OB_SUCCESS;
  ObString func_name;
  ObSEArray<ObString, 4> args;
  def.reset();
  def.result_type_ = result_type;
  if (OB_FAIL(ob_write_string(alloc, column_name, def.column_name_))) {
    LOG_WARN("copy failed", K(ret));
  } else if (OB_FAIL(ObAIColumnUtil::split_function_call(expr_text, func_name, args))) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN expression other than AI_COMPLETE()/AI_EMBED() call");
  } else if (FALSE_IT(def.func_type_ = ObAIColumnUtil::get_func_type(func_name))) {
  } else if (AI_COLUMN_FUNC_INVALID == def.func_type_) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN expression other than AI_COMPLETE()/AI_EMBED() call");
  } else if (args.count() < 2
             || (AI_COLUMN_FUNC_COMPLETE == def.func_type_ && args.count() > 2)
             || (AI_COLUMN_FUNC_EMBED == def.func_type_ && args.count() > 3)) {
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name.length(), func_name.ptr());
  } else {
    bool is_literal = false;
    ObString model;
    if (OB_FAIL(ObAIColumnUtil::unquote_string_literal(args.at(0), alloc, model, is_literal))) {
      LOG_WARN("unquote failed", K(ret));
    } else if (!is_literal || model.empty()) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN model name that is not a constant string");
    } else {
      def.model_name_ = model;
    }
    if (OB_SUCC(ret) && OB_FAIL(ob_write_string(alloc, args.at(1), def.input_expr_))) {
      LOG_WARN("copy failed", K(ret));
    }
  }
  // result type vs function
  if (OB_SUCC(ret)) {
    const bool is_vector = ObCollectionSQLType == result_type;
    if (AI_COLUMN_FUNC_EMBED == def.func_type_) {
      if (!is_vector || column_vector_dim <= 0) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI_EMBED AI COLUMN whose type is not VECTOR(n)");
      } else if (3 == args.count()) {
        int64_t dim = 0;
        if (!ai_col_parse_int(args.at(2), dim)) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI_EMBED dimension that is not an integer constant");
        } else if (dim != column_vector_dim) {
          ret = OB_ERR_INVALID_VECTOR_DIM;
          LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, static_cast<uint32_t>(column_vector_dim),
                         static_cast<uint32_t>(dim));
        }
      }
      def.vector_dim_ = column_vector_dim;
    } else if (is_vector || ObCollectionSQLType == result_type) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI_COMPLETE AI COLUMN of VECTOR type");
    } else if (ob_is_geometry(result_type)
               || ob_is_enum_or_set_type(result_type)) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN of this result type");
    }
  }
  // dependencies: ordinary existing columns only, no AI column chains
  for (int64_t i = 0; OB_SUCC(ret) && i < dep_names.count(); ++i) {
    bool exists = false;
    bool is_ai = false;
    bool is_hidden = false;
    uint64_t column_id = OB_INVALID_ID;
    const ObString &name = dep_names.at(i);
    if (OB_FAIL(lookup.find_column(name, exists, column_id, is_ai, is_hidden))) {
      LOG_WARN("find column failed", K(ret));
    } else if (is_ai || 0 == name.case_compare(column_name)) {
      ret = OB_NOT_SUPPORTED;  // AI column chains are not required / supported
      LOG_WARN("AI column depends on another AI column", K(ret), K(name), K(column_name));
    } else if (!exists || is_hidden) {
      ret = OB_ERR_BAD_FIELD_ERROR;
      LOG_USER_ERROR(OB_ERR_BAD_FIELD_ERROR, name.length(), name.ptr(),
                     column_name.length(), column_name.ptr());
    } else if (OB_FAIL(def.dependent_column_names_.push_back(name))) {
      LOG_WARN("push back failed", K(ret));
    } else if (OB_INVALID_ID != column_id && !def.depends_on(column_id)
               && OB_FAIL(def.dependent_column_ids_.push_back(column_id))) {
      LOG_WARN("push back failed", K(ret));
    }
  }
  if (OB_SUCC(ret) && 0 == dep_names.count()) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN whose input does not reference any column");
  }
  // options
  bool has_sync_mode = false;
  for (int64_t i = 0; OB_SUCC(ret) && i < options.count(); ++i) {
    const ObAIColumnOption &opt = options.at(i);
    if (0 == opt.name_.case_compare("sync_mode")) {
      if (opt.is_int_ || opt.is_null_ || 0 != opt.str_value_.case_compare("async")) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN sync_mode other than 'async'");
      } else {
        has_sync_mode = true;
      }
    } else if (0 == opt.name_.case_compare("max_retries")) {
      if (!opt.is_int_ || opt.int_value_ < 0 || opt.int_value_ > ObAIColumnUtil::MAX_RETRIES_LIMIT) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "AI COLUMN max_retries, must be an integer in [0, 100]");
      } else {
        def.max_retries_ = opt.int_value_;
      }
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "unknown AI COLUMN option");
    }
  }
  UNUSED(has_sync_mode);
  if (OB_SUCC(ret)) {
    def.sync_mode_ = ObString::make_string("async");
    def.def_version_ = 1;
    const char *prompt = (AI_COLUMN_FUNC_COMPLETE == def.func_type_)
                         ? ObAIColumnUtil::get_type_prompt(result_type) : NULL;
    if (OB_NOT_NULL(prompt)) {
      def.type_prompt_ = ObString::make_string(prompt);
    }
    if (OB_FAIL(ObAIColumnUtil::build_normalized_expr(def.func_type_, def.model_name_,
                                                      def.input_expr_, def.type_prompt_,
                                                      def.vector_dim_, alloc,
                                                      def.normalized_expr_))) {
      LOG_WARN("build normalized expr failed", K(ret));
    }
  }
  return ret;
}

int ObAIColumnResolveHelper::collect_column_refs(const ParseNode *node, ObIArray<ObString> &names)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) {
  } else if (T_COLUMN_REF == node->type_) {
    const ParseNode *col = (node->num_child_ >= 3) ? node->children_[2] : NULL;
    if (OB_ISNULL(col) || T_STAR == col->type_) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN input with '*'");
    } else {
      ObString name(static_cast<int32_t>(col->str_len_), col->str_value_);
      bool found = false;
      for (int64_t i = 0; !found && i < names.count(); ++i) {
        found = (0 == names.at(i).case_compare(name));
      }
      if (!found && OB_FAIL(names.push_back(name))) {
        LOG_WARN("push back failed", K(ret));
      }
    }
  } else if (T_SELECT == node->type_ || T_USER_VARIABLE_IDENTIFIER == node->type_
             || T_SYSTEM_VARIABLE == node->type_ || T_QUESTIONMARK == node->type_) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN input with subquery or variable");
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < node->num_child_; ++i) {
      if (OB_FAIL(collect_column_refs(node->children_[i], names))) {
      }
    }
  }
  return ret;
}

int ObAIColumnResolveHelper::resolve_options(const ParseNode *option_list,
                                             ObIArray<ObAIColumnOption> &options)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(option_list)) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < option_list->num_child_; ++i) {
      const ParseNode *opt = option_list->children_[i];
      ObAIColumnOption option;
      if (OB_ISNULL(opt) || T_AI_COLUMN_OPTION != opt->type_ || opt->num_child_ != 2
          || OB_ISNULL(opt->children_[0]) || OB_ISNULL(opt->children_[1])) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid option node", K(ret));
      } else {
        const ParseNode *v = opt->children_[1];
        option.name_.assign_ptr(opt->children_[0]->str_value_,
                                static_cast<int32_t>(opt->children_[0]->str_len_));
        if (T_INT == v->type_ || T_UINT64 == v->type_) {
          option.is_int_ = true;
          option.int_value_ = v->value_;
        } else if (T_NULL == v->type_) {
          option.is_null_ = true;
        } else {
          option.str_value_.assign_ptr(v->str_value_, static_cast<int32_t>(v->str_len_));
        }
        for (int64_t j = 0; OB_SUCC(ret) && j < options.count(); ++j) {
          if (0 == options.at(j).name_.case_compare(option.name_)) {
            ret = OB_ERR_PARSER_SYNTAX;
            LOG_WARN("duplicate AI column option", K(ret), K(option));
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(options.push_back(option))) {
          LOG_WARN("push back failed", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObAIColumnResolveHelper::check_model(ObIAllocator &alloc,
                                         ObSchemaGetterGuard &schema_guard,
                                         const ObAIColumnDef &def)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *info = NULL;
  if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(alloc, def.model_name_, schema_guard, info))) {
    LOG_WARN("get ai model failed", K(ret), K(def.model_name_));
  } else if (OB_ISNULL(info)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (AI_COLUMN_FUNC_COMPLETE == def.func_type_
             && OB_FAIL(ObAIFuncUtils::check_info_type_completion(info))) {
    LOG_WARN("model is not a completion model", K(ret));
  } else if (AI_COLUMN_FUNC_EMBED == def.func_type_
             && OB_FAIL(ObAIFuncUtils::check_info_type_dense_embedding(info))) {
    LOG_WARN("model is not an embedding model", K(ret));
  }
  return ret;
}

int ObAIColumnResolveHelper::resolve_attr(ObIAllocator &alloc,
                                          const ObColumnSchemaV2 &column,
                                          const ParseNode &attr_node,
                                          const ObAIColumnLookup &lookup,
                                          ObSchemaGetterGuard *schema_guard,
                                          ObAIColumnDef &def)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObString, 4> dep_names;
  ObSEArray<ObAIColumnOption, 4> options;
  int64_t vector_dim = 0;
  const ParseNode *expr_node = attr_node.num_child_ > 0 ? attr_node.children_[0] : NULL;
  const ParseNode *opt_node = attr_node.num_child_ > 1 ? attr_node.children_[1] : NULL;
  if (T_AI_COLUMN_ATTR != attr_node.type_ || OB_ISNULL(expr_node)
      || OB_ISNULL(expr_node->str_value_) || expr_node->str_len_ <= 0) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid AI column attr node", K(ret));
  } else if (column.is_collection()
             && OB_FAIL(share::ObVectorIndexUtil::get_vector_dim_from_extend_type_info(
                 column.get_extended_type_info(), vector_dim))) {
    // not a dense vector column
    ret = OB_SUCCESS;
    vector_dim = 0;
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(collect_column_refs(expr_node, dep_names))) {
    LOG_WARN("collect column refs failed", K(ret));
  } else if (OB_FAIL(resolve_options(opt_node, options))) {
    LOG_WARN("resolve options failed", K(ret));
  } else if (OB_FAIL(build_definition(alloc, column.get_column_name_str(),
                                      column.get_data_type(), vector_dim,
                                      ObString(static_cast<int32_t>(expr_node->str_len_),
                                               expr_node->str_value_),
                                      dep_names, options, lookup, def))) {
    LOG_WARN("build AI column definition failed", K(ret));
  } else if (OB_NOT_NULL(schema_guard) && OB_FAIL(check_model(alloc, *schema_guard, def))) {
    LOG_WARN("check model failed", K(ret));
  }
  return ret;
}

int ObAIColumnResolveHelper::build_hidden_columns(ObIAllocator &alloc,
                                                  const ObAIColumnDef &def,
                                                  const ObColumnSchemaV2 &result_column,
                                                  ObIArray<ObColumnSchemaV2> &hidden_columns)
{
  int ret = OB_SUCCESS;
  for (int64_t k = 0; OB_SUCC(ret) && k < AI_HIDDEN_MAX; ++k) {
    ObColumnSchemaV2 col;
    ObString name;
    ObObj default_value;
    const ObAIColumnHiddenKind kind = static_cast<ObAIColumnHiddenKind>(k);
    col.set_table_id(result_column.get_table_id());
    col.set_is_hidden(true);
    col.add_column_flag(AI_HIDDEN_COLUMN_FLAG);
    col.set_nullable(true);
    col.set_rowkey_position(0);
    if (OB_FAIL(ObAIColumnUtil::build_hidden_column_name(kind, def.column_name_, alloc, name))) {
      LOG_WARN("build hidden column name failed", K(ret));
    } else if (OB_FAIL(col.set_column_name(name))) {
      LOG_WARN("set column name failed", K(ret));
    } else if (AI_HIDDEN_REQUEST == kind) {
      col.set_data_type(ObVarcharType);
      col.set_charset_type(CHARSET_BINARY);
      col.set_collation_type(CS_TYPE_BINARY);
      col.set_data_length(64);
      default_value.set_null();
    } else {
      col.set_data_type(ObIntType);
      col.set_charset_type(CHARSET_BINARY);
      col.set_collation_type(CS_TYPE_BINARY);
      col.set_data_precision(20);
      col.set_data_scale(0);
      if (AI_HIDDEN_STATE == kind) {
        default_value.set_int(AI_ROW_PENDING);
      } else {
        default_value.set_null();
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(col.set_cur_default_value(default_value, false))) {
      LOG_WARN("set default failed", K(ret));
    } else if (OB_FAIL(col.set_orig_default_value(default_value))) {
      LOG_WARN("set default failed", K(ret));
    } else if (OB_FAIL(hidden_columns.push_back(col))) {
      LOG_WARN("push back failed", K(ret));
    }
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
