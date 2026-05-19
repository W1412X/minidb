#include "network/server.h"
#include "sql/parser/parser.h"
#include "sql/planner/planner.h"
#include "sql/executor/executor_factory.h"
#include "sql/executor/expression_evaluator.h"
#include "catalog/catalog.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <signal.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <unordered_map>
#include <memory>

namespace minidb {

static thread_local u64 g_statement_timeout_ms = 0;
static Mutex g_prepared_latch;
struct PreparedEntry {
    String sql;
    StmtType type = StmtType::kSelect;
    UniquePtr<PlanNode> plan;
};
static std::unordered_map<std::string, std::unique_ptr<PreparedEntry>> g_prepared_cache;

struct CursorState {
    UniquePtr<Executor> exec;
    Schema schema;
    u64 timeout_ms = 0;
};

static thread_local std::unordered_map<std::string, std::unique_ptr<CursorState>> g_session_cursors;

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
    const char* as_kw = "AS ";
    if (rest_upper.size() < 3 || std::strncmp(rest_upper.c_str(), as_kw, 3) != 0) return false;
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

static bool parse_declare_cursor(const String& sql, String* name, String* body) {
    String t = trim_sql(sql);
    String u = upper_sql(t);
    const char* prefix = "DECLARE ";
    u32 n = static_cast<u32>(std::strlen(prefix));
    if (u.size() <= n || std::strncmp(u.c_str(), prefix, n) != 0) return false;
    u32 pos = n;
    while (pos < t.size() && (t[pos] == ' ' || t[pos] == '\t')) pos++;
    u32 name_start = pos;
    while (pos < t.size() && t[pos] != ' ' && t[pos] != '\t') pos++;
    if (pos == name_start) return false;
    String cursor_name = t.substr(name_start, pos - name_start);
    String rest = trim_sql(t.substr(pos));
    String rest_upper = upper_sql(rest);
    const char* mid = "CURSOR FOR ";
    u32 m = static_cast<u32>(std::strlen(mid));
    if (rest_upper.size() <= m || std::strncmp(rest_upper.c_str(), mid, m) != 0) return false;
    String cursor_sql = trim_sql(rest.substr(m));
    if (cursor_sql.empty()) return false;
    if (name) *name = cursor_name;
    if (body) *body = cursor_sql;
    return true;
}

static bool parse_fetch_cursor(const String& sql, String* name, u64* count) {
    String t = trim_sql(sql);
    String u = upper_sql(t);
    const char* prefix = "FETCH ";
    u32 n = static_cast<u32>(std::strlen(prefix));
    if (u.size() <= n || std::strncmp(u.c_str(), prefix, n) != 0) return false;
    const char* p = t.c_str() + n;
    while (*p == ' ' || *p == '\t') p++;
    char* end = nullptr;
    unsigned long long v = std::strtoull(p, &end, 10);
    if (end == p) return false;
    while (*end == ' ' || *end == '\t') end++;
    const char* from_kw = "FROM ";
    if (std::strncmp(upper_sql(String(end)).c_str(), from_kw, 5) != 0) return false;
    end += 5;
    while (*end == ' ' || *end == '\t') end++;
    const char* start = end;
    while (*end && *end != ' ' && *end != '\t') end++;
    if (end == start) return false;
    if (name) *name = String(start, static_cast<u32>(end - start));
    if (count) *count = static_cast<u64>(v);
    return true;
}

static bool parse_close_cursor(const String& sql, String* name) {
    String t = trim_sql(sql);
    String u = upper_sql(t);
    const char* prefix = "CLOSE ";
    u32 n = static_cast<u32>(std::strlen(prefix));
    if (u.size() <= n || std::strncmp(u.c_str(), prefix, n) != 0) return false;
    String cursor_name = trim_sql(t.substr(n));
    if (cursor_name.empty()) return false;
    if (name) *name = cursor_name;
    return true;
}

static bool needs_schema_execution_lock(const Statement& stmt) {
    return stmt.type == StmtType::kCreateTable || stmt.type == StmtType::kDropTable ||
           stmt.type == StmtType::kCreateIndex || stmt.type == StmtType::kDropIndex ||
           stmt.type == StmtType::kAlterTable || stmt.type == StmtType::kAnalyze;
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

static bool is_plannable_statement_type(StmtType type) {
    return type == StmtType::kSelect || type == StmtType::kInsert ||
           type == StmtType::kUpdate || type == StmtType::kDelete;
}

static void clear_prepared_cache() {
    LockGuard guard(g_prepared_latch);
    g_prepared_cache.clear();
}

struct ExecRwGuard {
    RwLock& lock;
    bool write;
    ExecRwGuard(RwLock& l, bool w) : lock(l), write(w) {
        if (write) lock.write_lock();
    }
    ~ExecRwGuard() {
        if (write) lock.write_unlock();
    }
};

static OptimizerConfig optimizer_config_from_db(const Database& db) {
    OptimizerConfig cfg;
    cfg.enable_hashjoin = db.config().enable_hashjoin;
    cfg.enable_indexscan = db.config().enable_indexscan;
    cfg.enable_indexonlyscan = db.config().enable_indexonlyscan;
    cfg.remote_storage = db.config().storage_mode == "remote";
    return cfg;
}

static String fetch_cursor_rows(CursorState* cursor, u64 count) {
    if (!cursor || !cursor->exec) return String("Error: cursor not found.\n");
    String result;
    const Schema& out = cursor->schema;
    if (out.column_count() > 0) {
        for (u32 i = 0; i < out.column_count(); i++) {
            if (i > 0) result += " | ";
            result += out.get_column(i).name;
        }
        result += "\n";
    }
    clear_executor_error();
    set_executor_deadline_ms(cursor->timeout_ms);
    u64 row_count = 0;
    while (row_count < count) {
        if (executor_cancelled()) break;
        ExecResult r = cursor->exec->next();
        if (!r.ok()) break;
        for (u32 i = 0; i < out.column_count(); i++) {
            if (i > 0) result += " | ";
            Value v = r.tuple.get_value(i);
            result += v.is_null() ? String("NULL") : v.to_string();
        }
        result += "\n";
        row_count++;
    }
    if (executor_error()) {
        return String("Error: ") + executor_error() + "\n";
    }
    return result;
}

Server::Server(Database& db, u16 port)
    : db_(db), port_(port), server_fd_(-1), running_(false) {}

Server::~Server() { stop(); }

String Server::execute_plan_result(StmtType type, PlanNode* plan) {
    bool is_write_stmt = type == StmtType::kInsert ||
                         type == StmtType::kDelete ||
                         type == StmtType::kUpdate;
    bool implicit_txn = false;
    String result;

    if (is_write_stmt && !db_.txn_manager().current()) {
        implicit_txn = db_.txn_manager().begin() != nullptr;
        if (!implicit_txn) return String("Error: failed to start implicit transaction.\n");
    }

    ExecutorFactory factory(db_);
    UniquePtr<Executor> exec = factory.create(plan);
    if (!exec) {
        if (implicit_txn) db_.txn_manager().rollback(db_.txn_manager().current());
        return String("Error: failed to create executor.\n");
    }

    u64 effective_timeout_ms = g_statement_timeout_ms != 0
                               ? g_statement_timeout_ms
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
            if (i > 0) result += " | ";
            result += out.get_column(i).name;
        }
        result += "\n";
    }

