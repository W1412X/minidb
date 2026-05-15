/**
 * @file lexer.cpp
 * @brief Lexer implementation
 */
#include "sql/parser/lexer.h"

namespace minidb {

// ============================================================
// Keyword mapping
// ============================================================

static HashMap<String, TokenType> make_keyword_map() {
    HashMap<String, TokenType> m;
    m.insert("SELECT", TokenType::KW_SELECT);
    m.insert("EXPLAIN", TokenType::KW_EXPLAIN);
    m.insert("ANALYZE", TokenType::KW_ANALYZE);
    m.insert("INSERT", TokenType::KW_INSERT);
    m.insert("UPDATE", TokenType::KW_UPDATE);
    m.insert("DELETE", TokenType::KW_DELETE);
    m.insert("CREATE", TokenType::KW_CREATE);
    m.insert("DROP", TokenType::KW_DROP);
    m.insert("TABLE", TokenType::KW_TABLE);
    m.insert("INDEX", TokenType::KW_INDEX);
    m.insert("ON", TokenType::KW_ON);
    m.insert("FROM", TokenType::KW_FROM);
    m.insert("WHERE", TokenType::KW_WHERE);
    m.insert("INTO", TokenType::KW_INTO);
    m.insert("VALUES", TokenType::KW_VALUES);
    m.insert("SET", TokenType::KW_SET);
    m.insert("AND", TokenType::KW_AND);
    m.insert("OR", TokenType::KW_OR);
    m.insert("NOT", TokenType::KW_NOT);
    m.insert("IS", TokenType::KW_IS);
    m.insert("NULL", TokenType::KW_NULL);
    m.insert("PRIMARY", TokenType::KW_PRIMARY);
    m.insert("KEY", TokenType::KW_KEY);
    m.insert("UNIQUE", TokenType::KW_UNIQUE);
    m.insert("INT", TokenType::KW_INT);
    m.insert("INTEGER", TokenType::KW_INT);
    m.insert("BIGINT", TokenType::KW_INT64);
    m.insert("FLOAT", TokenType::KW_FLOAT);
    m.insert("DOUBLE", TokenType::KW_DOUBLE);
    m.insert("VARCHAR", TokenType::KW_VARCHAR);
    m.insert("BOOL", TokenType::KW_BOOL);
    m.insert("BOOLEAN", TokenType::KW_BOOL);
    m.insert("TEXT", TokenType::KW_TEXT);
    m.insert("TRUE", TokenType::KW_TRUE);
    m.insert("FALSE", TokenType::KW_FALSE);
    m.insert("BEGIN", TokenType::KW_BEGIN);
    m.insert("COMMIT", TokenType::KW_COMMIT);
    m.insert("ROLLBACK", TokenType::KW_ROLLBACK);
    m.insert("TRANSACTION", TokenType::KW_TRANSACTION);
    m.insert("INNER", TokenType::KW_INNER);
    m.insert("JOIN", TokenType::KW_JOIN);
    m.insert("LEFT", TokenType::KW_LEFT);
    m.insert("RIGHT", TokenType::KW_RIGHT);
    m.insert("OUTER", TokenType::KW_OUTER);
    m.insert("GROUP", TokenType::KW_GROUP);
    m.insert("BY", TokenType::KW_BY);
    m.insert("ORDER", TokenType::KW_ORDER);
    m.insert("HAVING", TokenType::KW_HAVING);
    m.insert("ASC", TokenType::KW_ASC);
    m.insert("DESC", TokenType::KW_DESC);
    m.insert("LIMIT", TokenType::KW_LIMIT);
    m.insert("OFFSET", TokenType::KW_OFFSET);
    m.insert("COUNT", TokenType::KW_COUNT);
    m.insert("SUM", TokenType::KW_SUM);
    m.insert("AVG", TokenType::KW_AVG);
    m.insert("MIN", TokenType::KW_MIN);
    m.insert("MAX", TokenType::KW_MAX);
    m.insert("IN", TokenType::KW_IN);
    m.insert("LIKE", TokenType::KW_LIKE);
    m.insert("BETWEEN", TokenType::KW_BETWEEN);
    m.insert("AS", TokenType::KW_AS);
    m.insert("DISTINCT", TokenType::KW_DISTINCT);
    m.insert("SHOW", TokenType::KW_SHOW);
    m.insert("TABLES", TokenType::KW_TABLES);
    m.insert("DESCRIBE", TokenType::KW_DESCRIBE);
    m.insert("COLUMNS", TokenType::KW_COLUMNS);
    m.insert("UNION", TokenType::KW_UNION);
    m.insert("ALL", TokenType::KW_ALL);
    m.insert("CASE", TokenType::KW_CASE);
    m.insert("WHEN", TokenType::KW_WHEN);
    m.insert("THEN", TokenType::KW_THEN);
    m.insert("ELSE", TokenType::KW_ELSE);
    m.insert("END", TokenType::KW_END);
    m.insert("CAST", TokenType::KW_CAST);
    m.insert("COALESCE", TokenType::KW_COALESCE);
    m.insert("NULLIF", TokenType::KW_NULLIF);
    m.insert("ALTER", TokenType::KW_ALTER);
    m.insert("ADD", TokenType::KW_ADD);
    m.insert("COLUMN", TokenType::KW_COLUMN);
    m.insert("RENAME", TokenType::KW_RENAME);
    m.insert("DEFAULT", TokenType::KW_DEFAULT);
    m.insert("TO", TokenType::KW_TO);
    m.insert("PREPARE", TokenType::KW_PREPARE);
    m.insert("EXECUTE", TokenType::KW_EXECUTE);
    m.insert("DEALLOCATE", TokenType::KW_DEALLOCATE);
    return m;
}

// ============================================================
// Constructor
// ============================================================

Lexer::Lexer(const String& sql)
    : sql_(sql), pos_(0), line_(1), column_(1), has_peeked_(false) {}

// ============================================================
// Public interface
// ============================================================

Token Lexer::next_token() {
    if (has_peeked_) {
        has_peeked_ = false;
        return peeked_token_;
    }
    skip_whitespace();

    if (!has_more()) {
        return Token(TokenType::END_OF_INPUT, "", line_, column_);
    }

    char c = peek_char();

    // Number
    if (c >= '0' && c <= '9') {
        return read_number();
    }

    // String
    if (c == '\'' || c == '"') {
        return read_string();
    }

    // Quoted identifier (MySQL-style `name`). Double quotes remain string
    // literals for backwards compatibility with existing MiniDB SQL tests.
    if (c == '`') {
        return read_quoted_identifier();
    }

    // Identifier/keyword
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        return read_identifier();
    }

