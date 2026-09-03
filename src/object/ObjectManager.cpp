#include "amlp/object/ObjectManager.hpp"
#include "amlp/object/LivingNameRegistry.hpp"
#include "amlp/object/LiveObjectRegistry.hpp"
#include "amlp/config/Config.hpp"
#include "amlp/core/Errors.hpp"
#include "amlp/compiler/Lexer.hpp"
#include "amlp/compiler/Parser.hpp"
#include "amlp/compiler/CodeGen.hpp"
#include "amlp/dialect/LpcDialect.hpp"
#include "amlp/vm/VM.hpp"
#include <cctype>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <algorithm>
#include <chrono>
#include <iterator>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace amlp {

namespace {

// Number of ObjectManager instances currently alive. The global
// LiveObjectRegistry is process-wide and shared by every manager (a
// single one in the real driver; two or more at once only in a handful
// of tests that compare state across harnesses), so ~ObjectManager only
// runs the registry-wide cycle-break sweep when it is the last manager
// going away. An earlier manager's destruction still drops its own
// loaded_/restoredObjects_/caches; a self-referential clone it created
// stays pinned by its own variable slot only until that final sweep.
int g_liveManagerCount = 0;

struct PreprocessResult {
    bool ok = false;
    std::string output;
    std::string errorOutput;
};

// FluffOS injects these into every compile itself (option_defs.c's
// predefined-macro table), independent of anything the mudlib's own
// headers define -- so real mudlib code freely uses them as bare
// identifiers (e.g. secure/daemon/master.c's
// "str+__SAVE_EXTENSION__") expecting the driver to have already
// substituted them, the same way __FILE__/__LINE__ work in C. This
// driver shells out to the system's real cpp instead of a bespoke
// preprocessor, so the same effect is achieved by passing each one as a
// "-D" flag. Values are copied verbatim from the FluffOS reference
// driver's option_defs.c; entries with an empty value there are
// feature-flags mudlib code checks with #ifdef, not values it expects to
// read.
struct PredefinedMacro { const char* name; const char* value; };
constexpr PredefinedMacro kFluffosPredefinedMacros[] = {
    {"__STRIP_BEFORE_PROCESS_INPUT__", ""},
    {"__NO_LIGHT__", ""},
    {"__CUSTOM_CRYPT__", ""},
    {"__RECEIVE_SNOOP__", ""},
    {"__ARGUMENTS_IN_TRACEBACK__", ""},
    {"__NEXT_MALLOC_DEBUG__", ""},
    {"__ARRAY_STATS__", ""},
    {"__LOCALS_IN_TRACEBACK__", ""},
    {"__SYSMALLOC__", ""},
    {"__CFG_MAX_CALL_DEPTH__", "150"},
    {"__SAVE_EXTENSION__", "\\\".o\\\""},
    {"__LOG_CATCHES__", ""},
    {"__NONINTERACTIVE_STDERR_WRITE__", ""},
    {"__SMALL_STRING_SIZE__", "100"},
    {"__CALLOUT_CYCLE_SIZE__", "32"},
    {"__PACKAGE_MATH__", ""},
    {"__PACKAGE_DEVELOP__", ""},
    {"__SUPPRESS_ARGUMENT_WARNINGS__", ""},
    {"__LARGEST_PRINTABLE_STRING__", "8192"},
    {"__CFG_LIVING_HASH_SIZE__", "256"},
    {"__CACHE_STATS__", ""},
    {"__CONFIG_FILE_DIR__", "\\\"./\\\""},
    {"__PARSE_DEBUG__", ""},
    {"__TRAP_CRASHES__", ""},
    {"__RESTRICTED_ED__", ""},
    {"__STRING_STATS__", ""},
    {"__CALLOUT_HANDLES__", ""},
    {"__FLUFFOS__", ""},
    {"__CFG_COMPILER_STACK_SIZE__", "600000"},
    {"__LARGE_STRING_SIZE__", "1000"},
    {"__HEARTBEAT_INTERVAL__", "1"},
    {"__PACKAGE_MATRIX__", ""},
    {"__PRIVS__", ""},
    {"__COMMAND_BUF_SIZE__", "2000"},
    {"__OLD_ED__", ""},
    {"__PACKAGE_PARSER__", ""},
    {"__THIS_PLAYER_IN_CALL_OUT__", ""},
    {"__PACKAGE_CONTRIB__", ""},
    {"__ALLOW_INHERIT_AFTER_GLOBAL_VARIABLES__", ""},
    {"__CFG_MAX_LOCAL_VARIABLES__", "50"},
    {"__MESSAGE_BUFFER_SIZE__", "4096"},
    {"__NO_WIZARDS__", ""},
    {"__SAVE_GZ_EXTENSION__", "\\\".o.gz\\\""},
    {"__PACKAGE_SOCKETS__", ""},
    {"__NUM_EXTERNAL_CMDS__", "100"},
    {"__HEART_BEAT_CHUNK__", "32"},
    {"__INTERACTIVE_CATCH_TELL__", ""},
    {"__MAX_SAVE_SVALUE_DEPTH__", "100"},
    {"__DEFAULT_PRAGMAS__", "0"},
    {"__NO_ANSI__", ""},
    {"__HAS_STATUS_TYPE__", ""},
    {"__CFG_EVALUATOR_STACK_SIZE__", "3000"},
    {"__PACKAGE_MUDLIB_STATS__", ""},
    {"__WARN_TAB__", ""},
    {"__ALLOW_INHERIT_AFTER_FUNCTION__", ""},
    {"__APPLY_CACHE_BITS__", "11"},
    {"__MUDLIB_ERROR_HANDLER__", ""},
};

// A second, smaller set of predefines FluffOS injects from lex.c's own
// add_predefines() rather than option_defs.c's static table above --
// still driver-injected the same way, but each one is computed at boot
// (driver version string, build arch, etc) instead of being a fixed
// per-build feature flag. Confirmed live: secure/SimulEfun/mud_info.c's
// own "string mud_name() { return MUD_NAME; }" -- MUD_NAME (and
// __PORT__, the other config-dependent one here) are handled separately
// below since their value comes from this driver's own Config, not a
// fixed literal.
constexpr PredefinedMacro kFluffosRuntimePredefinedMacros[] = {
    {"MUDOS", ""},
    // No spaces in any of these three values (unlike real lex.c's own
    // "FluffOS v2.9-ds2.08 for Linux." __VERSION__ string): the cpp
    // invocation below is built as one unquoted shell command string via
    // popen(), so an embedded space would get word-split into extra
    // (nonexistent) input filenames -- "cpp: fatal error: too many input
    // files" -- exactly like MUD_NAME below would if it were ever
    // configured with a space in it. Not fixed at the quoting level
    // here, just avoided in the one value this code controls.
    {"__VERSION__", "\\\"2.9-ds2.08\\\""},
    {"__ARCH__", "\\\"Linux\\\""},
    {"__COMPILER__", "\\\"g++\\\""},
    {"__OPTIMIZATION__", "\\\"-O2\\\""},
    // sizeof(long) and the resulting max signed value on a 64-bit Linux
    // build, matching real lex.c's own "sizeof(long)"/"(long)1<<63 - 1"
    // computation rather than a value copied from someone else's build.
    {"SIZEOFINT", "8"},
    {"MAX_INT", "9223372036854775807"},
    // HAS_ED / HAS_PRINTF / HAS_RUSAGE / HAS_DEBUG_LEVEL are deliberately
    // not defined here: real lex.c only defines each one when the
    // corresponding driver feature (the "ed" package, F_PRINTF, rusage
    // reporting, debug levels) was actually compiled in, and none of
    // those exist in this driver yet -- defining them would tell mudlib
    // code a capability exists when calling it would just throw
    // NotImplementedError.
};

// __PACKAGE_PCRE__ and the PCRE_* flag constants (Phase 2 row 2.12,
// backing the new pcre_match()/pcre_assoc() efuns in EfunTable.cpp --
// see that file's own much longer citation comment for the full real
// scope-check). Unlike every macro in the two tables above, these are
// NOT sourced from the pinned temp/reference/fluffos-2.9-ds2.08 tree --
// that tree has no PCRE package at all (confirmed: zero "pcre" hits
// anywhere in it). Real source is instead the separately-vendored
// modern FluffOS tree, temp/fluffos (a master-branch checkout): its
// own src/CMakeLists.txt "option(PACKAGE_PCRE ...)" feeds the same
// "__PACKAGE_X__ compiled in" predefine convention __PACKAGE_SOCKETS__
// etc. above already use (confirmed via that package's own testsuite,
// every pcre_*.lpc test file there guards itself with
// "#ifdef __PACKAGE_PCRE__"), and the flag values themselves are
// copied verbatim from that tree's src/include/pcre_flags.h.
// Values given as plain decimal literals (1<<16 etc, pre-computed), not
// as "(1 << 16)"-shaped expressions like pcre_flags.h's own source: the
// -D flags below are assembled into one unquoted shell command string
// (popen(), same real constraint __VERSION__'s own comment above
// documents) and both the embedded spaces and the parens in that literal
// form would break shell parsing (parens unescaped and unquoted open a
// subshell in bash), not just get word-split.
constexpr PredefinedMacro kPcrePackagePredefinedMacros[] = {
    {"__PACKAGE_PCRE__", ""},
    {"PCRE_DEFAULT", "0"},
    {"PCRE_I", "65536"},
    {"PCRE_M", "131072"},
    {"PCRE_S", "262144"},
    {"PCRE_U", "524288"},
    {"PCRE_X", "1048576"},
    {"PCRE_A", "2097152"},
};

// compiledFilename is the object being compiled's own normalized LPC
// path (leading '/', no ".c" -- ObjectManager::compile()'s own
// "filename", not the real on-disk path).
std::string buildPredefinedMacroFlags(const Config& config, const std::string& compiledFilename) {
    std::ostringstream flags;
    for (const auto& macro : kFluffosPredefinedMacros) {
        flags << " -D" << macro.name << "=" << macro.value;
    }
    for (const auto& macro : kFluffosRuntimePredefinedMacros) {
        flags << " -D" << macro.name << "=" << macro.value;
    }
    for (const auto& macro : kPcrePackagePredefinedMacros) {
        flags << " -D" << macro.name << "=" << macro.value;
    }
    flags << " -D__PORT__=" << config.port();
    flags << " -DMUD_NAME=\\\"" << config.mudName() << "\\\"";

    // __FILE__/__DIR__: real lex.c's own start_new_file() (confirmed
    // directly, not guessed): "__FILE__" is "/" + current_file (the
    // compiled object's own mudlib-relative path, WITH its ".c"
    // extension -- confirmed against lil_0.3's own reference testsuite,
    // single/tests/efuns/file_name.c's own "ASSERT(file_name() + \".c\"
    // == __FILE__)"), and "__DIR__" is that same string truncated right
    // after its own last '/', trailing slash kept. Explicitly defined
    // here (a "-D" always overrides a same-named compiler built-in) since
    // gcc's own cpp already predefines __FILE__ itself, to a value that
    // is real but wrong for LPC purposes: this driver's own "# 1
    // \"originalPath\"" line-marker rewrite (stageSourceForPreprocessing,
    // needed so compile-error messages point at the real file) makes
    // gcc's built-in __FILE__ resolve to that same real *host filesystem*
    // absolute path, not the LPC-visible mudlib path any real mudlib
    // code actually expects. gcc's cpp has no built-in __DIR__ at all (it
    // is not a standard C macro), so without this it stays a bare,
    // undefined identifier -- confirmed real and hard-failing, not
    // theoretical: lil_0.3's own single/tests/efuns/shadow.c "new(__DIR__
    // \"badshad\", 1)" could not compile at all (an undefined identifier
    // directly followed by a string literal, with no operator between
    // them for this driver's own already-working adjacent-string-literal
    // concatenation, Parser.cpp's own parsePrimary(), to ever reach).
    std::string lpcFile = compiledFilename + ".c";
    std::string lpcDir = lpcFile.substr(0, lpcFile.find_last_of('/') + 1);
    flags << " -D__FILE__=\\\"" << lpcFile << "\\\"";
    flags << " -D__DIR__=\\\"" << lpcDir << "\\\"";
    return flags.str();
}

// Forward declaration: maskHashQuote() is defined further down this same
// anonymous namespace, but rewriteAbsoluteIncludesRecursive() below
// needs to apply it to every spliced-in file's own content too, not
// just the outermost file's.
std::string maskHashQuote(const std::string& source);

// Real LPC/FluffOS resolves a quoted #include path that starts with '/'
// against the mudlib root, the same convention as every other absolute
// LPC path in this codebase (inherit "/path";, clone_object("/path"),
// etc) -- confirmed live: secure/SimulEfun/SimulEfun.c #includes ~50
// other files this way ("#include \"/secure/SimulEfun/identify.c\"").
// A real system cpp has no concept of a mudlib root and treats a
// leading '/' as the actual filesystem root, so without this rewrite it
// fails outright ("No such file or directory"). This driver shells out
// to a real cpp (see the module comment above runPreprocessor), so the
// fix has to happen before cpp ever sees the source.
//
// Rewriting the #include line's own path text to a mudlib-root-relative
// form (letting cpp's own quote-search plus this driver's "-I '.'"
// resolve it) is enough for the *outermost* file being compiled, but not
// for an absolute include belonging to a file reached transitively via a
// real cpp #include of its own -- found live against a real third-party
// mudlib corpus (row 3.8's TMI-2 boot attempt): std/object/sec_ob.c's own
// real "#include \"/std/object/prop.c\"" (already correctly rewritten,
// resolved, and inlined by cpp) itself contains a further real
// "#include \"/std/object/prop_logic.c\"" -- that second include's raw
// text was never touched by this function (it only ever ran once, on
// sec_ob.c's own outer text, before cpp started), so cpp hit it during
// its own recursive expansion still bearing the untouched leading '/',
// and (confirmed directly, not assumed: a leading '/' quoted include is
// resolved as a literal OS-absolute path by real cpp unconditionally, no
// -I search path applies to it at all, and GCC's own "--sysroot" flag
// does not redirect quote-form includes either) failed outright.
//
// Fixed by splicing the real target file's own content directly in
// place of the #include line (recursing into that spliced content the
// same way, so an absolute include nested arbitrarily deep resolves
// correctly) instead of only ever rewriting the path text -- real cpp
// itself already works exactly this way for an ordinary, resolvable
// #include (textual substitution), this is the identical operation,
// just performed here for the one case (an absolute leading-'/' quoted
// path) real cpp cannot resolve on its own. A target that cannot
// actually be read from disk (a genuinely missing file, or a real
// include cycle -- activeIncludes guards against ever splicing the same
// real path into itself) falls back to the original path-text rewrite,
// so real cpp's own "No such file or directory" diagnostic still
// surfaces for an actually-broken include rather than this driver
// silently swallowing it.
// Real C's own "computed include" form (C99/C11 6.10.2p4: if a
// '#include' line's own pp-tokens do not already form a
// '<h-char-sequence>' or a '"q-char-sequence"', they are macro-expanded
// first, and the result must form one of those two shapes instead) --
// confirmed real and reachable, not a corner of the standard nobody
// actually uses: Dead Souls 3.8.2's own real secure/include/global.h
// does exactly "#define CONFIG_H \"/secure/include/config.h\"" then
// "#include CONFIG_H" a few lines later, in the very file this
// mudlib's own real global_include_file setting (<global.h>) implicitly
// #includes into *every single object*. Fixed narrowly at first: a
// simple, single-token object-like "#define NAME \"value\"" is recorded
// as it is seen (real cpp macro scope is whole-compilation, not
// per-file, so this map threads through the same recursion the spliced-
// include text already does); a later "#include NAME" line with no '<'
// or '"' of its own is resolved through that map first.
//
// ROADMAP.md row 3.10's own "not just the two special-cased entry
// points" follow-up: this whole pass previously only ever recursed into
// an *absolute* quoted include ("#include \"/...\"", literal or
// macro-computed) -- an ordinary relative quoted ("#include \"foo.h\"")
// or angle-bracket ("#include <foo.h>") include was always left
// completely untouched for real cpp's own "-I" search to resolve
// blind, with no opportunity for anything *inside* that file to ever
// reach this driver's own mudlib-root-aware logic. Found live
// necessary, not a speculative generalization: Dead Souls 3.8.2's own
// real secure/include/logs.h is reached from secure/daemon/master.c via
// an entirely ordinary "#include <logs.h>" (real cpp resolves it fine
// via "-I", never touching this pass at all), and logs.h itself
// contains a *third*, independent real "#include CONFIG_H" of its own
// -- a corpus-wide scan found 247 real files using the bare-macro-name
// "#include NAME" form at all, so any of them could be the next one.
// Generalized here: every '#include' this pass encounters, of every
// form (angle-bracket, quoted-relative, quoted-absolute, and each of
// those reached via a computed macro name) is now resolved and spliced
// by this driver itself, real cpp's own "-I" search never gets a
// chance to run on local mudlib content at all any more (its own
// diagnostic still surfaces, unchanged, for anything genuinely missing
// -- see the two-branch fallback at the bottom of the loop below).
//
// This reuses activeIncludes/macroDefs completely unchanged (real cpp
// macro scope and cycle detection both already worked exactly the same
// way for every file this pass touches, absolute-only or not -- nothing
// about generalizing which files get spliced changes what "the same
// macro name means the same thing everywhere" or "don't re-splice a
// file already being spliced" actually require). The one new piece of
// state is currentDir: a relative quoted include is real cpp's own
// "search the including file's own directory first" case (confirmed
// against real cpp's documented quote-form search order), which an
// absolute path never needs (always mudlibRoot-relative, matching every
// other absolute LPC path in this codebase) and an angle-bracket
// include never gets (real cpp's own "<>" form skips the including
// file's own directory entirely, only ever searching "-I" dirs) --
// currentDir is updated to each newly-spliced file's own real directory
// before recursing into it, so a file reached two levels deep resolves
// its own relative includes against *its own* directory, not the
// outermost compiled file's.
std::string rewriteAbsoluteIncludesRecursive(const std::string& source, const std::string& mudlibRoot,
                                              const std::vector<std::string>& includeDirs,
                                              const std::string& currentDir,
                                              std::unordered_set<std::string>& activeIncludes,
                                              std::unordered_map<std::string, std::string>& macroDefs,
                                              int depth) {
    // Real, pathological include cycles aside (already guarded by
    // activeIncludes), 64 is far deeper than any genuine mudlib's own
    // include nesting -- the deepest real corpus case seen so far
    // (Dead Souls 3.8.2's own master.c -> logs.h -> config.h, or
    // TMI-2's own sec_ob.c -> prop.c -> prop_logic.c) is depth 2 or 3.
    // A pure safety net, not expected to ever actually fire.
    if (depth > 64) return source;

    std::istringstream in(source);
    std::ostringstream out;
    std::string line;
    while (std::getline(in, line)) {
        bool spliced = false;
        size_t hashPos = line.find_first_not_of(" \t");
        if (hashPos != std::string::npos && line[hashPos] == '#') {
            // Identify the directive keyword itself (the run of letters
            // right after '#', skipping only whitespace) once, rather
            // than independently searching for the substrings "define"/
            // "include" anywhere on the line -- the latter would false-
            // positive on, say, a #define whose own quoted *value*
            // happens to contain the word "include" (a real risk this
            // fix's own citation, secure/include/global.h, does not
            // trigger, but a plausible one elsewhere in a real corpus,
            // not worth risking).
            size_t kwStart = line.find_first_not_of(" \t", hashPos + 1);
            size_t kwEnd = kwStart;
            while (kwEnd != std::string::npos && kwEnd < line.size() &&
                   std::isalpha(static_cast<unsigned char>(line[kwEnd]))) {
                ++kwEnd;
            }
            bool isDefine = kwStart != std::string::npos &&
                             line.compare(kwStart, kwEnd - kwStart, "define") == 0;
            bool isInclude = kwStart != std::string::npos &&
                              line.compare(kwStart, kwEnd - kwStart, "include") == 0;

            if (isDefine) {
                // "#define NAME "value"" -- capture the macro name and a
                // quoted string value only; anything else (no value, an
                // unquoted value, a function-like macro's own "(") is
                // simply not recorded, and any '#include' referring to
                // it falls through to real cpp exactly as before, which
                // will correctly fail on it the same way it already
                // would have without this whole fix.
                size_t nameStart = line.find_first_not_of(" \t", kwEnd);
                if (nameStart != std::string::npos) {
                    size_t nameEnd = nameStart;
                    while (nameEnd < line.size() &&
                           (std::isalnum(static_cast<unsigned char>(line[nameEnd])) || line[nameEnd] == '_')) {
                        ++nameEnd;
                    }
                    if (nameEnd > nameStart && (nameEnd >= line.size() || line[nameEnd] != '(')) {
                        std::string macroName = line.substr(nameStart, nameEnd - nameStart);
                        size_t q1 = line.find('"', nameEnd);
                        if (q1 != std::string::npos) {
                            size_t q2 = line.find('"', q1 + 1);
                            if (q2 != std::string::npos) {
                                macroDefs[macroName] = line.substr(q1 + 1, q2 - q1 - 1);
                            }
                        }
                    }
                }
            } else if (isInclude) {
                // Extract the raw target text and its own delimiter
                // style, either directly from this line (the ordinary
                // case) or, when neither '<' nor '"' appears on the
                // line at all, via a single bare macro name already
                // recorded by the "#define" branch above (real C's own
                // "computed include", see this function's own header
                // comment).
                size_t quotePos = line.find('"', kwEnd);
                size_t anglePos = line.find('<', kwEnd);
                std::string targetPath;
                char delim = 0;
                bool viaMacro = false;
                std::string macroExpandedLine;

                if (quotePos != std::string::npos) {
                    size_t endQuote = line.find('"', quotePos + 1);
                    if (endQuote != std::string::npos) {
                        targetPath = line.substr(quotePos + 1, endQuote - quotePos - 1);
                        delim = '"';
                    }
                } else if (anglePos != std::string::npos) {
                    size_t endAngle = line.find('>', anglePos + 1);
                    if (endAngle != std::string::npos) {
                        targetPath = line.substr(anglePos + 1, endAngle - anglePos - 1);
                        delim = '<';
                    }
                } else {
                    size_t nameStart = line.find_first_not_of(" \t", kwEnd);
                    if (nameStart != std::string::npos) {
                        size_t nameEnd = line.find_last_not_of(" \t\r") + 1;
                        if (nameEnd > nameStart) {
                            std::string macroName = line.substr(nameStart, nameEnd - nameStart);
                            auto it = macroDefs.find(macroName);
                            if (it != macroDefs.end()) {
                                targetPath = it->second;
                                delim = '"'; // macroDefs only ever records a quoted value
                                viaMacro = true;
                                macroExpandedLine = line.substr(0, kwStart) + "include \"" + targetPath + "\"";
                            }
                        }
                    }
                }

                if (delim != 0) {
                    bool isAbsolute = !targetPath.empty() && targetPath[0] == '/';
                    std::string realPath;
                    bool found = false;
                    if (isAbsolute) {
                        realPath = mudlibRoot + targetPath;
                        std::ifstream probe(realPath);
                        found = static_cast<bool>(probe);
                    } else {
                        // Real cpp's own quote-search precedence: the
                        // including file's own directory first, only
                        // for the quoted ('"') form -- angle-bracket
                        // ('<') never searches it, matching real C's
                        // own "<>" semantics exactly. Both then fall
                        // back to the configured include dirs, in
                        // order, same as real cpp's own "-I" list.
                        std::vector<std::string> searchDirs;
                        if (delim == '"' && !currentDir.empty()) searchDirs.push_back(currentDir);
                        for (const auto& d : includeDirs) searchDirs.push_back(d);
                        for (const auto& d : searchDirs) {
                            std::string candidate = d + "/" + targetPath;
                            std::ifstream probe(candidate);
                            if (probe) {
                                realPath = candidate;
                                found = true;
                                break;
                            }
                        }
                    }

                    if (found && !activeIncludes.count(realPath)) {
                        std::ifstream target(realPath);
                        std::ostringstream targetBuf;
                        targetBuf << target.rdbuf();
                        activeIncludes.insert(realPath);
                        std::string targetDir = realPath.substr(0, realPath.find_last_of('/'));
                        out << rewriteAbsoluteIncludesRecursive(
                                   maskHashQuote(targetBuf.str()), mudlibRoot, includeDirs, targetDir,
                                   activeIncludes, macroDefs, depth + 1)
                            << "\n";
                        activeIncludes.erase(realPath);
                        spliced = true;
                    }

                    if (!spliced) {
                        // Could not resolve/splice ourselves (genuinely
                        // missing, or an already-active real include
                        // cycle -- a real, correctly-guarded header
                        // still no-ops itself the normal way, via its
                        // own "#ifndef" once cpp processes the fully
                        // assembled text, exactly as real cpp's own
                        // recursive #include handling already relies
                        // on): fall back to letting real cpp's own "-I"
                        // search (unchanged) have a fair attempt, same
                        // discipline this whole pass has always used.
                        // An absolute target's line text is rewritten
                        // to a mudlib-root-relative form first (real
                        // cpp has no concept of a mudlib root, a bare
                        // leading '/' would resolve against the actual
                        // host filesystem root); a macro-computed
                        // target (of any form) is rewritten to its own
                        // literal expansion, since cpp cannot resolve
                        // an opaque macro name as a filename at all; an
                        // already-literal, non-absolute include (the
                        // ordinary case) is left completely untouched.
                        if (isAbsolute) {
                            line = viaMacro ? macroExpandedLine : line;
                            size_t p = line.find('"', kwEnd);
                            if (p != std::string::npos && p + 1 < line.size() && line[p + 1] == '/') {
                                line.insert(p + 1, mudlibRoot);
                            }
                        } else if (viaMacro) {
                            line = macroExpandedLine;
                        }
                    }
                }
            }
        }
        if (!spliced) out << line << "\n";
    }
    return out.str();
}

// Real FluffOS's own driver-internal "efun_defined(name)" (distinct
// from the standard cpp "defined(name)", which real system cpp already
// evaluates correctly on its own, so left untouched here): a real,
// hand-rolled preprocessor construct usable inside a real "#if"/"#elif"
// to conditionally compile mudlib code based on whether a given
// optional efun is actually compiled into the real driver -- confirmed
// directly against Dead Souls 3.8.2's own bundled fluffos-2.23-ds03/
// lex.c:3040 ("if (strcmp(yytext, \"defined\") == 0 || strcmp(yytext,
// \"efun_defined\") == 0) { ... if (efund) { ihe = lookup_ident(yytext);
// flag = (ihe && ihe->dn.efun_num != -1); } ... }"), not assumed. This
// driver shells out to real, unmodified system cpp, which has no
// knowledge of this driver-specific extension at all -- real corpus:
// Dead Souls 3.8.2's own secure/sefun/sefun.c (its own real "#if
// efun_defined(query_charmode)"), lib/editor.c, and cmds/players/env.c
// all use it, and system cpp's own real "#if" constant-expression
// evaluator trips over the bare, unexpanded "efun_defined" identifier
// (a real cpp "#if" defaults *any* undefined bare identifier to 0, real
// C99 6.10.1p4) immediately followed by "(", producing "0(query_charmode)"
// -- system cpp's own real "missing binary operator before token '('"
// diagnostic, blocking secure/sefun/sefun.c from compiling at all.
// Fixed by rewriting "efun_defined(name)" to a literal "1" or "0" before
// system cpp ever sees it, using the real driver's own actual efun
// table (via efunExists, injected from main.cpp -- see
// ObjectManager::setEfunExistsChecker()'s own comment for why this
// cannot simply call EfunTable directly from here) -- matching real
// semantics exactly, not guessing which optional efuns this driver
// happens to have. Scoped to only "#if"/"#elif" lines specifically,
// matching the one real grammatical position this construct actually
// occupies, rather than a blanket text substitution anywhere the bare
// word "efun_defined" might appear.
std::string rewriteEfunDefined(const std::string& source,
                                const std::function<bool(const std::string&)>& efunExists) {
    std::istringstream in(source);
    std::ostringstream out;
    std::string line;
    while (std::getline(in, line)) {
        size_t hashPos = line.find_first_not_of(" \t");
        if (hashPos != std::string::npos && line[hashPos] == '#') {
            size_t kwStart = line.find_first_not_of(" \t", hashPos + 1);
            size_t kwEnd = kwStart;
            while (kwEnd != std::string::npos && kwEnd < line.size() &&
                   std::isalpha(static_cast<unsigned char>(line[kwEnd]))) {
                ++kwEnd;
            }
            bool isIfOrElif = kwStart != std::string::npos &&
                (line.compare(kwStart, kwEnd - kwStart, "if") == 0 ||
                 line.compare(kwStart, kwEnd - kwStart, "elif") == 0);
            if (isIfOrElif) {
                size_t searchFrom = kwEnd;
                size_t callPos;
                while ((callPos = line.find("efun_defined", searchFrom)) != std::string::npos) {
                    size_t nameStart = line.find_first_not_of(" \t", callPos + 12);
                    if (nameStart == std::string::npos || line[nameStart] != '(') {
                        searchFrom = callPos + 12;
                        continue;
                    }
                    nameStart = line.find_first_not_of(" \t", nameStart + 1);
                    size_t nameEnd = nameStart;
                    while (nameEnd != std::string::npos && nameEnd < line.size() &&
                           (std::isalnum(static_cast<unsigned char>(line[nameEnd])) || line[nameEnd] == '_')) {
                        ++nameEnd;
                    }
                    size_t closeParen = line.find_first_not_of(" \t", nameEnd);
                    if (nameStart == std::string::npos || nameEnd == nameStart ||
                        closeParen == std::string::npos || line[closeParen] != ')') {
                        searchFrom = callPos + 12;
                        continue;
                    }
                    std::string efunName = line.substr(nameStart, nameEnd - nameStart);
                    std::string replacement = efunExists(efunName) ? "1" : "0";
                    line.replace(callPos, closeParen + 1 - callPos, replacement);
                    searchFrom = callPos + replacement.size();
                }
            }
        }
        out << line << "\n";
    }
    return out.str();
}

// ROADMAP.md row 1.2/1.3's own scoping note: real system cpp (see
// runPreprocessor's own module comment below) hard-errors on any line
// whose first non-whitespace character is '#' and does not match a real
// preprocessor directive it recognizes ("invalid preprocessing
// directive") -- and LDMud's own "#'name" closure-literal syntax is
// exactly that shape. A completely ordinary "#'some_function;" statement
// written on its own line would otherwise take the *whole file's*
// preprocessing down before this driver's own Lexer/Parser ever run,
// regardless of what they are taught to recognize -- confirmed directly
// against real cpp's own documented directive-parsing rule (only the
// first non-whitespace token on a line is ever treated as a directive
// introducer), not assumed. This was the one genuinely architecture-
// touching prerequisite the scoping note flagged as needed before any
// "#'" lexer/parser work could land at all.
//
// Fixed here by masking every "#'" occurrence in the raw source (not
// just line-initial ones -- simpler and exactly as correct, since a
// mid-line "#'" round-trips losslessly through this same mask/unmask
// pair whether or not it would actually have tripped cpp) to a marker
// text cpp treats as an ordinary identifier, then unmasking it back
// after cpp returns (runPreprocessor()'s own result.output). Runs
// unconditionally for every file, not gated on dialect -- nothing
// dialect-specific about the fix itself (matches how
// rewriteAbsoluteIncludesRecursive() above already runs unconditionally too); a
// file with no "#'" anywhere in it, in any dialect, is untouched either
// way. Whether a bare "#'name" is then recognized as anything by the
// Lexer/Parser stays a real per-dialect question, handled separately
// there (Lexer::tokenize(), gated on LpcDialect::LdMud).
const std::string kHashQuoteMarker = "__AMLP_HASHQUOTE_MARKER__";

std::string maskHashQuote(const std::string& source) {
    std::string result;
    result.reserve(source.size());
    for (size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '#' && i + 1 < source.size() && source[i + 1] == '\'') {
            result += kHashQuoteMarker;
            ++i; // also consume the '\'' -- the marker stands in for both characters
        } else {
            result += source[i];
        }
    }
    return result;
}

