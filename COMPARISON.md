# AMLP vs. real FluffOS, LDMud, and DGD

An evidence-based comparison, not a marketing page. Every number here is
either read directly out of `ROADMAP.md`'s own accounting (which is
itself sourced from `git log`-recorded, per-row citations against the
vendored reference sources) or freshly re-checked against those same
vendored sources while writing this file (`temp/reference/fluffos-2.9-ds2.08/`,
`temp/ldmud/`, `temp/dgd/`: see `CLAUDE.md` for how these are tracked
and why they are gitignored). Nothing here is from memory or general
reputation. Where a number is an estimate rather than an exact count,
it is labeled as one.

**DGD is comparison-only context, not a target.** `ROADMAP.md`'s own
Phase 1 header, and the scope clarification dated 2026-08-18, are
explicit about this: the actual goal is a FluffOS/LDMud-level driver
done better than either, not three-way parity with DGD. DGD numbers
below exist so a reader can see how a third, architecturally different
driver solved the same problems, not because AMLP is trying to match it
feature-for-feature.

Last updated: 2026-08-21 (a fresh full-project status sweep: Phase 0
confirmed still 16/16 with no open sub-gap of its own, row 1.7's own
`privilege_violation()` authorization gate now real for four trigger
points, `bind_lambda()`'s cross-object form, `set_driver_hook()`,
`call_out_info()`, and `input_to()`, and every driver-hook/efun-surface
number below re-checked against `ROADMAP.md`'s own current row text
rather than assumed carried forward. Phase 1's real-blocker fraction
itself is unchanged at 10/11, row 1.7 was already checked off before
this pass; what changed is how much of its own remaining cell is now
closed. See `ROADMAP.md`'s own dated corrections on each row and this
file's own rewritten sections below for the full accounting.)

**Re-swept 2026-08-21 (later the same day, once more, after
`notes/ACCOUNT_LOGIN_PLAN.md` closed out its own originally scoped
build ordering):** every Phase/row number above re-checked directly
against `ROADMAP.md`'s own current checkboxes (`awk`-counted per phase
section, not eyeballed) rather than trusted stale -- all five numbers
in the table below are unchanged (Phase 0 16/16, Phase 1 10/11 real-
blockers-only and 10/16 including DGD, Phase 2 0/22, Phase 3 0/8), row
1.8 confirmed still `[ ]` with the same zero-real-evidence scope
already on record, and every Phase 2/3 `src/` directory confirmed still
`instruct.md`-only (the one exception, `src/scheduler/Scheduler.cpp`,
predates this sweep by many sessions and backs Phase 0/1's own already-
shipped `call_out()`/`heart_beat()`, not any Phase 2 concurrency item).
This session's own real work (`notes/ACCOUNT_LOGIN_PLAN.md`'s build
ordering items 4 and 5, character creation and character selection,
plus a bounded stopgap fix for a width > 1 mapping save/restore
silent-truncation bug found by the immediately prior session's own
fact-check) is real mudlib content and a driver-level correctness fix,
not a `ROADMAP.md` row of its own -- see `STATUS.md`'s own dated
entries for the full writeups.
`ROADMAP.md` and `STATUS.md` are the living
documents; if this file and either of those disagree on a specific row's
status, trust `ROADMAP.md`'s own checkbox and re-derive this file's own
summary from it rather than the reverse.