    // Symbol
    u32 start_line = line_;
    u32 start_col = column_;
    next_char();

    switch (c) {
        case '+': return Token(TokenType::PLUS, "+", start_line, start_col);
        case '-':
            if (has_more() && peek_char() == '-') {
                skip_comment();
                return next_token();
            }
            return Token(TokenType::MINUS, "-", start_line, start_col);
        case '*': return Token(TokenType::STAR, "*", start_line, start_col);
        case '/': return Token(TokenType::SLASH, "/", start_line, start_col);
        case '%': return Token(TokenType::PERCENT, "%", start_line, start_col);
        case '(': return Token(TokenType::LPAREN, "(", start_line, start_col);
        case ')': return Token(TokenType::RPAREN, ")", start_line, start_col);
        case ',': return Token(TokenType::COMMA, ",", start_line, start_col);
        case ';': return Token(TokenType::SEMICOLON, ";", start_line, start_col);
        case '.': return Token(TokenType::DOT, ".", start_line, start_col);
        case '=': return Token(TokenType::EQ, "=", start_line, start_col);
        case '!':
            if (has_more() && peek_char() == '=') {
                next_char();
                return Token(TokenType::NEQ, "!=", start_line, start_col);
            }
            return Token(TokenType::ERROR, "!", start_line, start_col);
        case '<':
            if (has_more() && peek_char() == '=') {
                next_char();
                return Token(TokenType::LTE, "<=", start_line, start_col);
            }
            if (has_more() && peek_char() == '>') {
                next_char();
                return Token(TokenType::NEQ, "<>", start_line, start_col);
            }
            return Token(TokenType::LT, "<", start_line, start_col);
        case '>':
            if (has_more() && peek_char() == '=') {
                next_char();
                return Token(TokenType::GTE, ">=", start_line, start_col);
            }
            return Token(TokenType::GT, ">", start_line, start_col);
        default:
            return Token(TokenType::ERROR, String(&c, 1), start_line, start_col);
    }
}

