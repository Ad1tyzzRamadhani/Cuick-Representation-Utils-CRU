#ifndef CRU_PARSER_HPP
#define CRU_PARSER_HPP

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cru {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens)
        : tokens_(std::move(tokens)) {}

    ast::Module parseModule() {
        ast::Module module;
        module.location = location();

        while (!isAtEnd()) {
            module.declarations.push_back(parseDeclaration());
        }

        return module;
    }

private:
    std::vector<Token> tokens_;
    std::size_t current_ = 0;

    // =========================================================
    // Parser Core
    // =========================================================

    const Token& peek(std::size_t offset = 0) const {
        const std::size_t index = current_ + offset;

        if (index >= tokens_.size())
            return tokens_.back();

        return tokens_[index];
    }

    const Token& previous() const {
        return tokens_[current_ - 1];
    }

    bool isAtEnd() const {
        return check(TokenType::END_OF_FILE);
    }

    const Token& advance() {
        if (!isAtEnd())
            ++current_;

        return previous();
    }

    bool check(TokenType type) const {
        return peek().type == type;
    }

    bool match(TokenType type) {
        if (!check(type))
            return false;

        advance();
        return true;
    }

    const Token& expect(
        TokenType type,
        const std::string& message
    ) {
        if (!check(type))
            error(message);

        return advance();
    }

    [[noreturn]]
    void error(const std::string& message) const {
        const Token& token = peek();

        throw std::runtime_error(
            token.file + ":" +
            std::to_string(token.line) + ":" +
            std::to_string(token.column) +
            ": parser error: " +
            message +
            " (got '" +
            token.value +
            "')"
        );
    }

    [[noreturn]]
    void errorAt(
        const Token& token,
        const std::string& message
    ) const {
        throw std::runtime_error(
            token.file + ":" +
            std::to_string(token.line) + ":" +
            std::to_string(token.column) +
            ": parser error: " +
            message +
            " (got '" +
            token.value +
            "')"
        );
    }

    ast::SourceLocation location(
        const Token& token
    ) const {
        return {
            token.file,
            token.line,
            token.column
        };
    }

    ast::SourceLocation location() const {
        return location(peek());
    }

    std::string expectIdentifier(
        const std::string& message =
            "Expected identifier"
    ) {
        if (!check(TokenType::IDENTIFIER))
            error(message);

        return advance().value;
    }

    // =========================================================
    // Result
    // =========================================================

    ast::ResultRef parseResult() {
        const Token& token =
            expect(
                TokenType::RESULT,
                "Expected result reference"
            );

        ast::ResultRef result;

        result.location = location(token);

        // Lexer produces "%name".
        // AST stores only "name".
        result.name = token.value.substr(1);

        return result;
    }

    // =========================================================
    // Literal
    // =========================================================

    ast::Literal parseLiteral() {
        const Token& start = peek();

        ast::Literal literal;
        literal.location = location(start);

        if (match(TokenType::STRING_LITERAL)) {
            literal.kind = ast::LiteralKind::String;
            literal.text = previous().value;
            return literal;
        }

        if (match(TokenType::CHAR_LITERAL)) {
            literal.kind = ast::LiteralKind::Character;
            
            // Konversi isi text ('C' atau '\n') menjadi nilai integer ASCII-nya
            std::string raw = previous().value; // Termasuk petik tunggal, misal "'C'" atau "'\n'"
            std::string inner = raw.substr(1, raw.size() - 2); // Ambil isi di dalam petik
            
            uint8_t charVal = 0;
            if (inner.size() == 1) {
                charVal = static_cast<uint8_t>(inner[0]);
            } else if (inner.size() == 2 && inner[0] == '\\') {
                switch (inner[1]) {
                case 'n': charVal = '\n'; break;
                case 't': charVal = '\t'; break;
                case 'r': charVal = '\r'; break;
                case '0': charVal = '\0'; break;
                case '\\': charVal = '\\'; break;
                case '\'': charVal = '\''; break;
                default: charVal = static_cast<uint8_t>(inner[1]); break;
                }
            }
            
            // Simpan sebagai string angka ASCII (misal 'A' -> "65")
            literal.text = std::to_string(charVal);
            return literal;
        }



        if (match(TokenType::INTEGER_LITERAL)) {
            literal.kind =
                ast::LiteralKind::Integer;

            literal.text =
                previous().value;

            return literal;
        }

        if (match(TokenType::FLOAT_LITERAL)) {
            literal.kind =
                ast::LiteralKind::Float;

            literal.text =
                previous().value;

            return literal;
        }

        if (match(TokenType::TRUE_LITERAL)) {
            literal.kind =
                ast::LiteralKind::Boolean;

            literal.text =
                previous().value;

            return literal;
        }

        if (match(TokenType::FALSE_LITERAL)) {
            literal.kind =
                ast::LiteralKind::Boolean;

            literal.text =
                previous().value;

            return literal;
        }

        if (match(TokenType::NULL_LITERAL)) {
            literal.kind =
                ast::LiteralKind::Null;

            literal.text =
                previous().value;

            return literal;
        }

        /*
         * Lexer separates:
         *
         * -123
         *
         * into:
         *
         * MINUS
         * INTEGER_LITERAL
         *
         * So parser combines them here.
         */
        if (match(TokenType::MINUS)) {
            const Token& number = peek();

            if (!check(TokenType::INTEGER_LITERAL) &&
                !check(TokenType::FLOAT_LITERAL)) {
                error(
                    "Expected integer or float literal after '-'"
                );
            }

            advance();

            literal.kind =
                number.type == TokenType::FLOAT_LITERAL
                    ? ast::LiteralKind::Float
                    : ast::LiteralKind::Integer;

            literal.text =
                "-" + number.value;

            return literal;
        }

        error("Expected literal");
    }

    // =========================================================
    // Integer literal used by AST uint64 fields
    // =========================================================

    std::uint64_t parseUnsignedInteger(
        const std::string& what
    ) {
        const Token& start = peek();

        if (match(TokenType::MINUS)) {
            errorAt(
                start,
                "Expected non-negative integer for " +
                what
            );
        }

        if (!check(TokenType::INTEGER_LITERAL))
            error(
                "Expected integer literal for " +
                what
            );

        const std::string text =
            advance().value;

        try {
            std::size_t consumed = 0;
            unsigned long long value = 0;

            if (
                text.size() > 2 &&
                text[0] == '0' &&
                (text[1] == 'x' ||
                 text[1] == 'X')
            ) {
                value =
                    std::stoull(
                        text,
                        &consumed,
                        16
                    );
            }
            else if (
                text.size() > 2 &&
                text[0] == '0' &&
                (text[1] == 'b' ||
                 text[1] == 'B')
            ) {
                value =
                    std::stoull(
                        text.substr(2),
                        &consumed,
                        2
                    );

                consumed += 2;
            }
            else {
                value =
                    std::stoull(
                        text,
                        &consumed,
                        10
                    );
            }

            if (consumed != text.size()) {
                errorAt(
                    start,
                    "Invalid integer literal for " +
                    what
                );
            }

            return static_cast<std::uint64_t>(value);
        }
        catch (const std::exception&) {
            errorAt(
                start,
                "Integer literal out of range for " +
                what
            );
        }
    }

    // =========================================================
    // ParserType
    // =========================================================

    ast::Type parseType(
        bool allowVoid = false
    ) {
        /*
         * Array:
         *
         * [10]i32
         * [4]Point
         * [2][8]i32
         */
        if (match(TokenType::LBRACKET)) {
            const std::uint64_t size =
                parseUnsignedInteger(
                    "array size"
                );

            expect(
                TokenType::RBRACKET,
                "Expected ']' after array size"
            );

            ast::Type element =
                parseType(false);

            return ast::Type(
                size,
                std::move(element)
            );
        }

        // Integer

        if (match(TokenType::TYPE_I8))
            return ast::Type(ast::TypeKind::I8);

        if (match(TokenType::TYPE_I16))
            return ast::Type(ast::TypeKind::I16);

        if (match(TokenType::TYPE_I32))
            return ast::Type(ast::TypeKind::I32);

        if (match(TokenType::TYPE_I64))
            return ast::Type(ast::TypeKind::I64);

        if (match(TokenType::TYPE_U8))
            return ast::Type(ast::TypeKind::U8);

        if (match(TokenType::TYPE_U16))
            return ast::Type(ast::TypeKind::U16);

        if (match(TokenType::TYPE_U32))
            return ast::Type(ast::TypeKind::U32);

        if (match(TokenType::TYPE_U64))
            return ast::Type(ast::TypeKind::U64);

        if (match(TokenType::TYPE_ISIZE))
            return ast::Type(ast::TypeKind::ISize);

        if (match(TokenType::TYPE_USIZE))
            return ast::Type(ast::TypeKind::USize);

        // Float

        if (match(TokenType::TYPE_F32))
            return ast::Type(ast::TypeKind::F32);

        if (match(TokenType::TYPE_F64))
            return ast::Type(ast::TypeKind::F64);

        // Other builtins

        if (match(TokenType::TYPE_BOOL))
            return ast::Type(ast::TypeKind::Bool);

        if (match(TokenType::TYPE_PTR))
            return ast::Type(ast::TypeKind::Ptr);

        if (match(TokenType::TYPE_VOID)) {
            if (!allowVoid) {
                errorAt(
                    previous(),
                    "'void' is only valid as a function return type"
                );
            }

            return ast::Type(ast::TypeKind::Void);
        }

        // Named type

        if (check(TokenType::IDENTIFIER)) {
            return ast::Type(
                expectIdentifier(
                    "Expected type name"
                )
            );
        }

        error("Expected type");
    }

    ast::Type parseReturnType() {
        return parseType(true);
    }

    // =========================================================
    // ParserOperand
    // =========================================================

    ast::Operand parseOperand() {
        if (check(TokenType::RESULT))
            return parseResult();

        return parseLiteral();
    }

    // =========================================================
    // Align
    // =========================================================

    std::optional<ast::AlignAttr>
    parseOptionalAlign() {
        if (!match(TokenType::KW_ALIGN))
            return std::nullopt;

        ast::AlignAttr attr;

        attr.value =
            parseUnsignedInteger(
                "alignment"
            );

        return attr;
    }

    // =========================================================
    // Declaration
    // =========================================================

    ast::Declaration parseDeclaration() {
        switch (peek().type) {
        case TokenType::KW_STRUCT:
            return parseStruct();

        case TokenType::KW_UNION:
            return parseUnion();

        case TokenType::KW_GLOBAL:
            return parseGlobal(
                ast::GlobalKind::Mutable
            );

        case TokenType::KW_CONSTANT:
            return parseGlobal(
                ast::GlobalKind::Constant
            );

        case TokenType::KW_EXTERN:
            return parseExtern();

        case TokenType::KW_DEF:
            return parseFunction();

        default:
            error("Expected declaration");
        }
    }

    // =========================================================
    // Struct
    // =========================================================

    ast::StructDecl parseStruct() {
        const Token& start =
            expect(
                TokenType::KW_STRUCT,
                "Expected 'struct'"
            );

        ast::StructDecl node;

        node.location =
            location(start);

        node.name =
            expectIdentifier(
                "Expected struct name"
            );

        node.alignment =
            parseOptionalAlign();

        expect(
            TokenType::COLON,
            "Expected ':' after struct header"
        );

        while (!check(TokenType::KW_END)) {
            node.fields.push_back(
                parseField()
            );
        }

        expect(
            TokenType::KW_END,
            "Expected 'end' after struct"
        );

        return node;
    }

    // =========================================================
    // Union
    // =========================================================

    ast::UnionDecl parseUnion() {
        const Token& start =
            expect(
                TokenType::KW_UNION,
                "Expected 'union'"
            );

        ast::UnionDecl node;

        node.location =
            location(start);

        node.name =
            expectIdentifier(
                "Expected union name"
            );

        node.alignment =
            parseOptionalAlign();

        expect(
            TokenType::COLON,
            "Expected ':' after union header"
        );

        while (!check(TokenType::KW_END)) {
            node.fields.push_back(
                parseField()
            );
        }

        expect(
            TokenType::KW_END,
            "Expected 'end' after union"
        );

        return node;
    }

    // =========================================================
    // Field
    // =========================================================

    ast::FieldDecl parseField() {
        const Token& start = peek();

        ast::FieldDecl field;

        field.location =
            location(start);

        field.name =
            expectIdentifier(
                "Expected field name"
            );

        expect(
            TokenType::COLON,
            "Expected ':' after field name"
        );

        field.type =
            parseType(false);

        field.alignment =
            parseOptionalAlign();

        return field;
    }

    // =========================================================
    // Global / Constant
    // =========================================================

    ast::GlobalDecl parseGlobal(
        ast::GlobalKind kind
    ) {
        const Token& start = advance();

        ast::GlobalDecl node;

        node.location =
            location(start);

        node.kind = kind;

        node.name =
            expectIdentifier(
                "Expected global name"
            );

        expect(
            TokenType::COLON,
            "Expected ':' after global name"
        );

        node.type =
            parseType(false);

        node.alignment =
            parseOptionalAlign();

        if (match(TokenType::EQUAL)) {
            node.initializer =
                parseInitializer();
        }

        return node;
    }

        // =========================================================
    // Initializer
    // =========================================================

    ast::Initializer parseInitializer() {
        if (!match(TokenType::LBRACKET)) {
            return ast::Initializer{parseLiteral()};
        }

        auto array = std::make_shared<ast::ArrayLiteral>();

        if (!check(TokenType::RBRACKET)) {
            do {
                array->elements.push_back(
                    parseInitializer()
                );
            } while (match(TokenType::COMMA));
        }

        expect(
            TokenType::RBRACKET,
            "Expected ']' after array initializer"
        );

        return ast::Initializer{std::move(array)};
    }

    // =========================================================
    // Function Header
    // =========================================================

    ast::FunctionHeader
    parseFunctionHeader() {
        const Token& start =
            expect(
                TokenType::KW_DEF,
                "Expected 'def'"
            );

        ast::FunctionHeader header;

        header.location =
            location(start);

        header.returnType =
            parseReturnType();

        header.name =
            expectIdentifier(
                "Expected function name"
            );

        expect(
            TokenType::LPAREN,
            "Expected '(' after function name"
        );

        if (!check(TokenType::RPAREN)) {
            do {
                header.params.push_back(
                    parseParam()
                );
            }
            while (match(TokenType::COMMA));
        }

        expect(
            TokenType::RPAREN,
            "Expected ')' after parameters"
        );

        return header;
    }
    // =========================================================
    // Parameter
    // =========================================================

    ast::Param parseParam() {
        const Token& start = peek();

        ast::Param param;

        param.location =
            location(start);

        param.type =
            parseType(false);

        param.name =
            expectIdentifier(
                "Expected parameter name"
            );

        return param;
    }

    // =========================================================
    // Extern
    // =========================================================

    ast::ExternDecl parseExtern() {
        const Token& start =
            expect(
                TokenType::KW_EXTERN,
                "Expected 'extern'"
            );

        ast::ExternDecl node;

        node.location =
            location(start);

        node.header =
            parseFunctionHeader();

        return node;
    }

    // =========================================================
    // Function
    // =========================================================

    ast::Function parseFunction() {
        const Token& start = peek();

        ast::Function function;

        function.location =
            location(start);

        function.header =
            parseFunctionHeader();

        while (!check(TokenType::KW_END)) {
            function.blocks.push_back(
                parseLabelBlock()
            );
        }

        expect(
            TokenType::KW_END,
            "Expected 'end' after function"
        );

        return function;
    }

    // =========================================================
    // Label / Basic Block
    // =========================================================

    bool isLabelStart() const {
        return
            check(TokenType::IDENTIFIER) &&
            peek(1).type == TokenType::COLON;
    }

    ast::LabelBlock parseLabelBlock() {
        const Token& start = peek();

        ast::LabelBlock block;

        block.location =
            location(start);

        block.name =
            expectIdentifier(
                "Expected label name"
            );

        expect(
            TokenType::COLON,
            "Expected ':' after label name"
        );

        while (
            !isAtEnd() &&
            !check(TokenType::KW_END) &&
            !isLabelStart()
        ) {
            if (isTerminatorStart()) {
                if (block.terminator.has_value()) {
                    error(
                        "Unexpected second terminator "
                        "in basic block"
                    );
                }

                block.terminator =
                    parseTerminator();

                break;
            }

            block.instructions.push_back(
                parseInstruction()
            );
        }

        if (!block.terminator.has_value()) {
            error(
                "Basic block must end with a terminator"
            );
        }

        return block;
    }

    // =========================================================
    // Instruction dispatcher
    // =========================================================

        ast::SyscallInst parseSyscall(ast::ResultRef result) {
        const Token& start = expect(TokenType::KW_SYSCALL, "Expected 'syscall'");

        ast::SyscallInst node;
        node.location = location(start);
        node.result = std::move(result);

        // Contoh sintaks: %res = syscall.i64 64, %arg1, %arg2
        node.resultType = parseDottedType();
        node.number = parseOperand();

        if (match(TokenType::COMMA)) {
            do {
                node.args.push_back(parseOperand());
            } while (match(TokenType::COMMA));
        }

        return node;
    }


    ast::Instruction parseInstruction() {
        /*
         * Instructions without result:
         *
         * store
         * call
         * atomic.store
         */

        

        if (!check(TokenType::RESULT)) {
            switch (peek().type) {
            case TokenType::KW_STORE:
                return parseStore();

            case TokenType::KW_CALL:
                return parseCallWithoutResult();

            case TokenType::KW_ATOMIC_RMW:
                return parseAtomicInstruction();

            default:
                error(
                    "Expected instruction"
                );
            }
        }

        /*
         * Atomic CAS has two results:
         *
         * %old, %success = atomic.cas...
         */

        if (peek(1).type == TokenType::COMMA)
            return parseAtomicCAS();

        /*
         * Normal result-producing instruction:
         *
         * %result = instruction...
         */

        ast::ResultRef result =
            parseResult();

        expect(
            TokenType::EQUAL,
            "Expected '=' after result"
        );

        switch (peek().type) {
        case TokenType::KW_SYSCALL: return parseSyscall(std::move(result));
        case TokenType::KW_CONST:
            return parseConstant(
                std::move(result)
            );

        case TokenType::OP_NEG:
        case TokenType::OP_NOT:
        case TokenType::OP_BITNOT:
            return parseUnary(
                std::move(result)
            );

        case TokenType::OP_ADD:
        case TokenType::OP_SUB:
        case TokenType::OP_MUL:
        case TokenType::OP_DIV:
        case TokenType::OP_REM:
        case TokenType::OP_AND:
        case TokenType::OP_OR:
        case TokenType::OP_XOR:
        case TokenType::OP_SHL:
        case TokenType::OP_SHR:
            return parseBinary(
                std::move(result)
            );

        case TokenType::OP_EQ:
        case TokenType::OP_NE:
        case TokenType::OP_LT:
        case TokenType::OP_LE:
        case TokenType::OP_GT:
        case TokenType::OP_GE:
            return parseCompare(
                std::move(result)
            );

        case TokenType::OP_FCMP:
            return parseFloatCompare(
                std::move(result)
            );

        case TokenType::OP_FADD:
        case TokenType::OP_FSUB:
        case TokenType::OP_FMUL:
        case TokenType::OP_FDIV:
        case TokenType::OP_FREM:
            return parseFloatBinary(
                std::move(result)
            );

        case TokenType::KW_CAST:
            return parseCast(
                std::move(result)
            );

        case TokenType::KW_ALLOCA:
            return parseAlloca(
                std::move(result)
            );

        case TokenType::KW_LOAD:
            return parseLoad(
                std::move(result)
            );

        case TokenType::KW_GLOBAL:
            return parseGlobalAddr(
                std::move(result)
            );

        case TokenType::KW_FIELD_ADDR:
            return parseFieldAddr(
                std::move(result)
            );

        case TokenType::KW_ELEM_ADDR:
            return parseElementAddr(
                std::move(result)
            );

        case TokenType::KW_CALL:
            return parseCallWithResult(
                std::move(result)
            );

        case TokenType::KW_ATOMIC_RMW:
            return parseAtomicWithResult(
                std::move(result)
            );

        default:
            error(
                "Unknown result-producing instruction"
            );
        }
    }

    // =========================================================
    // Constant
    // =========================================================

    ast::ConstantInst parseConstant(
        ast::ResultRef result
    ) {
        const Token& start =
            expect(
                TokenType::KW_CONST,
                "Expected 'const'"
            );

        ast::ConstantInst node;

        node.location =
            location(start);

        node.result =
            std::move(result);

        node.type =
            parseDottedType();

        node.value =
            parseLiteral();

        return node;
    }

    // =========================================================
    // Unary
    // =========================================================

    ast::UnaryOp parseUnaryOp(
        TokenType type
    ) {
        switch (type) {
        case TokenType::OP_NEG:
            return ast::UnaryOp::Neg;

        case TokenType::OP_NOT:
            return ast::UnaryOp::Not;

        case TokenType::OP_BITNOT:
            return ast::UnaryOp::BitNot;

        default:
            error(
                "Expected unary operator"
            );
        }
    }

    ast::UnaryInst parseUnary(
        ast::ResultRef result
    ) {
        const Token& start = peek();

        const TokenType opToken =
            advance().type;

        ast::UnaryInst node;

        node.location =
            location(start);

        node.result =
            std::move(result);

        node.op =
            parseUnaryOp(opToken);

        node.type =
            parseDottedType();

        node.operand =
            parseOperand();

        return node;
    }

    // =========================================================
    // Binary
    // =========================================================

    ast::BinaryOp parseBinaryOp(
        TokenType type
    ) {
        switch (type) {
        case TokenType::OP_ADD:
            return ast::BinaryOp::Add;

        case TokenType::OP_SUB:
            return ast::BinaryOp::Sub;

        case TokenType::OP_MUL:
            return ast::BinaryOp::Mul;

        case TokenType::OP_DIV:
            return ast::BinaryOp::Div;

        case TokenType::OP_REM:
            return ast::BinaryOp::Rem;

        case TokenType::OP_AND:
            return ast::BinaryOp::And;

        case TokenType::OP_OR:
            return ast::BinaryOp::Or;

        case TokenType::OP_XOR:
            return ast::BinaryOp::Xor;

        case TokenType::OP_SHL:
            return ast::BinaryOp::Shl;

        case TokenType::OP_SHR:
            return ast::BinaryOp::Shr;

        default:
            error(
                "Expected binary operator"
            );
        }
    }

    ast::BinaryInst parseBinary(
        ast::ResultRef result
    ) {
        const Token& start = peek();

        const TokenType opToken =
            advance().type;

        ast::BinaryInst node;

        node.location =
            location(start);

        node.result =
            std::move(result);

        node.op =
            parseBinaryOp(opToken);

        node.type =
            parseDottedType();

        node.lhs =
            parseOperand();

        expect(
            TokenType::COMMA,
            "Expected ',' between binary operands"
        );

        node.rhs =
            parseOperand();

        return node;
    }

    // =========================================================
    // Integer Compare
    // =========================================================

    ast::CompareOp parseCompareOp(
        TokenType type
    ) {
        switch (type) {
        case TokenType::OP_EQ:
            return ast::CompareOp::Eq;

        case TokenType::OP_NE:
            return ast::CompareOp::Ne;

        case TokenType::OP_LT:
            return ast::CompareOp::Lt;

        case TokenType::OP_LE:
            return ast::CompareOp::Le;

        case TokenType::OP_GT:
            return ast::CompareOp::Gt;

        case TokenType::OP_GE:
            return ast::CompareOp::Ge;

        default:
            error(
                "Expected comparison operator"
            );
        }
    }

    ast::CompareInst parseCompare(
        ast::ResultRef result
    ) {
        const Token& start = peek();

        const TokenType opToken =
            advance().type;

        ast::CompareInst node;

        node.location =
            location(start);

        node.result =
            std::move(result);

        node.op =
            parseCompareOp(opToken);

        node.type =
            parseDottedIntegerType();

        node.lhs =
            parseOperand();

        expect(
            TokenType::COMMA,
            "Expected ',' between comparison operands"
        );

        node.rhs =
            parseOperand();

        return node;
    }

    // =========================================================
    // Float Compare
    // =========================================================

    TokenType parseFloatCompareOperator() {
        switch (peek().type) {
        case TokenType::OP_EQ:
        case TokenType::OP_NE:
        case TokenType::OP_LT:
        case TokenType::OP_LE:
        case TokenType::OP_GT:
        case TokenType::OP_GE:
            return advance().type;

        default:
            error(
                "Expected float comparison operator"
            );
        }
    }

    ast::FloatCompareInst parseFloatCompare(
        ast::ResultRef result
    ) {
        const Token& start =
            expect(
                TokenType::OP_FCMP,
                "Expected 'fcmp'"
            );

        ast::FloatCompareInst node;

        node.location =
            location(start);

        node.result =
            std::move(result);

        expect(
            TokenType::DOT,
            "Expected '.' before float comparison operator"
        );

        node.op =
            parseCompareOp(
                parseFloatCompareOperator()
            );

        node.type =
            parseDottedFloatType();

        node.lhs =
            parseOperand();

        expect(
            TokenType::COMMA,
            "Expected ',' between float comparison operands"
        );

        node.rhs =
            parseOperand();

        return node;
    }

    // =========================================================
    // Float Binary
    // =========================================================

    ast::FloatBinaryOp parseFloatBinaryOp(
        TokenType type
    ) {
        switch (type) {
        case TokenType::OP_FADD:
            return ast::FloatBinaryOp::Add;

        case TokenType::OP_FSUB:
            return ast::FloatBinaryOp::Sub;

        case TokenType::OP_FMUL:
            return ast::FloatBinaryOp::Mul;

        case TokenType::OP_FDIV:
            return ast::FloatBinaryOp::Div;

        case TokenType::OP_FREM:
            return ast::FloatBinaryOp::Rem;

        default:
            error(
                "Expected floating-point binary operator"
            );
        }
    }

    ast::FloatBinaryInst parseFloatBinary(
        ast::ResultRef result
    ) {
        const Token& start = peek();

        const TokenType opToken =
            advance().type;

        ast::FloatBinaryInst node;

        node.location =
            location(start);

        node.result =
            std::move(result);

        node.op =
            parseFloatBinaryOp(opToken);

        node.type =
            parseDottedFloatType();

        node.lhs =
            parseOperand();

        expect(
            TokenType::COMMA,
            "Expected ',' between float operands"
        );

        node.rhs =
            parseOperand();

        return node;
    }

    // =========================================================
    // Cast
    // =========================================================

    ast::CastInst parseCast(
        ast::ResultRef result
    ) {
        const Token& start =
            expect(
                TokenType::KW_CAST,
                "Expected 'cast'"
            );

        ast::CastInst node;

        node.location =
            location(start);

        node.result =
            std::move(result);

        node.operand =
            parseOperand();

        expectIdentifierValue("to");

        node.targetType =
            parseType(false);

        return node;
    }

    // =========================================================
    // Alloca
    // =========================================================

    ast::AllocaInst parseAlloca(
        ast::ResultRef result
    ) {
        const Token& start =
            expect(
                TokenType::KW_ALLOCA,
                "Expected 'alloca'"
            );

        ast::AllocaInst node;

        node.location =
            location(start);

        node.result =
            std::move(result);

        node.allocatedType =
            parseDottedType();

        node.alignment =
            parseOptionalAlign();

        return node;
    }

    // =========================================================
    // Load
    // =========================================================

    ast::LoadInst parseLoad(
        ast::ResultRef result
    ) {
        const Token& start =
            expect(
                TokenType::KW_LOAD,
                "Expected 'load'"
            );

        ast::LoadInst node;

        node.location =
            location(start);

        node.result =
            std::move(result);

        /*
         * Grammar:
         *
         * load [ ".volatile" ] "." Type Operand
         *
         * Lexer:
         *
         * .
         * IDENTIFIER("volatile")
         *
         * We only consume it if the lookahead matches.
         */
        if (
            check(TokenType::DOT) &&
            peek(1).type == TokenType::KW_VOLATILE
        ) {
            advance();
            advance();

            node.isVolatile = true;
        }

        node.type =
            parseDottedType();

        node.address =
            parseOperand();

        return node;
    }

    // =========================================================
    // Store
    // =========================================================

    ast::StoreInst parseStore() {
        const Token& start =
            expect(
                TokenType::KW_STORE,
                "Expected 'store'"
            );

        ast::StoreInst node;

        node.location =
            location(start);

        if (
            check(TokenType::DOT) &&
            peek(1).type == TokenType::KW_VOLATILE
        ) {
            advance();
            advance();

            node.isVolatile = true;
        }

        node.type =
            parseDottedType();

        node.value =
            parseOperand();

        expect(
            TokenType::COMMA,
            "Expected ',' between store value and address"
        );

        node.address =
            parseOperand();

        return node;
    }

    // =========================================================
    // Global Address
    // =========================================================

    ast::GlobalAddrInst parseGlobalAddr(
        ast::ResultRef result
    ) {
        const Token& start =
            expect(
                TokenType::KW_GLOBAL,
                "Expected 'global'"
            );

        expectDotIdentifier("addr");

        ast::GlobalAddrInst node;

        node.location =
            location(start);

        node.result =
            std::move(result);

        node.globalName =
            expectIdentifier(
                "Expected global name after 'global.addr'"
            );

        return node;
    }

    // =========================================================
    // Field Address
    // =========================================================

    ast::FieldAddrInst parseFieldAddr(
        ast::ResultRef result
    ) {
        const Token& start =
            expect(
                TokenType::KW_FIELD_ADDR,
                "Expected 'field'"
            );

        expectDotIdentifier("addr");

        ast::FieldAddrInst node;

        node.location =
            location(start);

        node.result =
            std::move(result);

        node.structName =
            expectIdentifier(
                "Expected struct name after 'field.addr'"
            );

        expect(
            TokenType::COMMA,
            "Expected ',' after struct name"
        );

        node.object =
            parseOperand();

        expect(
            TokenType::COMMA,
            "Expected ',' before field index"
        );

        node.fieldIndex =
            parseUnsignedInteger(
                "field index"
            );

        return node;
    }

    // =========================================================
    // Element Address
    // =========================================================

    ast::ElementAddrInst parseElementAddr(
        ast::ResultRef result
    ) {
        const Token& start =
            expect(
                TokenType::KW_ELEM_ADDR,
                "Expected 'elem'"
            );

        expectDotIdentifier("addr");

        /*
         * elem.addr.Type
         */
        expect(
            TokenType::DOT,
            "Expected '.' before element type"
        );

        ast::ElementAddrInst node;

        node.location =
            location(start);

        node.result =
            std::move(result);

        node.elementType =
            parseType(false);

        node.base =
            parseOperand();

        expect(
            TokenType::COMMA,
            "Expected ',' between base and index"
        );

        node.index =
            parseOperand();

        return node;
    }

    // =========================================================
    // Calls
    // =========================================================

    ast::CallInst parseCallWithResult(
        ast::ResultRef result
    ) {
        const Token& start =
            expect(
                TokenType::KW_CALL,
                "Expected 'call'"
            );

        /*
         * call.indirect...
         */
        if (
        check(TokenType::DOT) &&
        peek(1).type == TokenType::IDENTIFIER &&
        peek(1).value == "indirect"
    ) {
        advance(); // .
        advance(); // indirect

        return parseIndirectCall(
            std::move(result),
            start
        );
        }

        /*
         * call.ReturnType...
         */
        return parseDirectCall(
            std::move(result),
            start
        );
    }

    ast::CallInst parseCallWithoutResult() {
        const Token& start =
            expect(
                TokenType::KW_CALL,
                "Expected 'call'"
            );

        if (
        check(TokenType::DOT) &&
        peek(1).type == TokenType::IDENTIFIER &&
        peek(1).value == "indirect"
    ) {
        advance(); // .
        advance(); // indirect

        return parseIndirectCall(
            std::nullopt,
            start
        );
        }

        return parseDirectCall(
            std::nullopt,
            start
        );
    }

    ast::DirectCallInst parseDirectCall(
        std::optional<ast::ResultRef> result,
        const Token& start
    ) {
        ast::DirectCallInst node;

        node.location =
            location(start);

        node.result =
            std::move(result);

        node.returnType =
            parseCallReturnType();

        node.functionName =
            expectIdentifier(
                "Expected function name in direct call"
            );

        parseCallArguments(
            node.arguments
        );

        return node;
    }

    ast::IndirectCallInst parseIndirectCall(
        std::optional<ast::ResultRef> result,
        const Token& start
    ) {
        ast::IndirectCallInst node;

        node.location =
            location(start);

        node.result =
            std::move(result);

        node.returnType =
            parseCallReturnType();

        node.function =
            parseOperand();

        parseCallArguments(
            node.arguments
        );

        return node;
    }

    ast::Type parseCallReturnType() {
        expect(
            TokenType::DOT,
            "Expected '.' before call return type"
        );

        if (match(TokenType::TYPE_VOID))
            return ast::Type(
                ast::TypeKind::Void
            );

        return parseType(false);
    }

    void parseCallArguments(
        std::vector<ast::Operand>& arguments
    ) {
        expect(
            TokenType::LPAREN,
            "Expected '(' before call arguments"
        );

        if (!check(TokenType::RPAREN)) {
            do {
                arguments.push_back(
                    parseOperand()
                );
            }
            while (match(TokenType::COMMA));
        }

        expect(
            TokenType::RPAREN,
            "Expected ')' after call arguments"
        );
    }

    // =========================================================
    // Atomic
    // =========================================================

    ast::Instruction parseAtomicWithResult(
        ast::ResultRef result
    ) {
        if (peek().type !=
            TokenType::KW_ATOMIC_RMW) {
            error("Expected 'atomic'");
        }

        if (
            peek(1).type != TokenType::DOT
        ) {
            error(
                "Expected atomic instruction suffix"
            );
        }

        const std::string suffix =
            peek(2).value;

        if (suffix == "load")
            return parseAtomicLoad(
                std::move(result)
            );

        if (suffix == "rmw")
            return parseAtomicRMW(
                std::move(result)
            );

        error(
            "Unknown result-producing atomic instruction"
        );
    }

    ast::Instruction parseAtomicInstruction() {
        if (
            peek(1).type != TokenType::DOT
        ) {
            error(
                "Expected atomic instruction suffix"
            );
        }

        const std::string suffix =
            peek(2).value;

        if (suffix == "store")
            return parseAtomicStore();

        error(
            "Atomic instruction without a result "
            "must be 'atomic.store'"
        );
    }

    // ---------------------------------------------------------
    // atomic.load
    // ---------------------------------------------------------

    ast::AtomicLoadInst parseAtomicLoad(
        ast::ResultRef result
    ) {
        const Token& start =
            expect(
                TokenType::KW_ATOMIC_RMW,
                "Expected 'atomic'"
            );

        expectDotIdentifier("load");

        ast::AtomicLoadInst node;

        node.location =
            location(start);

        node.result =
            std::move(result);

        node.type =
            parseDottedIntegerType();

        node.address =
            parseOperand();

        node.ordering =
            parseMemoryOrdering();

        return node;
    }

    // ---------------------------------------------------------
    // atomic.store
    // ---------------------------------------------------------

    ast::AtomicStoreInst parseAtomicStore() {
        const Token& start =
            expect(
                TokenType::KW_ATOMIC_RMW,
                "Expected 'atomic'"
            );

        expectDotIdentifier("store");

        ast::AtomicStoreInst node;

        node.location =
            location(start);

        node.type =
            parseDottedIntegerType();

        node.value =
            parseOperand();

        expect(
            TokenType::COMMA,
            "Expected ',' between atomic store value and address"
        );

        node.address =
            parseOperand();

        node.ordering =
            parseMemoryOrdering();

        return node;
    }

    // ---------------------------------------------------------
    // atomic.rmw
    // ---------------------------------------------------------

    ast::AtomicRMWInst parseAtomicRMW(
        ast::ResultRef result
    ) {
        const Token& start =
            expect(
                TokenType::KW_ATOMIC_RMW,
                "Expected 'atomic'"
            );

        expectDotIdentifier("rmw");

        ast::AtomicRMWInst node;

        node.location =
            location(start);

        node.result =
            std::move(result);

        node.op =
            parseRMWOp();

        expect(
            TokenType::DOT,
            "Expected '.' before atomic RMW type"
        );

        node.type =
            parseIntegerType();

        node.address =
            parseOperand();

        expect(
            TokenType::COMMA,
            "Expected ',' between atomic RMW address and value"
        );

        node.value =
            parseOperand();

        node.ordering =
            parseMemoryOrdering();

        return node;
    }

    // ---------------------------------------------------------
    // atomic.cas
    // ---------------------------------------------------------

    ast::AtomicCASInst parseAtomicCAS() {
        const Token& start = peek();

        ast::AtomicCASInst node;

        node.location =
            location(start);

        node.oldValue =
            parseResult();

        expect(
            TokenType::COMMA,
            "Expected ',' between atomic CAS results"
        );

        node.success =
            parseResult();

        expect(
            TokenType::EQUAL,
            "Expected '=' after atomic CAS results"
        );

        expect(
            TokenType::KW_ATOMIC_RMW,
            "Expected 'atomic'"
        );

        expectDotIdentifier("cas");

        /*
         * atomic.cas.i32
         */
        expect(
            TokenType::DOT,
            "Expected '.' before atomic CAS type"
        );

        node.type =
            parseIntegerType();

        node.address =
            parseOperand();

        expect(
            TokenType::COMMA,
            "Expected ',' after atomic CAS address"
        );

        node.expected =
            parseOperand();

        expect(
            TokenType::COMMA,
            "Expected ',' after atomic CAS expected value"
        );

        node.desired =
            parseOperand();

        node.successOrdering =
            parseMemoryOrdering();

        expect(
            TokenType::COMMA,
            "Expected ',' between CAS success and failure ordering"
        );

        node.failureOrdering =
            parseFailureOrdering();

        return node;
    }

    // =========================================================
    // RMW operation
    // =========================================================

    ast::RMWOp parseRMWOp() {
        switch (peek().type) {
        case TokenType::OP_ADD:
            advance();
            return ast::RMWOp::Add;

        case TokenType::OP_SUB:
            advance();
            return ast::RMWOp::Sub;

        case TokenType::OP_AND:
            advance();
            return ast::RMWOp::And;

        case TokenType::OP_OR:
            advance();
            return ast::RMWOp::Or;

        case TokenType::OP_XOR:
            advance();
            return ast::RMWOp::Xor;

        case TokenType::OP_XCHG:
            advance();
            return ast::RMWOp::Xchg;

        default:
            error(
                "Expected atomic RMW operation"
            );
        }
    }

    // =========================================================
    // Memory Ordering
    // =========================================================

    ast::MemoryOrdering parseMemoryOrdering() {
        switch (peek().type) {
        case TokenType::ORDER_RELAXED:
            advance();
            return ast::MemoryOrdering::Relaxed;

        case TokenType::ORDER_ACQUIRE:
            advance();
            return ast::MemoryOrdering::Acquire;

        case TokenType::ORDER_RELEASE:
            advance();
            return ast::MemoryOrdering::Release;

        case TokenType::ORDER_ACQ_REL:
            advance();
            return ast::MemoryOrdering::AcqRel;

        case TokenType::ORDER_SEQ_CST:
            advance();
            return ast::MemoryOrdering::SeqCst;

        default:
            error(
                "Expected atomic memory ordering"
            );
        }
    }

    ast::FailureOrdering parseFailureOrdering() {
        switch (peek().type) {
        case TokenType::ORDER_RELAXED:
            advance();
            return ast::FailureOrdering::Relaxed;

        case TokenType::ORDER_ACQUIRE:
            advance();
            return ast::FailureOrdering::Acquire;

        case TokenType::ORDER_SEQ_CST:
            advance();
            return ast::FailureOrdering::SeqCst;

        default:
            error(
                "Expected atomic CAS failure ordering"
            );
        }
    }

    // =========================================================
    // Terminators
    // =========================================================

    bool isTerminatorStart() const {
        switch (peek().type) {
        case TokenType::KW_RET:
        case TokenType::KW_JMP:
        case TokenType::KW_BR_IF:
        case TokenType::KW_UNREACHABLE:
            return true;

        default:
            return false;
        }
    }

    ast::Terminator parseTerminator() {
        switch (peek().type) {
        case TokenType::KW_RET:
            return parseReturn();

        case TokenType::KW_JMP:
            return parseJump();

        case TokenType::KW_BR_IF:
            return parseBranchIf();

        case TokenType::KW_UNREACHABLE:
            return parseUnreachable();

        default:
            error(
                "Expected terminator"
            );
        }
    }

    // ---------------------------------------------------------
    // ret
    // ---------------------------------------------------------

    ast::ReturnTerminator parseReturn() {
        const Token& start =
            expect(
                TokenType::KW_RET,
                "Expected 'ret'"
            );

        ast::ReturnTerminator node;

        node.location =
            location(start);

        if (match(TokenType::TYPE_VOID)) {
            node.value =
                std::nullopt;

            return node;
        }

        node.value =
            parseOperand();

        return node;
    }

    // ---------------------------------------------------------
    // jmp
    // ---------------------------------------------------------

    ast::JumpTerminator parseJump() {
        const Token& start =
            expect(
                TokenType::KW_JMP,
                "Expected 'jmp'"
            );

        ast::JumpTerminator node;

        node.location =
            location(start);

        node.target =
            expectIdentifier(
                "Expected label after 'jmp'"
            );

        return node;
    }

    // ---------------------------------------------------------
    // br_if
    // ---------------------------------------------------------

    ast::BranchIfTerminator parseBranchIf() {
        const Token& start =
            expect(
                TokenType::KW_BR_IF,
                "Expected 'br_if'"
            );

        ast::BranchIfTerminator node;

        node.location =
            location(start);

        node.condition =
            parseOperand();

        expect(
            TokenType::COMMA,
            "Expected ',' after branch condition"
        );

        node.trueTarget =
            expectIdentifier(
                "Expected true target label"
            );

        expect(
            TokenType::COMMA,
            "Expected ',' between branch targets"
        );

        node.falseTarget =
            expectIdentifier(
                "Expected false target label"
            );

        return node;
    }

    // ---------------------------------------------------------
    // unreachable
    // ---------------------------------------------------------

    ast::UnreachableTerminator
    parseUnreachable() {
        const Token& start =
            expect(
                TokenType::KW_UNREACHABLE,
                "Expected 'unreachable'"
            );

        ast::UnreachableTerminator node;

        node.location =
            location(start);

        return node;
    }

    // =========================================================
    // Dotted Syntax
    // =========================================================

    void expectDotIdentifier(
        const std::string& value
    ) {
        expect(
            TokenType::DOT,
            "Expected '.' before '" +
            value +
            "'"
        );

        if (
            peek().value != value
        ) {
            error(
                "Expected '" +
                value +
                "' after '.'"
            );
        }

        advance();
    }

    void expectIdentifierValue(
        const std::string& value
    ) {
        if (
            !check(TokenType::IDENTIFIER) ||
            peek().value != value
        ) {
            error(
                "Expected '" +
                value +
                "'"
            );
        }

        advance();
    }

    // =========================================================
    // Dotted Type
    // =========================================================

    ast::Type parseDottedType() {
        expect(
            TokenType::DOT,
            "Expected '.' before instruction type"
        );

        return parseType(false);
    }

    ast::Type parseDottedIntegerType() {
        expect(
            TokenType::DOT,
            "Expected '.' before integer type"
        );

        return parseIntegerType();
    }

    ast::Type parseDottedFloatType() {
        expect(
            TokenType::DOT,
            "Expected '.' before float type"
        );

        return parseFloatType();
    }

    // =========================================================
    // Integer Type
    // =========================================================

    ast::Type parseIntegerType() {
        switch (peek().type) {
        case TokenType::TYPE_I8:
            advance();
            return ast::Type(ast::TypeKind::I8);

        case TokenType::TYPE_I16:
            advance();
            return ast::Type(ast::TypeKind::I16);

        case TokenType::TYPE_I32:
            advance();
            return ast::Type(ast::TypeKind::I32);

        case TokenType::TYPE_I64:
            advance();
            return ast::Type(ast::TypeKind::I64);

        case TokenType::TYPE_U8:
            advance();
            return ast::Type(ast::TypeKind::U8);

        case TokenType::TYPE_U16:
            advance();
            return ast::Type(ast::TypeKind::U16);

        case TokenType::TYPE_U32:
            advance();
            return ast::Type(ast::TypeKind::U32);

        case TokenType::TYPE_U64:
            advance();
            return ast::Type(ast::TypeKind::U64);

        case TokenType::TYPE_ISIZE:
            advance();
            return ast::Type(ast::TypeKind::ISize);

        case TokenType::TYPE_USIZE:
            advance();
            return ast::Type(ast::TypeKind::USize);

        default:
            error(
                "Expected integer type"
            );
        }
    }

    // =========================================================
    // Float Type
    // =========================================================

    ast::Type parseFloatType() {
        switch (peek().type) {
        case TokenType::TYPE_F32:
            advance();
            return ast::Type(ast::TypeKind::F32);

        case TokenType::TYPE_F64:
            advance();
            return ast::Type(ast::TypeKind::F64);

        default:
            error(
                "Expected floating-point type"
            );
        }
    }
};

} // namespace cru

#endif // CRU_PARSER_HPP
