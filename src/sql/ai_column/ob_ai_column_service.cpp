/**
 * Copyright (c) 2026 OceanBase.
 *
 * AI COLUMN asynchronous worker, see ob_ai_column_service.h.
 */

#define USING_LOG_PREFIX SQL
#include "sql/ai_column/ob_ai_column_service.h"
#include "sql/ai_column/ob_ai_column_define.h"
#include "sql/ai_column/ob_ai_column_ddl.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_client.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "share/ob_server_struct.h"
#include "lib/time/ob_time_utility.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

static int ai_svc_quote_ident(const ObString &name, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("`"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if ('`' == name.ptr()[i]) {
      ret = sql.append("``");
    } else {
      ret = sql.append(name.ptr() + i, 1);
    }
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("`");
  }
  return ret;
}

static int ai_svc_append_literal(const ObString &value, ObIAllocator &alloc, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  ObString escaped;
  if (OB_FAIL(ObAIColumnUtil::escape_sql_string(value, alloc, escaped))) {
  } else if (OB_FAIL(sql.append_fmt("'%.*s'", escaped.length(), escaped.ptr()))) {
  }
  return ret;
}

static int ai_svc_append_group_cond(const ObAIColumnTaskItem &head, ObIAllocator &alloc,
                                    ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append_fmt(" table_id = %lu and rowkey_predicate = ", head.table_id_))) {
  } else if (OB_FAIL(ai_svc_append_literal(head.predicate_, alloc, sql))) {
  } else if (OB_FAIL(sql.append(" and request_id = "))) {
  } else if (OB_FAIL(ai_svc_append_literal(head.request_id_, alloc, sql))) {
  }
  return ret;
}

ObAIColumnService &ObAIColumnService::get_instance()
{
  static ObAIColumnService instance;
  return instance;
}

int ObAIColumnService::init()
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
  } else if (FALSE_IT(set_run_wrapper(share::server_runtime()))) {
  } else if (OB_FAIL(share::ObThreadPool::init())) {
    LOG_WARN("init AI column worker failed", K(ret));
  } else {
    is_inited_ = true;
  }
  return ret;
}

int ObAIColumnService::start()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(share::ObThreadPool::start())) {
    LOG_WARN("start AI column worker failed", K(ret));
  } else {
    LOG_INFO("AI column worker started");
  }
  return ret;
}

void ObAIColumnService::stop()
{
  if (is_inited_) {
    share::ObThreadPool::stop();
  }
}

void ObAIColumnService::wait()
{
  if (is_inited_) {
    share::ObThreadPool::wait();
  }
}

void ObAIColumnService::destroy()
{
  if (is_inited_) {
    stop();
    wait();
    share::ObThreadPool::destroy();
    is_inited_ = false;
  }
}

void ObAIColumnService::run1()
{
  lib::set_thread_name("AIColumnWorker");
  while (!has_set_stop()) {
    int64_t processed = 0;
    if (GCTX.status_ != share::SS_SERVING || OB_ISNULL(GCTX.sql_proxy_)
        || GCTX.start_service_time_ <= 0) {
    } else {
      int ret = run_once(processed);
      if (OB_FAIL(ret)) {
        LOG_WARN("AI column worker round failed", K(ret));
      }
    }
    if (0 == processed && !has_set_stop()) {
      ob_usleep(IDLE_INTERVAL_US);
    }
  }
}

int ObAIColumnService::extract_model_text(ObIAllocator &alloc, const ObString &raw, ObString &text)
{
  // completion providers return a JSON string; unwrap it so the column gets
  // the plain model answer
  int ret = OB_SUCCESS;
  text = raw;
  ObString trimmed = raw;
  trimmed = trimmed.trim();
  if (trimmed.length() >= 2 && '"' == trimmed.ptr()[0] && '"' == trimmed.ptr()[trimmed.length() - 1]) {
    ObIJsonBase *j_base = NULL;
    if (OB_FAIL(ObJsonBaseFactory::get_json_base(&alloc, trimmed, ObJsonInType::JSON_TREE,
                                                 ObJsonInType::JSON_TREE, j_base))) {
      ret = OB_SUCCESS;  // keep raw text
    } else if (OB_NOT_NULL(j_base) && ObJsonNodeType::J_STRING == j_base->json_type()) {
      text.assign_ptr(j_base->get_data(), static_cast<int32_t>(j_base->get_data_length()));
    }
  }
  return ret;
}

