#include "amlp/compiler/Lexer.hpp"
#include "amlp/core/Errors.hpp"
#include <cctype>
#include <unordered_set>

namespace amlp {

namespace {
// "array" is deliberately not reserved here even though real LPC/FluffOS
// does reserve it (lex.c: {"array", L_ARRAY, 0}): this whole mudlib never
// once uses it as an actual type (every array-typed declaration uses
// "mixed *", "string *", etc. instead), but does use it as a plain
// identifier -- secure/SimulEfun/SimulEfun.h/exclude_array.c's own
// "exclude_array(mixed *array, int from, int to)" names a parameter
// "array". Reserving a word that is never needed as a type but is needed
// as an identifier would only break real code for no benefit.
const std::unordered_set<std::string> kKeywords = {
    "void", "int", "string", "object", "float", "mapping",
    // "status": real lex.c's own "{\"status\", L_BASIC_TYPE,
    // TYPE_NUMBER}" -- a legacy basic-type keyword that is just a plain
    // synonym for "int" (same TYPE_NUMBER), needing no separate CodeGen/
    // VM handling here since this driver's own Value model is already
    // dynamically typed regardless of the declared type -- confirmed
    // live: std/user.c's own "static status snoop, earmuffs;". Safe to
    // reserve (unlike "array", see this file's own comment on that one):
    // every "status" in this mudlib outside that declaration is inside
    // a string literal (a "status" command word), never a bare
    // identifier.
    "status",
    "mixed", "function", "return", "if", "else", "while", "for", "do",
    "static", "private", "public", "protected", "nomask", "varargs",
    "inherit", "break", "continue", "foreach", "in",
    "switch", "case", "default"
};
}

Lexer::Lexer(std::string source, LpcDialect dialect)
    : src_(std::move(source)), dialect_(dialect) {}

bool Lexer::atEnd() const { return pos_ >= src_.size(); }

char Lexer::peek() const { return atEnd() ? '\0' : src_[pos_]; }

char Lexer::peekNext() const {
    return (pos_ + 1 < src_.size()) ? src_[pos_ + 1] : '\0';
}

char Lexer::advance() {
    char c = src_[pos_++];
    if (c == '\n') ++line_;
    return c;
}

void Lexer::skipWhitespaceAndComments() {
    for (;;) {
        if (atEnd()) return;
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
            continue;
        }
        if (c == '/' && peekNext() == '/') {
            while (!atEnd() && peek() != '\n') advance();
            continue;
        }
        if (c == '/' && peekNext() == '*') {
            advance(); advance();
            while (!atEnd() && !(peek() == '*' && peekNext() == '/')) {
                advance();
            }
            if (!atEnd()) { advance(); advance(); }
            continue;
        }
        return;
    }
}

Token Lexer::lexIdentOrKeyword() {
    int startLine = line_;
    std::string text;
    while (!atEnd() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) {
        text += advance();
    }
    // "atomic" -- DGD's real function-declaration modifier (ROADMAP.md
    // row 1.2's scoping note; confirmed against temp/dgd/src/comp/
    // parser.y's own "ATOMIC { $$ = C_ATOMIC; }", the same
    // non_private modifier-list production as STATIC/NOMASK/VARARGS).
    // "nil" -- DGD's real nil literal (same scoping note; confirmed
    // against parser.y's own "NIL { $$ = Node::createNil(); }" and
    // data.h's own "T_NIL" value tag -- a genuinely distinct runtime
    // value under DGD's strict-typechecking mode, the mode this
    // implementation targets, since that is the only mode where nil
    // means anything at all: temp/dgd/src/data.cpp's own "nil.type =
    // (stricttc) ? T_NIL : T_INT;" shows non-strict DGD collapses nil
    // into plain integer 0 with no distinct representation, which would
    // give this row nothing worth tracking). Both deliberately kept out
    // of the shared kKeywords set below and gated here instead:
    // reserving either under FluffOS/LDMud too would risk breaking real
    // code using them as plain identifiers, the same hazard this file's
    // own "array" comment documents, and neither word is reserved by
    // either real dialect.
    static const std::unordered_set<std::string> kDgdOnlyKeywords = {"atomic", "nil"};
    bool isDialectKeyword = dialect_ == LpcDialect::DGD && kDgdOnlyKeywords.count(text);
    TokenType type = (kKeywords.count(text) || isDialectKeyword) ? TokenType::Keyword : TokenType::Ident;
    return Token{type, text, startLine};
}