**Re-swept 2026-08-22 (a fresh full-project status sweep, row 1.7's own
remaining `call_out_info()`/`input_to()` privilege-gate follow-on now
real too): the Phase 2/3 "not started" framing two paragraphs below was
stale and is corrected here.** Re-counted every phase directly against
`ROADMAP.md`'s own current checkboxes (`awk`-counted per phase section,
not eyeballed, and cross-checked against a fresh local build/test run)
rather than trusted forward from the 2026-08-21 sweep above: Phase 0 is
still 16/16 (100%, unchanged); Phase 1 is still 10/11 real-blockers-only
(91%, unchanged -- row 1.7 was already checked off before this pass and
stays so, its own remaining cell items, `H_LOAD_UIDS`/`H_CLONE_UIDS`/
`H_INCLUDE_DIRS`, type-map validation, plain `lambda()`, and
`inaugurate_master()`'s own arg=1/2/3 reload cases, all confirmed still
either zero real corpus evidence or the same weak, single-file evidence
already on record, not newly re-derived); but **Phase 2 is 3/22 (2.9
apply cache, 2.12 full PCRE suite, and 2.15 SQLite, all built and landed
2026-08-21, one sweep before this one) and Phase 3 is 1/9 (3.9, a real
third-party mudlib boot-and-play confirmation, also landed 2026-08-21;
row 3.9 itself is new since the 2026-08-21 sweep above, so Phase 3's own
row count moved from 8 to 9, not just its done count)** -- both phases
had already moved off 0% by the time of the 2026-08-21 sweep two
paragraphs below, which undercounted them; this pass corrects it rather
than repeating the same stale number forward again. One real
documentation inconsistency found and worth naming rather than quietly
smoothing over: `ROADMAP.md` row 1.7's own "715 tests passing" note
undercounts the actual suite size at that point in real history, since
it measured only that row's own local delta (709 to 715) without
accounting for the ~28 additional tests rows 2.9/2.12/2.15 had already
added in an intervening session; the real, freshly-measured total as of
this sweep, rebuilt and re-run directly rather than trusted from any
prior note, is **743**, matching `STATUS.md`'s own current dated entry,
not 715. See that entry for the full sweep and the recommendation
written there for what to pick up next.

---

## How far along is AMLP, in plain language

AMLP is a working, from-scratch LPC driver (lexer/parser/compiler/VM/
object system/network layer, no code shared with any real driver) that
already runs a real bundled mudlib end to end, login, movement,
command dispatch, object creation, persistence, sockets, and has grown
a substantial fraction of real FluffOS's own efun surface plus the start
of genuine LDMud and DGD dialect support behind a config switch. It is
not yet a drop-in replacement for either real driver: Phase 2 and Phase 3
(the features meant to eventually *exceed* what either real driver
offers) are still mostly planning documents rather than implemented
code, though three Phase 2 rows (apply cache, full PCRE suite, built-in
SQLite) and one Phase 3 row (a real third-party mudlib boot-and-play
confirmation) are now real and landed, not just planned, and Phase 1's
own real, corpus-driven dialect-compatibility work is now substantially
exhausted: see immediately below.

**Phase 0 (stabilize the current base): complete.**
16 of 16 rows checked off, including `parse_*` (0.13a), the large
natural-language parser package that was the one still-open row as of
an earlier revision of this file: all 8 named efuns are implemented,
including real two-object matching and the `nicks` nickname-mapping
argument (`parse_sentence()`'s own last remaining sub-gap, closed
2026-08-20), no open sub-gap remains anywhere in this row.

**Phase 1 (dialect universality): the real, corpus-driven work is now
substantially exhausted, not "a bit under half done."** An earlier
revision of this file undercounted this badly: five rows (1.2, 1.3,
1.4, 1.9, 1.16) had real, already-landed work sitting in their own
`ROADMAP.md` cells across several earlier sessions that was never
reflected in their checkboxes, corrected 2026-08-20. Counting only the
rows that actually gate Phase 1 completion (DGD-only rows are comparison
context, not blockers, per the scope clarification above): **10 of 11
real blocking rows are done.** The one still open, row 1.8
(`#'lfun::`/`#'sefun::`/`#'var::` closure-literal prefixes), was
investigated for the first time this same session and found to have
**zero real corpus usage** for its own remaining scope across every
vendored mudlib corpus in `temp/`: deferred on the same
zero-evidence-discipline basis this project has applied consistently
elsewhere (`bind_lambda()`'s cross-object form, `lambda()` itself, DGD's
own five still-open rows), not forgotten or blocked. Counting only rows
with real, non-DGD, non-zero-evidence scope still open, as opposed to
counting every unchecked box regardless of what is actually left in it:
**zero real Phase 1 blockers remain.** See `ROADMAP.md`'s own row-by-row
citations for the underlying evidence on every count below, not repeated
here.