    u64 row_count = 0;
    while (true) {
        if (effective_timeout_ms != 0 && std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            break;
        }
        if (db_.config().max_result_rows != 0 && row_count >= db_.config().max_result_rows) {
            result += "Error: result row limit exceeded.\n";
            break;
        }
        if (db_.config().max_result_bytes != 0 && result.size() >= db_.config().max_result_bytes) {
            result += "Error: result byte limit exceeded.\n";
            break;
        }
        ExecResult r = exec->next();
        if (!r.ok()) break;
        for (u32 i = 0; i < out.column_count(); i++) {
            if (i > 0) result += " | ";
            Value v = r.tuple.get_value(i);
            result += v.is_null() ? String("NULL") : v.to_string();
        }
        result += "\n";
        row_count++;
    }

    if (executor_error()) {
        if (implicit_txn) db_.txn_manager().rollback(db_.txn_manager().current());
        return String("Error: ") + executor_error() + "\n";
    }

    if (timed_out) {
        if (implicit_txn) db_.txn_manager().rollback(db_.txn_manager().current());
        return String("Error: statement timeout.\n");
    }

    if (is_write_stmt) {
        if (implicit_txn) {
            Transaction* txn = db_.txn_manager().current();
            if (!txn || !db_.txn_manager().commit(txn)) {
                return String("Error: implicit transaction commit failed.\n");
            }
        } else if (!db_.txn_manager().current()) {
            db_.flush();
        }
        db_.maybe_gc();
    }

    return result;
}

void Server::stop() {
    running_ = false;
    queue_cv_.notify_all();
    if (server_fd_ >= 0) {
        ::shutdown(server_fd_, SHUT_RDWR);
        ::close(server_fd_);
        server_fd_ = -1;
    }
    {
        LockGuard guard(clients_latch_);
        for (int fd : active_clients_) {
            const char* msg = "Server shutting down.\n";
            send(fd, msg, std::strlen(msg), 0);
            ::shutdown(fd, SHUT_RDWR);
        }
    }
    {
        std::lock_guard<std::mutex> guard(queue_mutex_);
        while (!pending_clients_.empty()) {
            ::close(pending_clients_.front());
            pending_clients_.pop();
        }
    }
    for (auto& t : client_workers_) {
        if (t.joinable()) t.join();
    }
    client_workers_.clear();
}