int ObAIColumnService::run_once(int64_t &processed)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc("AIColWorker");
  ObSEArray<ObAIColumnTaskItem, 16> tasks;
  processed = 0;
  if (OB_FAIL(fetch_tasks(alloc, tasks))) {
    LOG_WARN("fetch AI column tasks failed", K(ret));
  } else {
    // group by (table, predicate, request): tasks are ordered so groups are contiguous
    int64_t begin = 0;
    while (begin < tasks.count() && !has_set_stop()) {
      int64_t end = begin + 1;
      const ObAIColumnTaskItem &head = tasks.at(begin);
      while (end < tasks.count()
             && tasks.at(end).table_id_ == head.table_id_
             && tasks.at(end).predicate_ == head.predicate_
             && tasks.at(end).request_id_ == head.request_id_) {
        ++end;
      }
      ObSEArray<ObAIColumnTaskItem, 4> group;
      int tmp_ret = OB_SUCCESS;
      for (int64_t i = begin; OB_SUCCESS == tmp_ret && i < end; ++i) {
        tmp_ret = group.push_back(tasks.at(i));
      }
      if (OB_SUCCESS == tmp_ret && OB_SUCCESS != (tmp_ret = process_group(group))) {
        LOG_WARN("process AI column task group failed", K(tmp_ret), K(head));
      }
      ++processed;
      begin = end;
    }
  }
  return ret;
}

int ObAIColumnService::fetch_tasks(ObIAllocator &alloc, ObIArray<ObAIColumnTaskItem> &tasks)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  const int64_t now = ObTimeUtility::current_time();
  if (OB_FAIL(sql.append_fmt(
      "select table_id, column_id, rowkey_predicate, request_id, def_version, retry_count,"
      " max_retries from oceanbase.__all_ai_column_task where"
      " (state in (%d, %d) and eligible_ts <= now(6)) or (state = %d and lease_expire_ts < %ld)"
      " order by table_id, rowkey_predicate, request_id, column_id limit %ld",
      AI_TASK_PENDING, AI_TASK_RETRY_WAIT, AI_TASK_RUNNING, now, BATCH_TASKS))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        LOG_WARN("read tasks failed", K(ret));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      }
      while (OB_SUCC(ret) && OB_SUCC(result->next())) {
        ObAIColumnTaskItem item;
        int64_t v = 0;
        ObString pred;
        ObString rid;
        EXTRACT_INT_FIELD_MYSQL(*result, "table_id", v, int64_t);
        item.table_id_ = static_cast<uint64_t>(v);
        EXTRACT_INT_FIELD_MYSQL(*result, "column_id", v, int64_t);
        item.column_id_ = static_cast<uint64_t>(v);
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "rowkey_predicate", pred);
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "request_id", rid);
        EXTRACT_INT_FIELD_MYSQL(*result, "def_version", item.def_version_, int64_t);
        EXTRACT_INT_FIELD_MYSQL(*result, "retry_count", item.retry_count_, int64_t);
        EXTRACT_INT_FIELD_MYSQL(*result, "max_retries", item.max_retries_, int64_t);
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(ob_write_string(alloc, pred, item.predicate_))) {
        } else if (OB_FAIL(ob_write_string(alloc, rid, item.request_id_))) {
        } else if (OB_FAIL(tasks.push_back(item))) {
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
  }
  return ret;
}

