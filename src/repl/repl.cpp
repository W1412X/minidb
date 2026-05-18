/**
 * @file repl.cpp
 * @brief Interactive SQL REPL — Single database
 */
#include "repl/repl.h"
#include "sql/parser/parser.h"
#include "sql/planner/planner.h"
#include "sql/executor/executor_factory.h"
#include "catalog/catalog.h"
#include <cstdio>
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <unordered_map>

namespace minidb {

static u64 g_repl_statement_timeout_ms = 0;
static std::unordered_map<std::string, String> g_repl_prepared_sql;

static String trim_sql(const String& sql) {
    u32 s = 0;
    u32 e = sql.size();
    while (s < e && (sql[s] == ' ' || sql[s] == '\t' || sql[s] == '\n' || sql[s] == '\r')) s++;
    while (e > s && (sql[e - 1] == ' ' || sql[e - 1] == '\t' ||
                     sql[e - 1] == '\n' || sql[e - 1] == '\r' || sql[e - 1] == ';')) e--;
    return sql.substr(s, e - s);
}

static String upper_sql(const String& sql) {
    String out;
    for (u32 i = 0; i < sql.size(); i++) {
        char c = sql[i];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        out += c;
    }
    return out;
}

static bool parse_statement_timeout(const String& sql, u64* out_ms) {
    String t = trim_sql(sql);
    String u = upper_sql(t);
    const char* prefix = "SET STATEMENT_TIMEOUT";
    u32 n = static_cast<u32>(std::strlen(prefix));
    if (u.size() < n || std::strncmp(u.c_str(), prefix, n) != 0) return false;
    const char* p = t.c_str() + n;
    while (*p == ' ' || *p == '\t' || *p == '=') p++;
    char* end = nullptr;
    unsigned long long v = std::strtoull(p, &end, 10);
    if (end == p) return false;
    if (out_ms) *out_ms = static_cast<u64>(v);
    return true;
}

static bool parse_prepare(const String& sql, String* name, String* body) {
    String t = trim_sql(sql);
    String u = upper_sql(t);
    const char* prefix = "PREPARE ";
    u32 n = static_cast<u32>(std::strlen(prefix));
    if (u.size() <= n || std::strncmp(u.c_str(), prefix, n) != 0) return false;
    u32 pos = n;
    while (pos < t.size() && (t[pos] == ' ' || t[pos] == '\t')) pos++;
    u32 name_start = pos;
    while (pos < t.size() && t[pos] != ' ' && t[pos] != '\t') pos++;
    if (pos == name_start) return false;
    String stmt_name = t.substr(name_start, pos - name_start);
    while (pos < t.size() && (t[pos] == ' ' || t[pos] == '\t')) pos++;
    String rest_upper = upper_sql(t.substr(pos));
    if (rest_upper.size() < 3 || std::strncmp(rest_upper.c_str(), "AS ", 3) != 0) return false;
    String stmt_body = trim_sql(t.substr(pos + 3));
    if (stmt_body.empty()) return false;
    if (name) *name = stmt_name;
    if (body) *body = stmt_body;
    return true;
}

static bool parse_execute_or_deallocate(const String& sql, const char* keyword, String* name) {
    String t = trim_sql(sql);
    String u = upper_sql(t);
    u32 n = static_cast<u32>(std::strlen(keyword));
    if (u.size() <= n || std::strncmp(u.c_str(), keyword, n) != 0) return false;
    u32 pos = n;
    while (pos < t.size() && (t[pos] == ' ' || t[pos] == '\t')) pos++;
    u32 start = pos;
    while (pos < t.size() && t[pos] != ' ' && t[pos] != '\t' && t[pos] != '(') pos++;
    if (pos == start) return false;
    if (name) *name = t.substr(start, pos - start);
    return true;
}

static OptimizerConfig optimizer_config_from_db(const Database& db) {
    OptimizerConfig cfg;
    cfg.enable_hashjoin = db.config().enable_hashjoin;
    cfg.enable_indexscan = db.config().enable_indexscan;
    cfg.enable_indexonlyscan = db.config().enable_indexonlyscan;
    cfg.remote_storage = db.config().storage_mode == "remote";
    return cfg;
}

static bool is_write_statement_type(StmtType type) {
    return type == StmtType::kInsert || type == StmtType::kDelete ||
           type == StmtType::kUpdate || type == StmtType::kCreateTable ||
           type == StmtType::kDropTable || type == StmtType::kCreateIndex ||
           type == StmtType::kDropIndex || type == StmtType::kAlterTable ||
           type == StmtType::kAnalyze ||
           type == StmtType::kBegin || type == StmtType::kCommit ||
           type == StmtType::kRollback;
}

REPL::REPL(Database& db) : db_(db) {}

void REPL::run() {
    printf("MiniADB v0.3.0 — Interactive SQL Shell\n");
    printf("Type 'exit' to quit, 'help' for commands.\n\n");

    char line[4096];
    while (true) {
        printf("minidb> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;

        u32 len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }

        if (len == 0) continue;
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (p[0] == '-' && p[1] == '-') continue;
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) break;

        if (strcmp(line, "help") == 0) {
            printf("Commands:\n");
            printf("  SQL statement ending with ;\n");
            printf("  SHOW TABLES          — list all tables\n");
            printf("  DESC <table>         — describe table schema\n");
            printf("  EXPLAIN <statement>  — show query plan\n");
            printf("  BEGIN / COMMIT / ROLLBACK — transactions\n");
            printf("  exit / quit          — exit shell\n\n");
            continue;
        }

        // W25: Split multiple statements on one line by semicolons
        bool in_sq = false, in_dq = false;
        String line_str(line);
        u32 stmt_start = 0;
        for (u32 i = 0; i < line_str.size(); i++) {
            if (line_str[i] == '\'' && !in_dq) in_sq = !in_sq;
            else if (line_str[i] == '"' && !in_sq) in_dq = !in_dq;
            else if (line_str[i] == ';' && !in_sq && !in_dq) {
                String stmt = line_str.substr(stmt_start, i - stmt_start);
                execute_sql(stmt);
                stmt_start = i + 1;
                while (stmt_start < line_str.size() &&
                       (line_str[stmt_start] == ' ' || line_str[stmt_start] == '\t'))
                    stmt_start++;
            }
        }
        if (stmt_start < line_str.size()) {
            String stmt = line_str.substr(stmt_start);
            u32 s = 0;
            while (s < stmt.size() && (stmt[s] == ' ' || stmt[s] == '\t')) s++;
            if (s > 0) stmt = stmt.substr(s);
            u32 e = stmt.size();
            while (e > 0 && (stmt[e-1] == ' ' || stmt[e-1] == '\t')) e--;
            if (e < stmt.size()) stmt = stmt.substr(0, e);
            if (!stmt.empty()) {
                execute_sql(stmt);
            }
        }
    }
    printf("Goodbye.\n");
}

