/**
 * @file token.h
 * @brief Token definition — output unit of SQL lexical analysis.
 */
#pragma once

#include "common/defs.h"
#include "container/string.h"

namespace minidb {

enum class TokenType : u16 {
    // Keywords.
    KW_SELECT, KW_INSERT, KW_UPDATE, KW_DELETE, KW_EXPLAIN, KW_ANALYZE,
    KW_CREATE, KW_DROP, KW_TABLE, KW_INDEX, KW_ON,
    KW_FROM, KW_WHERE, KW_INTO, KW_VALUES, KW_SET,
    KW_AND, KW_OR, KW_NOT, KW_IS, KW_NULL,
    KW_PRIMARY, KW_KEY, KW_UNIQUE,
    KW_INT, KW_INT64, KW_FLOAT, KW_DOUBLE,
    KW_VARCHAR, KW_BOOL, KW_TEXT,
    KW_TRUE, KW_FALSE,
    KW_BEGIN, KW_COMMIT, KW_ROLLBACK, KW_TRANSACTION,
    KW_INNER, KW_JOIN, KW_LEFT, KW_RIGHT, KW_OUTER, KW_CROSS,
    KW_GROUP, KW_BY, KW_ORDER, KW_HAVING,
    KW_ASC, KW_DESC, KW_LIMIT, KW_OFFSET,
    KW_COUNT, KW_SUM, KW_AVG, KW_MIN, KW_MAX,
    KW_IN, KW_LIKE, KW_BETWEEN,
    KW_AS, KW_DISTINCT,
    KW_SHOW, KW_TABLES, KW_DESCRIBE, KW_COLUMNS, KW_STATUS, KW_UNION, KW_ALL,
    KW_CASE, KW_WHEN, KW_THEN, KW_ELSE, KW_END, KW_CAST, KW_COALESCE, KW_NULLIF,
    KW_ALTER, KW_ADD, KW_COLUMN, KW_RENAME, KW_DEFAULT, KW_TO,
    KW_PREPARE, KW_EXECUTE, KW_DEALLOCATE,
    KW_CHECK,
    KW_NULLS, KW_FIRST, KW_LAST,
    KW_VACUUM,

    // Symbol
    PLUS, MINUS, STAR, SLASH, PERCENT,
    EQ, NEQ, LT, GT, LTE, GTE,
    LPAREN, RPAREN, COMMA, SEMICOLON, DOT,
    ASSIGN,

    // Literals.
    INT_LITERAL,
    FLOAT_LITERAL,
    STRING_LITERAL,
    IDENTIFIER,

    // Special / punctuation.
    END_OF_INPUT,
    ERROR,
};

struct Token {
    TokenType   type;
    String      value;
    u32         line;
    u32         column;

    Token() : type(TokenType::ERROR), line(0), column(0) {}
    Token(TokenType t, const String& v, u32 l, u32 c)
        : type(t), value(v), line(l), column(c) {}
};

} // namespace minidb