int ObAIColumnService::load_table(ObIAllocator &alloc, const uint64_t table_id,
                                  ObString &db_name, ObString &table_name,
                                  ObIArray<ObAIColumnRuntimeDef> &defs, bool &table_exists)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  table_exists = false;
  if (OB_FAIL(sql.append_fmt("select d.database_name as db, t.table_name as tbl from"
                             " oceanbase.__all_table t join oceanbase.__all_database d on"
                             " d.database_id = t.database_id where t.table_id = %lu", table_id))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      } else {
        ObString db;
        ObString tbl;
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "db", db);
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "tbl", tbl);
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(ob_write_string(alloc, db, db_name))) {
        } else if (OB_FAIL(ob_write_string(alloc, tbl, table_name))) {
        } else {
          table_exists = true;
        }
      }
    }
  }
  if (OB_SUCC(ret) && table_exists) {
    sql.reuse();
    if (OB_FAIL(sql.append_fmt("select column_id, column_name, func_type, model_name, input_expr,"
                               " type_prompt, def_version from oceanbase.__all_ai_column"
                               " where table_id = %lu", table_id))) {
    } else {
      SMART_VAR(ObMySQLProxy::MySQLResult, res) {
        ObMySQLResult *result = NULL;
        if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        }
        while (OB_SUCC(ret) && OB_SUCC(result->next())) {
          ObAIColumnRuntimeDef def;
          int64_t v = 0;
          ObString name;
          ObString model;
          ObString input;
          ObString prompt;
          EXTRACT_INT_FIELD_MYSQL(*result, "column_id", v, int64_t);
          def.column_id_ = static_cast<uint64_t>(v);
          EXTRACT_VARCHAR_FIELD_MYSQL(*result, "column_name", name);
          EXTRACT_INT_FIELD_MYSQL(*result, "func_type", def.func_type_, int64_t);
          EXTRACT_VARCHAR_FIELD_MYSQL(*result, "model_name", model);
          EXTRACT_VARCHAR_FIELD_MYSQL(*result, "input_expr", input);
          EXTRACT_VARCHAR_FIELD_MYSQL_SKIP_RET(*result, "type_prompt", prompt);
          EXTRACT_INT_FIELD_MYSQL(*result, "def_version", def.def_version_, int64_t);
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(ob_write_string(alloc, name, def.column_name_))) {
          } else if (OB_FAIL(ob_write_string(alloc, model, def.model_name_))) {
          } else if (OB_FAIL(ob_write_string(alloc, input, def.input_expr_))) {
          } else if (OB_FAIL(ob_write_string(alloc, prompt, def.type_prompt_))) {
          } else if (OB_FAIL(defs.push_back(def))) {
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

int ObAIColumnService::claim_group(const ObAIColumnTaskItem &head, bool &claimed)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc("AIColClaim");
  ObSqlString sql;
  int64_t affected_rows = 0;
  const int64_t now = ObTimeUtility::current_time();
  claimed = false;
  if (OB_FAIL(sql.append_fmt("update oceanbase.__all_ai_column_task set state = %d,"
                             " owner = 'local', lease_expire_ts = %ld where",
                             AI_TASK_RUNNING, now + LEASE_US))) {
  } else if (OB_FAIL(ai_svc_append_group_cond(head, alloc, sql))) {
  } else if (OB_FAIL(sql.append_fmt(" and ((state in (%d, %d) and eligible_ts <= now(6))"
                                    " or (state = %d and lease_expire_ts < %ld))",
                                    AI_TASK_PENDING, AI_TASK_RETRY_WAIT, AI_TASK_RUNNING, now))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
    LOG_WARN("claim task failed", K(ret), K(sql));
  } else {
    claimed = affected_rows > 0;
  }
  return ret;
}

int ObAIColumnService::read_input(ObIAllocator &alloc, const ObString &db_name,
                                  const ObString &table_name, const ObString &input_expr,
                                  const ObString &predicate, bool &row_exists, bool &is_null,
                                  ObString &input)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  row_exists = false;
  is_null = true;
  if (OB_FAIL(sql.append_fmt("select cast((%.*s) as char(65535)) as v from ",
                             input_expr.length(), input_expr.ptr()))) {
  } else if (OB_FAIL(ai_svc_quote_ident(db_name, sql)) || OB_FAIL(sql.append("."))
             || OB_FAIL(ai_svc_quote_ident(table_name, sql))) {
  } else if (OB_FAIL(sql.append_fmt(" where %.*s", predicate.length(), predicate.ptr()))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        LOG_WARN("read AI input failed", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      } else {
        ObString v;
        row_exists = true;
        ret = result->get_varchar("v", v);
        if (OB_ERR_NULL_VALUE == ret) {
          ret = OB_SUCCESS;
        } else if (OB_FAIL(ret)) {
          LOG_WARN("get input failed", K(ret));
        } else if (OB_FAIL(ob_write_string(alloc, v, input))) {
        } else {
          is_null = false;
        }
      }
    }
  }
  return ret;
}