void REPL::execute_sql(const String& sql) {
    u64 timeout_ms = 0;
    if (parse_statement_timeout(sql, &timeout_ms)) {
        g_repl_statement_timeout_ms = timeout_ms;
        printf("SET\n\n");
        return;
    }
    String prepared_name;
    String prepared_body;
    if (parse_prepare(sql, &prepared_name, &prepared_body)) {
        g_repl_prepared_sql[std::string(prepared_name.c_str())] = prepared_body;
        printf("PREPARE\n\n");
        return;
    }
    if (parse_execute_or_deallocate(sql, "EXECUTE", &prepared_name)) {
        auto it = g_repl_prepared_sql.find(std::string(prepared_name.c_str()));
        if (it == g_repl_prepared_sql.end()) {
            printf("Error: prepared statement not found.\n\n");
            return;
        }
        execute_sql(it->second);
        return;
    }
    if (parse_execute_or_deallocate(sql, "DEALLOCATE", &prepared_name)) {
        g_repl_prepared_sql.erase(std::string(prepared_name.c_str()));
        printf("DEALLOCATE\n\n");
        return;
    }

    Parser parser(sql);
    String normalized = upper_sql(trim_sql(sql));
    if (normalized == "SHOW CONFIG") {
        printf("%s\n", DbConfigLoader::describe(db_.config()).c_str());
        return;
    }
    if (normalized == "SHOW STATS") {
        printf("%s\n", db_.stats_summary().c_str());
        return;
    }
    Statement stmt = parser.parse();
    QueryResourceGuard query_guard(db_.resources(), is_write_statement_type(stmt.type),
                                   db_.config().work_mem_bytes);
    if (!query_guard.acquired()) {
        printf("Error: server busy: admission timeout.\n\n");
        return;
    }
    bool is_write_stmt = stmt.type == StmtType::kInsert ||
                         stmt.type == StmtType::kDelete ||
                         stmt.type == StmtType::kUpdate;
    bool implicit_txn = false;

    // Check empty/unknown statement
    if (stmt.type == StmtType::kSelect && !stmt.select) {
        printf("Error: unsupported or unrecognized command.\n");
        printf("Type 'help' for available commands.\n\n");
        return;
    }

    // SHOW TABLES
    if (stmt.type == StmtType::kShowTables) {
        printf("Tables:\n");
        struct Ctx { int count; } ctx = {0};
        auto cb = [](TableEntry& e, void* c) {
            auto* ctx = static_cast<Ctx*>(c);
            printf("  %s\n", e.table_name.c_str());
            ctx->count++;
        };
        db_.catalog().for_each_table(cb, &ctx);
        if (ctx.count == 0) printf("  (empty)\n");
        printf("\n");
        return;
    }

    if (stmt.type == StmtType::kAnalyze) {
        TableEntry* table = db_.get_table(stmt.analyze_table_name);
        if (!table) {
            printf("Error: table '%s' not found\n\n", stmt.analyze_table_name.c_str());
            return;
        }
        db_.collect_statistics(table->table_id);
        printf("ANALYZE\n\n");
        return;
    }

    // BEGIN / COMMIT / ROLLBACK
    if (stmt.type == StmtType::kBegin) {
        Transaction* txn = db_.txn_manager().begin();
        if (txn) {
            printf("Transaction %lu started.\n\n", static_cast<unsigned long>(txn->id()));
        } else {
            printf("Error: transaction already active.\n\n");
        }
        return;
    }
    if (stmt.type == StmtType::kCommit) {
        Transaction* txn = db_.txn_manager().current();
        if (txn && db_.txn_manager().commit(txn)) {
            printf("Transaction committed.\n\n");
        } else {
            printf("Error: no active transaction.\n\n");
        }
        return;
    }
    if (stmt.type == StmtType::kRollback) {
        Transaction* txn = db_.txn_manager().current();
        if (txn && db_.txn_manager().rollback(txn)) {
            printf("Transaction rolled back.\n\n");
        } else {
            printf("Error: no active transaction.\n\n");
        }
        return;
    }

    // DESC TABLE
    if (stmt.type == StmtType::kDescTable && stmt.desc_table) {
        TableEntry* te = db_.get_table(stmt.desc_table->table_name);
        if (!te) {
            printf("Error: table '%s' not found\n\n", stmt.desc_table->table_name.c_str());
            return;
        }
        printf("Table: %s\n", te->table_name.c_str());
        printf("%-4s %-20s %-10s %-8s %-8s %-8s\n",
               "Col", "Name", "Type", "NotNull", "Primary", "Unique");
        printf("---- -------------------- ---------- -------- -------- --------\n");
        for (u32 i = 0; i < te->schema.column_count(); i++) {
            const Column& c = te->schema.get_column(i);
            const char* type_str = "?";
            switch (c.type) {
                case TypeId::kBool:    type_str = "BOOL"; break;
                case TypeId::kInt32:   type_str = "INT"; break;
                case TypeId::kInt64:   type_str = "BIGINT"; break;
                case TypeId::kFloat:   type_str = "FLOAT"; break;
                case TypeId::kDouble:  type_str = "DOUBLE"; break;
                case TypeId::kVarchar: type_str = "VARCHAR"; break;
                default: type_str = "UNKNOWN"; break;
            }
            printf("%-4u %-20s %-10s %-8s %-8s %-8s\n", i, c.name.c_str(), type_str,
                   c.not_null ? "YES" : "NO", c.is_primary ? "YES" : "NO",
                   c.is_unique ? "YES" : "NO");
        }
        printf("\n");
        return;
    }

    // CREATE TABLE
    if (stmt.type == StmtType::kCreateTable && stmt.create_table) {
        Schema schema;
        for (u32 i = 0; i < stmt.create_table->columns.size(); i++) {
            const auto& col = stmt.create_table->columns[i];
            Column c;
            c.name = col.name;
            c.not_null = col.not_null;
            c.is_primary = col.is_primary;
            c.is_unique = col.is_unique;
            if (col.type_name == "INT" || col.type_name == "INTEGER") c.type = TypeId::kInt32;
            else if (col.type_name == "BIGINT") c.type = TypeId::kInt64;
            else if (col.type_name == "FLOAT" || col.type_name == "REAL") c.type = TypeId::kFloat;
            else if (col.type_name == "DOUBLE" || col.type_name == "DECIMAL" ||
                     col.type_name == "NUMERIC") c.type = TypeId::kDouble;
            else if (col.type_name == "VARCHAR" || col.type_name == "TEXT") c.type = TypeId::kVarchar;
            else if (col.type_name == "BOOL" || col.type_name == "BOOLEAN") c.type = TypeId::kBool;
            else c.type = TypeId::kVarchar;
            schema.add_column(c);
        }
        if (db_.create_table(stmt.create_table->table_name, schema)) {
            printf("Table '%s' created.\n\n", stmt.create_table->table_name.c_str());
        } else {
            printf("Error: failed to create table '%s'\n\n",
                   stmt.create_table->table_name.c_str());
        }
        return;
    }

    if (stmt.type == StmtType::kDropTable) {
        if (db_.drop_table(stmt.drop_table_name)) {
            printf("Table '%s' dropped.\n\n", stmt.drop_table_name.c_str());
        } else {
            printf("Error: table '%s' not found\n\n", stmt.drop_table_name.c_str());
        }
        return;
    }

    // W22: ALTER TABLE
    if (stmt.type == StmtType::kAlterTable && stmt.alter_table) {
        auto* alt = stmt.alter_table.get();
        TableEntry* table = db_.get_table(alt->table_name);
        if (!table) {
            printf("Error: table '%s' not found\n\n", alt->table_name.c_str());
            return;
        }
        switch (alt->alter_type) {
            case AlterType::kAddColumn: {
                Column col;
                col.name = alt->new_column.name;
                if (alt->new_column.type_name == "INT") col.type = TypeId::kInt32;
                else if (alt->new_column.type_name == "BIGINT") col.type = TypeId::kInt64;
                else if (alt->new_column.type_name == "FLOAT") col.type = TypeId::kFloat;
                else if (alt->new_column.type_name == "DOUBLE") col.type = TypeId::kDouble;
                else if (alt->new_column.type_name == "VARCHAR" || alt->new_column.type_name == "TEXT") col.type = TypeId::kVarchar;
                else if (alt->new_column.type_name == "BOOL") col.type = TypeId::kBool;
                else col.type = TypeId::kVarchar;
                col.not_null = alt->new_column.not_null;
                col.default_value = alt->new_column.default_value;
                table->schema.add_column(col);
                db_.save_catalog();
                printf("Column '%s' added.\n\n", alt->new_column.name.c_str());
                break;
            }
            case AlterType::kDropColumn: {
                int idx = table->schema.get_column_index(alt->drop_column_name);
                if (idx < 0) {
                    printf("Error: column '%s' not found\n\n", alt->drop_column_name.c_str());
                } else {
                    table->schema.remove_column(static_cast<u32>(idx));
                    db_.save_catalog();
                    printf("Column '%s' dropped.\n\n", alt->drop_column_name.c_str());
                }
                break;
            }
            case AlterType::kRenameColumn: {
                int idx = table->schema.get_column_index(alt->rename_from);
                if (idx < 0) {
                    printf("Error: column '%s' not found\n\n", alt->rename_from.c_str());
                } else {
                    table->schema.rename_column(static_cast<u32>(idx), alt->rename_to);
                    db_.save_catalog();
                    printf("Column '%s' renamed to '%s'.\n\n", alt->rename_from.c_str(), alt->rename_to.c_str());
                }
                break;
            }
        }
        return;
    }

    if (stmt.type == StmtType::kCreateIndex && stmt.create_index) {
        if (db_.create_index(stmt.create_index->index_name,
                             stmt.create_index->table_name,
                             stmt.create_index->columns,
                             stmt.create_index->unique)) {
            printf("Index '%s' created.\n\n", stmt.create_index->index_name.c_str());
        } else {
            printf("Error: failed to create index '%s'\n\n", stmt.create_index->index_name.c_str());
        }
        return;
    }

    if (stmt.type == StmtType::kDropIndex) {
        if (db_.drop_index(stmt.drop_index_name)) {
            printf("Index '%s' dropped.\n\n", stmt.drop_index_name.c_str());
        } else {
            printf("Error: index '%s' not found\n\n", stmt.drop_index_name.c_str());
        }
        return;
    }

    // EXPLAIN
    if (stmt.type == StmtType::kExplain && stmt.explain_stmt) {
        Planner planner(&db_.catalog(), optimizer_config_from_db(db_));
        UniquePtr<PlanNode> plan = planner.plan(*stmt.explain_stmt);
        if (plan) {
            printf("Plan: cost=%.2f..%.2f rows=%.0f\n",
                   plan->startup_cost, plan->total_cost, plan->plan_rows);
            print_plan(plan.get());
            if (stmt.explain_analyze) {
                if (is_write_statement_type(stmt.explain_stmt->type)) {
                    printf("Execution: skipped for write statement\n");
                } else {
                    ExecutorFactory factory(db_);
                    UniquePtr<Executor> exec = factory.create(plan.get());
                    if (!exec) {
                        printf("Execution: failed to create executor\n");
                    } else {
                        clear_executor_error();
                        set_executor_deadline_ms(g_repl_statement_timeout_ms != 0
                            ? g_repl_statement_timeout_ms
                            : db_.config().statement_timeout_ms);
                        auto start = std::chrono::steady_clock::now();
                        u64 rows = 0;
                        exec->init();
                        while (true) {
                            ExecResult row = exec->next();
                            if (!row.ok()) break;
                            rows++;
                        }
                        auto end = std::chrono::steady_clock::now();
                        set_executor_deadline_ms(0);
                        double ms = static_cast<double>(
                            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()) / 1000.0;
                        printf("Execution: actual_rows=%llu execution_time_ms=%.3f",
                               static_cast<unsigned long long>(rows), ms);
                        if (executor_error()) printf(" error=\"%s\"", executor_error());
                        printf("\n");
                    }
                }
            }
            printf("\n");
        } else {
            printf("Error: failed to build plan.\n\n");
        }
        return;
    }

    // Generic: SELECT / INSERT / UPDATE / DELETE
    if (is_write_stmt && !db_.txn_manager().current()) {
        implicit_txn = db_.txn_manager().begin() != nullptr;
        if (!implicit_txn) {
            printf("Error: failed to start implicit transaction.\n\n");
            return;
        }
    }

    Planner planner(&db_.catalog(), optimizer_config_from_db(db_));
    UniquePtr<PlanNode> plan = planner.plan(stmt);
    if (!plan) {
        if (implicit_txn) db_.txn_manager().rollback(db_.txn_manager().current());
        printf("Error: failed to build plan.\n\n");
        return;
    }

    ExecutorFactory factory(db_);
    UniquePtr<Executor> exec = factory.create(plan.get());
    if (!exec) {
        if (implicit_txn) db_.txn_manager().rollback(db_.txn_manager().current());
        printf("Error: failed to create executor.\n\n");
        return;
    }

    u64 effective_timeout_ms = g_repl_statement_timeout_ms != 0
                               ? g_repl_statement_timeout_ms
                               : db_.config().statement_timeout_ms;
    clear_executor_error();
    set_executor_deadline_ms(effective_timeout_ms);
    exec->init();

    const Schema& out = exec->output_schema();
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(effective_timeout_ms);
    bool timed_out = false;

    if (out.column_count() > 0) {
        for (u32 i = 0; i < out.column_count(); i++) {
            if (i > 0) printf(" | ");
            printf("%s", out.get_column(i).name.c_str());
        }
        printf("\n");
    }

    u64 row_count = 0;
    while (true) {
        if (effective_timeout_ms != 0 && std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            break;
        }
        if (db_.config().max_result_rows != 0 && row_count >= db_.config().max_result_rows) {
            printf("Error: result row limit exceeded.\n");
            break;
        }
        ExecResult r = exec->next();
        if (!r.ok()) break;
        print_tuple(r.tuple, out);
        row_count++;
    }

    if (executor_error()) {
        if (implicit_txn) db_.txn_manager().rollback(db_.txn_manager().current());
        printf("Error: %s\n\n", executor_error());
        return;
    }

    if (timed_out) {
        if (implicit_txn) db_.txn_manager().rollback(db_.txn_manager().current());
        printf("Error: statement timeout.\n\n");
        return;
    }

    if (is_write_stmt) {
        if (implicit_txn) {
            Transaction* txn = db_.txn_manager().current();
            if (!txn || !db_.txn_manager().commit(txn)) {
                printf("Error: implicit transaction commit failed.\n\n");
                return;
            }
        } else if (!db_.txn_manager().current()) {
            db_.flush();
        }
        db_.maybe_gc();
    }

    printf("\n");
}

