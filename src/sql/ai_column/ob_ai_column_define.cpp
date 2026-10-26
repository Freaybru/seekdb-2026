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

#define USING_LOG_PREFIX SQL
#include "sql/ai_column/ob_ai_column_define.h"
#include "lib/string/ob_sql_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "lib/oblog/ob_log_module.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

void ObAIColumnDef::reset()
{
  table_id_ = OB_INVALID_ID;
  column_id_ = OB_INVALID_ID;
  column_name_.reset();
  state_column_id_ = OB_INVALID_ID;
  request_column_id_ = OB_INVALID_ID;
  result_spec_column_id_ = OB_INVALID_ID;
  func_type_ = AI_COLUMN_FUNC_INVALID;
  model_name_.reset();
  input_expr_.reset();
  normalized_expr_.reset();
  type_prompt_.reset();
  sync_mode_.reset();
  max_retries_ = 0;
  vector_dim_ = 0;
  def_version_ = 1;
  result_type_ = ObNullType;
  dependent_column_ids_.reset();
  dependent_column_names_.reset();
}

bool ObAIColumnDef::is_valid() const
{
  return !column_name_.empty()
         && (AI_COLUMN_FUNC_COMPLETE == func_type_ || AI_COLUMN_FUNC_EMBED == func_type_)
         && !model_name_.empty() && !input_expr_.empty()
         && max_retries_ >= 0 && def_version_ > 0;
}