std::string unmaskHashQuote(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    size_t pos = 0;
    while (pos < text.size()) {
        if (text.compare(pos, kHashQuoteMarker.size(), kHashQuoteMarker) == 0) {
            result += "#'";
            pos += kHashQuoteMarker.size();
        } else {
            result += text[pos];
            ++pos;
        }
    }
    return result;
}

struct StagedSource {
    bool ok = false;
    std::string tempPath;
    std::string errorMessage;
};

// Writes the rewritten source (see rewriteAbsoluteIncludesRecursive()) to a fresh
// temp file for cpp to actually read, so the on-disk mudlib source is
// never touched. A synthetic "# 1 \"originalPath\"" line is prepended so
// every line-marker cpp itself emits from here on still names the real
// source file, not the temp path -- otherwise this driver's own
// compile-error messages (and any manual line-marker unwinding) would
// point at a throwaway /tmp file instead of the real one.
//
// globalIncludeFile (real FluffOS/MudOS's own "global include file"
// runtime config option, ROADMAP row 0.14): when non-empty, an
// "#include " line built from it is emitted first, ahead of the real
// file's own content -- matching real lex.c's own start_new_file()
// exactly: "if (*GLOBAL_INCLUDE_FILE) { ...; handle_include(gifile, 1);
// } else refill_buffer();" runs before a single byte of the object's
// own real source is read, for every compiled object, no exceptions.
// The configured value is used verbatim, delimiters included (real
// GLOBAL_INCLUDE_FILE already stores the raw config text with its own
// leading '"'/'<' -- confirmed directly against handle_include()'s own
// "delim = *name++ == '\"' ? '\"' : '>';", which derives the include
// style from the value's own first character, not a separate flag), so
// a driver.cfg entry of "global_include_file: <config.h>" or
// "global_include_file: \"/some/header.h\"" both work exactly as
// configured. The include line's own "# 1 \"originalPath\"" marker is
// emitted *after* it, not before -- cpp's own line-tracking naturally
// attributes anything the auto-included header itself does to that
// header's own file, and once it returns, this second marker resets
// numbering back to a clean line 1 for the real object's own content,
// so a compile error inside the object itself still reports the exact
// real line number, undisturbed by the extra line this injects ahead
// of it. Also run through rewriteAbsoluteIncludesRecursive() (same as the real
// file's own body just below) so an absolute-quoted-path form gets the
// identical mudlib-root rewrite every other #include in this driver
// already gets, rather than a second, divergent code path.
//
// An angle-bracket globalIncludeFile ("<global.h>", the overwhelmingly
// common real form -- confirmed against every vendored corpus's own
// runtime config) is resolved and *spliced* directly here, its own real
// on-disk content run through the same rewriteAbsoluteIncludesRecursive()
// pass as everything else, rather than left as a bare "#include
// <global.h>" line for real cpp's own "-I" search to resolve on its
// own. Found live necessary, not a speculative generalization: Dead
// Souls 3.8.2's own real secure/include/global.h (this mudlib's own
// configured global include file) itself contains a real macro-computed
// absolute include ("#define CONFIG_H \"/secure/include/config.h\"" then
// "#include CONFIG_H" a few lines later) that only this driver's own
// rewrite pass can resolve (see rewriteAbsoluteIncludesRecursive()'s own
// header comment for the full citation) -- real cpp's own "-I" mechanism
// finds and reads global.h itself just fine, but everything *inside* it
// is then real cpp's own problem to resolve, with no opportunity for
// this driver's own mudlib-root-aware rewrite to run on it at all. Only
// the angle-bracket form is handled specially; a quoted-form
// globalIncludeFile (config.hpp's own documented "\"/some/header.h\""
// possibility) already goes through the plain line-rewrite path below
// unchanged, since that form is already a literal, directly-scannable
// absolute quoted path the existing pass already resolves correctly
// without needing this. If the angle-bracket header cannot actually be
// found under any configured include dir, this falls back to the
// original plain "#include <name>" line, so real cpp's own "No such
// file or directory" diagnostic still surfaces for a genuinely missing
// global include file, matching this whole rewrite pass's own
// established "let real cpp's own diagnostic surface for anything this
// driver cannot itself resolve" discipline.
StagedSource stageSourceForPreprocessing(const std::string& originalPath,
                                          const std::string& mudlibRoot,
                                          const std::string& globalIncludeFile,
                                          const std::vector<std::string>& includeDirs,
                                          const std::function<bool(const std::string&)>& efunExists) {
    StagedSource result;

    std::ifstream f(originalPath);
    if (!f) {
        result.errorMessage = "source file not found: " + originalPath;
        return result;
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    f.close();

    // Shared across both the global-include-file splice below and the
    // real object's own body just after it -- real cpp macro scope is
    // whole-compilation, not per-file (a #define in one #include'd file
    // stays visible for the rest of the unit, including whatever
    // #included it), so these must be the *same* map/set across both,
    // not two independent ones. Found live necessary, not assumed:
    // secure/daemon/master.c's own real "#include ROOMS_H" (a second,
    // separate macro-computed include, the object file's own body, not
    // global.h's) depends on "#define ROOMS_H ..." from
    // secure/include/global.h, its own configured global include file --
    // with two independent maps (this function's own earlier version),
    // ROOMS_H was recorded while resolving the prefix and then simply
    // never seen again once the real file's own body was scanned.
    std::unordered_set<std::string> activeIncludes;
    std::unordered_map<std::string, std::string> macroDefs;

    std::string prefix;
    if (!globalIncludeFile.empty()) {
        bool splicedGlobalHeader = false;
        if (globalIncludeFile.size() >= 2 && globalIncludeFile.front() == '<' &&
            globalIncludeFile.back() == '>') {
            std::string headerName = globalIncludeFile.substr(1, globalIncludeFile.size() - 2);
            for (const auto& dir : includeDirs) {
                std::ifstream header(dir + "/" + headerName);
                if (header) {
                    std::ostringstream headerBuf;
                    headerBuf << header.rdbuf();
                    prefix = rewriteAbsoluteIncludesRecursive(
                        maskHashQuote(headerBuf.str()), mudlibRoot, includeDirs, dir, activeIncludes, macroDefs, 0);
                    splicedGlobalHeader = true;
                    break;
                }
            }
        }
        if (!splicedGlobalHeader) {
            // Nothing was actually found to splice, so there is no real
            // "current file's own directory" to speak of here -- the
            // value is never consulted (this synthetic one-line string
            // has no relative include of its own to resolve), passed
            // only because the function signature requires one.
            prefix = rewriteAbsoluteIncludesRecursive("#include " + globalIncludeFile + "\n",
                                                        mudlibRoot, includeDirs, mudlibRoot, activeIncludes, macroDefs, 0);
        }
    }

    std::string originalSourceDir = originalPath.substr(0, originalPath.find_last_of('/'));
    std::string rewritten = prefix + "# 1 \"" + originalPath + "\"\n" +
        rewriteAbsoluteIncludesRecursive(maskHashQuote(buf.str()), mudlibRoot, includeDirs, originalSourceDir,
                                          activeIncludes, macroDefs, 0);
    rewritten = rewriteEfunDefined(rewritten, efunExists);

    char tmpPathTemplate[] = "/tmp/amlp_src_XXXXXX";
    int fd = mkstemp(tmpPathTemplate);
    if (fd == -1) {
        result.errorMessage = "failed to create temp file for staged source";
        return result;
    }
    std::string tmpPath = tmpPathTemplate;

    ssize_t written = write(fd, rewritten.data(), rewritten.size());
    close(fd);
    if (written < 0 || static_cast<size_t>(written) != rewritten.size()) {
        std::remove(tmpPath.c_str());
        result.errorMessage = "failed to write staged source to temp file";
        return result;
    }

    result.ok = true;
    result.tempPath = tmpPath;
    return result;
}

// cpp emits line marker directives like `# 1 "file.h"`; the Lexer has no
// concept of these and they are not real LPC syntax, so drop any line
// whose first non-whitespace character is '#'.
std::string stripLineMarkers(const std::string& text) {
    std::istringstream in(text);
    std::ostringstream out;
    std::string line;
    while (std::getline(in, line)) {
        size_t i = line.find_first_not_of(" \t");
        if (i != std::string::npos && line[i] == '#') continue;
        out << line << "\n";
    }
    return out.str();
}

// Real FluffOS's own "include directories" mudos.cfg setting is a
// colon-separated *list*, not a single path (confirmed directly against
// rc.c's own "CONFIG_STR(__INCLUDE_DIRS__)"/set_inc_list(), and against
// this actual mudlib's own historical config, bin/mudos.cfg: "include
// directories : /secure/include:/include") -- lex.c consults every
// entry in order for a "#include <...>" (angle-bracket) lookup, not just
// the first. This driver's own Config previously modeled includeDir()
// as one single path, silently dropping every entry after the first --
// confirmed real and reachable, not theoretical: this mudlib's own
// "/include" (holding chat.h and vehicle.h, distinct from the ~50
// headers under "/secure/include") is never searched at all, so any
// file "#include <vehicle.h>"-ing (std/rifts_vehicle.c, real: driven by
// cmds/mortal/_drive.c, and inherited by two real domain vehicle
// objects) fails outright with "No such file or directory" the instant
// it is loaded. Splitting on ':' here, one -I per entry (each resolved
// against the mudlib root the same way the prior single-path code
// already did), matches that real list semantics exactly rather than
// hardcoding a second fixed path -- an operator listing three or more
// directories in a future driver.cfg is handled the same way with no
// further code change.
std::vector<std::string> splitIncludeDirs(const std::string& raw, const std::string& mudlibRoot) {
    std::vector<std::string> dirs;
    size_t start = 0;
    while (start <= raw.size()) {
        size_t colon = raw.find(':', start);
        std::string entry = raw.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (!entry.empty()) {
            if (entry[0] != '/') entry = mudlibRoot + "/" + entry;
            dirs.push_back(entry);
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return dirs;
}

PreprocessResult runPreprocessor(const std::string& sourcePath, const std::vector<std::string>& includeDirs,
                                  const std::string& originalSourceDir, const Config& config,
                                  const std::string& compiledFilename) {
    PreprocessResult result;

    char errPathTemplate[] = "/tmp/amlp_cpp_stderr_XXXXXX";
    int errFd = mkstemp(errPathTemplate);
    if (errFd == -1) {
        result.errorOutput = "failed to create temp file for cpp stderr output";
        return result;
    }
    close(errFd);
    std::string errPath = errPathTemplate;

    // sourcePath is a staged copy in /tmp (see stageSourceForPreprocessing),
    // not the real file's own directory, so a same-directory quoted
    // #include (e.g. secure/SimulEfun/SimulEfun.c's own "#include
    // \"SimulEfun.h\"") would otherwise resolve against /tmp instead of
    // where the real file lives. Passing the original directory as an
    // extra -I restores that lookup.
    //
    // "-I '.'" (the driver's own CWD, never chdir()'d away from anywhere
    // in this codebase -- resolveMudlibPath() and every other
    // mudlibRoot()-relative path already assume this implicitly) exists
    // for a second, distinct reason: rewriteAbsoluteIncludesRecursive() above
    // rewrites a real LPC absolute quoted #include ("#include
    // \"/sys/driver_hook.h\"") into a path relative to CWD ("mudlib/sys/
    // driver_hook.h", mudlibRoot prepended onto the original text) --
    // without CWD itself in the search list, neither of the two -I dirs
    // already here can resolve it: originalSourceDir is the including
    // file's own directory (e.g. plain "mudlib" for a top-level file),
    // giving a doubled "mudlib/mudlib/sys/..." lookup, and every entry in
    // includeDirs is already mudlibRoot-relative for the same reason.
    // Found live (mudlib/tmp_hooks_test.c's own real "#include \"/sys/
    // driver_hook.h\"", ROADMAP.md row 1.7/1.8's own live-verification
    // scratch file) -- this driver's own bundled mudlib had never
    // exercised an absolute quoted #include before that file, so this
    // was a real, latent, never-before-hit gap, not a regression.
    std::string cmd = "cpp -I '.' -I '" + originalSourceDir + "'";
    for (const auto& dir : includeDirs) {
        cmd += " -I '" + dir + "'";
    }
    cmd += buildPredefinedMacroFlags(config, compiledFilename) +
           " -x c '" + sourcePath + "' 2>'" + errPath + "'";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        result.errorOutput = "failed to launch cpp preprocessor";
        std::remove(errPath.c_str());
        return result;
    }

    std::ostringstream outBuf;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) {
        outBuf.write(buf, static_cast<std::streamsize>(n));
    }
    int status = pclose(pipe);

    std::ifstream errFile(errPath);
    std::ostringstream errBuf;
    errBuf << errFile.rdbuf();
    std::remove(errPath.c_str());
    result.errorOutput = errBuf.str();

    // Success is the exit code alone, not "stderr is empty": cpp writes
    // real warnings there too (e.g. GCC's cpp warns, but does not fail,
    // on the "#endif LABEL" trailing-token style used throughout this
    // mudlib's own secure/include/debug.h), and treating every warning
    // as a hard preprocessing failure was rejecting files with no actual
    // error in them (hit live loading secure/SimulEfun/SimulEfun.c).
    bool exitedOk = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    result.output = unmaskHashQuote(stripLineMarkers(outBuf.str()));
    result.ok = exitedOk;
    return result;
}

} // namespace

