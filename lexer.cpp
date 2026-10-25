
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

enum class TokenType {
    // General
    IDENTIFIER,
    RESULT,
    INTEGER_LITERAL,
    FLOAT_LITERAL, 
    STRING_LITERAL, 

    // Literals
    TRUE_LITERAL,
    FALSE_LITERAL,
    NULL_LITERAL,

    // Keywords / declarations
    KW_STRUCT,
    KW_UNION,
    KW_GLOBAL,
    KW_CONSTANT,
    KW_EXTERN,
    KW_DEF,
    KW_END,
    KW_ALIGN,

    // Terminators
    KW_RET,
    KW_JMP,
    KW_BR_IF,
    KW_UNREACHABLE,

    // Instructions
    KW_CONST,
    KW_ALLOCA,
    KW_LOAD,
    KW_STORE,
    KW_VOLATILE,
    KW_FIELD_ADDR,
    KW_ELEM_ADDR,
    KW_CALL,
    KW_CALL_INDIRECT,
    KW_CAST,
    KW_SYSCALL, 

    // Atomic instructions
    KW_ATOMIC_LOAD,
    KW_ATOMIC_STORE,
    KW_ATOMIC_RMW,
    KW_ATOMIC_CAS,

    // Integer operations
    OP_NEG,
    OP_NOT,
    OP_BITNOT,

    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_REM,
    OP_AND,
    OP_OR,
    OP_XOR,
    OP_SHL,
    OP_SHR,

    OP_EQ,
    OP_NE,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,

    OP_FADD,
    OP_FSUB,
    OP_FMUL,
    OP_FDIV,
    OP_FREM,

    OP_FCMP,

    OP_XCHG,

    // Atomic orderings
    ORDER_RELAXED,
    ORDER_ACQUIRE,
    ORDER_RELEASE,
    ORDER_ACQ_REL,
    ORDER_SEQ_CST,

    // Types
    TYPE_I8,
    TYPE_I16,
    TYPE_I32,
    TYPE_I64,

    TYPE_U8,
    TYPE_U16,
    TYPE_U32,
    TYPE_U64,

    TYPE_ISIZE,
    TYPE_USIZE,

    TYPE_F32,
    TYPE_F64,

    TYPE_BOOL,
    TYPE_PTR,
    TYPE_VOID,

    // Punctuation
    EQUAL,
    DOT,
    COMMA,
    COLON,
    LPAREN,
    RPAREN,
    LBRACKET,
    RBRACKET,
    MINUS,

    END_OF_FILE,
    INVALID
};

struct Token {
    TokenType type;
    std::string value;
    std::size_t line;
    std::size_t column;
    std::string file;
};

class Lexer {
public:
    Lexer(std::string source, std::string filename = "<input>")
        : source_(std::move(source)),
          filename_(std::move(filename)) {}

    std::vector<Token> tokenize() {
        tokens_.clear();
        pos_ = 0;
        line_ = 1;
        column_ = 1;

        while (!isAtEnd()) {
            skipTrivia();

            if (isAtEnd())
                break;

            const std::size_t start = pos_;
            const std::size_t startLine = line_;
            const std::size_t startColumn = column_;
            const char c = peek();

            if (c == '@') {
                escapedIdentifier();
            } else if (isIdentifierStart(c)) {
                identifier();
            } else if (std::isdigit(static_cast<unsigned char>(c))) {
                number();
            } else if (c == '%') {
                result();
            } else if (c == '"') {
                stringLiteral();
            } else {
                punctuationOrError();
            }

            // Prevent accidental infinite loops if a branch fails
            // to consume input.
            if (pos_ == start) {
                errorAt(startLine, startColumn,
                        "Lexer did not consume input");
            }
        }

        tokens_.push_back({
            TokenType::END_OF_FILE,
            "",
            line_,
            column_,
            filename_
        });

        return tokens_;
    }

private:
    std::string source_;
    std::string filename_;
    std::vector<Token> tokens_;

