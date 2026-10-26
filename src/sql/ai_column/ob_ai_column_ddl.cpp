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
#include "sql/ai_column/ob_ai_column_ddl.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "lib/time/ob_time_utility.h"
#include "share/ob_server_struct.h"

namespace oceanbase
{
using namespace common;
using namespace common::sqlclient;
namespace sql
{

static int ai_ddl_quote_ident(const ObString &name, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("`"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    const char c = name.ptr()[i];
    if ('`' == c) {
      ret = sql.append("``");
    } else {
      ret = sql.append(ObString(1, &name.ptr()[i]));
    }
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("`");
  }
  return ret;
}

static int ai_ddl_append_literal(const ObString &value, ObIAllocator &alloc, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  ObString escaped;
  if (OB_FAIL(ObAIColumnUtil::escape_sql_string(value, alloc, escaped))) {
    LOG_WARN("escape failed", K(ret));
  } else if (OB_FAIL(sql.append_fmt("'%.*s'", escaped.length(), escaped.ptr()))) {
    LOG_WARN("append failed", K(ret));
  }
  return ret;
}

static int ai_ddl_append_id_list(const ObIArray<uint64_t> &ids, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < ids.count(); ++i) {
    ret = sql.append_fmt("%s%lu", 0 == i ? "" : ",", ids.at(i));
  }
  return ret;
}

int ObAIColumnDDLUtil::gen_request_id(char *buf, const int64_t buf_len)
{
  int ret = OB_SUCCESS;
  static int64_t seq = 0;
  const int64_t s = ATOMIC_AAF(&seq, 1);
  const uint64_t addr_hash = GCTX.self_addr().hash();
  if (OB_ISNULL(buf) || buf_len < 33) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    snprintf(buf, buf_len, "%016lx%08lx%08lx",
             static_cast<uint64_t>(ObTimeUtility::current_time()),
             static_cast<uint64_t>(addr_hash & 0xffffffffUL),
             static_cast<uint64_t>(s & 0xffffffffUL));
  }
  return ret;
}

int ObAIColumnDDLUtil::append_task_delete_sql(const uint64_t table_id,
                                              const ObIArray<uint64_t> &column_ids,
                                              const ObString &escaped_predicate,
                                              ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append_fmt("delete from oceanbase.__all_ai_column_task where table_id = %lu"
                             " and rowkey_predicate = '%.*s' and column_id in (",
                             table_id, escaped_predicate.length(), escaped_predicate.ptr()))) {
  } else if (OB_FAIL(ai_ddl_append_id_list(column_ids, sql))) {
  } else if (OB_FAIL(sql.append(")"))) {
  }
  return ret;
}

int ObAIColumnDDLUtil::append_task_insert_sql(const uint64_t table_id,
                                              const ObIArray<uint64_t> &column_ids,
                                              const ObString &escaped_predicate,
                                              const ObString &request_id,
                                              ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append_fmt(
      "insert into oceanbase.__all_ai_column_task (table_id, column_id, rowkey_predicate,"
      " request_id, def_version, state, retry_count, max_retries, eligible_ts, owner,"
      " lease_expire_ts) select table_id, column_id, '%.*s', '%.*s', def_version, %d, 0,"
      " max_retries, now(6), NULL, 0 from oceanbase.__all_ai_column where table_id = %lu"
      " and column_id in (",
      escaped_predicate.length(), escaped_predicate.ptr(),
      request_id.length(), request_id.ptr(), AI_TASK_PENDING, table_id))) {
  } else if (OB_FAIL(ai_ddl_append_id_list(column_ids, sql))) {
  } else if (OB_FAIL(sql.append(")"))) {
  }
  return ret;
}

int ObAIColumnDDLUtil::print_rowkey_predicate(const ObIArray<ObString> &names,
                                              const ObIArray<ObObj> &values,
                                              const ObTimeZoneInfo *tz_info,
                                              ObIAllocator &alloc,
                                              ObString &predicate)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  int64_t len = 0;
  int64_t pos = 0;
  ObObjPrintParams print_params(tz_info);
  if (names.count() != values.count() || names.count() <= 0 || OB_ISNULL(tz_info)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(names.count()), K(values.count()), KP(tz_info));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
    if (OB_FAIL(databuff_printf(buf, len, pos, alloc, "%s`%.*s` = ", 0 == i ? "" : " and ",
                                names.at(i).length(), names.at(i).ptr()))) {
    } else if (OB_FAIL(values.at(i).print_sql_literal(buf, len, pos, alloc, print_params))) {
    }
  }
  if (OB_SUCC(ret)) {
    predicate.assign_ptr(buf, static_cast<int32_t>(pos));
  }
  return ret;
}

