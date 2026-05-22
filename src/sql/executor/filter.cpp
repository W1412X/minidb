#include "sql/executor/filter.h"
#include "sql/executor/expression_evaluator.h"
#include "sql/parser/ast.h"

namespace minidb {

FilterExecutor::FilterExecutor(UniquePtr<Executor> child, UniquePtr<Expression> predicate)
    : child_(static_cast<UniquePtr<Executor>&&>(child)),
      predicate_(static_cast<UniquePtr<Expression>&&>(predicate)) {}

void FilterExecutor::init() {
    child_->init();
    compiled_nodes_.clear();
    compiled_root_ = compile_expr(predicate_.get(), child_->output_schema());
    if (compiled_root_ < 0) compiled_nodes_.clear();
    // Allocate the eval stack once. eval_compiled() previously did
    // `std::vector<Value>(N)` on every row — a heap alloc + default-init
    // pass per evaluation. With this we reuse the same buffer and only
    // pay one allocation per scan.
    eval_stack_.assign(compiled_nodes_.size(), Value());
}

int FilterExecutor::compile_expr(const Expression* expr, const Schema& schema) {
    if (!expr) return -1;
    if (expr->type == ExprType::kLiteral) {
        CompiledNode node;
        node.kind = CompiledNode::Kind::kLiteral;
        node.literal = expr->literal_value;
        compiled_nodes_.push_back(node);
        return static_cast<int>(compiled_nodes_.size() - 1);
    }
    if (expr->type == ExprType::kColumnRef) {
        int idx = expr->table_name.empty()
            ? schema.get_column_index(expr->column_name)
            : schema.get_column_index(expr->table_name, expr->column_name);
        if (idx < 0) return -1;
        CompiledNode node;
        node.kind = CompiledNode::Kind::kColumn;
        node.column_idx = static_cast<u32>(idx);
        compiled_nodes_.push_back(node);
        return static_cast<int>(compiled_nodes_.size() - 1);
    }
    if (expr->type == ExprType::kUnaryOp) {
        int child = compile_expr(expr->child.get(), schema);
        if (child < 0) return -1;
        CompiledNode node;
        if (expr->op == "NOT") node.kind = CompiledNode::Kind::kNot;
        else if (expr->op == "IS_NULL") node.kind = CompiledNode::Kind::kIsNull;
        else if (expr->op == "IS_NOT_NULL") node.kind = CompiledNode::Kind::kIsNotNull;
        else return -1;
        node.left = child;
        compiled_nodes_.push_back(node);
        return static_cast<int>(compiled_nodes_.size() - 1);
    }
    if (expr->type == ExprType::kBinaryOp) {
        int left = compile_expr(expr->left.get(), schema);
        int right = compile_expr(expr->right.get(), schema);
        if (left < 0 || right < 0) return -1;
        CompiledNode node;
        if (expr->op == "AND") node.kind = CompiledNode::Kind::kAnd;
        else if (expr->op == "OR") node.kind = CompiledNode::Kind::kOr;
        else if (expr->op == "=" || expr->op == "<>" || expr->op == "!=" ||
                 expr->op == "<" || expr->op == ">" ||
                 expr->op == "<=" || expr->op == ">=") {
            node.kind = CompiledNode::Kind::kCompare;
            node.op = expr->op;
        } else {
            return -1;
        }
        node.left = left;
        node.right = right;
        compiled_nodes_.push_back(node);
        return static_cast<int>(compiled_nodes_.size() - 1);
    }
    return -1;
}

bool FilterExecutor::eval_compiled(const Tuple& tuple, Value* out) const {
    if (compiled_root_ < 0 || !out) return false;
    std::vector<Value>& stack = eval_stack_;
    for (u32 i = 0; i < compiled_nodes_.size(); i++) {
        const CompiledNode& node = compiled_nodes_[i];
        switch (node.kind) {
            case CompiledNode::Kind::kLiteral:
                stack[i] = node.literal;
                break;
            case CompiledNode::Kind::kColumn:
                stack[i] = tuple.get_value(node.column_idx);
                break;
            case CompiledNode::Kind::kCompare: {
                const Value& left = stack[static_cast<u32>(node.left)];
                const Value& right = stack[static_cast<u32>(node.right)];
                if (left.is_null() || right.is_null()) {
                    stack[i] = Value();
                } else if (node.op == "=") {
                    stack[i] = Value(left == right);
                } else if (node.op == "<>" || node.op == "!=") {
                    stack[i] = Value(left != right);
                } else if (node.op == "<") {
                    stack[i] = Value(left < right);
                } else if (node.op == ">") {
                    stack[i] = Value(left > right);
                } else if (node.op == "<=") {
                    stack[i] = Value(left <= right);
                } else {
                    stack[i] = Value(left >= right);
                }
                break;
            }
            case CompiledNode::Kind::kAnd: {
                const Value& left = stack[static_cast<u32>(node.left)];
                const Value& right = stack[static_cast<u32>(node.right)];
                if ((!left.is_null() && !left.get_bool()) ||
                    (!right.is_null() && !right.get_bool())) {
                    stack[i] = Value(false);
                } else if (left.is_null() || right.is_null()) {
                    stack[i] = Value();
                } else {
                    stack[i] = Value(left.get_bool() && right.get_bool());
                }
                break;
            }
            case CompiledNode::Kind::kOr: {
                const Value& left = stack[static_cast<u32>(node.left)];
                const Value& right = stack[static_cast<u32>(node.right)];
                if ((!left.is_null() && left.get_bool()) ||
                    (!right.is_null() && right.get_bool())) {
                    stack[i] = Value(true);
                } else if (left.is_null() || right.is_null()) {
                    stack[i] = Value();
                } else {
                    stack[i] = Value(left.get_bool() || right.get_bool());
                }
                break;
            }
            case CompiledNode::Kind::kNot: {
                const Value& child = stack[static_cast<u32>(node.left)];
                stack[i] = child.is_null() ? Value() : Value(!child.get_bool());
                break;
            }
            case CompiledNode::Kind::kIsNull:
                stack[i] = Value(stack[static_cast<u32>(node.left)].is_null());
                break;
            case CompiledNode::Kind::kIsNotNull:
                stack[i] = Value(!stack[static_cast<u32>(node.left)].is_null());
                break;
        }
    }
    *out = stack[static_cast<u32>(compiled_root_)];
    return true;
}

ExecResult FilterExecutor::next() {
    while (true) {
        ExecResult result = child_->next();
        if (!result.ok()) return result;
        // Try fast path first (avoids AST recursion for simple column-op-literal)
        Value cond;
        if (!eval_compiled(result.tuple, &cond) &&
            !ExpressionEvaluator::fast_evaluate(*predicate_, result.tuple, &cond)) {
            cond = ExpressionEvaluator::evaluate(*predicate_, result.tuple);
        }
        if (!cond.is_null() && cond.get_bool()) return result;
    }
}

const Schema& FilterExecutor::output_schema() const { return child_->output_schema(); }

bool FilterExecutor::last_record_id(RecordId* rid) const {
    return child_->last_record_id(rid);
}

} // namespace minidb