    std::size_t pos_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;

    static const std::unordered_map<std::string, TokenType>
    keyword_map_;

    bool isAtEnd() const {
        return pos_ >= source_.size();
    }

    char peek(std::size_t offset = 0) const {
        const std::size_t index = pos_ + offset;
        if (index >= source_.size())
            return '\0';

        return source_[index];
    }

    char advance() {
        if (isAtEnd())
            return '\0';

        const char c = source_[pos_++];

        if (c == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }

        return c;
    }

    bool match(char expected) {
        if (isAtEnd() || peek() != expected)
            return false;

        advance();
        return true;
    }

    static bool isIdentifierStart(char c) {
        const unsigned char uc = static_cast<unsigned char>(c);
        return std::isalpha(uc) || c == '_';
    }

    static bool isIdentifierContinue(char c) {
        const unsigned char uc = static_cast<unsigned char>(c);
        return std::isalnum(uc) || c == '_';
    }

    void emit(TokenType type, std::size_t start,
              std::size_t startLine, std::size_t startColumn) {
        tokens_.push_back({
            type,
            source_.substr(start, pos_ - start),
            startLine,
            startColumn,
            filename_
        });
    }

    [[noreturn]] void errorAt(std::size_t line,
                              std::size_t column,
                              const std::string& message) const {
        throw std::runtime_error(
            filename_ + ":" + std::to_string(line) + ":" +
            std::to_string(column) + ": " + message
        );
    }

    void skipTrivia() {
        while (!isAtEnd()) {
            // Whitespace
            if (std::isspace(
                    static_cast<unsigned char>(peek()))) {
                advance();
                continue;
            }

            // Single-line comment: # comment
            // A ## pair starts a multiline comment instead.
            if (peek() == '#' && peek(1) != '#') {
                while (!isAtEnd() && peek() != '\n')
                    advance();

                continue;
            }

            // Multiline comment: ## comment ##
            if (peek() == '#' && peek(1) == '#') {
                const std::size_t commentLine = line_;
                const std::size_t commentColumn = column_;

                advance();
                advance();

                bool closed = false;

                while (!isAtEnd()) {
                    if (peek() == '#' && peek(1) == '#') {
                        advance();
                        advance();
                        closed = true;
                        break;
                    }

                    advance();
                }

                if (!closed) {
                    errorAt(commentLine, commentColumn,
                             "Unterminated multiline comment");
                }

                continue;
            }

            break;
        }
    }

    void escapedIdentifier() {
    const std::size_t start = pos_;
    const std::size_t startLine = line_;
    const std::size_t startColumn = column_;

    advance(); // Skip '@'

    if (!isIdentifierStart(peek())) {
        errorAt(startLine, startColumn,
                "Expected identifier after '@'");
    }

    const std::size_t nameStart = pos_;

    while (isIdentifierContinue(peek())) {
        advance();
    }

    tokens_.push_back({
        TokenType::IDENTIFIER,
        source_.substr(nameStart, pos_ - nameStart),
        startLine,
        startColumn,
        filename_
    });
}

    void identifier() {
        const std::size_t start = pos_;
        const std::size_t startLine = line_;
        const std::size_t startColumn = column_;

        while (isIdentifierContinue(peek()))
            advance();

        const std::string text =
            source_.substr(start, pos_ - start);

        const auto it = keyword_map_.find(text);

        emit(it != keyword_map_.end()
                 ? it->second
                 : TokenType::IDENTIFIER,
             start, startLine, startColumn);
    }

    void result() {
        const std::size_t start = pos_;
        const std::size_t startLine = line_;
        const std::size_t startColumn = column_;

        advance(); // %

        if (!isIdentifierStart(peek())) {
            errorAt(startLine, startColumn,
                    "Expected identifier after '%'");
        }

        while (isIdentifierContinue(peek()))
            advance();

        emit(TokenType::RESULT, start,
             startLine, startColumn);
    }