ObjectManager::ObjectManager(Config& config) : config_(config) { ++g_liveManagerCount; }

std::string ObjectManager::normalizeFilename(const std::string& filename) {
    std::string result = filename;
    if (result.size() >= 2 && result.compare(result.size() - 2, 2, ".c") == 0) {
        result = result.substr(0, result.size() - 2);
    }
    // Real LPC object pathnames are always mudlib-root-relative whether
    // or not the string itself carries a leading '/' -- confirmed live
    // against a real third-party mudlib corpus (row 3.8's TMI-2 boot
    // attempt): std/object/sec_ob.c's own real "inherit
    // \"std/object/ob_logic\";" (no leading slash), whose real target
    // (std/object/ob_logic.c) sits directly under the mudlib root at
    // exactly the mudlib-root-relative path the string already names.
    // Every caller of compile()/loadObject()/cloneObject()/etc. that
    // builds a real filesystem path from this normalized name
    // (`config_.mudlibRoot() + filename + ".c"`) has always assumed a
    // leading '/' was already present -- true for every absolute path
    // this driver's own corpus had exercised until now, but not a real
    // LPC requirement, so a relative inherit target like this one
    // produced a malformed concatenation missing the path separator
    // entirely ("...tmi2_fluffos_v3/lib" + "std/object/ob_logic.c" =
    // "...tmi2_fluffos_v3/libstd/object/ob_logic.c", confirmed live: a
    // real "source file not found" failure with exactly that missing
    // slash). Prepending '/' here when absent makes both forms resolve
    // identically, matching real semantics, for every one of this
    // function's own callers at once.
    if (result.empty() || result[0] != '/') {
        result.insert(result.begin(), '/');
    }
    return result;
}

