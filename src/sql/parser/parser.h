/**
 * @file parser.h
 * @brief Recursive descent parser — Parse Token stream into AST
 */
#pragma once

#include "common/defs.h"
#include "container/string.h"
#include "container/vector.h"
#include "container/unique_ptr.h"
#include "sql/parser/token.h"
#include "sql/parser/lexer.h"
#include "sql/parser/ast.h"

namespace minidb {

class Parser {
public:
    explicit Parser(const String& sql);

    Statement parse();

private:
    // Statement parsers.
    UniquePtr<SelectStmt> parse_select();
    UniquePtr<SelectStmt> parse_select_body();
    UniquePtr<InsertStmt> parse_insert();
    UniquePtr<UpdateStmt> parse_update();
    UniquePtr<DeleteStmt> parse_delete();
    UniquePtr<CreateTableStmt> parse_create_table();
    UniquePtr<CreateIndexStmt> parse_create_index();
    UniquePtr<DescTableStmt> parse_desc_table();
    UniquePtr<AlterTableStmt> parse_alter_table();
    UniquePtr<PrepareStmt> parse_prepare();
    UniquePtr<ExecuteStmt> parse_execute();
    UniquePtr<DeallocateStmt> parse_deallocate();
    String parse_drop_table();
    String parse_drop_index();

    // Table expression parsing (precedence climbing)
    UniquePtr<Expression> parse_expression();
    UniquePtr<Expression> parse_or_expr();
    UniquePtr<Expression> parse_and_expr();
    UniquePtr<Expression> parse_not_expr();
    UniquePtr<Expression> parse_comparison();
    UniquePtr<Expression> parse_addition();
    UniquePtr<Expression> parse_multiplication();
    UniquePtr<Expression> parse_unary();
    UniquePtr<Expression> parse_primary();

    // Utility
    Token expect(TokenType type);
    Token expect_keyword(TokenType kw);
    Token expect_alias();
    Token expect_identifier();
    bool match(TokenType type);
    bool match_keyword(TokenType kw);
    Token peek();
    bool check(TokenType type);
    bool check_keyword(TokenType kw);
    bool check_identifier();
    bool is_identifier_token(TokenType type) const;
    void mark_error();

    Lexer lexer_;
    bool ok_;
};

} // namespace minidb
