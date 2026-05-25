/**
 * @file update.h
 * @brief Update Executor
 */
#pragma once

#include "sql/executor/executor.h"
#include "container/unique_ptr.h"
#include "container/vector.h"
#include "container/utility.h"
#include "storage/buffer_pool.h"
#include "storage/heap_file.h"
#include "record/schema.h"
#include "record/value.h"
#include "index/btree.h"

namespace minidb {

struct Expression;
class TransactionManager;
class WalManager;
class Catalog;
class Database;

class UpdateExecutor : public Executor {
public:
    UpdateExecutor(BufferPool* pool, HeapFile* heap, const Schema& schema,
                   Vector<Pair<String, UniquePtr<Expression>>> set_clauses,
                   UniquePtr<Executor> child,
                   TransactionManager* txn_mgr = nullptr, u32 table_id = 0,
                   WalManager* wal = nullptr, Catalog* catalog = nullptr,
                   Database* db = nullptr);
    void init() override;
    ExecResult next() override;
    const Schema& output_schema() const override;

private:
    bool row_satisfies_schema(const Vector<Value>& row) const;
    bool violates_unique_constraints(const Vector<Value>& row, const RecordId& self_rid,
                                     const Vector<Vector<u32>>& unique_groups) const;

    BufferPool* pool_;
    HeapFile* heap_;
    Schema schema_;
    Vector<Pair<String, UniquePtr<Expression>>> set_clauses_;
    UniquePtr<Executor> child_;
    Schema result_schema_;
    bool executed_;
    TransactionManager* txn_mgr_;
    u32 table_id_;
    WalManager* wal_;
    Catalog* catalog_;
    Database* db_;
};

} // namespace minidb