std::shared_ptr<CompiledProgram> ObjectManager::compile(const std::string& rawFilename) {
    std::string filename = normalizeFilename(rawFilename);
    std::string path = config_.mudlibRoot() + filename + ".c";

    // Row 0.15 fix: read the file's current raw content up front, before
    // trusting any cache hit below -- this is what actually lets a
    // same-path recompile be detected at all (see programCache_'s own
    // comment). ROADMAP.md's 0.15 note confirmed the exact live failure:
    // eval.c's own rm()+destruct()+write_file()+reload cycle (and
    // wand_of_creation.c's cmd_create() doing the identical thing for a
    // reused item name) both silently kept re-running the *first* call's
    // bytecode, because the old code below returned a cache hit
    // unconditionally with no check against the file at all.
    std::ifstream f(path);
    bool sourceExists = static_cast<bool>(f);
    std::string currentSource;
    if (sourceExists) {
        std::ostringstream buf;
        buf << f.rdbuf();
        currentSource = buf.str();
    }
    f.close();

    auto cached = programCache_.find(filename);
    if (cached != programCache_.end()) {
        auto sourceIt = programSource_.find(filename);
        bool sourceUnchanged =
            sourceExists && sourceIt != programSource_.end() && sourceIt->second == currentSource;
        // Source vanished entirely since the cached compile (e.g. a
        // subsequent purge/rm with nothing yet written back) -- keep
        // serving the last good compiled program rather than failing a
        // call that never asked to recompile, matching real semantics:
        // an already-compiled program's bytecode does not stop working
        // just because its own source file was later deleted.
        if (sourceUnchanged || !sourceExists) return cached->second;
        // Otherwise: same path, but the file's own bytes genuinely
        // differ from what produced the cached entry -- fall through
        // and recompile fresh below instead of returning the stale one.
        // Objects that already hold the old CompiledProgram (via their
        // own program_ shared_ptr, LpcObject.hpp) are unaffected: this
        // only replaces this cache's own entry, never mutates the old
        // CompiledProgram object itself, so they keep running the
        // bytecode they were actually compiled and created against.
    }

    if (compiling_.count(filename)) {
        std::cerr << "[object] inherit cycle detected involving " << filename << "\n";
        return nullptr;
    }
    compiling_.insert(filename);
    struct InProgressGuard {
        std::unordered_set<std::string>& set;
        const std::string& filename;
        ~InProgressGuard() { set.erase(filename); }
    } inProgressGuard{compiling_, filename};

    if (!sourceExists) {
        std::cerr << "[object] source file not found: " << path << "\n";
        return nullptr;
    }

    std::vector<std::string> includeDirs = splitIncludeDirs(config_.includeDir(), config_.mudlibRoot());

    StagedSource staged =
        stageSourceForPreprocessing(path, config_.mudlibRoot(), config_.globalIncludeFile(), includeDirs,
                                     efunExistsChecker_);
    if (!staged.ok) {
        std::cerr << "[object] " << staged.errorMessage << "\n";
        return nullptr;
    }
    std::string originalSourceDir = path.substr(0, path.find_last_of('/'));
    PreprocessResult preprocessed =
        runPreprocessor(staged.tempPath, includeDirs, originalSourceDir, config_, filename);
    std::remove(staged.tempPath.c_str());
    if (!preprocessed.ok) {
        std::cerr << "[object] preprocessing failed for " << path << ":\n"
                   << preprocessed.errorOutput << "\n";
        return nullptr;
    }

    try {
        // ROADMAP.md row 1.2/1.3's own "zero behavior change" plumbing
        // slice: Config::dialect() (row 1.1) already defaults to
        // "fluffos" for any config that never sets the key, and
        // dialectFromString() throws on an unrecognized string (already
        // used identically in DialectSelect.cpp's own
        // makeBootApiForConfig()) -- so this is the one real call site
        // where the dialect actually reaches the Lexer/Parser, every
        // other construction (this driver's own test suite included)
        // still defaults to LpcDialect::FluffOS via the constructors'
        // own default argument.
        LpcDialect dialect = dialectFromString(config_.dialect());
        Lexer lexer(preprocessed.output, dialect);
        auto tokens = lexer.tokenize();
        Parser parser(std::move(tokens), dialect);
        auto ast = parser.parseProgram();

        // Resolve "inherit \"path\";" targets by recursively compiling each
        // one (reusing this same cache, so a file inherited by several
        // others is only compiled once) *before* generating this file's
        // own code, so CodeGen can flatten the parents' object variables
        // in ahead of this file's own -- see CodeGen::generate()'s
        // inheritedObjectVarNames parameter. Only single-level lookups are
        // done directly here; each parent's *own* inherits, if any, were
        // already resolved recursively when that parent itself went
        // through this same function, so multi-level chains still work
        // (see Bytecode.hpp's CompiledProgram::inheritedPrograms comment).
        // Alongside the flattened name list, also build ancestorBaseOffsets
        // (see Bytecode.hpp's own comment on that member): for each direct
        // parent, its own object variables start at "base" within this
        // file's flattened layout (the count of inherited names collected
        // so far); every ancestor *that parent* itself already knows about
        // (its own ancestorBaseOffsets, from when it was compiled) is
        // re-recorded here shifted by that same base, composing offsets
        // correctly across arbitrarily deep chains, not just one level of
        // direct siblings.
        std::vector<std::shared_ptr<CompiledProgram>> parents;
        std::vector<std::string> inheritedObjectVarNames;
        std::unordered_map<const CompiledProgram*, int> ancestorBaseOffsets;
        for (const auto& inheritPath : ast->inherits) {
            auto parentProgram = compile(inheritPath);
            if (!parentProgram) {
                std::cerr << "[object] " << path << ": failed to compile inherited file \""
                          << inheritPath << "\"\n";
                return nullptr;
            }
            int base = static_cast<int>(inheritedObjectVarNames.size());
            ancestorBaseOffsets[parentProgram.get()] = base;
            for (const auto& entry : parentProgram->ancestorBaseOffsets) {
                ancestorBaseOffsets[entry.first] = base + entry.second;
            }
            parents.push_back(parentProgram);
            inheritedObjectVarNames.insert(inheritedObjectVarNames.end(),
                                            parentProgram->objectVarNames.begin(),
                                            parentProgram->objectVarNames.end());
        }

        CodeGen codegen;
        auto program = std::make_shared<CompiledProgram>(
            codegen.generate(*ast, inheritedObjectVarNames));
        program->inheritedPrograms = std::move(parents);
        program->ancestorBaseOffsets = std::move(ancestorBaseOffsets);

        programCache_[filename] = program;
        programSource_[filename] = currentSource;
        return program;
    } catch (const LpcRuntimeError& e) {
        std::cerr << "[object] compile error in " << path << ": " << e.what() << "\n";
        return nullptr;
    } catch (const std::exception& e) {
        std::cerr << "[object] unexpected error compiling " << path << ": " << e.what() << "\n";
        return nullptr;
    }
}

