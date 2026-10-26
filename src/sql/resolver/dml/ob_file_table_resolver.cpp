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
#include "sql/resolver/dml/ob_file_table_resolver.h"
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/resolver/dml/ob_select_stmt.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/parser/ob_parser.h"
#include "sql/session/ob_sql_session_info.h"
#include "lib/string/ob_sql_string.h"
#include <glob.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>

namespace oceanbase
{
using namespace common;
namespace sql
{

static bool fsql_name_eq(const ObString &a, const char *b)
{
  return 0 == a.case_compare(b);
}

int ObFileSqlResolverUtil::check_enabled(const ObSQLSessionInfo *session, const char *what)
{
  int ret = OB_SUCCESS;
  bool enabled = false;
  if (OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(session->get_enable_file_sql(enabled))) {
    LOG_WARN("failed to get enable_file_sql", K(ret));
  } else if (!enabled) {
    ret = OB_NOT_SUPPORTED;
    char msg[256];
    snprintf(msg, sizeof(msg), "%s while enable_file_sql is OFF, it is", what);
    LOG_USER_ERROR(OB_NOT_SUPPORTED, msg);
  }
  return ret;
}

int ObFileSqlResolverUtil::is_path_allowed(const ObSQLSessionInfo &session, const char *real_path,
                                           bool &allowed)
{
  int ret = OB_SUCCESS;
  ObString secure_file_priv;
  allowed = false;
  if (session.is_inner()) {
    allowed = true;
  } else if (OB_FAIL(session.get_secure_file_priv(secure_file_priv))) {
    LOG_WARN("failed to get secure_file_priv", K(ret));
  } else {
    int tmp_ret = ObResolverUtils::check_secure_path(secure_file_priv, ObString(real_path));
    if (OB_SUCCESS == tmp_ret) {
      allowed = true;
    } else if (OB_ERR_NO_PRIVILEGE == tmp_ret) {
      allowed = false;
    } else {
      ret = tmp_ret;
    }
  }
  return ret;
}

int ObFileSqlResolverUtil::check_file_path(const ObSQLSessionInfo &session, const ObString &func,
                                           const ObString &raw_path, bool must_be_file,
                                           ObIAllocator &allocator, ObString &real_path)
{
  int ret = OB_SUCCESS;
  char path_buf[PATH_MAX + 1];
  char real_buf[PATH_MAX + 1];
  bool allowed = false;
  struct stat st;
  if (raw_path.empty() || raw_path.length() >= PATH_MAX
      || NULL != MEMCHR(raw_path.ptr(), '\0', raw_path.length())) {
    ret = OB_INVALID_ARGUMENT;
    char msg[256];
    snprintf(msg, sizeof(msg), "%.*s: invalid file path", func.length(), func.ptr());
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, msg);
  } else {
    MEMCPY(path_buf, raw_path.ptr(), raw_path.length());
    path_buf[raw_path.length()] = '\0';
    if (NULL == ::realpath(path_buf, real_buf)) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("file sql path does not exist", K(ret), K(raw_path), K(errno));
      LOG_USER_ERROR(OB_FILE_NOT_EXIST);
    } else if (OB_FAIL(is_path_allowed(session, real_buf, allowed))) {
    } else if (!allowed) {
      ret = OB_ERR_NO_PRIVILEGE;
      LOG_WARN("file sql path is outside secure_file_priv", K(ret), K(raw_path));
      LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "FILE (path is outside secure_file_priv)");
    } else if (must_be_file && (0 != ::stat(real_buf, &st) || !S_ISREG(st.st_mode))) {
      ret = OB_INVALID_ARGUMENT;
      char msg[256];
      snprintf(msg, sizeof(msg), "%.*s: not a regular file", func.length(), func.ptr());
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, msg);
    } else if (OB_FAIL(ob_write_string(allocator, ObString(real_buf), real_path, true /*c_style*/))) {
    }
  }
  return ret;
}

int ObFileSqlResolverUtil::get_format_by_func(const ObString &func, int8_t &format, int8_t &json_layout)
{
  int ret = OB_SUCCESS;
  json_layout = FILE_JSON_AUTO;
  if (fsql_name_eq(func, "read_csv") || fsql_name_eq(func, "read_csv_auto")) {
    format = FILE_FORMAT_CSV;
  } else if (fsql_name_eq(func, "read_json") || fsql_name_eq(func, "read_json_auto")) {
    format = FILE_FORMAT_JSON;
  } else if (fsql_name_eq(func, "read_ndjson") || fsql_name_eq(func, "read_ndjson_auto")) {
    format = FILE_FORMAT_JSON;
    json_layout = FILE_JSON_NDJSON;
  } else if (fsql_name_eq(func, "read_parquet") || fsql_name_eq(func, "parquet_scan")) {
    format = FILE_FORMAT_PARQUET;
  } else if (fsql_name_eq(func, "glob")) {
    format = FILE_FORMAT_GLOB;
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file table function");
  }
  return ret;
}