int ObAIColumnService::call_model(ObIAllocator &alloc, const ObAIColumnRuntimeDef &def,
                                  const ObString &input, ObString &output)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *info = NULL;
  share::ObAiModelEndpointInfo endpoint;
  query::ObIAiEndpointResolver *resolver =
      ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
  ObString content = input;
  if (input.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(resolver)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(alloc, def.model_name_, info))) {
    LOG_WARN("get model info failed", K(ret), K(def.model_name_));
  } else if (OB_ISNULL(info)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(resolver->resolve_by_model_name(def.model_name_, alloc, endpoint, false))) {
    LOG_WARN("resolve endpoint failed", K(ret), K(def.model_name_));
  } else if (AI_COLUMN_FUNC_EMBED == def.func_type_) {
    ObAIFuncModel model(alloc, *info, endpoint);
    if (OB_FAIL(model.call_dense_embedding(content, NULL, output))) {
      LOG_WARN("embedding call failed", K(ret));
    }
  } else {
    ObArray<ObString> headers;
    ObJsonObject *body = NULL;
    ObJsonObject *response = NULL;
    ObIJsonBase *result_base = NULL;
    ObAIFuncIComplete *provider = NULL;
    ObAIFuncClient client;
    ObString access_key;
    ObString system_prompt = def.type_prompt_;
    ObString request_model = endpoint.get_request_model_name().empty()
                             ? info->model_ : endpoint.get_request_model_name();
    ObString raw;
    if (OB_FAIL(ObAIFuncUtils::get_complete_provider(alloc, endpoint.get_provider(), provider))) {
    } else if (OB_ISNULL(provider)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint.get_unencrypted_access_key(alloc, access_key))) {
    } else if (OB_FAIL(provider->get_header(alloc, access_key, headers))) {
    } else if (OB_FAIL(provider->get_body(alloc, request_model, system_prompt, content, NULL, body))) {
    } else if (OB_FAIL(client.send_post(alloc, endpoint.get_url(), headers, body, response))) {
      LOG_WARN("completion call failed", K(ret));
    } else if (OB_FAIL(provider->parse_output(alloc, response, result_base))) {
      LOG_WARN("parse completion output failed", K(ret));
    } else if (OB_ISNULL(result_base)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (ObJsonNodeType::J_STRING == result_base->json_type()) {
      output.assign_ptr(result_base->get_data(), static_cast<int32_t>(result_base->get_data_length()));
    } else if (OB_FAIL(ObAIFuncJsonUtils::print_json_to_str(alloc, result_base, raw))) {
    } else if (OB_FAIL(extract_model_text(alloc, raw, output))) {
    }
  }
  return ret;
}