bool ObjectManager::loadMasterObject() {
    master_ = loadObject(config_.masterFile());
    if (master_) captureBootUids();
    return master_ != nullptr;
}

void ObjectManager::captureBootUids() {
    if (!vm_ || !master_) return;

    // real master.c:108 "ret = apply_master_ob(APPLY_GET_ROOT_UID, 0);".
    // callFunction() returns a monostate Value for a function the master
    // does not define, so a non-string result here just leaves the model
    // inactive (real exits(-1) instead, but this driver treats "no
    // get_root_uid()" as "this mudlib was built without PACKAGE_UIDS").
    Value rootRet;
    try {
        rootRet = vm_->callFunction(master_, "get_root_uid", {});
        if (std::holds_alternative<std::monostate>(rootRet.data)) {
            // LDMud renamed this apply get_master_uid in 3.2.1@40
            // (doc/master/get_master_uid HISTORY, and BootApi::
            // masterUidApply()'s own comment). Try it as a fallback so
            // an LDMud-dialect mudlib (temp/core-lib) activates the same
            // model.
            rootRet = vm_->callFunction(master_, "get_master_uid", {});
        }
    } catch (const std::exception&) {
        return;
    }
    auto* rootStr = std::get_if<std::string>(&rootRet.data);
    if (!rootStr || rootStr->empty()) return;
    uidModel_.rootUid = *rootStr;

    // real master.c:126 "ret = apply_master_ob(APPLY_GET_BACKBONE_UID,
    // 0);". Apply name "get_bb_uid" (applies_table.c:12); LDMud uses the
    // same string. Only give_uid_to_object()'s AUTO_TRUST_BACKBONE branch
    // consumes it (resolveObjectUids(), reached from assignObjectUid()),
    // and that branch is off unless the auto_trust_backbone config key is
    // set, so a missing get_bb_uid() is not fatal here (real exits(-1)).
    try {
        Value bbRet = vm_->callFunction(master_, "get_bb_uid", {});
        if (auto* bb = std::get_if<std::string>(&bbRet.data); bb && !bb->empty()) {
            uidModel_.backboneUid = *bb;
        }
    } catch (const std::exception&) {
        // leave backboneUid unset
    }

    // real: the AUTO_TRUST_BACKBONE compile-time flag. This driver reads
    // it from the "auto_trust_backbone" config key (Config::
    // autoTrustBackbone(), default false, matching the vendored
    // local_options #undef).
    uidModel_.autoTrustBackbone = config_.autoTrustBackbone();

    // real master.c:121-122 "master_ob->uid = set_root_uid(ret->u.string);
    // master_ob->euid = master_ob->uid;".
    master_->setUid(*rootStr);
    master_->setEuid(*rootStr);
}