String Server::execute_sql(const String& sql) {
    String result;
    u64 timeout_ms = 0;
    if (parse_statement_timeout(sql, &timeout_ms)) {
        g_statement_timeout_ms = timeout_ms;
        return String("SET\n");
    }
    String prepared_name;
    String prepared_body;
    if (parse_prepare(sql, &prepared_name, &prepared_body)) {
        auto entry = std::make_unique<PreparedEntry>();
        entry->sql = prepared_body;
        Parser prepared_parser(prepared_body);
        Statement prepared_stmt = prepared_parser.parse();
        if (is_plannable_statement_type(prepared_stmt.type)) {
            Planner planner(&db_.catalog(), optimizer_config_from_db(db_));
            entry->type = prepared_stmt.type;
            entry->plan = planner.plan(prepared_stmt);
            if (!entry->plan) return String("Error: failed to build plan.\n");
        } else {
            entry->type = prepared_stmt.type;
        }
        LockGuard guard(g_prepared_latch);
        g_prepared_cache[std::string(prepared_name.c_str())] = std::move(entry);
        return String("PREPARE\n");
    }
    if (parse_execute_or_deallocate(sql, "EXECUTE", &prepared_name)) {
        String body;
        {
            LockGuard guard(g_prepared_latch);
            auto it = g_prepared_cache.find(std::string(prepared_name.c_str()));
            if (it == g_prepared_cache.end()) return String("Error: prepared statement not found.\n");
            if (it->second->plan) {
                QueryResourceGuard query_guard(db_.resources(), is_write_statement_type(it->second->type),
                                               db_.config().work_mem_bytes);
                if (!query_guard.acquired()) {
                    return String("Error: server busy: admission timeout.\n");
                }
                return execute_plan_result(it->second->type, it->second->plan.get());
            }
            body = it->second->sql;
        }
        return execute_sql(body);
    }
    if (parse_execute_or_deallocate(sql, "DEALLOCATE", &prepared_name)) {
        LockGuard guard(g_prepared_latch);
        g_prepared_cache.erase(std::string(prepared_name.c_str()));
        return String("DEALLOCATE\n");
    }
    String cursor_name;
    String cursor_body;
    if (parse_declare_cursor(sql, &cursor_name, &cursor_body)) {
        Parser cursor_parser(cursor_body);
        Statement cursor_stmt = cursor_parser.parse();
        if (cursor_stmt.type != StmtType::kSelect || !cursor_stmt.select) {
            return String("Error: cursor requires SELECT.\n");
        }
        QueryResourceGuard query_guard(db_.resources(), false, db_.config().work_mem_bytes);
        if (!query_guard.acquired()) return String("Error: server busy: admission timeout.\n");
        ExecRwGuard guard(exec_latch_, false);
        Planner planner(&db_.catalog(), optimizer_config_from_db(db_));
        UniquePtr<PlanNode> plan = planner.plan(cursor_stmt);
        if (!plan) return String("Error: failed to build plan.\n");
        ExecutorFactory factory(db_);
        UniquePtr<Executor> exec = factory.create(plan.get());
        if (!exec) return String("Error: failed to create executor.\n");
        clear_executor_error();
        u64 effective_timeout_ms = g_statement_timeout_ms != 0
                                   ? g_statement_timeout_ms
                                   : db_.config().statement_timeout_ms;
        set_executor_deadline_ms(effective_timeout_ms);
        exec->init();
        if (executor_error()) return String("Error: ") + executor_error() + "\n";
        auto cursor = std::make_unique<CursorState>();
        cursor->schema = exec->output_schema();
        cursor->timeout_ms = effective_timeout_ms;
        cursor->exec = static_cast<UniquePtr<Executor>&&>(exec);
        g_session_cursors[std::string(cursor_name.c_str())] = std::move(cursor);
        return String("DECLARE CURSOR\n");
    }
    u64 fetch_count = 0;
    if (parse_fetch_cursor(sql, &cursor_name, &fetch_count)) {
        auto it = g_session_cursors.find(std::string(cursor_name.c_str()));
        if (it == g_session_cursors.end()) return String("Error: cursor not found.\n");
        return fetch_cursor_rows(it->second.get(), fetch_count);
    }
    if (parse_close_cursor(sql, &cursor_name)) {
        auto erased = g_session_cursors.erase(std::string(cursor_name.c_str()));
        return erased ? String("CLOSE CURSOR\n") : String("Error: cursor not found.\n");
    }

    String normalized = upper_sql(trim_sql(sql));
    if (normalized == "SHOW CONFIG") {
        return DbConfigLoader::describe(db_.config()) + "\n";
    }
    if (normalized == "SHOW STATS") {
        return db_.stats_summary() + "\n";
    }

    Parser parser(sql);
    Statement stmt = parser.parse();
    QueryResourceGuard query_guard(db_.resources(), is_write_statement_type(stmt.type),
                                   db_.config().work_mem_bytes);
    if (!query_guard.acquired()) {
        return String("Error: server busy: admission timeout.\n");
    }
    ExecRwGuard guard(exec_latch_, is_write_statement_type(stmt.type) ||
                                   needs_schema_execution_lock(stmt));

    char buf[512];

    if (stmt.type == StmtType::kSelect && !stmt.select) {
        return String("Error: unsupported or unrecognized command.\n");
    }

    if (stmt.type == StmtType::kBegin) {
        Transaction* txn = db_.txn_manager().begin();
        if (txn) {
            snprintf(buf, sizeof(buf), "Transaction %lu started.\n",
                     static_cast<unsigned long>(txn->id()));
            result = String(buf);
        } else {
            result = String("Error: transaction already active.\n");
        }
        return result;
    }
    if (stmt.type == StmtType::kCommit) {
        Transaction* txn = db_.txn_manager().current();
        if (txn && db_.txn_manager().commit(txn))
            result = String("Transaction committed.\n");
        else
            result = String("Error: no active transaction.\n");
        return result;
    }
    if (stmt.type == StmtType::kRollback) {
        Transaction* txn = db_.txn_manager().current();
        if (txn && db_.txn_manager().rollback(txn))
            result = String("Transaction rolled back.\n");
        else
            result = String("Error: no active transaction.\n");
        return result;
    }

    if (stmt.type == StmtType::kShowTables) {
        result = String("Tables:\n");
        struct Ctx { String* out; int count; } ctx = {&result, 0};
        auto cb = [](TableEntry& e, void* c) {
            auto* ctx = static_cast<Ctx*>(c);
            *ctx->out += "  ";
            *ctx->out += e.table_name;
            *ctx->out += "\n";
            ctx->count++;
        };
        db_.catalog().for_each_table(cb, &ctx);
        if (ctx.count == 0) result += "  (empty)\n";
        return result;
    }

    if (stmt.type == StmtType::kAnalyze) {
        TableEntry* table = db_.get_table(stmt.analyze_table_name);
        if (!table) {
            snprintf(buf, sizeof(buf), "Error: table '%s' not found.\n",
                     stmt.analyze_table_name.c_str());
            return String(buf);
        }
        db_.collect_statistics(table->table_id);
        clear_prepared_cache();
        return String("ANALYZE\n");
    }

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
            clear_prepared_cache();
            snprintf(buf, sizeof(buf), "Table '%s' created.\n", stmt.create_table->table_name.c_str());
        } else {
            snprintf(buf, sizeof(buf), "Error: failed to create table '%s'.\n",
                     stmt.create_table->table_name.c_str());
        }
        result = String(buf);
        return result;
    }

    if (stmt.type == StmtType::kDropTable) {
        if (db_.drop_table(stmt.drop_table_name)) {
            clear_prepared_cache();
            snprintf(buf, sizeof(buf), "Table '%s' dropped.\n", stmt.drop_table_name.c_str());
        } else {
            snprintf(buf, sizeof(buf), "Error: table '%s' not found.\n", stmt.drop_table_name.c_str());
        }
        result = String(buf);
        return result;
    }

    // W22: ALTER TABLE
    if (stmt.type == StmtType::kAlterTable && stmt.alter_table) {
        auto* alt = stmt.alter_table.get();
        TableEntry* table = db_.get_table(alt->table_name);
        if (!table) {
            snprintf(buf, sizeof(buf), "Error: table '%s' not found.\n", alt->table_name.c_str());
            return String(buf);
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
                clear_prepared_cache();
                snprintf(buf, sizeof(buf), "Column '%s' added to table '%s'.\n",
                         alt->new_column.name.c_str(), alt->table_name.c_str());
                break;
            }
            case AlterType::kDropColumn: {
                int idx = table->schema.get_column_index(alt->drop_column_name);
                if (idx < 0) {
                    snprintf(buf, sizeof(buf), "Error: column '%s' not found.\n",
                             alt->drop_column_name.c_str());
                } else {
                    table->schema.remove_column(static_cast<u32>(idx));
                    db_.save_catalog();
                    clear_prepared_cache();
                    snprintf(buf, sizeof(buf), "Column '%s' dropped from table '%s'.\n",
                             alt->drop_column_name.c_str(), alt->table_name.c_str());
                }
                break;
            }
            case AlterType::kRenameColumn: {
                int idx = table->schema.get_column_index(alt->rename_from);
                if (idx < 0) {
                    snprintf(buf, sizeof(buf), "Error: column '%s' not found.\n",
                             alt->rename_from.c_str());
                } else {
                    table->schema.rename_column(static_cast<u32>(idx), alt->rename_to);
                    db_.save_catalog();
                    clear_prepared_cache();
                    snprintf(buf, sizeof(buf), "Column '%s' renamed to '%s'.\n",
                             alt->rename_from.c_str(), alt->rename_to.c_str());
                }
                break;
            }
        }
        return String(buf);
    }

    if (stmt.type == StmtType::kCreateIndex && stmt.create_index) {
        if (db_.create_index(stmt.create_index->index_name,
                             stmt.create_index->table_name,
                             stmt.create_index->columns,
                             stmt.create_index->unique)) {
            clear_prepared_cache();
            snprintf(buf, sizeof(buf), "Index '%s' created.\n", stmt.create_index->index_name.c_str());
        } else {
            snprintf(buf, sizeof(buf), "Error: failed to create index '%s'.\n",
                     stmt.create_index->index_name.c_str());
        }
        result = String(buf);
        return result;
    }

    if (stmt.type == StmtType::kDropIndex) {
        if (db_.drop_index(stmt.drop_index_name)) {
            clear_prepared_cache();
            snprintf(buf, sizeof(buf), "Index '%s' dropped.\n", stmt.drop_index_name.c_str());
        } else {
            snprintf(buf, sizeof(buf), "Error: index '%s' not found.\n",
                     stmt.drop_index_name.c_str());
        }
        result = String(buf);
        return result;
    }

    if (stmt.type == StmtType::kDescTable && stmt.desc_table) {
        TableEntry* te = db_.get_table(stmt.desc_table->table_name);
        if (!te) {
            snprintf(buf, sizeof(buf), "Error: table '%s' not found.\n",
                     stmt.desc_table->table_name.c_str());
            return String(buf);
        }
        result = String("Table: ") + te->table_name + "\n";
        snprintf(buf, sizeof(buf), "%-4s %-20s %-10s %-8s %-8s %-8s\n",
                 "Col", "Name", "Type", "NotNull", "Primary", "Unique");
        result += buf;
        for (u32 i = 0; i < te->schema.column_count(); i++) {
            const Column& c = te->schema.get_column(i);
            const char* type_str = "?";
            switch (c.type) {
                case TypeId::kBool: type_str = "BOOL"; break;
                case TypeId::kInt32: type_str = "INT"; break;
                case TypeId::kInt64: type_str = "BIGINT"; break;
                case TypeId::kFloat: type_str = "FLOAT"; break;
                case TypeId::kDouble: type_str = "DOUBLE"; break;
                case TypeId::kVarchar: type_str = "VARCHAR"; break;
                default: type_str = "UNKNOWN"; break;
            }
            snprintf(buf, sizeof(buf), "%-4u %-20s %-10s %-8s %-8s %-8s\n",
                     i, c.name.c_str(), type_str,
                     c.not_null ? "YES" : "NO", c.is_primary ? "YES" : "NO",
                     c.is_unique ? "YES" : "NO");
            result += buf;
        }
        return result;
    }

    if (stmt.type == StmtType::kExplain && stmt.explain_stmt) {
        Planner planner(&db_.catalog(), optimizer_config_from_db(db_));
        UniquePtr<PlanNode> plan = planner.plan(*stmt.explain_stmt);
        if (!plan) return String("Error: failed to build plan.\n");
        snprintf(buf, sizeof(buf), "Plan: cost=%.2f..%.2f rows=%.0f\n",
                 plan->startup_cost, plan->total_cost, plan->plan_rows);
        result = String(buf);
        switch (plan->type) {
            case PlanNodeType::kSeqScan: result += "  SeqScan\n"; break;
            case PlanNodeType::kIndexScan: result += "  IndexScan\n"; break;
            case PlanNodeType::kIndexOnlyScan: result += "  IndexOnlyScan\n"; break;
            case PlanNodeType::kFilter: result += "  Filter\n"; break;
            case PlanNodeType::kProject: result += "  Project\n"; break;
            case PlanNodeType::kJoin: result += "  Join\n"; break;
            case PlanNodeType::kSort: result += "  Sort\n"; break;
            case PlanNodeType::kAggregate: result += "  Aggregate\n"; break;
            case PlanNodeType::kInsert: result += "  Insert\n"; break;
            case PlanNodeType::kDelete: result += "  Delete\n"; break;
            case PlanNodeType::kUpdate: result += "  Update\n"; break;
            default: result += "  Unknown\n"; break;
        }
        return result;
    }

    Planner planner(&db_.catalog(), optimizer_config_from_db(db_));
    UniquePtr<PlanNode> plan = planner.plan(stmt);
    if (!plan) {
        return String("Error: failed to build plan.\n");
    }
    return execute_plan_result(stmt.type, plan.get());
}