Token Lexer::lexNumber() {
    int startLine = line_;
    std::string text;
    while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
        text += advance();
    }

    // Hex literal ("0x1A", real C/LPC syntax): a leading "0" (already
    // consumed by the loop above, so text == "0" here) immediately
    // followed by 'x'/'X' and at least one hex digit. Found live against
    // a real third-party mudlib corpus (row 3.8's TMI-2 boot attempt):
    // adm/simul_efun/vt100.c's own real "if (color_mode & 0x0010)"
    // (VT100 bright/bold-attribute bitflag test) -- previously this
    // driver's own lexNumber() only ever consumed the leading "0" as a
    // complete Number token, then re-entered the tokenizer's main loop
    // at 'x', producing a stray Ident token ("x0010") the parser could
    // not make sense of ("expected \")\" in if condition ... got
    // \"x0010\""). Kept as its own branch, not folded into the plain
    // decimal path, and the "0x"/"0X" prefix is kept in the token's own
    // text (not stripped here) -- Parser.cpp's IntLiteral construction
    // is the one place that actually interprets a Number token's text,
    // exactly like the float '.' case just below already keeps the '.'
    // in the text for that same later decision point. Octal ("0755") is
    // a deliberately separate, unevidenced concern -- no octal literal
    // appears anywhere in this same corpus pass, and unlike hex, blindly
    // reinterpreting any leading-zero decimal integer as octal would be
    // a real, silent behavior change for a literal like "010" wherever
    // one already exists -- not made here without real evidence forcing it.
    if (text == "0" && (peek() == 'x' || peek() == 'X') &&
        std::isxdigit(static_cast<unsigned char>(peekNext()))) {
        text += advance(); // 'x' / 'X'
        while (!atEnd() && std::isxdigit(static_cast<unsigned char>(peek()))) {
            text += advance();
        }
        return Token{TokenType::Number, text, startLine};
    }

    // Float literal ("1.5"): a '.' immediately after the integer part,
    // followed by a digit, distinct from the ".." range operator
    // ("arr[a..b]") and "..." varargs marker, which both start with a
    // second '.' rather than a digit. The '.' is folded straight into
    // this same Number token's text (no separate token type at the lexer
    // level) -- the Parser tells int and float apart downstream just by
    // checking for a '.' in the text, same as how it already relies on
    // this token's raw text for int parsing. The leading-dot form
    // (".5", no digit before the '.') is handled by tokenize() routing
    // straight here instead of through lexIdentOrKeyword/lexSymbol --
    // see its own comment -- so this loop simply does nothing on its
    // first pass for that case (text starts empty, peek() is already
    // '.') and falls through to the same handling below.
    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peekNext()))) {
        text += advance(); // '.'
        while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
            text += advance();
        }
    }

    return Token{TokenType::Number, text, startLine};
}

