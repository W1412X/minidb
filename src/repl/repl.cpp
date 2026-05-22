/**
 * @file repl.cpp
 * @brief Interactive SQL REPL — Single database
 */
#include "repl/repl.h"
#include "sql/parser/parser.h"
#include "sql/planner/planner.h"
#include "sql/executor/executor_factory.h"
#include "sql/executor/executor.h"
#include "catalog/catalog.h"
#include <cstdio>
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <cerrno>
#include <unistd.h>
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

static bool parse_isolation_level(const String& sql, IsolationLevel* out) {
    String t = trim_sql(sql);
    String u = upper_sql(t);
    const char* prefix = "SET ISOLATION_LEVEL";
    u32 n = static_cast<u32>(std::strlen(prefix));
    if (u.size() < n || std::strncmp(u.c_str(), prefix, n) != 0) return false;
    const char* p = u.c_str() + n;
    while (*p == ' ' || *p == '\t' || *p == '=') p++;
    String value(p);
    while (!value.empty() && (value[value.size() - 1] == ';' ||
                              value[value.size() - 1] == ' ' ||
                              value[value.size() - 1] == '\t' ||
                              value[value.size() - 1] == '\n')) {
        value = value.substr(0, value.size() - 1);
    }
    if (value == "SERIALIZABLE") { if (out) *out = IsolationLevel::kSerializable; return true; }
    if (value == "SNAPSHOT")     { if (out) *out = IsolationLevel::kSnapshot;     return true; }
    return false;
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

REPL::REPL(Database& db) : db_(db) {
    interactive_ = isatty(fileno(stdin)) != 0;
}

extern "C" void repl_sigint_handler(int) {
    // Signal handler context: only async-signal-safe ops are allowed.
    // request_executor_interrupt() does a single relaxed atomic store.
    minidb::request_executor_interrupt();
}

namespace {

// Walk `text` and track per-character quote state. Returns the index of the
// first ';' that sits outside quotes, or text.size() if none.
struct ScanState {
    bool in_single = false;   // inside '...'
    bool in_double = false;   // inside "..."
    bool in_line_comment = false;
};

u32 find_unquoted_semicolon(const String& text, u32 from, ScanState* state) {
    for (u32 i = from; i < text.size(); i++) {
        char c = text[i];
        if (state->in_line_comment) {
            if (c == '\n') state->in_line_comment = false;
            continue;
        }
        if (state->in_single) {
            if (c == '\'') state->in_single = false;
            continue;
        }
        if (state->in_double) {
            if (c == '"') state->in_double = false;
            continue;
        }
        if (c == '-' && i + 1 < text.size() && text[i + 1] == '-') {
            state->in_line_comment = true;
            i++;
            continue;
        }
        if (c == '\'') { state->in_single = true; continue; }
        if (c == '"')  { state->in_double = true; continue; }
        if (c == ';')  return i;
    }
    return text.size();
}

bool statement_is_complete(const String& text) {
    ScanState s;
    u32 pos = find_unquoted_semicolon(text, 0, &s);
    return pos < text.size();
}

String strip_leading_ws(const String& s) {
    u32 i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    return s.substr(i);
}

bool is_blank_or_comment(const String& s) {
    String t = strip_leading_ws(s);
    if (t.empty()) return true;
    if (t.size() >= 2 && t[0] == '-' && t[1] == '-') return true;
    return false;
}

bool starts_with_word_ci(const String& s, const char* word) {
    String t = strip_leading_ws(s);
    u32 n = static_cast<u32>(std::strlen(word));
    if (t.size() < n) return false;
    for (u32 i = 0; i < n; i++) {
        char a = t[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != word[i]) return false;
    }
    // word must be followed by end / ws / ';' so "exits" does not match "exit"
    if (t.size() == n) return true;
    char c = t[n];
    return c == ' ' || c == '\t' || c == ';' || c == '\n';
}

}  // namespace

void REPL::run() {
    if (interactive_) {
        // Ctrl-C cancels the current statement instead of killing the
        // process. We install the handler only in interactive mode so
        // scripted runs (pipes / shell tests) keep the standard
        // terminate-on-SIGINT behaviour.
        struct sigaction sa;
        sa.sa_handler = repl_sigint_handler;
        sigemptyset(&sa.sa_mask);
        // Intentionally NOT SA_RESTART — when a Ctrl-C lands while we are
        // blocked in fgets() the read should return EINTR so the loop can
        // discard the half-typed line and re-prompt, psql-style.
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, nullptr);

        printf("MiniDB v0.3.0 — Interactive SQL Shell\n");
        printf("Type 'help' for commands, 'exit' to quit. Statements end with ';'.\n");
        printf("Ctrl-C cancels the current statement.\n\n");
    }

    String buffer;
    char line[4096];
    while (true) {
        const bool partial = !buffer.empty();
        if (interactive_) {
            const char* tail = ">";
            if (partial) {
                tail = "->";
            } else if (db_.txn_manager().current()) {
                tail = "*>";
            }
            printf("minidb%s ", tail);
            fflush(stdout);
        }
        if (!fgets(line, sizeof(line), stdin)) {
            if (interactive_ && errno == EINTR) {
                // Ctrl-C at the prompt: drop any partial statement and start fresh.
                clearerr(stdin);
                buffer.clear();
                clear_executor_interrupt();
                printf("^C\n");
                continue;
            }
            break;
        }

        // Top-level meta commands only apply when a fresh statement is starting.
        if (!partial) {
            String trimmed(line);
            // Strip trailing newline for the meta-command check.
            while (!trimmed.empty() &&
                   (trimmed[trimmed.size() - 1] == '\n' ||
                    trimmed[trimmed.size() - 1] == '\r')) {
                trimmed = trimmed.substr(0, trimmed.size() - 1);
            }
            if (starts_with_word_ci(trimmed, "exit") ||
                starts_with_word_ci(trimmed, "quit")) {
                break;
            }
            if (starts_with_word_ci(trimmed, "help")) {
                if (interactive_) {
                    printf("Commands:\n");
                    printf("  <SQL>;             one or more SQL statements terminated by ';'.\n");
                    printf("                     Statements may span multiple lines.\n");
                    printf("  SHOW TABLES;       list every table in the current database.\n");
                    printf("  DESC <table>;      describe a table's schema.\n");
                    printf("  EXPLAIN <stmt>;    show the query plan (read-only statements).\n");
                    printf("  EXPLAIN ANALYZE <stmt>;  run the read-only statement and report timing.\n");
                    printf("  BEGIN; COMMIT; ROLLBACK;   transaction control.\n");
                    printf("  \\timing [on|off]   toggle per-statement timing.\n");
                    printf("  help               this list.\n");
                    printf("  exit | quit        leave the shell.\n\n");
                }
                continue;
            }
            if (interactive_ && trimmed.size() > 0 && trimmed[0] == '\\') {
                String cmd = strip_leading_ws(trimmed.substr(1));
                if (starts_with_word_ci(cmd, "timing")) {
                    String arg = strip_leading_ws(cmd.substr(6));
                    if (arg.empty()) {
                        show_timing_ = !show_timing_;
                    } else if (arg == "on" || arg == "ON") {
                        show_timing_ = true;
                    } else if (arg == "off" || arg == "OFF") {
                        show_timing_ = false;
                    } else {
                        printf("Error: usage: \\timing [on|off]\n\n");
                        continue;
                    }
                    printf("Timing is %s.\n\n", show_timing_ ? "on" : "off");
                    continue;
                }
                printf("Error: unknown meta-command. Try '\\timing'.\n\n");
                continue;
            }
            if (is_blank_or_comment(trimmed)) {
                continue;
            }
        }

        // Accumulate raw lines into the statement buffer.
        buffer += line;

        // Drain every complete statement (terminated by an unquoted ';').
        while (true) {
            ScanState scan;
            u32 sc = find_unquoted_semicolon(buffer, 0, &scan);
            if (sc >= buffer.size()) break;     // need more input
            String stmt = buffer.substr(0, sc);
            buffer = buffer.substr(sc + 1);
            String trimmed_stmt = strip_leading_ws(stmt);
            if (!trimmed_stmt.empty()) {
                execute_sql(trimmed_stmt);
            }
        }

        // If the buffer is now just whitespace / line comments, clear it so we
        // do not stay in a fake "partial" state forever.
        if (is_blank_or_comment(buffer)) {
            buffer.clear();
        }
    }
    // Printed unconditionally — several tests in tests/regression/ grep for
    // "Goodbye" as a clean-exit sentinel. testlib._clean_minidb_lines already
    // filters this line out of result-row parsing, so the legacy shell tests
    // that pipe input to minidb stay unaffected.
    printf("Goodbye.\n");
}

