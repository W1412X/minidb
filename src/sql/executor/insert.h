/**
 * @file insert.h / .cpp
 * @brief Insert Executor
 */
#pragma once

#include "sql/executor/executor.h"
#include "storage/buffer_pool.h"
#include "storage/heap_file.h"

namespace minidb {

class TransactionManager;
class WalManager;
class Catalog;
class Database;

class InsertExecutor : public Executor {
public:
    InsertExecutor(BufferPool* pool, HeapFile* heap, const Schema& schema,
                   const Vector<Vector<Value>>& values,
                   TransactionManager* txn_mgr = nullptr, u32 table_id = 0,
                   WalManager* wal = nullptr, Catalog* catalog = nullptr,
                   Database* db = nullptr);
    void init() override;
    ExecResult next() override;
    const Schema& output_schema() const override;

private:
    bool row_satisfies_schema(const Vector<Value>& row) const;
    bool violates_unique_constraints(const Vector<Value>& row) const;

    BufferPool* pool_;
    HeapFile* heap_;
    Schema schema_;
    Vector<Vector<Value>> values_;
    Schema result_schema_;
    bool executed_;
    TransactionManager* txn_mgr_;
    u32 table_id_;
    WalManager* wal_;
    Catalog* catalog_;
    Database* db_;
};

} // namespace minidb