// "$N" (e.g. "$1") -- real lex.c's own '$' case: a digit must follow
// immediately (a bare "$" or "$(expr)" is real LPC too, but nothing
// reachable in this mudlib uses either form -- see Ast.hpp's
// LambdaParamExpr comment for the full citation -- so this throws a
// clear error instead of silently mishandling them, matching this
// codebase's existing convention for other partially-implemented
// syntax). Emitted as an ordinary Ident token whose text is "$" plus
// the digits: real identifiers can never start with '$' (confirmed by
// CodeGen.cpp's own "$lambda#N" synthetic-name comment), so this can
// never collide with a real variable/function name, and Parser.cpp
// tells the two apart with one cheap check rather than needing a whole
// new TokenType threaded through every place that already matches on
// TokenType::Ident. Whether "$N" is actually legal at this point in the
// source (real lex.c: "$var illegal outside of function pointer") is a
// parse-time concern, not a lexical one -- see Parser.cpp's own
// lambdaParamMaxStack_.
Token Lexer::lexLambdaParam() {
    int startLine = line_;
    advance(); // '$'
    // Real lex.c's own '$' case (fluffos-2.23-ds03): "if (!isdigit(c =
    // *outp++)) { outp--; return '$'; }" -- a bare, single-character '$'
    // token, unconditionally, whenever a digit does not immediately
    // follow (never a lexer-level error). Real grammar.y.pre's own
    // "'$' '(' comma_expr ')'" is the one production that ever consumes
    // this bare token (Ast.hpp's InlineLambdaExpr::boundValueExprs --
    // real, bound-at-construction-time closure values, e.g. "(:
    // eventCast($(spell), $1) :)"); anything else following a bare '$'
    // is a genuine parse-time error, not a lexical one, so this returns
    // the token uninterpreted and lets Parser.cpp decide, matching real
    // lex.c's own division of labor exactly.
    if (!std::isdigit(static_cast<unsigned char>(peek()))) {
        return Token{TokenType::Symbol, "$", startLine};
    }
    std::string text = "$";
    while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
        text += advance();
    }
    return Token{TokenType::Ident, text, startLine};
}

Token Lexer::lexString() {
    int startLine = line_;
    advance();
    std::string value;
    while (!atEnd() && peek() != '"') {
        char c = advance();
        if (c == '\\' && !atEnd()) {
            char esc = advance();
            switch (esc) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case '"': value += '"'; break;
                case '\\': value += '\\'; break;
                default: value += esc; break;
            }
        } else {
            value += c;
        }
    }
    if (atEnd()) {
        throw LpcRuntimeError("unterminated string literal at line " + std::to_string(startLine));
    }
    advance();
    return Token{TokenType::String, value, startLine};
}

Token Lexer::lexChar() {
    int startLine = line_;
    advance(); // consume opening '

    if (atEnd()) {
        throw LpcRuntimeError("unterminated character literal at line " +
                               std::to_string(startLine));
    }

    char c = advance();
    int64_t code;
    if (c == '\\' && !atEnd()) {
        char esc = advance();
        switch (esc) {
            case 'n': code = '\n'; break;
            case 't': code = '\t'; break;
            case '\'': code = '\''; break;
            case '"': code = '"'; break;
            case '\\': code = '\\'; break;
            default: code = static_cast<unsigned char>(esc); break;
        }
    } else {
        code = static_cast<unsigned char>(c);
    }

    if (atEnd() || peek() != '\'') {
        throw LpcRuntimeError(
            "character literal must contain exactly one character, at line " +
            std::to_string(startLine));
    }
    advance(); // consume closing '

    return Token{TokenType::Number, std::to_string(code), startLine};
}

// Real LPC's "@TERM ... TERM" heredoc string literal (confirmed against
// the FluffOS reference driver's lex.c: "case '@'": get_terminator() then
// get_text_block()). Hit live in secure/SimulEfun/misc.c:
//   ret = @END
//   Fd    State      Mode       Local Address          Remote Address
//   --  ---------  --------  ---------------------  ---------------------
//   END;
// The terminator is read up to the end of the "@TERM" line; the text
// block runs verbatim (no escape processing) until a line that *starts*
// with the terminator, at which point the terminator is consumed and
// whatever follows on that same line (here, ";") resumes as normal LPC
// source. The "@@TERM ... TERM" array-block variant (each line becomes
// a separate array element) is not implemented -- not hit anywhere in
// this mudlib's boot-path files -- and throws rather than silently
// misparsing.
Token Lexer::lexHeredoc() {
    int startLine = line_;
    advance(); // consume '@'

    if (peek() == '@') {
        throw NotImplementedError("\"@@\" array-block heredoc syntax");
    }

    std::string terminator;
    while (!atEnd() && peek() != '\n') {
        terminator += advance();
    }
    if (terminator.empty()) {
        throw LpcRuntimeError("heredoc: missing terminator at line " + std::to_string(startLine));
    }
    if (atEnd()) {
        throw LpcRuntimeError("unterminated heredoc: missing closing \"" + terminator +
                               "\" at line " + std::to_string(startLine));
    }
    advance(); // consume the newline ending the "@TERM" line

    std::string value;
    bool atLineStart = true;
    for (;;) {
        if (atLineStart) {
            bool matches = pos_ + terminator.size() <= src_.size() &&
                           src_.compare(pos_, terminator.size(), terminator) == 0;
            if (matches) {
                for (size_t i = 0; i < terminator.size(); ++i) advance();
                break;
            }
        }
        if (atEnd()) {
            throw LpcRuntimeError("unterminated heredoc: missing closing \"" + terminator +
                                   "\" at line " + std::to_string(startLine));
        }
        char c = advance();
        value += c;
        atLineStart = (c == '\n');
    }

    return Token{TokenType::String, value, startLine};
}