void REPL::print_tuple(const Tuple& tuple, const Schema& schema) {
    for (u32 i = 0; i < schema.column_count(); i++) {
        if (i > 0) printf(" | ");
        Value v = tuple.get_value(i);
        if (v.is_null()) {
            printf("NULL");
        } else {
            printf("%s", v.to_string().c_str());
        }
    }
    printf("\n");
}

void REPL::print_plan(const PlanNode* plan, int indent) {
    if (!plan) return;
    for (int i = 0; i < indent; i++) printf("  ");
    auto print_note = [](const PlanNode* p) {
        if (p && !p->optimizer_note.empty()) {
            printf(" note=\"%s\"", p->optimizer_note.c_str());
        }
    };
    switch (plan->type) {
        case PlanNodeType::kOneRow:
            printf("OneRow cost=%.2f..%.2f rows=%.0f",
                   plan->startup_cost, plan->total_cost, plan->plan_rows);
            print_note(plan);
            printf("\n");
            break;
        case PlanNodeType::kSeqScan: {
            auto* p = static_cast<const SeqScanPlan*>(plan);
            printf("SeqScan table=%s", p->table_name.c_str());
            if (!p->projected_columns.empty()) printf(" projected_cols=%u", p->projected_columns.size());
            printf(" cost=%.2f..%.2f rows=%.0f",
                   plan->startup_cost, plan->total_cost, plan->plan_rows);
            print_note(plan);
            printf("\n");
            break;
        }
        case PlanNodeType::kIndexScan: {
            auto* p = static_cast<const IndexScanPlan*>(plan);
            printf("IndexScan table=%s index=%u", p->table_name.c_str(), p->index_id);
            if (p->is_range) {
                printf(" range=[%s,%s]", p->search_key.to_string().c_str(),
                       p->range_high.to_string().c_str());
            } else {
                printf(" key=%s", p->search_key.to_string().c_str());
            }
            printf(" cost=%.2f..%.2f rows=%.0f",
                   plan->startup_cost, plan->total_cost, plan->plan_rows);
            print_note(plan);
            printf("\n");
            break;
        }
        case PlanNodeType::kIndexOnlyScan: {
            auto* p = static_cast<const IndexOnlyScanPlan*>(plan);
            printf("IndexOnlyScan table=%s index=%u", p->table_name.c_str(), p->index_id);
            if (p->is_range) {
                printf(" range=[%s,%s]", p->search_key.to_string().c_str(),
                       p->range_high.to_string().c_str());
            } else {
                printf(" key=%s", p->search_key.to_string().c_str());
            }
            printf(" cost=%.2f..%.2f rows=%.0f",
                   plan->startup_cost, plan->total_cost, plan->plan_rows);
            print_note(plan);
            printf("\n");
            break;
        }
        case PlanNodeType::kFilter: {
            auto* p = static_cast<const FilterPlan*>(plan);
            printf("Filter\n");
            print_plan(p->child.get(), indent + 1);
            break;
        }
        case PlanNodeType::kProject: {
            auto* p = static_cast<const ProjectPlan*>(plan);
            printf("Project\n");
            print_plan(p->child.get(), indent + 1);
            break;
        }
        case PlanNodeType::kInsert:    printf("Insert\n"); break;
        case PlanNodeType::kDelete: {
            auto* p = static_cast<const DeletePlan*>(plan);
            printf("Delete\n");
            print_plan(p->child.get(), indent + 1);
            break;
        }
        case PlanNodeType::kUpdate: {
            auto* p = static_cast<const UpdatePlan*>(plan);
            printf("Update\n");
            print_plan(p->child.get(), indent + 1);
            break;
        }
        case PlanNodeType::kJoin: {
            auto* p = static_cast<const JoinPlan*>(plan);
            const char* alg = "NestedLoopJoin";
            const char* detail = "";
            if (p->algorithm == JoinAlgorithm::kHash) {
                alg = "HashJoin";
                detail = p->hash_build_left ? " build=left" : " build=right";
            } else if (p->algorithm == JoinAlgorithm::kIndexLookup) {
                alg = "IndexLookupJoin";
            } else if (p->algorithm == JoinAlgorithm::kMerge) {
                alg = "MergeJoin";
            }
            if (p->join_type == JoinType::kLeft) {
                printf("LeftJoin algorithm=%s cost=%.2f..%.2f rows=%.0f%s\n",
                       alg, plan->startup_cost, plan->total_cost, plan->plan_rows, detail);
            } else {
                printf("%s cost=%.2f..%.2f rows=%.0f%s\n",
                       alg, plan->startup_cost, plan->total_cost, plan->plan_rows, detail);
            }
            print_plan(p->left.get(), indent + 1);
            print_plan(p->right.get(), indent + 1);
            break;
        }
        case PlanNodeType::kSort: {
            auto* p = static_cast<const SortPlan*>(plan);
            printf("Sort\n");
            print_plan(p->child.get(), indent + 1);
            break;
        }
        case PlanNodeType::kLimit: {
            auto* p = static_cast<const LimitPlan*>(plan);
            printf("Limit limit=%d offset=%d\n", p->limit, p->offset);
            print_plan(p->child.get(), indent + 1);
            break;
        }
        case PlanNodeType::kDistinct: {
            auto* p = static_cast<const DistinctPlan*>(plan);
            printf("Distinct\n");
            print_plan(p->child.get(), indent + 1);
            break;
        }
        case PlanNodeType::kAggregate: {
            auto* p = static_cast<const AggregatePlan*>(plan);
            printf("Aggregate\n");
            print_plan(p->child.get(), indent + 1);
            break;
        }
        case PlanNodeType::kUnion: {
            auto* p = static_cast<const UnionPlan*>(plan);
            printf(p->all ? "UnionAll\n" : "Union\n");
            print_plan(p->left.get(), indent + 1);
            print_plan(p->right.get(), indent + 1);
            break;
        }
    }
}

} // namespace minidb