int ObAIColumnService::delete_group(const ObAIColumnTaskItem &head)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc("AIColDel");
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.append("delete from oceanbase.__all_ai_column_task where"))) {
  } else if (OB_FAIL(ai_svc_append_group_cond(head, alloc, sql))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
    LOG_WARN("delete task group failed", K(ret));
  }
  return ret;
}

int ObAIColumnService::delete_table_tasks(const uint64_t table_id)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.append_fmt("delete from oceanbase.__all_ai_column_task where table_id = %lu",
                             table_id))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
  }
  return ret;
}

// lock the task rows of the group; returns false when a newer request replaced
// them (stale result) or they were removed
static int ai_svc_lock_group(ObMySQLTransaction &trans, const ObAIColumnTaskItem &head,
                             const int64_t expect_cnt, ObIAllocator &alloc, bool &still_valid)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  still_valid = false;
  if (OB_FAIL(sql.append("select count(*) as cnt from (select column_id from"
                         " oceanbase.__all_ai_column_task where"))) {
  } else if (OB_FAIL(ai_svc_append_group_cond(head, alloc, sql))) {
  } else if (OB_FAIL(sql.append(" for update) x"))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      ObMySQLResult *result = NULL;
      int64_t cnt = 0;
      if (OB_FAIL(trans.read(res, sql.ptr()))) {
        LOG_WARN("lock task group failed", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result->next())) {
      } else {
        EXTRACT_INT_FIELD_MYSQL(*result, "cnt", cnt, int64_t);
        still_valid = OB_SUCC(ret) && cnt >= expect_cnt && cnt > 0;
      }
    }
  }
  return ret;
}

int ObAIColumnService::publish(const ObString &db_name, const ObString &table_name,
                               const ObAIColumnTaskItem &head,
                               const ObIArray<const ObAIColumnRuntimeDef *> &defs,
                               const ObIArray<ObString> &values,
                               const ObIArray<bool> &value_null,
                               bool &published)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc("AIColPub");
  ObMySQLTransaction trans;
  bool still_valid = false;
  published = false;
  if (defs.count() <= 0 || defs.count() != values.count() || defs.count() != value_null.count()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
    LOG_WARN("start trans failed", K(ret));
  } else {
    ObSqlString sql;
    int64_t affected_rows = 0;
    if (OB_FAIL(ai_svc_lock_group(trans, head, 1, alloc, still_valid))) {
    } else if (!still_valid) {
      // stale: a newer request (or delete) replaced the task
    } else if (OB_FAIL(sql.append("update "))) {
    } else if (OB_FAIL(ai_svc_quote_ident(db_name, sql)) || OB_FAIL(sql.append("."))
               || OB_FAIL(ai_svc_quote_ident(table_name, sql)) || OB_FAIL(sql.append(" set "))) {
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < defs.count(); ++i) {
        const ObAIColumnRuntimeDef *def = defs.at(i);
        ObString state_name;
        ObString spec_name;
        if (OB_ISNULL(def)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(ObAIColumnUtil::build_hidden_column_name(AI_HIDDEN_STATE,
                       def->column_name_, alloc, state_name))) {
        } else if (OB_FAIL(ObAIColumnUtil::build_hidden_column_name(AI_HIDDEN_RESULT_SPEC,
                       def->column_name_, alloc, spec_name))) {
        } else if (OB_FAIL(sql.append(i > 0 ? ", " : ""))) {
        } else if (OB_FAIL(ai_svc_quote_ident(def->column_name_, sql))
                   || OB_FAIL(sql.append(" = "))) {
        } else if (value_null.at(i) ? OB_FAIL(sql.append("NULL"))
                                    : OB_FAIL(ai_svc_append_literal(values.at(i), alloc, sql))) {
        } else if (OB_FAIL(sql.append(", ")) || OB_FAIL(ai_svc_quote_ident(state_name, sql))
                   || OB_FAIL(sql.append_fmt(" = %d, ", AI_ROW_READY))
                   || OB_FAIL(ai_svc_quote_ident(spec_name, sql))
                   || OB_FAIL(sql.append_fmt(" = %ld", def->def_version_))) {
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(sql.append_fmt(" where %.*s", head.predicate_.length(),
                                        head.predicate_.ptr()))) {
      } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
        LOG_WARN("publish AI result failed", K(ret), K(sql));
      } else {
        sql.reuse();
        if (OB_FAIL(sql.append("delete from oceanbase.__all_ai_column_task where"))) {
        } else if (OB_FAIL(ai_svc_append_group_cond(head, alloc, sql))) {
        } else if (OB_FAIL(sql.append(" and column_id in ("))) {
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < defs.count(); ++i) {
          ret = sql.append_fmt("%s%lu", i > 0 ? ", " : "", defs.at(i)->column_id_);
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(sql.append(")"))) {
        } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
          LOG_WARN("delete published tasks failed", K(ret));
        } else {
          published = true;
        }
      }
    }
    int tmp_ret = trans.end(OB_SUCC(ret) && published);
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("end trans failed", K(tmp_ret));
      ret = OB_SUCCESS == ret ? tmp_ret : ret;
      published = false;
    }
  }
  return ret;
}