Token Lexer::peek_token() {
    if (!has_peeked_) {
        peeked_token_ = next_token();
        has_peeked_ = true;
    }
    return peeked_token_;
}

void Lexer::consume_token() {
    next_token();
}

// ============================================================
// Internal methods
// ============================================================

void Lexer::skip_whitespace() {
    while (has_more()) {
        char c = peek_char();
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            next_char();
        } else {
            break;
        }
    }
}

void Lexer::skip_comment() {
    // -- 单行注释
    while (has_more() && peek_char() != '\n') {
        next_char();
    }
}

Token Lexer::read_number() {
    u32 start = pos_;
    u32 start_line = line_;
    u32 start_col = column_;
    bool is_float = false;

    while (has_more() && peek_char() >= '0' && peek_char() <= '9') {
        next_char();
    }

    if (has_more() && peek_char() == '.') {
        is_float = true;
        next_char();
        while (has_more() && peek_char() >= '0' && peek_char() <= '9') {
            next_char();
        }
    }

    String value(sql_.c_str() + start, pos_ - start);
    if (is_float) {
        return Token(TokenType::FLOAT_LITERAL, value, start_line, start_col);
    }
    return Token(TokenType::INT_LITERAL, value, start_line, start_col);
}

Token Lexer::read_string() {
    u32 start_line = line_;
    u32 start_col = column_;
    char quote = peek_char();
    next_char();  // skip opening quote

    u32 start = pos_;
    while (has_more() && peek_char() != quote) {
        if (peek_char() == '\\') next_char();  // skip escape
        next_char();
    }

    String value(sql_.c_str() + start, pos_ - start);
    if (has_more()) next_char();  // skip closing quote

    return Token(TokenType::STRING_LITERAL, value, start_line, start_col);
}

Token Lexer::read_quoted_identifier() {
    u32 start_line = line_;
    u32 start_col = column_;
    char quote = peek_char();
    next_char();  // skip opening quote

    String value;
    while (has_more()) {
        char c = peek_char();
        if (c == quote) {
            next_char();
            if (has_more() && peek_char() == quote) {
                value += quote;
                next_char();
                continue;
            }
            break;
        }
        value += next_char();
    }

    return Token(TokenType::IDENTIFIER, value, start_line, start_col);
}

Token Lexer::read_identifier() {
    u32 start = pos_;
    u32 start_line = line_;
    u32 start_col = column_;

    while (has_more()) {
        char c = peek_char();
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            next_char();
        } else {
            break;
        }
    }

    String word(sql_.c_str() + start, pos_ - start);

    // 转大写检查关键词
    String upper = word;
    for (u32 i = 0; i < upper.size(); i++) {
        char& ch = upper[i];
        if (ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
    }

    static HashMap<String, TokenType> keywords = make_keyword_map();
    auto* kw = keywords.find(upper);
    if (kw) {
        return Token(*kw, word, start_line, start_col);
    }

    return Token(TokenType::IDENTIFIER, word, start_line, start_col);
}

char Lexer::peek_char() const {
    return sql_[pos_];
}

char Lexer::next_char() {
    char c = sql_[pos_++];
    if (c == '\n') {
        line_++;
        column_ = 1;
    } else {
        column_++;
    }
    return c;
}

bool Lexer::has_more() const {
    return pos_ < sql_.size();
}

} // namespace minidb