Token Lexer::lexSymbol() {
    int startLine = line_;
    char c = advance();

    if (c == '-' && peek() == '>') {
        advance();
        return Token{TokenType::Symbol, "->", startLine};
    }
    if (c == ':' && peek() == ':') {
        advance();
        // "efun::name(...)", real LPC's explicit escape hatch to the
        // core efun table (grammar.y: "L_EFUN L_COLON_COLON identifier"
        // -- confirmed live: secure/SimulEfun/misc.c's own
        // "efun::destruct(ob)"). A single ':' is otherwise only ever a
        // ternary's own, so this cannot misfire there.
        return Token{TokenType::Symbol, "::", startLine};
    }
    if (c == '+' && peek() == '+') {
        advance();
        return Token{TokenType::Symbol, "++", startLine};
    }
    if (c == '-' && peek() == '-') {
        advance();
        return Token{TokenType::Symbol, "--", startLine};
    }
    if ((c == '+' || c == '-' || c == '*' || c == '/' || c == '%') && peek() == '=') {
        advance();
        return Token{TokenType::Symbol, std::string(1, c) + "=", startLine};
    }
    // Bitwise compound-assignment forms ("|=", "&=", "^="), real LPC's
    // own operators same as C's -- previously absent here entirely, so
    // "|=" (etc) lexed as two separate tokens, '|' then '=', which the
    // parser's own compound-assignment recognition never matches (it
    // checks the *next* token's exact text against "|="), silently
    // falling through to ordinary binary-or parsing and then failing on
    // the bare "=" it did not expect. Found live against a real
    // third-party mudlib corpus (row 3.8's TMI-2 boot attempt):
    // adm/obj/master/access.c's own "access[path][name] |= WRITE;",
    // a real, common LPC permission-bitmask idiom -- corpus-wide, TMI-2
    // alone has 30 "|=", 6 "&=", 2 "^=" real call sites.
    if ((c == '|' || c == '&' || c == '^') && peek() == '=') {
        advance();
        return Token{TokenType::Symbol, std::string(1, c) + "=", startLine};
    }
    if (c == '=' && peek() == '=') {
        advance();
        return Token{TokenType::Symbol, "==", startLine};
    }
    if (c == '!' && peek() == '=') {
        advance();
        return Token{TokenType::Symbol, "!=", startLine};
    }
    // Shift operators ("<<"/">>", real C-family bitwise left/right
    // shift, and their compound-assignment forms "<<="/">>=") -- row
    // 3.8's own TMI-2 pass explicitly left these unimplemented ("a
    // separate, unevidenced gap"), and this driver had no plain "<<"/
    // ">>" binary operator at all before now. Found live against a
    // real third-party mudlib corpus (Dead Souls 3.8.2's own boot
    // attempt): secure/daemon/master.c's own real "((1 << 10) | (1 <<
    // 0))" flag-combining idiom alone blocked this driver's own
    // required master object from compiling at all; a corpus-wide scan
    // found 226 real plain "<<"/">>" call sites and 2 real compound
    // ones (secure/sefun/astar.c's own "i <<= 1;"/"v >>= 1;"), so both
    // forms are real, not speculative. "<<=" must be checked before a
    // bare "<<" (and "<" alone) or the greedy two-char match would
    // leave a stray "=" behind for the next token -- same three-step
    // lookahead shape "..."/".." already uses just below.
    if (c == '<' && peek() == '<') {
        advance();
        if (peek() == '=') {
            advance();
            return Token{TokenType::Symbol, "<<=", startLine};
        }
        return Token{TokenType::Symbol, "<<", startLine};
    }
    if (c == '>' && peek() == '>') {
        advance();
        if (peek() == '=') {
            advance();
            return Token{TokenType::Symbol, ">>=", startLine};
        }
        return Token{TokenType::Symbol, ">>", startLine};
    }
    if (c == '<' && peek() == '=') {
        advance();
        return Token{TokenType::Symbol, "<=", startLine};
    }
    if (c == '>' && peek() == '=') {
        advance();
        return Token{TokenType::Symbol, ">=", startLine};
    }
    if (c == '|' && peek() == '|') {
        advance();
        return Token{TokenType::Symbol, "||", startLine};
    }
    if (c == '&' && peek() == '&') {
        advance();
        return Token{TokenType::Symbol, "&&", startLine};
    }
    if (c == '.' && peek() == '.') {
        advance();
        if (peek() == '.') {
            advance();
            // Trailing varargs marker after a function's parameter list,
            // e.g. "int true(mixed args...)" (confirmed against
            // grammar.y: "argument_list L_DOT_DOT_DOT"), distinct from
            // the two-dot range operator ("arr[a..b]").
            return Token{TokenType::Symbol, "...", startLine};
        }
        return Token{TokenType::Symbol, "..", startLine};
    }

    return Token{TokenType::Symbol, std::string(1, c), startLine};
}