int ObFileSqlResolverUtil::get_option_name(const ParseNode *opt, ObString &name)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(opt) || OB_UNLIKELY(opt->num_child_ != 2) || OB_ISNULL(opt->children_)
      || OB_ISNULL(opt->children_[0])) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    name.assign_ptr(opt->children_[0]->str_value_, static_cast<int32_t>(opt->children_[0]->str_len_));
  }
  return ret;
}

int ObFileSqlResolverUtil::get_bool_value(const ParseNode *v, bool &b)
{
  int ret = OB_SUCCESS;
  if (NULL == v) {
    b = true;   // a bare option name such as HEADER means true
  } else if (T_BOOL == v->type_ || T_INT == v->type_) {
    b = (0 != v->value_);
  } else if (NULL != v->str_value_) {
    ObString s(static_cast<int32_t>(v->str_len_), v->str_value_);
    if (fsql_name_eq(s, "true") || fsql_name_eq(s, "1") || fsql_name_eq(s, "on")) {
      b = true;
    } else if (fsql_name_eq(s, "false") || fsql_name_eq(s, "0") || fsql_name_eq(s, "off")) {
      b = false;
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
  }
  return ret;
}

int ObFileSqlResolverUtil::get_string_value(const ParseNode *v, ObString &s)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(v) || T_NULL == v->type_ || T_BOOL == v->type_ || OB_ISNULL(v->str_value_)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    s.assign_ptr(v->str_value_, static_cast<int32_t>(v->str_len_));
  }
  return ret;
}

int ObFileSqlResolverUtil::get_int_value(const ParseNode *v, int64_t &i)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(v) || T_INT != v->type_) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    i = v->value_;
  }
  return ret;
}

int ObFileSqlResolverUtil::get_char_value(const ParseNode *v, bool allow_empty, int8_t &c)
{
  int ret = OB_SUCCESS;
  ObString s;
  if (OB_FAIL(get_string_value(v, s))) {
  } else if (1 == s.length()) {
    c = static_cast<int8_t>(s.ptr()[0]);
  } else if (0 == s.length() && allow_empty) {
    c = 0;
  } else {
    ret = OB_INVALID_ARGUMENT;
  }
  return ret;
}