void ObjectManager::assignObjectUid(const std::shared_ptr<LpcObject>& obj,
                                     const std::string& filename) {
    if (!obj || !vm_ || !master_ || !uidModel_.active()) return;

    // real simulate.c:149-151 "push_malloced_string(add_slash(ob->obname));
    // ret = apply_master_ob(APPLY_CREATOR_FILE, 1);".
    std::string slashPath =
        (filename.empty() || filename[0] == '/') ? filename : ("/" + filename);
    std::string creatorName;
    bool haveCreator = false;
    if (vm_->functionExists(master_, "creator_file")) {
        try {
            Value ret = vm_->callFunction(master_, "creator_file", {Value(slashPath)});
            if (auto* s = std::get_if<std::string>(&ret.data)) {
                creatorName = *s;
                haveCreator = true;
            }
        } catch (const std::exception&) {
            // fall through to the safe default below
        }
    }
    if (!haveCreator) {
        // Named divergence from real simulate.c:157-160, which destructs
        // the object and errors ("return value of master::creator_file()
        // was not a string"). This driver keeps the object and makes it
        // root-owned so a mudlib that defines get_root_uid() but not a
        // working creator_file() still boots. rootUid is set here because
        // uidModel_.active() is true.
        obj->setUid(*uidModel_.rootUid);
        return;
    }

    auto caller = vm_->currentObject();
    std::optional<std::string> loaderUid = caller ? caller->uid() : uidModel_.rootUid;
    std::optional<std::string> loaderEuid = caller ? caller->euid() : uidModel_.rootUid;

    ResolvedObjectUids r =
        resolveObjectUids(uidModel_, creatorName, loaderUid, loaderEuid);
    obj->setUid(r.uid);
    obj->setEuid(r.euid);
}

bool ObjectManager::loadSimulEfunObject() {
    if (config_.simulEfunFile().empty()) return false;
    simulEfunObject_ = loadObject(config_.simulEfunFile());
    return simulEfunObject_ != nullptr;
}

bool ObjectManager::sourceFileExists(const std::string& rawFilename) const {
    std::string filename = normalizeFilename(rawFilename);
    std::string path = config_.mudlibRoot() + filename + ".c";
    struct stat st;
    // Matches real int_load_object()'s own check (simulate.c): "stat(
    // real_name, &c_st) == -1 || S_ISDIR(c_st.st_mode)" -- a directory
    // sharing the requested name does not count as a source file
    // either.
    return ::stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
}

