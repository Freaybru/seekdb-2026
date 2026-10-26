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

// Resolver side of "file as database":
//  * read_csv / read_json / read_ndjson / read_parquet / glob table functions are turned
//    into values-table items (ObValuesTableDef::ACCESS_FILE / ACCESS_OBJ);
//  * DESCRIBE <select on files> is rewritten into a constant select;
//  * COPY (query) | table TO 'file' (...) is resolved as a select with a select-into item.

#ifndef OCEANBASE_SQL_RESOLVER_DML_OB_FILE_TABLE_RESOLVER_H_
#define OCEANBASE_SQL_RESOLVER_DML_OB_FILE_TABLE_RESOLVER_H_

#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"
#include "sql/parser/parse_node.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{
class ObSQLSessionInfo;
class ObStmt;
struct ObResolverParams;
class ObRawExprResType;

class ObFileSqlResolverUtil
{
public:
  static const int64_t MAX_GLOB_FILES = 10000;
  // OB_NOT_SUPPORTED unless session variable enable_file_sql is ON
  static int check_enabled(const ObSQLSessionInfo *session, const char *what);
  // normalize a user path with realpath(3) and enforce secure_file_priv
  // (skipped for inner sessions); must_be_file additionally requires a regular file
  static int check_file_path(const ObSQLSessionInfo &session, const common::ObString &func,
                             const common::ObString &raw_path, bool must_be_file,
                             common::ObIAllocator &allocator, common::ObString &real_path);
  // secure check only, for already normalized paths; returns false when denied
  static int is_path_allowed(const ObSQLSessionInfo &session, const char *real_path, bool &allowed);
  static int get_format_by_func(const common::ObString &func, int8_t &format, int8_t &json_layout);
  static int parse_scan_options(const common::ObString &func, const ParseNode *opt_list,
                                common::ObIAllocator &allocator, ObFileScanOptions &opts);
  static int parse_copy_options(const ParseNode *opt_list, common::ObIAllocator &allocator,
                                ObFileCopyOptions &opts);
  static bool contains_file_table(const ParseNode *node);
  static int file_column_res_type(const ObFileColumnDesc &col, common::ObCollationType cs_type,
                                  ObRawExprResType &res_type);
  // DESCRIBE select ... from read_xxx(...): resolve, then build a constant result select
  static int resolve_describe(ObResolverParams &params, const ParseNode &select_node, ObStmt *&stmt);
  // COPY (query) | table TO 'path' (options)
  static int resolve_copy(ObResolverParams &params, const ParseNode &copy_node, ObStmt *&stmt);
private:
  static int get_option_name(const ParseNode *opt, common::ObString &name);
  static int get_bool_value(const ParseNode *v, bool &b);
  static int get_string_value(const ParseNode *v, common::ObString &s);
  static int get_int_value(const ParseNode *v, int64_t &i);
  static int get_char_value(const ParseNode *v, bool allow_empty, int8_t &c);
};

} // namespace sql
} // namespace oceanbase
#endif