// Disambiguates real LDMud's own two "'"-prefixed forms (lex.c:6186-6266,
// "--- ': Character constant or lambda symbol ---"): an ordinary
// character constant ('a', '\n', ..., already handled by lexChar()
// before this method existed) and LDMud's own "'name" symbol literal
// (see Value.hpp's Symbol comment, ROADMAP.md row 1.7/1.8's own
// unbound_lambda() investigation -- symbols are what its quoted-code
// bodies use to mean "substitute this lambda parameter's own value
// here"). Real lex.c's own rule (paraphrased): read the char after the
// quote and the char after that; if the second one is anything but a
// closing quote, or the whole thing is a lone quote followed by another
// quote/alnum/'(' (the "'''x" and "'({" cases), it is a symbol or
// quoted-aggregate, not a character constant. This driver's own scope
// is narrower, matching zero confirmed real corpus usage for either of
// those two edge forms: a single leading quote (no "''name" multi-quote
// symbols) followed by a bare alnum/underscore identifier of length > 1
// (no "'({" quoted-aggregate literal, and a length-1 identifier
// followed immediately by a closing quote stays an ordinary character
// constant exactly as before -- real lex.c resolves that same
// length-1-then-quote case as a char constant too, "the test rejects
// all sequences of the form 'x'"). Only reached under LpcDialect::LdMud
// (see tokenize()'s own dispatch below) -- FluffOS/DGD source keeps
// lexChar()'s pre-existing behavior for every bare "'" unchanged, since
// neither real dialect has a symbol-literal concept at all.
Token Lexer::lexQuote() {
    size_t p = pos_ + 1;
    if (p < src_.size() &&
        (std::isalpha(static_cast<unsigned char>(src_[p])) || src_[p] == '_')) {
        size_t identStart = p;
        while (p < src_.size() &&
               (std::isalnum(static_cast<unsigned char>(src_[p])) || src_[p] == '_')) {
            ++p;
        }
        bool singleCharThenQuote = (p - identStart == 1) && p < src_.size() && src_[p] == '\'';
        if (!singleCharThenQuote) {
            int startLine = line_;
            advance(); // '
            std::string name;
            while (!atEnd() &&
                   (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) {
                name += advance();
            }
            return Token{TokenType::QuotedSymbol, name, startLine};
        }
    }
    return lexChar();
}

// LDMud's own closure-literal prefix, "#'name" (ROADMAP.md row 1.2/1.3's
// own scoping note; confirmed against temp/ldmud/src/lex.c's own
// closure() function, "case '#': if (*yyp == '\'') return closure(yyp);"
// at lex.c:6158-6162). Real LDMud's own "#'" grammar is far richer than
// this -- ~50 operator spellings (#'+, #'+=, ...), #'[ index/range/map-
// index forms, #'({ aggregate-array closures, and #'efun::/#'sefun::/
// #'lfun::/#'var::/#'Name:: scope prefixes, all resolving to one token,
// L_CLOSURE -- deliberately not any of that here. This is the single
// bare-name first slice only: "#'identifier", a plain lfun/efun/
// simul_efun reference with no scope prefix, no bound args at the
// literal site (matching real LDMud semantics for this exact form: any
// arguments are supplied by whoever later calls the closure, not baked
// in at the literal). Combined into one two-character Symbol token here
// (matching the existing "->"/"::"/"++" precedent just above, not a
// dedicated new TokenType) so Parser.cpp can recognize it with a single
// checkText("#'") the same way it already recognizes "(:" -- see
// Parser.cpp's own comment on ClosureLiteralExpr for why this reuses
// that exact AST node rather than needing a new one: a bare "#'name" and
// a bare "(: name :)" are the same underlying concept (a closure bound
// to a function by name, no bound args), just two different dialects'
// own spelling of it.
Token Lexer::lexHashQuote() {
    int startLine = line_;
    advance(); // #
    advance(); // '
    return Token{TokenType::Symbol, "#'", startLine};
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;

    for (;;) {
        skipWhitespaceAndComments();
        if (atEnd()) break;

        char c = peek();
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            tokens.push_back(lexIdentOrKeyword());
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            tokens.push_back(lexNumber());
        } else if (c == '.' && std::isdigit(static_cast<unsigned char>(peekNext()))) {
            // Leading-dot float literal (".5", real LPC/C for "0.5"),
            // confirmed used live across the mudlib. A bare '.' otherwise
            // never starts a token (LPC has no member-access operator --
            // "->" fills that role -- so outside of a float literal '.'
            // only ever appears as ".." range or "..." varargs, both
            // handled below by lexSymbol(), neither of which is a digit).
            tokens.push_back(lexNumber());
        } else if (c == '"') {
            tokens.push_back(lexString());
        } else if (c == '\'') {
            // Gated on dialect_ the same way "#'" is just below: neither
            // FluffOS nor DGD has a symbol-literal concept (this is an
            // LDMud-only closure-body feature, unbound_lambda()'s own
            // quoted-code bodies -- see Value.hpp's Symbol comment), so a
            // bare "'" there keeps lexChar()'s pre-existing, unconditional
            // char-literal behavior exactly as before -- only under
            // LpcDialect::LdMud does lexQuote()'s own disambiguation
            // apply (see its own comment).
            tokens.push_back(dialect_ == LpcDialect::LdMud ? lexQuote() : lexChar());
        } else if (c == '#' && dialect_ == LpcDialect::LdMud && peekNext() == '\'') {
            // Gated the same way "atomic"/"nil" are gated for DGD above:
            // reserving "#" for anything under FluffOS/DGD would risk
            // breaking real code (and today it is simply not lexable at
            // all outside real preprocessor directives, which never reach
            // this point -- the "else" branch below throws "unrecognized
            // character '#'", unchanged for both other dialects).
            tokens.push_back(lexHashQuote());
        } else if (c == '@') {
            tokens.push_back(lexHeredoc());
        } else if (c == '$') {
            tokens.push_back(lexLambdaParam());
        } else if (c == '(' || c == ')' || c == '{' || c == '}' ||
                   c == '[' || c == ']' || c == ':' ||
                   c == ';' || c == ',' || c == '-' || c == '=' ||
                   c == '!' || c == '<' || c == '>' || c == '*' || c == '+' ||
                   c == '|' || c == '&' || c == '/' || c == '%' || c == '.' ||
                   c == '?' || c == '^' || c == '~') {
            tokens.push_back(lexSymbol());
        } else {
            int errLine = line_;
            char unrecognized = advance();
            throw LpcRuntimeError(
                "lexer: unrecognized character '" + std::string(1, unrecognized) +
                "' at line " + std::to_string(errLine));
        }
    }

    tokens.push_back(Token{TokenType::End, "", line_});
    return tokens;
}

} // namespace amlp
