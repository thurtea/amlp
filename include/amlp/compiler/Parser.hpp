#pragma once
#include <memory>
#include <vector>
#include "amlp/compiler/Lexer.hpp"
#include "amlp/compiler/Ast.hpp"

namespace amlp {

class Parser {
public:
    // dialect defaults to FluffOS, same reasoning as Lexer's own default
    // -- every pre-existing "Parser parser(tokens)" call site (this
    // driver's own ~80 direct test constructions) keeps its exact prior
    // behavior unchanged. Only ObjectManager::compile() passes a real,
    // config-derived dialect explicitly.
    explicit Parser(std::vector<Token> tokens, LpcDialect dialect = LpcDialect::FluffOS);
    std::unique_ptr<Program> parseProgram();

private:
    const Token& peek() const;
    const Token& peekAt(size_t offset) const;
    const Token& advance();
    bool check(TokenType type) const;
    bool checkText(const std::string& text) const;
    const Token& expect(TokenType type, const std::string& context);
    const Token& expectText(const std::string& text, const std::string& context);
    bool atEnd() const;
    bool isTypeKeyword(const Token& tok) const;
    bool isModifierKeyword(const Token& tok) const;

    // Shared prefix of every top-level declaration (modifiers, type, an
    // optional array '*', and a name), factored out so parseProgram()
    // can consume it exactly once and then branch on whether '(' follows
    // (a function) or not (an object variable declaration), rather than
    // duplicating the modifier/type/star/name consumption to peek ahead.
    struct DeclPrefix {
        std::string type;
        bool isArray;
        std::string name;
        // Whether "private" appeared among this declaration's modifier
        // keywords. Every other modifier (static, public, protected,
        // nomask, varargs) is still discarded unrecorded -- see
        // parseDeclPrefix()'s own comment -- but "private" specifically
        // has to survive onto ObjectVarDecl: real LPC scopes a private
        // object variable to the file that declares it, invisible to
        // (and non-collidable with) an inheriting child's own variable
        // of the same name (confirmed live: std/living.c's own "static
        // private int __Locked, __LastAged;" and std/user.c's separate,
        // unrelated "static int __LastAged;" are two different real
        // mudlib files that legitimately reuse the same name this way).
        // See CodeGen::generate()'s own use of ObjectVarDecl::isPrivate.
        bool isPrivate = false;
    };
    DeclPrefix parseDeclPrefix(const std::string& context);
    std::unique_ptr<FunctionDecl> parseFunctionRest(DeclPrefix prefix);
    std::vector<std::unique_ptr<ObjectVarDecl>> parseObjectVarDeclRest(DeclPrefix prefix);
    std::vector<Param> parseParamList();
    std::unique_ptr<Block> parseBlock();
    std::unique_ptr<Block> parseBranch();
    AstPtr parseStatement();
    AstPtr parseIfStatement();
    AstPtr parseWhileStatement();
    AstPtr parseDoWhileStatement();
    AstPtr parseForStatement();
    AstPtr parseReturnStatement();
    AstPtr parseBreakStatement();
    AstPtr parseContinueStatement();
    AstPtr parseForeachStatement();
    AstPtr parseSwitchStatement();
    struct ForeachVarSpec { std::string name; bool isNewDecl; };
    ForeachVarSpec parseForeachVar();
    std::unique_ptr<VarDeclStmt> parseSingleVarDecl(const Token& typeTok);
    AstPtr parseVarDeclStatement();
    AstPtr parseAssignStatement();
    void parseInheritStatement(Program& program);
    std::string parseInheritPathString();

    AstPtr parseExpr();
    AstPtr parseCommaExprChain();
    AstPtr continueStatementCommaChain(AstPtr firstStmt);
    AstPtr parseTernary();
    AstPtr parseLogicalOr();
    AstPtr parseLogicalAnd();
    AstPtr parseBitOr();
    AstPtr parseBitXor();
    AstPtr parseBitAnd();
    AstPtr parseEquality();
    AstPtr parseComparison();
    AstPtr parseAdditive();
    AstPtr parseMultiplicative();
    AstPtr parseUnary();
    AstPtr parsePostfix();
    AstPtr parsePrimary();
    std::vector<AstPtr> parseArgList();

    std::vector<Token> tokens_;
    size_t pos_ = 0;
    LpcDialect dialect_;

    // One entry per "(: ... :)" general-lambda body currently being
    // parsed (nested lambdas push their own), each tracking the highest
    // "$N" seen so far within that lambda specifically -- real lex.c's
    // own "current_function_context" (a stack there too, pushed/popped
    // per nested function-pointer context). Empty means "not currently
    // inside a lambda body", the real "$var illegal outside of function
    // pointer" condition -- see parsePrimary()'s own "$N" handling.
    std::vector<int> lambdaParamMaxStack_;
};

} // namespace amlp
