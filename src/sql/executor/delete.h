/**
 * @file delete.h
 */
#pragma once

#include "sql/executor/executor.h"
#include "container/unique_ptr.h"
#include "storage/buffer_pool.h"
#include "storage/heap_file.h"

namespace minidb {

class TransactionManager;
class WalManager;
class Database;

class DeleteExecutor : public Executor {
public:
    DeleteExecutor(BufferPool* pool, HeapFile* heap, UniquePtr<Executor> child,
                   TransactionManager* txn_mgr = nullptr, u32 table_id = 0,
                   WalManager* wal = nullptr, Database* db = nullptr);
    void init() override;
    ExecResult next() override;
    const Schema& output_schema() const override;

private:
    HeapFile* heap_;
    UniquePtr<Executor> child_;
    Schema result_schema_;
    bool executed_;
    TransactionManager* txn_mgr_;
    u32 table_id_;
    WalManager* wal_;
    Database* db_;
};

} // namespace minidb