int ObAIColumnService::fail_task(const ObString &db_name, const ObString &table_name,
                                 const ObAIColumnTaskItem &task, const ObAIColumnRuntimeDef *def)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc("AIColFail");
  ObSqlString cond;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(ai_svc_append_group_cond(task, alloc, cond))) {
  } else if (OB_FAIL(cond.append_fmt(" and column_id = %lu", task.column_id_))) {
  } else if (task.retry_count_ < task.max_retries_) {
    if (OB_FAIL(sql.append_fmt("update oceanbase.__all_ai_column_task set state = %d,"
                               " retry_count = retry_count + 1, owner = NULL, lease_expire_ts = 0,"
                               " eligible_ts = date_add(now(6), interval %ld microsecond) where %s",
                               AI_TASK_RETRY_WAIT, RETRY_BACKOFF_US * (1L << MIN(task.retry_count_, 5)),
                               cond.ptr()))) {
    } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
      LOG_WARN("schedule retry failed", K(ret));
    }
  } else {
    // terminal failure: row becomes FAILED, task is removed
    ObMySQLTransaction trans;
    bool still_valid = false;
    if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
    } else {
      ObString state_name;
      if (OB_FAIL(ai_svc_lock_group(trans, task, 1, alloc, still_valid))) {
      } else if (!still_valid) {
      } else if (OB_NOT_NULL(def)) {
        if (OB_FAIL(ObAIColumnUtil::build_hidden_column_name(AI_HIDDEN_STATE, def->column_name_,
                                                             alloc, state_name))) {
        } else if (OB_FAIL(sql.append("update "))) {
        } else if (OB_FAIL(ai_svc_quote_ident(db_name, sql)) || OB_FAIL(sql.append("."))
                   || OB_FAIL(ai_svc_quote_ident(table_name, sql)) || OB_FAIL(sql.append(" set "))
                   || OB_FAIL(ai_svc_quote_ident(state_name, sql))
                   || OB_FAIL(sql.append_fmt(" = %d where %.*s", AI_ROW_FAILED,
                                             task.predicate_.length(), task.predicate_.ptr()))) {
        } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
          LOG_WARN("mark row failed failed", K(ret), K(sql));
        }
      }
      if (OB_SUCC(ret) && still_valid) {
        sql.reuse();
        if (OB_FAIL(sql.append_fmt("delete from oceanbase.__all_ai_column_task where %s",
                                   cond.ptr()))) {
        } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
        }
      }
      int tmp_ret = trans.end(OB_SUCC(ret) && still_valid);
      if (OB_SUCCESS != tmp_ret) {
        ret = OB_SUCCESS == ret ? tmp_ret : ret;
      }
    }
  }
  return ret;
}

