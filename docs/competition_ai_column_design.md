# AI COLUMN (赛题三) implementation guide

This document describes how to finish the asynchronous **AI COLUMN** feature on top of the
AI function runtime that already exists in `seekdb`. Every step is anchored to real symbols
in the tree so it can be applied directly.

## 0. Current state

| Area | State |
|---|---|
| `AI_PROMPT` / `AI_COMPLETE` / `AI_EMBED` / `AI_RERANK` runtime | implemented (branch `feat/ai-enhance-submit`) |
| `read_csv` / `read_json` / `read_parquet` / `glob` / `COPY TO` | implemented (branch `feat/competition-submit`) |
| `AI COLUMN` grammar | parses `col <type> AI COLUMN (expr) WITH (...)`, but the branch did not compile |
| `__all_ai_column` / `__all_ai_column_task` | **added** in `src/share/inner_table/ob_inner_table_schema_def.py` |
| `ai_column_strict_read` session variable | declared; sys-var metadata regenerated |
| DML linkage, worker, retry, strict read, vector sync | **not implemented** |

Two build blockers were fixed while preparing this work:

* `src/sql/parser/sql_parser_mysql_mode.y` — the `AI COLUMN` rule passed a bare `NULL`
  where a `ParseNode *` is expected (`set_data_type_collation`), and the `WITH (...)` option
  list was parsed but dropped. The rule now keeps the option list in child slot 4 and uses a
  typed null for the collation attribute.
* `src/share/system_variable/ob_sys_var_meta.h` — `OB_SYS_VARS_COUNT` was not regenerated
  after `ai_column_strict_read` was added to `ob_system_variable_init.json`, so the three
  sorted sys-var arrays overflowed. Re-run
  `cd src/share/system_variable && python3 gen_ob_sys_variables.py`.

## 1. Data model

### 1.1 Inner tables (already added)

`__all_ai_column` (table_id `1131`) is keyed by `(table_id, column_id)` and stores
`column_name`, the three hidden column ids, `max_retries`, `sync_mode`, `model_name`,
`result_type`, `vector_dimension`, `dependency_columns` and `normalized_expr`.

`__all_ai_column_task` (table_id `1132`) holds one row per **(row, AI column)** pair:
`table_id`, `column_id`, `request_id`, `state`, `retry_count`, `rowkey_predicate` and
`eligible_ts`. Rows are deleted once they reach a terminal state, so the table only ever
contains pending work.

Table ids must stay in ascending order in that file — the generator
(`generate_inner_table_schema.py`, `check_split_file`) aborts otherwise.

Generated constants available after a build: `OB_ALL_AI_COLUMN_TID`,
`OB_ALL_AI_COLUMN_TNAME`, `OB_ALL_AI_COLUMN_TASK_TID`, `OB_ALL_AI_COLUMN_TASK_TNAME`.

### 1.2 Hidden columns

Every AI column owns three hidden columns appended after the user columns:

| Hidden column | Type | Content |
|---|---|---|
| state | `INT` | `0` PENDING, `1` READY, `2` FAILED |
| request | `BIGINT` | request id of the change that produced the current state |
| result spec | `VARCHAR(2048)` | normalized expression plus the result type hint |

`state` and `request` are **row level**; the table level metadata lives in
`__all_ai_column`.

### 1.3 Result type hints

The mock service keys its answers off the system prompt, so the request must carry a type
hint derived from the target column:

* `JSON` -> `Return exactly one valid JSON value.`
* integer types -> `Answer with a single base-10 integer.`
* other types -> no hint

The hint is stored in `normalized_expr` so that `__all_ai_column.normalized_expr` contains
the literal text, and is sent as the **system** message while the evaluated prompt is sent
as the **user** message.

## 2. DDL layer

### 2.1 Grammar

`src/sql/parser/sql_parser_mysql_mode.y` — `column_definition` now has one AI rule with
this shape:

    column_definition_ref data_type AI COLUMN ( expr ) opt_ai_column_options
        opt_generated_column_attribute_list opt_column_reference opt_position_column

Child layout of the resulting `T_COLUMN_DEFINITION` (`AI_COLUMN_DEFINITION_NUM_CHILD == 8`):

| slot | meaning |
|---|---|
| 0 | column ref |
| 1 | data type |
| 2 | attributes |
| 3 | AI expression |
| 4 | storage type (`T_STORED_COLUMN`) |
| 5 | `WITH (...)` option list (`T_EXPR_LIST`, may be `NULL`) |
| 6 | position |
| 7 | generated key name |

