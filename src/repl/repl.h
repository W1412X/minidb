/**
 * @file repl.h
 * @brief Interactive SQL REPL
 */
#pragma once

#include "database/database.h"
#include "record/tuple.h"

namespace minidb {

struct PlanNode;

class REPL {
public:
    explicit REPL(Database& db);
    void run();

private:
    void execute_sql(const String& sql);
    void print_tuple(const Tuple& tuple, const Schema& schema);
    void print_plan(const PlanNode* plan, int indent = 0);

    // True when stdin is a terminal. Drives interactive niceties (prompts,
    // banners, aligned output, timing). Pipe/script input gets the legacy
    // format so existing shell tests are unaffected.
    bool interactive_ = false;
    // \timing toggle (psql-style). Always false in non-interactive mode.
    bool show_timing_ = false;

    Database& db_;
};

} // namespace minidb