std::shared_ptr<LpcObject> ObjectManager::loadVirtualObject(const std::string& filename) {
    // See simulate.c's load_virtual_object(): "if (!master_ob) { ...
    // return 0; }" -- no master loaded yet (e.g. this call is itself
    // trying to load the master file), nothing to ask.
    if (!master_ || !vm_) return nullptr;

    if (virtualCompiling_.count(filename)) {
        std::cerr << "[object] compile_object() recursion detected for " << filename << "\n";
        return nullptr;
    }
    virtualCompiling_.insert(filename);
    struct InProgressGuard {
        std::unordered_set<std::string>& set;
        const std::string& filename;
        ~InProgressGuard() { set.erase(filename); }
    } inProgressGuard{virtualCompiling_, filename};

    // real simulate.c: "push_malloced_string(add_slash(name));
    // push_number(clone); ... apply_master_ob(APPLY_COMPILE_OBJECT,
    // argc);" -- clone is 0 here (int_load_object()'s own call site
    // always passes 0; only clone_object() on an already-virtual
    // object passes a nonzero clone count, a path this driver does not
    // implement -- see loadVirtualObject()'s own header comment).
    // master.c's own compile_object(string str) declares only one
    // parameter, so the extra int argument is simply unused by it, the
    // same non-strict-arg-count LPC calling convention every other
    // apply in this driver already relies on.
    Value result;
    try {
        result = vm_->applyMaster("compile_object", {Value(filename), Value(int64_t{0})});
    } catch (const std::exception& e) {
        std::cerr << "[object] compile_object(" << filename << ") failed: " << e.what() << "\n";
        return nullptr;
    }

    if (!std::holds_alternative<std::shared_ptr<LpcObject>>(result.data)) {
        // Real driver: "if (!v || (v->type != T_OBJECT)) return 0;" --
        // compile_object() declining (returning 0/void) means this
        // path genuinely does not exist, virtual or otherwise. Not
        // logged as an error here: this is the normal "no such object"
        // outcome for the overwhelming majority of missing-file
        // lookups, which are not virtual paths at all.
        return nullptr;
    }
    auto ob = std::get<std::shared_ptr<LpcObject>>(result.data);
    if (!ob) return nullptr;

    // real load_virtual_object(): renames the returned object to the
    // requested virtual path and reinserts it into the object hash
    // under that name (SETOBNAME + enter_object_hash()) -- from this
    // point on the object IS "filename" as far as file_name()/
    // base_name() and any later find_object()/load_object() for the
    // same path are concerned, indistinguishable from having been
    // compiled there directly.
    ob->rebindFilename(filename);
    loaded_[filename] = ob;
    // real object.h O_VIRTUAL: set once an object is confirmed to have
    // actually come back from compile_object() rather than a direct
    // on-disk compile -- backs virtualp().
    ob->setIsVirtual(true);
    return ob;
}

std::shared_ptr<LpcObject> ObjectManager::loadObject(const std::string& rawFilename) {
    std::string filename = normalizeFilename(rawFilename);
    auto existing = loaded_.find(filename);
    if (existing != loaded_.end()) return existing->second;

    if (!sourceFileExists(filename)) {
        auto virtualObj = loadVirtualObject(filename);
        if (virtualObj) return virtualObj;
        std::cerr << "[object] source file not found: "
                   << config_.mudlibRoot() << filename << ".c"
                   << " (and compile_object() did not provide a virtual object)\n";
        return nullptr;
    }

    auto program = compile(filename);
    if (!program) return nullptr;

    auto obj = std::make_shared<LpcObject>(filename, program);
    loaded_[filename] = obj;
    LiveObjectRegistry::add(obj);
    initPrivsForObject(obj, filename);
    // real init_object() -> give_uid_to_object() (simulate.c), run
    // before create() so a "void create() { seteuid(getuid()); }" body
    // sees the right owner uid. No-op unless the uid model is active.
    assignObjectUid(obj, filename);

    // A runtime error thrown out of create() (a missing efun, a bad
    // sscanf(), etc.) must fail this one object's load, not crash the
    // whole driver process -- compile()'s own try/catch below only covers
    // the lex/parse/codegen phase, not this call, since compiling a file
    // successfully and having its create() throw are different failures
    // (see the compile-error path's own [object] message for the other
    // one).
    if (vm_) {
        try {
            runObjectVarInitializers(obj, *program);
            vm_->callFunction(obj, "create", {});
        } catch (const std::exception& e) {
            std::cerr << "[object] create() failed for " << filename << ": " << e.what() << "\n";
            loaded_.erase(filename);
            return nullptr;
        }
    }
    armResetAndCleanup(obj);

    return obj;
}

void ObjectManager::runObjectVarInitializers(const std::shared_ptr<LpcObject>& obj,
                                              const CompiledProgram& program) {
    if (!vm_) return;
    for (const auto& parent : program.inheritedPrograms) {
        if (parent) runObjectVarInitializers(obj, *parent);
    }
    vm_->callFunctionInProgram(obj, program, "$objvarinit", {});
}

std::shared_ptr<LpcObject> ObjectManager::cloneObject(const std::string& rawFilename) {
    // real simulate.c:545-549, the very first thing clone_object() does:
    //
    //   #ifdef PACKAGE_UIDS
    //       if (current_object && current_object->euid == 0) {
    //           error("Object must call seteuid() prior to calling clone_object().\n");
    //       }
    //   #endif
    //
    // A caller whose effective uid is NULL is refused before find_object()
    // or any compile runs: give_uid_to_object() on the new clone would
    // otherwise have no euid to copy from the loader. Gated on
    // uidModel_.active(), this driver's stand-in for the PACKAGE_UIDS
    // compile flag (the same gate assignObjectUid()/captureBootUids() use).
    // The condition matches real exactly: only when there IS a current
    // object and its euid() is std::nullopt (real euid == 0 == NULL); a
    // direct clone with no current object, or a caller that has seteuid()d
    // itself, is unaffected. Message text is verbatim from real, including
    // the trailing newline, since a mudlib may catch() and match on it.
    if (uidModel_.active() && vm_) {
        auto callerOb = vm_->currentObject();
        if (callerOb && !callerOb->euid().has_value()) {
            throw LpcRuntimeError(
                "Object must call seteuid() prior to calling clone_object().\n");
        }
    }

    std::string filename = normalizeFilename(rawFilename);
    auto program = compile(filename);
    if (!program) return nullptr;

    auto obj = std::make_shared<LpcObject>(filename, program);
    LiveObjectRegistry::add(obj);
    initPrivsForObject(obj, filename);
    // real clone_object() -> give_uid_to_object(new_ob) (simulate.c:309),
    // same per-object uid/euid assignment as a fresh load. No-op unless
    // the uid model is active.
    assignObjectUid(obj, filename);
    // real simulate.c's own "new_ob->flags |= O_CLONE | O_WILL_CLEAN_UP;"
    // (clone_object(), simulate.c:2448) -- O_CLONE set unconditionally,
    // right alongside O_WILL_CLEAN_UP, at construction, not deferred to
    // armResetAndCleanup() below (which only sets the willCleanUp() half
    // for both loadObject() and cloneObject() uniformly). See
    // LpcObject::isClone()'s own header comment for what this backs.
    obj->setIsClone(true);

    if (vm_) {
        try {
            runObjectVarInitializers(obj, *program);
            vm_->callFunction(obj, "create", {});
        } catch (const std::exception& e) {
            std::cerr << "[object] create() failed for " << filename << ": " << e.what() << "\n";
            return nullptr;
        }
    }
    armResetAndCleanup(obj);

    return obj;
}

void ObjectManager::initPrivsForObject(const std::shared_ptr<LpcObject>& obj, const std::string& filename) {
    if (!vm_ || !master_ || !obj) return;
    Value result;
    try {
        result = vm_->applyMaster("privs_file", {Value(filename)});
    } catch (const std::exception&) {
        // Real driver: any non-string result (including a thrown/failed
        // apply) just leaves privs unset, not a hard failure.
        return;
    }
    if (auto* s = std::get_if<std::string>(&result.data)) {
        obj->setPrivs(*s);
    }
}

std::shared_ptr<LpcObject> ObjectManager::lookupLoadedObject(const std::string& rawFilename) const {
    auto it = loaded_.find(normalizeFilename(rawFilename));
    return it != loaded_.end() ? it->second : nullptr;
}