int ObAIColumnDDLUtil::get_table_id(ObISQLClient &client,
                                    const ObString &db_name,
                                    const ObString &table_name,
                                    uint64_t &table_id)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc;
  ObSqlString sql;
  table_id = OB_INVALID_ID;
  if (OB_FAIL(sql.append("select t.table_id as table_id from oceanbase.__all_table t join "
                         "oceanbase.__all_database d on d.database_id = t.database_id where "
                         "d.database_name = "))) {
  } else if (OB_FAIL(ai_ddl_append_literal(db_name, alloc, sql))) {
  } else if (OB_FAIL(sql.append(" and t.table_name = "))) {
  } else if (OB_FAIL(ai_ddl_append_literal(table_name, alloc, sql))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(client.read(res, sql.ptr()))) {
        LOG_WARN("read failed", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_TABLE_NOT_EXIST;
        }
        LOG_WARN("table not found", K(ret), K(sql));
      } else {
        int64_t v = 0;
        EXTRACT_INT_FIELD_MYSQL(*result, "table_id", v, int64_t);
        table_id = static_cast<uint64_t>(v);
      }
    }
  }
  return ret;
}

struct ObAIColumnNameId
{
  ObAIColumnNameId() : name_(), id_(OB_INVALID_ID) {}
  TO_STRING_KV(K_(name), K_(id));
  ObString name_;
  uint64_t id_;
};

static int ai_ddl_load_columns(ObISQLClient &client, const uint64_t table_id,
                               ObIAllocator &alloc, ObIArray<ObAIColumnNameId> &columns)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.append_fmt("select column_id, column_name from oceanbase.__all_column "
                             "where table_id = %lu", table_id))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(client.read(res, sql.ptr()))) {
        LOG_WARN("read failed", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      }
      while (OB_SUCC(ret) && OB_SUCC(result->next())) {
        ObAIColumnNameId item;
        int64_t id = 0;
        ObString name;
        EXTRACT_INT_FIELD_MYSQL(*result, "column_id", id, int64_t);
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "column_name", name);
        if (OB_SUCC(ret)) {
          item.id_ = static_cast<uint64_t>(id);
          if (OB_FAIL(ob_write_string(alloc, name, item.name_))) {
          } else if (OB_FAIL(columns.push_back(item))) {
          }
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
  }
  return ret;
}

static uint64_t ai_ddl_find_column(const ObIArray<ObAIColumnNameId> &columns, const ObString &name)
{
  uint64_t id = OB_INVALID_ID;
  for (int64_t i = 0; OB_INVALID_ID == id && i < columns.count(); ++i) {
    if (0 == columns.at(i).name_.case_compare(name)) {
      id = columns.at(i).id_;
    }
  }
  return id;
}

int ObAIColumnDDLUtil::get_rowkey_column_names(ObISQLClient &client,
                                               const uint64_t table_id,
                                               ObIAllocator &alloc,
                                               ObIArray<ObString> &names)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.append_fmt("select column_name from oceanbase.__all_column where table_id = %lu"
                             " and rowkey_position > 0 order by rowkey_position", table_id))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(client.read(res, sql.ptr()))) {
        LOG_WARN("read failed", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      }
      while (OB_SUCC(ret) && OB_SUCC(result->next())) {
        ObString name;
        ObString copy;
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "column_name", name);
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(ob_write_string(alloc, name, copy))) {
        } else if (OB_FAIL(names.push_back(copy))) {
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
  }
  if (OB_SUCC(ret) && names.count() <= 0) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("AI column table has no primary key", K(ret), K(table_id));
  }
  return ret;
}