int ObFileSqlResolverUtil::parse_scan_options(const ObString &func, const ParseNode *opt_list,
                                              ObIAllocator &allocator, ObFileScanOptions &opts)
{
  int ret = OB_SUCCESS;
  int8_t format = FILE_FORMAT_INVALID;
  int8_t layout = FILE_JSON_AUTO;
  opts.reset();
  if (OB_FAIL(get_format_by_func(func, format, layout))) {
  } else {
    opts.format_ = format;
    opts.json_layout_ = layout;
  }
  for (int64_t i = 0; OB_SUCC(ret) && NULL != opt_list && i < opt_list->num_child_; ++i) {
    const ParseNode *opt = opt_list->children_[i];
    const ParseNode *v = NULL;
    ObString name;
    bool unknown = false;
    bool bad_value = false;
    if (OB_FAIL(get_option_name(opt, name))) {
    } else {
      v = opt->children_[1];
      int tmp = OB_SUCCESS;
      if (fsql_name_eq(name, "sample_size")) {
        // schema inference always reads the whole file; only -1 (= all rows) is accepted
        int64_t n = 0;
        if (OB_SUCCESS != (tmp = get_int_value(v, n))) {
          bad_value = true;
        } else if (-1 != n) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "sample_size other than -1 (full scan inference) is");
        }
      } else if (fsql_name_eq(name, "ignore_errors")) {
        bad_value = (OB_SUCCESS != get_bool_value(v, opts.ignore_errors_));
      } else if (fsql_name_eq(name, "auto_detect")) {
        bool b = true;
        if (OB_SUCCESS != get_bool_value(v, b)) {
          bad_value = true;
        } else if (!b) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "auto_detect = false is");
        }
      } else if (FILE_FORMAT_CSV == format) {
        if (fsql_name_eq(name, "header")) {
          bad_value = (OB_SUCCESS != get_bool_value(v, opts.header_));
        } else if (fsql_name_eq(name, "delim") || fsql_name_eq(name, "sep")
                   || fsql_name_eq(name, "delimiter")) {
          bad_value = (OB_SUCCESS != get_char_value(v, false, opts.delim_))
                      || '\n' == opts.delim_ || '\r' == opts.delim_;
        } else if (fsql_name_eq(name, "quote")) {
          bad_value = (OB_SUCCESS != get_char_value(v, true, opts.quote_));
        } else if (fsql_name_eq(name, "escape")) {
          bad_value = (OB_SUCCESS != get_char_value(v, true, opts.escape_));
        } else if (fsql_name_eq(name, "nullstr") || fsql_name_eq(name, "null_str")) {
          ObString s;
          if (OB_SUCCESS != get_string_value(v, s)) {
            bad_value = true;
          } else if (OB_FAIL(ob_write_string(allocator, s, opts.null_str_))) {
          }
        } else if (fsql_name_eq(name, "skip")) {
          bad_value = (OB_SUCCESS != get_int_value(v, opts.skip_rows_)) || opts.skip_rows_ < 0;
        } else if (fsql_name_eq(name, "null_padding")) {
          bad_value = (OB_SUCCESS != get_bool_value(v, opts.null_padding_));
        } else if (fsql_name_eq(name, "all_varchar")) {
          bad_value = (OB_SUCCESS != get_bool_value(v, opts.all_varchar_));
        } else if (fsql_name_eq(name, "max_line_size") || fsql_name_eq(name, "maximum_line_size")) {
          bad_value = (OB_SUCCESS != get_int_value(v, opts.max_record_size_)) || opts.max_record_size_ <= 0;
        } else {
          unknown = true;
        }
        if (OB_SUCC(ret) && !bad_value && opts.delim_ == opts.quote_) {
          bad_value = true;
        }
      } else if (FILE_FORMAT_JSON == format) {
        if (fsql_name_eq(name, "format")) {
          ObString s;
          if (OB_SUCCESS != get_string_value(v, s)) {
            bad_value = true;
          } else if (fsql_name_eq(s, "auto")) {
            opts.json_layout_ = FILE_JSON_AUTO;
          } else if (fsql_name_eq(s, "newline_delimited") || fsql_name_eq(s, "nd")
                     || fsql_name_eq(s, "ndjson") || fsql_name_eq(s, "jsonl")) {
            opts.json_layout_ = FILE_JSON_NDJSON;
          } else if (fsql_name_eq(s, "array")) {
            opts.json_layout_ = FILE_JSON_ARRAY;
          } else {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "json format other than auto/newline_delimited/array is");
          }
        } else if (fsql_name_eq(name, "records")) {
          ObString s;
          bool b = true;
          if (OB_SUCCESS == get_bool_value(v, b)) {
            if (!b) {
              ret = OB_NOT_SUPPORTED;
              LOG_USER_ERROR(OB_NOT_SUPPORTED, "records = false is");
            }
          } else if (OB_SUCCESS == get_string_value(v, s) && fsql_name_eq(s, "auto")) {
          } else {
            bad_value = true;
          }
        } else if (fsql_name_eq(name, "maximum_object_size")) {
          bad_value = (OB_SUCCESS != get_int_value(v, opts.max_record_size_)) || opts.max_record_size_ <= 0;
        } else {
          unknown = true;
        }
      } else {
        unknown = true;
      }
      if (OB_FAIL(ret)) {
      } else if (unknown) {
        ret = OB_NOT_SUPPORTED;
        char msg[256];
        snprintf(msg, sizeof(msg), "option '%.*s' of %.*s is", name.length(), name.ptr(),
                 func.length(), func.ptr());
        LOG_USER_ERROR(OB_NOT_SUPPORTED, msg);
      } else if (bad_value) {
        ret = OB_INVALID_ARGUMENT;
        char msg[256];
        snprintf(msg, sizeof(msg), "%.*s option '%.*s'", func.length(), func.ptr(),
                 name.length(), name.ptr());
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, msg);
      }
    }
  }
  return ret;
}

int ObFileSqlResolverUtil::parse_copy_options(const ParseNode *opt_list, ObIAllocator &allocator,
                                              ObFileCopyOptions &opts)
{
  int ret = OB_SUCCESS;
  opts.reset();
  for (int64_t i = 0; OB_SUCC(ret) && NULL != opt_list && i < opt_list->num_child_; ++i) {
    const ParseNode *opt = opt_list->children_[i];
    const ParseNode *v = NULL;
    ObString name;
    bool bad_value = false;
    if (OB_FAIL(get_option_name(opt, name))) {
    } else {
      v = opt->children_[1];
      int8_t c = 0;
      if (fsql_name_eq(name, "format")) {
        ObString s;
        if (OB_SUCCESS != get_string_value(v, s)) {
          bad_value = true;
        } else if (!fsql_name_eq(s, "csv")) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "COPY TO format other than csv is");
        }
      } else if (fsql_name_eq(name, "header")) {
        bad_value = (OB_SUCCESS != get_bool_value(v, opts.header_));
      } else if (fsql_name_eq(name, "delimiter") || fsql_name_eq(name, "delim")
                 || fsql_name_eq(name, "sep")) {
        bad_value = (OB_SUCCESS != get_char_value(v, false, c)) || '\n' == c || '\r' == c;
        opts.delim_ = static_cast<char>(c);
      } else if (fsql_name_eq(name, "quote")) {
        bad_value = (OB_SUCCESS != get_char_value(v, false, c)) || '\n' == c || '\r' == c;
        opts.quote_ = static_cast<char>(c);
      } else if (fsql_name_eq(name, "escape")) {
        bad_value = (OB_SUCCESS != get_char_value(v, false, c)) || '\n' == c || '\r' == c;
        opts.escape_ = static_cast<char>(c);
      } else if (fsql_name_eq(name, "nullstr") || fsql_name_eq(name, "null_str")) {
        ObString s;
        if (OB_SUCCESS != get_string_value(v, s)) {
          bad_value = true;
        } else if (OB_FAIL(ob_write_string(allocator, s, opts.null_str_))) {
        }
      } else {
        ret = OB_NOT_SUPPORTED;
        char msg[256];
        snprintf(msg, sizeof(msg), "COPY TO option '%.*s' is", name.length(), name.ptr());
        LOG_USER_ERROR(OB_NOT_SUPPORTED, msg);
      }
      if (OB_SUCC(ret) && bad_value) {
        ret = OB_INVALID_ARGUMENT;
        char msg[256];
        snprintf(msg, sizeof(msg), "COPY TO option '%.*s'", name.length(), name.ptr());
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, msg);
      }
    }
  }
  if (OB_SUCC(ret) && (opts.delim_ == opts.quote_
                       || (0 == opts.null_str_.length() ? false
                           : (NULL != MEMCHR(opts.null_str_.ptr(), opts.delim_, opts.null_str_.length()))))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "COPY TO options (delimiter must differ from quote and nullstr)");
  }
  return ret;
}