void ObjectManager::destructObject(const std::shared_ptr<LpcObject>& obj,
                                    const std::function<void(const std::shared_ptr<LpcObject>&)>& onDestructed) {
    if (!obj || obj->isDestructed()) return;

    // Shadow chain cleanup (Phase 0.6), confirmed directly against real
    // destruct_object()'s own two-branch shadow handling (simulate.c),
    // not guessed. Real semantics are asymmetric, not a plain unlink in
    // both cases:
    //
    // If obj is the base victim of a chain (something shadows it, and
    // it does not itself shadow anything -- real "ob->shadowed &&
    // !ob->shadowing"), destructing it cascades: the ENTIRE chain is
    // destructed too, walking from the outermost shadow down to obj
    // itself, each one severed from its neighbors before its own
    // recursive destructObject() call. This function then returns
    // without doing its own further processing, since the recursive
    // call for obj itself (the last iteration) already did it -- exact
    // mirror of the real source's own "return;" right after that loop.
    auto shadowedBy = obj->shadowedBy().lock();
    if (shadowedBy && !obj->shadowing().lock()) {
        auto top = shadowedBy;
        while (auto next = top->shadowedBy().lock()) top = next;
        while (top) {
            auto below = top->shadowing().lock();
            if (below) below->setShadowedBy(std::weak_ptr<LpcObject>());
            top->setShadowing(std::weak_ptr<LpcObject>());
            auto current = top;
            top = below;
            destructObject(current, onDestructed);
        }
        return;
    }
    // Otherwise (obj is itself a shadow, or has no shadow relationship
    // at all) destructing it just splices it out of the middle of
    // whatever chain it was part of -- a real doubly-linked-list
    // unlink, reconnecting its neighbors to each other directly, then
    // falls through to the normal destruction steps below for obj
    // itself. A no-shadow-relationship object naturally no-ops both
    // branches here (both fields are already null).
    if (auto shadowing = obj->shadowing().lock()) {
        shadowing->setShadowedBy(shadowedBy);
    }
    if (shadowedBy) {
        shadowedBy->setShadowing(obj->shadowing());
    }
    obj->setShadowing(std::weak_ptr<LpcObject>());
    obj->setShadowedBy(std::weak_ptr<LpcObject>());

    // Real destruct_object()'s own "if (ob->flags & O_EFUN_SOCKET)
    // close_referencing_sockets(ob);" (simulate.c) -- confirmed sitting
    // right near the top of the real function, before its own shadow/
    // snoop/environment handling, closing every efun socket obj owns.
    // See this method's own header comment for why this is a callback
    // rather than a direct call: ObjectManager cannot depend on `net`
    // (SocketRegistry's own module), which already depends on `object`.
    // Fires exactly once per object this call actually destructs --
    // reached here both for a direct call on obj and, via the
    // recursively-forwarded onDestructed above, for every shadowing
    // object the cascade branch destructs along with it.
    if (onDestructed) onDestructed(obj);

    // Real destruct_object() (simulate.c): "if (ob->flags & O_SNOOP) {
    // for (i...) if (all_users[i] && all_users[i]->snooped_by == ob)
    // all_users[i]->snooped_by = 0; ob->flags &= ~O_SNOOP; }" -- if the
    // object being destructed was itself acting as a snooper, whoever it
    // was watching is unlinked too, so a stale snoopedBy_ never survives
    // pointing at a destructed object. This is the snooper-side half only
    // (real code never touches ob->interactive->snooped_by here at all --
    // that is remove_interactive()'s job, called separately from this
    // same real function right after this block when ob->interactive is
    // set; this driver's own equivalent is Connection::close(), reached
    // via the "destruct" efun's own follow-up close() call, see its
    // comment). Runs regardless of whether obj currently has a live
    // connection, matching real code exactly -- the O_SNOOP flag (and
    // this driver's own snooping_ field) is a plain object property, not
    // tied to interactive_t.
    if (auto victim = obj->snooping().lock()) {
        victim->setSnoopedBy(std::weak_ptr<LpcObject>());
    }
    obj->setSnooping(std::weak_ptr<LpcObject>());

    obj->setDestructed(true);

    // real destruct_object() -> destruct2()'s own "if (ob->replaced_program)
    // { FREE_MSTR(ob->replaced_program); ob->replaced_program = 0; }"
    // (object.c) -- backs query_replaced_program() (EfunTable.cpp).
    obj->setReplacedProgramName(std::nullopt);

    // real destruct_object() (simulate.c): "remove_living_name(ob);",
    // right alongside its own environment-unlink step just below --
    // without this, a destructed object with a still-live shared_ptr
    // reference elsewhere (the exact case this driver's own O_DESTRUCTED
    // guard was written for) would keep matching find_player()/
    // find_living() lookups after being destructed, contradicting
    // LivingNameRegistry::find()'s own isDestructed() check, which is a
    // second, redundant layer of defense for a stale weak_ptr entry that
    // is never cleaned up otherwise, not a substitute for this.
    LivingNameRegistry::remove(obj);

    // Real obj_list unthreading (simulate.c's own destruct_object()):
    // this driver's own LiveObjectRegistry equivalent -- objects()/
    // livings() must stop returning this object the instant it is
    // destructed, not merely once its last shared_ptr reference happens
    // to drop. LiveObjectRegistry::all() also independently filters on
    // isDestructed() (the same redundant-defense-for-a-stale-entry
    // reasoning as LivingNameRegistry::find() above), so this call is
    // an active cleanup, not the only thing standing between a
    // destructed object and a false-positive match.
    LiveObjectRegistry::remove(obj);

    // real destruct_object() (simulate.c): "ob->super = 0; ob->next_inv
    // = 0; ob->contains = 0;" -- unlinks the object from its environment
    // and severs its own inventory pointer, done immediately, not lazily.
    // This is the environment-unlink half of that (closes the actual
    // confirmed bug: a destructed object stayed visible in
    // all_inventory()/environment() results indefinitely, since nothing
    // ever removed it from its old environment's own inventory vector).
    // Not replicated here: real destruct_object()'s own contents-
    // relocation loop, which calls each contained item's own real
    // move()/APPLY_MOVE apply to relocate it out to the destructed
    // object's environment before severing -- a materially bigger
    // feature (an LPC-level apply cascade during what is otherwise a
    // pure bookkeeping operation) nothing confirmed live needs yet; a
    // destructed object's own remaining inventory is simply left
    // attached to it rather than relocated or dropped, flagged here
    // rather than silently assumed equivalent.
    if (auto env = obj->environment().lock()) {
        auto& inv = env->inventory();
        inv.erase(std::remove(inv.begin(), inv.end(), obj), inv.end());
    }
    obj->setEnvironment(std::weak_ptr<LpcObject>());

    loaded_.erase(obj->filename());
    restoredObjects_.erase(std::remove(restoredObjects_.begin(), restoredObjects_.end(), obj),
                            restoredObjects_.end());
}

void ObjectManager::retainRestoredObjects(std::vector<std::shared_ptr<LpcObject>> objs) {
    restoredObjects_.insert(restoredObjects_.end(),
                             std::make_move_iterator(objs.begin()),
                             std::make_move_iterator(objs.end()));
}

ObjectManager::~ObjectManager() {
    // Drop this manager's own retaining containers. On its own this is
    // not enough: a self-referential object (one whose LPC variable or
    // inventory holds a shared_ptr back to itself or a sibling) stays
    // pinned at refcount 1 by its own slot even after loaded_ lets go.
    loaded_.clear();
    restoredObjects_.clear();
    programCache_.clear();
    programSource_.clear();
    compiling_.clear();
    virtualCompiling_.clear();
    master_.reset();
    simulEfunObject_.reset();

    // Once this is the last manager alive, break those cycles across the
    // whole process-wide table so every LpcObject (and its
    // CompiledProgram and bytecode) can actually reach refcount zero.
    // releaseAll() only zeroes variable/inventory slots, it frees
    // nothing directly, so ordering it after the clears above is safe.
    if (--g_liveManagerCount == 0) {
        LiveObjectRegistry::releaseAll();
    }
}

void ObjectManager::reloadObject(const std::shared_ptr<LpcObject>& obj,
                                  const std::function<void(const std::shared_ptr<LpcObject>&)>& onDestructed) {
    // Real "if (!obj->prog) return;" -- a swapped-out object with no
    // compiled program at all. This driver's LpcObject always has one
    // (no swap concept), so the only real equivalent guard is a plain
    // null check; deliberately no isDestructed() guard either, matching
    // real reload_object()'s own lack of one (see this method's own
    // header comment).
    if (!obj) return;

    // Real "for (i...) { free_svalue(&obj->variables[i]); obj->variables[i]
    // = const0u; }" -- every object variable back to a real int 0, not
    // this driver's own separate "declared but never assigned reads as
    // void" convention (LpcObject.cpp), which is about initial
    // declaration state, not what an explicit reload sets afterward.
    for (auto& v : obj->variables()) {
        v = Value(int64_t{0});
    }

    // Shadow chain: identical real semantics to destructObject()'s own
    // two-branch handling just above (same real source citation), with
    // one real difference -- obj itself is never destructed here, only
    // spliced out of whatever chain it was part of (or has every object
    // that was shadowing it destructed, if it was the base victim).
    // Real code's own cascade loop pre-severs each shadowing link's own
    // shadowed/shadowing fields *before* destructing it (so that
    // destructObject()'s own shadow-handling becomes a no-op when it
    // runs for that link), confirmed directly rather than assumed to
    // match destructObject()'s own cascade shape.
    auto shadowedBy = obj->shadowedBy().lock();
    if (shadowedBy && !obj->shadowing().lock()) {
        auto cur = shadowedBy;
        while (cur) {
            auto next = cur->shadowedBy().lock();
            cur->setShadowing(std::weak_ptr<LpcObject>());
            cur->setShadowedBy(std::weak_ptr<LpcObject>());
            destructObject(cur, onDestructed);
            cur = next;
        }
    } else {
        if (auto shadowing = obj->shadowing().lock()) {
            shadowing->setShadowedBy(shadowedBy);
        }
        if (shadowedBy) {
            shadowedBy->setShadowing(obj->shadowing());
        }
    }
    obj->setShadowing(std::weak_ptr<LpcObject>());
    obj->setShadowedBy(std::weak_ptr<LpcObject>());

    // real "remove_living_name(obj);" (object.c), alongside the shadow
    // handling just above in the real source too.
    LivingNameRegistry::remove(obj);

    // real call_create()'s own real body: "call___INIT(ob); if (ob->flags
    // & O_DESTRUCTED) { ...; return; } apply(APPLY_CREATE, ob, 0,
    // ORIGIN_DRIVER);" -- confirmed directly, not just create() alone,
    // so a top-level initialized variable declaration really does end up
    // back at its own initializer value, not merely 0, after a reload.
    // The real O_DESTRUCTED check only guards the trailing create() apply
    // itself, not call___INIT -- that runs unconditionally, matched here
    // by not gating runObjectVarInitializers on isDestructed() either,
    // only the create() call below it.
    if (vm_) {
        try {
            runObjectVarInitializers(obj, obj->program());
            if (!obj->isDestructed()) {
                vm_->callFunction(obj, "create", {});
            }
        } catch (const std::exception& e) {
            std::cerr << "[object] reload_object: create() failed for "
                       << obj->filename() << ": " << e.what() << "\n";
        }
    }
    // real reset_object(obj, H_CREATE_OB|H_CREATE_CLONE, 0) runs again as
    // part of the same real call_create() this reload just re-ran (object.c's
    // own reload_object() calls call_create() exactly the way a fresh
    // load/clone does) -- so this object's own reset/clean_up timers are
    // rearmed the same way, not left at whatever they were before the
    // reload. isClone() is deliberately left untouched: reload_object()
    // does not change O_CLONE in real code either, only re-runs
    // initializers/create().
    if (!obj->isDestructed()) armResetAndCleanup(obj);
}

void ObjectManager::armResetAndCleanup(const std::shared_ptr<LpcObject>& obj) {
    if (!obj || obj->isDestructed()) return;
    // Real TIME_TO_RESET default, 1800 seconds -- see this method's own
    // header comment for the exact real citation. Kept local to this one
    // call site and Scheduler::tickResetsAndCleanup()'s own reschedule
    // (not a shared header constant): this driver's own established
    // convention for real driver-hook-number/timing constants is a small
    // locally-scoped citation at each real use site rather than a central
    // header (see VM.cpp's own kHModifyCommand/kHMoveObject0/1 for the
    // same pattern with hook numbers).
    obj->armReset(std::chrono::seconds{1800});
    obj->setWillCleanUp(true);
}

} // namespace amlp