int ObAIColumnDDLUtil::invalidate_rows(ObISQLClient &client,
                                       const ObString &db_name,
                                       const ObString &table_name,
                                       const uint64_t table_id,
                                       const uint64_t column_id,
                                       const ObString &column_name,
                                       const ObString &where_text,
                                       const ObTimeZoneInfo *tz_info,
                                       ObIAllocator &alloc,
                                       int64_t &row_count)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObString, 4> pk_names;
  ObSEArray<ObString, 16> predicates;
  ObString state_name;
  ObString req_name;
  ObString spec_name;
  ObString rewritten_where;
  ObSqlString sql;
  row_count = 0;
  if (OB_FAIL(get_rowkey_column_names(client, table_id, alloc, pk_names))) {
    LOG_WARN("get rowkey names failed", K(ret));
  } else if (OB_FAIL(ObAIColumnUtil::build_hidden_column_name(AI_HIDDEN_STATE, column_name, alloc, state_name))
             || OB_FAIL(ObAIColumnUtil::build_hidden_column_name(AI_HIDDEN_REQUEST, column_name, alloc, req_name))
             || OB_FAIL(ObAIColumnUtil::build_hidden_column_name(AI_HIDDEN_RESULT_SPEC, column_name, alloc, spec_name))) {
    LOG_WARN("build hidden names failed", K(ret));
  } else if (!where_text.empty()
             && OB_FAIL(ObAIColumnUtil::rewrite_status_pseudo_column(where_text, state_name, alloc,
                                                                     rewritten_where))) {
    LOG_WARN("rewrite status failed", K(ret));
  } else if (OB_FAIL(sql.append("select "))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < pk_names.count(); ++i) {
    if (i > 0 && OB_FAIL(sql.append(", "))) {
    } else if (OB_FAIL(ai_ddl_quote_ident(pk_names.at(i), sql))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(sql.append(" from "))) {
  } else if (OB_FAIL(ai_ddl_quote_ident(db_name, sql))) {
  } else if (OB_FAIL(sql.append("."))) {
  } else if (OB_FAIL(ai_ddl_quote_ident(table_name, sql))) {
  } else if (!rewritten_where.empty()
             && OB_FAIL(sql.append_fmt(" where (%.*s)", rewritten_where.length(),
                                       rewritten_where.ptr()))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(client.read(res, sql.ptr()))) {
        LOG_WARN("read failed", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      }
      while (OB_SUCC(ret) && OB_SUCC(result->next())) {
        ObSEArray<ObObj, 4> values;
        ObString predicate;
        for (int64_t i = 0; OB_SUCC(ret) && i < pk_names.count(); ++i) {
          ObObj obj;
          if (OB_FAIL(result->get_obj(i, obj))) {
            LOG_WARN("get obj failed", K(ret));
          } else if (OB_FAIL(values.push_back(obj))) {
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(print_rowkey_predicate(pk_names, values, tz_info, alloc, predicate))) {
          LOG_WARN("print predicate failed", K(ret));
        } else if (OB_FAIL(predicates.push_back(predicate))) {
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
  }
  ObSEArray<uint64_t, 1> column_ids;
  OZ (column_ids.push_back(column_id));
  for (int64_t i = 0; OB_SUCC(ret) && i < predicates.count(); ++i) {
    int64_t affected_rows = 0;
    ObString escaped;
    char request_id[REQUEST_ID_BUF_LEN];
    const ObString &pred = predicates.at(i);
    sql.reuse();
    if (OB_FAIL(ObAIColumnUtil::escape_sql_string(pred, alloc, escaped))) {
    } else if (OB_FAIL(gen_request_id(request_id, sizeof(request_id)))) {
    } else if (OB_FAIL(sql.append("update "))) {
    } else if (OB_FAIL(ai_ddl_quote_ident(db_name, sql)) || OB_FAIL(sql.append("."))
               || OB_FAIL(ai_ddl_quote_ident(table_name, sql)) || OB_FAIL(sql.append(" set "))
               || OB_FAIL(ai_ddl_quote_ident(column_name, sql)) || OB_FAIL(sql.append(" = NULL, "))
               || OB_FAIL(ai_ddl_quote_ident(state_name, sql))
               || OB_FAIL(sql.append_fmt(" = %d, ", AI_ROW_PENDING))
               || OB_FAIL(ai_ddl_quote_ident(req_name, sql)) || OB_FAIL(sql.append(" = NULL, "))
               || OB_FAIL(ai_ddl_quote_ident(spec_name, sql))
               || OB_FAIL(sql.append_fmt(" = NULL where %.*s", pred.length(), pred.ptr()))) {
      LOG_WARN("build sql failed", K(ret));
    } else if (OB_FAIL(client.write(sql.ptr(), affected_rows))) {
      LOG_WARN("invalidate row failed", K(ret), K(sql));
    } else if (FALSE_IT(sql.reuse())) {
    } else if (OB_FAIL(append_task_delete_sql(table_id, column_ids, escaped, sql))) {
    } else if (OB_FAIL(client.write(sql.ptr(), affected_rows))) {
      LOG_WARN("delete task failed", K(ret), K(sql));
    } else if (FALSE_IT(sql.reuse())) {
    } else if (OB_FAIL(append_task_insert_sql(table_id, column_ids, escaped,
                                              ObString::make_string(request_id), sql))) {
    } else if (OB_FAIL(client.write(sql.ptr(), affected_rows))) {
      LOG_WARN("insert task failed", K(ret), K(sql));
    } else {
      ++row_count;
    }
  }
  return ret;
}

int ObAIColumnDDLUtil::persist_definitions(const ObString &db_name,
                                           const ObString &table_name,
                                           const ObIArray<ObAIColumnDef> &defs,
                                           const bool backfill,
                                           const ObTimeZoneInfo *tz_info)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  ObArenaAllocator alloc("AIColDDL");
  uint64_t table_id = OB_INVALID_ID;
  ObSEArray<ObAIColumnNameId, 16> columns;
  if (defs.count() <= 0) {
  } else if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
    LOG_WARN("start trans failed", K(ret));
  } else if (OB_FAIL(get_table_id(trans, db_name, table_name, table_id))) {
    LOG_WARN("get table id failed", K(ret), K(db_name), K(table_name));
  } else if (OB_FAIL(ai_ddl_load_columns(trans, table_id, alloc, columns))) {
    LOG_WARN("load columns failed", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < defs.count(); ++i) {
    ObAIColumnDef def;
    ObString names[AI_HIDDEN_MAX];
    ObString dep_str;
    ObSqlString sql;
    int64_t affected_rows = 0;
    if (OB_FAIL(def.deep_copy(defs.at(i), alloc))) {
    } else {
      def.table_id_ = table_id;
      def.column_id_ = ai_ddl_find_column(columns, def.column_name_);
    }
    for (int64_t k = 0; OB_SUCC(ret) && k < AI_HIDDEN_MAX; ++k) {
      if (OB_FAIL(ObAIColumnUtil::build_hidden_column_name(static_cast<ObAIColumnHiddenKind>(k),
                                                           def.column_name_, alloc, names[k]))) {
      }
    }
    if (OB_SUCC(ret)) {
      def.state_column_id_ = ai_ddl_find_column(columns, names[AI_HIDDEN_STATE]);
      def.request_column_id_ = ai_ddl_find_column(columns, names[AI_HIDDEN_REQUEST]);
      def.result_spec_column_id_ = ai_ddl_find_column(columns, names[AI_HIDDEN_RESULT_SPEC]);
      if (def.dependent_column_names_.count() > 0) {
        def.dependent_column_ids_.reset();
        for (int64_t k = 0; OB_SUCC(ret) && k < def.dependent_column_names_.count(); ++k) {
          const uint64_t dep_id = ai_ddl_find_column(columns, def.dependent_column_names_.at(k));
          if (OB_INVALID_ID == dep_id) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("dependency column not found", K(ret), K(def));
          } else if (!def.depends_on(dep_id) && OB_FAIL(def.dependent_column_ids_.push_back(dep_id))) {
          }
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_INVALID_ID == def.column_id_ || OB_INVALID_ID == def.state_column_id_
               || OB_INVALID_ID == def.request_column_id_
               || OB_INVALID_ID == def.result_spec_column_id_) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("AI column ids not found after DDL", K(ret), K(def));
    } else if (OB_FAIL(def.dependent_ids_to_string(alloc, dep_str))) {
    } else if (OB_FAIL(sql.append_fmt("delete from oceanbase.__all_ai_column where table_id = %lu"
                                      " and column_id = %lu", table_id, def.column_id_))) {
    } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
      LOG_WARN("delete old metadata failed", K(ret), K(sql));
    } else if (FALSE_IT(sql.reuse())) {
    } else if (OB_FAIL(sql.append_fmt("insert into oceanbase.__all_ai_column (table_id, column_id,"
                   " column_name, state_column_id, request_column_id, result_spec_column_id,"
                   " func_type, model_name, input_expr, normalized_expr, type_prompt,"
                   " dependent_column_ids, sync_mode, max_retries, vector_dim, def_version)"
                   " values (%lu, %lu, ", table_id, def.column_id_))) {
    } else if (OB_FAIL(ai_ddl_append_literal(def.column_name_, alloc, sql))) {
    } else if (OB_FAIL(sql.append_fmt(", %lu, %lu, %lu, %ld, ", def.state_column_id_,
                                      def.request_column_id_, def.result_spec_column_id_,
                                      def.func_type_))) {
    } else if (OB_FAIL(ai_ddl_append_literal(def.model_name_, alloc, sql))
               || OB_FAIL(sql.append(", "))
               || OB_FAIL(ai_ddl_append_literal(def.input_expr_, alloc, sql))
               || OB_FAIL(sql.append(", "))
               || OB_FAIL(ai_ddl_append_literal(def.normalized_expr_, alloc, sql))
               || OB_FAIL(sql.append(", "))
               || OB_FAIL(ai_ddl_append_literal(def.type_prompt_, alloc, sql))
               || OB_FAIL(sql.append(", "))
               || OB_FAIL(ai_ddl_append_literal(dep_str, alloc, sql))
               || OB_FAIL(sql.append(", "))
               || OB_FAIL(ai_ddl_append_literal(def.sync_mode_, alloc, sql))) {
    } else if (OB_FAIL(sql.append_fmt(", %ld, %ld, %ld)", def.max_retries_, def.vector_dim_,
                                      def.def_version_))) {
    } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
      LOG_WARN("insert metadata failed", K(ret), K(sql));
    } else if (backfill) {
      int64_t row_count = 0;
      if (OB_FAIL(invalidate_rows(trans, db_name, table_name, table_id, def.column_id_,
                                  def.column_name_, ObString(), tz_info, alloc, row_count))) {
        LOG_WARN("backfill failed", K(ret));
      }
    }
  }
  if (trans.is_started()) {
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = trans.end(OB_SUCC(ret)))) {
      LOG_WARN("end trans failed", K(tmp_ret));
      ret = OB_SUCC(ret) ? tmp_ret : ret;
    }
  }
  return ret;
}

