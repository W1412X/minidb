/**
 * @file ast.h
 * @brief AST node definitions — parsed SQL tree.
 */
#pragma once

#include "common/defs.h"
#include "container/string.h"
#include "container/vector.h"
#include "container/unique_ptr.h"
#include "record/value.h"
#include "sql/planner/plan_node.h"

namespace minidb {

// ============================================================
// Expressions.
// ============================================================

enum class ExprType {
    kLiteral, kColumnRef, kBinaryOp, kUnaryOp, kStar, kSubquery, kCase, kCast,
};

struct Expression {
    ExprType type;

    // COLUMN_REF
    String table_name;
    String column_name;

    // LITERAL
    Value literal_value;

    // SELECT-list alias (used by planner for output column naming)
    String alias;

    // BINARY_OP
    String op;
    UniquePtr<Expression> left;
    UniquePtr<Expression> right;

    // UNARY_OP
    UniquePtr<Expression> child;

    // SUBQUERY
    UniquePtr<struct SelectStmt> subquery;

    // CASE WHEN ... ... THEN ... ELSE ... END
    Vector<Pair<UniquePtr<Expression>, UniquePtr<Expression>>> when_clauses;
    UniquePtr<Expression> else_expr;

    // CAST(expr AS type)  — child holds the source expression
    TypeId cast_target;   // target type for kCast

    Expression() : type(ExprType::kLiteral), cast_target(TypeId::kNull) {}
    Expression* clone() const;
};

// ============================================================
// Table reference.
// ============================================================

struct TableRef {
    String name;
    String alias;
    UniquePtr<struct SelectStmt> subquery;

    TableRef() = default;
    TableRef(const TableRef& other);
    TableRef& operator=(const TableRef& other);
    TableRef(TableRef&&) noexcept = default;
    TableRef& operator=(TableRef&&) noexcept = default;
};

// ============================================================
// JOIN
// ============================================================

struct JoinClause {
    JoinType type;
    TableRef table;
    UniquePtr<Expression> on_condition;

    JoinClause() : type(JoinType::kInner) {}
};

// ============================================================
// Statement type.
// ============================================================

enum class StmtType {
    kSelect, kInsert, kUpdate, kDelete,
    kCreateTable, kDropTable, kCreateIndex, kDropIndex,
    kShowTables, kDescTable,
    kBegin, kCommit, kRollback,
    kExplain,
    kAlterTable,
    kPrepare, kExecute, kDeallocate,
    kAnalyze,
    kVacuum,
};

// ============================================================
// SELECT
// ============================================================

struct OrderByItem {
    UniquePtr<Expression> expression;
    bool ascending;
    bool nulls_first;   // PostgreSQL default: NULLS FIRST for DESC, NULLS LAST for ASC
    OrderByItem() : ascending(true), nulls_first(true) {}
};

struct SelectStmt {
    Vector<UniquePtr<Expression>> select_list;
    Vector<TableRef> from_tables;
    Vector<JoinClause> joins;
    UniquePtr<Expression> where_clause;
    Vector<UniquePtr<Expression>> group_by;
    UniquePtr<Expression> having;
    Vector<OrderByItem> order_by;
    i32 limit;
    i32 offset;
    bool distinct;
    bool union_all;
    UniquePtr<SelectStmt> union_rhs;

    SelectStmt() : limit(-1), offset(-1), distinct(false), union_all(false) {}

