#include "amlp/scheduler/Scheduler.hpp"
#include "amlp/config/Config.hpp"
#include "amlp/dialect/LpcDialect.hpp"
#include "amlp/net/Server.hpp"
#include "amlp/object/LiveObjectRegistry.hpp"
#include "amlp/object/LpcObject.hpp"
#include "amlp/vm/VM.hpp"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <thread>

namespace amlp {

namespace {
std::atomic<bool> g_shutdownRequested{false};
}

Scheduler::Scheduler(VM& vm)
    : vm_(vm), lastHeartbeat_(std::chrono::steady_clock::now()) {}

void Scheduler::requestShutdown() {
    g_shutdownRequested.store(true);
}

bool Scheduler::isShutdownRequested() {
    return g_shutdownRequested.load();
}

void Scheduler::run(Server& server, int maxIterations) {
    g_shutdownRequested.store(false);
    int iterations = 0;

    for (;;) {
        server.pollOnce();

        // Real backend.c's own "if (obj_list_replace || obj_list_destruct)
        // remove_destructed_objects();", right at the top of its own
        // while(1) loop -- this driver has no deferred-destruct list to
        // match (LpcObject is destructed immediately, see VM::
        // destructObject()'s own comment), only the replace_program()
        // half. A no-op call when nothing is pending (processPendingReplacePrograms()'s
        // own early-out), so unlike tickHeartbeats() this needs no
        // separate elapsed-time or "is anything pending" gate here.
        vm_.processPendingReplacePrograms();

        // Real backend.c only calls call_heart_beat() once real elapsed
        // time crosses a HEARTBEAT_INTERVAL boundary ("if(!(current_time %
        // HEARTBEAT_INTERVAL)) call_heart_beat();" in call_out.c's own main
        // loop) -- reproduced here as a plain elapsed-wall-time gate around
        // tickHeartbeats() rather than inside it, so tickHeartbeats() itself
        // stays a deterministic, directly-testable "process one cycle"
        // function (see its own header comment).
        auto now = std::chrono::steady_clock::now();
        if (now - lastHeartbeat_ >= kHeartbeatCycle) {
            lastHeartbeat_ = now;
            tickHeartbeats();
            // Real ALARM_TIME's own doc comment (config.h.in, LDMud):
            // "the granularity of the call_outs, and base granularity of
            // heart_beat, reset und clean_up calls" -- reset/clean_up
            // share heart_beat's own real timing granularity in both real
            // drivers (LDMud's ALARM_TIME default is 2, matching
            // kHeartbeatCycle exactly; real FluffOS's own
            // look_for_objects_to_swap() runs on its own coarser 5-minute
            // sweep instead, backend.c:216 -- LDMud's tighter granularity
            // is used here since it is the one this driver's own
            // kHeartbeatCycle already matches).
            tickResetsAndCleanup();
        }
        tickCallOuts();

        // ROADMAP.md row 2.5's own first slice: resumes any runAsync()
        // coroutine parked on OpCode::Suspend whose own delay has now
        // elapsed. Deliberately a VM method the same way
        // processPendingReplacePrograms() just above already is, not a
        // Scheduler method of its own the way this row's ROADMAP.md
        // note first sketched it ("Scheduler::resumeReadyTasks(now)")
        // -- see VM.hpp's own resumeReadyAsyncTasks() comment for why
        // that changed once the pieces were actually wired together.
        vm_.resumeReadyAsyncTasks(now);

        ++iterations;
        if (maxIterations > 0 && iterations >= maxIterations) break;
        if (g_shutdownRequested.load()) break;

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

int64_t Scheduler::newCallOutHandle() {
    // Real new_call_out() encodes the handle from the ring-buffer slot plus
    // a per-process "unique" counter (call_out.c: "tm += CALLOUT_CYCLE_SIZE
    // * ++unique; cop->handle = tm;") -- purely so it can find the entry
    // again in O(1) by decoding the slot back out of the handle. This
    // driver has no equivalent ring buffer (callOuts_ is a plain vector,
    // see the class's own header comment), so a bare monotonic counter is
    // an equivalent, simpler encoding: still unique, still stable, and
    // nothing in this mudlib inspects a handle's bit structure -- every
    // real call site only ever stores it and later compares it back
    // (cmds/mortal/_trade.c's own "tid = call_out(...); ...;
    // remove_call_out(tid)") or checks it for truthiness/non- -1.
    return nextHandle_++;
}

int64_t Scheduler::addCallOut(CallOutEntry entry) {
    entry.handle = newCallOutHandle();
    int64_t handle = entry.handle;
    callOuts_.push_back(std::move(entry));
    return handle;
}

namespace {
// Real time_left() (call_out.c): remaining whole seconds until an entry
// would have fired. This driver stores an absolute dueAt time_point rather
// than real call_out.c's delta-linked-list-of-slots representation, so the
// equivalent is just dueAt - now, clamped to 0 (a due-but-not-yet-fired
// entry reports 0 remaining, not negative).
int64_t remainingSeconds(const std::chrono::steady_clock::time_point& dueAt) {
    auto now = std::chrono::steady_clock::now();
    if (dueAt <= now) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(dueAt - now).count();
}
} // namespace

int64_t Scheduler::removeCallOutByName(const std::shared_ptr<LpcObject>& owner, const std::string& function) {
    if (!owner) return -1;
    // Real remove_call_out(object_t*, const char*): "(*copp)->ob == ob &&
    // strcmp((*copp)->function.s, fun) == 0" -- a closure-bound entry's own
    // real cop->ob is never set for the string form, so it can never match
    // here either (entry.closure being non-null already means entry.target
    // was never populated for that entry).
    for (auto it = callOuts_.begin(); it != callOuts_.end(); ++it) {
        if (!it->closure && it->target.lock() == owner && it->function == function) {
            int64_t remaining = remainingSeconds(it->dueAt);
            callOuts_.erase(it);
            return remaining;
        }
    }
    return -1;
}

int64_t Scheduler::removeCallOutByHandle(int64_t handle) {
    for (auto it = callOuts_.begin(); it != callOuts_.end(); ++it) {
        if (it->handle == handle) {
            int64_t remaining = remainingSeconds(it->dueAt);
            callOuts_.erase(it);
            return remaining;
        }
    }
    return -1;
}

void Scheduler::removeAllCallOutsForObject(const std::shared_ptr<LpcObject>& obj) {
    for (auto it = callOuts_.begin(); it != callOuts_.end();) {
        std::shared_ptr<LpcObject> owner =
            it->closure ? it->closure->owner.lock() : it->target.lock();
        bool matchesObj = obj && owner == obj;
        bool ownerGone = !owner || owner->isDestructed();
        if (matchesObj || ownerGone) {
            it = callOuts_.erase(it);
        } else {
            ++it;
        }
    }
}

int64_t Scheduler::findCallOutByName(const std::shared_ptr<LpcObject>& owner, const std::string& function) const {
    if (!owner) return -1;
    for (const auto& entry : callOuts_) {
        if (!entry.closure && entry.target.lock() == owner && entry.function == function) {
            return remainingSeconds(entry.dueAt);
        }
    }
    return -1;
}

int64_t Scheduler::findCallOutByHandle(int64_t handle) const {
    for (const auto& entry : callOuts_) {
        if (entry.handle == handle) return remainingSeconds(entry.dueAt);
    }
    return -1;
}

void Scheduler::setHeartbeatInterval(const std::shared_ptr<LpcObject>& obj, int64_t to) {
    if (!obj) return;

    // Real set_heart_beat(): "if (!to) { ... remove from heart_beats[] ...
    // ob->flags &= ~O_HEART_BEAT; return 1; }"
    if (to == 0) {
        obj->setHeartbeatInterval(0);
        auto it = std::find_if(heartbeats_.begin(), heartbeats_.end(),
            [&obj](const HeartbeatEntry& e) { return e.target.lock() == obj; });
        if (it != heartbeats_.end()) heartbeats_.erase(it);
        return;
    }

    auto it = std::find_if(heartbeats_.begin(), heartbeats_.end(),
        [&obj](const HeartbeatEntry& e) { return e.target.lock() == obj; });
    if (it != heartbeats_.end()) {
        // Real: "if (ob->flags & O_HEART_BEAT) { if (to < 0) return 0; ...
        // heart_beats[index].time_to_heart_beat = heart_beats[index].heart_beat_ticks = to; }"
        // -- already enabled: a negative update is rejected outright,
        // leaving the existing interval and countdown untouched.
        if (to < 0) return;
        obj->setHeartbeatInterval(static_cast<int>(to));
        it->ticksRemaining = static_cast<int>(to);
        return;
    }

    // Real: "if (to < 0) to = 1;" -- only the fresh-enable branch clamps a
    // negative interval, to 1 (fire every cycle), rather than rejecting it.
    int interval = to < 0 ? 1 : static_cast<int>(to);
    obj->setHeartbeatInterval(interval);
    heartbeats_.push_back(HeartbeatEntry{obj, interval});
}

void Scheduler::tickHeartbeats() {
    // Decide who fires this cycle and prune dead entries FIRST, entirely
    // before calling any LPC code -- heart_beat() itself is free to call
    // set_heart_beat() on any object, including itself (real std/user.c's
    // own "if(!interactive(this_object())) { set_heart_beat(0); return; }"),
    // which mutates heartbeats_ via setHeartbeatInterval()'s own erase()/
    // find_if(). An earlier version of this function held a live iterator
    // into heartbeats_ across the vm_.callFunction() call below and
    // crashed for exactly this reason: a heart_beat() call re-entering
    // setHeartbeatInterval() invalidated the outer loop's own iterator --
    // confirmed live (a genuine SIGSEGV in vector::erase(), caught during
    // this slice's own live-verification pass, not a hypothetical
    // concern). Collecting a separate snapshot first, the same way
    // tickCallOuts() below already does, closes this the same way call_out
    // firing was already safe against a callout that reschedules itself.
    std::vector<std::shared_ptr<LpcObject>> due;
    for (auto it = heartbeats_.begin(); it != heartbeats_.end();) {
        auto obj = it->target.lock();
        if (!obj) {
            // Real call_heart_beat() relies on O_DESTRUCTED objects having
            // already been pruned from heart_beats[] by remove_all_call_out()-
            // style cleanup at destruct() time; this driver has no
            // O_DESTRUCTED flag (see LpcObject's own known-stub note), so a
            // dead weak_ptr here is the equivalent signal -- just as valid a
            // "this object is gone" check, pruned the same way.
            it = heartbeats_.erase(it);
            continue;
        }
        if (--it->ticksRemaining < 1) {
            it->ticksRemaining = obj->heartbeatInterval();
            due.push_back(obj);
        }
        ++it;
    }

    for (auto& obj : due) {
        try {
            vm_.resetEvalCost();
            // Origin::Driver (this call's own default): real backend.c's
            // own heart_beat firing is "call_direct(ob, ...,
            // ORIGIN_DRIVER, 0)", confirmed directly -- unlike call_out's
            // own string-form firing just below in tickCallOuts(), which
            // is real ORIGIN_INTERNAL instead (see that call site's own
            // citation); the two driver-fired timers are not the same
            // origin, so this is left explicit-by-omission deliberately,
            // not an oversight.
            vm_.callFunction(obj, "heart_beat", {});
        } catch (const std::exception& e) {
            std::cerr << "[heart_beat] " << obj->filename() << ": " << e.what() << "\n";
        }
    }
}

namespace {
// Real reset_object()'s own string-hook dispatch (LDMud object.c:865-873):
// driverHooks_[hookNum] can be a closure, a string, or (having never had
// set_driver_hook() called for that slot) unset. Real closure-typed
// H_RESET/H_CLEAN_UP have zero confirmed real corpus usage -- core-lib's
// own real hooks.c (the one confirmed LDMud-targeting corpus vendored
// here) configures both as plain strings, "reset"/"clean_up", the exact
// fallback default used when the slot is unset too -- so the closure
// form is honestly left unimplemented here, the same "zero confirmed
// real corpus usage" stance H_MODIFY_COMMAND's own T_CLOSURE/T_STRING
// forms already took (VM::dispatchCommand()'s own comment, VM.cpp).
// FluffOS has no hook indirection here at all (a fixed APPLY_RESET/
// APPLY_CLEAN_UP apply, real object.c:1896-1927), so the same fallback
// name is exactly what real FluffOS always calls too.
std::string hookFunctionName(VM& vm, int hookNum, const char* fallback) {
    Value hookVal = vm.getDriverHook(hookNum);
    if (auto* name = std::get_if<std::string>(&hookVal.data)) return *name;
    return fallback;
}
} // namespace

void Scheduler::tickResetsAndCleanup() {
    constexpr int kHReset = 7;    // mudlib/sys/driver_hook.h
    constexpr int kHCleanUp = 8;
    // Real TIME_TO_RESET/TIME_TO_CLEAN_UP defaults (LDMud
    // temp/ldmud/autoconf/configure: "DEFAULTwith_time_to_reset=1800",
    // "DEFAULTwith_time_to_clean_up=3600", confirmed against the real
    // vendored ./configure --help text too). Real FluffOS uses the same
    // formula shape in its own reset_object() (object.c:1898-1900,
    // "current_time + TIME_TO_RESET/2 + random_number(TIME_TO_RESET/2)"),
    // confirmed identical to LDMud's; its own specific default value is
    // read from a runtime config file rather than a compile-time default
    // (include/runtime_config.h's own CFG_INT indirection), not
    // independently confirmed here, so these numbers are cited to LDMud's
    // real compile-time default specifically.
    constexpr std::chrono::seconds kTimeToReset{1800};
    constexpr std::chrono::seconds kTimeToCleanUp{3600};

    // Real FluffOS vs. real LDMud disagree on exactly one rule in this
    // whole mechanism: whether a real (non-virtual) reset() firing this
    // same cycle suppresses clean_up() this same cycle. This was
    // previously hardcoded to LDMud's rule regardless of configured
    // dialect; resolved 2026-08-20 by re-reading both real sources side
    // by side rather than guessing which one "the more conservative
    // choice" actually matches in practice:
    //   - LDMud backend.c:1321 computes "time_since_ref = current_time -
    //     obj->time_of_ref" into a local BEFORE its own reset block runs
    //     (backend.c:1331-1387), then gates clean_up() on that same local
    //     plus an explicit "!bResetCalled" (backend.c:1402-1406) -- a real
    //     reset() firing this tick genuinely suppresses clean_up() this
    //     same tick, confirmed directly, not inferred.
    //   - Real FluffOS's own look_for_objects_to_swap() computes
    //     "ready_for_clean_up" into a local at backend.c:241, ALSO before
    //     its own reset_object() call at backend.c:251, but never gates
    //     it on whether that reset_object() call actually ran -- the
    //     comment right above the check even says so explicitly ("Check
    //     reference time before reset() is called."), and clean_up() at
    //     backend.c:267 fires whenever ready_for_clean_up and
    //     O_WILL_CLEAN_UP hold, unconditionally. Confirmed no same-cycle
    //     suppression exists in real FluffOS at all.
    // Both real drivers agree that clean_up() *eligibility* itself must
    // be decided from time_of_ref as it stood before reset() ran, not
    // after -- reset()'s own call is an ordinary "touch" (this driver's
    // own VM::callFunction() touchTimeOfRef(), matching real apply_low())
    // that would otherwise reset the very clock clean_up() eligibility
    // reads, silently defeating a same-cycle collision under either
    // dialect if read after the fact. So `readyForCleanUp` below is
    // latched from `obj->timeOfRef()` before the reset block runs, the
    // same ordering both real drivers use, and only the *suppression* on
    // top of that latched value is dialect-gated.
    LpcDialect dialect = dialectFromString(vm_.config().dialect());

    auto now = std::chrono::steady_clock::now();
    // Snapshot every live object first, the same "decide who fires before
    // calling any LPC code" reasoning tickHeartbeats() above already
    // documents in full -- reset()/clean_up() are just as free to call
    // set_heart_beat()/destruct()/move_object() on arbitrary objects as
    // heart_beat() is, and LiveObjectRegistry::all() already returns a
    // fresh vector, not a live view, so this is naturally safe against
    // that the same way.
    for (auto& obj : LiveObjectRegistry::all()) {
        if (obj->isDestructed()) continue;

        // Latched before the reset block below runs -- see this method's
        // own header comment just above for why (both real drivers do the
        // same, backend.c:241 FluffOS / backend.c:1321 LDMud): reset()
        // firing touches timeOfRef() like any other call into the object,
        // which would otherwise silently and dialect-independently defeat
        // this same-cycle reading if it were taken after the fact.
        bool readyForCleanUp = obj->willCleanUp() && (now - obj->timeOfRef()) > kTimeToCleanUp;

        // ------ Reset ------
        // Real backend.c's own due-check ("obj->time_reset && obj->
        // time_reset < current_time" in LDMud; "(ob->flags & O_WILL_RESET)
        // && (ob->next_reset < current_time) && !(ob->flags &
        // O_RESET_STATE)" in FluffOS -- both real drivers agree on the
        // substance: skip a resetState() object with a "virtual" reset
        // (object.c:75-82's own real doc comment: "the backend simply
        // sets a new .time_reset time, but does not do any real action"),
        // only really call reset() once something has touched the object
        // since its last one. Deliberately not replicating either real
        // driver's own further per-cycle batching gate (LDMud's "!did_reset
        // || !comm_time_to_call_heart_beat", FluffOS's whole-list-per-5-
        // minutes sweep cap) -- a real-driver performance strategy for
        // large object counts under a fixed per-tick time budget, not a
        // semantic requirement; every object due this tick is processed
        // this tick here, a flagged, deliberate simplification for this
        // driver's own much smaller expected object counts.
        bool resetCalledThisCycle = false;
        if (now >= obj->timeReset()) {
            if (obj->resetState()) {
                // Virtual reset: nothing touched this object, just push
                // the timer out again.
                obj->armReset(kTimeToReset);
            } else {
                resetCalledThisCycle = true;
                // "Be sure to update time first!" -- real reset_object()'s
                // own comment, both drivers, cited above.
                obj->armReset(kTimeToReset);
                std::string resetFn = hookFunctionName(vm_, kHReset, "reset");
                if (vm_.functionExists(obj, resetFn)) {
                    try {
                        vm_.resetEvalCost();
                        vm_.callFunction(obj, resetFn, {}, Origin::Driver);
                    } catch (const std::exception& e) {
                        std::cerr << "[reset] " << obj->filename() << ": " << e.what() << "\n";
                    }
                } else {
                    // Real "no reset() in the object" branch, both real
                    // drivers: permanently stop trying (object.c:1904-1906
                    // FluffOS, object.c:869-870 LDMud).
                    obj->disableReset();
                }
                if (obj->isDestructed()) continue;
                // reset_object()'s own unconditional final step, both
                // real drivers (object.c:884 LDMud, object.c:1908 FluffOS):
                // set regardless of whether reset() was actually found/
                // ran/returned anything.
                obj->setResetState(true);
            }
        }

        // ------ Clean Up ------
        // Real backend.c: only when O_WILL_CLEAN_UP was still set and
        // enough time had passed since the last touch as of the top of
        // this iteration (readyForCleanUp, latched above) -- plus, real
        // LDMud only (backend.c:1402-1406's own "!bResetCalled"), not on
        // the same cycle a real reset() just fired for this same object.
        // Real FluffOS has no analog of that suppression at all (see this
        // method's own header comment for the full backend.c citation on
        // both sides), so this is now genuinely dialect-gated rather than
        // hardcoded to LDMud's rule for every dialect.
        bool suppressedBySameCycleReset = resetCalledThisCycle && dialect == LpcDialect::LdMud;
        if (readyForCleanUp && !suppressedBySameCycleReset) {
            // Real "push_number(ob->flags & (O_CLONE) ? 0 : ob->prog->
            // ref)" (FluffOS, object.c:290) / equivalent LDMud
            // (O_CLONE|O_REPLACED -> 0, else prog->ref, backend.c:1425-1431)
            // -- see LpcObject::isClone()'s own header comment for why a
            // fixed 1 stands in for the real prog->ref count here.
            std::vector<Value> args{Value(obj->isClone() ? int64_t{0} : int64_t{1})};
            // real "int save_reset_state = obj->flags & O_RESET_STATE;"
            // (both drivers) -- calling clean_up() would otherwise clear
            // it as an ordinary touch (VM::callFunction()'s own real
            // apply_low()-equivalent clear), but real code explicitly
            // restores whatever it was beforehand once the call returns.
            bool savedResetState = obj->resetState();
            Value result;
            try {
                vm_.resetEvalCost();
                result = vm_.callFunction(obj, hookFunctionName(vm_, kHCleanUp, "clean_up"), args, Origin::Driver);
            } catch (const std::exception& e) {
                std::cerr << "[clean_up] " << obj->filename() << ": " << e.what() << "\n";
            }
            if (obj->isDestructed()) continue;
            if (!isTruthy(result)) obj->setWillCleanUp(false);
            obj->setResetState(savedResetState);
        }
    }
}

void Scheduler::tickCallOuts() {
    auto now = std::chrono::steady_clock::now();
    // Real call_out.c's own main loop: "Move the first call_out out of the
    // chain" before invoking it, so a call_out whose own body reschedules
    // itself (the dominant repeating-timer idiom this mudlib uses, e.g.
    // std/user.c's own rifts_regen_tick()) never corrupts the structure
    // being iterated. Collect due entries first, erase them from callOuts_,
    // then fire each one -- same ordering, expressed without an intrusive
    // linked list.
    std::vector<CallOutEntry> due;
    for (auto it = callOuts_.begin(); it != callOuts_.end();) {
        if (it->dueAt <= now) {
            due.push_back(std::move(*it));
            it = callOuts_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto& entry : due) {
        if (entry.closure) {
            // Real call_function_pointer(): a destructed closure owner
            // fails the call, not a silent skip -- but that failure is
            // still just one call_out's own error, isolated the same way.
            try {
                vm_.resetEvalCost();
                vm_.callClosure(entry.closure, entry.args);
            } catch (const std::exception& e) {
                std::cerr << "[call_out] (closure): " << e.what() << "\n";
            }
            continue;
        }
        auto obj = entry.target.lock();
        if (!obj) continue; // real: "if (!ob || (ob->flags & O_DESTRUCTED)) { free_call(cop); }" -- silently dropped, not an error.
        try {
            vm_.resetEvalCost();
            // Real call_out.c's own firing loop: "apply(cop->function.s,
            // cop->ob, extra, ORIGIN_INTERNAL)" for the string-name form
            // (confirmed directly) -- *not* ORIGIN_DRIVER despite being a
            // driver-triggered timer fire, the same real distinction
            // socket callbacks and input_to() dispatch also make (see
            // Server.cpp's own two citations). heart_beat firing just
            // above this, by contrast, really is ORIGIN_DRIVER (backend.c's
            // own "call_direct(ob, ..., ORIGIN_DRIVER, 0)") -- the two
            // are not interchangeable despite both being Scheduler-fired.
            vm_.callFunction(obj, entry.function, entry.args, Origin::Internal);
        } catch (const std::exception& e) {
            std::cerr << "[call_out] " << obj->filename() << "::" << entry.function << "(): " << e.what() << "\n";
        }
    }
}

} // namespace amlp