u64 Server::execute_sql_streaming(const String& sql, int fd) {
    u64 timeout_ms = 0;
    auto send_str = [fd](const String& s) { send(fd, s.c_str(), s.size(), 0); };
    if (parse_statement_timeout(sql, &timeout_ms)) {
        g_statement_timeout_ms = timeout_ms;
        send_str("SET\n");
        return 0;
    }
    String prepared_name;
    String prepared_body;
    if (parse_prepare(sql, &prepared_name, &prepared_body)) {
        auto entry = std::make_unique<PreparedEntry>();
        entry->sql = prepared_body;
        Parser prepared_parser(prepared_body);
        Statement prepared_stmt = prepared_parser.parse();
        if (is_plannable_statement_type(prepared_stmt.type)) {
            Planner planner(&db_.catalog(), optimizer_config_from_db(db_));
            entry->type = prepared_stmt.type;
            entry->plan = planner.plan(prepared_stmt);
            if (!entry->plan) {
                send_str("Error: failed to build plan.\n");
                return 0;
            }
        } else {
            entry->type = prepared_stmt.type;
        }
        LockGuard guard(g_prepared_latch);
        g_prepared_cache[std::string(prepared_name.c_str())] = std::move(entry);
        send_str("PREPARE\n");
        return 0;
    }
    if (parse_execute_or_deallocate(sql, "EXECUTE", &prepared_name)) {
        String body;
        {
            LockGuard guard(g_prepared_latch);
            auto it = g_prepared_cache.find(std::string(prepared_name.c_str()));
            if (it == g_prepared_cache.end()) {
                send_str("Error: prepared statement not found.\n");
                return 0;
            }
            if (it->second->plan) {
                QueryResourceGuard query_guard(db_.resources(), is_write_statement_type(it->second->type),
                                               db_.config().work_mem_bytes);
                if (!query_guard.acquired()) {
                    send_str("Error: server busy: admission timeout.\n");
                    return 0;
                }
                String out = execute_plan_result(it->second->type, it->second->plan.get());
                send_str(out);
                return 0;
            }
            body = it->second->sql;
        }
        return execute_sql_streaming(body, fd);
    }
    if (parse_execute_or_deallocate(sql, "DEALLOCATE", &prepared_name)) {
        LockGuard guard(g_prepared_latch);
        g_prepared_cache.erase(std::string(prepared_name.c_str()));
        send_str("DEALLOCATE\n");
        return 0;
    }

    String normalized = upper_sql(trim_sql(sql));
    if (normalized == "SHOW CONFIG") {
        send_str(DbConfigLoader::describe(db_.config()) + "\n");
        return 0;
    }
    if (normalized == "SHOW STATS") {
        send_str(db_.stats_summary() + "\n");
        return 0;
    }

    Parser parser(sql);
    Statement stmt = parser.parse();
    QueryResourceGuard query_guard(db_.resources(), is_write_statement_type(stmt.type),
                                   db_.config().work_mem_bytes);
    if (!query_guard.acquired()) {
        send_str("Error: server busy: admission timeout.\n");
        return 0;
    }
    ExecRwGuard guard(exec_latch_, is_write_statement_type(stmt.type) ||
                                   needs_schema_execution_lock(stmt));

    char buf[512];

    // Handle non-DML/SELECT statements (direct response, bypass Executor)

    if (stmt.type == StmtType::kBegin) {
        Transaction* txn = db_.txn_manager().begin();
        if (txn) { snprintf(buf, sizeof(buf), "Transaction %lu started.\n", (unsigned long)txn->id()); send_str(buf); }
        else send_str("Error: transaction already active.\n");
        return 0;
    }
    if (stmt.type == StmtType::kCommit) {
        Transaction* txn = db_.txn_manager().current();
        if (txn && db_.txn_manager().commit(txn)) send_str("Transaction committed.\n");
        else send_str("Error: no active transaction.\n");
        return 0;
    }
    if (stmt.type == StmtType::kRollback) {
        Transaction* txn = db_.txn_manager().current();
        if (txn && db_.txn_manager().rollback(txn)) send_str("Transaction rolled back.\n");
        else send_str("Error: no active transaction.\n");
        return 0;
    }
    if (stmt.type == StmtType::kShowTables) {
        String result = "Tables:\n";
        struct Ctx { String* out; int count; } ctx = {&result, 0};
        auto cb = [](TableEntry& e, void* c) { auto* x = static_cast<Ctx*>(c); *x->out += "  "; *x->out += e.table_name; *x->out += "\n"; x->count++; };
        db_.catalog().for_each_table(cb, &ctx);
        if (ctx.count == 0) result += "  (empty)\n";
        send_str(result);
        return 0;
    }
    if (stmt.type == StmtType::kAnalyze) {
        TableEntry* table = db_.get_table(stmt.analyze_table_name);
        if (!table) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Error: table '%s' not found.\n",
                     stmt.analyze_table_name.c_str());
            send_str(buf);
            return 0;
        }
        db_.collect_statistics(table->table_id);
        clear_prepared_cache();
        send_str("ANALYZE\n");
        return 0;
    }
    if (stmt.type == StmtType::kCreateTable && stmt.create_table) {
        Schema schema;
        for (u32 i = 0; i < stmt.create_table->columns.size(); i++) {
            const auto& col = stmt.create_table->columns[i];
            Column c; c.name = col.name; c.not_null = col.not_null; c.is_primary = col.is_primary; c.is_unique = col.is_unique;
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
        if (db_.create_table(stmt.create_table->table_name, schema))
            snprintf(buf, sizeof(buf), "Table '%s' created.\n", stmt.create_table->table_name.c_str());
        else
            snprintf(buf, sizeof(buf), "Error: failed to create table '%s'.\n", stmt.create_table->table_name.c_str());
        send_str(buf); return 0;
    }
    if (stmt.type == StmtType::kDropTable) {
        if (db_.drop_table(stmt.drop_table_name)) snprintf(buf, sizeof(buf), "Table '%s' dropped.\n", stmt.drop_table_name.c_str());
        else snprintf(buf, sizeof(buf), "Error: table '%s' not found.\n", stmt.drop_table_name.c_str());
        send_str(buf); return 0;
    }
    if (stmt.type == StmtType::kCreateIndex && stmt.create_index) {
        if (db_.create_index(stmt.create_index->index_name, stmt.create_index->table_name, stmt.create_index->columns, stmt.create_index->unique))
            snprintf(buf, sizeof(buf), "Index '%s' created.\n", stmt.create_index->index_name.c_str());
        else snprintf(buf, sizeof(buf), "Error: failed to create index '%s'.\n", stmt.create_index->index_name.c_str());
        send_str(buf); return 0;
    }
    if (stmt.type == StmtType::kDropIndex) {
        if (db_.drop_index(stmt.drop_index_name)) snprintf(buf, sizeof(buf), "Index '%s' dropped.\n", stmt.drop_index_name.c_str());
        else snprintf(buf, sizeof(buf), "Error: index '%s' not found.\n", stmt.drop_index_name.c_str());
        send_str(buf); return 0;
    }
    // W22: ALTER TABLE (streaming path)
    if (stmt.type == StmtType::kAlterTable && stmt.alter_table) {
        auto* alt = stmt.alter_table.get();
        TableEntry* table = db_.get_table(alt->table_name);
        if (!table) {
            snprintf(buf, sizeof(buf), "Error: table '%s' not found.\n", alt->table_name.c_str());
            send_str(buf); return 0;
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
                snprintf(buf, sizeof(buf), "Column '%s' added.\n", alt->new_column.name.c_str());
                break;
            }
            case AlterType::kDropColumn: {
                int idx = table->schema.get_column_index(alt->drop_column_name);
                if (idx < 0) {
                    snprintf(buf, sizeof(buf), "Error: column '%s' not found.\n", alt->drop_column_name.c_str());
                } else {
                    table->schema.remove_column(static_cast<u32>(idx));
                    db_.save_catalog();
                    snprintf(buf, sizeof(buf), "Column '%s' dropped.\n", alt->drop_column_name.c_str());
                }
                break;
            }
            case AlterType::kRenameColumn: {
                int idx = table->schema.get_column_index(alt->rename_from);
                if (idx < 0) {
                    snprintf(buf, sizeof(buf), "Error: column '%s' not found.\n", alt->rename_from.c_str());
                } else {
                    table->schema.rename_column(static_cast<u32>(idx), alt->rename_to);
                    db_.save_catalog();
                    snprintf(buf, sizeof(buf), "Column '%s' renamed to '%s'.\n", alt->rename_from.c_str(), alt->rename_to.c_str());
                }
                break;
            }
        }
        send_str(buf); return 0;
    }
    if (stmt.type == StmtType::kDescTable && stmt.desc_table) {
        TableEntry* te = db_.get_table(stmt.desc_table->table_name);
        if (!te) { snprintf(buf, sizeof(buf), "Error: table '%s' not found.\n", stmt.desc_table->table_name.c_str()); send_str(buf); return 0; }
        String result = String("Table: ") + te->table_name + "\n";
        snprintf(buf, sizeof(buf), "%-4s %-20s %-10s %-8s %-8s %-8s\n", "Col", "Name", "Type", "NotNull", "Primary", "Unique");
        result += buf;
        for (u32 i = 0; i < te->schema.column_count(); i++) {
            const Column& c = te->schema.get_column(i);
            const char* ts = "?";
            switch (c.type) { case TypeId::kBool: ts="BOOL"; break; case TypeId::kInt32: ts="INT"; break; case TypeId::kInt64: ts="BIGINT"; break; case TypeId::kFloat: ts="FLOAT"; break; case TypeId::kDouble: ts="DOUBLE"; break; case TypeId::kVarchar: ts="VARCHAR"; break; default: ts="UNKNOWN"; break; }
            snprintf(buf, sizeof(buf), "%-4u %-20s %-10s %-8s %-8s %-8s\n", i, c.name.c_str(), ts, c.not_null?"YES":"NO", c.is_primary?"YES":"NO", c.is_unique?"YES":"NO");
            result += buf;
        }
        send_str(result); return 0;
    }
    if (stmt.type == StmtType::kExplain && stmt.explain_stmt) {
        Planner pl(&db_.catalog(), optimizer_config_from_db(db_));
        UniquePtr<PlanNode> p = pl.plan(*stmt.explain_stmt);
        if (!p) { send_str("Error: failed to build plan.\n"); return 0; }
        snprintf(buf, sizeof(buf), "Plan: cost=%.2f..%.2f rows=%.0f\n", p->startup_cost, p->total_cost, p->plan_rows);
        String result = buf;
        switch (p->type) {
            case PlanNodeType::kSeqScan: result += "  SeqScan\n"; break;
            case PlanNodeType::kIndexScan: result += "  IndexScan\n"; break;
            case PlanNodeType::kIndexOnlyScan: result += "  IndexOnlyScan\n"; break;
            case PlanNodeType::kFilter: result += "  Filter\n"; break;
            case PlanNodeType::kProject: result += "  Project\n"; break;
            case PlanNodeType::kJoin: result += "  Join\n"; break;
            case PlanNodeType::kSort: result += "  Sort\n"; break;
            case PlanNodeType::kAggregate: result += "  Aggregate\n"; break;
            case PlanNodeType::kInsert: result += "  Insert\n"; break;
            case PlanNodeType::kDelete: result += "  Delete\n"; break;
            case PlanNodeType::kUpdate: result += "  Update\n"; break;
            default: result += "  Unknown\n"; break;
        }
        send_str(result); return 0;
    }

    // W17: PREPARE — store SQL with name
    if (stmt.type == StmtType::kPrepare && stmt.prepare_stmt) {
        prepared_stmts_[stmt.prepare_stmt->name] = stmt.prepare_stmt->sql;
        snprintf(buf, sizeof(buf), "Statement '%s' prepared.\n", stmt.prepare_stmt->name.c_str());
        send_str(buf); return 0;
    }

    // W17: EXECUTE — substitute params and run
    if (stmt.type == StmtType::kExecute && stmt.execute_stmt) {
        String* sql = prepared_stmts_.find(stmt.execute_stmt->name);
        if (!sql) {
            snprintf(buf, sizeof(buf), "Error: prepared statement '%s' not found.\n",
                     stmt.execute_stmt->name.c_str());
            send_str(buf); return 0;
        }
        // Replace ? placeholders with actual parameter values
        String expanded;
        u32 param_idx = 0;
        for (u32 i = 0; i < sql->size(); i++) {
            if ((*sql)[i] == '?' && param_idx < stmt.execute_stmt->params.size()) {
                // Evaluate the parameter expression
                Tuple dummy;
                Value v = ExpressionEvaluator::evaluate(*stmt.execute_stmt->params[param_idx], dummy);
                expanded += v.to_string();
                param_idx++;
            } else {
                expanded += (*sql)[i];
            }
        }
        // Execute the expanded SQL
        u64 rows = execute_sql_streaming(expanded, fd);
        return rows;
    }

    // W17: DEALLOCATE — remove prepared statement
    if (stmt.type == StmtType::kDeallocate && stmt.deallocate_stmt) {
        if (prepared_stmts_.erase(stmt.deallocate_stmt->name)) {
            snprintf(buf, sizeof(buf), "Statement '%s' deallocated.\n", stmt.deallocate_stmt->name.c_str());
        } else {
            snprintf(buf, sizeof(buf), "Error: prepared statement '%s' not found.\n",
                     stmt.deallocate_stmt->name.c_str());
        }
        send_str(buf); return 0;
    }

    // SELECT / INSERT / UPDATE / DELETE — Streaming path
    bool is_write_stmt = stmt.type == StmtType::kInsert || stmt.type == StmtType::kDelete || stmt.type == StmtType::kUpdate;
    if (stmt.type == StmtType::kSelect && !stmt.select) {
        send_str("Error: unsupported or unrecognized command.\n"); return 0;
    }

    bool implicit_txn = false;
    if (is_write_stmt && !db_.txn_manager().current()) {
        implicit_txn = db_.txn_manager().begin() != nullptr;
        if (!implicit_txn) { send_str("Error: failed to start implicit transaction.\n"); return 0; }
    }

    Planner planner(&db_.catalog(), optimizer_config_from_db(db_));
    UniquePtr<PlanNode> plan = planner.plan(stmt);
    if (!plan) {
        if (implicit_txn) db_.txn_manager().rollback(db_.txn_manager().current());
        send_str("Error: failed to build plan.\n"); return 0;
    }

    ExecutorFactory factory(db_);
    UniquePtr<Executor> exec = factory.create(plan.get());
    if (!exec) {
        if (implicit_txn) db_.txn_manager().rollback(db_.txn_manager().current());
        send_str("Error: failed to create executor.\n"); return 0;
    }

    u64 effective_timeout_ms = g_statement_timeout_ms != 0
                               ? g_statement_timeout_ms
                               : db_.config().statement_timeout_ms;
    clear_executor_error();
    set_executor_deadline_ms(effective_timeout_ms);
    exec->init();
    const Schema& out = exec->output_schema();
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(effective_timeout_ms);
    bool timed_out = false;

    // Column header
    if (out.column_count() > 0) {
        String header;
        for (u32 i = 0; i < out.column_count(); i++) {
            if (i > 0) header += " | ";
            header += out.get_column(i).name;
        }
        header += "\n";
        send(fd, header.c_str(), header.size(), 0);
    }

    // Streaming: write each row directly to socket, stack buffer 8KB
    u64 row_count = 0;
    char row_buf[8192];
    while (true) {
        if (effective_timeout_ms != 0 && std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            break;
        }
        if (db_.config().max_result_rows != 0 && row_count >= db_.config().max_result_rows) {
            send_str("Error: result row limit exceeded.\n");
            break;
        }
        ExecResult r = exec->next();
        if (!r.ok()) break;
        u32 pos = 0;
        for (u32 i = 0; i < out.column_count(); i++) {
            if (i > 0 && pos + 3 < sizeof(row_buf)) { row_buf[pos++] = ' '; row_buf[pos++] = '|'; row_buf[pos++] = ' '; }
            Value v = r.tuple.get_value(i);
            String vs = v.is_null() ? String("NULL") : v.to_string();
            u32 cl = vs.size();
            if (pos + cl >= sizeof(row_buf) - 2) cl = sizeof(row_buf) - pos - 2;
            std::memcpy(row_buf + pos, vs.c_str(), cl); pos += cl;
        }
        if (pos < sizeof(row_buf)) row_buf[pos++] = '\n';
        send(fd, row_buf, pos, 0);
        row_count++;
    }

    if (executor_error()) {
        if (implicit_txn) db_.txn_manager().rollback(db_.txn_manager().current());
        send_str(String("Error: ") + executor_error() + "\n");
        return row_count;
    }

    if (timed_out) {
        if (implicit_txn) db_.txn_manager().rollback(db_.txn_manager().current());
        send_str("Error: statement timeout.\n");
        return row_count;
    }

    if (is_write_stmt) {
        if (implicit_txn) {
            Transaction* txn = db_.txn_manager().current();
            if (!txn || !db_.txn_manager().commit(txn)) send_str("Error: implicit transaction commit failed.\n");
        } else if (!db_.txn_manager().current()) {
            db_.flush();
        }
        db_.maybe_gc();
    }

    return row_count;
}