bool ObFileSqlResolverUtil::contains_file_table(const ParseNode *node)
{
  bool found = false;
  if (NULL != node) {
    if (T_FILE_TABLE_FUNCTION == node->type_) {
      found = true;
    } else {
      for (int64_t i = 0; !found && i < node->num_child_; ++i) {
        found = contains_file_table(node->children_[i]);
      }
    }
  }
  return found;
}

int ObFileSqlResolverUtil::file_column_res_type(const ObFileColumnDesc &col, ObCollationType cs_type,
                                                ObRawExprResType &res_type)
{
  int ret = OB_SUCCESS;
  res_type.reset();
  switch (col.type_) {
    case FILE_COL_BOOLEAN: {
      res_type.set_tinyint();
      res_type.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY2[0][ObTinyIntType]);
      res_type.set_precision(1);
      res_type.set_collation_type(CS_TYPE_BINARY);
      res_type.set_collation_level(CS_LEVEL_NUMERIC);
      break;
    }
    case FILE_COL_BIGINT: {
      res_type.set_int();
      res_type.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY2[0][ObIntType]);
      res_type.set_collation_type(CS_TYPE_BINARY);
      res_type.set_collation_level(CS_LEVEL_NUMERIC);
      break;
    }
    case FILE_COL_DOUBLE: {
      res_type.set_double();
      res_type.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY2[0][ObDoubleType]);
      res_type.set_collation_type(CS_TYPE_BINARY);
      res_type.set_collation_level(CS_LEVEL_NUMERIC);
      break;
    }
    case FILE_COL_DATE: {
      res_type.set_date();
      res_type.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY2[0][ObDateType]);
      res_type.set_collation_type(CS_TYPE_BINARY);
      res_type.set_collation_level(CS_LEVEL_NUMERIC);
      break;
    }
    case FILE_COL_DATETIME: {
      const int16_t scale = col.scale_ > 0 ? MIN(col.scale_, 6) : 0;
      res_type.set_datetime();
      res_type.set_scale(scale);
      res_type.set_precision(static_cast<ObPrecision>(19 + (scale > 0 ? scale + 1 : 0)));
      res_type.set_collation_type(CS_TYPE_BINARY);
      res_type.set_collation_level(CS_LEVEL_NUMERIC);
      break;
    }
    default: {
      // VARCHAR, or a column that only ever held NULL
      const int64_t len = MIN(MAX(col.max_length_, 1), OB_MAX_VARCHAR_LENGTH / 4);
      res_type.set_varchar();
      res_type.set_length(static_cast<ObLength>(len));
      res_type.set_length_semantics(LS_CHAR);
      res_type.set_collation_type(cs_type);
      res_type.set_collation_level(CS_LEVEL_IMPLICIT);
      break;
    }
  }
  return ret;
}

static int fsql_escape_literal(const ObString &in, ObSqlString &out)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < in.length(); ++i) {
    const char c = in.ptr()[i];
    if ('\'' == c) {
      ret = out.append("''");
    } else if ('\\' == c) {
      ret = out.append("\\\\");
    } else {
      ret = out.append(&c, 1);
    }
  }
  return ret;
}

static const char *fsql_generic_type_name(const ObRawExpr &expr)
{
  const ObObjType t = expr.get_result_type().get_type();
  const char *name = NULL;
  if (ob_is_int_tc(t)) {
    name = (ObTinyIntType == t && 1 == expr.get_result_type().get_precision()) ? "BOOLEAN" : "BIGINT";
  } else if (ob_is_uint_tc(t)) {
    name = "UBIGINT";
  } else if (ob_is_float_tc(t) || ob_is_double_tc(t)) {
    name = "DOUBLE";
  } else if (ob_is_number_tc(t) || ob_is_decimal_int_tc(t)) {
    name = "DECIMAL";
  } else if (ObDateType == t || ObMySQLDateType == t) {
    name = "DATE";
  } else if (ob_is_datetime_tc(t) || ObMySQLDateTimeType == t) {
    name = "DATETIME";
  } else if (ob_is_time_tc(t)) {
    name = "TIME";
  } else if (ObNullType == t) {
    name = "NULL";
  } else if (ob_is_json(t)) {
    name = "JSON";
  } else if (ob_is_blob(t, expr.get_result_type().get_collation_type())
             || ob_is_varbinary_or_binary(t, expr.get_result_type().get_collation_type())) {
    name = "BLOB";
  } else {
    name = "VARCHAR";
  }
  return name;
}