int ObAIColumnDef::deep_copy(const ObAIColumnDef &src, ObIAllocator &alloc)
{
  int ret = OB_SUCCESS;
  reset();
  table_id_ = src.table_id_;
  column_id_ = src.column_id_;
  state_column_id_ = src.state_column_id_;
  request_column_id_ = src.request_column_id_;
  result_spec_column_id_ = src.result_spec_column_id_;
  func_type_ = src.func_type_;
  max_retries_ = src.max_retries_;
  vector_dim_ = src.vector_dim_;
  def_version_ = src.def_version_;
  result_type_ = src.result_type_;
  if (OB_FAIL(ob_write_string(alloc, src.column_name_, column_name_))) {
    LOG_WARN("copy failed", K(ret));
  } else if (OB_FAIL(ob_write_string(alloc, src.model_name_, model_name_))) {
    LOG_WARN("copy failed", K(ret));
  } else if (OB_FAIL(ob_write_string(alloc, src.input_expr_, input_expr_))) {
    LOG_WARN("copy failed", K(ret));
  } else if (OB_FAIL(ob_write_string(alloc, src.normalized_expr_, normalized_expr_))) {
    LOG_WARN("copy failed", K(ret));
  } else if (OB_FAIL(ob_write_string(alloc, src.type_prompt_, type_prompt_))) {
    LOG_WARN("copy failed", K(ret));
  } else if (OB_FAIL(ob_write_string(alloc, src.sync_mode_, sync_mode_))) {
    LOG_WARN("copy failed", K(ret));
  } else if (OB_FAIL(dependent_column_ids_.assign(src.dependent_column_ids_))) {
    LOG_WARN("copy failed", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < src.dependent_column_names_.count(); ++i) {
    ObString name;
    if (OB_FAIL(ob_write_string(alloc, src.dependent_column_names_.at(i), name))) {
      LOG_WARN("copy failed", K(ret));
    } else if (OB_FAIL(dependent_column_names_.push_back(name))) {
      LOG_WARN("push back failed", K(ret));
    }
  }
  return ret;
}

static uint64_t ai_col_remap_one_id(const ObIArray<ObAIColumnIdPair> &pairs, const uint64_t id)
{
  uint64_t new_id = id;
  for (int64_t i = 0; i < pairs.count(); ++i) {
    if (pairs.at(i).old_id_ == id) {
      new_id = pairs.at(i).new_id_;
      break;
    }
  }
  return new_id;
}

int ObAIColumnDef::remap_column_ids(const ObIArray<ObAIColumnIdPair> &pairs)
{
  int ret = OB_SUCCESS;
  column_id_ = ai_col_remap_one_id(pairs, column_id_);
  state_column_id_ = ai_col_remap_one_id(pairs, state_column_id_);
  request_column_id_ = ai_col_remap_one_id(pairs, request_column_id_);
  result_spec_column_id_ = ai_col_remap_one_id(pairs, result_spec_column_id_);
  for (int64_t i = 0; i < dependent_column_ids_.count(); ++i) {
    dependent_column_ids_.at(i) = ai_col_remap_one_id(pairs, dependent_column_ids_.at(i));
  }
  return ret;
}

bool ObAIColumnDef::depends_on(const uint64_t column_id) const
{
  bool found = false;
  for (int64_t i = 0; !found && i < dependent_column_ids_.count(); ++i) {
    found = (dependent_column_ids_.at(i) == column_id);
  }
  return found;
}

uint64_t ObAIColumnDef::get_hidden_column_id(const ObAIColumnHiddenKind kind) const
{
  uint64_t id = OB_INVALID_ID;
  switch (kind) {
    case AI_HIDDEN_STATE: id = state_column_id_; break;
    case AI_HIDDEN_REQUEST: id = request_column_id_; break;
    case AI_HIDDEN_RESULT_SPEC: id = result_spec_column_id_; break;
    default: break;
  }
  return id;
}

int ObAIColumnDef::dependent_ids_to_string(ObIAllocator &alloc, ObString &out) const
{
  int ret = OB_SUCCESS;
  ObSqlString buf;
  for (int64_t i = 0; OB_SUCC(ret) && i < dependent_column_ids_.count(); ++i) {
    if (OB_FAIL(buf.append_fmt("%s%lu", i > 0 ? "," : "", dependent_column_ids_.at(i)))) {
      LOG_WARN("append failed", K(ret));
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(ob_write_string(alloc, buf.string(), out))) {
    LOG_WARN("copy failed", K(ret));
  }
  return ret;
}

int ObAIColumnDef::parse_dependent_ids(const ObString &str)
{
  int ret = OB_SUCCESS;
  dependent_column_ids_.reset();
  uint64_t cur = 0;
  bool has_digit = false;
  for (int64_t i = 0; OB_SUCC(ret) && i <= str.length(); ++i) {
    const char c = (i < str.length()) ? str.ptr()[i] : ',';
    if (c >= '0' && c <= '9') {
      cur = cur * 10 + (c - '0');
      has_digit = true;
    } else if (',' == c) {
      if (has_digit && OB_FAIL(dependent_column_ids_.push_back(cur))) {
        LOG_WARN("push back failed", K(ret));
      }
      cur = 0;
      has_digit = false;
    } else if (' ' == c) {
      // skip
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid dependent column id list", K(ret), K(str));
    }
  }
  return ret;
}

OB_DEF_SERIALIZE(ObAIColumnDef)
{
  int ret = OB_SUCCESS;
  LST_DO_CODE(OB_UNIS_ENCODE, table_id_, column_id_, column_name_, state_column_id_,
              request_column_id_, result_spec_column_id_, func_type_, model_name_,
              input_expr_, normalized_expr_, type_prompt_, sync_mode_, max_retries_,
              vector_dim_, def_version_, result_type_);
  const int64_t dep_cnt = dependent_column_ids_.count();
  OB_UNIS_ENCODE(dep_cnt);
  for (int64_t i = 0; OB_SUCC(ret) && i < dep_cnt; ++i) {
    OB_UNIS_ENCODE(dependent_column_ids_.at(i));
  }
  return ret;
}

OB_DEF_DESERIALIZE(ObAIColumnDef)
{
  int ret = OB_SUCCESS;
  LST_DO_CODE(OB_UNIS_DECODE, table_id_, column_id_, column_name_, state_column_id_,
              request_column_id_, result_spec_column_id_, func_type_, model_name_,
              input_expr_, normalized_expr_, type_prompt_, sync_mode_, max_retries_,
              vector_dim_, def_version_, result_type_);
  int64_t dep_cnt = 0;
  OB_UNIS_DECODE(dep_cnt);
  dependent_column_ids_.reset();
  for (int64_t i = 0; OB_SUCC(ret) && i < dep_cnt; ++i) {
    uint64_t id = 0;
    OB_UNIS_DECODE(id);
    if (OB_SUCC(ret) && OB_FAIL(dependent_column_ids_.push_back(id))) {
      LOG_WARN("push back failed", K(ret));
    }
  }
  return ret;
}

OB_DEF_SERIALIZE_SIZE(ObAIColumnDef)
{
  int64_t len = 0;
  LST_DO_CODE(OB_UNIS_ADD_LEN, table_id_, column_id_, column_name_, state_column_id_,
              request_column_id_, result_spec_column_id_, func_type_, model_name_,
              input_expr_, normalized_expr_, type_prompt_, sync_mode_, max_retries_,
              vector_dim_, def_version_, result_type_);
  const int64_t dep_cnt = dependent_column_ids_.count();
  OB_UNIS_ADD_LEN(dep_cnt);
  for (int64_t i = 0; i < dep_cnt; ++i) {
    OB_UNIS_ADD_LEN(dependent_column_ids_.at(i));
  }
  return len;
}

const char *const ObAIColumnUtil::STATE_COLUMN_PREFIX = "__ai_state_";
const char *const ObAIColumnUtil::REQUEST_COLUMN_PREFIX = "__ai_req_";
const char *const ObAIColumnUtil::RESULT_SPEC_COLUMN_PREFIX = "__ai_spec_";
const char *const ObAIColumnUtil::JSON_TYPE_PROMPT =
    "Return exactly one valid JSON value and nothing else. Do not wrap it in markdown.";
const char *const ObAIColumnUtil::INTEGER_TYPE_PROMPT =
    "Return exactly one base-10 integer and nothing else.";
const char *const ObAIColumnUtil::DECIMAL_TYPE_PROMPT =
    "Return exactly one decimal number in plain notation and nothing else.";
const char *const ObAIColumnUtil::FLOAT_TYPE_PROMPT =
    "Return exactly one floating-point number and nothing else.";
const char *const ObAIColumnUtil::DATE_TYPE_PROMPT =
    "Return exactly one date in YYYY-MM-DD format and nothing else.";
const char *const ObAIColumnUtil::DATETIME_TYPE_PROMPT =
    "Return exactly one datetime in YYYY-MM-DD HH:MM:SS format and nothing else.";

int ObAIColumnUtil::build_hidden_column_name(const ObAIColumnHiddenKind kind,
                                             const ObString &column_name,
                                             ObIAllocator &alloc,
                                             ObString &out)
{
  int ret = OB_SUCCESS;
  const char *prefix = NULL;
  switch (kind) {
    case AI_HIDDEN_STATE: prefix = STATE_COLUMN_PREFIX; break;
    case AI_HIDDEN_REQUEST: prefix = REQUEST_COLUMN_PREFIX; break;
    case AI_HIDDEN_RESULT_SPEC: prefix = RESULT_SPEC_COLUMN_PREFIX; break;
    default: break;
  }
  if (OB_ISNULL(prefix) || column_name.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(kind), K(column_name));
  } else {
    const int64_t len = STRLEN(prefix) + column_name.length();
    char *buf = static_cast<char *>(alloc.alloc(len + 1));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("alloc failed", K(ret));
    } else {
      MEMCPY(buf, prefix, STRLEN(prefix));
      MEMCPY(buf + STRLEN(prefix), column_name.ptr(), column_name.length());
      buf[len] = '\0';
      out.assign_ptr(buf, static_cast<int32_t>(len));
    }
  }
  return ret;
}

