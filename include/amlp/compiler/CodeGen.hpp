#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include "amlp/compiler/Ast.hpp"
#include "amlp/vm/Bytecode.hpp"

namespace amlp {

class CodeGen {
public:
    // inheritedObjectVarNames is the flattened, in-order list of object
    // variable names declared by this program's inherit chain (already
    // resolved by the caller -- ObjectManager -- since CodeGen only ever
    // sees one file's own AST and has no file-loading capability of its
    // own). Passing it in lets this file's own PushObjectVar/StoreObjectVar
    // slot numbers, and any bare references to an inherited variable by
    // name, line up with the same slots the inherited program's own
    // bytecode already uses -- see ObjectManager::compile() for how the
    // two are kept consistent.
    CompiledProgram generate(const Program& program,
                              const std::vector<std::string>& inheritedObjectVarNames = {});

private:
    // Distinguishes a bare identifier resolving to a per-call local
    // (which also covers parameters, since they already share the same
    // slot space as locals) versus a per-object variable. A local always
    // shadows an object variable of the same name, matching real LPC's
    // local-wins-over-global precedence: resolveVariable() checks
    // locals_ before objectVars_.
    enum class VarKind { Local, ObjectVar };
    struct ResolvedVar { VarKind kind; int slot; };
    ResolvedVar resolveVariable(const std::string& name) const;

    int internString(const std::string& s);
    int internFloat(double d);
    void emitExpr(const AstNode& expr);
    // Emits one OpCode::ExpandVarargs per spread-marked position in
    // isSpread, real generate_expr_list()'s own second pass (icode.c:
    // 264-274) -- see Bytecode.hpp's OpCode::ExpandVarargs comment for
    // the full citation. Called after every element the list describes
    // has already been emitted (pushed) in order; a no-op when isSpread
    // is empty (the common, no-spread case).
    void emitSpreadExpansions(const std::vector<bool>& isSpread);
    void emitCallExpr(const CallExpr& call);
    void emitCallOtherExpr(const CallOtherExpr& callOther);
    void emitSscanfExpr(const SscanfExpr& sscanf);
    void emitBinaryExpr(const BinaryExpr& bin);
    void emitLogicalExpr(const BinaryExpr& bin);
    void emitTernaryExpr(const TernaryExpr& tern);
    void emitCatchExpr(const CatchExpr& catchExpr);
    void emitTimeExpressionExpr(const TimeExpressionExpr& timeExpr);
    void emitAssignExpr(const AssignExpr& assign);
    void emitIndexAssignExpr(const IndexAssignExpr& assign);
    void emitIncDecExpr(const IncDecExpr& incDec);
    void emitStatement(const AstNode& stmt);
    void emitReturnStmt(const ReturnStmt& stmt);
    void emitVarDeclStmt(const VarDeclStmt& stmt);
    void emitAssignStmt(const AssignStmt& stmt);
    void emitIndexAssignStmt(const IndexAssignStmt& stmt);
    void emitIfStmt(const IfStmt& stmt);
    void emitWhileStmt(const WhileStmt& stmt);
    void emitDoWhileStmt(const DoWhileStmt& stmt);
    void emitForStmt(const ForStmt& stmt);
    void emitForeachStmt(const ForeachStmt& stmt);
    void emitSwitchStmt(const SwitchStmt& stmt);
    void emitBreakStmt();
    void emitContinueStmt();
    void emitBlock(const Block& block);

    int declareLocal(const std::string& name);

    size_t emitJumpPlaceholder(OpCode op);
    void patchJumpTo(size_t jumpInstrIndex, size_t target);
    void patchJumpToHere(size_t jumpInstrIndex);

    // One entry per loop currently being compiled (nested loops push
    // their own), holding the as-yet-unpatched Jump placeholders emitted
    // by any break/continue inside it. Each loop's own emitWhileStmt()/
    // emitForStmt() patches its entry's jumps once it knows where they
    // should land (loop exit for break; the condition re-check for a
    // while's continue, the update clause for a for's) and pops it.
    // isSwitch marks a switch statement's own frame: break targets the
    // innermost frame regardless of kind, but continue must skip past
    // any switch frames to reach the nearest *loop* -- "continue" inside
    // a switch nested in a loop still continues the loop, real LPC/C
    // switch has no continue semantics of its own. See emitContinueStmt.
    struct LoopContext {
        std::vector<size_t> breakJumps;
        std::vector<size_t> continueJumps;
        bool isSwitch = false;
    };
    std::vector<LoopContext> loopStack_;
    // Gives each foreach statement's hidden bookkeeping locals
    // ("$foreach_orig_<n>" etc, see emitForeachStmt) a unique name within
    // the function being compiled, so sibling and nested foreach loops
    // never collide. Reset per function alongside locals_/loopStack_.
    int foreachCounter_ = 0;
    // Same idea as foreachCounter_, for switch's own hidden subject local.
    int switchCounter_ = 0;
    // Same idea again, for emitIndexAssignExpr()'s hidden temp local
    // (holds the newly assigned value so it can be pushed back onto the
    // stack after OpCode::IndexAssign, which itself leaves nothing
    // behind -- see that function's own comment).
    int indexAssignCounter_ = 0;