int ObFileSqlResolverUtil::resolve_describe(ObResolverParams &params, const ParseNode &select_node,
                                            ObStmt *&stmt)
{
  int ret = OB_SUCCESS;
  ObSelectStmt *inner = NULL;
  ObSqlString sql;
  stmt = NULL;
  if (OB_ISNULL(params.allocator_) || OB_ISNULL(params.session_info_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    HEAP_VAR(ObSelectResolver, resolver, params) {
      if (OB_FAIL(SMART_CALL(resolver.resolve(select_node)))) {
      } else if (OB_ISNULL(inner = resolver.get_select_stmt())) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < inner->get_select_item_size(); ++i) {
    const SelectItem &item = inner->get_select_item(i);
    const ObRawExpr *expr = item.expr_;
    const char *type_name = NULL;
    bool nullable = true;
    if (OB_ISNULL(expr)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      if (expr->is_column_ref_expr()) {
        const ObColumnRefRawExpr *col = static_cast<const ObColumnRefRawExpr *>(expr);
        const TableItem *table = inner->get_table_item_by_id(col->get_table_id());
        const int64_t idx = static_cast<int64_t>(col->get_column_id()) - OB_APP_MIN_COLUMN_ID;
        if (NULL != table && NULL != table->values_table_def_
            && NULL != table->values_table_def_->file_info_
            && idx >= 0 && idx < table->values_table_def_->file_info_->columns_.count()) {
          const ObFileColumnDesc &desc = table->values_table_def_->file_info_->columns_.at(idx);
          type_name = ObFileTypeUtil::type_name(desc.type_);
          nullable = desc.nullable_;
        }
      }
      if (NULL == type_name) {
        type_name = fsql_generic_type_name(*expr);
        nullable = !expr->get_result_type().has_result_flag(NOT_NULL_FLAG);
      }
      const ObString &name = item.alias_name_.empty() ? item.expr_name_ : item.alias_name_;
      if (i > 0 && OB_FAIL(sql.append(" UNION ALL "))) {
      } else if (OB_FAIL(sql.append("SELECT '"))) {
      } else if (OB_FAIL(fsql_escape_literal(name, sql))) {
      } else if (OB_FAIL(sql.append_fmt("' AS column_name, '%s' AS column_type, '%s' AS `null`, "
                                        "NULL AS `key`, NULL AS `default`, NULL AS `extra`",
                                        type_name, nullable ? "YES" : "NO"))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    ParseResult parse_result;
    ObParser parser(*params.allocator_, params.session_info_->get_sql_mode(),
                    params.session_info_->get_charsets4parser());
    ObString sql_str;
    if (OB_FAIL(ob_write_string(*params.allocator_, sql.string(), sql_str))) {
    } else if (OB_FAIL(parser.parse(sql_str, parse_result))) {
      LOG_WARN("failed to parse describe sql", K(ret), K(sql_str));
    } else if (OB_ISNULL(parse_result.result_tree_) || OB_ISNULL(parse_result.result_tree_->children_)
               || OB_ISNULL(parse_result.result_tree_->children_[0])) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      HEAP_VAR(ObSelectResolver, resolver, params) {
        resolver.set_calc_found_rows(true);
        resolver.set_has_top_limit(true);
        if (OB_FAIL(SMART_CALL(resolver.resolve(*parse_result.result_tree_->children_[0])))) {
        }
        stmt = resolver.get_basic_stmt();
      }
    }
  }
  return ret;
}

int ObFileSqlResolverUtil::resolve_copy(ObResolverParams &params, const ParseNode &copy_node,
                                        ObStmt *&stmt)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = params.session_info_;
  ObIAllocator *allocator = params.allocator_;
  const ParseNode *select_node = NULL;
  const ParseNode *path_node = NULL;
  ObFileCopyOptions opts;
  ObString target;
  ObSelectStmt *select_stmt = NULL;
  stmt = NULL;
  if (OB_ISNULL(session) || OB_ISNULL(allocator) || OB_UNLIKELY(4 != copy_node.num_child_)
      || OB_ISNULL(path_node = copy_node.children_[2])) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(check_enabled(session, "COPY TO"))) {
  } else if (OB_FAIL(parse_copy_options(copy_node.children_[3], *allocator, opts))) {
  } else {
    // target: normalize the directory, enforce secure_file_priv, keep the file name
    ObString raw(static_cast<int32_t>(path_node->str_len_), path_node->str_value_);
    const char *slash = static_cast<const char *>(NULL);
    for (int64_t i = raw.length() - 1; i >= 0 && NULL == slash; --i) {
      if ('/' == raw.ptr()[i]) {
        slash = raw.ptr() + i;
      }
    }
    ObString dir = (NULL == slash) ? ObString(".")
                   : (slash == raw.ptr() ? ObString("/") : ObString(static_cast<int32_t>(slash - raw.ptr()), raw.ptr()));
    ObString base = (NULL == slash) ? raw
                    : ObString(static_cast<int32_t>(raw.ptr() + raw.length() - slash - 1), slash + 1);
    ObString real_dir;
    struct stat st;
    if (raw.empty() || base.empty() || 0 == base.compare(".") || 0 == base.compare("..")
        || NULL != MEMCHR(raw.ptr(), '\0', raw.length())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "COPY TO target file name");
    } else if (OB_FAIL(check_file_path(*session, ObString("COPY TO"), dir, false, *allocator, real_dir))) {
    } else if (0 != ::stat(real_dir.ptr(), &st) || !S_ISDIR(st.st_mode)) {
      ret = OB_FILE_NOT_EXIST;
      LOG_USER_ERROR(OB_FILE_NOT_EXIST);
    } else {
      ObSqlString full;
      if (OB_FAIL(full.append(real_dir))) {
      } else if (!(1 == real_dir.length() && '/' == real_dir.ptr()[0]) && OB_FAIL(full.append("/"))) {
      } else if (OB_FAIL(full.append(base))) {
      } else if (full.length() + 64 >= PATH_MAX) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "COPY TO target path (too long)");
      } else if (0 == ::lstat(full.ptr(), &st)) {
        // never overwrite: fail early, the executor re-checks atomically with link(2)
        ret = OB_FILE_ALREADY_EXIST;
        LOG_USER_ERROR(OB_FILE_ALREADY_EXIST);
      } else if (OB_FAIL(ob_write_string(*allocator, full.string(), target, true))) {
      }
    }
  }
  // the source query
  ParseResult parse_result;
  if (OB_FAIL(ret)) {
  } else if (NULL != copy_node.children_[0]) {
    select_node = copy_node.children_[0];
  } else if (OB_ISNULL(copy_node.children_[1]) || OB_ISNULL(copy_node.str_value_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObSqlString sql;
    ObString sql_str;
    ObParser parser(*allocator, session->get_sql_mode(), session->get_charsets4parser());
    if (OB_FAIL(sql.append_fmt("SELECT * FROM %.*s", static_cast<int>(copy_node.str_len_),
                               copy_node.str_value_))) {
    } else if (OB_FAIL(ob_write_string(*allocator, sql.string(), sql_str))) {
    } else if (OB_FAIL(parser.parse(sql_str, parse_result))) {
      LOG_WARN("failed to parse copy source", K(ret), K(sql_str));
    } else if (OB_ISNULL(parse_result.result_tree_) || OB_ISNULL(parse_result.result_tree_->children_)
               || OB_ISNULL(parse_result.result_tree_->children_[0])) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      select_node = parse_result.result_tree_->children_[0];
    }
  }
  if (OB_SUCC(ret)) {
    HEAP_VAR(ObSelectResolver, resolver, params) {
      if (OB_FAIL(SMART_CALL(resolver.resolve(*select_node)))) {
      } else if (OB_ISNULL(select_stmt = resolver.get_select_stmt())) {
        ret = OB_ERR_UNEXPECTED;
      }
      stmt = resolver.get_basic_stmt();
    }
  }
  if (OB_SUCC(ret)) {
    ObSelectIntoItem *into_item = NULL;
    void *buf = NULL;
    if (select_stmt->has_select_into()) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "SELECT ... INTO inside COPY is");
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < select_stmt->get_select_item_size(); ++i) {
      const SelectItem &item = select_stmt->get_select_item(i);
      ObString name;
      if (OB_FAIL(ob_write_string(*allocator, item.alias_name_.empty() ? item.expr_name_
                                                                        : item.alias_name_, name))) {
      } else if (OB_FAIL(opts.col_names_.push_back(name))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(buf = allocator->alloc(sizeof(ObSelectIntoItem)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      into_item = new (buf) ObSelectIntoItem();
      into_item->into_type_ = T_COPY_TO;
      into_item->outfile_name_.set_varchar(target);
      into_item->outfile_name_.set_collation_type(ObCharset::get_system_collation());
      into_item->is_single_ = true;
      if (OB_FAIL(opts.encode(*allocator, into_item->external_properties_))) {
      } else {
        select_stmt->set_select_into(into_item);
        if (NULL != params.query_ctx_) {
          params.query_ctx_->has_file_table_ = true;
        }
      }
    }
  }
  return ret;
}

/* ------------------------------------------------------------------ */
/*  ObDMLResolver: file table items                                    */
/* ------------------------------------------------------------------ */
int ObDMLResolver::gen_file_table_column_items(TableItem &table_item,
                                               const ObIArray<ObString> &names,
                                               const ObIArray<ObRawExprResType> &types)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(params_.expr_factory_) || OB_ISNULL(get_stmt())
      || OB_UNLIKELY(names.count() != types.count() || names.empty())) {
    ret = OB_ERR_UNEXPECTED;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
    ObColumnRefRawExpr *column_expr = NULL;
    if (OB_FAIL(params_.expr_factory_->create_raw_expr(T_REF_COLUMN, column_expr))) {
    } else if (OB_ISNULL(column_expr)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      column_expr->set_result_type(types.at(i));
      column_expr->set_ref_id(table_item.table_id_, i + OB_APP_MIN_COLUMN_ID);
      column_expr->set_column_attr(table_item.table_name_, names.at(i));
      if (OB_FAIL(column_expr->add_flag(IS_COLUMN))) {
      } else {
        ColumnItem column_item;
        column_item.expr_ = column_expr;
        column_item.table_id_ = column_expr->get_table_id();
        column_item.column_id_ = column_expr->get_column_id();
        column_item.column_name_ = column_expr->get_column_name();
        if (OB_FAIL(get_stmt()->add_column_item(column_item))) {
        }
      }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_file_table_item(const ParseNode &table_node, TableItem *&table_item)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  const ParseNode *func_node = NULL;
  const ParseNode *path_node = NULL;
  const ParseNode *alias_node = NULL;
  ObString func;
  ObString raw_path;
  ObString alias_name;
  TableItem *new_item = NULL;
  ObValuesTableDef *def = NULL;
  ObCollationType cs_type = CS_TYPE_INVALID;
  if (OB_ISNULL(stmt) || OB_ISNULL(allocator_) || OB_ISNULL(session_info_)
      || OB_UNLIKELY(T_FILE_TABLE_FUNCTION != table_node.type_ || 4 != table_node.num_child_)
      || OB_ISNULL(func_node = table_node.children_[0])
      || OB_ISNULL(path_node = table_node.children_[1])) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ObFileSqlResolverUtil::check_enabled(session_info_, "file table function"))) {
  } else if (OB_FAIL(session_info_->get_collation_connection(cs_type))) {
  } else {
    func.assign_ptr(func_node->str_value_, static_cast<int32_t>(func_node->str_len_));
    raw_path.assign_ptr(path_node->str_value_, static_cast<int32_t>(path_node->str_len_));
    alias_node = table_node.children_[3];
    if (NULL != alias_node) {
      alias_name.assign_ptr(alias_node->str_value_, static_cast<int32_t>(alias_node->str_len_));
    } else if (OB_FAIL(stmt->generate_values_table_name(*allocator_, alias_name))) {
    }
  }
  if (OB_SUCC(ret)) {
    void *buf = NULL;
    if (OB_ISNULL(new_item = stmt->create_table_item(*allocator_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_ISNULL(buf = allocator_->alloc(sizeof(ObValuesTableDef)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      def = new (buf) ObValuesTableDef();
      def->is_file_table_ = true;
      def->is_const_ = false;
      new_item->values_table_def_ = def;
      new_item->table_id_ = generate_table_id();
      new_item->table_name_ = alias_name;
      new_item->alias_name_ = alias_name;
      new_item->type_ = TableItem::VALUES_TABLE;
      new_item->is_view_table_ = false;
    }
  }
  ObSEArray<ObString, 16> names;
  ObSEArray<ObRawExprResType, 16> types;
  if (OB_FAIL(ret)) {
  } else if (fsql_name_eq(func, "glob")) {
    // glob(pattern): one VARCHAR column named "file", one row per matching regular file
    char pattern[PATH_MAX + 1];
    glob_t g;
    MEMSET(&g, 0, sizeof(g));
    int64_t max_len = 1;
    if (NULL != table_node.children_[2]) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "options of glob are");
    } else if (raw_path.empty() || raw_path.length() >= PATH_MAX
               || NULL != MEMCHR(raw_path.ptr(), '\0', raw_path.length())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "glob: invalid pattern");
    } else {
      MEMCPY(pattern, raw_path.ptr(), raw_path.length());
      pattern[raw_path.length()] = '\0';
      int gret = ::glob(pattern, GLOB_ERR | GLOB_NOSORT, NULL, &g);
      if (0 != gret && GLOB_NOMATCH != gret) {
        ret = OB_IO_ERROR;
        LOG_WARN("glob failed", K(ret), K(gret), K(raw_path));
      } else {
        // sort for a deterministic result, like DuckDB
        ObSEArray<ObString, 16> files;
        for (size_t i = 0; OB_SUCC(ret) && 0 == gret && i < g.gl_pathc; ++i) {
          char real_buf[PATH_MAX + 1];
          struct stat st;
          bool allowed = false;
          if (NULL == ::realpath(g.gl_pathv[i], real_buf) || 0 != ::stat(real_buf, &st)
              || !S_ISREG(st.st_mode)) {
            // directories, dangling links etc. are skipped
          } else if (OB_FAIL(ObFileSqlResolverUtil::is_path_allowed(*session_info_, real_buf, allowed))) {
          } else if (!allowed) {
            // silently hide files outside secure_file_priv
          } else if (files.count() >= ObFileSqlResolverUtil::MAX_GLOB_FILES) {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "glob matching more than 10000 files is");
          } else {
            ObString f;
            if (OB_FAIL(ob_write_string(*allocator_, ObString(g.gl_pathv[i]), f))) {
            } else if (OB_FAIL(files.push_back(f))) {
            }
          }
        }
        if (OB_SUCC(ret)) {
          lib::ob_sort(files.begin(), files.end(),
                       [](const ObString &a, const ObString &b) { return a.compare(b) < 0; });
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < files.count(); ++i) {
          ObObjParam obj;
          obj.set_varchar(files.at(i));
          obj.set_collation_type(cs_type);
          obj.set_collation_level(CS_LEVEL_IMPLICIT);
          obj.set_param_meta();
          max_len = MAX(max_len, files.at(i).length());
          if (OB_FAIL(def->access_objs_.push_back(obj))) {
          }
        }
        if (OB_SUCC(ret)) {
          def->row_cnt_ = files.count();
        }
      }
      ::globfree(&g);
    }
    if (OB_SUCC(ret)) {
      ObRawExprResType t;
      t.set_varchar();
      t.set_length(static_cast<ObLength>(MIN(max_len, OB_MAX_VARCHAR_LENGTH / 4)));
      t.set_length_semantics(LS_CHAR);
      t.set_collation_type(cs_type);
      t.set_collation_level(CS_LEVEL_IMPLICIT);
      def->access_type_ = ObValuesTableDef::ACCESS_OBJ;
      def->column_cnt_ = 1;
      if (OB_FAIL(names.push_back(ObString("file")))) {
      } else if (OB_FAIL(types.push_back(t))) {
      } else if (OB_FAIL(def->column_types_.push_back(t))) {
      } else if (OB_FAIL(def->column_ndvs_.push_back(MAX(def->row_cnt_, 1L)))) {
      } else if (OB_FAIL(def->column_nnvs_.push_back(0))) {
      }
    }
  } else {
    // read_csv / read_json / read_ndjson / read_parquet
    ObFileTableInfo *info = NULL;
    void *buf = NULL;
    ObFileErrorInfo err;
    if (OB_ISNULL(buf = allocator_->alloc(sizeof(ObFileTableInfo)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (FALSE_IT(info = new (buf) ObFileTableInfo())) {
    } else if (OB_FAIL(ObFileSqlResolverUtil::parse_scan_options(func, table_node.children_[2],
                                                                 *allocator_, info->options_))) {
    } else if (OB_FAIL(ObFileSqlResolverUtil::check_file_path(*session_info_, func, raw_path, true,
                                                              *allocator_, info->path_))) {
    } else if (OB_FAIL(ob_write_string(*allocator_, func, info->func_name_))) {
    } else if (OB_FAIL(ObFileSchemaInferrer::infer(info->path_.ptr(), info->options_, *allocator_,
                                                   info->columns_, info->row_count_,
                                                   info->identity_, err))) {
      LOG_WARN("failed to infer file schema", K(ret), K(info->path_));
      ret = ObFileSqlErrorReporter::report(ret, func, raw_path, err);
    } else if (info->columns_.count() > ObFileScanOptions::MAX_COLUMN_COUNT) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "files with more than 4096 columns are");
    } else {
      def->access_type_ = ObValuesTableDef::ACCESS_FILE;
      def->file_info_ = info;
      def->column_cnt_ = info->columns_.count();
      def->row_cnt_ = info->row_count_;
      for (int64_t i = 0; OB_SUCC(ret) && i < info->columns_.count(); ++i) {
        const ObFileColumnDesc &col = info->columns_.at(i);
        ObRawExprResType t;
        const int64_t rows = MAX(info->row_count_, 1);
        if (OB_FAIL(ObFileSqlResolverUtil::file_column_res_type(col, cs_type, t))) {
        } else if (OB_FAIL(names.push_back(col.name_))) {
        } else if (OB_FAIL(types.push_back(t))) {
        } else if (OB_FAIL(def->column_types_.push_back(t))) {
        } else if (OB_FAIL(def->column_ndvs_.push_back(FILE_COL_BOOLEAN == col.type_ ? MIN(2L, rows) : rows))) {
        } else if (OB_FAIL(def->column_nnvs_.push_back(col.nullable_ ? MAX(rows / 10, 1L) : 0L))) {
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(stmt->add_table_item(session_info_, new_item))) {
    } else if (OB_FAIL(gen_file_table_column_items(*new_item, names, types))) {
    } else {
      table_item = new_item;
      if (NULL != params_.query_ctx_) {
        params_.query_ctx_->has_file_table_ = true;
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