bool ObAIColumnUtil::is_hidden_column_name(const ObString &name)
{
  return name.prefix_match_ci(STATE_COLUMN_PREFIX)
         || name.prefix_match_ci(REQUEST_COLUMN_PREFIX)
         || name.prefix_match_ci(RESULT_SPEC_COLUMN_PREFIX);
}

ObAIColumnFuncType ObAIColumnUtil::get_func_type(const ObString &func_name)
{
  ObAIColumnFuncType type = AI_COLUMN_FUNC_INVALID;
  if (0 == func_name.case_compare("ai_complete")) {
    type = AI_COLUMN_FUNC_COMPLETE;
  } else if (0 == func_name.case_compare("ai_embed")) {
    type = AI_COLUMN_FUNC_EMBED;
  }
  return type;
}

const char *ObAIColumnUtil::get_type_prompt(const ObObjType type)
{
  const char *prompt = NULL;
  if (ObJsonType == type) {
    prompt = JSON_TYPE_PROMPT;
  } else if (ob_is_integer_type(type)) {
    prompt = INTEGER_TYPE_PROMPT;
  } else if (ob_is_number_tc(type) || ob_is_decimal_int_tc(type)) {
    prompt = DECIMAL_TYPE_PROMPT;
  } else if (ob_is_float_tc(type) || ob_is_double_tc(type)) {
    prompt = FLOAT_TYPE_PROMPT;
  } else if (ObDateType == type || ObMySQLDateType == type) {
    prompt = DATE_TYPE_PROMPT;
  } else if (ob_is_datetime_tc(type) || ObTimestampType == type || ObMySQLDateTimeType == type) {
    prompt = DATETIME_TYPE_PROMPT;
  }
  return prompt;
}

