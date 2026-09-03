#include "amlp/compiler/CodeGen.hpp"
#include "amlp/core/Errors.hpp"

namespace amlp {

int CodeGen::internString(const std::string& s) {
    for (size_t i = 0; i < out_->stringPool.size(); ++i) {
        if (out_->stringPool[i] == s) return static_cast<int>(i);
    }
    out_->stringPool.push_back(s);
    return static_cast<int>(out_->stringPool.size() - 1);
}

int CodeGen::internFloat(double d) {
    for (size_t i = 0; i < out_->floatPool.size(); ++i) {
        if (out_->floatPool[i] == d) return static_cast<int>(i);
    }
    out_->floatPool.push_back(d);
    return static_cast<int>(out_->floatPool.size() - 1);
}

CodeGen::ResolvedVar CodeGen::resolveVariable(const std::string& name) const {
    auto localIt = locals_.find(name);
    if (localIt != locals_.end()) {
        return ResolvedVar{VarKind::Local, localIt->second};
    }
    auto objVarIt = objectVars_.find(name);
    if (objVarIt != objectVars_.end()) {
        return ResolvedVar{VarKind::ObjectVar, objVarIt->second};
    }
    throw LpcRuntimeError("codegen: undeclared variable \"" + name + "\"");
}

int CodeGen::declareLocal(const std::string& name) {
    // An empty name is real LPC's own unnamed function parameter (e.g.
    // "string crash(string, object, object)" -- confirmed against
    // grammar.y's own "new_arg: arg_type optional_star { ...
    // add_local_name(\"\", $1 | $2); ... }", the case where a parameter
    // has a type but no identifier at all: real FluffOS still allocates
    // it a real slot in the local-variable table, it is just never
    // reachable by name afterwards). Reserves the slot (so the calling
    // convention's positional args[i] -> locals[i] copy still lines up
    // for every later named parameter) without registering it in
    // locals_ at all: unlike real FluffOS's own local-variable table (a
    // plain list, so several entries can share the empty name ""
    // simultaneously), this driver's locals_ is a name-keyed map, and
    // Parser.cpp's own "function parameter type" loop can and does parse
    // more than one unnamed parameter in the same function (that same
    // "string, object, object" needs three, not one) -- the ordinary
    // "already declared in this scope" collision check below would
    // wrongly reject the second one on nothing but a shared, meaningless
    // empty key.
    if (name.empty()) {
        return nextLocalSlot_++;
    }
    if (locals_.count(name)) {
        throw LpcRuntimeError("codegen: variable \"" + name + "\" already declared in this scope");
    }
    int slot = nextLocalSlot_++;
    locals_[name] = slot;
    // Record this name against the innermost open block scope (if any --
    // function parameters and locals declared directly in a function's
    // own top-level body, before emitBlock() has pushed anything, simply
    // have nothing to record against and live for the whole function, as
    // they always have), so emitBlock() can remove it again when that
    // block closes. See CodeGen.hpp's localScopeStack_ comment.
    if (!localScopeStack_.empty()) {
        localScopeStack_.back().push_back(name);
    }
    return slot;
}

size_t CodeGen::emitJumpPlaceholder(OpCode op) {
    size_t idx = out_->code.size();
    out_->code.push_back(Instruction{op, -1, 0});
    return idx;
}

void CodeGen::patchJumpTo(size_t jumpInstrIndex, size_t target) {
    out_->code[jumpInstrIndex].operand = static_cast<int32_t>(target);
}

void CodeGen::patchJumpToHere(size_t jumpInstrIndex) {
    patchJumpTo(jumpInstrIndex, out_->code.size());
}

void CodeGen::emitBinaryExpr(const BinaryExpr& bin) {
    emitExpr(*bin.left);
    emitExpr(*bin.right);

    OpCode op;
    switch (bin.op) {
        case BinOp::Eq:  op = OpCode::Eq;  break;
        case BinOp::Neq: op = OpCode::Neq; break;
        case BinOp::Lt:  op = OpCode::Lt;  break;
        case BinOp::Lte: op = OpCode::Lte; break;
        case BinOp::Gt:  op = OpCode::Gt;  break;
        case BinOp::Gte: op = OpCode::Gte; break;
        case BinOp::Add: op = OpCode::Add; break;
        case BinOp::Sub: op = OpCode::Sub; break;
        case BinOp::Mul: op = OpCode::Mul; break;
        case BinOp::Div: op = OpCode::Div; break;
        case BinOp::Mod: op = OpCode::Mod; break;
        case BinOp::BitAnd: op = OpCode::BitAnd; break;
        case BinOp::BitOr:  op = OpCode::BitOr;  break;
        case BinOp::BitXor: op = OpCode::BitXor; break;
        default: throw LpcRuntimeError("codegen: unknown BinOp");
    }
    out_->code.push_back(Instruction{op, 0, 0});
}

// Or/And cannot be a plain "evaluate both operands, then combine" opcode:
// that would evaluate the right operand unconditionally, defeating
// short-circuit evaluation and its correctness guarantee (see the plan
// document, e.g. "lines[i][0]" must never run if "lines[i]" is already
// known empty/null). Instead this builds the same shape emitIfStmt and
// emitWhileStmt already use: evaluate the left operand, Dup it so a copy
// survives the JumpIfFalse test, and only evaluate the right operand on
// the branch where the left operand did not already decide the result.
void CodeGen::emitLogicalExpr(const BinaryExpr& bin) {
    emitExpr(*bin.left);
    out_->code.push_back(Instruction{OpCode::Dup, 0, 0});
    size_t jumpIfFalseIdx = emitJumpPlaceholder(OpCode::JumpIfFalse);

    if (bin.op == BinOp::Or) {
        // Left was truthy: keep its value, skip the right operand entirely.
        size_t jumpToEndIdx = emitJumpPlaceholder(OpCode::Jump);
        patchJumpToHere(jumpIfFalseIdx);
        // Left was falsy: discard it, the result is whatever the right
        // operand evaluates to.
        out_->code.push_back(Instruction{OpCode::Pop, 0, 0});
        emitExpr(*bin.right);
        patchJumpToHere(jumpToEndIdx);
    } else {
        // BinOp::And. Left was falsy: JumpIfFalse's own taken branch is
        // already the short-circuit path, its remaining Dup'd copy is the
        // result, no separate jump-to-end is needed.
        // Left was truthy: discard it, the result is whatever the right
        // operand evaluates to.
        out_->code.push_back(Instruction{OpCode::Pop, 0, 0});
        emitExpr(*bin.right);
        patchJumpToHere(jumpIfFalseIdx);
    }
}

// A ternary is structurally an expression-position if/else: evaluate the
// condition, branch, and (unlike if/else as a statement) always leave
// exactly one value on the stack as the expression's result. Reuses the
// same emitJumpPlaceholder()/patchJumpToHere() shape emitIfStmt() already
// uses; unlike emitLogicalExpr(), no Dup/Pop is needed since a ternary
// always evaluates exactly one of its two branches and always produces
// that branch's freshly-computed value, never a conditionally-kept
// earlier value.
void CodeGen::emitTernaryExpr(const TernaryExpr& tern) {
    emitExpr(*tern.condition);
    size_t jumpIfFalseIdx = emitJumpPlaceholder(OpCode::JumpIfFalse);

    emitExpr(*tern.thenBranch);
    size_t jumpToEndIdx = emitJumpPlaceholder(OpCode::Jump);

    patchJumpToHere(jumpIfFalseIdx);
    emitExpr(*tern.elseBranch);

    patchJumpToHere(jumpToEndIdx);
}

// catch(expr) (see Ast.hpp's CatchExpr comment and Bytecode.hpp's
// PushCatchFrame/PopCatchFrame comments for the full runtime picture).
// Mirrors real FluffOS's icode.c NODE_CATCH codegen exactly: F_CATCH
// (here PushCatchFrame) with a forward-patched "resume after the whole
// catch expression" offset, then the guarded bytecode, then
// F_END_CATCH (here PopCatchFrame). The explicit Pop between the
// guarded expression and PopCatchFrame matches real FluffOS's own
// insert_pop_value() on the catch argument (trees.c) -- the guarded
// expression's own result (e.g. the value clone_object() returns, or
// what an assignment inside it evaluates to) is always discarded;
// catch(expr) itself only ever evaluates to 0 or the error string, one
// value either way, never expr's own result.
void CodeGen::emitCatchExpr(const CatchExpr& catchExpr) {
    size_t catchFrameIdx = emitJumpPlaceholder(OpCode::PushCatchFrame);

    emitExpr(*catchExpr.guarded);
    out_->code.push_back(Instruction{OpCode::Pop, 0, 0});
    out_->code.push_back(Instruction{OpCode::PopCatchFrame, 0, 0});

    patchJumpToHere(catchFrameIdx);
}

// Assignment used as an expression must leave the assigned value on the
// stack (its own value, matching real LPC's "x = (y = 5)" reading 5 into
// x too), unlike emitAssignStmt()'s statement form which just stores and
// leaves nothing behind. Dup before the store so one copy survives it.
// Compound assignment ("+=" etc) additionally pushes the variable's
// current value first and combines it with the right-hand side using the
// same Add/Sub/Mul/Div/Mod opcodes plain binary expressions use, before
// the Dup/store -- see Ast.hpp's AssignExpr comment on why desugaring to
// a read-modify-write is safe here.
void CodeGen::emitAssignExpr(const AssignExpr& assign) {
    ResolvedVar var = resolveVariable(assign.name);
    OpCode pushOp = (var.kind == VarKind::Local) ? OpCode::PushLocal : OpCode::PushObjectVar;
    OpCode storeOp = (var.kind == VarKind::Local) ? OpCode::StoreLocal : OpCode::StoreObjectVar;

    if (assign.isCompound) {
        out_->code.push_back(Instruction{pushOp, var.slot, 0});
        emitExpr(*assign.value);
        OpCode combineOp;
        switch (assign.compoundOp) {
            case BinOp::Add: combineOp = OpCode::Add; break;
            case BinOp::Sub: combineOp = OpCode::Sub; break;
            case BinOp::Mul: combineOp = OpCode::Mul; break;
            case BinOp::Div: combineOp = OpCode::Div; break;
            case BinOp::Mod: combineOp = OpCode::Mod; break;
            // "|="/"&="/"^=" (Parser.cpp's own kCompoundOps, real corpus
            // evidence in Lexer.cpp's lexSymbol() comment) reuse the same
            // OpCode::BitOr/BitAnd/BitXor plain binary "|"/"&"/"^" already use.
            case BinOp::BitOr: combineOp = OpCode::BitOr; break;
            case BinOp::BitAnd: combineOp = OpCode::BitAnd; break;
            case BinOp::BitXor: combineOp = OpCode::BitXor; break;
            default: throw LpcRuntimeError("codegen: unsupported compound assignment operator");
        }
        out_->code.push_back(Instruction{combineOp, 0, 0});
    } else {
        emitExpr(*assign.value);
    }

    out_->code.push_back(Instruction{OpCode::Dup, 0, 0});
    out_->code.push_back(Instruction{storeOp, var.slot, 0});
}

// Prefix and postfix ++/-- both reduce to "read, add/subtract 1, store",
// differing only in which value (the pre- or post-mutation one) is left
// on the stack as the expression's result:
//   prefix  (++x): push old, +1, Dup(new), store  -> leaves new
//   postfix (x++): push old, Dup(old), +1, store   -> leaves old
void CodeGen::emitIncDecExpr(const IncDecExpr& incDec) {
    OpCode deltaOp = (incDec.op == IncDecOp::Inc) ? OpCode::Add : OpCode::Sub;

    if (incDec.indexTarget) {
        // Indexed target (see Ast.hpp's IncDecExpr comment). Same
        // approach as emitIndexAssignExpr(): OpCode::IndexAssign leaves
        // nothing on the stack, so the value this expression needs to
        // produce (the pre- or post-mutation one, per incDec.prefix) is
        // stashed in a hidden temp local rather than kept on the stack
        // across the target/index pushes needed for the write.
        //
        // mapColumn (LDMud map[key, n]++, real F_MAP_INDEX_LVALUE) rides
        // along the same way emitExpr()'s own IndexExpr case pushes it:
        // one extra value after the key, with bit 2 of Index/IndexAssign's
        // flags operand set so the VM reads it back. Re-evaluated twice
        // (once per Index/IndexAssign pair below) same as indexTarget/
        // indexKey already are -- see emitIndexAssignStmt()'s own comment
        // on why that is harmless for every real target/index/column this
        // mudlib's own call sites actually use.
        std::string oldName = "$idxassign#" + std::to_string(indexAssignCounter_++);
        int oldSlot = declareLocal(oldName);
        std::string newName = "$idxassign#" + std::to_string(indexAssignCounter_++);
        int newSlot = declareLocal(newName);
        int32_t flags = incDec.mapColumn ? 0x4 : 0;

        emitExpr(*incDec.indexTarget);
        emitExpr(*incDec.indexKey);
        if (incDec.mapColumn) emitExpr(*incDec.mapColumn);
        out_->code.push_back(Instruction{OpCode::Index, 0, flags});
        out_->code.push_back(Instruction{OpCode::StoreLocal, oldSlot, 0});

        out_->code.push_back(Instruction{OpCode::PushLocal, oldSlot, 0});
        out_->code.push_back(Instruction{OpCode::PushInt, 1, 0});
        out_->code.push_back(Instruction{deltaOp, 0, 0});
        out_->code.push_back(Instruction{OpCode::StoreLocal, newSlot, 0});

        emitExpr(*incDec.indexTarget);
        emitExpr(*incDec.indexKey);
        if (incDec.mapColumn) emitExpr(*incDec.mapColumn);
        out_->code.push_back(Instruction{OpCode::PushLocal, newSlot, 0});
        out_->code.push_back(Instruction{OpCode::IndexAssign, 0, flags});

        out_->code.push_back(
            Instruction{OpCode::PushLocal, incDec.prefix ? newSlot : oldSlot, 0});
        return;
    }

    ResolvedVar var = resolveVariable(incDec.name);
    OpCode pushOp = (var.kind == VarKind::Local) ? OpCode::PushLocal : OpCode::PushObjectVar;
    OpCode storeOp = (var.kind == VarKind::Local) ? OpCode::StoreLocal : OpCode::StoreObjectVar;

    out_->code.push_back(Instruction{pushOp, var.slot, 0});
    if (incDec.prefix) {
        out_->code.push_back(Instruction{OpCode::PushInt, 1, 0});
        out_->code.push_back(Instruction{deltaOp, 0, 0});
        out_->code.push_back(Instruction{OpCode::Dup, 0, 0});
        out_->code.push_back(Instruction{storeOp, var.slot, 0});
    } else {
        out_->code.push_back(Instruction{OpCode::Dup, 0, 0});
        out_->code.push_back(Instruction{OpCode::PushInt, 1, 0});
        out_->code.push_back(Instruction{deltaOp, 0, 0});
        out_->code.push_back(Instruction{storeOp, var.slot, 0});
    }
}

void CodeGen::emitExpr(const AstNode& expr) {
    if (auto* lit = dynamic_cast<const StringLiteral*>(&expr)) {
        int idx = internString(lit->value);
        out_->code.push_back(Instruction{OpCode::PushConst, idx, 0});
        return;
    }
    if (auto* intLit = dynamic_cast<const IntLiteral*>(&expr)) {
        out_->code.push_back(Instruction{OpCode::PushInt, static_cast<int32_t>(intLit->value), 0});
        return;
    }
    if (auto* floatLit = dynamic_cast<const FloatLiteral*>(&expr)) {
        int idx = internFloat(floatLit->value);
        out_->code.push_back(Instruction{OpCode::PushFloat, idx, 0});
        return;
    }
    if (dynamic_cast<const NilLiteral*>(&expr)) {
        // DGD's nil (ROADMAP.md row 1.2/1.3's greenlit slice). No
        // operand -- see Bytecode.hpp's own PushNil comment.
        out_->code.push_back(Instruction{OpCode::PushNil, 0, 0});
        return;
    }
    if (auto* sym = dynamic_cast<const SymbolLiteralExpr*>(&expr)) {
        // LDMud "'name" symbol literal (ROADMAP.md row 1.7/1.8) -- see
        // Bytecode.hpp's own PushSymbol comment.
        int idx = internString(sym->name);
        out_->code.push_back(Instruction{OpCode::PushSymbol, idx, 0});
        return;
    }
    if (auto* ref = dynamic_cast<const VarRefExpr*>(&expr)) {
        ResolvedVar var = resolveVariable(ref->name);
        OpCode op = (var.kind == VarKind::Local) ? OpCode::PushLocal : OpCode::PushObjectVar;
        out_->code.push_back(Instruction{op, var.slot, 0});
        return;
    }
    if (auto* bin = dynamic_cast<const BinaryExpr*>(&expr)) {
        if (bin->op == BinOp::Or || bin->op == BinOp::And) {
            emitLogicalExpr(*bin);
        } else {
            emitBinaryExpr(*bin);
        }
        return;
    }
    if (auto* un = dynamic_cast<const UnaryExpr*>(&expr)) {
        if (un->op == UnaryOp::Not) {
            emitExpr(*un->operand);
            out_->code.push_back(Instruction{OpCode::Not, 0, 0});
        } else if (un->op == UnaryOp::BitNot) {
            emitExpr(*un->operand);
            out_->code.push_back(Instruction{OpCode::BitNot, 0, 0});
        } else {
            // UnaryOp::Neg. Desugared into the existing Sub opcode
            // ("0 - operand") instead of a new dedicated opcode, reusing
            // Sub's own numeric type-checking for free.
            out_->code.push_back(Instruction{OpCode::PushInt, 0, 0});
            emitExpr(*un->operand);
            out_->code.push_back(Instruction{OpCode::Sub, 0, 0});
        }
        return;
    }
    if (auto* call = dynamic_cast<const CallExpr*>(&expr)) {
        emitCallExpr(*call);
        return;
    }
    if (auto* callOther = dynamic_cast<const CallOtherExpr*>(&expr)) {
        emitCallOtherExpr(*callOther);
        return;
    }
    if (auto* sscanf = dynamic_cast<const SscanfExpr*>(&expr)) {
        emitSscanfExpr(*sscanf);
        return;
    }
    if (auto* assign = dynamic_cast<const AssignExpr*>(&expr)) {
        emitAssignExpr(*assign);
        return;
    }
    if (auto* idxAssign = dynamic_cast<const IndexAssignExpr*>(&expr)) {
        emitIndexAssignExpr(*idxAssign);
        return;
    }
    if (auto* incDec = dynamic_cast<const IncDecExpr*>(&expr)) {
        emitIncDecExpr(*incDec);
        return;
    }
    if (auto* arrLit = dynamic_cast<const ArrayLiteralExpr*>(&expr)) {
        for (const auto& elem : arrLit->elements) {
            emitExpr(*elem);
        }
        out_->code.push_back(
            Instruction{OpCode::MakeArray, 0, static_cast<int32_t>(arrLit->elements.size())});
        return;
    }
    if (auto* mapLit = dynamic_cast<const MappingLiteralExpr*>(&expr)) {
        int width = 1;
        if (!mapLit->entries.empty()) {
            width = static_cast<int>(mapLit->entries[0].second.size());
            if (width < 1) width = 1;
        }
        for (const auto& entry : mapLit->entries) {
            emitExpr(*entry.first);
            for (const auto& val : entry.second) {
                emitExpr(*val);
            }
        }
        out_->code.push_back(
            Instruction{OpCode::MakeMapping, width, static_cast<int32_t>(mapLit->entries.size())});
        return;
    }
    if (auto* tern = dynamic_cast<const TernaryExpr*>(&expr)) {
        emitTernaryExpr(*tern);
        return;
    }
    if (auto* catchExpr = dynamic_cast<const CatchExpr*>(&expr)) {
        emitCatchExpr(*catchExpr);
        return;
    }
    if (auto* idx = dynamic_cast<const IndexExpr*>(&expr)) {
        emitExpr(*idx->target);
        emitExpr(*idx->index);
        // Index/RangeIndex never take a real argument count (their
        // operands are already fully described by the values pushed
        // above), so argCount is repurposed here as a small "from the
        // end" flags bitmask instead: bit 0 = the start/single index,
        // bit 1 = the range end -- see Ast.hpp's IndexExpr comment and
        // VM.cpp's own handling of these two opcodes for where the
        // flags are actually consumed. Bit 2 = LDMud mapping column
        // (map[key, n]): an extra int is pushed after the key.
        int32_t flags = (idx->indexFromEnd ? 0x1 : 0);
        if (idx->rangeEnd) {
            emitExpr(*idx->rangeEnd);
            flags |= (idx->rangeEndFromEnd ? 0x2 : 0);
            out_->code.push_back(Instruction{OpCode::RangeIndex, 0, flags});
        } else {
            if (idx->mapColumn) {
                emitExpr(*idx->mapColumn);
                flags |= 0x4;
            }
            out_->code.push_back(Instruction{OpCode::Index, 0, flags});
        }
        return;
    }
    if (auto* closure = dynamic_cast<const ClosureLiteralExpr*>(&expr)) {
        for (const auto& argNode : closure->boundArgs) {
            emitExpr(*argNode);
        }
        int nameIdx = internString(closure->functionName);
        // Same Call/CallEfun split this file's own forceEfun comment
        // already documents for ordinary calls, applied to closure
        // literals instead -- see Bytecode.hpp's own PushEfunClosure
        // comment.
        OpCode op = closure->forceEfun ? OpCode::PushEfunClosure : OpCode::PushClosure;
        out_->code.push_back(
            Instruction{op, nameIdx, static_cast<int32_t>(closure->boundArgs.size())});
        return;
    }
    if (auto* param = dynamic_cast<const LambdaParamExpr*>(&expr)) {
        // "$N" reads as a plain local: emitPendingLambdas() reserves
        // slots 0..paramCount-1 for the closure's own call-time
        // arguments up front (see its own comment), the same slots an
        // ordinary function's declared params would occupy -- so this is
        // exactly the same instruction shape emitVarExpr's own
        // VarKind::Local case uses, just without needing a name lookup.
        out_->code.push_back(Instruction{OpCode::PushLocal, param->index, 0});
        return;
    }
    if (auto* lambda = dynamic_cast<const InlineLambdaExpr*>(&expr)) {
        // See CodeGen.hpp's PendingLambda comment: the body is compiled
        // later, at the enclosing function's own boundary, not here.
        // "$" can never start (or appear in) a real LPC identifier, so
        // this name is guaranteed not to collide with anything user code
        // could reference directly -- the closure only ever reaches it
        // through the Closure value's own stored functionName.
        std::string name = "$lambda#" + std::to_string(nextLambdaId_++);
        pendingLambdas_.push_back(PendingLambda{name, lambda});
        int nameIdx = internString(name);
        out_->code.push_back(Instruction{OpCode::PushClosure, nameIdx, 0});
        return;
    }
    throw LpcRuntimeError("codegen: unsupported expression kind this slice");
}

// Every plain "foo(...)" call compiles to the same OpCode::Call and is
// resolved at run time (local function, then inherited functions, then
// the simul_efun object, then the efun table -- see Bytecode.hpp's
// comment on OpCode::Call and VM.cpp's findFunctionInChain()), rather
// than being decided here at compile time. CodeGen deliberately does not
// special-case known efun names: doing so would need the compiler
// library linked against the efun table, which would create a link
// cycle (efun already depends on object, which depends on compiler).
// Resolving purely at run time avoids that, and as a side effect lets a
// local function (or the simul_efun object) legitimately shadow an efun
// of the same name, matching real LPC's local-wins precedent already
// used for plain variables (see CodeGen.hpp's VarKind comment).
//
// "efun::foo(...)" (call.forceEfun) is the one deliberate exception:
// real LPC's own explicit escape hatch past all of that straight to the
// core efun, needed by code that defines a same-named simul_efun/local
// function but still needs to reach the real one (see CallExpr's own
// comment). That maps directly onto the existing OpCode::CallEfun, which
// already skips straight to the efun table -- currently also used for
// "->" / call_other()'s own translation, see emitCallOtherExpr.
//
// "::foo(...)" / "qualifier::foo(...)" (call.parentCall) is a second,
// separate exception: an explicit call to an *inherited* definition
// (see Ast.hpp's CallExpr::parentCall comment) -- OpCode::CallParent,
// plus its own trailing CallParentQualifierSlot data instruction, the
// same "opcode plus immediately-following data instruction" shape
// Sscanf already uses for its var-slot table.
void CodeGen::emitCallExpr(const CallExpr& call) {
    for (const auto& argNode : call.args) {
        emitExpr(*argNode);
    }
    int calleeIdx = internString(call.callee);

    if (call.parentCall) {
        int qualifierIdx = call.parentQualifier.empty()
                                ? -1
                                : internString(call.parentQualifier);
        out_->code.push_back(
            Instruction{OpCode::CallParent, calleeIdx, static_cast<int32_t>(call.args.size())});
        out_->code.push_back(Instruction{OpCode::CallParentQualifierSlot, qualifierIdx, 0});
        return;
    }

    OpCode op = call.forceEfun ? OpCode::CallEfun : OpCode::Call;
    out_->code.push_back(
        Instruction{op, calleeIdx, static_cast<int32_t>(call.args.size())});
}

void CodeGen::emitCallOtherExpr(const CallOtherExpr& callOther) {
    emitExpr(*callOther.target);

    // The function name is a full expression now (see Ast.hpp's
    // CallOtherExpr comment) -- for the common "target->name(...)" and
    // literal call_other(target, "name", ...) shapes this is a
    // StringLiteral, which emitExpr() compiles down to the exact same
    // PushConst this used to emit directly; a variable or other
    // expression here now works too, resolved at run time same as any
    // other efun argument.
    emitExpr(*callOther.function);

    for (const auto& argNode : callOther.args) {
        emitExpr(*argNode);
    }

    int calleeIdx = internString("call_other");
    int32_t argCount = static_cast<int32_t>(2 + callOther.args.size());
    out_->code.push_back(Instruction{OpCode::CallEfun, calleeIdx, argCount});
}

// sscanf(target, format, ...vars) -> push target, push format, then a
// Sscanf instruction followed immediately by one SscanfVarSlot data record
// per output variable, each already resolved to its local-or-object-var
// slot at compile time (the same resolveVariable() plain assignment uses).
// The var-slot table has to travel as inline data rather than as normal
// pushed values because Instruction has no "list of (kind, slot) pairs"
// operand shape and the number of vars is not fixed -- see VM.cpp's
// OpCode::Sscanf handler for how it reads this table back out.
//
// An indexed output argument (sscanf.indexedTargets[i] non-null, e.g.
// "arr[i]" -- see Ast.hpp's own SscanfExpr comment for the real TMI-2
// corpus evidence) cannot point at a SscanfVarSlot table entry directly:
// that table only ever names a fixed local-or-object-var slot, resolved
// once at compile time, but an indexed target's own container and index
// are themselves runtime values (e.g. the "i" in "arr[i]" is a loop
// variable, not known until the match actually runs). Rather than
// widening OpCode::Sscanf's own stack/table protocol to carry runtime
// container+index operands as well -- a much larger, riskier change to
// an already-working opcode -- this reuses the exact same "hidden temp
// local, then a separate IndexAssign afterward" idiom
// emitIndexAssignExpr() already established just above for an
// unrelated but structurally identical problem (a computed value that
// needs to end up written through an indexed lvalue): the match result
// is written into an ordinary compiler-synthesized local exactly like
// any other plain-variable output (OpCode::Sscanf itself needs no
// changes at all), then, once every output has been written, the real
// indexed targets are populated from those hidden locals via the same
// real IndexAssign opcode ordinary "arr[i] = x" already uses -- so an
// indexed sscanf output gets exactly real "arr[i] = <matched value>"
// semantics, just deferred by one hidden local's worth of indirection.
void CodeGen::emitSscanfExpr(const SscanfExpr& sscanf) {
    emitExpr(*sscanf.target);
    emitExpr(*sscanf.format);

    out_->code.push_back(
        Instruction{OpCode::Sscanf, static_cast<int32_t>(sscanf.varNames.size()), 0});

    // (slot, indexed-target expr) pairs to flush into their real indexed
    // lvalue once the Sscanf instruction + its inline var-slot table has
    // been fully emitted (that table cannot be interleaved with other
    // instructions).
    std::vector<std::pair<int, const AstNode*>> pendingIndexedWrites;

    for (size_t i = 0; i < sscanf.varNames.size(); ++i) {
        int slot;
        int32_t kindFlag;
        if (sscanf.indexedTargets[i]) {
            std::string tempName = "$sscanf_out#" + std::to_string(indexAssignCounter_++);
            slot = declareLocal(tempName);
            kindFlag = 0; // always a local -- see declareLocal() above
            pendingIndexedWrites.emplace_back(slot, sscanf.indexedTargets[i].get());
        } else {
            ResolvedVar var = resolveVariable(sscanf.varNames[i]);
            // argCount doubles as the kind flag here (0 = local, 1 =
            // object var) since this instruction is never dispatched
            // through the normal switch -- OpCode::Sscanf's handler
            // reads it directly.
            slot = var.slot;
            kindFlag = (var.kind == VarKind::ObjectVar) ? 1 : 0;
        }
        out_->code.push_back(Instruction{OpCode::SscanfVarSlot, slot, kindFlag});
    }

    for (const auto& [tempSlot, targetNode] : pendingIndexedWrites) {
        const auto& idx = static_cast<const IndexExpr&>(*targetNode);
        emitExpr(*idx.target);
        emitExpr(*idx.index);
        int32_t flags = idx.mapColumn ? 0x4 : 0;
        if (idx.mapColumn) emitExpr(*idx.mapColumn);
        out_->code.push_back(Instruction{OpCode::PushLocal, tempSlot, 0});
        out_->code.push_back(Instruction{OpCode::IndexAssign, 0, flags});
    }
}

void CodeGen::emitReturnStmt(const ReturnStmt& stmt) {
    if (stmt.expr) {
        emitExpr(*stmt.expr);
    }
    out_->code.push_back(Instruction{OpCode::Return, 0, 0});
}

void CodeGen::emitVarDeclStmt(const VarDeclStmt& stmt) {
    int slot = declareLocal(stmt.name);
    if (stmt.initializer) {
        emitExpr(*stmt.initializer);
        out_->code.push_back(Instruction{OpCode::StoreLocal, slot, 0});
    }
}

void CodeGen::emitAssignStmt(const AssignStmt& stmt) {
    ResolvedVar var = resolveVariable(stmt.name);
    emitExpr(*stmt.value);
    OpCode op = (var.kind == VarKind::Local) ? OpCode::StoreLocal : OpCode::StoreObjectVar;
    out_->code.push_back(Instruction{op, var.slot, 0});
}

// Compound form ("target[index] += value" etc, see Ast.hpp's
// IndexAssignStmt comment) desugars to a read-modify-write, the same
// idea emitAssignExpr() already uses for a bare variable -- except
// IndexAssign needs [target, index, newValue] on the stack in that
// exact order, and there is no "duplicate the top two stack entries as
// a pair" opcode to set that up from a single evaluation of target/
// index. Emitting target and index twice (once for the read via
// Index, once for the write via IndexAssign) sidesteps needing one:
// target(T) / index(I) / target(T) / index(I) / Index -> [T, I,
// currentValue]; then the rhs and combine op leave [T, I, newValue],
// exactly IndexAssign's expected layout.
void CodeGen::emitIndexAssignStmt(const IndexAssignStmt& stmt) {
    int32_t flags = stmt.mapColumn ? 0x4 : 0;
    if (stmt.isCompound) {
        emitExpr(*stmt.target);
        emitExpr(*stmt.index);
        if (stmt.mapColumn) emitExpr(*stmt.mapColumn);
        emitExpr(*stmt.target);
        emitExpr(*stmt.index);
        if (stmt.mapColumn) emitExpr(*stmt.mapColumn);
        out_->code.push_back(Instruction{OpCode::Index, 0, flags});
        emitExpr(*stmt.value);
        OpCode combineOp;
        switch (stmt.compoundOp) {
            case BinOp::Add: combineOp = OpCode::Add; break;
            case BinOp::Sub: combineOp = OpCode::Sub; break;
            case BinOp::Mul: combineOp = OpCode::Mul; break;
            case BinOp::Div: combineOp = OpCode::Div; break;
            case BinOp::Mod: combineOp = OpCode::Mod; break;
            // "|="/"&="/"^=" (Parser.cpp's own kCompoundOps, real corpus
            // evidence in Lexer.cpp's lexSymbol() comment) reuse the same
            // OpCode::BitOr/BitAnd/BitXor plain binary "|"/"&"/"^" already use.
            case BinOp::BitOr: combineOp = OpCode::BitOr; break;
            case BinOp::BitAnd: combineOp = OpCode::BitAnd; break;
            case BinOp::BitXor: combineOp = OpCode::BitXor; break;
            default: throw LpcRuntimeError("codegen: unsupported compound assignment operator");
        }
        out_->code.push_back(Instruction{combineOp, 0, 0});
        out_->code.push_back(Instruction{OpCode::IndexAssign, 0, flags});
        return;
    }

    emitExpr(*stmt.target);
    emitExpr(*stmt.index);
    if (stmt.mapColumn) emitExpr(*stmt.mapColumn);
    emitExpr(*stmt.value);
    out_->code.push_back(Instruction{OpCode::IndexAssign, 0, flags});
}

// The expression-producing counterpart above (see Ast.hpp's
// IndexAssignExpr comment for the real call site, std/user/more.c's own
// "if(!(__More[\"class\"] = cl)) ..."). OpCode::IndexAssign always
// consumes its three operands and leaves nothing on the stack -- correct
// for the statement form above, wrong here, where the assigned value
// needs to survive as this expression's own result. There is no
// "duplicate the value under two other stack entries" opcode to route
// around that in place, so this computes the new value once, stashes it
// in a hidden temp local (never reachable by name -- see
// indexAssignCounter_'s own comment), and pushes it back from there
// after the mutation instead of trying to keep it on the stack across
// the target/index pushes.
void CodeGen::emitIndexAssignExpr(const IndexAssignExpr& assign) {
    std::string tempName = "$idxassign#" + std::to_string(indexAssignCounter_++);
    int tempSlot = declareLocal(tempName);

    if (assign.isCompound) {
        emitExpr(*assign.target);
        emitExpr(*assign.index);
        int32_t flags = assign.mapColumn ? 0x4 : 0;
        if (assign.mapColumn) emitExpr(*assign.mapColumn);
        out_->code.push_back(Instruction{OpCode::Index, 0, flags});
        emitExpr(*assign.value);
        OpCode combineOp;
        switch (assign.compoundOp) {
            case BinOp::Add: combineOp = OpCode::Add; break;
            case BinOp::Sub: combineOp = OpCode::Sub; break;
            case BinOp::Mul: combineOp = OpCode::Mul; break;
            case BinOp::Div: combineOp = OpCode::Div; break;
            case BinOp::Mod: combineOp = OpCode::Mod; break;
            // "|="/"&="/"^=" (Parser.cpp's own kCompoundOps, real corpus
            // evidence in Lexer.cpp's lexSymbol() comment) reuse the same
            // OpCode::BitOr/BitAnd/BitXor plain binary "|"/"&"/"^" already use.
            case BinOp::BitOr: combineOp = OpCode::BitOr; break;
            case BinOp::BitAnd: combineOp = OpCode::BitAnd; break;
            case BinOp::BitXor: combineOp = OpCode::BitXor; break;
            default: throw LpcRuntimeError("codegen: unsupported compound assignment operator");
        }
        out_->code.push_back(Instruction{combineOp, 0, 0});
    } else {
        emitExpr(*assign.value);
    }
    out_->code.push_back(Instruction{OpCode::StoreLocal, tempSlot, 0});

    emitExpr(*assign.target);
    emitExpr(*assign.index);
    int32_t flags = assign.mapColumn ? 0x4 : 0;
    if (assign.mapColumn) emitExpr(*assign.mapColumn);
    out_->code.push_back(Instruction{OpCode::PushLocal, tempSlot, 0});
    out_->code.push_back(Instruction{OpCode::IndexAssign, 0, flags});

    out_->code.push_back(Instruction{OpCode::PushLocal, tempSlot, 0});
}

void CodeGen::emitIfStmt(const IfStmt& stmt) {
    emitExpr(*stmt.condition);
    size_t jumpIfFalseIdx = emitJumpPlaceholder(OpCode::JumpIfFalse);

    emitBlock(*stmt.thenBranch);

    if (stmt.elseBranch) {
        size_t jumpToEndIdx = emitJumpPlaceholder(OpCode::Jump);
        patchJumpToHere(jumpIfFalseIdx);
        emitBlock(*stmt.elseBranch);
        patchJumpToHere(jumpToEndIdx);
    } else {
        patchJumpToHere(jumpIfFalseIdx);
    }
}

void CodeGen::emitWhileStmt(const WhileStmt& stmt) {
    size_t loopTop = out_->code.size();
    emitExpr(*stmt.condition);
    size_t jumpIfFalseIdx = emitJumpPlaceholder(OpCode::JumpIfFalse);

    loopStack_.emplace_back();
    emitBlock(*stmt.body);
    LoopContext ctx = std::move(loopStack_.back());
    loopStack_.pop_back();

    // continue jumps straight back to the condition re-check -- a while
    // loop has no update clause to run first.
    for (size_t idx : ctx.continueJumps) patchJumpTo(idx, loopTop);

    out_->code.push_back(Instruction{OpCode::Jump, static_cast<int32_t>(loopTop), 0});
    patchJumpToHere(jumpIfFalseIdx);
    for (size_t idx : ctx.breakJumps) patchJumpToHere(idx);
}

// "do body while (condition);" -- a post-test loop, the mirror image of
// emitWhileStmt()'s pre-test shape: the body is emitted first and always
// runs once, then the condition is checked, and only a *true* result jumps
// back to the body's own start (there is no OpCode::JumpIfTrue in this VM,
// so this is built from the same JumpIfFalse-skips-the-jump-back idiom
// emitForStmt() already relies on for its own trailing condition check).
// continue jumps to the condition check, not back to the body's start
// directly -- real do-while semantics still re-evaluate the condition
// before deciding whether to loop again, exactly like emitForStmt()'s own
// continueTarget sits before the update clause rather than back at the
// loop's very top.
void CodeGen::emitDoWhileStmt(const DoWhileStmt& stmt) {
    size_t bodyStart = out_->code.size();

    loopStack_.emplace_back();
    emitBlock(*stmt.body);
    LoopContext ctx = std::move(loopStack_.back());
    loopStack_.pop_back();

    size_t continueTarget = out_->code.size();
    for (size_t idx : ctx.continueJumps) patchJumpTo(idx, continueTarget);

    emitExpr(*stmt.condition);
    size_t jumpIfFalseIdx = emitJumpPlaceholder(OpCode::JumpIfFalse);
    out_->code.push_back(Instruction{OpCode::Jump, static_cast<int32_t>(bodyStart), 0});
    patchJumpToHere(jumpIfFalseIdx);
    for (size_t idx : ctx.breakJumps) patchJumpToHere(idx);
}

// "for (init; condition; update) body" desugars to the same
// evaluate-condition/JumpIfFalse/body/Jump-back shape emitWhileStmt()
// already uses, with init run once before the loop and update run once
// per iteration after the body (before the condition is re-checked). init
// and update are plain expressions here (not statements), so their
// results -- unused -- are explicitly popped, matching how emitStatement's
// ExprStmt case pops a plain expression statement's value. An absent
// condition (real LPC's "for (;;)") always takes the loop, matching C.
void CodeGen::emitForStmt(const ForStmt& stmt) {
    if (stmt.init) {
        if (auto* varDecl = dynamic_cast<const VarDeclStmt*>(stmt.init.get())) {
            emitVarDeclStmt(*varDecl);
        } else if (auto* block = dynamic_cast<const Block*>(stmt.init.get())) {
            // A comma_expr chain ("i = 0, s = sizeof(x)"), parsed into a
            // Block of ExprStmts (see Parser::parseCommaExprChain) --
            // each one already emits its own Pop via emitStatement.
            emitBlock(*block);
        } else {
            emitExpr(*stmt.init);
            out_->code.push_back(Instruction{OpCode::Pop, 0, 0});
        }
    }

    size_t loopTop = out_->code.size();
    size_t jumpIfFalseIdx = 0;
    bool hasCondition = static_cast<bool>(stmt.condition);
    if (hasCondition) {
        emitExpr(*stmt.condition);
        jumpIfFalseIdx = emitJumpPlaceholder(OpCode::JumpIfFalse);
    }

    loopStack_.emplace_back();
    emitBlock(*stmt.body);
    LoopContext ctx = std::move(loopStack_.back());
    loopStack_.pop_back();

    // continue jumps here, right before the update clause: real C/LPC
    // "continue" inside a for-loop still runs the update step, unlike a
    // while loop's, which has none to run.
    size_t continueTarget = out_->code.size();
    for (size_t idx : ctx.continueJumps) patchJumpTo(idx, continueTarget);

    if (stmt.update) {
        if (auto* block = dynamic_cast<const Block*>(stmt.update.get())) {
            emitBlock(*block);
        } else {
            emitExpr(*stmt.update);
            out_->code.push_back(Instruction{OpCode::Pop, 0, 0});
        }
    }

    out_->code.push_back(Instruction{OpCode::Jump, static_cast<int32_t>(loopTop), 0});
    if (hasCondition) {
        patchJumpToHere(jumpIfFalseIdx);
    }
    for (size_t idx : ctx.breakJumps) patchJumpToHere(idx);
}

// Desugars entirely into the existing opcode set plus one small runtime
// helper (OpCode::ForeachKeys, see Bytecode.hpp) rather than a dedicated
// iterator concept: three hidden locals hold the original collection, a
// normalized "array to walk" (itself for an array, keys(mapping) for a
// mapping), and a running index, then the loop body indexes into that
// normalized array each iteration -- structurally identical to a
// desugared "for (i = 0; i < sizeof(iter); i++)", reusing loopStack_ for
// break/continue exactly like emitForStmt() does.
void CodeGen::emitForeachStmt(const ForeachStmt& stmt) {
    int id = foreachCounter_++;
    std::string origName = "$foreach_orig_" + std::to_string(id);
    std::string iterName = "$foreach_iter_" + std::to_string(id);
    std::string idxName = "$foreach_idx_" + std::to_string(id);

    int origSlot = declareLocal(origName);
    emitExpr(*stmt.collection);
    out_->code.push_back(Instruction{OpCode::StoreLocal, origSlot, 0});

    int iterSlot = declareLocal(iterName);
    out_->code.push_back(Instruction{OpCode::PushLocal, origSlot, 0});
    out_->code.push_back(Instruction{OpCode::ForeachKeys, 0, 0});
    out_->code.push_back(Instruction{OpCode::StoreLocal, iterSlot, 0});

    int idxSlot = declareLocal(idxName);
    out_->code.push_back(Instruction{OpCode::PushInt, 0, 0});
    out_->code.push_back(Instruction{OpCode::StoreLocal, idxSlot, 0});

    size_t loopTop = out_->code.size();
    out_->code.push_back(Instruction{OpCode::PushLocal, idxSlot, 0});
    out_->code.push_back(Instruction{OpCode::PushLocal, iterSlot, 0});
    out_->code.push_back(Instruction{OpCode::Call, internString("sizeof"), 1});
    out_->code.push_back(Instruction{OpCode::Lt, 0, 0});
    size_t jumpIfFalseIdx = emitJumpPlaceholder(OpCode::JumpIfFalse);

    ResolvedVar keyVar = stmt.declareVar
        ? ResolvedVar{VarKind::Local, declareLocal(stmt.varName)}
        : resolveVariable(stmt.varName);
    OpCode keyStoreOp = (keyVar.kind == VarKind::Local) ? OpCode::StoreLocal : OpCode::StoreObjectVar;
    out_->code.push_back(Instruction{OpCode::PushLocal, iterSlot, 0});
    out_->code.push_back(Instruction{OpCode::PushLocal, idxSlot, 0});
    out_->code.push_back(Instruction{OpCode::Index, 0, 0});
    out_->code.push_back(Instruction{keyStoreOp, keyVar.slot, 0});

    if (stmt.hasValueVar) {
        // Correct for the common real-mudlib case, a mapping: iter
        // holds keys, so orig[key] is the matching value. For a plain
        // array this would instead try to use the element as an index
        // into itself, which is not meaningful -- two-variable foreach
        // over a bare array is not supported (every real use of the
        // two-variable form in this mudlib is over a mapping).
        ResolvedVar valVar = stmt.declareValueVar
            ? ResolvedVar{VarKind::Local, declareLocal(stmt.valueVarName)}
            : resolveVariable(stmt.valueVarName);
        OpCode valStoreOp = (valVar.kind == VarKind::Local) ? OpCode::StoreLocal : OpCode::StoreObjectVar;
        OpCode keyPushOp = (keyVar.kind == VarKind::Local) ? OpCode::PushLocal : OpCode::PushObjectVar;
        out_->code.push_back(Instruction{OpCode::PushLocal, origSlot, 0});
        out_->code.push_back(Instruction{keyPushOp, keyVar.slot, 0});
        out_->code.push_back(Instruction{OpCode::Index, 0, 0});
        out_->code.push_back(Instruction{valStoreOp, valVar.slot, 0});
    }

    loopStack_.emplace_back();
    emitBlock(*stmt.body);
    LoopContext ctx = std::move(loopStack_.back());
    loopStack_.pop_back();

    size_t continueTarget = out_->code.size();
    for (size_t idx : ctx.continueJumps) patchJumpTo(idx, continueTarget);

    out_->code.push_back(Instruction{OpCode::PushLocal, idxSlot, 0});
    out_->code.push_back(Instruction{OpCode::PushInt, 1, 0});
    out_->code.push_back(Instruction{OpCode::Add, 0, 0});
    out_->code.push_back(Instruction{OpCode::StoreLocal, idxSlot, 0});

    out_->code.push_back(Instruction{OpCode::Jump, static_cast<int32_t>(loopTop), 0});
    patchJumpToHere(jumpIfFalseIdx);
    for (size_t idx : ctx.breakJumps) patchJumpToHere(idx);
}

// A dispatch-then-fallthrough shape, evaluated in two passes over
// stmt.body since the comparison/jump dispatch table has to precede every
// case body in the final bytecode, but each jump's target (a case body's
// start position) is only known once that body is actually being
// emitted:
//   pass 1: for each CaseLabel, emit "push subject, push case value, Eq,
//           Not, JumpIfFalse <placeholder>" (jumps to the case body when
//           the comparison matched -- there is no JumpIfTrue opcode, so
//           inverting with Not and reusing JumpIfFalse gets the same
//           effect), remembering which body index each placeholder
//           targets. A final unconditional jump placeholder covers "no
//           case matched" (target: default's body, or the switch's end).
//   pass 2: walk stmt.body again in source order, patching any jumps
//           aimed at each index just before emitting that index's real
//           statement (a CaseLabel itself contributes no code -- it was
//           only a marker) -- since nothing emits a jump between cases,
//           falling out of one case's body runs straight into the next,
//           i.e. real C/LPC fallthrough, for free.
void CodeGen::emitSwitchStmt(const SwitchStmt& stmt) {
    std::string subjName = "$switch_subj_" + std::to_string(switchCounter_++);
    int subjSlot = declareLocal(subjName);
    emitExpr(*stmt.subject);
    out_->code.push_back(Instruction{OpCode::StoreLocal, subjSlot, 0});

    std::unordered_map<size_t, std::vector<size_t>> jumpsToIndex;
    size_t defaultBodyIndex = stmt.body.size(); // sentinel: no default seen

    for (size_t i = 0; i < stmt.body.size(); ++i) {
        auto* label = dynamic_cast<const CaseLabel*>(stmt.body[i].get());
        if (!label) continue;
        if (!label->value) {
            defaultBodyIndex = i;
            continue;
        }
        if (label->rangeEnd) {
            // "case low..high:" -- matches when low <= subject <= high.
            // Built as the same short-circuit "(subject >= low) &&
            // (subject <= high)" shape emitLogicalExpr()'s own AND branch
            // uses (Dup the left result so the false branch keeps it as
            // the whole expression's result without re-evaluating
            // anything), just written directly against subjSlot instead
            // of a synthesized BinaryExpr AST -- there is no source-level
            // "subject" expression to build one around, only its already-
            // materialized local slot. The final boolean this leaves on
            // the stack then feeds the exact same "Not; JumpIfFalse"
            // jump-to-body idiom the plain-value case just below uses.
            out_->code.push_back(Instruction{OpCode::PushLocal, subjSlot, 0});
            emitExpr(*label->value);
            out_->code.push_back(Instruction{OpCode::Gte, 0, 0});
            out_->code.push_back(Instruction{OpCode::Dup, 0, 0});
            size_t lowFailedJumpIdx = emitJumpPlaceholder(OpCode::JumpIfFalse);
            out_->code.push_back(Instruction{OpCode::Pop, 0, 0});
            out_->code.push_back(Instruction{OpCode::PushLocal, subjSlot, 0});
            emitExpr(*label->rangeEnd);
            out_->code.push_back(Instruction{OpCode::Lte, 0, 0});
            patchJumpToHere(lowFailedJumpIdx);
            out_->code.push_back(Instruction{OpCode::Not, 0, 0});
            size_t jumpIdx = emitJumpPlaceholder(OpCode::JumpIfFalse);
            jumpsToIndex[i].push_back(jumpIdx);
            continue;
        }
        out_->code.push_back(Instruction{OpCode::PushLocal, subjSlot, 0});
        emitExpr(*label->value);
        out_->code.push_back(Instruction{OpCode::Eq, 0, 0});
        out_->code.push_back(Instruction{OpCode::Not, 0, 0});
        size_t jumpIdx = emitJumpPlaceholder(OpCode::JumpIfFalse);
        jumpsToIndex[i].push_back(jumpIdx);
    }

    size_t noMatchJumpIdx = emitJumpPlaceholder(OpCode::Jump);

    loopStack_.push_back(LoopContext{{}, {}, /*isSwitch=*/true});

    for (size_t i = 0; i < stmt.body.size(); ++i) {
        auto found = jumpsToIndex.find(i);
        if (found != jumpsToIndex.end()) {
            for (size_t jumpIdx : found->second) patchJumpToHere(jumpIdx);
        }
        if (i == defaultBodyIndex) {
            patchJumpToHere(noMatchJumpIdx);
            continue; // the label itself has no code
        }
        if (dynamic_cast<const CaseLabel*>(stmt.body[i].get())) {
            continue; // a non-default label: already patched above
        }
        emitStatement(*stmt.body[i]);
    }

    if (defaultBodyIndex == stmt.body.size()) {
        // No default: "no match" skips the whole switch.
        patchJumpToHere(noMatchJumpIdx);
    }

    LoopContext ctx = std::move(loopStack_.back());
    loopStack_.pop_back();
    for (size_t idx : ctx.breakJumps) patchJumpToHere(idx);
}

void CodeGen::emitBreakStmt() {
    if (loopStack_.empty()) {
        throw LpcRuntimeError("codegen: break statement outside of a loop");
    }
    size_t idx = emitJumpPlaceholder(OpCode::Jump);
    loopStack_.back().breakJumps.push_back(idx);
}

void CodeGen::emitContinueStmt() {
    // Skip past any switch frames (isSwitch) to find the nearest
    // enclosing *loop* -- see LoopContext's comment.
    for (auto it = loopStack_.rbegin(); it != loopStack_.rend(); ++it) {
        if (it->isSwitch) continue;
        size_t idx = emitJumpPlaceholder(OpCode::Jump);
        it->continueJumps.push_back(idx);
        return;
    }
    throw LpcRuntimeError("codegen: continue statement outside of a loop");
}

void CodeGen::emitStatement(const AstNode& stmt) {
    // A real nested "{ ... }" used as a standalone statement gets its own
    // scope (emitBlock() below). Three other Parser sites build a Block
    // purely as an AST convenience, not a real scope, and mark it
    // isRealScope = false: a comma-separated local var decl ("string a,
    // b;", Parser::parseVarDeclStatement), a for-loop's comma-chained
    // init/update clause (parseCommaExprChain), and a braceless single-
    // statement if/while/for branch (parseBranch). Those must flatten
    // directly into the *enclosing* scope, not open a new one -- emitBlock()
    // would otherwise erase a comma-decl's own names from locals_ right
    // after that one statement, before anything later in the function
    // could reference them (confirmed live: "string a, b, c; a = ...;
    // return a + b + c;" threw "undeclared variable a" once emitBlock()
    // started scoping unconditionally).
    if (auto* nestedBlock = dynamic_cast<const Block*>(&stmt)) {
        if (nestedBlock->isRealScope) {
            emitBlock(*nestedBlock);
        } else {
            for (const auto& inner : nestedBlock->statements) {
                emitStatement(*inner);
            }
        }
        return;
    }
    if (auto* exprStmt = dynamic_cast<const ExprStmt*>(&stmt)) {
        emitExpr(*exprStmt->expr);
        out_->code.push_back(Instruction{OpCode::Pop, 0, 0});
        return;
    }
    if (auto* returnStmt = dynamic_cast<const ReturnStmt*>(&stmt)) {
        emitReturnStmt(*returnStmt);
        return;
    }
    if (auto* varDecl = dynamic_cast<const VarDeclStmt*>(&stmt)) {
        emitVarDeclStmt(*varDecl);
        return;
    }
    if (auto* assign = dynamic_cast<const AssignStmt*>(&stmt)) {
        emitAssignStmt(*assign);
        return;
    }
    if (auto* indexAssign = dynamic_cast<const IndexAssignStmt*>(&stmt)) {
        emitIndexAssignStmt(*indexAssign);
        return;
    }
    if (auto* ifStmt = dynamic_cast<const IfStmt*>(&stmt)) {
        emitIfStmt(*ifStmt);
        return;
    }
    if (auto* whileStmt = dynamic_cast<const WhileStmt*>(&stmt)) {
        emitWhileStmt(*whileStmt);
        return;
    }
    if (auto* doWhileStmt = dynamic_cast<const DoWhileStmt*>(&stmt)) {
        emitDoWhileStmt(*doWhileStmt);
        return;
    }
    if (auto* forStmt = dynamic_cast<const ForStmt*>(&stmt)) {
        emitForStmt(*forStmt);
        return;
    }
    if (auto* foreachStmt = dynamic_cast<const ForeachStmt*>(&stmt)) {
        emitForeachStmt(*foreachStmt);
        return;
    }
    if (auto* switchStmt = dynamic_cast<const SwitchStmt*>(&stmt)) {
        emitSwitchStmt(*switchStmt);
        return;
    }
    if (dynamic_cast<const BreakStmt*>(&stmt)) {
        emitBreakStmt();
        return;
    }
    if (dynamic_cast<const ContinueStmt*>(&stmt)) {
        emitContinueStmt();
        return;
    }
    throw LpcRuntimeError("codegen: unsupported statement kind this slice");
}

void CodeGen::emitBlock(const Block& block) {
    // Real LPC/C89 block scoping: every "{ ... }" -- a standalone
    // statement, or an if/while/for body, or a function's own top-level
    // body -- opens its own nested scope for local declarations. Push an
    // empty scope, compile the block's statements (declareLocal() records
    // each new name into this scope as it runs), then pop the scope,
    // erasing those names from locals_ so a sibling block declared later
    // in the same function may reuse them -- see CodeGen.hpp's
    // localScopeStack_ comment and declareLocal()'s own.
    localScopeStack_.emplace_back();
    for (const auto& stmt : block.statements) {
        emitStatement(*stmt);
    }
    for (const auto& name : localScopeStack_.back()) {
        locals_.erase(name);
    }
    localScopeStack_.pop_back();
}

CompiledProgram CodeGen::generate(const Program& program,
                                   const std::vector<std::string>& inheritedObjectVarNames) {
    CompiledProgram result;
    out_ = &result;

    // Inherited object variables occupy the first slots, in the order
    // ObjectManager already flattened them in (parent-before-child, and
    // in inherit-statement order for multiple inherits), so this file's
    // own PushObjectVar/StoreObjectVar slot numbers -- and any inherited
    // function's, running via the parent's own bytecode against this same
    // object's variables() vector -- agree on what each slot means.
    objectVars_.clear();
    result.objectVarNames = inheritedObjectVarNames;
    for (size_t i = 0; i < inheritedObjectVarNames.size(); ++i) {
        objectVars_[inheritedObjectVarNames[i]] = static_cast<int>(i);
    }
    // The next slot number for a variable this file itself newly declares
    // must come from inheritedObjectVarNames.size() -- a plain count --
    // not objectVars_.size(). objectVars_ is a *name*-keyed map, and a
    // private variable's synthesized "$private#N" name is only unique
    // relative to the file that declared it: two files reached via
    // separate inherit branches (e.g. one leaf directly inherited, and
    // another leaf several levels down a different branch) can each
    // independently produce "$private#0" for their own first private
    // variable, since each was compiled standalone starting its own
    // local numbering at 0. When both branches are flattened into the
    // same inheritedObjectVarNames list, those two unrelated variables'
    // names collide in objectVars_, which silently keeps only the later
    // one -- undercounting the true number of inherited slots and
    // handing this file's own new variables slot numbers that overlap
    // ones already in use. Confirmed live via a multi-level inherit
    // chain test (see test_lexer.cpp's
    // testObjectVariableOffsetsComposeAcrossMultiLevelInheritChain):
    // objectVars_.size() read 2 instead of the real inherited count of
    // 4, handing the child's own first new variable slot 2 -- the same
    // absolute slot a sibling branch's own variable already legitimately
    // occupied.
    size_t nextObjectVarSlot = inheritedObjectVarNames.size();

    // Assign every top-level object variable a sequential slot before any
    // function body is compiled, so declaration order relative to the
    // functions that reference them does not matter (this driver already
    // fully separates parsing from codegen, unlike a single-pass real
    // LPC compiler, so this is a deliberate, harmless simplification).
    // (slot, initializer expr) for every object var declared with a
    // "= expr" initializer, collected here and compiled into a
    // synthesized "$objvarinit" function below -- see this function's
    // own comment further down for why a dedicated function is needed
    // rather than just running these inline right here.
    std::vector<std::pair<int, const AstNode*>> pendingVarInitializers;

    for (const auto& varDecl : program.objectVars) {
        // Redeclaring a variable name this file already inherits is real,
        // legal LPC, not a compile error: real compiler.c's own
        // define_variable() (compiler.c:1251-1296) only ever yywarn()s
        // "Redeclaration of global variable '...'." (compiler.c:1272) for
        // this exact case, hard-erroring only when the *existing* slot was
        // declared "nomask" (compiler.c:1282, "Illegal to redefine
        // 'nomask' variable"), which this driver does not yet track on
        // object variables at all (no evidence of a nomask-variable
        // redeclaration in any vendored corpus, so not added here). Real
        // corpus: TMI-2's own std/monster.c ("mapping alias ;", line 44)
        // redeclares the "alias" already declared by its own multiply-
        // inherited ancestor std/body/alias.c ("mapping alias;", line 15,
        // reached via std/body.c -> std/living.c -> std/monster.c),
        // confirmed live ("codegen: object variable \"alias\" already
        // declared" previously aborted compiling every single monster in
        // this mudlib, e.g. /obj/orc.c). Real semantics: the two slots
        // are separate storage (the ancestor's own already-compiled
        // bytecode keeps referencing its own fixed slot offset,
        // unaffected by anything a descendant does later); only which
        // *slot* an unqualified "alias" resolves to *within this file's
        // own new code from here on* changes, exactly like the ordinary
        // no-collision case just below -- so this is a no-op fall-through,
        // not a special case: the plain objectVars_[name] = slot
        // assignment a few lines down already provides the correct
        // shadowing behavior once the throw is removed.
        int slot = static_cast<int>(nextObjectVarSlot);
        ++nextObjectVarSlot;
        // The real name is what this file's own code resolves through
        // (objectVars_, used for every PushObjectVar/StoreObjectVar in
        // this compile), regardless of privacy -- privacy only affects
        // what a *child* sees. A private variable's slot still has to
        // exist at this same position in result.objectVarNames (an
        // inheriting child's own new variables must start numbering
        // after it, matching where this file's own already-compiled
        // bytecode expects it), but is recorded there under a
        // synthesized name a real LPC identifier can never equal (see
        // the lambda/temp-local synthesized-name precedent elsewhere in
        // this file), so a child can never resolve it by the real name
        // and is free to declare its own unrelated variable reusing that
        // name without a collision -- confirmed live: std/living.c's
        // own "static private int __Locked, __LastAged;" and
        // std/user.c's separate, unrelated "static int __LastAged;".
        objectVars_[varDecl->name] = slot;
        result.objectVarNames.push_back(
            varDecl->isPrivate ? "$private#" + std::to_string(slot) : varDecl->name);
        if (varDecl->initializer) {
            pendingVarInitializers.emplace_back(slot, varDecl->initializer.get());
        }
    }

    result.inherits = program.inherits;

    // "type name = expr;" object variable initializers (see Ast.hpp's
    // ObjectVarDecl comment) have no dedicated apply real LPC calls for
    // them -- they run as part of the object's own implicit
    // initialization, before create(). This driver makes that explicit:
    // a synthesized "$objvarinit" function (a name no real LPC
    // identifier can equal, same convention as the lambda/private-slot
    // synthesized names elsewhere in this file) assigns each one, and
    // ObjectManager calls it (walking the inherit chain parent-first)
    // immediately before "create" on every new instance -- see
    // ObjectManager::runObjectVarInitializers(). Built via the same
    // per-function reset/entryPoint/FunctionEntry pattern the real
    // function-compilation loop below uses, so an initializer expression
    // containing its own closure literal ("(: ... :)") still gets its
    // pending lambda drained correctly.
    if (!pendingVarInitializers.empty()) {
        locals_.clear();
        nextLocalSlot_ = 0;
        localScopeStack_.clear();
        loopStack_.clear();
        foreachCounter_ = 0;
        switchCounter_ = 0;
        indexAssignCounter_ = 0;

        FunctionEntry entry;
        entry.name = "$objvarinit";
        entry.entryPoint = static_cast<uint32_t>(result.code.size());
        entry.numArgs = 0;

        for (const auto& [slot, initExpr] : pendingVarInitializers) {
            emitExpr(*initExpr);
            result.code.push_back(Instruction{OpCode::StoreObjectVar, slot, 0});
        }
        result.code.push_back(Instruction{OpCode::Return, 0, 0});

        entry.numLocals = static_cast<uint8_t>(nextLocalSlot_);
        result.functions.push_back(entry);
        emitPendingLambdas();
    }

    for (const auto& fn : program.functions) {
        // Prototype-only declarations (e.g. "void create();" in a header)
        // have nothing to generate. Skipping them here also means that if
        // the same name has a real definition elsewhere in this compiled
        // unit, only that definition produces a FunctionEntry, so there is
        // never a duplicate/conflicting entry for one function name.
        if (!fn->body) continue;

        locals_.clear();
        nextLocalSlot_ = 0;
        localScopeStack_.clear();
        loopStack_.clear();
        foreachCounter_ = 0;
        switchCounter_ = 0;
        indexAssignCounter_ = 0;
        for (const auto& param : fn->params) {
            declareLocal(param.name);
        }

        FunctionEntry entry;
        entry.name = fn->name;
        entry.entryPoint = static_cast<uint32_t>(result.code.size());
        entry.numArgs = static_cast<uint8_t>(fn->params.size());

        if (fn->body) {
            emitBlock(*fn->body);
        }
        result.code.push_back(Instruction{OpCode::Return, 0, 0});

        entry.numLocals = static_cast<uint8_t>(nextLocalSlot_);
        result.functions.push_back(entry);

        // Any InlineLambdaExpr this function's own body queued (see
        // CodeGen.hpp's PendingLambda comment) compiles right here, after
        // this function's Return, so its bytecode lands at its own
        // distinct offset rather than inside the function above.
        emitPendingLambdas();
    }

    out_ = nullptr;
    return result;
}

// See CodeGen.hpp's PendingLambda comment for why this exists instead of
// emitting a lambda's body in place. Each entry compiles exactly like an
// ordinary top-level function in the loop above (own locals_ scope, own
// FunctionEntry, ends in Return), except its body is a plain comma-
// separated expression list rather than a Block: every expression is
// evaluated for effect except the last, whose value is left on the stack
// for Return to pick up, matching real LPC's comma-expression semantics
// for a functional's body (see Ast.hpp's InlineLambdaExpr comment).
// Index-based, not range-for: compiling one lambda's body can itself
// queue more (a "(: :)" nested inside this lambda), and copying each
// entry by value before compiling protects against the vector
// reallocating out from under a reference when that happens.
void CodeGen::emitPendingLambdas() {
    size_t i = 0;
    while (i < pendingLambdas_.size()) {
        PendingLambda pending = pendingLambdas_[i];
        ++i;

        locals_.clear();
        // Reserves slots 0..paramCount-1 up front for "$1".."$N" (see
        // LambdaParamExpr's own PushLocal emission just above) -- real
        // LPC comma-expr lambda bodies are expression-only, so there is
        // no "int x;" declaration inside one that could otherwise want
        // slot 0 for itself; any locals a nested lambda declares get its
        // own separate slot space in its own later iteration of this
        // same loop instead.
        nextLocalSlot_ = pending.expr->paramCount;
        localScopeStack_.clear();
        loopStack_.clear();
        foreachCounter_ = 0;
        switchCounter_ = 0;
        indexAssignCounter_ = 0;

        FunctionEntry entry;
        entry.name = pending.name;
        entry.entryPoint = static_cast<uint32_t>(out_->code.size());
        entry.numArgs = static_cast<uint8_t>(pending.expr->paramCount);

        const auto& bodyExprs = pending.expr->bodyExprs;
        for (size_t j = 0; j < bodyExprs.size(); ++j) {
            emitExpr(*bodyExprs[j]);
            if (j + 1 < bodyExprs.size()) {
                out_->code.push_back(Instruction{OpCode::Pop, 0, 0});
            }
        }
        out_->code.push_back(Instruction{OpCode::Return, 0, 0});

        entry.numLocals = static_cast<uint8_t>(nextLocalSlot_);
        out_->functions.push_back(entry);
    }
    pendingLambdas_.clear();
}

} // namespace amlp