bool Server::send_response(int client_fd, const String& response) {
    if (db_.config().client_output_buffer_limit_bytes != 0 &&
        response.size() > db_.config().client_output_buffer_limit_bytes) {
        const char* msg = "Error: client output buffer limit exceeded.\n";
        send(client_fd, msg, std::strlen(msg), 0);
        return false;
    }

    const char* data = response.c_str();
    size_t remaining = response.size();
    while (remaining > 0) {
        ssize_t n = send(client_fd, data, remaining, 0);
        if (n <= 0) return false;
        data += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

void Server::handle_client(int client_fd) {
    g_session_cursors.clear();
    ConnectionResourceGuard connection_guard(db_.resources());
    if (!connection_guard.acquired()) {
        const char* msg = "Error: too many connections.\n";
        send(client_fd, msg, std::strlen(msg), 0);
        ::close(client_fd);
        return;
    }
    {
        LockGuard guard(clients_latch_);
        active_clients_.push_back(client_fd);
    }
    const char* welcome = "MiniADB v0.3.0 — Connected.\nType SQL statements ending with ;\n";
    send(client_fd, welcome, strlen(welcome), 0);
    timeval send_timeout;
    send_timeout.tv_sec = 2;
    send_timeout.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

    char buffer[4096];
    String sql_buffer;
    const u32 max_sql_buffer = db_.config().max_sql_size;

    while (true) {
        memset(buffer, 0, sizeof(buffer));
        ssize_t n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        if (n <= 0) break;

        for (ssize_t i = 0; i < n; i++) {
            sql_buffer += buffer[i];
        }

        if (sql_buffer.size() > max_sql_buffer) {
            String err = "Error: SQL statement too long\n";
            send(client_fd, err.c_str(), err.size(), 0);
            sql_buffer = String();
            continue;
        }

        bool in_single_quote = false;
        bool in_double_quote = false;
        bool found_semi = false;
        for (u32 i = 0; i < sql_buffer.size(); i++) {
            char c = sql_buffer[i];
            if (c == '\'' && !in_double_quote) {
                in_single_quote = !in_single_quote;
            } else if (c == '"' && !in_single_quote) {
                in_double_quote = !in_double_quote;
            } else if (c == ';' && !in_single_quote && !in_double_quote) {
                found_semi = true;
                break;
            }
        }

        if (found_semi) {
            u32 start = 0;
            while (start < sql_buffer.size()) {
                u32 semi_pos = sql_buffer.size();
                in_single_quote = false;
                in_double_quote = false;
                for (u32 i = start; i < sql_buffer.size(); i++) {
                    char c = sql_buffer[i];
                    if (c == '\'' && !in_double_quote) {
                        in_single_quote = !in_single_quote;
                    } else if (c == '"' && !in_single_quote) {
                        in_double_quote = !in_double_quote;
                    } else if (c == ';' && !in_single_quote && !in_double_quote) {
                        semi_pos = i;
                        break;
                    }
                }
                if (semi_pos >= sql_buffer.size()) break;

                String stmt = sql_buffer.substr(start, semi_pos - start + 1);
                u32 s = 0, e = stmt.size();
                while (s < e && (stmt[s] == ' ' || stmt[s] == '\t' || stmt[s] == '\n' || stmt[s] == '\r')) s++;
                while (e > s && (stmt[e-1] == ' ' || stmt[e-1] == '\t' || stmt[e-1] == '\n' || stmt[e-1] == '\r')) e--;
                if (e > s) {
                    String trimmed = stmt.substr(s, e - s);
                    try {
                        String response = execute_sql(trimmed);
                        if (!send_response(client_fd, response)) {
                            start = sql_buffer.size();
                            break;
                        }
                    } catch (const std::exception& ex) {
                        String err = String("Error: ") + ex.what() + "\n";
                        send(client_fd, err.c_str(), err.size(), 0);
                    } catch (...) {
                        String err = "Error: internal error\n";
                        send(client_fd, err.c_str(), err.size(), 0);
                    }
                }
                start = semi_pos + 1;
            }
            if (start < sql_buffer.size()) {
                sql_buffer = sql_buffer.substr(start);
            } else {
                sql_buffer = String();
            }
        }
    }

    ::close(client_fd);
    g_session_cursors.clear();
    {
        LockGuard guard(clients_latch_);
        active_clients_.erase(std::remove(active_clients_.begin(), active_clients_.end(), client_fd),
                              active_clients_.end());
    }
}

bool Server::enqueue_client(int client_fd) {
    std::lock_guard<std::mutex> guard(queue_mutex_);
    u32 limit = db_.config().admission_queue_size == 0 ? 1024 : db_.config().admission_queue_size;
    if (pending_clients_.size() >= limit) {
        return false;
    }
    pending_clients_.push(client_fd);
    queue_cv_.notify_one();
    return true;
}

void Server::worker_loop() {
    while (true) {
        int client_fd = -1;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return !running_ || !pending_clients_.empty();
            });
            if (!running_ && pending_clients_.empty()) return;
            client_fd = pending_clients_.front();
            pending_clients_.pop();
        }
        handle_client(client_fd);
    }
}

void Server::start() {
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) { printf("Error: cannot create socket\n"); return; }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("Error: cannot bind to port %u\n", port_);
        ::close(server_fd_);
        return;
    }

    if (listen(server_fd_, SOMAXCONN) < 0) {
        printf("Error: cannot listen\n");
        ::close(server_fd_);
        return;
    }

    running_ = true;
    printf("MiniADB Server listening on port %u\n", port_);
    printf("Connect with: nc localhost %u\n", port_);

    signal(SIGPIPE, SIG_IGN);

    u32 worker_count = db_.config().query_workers == 0 ? 1 : db_.config().query_workers;
    client_workers_.reserve(worker_count);
    for (u32 i = 0; i < worker_count; i++) {
        client_workers_.emplace_back(&Server::worker_loop, this);
    }

    while (running_) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) continue;
        if (!running_) {
            ::close(client_fd);
            break;
        }

        printf("Client connected: %s:%d\n",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        if (!enqueue_client(client_fd)) {
            const char* msg = "Error: server busy: connection queue full.\n";
            send(client_fd, msg, std::strlen(msg), 0);
            ::close(client_fd);
        }
    }
}

} // namespace minidb