int ObAIColumnUtil::build_normalized_expr(const int64_t func_type,
                                          const ObString &model_name,
                                          const ObString &input_expr,
                                          const ObString &type_prompt,
                                          const int64_t vector_dim,
                                          ObIAllocator &alloc,
                                          ObString &out)
{
  int ret = OB_SUCCESS;
  ObSqlString buf;
  ObString escaped_model;
  ObString escaped_prompt;
  if (OB_FAIL(escape_sql_string(model_name, alloc, escaped_model))) {
    LOG_WARN("escape failed", K(ret));
  } else if (AI_COLUMN_FUNC_COMPLETE == func_type) {
    if (type_prompt.empty()) {
      ret = buf.append_fmt("AI_COMPLETE('%.*s', %.*s)",
                           escaped_model.length(), escaped_model.ptr(),
                           input_expr.length(), input_expr.ptr());
    } else if (OB_FAIL(escape_sql_string(type_prompt, alloc, escaped_prompt))) {
      LOG_WARN("escape failed", K(ret));
    } else {
      ret = buf.append_fmt("AI_COMPLETE('%.*s', %.*s) SYSTEM PROMPT '%.*s'",
                           escaped_model.length(), escaped_model.ptr(),
                           input_expr.length(), input_expr.ptr(),
                           escaped_prompt.length(), escaped_prompt.ptr());
    }
  } else if (AI_COLUMN_FUNC_EMBED == func_type) {
    ret = buf.append_fmt("AI_EMBED('%.*s', %.*s, %ld)",
                         escaped_model.length(), escaped_model.ptr(),
                         input_expr.length(), input_expr.ptr(), vector_dim);
  } else {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid func type", K(ret), K(func_type));
  }
  if (OB_SUCC(ret) && OB_FAIL(ob_write_string(alloc, buf.string(), out))) {
    LOG_WARN("copy failed", K(ret));
  }
  return ret;
}

static void ai_col_trim(const char *&b, const char *&e)
{
  while (b < e && isspace(static_cast<unsigned char>(*b))) { ++b; }
  while (e > b && isspace(static_cast<unsigned char>(*(e - 1)))) { --e; }
}

