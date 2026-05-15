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

    Database& db_;
};

} // namespace minidb
