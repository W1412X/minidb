/**
 * @file parser.cpp
 * @brief Parser implementation — Recursive descent
 */
#include "sql/parser/parser.h"

namespace minidb {

static String upper_ascii(const String& value) {
    String out;
    for (u32 i = 0; i < value.size(); i++) {
        char c = value[i];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        out += c;
    }
    return out;
}

Parser::Parser(const String& sql) : lexer_(sql), ok_(true) {}

Statement Parser::parse() {
    Statement stmt;
    Token t = peek();

    switch (t.type) {
        case TokenType::KW_SELECT:
            stmt.type = StmtType::kSelect;
            stmt.select = parse_select();
            break;
        case TokenType::KW_EXPLAIN: {
            lexer_.consume_token();
            stmt.type = StmtType::kExplain;
            if (check_keyword(TokenType::KW_ANALYZE)) {
                lexer_.consume_token();
                stmt.explain_analyze = true;
            }
            auto inner = make_unique<Statement>();
            Token next = peek();
            switch (next.type) {
                case TokenType::KW_SELECT:
                    inner->type = StmtType::kSelect;
                    inner->select = parse_select();
                    break;
                case TokenType::KW_UPDATE:
                    inner->type = StmtType::kUpdate;
                    inner->update = parse_update();
                    break;
                case TokenType::KW_DELETE:
                    inner->type = StmtType::kDelete;
                    inner->delete_stmt = parse_delete();
                    break;
                case TokenType::KW_INSERT:
                    inner->type = StmtType::kInsert;
                    inner->insert = parse_insert();
                    break;
                default:
                    set_error_at(String("EXPLAIN requires SELECT/INSERT/UPDATE/DELETE"), next);
                    return Statement();
            }
            stmt.explain_stmt = static_cast<UniquePtr<Statement>&&>(inner);
            break;
        }
        case TokenType::KW_ANALYZE: {
            lexer_.consume_token();
            stmt.type = StmtType::kAnalyze;
            if (match_keyword(TokenType::KW_TABLE)) {
                // Accept PostgreSQL-style ANALYZE TABLE t as a compatibility alias.
            }
            Token table = expect_identifier();
            stmt.analyze_table_name = table.value;
            break;
        }
        case TokenType::KW_INSERT:
            stmt.type = StmtType::kInsert;
            stmt.insert = parse_insert();
            break;
        case TokenType::KW_UPDATE:
            stmt.type = StmtType::kUpdate;
            stmt.update = parse_update();
            break;
        case TokenType::KW_DELETE:
            stmt.type = StmtType::kDelete;
            stmt.delete_stmt = parse_delete();
            break;
        case TokenType::KW_CREATE: {
            lexer_.consume_token();
            Token next = peek();
            if (next.type == TokenType::KW_INDEX || next.type == TokenType::KW_UNIQUE) {
                stmt.type = StmtType::kCreateIndex;
                stmt.create_index = parse_create_index();
            } else {
                stmt.type = StmtType::kCreateTable;
                stmt.create_table = parse_create_table();
            }
            break;
        }
        case TokenType::KW_DROP: {
            lexer_.consume_token();
            if (check_keyword(TokenType::KW_INDEX)) {
                stmt.type = StmtType::kDropIndex;
                stmt.drop_index_name = parse_drop_index();
            } else {
                stmt.type = StmtType::kDropTable;
                stmt.drop_table_name = parse_drop_table();
            }
            break;
        }
        case TokenType::KW_SHOW: {
            lexer_.consume_token();
            if (match_keyword(TokenType::KW_TABLES)) {
                stmt.type = StmtType::kShowTables;
            } else {
                Token after_show = peek();
                set_error_at(String("only SHOW TABLES is supported"), after_show);
                return Statement();
            }
            break;
        }
        case TokenType::KW_DESC:
        case TokenType::KW_DESCRIBE:
            stmt.type = StmtType::kDescTable;
            stmt.desc_table = parse_desc_table();
            break;
        case TokenType::KW_BEGIN:
            stmt.type = StmtType::kBegin;
            lexer_.consume_token();
            break;
        case TokenType::KW_COMMIT:
            stmt.type = StmtType::kCommit;
            lexer_.consume_token();
            break;
        case TokenType::KW_ROLLBACK:
            stmt.type = StmtType::kRollback;
            lexer_.consume_token();
            break;
        case TokenType::KW_ALTER:
            stmt.type = StmtType::kAlterTable;
            stmt.alter_table = parse_alter_table();
            break;
        case TokenType::KW_PREPARE:
            stmt.type = StmtType::kPrepare;
            stmt.prepare_stmt = parse_prepare();
            break;
        case TokenType::KW_EXECUTE:
            stmt.type = StmtType::kExecute;
            stmt.execute_stmt = parse_execute();
            break;
        case TokenType::KW_DEALLOCATE:
            stmt.type = StmtType::kDeallocate;
            stmt.deallocate_stmt = parse_deallocate();
            break;
        case TokenType::KW_VACUUM: {
            lexer_.consume_token();
            stmt.type = StmtType::kVacuum;
            // Accept optional TABLE keyword: VACUUM [TABLE] [table_name]
            match_keyword(TokenType::KW_TABLE);
            Token next = peek();
            if (next.type != TokenType::SEMICOLON && next.type != TokenType::END_OF_INPUT) {
                Token table = expect_identifier();
                stmt.vacuum_table_name = table.value;
            }
            break;
        }
        default:
            set_error_at(String("unknown statement"), t);
            return Statement();
    }

    if (!ok_) return Statement();
    match(TokenType::SEMICOLON);
    Token trailing = peek();
    if (trailing.type != TokenType::END_OF_INPUT) {
        set_error_at(String("unexpected trailing token after statement"), trailing);
        return Statement();
    }
    return stmt;
}

// ============================================================
// SELECT
// ============================================================

UniquePtr<SelectStmt> Parser::parse_select() {
    auto stmt = parse_select_body();
    if (match_keyword(TokenType::KW_UNION)) {
        stmt->union_all = match_keyword(TokenType::KW_ALL);
        stmt->union_rhs = parse_select();
        if (stmt->union_rhs) {
            if (stmt->order_by.empty() && !stmt->union_rhs->order_by.empty()) {
                for (u32 i = 0; i < stmt->union_rhs->order_by.size(); i++) {
                    stmt->order_by.push_back(static_cast<OrderByItem&&>(
                        stmt->union_rhs->order_by[i]));
                }
                stmt->union_rhs->order_by.clear();
            }
            if (stmt->limit < 0 && stmt->union_rhs->limit >= 0) {
                stmt->limit = stmt->union_rhs->limit;
                stmt->union_rhs->limit = -1;
            }
            if (stmt->offset < 0 && stmt->union_rhs->offset >= 0) {
                stmt->offset = stmt->union_rhs->offset;
                stmt->union_rhs->offset = -1;
            }
        }
    }
    return stmt;
}

UniquePtr<SelectStmt> Parser::parse_select_body() {
    auto stmt = make_unique<SelectStmt>();
    expect_keyword(TokenType::KW_SELECT);
    stmt->distinct = match_keyword(TokenType::KW_DISTINCT);

    // Reject keywords that introduce a downstream SELECT clause from
    // appearing in the select list. is_identifier_token() accepts every
    // keyword in the KW_SELECT..KW_DEALLOCATE range, so without this guard
    // `SELECT FROM t` silently parses as "SELECT [column-named-FROM] FROM t"
    // and yields an empty result with no diagnostic. Caught early so the
    // REPL surfaces a precise line:col error.
    {
        Token next = peek();
        switch (next.type) {
            case TokenType::KW_FROM:
            case TokenType::KW_WHERE:
            case TokenType::KW_GROUP:
            case TokenType::KW_HAVING:
            case TokenType::KW_ORDER:
            case TokenType::KW_LIMIT:
            case TokenType::KW_OFFSET:
            case TokenType::KW_UNION:
            case TokenType::KW_JOIN:
            case TokenType::KW_INNER:
            case TokenType::KW_LEFT:
            case TokenType::KW_RIGHT:
            case TokenType::KW_CROSS:
            case TokenType::KW_OUTER:
            case TokenType::KW_ON:
            case TokenType::SEMICOLON:
            case TokenType::END_OF_INPUT:
                set_error_at(String("expected select expression"), next);
                return stmt;
            default:
                break;
        }
    }

    // select list
    if (check(TokenType::STAR)) {
        lexer_.consume_token();
        auto expr = make_unique<Expression>();
        expr->type = ExprType::kStar;
        stmt->select_list.push_back(static_cast<UniquePtr<Expression>&&>(expr));
        // Handle SELECT *, col case
        while (match(TokenType::COMMA)) {
            stmt->select_list.push_back(static_cast<UniquePtr<Expression>&&>(parse_expression()));
            if (match_keyword(TokenType::KW_AS)) {
                stmt->select_list[stmt->select_list.size() - 1]->alias =
                    expect_alias().value;
            } else if (check(TokenType::IDENTIFIER)) {
                stmt->select_list[stmt->select_list.size() - 1]->alias = peek().value;
                lexer_.consume_token();
            }
        }
    } else {
        stmt->select_list.push_back(static_cast<UniquePtr<Expression>&&>(parse_expression()));
        if (match_keyword(TokenType::KW_AS)) {
            stmt->select_list[stmt->select_list.size() - 1]->alias =
                expect_alias().value;
        } else if (check(TokenType::IDENTIFIER)) {
            stmt->select_list[stmt->select_list.size() - 1]->alias = peek().value;
            lexer_.consume_token();
        }
        while (match(TokenType::COMMA)) {
            stmt->select_list.push_back(static_cast<UniquePtr<Expression>&&>(parse_expression()));
            if (match_keyword(TokenType::KW_AS)) {
                stmt->select_list[stmt->select_list.size() - 1]->alias =
                    expect_alias().value;
            } else if (check(TokenType::IDENTIFIER)) {
                stmt->select_list[stmt->select_list.size() - 1]->alias = peek().value;
                lexer_.consume_token();
            }
        }
    }

    // FROM
    if (match_keyword(TokenType::KW_FROM)) {
        stmt->from_tables.push_back(parse_table_ref());
    }

    // JOINs
    while (check_keyword(TokenType::KW_INNER) || check_keyword(TokenType::KW_JOIN) ||
           check_keyword(TokenType::KW_LEFT) || check_keyword(TokenType::KW_CROSS) ||
           check_keyword(TokenType::KW_RIGHT)) {
        JoinClause join;
        bool requires_on = true;
        if (check_keyword(TokenType::KW_RIGHT)) {
            mark_error();
            lexer_.consume_token();
            break;
        } else if (check_keyword(TokenType::KW_CROSS)) {
            lexer_.consume_token();
            join.type = JoinType::kInner;
            requires_on = false;
        } else if (check_keyword(TokenType::KW_LEFT)) {
            lexer_.consume_token();
            match_keyword(TokenType::KW_OUTER);
            join.type = JoinType::kLeft;
        } else {
            match_keyword(TokenType::KW_INNER);
            join.type = JoinType::kInner;
        }
        expect_keyword(TokenType::KW_JOIN);
        join.table = parse_table_ref();
        if (!requires_on && check_keyword(TokenType::KW_ON)) {
            mark_error();
            break;
        }
        if (match_keyword(TokenType::KW_ON)) {
            join.on_condition = static_cast<UniquePtr<Expression>&&>(parse_expression());
        } else if (requires_on) {
            mark_error();
            break;
        }
        stmt->joins.push_back(static_cast<JoinClause&&>(join));
    }

    // WHERE
    if (match_keyword(TokenType::KW_WHERE)) {
        stmt->where_clause = static_cast<UniquePtr<Expression>&&>(parse_expression());
    }

    // GROUP BY
    if (match_keyword(TokenType::KW_GROUP)) {
        expect_keyword(TokenType::KW_BY);
        stmt->group_by.push_back(static_cast<UniquePtr<Expression>&&>(parse_expression()));
        while (match(TokenType::COMMA)) {
            stmt->group_by.push_back(static_cast<UniquePtr<Expression>&&>(parse_expression()));
        }
    }

    // HAVING
    if (match_keyword(TokenType::KW_HAVING)) {
        stmt->having = static_cast<UniquePtr<Expression>&&>(parse_expression());
    }

    // ORDER BY
    if (match_keyword(TokenType::KW_ORDER)) {
        expect_keyword(TokenType::KW_BY);
        do {
            auto expr = static_cast<UniquePtr<Expression>&&>(parse_expression());
            bool is_asc = true;
            if (match_keyword(TokenType::KW_ASC)) is_asc = true;
            else if (match_keyword(TokenType::KW_DESC)) is_asc = false;
            // PostgreSQL default: NULLS LAST for ASC, NULLS FIRST for DESC
            bool nf = !is_asc;
            if (match_keyword(TokenType::KW_NULLS)) {
                if (match_keyword(TokenType::KW_FIRST)) nf = true;
                else { expect_keyword(TokenType::KW_LAST); nf = false; }
            }
            OrderByItem item;
            item.expression = static_cast<UniquePtr<Expression>&&>(expr);
            item.ascending = is_asc;
            item.nulls_first = nf;
            stmt->order_by.push_back(static_cast<OrderByItem&&>(item));
        } while (match(TokenType::COMMA));
    }

    // LIMIT
    if (match_keyword(TokenType::KW_LIMIT)) {
        bool negative = match(TokenType::MINUS);
        Token t = expect(TokenType::INT_LITERAL);
        char* end = nullptr;
        long val = strtol(t.value.c_str(), &end, 10);
        if (negative) {
            stmt->limit = -1;  // PostgreSQL-compatible: negative LIMIT means no limit.
        } else {
            stmt->limit = (val > 0) ? static_cast<i32>(val) : 0;
        }
    }

    // OFFSET
    if (match_keyword(TokenType::KW_OFFSET)) {
        bool negative = match(TokenType::MINUS);
        Token t = expect(TokenType::INT_LITERAL);
        char* end = nullptr;
        long val = strtol(t.value.c_str(), &end, 10);
        stmt->offset = (!negative && val > 0) ? static_cast<i32>(val) : 0;
    }

    return stmt;
}

TableRef Parser::parse_table_ref() {
    TableRef ref;
    if (match(TokenType::LPAREN)) {
        if (!check_keyword(TokenType::KW_SELECT)) {
            set_error_at(String("expected SELECT in derived table"), peek());
            return ref;
        }
        ref.subquery = parse_select();
        expect(TokenType::RPAREN);
    } else {
        ref.name = expect_identifier().value;
    }

    if (match_keyword(TokenType::KW_AS)) {
        ref.alias = expect_identifier().value;
    } else if (check(TokenType::IDENTIFIER)) {
        ref.alias = peek().value;
        lexer_.consume_token();
    }
    return ref;
}

// ============================================================
// INSERT
// ============================================================

UniquePtr<InsertStmt> Parser::parse_insert() {
    auto stmt = make_unique<InsertStmt>();
    expect_keyword(TokenType::KW_INSERT);
    expect_keyword(TokenType::KW_INTO);
    stmt->table.name = expect_identifier().value;

    // columns (optional)
    if (match(TokenType::LPAREN)) {
        stmt->columns.push_back(expect_identifier().value);
        while (match(TokenType::COMMA)) {
            stmt->columns.push_back(expect_identifier().value);
        }
        expect(TokenType::RPAREN);
    }

    expect_keyword(TokenType::KW_VALUES);

    // one or more value lists
    do {
        expect(TokenType::LPAREN);
        Vector<UniquePtr<Expression>> row;
        row.push_back(static_cast<UniquePtr<Expression>&&>(parse_expression()));
        while (match(TokenType::COMMA)) {
            row.push_back(static_cast<UniquePtr<Expression>&&>(parse_expression()));
        }
        expect(TokenType::RPAREN);
        stmt->values_list.push_back(static_cast<Vector<UniquePtr<Expression>>&&>(row));
    } while (match(TokenType::COMMA));

    return stmt;
}

// ============================================================
// UPDATE
// ============================================================

UniquePtr<UpdateStmt> Parser::parse_update() {
    auto stmt = make_unique<UpdateStmt>();
    expect_keyword(TokenType::KW_UPDATE);
    stmt->table.name = expect_identifier().value;
    expect_keyword(TokenType::KW_SET);

    do {
        String col = expect_identifier().value;
        expect(TokenType::EQ);
        auto val = static_cast<UniquePtr<Expression>&&>(parse_expression());
        stmt->set_clauses.push_back(Pair<String, UniquePtr<Expression>>(
            static_cast<String&&>(col), static_cast<UniquePtr<Expression>&&>(val)));
    } while (match(TokenType::COMMA));

    if (match_keyword(TokenType::KW_WHERE)) {
        stmt->where_clause = static_cast<UniquePtr<Expression>&&>(parse_expression());
    }

    return stmt;
}

// ============================================================
// DELETE
// ============================================================

UniquePtr<DeleteStmt> Parser::parse_delete() {
    auto stmt = make_unique<DeleteStmt>();
    expect_keyword(TokenType::KW_DELETE);
    expect_keyword(TokenType::KW_FROM);
    stmt->table.name = expect_identifier().value;

    if (match_keyword(TokenType::KW_WHERE)) {
        stmt->where_clause = static_cast<UniquePtr<Expression>&&>(parse_expression());
    }

    return stmt;
}

// ============================================================
// CREATE TABLE
// ============================================================

UniquePtr<CreateTableStmt> Parser::parse_create_table() {
    auto stmt = make_unique<CreateTableStmt>();
    // CREATE keyword already consumed by main parse() dispatch
    expect_keyword(TokenType::KW_TABLE);
    stmt->table_name = expect_identifier().value;
    expect(TokenType::LPAREN);

    do {
        ColumnDef col;
        // Allow column names that collide with reserved keywords (e.g. status).
        col.name = expect_alias().value;

        // type
        Token type_tok = peek();
        if (match_keyword(TokenType::KW_INT)) {
            col.type_name = "INT";
        } else if (match_keyword(TokenType::KW_INT64)) {
            col.type_name = "BIGINT";
        } else if (match_keyword(TokenType::KW_BOOL)) {
            col.type_name = "BOOL";
        } else if (match_keyword(TokenType::KW_FLOAT)) {
            col.type_name = "FLOAT";
        } else if (match_keyword(TokenType::KW_DOUBLE)) {
            col.type_name = "DOUBLE";
        } else if (match_keyword(TokenType::KW_TEXT)) {
            col.type_name = "TEXT";
        } else if (match_keyword(TokenType::KW_TIMESTAMP)) {
            col.type_name = "TIMESTAMP";
        } else if (match_keyword(TokenType::KW_DATETIME)) {
            col.type_name = "DATETIME";
        } else if (match_keyword(TokenType::KW_VARCHAR)) {
            col.type_name = "VARCHAR";
            if (match(TokenType::LPAREN)) {
                col.varchar_length = atoi(expect(TokenType::INT_LITERAL).value.c_str());
                expect(TokenType::RPAREN);
            }
        } else {
            col.type_name = upper_ascii(expect_identifier().value);
            if (match(TokenType::LPAREN)) {
                expect(TokenType::INT_LITERAL);
                if (match(TokenType::COMMA)) {
                    expect(TokenType::INT_LITERAL);
                }
                expect(TokenType::RPAREN);
            }
        }

        // Constraints: order-agnostic, mirrors the ALTER TABLE ADD COLUMN
        // grammar so PRIMARY KEY / NOT NULL / UNIQUE / DEFAULT can appear in
        // permutation. Each clause is recognised at most once.
        while (true) {
            if (match_keyword(TokenType::KW_PRIMARY)) {
                expect_keyword(TokenType::KW_KEY);
                col.is_primary = true;
                col.not_null = true;
            } else if (match_keyword(TokenType::KW_NOT)) {
                expect_keyword(TokenType::KW_NULL);
                col.not_null = true;
            } else if (match_keyword(TokenType::KW_UNIQUE)) {
                col.is_unique = true;
            } else if (match_keyword(TokenType::KW_DEFAULT)) {
                Token def = peek();
                lexer_.consume_token();
                col.default_value = def.value;
            } else if (match_keyword(TokenType::KW_CHECK)) {
                // Slice the original SQL between the `(` after CHECK and
                // the matching `)`. Storing the raw text lets the catalog
                // stay a simple string-keyed store and avoids carrying the
                // parser AST through serialisation. Re-parsed at INSERT /
                // UPDATE time.
                expect(TokenType::LPAREN);
                u32 start = lexer_.byte_pos();
                int depth = 1;
                while (depth > 0) {
                    Token t = peek();
                    if (t.type == TokenType::END_OF_INPUT) { mark_error(); break; }
                    if (t.type == TokenType::LPAREN) depth++;
                    else if (t.type == TokenType::RPAREN) {
                        if (--depth == 0) break;
                    }
                    lexer_.consume_token();
                }
                if (ok_) {
                    // The closing `)` was peeked but not consumed; byte_pos
                    // sits PAST it, so end_pos = byte_pos - 1 gives the
                    // index of `)` itself. Substring stops right before.
                    u32 raw_end = lexer_.byte_pos();
                    const String& sql = lexer_.source();
                    u32 end_pos = raw_end;
                    // Walk back past the `)` glyph and any trailing whitespace.
                    while (end_pos > start && sql[end_pos - 1] != ')') end_pos--;
                    if (end_pos > start && sql[end_pos - 1] == ')') end_pos--;
                    while (end_pos > start &&
                           (sql[end_pos - 1] == ' ' || sql[end_pos - 1] == '\t' ||
                            sql[end_pos - 1] == '\n')) {
                        end_pos--;
                    }
                    col.check_expr = sql.substr(start, end_pos - start);
                    expect(TokenType::RPAREN);
                }
            } else {
                break;
            }
        }

        stmt->columns.push_back(col);
    } while (match(TokenType::COMMA));

    expect(TokenType::RPAREN);
    return stmt;
}

String Parser::parse_drop_table() {
    if (check_keyword(TokenType::KW_DROP)) expect_keyword(TokenType::KW_DROP);
    expect_keyword(TokenType::KW_TABLE);
    return expect_identifier().value;
}

String Parser::parse_drop_index() {
    if (check_keyword(TokenType::KW_DROP)) expect_keyword(TokenType::KW_DROP);
    expect_keyword(TokenType::KW_INDEX);
    return expect_identifier().value;
}

UniquePtr<CreateIndexStmt> Parser::parse_create_index() {
    auto stmt = make_unique<CreateIndexStmt>();
    if (check_keyword(TokenType::KW_CREATE)) {
        expect_keyword(TokenType::KW_CREATE);
    }
    stmt->unique = match_keyword(TokenType::KW_UNIQUE);
    expect_keyword(TokenType::KW_INDEX);
    stmt->index_name = expect_identifier().value;
    expect_keyword(TokenType::KW_ON);
    stmt->table_name = expect_identifier().value;
    expect(TokenType::LPAREN);
    stmt->columns.push_back(expect_identifier().value);
    while (match(TokenType::COMMA)) {
        stmt->columns.push_back(expect_identifier().value);
    }
    expect(TokenType::RPAREN);
    return stmt;
}

UniquePtr<DescTableStmt> Parser::parse_desc_table() {
    auto stmt = make_unique<DescTableStmt>();
    if (check_keyword(TokenType::KW_DESCRIBE)) expect_keyword(TokenType::KW_DESCRIBE);
    else expect_keyword(TokenType::KW_DESC);
    stmt->table_name = expect_identifier().value;
    return stmt;
}

UniquePtr<AlterTableStmt> Parser::parse_alter_table() {
    auto stmt = make_unique<AlterTableStmt>();
    expect_keyword(TokenType::KW_ALTER);
    expect_keyword(TokenType::KW_TABLE);
    stmt->table_name = expect_identifier().value;

    if (check_keyword(TokenType::KW_ADD)) {
        lexer_.consume_token();
        if (check_keyword(TokenType::KW_COLUMN)) lexer_.consume_token();
        stmt->alter_type = AlterType::kAddColumn;
        stmt->new_column.name = expect_identifier().value;
        if (match_keyword(TokenType::KW_INT)) {
            stmt->new_column.type_name = "INT";
        } else if (match_keyword(TokenType::KW_INT64)) {
            stmt->new_column.type_name = "BIGINT";
        } else if (match_keyword(TokenType::KW_BOOL)) {
            stmt->new_column.type_name = "BOOL";
        } else if (match_keyword(TokenType::KW_FLOAT)) {
            stmt->new_column.type_name = "FLOAT";
        } else if (match_keyword(TokenType::KW_DOUBLE)) {
            stmt->new_column.type_name = "DOUBLE";
        } else if (match_keyword(TokenType::KW_TEXT)) {
            stmt->new_column.type_name = "TEXT";
        } else if (match_keyword(TokenType::KW_TIMESTAMP)) {
            stmt->new_column.type_name = "TIMESTAMP";
        } else if (match_keyword(TokenType::KW_DATETIME)) {
            stmt->new_column.type_name = "DATETIME";
        } else if (match_keyword(TokenType::KW_VARCHAR)) {
            stmt->new_column.type_name = "VARCHAR";
            if (match(TokenType::LPAREN)) {
                stmt->new_column.varchar_length =
                    atoi(expect(TokenType::INT_LITERAL).value.c_str());
                expect(TokenType::RPAREN);
            }
        } else {
            stmt->new_column.type_name = upper_ascii(expect_identifier().value);
            if (match(TokenType::LPAREN)) {
                expect(TokenType::INT_LITERAL);
                if (match(TokenType::COMMA)) expect(TokenType::INT_LITERAL);
                expect(TokenType::RPAREN);
            }
        }
        while (true) {
            if (match_keyword(TokenType::KW_NOT)) {
                expect_keyword(TokenType::KW_NULL);
                stmt->new_column.not_null = true;
            } else if (match_keyword(TokenType::KW_DEFAULT)) {
                Token def = peek();
                lexer_.consume_token();
                stmt->new_column.default_value = def.value;
            } else if (match_keyword(TokenType::KW_UNIQUE)) {
                stmt->new_column.is_unique = true;
            } else {
                break;
            }
        }
    } else if (check_keyword(TokenType::KW_DROP)) {
        lexer_.consume_token();
        if (check_keyword(TokenType::KW_COLUMN)) lexer_.consume_token();
        stmt->alter_type = AlterType::kDropColumn;
        stmt->drop_column_name = expect_identifier().value;
    } else if (check_keyword(TokenType::KW_RENAME)) {
        lexer_.consume_token();
        if (check_keyword(TokenType::KW_COLUMN)) lexer_.consume_token();
        stmt->alter_type = AlterType::kRenameColumn;
        stmt->rename_from = expect_identifier().value;
        expect_keyword(TokenType::KW_TO);
        stmt->rename_to = expect_identifier().value;
    } else {
        mark_error();
    }
    return stmt;
}

UniquePtr<PrepareStmt> Parser::parse_prepare() {
    auto stmt = make_unique<PrepareStmt>();
    expect_keyword(TokenType::KW_PREPARE);
    stmt->name = expect_identifier().value;
    expect_keyword(TokenType::KW_AS);
    // Rest of the SQL is the statement to prepare
    stmt->sql = "";
    while (!check(TokenType::END_OF_INPUT) && !check(TokenType::SEMICOLON)) {
        Token t = peek();
        if (!stmt->sql.empty()) stmt->sql += " ";
        stmt->sql += t.value;
        lexer_.consume_token();
    }
    return stmt;
}

UniquePtr<ExecuteStmt> Parser::parse_execute() {
    auto stmt = make_unique<ExecuteStmt>();
    expect_keyword(TokenType::KW_EXECUTE);
    stmt->name = expect_identifier().value;
    if (match(TokenType::LPAREN)) {
        if (!check(TokenType::RPAREN)) {
            stmt->params.push_back(static_cast<UniquePtr<Expression>&&>(parse_expression()));
            while (match(TokenType::COMMA)) {
                stmt->params.push_back(static_cast<UniquePtr<Expression>&&>(parse_expression()));
            }
        }
        expect(TokenType::RPAREN);
    }
    return stmt;
}

UniquePtr<DeallocateStmt> Parser::parse_deallocate() {
    auto stmt = make_unique<DeallocateStmt>();
    expect_keyword(TokenType::KW_DEALLOCATE);
    stmt->name = expect_identifier().value;
    return stmt;
}

// ============================================================
// Table expression parsing (precedence climbing)
// ============================================================

UniquePtr<Expression> Parser::parse_expression() {
    return parse_or_expr();
}

UniquePtr<Expression> Parser::parse_or_expr() {
    auto left = static_cast<UniquePtr<Expression>&&>(parse_and_expr());
    while (match_keyword(TokenType::KW_OR)) {
        auto right = static_cast<UniquePtr<Expression>&&>(parse_and_expr());
        auto bin = make_unique<Expression>();
        bin->type = ExprType::kBinaryOp;
        bin->op = "OR";
        bin->left = static_cast<UniquePtr<Expression>&&>(left);
        bin->right = static_cast<UniquePtr<Expression>&&>(right);
        left = static_cast<UniquePtr<Expression>&&>(bin);
    }
    return left;
}

UniquePtr<Expression> Parser::parse_and_expr() {
    auto left = static_cast<UniquePtr<Expression>&&>(parse_not_expr());
    while (match_keyword(TokenType::KW_AND)) {
        auto right = static_cast<UniquePtr<Expression>&&>(parse_not_expr());
        auto bin = make_unique<Expression>();
        bin->type = ExprType::kBinaryOp;
        bin->op = "AND";
        bin->left = static_cast<UniquePtr<Expression>&&>(left);
        bin->right = static_cast<UniquePtr<Expression>&&>(right);
        left = static_cast<UniquePtr<Expression>&&>(bin);
    }
    return left;
}

UniquePtr<Expression> Parser::parse_not_expr() {
    if (match_keyword(TokenType::KW_NOT)) {
        auto child = static_cast<UniquePtr<Expression>&&>(parse_not_expr());
        auto unary = make_unique<Expression>();
        unary->type = ExprType::kUnaryOp;
        unary->op = "NOT";
        unary->child = static_cast<UniquePtr<Expression>&&>(child);
        return unary;
    }
    return parse_comparison();
}

UniquePtr<Expression> Parser::parse_comparison() {
    auto left = static_cast<UniquePtr<Expression>&&>(parse_addition());
    Token t = peek();

    if (t.type == TokenType::EQ || t.type == TokenType::NEQ ||
        t.type == TokenType::LT || t.type == TokenType::GT ||
        t.type == TokenType::LTE || t.type == TokenType::GTE) {
        lexer_.consume_token();
        auto right = static_cast<UniquePtr<Expression>&&>(parse_addition());
        auto bin = make_unique<Expression>();
        bin->type = ExprType::kBinaryOp;
        bin->op = t.value;
        bin->left = static_cast<UniquePtr<Expression>&&>(left);
        bin->right = static_cast<UniquePtr<Expression>&&>(right);
        return bin;
    }

    // IS [NOT] NULL
    if (match_keyword(TokenType::KW_IS)) {
        bool negate = match_keyword(TokenType::KW_NOT);
        match_keyword(TokenType::KW_NULL);
        auto unary = make_unique<Expression>();
        unary->type = ExprType::kUnaryOp;
        unary->op = negate ? "IS_NOT_NULL" : "IS_NULL";
        unary->child = static_cast<UniquePtr<Expression>&&>(left);
        return unary;
    }

    if (match_keyword(TokenType::KW_LIKE)) {
        auto right = static_cast<UniquePtr<Expression>&&>(parse_addition());
        auto bin = make_unique<Expression>();
        bin->type = ExprType::kBinaryOp;
        bin->op = "LIKE";
        bin->left = static_cast<UniquePtr<Expression>&&>(left);
        bin->right = static_cast<UniquePtr<Expression>&&>(right);
        return static_cast<UniquePtr<Expression>&&>(bin);
    }

    // NOT LIKE / NOT IN / NOT BETWEEN — only consume NOT if followed by LIKE/IN/BETWEEN
    if (check_keyword(TokenType::KW_NOT)) {
        // Peek at the next token after NOT (advance lexer, peek, then restore)
        lexer_.consume_token(); // consume NOT
        Token after_not = peek(); // peek at what follows
        // We can't un-consume, but we check: only proceed if after_not is LIKE/IN/BETWEEN
        if (after_not.type == TokenType::KW_LIKE) {
            // NOT LIKE
            lexer_.consume_token(); // consume LIKE
            auto right = static_cast<UniquePtr<Expression>&&>(parse_addition());
            auto bin = make_unique<Expression>();
            bin->type = ExprType::kBinaryOp;
            bin->op = "LIKE";
            bin->left = static_cast<UniquePtr<Expression>&&>(left);
            bin->right = static_cast<UniquePtr<Expression>&&>(right);

            auto unary = make_unique<Expression>();
            unary->type = ExprType::kUnaryOp;
            unary->op = "NOT";
            unary->child = static_cast<UniquePtr<Expression>&&>(bin);
            return static_cast<UniquePtr<Expression>&&>(unary);
        } else if (after_not.type == TokenType::KW_IN) {
            // NOT IN — desugar into NOT(OR chain) or NOT(subquery)
            lexer_.consume_token(); // consume IN
            expect(TokenType::LPAREN);

            if (check_keyword(TokenType::KW_SELECT)) {
                auto sub = parse_select();
                expect(TokenType::RPAREN);
                auto expr = make_unique<Expression>();
                expr->type = ExprType::kSubquery;
                expr->subquery = static_cast<UniquePtr<SelectStmt>&&>(sub);
                auto bin = make_unique<Expression>();
                bin->type = ExprType::kBinaryOp;
                bin->op = "NOT_IN_SUBQUERY";
                bin->left = static_cast<UniquePtr<Expression>&&>(left);
                bin->right = static_cast<UniquePtr<Expression>&&>(expr);
                return static_cast<UniquePtr<Expression>&&>(bin);
            }

            auto first_val = static_cast<UniquePtr<Expression>&&>(parse_expression());
            auto eq1 = make_unique<Expression>();
            eq1->type = ExprType::kBinaryOp;
            eq1->op = "=";
            eq1->left = UniquePtr<Expression>(left->clone());
            eq1->right = static_cast<UniquePtr<Expression>&&>(first_val);

            auto result = static_cast<UniquePtr<Expression>&&>(eq1);
            while (match(TokenType::COMMA)) {
                auto val = static_cast<UniquePtr<Expression>&&>(parse_expression());
                auto eq2 = make_unique<Expression>();
                eq2->type = ExprType::kBinaryOp;
                eq2->op = "=";
                eq2->left = UniquePtr<Expression>(left->clone());
                eq2->right = static_cast<UniquePtr<Expression>&&>(val);
                auto or_node = make_unique<Expression>();
                or_node->type = ExprType::kBinaryOp;
                or_node->op = "OR";
                or_node->left = static_cast<UniquePtr<Expression>&&>(result);
                or_node->right = static_cast<UniquePtr<Expression>&&>(eq2);
                result = static_cast<UniquePtr<Expression>&&>(or_node);
            }
            expect(TokenType::RPAREN);

            // Wrap in NOT
            auto unary = make_unique<Expression>();
            unary->type = ExprType::kUnaryOp;
            unary->op = "NOT";
            unary->child = static_cast<UniquePtr<Expression>&&>(result);
            return static_cast<UniquePtr<Expression>&&>(unary);
        } else if (after_not.type == TokenType::KW_BETWEEN) {
            // NOT BETWEEN
            lexer_.consume_token(); // consume BETWEEN
            auto low = static_cast<UniquePtr<Expression>&&>(parse_addition());
            expect_keyword(TokenType::KW_AND);
            auto high = static_cast<UniquePtr<Expression>&&>(parse_addition());

            auto ge = make_unique<Expression>();
            ge->type = ExprType::kBinaryOp;
            ge->op = ">=";
            ge->left = UniquePtr<Expression>(left->clone());
            ge->right = static_cast<UniquePtr<Expression>&&>(low);

            auto le = make_unique<Expression>();
            le->type = ExprType::kBinaryOp;
            le->op = "<=";
            le->left = UniquePtr<Expression>(left->clone());
            le->right = static_cast<UniquePtr<Expression>&&>(high);

            auto and_node = make_unique<Expression>();
            and_node->type = ExprType::kBinaryOp;
            and_node->op = "AND";
            and_node->left = static_cast<UniquePtr<Expression>&&>(ge);
            and_node->right = static_cast<UniquePtr<Expression>&&>(le);

            auto unary = make_unique<Expression>();
            unary->type = ExprType::kUnaryOp;
            unary->op = "NOT";
            unary->child = static_cast<UniquePtr<Expression>&&>(and_node);
            return static_cast<UniquePtr<Expression>&&>(unary);
        } else {
            // NOT followed by something unexpected — consume it anyway and
            // wrap the next comparison in a NOT unary (same as parse_not_expr behavior).
            auto child = static_cast<UniquePtr<Expression>&&>(parse_addition());
            auto unary = make_unique<Expression>();
            unary->type = ExprType::kUnaryOp;
            unary->op = "NOT";
            unary->child = static_cast<UniquePtr<Expression>&&>(child);
            return static_cast<UniquePtr<Expression>&&>(unary);
        }
    }

    // IN (...) — desugar into OR chain, or subquery
    if (match_keyword(TokenType::KW_IN)) {
        expect(TokenType::LPAREN);

        // Check if it's a subquery: IN (SELECT ...)
        if (check_keyword(TokenType::KW_SELECT)) {
            auto sub = parse_select();
            expect(TokenType::RPAREN);
            auto expr = make_unique<Expression>();
            expr->type = ExprType::kSubquery;
            expr->subquery = static_cast<UniquePtr<SelectStmt>&&>(sub);
            // Build: left IN subquery → represent with special BinaryOp
            auto bin = make_unique<Expression>();
            bin->type = ExprType::kBinaryOp;
            bin->op = "IN_SUBQUERY";
            bin->left = static_cast<UniquePtr<Expression>&&>(left);
            bin->right = static_cast<UniquePtr<Expression>&&>(expr);
            return static_cast<UniquePtr<Expression>&&>(bin);
        }

        // Value list: IN (1, 2, 3)
        auto first_val = static_cast<UniquePtr<Expression>&&>(parse_expression());

        // Build first equality: left = first_val
        auto eq = make_unique<Expression>();
        eq->type = ExprType::kBinaryOp;
        eq->op = "=";
        eq->left = UniquePtr<Expression>(left->clone());
        eq->right = static_cast<UniquePtr<Expression>&&>(first_val);
        auto result = static_cast<UniquePtr<Expression>&&>(eq);

        // Parse remaining values, chain with OR
        while (match(TokenType::COMMA)) {
            auto val = static_cast<UniquePtr<Expression>&&>(parse_expression());
            auto eq2 = make_unique<Expression>();
            eq2->type = ExprType::kBinaryOp;
            eq2->op = "=";
            eq2->left = UniquePtr<Expression>(left->clone());
            eq2->right = static_cast<UniquePtr<Expression>&&>(val);
            auto or_node = make_unique<Expression>();
            or_node->type = ExprType::kBinaryOp;
            or_node->op = "OR";
            or_node->left = static_cast<UniquePtr<Expression>&&>(result);
            or_node->right = static_cast<UniquePtr<Expression>&&>(eq2);
            result = static_cast<UniquePtr<Expression>&&>(or_node);
        }

        expect(TokenType::RPAREN);
        return result;
    }

    // BETWEEN x AND y — desugar to x >= low AND x <= high
    if (match_keyword(TokenType::KW_BETWEEN)) {
        auto low = static_cast<UniquePtr<Expression>&&>(parse_addition());
        expect_keyword(TokenType::KW_AND);
        auto high = static_cast<UniquePtr<Expression>&&>(parse_addition());

        auto ge = make_unique<Expression>();
        ge->type = ExprType::kBinaryOp;
        ge->op = ">=";
        ge->left = UniquePtr<Expression>(left->clone());
        ge->right = static_cast<UniquePtr<Expression>&&>(low);

        auto le = make_unique<Expression>();
        le->type = ExprType::kBinaryOp;
        le->op = "<=";
        le->left = UniquePtr<Expression>(left->clone());
        le->right = static_cast<UniquePtr<Expression>&&>(high);

        auto and_node = make_unique<Expression>();
        and_node->type = ExprType::kBinaryOp;
        and_node->op = "AND";
        and_node->left = static_cast<UniquePtr<Expression>&&>(ge);
        and_node->right = static_cast<UniquePtr<Expression>&&>(le);
        return static_cast<UniquePtr<Expression>&&>(and_node);
    }

    return left;
}

UniquePtr<Expression> Parser::parse_addition() {
    auto left = static_cast<UniquePtr<Expression>&&>(parse_multiplication());
    while (check(TokenType::PLUS) || check(TokenType::MINUS)) {
        Token op = peek();
        lexer_.consume_token();
        auto right = static_cast<UniquePtr<Expression>&&>(parse_multiplication());
        auto bin = make_unique<Expression>();
        bin->type = ExprType::kBinaryOp;
        bin->op = op.value;
        bin->left = static_cast<UniquePtr<Expression>&&>(left);
        bin->right = static_cast<UniquePtr<Expression>&&>(right);
        left = static_cast<UniquePtr<Expression>&&>(bin);
    }
    return left;
}

UniquePtr<Expression> Parser::parse_multiplication() {
    auto left = static_cast<UniquePtr<Expression>&&>(parse_unary());
    while (check(TokenType::STAR) || check(TokenType::SLASH) || check(TokenType::PERCENT)) {
        Token op = peek();
        lexer_.consume_token();
        auto right = static_cast<UniquePtr<Expression>&&>(parse_unary());
        auto bin = make_unique<Expression>();
        bin->type = ExprType::kBinaryOp;
        bin->op = op.value;
        bin->left = static_cast<UniquePtr<Expression>&&>(left);
        bin->right = static_cast<UniquePtr<Expression>&&>(right);
        left = static_cast<UniquePtr<Expression>&&>(bin);
    }
    return left;
}

UniquePtr<Expression> Parser::parse_unary() {
    if (check(TokenType::MINUS)) {
        lexer_.consume_token();
        auto child = static_cast<UniquePtr<Expression>&&>(parse_primary());
        auto unary = make_unique<Expression>();
        unary->type = ExprType::kUnaryOp;
        unary->op = "-";
        unary->child = static_cast<UniquePtr<Expression>&&>(child);
        return unary;
    }
    return parse_primary();
}

UniquePtr<Expression> Parser::parse_primary() {
    Token t = peek();

    // literal
    if (t.type == TokenType::INT_LITERAL) {
        lexer_.consume_token();
        auto expr = make_unique<Expression>();
        expr->type = ExprType::kLiteral;
        i64 v = static_cast<i64>(atoll(t.value.c_str()));
        if (v >= static_cast<i64>(-2147483647 - 1) && v <= static_cast<i64>(2147483647)) {
            expr->literal_value = Value(static_cast<i32>(v));
        } else {
            expr->literal_value = Value(v);
        }
        return expr;
    }

    if (t.type == TokenType::FLOAT_LITERAL) {
        lexer_.consume_token();
        auto expr = make_unique<Expression>();
        expr->type = ExprType::kLiteral;
        expr->literal_value = Value(static_cast<float>(atof(t.value.c_str())));
        return expr;
    }

    if (t.type == TokenType::STRING_LITERAL) {
        lexer_.consume_token();
        auto expr = make_unique<Expression>();
        expr->type = ExprType::kLiteral;
        expr->literal_value = Value(t.value);
        return expr;
    }

    if (match_keyword(TokenType::KW_TRUE)) {
        auto expr = make_unique<Expression>();
        expr->type = ExprType::kLiteral;
        expr->literal_value = Value(true);
        return expr;
    }

    if (match_keyword(TokenType::KW_FALSE)) {
        auto expr = make_unique<Expression>();
        expr->type = ExprType::kLiteral;
        expr->literal_value = Value(false);
        return expr;
    }

    if (match_keyword(TokenType::KW_NULL)) {
        auto expr = make_unique<Expression>();
        expr->type = ExprType::kLiteral;
        expr->literal_value = Value();
        return expr;
    }

    // CASE WHEN ... THEN ... ELSE ... END
    if (match_keyword(TokenType::KW_CASE)) {
        auto expr = make_unique<Expression>();
        expr->type = ExprType::kCase;
        while (match_keyword(TokenType::KW_WHEN)) {
            auto cond = static_cast<UniquePtr<Expression>&&>(parse_expression());
            expect_keyword(TokenType::KW_THEN);
            auto result = static_cast<UniquePtr<Expression>&&>(parse_expression());
            Pair<UniquePtr<Expression>, UniquePtr<Expression>> clause;
            clause.first = static_cast<UniquePtr<Expression>&&>(cond);
            clause.second = static_cast<UniquePtr<Expression>&&>(result);
            expr->when_clauses.push_back(static_cast<Pair<UniquePtr<Expression>, UniquePtr<Expression>>&&>(clause));
        }
        if (match_keyword(TokenType::KW_ELSE)) {
            expr->else_expr = static_cast<UniquePtr<Expression>&&>(parse_expression());
        }
        expect_keyword(TokenType::KW_END);
        return expr;
    }

    // CAST(expr AS type)
    if (match_keyword(TokenType::KW_CAST)) {
        expect(TokenType::LPAREN);
        auto source = static_cast<UniquePtr<Expression>&&>(parse_expression());
        expect_keyword(TokenType::KW_AS);
        TypeId target = TypeId::kNull;
        if (match_keyword(TokenType::KW_INT))         target = TypeId::kInt32;
        else if (match_keyword(TokenType::KW_INT64))   target = TypeId::kInt64;
        else if (match_keyword(TokenType::KW_FLOAT))   target = TypeId::kFloat;
        else if (match_keyword(TokenType::KW_DOUBLE))  target = TypeId::kDouble;
        else if (match_keyword(TokenType::KW_BOOL))    target = TypeId::kBool;
        else if (match_keyword(TokenType::KW_VARCHAR))  { target = TypeId::kVarchar; if (match(TokenType::LPAREN)) { expect(TokenType::INT_LITERAL); expect(TokenType::RPAREN); } }
        else if (match_keyword(TokenType::KW_TEXT))    target = TypeId::kVarchar;
        else if (match_keyword(TokenType::KW_TIMESTAMP)) target = TypeId::kTimestamp;
        else if (match_keyword(TokenType::KW_DATETIME))  target = TypeId::kDatetime;
        else { set_error("expected type name after AS"); }
        expect(TokenType::RPAREN);
        auto expr = make_unique<Expression>();
        expr->type = ExprType::kCast;
        expr->child = static_cast<UniquePtr<Expression>&&>(source);
        expr->cast_target = target;
        return expr;
    }

    // COALESCE(a, b, c) → left-fold
    if (match_keyword(TokenType::KW_COALESCE)) {
        expect(TokenType::LPAREN);
        auto result = static_cast<UniquePtr<Expression>&&>(parse_expression());
        while (match(TokenType::COMMA)) {
            auto alt = static_cast<UniquePtr<Expression>&&>(parse_expression());
            auto coalesce = make_unique<Expression>();
            coalesce->type = ExprType::kBinaryOp;
            coalesce->op = "COALESCE";
            coalesce->left = static_cast<UniquePtr<Expression>&&>(result);
            coalesce->right = static_cast<UniquePtr<Expression>&&>(alt);
            result = static_cast<UniquePtr<Expression>&&>(coalesce);
        }
        expect(TokenType::RPAREN);
        return result;
    }

    // NULLIF(a, b)
    if (match_keyword(TokenType::KW_NULLIF)) {
        expect(TokenType::LPAREN);
        auto a = static_cast<UniquePtr<Expression>&&>(parse_expression());
        expect(TokenType::COMMA);
        auto b = static_cast<UniquePtr<Expression>&&>(parse_expression());
        expect(TokenType::RPAREN);
        auto expr = make_unique<Expression>();
        expr->type = ExprType::kBinaryOp;
        expr->op = "NULLIF";
        expr->left = static_cast<UniquePtr<Expression>&&>(a);
        expr->right = static_cast<UniquePtr<Expression>&&>(b);
        return expr;
    }

    if (t.type == TokenType::KW_COUNT || t.type == TokenType::KW_SUM ||
        t.type == TokenType::KW_AVG || t.type == TokenType::KW_MIN ||
        t.type == TokenType::KW_MAX) {
        lexer_.consume_token();
        if (!match(TokenType::LPAREN)) {
            auto expr = make_unique<Expression>();
            expr->type = ExprType::kColumnRef;
            expr->column_name = t.value;
            return expr;
        }
        auto expr = make_unique<Expression>();
        expr->type = ExprType::kBinaryOp;
        if (t.type == TokenType::KW_COUNT) expr->op = "COUNT";
        else if (t.type == TokenType::KW_SUM) expr->op = "SUM";
        else if (t.type == TokenType::KW_AVG) expr->op = "AVG";
        else if (t.type == TokenType::KW_MIN) expr->op = "MIN";
        else if (t.type == TokenType::KW_MAX) expr->op = "MAX";
        bool distinct_arg = match_keyword(TokenType::KW_DISTINCT);
        if (!match(TokenType::STAR)) {
            expr->left = static_cast<UniquePtr<Expression>&&>(parse_expression());
        }
        if (distinct_arg) expr->op += "_DISTINCT";
        expect(TokenType::RPAREN);
        return expr;
    }

    // parenthesized expression
    if (match(TokenType::LPAREN)) {
        if (check_keyword(TokenType::KW_SELECT)) {
            auto sub = parse_select();
            expect(TokenType::RPAREN);
            auto expr = make_unique<Expression>();
            expr->type = ExprType::kSubquery;
            expr->subquery = static_cast<UniquePtr<SelectStmt>&&>(sub);
            return expr;
        }
        auto expr = static_cast<UniquePtr<Expression>&&>(parse_expression());
        expect(TokenType::RPAREN);
        return expr;
    }

    // star
    if (match(TokenType::STAR)) {
        auto expr = make_unique<Expression>();
        expr->type = ExprType::kStar;
        return expr;
    }

    // Simple function call: LENGTH(expr). If LENGTH appears without parentheses,
    // treat it as a normal identifier/column name.
    if (is_identifier_token(t.type) && upper_ascii(t.value) == "LENGTH") {
        lexer_.consume_token();
        if (!match(TokenType::LPAREN)) {
            auto expr = make_unique<Expression>();
            expr->type = ExprType::kColumnRef;
            expr->column_name = t.value;
            return expr;
        }
        auto arg = static_cast<UniquePtr<Expression>&&>(parse_expression());
        expect(TokenType::RPAREN);
        auto expr = make_unique<Expression>();
        expr->type = ExprType::kUnaryOp;
        expr->op = "LENGTH";
        expr->child = static_cast<UniquePtr<Expression>&&>(arg);
        return expr;
    }

    // column ref (table.column or column)
    if (is_identifier_token(t.type)) {
        lexer_.consume_token();
        auto expr = make_unique<Expression>();
        expr->type = ExprType::kColumnRef;
        if (check(TokenType::DOT)) {
            lexer_.consume_token();
            expr->table_name = t.value;
            expr->column_name = expect_identifier().value;
        } else {
            expr->column_name = t.value;
        }
        return expr;
    }

    // default: unrecognized token in expression — return NULL literal
    // (Caller should check for parse errors via trailing garbage detection in parse())
    Token bad = peek();
    mark_error();
    if (bad.type != TokenType::END_OF_INPUT) {
        lexer_.consume_token();  // consume the unexpected token to avoid infinite loop
    }
    auto expr = make_unique<Expression>();
    expr->type = ExprType::kLiteral;
    return expr;
}

// ============================================================
// Utility helpers.
// ============================================================

Token Parser::expect(TokenType type) {
    Token t = peek();
    if (t.type != type) {
        set_error_at(String("expected different token"), t);
        // Consume the bad token to avoid spinning.
        lexer_.consume_token();
        return Token(TokenType::ERROR, "unexpected token", t.line, t.column);
    }
    lexer_.consume_token();
    return t;
}

Token Parser::expect_keyword(TokenType kw) {
    Token t = peek();
    if (t.type != kw) {
        set_error_at(String("expected keyword"), t);
        lexer_.consume_token();
        return Token(TokenType::ERROR, "unexpected keyword", t.line, t.column);
    }
    lexer_.consume_token();
    return t;
}

Token Parser::expect_alias() {
    Token t = peek();
    if (is_identifier_token(t.type)) {
        lexer_.consume_token();
        return t;
    }
    set_error_at(String("expected identifier"), t);
    lexer_.consume_token();
    return Token(TokenType::ERROR, "unexpected token", t.line, t.column);
}

Token Parser::expect_identifier() {
    return expect_alias();
}

bool Parser::match(TokenType type) {
    if (check(type)) {
        lexer_.consume_token();
        return true;
    }
    return false;
}

bool Parser::match_keyword(TokenType kw) {
    if (check_keyword(kw)) {
        lexer_.consume_token();
        return true;
    }
    return false;
}

Token Parser::peek() {
    return lexer_.peek_token();
}

bool Parser::check(TokenType type) {
    return peek().type == type;
}

bool Parser::check_keyword(TokenType kw) {
    return peek().type == kw;
}

bool Parser::check_identifier() {
    return is_identifier_token(peek().type);
}

bool Parser::is_identifier_token(TokenType type) const {
    return type == TokenType::IDENTIFIER ||
           (type >= TokenType::KW_SELECT && type <= TokenType::KW_DEALLOCATE);
}

void Parser::mark_error() {
    if (ok_) {
        // Capture the current token so the REPL can quote it. Callers that
        // know more about the failure should use set_error_at().
        Token t = peek();
        set_error_at(String("syntax error"), t);
    } else {
        ok_ = false;
    }
}

void Parser::set_error(const String& message) {
    if (ok_) {
        ok_ = false;
        error_.message = message;
        error_.near = String("");
        error_.line = 0;
        error_.column = 0;
    }
}

void Parser::set_error_at(const String& message, const Token& tok) {
    if (ok_) {
        ok_ = false;
        error_.message = message;
        if (tok.type == TokenType::END_OF_INPUT) {
            error_.near = String("<end of input>");
        } else if (!tok.value.empty()) {
            error_.near = tok.value;
        } else {
            error_.near = String("<unknown>");
        }
        error_.line = tok.line;
        error_.column = tok.column;
    }
}

} // namespace minidb
