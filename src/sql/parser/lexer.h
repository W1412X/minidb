/**
 * @file lexer.h
 * @brief Lexical analyzer — splits SQL text into a Token stream.
 */
#pragma once

#include "common/defs.h"
#include "container/string.h"
#include "container/hash_map.h"
#include "sql/parser/token.h"

namespace minidb {

class Lexer {
public:
    explicit Lexer(const String& sql);

    Token next_token();
    Token peek_token();
    void consume_token();

    // For the parser's slice-the-source case (CHECK constraint text). The
    // byte position points at the next un-tokenised character; the source
    // is the original SQL we were constructed with.
    const String& source() const { return sql_; }
    u32 byte_pos() const { return pos_; }

private:
    void skip_whitespace();
    void skip_comment();

    Token read_number();
    Token read_string();
    Token read_identifier();
    Token read_quoted_identifier();

    char peek_char() const;
    char next_char();
    bool has_more() const;

    String sql_;
    u32 pos_;
    u32 line_;
    u32 column_;
    Token peeked_token_;
    bool has_peeked_;
};

} // namespace minidb