    CompiledProgram* out_ = nullptr;
    std::unordered_map<std::string, int> locals_;
    int nextLocalSlot_ = 0;
    // Real LPC/C89 block scoping: a "{ ... }" introduces its own nested
    // scope for local declarations, so two sibling blocks (e.g. two
    // separate "{ int me; ... }" groups back to back in the same
    // function, neither nested in the other) may each declare a
    // same-named local without colliding -- confirmed live needed
    // compiling domains/Praxis/setter.c's own "Store PPE"/"Store ISP"
    // blocks, each with their own "int ... me;". emitBlock() pushes a
    // new (empty) entry before compiling a block's statements and, on
    // exit, erases from locals_ every name declared during that block
    // (recorded here by declareLocal()) before popping it -- restoring
    // exactly the set of names visible before the block was entered.
    // Slots themselves are never reused (nextLocalSlot_ only ever
    // grows), which is simpler and harmless: mirrors this driver's own
    // existing preference for simple/monotonic allocation elsewhere
    // (e.g. object variable slots), just uses a few more of a function's
    // numLocals than a slot-reusing implementation would.
    std::vector<std::vector<std::string>> localScopeStack_;
    // Per-object variable slots, populated once per generate() call from
    // Program::objectVars before any function body is compiled, and left
    // untouched afterward (unlike locals_, which is per-function).
    std::unordered_map<std::string, int> objectVars_;

    // An InlineLambdaExpr (see Ast.hpp) encountered while compiling some
    // enclosing function's body cannot have its own bytecode emitted
    // in-place: that would splice the lambda's Return into the middle of
    // the enclosing function's own instruction stream, since every
    // function in a CompiledProgram shares one flat code_ array
    // addressed only by entryPoint (see generate()'s own per-function
    // loop). Instead, emitExpr() records the pending body here and emits
    // an ordinary PushClosure referencing a synthesized name; generate()
    // drains this list right after finishing the enclosing function's
    // own Return, compiling each one exactly like a normal top-level
    // function (own locals_/entryPoint/FunctionEntry) so it lands at a
    // distinct, self-contained offset. The synthesized name is never a
    // legal LPC identifier (see nextLambdaId_'s use in CodeGen.cpp), so
    // it can never collide with a real function and is only ever reached
    // via the Closure's stored functionName at call time -- the exact
    // same findFunctionInChain() lookup an ordinary bare-name closure
    // already uses, no VM.cpp changes needed.
    struct PendingLambda {
        std::string name;
        const InlineLambdaExpr* expr = nullptr;
    };
    std::vector<PendingLambda> pendingLambdas_;
    int nextLambdaId_ = 0;
    void emitPendingLambdas();

    // Real "$(expr)" offsetting (see Ast.hpp's LambdaParamExpr::
    // isBoundValue comment and icode.c's own current_num_values): set by
    // emitPendingLambdas() right before compiling each pending lambda's
    // own bodyExprs, to that lambda's own InlineLambdaExpr::
    // boundValueExprs.size() -- the number of slots its own "$(expr)"
    // bindings already occupy (0..K-1), so every ordinary "$N" reference
    // encountered while compiling that same body can be shifted past
    // them (K..K+paramCount-1). Read only inside the LambdaParamExpr
    // emitExpr() case; a nested "(: :)" lambda's own body is never
    // compiled here (it only gets queued into pendingLambdas_, see
    // InlineLambdaExpr's own emitExpr() case), so this is never stale by
    // the time it is actually read.
    size_t currentLambdaBoundValueCount_ = 0;

    // Same deferred-compilation shape as PendingLambda just above, for
    // real "function(<params>) { <body> }" anonymous-function literals
    // (Ast.hpp's AnonFunctionExpr) instead -- a genuinely different
    // compilation path (real named parameters via declareLocal(), a
    // real Block body via emitBlock(), not InlineLambdaExpr's own
    // paramCount/bodyExprs shape), so kept as its own parallel list
    // rather than folded into PendingLambda itself. A separate queue,
    // not a variant sharing one, since the two bodies are compiled
    // through genuinely different logic (declareLocal()+emitBlock() vs.
    // the $N-slot-reservation+comma-expression loop) -- see
    // emitPendingAnonFuncs()'s own comment. generate()'s own top-level
    // loop drains both this and pendingLambdas_ in alternation until
    // both are empty, since a body compiled while draining either one
    // can itself queue more of either kind (a "(: :)" nested inside a
    // "function(){}" body, or vice versa) -- no real corpus evidence
    // either nesting actually occurs in any vendored mudlib, but this
    // costs nothing extra to get right regardless, the same "handle a
    // lambda body queuing more lambdas" defensiveness
    // emitPendingLambdas() already has for its own single kind.
    struct PendingAnonFunc {
        std::string name;
        const AnonFunctionExpr* expr = nullptr;
    };
    std::vector<PendingAnonFunc> pendingAnonFuncs_;
    int nextAnonFuncId_ = 0;
    void emitPendingAnonFuncs();
};

} // namespace amlp