    SelectStmt* clone() const {
        auto* s = new SelectStmt();
        for (u32 i = 0; i < select_list.size(); i++) {
            s->select_list.push_back(UniquePtr<Expression>(select_list[i]->clone()));
        }
        for (u32 i = 0; i < from_tables.size(); i++) {
            s->from_tables.push_back(from_tables[i]);
        }
        for (u32 i = 0; i < joins.size(); i++) {
            JoinClause j;
            j.type = joins[i].type;
            j.table = joins[i].table;
            if (joins[i].on_condition) j.on_condition = UniquePtr<Expression>(joins[i].on_condition->clone());
            s->joins.push_back(static_cast<JoinClause&&>(j));
        }
        if (where_clause) s->where_clause = UniquePtr<Expression>(where_clause->clone());
        for (u32 i = 0; i < group_by.size(); i++) {
            s->group_by.push_back(UniquePtr<Expression>(group_by[i]->clone()));
        }
        if (having) s->having = UniquePtr<Expression>(having->clone());
        for (u32 i = 0; i < order_by.size(); i++) {
            OrderByItem item;
            item.expression = UniquePtr<Expression>(order_by[i].expression->clone());
            item.ascending = order_by[i].ascending;
            item.nulls_first = order_by[i].nulls_first;
            s->order_by.push_back(static_cast<OrderByItem&&>(item));
        }
        s->limit = limit;
        s->offset = offset;
        s->distinct = distinct;
        s->union_all = union_all;
        if (union_rhs) s->union_rhs = UniquePtr<SelectStmt>(union_rhs->clone());
        return s;
    }
};

inline TableRef::TableRef(const TableRef& other) : name(other.name), alias(other.alias) {
    if (other.subquery) subquery = UniquePtr<SelectStmt>(other.subquery->clone());
}

inline TableRef& TableRef::operator=(const TableRef& other) {
    if (this == &other) return *this;
    name = other.name;
    alias = other.alias;
    subquery = other.subquery
        ? UniquePtr<SelectStmt>(other.subquery->clone())
        : UniquePtr<SelectStmt>();
    return *this;
}

// ============================================================
// INSERT
// ============================================================

struct InsertStmt {
    TableRef table;
    Vector<String> columns;
    Vector<Vector<UniquePtr<Expression>>> values_list;
};

// ============================================================
// UPDATE
// ============================================================

struct UpdateStmt {
    TableRef table;
    Vector<Pair<String, UniquePtr<Expression>>> set_clauses;
    UniquePtr<Expression> where_clause;
};

// ============================================================
// DELETE
// ============================================================

struct DeleteStmt {
    TableRef table;
    UniquePtr<Expression> where_clause;
};

// ============================================================
// CREATE TABLE
// ============================================================

struct ColumnDef {
    String name;
    String type_name;
    bool not_null;
    bool is_primary;
    bool is_unique;
    i32 varchar_length;
    String default_value;
    // CHECK expression source text (without the surrounding parens) when
    // the column declares `CHECK (...)`. Empty otherwise.
    String check_expr;

    ColumnDef() : not_null(false), is_primary(false), is_unique(false), varchar_length(-1) {}
};

struct CreateTableStmt {
    String table_name;
    Vector<ColumnDef> columns;
};

struct CreateIndexStmt {
    String index_name;
    String table_name;
    Vector<String> columns;
    bool unique;
    CreateIndexStmt() : unique(false) {}
};

struct DescTableStmt {
    String table_name;
};

// ============================================================
// ALTER TABLE
// ============================================================

enum class AlterType { kAddColumn, kDropColumn, kRenameColumn };

struct AlterTableStmt {
    String table_name;
    AlterType alter_type;
    ColumnDef new_column;       // ADD COLUMN
    String drop_column_name;    // DROP COLUMN
    String rename_from;         // RENAME COLUMN
    String rename_to;           // RENAME COLUMN
};

struct PrepareStmt {
    String name;
    String sql;
};

struct ExecuteStmt {
    String name;
    Vector<UniquePtr<Expression>> params;
};

struct DeallocateStmt {
    String name;
};

// ============================================================
// Top-level statement.
// ============================================================

struct Statement {
    StmtType type;
    UniquePtr<SelectStmt> select;
    UniquePtr<InsertStmt> insert;
    UniquePtr<UpdateStmt> update;
    UniquePtr<DeleteStmt> delete_stmt;
    UniquePtr<CreateTableStmt> create_table;
    UniquePtr<CreateIndexStmt> create_index;
    UniquePtr<DescTableStmt> desc_table;
    UniquePtr<AlterTableStmt> alter_table;
    UniquePtr<PrepareStmt> prepare_stmt;
    UniquePtr<ExecuteStmt> execute_stmt;
    UniquePtr<DeallocateStmt> deallocate_stmt;
    String drop_table_name;
    String drop_index_name;
    String analyze_table_name;
    String vacuum_table_name;
    UniquePtr<Statement> explain_stmt;
    bool explain_analyze;
    double join_hint;  // >0 favors hash join, <0 favors nested loop, 0 = no hint

    Statement() : type(StmtType::kSelect), explain_analyze(false), join_hint(0.0) {}
};

inline Expression* Expression::clone() const {
    auto c = new Expression();
    c->type = type;
    c->table_name = table_name;
    c->column_name = column_name;
    c->literal_value = literal_value;
    c->alias = alias;
    c->op = op;
    if (left) c->left = UniquePtr<Expression>(left->clone());
    if (right) c->right = UniquePtr<Expression>(right->clone());
    if (child) c->child = UniquePtr<Expression>(child->clone());
    if (subquery) c->subquery = UniquePtr<SelectStmt>(subquery->clone());
    for (u32 i = 0; i < when_clauses.size(); i++) {
        Pair<UniquePtr<Expression>, UniquePtr<Expression>> clause;
        if (when_clauses[i].first) clause.first = UniquePtr<Expression>(when_clauses[i].first->clone());
        if (when_clauses[i].second) clause.second = UniquePtr<Expression>(when_clauses[i].second->clone());
        c->when_clauses.push_back(static_cast<Pair<UniquePtr<Expression>, UniquePtr<Expression>>&&>(clause));
    }
    if (else_expr) c->else_expr = UniquePtr<Expression>(else_expr->clone());
    c->cast_target = cast_target;
    return c;
}

} // namespace minidb
