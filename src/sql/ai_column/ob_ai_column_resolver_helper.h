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

#ifndef OCEANBASE_SQL_AI_COLUMN_OB_AI_COLUMN_RESOLVER_HELPER_H_
#define OCEANBASE_SQL_AI_COLUMN_OB_AI_COLUMN_RESOLVER_HELPER_H_

#include "sql/ai_column/ob_ai_column_define.h"
#include "sql/parser/parse_node.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
class ObTableSchema;
class ObColumnSchemaV2;
class ObSchemaGetterGuard;
}
}
namespace sql
{
class ObSQLSessionInfo;

struct ObAIColumnOption
{
  ObAIColumnOption() : name_(), str_value_(), int_value_(0), is_int_(false), is_null_(false) {}
  TO_STRING_KV(K_(name), K_(str_value), K_(int_value), K_(is_int), K_(is_null));
  common::ObString name_;
  common::ObString str_value_;
  int64_t int_value_;
  bool is_int_;
  bool is_null_;
};

// How the resolver sees the columns an AI column may reference.
class ObAIColumnLookup
{
public:
  virtual ~ObAIColumnLookup() {}
  // exists=false when the column is unknown.
  virtual int find_column(const common::ObString &name, bool &exists, uint64_t &column_id,
                          bool &is_ai_column, bool &is_hidden) const = 0;
};

// Lookup over the table being created/altered plus (for ALTER) the existing table.
class ObAIColumnSchemaLookup : public ObAIColumnLookup
{
public:
  ObAIColumnSchemaLookup(const share::schema::ObTableSchema &new_schema,
                         const share::schema::ObTableSchema *base_schema,
                         const common::ObIArray<common::ObString> &pending_ai_names)
    : new_schema_(new_schema), base_schema_(base_schema), pending_ai_names_(pending_ai_names) {}
  virtual int find_column(const common::ObString &name, bool &exists, uint64_t &column_id,
                          bool &is_ai_column, bool &is_hidden) const override;
private:
  const share::schema::ObTableSchema &new_schema_;
  const share::schema::ObTableSchema *base_schema_;
  const common::ObIArray<common::ObString> &pending_ai_names_;
};

class ObAIColumnResolveHelper
{
public:
  // Name lookup that also works for AlterTableSchema, whose columns are not in the name hash.
  static const share::schema::ObColumnSchemaV2 *find_column_by_name(
      const share::schema::ObTableSchema &schema, const common::ObString &name);
  // Pure validation core (unit tested):
  //   expr_text      text of the AI function call
  //   dep_names      column names referenced by the input expression
  //   result_type    declared type; vector_dim the declared VECTOR(n) dim (or 0)
  static int build_definition(common::ObIAllocator &alloc,
                              const common::ObString &column_name,
                              const common::ObObjType result_type,
                              const int64_t column_vector_dim,
                              const common::ObString &expr_text,
                              const common::ObIArray<common::ObString> &dep_names,
                              const common::ObIArray<ObAIColumnOption> &options,
                              const ObAIColumnLookup &lookup,
                              ObAIColumnDef &def);
  static int collect_column_refs(const ParseNode *node, common::ObIArray<common::ObString> &names);
  static int resolve_options(const ParseNode *option_list, common::ObIArray<ObAIColumnOption> &options);
  // Resolve one "AI COLUMN (...) WITH (...)" attribute node.
  static int resolve_attr(common::ObIAllocator &alloc,
                          const share::schema::ObColumnSchemaV2 &column,
                          const ParseNode &attr_node,
                          const ObAIColumnLookup &lookup,
                          share::schema::ObSchemaGetterGuard *schema_guard,
                          ObAIColumnDef &def);
  // Build the three hidden columns (ids left invalid unless %next_column_id given).
  static int build_hidden_columns(common::ObIAllocator &alloc,
                                  const ObAIColumnDef &def,
                                  const share::schema::ObColumnSchemaV2 &result_column,
                                  common::ObIArray<share::schema::ObColumnSchemaV2> &hidden_columns);
  static int check_model(common::ObIAllocator &alloc,
                         share::schema::ObSchemaGetterGuard &schema_guard,
                         const ObAIColumnDef &def);
};

} // end namespace sql
} // end namespace oceanbase

#endif