The 8-child layout keeps the current behaviour of an AI column (it is materialised like a
stored generated column) **and** preserves the `WITH (...)` options, which the previous
grammar threw away. The resolver was extended accordingly:

* `src/sql/resolver/ddl/ob_ddl_resolver.h` — `AI_COLUMN_DEFINITION_NUM_CHILD = 8` next to
  `GEN_COLUMN_DEFINITION_NUM_CHILD = 7`.
* `src/sql/resolver/ddl/ob_ddl_resolver.cpp` — the generated-column branch accepts both
  child counts and the position is read from slot 6 for an AI column.
* `src/sql/resolver/ddl/ob_alter_table_resolver.cpp` — same position fix for
  `ALTER TABLE ... ADD COLUMN`.

Making the AI column asynchronous means the result column must become a **normal** column
that the worker writes; that is the point where the resolver stops treating it as a
generated column and starts filling `__all_ai_column` instead.

### 2.2 Resolution

`src/sql/resolver/ddl/ob_ddl_resolver.cpp` — `ObDDLResolver::resolve_column_definition()`
needs a new branch for the 6-child layout:

    if (AI_COLUMN_DEFINITION_NUM_CHILD == node->num_child_) {
      // node->children_[3]  : AI expression
      // node->children_[4]  : WITH options
    }

Steps inside that branch:

1. The AI expression must be a call to `AI_COMPLETE` or `AI_EMBED`. Resolve it with
   `ObResolverUtils::resolve_generated_column_expr()` — the generated-column resolver is
   the only code path that already resolves an expression against the *other* columns of
   the same table and reports its dependencies.
2. The first argument is the model name and it must exist.
   `ObAIFuncUtils::get_ai_func_info()`
   (`src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h`) resolves it from the schema cache
   and reports the endpoint type.
3. For `AI_EMBED` on a `VECTOR(n)` column the requested dimension must equal `n`,
   otherwise raise the error the DDL test maps to `7600`.
4. The expression must not reference another AI column of the same table — error `1235`.
5. Resolve the `WITH (...)` options: `sync_mode` (only `async` is supported) and
   `max_retries` (non-negative integer).
6. Compute the dependency column set from the resolved expression.
7. Create the three hidden columns and append them to the table schema.
8. Record an `ObAiColumnDef` into the DDL argument.

### 2.3 Persisting the metadata

`__all_ai_column` rows must be written in the same transaction as the DDL. The pattern to
copy is `ObAiModelSqlService` (`src/share/schema/ob_ai_model_sql_service.cpp`): build the
statement with `ObDMLSqlSplicer` (`src/share/ob_dml_sql_splicer.h`) and execute it through
the `ObDDLSQLTransaction` owned by the rootserver DDL service.

The AI column definition has to travel from the resolver to the rootserver inside
`obcall::ObCreateTableArg` / `obcall::ObAlterTableArg` (`src/share/ob_rpc_struct.h`). Both
use `OB_SERIALIZE_MEMBER` plus a hand written `assign()`, so a new
`ObSArray<ObAiColumnDef>` member has to be added to both.

## 3. DML linkage and transaction consistency

### 3.1 Required behaviour

| Event | AI column | task |
|---|---|---|
| INSERT | result -> NULL, state -> PENDING, request -> new id | one row per AI column |
| UPDATE of a dependency column | same as INSERT | one row per AI column |
| UPDATE of an unrelated column | untouched | none |
| UPDATE of the primary key | old row removed, new row invalidated | task for the new key |
| DELETE | row removed | pending tasks removed |
| ROLLBACK | everything undone | task rows vanish with the transaction |

The dependency decision is a **resolver time** decision: the set of columns in the `SET`
clause is known while the statement is resolved and the dependency set is stored in
`__all_ai_column.dependency_columns`.

### 3.2 Where to hook

* Row image override: `TriggerHandle::check_and_update_new_row()` and
  `TriggerHandle::set_rowid_into_row()` (`src/sql/engine/dml/ob_trigger_handler.cpp`) show
  exactly how a component rewrites column values of the row being written. That is the
  sanctioned mechanism for forcing the AI result column to NULL and the state/request
  hidden columns to their new values.
* Task row creation: the foreign-key cascade already runs a nested statement inside the
  user transaction from the DML operator. `ObTableModifyOp::begin_nested_session()` /
  `end_nested_session()` (`src/sql/engine/dml/ob_table_modify_op.cpp`) plus
  `ObTableModifyOp::execute_write()` provide exactly what is needed:

      // inside the insert/update row processing
      if (OB_ISNULL(dml_op.get_sql_proxy())) {
        // not a SQL session (e.g. internal DML) - nothing to do
      } else {
        ObSqlString task_sql;
        // INSERT INTO oceanbase.__all_ai_column_task (...) VALUES (...)
        if (OB_FAIL(dml_op.execute_write(task_sql.ptr()))) { ... }
        // do NOT commit: the outer DML commits or rolls back everything
      }