int ObAIColumnDDLUtil::retry_column(const ObString &db_name,
                                    const ObString &table_name,
                                    const ObString &column_name,
                                    const ObString &where_text,
                                    const ObTimeZoneInfo *tz_info)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  ObArenaAllocator alloc("AIColRetry");
  uint64_t table_id = OB_INVALID_ID;
  uint64_t column_id = OB_INVALID_ID;
  ObSEArray<ObAIColumnNameId, 16> columns;
  int64_t row_count = 0;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
    LOG_WARN("start trans failed", K(ret));
  } else if (OB_FAIL(get_table_id(trans, db_name, table_name, table_id))) {
    LOG_WARN("get table id failed", K(ret));
  } else if (OB_FAIL(ai_ddl_load_columns(trans, table_id, alloc, columns))) {
    LOG_WARN("load columns failed", K(ret));
  } else if (OB_INVALID_ID == (column_id = ai_ddl_find_column(columns, column_name))) {
    ret = OB_ERR_BAD_FIELD_ERROR;
    LOG_USER_ERROR(OB_ERR_BAD_FIELD_ERROR, column_name.length(), column_name.ptr(),
                   table_name.length(), table_name.ptr());
  } else if (OB_FAIL(invalidate_rows(trans, db_name, table_name, table_id, column_id, column_name,
                                     where_text, tz_info, alloc, row_count))) {
    LOG_WARN("invalidate rows failed", K(ret));
  }
  if (trans.is_started()) {
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = trans.end(OB_SUCC(ret)))) {
      LOG_WARN("end trans failed", K(tmp_ret));
      ret = OB_SUCC(ret) ? tmp_ret : ret;
    }
  }
  LOG_INFO("AI column retry", K(ret), K(db_name), K(table_name), K(column_name), K(row_count));
  return ret;
}

int ObAIColumnDDLUtil::cleanup_table(const uint64_t table_id)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(sql.append_fmt("delete from oceanbase.__all_ai_column_task where table_id = %lu",
                                    table_id))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
    LOG_WARN("cleanup tasks failed", K(ret), K(sql));
  } else if (FALSE_IT(sql.reuse())) {
  } else if (OB_FAIL(sql.append_fmt("delete from oceanbase.__all_ai_column where table_id = %lu",
                                    table_id))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
    LOG_WARN("cleanup metadata failed", K(ret), K(sql));
  }
  return ret;
}

} // end namespace sql
} // end namespace oceanbase
