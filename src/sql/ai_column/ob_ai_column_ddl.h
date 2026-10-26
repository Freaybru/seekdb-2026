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

// AI COLUMN DDL side effects that live outside the schema:
//   * __all_ai_column metadata rows written after CREATE / ALTER ADD
//   * ALTER TABLE ... RETRY AI COLUMN c [WHERE ...]
//   * shared SQL builders for __all_ai_column_task rows

#ifndef OCEANBASE_SQL_AI_COLUMN_OB_AI_COLUMN_DDL_H_
#define OCEANBASE_SQL_AI_COLUMN_OB_AI_COLUMN_DDL_H_

#include "sql/ai_column/ob_ai_column_define.h"
#include "lib/string/ob_sql_string.h"
#include "common/object/ob_object.h"

namespace oceanbase
{
namespace common
{
class ObISQLClient;
class ObTimeZoneInfo;
}
namespace sql
{

class ObAIColumnDDLUtil
{
public:
  static const int64_t REQUEST_ID_BUF_LEN = 40;
  // Write metadata rows for the AI columns defined by a CREATE TABLE / ALTER ADD.
  // When %backfill is set (ALTER ADD on a table that may hold rows) every row
  // gets a task.
  static int persist_definitions(const common::ObString &db_name,
                                 const common::ObString &table_name,
                                 const common::ObIArray<ObAIColumnDef> &defs,
                                 const bool backfill,
                                 const common::ObTimeZoneInfo *tz_info);
  static int retry_column(const common::ObString &db_name,
                          const common::ObString &table_name,
                          const common::ObString &column_name,
                          const common::ObString &where_text,
                          const common::ObTimeZoneInfo *tz_info);
  // remove metadata/tasks of a dropped table (best effort)
  static int cleanup_table(const uint64_t table_id);

  // fresh request id (hex, NUL terminated) unique within this process
  static int gen_request_id(char *buf, const int64_t buf_len);
  static int append_task_delete_sql(const uint64_t table_id,
                                    const common::ObIArray<uint64_t> &column_ids,
                                    const common::ObString &escaped_predicate,
                                    common::ObSqlString &sql);
  static int append_task_insert_sql(const uint64_t table_id,
                                    const common::ObIArray<uint64_t> &column_ids,
                                    const common::ObString &escaped_predicate,
                                    const common::ObString &request_id,
                                    common::ObSqlString &sql);
  // `c1` = v1 and `c2` = v2
  static int print_rowkey_predicate(const common::ObIArray<common::ObString> &names,
                                    const common::ObIArray<common::ObObj> &values,
                                    const common::ObTimeZoneInfo *tz_info,
                                    common::ObIAllocator &alloc,
                                    common::ObString &predicate);
  static int get_table_id(common::ObISQLClient &client,
                          const common::ObString &db_name,
                          const common::ObString &table_name,
                          uint64_t &table_id);
  static int get_rowkey_column_names(common::ObISQLClient &client,
                                     const uint64_t table_id,
                                     common::ObIAllocator &alloc,
                                     common::ObIArray<common::ObString> &names);
  // Load the column ids part of every AI column definition of %table_id
  // (column, hidden columns, dependencies) from committed metadata.
  static int load_column_links(const uint64_t table_id,
                               common::ObIArray<ObAIColumnDef> &defs);
  // ai_column_strict_read: fail with OB_AI_COLUMN_NOT_READY when any row of the
  // AI column is not READY yet (pending, running, retrying or failed).
  static int check_column_ready(const common::ObString &db_name,
                                const common::ObString &table_name,
                                const uint64_t table_id,
                                const uint64_t column_id,
                                const common::ObString &column_name);
  // invalidate selected rows of one AI column and create their tasks
  static int invalidate_rows(common::ObISQLClient &client,
                             const common::ObString &db_name,
                             const common::ObString &table_name,
                             const uint64_t table_id,
                             const uint64_t column_id,
                             const common::ObString &column_name,
                             const common::ObString &where_text,
                             const common::ObTimeZoneInfo *tz_info,
                             common::ObIAllocator &alloc,
                             int64_t &row_count);
};

} // end namespace sql
} // end namespace oceanbase

#endif
