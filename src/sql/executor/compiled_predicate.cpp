#include "sql/executor/compiled_predicate.h"
#include "sql/executor/executor.h"
#include "sql/executor/expression_evaluator.h"
#include "sql/parser/ast.h"

namespace minidb {

// Opcode encoding for the six comparison operators. Keeping them as small
// integers lets the hot evaluation loop dispatch with a single switch on
// `op_code` instead of repeated String comparisons.
namespace {
constexpr u8 kCmpEq = 0, kCmpNe = 1, kCmpLt = 2, kCmpGt = 3, kCmpLe = 4, kCmpGe = 5;
}

bool CompiledPredicate::compile(const Expression* expr, const Schema& schema) {
    nodes_.clear();
    root_ = compile_node(expr, schema);
    if (root_ < 0) {
        nodes_.clear();
        eval_stack_.clear();
        return false;
    }
    eval_stack_.assign(nodes_.size(), Value());
    return true;
}

i32 CompiledPredicate::compile_node(const Expression* expr, const Schema& schema) {
    if (!expr) return -1;

    if (expr->type == ExprType::kLiteral) {
        Node n;
        n.kind = Node::Kind::kLiteral;
        n.literal = expr->literal_value;
        nodes_.push_back(static_cast<Node&&>(n));
        return static_cast<i32>(nodes_.size() - 1);
    }

    if (expr->type == ExprType::kColumnRef) {
        int idx = expr->table_name.empty()
            ? schema.get_column_index(expr->column_name)
            : schema.get_column_index(expr->table_name, expr->column_name);
        if (idx < 0) return -1;
        Node n;
        n.kind = Node::Kind::kColumn;
        n.column_idx = static_cast<u32>(idx);
        nodes_.push_back(static_cast<Node&&>(n));
        return static_cast<i32>(nodes_.size() - 1);
    }

    if (expr->type == ExprType::kUnaryOp) {
        i32 child = compile_node(expr->child.get(), schema);
        if (child < 0) return -1;
        Node n;
        if (expr->op == "NOT") n.kind = Node::Kind::kNot;
        else if (expr->op == "IS_NULL") n.kind = Node::Kind::kIsNull;
        else if (expr->op == "IS_NOT_NULL") n.kind = Node::Kind::kIsNotNull;
        else return -1;
        n.left = child;
        nodes_.push_back(static_cast<Node&&>(n));
        return static_cast<i32>(nodes_.size() - 1);
    }

    if (expr->type == ExprType::kBinaryOp) {
        i32 left = compile_node(expr->left.get(), schema);
        i32 right = compile_node(expr->right.get(), schema);
        if (left < 0 || right < 0) return -1;
        Node n;
        if (expr->op == "AND") {
            n.kind = Node::Kind::kAnd;
        } else if (expr->op == "OR") {
            n.kind = Node::Kind::kOr;
        } else if (expr->op == "=") {
            n.kind = Node::Kind::kCompare; n.op_code = kCmpEq;
        } else if (expr->op == "<>" || expr->op == "!=") {
            n.kind = Node::Kind::kCompare; n.op_code = kCmpNe;
        } else if (expr->op == "<") {
            n.kind = Node::Kind::kCompare; n.op_code = kCmpLt;
        } else if (expr->op == ">") {
            n.kind = Node::Kind::kCompare; n.op_code = kCmpGt;
        } else if (expr->op == "<=") {
            n.kind = Node::Kind::kCompare; n.op_code = kCmpLe;
        } else if (expr->op == ">=") {
            n.kind = Node::Kind::kCompare; n.op_code = kCmpGe;
        } else {
            return -1;
        }
        n.left = left;
        n.right = right;
        nodes_.push_back(static_cast<Node&&>(n));
        return static_cast<i32>(nodes_.size() - 1);
    }

    return -1;
}

bool CompiledPredicate::passes(const Tuple& tuple) const {
    if (root_ < 0) return false;
    std::vector<Value>& s = eval_stack_;
    const u32 n = nodes_.size();
    for (u32 i = 0; i < n; i++) {
        const Node& node = nodes_[i];
        switch (node.kind) {
            case Node::Kind::kLiteral:
                s[i] = node.literal;
                break;
            case Node::Kind::kColumn:
                s[i] = tuple.get_value(node.column_idx);
                break;
            case Node::Kind::kCompare: {
                const Value& l = s[static_cast<u32>(node.left)];
                const Value& r = s[static_cast<u32>(node.right)];
                if (l.is_null() || r.is_null()) { s[i] = Value(); break; }
                bool b = false;
                switch (node.op_code) {
                    case kCmpEq: b = (l == r); break;
                    case kCmpNe: b = (l != r); break;
                    case kCmpLt: b = (l <  r); break;
                    case kCmpGt: b = (l >  r); break;
                    case kCmpLe: b = (l <= r); break;
                    case kCmpGe: b = (l >= r); break;
                }
                s[i] = Value(b);
                break;
            }
            case Node::Kind::kAnd: {
                const Value& l = s[static_cast<u32>(node.left)];
                const Value& r = s[static_cast<u32>(node.right)];
                bool lb = false;
                bool rb = false;
                if (!ExpressionEvaluator::predicate_truth(l, &lb) ||
                    !ExpressionEvaluator::predicate_truth(r, &rb)) {
                    set_executor_error("predicate expression must be BOOL");
                    return false;
                }
                // SQL 3VL: FALSE AND anything is FALSE (even NULL).
                if ((!l.is_null() && !lb) ||
                    (!r.is_null() && !rb)) {
                    s[i] = Value(false);
                } else if (l.is_null() || r.is_null()) {
                    s[i] = Value();
                } else {
                    s[i] = Value(lb && rb);
                }
                break;
            }
            case Node::Kind::kOr: {
                const Value& l = s[static_cast<u32>(node.left)];
                const Value& r = s[static_cast<u32>(node.right)];
                bool lb = false;
                bool rb = false;
                if (!ExpressionEvaluator::predicate_truth(l, &lb) ||
                    !ExpressionEvaluator::predicate_truth(r, &rb)) {
                    set_executor_error("predicate expression must be BOOL");
                    return false;
                }
                if ((!l.is_null() && lb) ||
                    (!r.is_null() && rb)) {
                    s[i] = Value(true);
                } else if (l.is_null() || r.is_null()) {
                    s[i] = Value();
                } else {
                    s[i] = Value(lb || rb);
                }
                break;
            }
            case Node::Kind::kNot: {
                const Value& c = s[static_cast<u32>(node.left)];
                bool cb = false;
                if (!ExpressionEvaluator::predicate_truth(c, &cb)) {
                    set_executor_error("predicate expression must be BOOL");
                    return false;
                }
                s[i] = c.is_null() ? Value() : Value(!cb);
                break;
            }
            case Node::Kind::kIsNull:
                s[i] = Value(s[static_cast<u32>(node.left)].is_null());
                break;
            case Node::Kind::kIsNotNull:
                s[i] = Value(!s[static_cast<u32>(node.left)].is_null());
                break;
        }
    }
    const Value& v = s[static_cast<u32>(root_)];
    // WHERE semantics: TRUE passes, FALSE/NULL fail.
    bool pass = false;
    if (!ExpressionEvaluator::predicate_truth(v, &pass)) {
        set_executor_error("predicate expression must be BOOL");
        return false;
    }
    return pass;
}

} // namespace minidb