**Phase 2 and Phase 3: mostly still planning, but no longer zero code.**
Most of the directories Phase 2/3 work would live in (`src/jit`,
`src/gc`, `src/lsp`, `src/persist`, `src/security`) still contain
nothing but their own planning `instruct.md`, and the large novel-
architecture items (coroutine scheduling, JIT compilation, hotboot,
world-level statedump, TLS, generational GC) are all still real,
considered plans, not real code. But seven Phase 2 rows and one Phase 3
row are now real, landed code, not plans: the apply cache (2.9), the
full PCRE `pcre_match`/`pcre_assoc` suite (2.12), built-in SQLite
`db_*` efuns (2.15), the `hash()` efun (2.16, landed 2026-08-27),
`time_ns()`/`perf_counter_ns()` (2.23, landed 2026-08-27),
`secure_random()` (2.24, landed 2026-08-27), and `log2()`/`round()`
(2.25, landed 2026-08-27), and a real third-party mudlib boot-and-play
confirmation (3.9). See the table immediately below for the current
fraction of each phase, not the "zero" framing an earlier revision of
this file gave both.

**Re-swept 2026-08-27:** row 2.16 (`hash()`) landed this session,
picked specifically because it is real *current* FluffOS surface (this
row's own original title's `bcrypt` name was wrong, corrected in
`ROADMAP.md`'s own row -- real `hash()` never had a `bcrypt` algorithm
at all) that the vendored 2.9 ds2.08 reference this file otherwise cites
never had, confirmed absent from it entirely rather than assumed. See
`ROADMAP.md` row 2.16 for the full citation trail, the OpenSSL EVP-based
implementation, and its 3 new regression tests (764 total, up from 761).

**Re-swept 2026-08-27 (a further session, same day):** two new rows,
2.23 (`time_ns()`/`perf_counter_ns()`) and 2.24 (`secure_random()`),
picked up directly from that same session's own ranked modernization
research (its top two candidates), both real, genuinely new-since-2.9
current FluffOS efuns, confirmed absent from the vendored 2.9 ds2.08
reference entirely and confirmed against real current source (not
guessed) before writing any code. `secure_random()` is a real, distinct
security gap this driver's own pre-existing `random()` never filled
(a seeded, non-cryptographic PRNG, fine for gameplay, not for anything
security-sensitive) -- deliberately ported to match real FluffOS's own
actual `std::random_device("/dev/urandom")` mechanism exactly rather
than reusing this driver's own already-linked OpenSSL dependency via
`RAND_bytes()`, since real FluffOS itself never uses OpenSSL for this.
See `ROADMAP.md` rows 2.23/2.24 for the full citation trail, and
`STATUS.md`'s own dated entry for the live-verification account. 6 new
regression tests (773 total, up from 767).

**Re-swept 2026-08-27 (a further session, same day):** row 2.17
(`json_encode`/`json_decode`) re-examined on request, specifically to
check whether its own real formatting semantics (mapping key order,
float precision) were actually specified in real current source rather
than assumed unspecified -- the check went further than that question
and found the row's own premise was wrong: `json_encode`/`json_decode`
are not real current-FluffOS efuns at all (a genuinely empty
`src/svalue_json.cc` placeholder, zero declarations across all 21 real
package `.spec` files, zero `docs/efun/` pages), only an unrelated
save-file CLI conversion pair (`json2o`/`o2json`) with a fully
documented but structurally different JSON schema. Deferred on
stronger, more definitive grounds than before, not built. Row 2.25
(`log2()`/`round()`) landed instead, this session's own fallback
candidate, found the same way rows 2.16/2.23/2.24 were: real,
genuinely new-since-2.9 current FluffOS efuns confirmed absent from the
vendored 2.9 reference, verified via plain standard math identities
with zero live-current-FluffOS-instance dependency, the same bar
`hash()`/`time_ns()`/`secure_random()` were held to. See `ROADMAP.md`
rows 2.17/2.25 and `STATUS.md`'s own dated entry for the full trail. 3
new regression tests (776 total, up from 773).