int ObAIColumnService::process_group(const ObIArray<ObAIColumnTaskItem> &group)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc("AIColGroup");
  ObString db_name;
  ObString table_name;
  ObSEArray<ObAIColumnRuntimeDef, 4> all_defs;
  bool table_exists = false;
  bool claimed = false;
  if (group.count() <= 0) {
  } else if (OB_FAIL(load_table(alloc, group.at(0).table_id_, db_name, table_name, all_defs,
                                table_exists))) {
  } else if (!table_exists || all_defs.count() <= 0) {
    // dropped table / dropped AI columns: garbage-collect its tasks
    ret = delete_table_tasks(group.at(0).table_id_);
  } else if (OB_FAIL(claim_group(group.at(0), claimed))) {
  } else if (!claimed) {
  } else {
    const ObAIColumnTaskItem &head = group.at(0);
    ObSEArray<const ObAIColumnRuntimeDef *, 4> ok_defs;
    ObSEArray<ObString, 4> values;
    ObSEArray<bool, 4> value_null;
    bool row_missing = false;
    for (int64_t i = 0; OB_SUCC(ret) && !row_missing && i < group.count(); ++i) {
      const ObAIColumnTaskItem &task = group.at(i);
      const ObAIColumnRuntimeDef *def = NULL;
      for (int64_t d = 0; NULL == def && d < all_defs.count(); ++d) {
        if (all_defs.at(d).column_id_ == task.column_id_) {
          def = &all_defs.at(d);
        }
      }
      if (OB_ISNULL(def)) {
        // AI column dropped meanwhile
        ObSqlString sql;
        int64_t affected_rows = 0;
        if (OB_FAIL(sql.append_fmt("delete from oceanbase.__all_ai_column_task where"
                                   " table_id = %lu and column_id = %lu",
                                   task.table_id_, task.column_id_))) {
        } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
        }
      } else {
        bool row_exists = false;
        bool is_null = true;
        ObString input;
        ObString output;
        int call_ret = OB_SUCCESS;
        if (OB_FAIL(read_input(alloc, db_name, table_name, def->input_expr_, head.predicate_,
                               row_exists, is_null, input))) {
          call_ret = ret;
          ret = OB_SUCCESS;
        } else if (!row_exists) {
          row_missing = true;
        } else if (is_null || input.empty()) {
          // NULL input -> NULL result, column is READY
          if (OB_FAIL(ok_defs.push_back(def)) || OB_FAIL(values.push_back(ObString()))
              || OB_FAIL(value_null.push_back(true))) {
          }
        } else if (OB_SUCCESS != (call_ret = call_model(alloc, *def, input, output))) {
          LOG_WARN("AI model call failed", K(call_ret), K(task));
        } else if (OB_FAIL(ok_defs.push_back(def)) || OB_FAIL(values.push_back(output))
                   || OB_FAIL(value_null.push_back(false))) {
        }
        if (OB_SUCC(ret) && OB_SUCCESS != call_ret && !row_missing) {
          if (OB_FAIL(fail_task(db_name, table_name, task, def))) {
            LOG_WARN("record task failure failed", K(ret), K(task));
          }
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (row_missing) {
      ret = delete_group(head);
    } else if (ok_defs.count() > 0) {
      bool published = false;
      if (OB_FAIL(publish(db_name, table_name, head, ok_defs, values, value_null, published))) {
        LOG_WARN("publish failed", K(ret), K(head));
        // result does not fit the column type etc.: count as a model failure
        int tmp_ret = OB_SUCCESS;
        for (int64_t i = 0; i < group.count(); ++i) {
          for (int64_t d = 0; d < ok_defs.count(); ++d) {
            if (ok_defs.at(d)->column_id_ == group.at(i).column_id_
                && OB_SUCCESS != (tmp_ret = fail_task(db_name, table_name, group.at(i), ok_defs.at(d)))) {
              LOG_WARN("record task failure failed", K(tmp_ret));
            }
          }
        }
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