void REPL::execute_sql(const String& sql) {
    // Each statement starts with a fresh cancellation flag so a SIGINT
    // delivered between statements does not abort the next one.
    clear_executor_interrupt();
    u64 timeout_ms = 0;
    if (parse_statement_timeout(sql, &timeout_ms)) {
        g_repl_statement_timeout_ms = timeout_ms;
        printf("SET\n\n");
        return;
    }
    IsolationLevel iso_level;
    if (parse_isolation_level(sql, &iso_level)) {
        db_.txn_manager().set_default_isolation(iso_level);
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
    if (!parser.ok()) {
        const ParserError& e = parser.error();
        if (e.line != 0 || e.column != 0) {
            printf("Error: %s near \"%s\" at line %u column %u\n\n",
                   e.message.c_str(), e.near.c_str(), e.line, e.column);
        } else if (!e.message.empty()) {
            printf("Error: %s\n\n", e.message.c_str());
        } else {
            printf("Error: failed to parse statement.\n\n");
        }
        return;
    }
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

    // After a successful parse, stmt.type+stmt.select are always consistent.
    // The "empty SELECT" sentinel that the old code printed as
    // "Error: unsupported or unrecognized command." can no longer occur:
    // the parser sets a proper error before returning a default Statement.

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

    // DDL implicitly commits the surrounding user transaction (MySQL /
    // SQL-standard semantics). MiniDB has no full transactional DDL —
    // catalog edits go to disk synchronously via save_catalog(). Auto-
    // committing here makes the contract explicit: CREATE / DROP / ALTER
    // inside BEGIN ends that BEGIN, so a later ROLLBACK cannot pretend
    // to undo the schema change.
    bool is_ddl = stmt.type == StmtType::kCreateTable ||
                  stmt.type == StmtType::kDropTable ||
                  stmt.type == StmtType::kCreateIndex ||
                  stmt.type == StmtType::kDropIndex ||
                  stmt.type == StmtType::kAlterTable ||
                  stmt.type == StmtType::kAnalyze;
    if (is_ddl && db_.txn_manager().current()) {
        Transaction* txn = db_.txn_manager().current();
        if (!db_.txn_manager().commit(txn)) {
            printf("Error: implicit commit before DDL failed.\n\n");
            return;
        }
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
            c.default_value = col.default_value;
            if (col.type_name == "INT" || col.type_name == "INTEGER") c.type = TypeId::kInt32;
            else if (col.type_name == "BIGINT") c.type = TypeId::kInt64;
            else if (col.type_name == "FLOAT" || col.type_name == "REAL") c.type = TypeId::kFloat;
            else if (col.type_name == "DOUBLE" || col.type_name == "DECIMAL" ||
                     col.type_name == "NUMERIC") c.type = TypeId::kDouble;
            else if (col.type_name == "VARCHAR" || col.type_name == "TEXT") c.type = TypeId::kVarchar;
            else if (col.type_name == "BOOL" || col.type_name == "BOOLEAN") c.type = TypeId::kBool;
            else c.type = TypeId::kVarchar;
            // Only VARCHAR(n) carries a bound; TEXT and bare VARCHAR keep
            // varchar_length = 0 (unbounded). Negative parser sentinel is
            // mapped to 0 too.
            if (col.type_name == "VARCHAR" && col.varchar_length > 0) {
                c.varchar_length = static_cast<u32>(col.varchar_length);
            }
            c.check_expr = col.check_expr;
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
                if (alt->new_column.type_name == "VARCHAR" &&
                    alt->new_column.varchar_length > 0) {
                    col.varchar_length = static_cast<u32>(alt->new_column.varchar_length);
                }
                String error;
                if (db_.alter_table_add_column(alt->table_name, col, &error)) {
                    printf("Column '%s' added.\n\n", alt->new_column.name.c_str());
                } else {
                    printf("Error: %s\n\n", error.c_str());
                }
                break;
            }
            case AlterType::kDropColumn: {
                String error;
                if (db_.alter_table_drop_column(alt->table_name, alt->drop_column_name, &error)) {
                    printf("Column '%s' dropped.\n\n", alt->drop_column_name.c_str());
                } else {
                    printf("Error: %s\n\n", error.c_str());
                }
                break;
            }
            case AlterType::kRenameColumn: {
                String error;
                if (db_.alter_table_rename_column(alt->table_name, alt->rename_from,
                                                  alt->rename_to, &error)) {
                    printf("Column '%s' renamed to '%s'.\n\n", alt->rename_from.c_str(), alt->rename_to.c_str());
                } else {
                    printf("Error: %s\n\n", error.c_str());
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
        // Most common cause: a referenced table does not exist. Catch that
        // up front so the user gets a useful message instead of the generic
        // "failed to build plan".
        const String* missing = nullptr;
        if (stmt.type == StmtType::kInsert && stmt.insert) {
            if (!db_.get_table(stmt.insert->table.name)) missing = &stmt.insert->table.name;
        } else if (stmt.type == StmtType::kUpdate && stmt.update) {
            if (!db_.get_table(stmt.update->table.name)) missing = &stmt.update->table.name;
        } else if (stmt.type == StmtType::kDelete && stmt.delete_stmt) {
            if (!db_.get_table(stmt.delete_stmt->table.name)) missing = &stmt.delete_stmt->table.name;
        } else if (stmt.type == StmtType::kSelect && stmt.select) {
            for (u32 i = 0; i < stmt.select->from_tables.size(); i++) {
                if (!db_.get_table(stmt.select->from_tables[i].name)) {
                    missing = &stmt.select->from_tables[i].name;
                    break;
                }
            }
        }
        if (missing) {
            printf("Error: table '%s' not found.\n\n", missing->c_str());
        } else {
            printf("Error: failed to build plan (check column names and join conditions).\n\n");
        }
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

    // Statement-level savepoint (ACID A2): record where the undo log is
    // right now so a mid-statement error inside an explicit transaction
    // rolls back only this statement's writes. Implicit-txn errors fall
    // through to the existing full rollback below.
    bool savepoint_active = !implicit_txn && db_.txn_manager().current();
    u32 savepoint_mark = savepoint_active ? db_.txn_manager().current()->undo_mark() : 0;

    exec->init();

    const Schema& out = exec->output_schema();
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(effective_timeout_ms);
    bool timed_out = false;

    // Buffer the result rows first. Interactive mode then renders an
    // aligned table; non-interactive mode falls back to the legacy
    // pipe-separated stream, so shell tests are unaffected.
    Vector<Vector<String>> rows;
    bool row_limit_hit = false;
    auto exec_start = std::chrono::steady_clock::now();
    while (true) {
        if (executor_cancelled()) break;
        if (effective_timeout_ms != 0 && std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            break;
        }
        if (db_.config().max_result_rows != 0 && rows.size() >= db_.config().max_result_rows) {
            row_limit_hit = true;
            break;
        }
        ExecResult r = exec->next();
        if (!r.ok()) break;
        Vector<String> cells;
        for (u32 i = 0; i < out.column_count(); i++) {
            Value v = r.tuple.get_value(i);
            cells.push_back(v.is_null() ? String("NULL") : v.to_string());
        }
        rows.push_back(static_cast<Vector<String>&&>(cells));
    }
    auto exec_end = std::chrono::steady_clock::now();
    double elapsed_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(exec_end - exec_start).count()) / 1000.0;

    if (executor_error()) {
        if (implicit_txn) {
            db_.txn_manager().rollback(db_.txn_manager().current());
        } else if (savepoint_active) {
            db_.txn_manager().rollback_to_savepoint(
                db_.txn_manager().current(), savepoint_mark);
        }
        printf("Error: %s\n\n", executor_error());
        return;
    }
    if (timed_out) {
        if (implicit_txn) {
            db_.txn_manager().rollback(db_.txn_manager().current());
        } else if (savepoint_active) {
            db_.txn_manager().rollback_to_savepoint(
                db_.txn_manager().current(), savepoint_mark);
        }
        printf("Error: statement timeout.\n\n");
        return;
    }

    // DML statements report through a one-column tuple named
    // "affected_rows" (insert/update) or "deleted_rows" (delete). In
    // interactive mode we surface that as a friendly "INSERT N" /
    // "UPDATE N" / "DELETE N" line, mirroring psql. Non-interactive
    // mode keeps the legacy two-row column to avoid breaking tests.
    bool is_dml_summary = out.column_count() == 1 &&
        (out.get_column(0).name == String("affected_rows") ||
         out.get_column(0).name == String("deleted_rows"));

    if (interactive_) {
        if (is_dml_summary) {
            const char* verb = "RESULT";
            switch (stmt.type) {
                case StmtType::kInsert: verb = "INSERT"; break;
                case StmtType::kUpdate: verb = "UPDATE"; break;
                case StmtType::kDelete: verb = "DELETE"; break;
                default: break;
            }
            const char* n = "0";
            if (!rows.empty() && !rows[0].empty()) n = rows[0][0].c_str();
            printf("%s %s\n", verb, n);
        } else if (out.column_count() == 0) {
            // Statements that succeed without a result tuple — nothing to print.
        } else {
            render_aligned_table(rows, out);
            printf("(%llu row%s)\n",
                   static_cast<unsigned long long>(rows.size()),
                   rows.size() == 1 ? "" : "s");
            if (row_limit_hit) {
                printf("Note: result truncated at max_result_rows = %llu.\n",
                       static_cast<unsigned long long>(db_.config().max_result_rows));
            }
        }
        if (show_timing_) printf("Time: %.3f ms\n", elapsed_ms);
    } else {
        if (out.column_count() > 0) {
            for (u32 i = 0; i < out.column_count(); i++) {
                if (i > 0) printf(" | ");
                printf("%s", out.get_column(i).name.c_str());
            }
            printf("\n");
        }
        for (u32 r = 0; r < rows.size(); r++) {
            for (u32 i = 0; i < rows[r].size(); i++) {
                if (i > 0) printf(" | ");
                printf("%s", rows[r][i].c_str());
            }
            printf("\n");
        }
        if (row_limit_hit) {
            printf("Error: result row limit exceeded.\n");
        }
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

void REPL::render_aligned_table(const Vector<Vector<String>>& rows,
                                const Schema& schema) {
    u32 n = schema.column_count();
    if (n == 0) return;
    Vector<u32> widths;
    widths.resize(n);
    for (u32 i = 0; i < n; i++) widths[i] = schema.get_column(i).name.size();
    for (u32 r = 0; r < rows.size(); r++) {
        for (u32 i = 0; i < rows[r].size() && i < n; i++) {
            u32 w = rows[r][i].size();
            if (w > widths[i]) widths[i] = w;
        }
    }
    // Header
    for (u32 i = 0; i < n; i++) {
        if (i > 0) printf(" | ");
        printf("%-*s", static_cast<int>(widths[i]), schema.get_column(i).name.c_str());
    }
    printf("\n");
    // Separator: dashes per column, "-+-" at boundaries.
    for (u32 i = 0; i < n; i++) {
        if (i > 0) printf("-+-");
        for (u32 j = 0; j < widths[i]; j++) printf("-");
    }
    printf("\n");
    // Rows
    for (u32 r = 0; r < rows.size(); r++) {
        for (u32 i = 0; i < n; i++) {
            if (i > 0) printf(" | ");
            const String& cell = i < rows[r].size() ? rows[r][i] : String("");
            printf("%-*s", static_cast<int>(widths[i]), cell.c_str());
        }
        printf("\n");
    }
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