**Re-swept 2026-08-27 (a further session, same day):** the row 2.25
method (cross-check an already-partially-implemented efun category's
real current `.spec` file against this driver's own registered efuns)
applied systematically to `core.spec` (strings/arrays/mappings/objects/
general, all bundled in one real package in current FluffOS) plus
`trim.spec`, `contrib.spec`, and `ops.spec` (the latter confirmed to be
bytecode operators, not efuns, out of scope). Found 41 real names
confirmed genuinely absent from the vendored 2.9 reference; five small,
independently-verifiable ones built as five new rows (2.26-2.30:
`trim`/`ltrim`/`rtrim`, `explode_reversible`, `call_out_walltime`,
`enable_wizard`/`disable_wizard`/`wizardp`, `sys_network_ports`), the
rest named and scoped as six new deferred rows (2.31-2.36) rather than
built speculatively or dropped silently -- real protocol-negotiation
work (GMCP/MSDP/MSP/ZMP/MXP), a new value type plus a charset-conversion
dependency (buffers/encoding), a runtime-mutable config registry
`get_config()`'s own comment already flagged as missing, a real VM
frame-lifecycle feature (`defer()`), destruct-path wiring
(`query_notify_destruct`), and implementation-specific driver-internals
diagnostics. See `ROADMAP.md` rows 2.26-2.36 and `STATUS.md`'s own
dated entry for the full trail. 12 new regression tests (789 total, up
from 776).

**Re-swept 2026-08-27 (a further session, same day):** the full real
`src/packages/` directory enumerated live (21 real package `.spec`
files, confirmed via the GitHub API tree, not guessed) and cross-checked
against what this project had already swept -- `math`/`core`/`trim`/
`contrib`/`ops` in the prior sweep, `sockets`/`pcre`/`db` when those
Phase 0/2 rows were originally built. Found `dwlib`/`uids`/
`mudlib_stats`/`compress`/`external`/`async`/`develop`/`ffi`/`matrix`/
`jsbridge` had never actually been checked against this driver's
registered efuns. `dwlib.spec` yielded three small, independently-
verifiable names, built as row 2.37 (`roll_MdN`/`vowel`/`add_a`). The
rest named and scoped as eight new deferred rows (2.38-2.45) rather
than built speculatively: a real UID/EUID trust hierarchy correctly
declined as a bare, ungated stand-in (the same "looks real, isn't"
risk that would matter more here than it did for the wizard flag,
since uid/euid is explicitly security-relevant); TLS-only socket
options, deferred alongside row 2.13; `db_commit`/`db_rollback`,
excluded outright once their own real docs turned out to say **"Not
yet implemented!"** in real current FluffOS itself; a real, separate
finding along the way worth naming precisely -- this driver's own
already-shipped `db_*` family (row 2.15) was built and cited against
real **LDMud's** own db package, not real current FluffOS's own
`db.spec` at all (confirmed directly from `DbRegistry.hpp`'s own header
comment, not newly asserted), so `db_status()` is entangled with a
real, larger "which `db_*` contract does this driver actually honor"
question rather than a quick addition; a new mudlib-statistics
subsystem; zlib compression pending the same missing buffer type row
2.33 already named; subprocess spawning with a real security surface;
real async I/O architecture; and a group of remaining names (FFI,
further debug internals, a niche 3D-math package, a WASM-only bridge)
that each resolve to an already-stated reason rather than a new one.
See `ROADMAP.md` rows 2.37-2.45 and `STATUS.md`'s own dated entry for
the full trail. 3 new regression tests (792 total, up from 789).

| Phase | Rows | Done | Open | % done |
|---|---|---|---|---|
| 0, Stabilize | 16 | 16 | 0 | 100% |
| 1, Dialect universality (real blockers only, DGD-only rows excluded) | 11 | 10 | 1 | 91% |
| 1, Dialect universality (including 5 DGD-only comparison rows) | 16 | 10 | 6 | 63% |
| 2, Beyond both (novel features) | 45 | 13 | 32 | 29% |
| 3: Production hardening + docs | 9 | 1 | 8 | 11% |

**What is left open in Phase 1, and why each item stays open** (each
with its own detailed, source-cited scoping note in `ROADMAP.md`, not
guessed at here):

- **Row 1.8 (LDMud `#'lfun::`/`#'sefun::`/`#'var::` closure-literal
  prefixes), the one row with real remaining scope, deferred on
  evidence**: the bare `#'name` form and the `#'efun::name` forced-tier
  prefix (rows 1.2/1.3) are real and tested; `#'lfun::`/`#'sefun::`
  (more forced-tier prefixes) and `#'var::` (`CLOSURE_IDENTIFIER`, a
  reference-to-a-global-variable closure kind this driver has no model
  of at all, structurally distinct from a callable closure) have zero
  confirmed real mudlib call sites anywhere in `temp/`: the only hit
  for any of the three is the LDMud driver's own changelog prose noting
  when it added them, not a real mudlib using them.
- **Row 1.7's own remaining sub-items (the row itself is closed, these
  are real, named exceptions inside it, not a separate open row)**:
  `privilege_violation()`, once flagged as a defensive-completeness
  question with no corpus signal of its own by nature rather than a
  compatibility gap, was picked up on exactly that basis and is now
  real for four trigger points: `bind_lambda()`'s cross-object form,
  `set_driver_hook()`, `call_out_info()` (dialect-gated, real FluffOS
  never had this mechanism at all), and `input_to()` (gated on the real
  `INPUT_IGNORE_BANG` flag bit specifically). `call_out_info()`/
  `input_to()` were this row's own last precisely-scoped, real-evidence
  items; both are now closed. `H_LOAD_UIDS`/`H_CLONE_UIDS`/
  `H_INCLUDE_DIRS` driver-hook trigger points have real but minimal
  evidence, 3 real call sites, all in one file (`secure/master/
  hooks.c`), versus 324 files defining `reset()` and 43 defining
  `clean_up()`, which is why `H_RESET`/`H_CLEAN_UP` (now real,
  dialect-gated where the two real drivers genuinely disagree) was
  picked first. Plain dialect-agnostic `lambda()`, per-hook type-map
  validation, `inaugurate_master()`'s own arg=1/2/3 master-reload/
  reactivation cases (only arg=0, first boot, is wired), and the
  remaining plain-string `hooks.c` hooks all have zero real corpus
  pressure, deferred on the same evidence discipline as everywhere
  else in this row, not forgotten.