int ObAIColumnUtil::split_function_call(const ObString &text,
                                        ObString &func_name,
                                        ObIArray<ObString> &args)
{
  int ret = OB_SUCCESS;
  args.reset();
  func_name.reset();
  const char *b = text.ptr();
  const char *e = text.ptr() + text.length();
  ai_col_trim(b, e);
  const char *p = b;
  while (p < e && (isalnum(static_cast<unsigned char>(*p)) || '_' == *p)) { ++p; }
  const char *name_end = p;
  while (p < e && isspace(static_cast<unsigned char>(*p))) { ++p; }
  if (name_end == b || p >= e || '(' != *p || ')' != *(e - 1)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("not a function call", K(ret), K(text));
  } else {
    func_name.assign_ptr(b, static_cast<int32_t>(name_end - b));
    const char *body_b = p + 1;
    const char *body_e = e - 1;
    int64_t depth = 0;
    char quote = 0;
    const char *arg_b = body_b;
    for (const char *q = body_b; OB_SUCC(ret) && q <= body_e; ++q) {
      const bool at_end = (q == body_e);
      const char c = at_end ? ',' : *q;
      if (!at_end && 0 != quote) {
        if ('\\' == c && '`' != quote && q + 1 < body_e) {
          ++q;
        } else if (c == quote) {
          if (q + 1 < body_e && *(q + 1) == quote) {
            ++q;  // doubled quote
          } else {
            quote = 0;
          }
        }
      } else if (!at_end && ('\'' == c || '"' == c || '`' == c)) {
        quote = c;
      } else if (!at_end && '(' == c) {
        ++depth;
      } else if (!at_end && ')' == c) {
        if (--depth < 0) {
          ret = OB_INVALID_ARGUMENT;
        }
      } else if (',' == c && (at_end || 0 == depth)) {
        if (at_end && (0 != depth || 0 != quote)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("unbalanced function call", K(ret), K(text));
        } else {
          const char *ab = arg_b;
          const char *ae = q;
          ai_col_trim(ab, ae);
          if (ab == ae) {
            if (!(at_end && 0 == args.count())) {
              ret = OB_INVALID_ARGUMENT;
              LOG_WARN("empty argument", K(ret), K(text));
            }
          } else if (OB_FAIL(args.push_back(ObString(static_cast<int32_t>(ae - ab), ab)))) {
            LOG_WARN("push back failed", K(ret));
          }
          arg_b = q + 1;
        }
      }
    }
  }
  return ret;
}

int ObAIColumnUtil::unquote_string_literal(const ObString &text,
                                           ObIAllocator &alloc,
                                           ObString &out,
                                           bool &is_literal)
{
  int ret = OB_SUCCESS;
  is_literal = false;
  const char *b = text.ptr();
  const char *e = text.ptr() + text.length();
  ai_col_trim(b, e);
  if (e - b >= 2 && ('\'' == *b || '"' == *b) && *(e - 1) == *b) {
    const char quote = *b;
    char *buf = static_cast<char *>(alloc.alloc(e - b));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      int64_t pos = 0;
      is_literal = true;
      for (const char *q = b + 1; is_literal && q < e - 1; ++q) {
        if ('\\' == *q && q + 1 < e - 1) {
          ++q;
          buf[pos++] = *q;
        } else if (*q == quote) {
          if (q + 1 < e - 1 && *(q + 1) == quote) {
            buf[pos++] = quote;
            ++q;
          } else {
            is_literal = false;  // e.g. 'a' 'b' or 'a' || x
          }
        } else {
          buf[pos++] = *q;
        }
      }
      out.assign_ptr(buf, static_cast<int32_t>(pos));
    }
  }
  return ret;
}