* Rowkey predicate text: the expected format is a backquoted identifier, an equals sign and
  the literal. Use `ObSQLUtils::generate_new_name_with_escape_character()`
  (`src/sql/ob_sql_utils.h:514`) for the identifier and `ObDMLSqlSplicer::get_values()`
  for the literal, then format the two pieces yourself.
* Primary key columns: `ObTableSchema::get_rowkey_info()` plus
  `ObRowkeyInfo::is_rowkey_column()` (`src/oblib/common/rowkey/ob_rowkey_info.h`).
  `ObTableSchema::get_primary_key_columns()` does **not** exist in this tree.

## 4. Async worker

* Registration: `src/observer/omt/ob_ai_service.{h,cpp}` is the template for an observer
  managed task (`static int server_module_init(...)` plus
  `share::ObISharedTimer::schedule(task, delay, repeat, immediate)`).
* Transaction and internal SQL: `ObVectorRefreshTransaction`
  (`src/observer/vector_index/ob_vector_refresh_transaction.cpp`) is the ready-made helper
  that runs `SELECT` and `UPDATE` against tenant tables inside one transaction using
  `ObInnerSQLConnection`. `src/sql/engine/cmd/ob_vector_index_refresh.cpp` shows a full
  read-then-write cycle.
* Model call: `ObAIFuncUtils::get_complete_body()` and
  `ObAIFuncUtils::parse_complete_output()`
  (`src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h`) build and parse the request.
  `ObAIFuncModel::call_completion()` always sends the evaluated prompt as the **user**
  message and leaves the system message empty, so the worker must call
  `get_complete_body()` directly to inject the type hint as the system message.
* Group publish: all AI columns of one row share a `request_id`. The worker collects every
  task with that id and publishes the results and the states in a single transaction.
* Late result protection: before writing, compare the task `request_id` with the value in
  the row's request hidden column. A mismatch means a newer change already happened, so the
  late result is dropped.
* Retry: on failure increment `retry_count`; while `retry_count <= max_retries` move the
  task to state `3` (retry scheduled) and set `eligible_ts` to the next attempt time,
  otherwise mark the column FAILED and delete the task.

## 5. Strict read and retry

* `ai_column_strict_read` is a `GLOBAL | SESSION | NEED_SERIALIZE` boolean, default off.
  When it is on, a statement that references an AI column must fail with error `11120` if
  **any** row of that column is not READY. The check is **column level**: it does not
  depend on which rows the query happens to select.
* The `status` pseudo column exists only inside a retry statement. A backquoted `status`
  addresses the physical business column instead.

    ALTER TABLE t RETRY AI COLUMN result WHERE status = 'FAILED' AND category = 'news';

This is a new ALTER TABLE action: add a parse node type, a member on
`obcall::ObAlterTableArg`, a resolver branch and a rootserver executor. The retry
statement resolves the predicate and, for every matching row, invalidates the AI column
and creates a task exactly like a dependency update does.

## 6. Vector column and index

* The AI vector column must be a **normal** column, because the worker writes it.
* Write the generated vector into the same column the vector index is built on, so that
  the ordinary local index maintenance (`src/sql/optimizer/ob_del_upd_log_plan.cpp`)
  removes the stale entry from the `delta_buffer` table and the ANN scan
  (`src/sql/das/iter/ob_das_iter_utils.cpp`) picks up the new vector.
* `ObVectorIndexUtil::check_column_has_vector_index()`
  (`src/query/api/query/vector/ob_vector_index_util.h`) detects the dependency.
* `src/storage/ddl/ob_hnsw_embedmgr.h` (`ObEmbeddingTaskMgr`) is the batching template if
  embeddings are generated in batches.

## 7. Building and testing

    cd /root/seekdb/competition
    cmake -S . -B build_release -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build_release -j 10 --target seekdb

The mysqltest runner is a Bazel target and is not produced by the CMake build; the suite
SQL can also be replayed with `obclient` against a locally started server.

The AI suites need their mock services:

* `tools/deploy/mysql_test/test_suite/ai_column/include/ai_column_mock.py` (ports
  19120-19124) ships with the repository.
* `tools/deploy/mysql_test/test_suite/ai_function/include/ai_function_mock.py` serves the
  OpenAI and Ollama compatible endpoints on port 8000 that `ai_function_enhancement.test`
  expects.