- **Row 1.9's own remaining sub-items (the row itself is closed, same
  pattern as row 1.7 above)**: `m_allocate`/`m_entry`/`m_reallocate`/
  `m_add`/`m_contains` (the real N-columns-wide efun family) and the
  `([:width])` empty-mapping literal all have zero real call sites
  across every corpus in `temp/`: `m_indices()`/`m_values()` (the two
  real names with real usage), the width-2 `([ k: v1; v2 ])` literal, and
  `map[key, n]` indexing/assignment (including a real IncDec-on-column
  bug this project's own live-bug-first discipline caught and fixed) are
  the parts of this row real corpus evidence actually called for, and
  are done.
- **DGD's own five still-open rows (1.11-1.15)**: real, considered
  scope with real citations against `temp/dgd/`'s own source, but
  explicitly comparison context rather than a Phase 1 blocker per this
  project's own stated goal: a FluffOS/LDMud-level driver done better
  than either, not three-way parity with DGD.

## Row 0.13a (`parse_*`), now checked (partial), in detail

FluffOS's real natural-language sentence/grammar-rule parser package
(`packages/parser.c`, 3,419 lines): confirmed, not assumed, to matter:
Dead Souls' own core command dispatch calls `parse_sentence()` directly.
All 8 real efun names are now implemented (`parse_init`, `parse_add_rule`,
`parse_add_synonym`, `parse_remove`, `parse_dump`, `parse_refresh`,
`parse_sentence`, `parse_my_rules`), including full `OBJ`/`LIV`/`OBS`/`LVS`
noun-phrase-to-object matching, both single-object rules (candidate
resolution, adjective/ordinal narrowing, `LIV_MODIFIER`, "all of"/plural
`OBS` matching, ambiguity/error reporting) and real two-object rules
("give OBJ to LIV", both singular and plural shapes, e.g. "give OBS to
LIV") via `dependent_check_functions()`/`check_one_relation()`/
`check_object_relations()`, all live-verified against a real running
driver. Real corpus evidence found 77 real two-object rules in Dead
Souls' own `lib/verbs/` alone, so this was a real, sized piece of work,
not a theoretical corner case. `parse_sentence()`'s own 4th `nicks`
argument (a caller-supplied nickname mapping, real `add_nicknames()`/
`expand_node()`) is now real too, 2026-08-20, this row's own last
remaining sub-gap is closed. See `ROADMAP.md` row 0.13a for the full
component breakdown, citations, and live-verification history.

---

## Codebase scale

Raw line counts, `.c`/`.cpp`/`.h`/`.hpp` only, each driver's own real
source tree as vendored in `temp/`. A scale comparison, not a quality
one: AMLP is deliberately smaller because it targets specific,
confirmed-real-usage compatibility rather than reimplementing every
package (own database/crypto/ed-editor/full-MXP suites) either real
driver ships.

| Driver | Lines (`.c`/`.cpp`/`.h`) | Note |
|---|---|---|
| LDMud | ~211,600 | `temp/ldmud/src` |
| FluffOS 2.9 (ds2.08) | ~92,100 | `temp/reference/fluffos-2.9-ds2.08` |
| DGD (this vendored C++ port) | ~70,500 | `temp/dgd/src` |
| **AMLP** | **~22,500** | `src/` + `include/`, this repo |

## Efun / kfun surface

| Driver | Real count | Method |
|---|---|---|
| FluffOS 2.9 (ds2.08) | 270 | `ROADMAP.md` row 0.13's own `efun_defs.c` accounting (excludes ifdef'd-out/non-runtime entries; a raw `grep -c '^{"'` over the same file gives 276, the 6-name difference being exactly those exclusions) |
| LDMud | ~305 | Rough estimate: `temp/ldmud/doc/efun/` file count (one doc page per real efun is LDMud's own documentation convention; not independently cross-checked against a table the way the FluffOS/DGD/AMLP numbers were, so treat as approximate) |
| DGD (this vendored C++ port) | 243 | Real count: `grep -c '^FUNCDEF('` across `temp/dgd/src/kfun/{builtin,std,file,math,extra}.cpp` |
| **AMLP** | **248 of 270 real FluffOS names** (240 non-`parse_*` + all 8 `parse_*`) | `ROADMAP.md` row 0.13/0.13a's own accounting. The 22-name real gap: 40 non-`parse_*` names are documented, individually-verified exclusions (architecture mismatch, e.g. no `TYPE_CLASS`/buffer-type/ed()-editor equivalent, or zero real call sites across all six vendored mudlib corpora) minus the ones no longer counted against the gap. `parse_*` itself is no longer part of the gap at all (all 8 names implemented, every argument of every one of them real as of 2026-08-20's `nicks` slice, see row 0.13a's own entry). AMLP's own efun table primarily targets FluffOS's surface, with LDMud/DGD-specific additions layered on where a dialect diverges (`m_indices`/`m_values`, `#'name`, `nil`, `atomic`): it does not separately track coverage against LDMud's or DGD's own full efun/kfun lists the way it does for FluffOS. |

## Master/boot apply coverage

All three real drivers gate a running game through a "master object"
(FluffOS/LDMud) or "driver object" (DGD) that the driver core calls back
into for privilege checks, boot sequencing, and connection lifecycle
events, dozens of real named applies each. AMLP's own `BootApi`
abstraction (`include/amlp/dialect/BootApi.hpp`) currently recognizes
exactly **one** real per-dialect apply, `masterUidApply()`
(`get_root_uid` for FluffOS, `get_master_uid` for LDMud), deliberately
narrow, not an oversight: `connectApply()`/`netDeadApply()` are
explicitly omitted pending the three-way connect/disconnect design
question above (row 1.4/1.16). Real per-file `valid_read`/`valid_write`
privilege checks are implemented directly in `EfunTable.cpp` (dialect-
gated per real FluffOS 3-arg vs. LDMud 4-arg call convention) without
going through the `BootApi` abstraction at all. This is a real, current
gap against both real drivers' own much larger master-apply surface,
not yet closed.

---

## Feature-by-feature

Checkmarks mean "implemented and verified live against the real running
driver," not "attempted." A dash means the real driver in that column
does not have the feature at all (not a gap for it, just not
applicable).

| Feature | AMLP | FluffOS 2.9 | LDMud | DGD |
|---|---|---|---|---|
| Dialect selectable via config, one driver | Yes (`fluffos`/`ldmud`/`dgd`) |, (is FluffOS) |, (is LDMud) |, (is DGD) |
| Closures: `(: name :)` / `#'name` (FluffOS-style) | Yes | Yes | Yes (also has its own richer kinds) |, |
| Driver hooks (`set_driver_hook()`, `inaugurate_master()` boot wiring) | Partial (full 32-slot storage/dispatch real; 5 of several real trigger points wired, `H_MOVE_OBJECT0/1`, `H_MODIFY_COMMAND`, `H_RESET`, `H_CLEAN_UP`; only `inaugurate_master()`'s arg=0 first-boot case is wired, arg=1/2/3 reload/reactivation cases are not) |, | Yes |, |
| `privilege_violation()` authorization gate | Partial (4 real trigger points wired: `bind_lambda()` cross-object, `set_driver_hook()`, `call_out_info()` dialect-gated, `input_to()`'s `INPUT_IGNORE_BANG` flag; ~20 of 26 real doc-cataloged operations remain ungated, zero real corpus evidence or no implemented mechanism to gate for each) |, | Yes |, |
| Closures: real `lambda()`/`unbound_lambda()`/`bind_lambda()` kind distinction | Partial (`unbound_lambda()`/`bind_lambda()` real for the one confirmed corpus quoted-code shape; plain `lambda()` and the full closure-kind matrix not started, row 1.7/1.8 open) |, | Yes |, |
| Mapping width > 1 (`m_allocate`, N-column values) | Partial (`m_indices`/`m_values` real names ported, single-column only; row 1.9 open) |, | Yes |, |
| Shadows (`shadow()`, LDMud `unshadow()`/`query_allow_shadow`) | Yes | Yes (FluffOS shape) | Yes (LDMud shape, done) |, |
| `replace_program()`, LDMud no-arg sole-inherit form | Yes | Partial (has `replace_program`, not the LDMud no-arg form) | Yes |, |
| `nil` as a distinct value | Yes (dialect-gated) |, |, | Yes |
| `atomic` function modifier (checkpoint/rollback) | Lexed only, no VM semantics (row 1.12, not started) |, |, | Yes |
| `rlimits` (per-task tick/stack limits) | No (row 1.11, not started) |, |, | Yes |
| `parse_string` (grammar-driven string parsing kfun) | No (row 1.13, not started, confirmed comparable in size to `parse_*` itself, a dedicated DFA+LALR subsystem) |, |, | Yes |
| Lightweight objects (value-semantics objects) | No (row 1.14, not started) |, |, | Yes |
| `parse_*` natural-language sentence parser | All 8 efuns real, including single- and two-object `OBJ`/`LIV`/`OBS`/`LVS` matching and the `nicks` nickname-mapping argument | Yes (real source this work is ported from) |, |, |
| `save_object`/`restore_object`, real `.o` text format | Partial (restore-side only; save still uses this driver's own format) | Yes | Yes (own format) | Statedump-based, different model entirely |
| PCRE `regexp`/`regexplode`/`reg_assoc` | Yes | Yes | Yes (own regexp efuns) |, |
| Full telnet IAC negotiation, echo suppression, NAWS | Yes | Yes | Yes | Yes |
| `socket_*` efun family | Partial (STREAM/DATAGRAM only, no MUD mode, no binary modes) | Yes (full) | Yes (full) |, |
| Coroutine scheduler / `async`/`await` | No (Phase 2, not started) |, |, |, |
| LLVM JIT backend | No (Phase 2, not started) |, |, |, |
| Hotboot (fd-passing exec, connections survive) | No (Phase 2, not started) | Yes | Yes | Yes (via statedump/restart, different mechanism) |
| World-level statedump / object swapout | No (Phase 2, not started) |, |, | Yes (DGD's own signature architecture) |
| TLS / WebSocket | No (Phase 2, not started) | Not in this vendored ds2.08 snapshot | Not checked | Not checked |
| Built-in SQLite / hash / JSON efuns | Partial (SQLite `db_connect`/`db_exec`/`db_fetch`/`db_close`, row 2.15, and `hash()`, row 2.16, both built and landed; JSON efuns, row 2.17, not started) | Some (own DB package options) | Some | Some |
| LSP server (`--lsp`) | No (Phase 2, not started) |, |, |: |
| Generational GC (replacing `shared_ptr`) | No (Phase 3, not started) | Real GC | Real GC | Real GC |
| Full privilege/uid trust hierarchy | Partial (`privs()`, no full uid/euid/domain hierarchy) | Yes | Yes | Yes (own model) |

---

## What AMLP does not have, stated plainly

- **No `ed()` line editor, no database package, no crypto package
  beyond `crypt`/`oldcrypt`**: real FluffOS/LDMud both ship these;
  AMLP's own row 0.13 accounting lists them as confirmed architecture-
  mismatch exclusions, not silent gaps, but they are real absent
  features regardless of the reason.
- **No real garbage collector.** Object lifetime is plain
  `std::shared_ptr` reference counting throughout (`LpcObject`, closures,
  arrays, mappings). This means no cycle collection at all: a real,
  documented category of memory a genuine GC would reclaim that this
  driver currently never does. Phase 3's own `src/gc` item is exactly
  this, and is entirely unstarted.
- **Most Phase 2/3 differentiators are still a plan, not code.**
  Coroutines, JIT, hotboot, statedump, TLS, LSP, hot-reload, a
  conformance suite, generational GC -- all still have a real
  `instruct.md` and zero implementation, and none of them should be
  described as "in progress." Thirteen Phase 2 rows are the exception,
  real and landed rather than planned: the apply cache (2.9), the full
  PCRE suite (2.12), built-in SQLite (2.15), the `hash()` efun (2.16),
  `time_ns()`/`perf_counter_ns()` (2.23), `secure_random()` (2.24),
  `log2()`/`round()` (2.25), `trim()`/`ltrim()`/`rtrim()` (2.26),
  `explode_reversible()` (2.27), `call_out_walltime()` (2.28),
  `enable_wizard()`/`disable_wizard()`/`wizardp()` (2.29),
  `sys_network_ports()` (2.30), and `roll_MdN()`/`vowel()`/`add_a()`
  (2.37) -- along with Phase 3's own row 3.9, a
  real third-party mudlib boot-and-play confirmation.
- **Master/boot apply coverage is currently one name deep**
  (`masterUidApply()` only) against each real driver's own much larger
  master-object callback surface: see the section above. LDMud's
  separate driver-hook mechanism (`set_driver_hook()`,
  `inaugurate_master()`) is real and automatically wired at boot, but
  only 5 of its many real trigger points actually dispatch anything
  yet, and `privilege_violation()`, the real authorization gate several
  of those hooks and efuns sit behind, covers only 4 of 26 real
  doc-cataloged operations.
- **Dialect coverage is asymmetric.** FluffOS is the primary, most
  complete target (this is where the bundled mudlib and most of the
  regression corpus point); LDMud has real, working, dialect-gated
  pieces (closures, `m_indices`/`m_values`, shadows, `replace_program`,
  driver hooks) but real gaps (full closure kinds, mapping width, most
  hook trigger points); DGD support is the
  thinnest of the three by design (comparison-only, not a completion
  target): `nil` and `atomic`-the-keyword are the only DGD-dialect
  pieces implemented, with the rest (`rlimits`, `atomic`-the-semantics,
  `parse_string`, LWOs, the driver+auto boot path) confirmed real and
  scoped but not started.
- **This driver's own memory model can reach states real FluffOS
  cannot.** Documented directly in `STATUS.md`/`ROADMAP.md` where
  found (e.g. `parse_dump()`'s own `"(destructed)"` fallback for a
  weak_ptr that expired without ever going through `destruct()`) --
  a consequence of `shared_ptr`-based lifetime instead of real FluffOS's
  synchronous refcounted free, not a bug, but a real behavioral
  difference worth knowing about if porting mudlib code that depends on
  exact destruction timing.

## Where AMLP is already a real, working, from-scratch driver

Worth stating alongside the gaps above, since a gap list alone
undersells what already works: AMLP is not a wrapper or a fork of any
of the three real drivers: its own lexer, parser, code generator,
bytecode VM, object system, and network layer are original
implementations, verified continuously against real vendored source and
a real bundled mudlib rather than against assumption. 743 regression
tests pass as of this writing (see `STATUS.md` for the current count,
which changes every session), and the discipline behind every checked
row above is the same: read the real source, port the real behavior
(including confirmed real quirks and off-by-ones where they exist, not
just the "sensible" version), and verify live against a real running
instance before calling anything done.
