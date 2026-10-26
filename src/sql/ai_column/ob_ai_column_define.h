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

// Async AI COLUMN metadata model and pure helpers.
//
//   <col> <type> AI COLUMN (AI_COMPLETE('model', <expr>) | AI_EMBED('model', <expr>[, dim]))
//         [WITH (sync_mode='async', max_retries=N)]
//
// Every AI column owns three hidden physical columns (is_hidden = 1):
//   __ai_state_<col>  BIGINT  row state      0 PENDING / 1 READY / 2 FAILED
//   __ai_req_<col>    VARCHAR request id of the published result
//   __ai_spec_<col>   BIGINT  definition version the result was produced with
// The definition itself lives in oceanbase.__all_ai_column, pending work in
// oceanbase.__all_ai_column_task (written in the business transaction).

#ifndef OCEANBASE_SQL_AI_COLUMN_OB_AI_COLUMN_DEFINE_H_
#define OCEANBASE_SQL_AI_COLUMN_OB_AI_COLUMN_DEFINE_H_

#include "lib/string/ob_string.h"
#include "lib/container/ob_se_array.h"
#include "lib/container/ob_iarray.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/utility/ob_unify_serialize.h"
#include "lib/utility/ob_print_utils.h"
#include "common/object/ob_obj_type.h"

namespace oceanbase
{
namespace sql
{

enum ObAIColumnFuncType
{
  AI_COLUMN_FUNC_INVALID = -1,
  AI_COLUMN_FUNC_COMPLETE = 0,
  AI_COLUMN_FUNC_EMBED = 1,
};

// value of the hidden state column
enum ObAIColumnRowState
{
  AI_ROW_PENDING = 0,
  AI_ROW_READY = 1,
  AI_ROW_FAILED = 2,
};

// value of __all_ai_column_task.state
enum ObAIColumnTaskState
{
  AI_TASK_PENDING = 0,
  AI_TASK_RUNNING = 1,
  AI_TASK_SUCCEEDED = 2,
  AI_TASK_RETRY_WAIT = 3,
  AI_TASK_FAILED = 4,
};

enum ObAIColumnHiddenKind
{
  AI_HIDDEN_STATE = 0,
  AI_HIDDEN_REQUEST = 1,
  AI_HIDDEN_RESULT_SPEC = 2,
  AI_HIDDEN_MAX
};

struct ObAIColumnIdPair
{
  ObAIColumnIdPair() : old_id_(common::OB_INVALID_ID), new_id_(common::OB_INVALID_ID) {}
  ObAIColumnIdPair(uint64_t old_id, uint64_t new_id) : old_id_(old_id), new_id_(new_id) {}
  TO_STRING_KV(K_(old_id), K_(new_id));
  uint64_t old_id_;
  uint64_t new_id_;
};

struct ObAIColumnDef
{
  OB_UNIS_VERSION(1);
public:
  ObAIColumnDef() { reset(); }
  void reset();
  bool is_valid() const;
  // deep copy all strings into %alloc
  int deep_copy(const ObAIColumnDef &src, common::ObIAllocator &alloc);
  // remap every column id (result, hidden and dependencies); ids missing from
  // %pairs are kept.  Used when a DDL rebuilds the table with new column ids.
  int remap_column_ids(const common::ObIArray<ObAIColumnIdPair> &pairs);
  bool depends_on(const uint64_t column_id) const;
  bool is_embed() const { return AI_COLUMN_FUNC_EMBED == func_type_; }
  // "1,3,5" <-> ids
  int dependent_ids_to_string(common::ObIAllocator &alloc, common::ObString &out) const;
  int parse_dependent_ids(const common::ObString &str);
  uint64_t get_hidden_column_id(const ObAIColumnHiddenKind kind) const;
  TO_STRING_KV(K_(table_id), K_(column_id), K_(column_name), K_(state_column_id),
               K_(request_column_id), K_(result_spec_column_id), K_(func_type),
               K_(model_name), K_(input_expr), K_(normalized_expr), K_(type_prompt),
               K_(sync_mode), K_(max_retries), K_(vector_dim), K_(def_version),
               K_(result_type), K_(dependent_column_ids));

  uint64_t table_id_;
  uint64_t column_id_;
  common::ObString column_name_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t result_spec_column_id_;
  int64_t func_type_;
  common::ObString model_name_;
  common::ObString input_expr_;
  common::ObString normalized_expr_;
  common::ObString type_prompt_;
  common::ObString sync_mode_;
  int64_t max_retries_;
  int64_t vector_dim_;
  int64_t def_version_;
  int64_t result_type_;  // ObObjType of the result column
  common::ObSEArray<uint64_t, 4> dependent_column_ids_;
  // transient (not serialized): dependency names as written in the DDL; the
  // DDL executor maps them to the final column ids after the table is built.
  common::ObSEArray<common::ObString, 4> dependent_column_names_;
};

class ObAIColumnUtil
{
public:
  static const char *const STATE_COLUMN_PREFIX;
  static const char *const REQUEST_COLUMN_PREFIX;
  static const char *const RESULT_SPEC_COLUMN_PREFIX;
  static const char *const JSON_TYPE_PROMPT;
  static const char *const INTEGER_TYPE_PROMPT;
  static const char *const DECIMAL_TYPE_PROMPT;
  static const char *const FLOAT_TYPE_PROMPT;
  static const char *const DATE_TYPE_PROMPT;
  static const char *const DATETIME_TYPE_PROMPT;
  static const int64_t MAX_RETRIES_LIMIT = 100;

  static int build_hidden_column_name(const ObAIColumnHiddenKind kind,
                                      const common::ObString &column_name,
                                      common::ObIAllocator &alloc,
                                      common::ObString &out);
  static bool is_hidden_column_name(const common::ObString &name);
  static ObAIColumnFuncType get_func_type(const common::ObString &func_name);
  // System prompt appended according to the declared result type; NULL means
  // no extra instruction (character types, vectors).
  static const char *get_type_prompt(const common::ObObjType type);
  static int build_normalized_expr(const int64_t func_type,
                                   const common::ObString &model_name,
                                   const common::ObString &input_expr,
                                   const common::ObString &type_prompt,
                                   const int64_t vector_dim,
                                   common::ObIAllocator &alloc,
                                   common::ObString &out);
  // Split "FUNC(arg1, arg2, ...)" into the function name and its top level
  // arguments (quotes, backticks, comments and nested parentheses respected).
  static int split_function_call(const common::ObString &text,
                                 common::ObString &func_name,
                                 common::ObIArray<common::ObString> &args);
  // Strip one level of single/double quotes from a SQL string literal text.
  static int unquote_string_literal(const common::ObString &text,
                                    common::ObIAllocator &alloc,
                                    common::ObString &out,
                                    bool &is_literal);
  // RETRY ... WHERE: replace the bare STATUS pseudo column (not quoted, not
  // back-quoted, not qualified) with a CASE over the hidden state column.
  static int rewrite_status_pseudo_column(const common::ObString &where_text,
                                          const common::ObString &state_column_name,
                                          common::ObIAllocator &alloc,
                                          common::ObString &out);
  static const char *row_state_name(const int64_t state);
  // escape for embedding into a single-quoted SQL literal
  static int escape_sql_string(const common::ObString &in,
                               common::ObIAllocator &alloc,
                               common::ObString &out);
};

} // end namespace sql
} // end namespace oceanbase

#endif // OCEANBASE_SQL_AI_COLUMN_OB_AI_COLUMN_DEFINE_H_
