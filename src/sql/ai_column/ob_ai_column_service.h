/**
 * Copyright (c) 2026 OceanBase.
 *
 * AI COLUMN asynchronous worker.
 *
 * Tasks live in oceanbase.__all_ai_column_task and are created by the DML
 * operator (same transaction as the user change) or by DDL / RETRY.  The
 * worker claims runnable tasks, evaluates the AI input of the row, calls the
 * model endpoint outside of any transaction and publishes the result with a
 * request-id guard so a late (stale) answer can never overwrite newer data.
 * All AI columns of one row created by the same request are published in one
 * transaction (grouped visibility).
 */

#ifndef OCEANBASE_SQL_AI_COLUMN_OB_AI_COLUMN_SERVICE_H_
#define OCEANBASE_SQL_AI_COLUMN_OB_AI_COLUMN_SERVICE_H_

#include "share/ob_thread_pool.h"
#include "lib/string/ob_string.h"
#include "lib/container/ob_se_array.h"
#include "lib/allocator/page_arena.h"

namespace oceanbase
{
namespace sql
{

struct ObAIColumnTaskItem
{
  ObAIColumnTaskItem() : table_id_(0), column_id_(0), predicate_(), request_id_(),
                         def_version_(0), retry_count_(0), max_retries_(0) {}
  TO_STRING_KV(K_(table_id), K_(column_id), K_(predicate), K_(request_id),
               K_(def_version), K_(retry_count), K_(max_retries));
  uint64_t table_id_;
  uint64_t column_id_;
  common::ObString predicate_;   // unescaped rowkey predicate
  common::ObString request_id_;
  int64_t def_version_;
  int64_t retry_count_;
  int64_t max_retries_;
};

struct ObAIColumnRuntimeDef
{
  ObAIColumnRuntimeDef() : column_id_(0), func_type_(0), def_version_(0) {}
  TO_STRING_KV(K_(column_id), K_(column_name), K_(func_type), K_(model_name),
               K_(input_expr), K_(type_prompt), K_(def_version));
  uint64_t column_id_;
  common::ObString column_name_;
  int64_t func_type_;
  common::ObString model_name_;
  common::ObString input_expr_;
  common::ObString type_prompt_;
  int64_t def_version_;
};

class ObAIColumnService : public share::ObThreadPool
{
public:
  static const int64_t IDLE_INTERVAL_US = 100 * 1000;
  static const int64_t LEASE_US = 120L * 1000 * 1000;
  static const int64_t RETRY_BACKOFF_US = 1000L * 1000;
  static const int64_t BATCH_TASKS = 64;

  static ObAIColumnService &get_instance();
  int init();
  int start() override;
  void stop() override;
  void wait() override;
  void destroy();
  void run1() override;

  // process one round of runnable tasks; returns number of groups processed
  int run_once(int64_t &processed);

  // helpers (public for unit tests)
  static int extract_model_text(common::ObIAllocator &alloc,
                                const common::ObString &raw,
                                common::ObString &text);
private:
  ObAIColumnService() : is_inited_(false) {}
  int fetch_tasks(common::ObIAllocator &alloc, common::ObIArray<ObAIColumnTaskItem> &tasks);
  int process_group(const common::ObIArray<ObAIColumnTaskItem> &group);
  int load_table(common::ObIAllocator &alloc, const uint64_t table_id,
                 common::ObString &db_name, common::ObString &table_name,
                 common::ObIArray<ObAIColumnRuntimeDef> &defs, bool &table_exists);
  int claim_group(const ObAIColumnTaskItem &head, bool &claimed);
  int read_input(common::ObIAllocator &alloc, const common::ObString &db_name,
                 const common::ObString &table_name, const common::ObString &input_expr,
                 const common::ObString &predicate, bool &row_exists, bool &is_null,
                 common::ObString &input);
  int call_model(common::ObIAllocator &alloc, const ObAIColumnRuntimeDef &def,
                 const common::ObString &input, common::ObString &output);
  int publish(const common::ObString &db_name, const common::ObString &table_name,
              const ObAIColumnTaskItem &head,
              const common::ObIArray<const ObAIColumnRuntimeDef *> &defs,
              const common::ObIArray<common::ObString> &values,
              const common::ObIArray<bool> &value_null,
              bool &published);
  int fail_task(const common::ObString &db_name, const common::ObString &table_name,
                const ObAIColumnTaskItem &task, const ObAIColumnRuntimeDef *def);
  int delete_group(const ObAIColumnTaskItem &head);
  int delete_table_tasks(const uint64_t table_id);
private:
  bool is_inited_;
  DISALLOW_COPY_AND_ASSIGN(ObAIColumnService);
};

} // namespace sql
} // namespace oceanbase

#endif