    void number() {
        const std::size_t start = pos_;
        const std::size_t startLine = line_;
        const std::size_t startColumn = column_;

        // Hexadecimal integer: 0x...
        if (peek() == '0' &&
            (peek(1) == 'x' || peek(1) == 'X')) {
            advance();
            advance();

            const std::size_t digitsStart = pos_;

            while (std::isxdigit(
                       static_cast<unsigned char>(peek()))) {
                advance();
            }

            if (pos_ == digitsStart) {
                errorAt(startLine, startColumn,
                        "Expected hexadecimal digits after 0x");
            }

            emit(TokenType::INTEGER_LITERAL,
                 start, startLine, startColumn);
            return;
        }

        // Binary integer: 0b...
        if (peek() == '0' &&
            (peek(1) == 'b' || peek(1) == 'B')) {
            advance();
            advance();

            const std::size_t digitsStart = pos_;

            while (peek() == '0' || peek() == '1')
                advance();

            if (pos_ == digitsStart) {
                errorAt(startLine, startColumn,
                        "Expected binary digits after 0b");
            }

            // A trailing 2..9 is not valid in a binary literal.
            if (std::isdigit(
                    static_cast<unsigned char>(peek()))) {
                errorAt(line_, column_,
                        "Invalid digit in binary literal");
            }

            emit(TokenType::INTEGER_LITERAL,
                 start, startLine, startColumn);
            return;
        }

        while (std::isdigit(
                   static_cast<unsigned char>(peek()))) {
            advance();
        }

        // Float literal requires digits on both sides of '.'.
        if (peek() == '.' &&
            std::isdigit(
                static_cast<unsigned char>(peek(1)))) {
            advance();

            while (std::isdigit(
                       static_cast<unsigned char>(peek()))) {
                advance();
            }

            emit(TokenType::FLOAT_LITERAL,
                 start, startLine, startColumn);
            return;
        }

        emit(TokenType::INTEGER_LITERAL,
             start, startLine, startColumn);
    }

    void stringLiteral() {
        const std::size_t start = pos_;
        const std::size_t startLine = line_;
        const std::size_t startColumn = column_;

        advance(); // opening quote

        while (!isAtEnd()) {
            if (peek() == '"') {
                advance();

                emit(TokenType::STRING_LITERAL,
                    start, startLine, startColumn);
                return;

            }

            if (peek() == '\n') {
                errorAt(startLine, startColumn,
                        "Unterminated string literal");
            }

            if (peek() == '\\') {
                advance();

                if (isAtEnd()) {
                    errorAt(startLine, startColumn,
                            "Unterminated escape sequence");
                }

                advance();
                continue;
            }

            advance();
        }

        errorAt(startLine, startColumn,
                "Unterminated string literal");
    }

    void punctuationOrError() {
        const std::size_t start = pos_;
        const std::size_t startLine = line_;
        const std::size_t startColumn = column_;

        TokenType type;

        switch (peek()) {
            case '=': type = TokenType::EQUAL; break;
            case '.': type = TokenType::DOT; break;
            case ',': type = TokenType::COMMA; break;
            case ':': type = TokenType::COLON; break;
            case '(': type = TokenType::LPAREN; break;
            case ')': type = TokenType::RPAREN; break;
            case '[': type = TokenType::LBRACKET; break;
            case ']': type = TokenType::RBRACKET; break;
            case '-': type = TokenType::MINUS; break;

            default:
                errorAt(startLine, startColumn,
                        std::string("Unexpected character '") +
                        peek() + "'");
        }

        advance();
        emit(type, start, startLine, startColumn);
    }
};