int ObAIColumnUtil::rewrite_status_pseudo_column(const ObString &where_text,
                                                 const ObString &state_column_name,
                                                 ObIAllocator &alloc,
                                                 ObString &out)
{
  int ret = OB_SUCCESS;
  ObSqlString buf;
  const char *p = where_text.ptr();
  const char *e = where_text.ptr() + where_text.length();
  char prev_sig = 0;  // previous non-space char outside literals
  while (OB_SUCC(ret) && p < e) {
    const char c = *p;
    if ('\'' == c || '"' == c || '`' == c) {
      const char *q = p + 1;
      while (q < e) {
        if ('\\' == *q && '`' != c && q + 1 < e) {
          q += 2;
        } else if (*q == c) {
          if (q + 1 < e && *(q + 1) == c) {
            q += 2;
          } else {
            ++q;
            break;
          }
        } else {
          ++q;
        }
      }
      ret = buf.append(p, q - p);
      prev_sig = c;
      p = q;
    } else if (isalpha(static_cast<unsigned char>(c)) || '_' == c || '$' == c) {
      const char *q = p;
      while (q < e && (isalnum(static_cast<unsigned char>(*q)) || '_' == *q || '$' == *q)) { ++q; }
      ObString word(static_cast<int32_t>(q - p), p);
      const char *n = q;
      while (n < e && isspace(static_cast<unsigned char>(*n))) { ++n; }
      const bool qualified = ('.' == prev_sig) || (n < e && '.' == *n);
      const bool is_call = (n < e && '(' == *n);
      if (0 == word.case_compare("status") && !qualified && !is_call) {
        ret = buf.append_fmt("(case `%.*s` when %d then 'PENDING' when %d then 'READY'"
                             " when %d then 'FAILED' end)",
                             state_column_name.length(), state_column_name.ptr(),
                             AI_ROW_PENDING, AI_ROW_READY, AI_ROW_FAILED);
      } else {
        ret = buf.append(p, q - p);
      }
      prev_sig = 'a';
      p = q;
    } else if ('-' == c && p + 1 < e && '-' == *(p + 1)) {
      const char *q = p;
      while (q < e && '\n' != *q) { ++q; }
      p = q;  // drop line comment
    } else if ('/' == c && p + 1 < e && '*' == *(p + 1)) {
      const char *q = p + 2;
      while (q + 1 < e && !('*' == *q && '/' == *(q + 1))) { ++q; }
      q = (q + 1 < e) ? q + 2 : e;
      ret = buf.append(" ");
      p = q;
    } else if (isdigit(static_cast<unsigned char>(c))) {
      const char *q = p;
      while (q < e && (isalnum(static_cast<unsigned char>(*q)) || '.' == *q)) { ++q; }
      ret = buf.append(p, q - p);
      prev_sig = '0';
      p = q;
    } else {
      ret = buf.append(p, 1);
      if (!isspace(static_cast<unsigned char>(c))) {
        prev_sig = c;
      }
      ++p;
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(ob_write_string(alloc, buf.string(), out))) {
    LOG_WARN("copy failed", K(ret));
  }
  return ret;
}

const char *ObAIColumnUtil::row_state_name(const int64_t state)
{
  const char *name = "UNKNOWN";
  switch (state) {
    case AI_ROW_PENDING: name = "PENDING"; break;
    case AI_ROW_READY: name = "READY"; break;
    case AI_ROW_FAILED: name = "FAILED"; break;
    default: break;
  }
  return name;
}

int ObAIColumnUtil::escape_sql_string(const ObString &in, ObIAllocator &alloc, ObString &out)
{
  int ret = OB_SUCCESS;
  char *buf = static_cast<char *>(alloc.alloc(in.length() * 2 + 1));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    int64_t pos = 0;
    for (int64_t i = 0; i < in.length(); ++i) {
      const char c = in.ptr()[i];
      if ('\'' == c) {
        buf[pos++] = '\'';
        buf[pos++] = '\'';
      } else if ('\\' == c) {
        buf[pos++] = '\\';
        buf[pos++] = '\\';
      } else {
        buf[pos++] = c;
      }
    }
    buf[pos] = '\0';
    out.assign_ptr(buf, static_cast<int32_t>(pos));
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
