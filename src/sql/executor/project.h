#pragma once

#include "sql/executor/executor.h"
#include "container/vector.h"
#include "container/unique_ptr.h"
#include "record/schema.h"

namespace minidb {

struct Expression;

class ProjectExecutor : public Executor {
public:
    ProjectExecutor(UniquePtr<Executor> child, const Schema& output_schema,
                    const Vector<u32>& col_indices,
                    const Vector<UniquePtr<Expression>>& expressions);
    void init() override;
    ExecResult next() override;
    const Schema& output_schema() const override;
    bool last_record_id(RecordId* rid) const override;

    Executor* child() const { return child_.get(); }

private:
    UniquePtr<Executor> child_;
    Schema output_schema_;
    Vector<u32> col_indices_;
    Vector<UniquePtr<Expression>> expressions_;
};

} // namespace minidb