const std::unordered_map<std::string, TokenType>
Lexer::keyword_map_ = {
    // Declarations
    {"struct", TokenType::KW_STRUCT},
    {"union", TokenType::KW_UNION},
    {"global", TokenType::KW_GLOBAL},
    {"constant", TokenType::KW_CONSTANT},
    {"extern", TokenType::KW_EXTERN},
    {"def", TokenType::KW_DEF},
    {"end", TokenType::KW_END},
    {"align", TokenType::KW_ALIGN},

    // Terminators
    {"ret", TokenType::KW_RET},
    {"jmp", TokenType::KW_JMP},
    {"br_if", TokenType::KW_BR_IF},
    {"unreachable", TokenType::KW_UNREACHABLE},

    // Instructions
    {"const", TokenType::KW_CONST},
    {"alloca", TokenType::KW_ALLOCA},
    {"load", TokenType::KW_LOAD},
    {"store", TokenType::KW_STORE},
    {"volatile", TokenType::KW_VOLATILE},
    {"field", TokenType::KW_FIELD_ADDR},
    {"elem", TokenType::KW_ELEM_ADDR},
    {"call", TokenType::KW_CALL},
    {"cast", TokenType::KW_CAST},
    {"syscall", TokenType::KW_SYSCALL},

    // Atomic instructions
    {"atomic", TokenType::KW_ATOMIC_RMW},

    // Operators
    {"neg", TokenType::OP_NEG},
    {"not", TokenType::OP_NOT},
    {"bitnot", TokenType::OP_BITNOT},

    {"add", TokenType::OP_ADD},
    {"sub", TokenType::OP_SUB},
    {"mul", TokenType::OP_MUL},
    {"div", TokenType::OP_DIV},
    {"rem", TokenType::OP_REM},
    {"and", TokenType::OP_AND},
    {"or", TokenType::OP_OR},
    {"xor", TokenType::OP_XOR},
    {"shl", TokenType::OP_SHL},
    {"shr", TokenType::OP_SHR},

    {"eq", TokenType::OP_EQ},
    {"ne", TokenType::OP_NE},
    {"lt", TokenType::OP_LT},
    {"le", TokenType::OP_LE},
    {"gt", TokenType::OP_GT},
    {"ge", TokenType::OP_GE},

    {"fadd", TokenType::OP_FADD},
    {"fsub", TokenType::OP_FSUB},
    {"fmul", TokenType::OP_FMUL},
    {"fdiv", TokenType::OP_FDIV},
    {"frem", TokenType::OP_FREM},
    {"fcmp", TokenType::OP_FCMP},
    {"xchg", TokenType::OP_XCHG},

    // Atomic ordering
    {"relaxed", TokenType::ORDER_RELAXED},
    {"acquire", TokenType::ORDER_ACQUIRE},
    {"release", TokenType::ORDER_RELEASE},
    {"acq_rel", TokenType::ORDER_ACQ_REL},
    {"seq_cst", TokenType::ORDER_SEQ_CST},

    // Types
    {"i8", TokenType::TYPE_I8},
    {"i16", TokenType::TYPE_I16},
    {"i32", TokenType::TYPE_I32},
    {"i64", TokenType::TYPE_I64},

    {"u8", TokenType::TYPE_U8},
    {"u16", TokenType::TYPE_U16},
    {"u32", TokenType::TYPE_U32},
    {"u64", TokenType::TYPE_U64},

    {"isize", TokenType::TYPE_ISIZE},
    {"usize", TokenType::TYPE_USIZE},

    {"f32", TokenType::TYPE_F32},
    {"f64", TokenType::TYPE_F64},

    {"bool", TokenType::TYPE_BOOL},
    {"ptr", TokenType::TYPE_PTR},
    {"void", TokenType::TYPE_VOID},

    // Literals
    {"true", TokenType::TRUE_LITERAL},
    {"false", TokenType::FALSE_LITERAL},
    {"null", TokenType::NULL_LITERAL},
};

// Example usage:
// int main() {
//     Lexer lexer(sourceText, "example.cru");
//     for (const Token& token : lexer.tokenize()) {
//         std::cout << token.line << ':' << token.column
//                   << "  " << token.value << '\n';
//     }
// }
